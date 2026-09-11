#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "settings_app.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <commdlg.h>
#include <string>
#include <vector>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <thread>
#include <cmath>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include "core/config.h"
#include "core/ipc_server.h"
#include "core/engine_state.h"
#include "core/video_optimizer.h"
#include "platform/win32/hardware_info.h"
#include "platform/win32/lockscreen_manager.h"
#include "thumbnail_manager.h"
#include "icons_fontawesome6.h"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace litewp {

static HWND                     g_hWnd = nullptr;
static HINSTANCE                g_hInstance = nullptr;
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static bool                     g_isOpen = false;

// Hardware telemetry cache
static SystemHardwareInfo       g_hardwareInfo;
static bool                     g_hardwareInfoInit = false;

// Performance metrics cache
static bool   g_daemonConnected = false;
static bool   g_daemonPlaying = false;
static bool   g_daemonPaused = false;
static std::string g_daemonCurrentVideo = "";
static int    g_daemonFps = 0;
static double g_daemonVideoFps = 0.0;
static int    g_daemonWidth = 0;
static int    g_daemonHeight = 0;
static double g_daemonDuration = 0.0;
static double g_daemonCurrentTimeSec = 0.0;
static std::string g_daemonCodec = "";
static size_t g_daemonRamMB = 0;
static size_t g_daemonVramMB = 0;
static double g_daemonCpuPercent = 0.0;
static bool   g_daemonInjected = false;
static bool   g_daemonHwDecode = false;
static int    g_daemonActiveGpuIndex = 0;
static uint64_t g_daemonFramesRendered = 0;
static std::string g_daemonLastError = "";

static std::vector<float> g_cpuHistory(60, 0.0f);
static std::vector<float> g_ramHistory(60, 0.0f);
static std::vector<float> g_vramHistory(60, 0.0f);

// Modal state for video optimization prompt
static bool g_showOptimizeModal = false;
static std::string g_pendingOptimizePath;
static std::string g_pendingOptimizeAction;
static int g_pendingSourceW = 0;
static int g_pendingSourceH = 0;
static int g_pendingTargetW = 1920;
static int g_pendingTargetH = 1080;
static bool g_rememberDownscaleChoice = false;

// Modal state for Static Frame Capture & Target Selector
static bool g_showCaptureModal = false;
static std::string g_captureModalPath;
static double g_captureModalDuration = 10.0;
static double g_captureModalFps = 30.0;
static int g_captureModalSourceW = 1920;
static int g_captureModalSourceH = 1080;
static float g_captureModalTimeSec = 1.0f;
static int g_captureModalFrameNum = 30;
static bool g_captureModalTargetDesktop = true;
static bool g_captureModalTargetLockscreen = true;
static std::string g_captureModalStatus;

// Preview state in capture modal
static ComPtr<ID3D11ShaderResourceView> g_captureModalPreviewSRV;
static float g_captureModalPreviewSec = -1.0f;
static std::atomic<bool> g_captureModalPreviewPending{false};
static std::vector<uint8_t> g_captureModalPreviewBgra;
static int g_captureModalPreviewW = 320;
static int g_captureModalPreviewH = 180;
static std::atomic<bool> g_captureModalPreviewWorkerActive{false};

static void SetupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 5.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 6.0f;

    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;

    style.WindowPadding     = ImVec2(16.0f, 16.0f);
    style.FramePadding      = ImVec2(8.0f, 6.0f);
    style.ItemSpacing       = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 6.0f);

    colors[ImGuiCol_Text]                  = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.52f, 0.58f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.08f, 0.08f, 0.10f, 0.90f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.11f, 0.12f, 0.16f, 0.85f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.12f, 0.12f, 0.16f, 0.95f);
    colors[ImGuiCol_Border]                = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.14f, 0.15f, 0.19f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.20f, 0.22f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.24f, 0.27f, 0.35f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.10f, 0.10f, 0.13f, 1.00f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.11f, 0.11f, 0.14f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.08f, 0.08f, 0.10f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.24f, 0.26f, 0.32f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.32f, 0.35f, 0.42f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.40f, 0.44f, 0.52f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.36f, 0.69f, 1.00f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.16f, 0.18f, 0.23f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.22f, 0.26f, 0.34f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.16f, 0.18f, 0.23f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.22f, 0.26f, 0.34f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.26f, 0.59f, 0.98f, 0.78f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.11f, 0.11f, 0.14f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.20f, 0.24f, 0.32f, 1.00f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.16f, 0.19f, 0.26f, 1.00f);
    colors[ImGuiCol_TabUnfocused]          = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.11f, 0.11f, 0.14f, 1.00f);
    colors[ImGuiCol_PlotLines]             = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]      = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
}

static void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (g_pSwapChain && SUCCEEDED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)))) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

static bool CreateDeviceD3D(HWND hWnd) {
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

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevelArray,
        2,
        D3D11_SDK_VERSION,
        &sd,
        &g_pSwapChain,
        &g_pd3dDevice,
        &featureLevel,
        &g_pd3dDeviceContext
    );

    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

static void SendIpcAsync(const std::string& request_json) {
    std::thread([request_json]() {
        IpcClient client;
        client.SendRequest(request_json);
    }).detach();
}

static void ApplyAction(std::string utf8_path, std::string action) {
    if (action == "stop") {
        SendIpcAsync("{\"cmd\":\"stop\"}");
        g_daemonPlaying = false;
        g_daemonPaused = false;
        g_daemonCurrentVideo.clear();
        auto& cfg = g_config.Get();
        if (!cfg.wallpapers.empty()) {
            cfg.wallpapers[0].video_path = "";
            g_config.Save();
        }
        return;
    }
    if (action == "resume") {
        SendIpcAsync("{\"cmd\":\"resume\"}");
        g_daemonPlaying = true;
        g_daemonPaused = false;
        return;
    }

    if (utf8_path.empty()) return;
    if (utf8_path.find("\\LiteWallpaper\\optimized\\") == std::string::npos &&
        utf8_path.find("/LiteWallpaper/optimized/") == std::string::npos &&
        utf8_path.find("\\optimized\\") == std::string::npos &&
        utf8_path.find("/optimized/") == std::string::npos) {
        g_config.Get().AddToGallery(utf8_path);
        g_config.Save();
    }

    if (action == "wallpaper") {
        nlohmann::json req{{"cmd", "set_wallpaper"}, {"path", utf8_path}};
        SendIpcAsync(req.dump());
    }
}

static void StartVideoOptimization(const std::string& input_path, int target_w, int target_h, const std::string& action, int crop_mode = 0) {
    // Add source path to gallery immediately so user sees card right away
    g_config.Get().AddToGallery(input_path);
    g_config.Save();

    g_video_optimizer.StartOptimizeAsync(
        input_path,
        target_w,
        target_h,
        crop_mode,
        nullptr,
        [action](bool success, const std::string& output_path) {
            if (success && !output_path.empty()) {
                ApplyAction(output_path, action);
            }
        }
    );
}

static void RequestApplyVideo(std::string utf8_path, std::string action) {
    if (action == "stop" || action == "resume") {
        ApplyAction(utf8_path, action);
        return;
    }
    if (utf8_path.empty()) return;

    // Always ensure source path is in gallery
    g_config.Get().AddToGallery(utf8_path);
    g_config.Save();

    auto& cfg = g_config.Get();
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    if (screen_w <= 0) screen_w = 1920;
    if (screen_h <= 0) screen_h = 1080;

    auto probe = VideoOptimizer::Probe(utf8_path);
    auto [target_w, target_h] = VideoOptimizer::CalculateTargetDimensions(probe.width, probe.height, screen_w, screen_h, cfg.optimizer_crop_mode);

    // Check if optimized version already exists in cache
    if (VideoOptimizer::HasOptimizedCache(utf8_path, target_w, target_h)) {
        std::string opt_path = VideoOptimizer::GetOptimizedPath(utf8_path, target_w, target_h);
        ApplyAction(opt_path, action);
        return;
    }

    // Probe source video dimensions and framerate
    if (probe.valid && (probe.width > screen_w || probe.height > screen_h || probe.fps > 60.0)) {
        if (cfg.prompt_downscale) {
            g_pendingOptimizePath = utf8_path;
            g_pendingOptimizeAction = action;
            g_pendingSourceW = probe.width;
            g_pendingSourceH = probe.height;
            g_pendingTargetW = target_w;
            g_pendingTargetH = target_h;
            g_showOptimizeModal = true;
            return;
        } else if (cfg.auto_downscale_highres) {
            StartVideoOptimization(utf8_path, target_w, target_h, action, cfg.optimizer_crop_mode);
            return;
        }
    }

    ApplyAction(utf8_path, action);
}

static LRESULT WINAPI SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return true;
    }

    switch (msg) {
        case WM_SIZE:
            if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
                CleanupRenderTarget();
                g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;

        case WM_DROPFILES: {
            HDROP hDrop = reinterpret_cast<HDROP>(wParam);
            UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < fileCount; i++) {
                wchar_t filePath[MAX_PATH] = {};
                if (DragQueryFileW(hDrop, i, filePath, MAX_PATH)) {
                    int size_needed = WideCharToMultiByte(CP_UTF8, 0, filePath, -1, NULL, 0, NULL, NULL);
                    std::string utf8_path(size_needed - 1, 0);
                    WideCharToMultiByte(CP_UTF8, 0, filePath, -1, &utf8_path[0], size_needed, NULL, NULL);

                    std::error_code ec;
                    auto ext = fs::path(filePath).extension().string();
                    for (auto& c : ext) c = (char)::tolower(c);
                    if (ext == ".mp4" || ext == ".webm" || ext == ".mkv" || ext == ".avi" || ext == ".mov") {
                        RequestApplyVideo(utf8_path, "wallpaper");
                        break;
                    }
                }
            }
            DragFinish(hDrop);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 480;
            mmi->ptMinTrackSize.y = 380;
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

static void FetchDaemonStatus() {
    g_daemonConnected = g_shared_engine_state.connected.load();
    g_daemonPlaying = g_shared_engine_state.playing.load();
    g_daemonPaused = g_shared_engine_state.paused.load();
    g_daemonCurrentVideo = g_shared_engine_state.GetCurrentVideo();
    g_daemonFps = g_shared_engine_state.fps.load();
    g_daemonVideoFps = g_shared_engine_state.video_fps.load();
    g_daemonWidth = g_shared_engine_state.width.load();
    g_daemonHeight = g_shared_engine_state.height.load();
    g_daemonDuration = g_shared_engine_state.duration.load();
    g_daemonCurrentTimeSec = g_shared_engine_state.current_time_sec.load();
    g_daemonCodec = g_shared_engine_state.GetCodec();
    g_daemonRamMB = g_shared_engine_state.ram_mb.load();
    g_daemonVramMB = g_shared_engine_state.vram_mb.load();
    g_daemonCpuPercent = g_shared_engine_state.cpu_percent.load();
    g_daemonInjected = g_shared_engine_state.injected.load();
    g_daemonHwDecode = g_shared_engine_state.hw_decode.load();
    g_daemonActiveGpuIndex = g_shared_engine_state.active_gpu_index.load();
    g_daemonFramesRendered = g_shared_engine_state.frames_rendered.load();
    g_daemonLastError = g_shared_engine_state.GetLastError();

    g_ramHistory.erase(g_ramHistory.begin());
    g_ramHistory.push_back(static_cast<float>(g_daemonRamMB));

    g_cpuHistory.erase(g_cpuHistory.begin());
    g_cpuHistory.push_back(static_cast<float>(g_daemonCpuPercent));

    g_vramHistory.erase(g_vramHistory.begin());
    g_vramHistory.push_back(static_cast<float>(g_daemonVramMB));
}

static void OpenCaptureModal(const std::string& video_path, int targetFilter = 0);

static void RenderOriginalTooltip(const VideoProbeResult& probe, const char* size_str, const std::string& /*path*/) {
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_CIRCLE_INFO "  Original Video Details");
        ImGui::Separator();
        if (probe.valid) {
            const char* resCategory = (probe.width >= 3840) ? "4K Ultra HD" :
                                      (probe.width >= 2560) ? "1440p Quad HD" :
                                      (probe.width >= 1920) ? "1080p Full HD" : "Standard HD";
            ImGui::Text("Resolution : %d x %d (%s)", probe.width, probe.height, resCategory);
            ImGui::Text("Frame Rate : %.1f FPS", probe.fps);
            ImGui::Text("Codec      : %s", probe.codec_name.empty() ? "H.264 / AVC" : probe.codec_name.c_str());
            if (probe.duration > 0.0) {
                int totalSec = static_cast<int>(probe.duration);
                int mm = totalSec / 60;
                int ss = totalSec % 60;
                ImGui::Text("Duration   : %02d:%02d", mm, ss);
            }
        } else {
            ImGui::Text("Resolution : Probing on playback...");
        }
        if (size_str && size_str[0] != '\0') {
            ImGui::Text("File Size  : %s", size_str);
        }
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.65f, 0.70f, 0.80f, 1.00f), "Direct playback of original source with hardware acceleration.");
        ImGui::EndTooltip();
    }
}

static void RenderGalleryTab() {
    auto& cfg = g_config.Get();
    auto galleryCopy = cfg.gallery_history;

    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    if (screen_w <= 0) screen_w = 1920;
    if (screen_h <= 0) screen_h = 1080;

    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_FOLDER_OPEN "  Video Gallery & Management");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(Drag & drop any video file directly here)");
    ImGui::Spacing();

    if (ImGui::Button(ICON_FA_PLUS "  Add Video File...", ImVec2(170, 32))) {
        wchar_t filename[MAX_PATH] = L"";
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = g_hWnd;
        ofn.lpstrFilter = L"Video Files (*.mp4;*.webm;*.mkv;*.avi;*.mov)\0*.mp4;*.webm;*.mkv;*.avi;*.mov\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = filename;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

        if (GetOpenFileNameW(&ofn)) {
            int size_needed = WideCharToMultiByte(CP_UTF8, 0, filename, -1, NULL, 0, NULL, NULL);
            std::string utf8_path(size_needed - 1, 0);
            WideCharToMultiByte(CP_UTF8, 0, filename, -1, &utf8_path[0], size_needed, NULL, NULL);
            RequestApplyVideo(utf8_path, "wallpaper");
        }
    }

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ROTATE "  Refresh Gallery", ImVec2(150, 32))) {
        g_config.Load();
    }

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH_CAN "  Clear All", ImVec2(120, 32))) {
        for (const auto& p : cfg.gallery_history) {
            ThumbnailManager::DeleteThumbnailCache(p);
            VideoOptimizer::DeleteOptimizedCache(p);
        }
        cfg.gallery_history.clear();
        g_config.Save();
    }

    ImGui::Separator();
    ImGui::Spacing();

    // Multi-Monitor Target Checkboxes in Gallery
    if (!g_hardwareInfoInit) {
        g_hardwareInfo = HardwareDetector::QuerySystemInfo();
        g_hardwareInfoInit = true;
    }

    if (g_hardwareInfo.displays.size() > 1) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.16f, 0.22f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.45f, 0.65f, 1.00f));
        ImGui::BeginChild("MultiMonitorBanner", ImVec2(0, 40), true, ImGuiWindowFlags_NoScrollbar);
        
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_TV " Target Desktops:");
        ImGui::SameLine();

        bool all_checked = cfg.target_displays.empty() || (cfg.target_displays.size() == g_hardwareInfo.displays.size());
        if (ImGui::Checkbox("All Monitors", &all_checked)) {
            if (all_checked) {
                cfg.target_displays.clear();
            } else {
                cfg.target_displays = { 0 };
            }
            g_config.Save();
            nlohmann::json req{{"cmd", "set_target_displays"}, {"displays", cfg.target_displays}};
            SendIpcAsync(req.dump());
        }

        for (size_t d = 0; d < g_hardwareInfo.displays.size(); ++d) {
            ImGui::SameLine();
            bool is_d_checked = cfg.IsDisplayEnabled(static_cast<int>(d));
            std::string disp_label = "Display " + std::to_string(d + 1) + (g_hardwareInfo.displays[d].is_primary ? " (Primary)" : "");
            if (ImGui::Checkbox(disp_label.c_str(), &is_d_checked)) {
                cfg.SetDisplayEnabled(static_cast<int>(d), is_d_checked);
                g_config.Save();
                nlohmann::json req{{"cmd", "set_target_displays"}, {"displays", cfg.target_displays}};
                SendIpcAsync(req.dump());
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::Spacing();
    }

    if (galleryCopy.empty()) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No videos in gallery. Click 'Add Video File...' or Drag & Drop videos here.");
        return;
    }

    ImGui::BeginChild("GalleryGrid", ImVec2(0, 0), true, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    
    float availW = ImGui::GetContentRegionAvail().x;
    int numCols = (availW >= 1260.0f) ? 2 : 1;
    float spacingX = 14.0f;
    float cardWidth = (numCols == 1) ? availW : ((availW - spacingX) * 0.5f);
    float cardHeight = 150.0f;
    float thumbW = 210.0f;
    float thumbH = 118.0f;

    for (size_t i = 0; i < galleryCopy.size(); ++i) {
        const auto& path = galleryCopy[i];
        fs::path p(path);
        std::string filename = p.filename().string();
        if (filename.empty()) filename = path;

        int target_w = screen_w;
        int target_h = screen_h;
        auto probe = VideoOptimizer::Probe(path);
        if (probe.valid) {
            auto [pw, ph] = VideoOptimizer::CalculateTargetDimensions(probe.width, probe.height, screen_w, screen_h, cfg.optimizer_crop_mode);
            target_w = pw;
            target_h = ph;
        }

        std::string opt_path = VideoOptimizer::GetOptimizedPath(path, target_w, target_h);
        bool has_opt = VideoOptimizer::HasOptimizedCache(path, target_w, target_h);
        bool is_already_optimal = (probe.valid && probe.width <= screen_w && probe.height <= screen_h && probe.fps <= 60.5);

        bool is_playing_opt = (!g_daemonCurrentVideo.empty() && g_daemonCurrentVideo == opt_path);
        bool is_playing_ori = (!g_daemonCurrentVideo.empty() && g_daemonCurrentVideo == path);
        bool is_current = is_playing_opt || is_playing_ori ||
                          (!cfg.wallpapers.empty() && (cfg.wallpapers[0].video_path == path || cfg.wallpapers[0].video_path == opt_path));

        // Format file size and metadata summary line
        char size_str[32] = {};
        std::error_code fsec;
        if (fs::exists(path, fsec)) {
            uintmax_t bytes = fs::file_size(path, fsec);
            if (bytes >= 1024 * 1024 * 1024) {
                snprintf(size_str, sizeof(size_str), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
            } else if (bytes >= 1024 * 1024) {
                snprintf(size_str, sizeof(size_str), "%.1f MB", bytes / (1024.0 * 1024.0));
            } else if (bytes > 0) {
                snprintf(size_str, sizeof(size_str), "%.0f KB", bytes / 1024.0);
            }
        }

        std::string meta_line;
        if (probe.valid) {
            char meta_buf[128];
            snprintf(meta_buf, sizeof(meta_buf), "%dx%d • %.0f FPS • %s",
                     probe.width, probe.height, probe.fps,
                     probe.codec_name.empty() ? "video" : probe.codec_name.c_str());
            meta_line = meta_buf;
            if (size_str[0] != '\0') {
                meta_line += " • ";
                meta_line += size_str;
            }
        }

        ImGui::PushID(static_cast<int>(i));

        if (is_current) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.20f, 0.30f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.70f, 1.00f, 0.90f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.13f, 0.17f, 0.80f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.20f, 0.22f, 0.28f, 0.80f));
        }

        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, is_current ? 1.5f : 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));

        ImGui::BeginChild("Card", ImVec2(cardWidth, cardHeight), true, ImGuiWindowFlags_NoScrollbar);

        // --- Left: Full Height 16:9 Thumbnail ---
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + thumbW, p0.y + thumbH);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();

        ID3D11ShaderResourceView* thumb_srv = ThumbnailManager::Instance().GetThumbnailSRV(g_pd3dDevice, path);
        if (thumb_srv) {
            draw_list->AddImageRounded((ImTextureID)thumb_srv, p0, p1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 6.0f);
        } else {
            draw_list->AddRectFilled(p0, p1, IM_COL32(20, 22, 28, 255), 6.0f);
            draw_list->AddRect(p0, p1, IM_COL32(40, 44, 56, 255), 6.0f);
            const char* loadingText = "Loading...";
            ImVec2 lts = ImGui::CalcTextSize(loadingText);
            draw_list->AddText(ImVec2(p0.x + (thumbW - lts.x) * 0.5f, p0.y + (thumbH - lts.y) * 0.5f), IM_COL32(140, 145, 160, 255), loadingText);
        }
        draw_list->AddRect(p0, p1, IM_COL32(255, 255, 255, 20), 6.0f);
        ImGui::Dummy(ImVec2(thumbW, thumbH));

        // --- Right: Content Area ---
        ImGui::SameLine(0, 14.0f);
        ImGui::BeginGroup();

        float contentW = cardWidth - thumbW - 38.0f;
        if (contentW < 220.0f) contentW = 220.0f;

        // Top Row: Title on Left, Active/Status Badge on Top-Right Corner
        std::string badgeText;
        ImU32 badgeBgCol, badgeFgCol;
        if (is_current) {
            if (is_playing_opt) {
                badgeText = ICON_FA_BOLT "  ACTIVE (OPTIMIZED)";
                badgeBgCol = IM_COL32(20, 70, 45, 255);
                badgeFgCol = IM_COL32(60, 235, 130, 255);
            } else {
                badgeText = ICON_FA_CIRCLE_PLAY "  ACTIVE (ORIGINAL)";
                badgeBgCol = IM_COL32(20, 55, 95, 255);
                badgeFgCol = IM_COL32(60, 200, 255, 255);
            }
        } else if (has_opt) {
            badgeText = ICON_FA_CHECK "  Optimized Ready";
            badgeBgCol = IM_COL32(32, 40, 56, 255);
            badgeFgCol = IM_COL32(140, 190, 245, 255);
        } else if (is_already_optimal) {
            badgeText = "1080p Optimal";
            badgeBgCol = IM_COL32(28, 48, 38, 255);
            badgeFgCol = IM_COL32(120, 220, 150, 255);
        } else {
            badgeText = ICON_FA_FILM "  Video File";
            badgeBgCol = IM_COL32(30, 32, 40, 255);
            badgeFgCol = IM_COL32(140, 145, 160, 255);
        }

        ImVec2 badgeTextSize = ImGui::CalcTextSize(badgeText.c_str());
        float badgeTotalW = badgeTextSize.x + 18.0f;
        float maxTitleW = contentW - badgeTotalW - 14.0f;
        if (maxTitleW < 100.0f) maxTitleW = 100.0f;

        std::string titleStr = filename;
        while (titleStr.length() > 6 && ImGui::CalcTextSize((titleStr + "...").c_str()).x > maxTitleW) {
            titleStr.pop_back();
        }
        if (titleStr != filename) titleStr += "...";

        ImVec2 topRowPos = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.97f, 0.99f, 1.0f));
        ImGui::TextUnformatted(titleStr.c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\nPath: %s", filename.c_str(), path.c_str());
        }

        // Draw Badge on Top-Right
        ImVec2 bp0 = ImVec2(topRowPos.x + contentW - badgeTotalW, topRowPos.y);
        ImVec2 bp1 = ImVec2(bp0.x + badgeTotalW, bp0.y + badgeTextSize.y + 6.0f);
        draw_list->AddRectFilled(bp0, bp1, badgeBgCol, 4.0f);
        draw_list->AddRect(bp0, bp1, badgeFgCol & 0x60FFFFFF, 4.0f);
        draw_list->AddText(ImVec2(bp0.x + 9.0f, bp0.y + 3.0f), badgeFgCol, badgeText.c_str());

        // Middle Row: Subtitle Metadata
        ImGui::SetCursorScreenPos(ImVec2(topRowPos.x, topRowPos.y + badgeTextSize.y + 10.0f));
        if (!meta_line.empty()) {
            ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.70f, 1.0f), "%s", meta_line.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.45f, 0.50f, 0.58f, 1.0f), "Video source ready");
        }

        // Bottom Row: Action Buttons
        float btnH = 30.0f;
        ImGui::SetCursorScreenPos(ImVec2(topRowPos.x, p0.y + thumbH - btnH));

        if (is_current) {
            // STOP button (Crimson Red)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.22f, 0.22f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.28f, 0.28f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.00f, 0.38f, 0.38f, 1.00f));
            if (ImGui::Button(ICON_FA_STOP "  Stop", ImVec2(75, btnH))) {
                ApplyAction("", "stop");
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Stop video wallpaper and return to Windows static desktop");
            }

            ImGui::SameLine();
            if (is_playing_opt) {
                if (ImGui::Button(ICON_FA_PLAY "  Original", ImVec2(95, btnH))) {
                    ApplyAction(path, "wallpaper");
                }
                RenderOriginalTooltip(probe, size_str, path);
            } else {
                if (has_opt) {
                    if (ImGui::Button(ICON_FA_BOLT "  Opt", ImVec2(80, btnH))) {
                        ApplyAction(opt_path, "wallpaper");
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Switch to pre-rendered optimized version to save ~70% GPU");
                    }
                } else if (!is_already_optimal) {
                    if (ImGui::Button(ICON_FA_DOWNLOAD "  Optimize", ImVec2(100, btnH))) {
                        StartVideoOptimization(path, target_w, target_h, "wallpaper", cfg.optimizer_crop_mode);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Pre-render hardware-optimized 1080p copy (Low GPU load)");
                    }
                }
            }

            // Capture Frame Button (Clear label & tooltip)
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_CAMERA "  Capture Frame", ImVec2(135, btnH))) {
                OpenCaptureModal(path);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_CAMERA "  Capture Frame (Visual Wallpaper)");
                ImGui::Separator();
                ImGui::Text("Ambil gambar frame statis dari video ini untuk:");
                ImGui::BulletText("Desktop Wallpaper (0s instant boot visual)");
                ImGui::BulletText("Windows Lock Screen (Win+L seamless transition)");
                ImGui::EndTooltip();
            }

            // Delete Button
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH_CAN, ImVec2(34, btnH))) {
                if (is_current) ApplyAction("", "stop");
                ThumbnailManager::DeleteThumbnailCache(path);
                VideoOptimizer::DeleteOptimizedCache(path);
                cfg.RemoveFromGallery(path);
                g_config.Save();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove video from gallery");
            }
        } else {
            // Standby video buttons
            if (has_opt) {
                if (ImGui::Button(ICON_FA_BOLT "  Play (Opt)", ImVec2(110, btnH))) {
                    ApplyAction(opt_path, "wallpaper");
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Play pre-rendered optimized version (Low GPU usage)");
                }
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_PLAY "  Original", ImVec2(95, btnH))) {
                    ApplyAction(path, "wallpaper");
                }
                RenderOriginalTooltip(probe, size_str, path);
            } else if (is_already_optimal) {
                if (ImGui::Button(ICON_FA_PLAY "  Play", ImVec2(80, btnH))) {
                    RequestApplyVideo(path, "wallpaper");
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Play 1080p native video directly");
                }
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_CIRCLE_INFO "  Original", ImVec2(95, btnH))) {
                    RequestApplyVideo(path, "wallpaper");
                }
                RenderOriginalTooltip(probe, size_str, path);
            } else {
                if (ImGui::Button(ICON_FA_PLAY "  Original", ImVec2(95, btnH))) {
                    RequestApplyVideo(path, "wallpaper");
                }
                RenderOriginalTooltip(probe, size_str, path);
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_DOWNLOAD "  Optimize", ImVec2(100, btnH))) {
                    StartVideoOptimization(path, target_w, target_h, "wallpaper", cfg.optimizer_crop_mode);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Pre-render hardware-optimized 1080p copy (Low GPU load)");
                }
            }

            // Capture Frame Button
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_CAMERA "  Capture Frame", ImVec2(135, btnH))) {
                OpenCaptureModal(path);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_CAMERA "  Capture Frame (Visual Wallpaper)");
                ImGui::Separator();
                ImGui::Text("Ambil gambar frame statis dari video ini untuk:");
                ImGui::BulletText("Desktop Wallpaper (0s instant boot visual)");
                ImGui::BulletText("Windows Lock Screen (Win+L seamless transition)");
                ImGui::EndTooltip();
            }

            // Delete Button
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH_CAN, ImVec2(34, btnH))) {
                ThumbnailManager::DeleteThumbnailCache(path);
                VideoOptimizer::DeleteOptimizedCache(path);
                cfg.RemoveFromGallery(path);
                g_config.Save();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove video from gallery");
            }
        }

        ImGui::EndGroup();

        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        ImGui::PopID();

        // Responsive grid layout
        if (numCols > 1 && (i % numCols) < static_cast<size_t>(numCols - 1) && (i + 1 < galleryCopy.size())) {
            ImGui::SameLine(0, spacingX);
        } else {
            ImGui::Spacing();
        }
    }

    ImGui::EndChild();
}

static void RenderLockscreenTab() {
    auto& cfg = g_config.Get();

    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_LOCK "  Lock Screen & Static Wallpaper Management");
    ImGui::TextColored(ImVec4(0.68f, 0.72f, 0.80f, 1.00f), "Configure instant 0s Windows boot visuals and lock screen transitions with zero continuous CPU load.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // CARD 1: Windows Desktop Static Wallpaper (0s Instant Boot Visual)
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.13f, 0.17f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.24f, 0.28f, 0.38f, 1.00f));
    ImGui::BeginChild("DesktopStaticCard", ImVec2(0, 155), true, ImGuiWindowFlags_NoScrollbar);

    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_DESKTOP "  1. Windows Desktop Static Wallpaper (0s Instant Boot Visual)");
    ImGui::Spacing();

    if (ImGui::Checkbox("Enable Windows Desktop Static Wallpaper", &cfg.update_desktop_wallpaper)) {
        g_config.Save();
        if (cfg.update_desktop_wallpaper) {
            if (cfg.desktop_static_source_mode == 1 && !cfg.desktop_static_video_path.empty()) {
                nlohmann::json req{
                    {"cmd", "sync_desktop_wallpaper"},
                    {"path", cfg.desktop_static_video_path},
                    {"desktop", true},
                    {"lockscreen", false},
                    {"timestamp", cfg.desktop_static_timestamp}
                };
                SendIpcAsync(req.dump());
            } else {
                SendIpcAsync("{\"cmd\":\"sync_desktop_wallpaper\",\"desktop\":true,\"lockscreen\":false}");
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Injects a high-definition static frame directly into Windows Desktop Wallpaper.\nWhen Windows boots, your desktop appears in 0.0s with no black screen or loading delay.");
    }

    if (cfg.update_desktop_wallpaper) {
        ImGui::Indent(18.0f);
        if (cfg.desktop_static_source_mode == 0) {
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.95f, 1.0f), ICON_FA_ROTATE " Active Mode: Auto-Sync (Dynamically follows active live wallpaper)");
        } else {
            fs::path p(cfg.desktop_static_video_path);
            std::string vidName = p.filename().string();
            if (vidName.empty()) vidName = cfg.desktop_static_video_path;
            int curMin = static_cast<int>(cfg.desktop_static_timestamp) / 60;
            float curSec = static_cast<float>(cfg.desktop_static_timestamp) - curMin * 60.0f;
            ImGui::TextColored(ImVec4(0.40f, 0.95f, 0.50f, 1.0f), ICON_FA_IMAGE " Active Mode: Custom Frame [%s @ %02d:%05.2f]", vidName.c_str(), curMin, curSec);

            ImGui::SameLine();
            if (ImGui::SmallButton("Reset to Auto-Sync")) {
                cfg.desktop_static_source_mode = 0;
                g_config.Save();
                SendIpcAsync("{\"cmd\":\"sync_desktop_wallpaper\",\"reset_desktop_auto\":true}");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Switch back to automatically updating desktop wallpaper with whichever live video is currently playing");
            }
        }

        std::string cur_vid = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : g_daemonCurrentVideo;
        if (cfg.desktop_static_source_mode == 1 && !cfg.desktop_static_video_path.empty()) {
            cur_vid = cfg.desktop_static_video_path;
        }
        if (cur_vid.empty() && !cfg.gallery_history.empty()) {
            cur_vid = cfg.gallery_history[0];
        }

        if (ImGui::Button(ICON_FA_CAMERA "  Choose Custom Frame for Desktop...", ImVec2(320, 28))) {
            if (!cur_vid.empty()) {
                OpenCaptureModal(cur_vid, 1);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Select a specific video and exact timestamp/frame specifically for the Desktop Wallpaper");
        }
        ImGui::Unindent(18.0f);
    } else {
        ImGui::TextDisabled("  Desktop static wallpaper injection is currently disabled.");
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::Spacing();

    // CARD 2: Windows Lock Screen Wallpaper (Win + L Seamless Transition)
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.13f, 0.17f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.24f, 0.28f, 0.38f, 1.00f));
    ImGui::BeginChild("LockscreenCard", ImVec2(0, 155), true, ImGuiWindowFlags_NoScrollbar);

    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_LOCK "  2. Windows Lock Screen Wallpaper (Win + L Seamless Transition)");
    ImGui::Spacing();

    if (ImGui::Checkbox("Enable Windows Lock Screen Wallpaper", &cfg.update_lockscreen)) {
        g_config.Save();
        if (cfg.update_lockscreen) {
            if (cfg.lockscreen_source_mode == 1 && !cfg.lockscreen_video_path.empty()) {
                nlohmann::json req{
                    {"cmd", "sync_desktop_wallpaper"},
                    {"path", cfg.lockscreen_video_path},
                    {"desktop", false},
                    {"lockscreen", true},
                    {"timestamp", cfg.lockscreen_timestamp}
                };
                SendIpcAsync(req.dump());
            } else {
                SendIpcAsync("{\"cmd\":\"sync_desktop_wallpaper\",\"desktop\":false,\"lockscreen\":true}");
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pre-caches a pristine high-definition frame for the Windows Lock Screen.\nWhen pressing Win+L, it displays immediately with 0ms visual delay and 0% background load.");
    }

    if (cfg.update_lockscreen) {
        ImGui::Indent(18.0f);
        if (cfg.lockscreen_source_mode == 0) {
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.95f, 1.0f), ICON_FA_ROTATE " Active Mode: Auto-Sync (Dynamically follows active live wallpaper)");
        } else {
            fs::path p(cfg.lockscreen_video_path);
            std::string vidName = p.filename().string();
            if (vidName.empty()) vidName = cfg.lockscreen_video_path;
            int curMin = static_cast<int>(cfg.lockscreen_timestamp) / 60;
            float curSec = static_cast<float>(cfg.lockscreen_timestamp) - curMin * 60.0f;
            ImGui::TextColored(ImVec4(0.40f, 0.95f, 0.50f, 1.0f), ICON_FA_IMAGE " Active Mode: Custom Frame [%s @ %02d:%05.2f]", vidName.c_str(), curMin, curSec);

            ImGui::SameLine();
            if (ImGui::SmallButton("Reset to Auto-Sync")) {
                cfg.lockscreen_source_mode = 0;
                g_config.Save();
                SendIpcAsync("{\"cmd\":\"sync_desktop_wallpaper\",\"reset_lockscreen_auto\":true}");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Switch back to automatically updating lock screen wallpaper with whichever live video is currently playing");
            }
        }

        std::string cur_vid = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : g_daemonCurrentVideo;
        if (cfg.lockscreen_source_mode == 1 && !cfg.lockscreen_video_path.empty()) {
            cur_vid = cfg.lockscreen_video_path;
        }
        if (cur_vid.empty() && !cfg.gallery_history.empty()) {
            cur_vid = cfg.gallery_history[0];
        }

        if (ImGui::Button(ICON_FA_CAMERA "  Choose Custom Frame for Lock Screen...", ImVec2(320, 28))) {
            if (!cur_vid.empty()) {
                OpenCaptureModal(cur_vid, 2);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Select a specific video and exact timestamp/frame specifically for the Windows Lock Screen");
        }
        ImGui::Unindent(18.0f);
    } else {
        ImGui::TextDisabled("  Windows Lock Screen wallpaper integration is currently disabled.");
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::Spacing();

    // CARD 3: Windows Screensaver Integration
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.13f, 0.17f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.24f, 0.28f, 0.38f, 1.00f));
    ImGui::BeginChild("ScreensaverCard", ImVec2(0, 115), true, ImGuiWindowFlags_NoScrollbar);

    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_TV "  3. Windows Screensaver Integration");
    ImGui::Spacing();

    static bool scr_checked = false;
    static bool scr_installed = false;
    if (!scr_checked) {
        scr_installed = LockScreenManager::IsScreensaverInstalled();
        scr_checked = true;
    }

    if (ImGui::Checkbox("Enable LiteWallpaper as Windows Screensaver", &scr_installed)) {
        if (scr_installed) {
            wchar_t exePathBuf[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
            std::wstring exePath(exePathBuf);
            std::wstring scrPath = exePath.substr(0, exePath.find_last_of(L'.')) + L".scr";
            if (!fs::exists(scrPath)) {
                CopyFileW(exePath.c_str(), scrPath.c_str(), FALSE);
            }
            LockScreenManager::InstallScreensaver(scrPath, 300, true);
        } else {
            LockScreenManager::UninstallScreensaver();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Plays your live video wallpaper at full 60 FPS when your computer is idle!\nWhen waking the PC, it transitions seamlessly back.");
    }

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_GEAR "  Configure Screensaver...", ImVec2(220, 26))) {
        LockScreenManager::OpenWindowsScreensaverSettings();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Opens official Windows Screen Saver Settings to adjust idle timeout and resume password.");
    }

    ImGui::TextColored(ImVec4(0.60f, 0.64f, 0.72f, 1.00f), "Smoothly plays your live wallpaper when idle, waking up immediately upon mouse movement.");

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

static void RenderGeneralSettingsTab() {
    auto& cfg = g_config.Get();

    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_SLIDERS "  General Settings & Playback");
    ImGui::TextColored(ImVec4(0.68f, 0.72f, 0.80f, 1.00f), "Configure display scaling, master volume, Windows autostart, and power-saving rules.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float availW = ImGui::GetContentRegionAvail().x;
    float comboW = (std::min)(480.0f, availW);
    float sliderW = (std::min)(320.0f, availW);

    // SECTION 1: Display Scaling Mode
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_EXPAND "  Display Scaling & Multi-Monitor");
    ImGui::Spacing();

    static const char* scalingModes[] = {
        "Auto Aspect Fill (Cover - Smart Crop, No Black Bars)",
        "Aspect Fit (Letterbox - Full Frame with Black Bars)",
        "Stretch to Screen (Ignore Aspect Ratio)"
    };
    int currentMode = cfg.scaling_mode;
    ImGui::SetNextItemWidth(comboW);
    if (ImGui::Combo("Display Scaling Mode", &currentMode, scalingModes, IM_ARRAYSIZE(scalingModes))) {
        cfg.scaling_mode = currentMode;
        g_config.Save();
        nlohmann::json req{{"cmd", "set_scaling"}, {"mode", currentMode}};
        SendIpcAsync(req.dump());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Changes how video fits the screen.\n*Note: Effect is visible when video aspect ratio differs from display\n (e.g. 4:3, 21:9 ultrawide, or vertical video on 16:9 screen).");
    }

    if (!g_hardwareInfoInit) {
        g_hardwareInfo = HardwareDetector::QuerySystemInfo();
        g_hardwareInfoInit = true;
    }

    if (g_hardwareInfo.displays.size() > 1) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.75f, 1.0f), "Target Desktop Screen(s):");

        bool all_checked = cfg.target_displays.empty() || (cfg.target_displays.size() == g_hardwareInfo.displays.size());
        if (ImGui::Checkbox("All Monitors (Apply to All Desktops)", &all_checked)) {
            if (all_checked) {
                cfg.target_displays.clear();
            } else {
                cfg.target_displays = { 0 };
            }
            g_config.Save();
            nlohmann::json req{{"cmd", "set_target_displays"}, {"displays", cfg.target_displays}};
            SendIpcAsync(req.dump());
        }

        for (size_t d = 0; d < g_hardwareInfo.displays.size(); ++d) {
            const auto& disp = g_hardwareInfo.displays[d];
            bool is_d_checked = cfg.IsDisplayEnabled(static_cast<int>(d));
            std::string label = "Display " + std::to_string(d + 1) + ": " + disp.name + " (" +
                                std::to_string(disp.width) + "x" + std::to_string(disp.height) + ")" +
                                (disp.is_primary ? " [Primary]" : " [Secondary]");
            if (ImGui::Checkbox(label.c_str(), &is_d_checked)) {
                cfg.SetDisplayEnabled(static_cast<int>(d), is_d_checked);
                g_config.Save();
                nlohmann::json req{{"cmd", "set_target_displays"}, {"displays", cfg.target_displays}};
                SendIpcAsync(req.dump());
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // SECTION 2: Audio Configuration
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_VOLUME_HIGH "  Audio Configuration");
    ImGui::Spacing();

    static float volume = 0.0f;
    static bool volume_init = false;
    if (!volume_init && !cfg.wallpapers.empty()) {
        volume = cfg.wallpapers[0].volume;
        volume_init = true;
    }

    ImGui::SetNextItemWidth(sliderW);
    if (ImGui::SliderFloat("Master Volume", &volume, 0.0f, 1.0f, "%.2f")) {
        if (!cfg.wallpapers.empty()) {
            cfg.wallpapers[0].volume = volume;
        }
        g_config.Save();
        nlohmann::json req{{"cmd", "set_volume"}, {"volume", volume}};
        SendIpcAsync(req.dump());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // SECTION 3: Windows Startup & Automation
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_DESKTOP "  Windows Startup & Automation");
    ImGui::Spacing();

    static bool startup_inited = false;
    static bool startup_enabled = false;
    if (!startup_inited) {
        startup_enabled = WindowsAutostart::IsEnabled();
        startup_inited = true;
    }

    if (ImGui::Checkbox("Start LiteWallpaper automatically on Windows Boot", &startup_enabled)) {
        WindowsAutostart::SetEnabled(startup_enabled, 0);
        cfg.run_on_startup = startup_enabled;
        g_config.Save();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Launches LiteWallpaper silently in the background when Windows starts, seamlessly resuming your wallpaper.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // SECTION 4: Power & Occlusion Auto-Pause Rules
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_BATTERY_HALF "  Power & Occlusion Auto-Pause");
    ImGui::Spacing();

    if (ImGui::Checkbox("Auto-Pause when Fullscreen App/Game is active", &cfg.pause_on_fullscreen)) {
        g_config.Save();
    }
    if (ImGui::Checkbox("Auto-Pause when Window is Maximized / Desktop Covered", &cfg.pause_on_maximized)) {
        g_config.Save();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Automatically pauses wallpaper playback when an application (browser, code editor, etc.) is maximized and covers the desktop, reducing CPU and GPU usage to 0%.");
    }
    if (ImGui::Checkbox("Auto-Pause on Battery Power", &cfg.pause_on_battery)) {
        g_config.Save();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // SECTION 5: Auto Smooth Looping
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_ROTATE "  Auto Smooth & Seamless Looping");
    ImGui::Spacing();

    if (ImGui::Checkbox("Enable Auto Smooth Looping (Seamless Crossfade)", &cfg.auto_smooth_loop)) {
        g_config.Save();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Applies a smooth crossfade transition and time-warp deceleration at loop seams for a seamless continuous loop without hard cuts.");
    }

    if (cfg.auto_smooth_loop) {
        const char* loop_presets[] = {
            "Cinematic Speed Ramp (1.2s + 0.75x Slow-Mo Blend)",
            "Smoothstep S-Curve (0.8s Natural Easing)",
            "Gentle Flow (1.8s Ambient Scenery Blend)",
            "Instant Snap (0.4s Fast Seamless Snap)",
            "Custom Tuning..."
        };

        ImGui::SetNextItemWidth(comboW);
        if (ImGui::Combo("Looping Preset", &cfg.loop_preset, loop_presets, IM_ARRAYSIZE(loop_presets))) {
            g_config.Save();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Select curated looping behavior presets optimized for different types of wallpaper animations.");
        }

        if (cfg.loop_preset == 4) { // Custom Tuning
            ImGui::SetNextItemWidth(sliderW);
            if (ImGui::SliderFloat("Transition Duration", &cfg.smooth_loop_duration, 0.2f, 2.5f, "%.1f seconds")) {
                g_config.Save();
            }

            const char* easing_curves[] = {
                "Linear (Constant)",
                "Smoothstep (S-Curve Hermite)",
                "Sine Wave (Smooth Harmonic)",
                "Smootherstep (Perlin Ultra-Smooth)"
            };
            ImGui::SetNextItemWidth(sliderW);
            if (ImGui::Combo("Easing Curve", &cfg.loop_easing_curve, easing_curves, IM_ARRAYSIZE(easing_curves))) {
                g_config.Save();
            }

            if (ImGui::Checkbox("Enable Speed Ramping (Time Warp Deceleration)", &cfg.loop_speed_ramp)) {
                g_config.Save();
            }

            if (cfg.loop_speed_ramp) {
                ImGui::SetNextItemWidth(sliderW);
                if (ImGui::SliderFloat("Min Speed at Seam", &cfg.loop_min_speed, 0.50f, 0.95f, "%.2fx")) {
                    g_config.Save();
                }
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.00f), ICON_FA_CIRCLE_CHECK "  All settings are saved and applied automatically in real-time.");
    ImGui::Spacing();

    if (ImGui::Button(ICON_FA_EYE_SLASH "  Hide Window to Tray", ImVec2(200, 34))) {
        SettingsUI::Close();
    }
}

static void RenderPerformancePanel() {
    auto& cfg = g_config.Get();

    if (!g_hardwareInfoInit) {
        g_hardwareInfo = HardwareDetector::QuerySystemInfo();
        g_hardwareInfoInit = true;
    }

    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_GAUGE_HIGH "  Performance & Advanced Diagnostics");
    ImGui::TextColored(ImVec4(0.68f, 0.72f, 0.80f, 1.00f), "GPU hardware acceleration engine, frame pacing, gamer auto-sleep governor, and live telemetry.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float availW = ImGui::GetContentRegionAvail().x;
    float comboW = (std::min)(480.0f, availW);
    float sliderW = (std::min)(320.0f, availW);

    // SECTION 1: Hardware Rendering Engine & Frame Rate Control
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_MICROCHIP "  Hardware Rendering Engine & Frame Rate");
    ImGui::Spacing();

    std::vector<std::string> deviceNames;
    std::vector<int> deviceValues;

    for (size_t g = 0; g < g_hardwareInfo.gpus.size(); ++g) {
        std::string label = "GPU " + std::to_string(g + 1) + ": " + g_hardwareInfo.gpus[g].name + " (DirectX 11 HW)";
        deviceNames.push_back(label);
        deviceValues.push_back(static_cast<int>(g));
    }
    deviceNames.push_back("CPU: Software Processor Decode (swscale)");
    deviceValues.push_back(-1);

    int currentDeviceCombo = 0;
    for (size_t i = 0; i < deviceValues.size(); ++i) {
        if (deviceValues[i] == cfg.gpu_device_index) {
            currentDeviceCombo = static_cast<int>(i);
            break;
        }
    }

    std::vector<const char*> comboItems;
    for (const auto& name : deviceNames) {
        comboItems.push_back(name.c_str());
    }

    ImGui::SetNextItemWidth(comboW);
    if (ImGui::Combo("Video Rendering Engine", &currentDeviceCombo, comboItems.data(), static_cast<int>(comboItems.size()))) {
        cfg.gpu_device_index = deviceValues[currentDeviceCombo];
        g_config.Save();
        nlohmann::json req{{"cmd", "set_render_device"}, {"gpu_index", cfg.gpu_device_index}};
        SendIpcAsync(req.dump());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Choose whether video decoding & rendering runs on GPU 1, GPU 2, or CPU Software.");
    }

    ImGui::SetNextItemWidth(sliderW);
    if (ImGui::SliderInt("Target Render FPS", &cfg.target_fps, 15, 60)) {
        g_config.Save();
        nlohmann::json req{{"cmd", "set_fps"}, {"fps", cfg.target_fps}};
        SendIpcAsync(req.dump());
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.00f), "(Active: %d FPS)", g_daemonFps);

    ImGui::SetNextItemWidth(sliderW);
    if (ImGui::SliderInt("Battery Saver FPS", &cfg.battery_fps, 10, 30)) {
        g_config.Save();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // SECTION 2: Smart Gaming Resource Governor
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_GAUGE_HIGH "  Smart Resource Governor (Gaming & Heavy Load Auto-Sleep)");
    ImGui::Spacing();

    if (ImGui::Checkbox("Auto-Sleep when System RAM or GPU VRAM is under heavy load", &cfg.pause_on_resource_heavy)) {
        g_config.Save();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Essential for windowed gaming! When a heavy game or app pushes RAM or VRAM above the threshold, wallpaper enters Deep Sleep (0% CPU, 0 MB VRAM) to yield maximum performance to your game.");
    }

    if (cfg.pause_on_resource_heavy) {
        ImGui::Indent(18.0f);

        ImGui::SetNextItemWidth(sliderW - 18.0f);
        if (ImGui::SliderInt("System RAM Threshold", &cfg.resource_ram_threshold_pct, 60, 95, "%d%%")) {
            g_config.Save();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Triggers auto-sleep when overall PC system RAM usage hits this percentage (default 80%).");
        }

        ImGui::SetNextItemWidth(sliderW - 18.0f);
        if (ImGui::SliderInt("GPU VRAM Threshold", &cfg.resource_vram_threshold_pct, 60, 95, "%d%%")) {
            g_config.Save();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Triggers auto-sleep when GPU dedicated video memory usage hits this percentage (default 80%).");
        }

        int cur_ram = g_shared_engine_state.system_ram_percent.load();
        int cur_vram = g_shared_engine_state.gpu_vram_percent.load();
        bool is_sleeping = g_shared_engine_state.resource_heavy_sleep.load();

        char ram_buf[64], vram_buf[64];
        snprintf(ram_buf, sizeof(ram_buf), "Live RAM: %d%% (Limit: %d%%)", cur_ram, cfg.resource_ram_threshold_pct);
        snprintf(vram_buf, sizeof(vram_buf), "Live VRAM: %d%% (Limit: %d%%)", cur_vram, cfg.resource_vram_threshold_pct);

        ImVec4 ramColor = (cur_ram >= cfg.resource_ram_threshold_pct) ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f) : ImVec4(0.35f, 0.85f, 0.45f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ramColor);
        ImGui::ProgressBar(static_cast<float>(cur_ram) / 100.0f, ImVec2(sliderW - 18.0f, 18.0f), ram_buf);
        ImGui::PopStyleColor();

        if (cur_vram > 0) {
            ImVec4 vramColor = (cur_vram >= cfg.resource_vram_threshold_pct) ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f) : ImVec4(0.35f, 0.85f, 0.45f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, vramColor);
            ImGui::ProgressBar(static_cast<float>(cur_vram) / 100.0f, ImVec2(sliderW - 18.0f, 18.0f), vram_buf);
            ImGui::PopStyleColor();
        }

        if (is_sleeping) {
            ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.20f, 1.00f), ICON_FA_EYE_SLASH "  Auto-Sleep Active: Releasing 100% VRAM & pausing rendering for active game");
        }

        ImGui::Unindent(18.0f);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // SECTION 3: Video Optimization Scaling Mode
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_EXPAND "  Video Optimization Transcoding Mode");
    ImGui::Spacing();

    if (ImGui::RadioButton("Aspect Fit (Proportional - keeps entire video frame intact)", &cfg.optimizer_crop_mode, 0)) {
        g_config.Save();
    }
    if (ImGui::RadioButton("Aspect Fill / Center Crop (Exact Full Screen 1080p - zero black bars)", &cfg.optimizer_crop_mode, 1)) {
        g_config.Save();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // SECTION 4: Real-Time Engine Monitor & Telemetry
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_CHART_LINE "  Real-Time Engine Monitor & Telemetry");
    ImGui::Spacing();

    if (!g_daemonConnected) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Engine Status: Background Engine Initializing...");
    } else {
        if (g_daemonPlaying && !g_daemonPaused) {
            ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.00f), ICON_FA_CIRCLE_CHECK "  Engine Status: Playing (Active)");
        } else if (g_daemonPaused) {
            ImGui::TextColored(ImVec4(1.00f, 0.70f, 0.20f, 1.00f), ICON_FA_PAUSE "  Engine Status: Paused");
        } else {
            ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.75f, 1.00f), ICON_FA_STOP "  Engine Status: Idle");
        }

        ImGui::Text("Active Video: %s", g_daemonCurrentVideo.empty() ? "(None)" : g_daemonCurrentVideo.c_str());
        ImGui::Text("Render Frame Rate: %d FPS (Video Source: %.1f FPS)", g_daemonFps, g_daemonVideoFps);
        ImGui::Text("Video Resolution: %dx%d (%s)", g_daemonWidth, g_daemonHeight, g_daemonCodec.c_str());
        ImGui::Text("Video Duration: %.1f seconds", g_daemonDuration);

        ImGui::Spacing();
        ImGui::Text("Process CPU Usage: %.1f %%", g_daemonCpuPercent);
        ImGui::PlotLines("CPU (%)", g_cpuHistory.data(), (int)g_cpuHistory.size(), 0, nullptr, 0.0f, 100.0f, ImVec2(0, 50));

        ImGui::Text("Process RAM (Working Set): %zu MB", g_daemonRamMB);
        ImGui::PlotLines("RAM (MB)", g_ramHistory.data(), (int)g_ramHistory.size(), 0, nullptr, 0.0f, 100.0f, ImVec2(0, 50));

        std::string monitoredGpuName = "Default Adapter";
        if (g_daemonActiveGpuIndex == -1) {
            monitoredGpuName = "CPU Software Mode (iGPU Presenter)";
        } else if (g_daemonActiveGpuIndex >= 0 && g_daemonActiveGpuIndex < static_cast<int>(g_hardwareInfo.gpus.size())) {
            const auto& gpu = g_hardwareInfo.gpus[g_daemonActiveGpuIndex];
            monitoredGpuName = "GPU " + std::to_string(g_daemonActiveGpuIndex + 1) + ": " + gpu.name + (gpu.is_discrete ? " [dGPU]" : " [iGPU]");
        }

        ImGui::Text("Process Video Memory (%s): %zu MB", monitoredGpuName.c_str(), g_daemonVramMB);
        std::string plotVramLabel = "VRAM: " + monitoredGpuName + " (MB)";
        ImGui::PlotLines(plotVramLabel.c_str(), g_vramHistory.data(), (int)g_vramHistory.size(), 0, nullptr, 0.0f, 256.0f, ImVec2(0, 50));
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // SECTION 5: Connected Monitors Topology
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_TV "  Connected Monitors Topology");
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_ROTATE " Re-detect Displays")) {
        g_hardwareInfo.displays = HardwareDetector::GetDisplayList();
    }

    if (g_hardwareInfo.displays.empty()) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "No displays enumerated.");
    } else {
        for (size_t idx = 0; idx < g_hardwareInfo.displays.size(); ++idx) {
            const auto& disp = g_hardwareInfo.displays[idx];
            if (disp.is_primary) {
                ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.00f),
                    "• Display %zu [PRIMARY]: %s", idx + 1, disp.name.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f),
                    "• Display %zu [SECONDARY]: %s", idx + 1, disp.name.c_str());
            }
            ImGui::Text("   Resolution: %d x %d  |  Refresh Rate: %d Hz  |  Desktop Position: (%d, %d)",
                        disp.width, disp.height, disp.refresh_rate, disp.pos_x, disp.pos_y);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // SECTION 6: Hardware Topology
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_MICROCHIP "  Hardware Topology (CPU & GPU Detection)");
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_ROTATE " Re-detect Hardware")) {
        g_hardwareInfo = HardwareDetector::QuerySystemInfo();
    }

    if (g_daemonActiveGpuIndex == -1) {
        ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.00f),
            "• Processor (CPU): %s [ACTIVE RENDERING ENGINE (SOFTWARE CPU)]", g_hardwareInfo.cpu.model_name.c_str());
    } else {
        ImGui::TextColored(ImVec4(0.92f, 0.93f, 0.95f, 1.00f),
            "• Processor (CPU): %s", g_hardwareInfo.cpu.model_name.c_str());
    }
    ImGui::Text("   Cores / Topology: %d Physical Cores, %d Logical Threads",
                g_hardwareInfo.cpu.physical_cores, g_hardwareInfo.cpu.logical_cores);

    if (g_hardwareInfo.gpus.empty()) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "• Graphics (GPU): Standard Display Adapter");
    } else {
        for (size_t g_idx = 0; g_idx < g_hardwareInfo.gpus.size(); ++g_idx) {
            const auto& gpu = g_hardwareInfo.gpus[g_idx];
            bool is_this_gpu_active = (g_daemonActiveGpuIndex == static_cast<int>(g_idx));
            if (is_this_gpu_active) {
                ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.00f),
                    "• GPU %zu: %s (%s) [ACTIVE D3D11 RENDERER]",
                    g_idx + 1, gpu.name.c_str(), gpu.is_discrete ? "Discrete GPU" : "Integrated iGPU");
            } else {
                ImGui::TextColored(ImVec4(0.80f, 0.82f, 0.88f, 1.00f),
                    "• GPU %zu: %s (%s)",
                    g_idx + 1, gpu.name.c_str(), gpu.is_discrete ? "Discrete GPU" : "Integrated iGPU");
            }
            ImGui::Text("   Dedicated VRAM: %zu MB (%.1f GB)  |  Shared System Memory: %zu MB (%.1f GB)",
                        gpu.dedicated_vram_mb, static_cast<float>(gpu.dedicated_vram_mb) / 1024.0f,
                        gpu.shared_vram_mb, static_cast<float>(gpu.shared_vram_mb) / 1024.0f);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // SECTION 7: Diagnostics & Logs
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_CIRCLE_INFO "  Diagnostics & Activity Logs");
    ImGui::Text("Desktop Injection (WorkerW): %s", g_daemonInjected ? "OK" : "FAILED");
    if (g_daemonActiveGpuIndex == -1 || !g_daemonHwDecode) {
        ImGui::Text("Decoder Mode: Software (CPU / swscale mode)");
    } else {
        std::string gpu_name = "GPU";
        if (g_daemonActiveGpuIndex >= 0 && g_daemonActiveGpuIndex < static_cast<int>(g_hardwareInfo.gpus.size())) {
            gpu_name = "GPU " + std::to_string(g_daemonActiveGpuIndex + 1) + ": " + g_hardwareInfo.gpus[g_daemonActiveGpuIndex].name;
        }
        ImGui::Text("Decoder Mode: Hardware D3D11VA (%s Active)", gpu_name.c_str());
    }
    ImGui::Text("Frames Rendered: %llu", (unsigned long long)g_daemonFramesRendered);
    if (!g_daemonLastError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Last Error: %s", g_daemonLastError.c_str());
    } else {
        ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.00f), "Last Error: (none)");
    }

    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_EXPAND "  Flash Render Window (Diagnostic)", ImVec2(280, 28))) {
        SendIpcAsync("{\"cmd\":\"test_render\"}");
    }
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.65f, 1.0f),
        "*If the desktop turns green after flashing, injection works.");

    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open Engine Log (Notepad)", ImVec2(280, 30))) {
        const char* appData = getenv("APPDATA");
        if (appData) {
            std::string logPath = std::string(appData) + "\\LiteWallpaper\\engine.log";
            ShellExecuteA(nullptr, "open", "notepad.exe", logPath.c_str(), nullptr, SW_SHOW);
        }
    }
}

static void RequestCaptureModalPreview() {
    if (g_captureModalPreviewWorkerActive.load() || g_captureModalPath.empty()) return;
    g_captureModalPreviewWorkerActive.store(true);

    std::string vpath = g_captureModalPath;
    float timeSec = g_captureModalTimeSec;
    int pw = g_captureModalPreviewW;
    int ph = g_captureModalPreviewH;

    std::thread([vpath, timeSec, pw, ph]() {
        std::vector<uint8_t> bgra;
        if (ThumbnailManager::ExtractFrameToBGRA(vpath, bgra, pw, ph, static_cast<double>(timeSec))) {
            g_captureModalPreviewBgra = std::move(bgra);
            g_captureModalPreviewPending.store(true);
            g_captureModalPreviewSec = timeSec;
        }
        g_captureModalPreviewWorkerActive.store(false);
    }).detach();
}

static void OpenCaptureModal(const std::string& video_path, int targetFilter) {
    if (video_path.empty()) return;

    g_captureModalPath = video_path;
    auto probe = VideoOptimizer::Probe(video_path);
    if (probe.valid) {
        g_captureModalDuration = (probe.duration > 0.0) ? probe.duration : 10.0;
        g_captureModalFps = (probe.fps > 0.0) ? probe.fps : 30.0;
        g_captureModalSourceW = probe.width > 0 ? probe.width : 1920;
        g_captureModalSourceH = probe.height > 0 ? probe.height : 1080;
    } else {
        g_captureModalDuration = (g_daemonDuration > 0.0) ? g_daemonDuration : 10.0;
        g_captureModalFps = (g_daemonVideoFps > 0.0) ? g_daemonVideoFps : 30.0;
        g_captureModalSourceW = g_daemonWidth > 0 ? g_daemonWidth : 1920;
        g_captureModalSourceH = g_daemonHeight > 0 ? g_daemonHeight : 1080;
    }

    auto& cfg = g_config.Get();
    if (targetFilter == 1) {
        g_captureModalTargetDesktop = true;
        g_captureModalTargetLockscreen = false;
    } else if (targetFilter == 2) {
        g_captureModalTargetDesktop = false;
        g_captureModalTargetLockscreen = true;
    } else {
        g_captureModalTargetDesktop = cfg.update_desktop_wallpaper;
        g_captureModalTargetLockscreen = cfg.update_lockscreen;
        if (!g_captureModalTargetDesktop && !g_captureModalTargetLockscreen) {
            g_captureModalTargetDesktop = true;
        }
    }

    // Default time: if this video matches configured custom desktop/lockscreen or active video, snap to that timestamp
    if (targetFilter == 1 && cfg.desktop_static_source_mode == 1 && cfg.desktop_static_video_path == video_path) {
        g_captureModalTimeSec = static_cast<float>(cfg.desktop_static_timestamp);
    } else if (targetFilter == 2 && cfg.lockscreen_source_mode == 1 && cfg.lockscreen_video_path == video_path) {
        g_captureModalTimeSec = static_cast<float>(cfg.lockscreen_timestamp);
    } else if (!g_daemonCurrentVideo.empty() && (g_daemonCurrentVideo == video_path || g_daemonCurrentVideo.find(fs::path(video_path).stem().string()) != std::string::npos)) {
        g_captureModalTimeSec = static_cast<float>(g_daemonCurrentTimeSec);
    } else {
        g_captureModalTimeSec = 1.0f;
    }
    if (g_captureModalTimeSec > g_captureModalDuration && g_captureModalDuration > 0.0) {
        g_captureModalTimeSec = static_cast<float>(g_captureModalDuration * 0.5);
    }
    g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
    g_captureModalStatus.clear();
    g_captureModalPreviewSRV.Reset();
    g_captureModalPreviewSec = -1.0f;
    g_showCaptureModal = true;
}

static void RenderCaptureFrameModal() {
    if (g_showCaptureModal) {
        ImGui::OpenPopup("Capture Static Wallpaper / Lock Screen Frame");
        if (!g_captureModalPreviewSRV && !g_captureModalPreviewWorkerActive.load() && g_captureModalPreviewSec < 0.0f) {
            RequestCaptureModalPreview();
        }
    }

    if (g_captureModalPreviewPending.exchange(false) && g_pd3dDevice) {
        g_captureModalPreviewSRV = ThumbnailManager::CreateSRVFromBGRA(
            g_pd3dDevice,
            g_captureModalPreviewBgra.data(),
            g_captureModalPreviewW,
            g_captureModalPreviewH
        );
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(680, 580));

    if (ImGui::BeginPopupModal("Capture Static Wallpaper / Lock Screen Frame", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_CAMERA "  Select Frame & Injection Destination");
        ImGui::Separator();
        ImGui::Spacing();

        fs::path p(g_captureModalPath);
        std::string filename = p.filename().string();
        if (filename.empty()) filename = g_captureModalPath;

        auto& cfg = g_config.Get();
        if (!cfg.gallery_history.empty()) {
            int currentIdx = -1;
            for (size_t i = 0; i < cfg.gallery_history.size(); ++i) {
                if (cfg.gallery_history[i] == g_captureModalPath) {
                    currentIdx = static_cast<int>(i);
                    break;
                }
            }
            std::string currentLabel = (currentIdx >= 0) ? fs::path(cfg.gallery_history[currentIdx]).filename().string() : filename;
            ImGui::SetNextItemWidth(450.0f);
            if (ImGui::BeginCombo("Select Video Source", currentLabel.c_str())) {
                for (size_t i = 0; i < cfg.gallery_history.size(); ++i) {
                    bool isSel = (static_cast<int>(i) == currentIdx);
                    std::string itemLabel = fs::path(cfg.gallery_history[i]).filename().string();
                    if (ImGui::Selectable(itemLabel.c_str(), isSel)) {
                        int filter = (g_captureModalTargetDesktop && !g_captureModalTargetLockscreen) ? 1 :
                                     (!g_captureModalTargetDesktop && g_captureModalTargetLockscreen) ? 2 : 0;
                        OpenCaptureModal(cfg.gallery_history[i], filter);
                    }
                    if (isSel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::TextColored(ImVec4(0.92f, 0.93f, 0.95f, 1.0f), "Video Source: %s", filename.c_str());
        }

        // Video info row
        int durMin = static_cast<int>(g_captureModalDuration) / 60;
        float durSec = static_cast<float>(g_captureModalDuration) - durMin * 60.0f;
        int totalFrames = static_cast<int>(g_captureModalDuration * g_captureModalFps);
        ImGui::TextColored(ImVec4(0.65f, 0.68f, 0.75f, 1.0f), "Resolution: %dx%d  |  Framerate: %.1f FPS  |  Duration: %02d:%05.2f (%d frames)",
            g_captureModalSourceW, g_captureModalSourceH, g_captureModalFps, durMin, durSec, totalFrames);
        ImGui::Spacing();

        // 1. Destination Checkboxes
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_DESKTOP "  Target Destination (Where to inject this frame):");
        ImGui::Checkbox("Apply to Windows Desktop Wallpaper (0s Instant Boot Visual)", &g_captureModalTargetDesktop);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Sets this frame directly into the native Windows Desktop Wallpaper.\nIt appears instantly in 0.0s upon Windows boot without black screen or delay.");
        }

        ImGui::Checkbox("Apply to Windows Lock Screen (Win + L Seamless Transition)", &g_captureModalTargetLockscreen);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Pre-caches this frame for the Windows Lock Screen.\nWhen pressing Win+L or locking the PC, this image is shown seamlessly.");
        }

        if (!g_captureModalTargetDesktop && !g_captureModalTargetLockscreen) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " Please select at least one destination (Desktop, Lock Screen, or Both).");
        } else if (g_captureModalTargetDesktop && g_captureModalTargetLockscreen) {
            ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.0f), "[ Selected Target: BOTH Desktop Wallpaper and Lock Screen ]");
        } else if (g_captureModalTargetDesktop) {
            ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.0f), "[ Selected Target: Desktop Wallpaper ONLY ]");
        } else {
            ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.0f), "[ Selected Target: Lock Screen ONLY ]");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 2. Frame & Timestamp Selection
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_FILM "  Frame & Timestamp Selection:");

        float maxTime = static_cast<float>((std::max)(0.1, g_captureModalDuration));
        
        // Time Slider
        int curMin = static_cast<int>(g_captureModalTimeSec) / 60;
        float curSec = g_captureModalTimeSec - curMin * 60.0f;
        char sliderFmt[64];
        sprintf_s(sliderFmt, "%%.2f s  (%02d:%05.2f | Frame #%d)", curMin, curSec, g_captureModalFrameNum);
        
        ImGui::SetNextItemWidth(520.0f);
        if (ImGui::SliderFloat("##TimeSlider", &g_captureModalTimeSec, 0.0f, maxTime, sliderFmt)) {
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
        }

        // Two-way Synced Numeric Inputs
        ImGui::PushItemWidth(140.0f);
        if (ImGui::InputFloat("Time (seconds)", &g_captureModalTimeSec, 0.1f, 1.0f, "%.2f s")) {
            g_captureModalTimeSec = std::clamp(g_captureModalTimeSec, 0.0f, maxTime);
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
        }
        ImGui::SameLine();
        if (ImGui::InputInt("Frame Number", &g_captureModalFrameNum, 1, 10)) {
            g_captureModalFrameNum = std::clamp(g_captureModalFrameNum, 0, (std::max)(1, totalFrames));
            g_captureModalTimeSec = static_cast<float>(g_captureModalFrameNum / g_captureModalFps);
            g_captureModalTimeSec = std::clamp(g_captureModalTimeSec, 0.0f, maxTime);
        }
        ImGui::PopItemWidth();

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE " Preview Frame")) {
            RequestCaptureModalPreview();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Render a thumbnail preview of the selected frame");
        }

        // Presets & Snap Button
        ImGui::Spacing();
        if (ImGui::Button("0s (Start)")) {
            g_captureModalTimeSec = 0.0f;
            g_captureModalFrameNum = 0;
            RequestCaptureModalPreview();
        }
        ImGui::SameLine();
        if (ImGui::Button("25%")) {
            g_captureModalTimeSec = maxTime * 0.25f;
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            RequestCaptureModalPreview();
        }
        ImGui::SameLine();
        if (ImGui::Button("50% (Middle)")) {
            g_captureModalTimeSec = maxTime * 0.50f;
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            RequestCaptureModalPreview();
        }
        ImGui::SameLine();
        if (ImGui::Button("75%")) {
            g_captureModalTimeSec = maxTime * 0.75f;
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            RequestCaptureModalPreview();
        }
        ImGui::SameLine();
        if (ImGui::Button("End")) {
            g_captureModalTimeSec = (std::max)(0.0f, maxTime - 0.1f);
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            RequestCaptureModalPreview();
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_CROSSHAIRS " Snap to Live Video")) {
            g_captureModalTimeSec = static_cast<float>(g_daemonCurrentTimeSec);
            g_captureModalTimeSec = std::clamp(g_captureModalTimeSec, 0.0f, maxTime);
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            RequestCaptureModalPreview();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Snap time to the exact position currently playing on your desktop");
        }

        // Preview box
        ImGui::Spacing();
        float previewBoxW = 320.0f;
        float previewBoxH = 180.0f;
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.75f, 1.0f), "Frame Preview (Selected Time: %.2f s | Frame #%d):", g_captureModalTimeSec, g_captureModalFrameNum);
        if (g_captureModalPreviewSRV) {
            ImGui::Image((ImTextureID)g_captureModalPreviewSRV.Get(), ImVec2(previewBoxW, previewBoxH));
        } else {
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImVec2 p1 = ImVec2(p0.x + previewBoxW, p0.y + previewBoxH);
            draw_list->AddRectFilled(p0, p1, IM_COL32(20, 22, 28, 255), 4.0f);
            draw_list->AddRect(p0, p1, IM_COL32(45, 50, 62, 255), 4.0f);
            if (g_captureModalPreviewWorkerActive.load()) {
                draw_list->AddText(ImVec2(p0.x + 80.0f, p0.y + 80.0f), IM_COL32(100, 200, 255, 255), "Extracting Preview...");
            } else {
                draw_list->AddText(ImVec2(p0.x + 70.0f, p0.y + 80.0f), IM_COL32(140, 145, 155, 255), "Click 'Preview' to view frame");
            }
            ImGui::Dummy(ImVec2(previewBoxW, previewBoxH));
        }

        // Status message if any
        if (!g_captureModalStatus.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.0f), "%s", g_captureModalStatus.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 3. Action Buttons
        bool canApply = (g_captureModalTargetDesktop || g_captureModalTargetLockscreen);
        if (!canApply) ImGui::BeginDisabled();
        if (ImGui::Button(ICON_FA_CHECK "  Capture & Apply Static Wallpaper", ImVec2(320, 36))) {
            if (g_captureModalTargetDesktop) {
                cfg.desktop_static_source_mode = 1;
                cfg.desktop_static_video_path = g_captureModalPath;
                cfg.desktop_static_timestamp = static_cast<double>(g_captureModalTimeSec);
                cfg.update_desktop_wallpaper = true;
            }
            if (g_captureModalTargetLockscreen) {
                cfg.lockscreen_source_mode = 1;
                cfg.lockscreen_video_path = g_captureModalPath;
                cfg.lockscreen_timestamp = static_cast<double>(g_captureModalTimeSec);
                cfg.update_lockscreen = true;
            }
            g_config.Save();

            nlohmann::json req{
                {"cmd", "sync_desktop_wallpaper"},
                {"path", g_captureModalPath},
                {"desktop", g_captureModalTargetDesktop},
                {"lockscreen", g_captureModalTargetLockscreen},
                {"timestamp", static_cast<double>(g_captureModalTimeSec)},
                {"save_mode", 1}
            };
            SendIpcAsync(req.dump());

            std::string destStr = (g_captureModalTargetDesktop && g_captureModalTargetLockscreen) ? "Desktop & Lock Screen" :
                                  (g_captureModalTargetDesktop ? "Desktop Wallpaper" : "Lock Screen");
            char statusMsg[256];
            sprintf_s(statusMsg, "Pristine static frame at %.2fs successfully injected to %s!", g_captureModalTimeSec, destStr.c_str());
            g_captureModalStatus = statusMsg;

            g_showCaptureModal = false;
            ImGui::CloseCurrentPopup();
        }
        if (!canApply) ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 36))) {
            g_showCaptureModal = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

static void RenderOptimizeModal() {
    if (g_showOptimizeModal) {
        ImGui::OpenPopup("Optimize Video for Display?");
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(620, 310));

    if (ImGui::BeginPopupModal("Optimize Video for Display?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_COMPACT_DISC "  High-Resolution Video Detected");
        ImGui::Separator();
        ImGui::Spacing();

        int screen_w = GetSystemMetrics(SM_CXSCREEN);
        int screen_h = GetSystemMetrics(SM_CYSCREEN);
        if (screen_w <= 0) screen_w = 1920;
        if (screen_h <= 0) screen_h = 1080;

        auto& cfg = g_config.Get();

        ImGui::Text("Source Video Resolution : %dx%d", g_pendingSourceW, g_pendingSourceH);

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_EXPAND "  Optimization Scaling Mode:");

        if (ImGui::RadioButton("Aspect Fit (Proportional - keeps entire frame intact)", &cfg.optimizer_crop_mode, 0)) {
            g_config.Save();
        }
        if (ImGui::RadioButton("Aspect Fill / Center Crop (Exact Full Screen 1080p - zero black bars)", &cfg.optimizer_crop_mode, 1)) {
            g_config.Save();
        }

        auto [target_w, target_h] = VideoOptimizer::CalculateTargetDimensions(g_pendingSourceW, g_pendingSourceH, screen_w, screen_h, cfg.optimizer_crop_mode);
        g_pendingTargetW = target_w;
        g_pendingTargetH = target_h;

        ImGui::Spacing();
        if (cfg.optimizer_crop_mode == 0) {
            ImGui::Text("Target Resolution : %dx%d (Proportional Letterbox)", g_pendingTargetW, g_pendingTargetH);
        } else {
            ImGui::Text("Target Resolution : %dx%d (Full Screen Center-Crop)", g_pendingTargetW, g_pendingTargetH);
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.00f),
            "*Optimizing saves up to 75%% GPU & VRAM with crystal-clear Lanczos downscaling!");
        
        ImGui::Spacing();
        ImGui::Checkbox("Remember my choice (Auto-downscale in the future)", &g_rememberDownscaleChoice);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button(ICON_FA_DOWNLOAD "  Optimize Video (Recommended)", ImVec2(260, 36))) {
            if (g_rememberDownscaleChoice) {
                cfg.auto_downscale_highres = true;
                cfg.prompt_downscale = false;
                g_config.Save();
            }
            g_showOptimizeModal = false;
            ImGui::CloseCurrentPopup();
            StartVideoOptimization(g_pendingOptimizePath, g_pendingTargetW, g_pendingTargetH, g_pendingOptimizeAction, cfg.optimizer_crop_mode);
        }

        ImGui::SameLine();
        if (ImGui::Button("Play Original", ImVec2(130, 36))) {
            if (g_rememberDownscaleChoice) {
                auto& cfg = g_config.Get();
                cfg.auto_downscale_highres = false;
                cfg.prompt_downscale = false;
                g_config.Save();
            }
            g_showOptimizeModal = false;
            ImGui::CloseCurrentPopup();
            ApplyAction(g_pendingOptimizePath, g_pendingOptimizeAction);
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 36))) {
            g_showOptimizeModal = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

static void RenderOptimizationProgress() {
    if (!g_video_optimizer.IsRunning()) return;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.18f, 0.28f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.70f, 1.00f, 1.00f));
    ImGui::BeginChild("OptProgressBanner", ImVec2(0, 52), true, ImGuiWindowFlags_NoScrollbar);

    float prog = g_video_optimizer.GetProgress();
    std::string status = g_video_optimizer.GetCurrentStatus();

    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_ROTATE "  %s", status.c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - 95);
    if (ImGui::Button("Cancel", ImVec2(80, 20))) {
        g_video_optimizer.Cancel();
    }

    ImGui::ProgressBar(prog, ImVec2(-1, 14), "");

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::Spacing();
}

bool SettingsUI::Open(HINSTANCE hInstance) {
    if (g_hWnd) {
        ShowWindow(g_hWnd, SW_RESTORE);
        ShowWindow(g_hWnd, SW_SHOW);
        SetForegroundWindow(g_hWnd);
        g_isOpen = true;
        return true;
    }

    g_hInstance = hInstance;

    WNDCLASSEXW wc = {
        sizeof(wc),
        CS_CLASSDC,
        SettingsWndProc,
        0L, 0L,
        hInstance,
        LoadIconW(hInstance, MAKEINTRESOURCEW(1)),
        LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)), // IDC_ARROW
        nullptr, nullptr,
        L"LiteWallpaper_SettingsClass",
        nullptr
    };
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int winW = 960;
    int winH = 700;
    int winX = (sw > winW) ? (sw - winW) / 2 : 100;
    int winY = (sh > winH) ? (sh - winH) / 2 : 100;

    g_hWnd = CreateWindowExW(
        WS_EX_LAYERED,
        wc.lpszClassName,
        L"LiteWallpaper Control Panel",
        WS_OVERLAPPEDWINDOW,
        winX, winY, winW, winH,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (!g_hWnd || !CreateDeviceD3D(g_hWnd)) {
        CleanupDeviceD3D();
        if (g_hWnd) DestroyWindow(g_hWnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        g_hWnd = nullptr;
        return false;
    }

    SetLayeredWindowAttributes(g_hWnd, 0, 235, LWA_ALPHA);

    SendMessageW(g_hWnd, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    SendMessageW(g_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIcon);

    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(g_hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    COLORREF captionColor = RGB(20, 20, 26);
    DwmSetWindowAttribute(g_hWnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));

    DragAcceptFiles(g_hWnd, TRUE);

    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    SetupImGuiStyle();

    ImFont* mainFont = nullptr;
    if (fs::exists("C:\\Windows\\Fonts\\segoeui.ttf")) {
        mainFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f);
    }
    if (!mainFont) {
        io.Fonts->AddFontDefault();
    }

    static const ImWchar icons_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    ImFontConfig icons_config;
    icons_config.MergeMode = true;
    icons_config.PixelSnapH = true;
    icons_config.GlyphMinAdvanceX = 16.0f;

    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    fs::path fontPath = fs::path(exePath).parent_path() / "assets" / "fa-solid-900.ttf";

    if (fs::exists(fontPath)) {
        io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), 15.0f, &icons_config, icons_ranges);
    }

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    g_config.Load();
    g_isOpen = true;
    return true;
}

void SettingsUI::RenderFrame() {
    if (!g_isOpen || !g_hWnd || !IsWindowVisible(g_hWnd) || !g_pd3dDeviceContext || !g_mainRenderTargetView) return;

    FetchDaemonStatus();
    ThumbnailManager::Instance().Update(g_pd3dDevice);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    RECT rect;
    GetClientRect(g_hWnd, &rect);
    int winWidth = rect.right - rect.left;
    int winHeight = rect.bottom - rect.top;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(winWidth), static_cast<float>(winHeight)));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::Begin("LiteWallpaper Control Panel", nullptr, flags);

    RenderOptimizationProgress();

    if (ImGui::BeginTabBar("MainTabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem(ICON_FA_IMAGES "  Wallpaper Gallery")) {
            RenderGalleryTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_FA_LOCK "  Lock Screen & Static")) {
            RenderLockscreenTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_FA_SLIDERS "  General Settings")) {
            RenderGeneralSettingsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_FA_GAUGE_HIGH "  Performance & Advanced")) {
            RenderPerformancePanel();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    RenderOptimizeModal();
    RenderCaptureFrameModal();

    ImGui::End();

    ImGui::Render();
    const float clearColor[4] = { 0.08f, 0.08f, 0.10f, 0.90f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    g_pSwapChain->Present(1, 0);
}

bool SettingsUI::IsOpen() {
    return g_isOpen && g_hWnd && IsWindowVisible(g_hWnd);
}

void SettingsUI::Close() {
    if (g_hWnd) {
        ShowWindow(g_hWnd, SW_HIDE);
    }
    g_isOpen = false;
}

void SettingsUI::Shutdown() {
    if (!g_hWnd) return;

    g_isOpen = false;

    ThumbnailManager::Instance().ReleaseTextures();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();

    if (g_hWnd) {
        DestroyWindow(g_hWnd);
        UnregisterClassW(L"LiteWallpaper_SettingsClass", g_hInstance);
        g_hWnd = nullptr;
    }
}

HWND SettingsUI::GetHwnd() {
    return g_hWnd;
}

} // namespace litewp
