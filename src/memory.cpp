// memory.cpp - Process bodies. C++20, MSVC /W4 /permissive-.

#include "memory.h"
#include "syscall.h"

#include <tlhelp32.h>  // TH32CS_SNAPPROCESS only; module snapshots are never used

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>

namespace mem {
namespace {

// ---- layout block: every target-side offset lives here ----
constexpr uintptr_t kPebLdr           = 0x18;  // PEB.Ldr
constexpr uintptr_t kLdrInitialized   = 0x04;  // PEB_LDR_DATA.Initialized
constexpr uintptr_t kLdrLoadOrderList = 0x10;  // PEB_LDR_DATA.InLoadOrderModuleList

struct UStr {
    uint16_t length;    // bytes
    uint16_t capacity;
    uint32_t pad;
    uint64_t buffer;
};

struct LdrEntry {
    uint64_t flink, blink;   // 0x00 InLoadOrderLinks
    uint64_t memOrder[2];    // 0x10
    uint64_t initOrder[2];   // 0x20
    uint64_t dllBase;        // 0x30
    uint64_t entryPoint;     // 0x38
    uint32_t sizeOfImage;    // 0x40
    uint32_t pad;
    UStr     fullName;       // 0x48
    UStr     baseName;       // 0x58
};

struct ProcBasicInfo {
    LONG     exitStatus;
    uint32_t pad0;
    uint64_t peb;
    uint64_t affinity;
    LONG     basePriority;
    uint32_t pad1;
    uint64_t pid;
    uint64_t parentPid;
};

static_assert(sizeof(UStr) == 0x10);
static_assert(offsetof(LdrEntry, dllBase) == 0x30);
static_assert(offsetof(LdrEntry, sizeOfImage) == 0x40);
static_assert(offsetof(LdrEntry, baseName) == 0x58);
static_assert(sizeof(LdrEntry) == 0x68);
static_assert(offsetof(ProcBasicInfo, peb) == 0x08);
static_assert(sizeof(ProcBasicInfo) == 0x30);

// ---- bounds ----
constexpr size_t kMaxRead    = size_t{1} << 26;  // 64 MiB per ReadBuf
constexpr size_t kMaxName    = 260;
constexpr size_t kMaxModules = 4096;
constexpr size_t kMaxRegions = size_t{1} << 20;

bool NameEq(const wchar_t* a, size_t an, std::wstring_view b) noexcept {
    return an == b.size() && _wcsnicmp(a, b.data(), an) == 0;
}

}  // namespace

Process::Process(Process&& o) noexcept : h_(o.h_), pid_(o.pid_) {
    o.h_ = nullptr;
    o.pid_ = 0;
}

Process& Process::operator=(Process&& o) noexcept {
    if (this != &o) {
        Close();
        h_ = o.h_;
        pid_ = o.pid_;
        o.h_ = nullptr;
        o.pid_ = 0;
    }
    return *this;
}

Process::~Process() { Close(); }

std::optional<MEMORY_BASIC_INFORMATION> Process::QueryMemory(uintptr_t addr) const noexcept {
    if (!h_) return std::nullopt;
    MEMORY_BASIC_INFORMATION mbi{};
    if (sc::g_sc.ready) {
        SIZE_T ret = 0;
        if (sc::g_sc.queryVm(h_, reinterpret_cast<PVOID>(addr), 0, &mbi, sizeof mbi, &ret) < 0)
            return std::nullopt;
        return mbi;
    }
    if (!VirtualQueryEx(h_, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof mbi))
        return std::nullopt;
    return mbi;
}

DWORD Process::FindPid(std::wstring_view exe) {
    if (exe.empty()) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (NameEq(pe.szExeFile, wcsnlen(pe.szExeFile, MAX_PATH), exe)) {
            pid = pe.th32ProcessID;
            break;
        }
    }
    CloseHandle(snap);
    return pid;
}

bool Process::Attach(DWORD pid) {
    Close();
    if (pid == 0) return false;

    if (sc::g_sc.ready) {
        HANDLE h = nullptr;
        sc::ObjAttr oa{}; oa.length = sizeof(oa);
        sc::ClientId cid{}; cid.uniqueProcess = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(pid));
        if (sc::g_sc.openProc(&h, PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, &oa, &cid) < 0 || !h)
            return false;
        h_ = h; pid_ = pid;
        return true;
    }

    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    h_ = h;
    pid_ = pid;
    return true;
}

bool Process::Attach(std::wstring_view exe) {
    const DWORD pid = FindPid(exe);
    return pid != 0 && Attach(pid);
}

void Process::Close() noexcept {
    if (h_) CloseHandle(h_);
    h_ = nullptr;
    pid_ = 0;
}

bool Process::Alive() const noexcept {
    DWORD code = 0;
    return h_ && GetExitCodeProcess(h_, &code) && code == STILL_ACTIVE;
}

bool Process::ReadBuf(uintptr_t addr, void* dst, size_t size) const noexcept {
    if (!h_ || !dst || addr == 0 || size == 0 || size > kMaxRead) return false;
    if (addr + size < addr) return false;
    SIZE_T got = 0;
    if (sc::g_sc.ready)
        return sc::g_sc.readVm(h_, reinterpret_cast<PVOID>(addr), dst, size, &got) >= 0 && got == size;
    return ReadProcessMemory(h_, reinterpret_cast<LPCVOID>(addr), dst, size, &got) && got == size;
}

std::optional<ModuleInfo> Process::FindModule(std::wstring_view name) const {
    if (auto m = FindModuleLdr(name)) return m;
    return FindModuleScan(name);
}

std::optional<uintptr_t> Process::ModuleBase(std::wstring_view name) const {
    if (auto m = FindModule(name)) return m->base;
    return std::nullopt;
}

std::optional<ModuleInfo> Process::FindModuleLdr(std::wstring_view name) const {
    if (name.empty() || name.size() >= kMaxName || !Native64()) return std::nullopt;

    const uintptr_t peb = PebAddress();
    if (!peb) return std::nullopt;

    const auto ldr = Read<uint64_t>(peb + kPebLdr);
    if (!ldr || !*ldr) return std::nullopt;

    const auto inited = Read<uint8_t>(static_cast<uintptr_t>(*ldr) + kLdrInitialized);
    if (!inited || !*inited) return std::nullopt;

    const uintptr_t head = static_cast<uintptr_t>(*ldr) + kLdrLoadOrderList;
    const auto first = Read<uint64_t>(head);
    if (!first) return std::nullopt;

    uintptr_t cur = static_cast<uintptr_t>(*first);
    for (size_t i = 0; i < kMaxModules && cur && cur != head; ++i) {
        // InLoadOrderLinks is the first member, so the link address is the entry address.
        const auto e = Read<LdrEntry>(cur);
        if (!e) return std::nullopt;

        const UStr& bn = e->baseName;
        if (e->dllBase && bn.buffer && bn.length >= 2 && (bn.length & 1) == 0 &&
            bn.length / sizeof(wchar_t) < kMaxName) {
            wchar_t buf[kMaxName]{};
            const size_t chars = bn.length / sizeof(wchar_t);
            if (ReadBuf(static_cast<uintptr_t>(bn.buffer), buf, bn.length) && NameEq(buf, chars, name)) {
                return ModuleInfo{static_cast<uintptr_t>(e->dllBase), e->sizeOfImage};
            }
        }
        cur = static_cast<uintptr_t>(e->flink);
    }
    return std::nullopt;
}

std::optional<ModuleInfo> Process::FindModuleScan(std::wstring_view name) const {
    if (!h_ || name.empty() || name.size() >= kMaxName || !Native64()) return std::nullopt;

    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    const auto hi = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);

    for (size_t i = 0; i < kMaxRegions && addr < hi; ++i) {
        auto optMbi = QueryMemory(addr);
        if (!optMbi) break;
        auto& mbi = *optMbi;

        const auto rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const size_t rs = mbi.RegionSize;
        if (rs == 0 || rb + rs <= addr) break;  // no forward progress / wrap

        if (mbi.Type == MEM_IMAGE && mbi.State == MEM_COMMIT && mbi.AllocationBase == mbi.BaseAddress) {
            if (auto m = ProbeImage(rb, name)) return m;
        }
        addr = rb + rs;
    }
    return std::nullopt;
}

bool Process::Native64() const noexcept {
    BOOL wow = TRUE;
    return h_ && IsWow64Process(h_, &wow) && !wow;
}

uintptr_t Process::PebAddress() const noexcept {
    using NtQip = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const NtQip fn = reinterpret_cast<NtQip>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!fn || !h_) return 0;
    ProcBasicInfo pbi{};
    ULONG ret = 0;
    if (fn(h_, 0 /*ProcessBasicInformation*/, &pbi, sizeof pbi, &ret) < 0) return 0;
    return static_cast<uintptr_t>(pbi.peb);
}

std::optional<ModuleInfo> Process::ProbeImage(uintptr_t base, std::wstring_view name) const {
    const auto dos = Read<IMAGE_DOS_HEADER>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return std::nullopt;
    if (dos->e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) ||
        static_cast<size_t>(dos->e_lfanew) > 0x1000 - sizeof(IMAGE_NT_HEADERS64))
        return std::nullopt;

    const auto nt = Read<IMAGE_NT_HEADERS64>(base + static_cast<uintptr_t>(dos->e_lfanew));
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return std::nullopt;

    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY)) return std::nullopt;

    const auto exp = Read<IMAGE_EXPORT_DIRECTORY>(base + dir.VirtualAddress);
    if (!exp || !exp->Name || exp->Name >= nt->OptionalHeader.SizeOfImage) return std::nullopt;

    // Stay inside the name's page so a name at a page edge cannot fail the whole read.
    char raw[kMaxName]{};
    const uintptr_t na = base + exp->Name;
    const size_t n = (std::min)(sizeof raw - 1, static_cast<size_t>(0x1000 - (na & 0xFFF)));
    if (!ReadBuf(na, raw, n)) return std::nullopt;

    const size_t len = strnlen(raw, n);
    if (len != name.size()) return std::nullopt;
    for (size_t i = 0; i < len; ++i) {
        if (std::towlower(static_cast<unsigned char>(raw[i])) != std::towlower(name[i]))
            return std::nullopt;
    }
    return ModuleInfo{base, nt->OptionalHeader.SizeOfImage};
}

}  // namespace mem
