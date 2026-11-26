#pragma once
// memory.h - read-only external process access (x64 host, x64 target). Bodies in memory.cpp.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

static_assert(sizeof(void*) == 8, "x64 build only");

namespace mem {

struct ModuleInfo {
    uintptr_t base;
    uint32_t  size;
};

class Process {
public:
    Process() = default;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&& o) noexcept;
    Process& operator=(Process&& o) noexcept;
    ~Process();

    explicit operator bool() const noexcept { return h_ != nullptr; }
    DWORD  Pid() const noexcept { return pid_; }
    HANDLE Handle() const noexcept { return h_; }

    // 0 = not found / snapshot failed.
    static DWORD FindPid(std::wstring_view exe);

    bool Attach(DWORD pid);
    bool Attach(std::wstring_view exe);
    void Close() noexcept;
    bool Alive() const noexcept;

    // true only when the full range was read.
    bool ReadBuf(uintptr_t addr, void* dst, size_t size) const noexcept;

    template <class T>
    std::optional<T> Read(uintptr_t addr) const noexcept {
        static_assert(std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>);
        T v{};
        if (!ReadBuf(addr, &v, sizeof v)) return std::nullopt;
        return v;
    }

    // VirtualQueryEx wrapper for heap scanning.
    std::optional<MEMORY_BASIC_INFORMATION> QueryMemory(uintptr_t addr) const noexcept;

    // Ldr walk first, image scan if the loader list is empty / unreadable / missing the name.
    std::optional<ModuleInfo> FindModule(std::wstring_view name) const;
    std::optional<uintptr_t>  ModuleBase(std::wstring_view name) const;

    // PEB -> Ldr -> InLoadOrderModuleList, read out of the target.
    std::optional<ModuleInfo> FindModuleLdr(std::wstring_view name) const;

    // VirtualQueryEx sweep over MEM_IMAGE allocation bases, name matched against the export directory.
    // Misses images without exports (typically the main exe) - those come from the Ldr walk.
    std::optional<ModuleInfo> FindModuleScan(std::wstring_view name) const;

private:
    bool      Native64() const noexcept;
    uintptr_t PebAddress() const noexcept;
    std::optional<ModuleInfo> ProbeImage(uintptr_t base, std::wstring_view name) const;

    HANDLE h_   = nullptr;
    DWORD  pid_ = 0;
};

}  // namespace mem
