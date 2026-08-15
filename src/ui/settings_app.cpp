#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "settings_app.h"
#include <windows.h>
#include <commdlg.h>
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
        ofn.hwndOwner = g_hWnd;
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

    ImGui::BeginChild("VideoList", ImVec2(0, 260), true);
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
    ImGui::Checkbox("Auto-Pause on Fullscreen Apps / Games", &cfg.pause_on_fullscreen);
    ImGui::Checkbox("Pause on Battery Power", &cfg.pause_on_battery);
    ImGui::Checkbox("Capture Lock Screen Background", &cfg.update_lockscreen);
    ImGui::Checkbox("Launch on Windows Startup", &cfg.run_on_startup);

    ImGui::Spacing();
    ImGui::Text("Audio Output Settings");
    ImGui::Separator();

    static float volume = 0.5f;
    static bool volume_init = false;
    if (!volume_init && !cfg.wallpapers.empty()) {
        volume = cfg.wallpapers[0].volume;
        volume_init = true;
    }

    if (ImGui::SliderFloat("Master Volume", &volume, 0.0f, 1.0f, "%.2f")) {
        if (!cfg.wallpapers.empty()) {
            cfg.wallpapers[0].volume = volume;
        }
        nlohmann::json req{{"cmd", "set_volume"}, {"volume", volume}};
        g_ipcClient.SendRequest(req.dump());
    }

    ImGui::Spacing();
    if (ImGui::Button("Save Configuration", ImVec2(180, 32))) {
        g_settingsConfig.Save();
        g_ipcClient.SendRequest("{\"cmd\":\"reload_config\"}");
    }

    ImGui::SameLine();
    if (ImGui::Button("Hide Window to Tray", ImVec2(180, 32))) {
        SettingsUI::Close();
    }
}

static void RenderPerformancePanel() {
    ImGui::Text("Real-Time Engine Monitor");
    ImGui::Separator();

    if (!g_daemonConnected) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Engine Status: Background Engine Initializing...");
        return;
    }

    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Engine Status: Active (Running)");
    ImGui::Text("State: %s", g_daemonPaused ? "Paused (Game / Lock Screen)" : (g_daemonPlaying ? "Playing" : "Idle"));
    ImGui::Text("Render Frame Rate: %d FPS (Video Source: %.1f FPS)", g_daemonFps, g_daemonVideoFps);
    ImGui::Text("Video Resolution: %dx%d (%s)", g_daemonWidth, g_daemonHeight, g_daemonCodec.c_str());
    ImGui::Text("Video Duration: %.1f seconds", g_daemonDuration);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Process Working Set (RAM Usage): %zu MB", g_daemonRamMB);

    ImGui::PlotLines("RAM History (MB)", g_ramHistory.data(), (int)g_ramHistory.size(), 0, nullptr, 0.0f, 60.0f, ImVec2(0, 80));
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "*Target memory budget: < 45 MB");
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
        150, 150, 720, 560,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (!g_hWnd || !CreateDeviceD3D(g_hWnd)) {
        CleanupDeviceD3D();
        if (g_hWnd) DestroyWindow(g_hWnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        g_hWnd = nullptr;
        return false;
    }

    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    // Setup ImGui Context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    g_settingsConfig.Load();
    FetchDaemonStatus();

    g_isOpen = true;
    return true;
}

void SettingsUI::RenderFrame() {
    if (!g_isOpen || !g_hWnd) return;

    MSG msg;
    while (PeekMessageW(&msg, g_hWnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

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

    ImGui::Render();
    const float clear_color[4] = { 0.1f, 0.1f, 0.12f, 1.0f };
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
