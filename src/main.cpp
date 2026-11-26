#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <winternl.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "esp.h"
#include "memory.h"
#include "offsets.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

// compile-time XOR
constexpr wchar_t xkey = 0x4B2F;
template <size_t N>
struct XW {
    wchar_t d[N];
    constexpr XW(const wchar_t (&s)[N]) { for (size_t i = 0; i < N; i++) d[i] = s[i] ^ xkey; }
    struct Buf { wchar_t s[N]; };
    Buf dec() const { Buf b; for (size_t i = 0; i < N; i++) b.s[i] = d[i] ^ xkey; return b; }
};

static constexpr XW xTarget{L"ROB RUN.exe"};
static constexpr XW xDevPath{L"\\\\.\\WfpDiag"};

void Stage(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    std::vfprintf(stdout, fmt, args);
    va_end(args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
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
// Anti-debug bypass
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

void Apply() {
    HideThread();
    ClearHeaders();
}

}  // namespace bypass

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    Stage("[boot] starting");

    // load offsets config (next to exe)
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
        Stage("[config] created default offsets.cfg - fill in values and restart");
    }

    std::string settingsPath(exePath);
    {
        auto s2 = settingsPath.find_last_of("\\/");
        if (s2 != std::string::npos) settingsPath = settingsPath.substr(0, s2 + 1);
        settingsPath += "settings.ini";
    }

    if (offsets.battleModelTypeInfo == 0)
        Stage("[config] battleModelTypeInfo=0, will auto-detect by class name (slower)");

#if USE_DRIVER
    auto devPath = xDevPath.dec();
    if (mem::Driver::Instance().Open(devPath.s))
        Stage("[driver] connected");
    else
        Stage("[driver] not found - reads will fail");
#endif

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
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
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
    Stage("[bypass] applied");

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    // dark theme tweaks
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

    mem::Process proc;
    esp::Esp esp(proc, offsets);
    if (esp.LoadSettings(settingsPath))
        Stage("[config] loaded settings.ini");
    ULONGLONG nextAttachMs = 0;

    bool menuOpen = false;
    bool running  = true;
    const float clear[4] = {0.f, 0.f, 0.f, 0.f};

    Stage("[loop] enter");
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (GetAsyncKeyState(VK_END) & 1) break;
        if (GetAsyncKeyState(VK_INSERT) & 1) { menuOpen = !menuOpen; SetClickThrough(hwnd, !menuOpen); }

        if (g_resizeW && g_resizeH) {
            CleanupRenderTarget();
            g_dx.swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            if (!CreateRenderTarget()) break;
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

        if (proc && !proc.Alive()) proc.Close();
        if (!proc && GetTickCount64() >= nextAttachMs) {
            auto target = xTarget.dec();
            proc.Attach(target.s);
            nextAttachMs = GetTickCount64() + kAttachGapMs;
        }
        if (proc) esp.Update();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // menu window
        if (menuOpen) {
            ImGui::SetNextWindowSize(ImVec2(380.f, 420.f), ImGuiCond_FirstUseEver);
            ImGui::Begin("jb overlay##menu", &menuOpen, ImGuiWindowFlags_NoCollapse);
            ImGui::TextColored(ImVec4(0.4f, 0.55f, 1.f, 1.f), "%.0f FPS", static_cast<double>(io.Framerate));
            ImGui::SameLine();
            ImGui::TextDisabled("|  %s  |  INS: menu  END: exit  F5: offsets  F6: save",
                                proc ? "attached" : "waiting");
            ImGui::Separator();
            esp.RenderMenu();
            ImGui::End();
            if (!menuOpen) SetClickThrough(hwnd, true);
        }

        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        esp::Vec2 scr{io.DisplaySize.x, io.DisplaySize.y};

        esp.RenderMisc(dl, scr, io.Framerate);
        esp.Render(dl, scr);
        esp.RenderAim(scr);
        esp.RenderWorld(dl, scr);
        esp.RenderStatus(dl, scr, static_cast<bool>(proc));

        ImGui::Render();
        g_dx.context->OMSetRenderTargets(1, &g_dx.rtv, nullptr);
        g_dx.context->ClearRenderTargetView(g_dx.rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_dx.swapChain->Present(1, 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) break;
    }

    esp.SaveSettings(settingsPath);
    Stage("[config] auto-saved settings.ini");

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(className, hInstance);

#if USE_DRIVER
    mem::Driver::Instance().Close();
#endif
    Stage("[exit] clean");
    return 0;
}
