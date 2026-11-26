// C++20, Win32 + Direct3D 11 + Dear ImGui (win32/dx11 backends)
// Transparent, topmost, click-through overlay covering the primary monitor.
// Insert toggles the menu (and input capture), End exits.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>

#include <cstdarg>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "esp.h"
#include "memory.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

// Boot/stage log to stdout. GUI subsystem has no console of its own, so run
// the exe from a terminal (or redirect) to read it; fflush keeps it live.
void Stage(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stdout, fmt, args);
    va_end(args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

constexpr wchar_t kTargetExe[] = L"ROB RUN.exe";
constexpr ULONGLONG kAttachGapMs = 1000;

struct Dx {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
};

Dx g_dx;
UINT g_resizeW = 0;
UINT g_resizeH = 0;

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

bool CreateRenderTarget() {
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(g_dx.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || !backBuffer)
        return false;
    const HRESULT hr = g_dx.device->CreateRenderTargetView(backBuffer, nullptr, &g_dx.rtv);
    backBuffer->Release();
    return SUCCEEDED(hr);
}

void CleanupRenderTarget() { SafeRelease(g_dx.rtv); }

bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 0;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                                               2, D3D11_SDK_VERSION, &sd, &g_dx.swapChain,
                                               &g_dx.device, &got, &g_dx.context);
    if (hr == DXGI_ERROR_UNSUPPORTED) // no hardware device: fall back to WARP
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_dx.swapChain, &g_dx.device,
                                           &got, &g_dx.context);
    if (FAILED(hr))
        return false;
    return CreateRenderTarget();
}

void CleanupDevice() {
    CleanupRenderTarget();
    SafeRelease(g_dx.swapChain);
    SafeRelease(g_dx.context);
    SafeRelease(g_dx.device);
}

void SetClickThrough(HWND hwnd, bool clickThrough) {
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex = clickThrough ? (ex | WS_EX_TRANSPARENT) : (ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    if (!clickThrough)
        SetForegroundWindow(hwnd);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return 1;

    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_resizeW = LOWORD(lParam);
            g_resizeH = HIWORD(lParam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // swallow Alt menu activation
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    Stage("[boot] wWinMain enter");
    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"dx11_overlay";
    if (!RegisterClassExW(&wc)) {
        Stage("[boot] RegisterClassExW failed: %lu", GetLastError());
        return 1;
    }

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    Stage("[window] creating %dx%d overlay", screenW, screenH);

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
                                wc.lpszClassName, L"overlay", WS_POPUP, 0, 0, screenW, screenH,
                                nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        Stage("[window] CreateWindowExW failed: %lu", GetLastError());
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }
    Stage("[window] hwnd ok");

    // Layered at full opacity; per-pixel transparency comes from DWM + zero-alpha clear.
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    const MARGINS margins{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    Stage("[dx11] creating device + swap chain");
    if (!CreateDevice(hwnd)) {
        Stage("[dx11] CreateDevice failed");
        CleanupDevice();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }
    Stage("[dx11] device ok");

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    Stage("[imgui] init");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // no imgui.ini written next to the exe
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dx.device, g_dx.context);
    Stage("[imgui] backends up");

    mem::Process proc;  // declared before esp: esp holds a reference, destructs first
    esp::Esp esp(proc);
    ULONGLONG nextAttachMs = 0;

    bool menuOpen = false;
    bool running = true;
    const float clear[4] = {0.f, 0.f, 0.f, 0.f};

    Stage("[loop] enter");
    const char* exitReason = "WM_QUIT";
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                running = false;
        }
        if (!running)
            break;

        // Hotkeys polled globally: the window is click-through and normally unfocused.
        if (GetAsyncKeyState(VK_END) & 1) {
            exitReason = "End pressed";
            break;
        }
        if (GetAsyncKeyState(VK_INSERT) & 1) {
            menuOpen = !menuOpen;
            SetClickThrough(hwnd, !menuOpen);
        }

        if (g_resizeW != 0 && g_resizeH != 0) {
            CleanupRenderTarget();
            g_dx.swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            if (!CreateRenderTarget()) {
                exitReason = "CreateRenderTarget failed on resize";
                break;
            }
        }

        if (proc && !proc.Alive())
            proc.Close();
        if (!proc && GetTickCount64() >= nextAttachMs) {
            proc.Attach(kTargetExe);
            nextAttachMs = GetTickCount64() + kAttachGapMs;
        }
        if (proc && esp.Cfg().enabled)
            esp.Update();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (menuOpen) {
            ImGui::SetNextWindowSize(ImVec2(320.f, 200.f), ImGuiCond_FirstUseEver);
            ImGui::Begin("Overlay", &menuOpen);
            ImGui::Text("%.1f FPS", static_cast<double>(io.Framerate));
            ImGui::TextUnformatted("Insert: menu   End: exit");
            ImGui::Text("Target: %s", proc ? "attached" : "waiting");
            esp::Config& cfg = esp.Cfg();
            ImGui::Checkbox("ESP", &cfg.enabled);
            ImGui::Checkbox("FOV circle", &cfg.fovCircle);
            ImGui::SliderFloat("FOV", &cfg.fovRadius, 20.f, 600.f, "%.0f px");
            ImGui::End();
            if (!menuOpen) // closed via the title-bar X
                SetClickThrough(hwnd, true);
        }

        esp.Render(ImGui::GetBackgroundDrawList(), esp::Vec2{io.DisplaySize.x, io.DisplaySize.y});

        ImGui::Render();
        g_dx.context->OMSetRenderTargets(1, &g_dx.rtv, nullptr);
        g_dx.context->ClearRenderTargetView(g_dx.rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const HRESULT hr = g_dx.swapChain->Present(1, 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            exitReason = "Present device lost";
            break;
        }
    }
    Stage("[loop] exit: %s", exitReason);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);
    return 0;
}
