#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <winternl.h>
#include <intrin.h>
#include <tlhelp32.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "esp.h"
#include "memory.h"
#include "offsets.h"
#include "syscall.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

constexpr wchar_t xkey = 0x4B2F;
template <size_t N>
struct XW {
    wchar_t d[N];
    constexpr XW(const wchar_t (&s)[N]) { for (size_t i = 0; i < N; i++) d[i] = s[i] ^ xkey; }
    struct Buf { wchar_t s[N]; };
    Buf dec() const { Buf b; for (size_t i = 0; i < N; i++) b.s[i] = d[i] ^ xkey; return b; }
};

static constexpr XW xTarget0{L"ROB RUN.exe"};
static constexpr XW xTarget1{L"RobRun.exe"};
static constexpr XW xTarget2{L"Rob Run.exe"};
static constexpr XW xTarget3{L"robrun.exe"};

void Stage(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    std::vfprintf(stdout, fmt, args);
    va_end(args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void ScanRunningProcesses() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof pe;
    Stage("[scan] listing processes containing 'rob':");
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        char narrow[260]{};
        for (int i = 0; i < 259 && pe.szExeFile[i]; ++i)
            narrow[i] = pe.szExeFile[i] < 128 ? static_cast<char>(pe.szExeFile[i]) : '?';
        if (strstr(narrow, "rob") || strstr(narrow, "Rob") || strstr(narrow, "ROB") ||
            strstr(narrow, "run") || strstr(narrow, "Run") || strstr(narrow, "RUN"))
            Stage("  pid=%lu  %s", static_cast<unsigned long>(pe.th32ProcessID), narrow);
    }
    CloseHandle(snap);
}

constexpr ULONGLONG kAttachGapMs = 1000;

struct Dx {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
};

Dx g_dx;
UINT g_resizeW = 0, g_resizeH = 0;

template <typename T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool CreateRenderTarget() {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_dx.swapChain->GetBuffer(0, IID_PPV_ARGS(&bb))) || !bb) return false;
    HRESULT hr = g_dx.device->CreateRenderTargetView(bb, nullptr, &g_dx.rtv);
    bb->Release();
    return SUCCEEDED(hr);
}

void CleanupRenderTarget() { SafeRelease(g_dx.rtv); }

bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = {0, 1};
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION, &sd, &g_dx.swapChain, &g_dx.device, &got, &g_dx.context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION, &sd, &g_dx.swapChain, &g_dx.device, &got, &g_dx.context);
    if (FAILED(hr)) return false;
    return CreateRenderTarget();
}

void CleanupDevice() {
    CleanupRenderTarget();
    SafeRelease(g_dx.swapChain); SafeRelease(g_dx.context); SafeRelease(g_dx.device);
}

void SetClickThrough(HWND hwnd, bool ct) {
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex = ct ? (ex | WS_EX_TRANSPARENT) : (ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    if (!ct) SetForegroundWindow(hwnd);
}

void RandClassName(wchar_t* out, int len) {
    std::srand(static_cast<unsigned>(std::time(nullptr)) ^ GetCurrentProcessId());
    for (int i = 0; i < len - 1; i++) out[i] = L'a' + (std::rand() % 26);
    out[len - 1] = 0;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return 1;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) { g_resizeW = LOWORD(lParam); g_resizeH = HIWORD(lParam); }
        return 0;
    case WM_SYSCOMMAND: if ((wParam & 0xfff0) == SC_KEYMENU) return 0; break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================
// Game window finder
// ============================================================

struct WndFinder { DWORD pid; HWND result; };
static BOOL CALLBACK FindGameWndCb(HWND h, LPARAM lp) {
    auto* f = reinterpret_cast<WndFinder*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == f->pid && IsWindowVisible(h)) {
        RECT r;
        GetClientRect(h, &r);
        if (r.right > 200 && r.bottom > 200) {
            f->result = h;
            return FALSE;
        }
    }
    return TRUE;
}

HWND FindGameWindow(DWORD pid) {
    WndFinder f{pid, nullptr};
    EnumWindows(FindGameWndCb, reinterpret_cast<LPARAM>(&f));
    return f.result;
}

// ============================================================
// Anti-detection bypass
// ============================================================
namespace bypass {

void HideThread() {
    using NtSIT = NTSTATUS(NTAPI*)(HANDLE, ULONG, PVOID, ULONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    auto fn = reinterpret_cast<NtSIT>(GetProcAddress(ntdll, "NtSetInformationThread"));
    if (!fn) return;
    constexpr ULONG ThreadHideFromDebugger = 0x11;
    fn(GetCurrentThread(), ThreadHideFromDebugger, nullptr, 0);
}

void ClearHeaders() {
    HMODULE base = GetModuleHandleW(nullptr);
    if (!base) return;
    auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    auto nt  = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<BYTE*>(base) + dos->e_lfanew);
    DWORD sz = nt->OptionalHeader.SizeOfHeaders;
    DWORD old;
    if (VirtualProtect(base, sz, PAGE_READWRITE, &old)) {
        SecureZeroMemory(base, sz);
        VirtualProtect(base, sz, old, &old);
    }
}

void UnhookNtdll() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;

    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    wchar_t path[MAX_PATH];
    wsprintfW(path, L"%s\\ntdll.dll", sysDir);

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(file);
    if (!mapping) return;

    auto clean = reinterpret_cast<BYTE*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    CloseHandle(mapping);
    if (!clean) return;

    auto cleanDos = reinterpret_cast<PIMAGE_DOS_HEADER>(clean);
    auto cleanNt  = reinterpret_cast<PIMAGE_NT_HEADERS>(clean + cleanDos->e_lfanew);

    auto sec = IMAGE_FIRST_SECTION(cleanNt);
    for (WORD i = 0; i < cleanNt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (sec->Name[0] == '.' && sec->Name[1] == 't' &&
            sec->Name[2] == 'e' && sec->Name[3] == 'x' && sec->Name[4] == 't') {
            BYTE* dst = reinterpret_cast<BYTE*>(ntdll) + sec->VirtualAddress;
            BYTE* src = clean + sec->PointerToRawData;
            DWORD sz  = sec->Misc.VirtualSize;
            DWORD old;
            if (VirtualProtect(dst, sz, PAGE_EXECUTE_READWRITE, &old)) {
                std::memcpy(dst, src, sz);
                VirtualProtect(dst, sz, old, &old);
            }
            break;
        }
    }
    UnmapViewOfFile(clean);
}

void EraseCommandLine() {
    auto* peb = reinterpret_cast<BYTE*>(__readgsqword(0x60));
    if (!peb) return;
    auto* params = *reinterpret_cast<BYTE**>(peb + 0x20);
    if (!params) return;
    // CommandLine UNICODE_STRING at RTL_USER_PROCESS_PARAMETERS+0x70
    auto* cmdLen = reinterpret_cast<USHORT*>(params + 0x70);
    auto* cmdBuf = *reinterpret_cast<wchar_t**>(params + 0x78);
    if (cmdBuf && *cmdLen > 0) {
        SecureZeroMemory(cmdBuf, *cmdLen);
        *cmdLen = 0;
    }
    // ImagePathName at +0x60
    auto* imgLen = reinterpret_cast<USHORT*>(params + 0x60);
    auto* imgBuf = *reinterpret_cast<wchar_t**>(params + 0x68);
    if (imgBuf && *imgLen > 0) {
        SecureZeroMemory(imgBuf, *imgLen);
        *imgLen = 0;
    }
}

void Apply() {
    UnhookNtdll();
    HideThread();
    ClearHeaders();
    EraseCommandLine();
}

}  // namespace bypass

bool IsAdmin() {
    BOOL admin = FALSE;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    PSID grp = nullptr;
    if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &grp)) {
        CheckTokenMembership(nullptr, grp, &admin);
        FreeSid(grp);
    }
    return admin != FALSE;
}

void EnableDebugPriv() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;
    TOKEN_PRIVILEGES tp{};
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof tp, nullptr, nullptr);
    }
    CloseHandle(tok);
}

void RelaunchAsAdmin() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

}  // namespace

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    Stage("[CRASH] exception 0x%08X at 0x%p",
          ep->ExceptionRecord->ExceptionCode,
          ep->ExceptionRecord->ExceptionAddress);
    std::fflush(stdout);
    system("pause");
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    if (!IsAdmin()) {
        RelaunchAsAdmin();
        return 0;
    }

    AllocConsole();
    FILE* fDummy;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONOUT$", "w", stderr);
    freopen_s(&fDummy, "CONIN$",  "r", stdin);
    SetConsoleTitleA("jb overlay");
    SetUnhandledExceptionFilter(CrashHandler);
    Stage("[boot] starting v3 (admin: yes)");
    EnableDebugPriv();
    Stage("[boot] SeDebugPrivilege enabled");

    cfg::Offsets offsets;
    char exePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string cfgPath(exePath);
    auto slash = cfgPath.find_last_of("\\/");
    if (slash != std::string::npos) cfgPath = cfgPath.substr(0, slash + 1);
    cfgPath += "offsets.cfg";
    if (cfg::LoadOffsets(cfgPath, offsets))
        Stage("[config] loaded offsets.cfg");
    else {
        cfg::SaveDefaultOffsets(cfgPath);
        if (cfg::LoadOffsets(cfgPath, offsets))
            Stage("[config] created and loaded default offsets.cfg");
        else
            Stage("[config] created default offsets.cfg but load failed — check file");
    }

    std::string settingsPath(exePath);
    {
        auto s2 = settingsPath.find_last_of("\\/");
        if (s2 != std::string::npos) settingsPath = settingsPath.substr(0, s2 + 1);
        settingsPath += "settings.ini";
    }

    if (offsets.battleModelTypeInfo == 0)
        Stage("[config] battleModelTypeInfo=0, will auto-detect by class name (slower)");

    ImGui_ImplWin32_EnableDpiAwareness();

    wchar_t className[16];
    RandClassName(className, 16);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = className;
    if (!RegisterClassExW(&wc)) { Stage("[boot] RegisterClass failed"); return 1; }

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        className, L"", WS_POPUP, 0, 0, screenW, screenH,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) { Stage("[window] create failed"); UnregisterClassW(className, hInstance); return 1; }

    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    MARGINS margins{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);

    if (!CreateDevice(hwnd)) {
        Stage("[dx11] device create failed");
        CleanupDevice(); DestroyWindow(hwnd); UnregisterClassW(className, hInstance); return 1;
    }

    bypass::Apply();
    Stage("[bypass] applied (unhook+hide+clear+erase)");
    if (sc::g_sc.Init())
        Stage("[syscall] indirect stubs ready (NtRVM+NtQVM+NtOP)");
    else
        Stage("[syscall] stubs failed — fallback to WinAPI");

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.f;
    style.FrameRounding = 3.f;
    style.TabRounding = 3.f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.10f, 0.94f);
    style.Colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.15f, 0.20f, 1.f);
    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.25f, 0.30f, 0.50f, 1.f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dx.device, g_dx.context);
    Stage("[imgui] ready");

    Stage("[init] creating process handler...");
    mem::Process proc;
    Stage("[init] creating ESP...");
    esp::Esp esp(proc, offsets);
    Stage("[init] ESP created OK");
    if (esp.LoadSettings(settingsPath))
        Stage("[config] loaded settings.ini");
    ULONGLONG nextAttachMs = 0;

    bool espActive = true;
    bool menuOpen  = false;
    bool running   = true;
    HWND gameWindow = nullptr;
    const float clear[4] = {0.f, 0.f, 0.f, 0.f};

    // diagnostic state for on-screen HUD
    char diagLine1[128] = "OYUN BEKLENIYOR...";
    char diagLine2[128] = "";
    bool diagAttached = false;

    // open log file next to exe
    FILE* logFile = nullptr;
    {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring logPath(exePath);
        auto slash = logPath.find_last_of(L"\\/");
        if (slash != std::wstring::npos) logPath = logPath.substr(0, slash + 1);
        logPath += L"DURUM.txt";
        logFile = _wfopen(logPath.c_str(), L"w");
        if (logFile) { fprintf(logFile, "BASLATILDI\n"); fflush(logFile); }
    }

    Stage("[ready] v6 — INS: menu | END: exit | F1: esp ac/kapa");
    Stage("============================================");
    Stage("  ESP OTOMATIK ACIK. OYUNU AC, BEKLE.");
    Stage("  Oyun bulununca otomatik baglanacak.");
    Stage("============================================");
    ULONGLONG nextStatusMs = 0;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) { Stage("[exit] WM_QUIT received"); break; }

        if (GetAsyncKeyState(VK_END) & 1) { Stage("[exit] END key pressed"); break; }
        if (GetAsyncKeyState(VK_INSERT) & 1) { menuOpen = !menuOpen; SetClickThrough(hwnd, !menuOpen); }

        // F1: toggle ESP activation
        if (GetAsyncKeyState(VK_F1) & 1) {
            espActive = !espActive;
            if (espActive) {
                Stage("[esp] activated");
            } else {
                Stage("[esp] deactivated");
                esp.ResetScan();
            }
        }

        // F6 saves settings
        if (GetAsyncKeyState(VK_F6) & 1) {
            if (esp.SaveSettings(settingsPath))
                Stage("[config] saved settings.ini (F6)");
        }

        // F5 hot-reloads offsets.cfg
        if (GetAsyncKeyState(VK_F5) & 1) {
            cfg::Offsets newOff;
            if (cfg::LoadOffsets(cfgPath, newOff)) {
                offsets = newOff;
                esp.ResetScan();
                Stage("[config] reloaded offsets.cfg (F5)");
            }
        }

        if (g_resizeW && g_resizeH) {
            CleanupRenderTarget();
            g_dx.swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            if (!CreateRenderTarget()) { Stage("[exit] CreateRenderTarget failed after resize"); break; }
        }

        // process attach/detach
        if (proc && !proc.Alive()) {
            proc.Close(); gameWindow = nullptr; Stage("[detach] game closed");
            std::snprintf(diagLine1, sizeof diagLine1, "OYUN KAPANDI — tekrar ac");
            diagLine2[0] = 0; diagAttached = false;
        }
        if (!proc && GetTickCount64() >= nextAttachMs) {
            bool attached = false;
            // try configured name first
            if (offsets.targetProcess[0]) {
                DWORD pid = mem::Process::FindPid(offsets.targetProcess);
                if (pid) {
                    if (proc.Attach(pid)) {
                        Stage("*** OYUN BULUNDU VE BAGLANDI! pid=%lu ***", static_cast<unsigned long>(pid));
                        std::snprintf(diagLine1, sizeof diagLine1, "BAGLANDI! pid=%lu", static_cast<unsigned long>(pid));
                        diagLine2[0] = 0;
                        diagAttached = true;
                        attached = true;
                        if (logFile) { fprintf(logFile, "BAGLANDI pid=%lu\n", static_cast<unsigned long>(pid)); fflush(logFile); }
                    } else {
                        bool canQuery = mem::Process::CanQueryOnly(pid);
                        Stage("!!! OYUN BULUNDU AMA BAGLANAMADI !!! pid=%lu", static_cast<unsigned long>(pid));
                        Stage("!!! NtStatus=0x%08X WinErr=%lu QueryOnly=%s",
                              static_cast<unsigned>(proc.LastNtStatus()),
                              static_cast<unsigned long>(proc.LastWinError()),
                              canQuery ? "EVET(EAC VM_READ engelliyor)" : "HAYIR(tamamen engelli)");
                        std::snprintf(diagLine1, sizeof diagLine1, "EAC ENGELLIYOR! pid=%lu", static_cast<unsigned long>(pid));
                        std::snprintf(diagLine2, sizeof diagLine2, "NtSt=0x%X Err=%lu Q=%s",
                              static_cast<unsigned>(proc.LastNtStatus()),
                              static_cast<unsigned long>(proc.LastWinError()),
                              canQuery ? "EVET" : "HAYIR");
                        if (logFile) { fprintf(logFile, "EAC_ENGEL pid=%lu NtStatus=0x%08X WinErr=%lu QueryOnly=%s\n",
                              static_cast<unsigned long>(pid), static_cast<unsigned>(proc.LastNtStatus()),
                              static_cast<unsigned long>(proc.LastWinError()),
                              canQuery ? "EVET" : "HAYIR"); fflush(logFile); }
                    }
                }
            }
            // try built-in name variants
            if (!attached) {
                auto d0 = xTarget0.dec(); auto d1 = xTarget1.dec();
                auto d2 = xTarget2.dec(); auto d3 = xTarget3.dec();
                struct { const wchar_t* name; const char* label; } tries[] = {
                    {d0.s, "ROB RUN.exe"}, {d1.s, "RobRun.exe"},
                    {d2.s, "Rob Run.exe"}, {d3.s, "robrun.exe"},
                };
                for (auto& t : tries) {
                    DWORD pid = mem::Process::FindPid(t.name);
                    if (pid) {
                        if (proc.Attach(pid)) {
                            Stage("*** OYUN BULUNDU: %s pid=%lu — BAGLANDI! ***", t.label, static_cast<unsigned long>(pid));
                            std::snprintf(diagLine1, sizeof diagLine1, "BAGLANDI! %s pid=%lu", t.label, static_cast<unsigned long>(pid));
                            diagLine2[0] = 0;
                            diagAttached = true;
                            attached = true;
                            if (logFile) { fprintf(logFile, "BAGLANDI %s pid=%lu\n", t.label, static_cast<unsigned long>(pid)); fflush(logFile); }
                            break;
                        } else {
                            bool canQuery = mem::Process::CanQueryOnly(pid);
                            Stage("!!! %s pid=%lu BAGLANAMADI !!!", t.label, static_cast<unsigned long>(pid));
                            Stage("!!! NtStatus=0x%08X WinErr=%lu QueryOnly=%s",
                                  static_cast<unsigned>(proc.LastNtStatus()),
                                  static_cast<unsigned long>(proc.LastWinError()),
                                  canQuery ? "EVET(EAC VM_READ engelliyor)" : "HAYIR(tamamen engelli)");
                            std::snprintf(diagLine1, sizeof diagLine1, "EAC ENGELLIYOR! %s pid=%lu", t.label, static_cast<unsigned long>(pid));
                            std::snprintf(diagLine2, sizeof diagLine2, "NtSt=0x%X Err=%lu Q=%s",
                                  static_cast<unsigned>(proc.LastNtStatus()),
                                  static_cast<unsigned long>(proc.LastWinError()),
                                  canQuery ? "EVET" : "HAYIR");
                            if (logFile) { fprintf(logFile, "EAC_ENGEL %s pid=%lu NtStatus=0x%08X WinErr=%lu QueryOnly=%s\n",
                                  t.label, static_cast<unsigned long>(pid), static_cast<unsigned>(proc.LastNtStatus()),
                                  static_cast<unsigned long>(proc.LastWinError()),
                                  canQuery ? "EVET" : "HAYIR"); fflush(logFile); }
                        }
                    }
                }
            }
            static int failCount = 0;
            if (!attached) {
                ++failCount;
                if (failCount <= 3) {
                    Stage("--- OYUN BULUNAMADI (deneme %d) ---", failCount);
                    if (logFile) { fprintf(logFile, "OYUN_YOK deneme=%d\n", failCount); fflush(logFile); }
                }
            } else {
                failCount = 0;
            }
            nextAttachMs = GetTickCount64() + kAttachGapMs;
        }

        // track game window position
        if (proc) {
            if (!gameWindow || !IsWindow(gameWindow))
                gameWindow = FindGameWindow(proc.Pid());
            if (gameWindow) {
                RECT r;
                if (GetWindowRect(gameWindow, &r)) {
                    int gw = r.right - r.left, gh = r.bottom - r.top;
                    if (gw > 0 && gh > 0)
                        SetWindowPos(hwnd, HWND_TOPMOST, r.left, r.top, gw, gh, SWP_NOACTIVATE);
                }
            }
        }

        // periodic status every 5 seconds
        if (GetTickCount64() >= nextStatusMs) {
            if (proc) {
                Stage("[durum] oyun: BAGLI | esp: %s", espActive ? "ACIK" : "KAPALI");
                if (logFile) { fprintf(logFile, "DURUM bagli esp=%s\n", espActive ? "ACIK" : "KAPALI"); fflush(logFile); }
            }
            else
                Stage("[durum] oyun: BEKLENIYOR... | Oyunu ac, sonra F1 bas");
            nextStatusMs = GetTickCount64() + 5000;
        }

        // update ESP state only when active
        if (proc && espActive) esp.Update();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // menu window
        if (menuOpen) {
            ImGui::SetNextWindowSize(ImVec2(380.f, 420.f), ImGuiCond_FirstUseEver);
            ImGui::Begin("jb overlay##menu", &menuOpen, ImGuiWindowFlags_NoCollapse);
            ImGui::TextColored(ImVec4(0.4f, 0.55f, 1.f, 1.f), "%.0f FPS", static_cast<double>(io.Framerate));
            ImGui::SameLine();
            ImGui::TextDisabled("|  %s  |  %s",
                                proc ? "attached" : "waiting",
                                espActive ? "ACTIVE" : "dormant");
            ImGui::Separator();
            ImGui::TextDisabled("F1: activate  INS: menu  F5: offsets  F6: save  END: exit");
            ImGui::Separator();
            esp.RenderMenu();
            ImGui::End();
            if (!menuOpen) SetClickThrough(hwnd, true);
        }

        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        esp::Vec2 scr{io.DisplaySize.x, io.DisplaySize.y};

        if (espActive) {
            esp.RenderMisc(dl, scr, io.Framerate);
            esp.Render(dl, scr);
            esp.RenderAim(scr);
            esp.RenderWorld(dl, scr);
        }
        esp.RenderStatus(dl, scr, static_cast<bool>(proc), espActive);

        // === BIG ON-SCREEN DIAGNOSTIC ===
        {
            ImFont* font = ImGui::GetFont();
            float bigSize = font->FontSize * 2.0f;

            // line 1: "JB OVERLAY v5 CALISIYOR"
            const char* title = "JB OVERLAY v5 CALISIYOR";
            dl->AddRectFilled(ImVec2(10, 10), ImVec2(600, 110), IM_COL32(0, 0, 0, 200), 8.f);
            dl->AddRect(ImVec2(10, 10), ImVec2(600, 110), IM_COL32(0, 255, 0, 255), 8.f, 0, 2.f);
            dl->AddText(font, bigSize, ImVec2(20, 15), IM_COL32(0, 255, 0, 255), title);

            // line 2: attach status
            ImU32 statusCol = diagAttached ? IM_COL32(0, 255, 0, 255) : IM_COL32(255, 50, 50, 255);
            dl->AddText(font, font->FontSize * 1.5f, ImVec2(20, 55), statusCol, diagLine1);

            // line 3: error details
            if (diagLine2[0])
                dl->AddText(font, font->FontSize * 1.2f, ImVec2(20, 82), IM_COL32(255, 255, 0, 255), diagLine2);
        }

        ImGui::Render();
        g_dx.context->OMSetRenderTargets(1, &g_dx.rtv, nullptr);
        g_dx.context->ClearRenderTargetView(g_dx.rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_dx.swapChain->Present(0, 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            Stage("[dx11] device lost (0x%08X) — rebuilding...", static_cast<unsigned>(hr));
            ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            CleanupDevice();
            Sleep(1000);
            if (!CreateDevice(hwnd)) {
                Stage("[exit] device rebuild failed — exiting");
                break;
            }
            ImGui_ImplWin32_Init(hwnd);
            ImGui_ImplDX11_Init(g_dx.device, g_dx.context);
            Stage("[dx11] device rebuilt OK");
            continue;
        }
    }

    esp.SaveSettings(settingsPath);
    Stage("[config] auto-saved settings.ini");

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(className, hInstance);

    Stage("[exit] clean");
    if (logFile) { fprintf(logFile, "KAPATILDI\n"); fclose(logFile); }
    std::fflush(stdout);
    system("pause");
    FreeConsole();
    return 0;
}
