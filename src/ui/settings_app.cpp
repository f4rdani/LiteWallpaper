#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "settings_app.h"
#include "ui_internal.h"
#include "ui_modals.h"
#include "ui_pages.h"
#include "ui_widgets.h"
#include "thumbnail_manager.h"
#include "icons_fontawesome6.h"

#include "core/config.h"
#include "core/engine_controller.h"
#include "core/engine_state.h"
#include "core/logger.h"
#include "core/video_optimizer.h"
#include "core/win_utils.h"
#include "platform/win32/d3d11_presenter.h"
#include "platform/win32/hardware_info.h"

#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#if __has_include(<dxgi1_4.h>)
#include <dxgi1_4.h>
#endif
#include <oleidl.h>
#include <psapi.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <mimalloc.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace miniwp {

namespace ui {

UiContext g_ui;

void MarkUiInteraction() noexcept {
    g_ui.lastInteractionTickMs = GetTickCount64();
}

void EnsureHardwareInfoLoaded() {
    if (!g_ui.hardwareInfoInit) {
        g_ui.hardwareInfo = HardwareDetector::QuerySystemInfo();
        g_ui.hardwareInfoInit = true;
    }
}

void ApplyAction(const std::string& utf8_path, const std::string& action) {
    if (action == "stop") {
        EngineController::StopToStatic();
        g_ui.daemon.playing = false;
        g_ui.daemon.paused = false;
        g_ui.daemon.currentVideo.clear();
        return;
    }
    if (action == "resume") {
        EngineController::Resume();
        g_ui.daemon.playing = true;
        g_ui.daemon.paused = false;
        return;
    }

    if (utf8_path.empty()) return;
    if (!IsOptimizedCachePath(utf8_path)) {
        g_config.Get().AddToGallery(utf8_path);
        g_config.Save();
        InvalidateGalleryCache();
    }

    if (action == "wallpaper") {
        EngineController::SetWallpaper(utf8_path);
        g_ui.daemon.currentVideo = utf8_path;
        g_ui.daemon.playing = true;
        g_ui.daemon.paused = false;
    }
}

void StartVideoOptimization(
    const std::string& input_path,
    int target_w,
    int target_h,
    const std::string& action,
    int crop_mode
) {
    g_config.Get().AddToGallery(input_path);
    g_config.Save();
    InvalidateGalleryCache();
    MarkUiInteraction();

    g_video_optimizer.StartOptimizeAsync(
        input_path,
        target_w,
        target_h,
        crop_mode,
        nullptr,
        [action](bool success, const std::string& output_path) {
            EngineController::PostToMainThread([action, success, output_path]() {
                InvalidateGalleryCache();
                if (success && !output_path.empty()) {
                    ApplyAction(output_path, action);
                }
            });
        }
    );
}

void RequestApplyVideo(const std::string& utf8_path, const std::string& action) {
    if (action == "stop" || action == "resume") {
        ApplyAction(utf8_path, action);
        return;
    }
    if (utf8_path.empty()) return;

    g_config.Get().AddToGallery(utf8_path);
    g_config.Save();
    InvalidateGalleryCache();

    auto& cfg = g_config.Get();
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    if (screen_w <= 0) screen_w = 1920;
    if (screen_h <= 0) screen_h = 1080;

    auto probe = VideoOptimizer::Probe(utf8_path);
    auto [target_w, target_h] = VideoOptimizer::CalculateTargetDimensions(
        probe.width, probe.height, screen_w, screen_h, cfg.optimizer_crop_mode
    );

    if (VideoOptimizer::HasOptimizedCache(utf8_path, target_w, target_h)) {
        std::string opt_path = VideoOptimizer::GetOptimizedPath(utf8_path, target_w, target_h);
        ApplyAction(opt_path, action);
        return;
    }

    if (probe.valid && (probe.width > screen_w || probe.height > screen_h || probe.fps > 60.0)) {
        if (cfg.prompt_downscale) {
            OpenOptimizeModal(utf8_path, action, probe.width, probe.height, target_w, target_h);
            return;
        }
        if (cfg.auto_downscale_highres) {
            StartVideoOptimization(utf8_path, target_w, target_h, action, cfg.optimizer_crop_mode);
            return;
        }
    }

    ApplyAction(utf8_path, action);
}

} // namespace ui

namespace {

static HINSTANCE               g_hInstance = nullptr;
static IDXGISwapChain*         g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static bool                    g_isOpen = false;
static bool                    g_ownsD3DDevice = false;
static bool                    g_inRenderFrame = false;
static bool                    g_closeRequested = false;

static IDropTarget*            g_dropTarget = nullptr;
static bool                    g_dropComInit = false;

// Shared icon font buffer kept alive across ImGui 1.92+ dynamic glyph baking
// and released immediately after ImGui::DestroyContext() on panel close.
static std::vector<uint8_t>    g_iconFontBytes;

bool IsSupportedVideoExtension(const fs::path& filePath) {
    std::string ext = PathToUtf8(filePath.extension());
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return (ext == ".mp4" || ext == ".webm" || ext == ".mkv" || ext == ".avi" || ext == ".mov");
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (g_pSwapChain && SUCCEEDED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)))) {
        ui::g_ui.device->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

bool CreateDeviceD3D(HWND hWnd) {
    ID3D11Device* sharedDevice = GetSharedD3DDevice();
    ID3D11DeviceContext* sharedContext = GetSharedD3DDeviceContext();

    if (sharedDevice && sharedContext) {
        ui::g_ui.device = sharedDevice;
        ui::g_ui.device->AddRef();
        ui::g_ui.deviceContext = sharedContext;
        ui::g_ui.deviceContext->AddRef();
        g_ownsD3DDevice = false;

        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        if (SUCCEEDED(ui::g_ui.device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
            SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {

            ComPtr<IDXGIFactory2> factory2;
            if (SUCCEEDED(factory.As(&factory2))) {
                DXGI_SWAP_CHAIN_DESC1 sd1 = {};
                sd1.Width = 0;
                sd1.Height = 0;
                sd1.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                sd1.Stereo = FALSE;
                sd1.SampleDesc.Count = 1;
                sd1.SampleDesc.Quality = 0;
                sd1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                sd1.BufferCount = 2;
                sd1.Scaling = DXGI_SCALING_STRETCH;
                sd1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                sd1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                sd1.Flags = 0;

                ComPtr<IDXGISwapChain1> sc1;
                if (SUCCEEDED(factory2->CreateSwapChainForHwnd(ui::g_ui.device, hWnd, &sd1, nullptr, nullptr, &sc1))) {
                    g_pSwapChain = sc1.Detach();
                    factory2->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);
                    CreateRenderTarget();
                    return true;
                }
            }

            DXGI_SWAP_CHAIN_DESC sd = {};
            sd.BufferCount = 2;
            sd.BufferDesc.Width = 0;
            sd.BufferDesc.Height = 0;
            sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.BufferDesc.RefreshRate.Numerator = 60;
            sd.BufferDesc.RefreshRate.Denominator = 1;
            sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.OutputWindow = hWnd;
            sd.SampleDesc.Count = 1;
            sd.SampleDesc.Quality = 0;
            sd.Windowed = TRUE;
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

            if (SUCCEEDED(factory->CreateSwapChain(ui::g_ui.device, &sd, &g_pSwapChain))) {
                CreateRenderTarget();
                return true;
            }
        }

        ui::g_ui.deviceContext->Release();
        ui::g_ui.deviceContext = nullptr;
        ui::g_ui.device->Release();
        ui::g_ui.device = nullptr;
    }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        featureLevelArray,
        2,
        D3D11_SDK_VERSION,
        &sd,
        &g_pSwapChain,
        &ui::g_ui.device,
        &featureLevel,
        &ui::g_ui.deviceContext
    );

    if (res != S_OK) {
        return false;
    }

    g_ownsD3DDevice = true;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (ui::g_ui.deviceContext) { ui::g_ui.deviceContext->Release(); ui::g_ui.deviceContext = nullptr; }
    if (ui::g_ui.device) { ui::g_ui.device->Release(); ui::g_ui.device = nullptr; }
    g_ownsD3DDevice = false;
}

void FetchDaemonStatus() {
    static uint64_t s_lastFetchTick = 0;
    uint64_t now = GetTickCount64();
    if (now - s_lastFetchTick < 200) return; // 5Hz sampling rate avoids CPU busywork
    s_lastFetchTick = now;

    auto& d = ui::g_ui.daemon;
    d.connected = g_shared_engine_state.connected.load();
    d.playing = g_shared_engine_state.playing.load();
    d.paused = g_shared_engine_state.paused.load();
    d.currentVideo = g_shared_engine_state.GetCurrentVideo();
    d.fps = g_shared_engine_state.fps.load();
    d.videoFps = g_shared_engine_state.video_fps.load();
    d.width = g_shared_engine_state.width.load();
    d.height = g_shared_engine_state.height.load();
    d.duration = g_shared_engine_state.duration.load();
    d.currentTimeSec = g_shared_engine_state.current_time_sec.load();
    d.codec = g_shared_engine_state.GetCodec();
    d.ramMB = g_shared_engine_state.ram_mb.load();
    d.ramPrivateMB = g_shared_engine_state.ram_private_mb.load();
    d.vramMB = g_shared_engine_state.vram_mb.load();
    d.cpuPercent = g_shared_engine_state.cpu_percent.load();
    d.injected = g_shared_engine_state.injected.load();
    d.hwDecode = g_shared_engine_state.hw_decode.load();
    d.activeGpuIndex = g_shared_engine_state.active_gpu_index.load();
    d.gpuName = g_shared_engine_state.GetActiveGpuName();
    d.gpuType = g_shared_engine_state.GetActiveGpuType();
    d.framesRendered = g_shared_engine_state.frames_rendered.load();
    d.lastError = g_shared_engine_state.GetLastError();

    if (d.ramMB == 0) {
        PROCESS_MEMORY_COUNTERS_EX pmc = {};
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            d.ramMB = pmc.WorkingSetSize / (1024 * 1024);
            d.ramPrivateMB = pmc.PrivateUsage / (1024 * 1024);
        }
    }
}

class GalleryDropTarget : public IDropTarget {
public:
    GalleryDropTarget() : m_ref(1) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObj, DWORD, POINTL, DWORD* pdwEffect) override {
        bool ok = HasHDrop(pDataObj);
        if (pdwEffect) *pdwEffect = ok ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (ok) {
            ui::g_ui.dragOverlay = true;
            ui::MarkUiInteraction();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* pdwEffect) override {
        if (pdwEffect) *pdwEffect = ui::g_ui.dragOverlay ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        ui::MarkUiInteraction();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        ui::g_ui.dragOverlay = false;
        ui::MarkUiInteraction();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj, DWORD, POINTL, DWORD* pdwEffect) override {
        ui::g_ui.dragOverlay = false;
        ui::MarkUiInteraction();
        if (pdwEffect) *pdwEffect = DROPEFFECT_COPY;
        if (pDataObj) {
            FORMATETC f = {};
            f.cfFormat = static_cast<CLIPFORMAT>(CF_HDROP);
            f.dwAspect = DVASPECT_CONTENT;
            f.lindex = -1;
            f.tymed = TYMED_HGLOBAL;
            STGMEDIUM m = {};
            if (SUCCEEDED(pDataObj->GetData(&f, &m)) && m.hGlobal) {
                HDROP hDrop = static_cast<HDROP>(GlobalLock(m.hGlobal));
                if (hDrop) {
                    UINT n = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
                    for (UINT i = 0; i < n; i++) {
                        wchar_t fp[1024] = {};
                        if (DragQueryFileW(hDrop, i, fp, static_cast<UINT>(IM_ARRAYSIZE(fp)))) {
                            fs::path droppedPath(fp);
                            if (IsSupportedVideoExtension(droppedPath)) {
                                ui::RequestApplyVideo(WideToUtf8(fp), "wallpaper");
                                break;
                            }
                        }
                    }
                    GlobalUnlock(m.hGlobal);
                }
                ReleaseStgMedium(&m);
            }
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> m_ref{1};

    static bool HasHDrop(IDataObject* obj) {
        if (!obj) return false;
        FORMATETC f = {};
        f.cfFormat = static_cast<CLIPFORMAT>(CF_HDROP);
        f.dwAspect = DVASPECT_CONTENT;
        f.lindex = -1;
        f.tymed = TYMED_HGLOBAL;
        return obj->QueryGetData(&f) == S_OK;
    }
};

LRESULT WINAPI SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_MOUSEMOVE:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR:
        case WM_ACTIVATE:
            ui::MarkUiInteraction();
            break;
        default:
            break;
    }

    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return true;
    }

    switch (msg) {
        case WM_NCCALCSIZE: {
            if (wParam) {
                if (IsZoomed(hWnd)) {
                    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                    HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi = { sizeof(mi) };
                    if (GetMonitorInfoW(hMon, &mi)) {
                        params->rgrc[0] = mi.rcWork;
                    }
                }
                return 0;
            }
            return DefWindowProcW(hWnd, msg, wParam, lParam);
        }

        case WM_NCHITTEST: {
            if (IsZoomed(hWnd)) return HTCLIENT;

            POINT pt = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            ScreenToClient(hWnd, &pt);
            RECT cr;
            GetClientRect(hWnd, &cr);
            const int border = 8;
            bool left = (pt.x < border);
            bool right = (pt.x >= cr.right - border);
            bool top = (pt.y < border);
            bool bottom = (pt.y >= cr.bottom - border);

            if (top && pt.x >= cr.right - 145) return HTCLIENT;

            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (bottom) return HTBOTTOM;
            if (top) return HTTOP;

            return HTCLIENT;
        }

        case WM_SIZE:
            ui::MarkUiInteraction();
            if (wParam == SIZE_MINIMIZED) {
                SettingsUI::Close();
                return 0;
            }
            if (ui::g_ui.device != nullptr && g_pSwapChain != nullptr && wParam != SIZE_MINIMIZED) {
                CleanupRenderTarget();
                g_pSwapChain->ResizeBuffers(0, static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;

        case WM_DROPFILES: {
            ui::MarkUiInteraction();
            HDROP hDrop = reinterpret_cast<HDROP>(wParam);
            UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < fileCount; i++) {
                wchar_t filePath[1024] = {};
                if (DragQueryFileW(hDrop, i, filePath, static_cast<UINT>(IM_ARRAYSIZE(filePath)))) {
                    fs::path droppedPath(filePath);
                    if (IsSupportedVideoExtension(droppedPath)) {
                        ui::RequestApplyVideo(WideToUtf8(filePath), "wallpaper");
                        break;
                    }
                }
            }
            DragFinish(hDrop);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = static_cast<LONG>(600 * ui::g_ui.dpiScale);
            mmi->ptMinTrackSize.y = static_cast<LONG>(420 * ui::g_ui.dpiScale);
            return 0;
        }

        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;

        case WM_CLOSE:
            SettingsUI::Close();
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ReleaseUiStack() {
    g_config.FlushIfDirty(true);

    if (!ui::g_ui.hWnd && !ui::g_ui.device && !g_pSwapChain && !ImGui::GetCurrentContext()) {
        return;
    }

    if (g_dropTarget) {
        if (ui::g_ui.hWnd) RevokeDragDrop(ui::g_ui.hWnd);
        g_dropTarget->Release();
        g_dropTarget = nullptr;
    }
    if (g_dropComInit) {
        OleUninitialize();
        g_dropComInit = false;
    }
    ui::g_ui.dragOverlay = false;

    g_isOpen = false;

    ui::ShutdownCaptureModalWorker();
    ThumbnailManager::Instance().ReleaseTextures();
    ui::ReleaseAppLogoTexture();
    ui::ClearGalleryCardCache();
    ui::ResetUiAnimations();

    if (ImGui::GetCurrentContext()) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    ui::g_ui.fontSemibold = nullptr;
    std::vector<uint8_t>().swap(g_iconFontBytes);

    CleanupDeviceD3D();

    if (ui::g_ui.hWnd) {
        DestroyWindow(ui::g_ui.hWnd);
        UnregisterClassW(L"MiniWallpaper_SettingsClass", g_hInstance);
        ui::g_ui.hWnd = nullptr;
    }

    mi_collect(true);
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

struct UiFrameScope {
    UiFrameScope() { g_inRenderFrame = true; }
    ~UiFrameScope() {
        g_inRenderFrame = false;
        if (g_closeRequested) {
            g_closeRequested = false;
            ReleaseUiStack();
        }
    }
};

} // namespace

bool SettingsUI::Open(HINSTANCE hInstance) {
    if (ui::g_ui.hWnd) {
        ShowWindow(ui::g_ui.hWnd, SW_RESTORE);
        ShowWindow(ui::g_ui.hWnd, SW_SHOW);
        SetForegroundWindow(ui::g_ui.hWnd);
        g_isOpen = true;
        ui::MarkUiInteraction();
        return true;
    }

    g_hInstance = hInstance;

    // LR_SHARED prevents GDI HICON leaks across repeated Open/Close cycles
    HICON hIconBig = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
        LR_DEFAULTCOLOR | LR_SHARED
    ));
    HICON hIconSmall = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR | LR_SHARED
    ));
    if (!hIconBig) hIconBig = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    if (!hIconSmall) hIconSmall = hIconBig;

    WNDCLASSEXW wc = {
        sizeof(wc),
        CS_CLASSDC,
        SettingsWndProc,
        0L, 0L,
        hInstance,
        hIconBig,
        LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)), // IDC_ARROW
        nullptr, nullptr,
        L"MiniWallpaper_SettingsClass",
        hIconSmall
    };
    RegisterClassExW(&wc);

    float dpiScale = 1.0f;
    {
        UINT dpi = 96;
        if (HDC hdc = GetDC(nullptr)) {
            int v = GetDeviceCaps(hdc, LOGPIXELSX);
            if (v > 0) dpi = static_cast<UINT>(v);
            ReleaseDC(nullptr, hdc);
        }
        if (HMODULE hU32 = GetModuleHandleW(L"user32.dll")) {
            using GetSysDpiFn = UINT(WINAPI*)();
            if (auto fn = reinterpret_cast<GetSysDpiFn>(GetProcAddress(hU32, "GetDpiForSystem"))) {
                UINT d2 = fn();
                if (d2 > 0) dpi = d2;
            }
        }
        if (dpi < 96) dpi = 96;
        dpiScale = static_cast<float>(dpi) / 96.0f;
    }
    ui::g_ui.dpiScale = dpiScale;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int winW = static_cast<int>(1060 * dpiScale);
    int winH = static_cast<int>(730 * dpiScale);
    int winX = (sw > winW) ? (sw - winW) / 2 : 100;
    int winY = (sh > winH) ? (sh - winH) / 2 : 100;

    ui::g_ui.hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        L"MiniWallpaper v2.0.0",
        WS_OVERLAPPEDWINDOW,
        winX, winY, winW, winH,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (!ui::g_ui.hWnd || !CreateDeviceD3D(ui::g_ui.hWnd)) {
        CleanupDeviceD3D();
        if (ui::g_ui.hWnd) DestroyWindow(ui::g_ui.hWnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        ui::g_ui.hWnd = nullptr;
        return false;
    }

    SetWindowPos(ui::g_ui.hWnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(ui::g_ui.hWnd, &margins);

    int corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(ui::g_ui.hWnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));

    int backdrop = 3; // DWMSBT_TRANSIENTWINDOW (Acrylic)
    DwmSetWindowAttribute(ui::g_ui.hWnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &backdrop, sizeof(backdrop));

    enum ACCENT_STATE {
        ACCENT_ENABLE_ACRYLICBLURBEHIND = 4
    };
    struct ACCENT_POLICY {
        ACCENT_STATE AccentState;
        int AccentFlags;
        unsigned int GradientColor;
        int AnimationId;
    };
    struct WINCOMPATTRDATA {
        int nAttribute;
        void* pData;
        ULONG ulDataSize;
    };
    using pfnSetWindowCompositionAttribute = BOOL(WINAPI*)(HWND, WINCOMPATTRDATA*);
    if (HMODULE hUser = GetModuleHandleW(L"user32.dll")) {
        if (auto setAttr = reinterpret_cast<pfnSetWindowCompositionAttribute>(GetProcAddress(hUser, "SetWindowCompositionAttribute"))) {
            ACCENT_POLICY policy = { ACCENT_ENABLE_ACRYLICBLURBEHIND, 2, 0x80101826, 0 };
            WINCOMPATTRDATA data = { 19 /* WCA_ACCENT_POLICY */, &policy, sizeof(policy) };
            setAttr(ui::g_ui.hWnd, &data);
        }
    }

    SendMessageW(ui::g_ui.hWnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
    SendMessageW(ui::g_ui.hWnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));

    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(ui::g_ui.hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    COLORREF captionColor = RGB(20, 20, 26);
    DwmSetWindowAttribute(ui::g_ui.hWnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));

    DragAcceptFiles(ui::g_ui.hWnd, TRUE);

    {
        DWM_BLURBEHIND bb = {};
        bb.dwFlags = DWM_BB_ENABLE;
        bb.fEnable = TRUE;
        DwmEnableBlurBehindWindow(ui::g_ui.hWnd, &bb);
    }

    if (SUCCEEDED(OleInitialize(nullptr))) g_dropComInit = true;
    if (GalleryDropTarget* dt = new GalleryDropTarget()) {
        if (SUCCEEDED(RegisterDragDrop(ui::g_ui.hWnd, dt))) g_dropTarget = dt;
        else dt->Release();
    }

    ShowWindow(ui::g_ui.hWnd, SW_SHOW);
    UpdateWindow(ui::g_ui.hWnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Disable imgui.ini & debug log files (no unwanted disk polling/artifacts next to .exe)
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ui::SetupImGuiStyle();
    if (dpiScale > 1.01f) ImGui::GetStyle().ScaleAllSizes(dpiScale);

    ImFontConfig main_cfg;
    main_cfg.OversampleH = 1;
    main_cfg.OversampleV = 1;
    main_cfg.PixelSnapH = true;
    main_cfg.RasterizerMultiply = 1.12f;

    ImFont* mainFont = nullptr;
    std::error_code fontEc;
    if (fs::exists(L"C:\\Windows\\Fonts\\segoeui.ttf", fontEc)) {
        mainFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f * dpiScale, &main_cfg);
    }

    ImFontConfig bold_cfg = main_cfg;
    bold_cfg.RasterizerMultiply = 1.0f;
    if (fs::exists(L"C:\\Windows\\Fonts\\segoeuisb.ttf", fontEc)) {
        ui::g_ui.fontSemibold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuisb.ttf", 17.0f * dpiScale, &bold_cfg);
    }
    if (!mainFont) {
        io.Fonts->AddFontDefault();
    }

    // Minimal FontAwesome subset actually drawn by MiniWallpaper UI
    static const ImWchar icons_ranges[] = {
        0xf002, 0xf008, 0xf00c, 0xf00d, 0xf013, 0xf019, 0xf021, 0xf023,
        0xf028, 0xf030, 0xf03e, 0xf04b, 0xf04c, 0xf04d, 0xf058, 0xf05a,
        0xf05b, 0xf065, 0xf067, 0xf070, 0xf071, 0xf07b, 0xf07c, 0xf085,
        0xf0a0, 0xf0c7, 0xf0e7, 0xf108, 0xf144, 0xf1de, 0xf1f8, 0xf201,
        0xf242, 0xf2db, 0xf302, 0xf4c2, 0xf51f, 0xf5fe, 0xf625, 0xf6a9,
        0
    };

    fs::path fontPath = GetExecutableDir() / L"assets" / L"fa-solid-900.ttf";
    if (!fs::exists(fontPath, fontEc)) {
        fontPath = fs::path(L"assets") / L"fa-solid-900.ttf";
    }

    g_iconFontBytes.clear();
    if (fs::exists(fontPath, fontEc)) {
        std::ifstream fin(fontPath, std::ios::binary | std::ios::ate);
        if (fin.is_open()) {
            auto sz = fin.tellg();
            if (sz > 0) {
                g_iconFontBytes.resize(static_cast<size_t>(sz));
                fin.seekg(0, std::ios::beg);
                fin.read(reinterpret_cast<char*>(g_iconFontBytes.data()), sz);
            }
        }
    }

    if (!g_iconFontBytes.empty()) {
        ImFontConfig icons_config;
        icons_config.MergeMode = true;
        icons_config.PixelSnapH = true;
        icons_config.GlyphMinAdvanceX = 16.0f;
        icons_config.OversampleH = 1;
        icons_config.OversampleV = 1;
        icons_config.FontDataOwnedByAtlas = false; // Share single g_iconFontBytes buffer across both fonts!

        io.Fonts->AddFontFromMemoryTTF(
            g_iconFontBytes.data(),
            static_cast<int>(g_iconFontBytes.size()),
            15.0f * dpiScale,
            &icons_config,
            icons_ranges
        );
        if (ui::g_ui.fontSemibold) {
            ImFontConfig icons_config2 = icons_config;
            io.Fonts->AddFontFromMemoryTTF(
                g_iconFontBytes.data(),
                static_cast<int>(g_iconFontBytes.size()),
                15.0f * dpiScale,
                &icons_config2,
                icons_ranges
            );
        }
    }

    ImGui_ImplWin32_Init(ui::g_ui.hWnd);
    ImGui_ImplDX11_Init(ui::g_ui.device, ui::g_ui.deviceContext);

    g_config.FlushIfDirty(true);
    if (g_config.Get().config_path.empty()) {
        g_config.Load();
    }
    ui::InvalidateGalleryCache();
    ui::MarkUiInteraction();
    g_isOpen = true;
    return true;
}

uint32_t SettingsUI::GetPreferredWaitMs() noexcept {
    if (!g_isOpen || !ui::g_ui.hWnd || !IsWindowVisible(ui::g_ui.hWnd) || IsIconic(ui::g_ui.hWnd)) {
        return 250;
    }

    if (GetForegroundWindow() != ui::g_ui.hWnd) {
        if (g_video_optimizer.IsRunning() || ThumbnailManager::Instance().HasPendingWork()) {
            return 50;
        }
        return 100; // Unfocused window: ~10 FPS
    }

    uint64_t nowMs = GetTickCount64();
    bool recentInput = (nowMs - ui::g_ui.lastInteractionTickMs) < 500;
    bool activeWork = recentInput ||
                      ui::g_ui.animActiveThisFrame ||
                      ui::g_ui.dragOverlay ||
                      ui::IsAnyModalActive() ||
                      g_video_optimizer.IsRunning() ||
                      ThumbnailManager::Instance().HasPendingWork();

    if (!activeWork && ImGui::GetCurrentContext() && ImGui::IsAnyItemActive()) {
        activeWork = true;
    }

    if (activeWork) {
        return 16; // 60 FPS during any interaction, animation, modal, or background task
    }

    if (ui::g_ui.activePage == 3) {
        return 33; // ~30 FPS on Performance tab when idle so Starlume shimmer stays smooth at half GPU cost
    }

    return 66; // ~15 FPS when sitting completely static & untouched
}

void SettingsUI::RenderFrame() {
    if (!g_isOpen || !ui::g_ui.hWnd || !IsWindowVisible(ui::g_ui.hWnd) || IsIconic(ui::g_ui.hWnd) || !ui::g_ui.deviceContext || !g_mainRenderTargetView) {
        return;
    }

    UiFrameScope frameScope;
    ui::g_ui.animActiveThisFrame = false;

    EngineController::DrainMainThreadTasks();
    g_config.FlushIfDirty(false);
    FetchDaemonStatus();
    ThumbnailManager::Instance().Update(ui::g_ui.device);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    RECT rect;
    GetClientRect(ui::g_ui.hWnd, &rect);
    int winWidth = rect.right - rect.left;
    int winHeight = rect.bottom - rect.top;

    static bool s_firstLog = true;
    if (s_firstLog) {
        s_firstLog = false;
        Logger::Info("RenderFrame: winWidth=", winWidth, " winHeight=", winHeight, " activePage=", ui::g_ui.activePage);
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(winWidth), static_cast<float>(winHeight)));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("MiniWallpaper Control Panel", nullptr, flags);
    ImGui::PopStyleVar(2);

    ui::RenderCustomTitleBar(static_cast<float>(winWidth), 38.0f);
    ui::RenderOptimizationProgress();

    ImVec2 pageAvail = ImGui::GetContentRegionAvail();
    float pageH = pageAvail.y - 2.0f;
    if (pageH < 200.0f) pageH = pageAvail.y > 0 ? pageAvail.y : 200.0f;
    ImGuiWindowFlags pageFlags = (ui::g_ui.activePage == 0) ? (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse) : 0;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 16.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("PageHost", ImVec2(0, pageH), false, pageFlags);
    if (ui::g_ui.activePage == 0) ui::RenderGalleryTab();
    else if (ui::g_ui.activePage == 1) ui::RenderLockscreenTab();
    else if (ui::g_ui.activePage == 2) ui::RenderGeneralSettingsTab();
    else ui::RenderPerformancePanel();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ui::RenderFloatingDock();

    if (ui::g_ui.dragOverlay) {
        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 ws = ImGui::GetWindowSize();
        ImDrawList* odl = ImGui::GetWindowDrawList();
        odl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), IM_COL32(9, 12, 21, 190));
        ImVec2 f0 = ImVec2(wp.x + 56.0f, wp.y + 64.0f);
        ImVec2 f1 = ImVec2(wp.x + ws.x - 56.0f, wp.y + ws.y - 64.0f);
        odl->AddRect(f0, f1, IM_COL32(56, 232, 255, 235), 20.0f, 0, 3.0f);
        odl->AddRect(ImVec2(f0.x + 9.0f, f0.y + 9.0f), ImVec2(f1.x - 9.0f, f1.y - 9.0f), IM_COL32(56, 232, 255, 70), 14.0f, 0, 1.5f);
        ImVec2 ctr = ImVec2((f0.x + f1.x) * 0.5f, (f0.y + f1.y) * 0.5f);
        const char* dropTitle = "Drop video here";
        const char* dropSub = "MP4  /  WebM  /  MKV  /  AVI  /  MOV";
        ImGui::SetWindowFontScale(2.4f);
        ImVec2 tIco = ImGui::CalcTextSize(ICON_FA_ARROW_DOWN_TO_LINE);
        ImGui::SetWindowFontScale(1.0f);
        ImVec2 tTitle = ImGui::CalcTextSize(dropTitle);
        ImVec2 tSub = ImGui::CalcTextSize(dropSub);
        float gap1 = 18.0f, gap2 = 10.0f;
        float blockH = tIco.y + gap1 + tTitle.y + gap2 + tSub.y;
        float cy = ctr.y - blockH * 0.5f;
        ImGui::SetWindowFontScale(2.4f);
        ImGui::SetCursorScreenPos(ImVec2(ctr.x - tIco.x * 0.5f, cy));
        ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f), ICON_FA_ARROW_DOWN_TO_LINE);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::SetCursorScreenPos(ImVec2(ctr.x - tTitle.x * 0.5f, cy + tIco.y + gap1));
        ImGui::TextColored(ImVec4(0.945f, 0.961f, 0.976f, 1.0f), "%s", dropTitle);
        ImGui::SetCursorScreenPos(ImVec2(ctr.x - tSub.x * 0.5f, cy + tIco.y + gap1 + tTitle.y + gap2));
        ImGui::TextColored(ImVec4(0.580f, 0.639f, 0.722f, 1.0f), "%s", dropSub);
    }

    ImDrawList* wdl = ImGui::GetWindowDrawList();
    wdl->AddRect(ImVec2(0, 0), ImVec2(static_cast<float>(winWidth), static_cast<float>(winHeight)), IM_COL32(56, 232, 255, 45), 0.0f, 0, 1.0f);

    ui::RenderOptimizeModal();
    ui::RenderCaptureFrameModal();

    ImGui::End();

    ImGui::Render();
    const float clearColor[4] = { 0.032f, 0.045f, 0.078f, 0.50f };
    ui::g_ui.deviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    ui::g_ui.deviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    g_pSwapChain->Present(1, 0);

    static bool s_atlasLogged = false;
    if (!s_atlasLogged) {
        s_atlasLogged = true;
        int tw = 0, th = 0;
        SettingsUI::GetFontAtlasSize(tw, th);
        Logger::Info("[UI] ImGui font atlas: ", tw, "x", th, " (", (tw * th * 4) / 1024, " KB CPU-side RGBA)");
    }
}

bool SettingsUI::IsOpen() {
    return g_isOpen && ui::g_ui.hWnd && IsWindowVisible(ui::g_ui.hWnd);
}

void SettingsUI::Close() {
    if (!ui::g_ui.hWnd) {
        g_isOpen = false;
        g_config.FlushIfDirty(true);
        return;
    }
    if (g_inRenderFrame) {
        g_closeRequested = true;
        g_isOpen = false;
        return;
    }
    ReleaseUiStack();
}

void SettingsUI::Shutdown() {
    ReleaseUiStack();
}

HWND SettingsUI::GetHwnd() {
    return ui::g_ui.hWnd;
}

void SettingsUI::SetActivePage(int page) {
    if (page >= 0 && page < 4) {
        ui::g_ui.activePage = page;
        ui::MarkUiInteraction();
    }
}

int SettingsUI::GetActivePage() {
    return ui::g_ui.activePage;
}

void SettingsUI::SetGalleryDetailVisible(bool visible, int selectIdx) {
    ui::g_ui.detailVisible = visible;
    if (visible && selectIdx >= 0) {
        ui::g_ui.selectedGalleryIdx = selectIdx;
    }
    ui::MarkUiInteraction();
}

void SettingsUI::GetFontAtlasSize(int& width, int& height) {
    width = 0;
    height = 0;
    if (!ImGui::GetCurrentContext()) return;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    if (atlas && atlas->TexData) {
        width = atlas->TexData->Width;
        height = atlas->TexData->Height;
    }
}

bool SettingsUI::SaveScreenshot(const std::string& path) {
    if (!ui::g_ui.device || !ui::g_ui.deviceContext || !g_pSwapChain) return false;

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;

    D3D11_TEXTURE2D_DESC desc;
    backBuffer->GetDesc(&desc);

    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> stagingTex;
    if (FAILED(ui::g_ui.device->CreateTexture2D(&desc, nullptr, &stagingTex))) return false;

    ui::g_ui.deviceContext->CopyResource(stagingTex.Get(), backBuffer.Get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(ui::g_ui.deviceContext->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

    std::wstring wpath = Utf8ToWide(path);

    std::vector<uint8_t> pixelData(static_cast<size_t>(desc.Width) * desc.Height * 4);
    const uint8_t* srcRow = static_cast<const uint8_t*>(mapped.pData);
    for (UINT y = 0; y < desc.Height; ++y) {
        const uint8_t* src = srcRow + y * mapped.RowPitch;
        uint8_t* dst = pixelData.data() + static_cast<size_t>(y) * desc.Width * 4;
        for (UINT x = 0; x < desc.Width; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2]; // Red
            dst[x * 4 + 1] = src[x * 4 + 1]; // Green
            dst[x * 4 + 2] = src[x * 4 + 0]; // Blue
            dst[x * 4 + 3] = 255;             // Solid Alpha
        }
    }

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    bool ok = false;
    ComPtr<IWICImagingFactory> factory;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        ComPtr<IWICBitmapEncoder> encoder;
        if (SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) {
            ComPtr<IWICStream> stream;
            if (SUCCEEDED(factory->CreateStream(&stream)) &&
                SUCCEEDED(stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE)) &&
                SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
                ComPtr<IWICBitmapFrameEncode> frame;
                if (SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
                    SUCCEEDED(frame->Initialize(nullptr))) {
                    frame->SetSize(desc.Width, desc.Height);
                    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppRGBA;
                    frame->SetPixelFormat(&pixelFormat);
                    UINT stride = desc.Width * 4;
                    UINT totalBytes = stride * desc.Height;
                    if (SUCCEEDED(frame->WritePixels(desc.Height, stride, totalBytes, pixelData.data())) &&
                        SUCCEEDED(frame->Commit()) &&
                        SUCCEEDED(encoder->Commit())) {
                        ok = true;
                    }
                }
            }
        }
    }

    ui::g_ui.deviceContext->Unmap(stagingTex.Get(), 0);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return ok;
}

} // namespace miniwp
