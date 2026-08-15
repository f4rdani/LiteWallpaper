#include "settings_app.h"
#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <filesystem>
#include <vector>
#include <string>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include "core/config.h"
#include "core/ipc_server.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace litewp {

static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

static Config    g_settingsConfig;
static IpcClient g_ipcClient;

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

static void CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer.Get(), nullptr, &g_mainRenderTargetView);
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

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
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
    static char folderPath[MAX_PATH] = "C:\\";
    static std::vector<std::string> videoFiles;

    ImGui::Text("Browse Wallpaper Directory:");
    ImGui::InputText("##FolderPath", folderPath, MAX_PATH);
    ImGui::SameLine();
    if (ImGui::Button("Scan Folder")) {
        videoFiles.clear();
        std::error_code ec;
        if (fs::exists(folderPath, ec) && fs::is_directory(folderPath, ec)) {
            for (const auto& entry : fs::directory_iterator(folderPath, ec)) {
                if (entry.is_regular_file(ec)) {
                    auto ext = entry.path().extension().string();
                    for (auto& c : ext) c = (char)::tolower(c);
                    if (ext == ".mp4" || ext == ".webm" || ext == ".mkv" || ext == ".avi") {
                        videoFiles.push_back(entry.path().string());
                    }
                }
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Browse File...")) {
        wchar_t filename[MAX_PATH] = L"";
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFilter = L"Video Files (*.mp4;*.webm;*.mkv;*.avi)\0*.mp4;*.webm;*.mkv;*.avi\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = filename;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

        if (GetOpenFileNameW(&ofn)) {
            int size_needed = WideCharToMultiByte(CP_UTF8, 0, filename, -1, NULL, 0, NULL, NULL);
            std::string utf8_path(size_needed - 1, 0);
            WideCharToMultiByte(CP_UTF8, 0, filename, -1, &utf8_path[0], size_needed, NULL, NULL);
            
            nlohmann::json req{{"cmd", "set_wallpaper"}, {"path", utf8_path}};
            g_ipcClient.SendRequest(req.dump());
        }
    }

    ImGui::Separator();
    ImGui::Text("Found Video Files (%d):", (int)videoFiles.size());

    ImGui::BeginChild("VideoList", ImVec2(0, 300), true);
    for (const auto& file : videoFiles) {
        if (ImGui::Button(file.c_str(), ImVec2(-1, 30))) {
            nlohmann::json req{{"cmd", "set_wallpaper"}, {"path", file}};
            g_ipcClient.SendRequest(req.dump());
        }
    }
    ImGui::EndChild();
}

static void RenderSettingsPanel() {
    auto& cfg = g_settingsConfig.Get();

    ImGui::Text("Playback & Power Settings");
    ImGui::Separator();

    if (ImGui::SliderInt("Target FPS", &cfg.target_fps, 15, 60)) {
        nlohmann::json req{{"cmd", "set_fps"}, {"fps", cfg.target_fps}};
        g_ipcClient.SendRequest(req.dump());
    }

    ImGui::SliderInt("Battery Saver FPS", &cfg.battery_fps, 10, 30);
    ImGui::Checkbox("Pause Wallpaper on Fullscreen Apps / Games", &cfg.pause_on_fullscreen);
    ImGui::Checkbox("Pause Wallpaper on Battery Power", &cfg.pause_on_battery);
    ImGui::Checkbox("Update Lock Screen Snapshot on Lock (Win+L)", &cfg.update_lockscreen);
    ImGui::Checkbox("Run LiteWallpaper on Windows Startup", &cfg.run_on_startup);

    ImGui::Spacing();
    ImGui::Text("Audio Output");
    ImGui::Separator();

    static float currentVol = 0.0f;
    if (!cfg.wallpapers.empty()) {
        currentVol = cfg.wallpapers[0].volume;
    }

    if (ImGui::SliderFloat("Master Volume", &currentVol, 0.0f, 1.0f, "%.2f")) {
        if (!cfg.wallpapers.empty()) {
            cfg.wallpapers[0].volume = currentVol;
        }
        nlohmann::json req{{"cmd", "set_volume"}, {"volume", currentVol}};
        g_ipcClient.SendRequest(req.dump());
    }

    ImGui::Spacing();
    if (ImGui::Button("Save and Apply Configuration", ImVec2(240, 35))) {
        g_settingsConfig.Save();
        g_ipcClient.SendRequest("{\"cmd\":\"reload_config\"}");
    }
}

static void RenderPerformancePanel() {
    static int updateCounter = 0;
    if (++updateCounter % 30 == 0) {
        FetchDaemonStatus();
    }

    ImGui::Text("Daemon Status: %s", g_daemonConnected ? "Connected" : "Disconnected (Daemon Offline)");
    ImGui::Separator();

    if (g_daemonConnected) {
        ImGui::Text("Playback State: %s", g_daemonPaused ? "Paused" : "Playing");
        ImGui::Text("Engine Target FPS: %d", g_daemonFps);
        ImGui::Text("Video Resolution: %d x %d (Native FPS: %.2f)", g_daemonWidth, g_daemonHeight, g_daemonVideoFps);
        ImGui::Text("Video Codec: %s", g_daemonCodec.c_str());
        ImGui::Text("Video Duration: %.1f seconds", g_daemonDuration);
        ImGui::Text("Process Working Set RAM: %zu MB", g_daemonRamMB);

        ImGui::PlotLines("RAM Usage (MB)", g_ramHistory.data(), (int)g_ramHistory.size(), 0, nullptr, 0.0f, 100.0f, ImVec2(0, 80));

        ImGui::Spacing();
        if (ImGui::Button(g_daemonPaused ? "Resume Wallpaper" : "Pause Wallpaper", ImVec2(160, 30))) {
            if (g_daemonPaused) {
                g_ipcClient.SendRequest("{\"cmd\":\"resume\"}");
            } else {
                g_ipcClient.SendRequest("{\"cmd\":\"pause\"}");
            }
        }
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Make sure litewp_daemon.exe is running in background.");
    }
}

int SettingsApp::Run(void* hInstance, int nCmdShow) {
    g_settingsConfig.Load();

    // Register Win32 window class
    WNDCLASSEXW wc = {
        sizeof(wc),
        CS_CLASSDC,
        WndProc,
        0L, 0L,
        (HINSTANCE)hInstance,
        nullptr, nullptr, nullptr, nullptr,
        L"LiteWallpaper_Settings",
        nullptr
    };
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(
        wc.lpszClassName,
        L"LiteWallpaper Control Panel",
        WS_OVERLAPPEDWINDOW,
        100, 100, 720, 560,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Initial status fetch
    FetchDaemonStatus();

    // Main loop
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Settings Viewport Window
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("LiteWallpaper Settings", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        if (ImGui::BeginTabBar("MainTabBar")) {
            if (ImGui::BeginTabItem("Gallery")) {
                RenderGalleryPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings")) {
                RenderSettingsPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Performance")) {
                RenderPerformancePanel();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::End();

        // Render ImGui frame
        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.1f, 0.1f, 0.12f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

} // namespace litewp

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    return litewp::SettingsApp::Run(hInstance, nCmdShow);
}
