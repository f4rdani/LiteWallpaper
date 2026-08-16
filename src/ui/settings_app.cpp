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
    colors[ImGuiCol_WindowBg]              = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.11f, 0.11f, 0.14f, 1.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.12f, 0.12f, 0.16f, 0.98f);
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
    } else if (action == "lockscreen") {
        nlohmann::json req{{"cmd", "set_lockscreen"}, {"path", utf8_path}};
        SendIpcAsync(req.dump());
    } else if (action == "both") {
        nlohmann::json req{{"cmd", "set_both"}, {"path", utf8_path}};
        SendIpcAsync(req.dump());
    } else if (action == "stop") {
        SendIpcAsync("{\"cmd\":\"stop\"}");
    } else if (action == "resume") {
        SendIpcAsync("{\"cmd\":\"resume\"}");
    }
}

static void StartVideoOptimization(const std::string& input_path, int target_w, int target_h, const std::string& action) {
    // Add source path to gallery immediately so user sees card right away
    g_config.Get().AddToGallery(input_path);
    g_config.Save();

    g_video_optimizer.StartOptimizeAsync(
        input_path,
        target_w,
        target_h,
        nullptr,
        [action](bool success, const std::string& output_path) {
            if (success && !output_path.empty()) {
                ApplyAction(output_path, action);
            }
        }
    );
}

static void RequestApplyVideo(std::string utf8_path, std::string action) {
    if (utf8_path.empty()) return;
    if (action == "stop" || action == "resume") {
        ApplyAction(utf8_path, action);
        return;
    }

    // Always ensure source path is in gallery
    g_config.Get().AddToGallery(utf8_path);
    g_config.Save();

    auto& cfg = g_config.Get();
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    if (screen_w <= 0) screen_w = 1920;
    if (screen_h <= 0) screen_h = 1080;

    // Check if optimized version already exists in cache
    if (VideoOptimizer::HasOptimizedCache(utf8_path, screen_w, screen_h)) {
        std::string opt_path = VideoOptimizer::GetOptimizedPath(utf8_path, screen_w, screen_h);
        ApplyAction(opt_path, action);
        return;
    }

    // Probe source video dimensions
    auto probe = VideoOptimizer::Probe(utf8_path);
    if (probe.valid && (probe.width > screen_w || probe.height > screen_h)) {
        if (cfg.prompt_downscale) {
            g_pendingOptimizePath = utf8_path;
            g_pendingOptimizeAction = action;
            g_pendingSourceW = probe.width;
            g_pendingSourceH = probe.height;
            g_pendingTargetW = screen_w;
            g_pendingTargetH = screen_h;
            g_showOptimizeModal = true;
            return;
        } else if (cfg.auto_downscale_highres) {
            StartVideoOptimization(utf8_path, screen_w, screen_h, action);
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
    int numCols = (availW >= 740.0f) ? static_cast<int>(availW / 370.0f) : 1;
    if (numCols < 1) numCols = 1;

    float spacingX = ImGui::GetStyle().ItemSpacing.x;
    float cardWidth = (availW - (numCols - 1) * spacingX) / static_cast<float>(numCols);
    if (cardWidth < 280.0f) cardWidth = availW;

    float windowVisibleX2 = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;

    for (size_t i = 0; i < galleryCopy.size(); ++i) {
        const auto& path = galleryCopy[i];
        fs::path p(path);
        std::string filename = p.filename().string();
        if (filename.empty()) filename = path;

        std::string opt_path = VideoOptimizer::GetOptimizedPath(path, screen_w, screen_h);
        bool has_opt = VideoOptimizer::HasOptimizedCache(path, screen_w, screen_h);

        bool is_playing_opt = (!g_daemonCurrentVideo.empty() && g_daemonCurrentVideo == opt_path);
        bool is_playing_ori = (!g_daemonCurrentVideo.empty() && g_daemonCurrentVideo == path);
        bool is_current = is_playing_opt || is_playing_ori ||
                          (!cfg.wallpapers.empty() && (cfg.wallpapers[0].video_path == path || cfg.wallpapers[0].video_path == opt_path));

        ImGui::PushID(static_cast<int>(i));

        if (is_current) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.22f, 0.32f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.75f, 1.00f, 1.00f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.14f, 0.18f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.24f, 0.30f, 1.00f));
        }

        ImGui::BeginChild("Card", ImVec2(cardWidth, 185), true, ImGuiWindowFlags_NoScrollbar);

        if (is_current) {
            if (is_playing_opt) {
                ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.00f), ICON_FA_CIRCLE_PLAY "  [ ACTIVE: 1080p OPTIMIZED ]");
            } else {
                ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.00f), ICON_FA_CIRCLE_PLAY "  [ ACTIVE: ORIGINAL VIDEO ]");
            }
        } else {
            if (has_opt) {
                ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_COMPACT_DISC "  Video [ Original + 1080p Ready ]");
            } else {
                ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_FILM "  Video File");
            }
        }
        ImGui::TextUnformatted(filename.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", path.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float innerW = ImGui::GetContentRegionAvail().x;
        float itemPad = ImGui::GetStyle().ItemSpacing.x;

        // Row 1: Primary Playback Actions
        if (is_current) {
            float stopW = (innerW - itemPad) * 0.38f;
            float switchW = innerW - stopW - itemPad;
            if (stopW < 75.0f) stopW = 75.0f;
            if (switchW < 90.0f) switchW = 90.0f;

            if (g_daemonPaused) {
                if (ImGui::Button(ICON_FA_PLAY " Resume", ImVec2(stopW, 28))) {
                    RequestApplyVideo(path, "resume");
                }
            } else {
                if (ImGui::Button(ICON_FA_STOP " Stop", ImVec2(stopW, 28))) {
                    RequestApplyVideo(path, "stop");
                }
            }

            ImGui::SameLine();
            if (is_playing_opt) {
                if (ImGui::Button(ICON_FA_PLAY " Play Original", ImVec2(switchW, 28))) {
                    ApplyAction(path, "wallpaper");
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Switch playback to original high-resolution video");
                }
            } else {
                if (has_opt) {
                    if (ImGui::Button(ICON_FA_PLAY " Play 1080p", ImVec2(switchW, 28))) {
                        ApplyAction(opt_path, "wallpaper");
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Switch playback to 1080p optimized version");
                    }
                } else {
                    if (ImGui::Button(ICON_FA_DOWNLOAD " Optimize", ImVec2(switchW, 28))) {
                        StartVideoOptimization(path, screen_w, screen_h, "wallpaper");
                    }
                }
            }
        } else {
            if (has_opt) {
                float halfW = (innerW - itemPad) * 0.5f;
                if (ImGui::Button(ICON_FA_PLAY " Play 1080p", ImVec2(halfW, 28))) {
                    ApplyAction(opt_path, "wallpaper");
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Play lightweight 1080p version (Saves VRAM & GPU load)");
                }
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_PLAY " Original", ImVec2(halfW, 28))) {
                    ApplyAction(path, "wallpaper");
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Play original high-resolution video");
                }
            } else {
                float playW = (innerW - itemPad) * 0.65f;
                float optW = innerW - playW - itemPad;
                if (ImGui::Button(ICON_FA_PLAY " Play Wallpaper", ImVec2(playW, 28))) {
                    RequestApplyVideo(path, "wallpaper");
                }
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_DOWNLOAD " Opt", ImVec2(optW, 28))) {
                    StartVideoOptimization(path, screen_w, screen_h, "wallpaper");
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Pre-render 1080p optimized version to save ~75% GPU");
                }
            }
        }

        ImGui::Spacing();

        // Row 2: Target Desktop/Lock & Delete Management
        float delW = 34.0f;
        float actW = (innerW - delW - (2 * itemPad)) * 0.5f;
        if (actW < 50.0f) actW = 50.0f;

        std::string target_apply_path = (is_current && is_playing_opt) ? opt_path : (has_opt ? opt_path : path);

        if (ImGui::Button(ICON_FA_LOCK " Lock", ImVec2(actW, 26))) {
            ApplyAction(target_apply_path, "lockscreen");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Set current frame as Windows Lock Screen");
        }

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_IMAGES " Both", ImVec2(actW, 26))) {
            ApplyAction(target_apply_path, "both");
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Set as Desktop Wallpaper AND Lock Screen");
        }

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_TRASH, ImVec2(delW, 26))) {
            cfg.RemoveFromGallery(path);
            g_config.Save();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Remove video from gallery");
        }

        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopID();

        float lastButtonX2 = ImGui::GetItemRectMax().x;
        float nextButtonX2 = lastButtonX2 + spacingX + cardWidth;
        if (i + 1 < galleryCopy.size() && nextButtonX2 < windowVisibleX2) {
            ImGui::SameLine();
        }
    }

    ImGui::EndChild();
}

static void RenderSettingsPanel() {
    auto& cfg = g_config.Get();

    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_SLIDERS "  Display & Performance Settings");
    ImGui::Separator();

    float availW = ImGui::GetContentRegionAvail().x;
    float comboW = (std::min)(450.0f, availW);

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

    // Multi-Monitor Target Checkboxes in Settings
    if (g_hardwareInfo.displays.size() > 1) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_TV "  Multi-Desktop / Target Display Selection");
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.75f, 1.0f), "Choose which desktop screen(s) will display the active wallpaper:");

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

    // Dynamic Hardware / Rendering Device Selector

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

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_GAUGE_HIGH "  Frame Rate & Resource Control");

    float sliderW = (std::min)(280.0f, availW);
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

    if (ImGui::Checkbox("Auto-Pause when Fullscreen App/Game is active", &cfg.pause_on_fullscreen)) {
        g_config.Save();
    }
    if (ImGui::Checkbox("Auto-Pause on Battery Power", &cfg.pause_on_battery)) {
        g_config.Save();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_VOLUME_HIGH "  Audio Configuration");

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
    ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.00f), ICON_FA_CIRCLE_CHECK "  All settings are saved and applied automatically in real-time.");
    ImGui::Spacing();

    if (ImGui::Button(ICON_FA_EYE_SLASH "  Hide Window to Tray", ImVec2(200, 34))) {
        SettingsUI::Close();
    }
}

static void RenderPerformancePanel() {
    if (!g_hardwareInfoInit) {
        g_hardwareInfo = HardwareDetector::QuerySystemInfo();
        g_hardwareInfoInit = true;
    }

    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_MICROCHIP "  Real-Time Engine Monitor");
    ImGui::Separator();

    if (!g_daemonConnected) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Engine Status: Background Engine Initializing...");
        return;
    }

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
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_CHART_LINE "  Resource Telemetry");

    ImGui::Text("Process CPU Usage: %.1f %%", g_daemonCpuPercent);
    ImGui::PlotLines("CPU (%)", g_cpuHistory.data(), (int)g_cpuHistory.size(), 0, nullptr, 0.0f, 100.0f, ImVec2(0, 50));

    ImGui::Text("Process RAM (Working Set): %zu MB (Target: < 45 MB)", g_daemonRamMB);
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

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_TV "  Connected Monitors & Display Topology");
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
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_MICROCHIP "  Hardware Topology (CPU & GPU Detection)");
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_ROTATE " Re-detect Hardware")) {
        g_hardwareInfo = HardwareDetector::QuerySystemInfo();
    }

    // CPU info
    if (g_daemonActiveGpuIndex == -1) {
        ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.00f),
            "• Processor (CPU): %s [ACTIVE RENDERING ENGINE (SOFTWARE CPU)]", g_hardwareInfo.cpu.model_name.c_str());
    } else {
        ImGui::TextColored(ImVec4(0.92f, 0.93f, 0.95f, 1.00f),
            "• Processor (CPU): %s", g_hardwareInfo.cpu.model_name.c_str());
    }
    ImGui::Text("   Cores / Topology: %d Physical Cores, %d Logical Threads",
                g_hardwareInfo.cpu.physical_cores, g_hardwareInfo.cpu.logical_cores);

    // GPUs info (GPU 1, GPU 2, etc.)
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
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_CIRCLE_INFO "  Diagnostics & Injection");
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
    if (ImGui::Button(ICON_FA_EXPAND "  Flash Render Window (Diagnostic)", ImVec2(280, 28))) {
        SendIpcAsync("{\"cmd\":\"test_render\"}");
    }
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.65f, 1.0f),
        "*If the desktop turns green after flashing, injection works. Log: %%APPDATA%%\\LiteWallpaper\\engine.log");
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

        ImGui::Text("Source Video Resolution : %dx%d (4K / Ultra-HD)", g_pendingSourceW, g_pendingSourceH);
        ImGui::Text("Desktop Screen Resolution: %dx%d (%s)", g_pendingTargetW, g_pendingTargetH,
                    (g_pendingTargetW == 1920 && g_pendingTargetH == 1080) ? "1080p Full HD" : "Display Native");
        
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.00f),
            "*Downscaling to match your screen resolution will save ~75%% GPU Decode & ~50MB VRAM!");
        
        ImGui::Spacing();
        ImGui::Checkbox("Remember my choice (Auto-downscale in the future)", &g_rememberDownscaleChoice);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button(ICON_FA_DOWNLOAD "  Optimize for Display (Recommended)", ImVec2(270, 36))) {
            if (g_rememberDownscaleChoice) {
                auto& cfg = g_config.Get();
                cfg.auto_downscale_highres = true;
                cfg.prompt_downscale = false;
                g_config.Save();
            }
            g_showOptimizeModal = false;
            ImGui::CloseCurrentPopup();
            StartVideoOptimization(g_pendingOptimizePath, g_pendingTargetW, g_pendingTargetH, g_pendingOptimizeAction);
        }

        ImGui::SameLine();
        if (ImGui::Button("Play Original 4K", ImVec2(140, 36))) {
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

    g_hWnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"LiteWallpaper Control Panel",
        WS_OVERLAPPEDWINDOW,
        150, 150, 820, 680,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (!g_hWnd || !CreateDeviceD3D(g_hWnd)) {
        CleanupDeviceD3D();
        if (g_hWnd) DestroyWindow(g_hWnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        g_hWnd = nullptr;
        return false;
    }

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
        if (ImGui::BeginTabItem(ICON_FA_SLIDERS "  Settings & Display")) {
            RenderSettingsPanel();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_FA_MICROCHIP "  Performance & Diagnostics")) {
            RenderPerformancePanel();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    RenderOptimizeModal();

    ImGui::End();

    ImGui::Render();
    const float clearColor[4] = { 0.08f, 0.08f, 0.10f, 1.00f };
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
