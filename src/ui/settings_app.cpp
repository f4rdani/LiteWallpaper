#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "settings_app.h"
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <filesystem>
#include <vector>
#include <string>
#include <mimalloc.h>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include "core/config.h"
#include "core/ipc_server.h"
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

static Config                   g_settingsConfig;
static IpcClient                g_ipcClient;

// Performance metrics cache
static bool   g_daemonConnected = false;
static bool   g_daemonPlaying = false;
static bool   g_daemonPaused = false;
static int    g_daemonFps = 0;
static double g_daemonVideoFps = 0.0;
static int    g_daemonWidth = 0;
static int    g_daemonHeight = 0;
static double g_daemonDuration = 0.0;
static std::string g_daemonCodec = "";
static size_t g_daemonRamMB = 0;
static std::vector<float> g_ramHistory(60, 0.0f);
static int    g_fetchCounter = 0;

static void SetupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 5.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 1.0f;
    style.ItemSpacing = ImVec2(10, 8);
    style.FramePadding = ImVec2(8, 6);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]             = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_ChildBg]              = ImVec4(0.11f, 0.11f, 0.14f, 1.00f);
    colors[ImGuiCol_PopupBg]              = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    colors[ImGuiCol_Border]               = ImVec4(0.20f, 0.22f, 0.28f, 0.80f);
    colors[ImGuiCol_FrameBg]              = ImVec4(0.15f, 0.16f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.22f, 0.24f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.28f, 0.30f, 0.38f, 1.00f);
    colors[ImGuiCol_TitleBg]              = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgActive]        = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    colors[ImGuiCol_Button]               = ImVec4(0.16f, 0.32f, 0.54f, 1.00f);
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.22f, 0.44f, 0.72f, 1.00f);
    colors[ImGuiCol_ButtonActive]         = ImVec4(0.13f, 0.26f, 0.46f, 1.00f);
    colors[ImGuiCol_Header]               = ImVec4(0.16f, 0.32f, 0.54f, 0.80f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(0.22f, 0.44f, 0.72f, 0.80f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(0.13f, 0.26f, 0.46f, 1.00f);
    colors[ImGuiCol_Tab]                  = ImVec4(0.12f, 0.13f, 0.17f, 1.00f);
    colors[ImGuiCol_TabHovered]           = ImVec4(0.22f, 0.44f, 0.72f, 0.80f);
    colors[ImGuiCol_TabActive]            = ImVec4(0.16f, 0.32f, 0.54f, 1.00f);
    colors[ImGuiCol_TabUnfocused]         = ImVec4(0.09f, 0.10f, 0.13f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]  = ImVec4(0.14f, 0.16f, 0.22f, 1.00f);
    colors[ImGuiCol_SliderGrab]           = ImVec4(0.35f, 0.60f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.45f, 0.75f, 1.00f, 1.00f);
    colors[ImGuiCol_CheckMark]            = ImVec4(0.40f, 0.85f, 1.00f, 1.00f);
}

static void CreateRenderTarget() {
    if (!g_pSwapChain || !g_pd3dDevice) return;
    ComPtr<ID3D11Texture2D> pBackBuffer;
    if (SUCCEEDED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)))) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer.Get(), nullptr, &g_mainRenderTargetView);
    }
}

static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
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

static void ApplyAction(std::string utf8_path, std::string action) {
    if (utf8_path.empty()) return;
    g_settingsConfig.Get().AddToGallery(utf8_path);
    g_settingsConfig.Save();

    if (action == "wallpaper") {
        nlohmann::json req{{"cmd", "set_wallpaper"}, {"path", utf8_path}};
        g_ipcClient.SendRequest(req.dump());
    } else if (action == "lockscreen") {
        nlohmann::json req{{"cmd", "set_lockscreen"}, {"path", utf8_path}};
        g_ipcClient.SendRequest(req.dump());
    } else if (action == "both") {
        nlohmann::json req{{"cmd", "set_both"}, {"path", utf8_path}};
        g_ipcClient.SendRequest(req.dump());
    }
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
                        ApplyAction(utf8_path, "wallpaper");
                        break;
                    }
                }
            }
            DragFinish(hDrop);
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
    std::string response = g_ipcClient.SendRequest("{\"cmd\":\"get_status\"}");
    auto res = nlohmann::json::parse(response, nullptr, false);
    if (!res.is_discarded() && res.is_object() && res.value("ok", false)) {
        g_daemonConnected = true;
        g_daemonPlaying = res.value("playing", false);
        g_daemonPaused = res.value("paused", false);
        g_daemonFps = res.value("fps", 0);
        g_daemonVideoFps = res.value("video_fps", 0.0);
        g_daemonWidth = res.value("width", 0);
        g_daemonHeight = res.value("height", 0);
        g_daemonDuration = res.value("duration", 0.0);
        g_daemonCodec = res.value("codec", "unknown");
        g_daemonRamMB = res.value("ram_mb", 0);

        g_ramHistory.erase(g_ramHistory.begin());
        g_ramHistory.push_back(static_cast<float>(g_daemonRamMB));
    } else {
        g_daemonConnected = false;
    }
}

static void RenderGalleryPanel() {
    auto& cfg = g_settingsConfig.Get();

    // Drag and Drop Area Banner with Icon
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.55f, 0.95f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.15f, 0.22f, 0.90f));
    ImGui::BeginChild("DropZoneBanner", ImVec2(0, 52), true);
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f - 190.0f);
    ImGui::SetCursorPosY(15.0f);
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_DOWNLOAD "  Drag & Drop Video Files Anywhere Here");
    ImGui::EndChild();
    ImGui::PopStyleColor(2);

    ImGui::Spacing();

    // File Browse / Folder Scanner
    static char folderPath[MAX_PATH] = "C:\\";
    static std::vector<std::string> scannedFiles;

    ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.90f, 1.00f), "Add New Video Wallpaper:");
    if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Browse Video File...", ImVec2(195, 32))) {
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
            ApplyAction(utf8_path, "wallpaper");
        }
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(320);
    ImGui::InputText("##FolderPath", folderPath, MAX_PATH);
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_MAGNIFYING_GLASS "  Scan Folder", ImVec2(125, 32))) {
        scannedFiles.clear();
        std::error_code ec;
        if (fs::exists(folderPath, ec) && fs::is_directory(folderPath, ec)) {
            for (const auto& entry : fs::directory_iterator(folderPath, ec)) {
                if (entry.is_regular_file(ec)) {
                    auto ext = entry.path().extension().string();
                    for (auto& c : ext) c = (char)::tolower(c);
                    if (ext == ".mp4" || ext == ".webm" || ext == ".mkv" || ext == ".avi" || ext == ".mov") {
                        scannedFiles.push_back(entry.path().string());
                    }
                }
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Make a local snapshot copy of gallery history to prevent iterator invalidation
    std::vector<std::string> currentGallery = cfg.gallery_history;

    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.00f), ICON_FA_FILM "  Wallpaper Gallery (%d saved):", (int)currentGallery.size());

    ImGui::BeginChild("GalleryHistoryList", ImVec2(0, 260), true);
    if (currentGallery.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.65f, 1.0f), "No wallpapers in gallery yet. Drag & drop a video file or click Browse Video File!");
    } else {
        std::string toRemove = "";
        for (size_t i = 0; i < currentGallery.size(); i++) {
            std::string file = currentGallery[i];
            if (file.empty()) continue;

            std::string filenameOnly = fs::path(file).filename().string();
            if (filenameOnly.empty()) filenameOnly = file;

            ImGui::PushID((int)i);
            
            // Render an individual interactive card container
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.14f, 0.15f, 0.20f, 0.90f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.24f, 0.28f, 0.38f, 0.60f));
            ImGui::BeginChild("CardItem", ImVec2(0, 72), true);

            // Card Header & Filename
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.00f, 1.00f), "%s  %s", ICON_FA_FILM, filenameOnly.c_str());
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.60f, 1.00f), "- %s", file.c_str());

            ImGui::Spacing();

            // 3 Apply Target Action Buttons
            if (ImGui::Button(ICON_FA_PLAY "  Set Wallpaper", ImVec2(145, 26))) {
                ApplyAction(file, "wallpaper");
            }
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.25f, 0.55f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.35f, 0.75f, 1.0f));
            if (ImGui::Button(ICON_FA_IMAGE "  Set Lock Screen", ImVec2(155, 26))) {
                ApplyAction(file, "lockscreen");
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.48f, 0.32f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.65f, 0.42f, 1.0f));
            if (ImGui::Button(ICON_FA_CIRCLE_CHECK "  Apply Both", ImVec2(120, 26))) {
                ApplyAction(file, "both");
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.15f, 0.15f, 0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.20f, 0.20f, 1.0f));
            if (ImGui::Button(ICON_FA_TRASH, ImVec2(32, 26))) {
                toRemove = file;
            }
            ImGui::PopStyleColor(2);

            ImGui::EndChild();
            ImGui::PopStyleColor(2);

            ImGui::Spacing();
            ImGui::PopID();
        }
        if (!toRemove.empty()) {
            cfg.RemoveFromGallery(toRemove);
            g_settingsConfig.Save();
        }
    }
    ImGui::EndChild();

    if (!scannedFiles.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.75f, 0.85f, 1.0f, 1.0f), ICON_FA_COMPACT_DISC "  Scanned Folder Videos (%d found):", (int)scannedFiles.size());
        ImGui::BeginChild("ScannedFolderList", ImVec2(0, 100), true);
        for (const auto& file : scannedFiles) {
            std::string filenameOnly = fs::path(file).filename().string();
            if (ImGui::Button(filenameOnly.c_str(), ImVec2(-1, 26))) {
                ApplyAction(file, "wallpaper");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", file.c_str());
            }
        }
        ImGui::EndChild();
    }
}

static void RenderSettingsPanel() {
    auto& cfg = g_settingsConfig.Get();

    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_DESKTOP "  Display & Aspect Ratio Scaling");
    ImGui::Separator();

    const char* scalingModes[] = {
        "Auto Fill / Cover (No Black Bars - Best for Vertical & Ultrawide)",
        "Aspect Fit (Letterbox - Keep Full Video with Margins)",
        "Stretch (Fill Display Area)"
    };
    int currentMode = cfg.scaling_mode;
    ImGui::SetNextItemWidth(500);
    if (ImGui::Combo("##ScalingMode", &currentMode, scalingModes, IM_ARRAYSIZE(scalingModes))) {
        cfg.scaling_mode = currentMode;
        g_settingsConfig.Save();
        nlohmann::json req{{"cmd", "set_scaling"}, {"mode", currentMode}};
        g_ipcClient.SendRequest(req.dump());
    }
    ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.70f, 1.00f), "*Auto Fill proportionally scales and centers videos across vertical & horizontal monitors.");

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_GEARS "  Playback & Power Governance");
    ImGui::Separator();

    ImGui::SetNextItemWidth(250);
    if (ImGui::SliderInt("Target FPS", &cfg.target_fps, 15, 60)) {
        nlohmann::json req{{"cmd", "set_fps"}, {"fps", cfg.target_fps}};
        g_ipcClient.SendRequest(req.dump());
    }

    ImGui::SetNextItemWidth(250);
    ImGui::SliderInt("Battery Saver FPS", &cfg.battery_fps, 10, 30);
    ImGui::Checkbox("Auto-Pause on Fullscreen Apps / Games", &cfg.pause_on_fullscreen);
    ImGui::Checkbox("Pause on Battery Power", &cfg.pause_on_battery);
    ImGui::Checkbox("Capture Lock Screen Background", &cfg.update_lockscreen);
    ImGui::Checkbox("Launch on Windows Startup", &cfg.run_on_startup);

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_VOLUME_HIGH "  Audio Output");
    ImGui::Separator();

    static float volume = 0.5f;
    static bool volume_init = false;
    if (!volume_init && !cfg.wallpapers.empty()) {
        volume = cfg.wallpapers[0].volume;
        volume_init = true;
    }

    ImGui::SetNextItemWidth(250);
    if (ImGui::SliderFloat("Master Volume", &volume, 0.0f, 1.0f, "%.2f")) {
        if (!cfg.wallpapers.empty()) {
            cfg.wallpapers[0].volume = volume;
        }
        nlohmann::json req{{"cmd", "set_volume"}, {"volume", volume}};
        g_ipcClient.SendRequest(req.dump());
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save Configuration", ImVec2(190, 34))) {
        g_settingsConfig.Save();
        g_ipcClient.SendRequest("{\"cmd\":\"reload_config\"}");
    }

    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_EYE_SLASH "  Hide Window to Tray", ImVec2(190, 34))) {
        SettingsUI::Close();
    }
}

static void RenderPerformancePanel() {
    ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.00f, 1.00f), ICON_FA_MICROCHIP "  Real-Time Engine Monitor");
    ImGui::Separator();

    if (!g_daemonConnected) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Engine Status: Background Engine Initializing...");
        return;
    }

    ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.00f), ICON_FA_CIRCLE_CHECK "  Engine Status: Active (Running)");
    ImGui::Text("State: %s", g_daemonPaused ? "Paused (Game / Lock Screen)" : (g_daemonPlaying ? "Playing" : "Idle"));
    ImGui::Text("Render Frame Rate: %d FPS (Video Source: %.1f FPS)", g_daemonFps, g_daemonVideoFps);
    ImGui::Text("Video Resolution: %dx%d (%s)", g_daemonWidth, g_daemonHeight, g_daemonCodec.c_str());
    ImGui::Text("Video Duration: %.1f seconds", g_daemonDuration);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Process Working Set (RAM Usage): %zu MB", g_daemonRamMB);

    ImGui::PlotLines("RAM History (MB)", g_ramHistory.data(), (int)g_ramHistory.size(), 0, nullptr, 0.0f, 60.0f, ImVec2(0, 85));
    ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.70f, 1.00f), "*Target memory budget: < 45 MB");
}

bool SettingsUI::Open(HINSTANCE hInstance) {
    if (g_isOpen && g_hWnd) {
        ShowWindow(g_hWnd, SW_RESTORE);
        SetForegroundWindow(g_hWnd);
        return true;
    }

    g_hInstance = hInstance;

    WNDCLASSEXW wc = {
        sizeof(wc),
        CS_CLASSDC,
        SettingsWndProc,
        0L, 0L,
        hInstance,
        LoadIconW(hInstance, MAKEINTRESOURCEW(101)),
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
        150, 150, 780, 620,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (!g_hWnd || !CreateDeviceD3D(g_hWnd)) {
        CleanupDeviceD3D();
        if (g_hWnd) DestroyWindow(g_hWnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        g_hWnd = nullptr;
        return false;
    }

    // Apply Windows 10/11 Dark Title Bar & matching caption color
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(g_hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    COLORREF captionColor = RGB(20, 20, 26);
    DwmSetWindowAttribute(g_hWnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));

    // Enable Drag and Drop
    DragAcceptFiles(g_hWnd, TRUE);

    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    // Setup ImGui Context & Custom Dark Theme
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    SetupImGuiStyle();

    // 1. Load Segoe UI font if available on Windows
    ImFont* mainFont = nullptr;
    if (fs::exists("C:\\Windows\\Fonts\\segoeui.ttf")) {
        mainFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f);
    }
    if (!mainFont) {
        io.Fonts->AddFontDefault();
    }

    // 2. Merge FontAwesome 6 Icon Font
    static const ImWchar icons_ranges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    ImFontConfig icons_config;
    icons_config.MergeMode = true;
    icons_config.PixelSnapH = true;
    icons_config.GlyphMinAdvanceX = 16.0f;

    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    fs::path exeDir = fs::path(exePath).parent_path();
    fs::path fontPath1 = exeDir / "assets" / "fa-solid-900.ttf";
    fs::path fontPath2 = exeDir / "fa-solid-900.ttf";

    if (fs::exists(fontPath1)) {
        io.Fonts->AddFontFromFileTTF(fontPath1.string().c_str(), 15.0f, &icons_config, icons_ranges);
    } else if (fs::exists(fontPath2)) {
        io.Fonts->AddFontFromFileTTF(fontPath2.string().c_str(), 15.0f, &icons_config, icons_ranges);
    }

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    g_settingsConfig.Load();
    FetchDaemonStatus();

    g_isOpen = true;
    return true;
}

void SettingsUI::RenderFrame() {
    if (!g_isOpen || !g_pd3dDeviceContext || !g_mainRenderTargetView) return;

    if (++g_fetchCounter >= 30) {
        g_fetchCounter = 0;
        FetchDaemonStatus();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("LiteWallpaper Settings", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    if (ImGui::BeginTabBar("MainTabBar")) {
        if (ImGui::BeginTabItem(ICON_FA_IMAGES "  Gallery")) {
            RenderGalleryPanel();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_FA_GEAR "  Settings")) {
            RenderSettingsPanel();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_FA_GAUGE_HIGH "  Performance")) {
            RenderPerformancePanel();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();

    ImGui::Render();
    const float clear_color[4] = { 0.08f, 0.08f, 0.10f, 1.0f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    g_pSwapChain->Present(1, 0);
}

bool SettingsUI::IsOpen() {
    return g_isOpen;
}

HWND SettingsUI::GetHwnd() {
    return g_hWnd;
}

void SettingsUI::Close() {
    if (!g_isOpen) return;

    g_isOpen = false;

    // Shutdown ImGui
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    // Release DirectX 11 resources
    CleanupDeviceD3D();

    if (g_hWnd) {
        DestroyWindow(g_hWnd);
        g_hWnd = nullptr;
    }

    if (g_hInstance) {
        UnregisterClassW(L"LiteWallpaper_SettingsClass", g_hInstance);
    }

    // Force Windows OS and mimalloc to immediately trim working set memory!
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    mi_collect(true);
}

} // namespace litewp
