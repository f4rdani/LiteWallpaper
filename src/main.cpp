// LiteWallpaper — Ultra-lightweight animated video wallpaper engine
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <psapi.h>
#include <shellapi.h>
#include <string>
#include <thread>
#include <mutex>
#include <mimalloc.h>
#include <nlohmann/json.hpp>

#include "core/config.h"
#include "core/playback_clock.h"
#include "core/ipc_server.h"
#include "decoder/ffmpeg_hw_decoder.h"
#include "decoder/audio_player.h"
#include "platform/win32/desktop_injector.h"
#include "platform/win32/d3d11_presenter.h"
#include "platform/win32/power_governor.h"
#include "platform/win32/lockscreen_manager.h"
#include "platform/win32/tray_icon.h"
#include "ui/settings_app.h"

#define WM_APP_OPEN_SETTINGS (WM_APP + 10)

using namespace litewp;

// Global engine state
static Config            g_config;
static PlaybackClock     g_clock;
static DesktopInjector   g_injector;
static D3D11Presenter    g_presenter;
static FFmpegHWDecoder   g_decoder;
static AudioPlayer       g_audio;
static PowerGovernor     g_governor;
static LockScreenManager g_lockscreen;
static TrayIcon          g_tray;
static IpcServer         g_ipc;

static std::mutex        g_decoder_mutex; // Guards g_decoder and g_current_frame against race conditions

static bool g_running = true;
static bool g_paused = false;
static HWND g_main_hwnd = nullptr;
static VideoFrame g_current_frame;

// Forward declarations
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void OnTrayAction(TrayAction action);
std::string OnIpcRequest(const std::string& json);

static size_t GetProcessMemoryUsageMB() {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        return pmc.WorkingSetSize / (1024 * 1024);
    }
    return 0;
}

static void TrimWorkingSetMemory() {
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    mi_collect(true);
}

static void OpenWallpaperDialog() {
    std::thread([]() {
        wchar_t filename[MAX_PATH] = L"";
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = nullptr;
        ofn.lpstrFilter = L"Video Files (*.mp4;*.webm;*.mkv;*.avi)\0*.mp4;*.webm;*.mkv;*.avi\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = filename;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

        if (GetOpenFileNameW(&ofn)) {
            // Convert to UTF-8
            int size_needed = WideCharToMultiByte(CP_UTF8, 0, filename, -1, NULL, 0, NULL, NULL);
            std::string utf8_path(size_needed - 1, 0);
            WideCharToMultiByte(CP_UTF8, 0, filename, -1, &utf8_path[0], size_needed, NULL, NULL);

            auto& cfg = g_config.Get();
            if (cfg.wallpapers.empty()) {
                MonitorWallpaper mw;
                mw.video_path = utf8_path;
                cfg.wallpapers.push_back(mw);
            } else {
                cfg.wallpapers[0].video_path = utf8_path;
            }
            cfg.AddToGallery(utf8_path);
            g_config.Save();

            // Thread-safe decoder reload
            {
                std::lock_guard<std::mutex> lock(g_decoder_mutex);
                g_current_frame = VideoFrame{};
                g_decoder.Close();
                g_decoder.Open(utf8_path.c_str(), g_presenter.GetDevice());
            }

            TrimWorkingSetMemory();
        }
    }).detach();
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // 0. Single Instance Mutex Check
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"LiteWallpaper_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Another instance is already running! Tell it to open the Settings UI and exit this process
        IpcClient client;
        client.SendRequest("{\"cmd\":\"open_settings\"}");
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    // Enable 1ms multimedia timer resolution for high precision frame pacing without CPU spin
    timeBeginPeriod(1);

    // 1. Load config
    g_config.Load();
    auto& cfg = g_config.Get();

    // 2. Register window class for the background wallpaper canvas
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"LiteWallpaper_Daemon";
    RegisterClassExW(&wc);

    // Virtual screen dimensions (covering all monitors)
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    g_main_hwnd = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"LiteWallpaper_Daemon",
        L"LiteWallpaper",
        WS_POPUP,
        vx, vy, vw, vh,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_main_hwnd) {
        timeEndPeriod(1);
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 1;
    }

    // 3. Initialize Direct3D 11 Presenter
    if (!g_presenter.Init(g_main_hwnd, vw, vh)) {
        timeEndPeriod(1);
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 1;
    }

    // 4. Inject window behind desktop icons (WorkerW)
    g_injector.Attach(g_main_hwnd);
    g_injector.RegisterExplorerRestart(g_main_hwnd);

    // 5. Open video if available
    if (!cfg.wallpapers.empty() && !cfg.wallpapers[0].video_path.empty()) {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        g_decoder.Open(cfg.wallpapers[0].video_path.c_str(), g_presenter.GetDevice());
    }

    // 6. Setup audio parameters (Lazy-init: only allocate WASAPI if unmuted)
    if (!cfg.wallpapers.empty()) {
        g_audio.SetVolume(cfg.wallpapers[0].volume);
        g_audio.SetMuted(!cfg.wallpapers[0].audio_enabled);
        if (cfg.wallpapers[0].audio_enabled && cfg.wallpapers[0].volume > 0.0f) {
            g_audio.Init();
        }
    }

    // 7. Setup Power Governor
    g_governor.Init(g_main_hwnd);

    // 8. Setup System Tray Icon
    g_tray.Create(g_main_hwnd, OnTrayAction);

    // 9. Setup Playback Clock
    g_clock.SetTargetFPS(cfg.target_fps);

    // 10. Start IPC Server
    g_ipc.Start(OnIpcRequest);

    // Initial launch: Open Settings UI on-demand
    SettingsUI::Open(hInstance);

    // Initial memory working set cleanup (drops startup overhead down to ~15-25 MB)
    TrimWorkingSetMemory();

    // 11. Main loop
    MSG msg;
    float audio_buffer[4096];

    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!g_running) break;

        // Render Settings UI if open
        if (SettingsUI::IsOpen()) {
            SettingsUI::RenderFrame();
        }

        PowerState power = g_governor.GetCurrentState();

        // Correct evaluation of pause conditions:
        // - Workstation locked
        // - Fullscreen app/game detected (if pause_on_fullscreen is on)
        // - Battery power active (if pause_on_battery is on)
        // - Manually paused by user
        bool should_pause = (power == PowerState::Sleeping) ||
                            (power == PowerState::Paused && cfg.pause_on_fullscreen) ||
                            (power == PowerState::Reduced && cfg.pause_on_battery) ||
                            g_paused;

        if (should_pause) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
            continue;
        }

        // Adjust FPS based on power state:
        // When on battery power (and pause_on_battery is false), reduce FPS to battery_fps.
        // When on AC charger / desktop, always run at target_fps.
        if (power == PowerState::Reduced) {
            g_clock.SetTargetFPS(cfg.battery_fps);
        } else {
            g_clock.SetTargetFPS(cfg.target_fps);
        }

        if (g_clock.ShouldRenderFrame()) {
            std::lock_guard<std::mutex> lock(g_decoder_mutex);
            if (g_decoder.DecodeNextFrame(g_current_frame)) {
                if (g_current_frame.texture) {
                    g_presenter.RenderFrame(g_current_frame.texture, g_current_frame.texture_index, cfg.scaling_mode);
                    g_presenter.Present(0); // Zero-sync presentation driven by PlaybackClock pacing
                }

                // Decode and feed all available audio frames if enabled
                if (!g_audio.IsMuted() && g_decoder.HasAudio()) {
                    int samples = 0;
                    while ((samples = g_decoder.DecodeAudioSamples(audio_buffer, 4096)) > 0) {
                        g_audio.PushSamples(audio_buffer, samples);
                    }
                }
            }
        } else {
            DWORD sleepMs = g_clock.GetSleepDurationMs();
            if (sleepMs > 0) {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, sleepMs, QS_ALLINPUT);
            }
        }
    }

    // Cleanup
    SettingsUI::Close();
    g_ipc.Stop();
    g_tray.Destroy();
    g_governor.Shutdown();

    {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        g_current_frame = VideoFrame{};
        g_decoder.Close();
    }

    g_audio.Stop();
    g_presenter.Cleanup();
    g_injector.Detach();

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    timeEndPeriod(1);
    return 0;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_APP_OPEN_SETTINGS:
            SettingsUI::Open(GetModuleHandleW(nullptr));
            return 0;

        case WM_WTSSESSION_CHANGE:
            g_governor.HandleSessionChange(wParam);
            if (wParam == WTS_SESSION_LOCK && g_config.Get().update_lockscreen) {
                std::lock_guard<std::mutex> lock(g_decoder_mutex);
                if (g_current_frame.texture) {
                    g_lockscreen.CaptureAndSetLockScreen(
                        g_presenter.GetDevice(),
                        g_presenter.GetContext(),
                        g_current_frame.texture,
                        g_current_frame.texture_index
                    );
                }
            }
            return 0;

        case WM_POWERBROADCAST:
            g_governor.HandlePowerChange(wParam);
            return TRUE;

        case WM_DISPLAYCHANGE: {
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            g_presenter.Resize(vw, vh);
            g_injector.Reattach(hwnd);
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    if (msg == WM_APP + 1) {
        g_tray.HandleMessage(wParam, lParam);
        return 0;
    }

    if (msg == g_injector.GetTaskbarRestartMsg()) {
        g_injector.Reattach(hwnd);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void OnTrayAction(TrayAction action) {
    switch (action) {
        case TrayAction::PauseResume:
            g_paused = !g_paused;
            break;
        case TrayAction::MuteUnmute:
            g_audio.SetMuted(!g_audio.IsMuted());
            if (!g_audio.IsMuted() && !g_audio.IsRunning()) {
                g_audio.Init();
            }
            break;
        case TrayAction::OpenSettings:
            SettingsUI::Open(GetModuleHandleW(nullptr));
            break;
        case TrayAction::ChangeWallpaper:
            OpenWallpaperDialog();
            break;
        case TrayAction::Exit:
            g_running = false;
            PostQuitMessage(0);
            break;
    }
}

std::string OnIpcRequest(const std::string& request_json) {
    auto req = nlohmann::json::parse(request_json, nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        return "{\"ok\":false,\"error\":\"invalid JSON request\"}";
    }

    std::string cmd = req.value("cmd", "");

    if (cmd == "open_settings") {
        if (g_main_hwnd) {
            PostMessageW(g_main_hwnd, WM_APP_OPEN_SETTINGS, 0, 0);
        }
        return "{\"ok\":true}";
    } else if (cmd == "set_wallpaper") {
        std::string path = req.value("path", "");
        if (!path.empty()) {
            auto& cfg = g_config.Get();
            if (cfg.wallpapers.empty()) {
                MonitorWallpaper mw;
                mw.video_path = path;
                cfg.wallpapers.push_back(mw);
            } else {
                cfg.wallpapers[0].video_path = path;
            }
            cfg.AddToGallery(path);
            g_config.Save();

            std::lock_guard<std::mutex> lock(g_decoder_mutex);
            g_current_frame = VideoFrame{};
            g_decoder.Close();
            bool ok = g_decoder.Open(path.c_str(), g_presenter.GetDevice());
            g_paused = false;
            g_clock.Reset();
            TrimWorkingSetMemory();
            return nlohmann::json{{"ok", ok}}.dump();
        }
    } else if (cmd == "set_lockscreen") {
        std::string path = req.value("path", "");
        if (!path.empty()) {
            auto& cfg = g_config.Get();
            cfg.AddToGallery(path);
            g_config.Save();

            std::lock_guard<std::mutex> lock(g_decoder_mutex);
            FFmpegHWDecoder tempDecoder;
            if (tempDecoder.Open(path.c_str(), g_presenter.GetDevice())) {
                VideoFrame f;
                if (tempDecoder.DecodeNextFrame(f) && f.texture) {
                    g_lockscreen.CaptureAndSetLockScreen(g_presenter.GetDevice(), g_presenter.GetContext(), f.texture, f.texture_index);
                }
            }
            return "{\"ok\":true}";
        }
    } else if (cmd == "set_both") {
        std::string path = req.value("path", "");
        if (!path.empty()) {
            auto& cfg = g_config.Get();
            if (cfg.wallpapers.empty()) {
                MonitorWallpaper mw;
                mw.video_path = path;
                cfg.wallpapers.push_back(mw);
            } else {
                cfg.wallpapers[0].video_path = path;
            }
            cfg.AddToGallery(path);
            g_config.Save();

            std::lock_guard<std::mutex> lock(g_decoder_mutex);
            g_current_frame = VideoFrame{};
            g_decoder.Close();
            bool ok = g_decoder.Open(path.c_str(), g_presenter.GetDevice());
            g_paused = false;
            g_clock.Reset();

            if (ok) {
                if (g_decoder.DecodeNextFrame(g_current_frame) && g_current_frame.texture) {
                    g_lockscreen.CaptureAndSetLockScreen(g_presenter.GetDevice(), g_presenter.GetContext(), g_current_frame.texture, g_current_frame.texture_index);
                    g_presenter.RenderFrame(g_current_frame.texture, g_current_frame.texture_index, cfg.scaling_mode);
                    g_presenter.Present(0);
                }
            }
            TrimWorkingSetMemory();
            return nlohmann::json{{"ok", ok}}.dump();
        }
    } else if (cmd == "set_scaling") {
        int mode = req.value("mode", 0);
        g_config.Get().scaling_mode = mode;
        g_config.Save();
        return "{\"ok\":true}";
    } else if (cmd == "get_status") {
        size_t ram = GetProcessMemoryUsageMB();
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        auto info = g_decoder.GetInfo();
        return nlohmann::json{
            {"ok", true},
            {"playing", !g_paused},
            {"paused", g_paused},
            {"fps", g_clock.GetTargetFPS()},
            {"video_fps", info.fps},
            {"width", info.width},
            {"height", info.height},
            {"duration", info.duration_seconds},
            {"codec", info.codec_name},
            {"ram_mb", ram}
        }.dump();
    } else if (cmd == "pause") {
        g_paused = true;
        return "{\"ok\":true}";
    } else if (cmd == "resume") {
        g_paused = false;
        g_clock.Reset();
        return "{\"ok\":true}";
    } else if (cmd == "set_volume") {
        float vol = req.value("volume", 0.0f);
        g_audio.SetVolume(vol);
        if (vol > 0.0f && !g_audio.IsRunning()) {
            g_audio.Init();
        }
        return "{\"ok\":true}";
    } else if (cmd == "set_fps") {
        int fps = req.value("fps", 30);
        g_clock.SetTargetFPS(fps);
        return "{\"ok\":true}";
    } else if (cmd == "reload_config") {
        g_config.Load();
        auto& cfg = g_config.Get();
        g_clock.SetTargetFPS(cfg.target_fps);
        return "{\"ok\":true}";
    }

    return "{\"ok\":true}";
}
