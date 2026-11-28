#include "spoofer.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <random>

namespace spoof {
namespace {

std::mt19937& eng() {
    static std::mt19937 e(std::random_device{}());
    return e;
}

uint8_t rb() {
    return static_cast<uint8_t>(std::uniform_int_distribution<int>(0, 255)(eng()));
}

std::string randGuid() {
    char b[48];
    std::snprintf(b, sizeof b,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        rb(),rb(),rb(),rb(), rb(),rb(), rb(),rb(),
        rb(),rb(), rb(),rb(),rb(),rb(),rb(),rb());
    return b;
}

std::string randMac() {
    uint8_t m[6];
    for (auto& x : m) x = rb();
    m[0] = (m[0] & 0xFC) | 0x02;
    char b[13];
    std::snprintf(b, sizeof b, "%02X%02X%02X%02X%02X%02X",
        m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

bool regRead(HKEY root, const char* path, const char* name, char* buf, DWORD sz) {
    HKEY k;
    if (RegOpenKeyExA(root, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, n = sz;
    bool ok = RegQueryValueExA(k, name, nullptr, &type,
                  reinterpret_cast<BYTE*>(buf), &n) == ERROR_SUCCESS && type == REG_SZ;
    RegCloseKey(k);
    return ok;
}

bool regWrite(HKEY root, const char* path, const char* name, const char* val) {
    HKEY k;
    if (RegOpenKeyExA(root, path, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return false;
    bool ok = RegSetValueExA(k, name, 0, REG_SZ,
                  reinterpret_cast<const BYTE*>(val),
                  static_cast<DWORD>(std::strlen(val) + 1)) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

bool regWriteDw(HKEY root, const char* path, const char* name, DWORD val) {
    HKEY k;
    if (RegOpenKeyExA(root, path, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return false;
    bool ok = RegSetValueExA(k, name, 0, REG_DWORD,
                  reinterpret_cast<const BYTE*>(&val), sizeof(DWORD)) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

void delDir(const char* path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

}  // namespace

std::string ReadReg(const char* path, const char* name) {
    char buf[256]{};
    if (regRead(HKEY_LOCAL_MACHINE, path, name, buf, sizeof buf)) return buf;
    return "-";
}

bool SpoofMachineGuid(std::string& out) {
    out = randGuid();
    return regWrite(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid", out.c_str());
}

bool SpoofHwProfileGuid(std::string& out) {
    out = "{" + randGuid() + "}";
    return regWrite(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\IDConfigDB\\Hardware Profiles\\0001",
        "HwProfileGuid", out.c_str());
}

bool SpoofProductId(std::string& out) {
    std::uniform_int_distribution<int> d(0, 99999);
    char b[32];
    std::snprintf(b, sizeof b, "%05d-%05d-%05d-%05d", d(eng()), d(eng()), d(eng()), d(eng()));
    out = b;
    return regWrite(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductId", out.c_str());
}

bool SpoofBuildGuid(std::string& out) {
    out = randGuid();
    return regWrite(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "BuildGUID", out.c_str());
}

bool SpoofInstallDate(std::string& out) {
    auto now = static_cast<DWORD>(std::time(nullptr));
    std::uniform_int_distribution<DWORD> d(now - 63072000u, now - 2592000u);
    DWORD ts = d(eng());
    char b[16];
    std::snprintf(b, sizeof b, "%u", ts);
    out = b;
    return regWriteDw(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "InstallDate", ts);
}

bool SpoofSqmId(std::string& out) {
    out = "{" + randGuid() + "}";
    return regWrite(HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\SQMClient", "MachineId", out.c_str());
}

int SpoofMacAddresses() {
    const char* base = "SYSTEM\\CurrentControlSet\\Control\\Class\\"
                       "{4d36e972-e325-11ce-bfc1-08002be10318}";
    HKEY hBase;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, base, 0, KEY_READ, &hBase) != ERROR_SUCCESS)
        return 0;

    int changed = 0;
    char sub[16];
    DWORD subSz;
    for (DWORD i = 0; ; ++i) {
        subSz = sizeof sub;
        if (RegEnumKeyExA(hBase, i, sub, &subSz, nullptr, nullptr, nullptr, nullptr)
            != ERROR_SUCCESS)
            break;

        char full[256];
        std::snprintf(full, sizeof full, "%s\\%s", base, sub);

        char desc[128]{};
        if (!regRead(HKEY_LOCAL_MACHINE, full, "DriverDesc", desc, sizeof desc))
            continue;

        if (std::strstr(desc, "Virtual") || std::strstr(desc, "VMware") ||
            std::strstr(desc, "VPN")     || std::strstr(desc, "TAP")   ||
            std::strstr(desc, "Loopback")|| std::strstr(desc, "WAN Miniport") ||
            std::strstr(desc, "Hyper-V") || std::strstr(desc, "Npcap"))
            continue;

        std::string mac = randMac();
        if (regWrite(HKEY_LOCAL_MACHINE, full, "NetworkAddress", mac.c_str()))
            ++changed;
    }
    RegCloseKey(hBase);
    return changed;
}

int CleanEacCache() {
    int n = 0;
    auto tryDir = [&](const char* env, const char* sub) {
        char p[MAX_PATH]{};
        if (!GetEnvironmentVariableA(env, p, MAX_PATH)) return;
        std::string full = std::string(p) + "\\" + sub;
        std::error_code ec;
        if (std::filesystem::exists(full, ec)) {
            auto removed = std::filesystem::remove_all(full, ec);
            if (!ec && removed > 0) ++n;
        }
    };

    tryDir("ProgramData",    "EasyAntiCheat");
    tryDir("APPDATA",        "EasyAntiCheat");
    tryDir("LOCALAPPDATA",   "EasyAntiCheat");
    tryDir("TEMP",           "EasyAntiCheat");

    if (RegDeleteTreeA(HKEY_LOCAL_MACHINE, "SOFTWARE\\EasyAntiCheat") == ERROR_SUCCESS) ++n;
    if (RegDeleteTreeA(HKEY_CURRENT_USER,  "SOFTWARE\\EasyAntiCheat") == ERROR_SUCCESS) ++n;

    return n;
}

}  // namespace spoof
