#include "spoofer.h"

#include <winioctl.h>
#include <setupapi.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>

#pragma comment(lib, "setupapi.lib")

namespace spoof {
namespace {

// {4D36E972-E325-11CE-BFC1-08002BE10318}
static const GUID kNetClass =
    {0x4D36E972, 0xE325, 0x11CE, {0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18}};

// ---- rng ----

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

std::string randHostname() {
    std::uniform_int_distribution<int> letter(0, 25);
    std::uniform_int_distribution<int> digit(0, 9);
    char b[20];
    std::snprintf(b, sizeof b, "DESKTOP-%c%c%c%c%c%c%c",
        'A' + letter(eng()), 'A' + letter(eng()),
        '0' + digit(eng()),  '0' + digit(eng()),
        'A' + letter(eng()), '0' + digit(eng()),
        'A' + letter(eng()));
    return b;
}

// ---- registry helpers ----

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

bool isVirtualAdapter(const char* desc) {
    static const char* skip[] = {
        "Virtual", "VMware", "VPN", "TAP", "Loopback",
        "WAN Miniport", "Hyper-V", "Npcap", "Bluetooth",
        "Microsoft Wi-Fi Direct", "Microsoft Kernel Debug",
        "Teredo", "ISATAP", "6to4"
    };
    for (auto s : skip)
        if (std::strstr(desc, s)) return true;
    return false;
}

bool isVirtualAdapterW(const wchar_t* desc) {
    static const wchar_t* skip[] = {
        L"Virtual", L"VMware", L"VPN", L"TAP", L"Loopback",
        L"WAN Miniport", L"Hyper-V", L"Npcap", L"Bluetooth",
        L"Wi-Fi Direct", L"Kernel Debug", L"Teredo", L"ISATAP", L"6to4"
    };
    for (auto s : skip)
        if (wcsstr(desc, s)) return true;
    return false;
}

}  // namespace

// ============================================================
// Read current values
// ============================================================

std::string ReadReg(const char* path, const char* name) {
    char buf[256]{};
    if (regRead(HKEY_LOCAL_MACHINE, path, name, buf, sizeof buf)) return buf;
    return "-";
}

std::string ReadDiskSerial(int drive) {
    wchar_t devPath[32];
    _snwprintf_s(devPath, _countof(devPath), _TRUNCATE, L"\\\\.\\PhysicalDrive%d", drive);

    HANDLE h = CreateFileW(devPath, 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "(erisim yok)";

    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType  = PropertyStandardQuery;

    BYTE buf[1024]{};
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
            &q, sizeof q, buf, sizeof buf, &ret, nullptr)) {
        CloseHandle(h);
        return "(okunamadi)";
    }
    CloseHandle(h);

    auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf);
    if (desc->SerialNumberOffset == 0 || desc->SerialNumberOffset >= ret)
        return "(seri no yok)";

    std::string serial(reinterpret_cast<char*>(buf + desc->SerialNumberOffset));
    while (!serial.empty() && serial.back() == ' ') serial.pop_back();
    while (!serial.empty() && serial.front() == ' ') serial.erase(serial.begin());
    return serial.empty() ? "(bos)" : serial;
}

std::string ReadVolumeSerial(wchar_t letter) {
    wchar_t root[8] = {letter, L':', L'\\', 0};
    DWORD serial = 0;
    if (!GetVolumeInformationW(root, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0))
        return "(okunamadi)";
    char b[16];
    std::snprintf(b, sizeof b, "%04X-%04X",
        static_cast<unsigned>(serial >> 16),
        static_cast<unsigned>(serial & 0xFFFF));
    return b;
}

std::string ReadComputerName() {
    char buf[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD sz = sizeof buf;
    if (GetComputerNameA(buf, &sz)) return buf;
    return "-";
}

// ============================================================
// Spoof functions
// ============================================================

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
    std::snprintf(b, sizeof b, "%05d-%05d-%05d-%05d",
        d(eng()), d(eng()), d(eng()), d(eng()));
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

bool SpoofComputerName(std::string& out) {
    out = randHostname();
    wchar_t wn[32]{};
    MultiByteToWideChar(CP_ACP, 0, out.c_str(), -1, wn, 32);
    return SetComputerNameExW(ComputerNamePhysicalDnsHostname, wn) != 0;
}

// ============================================================
// Volume serial — NTFS VBR byte write at 0x48
// ============================================================

bool SpoofVolumeSerial(wchar_t letter, std::string& out) {
    wchar_t vol[16];
    _snwprintf_s(vol, _countof(vol), _TRUNCATE, L"\\\\.\\%c:", letter);

    HANDLE h = CreateFileW(vol, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    BYTE vbr[512]{};
    DWORD got = 0;
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    if (!ReadFile(h, vbr, 512, &got, nullptr) || got != 512) {
        CloseHandle(h); return false;
    }

    if (std::memcmp(vbr + 3, "NTFS", 4) != 0) {
        CloseHandle(h); return false;
    }

    for (int i = 0; i < 8; ++i)
        vbr[0x48 + i] = rb();

    DWORD lo = 0, hi = 0;
    std::memcpy(&lo, vbr + 0x48, 4);
    std::memcpy(&hi, vbr + 0x4C, 4);
    DWORD display = lo ^ hi;
    char b[16];
    std::snprintf(b, sizeof b, "%04X-%04X",
        static_cast<unsigned>(display >> 16),
        static_cast<unsigned>(display & 0xFFFF));
    out = b;

    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    DWORD written = 0;
    bool ok = WriteFile(h, vbr, 512, &written, nullptr) && written == 512;
    CloseHandle(h);
    return ok;
}

// ============================================================
// MAC addresses — registry + adapter reset
// ============================================================

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
        if (isVirtualAdapter(desc))
            continue;

        std::string mac = randMac();
        if (regWrite(HKEY_LOCAL_MACHINE, full, "NetworkAddress", mac.c_str()))
            ++changed;
    }
    RegCloseKey(hBase);
    return changed;
}

void ResetAdapters() {
    HDEVINFO devs = SetupDiGetClassDevsW(&kNetClass, nullptr, nullptr, DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) return;

    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof dev;

    // disable all physical adapters
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &dev); ++i) {
        wchar_t desc[256]{};
        if (!SetupDiGetDeviceRegistryPropertyW(devs, &dev, SPDRP_DEVICEDESC,
                nullptr, reinterpret_cast<BYTE*>(desc), sizeof desc, nullptr))
            continue;
        if (isVirtualAdapterW(desc)) continue;

        SP_PROPCHANGE_PARAMS pcp{};
        pcp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        pcp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        pcp.StateChange = DICS_DISABLE;
        pcp.Scope       = DICS_FLAG_CONFIGSPECIFIC;
        pcp.HwProfile   = 0;
        SetupDiSetClassInstallParamsW(devs, &dev, &pcp.ClassInstallHeader, sizeof pcp);
        SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devs, &dev);
    }

    Sleep(1500);

    // re-enable all physical adapters
    dev.cbSize = sizeof dev;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &dev); ++i) {
        wchar_t desc[256]{};
        if (!SetupDiGetDeviceRegistryPropertyW(devs, &dev, SPDRP_DEVICEDESC,
                nullptr, reinterpret_cast<BYTE*>(desc), sizeof desc, nullptr))
            continue;
        if (isVirtualAdapterW(desc)) continue;

        SP_PROPCHANGE_PARAMS pcp{};
        pcp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        pcp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        pcp.StateChange = DICS_ENABLE;
        pcp.Scope       = DICS_FLAG_CONFIGSPECIFIC;
        pcp.HwProfile   = 0;
        SetupDiSetClassInstallParamsW(devs, &dev, &pcp.ClassInstallHeader, sizeof pcp);
        SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devs, &dev);
    }

    SetupDiDestroyDeviceInfoList(devs);
}

// ============================================================
// Cleanup
// ============================================================

int CleanEac() {
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

    tryDir("ProgramData",  "EasyAntiCheat");
    tryDir("APPDATA",      "EasyAntiCheat");
    tryDir("LOCALAPPDATA", "EasyAntiCheat");
    tryDir("TEMP",         "EasyAntiCheat");

    // Kefir game data (ROB RUN)
    tryDir("LOCALAPPDATA", "Kefir");
    tryDir("APPDATA",      "Kefir");

    if (RegDeleteTreeA(HKEY_LOCAL_MACHINE, "SOFTWARE\\EasyAntiCheat") == ERROR_SUCCESS) ++n;
    if (RegDeleteTreeA(HKEY_CURRENT_USER,  "SOFTWARE\\EasyAntiCheat") == ERROR_SUCCESS) ++n;

    return n;
}

int CleanTraces() {
    int n = 0;
    std::error_code ec;

    // prefetch files related to EAC and game
    char windir[MAX_PATH]{};
    if (GetEnvironmentVariableA("SystemRoot", windir, MAX_PATH)) {
        std::string prefetch = std::string(windir) + "\\Prefetch";
        if (std::filesystem::exists(prefetch, ec)) {
            for (auto& entry : std::filesystem::directory_iterator(prefetch, ec)) {
                auto name = entry.path().filename().string();
                if (name.find("EASYANTICHEAT") != std::string::npos ||
                    name.find("ROB RUN") != std::string::npos ||
                    name.find("ROBRUN") != std::string::npos ||
                    name.find("DX11_OVERLAY") != std::string::npos) {
                    if (std::filesystem::remove(entry.path(), ec)) ++n;
                }
            }
        }
    }

    // temp EAC/game files
    char tmp[MAX_PATH]{};
    if (GetEnvironmentVariableA("TEMP", tmp, MAX_PATH)) {
        std::string tmpDir(tmp);
        for (auto& entry : std::filesystem::directory_iterator(tmpDir, ec)) {
            auto name = entry.path().filename().string();
            if (name.find("EasyAntiCheat") != std::string::npos ||
                name.find("eac_") != std::string::npos) {
                std::filesystem::remove_all(entry.path(), ec);
                if (!ec) ++n;
            }
        }
    }

    // recent/jump list entries
    char appdata[MAX_PATH]{};
    if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH)) {
        std::string recent = std::string(appdata) +
            "\\Microsoft\\Windows\\Recent";
        if (std::filesystem::exists(recent, ec)) {
            for (auto& entry : std::filesystem::directory_iterator(recent, ec)) {
                auto name = entry.path().filename().string();
                if (name.find("ROB RUN") != std::string::npos ||
                    name.find("RobRun") != std::string::npos ||
                    name.find("dx11_overlay") != std::string::npos) {
                    if (std::filesystem::remove(entry.path(), ec)) ++n;
                }
            }
        }
    }

    return n;
}

// ============================================================
// Backup
// ============================================================

bool BackupHwids(const char* path) {
    std::ofstream f(path);
    if (!f.is_open()) return false;

    f << "# HWID backup — generated by jb overlay spoofer\n";
    f << "# DO NOT SHARE — contains your original hardware IDs\n\n";

    f << "MachineGuid = "
      << ReadReg("SOFTWARE\\Microsoft\\Cryptography", "MachineGuid") << "\n";
    f << "HwProfileGuid = "
      << ReadReg("SYSTEM\\CurrentControlSet\\Control\\IDConfigDB\\Hardware Profiles\\0001",
                 "HwProfileGuid") << "\n";
    f << "ProductId = "
      << ReadReg("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductId") << "\n";
    f << "BuildGUID = "
      << ReadReg("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "BuildGUID") << "\n";
    f << "SQMMachineId = "
      << ReadReg("SOFTWARE\\Microsoft\\SQMClient", "MachineId") << "\n";
    f << "ComputerName = " << ReadComputerName() << "\n";
    f << "VolumeSerial = " << ReadVolumeSerial(L'C') << "\n";
    f << "DiskSerial = " << ReadDiskSerial(0) << "\n";

    return true;
}

}  // namespace spoof
