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
#include <cmath>
#include <mimalloc.h>
#include <nlohmann/json.hpp>

#include "core/config.h"
#include "core/playback_clock.h"
#include "core/ipc_server.h"
#include "core/logger.h"
#include "decoder/ffmpeg_hw_decoder.h"
#include "decoder/audio_player.h"
#include "platform/win32/desktop_injector.h"
#include "platform/win32/d3d11_presenter.h"
#include "platform/win32/power_governor.h"
#include "platform/win32/lockscreen_manager.h"
#include "platform/win32/tray_icon.h"
#include "ui/settings_app.h"

#define WM_APP_OPEN_SETTINGS (WM_APP + 10)
#define WM_APP_ATTACH        (WM_APP + 11)
#define WM_APP_DEATTACH      (WM_APP + 12)

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
static bool g_inject_ok = false;
static bool g_decoder_hw = false;
static std::string g_last_error;
static uint64_t g_frames_rendered = 0;
static int64_t g_flash_until_us = 0; // Diagnostic green-flash window (microseconds)

// Playback pacing state.
// g_video_fps is the native frame rate of the loaded video; the render clock is
// always paced at this rate so playback speed stays 1x regardless of target_fps.
// g_frame_skip is the display skip factor: when target_fps < video_fps we only
// present every N-th decoded frame (power saving) WITHOUT slowing the video.
static double g_video_fps = 0.0;
static int    g_frame_skip = 1;
static uint64_t g_frames_decoded = 0;

// True while the wallpaper is fully suspended because a fullscreen app/game is
// covering the desktop (decoder + audio released to save RAM/GPU resources).
static bool g_fullscreen_paused = false;

// Compute the display skip factor for the current effective display FPS cap.
// Returns >= 1. 1 means "present every decoded frame".
static int ComputeFrameSkip(double video_fps, int display_fps) {
    if (video_fps <= 0.0) return 1;
    if (display_fps <= 0) return 1;
    if (display_fps >= video_fps) return 1;
    double skip = std::ceil(video_fps / display_fps);
    if (skip < 1.0) skip = 1.0;
    return static_cast<int>(skip);
}

static void SetVideoPacing(double video_fps) {
    g_video_fps = video_fps;
    if (video_fps > 0.0) {
        int native = static_cast<int>(video_fps + 0.5);
        if (native < 1) native = 1;
        g_clock.SetTargetFPS(native);
    }
    g_frames_decoded = 0;
}

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
    wchar_t filename[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = L"Video Files (*.mp4;*.webm;*.mkv;*.avi;*.mov)\0*.mp4;*.webm;*.mkv;*.avi;*.mov\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (GetOpenFileNameW(&ofn)) {
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
            if (g_decoder.Open(utf8_path.c_str(), g_presenter.GetDevice())) {
                SetVideoPacing(g_decoder.GetInfo().fps);
            }
            g_paused = false;
            g_clock.Reset();
        }

        TrimWorkingSetMemory();
    }
}

static bool OpenWallpaperVideo(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_decoder_mutex);
    g_current_frame = VideoFrame{};
    g_decoder.Close();
    bool ok = g_decoder.Open(path.c_str(), g_presenter.GetDevice());
    g_decoder_hw = g_decoder.IsHWAccelerated();
    g_paused = false;
    g_clock.Reset();
    if (!ok) {
        g_last_error = "Failed to open video: " + path;
        Logger::Info(g_last_error);
        return false;
    }
    auto info = g_decoder.GetInfo();
    g_last_error.clear();
    SetVideoPacing(info.fps);
    Logger::Info("Opened video OK: ", path,
                 " codec=", info.codec_name,
                 " ", info.width, "x", info.height,
                 " fps=", info.fps,
                 " hw=", g_decoder_hw ? "yes" : "no");
    return true;
}

// Release ALL wallpaper resources (decoder GPU textures, audio, render window)
// when a fullscreen app covers the desktop, freeing RAM/VRAM and CPU/GPU usage.
// Call on the main thread.
static void SuspendWallpaperForFullscreen() {
    if (g_fullscreen_paused) return;
    g_fullscreen_paused = true;

    {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        g_current_frame = VideoFrame{};
        g_decoder.Close();
        g_decoder_hw = false;
    }
    g_audio.Stop();
    g_frames_decoded = 0;
    g_frames_rendered = 0;
    SetVideoPacing(0.0);

    // Remove the render window from the desktop so DWM stops compositing it
    if (g_main_hwnd) {
        PostMessageW(g_main_hwnd, WM_APP_DEATTACH, 0, 0);
    }
    TrimWorkingSetMemory();
    Logger::Info("Fullscreen detected: wallpaper resources released (RAM freed)");
}

// Restore the wallpaper after the fullscreen app closes. Re-opens the current
// video, restarts audio and re-attaches the render window. Call on main thread.
static void ResumeWallpaperFromFullscreen() {
    if (!g_fullscreen_paused) return;
    g_fullscreen_paused = false;

    auto& cfg = g_config.Get();
    if (!cfg.wallpapers.empty() && !cfg.wallpapers[0].video_path.empty()) {
        OpenWallpaperVideo(cfg.wallpapers[0].video_path);
        g_audio.SetVolume(cfg.wallpapers[0].volume);
        g_audio.SetMuted(!cfg.wallpapers[0].audio_enabled);
        if (cfg.wallpapers[0].audio_enabled && cfg.wallpapers[0].volume > 0.0f) {
            g_audio.Init();
        }
        if (g_main_hwnd) {
            PostMessageW(g_main_hwnd, WM_APP_ATTACH, 0, 0);
        }
        g_clock.Reset();
    }
    Logger::Info("Fullscreen closed: wallpaper resources restored");
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPWSTR /*lpCmdLine*/, int /*nCmdShow*/) {
    // 0. Single-Instance Check
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"LiteWallpaper_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Another instance is already running! Notify it via IPC to open Settings UI
        IpcClient client;
        client.SendRequest("{\"cmd\":\"open_settings\"}");
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    // High precision multimedia timer for frame pacing
    timeBeginPeriod(1);

    // 1. Load Configuration
    g_config.Load();
    auto& cfg = g_config.Get();

    // 2. Register Background Render Window Class
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

    // 3. Inject window behind desktop icons (WorkerW) BEFORE DXGI initialization
    g_inject_ok = g_injector.Attach(g_main_hwnd);
    g_injector.RegisterExplorerRestart(g_main_hwnd);
    Logger::Info("Attach: ", g_inject_ok ? "OK" : "FAILED",
                 " workerw=0x", reinterpret_cast<size_t>(g_injector.GetWorkerW()),
                 " visible=", IsWindowVisible(g_main_hwnd) ? "yes" : "no");
    if (!g_inject_ok) {
        g_last_error = "Desktop injection failed (WorkerW not found). Desktop icons hidden?";
        Logger::Info(g_last_error);
    }

    // 4. Initialize Direct3D 11 Presenter on the attached child window
    if (!g_presenter.Init(g_main_hwnd, vw, vh)) {
        g_last_error = "D3D11 presenter init failed";
        Logger::Info(g_last_error);
        timeEndPeriod(1);
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 1;
    }
    Logger::Info("Presenter init OK, swapchain size=", vw, "x", vh);

    // Smoke test: flash a solid green frame for ~1.5 s.
    // If the desktop turns green the injection + swapchain are working and any
    // "no wallpaper" issue is in the decode/render path, not the injection.
    g_flash_until_us = g_clock.GetCurrentTimeMicros() + 1500000;
    Logger::Info("Smoke test: flashing solid green for 1.5s");

    // 5. Open video if available
    if (!cfg.wallpapers.empty() && !cfg.wallpapers[0].video_path.empty()) {
        OpenWallpaperVideo(cfg.wallpapers[0].video_path);
    }

    // 6. Setup audio parameters
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
    // NOTE: pacing is set to the video's NATIVE fps when a wallpaper opens
    // (SetVideoPacing). target_fps is only a display cap (frame skipping), so
    // we intentionally do NOT call SetTargetFPS(cfg.target_fps) here.

    // 10. Start IPC Server
    g_ipc.Start(OnIpcRequest);

    // 11. Open Settings UI on first startup
    SettingsUI::Open(hInstance);

    // Initial RAM cleanup
    TrimWorkingSetMemory();

    // 12. Main Event & Render Loop
    MSG msg = {};
    float audio_buffer[4096 * 2];

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

        bool fullscreen_active = (power == PowerState::Paused && cfg.pause_on_fullscreen);

        // Hard-pause on fullscreen: release decoder + audio + render window to
        // free RAM/VRAM and CPU/GPU. Restore everything when the app closes.
        if (fullscreen_active) {
            if (!g_fullscreen_paused) {
                SuspendWallpaperForFullscreen();
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
            continue;
        } else if (g_fullscreen_paused) {
            ResumeWallpaperFromFullscreen();
        }

        bool should_pause = (power == PowerState::Sleeping) ||
                            (power == PowerState::Reduced && cfg.pause_on_battery) ||
                            g_paused;

        if (should_pause) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
            continue;
        }

        // Playback always advances at the video's native fps (SetVideoPacing),
        // so speed is 1x regardless of the display FPS cap. When on battery /
        // reduced power we lower the DISPLAY rate by skipping frames.
        int display_fps_cap = (power == PowerState::Reduced) ? cfg.battery_fps : cfg.target_fps;
        g_frame_skip = ComputeFrameSkip(g_video_fps, display_fps_cap);

        if (g_clock.ShouldRenderFrame()) {
            // Diagnostic flash window active: paint solid green instead of video
            if (g_clock.GetCurrentTimeMicros() < g_flash_until_us) {
                g_presenter.ClearAndPresent(0.05f, 0.80f, 0.10f);
                g_frames_rendered++;
                DWORD sleepMs = g_clock.GetSleepDurationMs();
                if (sleepMs > 0) {
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, sleepMs, QS_ALLINPUT);
                }
                continue;
            }

            std::lock_guard<std::mutex> lock(g_decoder_mutex);
            if (g_decoder.DecodeNextFrame(g_current_frame)) {
                // Always decode every frame (keeps playback position at native
                // speed + audio in sync); only PRESENT every N-th frame when the
                // display FPS cap is below the video's native rate.
                g_frames_decoded++;
                bool should_present = (g_frame_skip <= 1) ||
                                      ((g_frames_decoded % g_frame_skip) == 0);

                if (g_current_frame.texture && should_present) {
                    g_presenter.RenderFrame(g_current_frame.texture, g_current_frame.texture_index, cfg.scaling_mode);
                    if (FAILED(g_presenter.Present(0))) {
                        if (g_last_error.empty()) {
                            g_last_error = "Present() failed (DXGI error)";
                            Logger::Info(g_last_error);
                        }
                    }
                    g_frames_rendered++;
                } else if (g_current_frame.texture && g_last_error.empty()) {
                    // Frame decoded but intentionally skipped for power saving.
                } else if (!g_current_frame.texture && g_last_error.empty()) {
                    // Decoder produced a non-GPU frame (SW fallback not engaged yet)
                    g_last_error = "Decoded frame has no GPU texture (HW decode inactive)";
                    Logger::Info(g_last_error);
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
    g_audio.Stop();
    {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        g_decoder.Close();
    }
    g_presenter.Cleanup();
    g_injector.Detach();

    timeEndPeriod(1);
    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    return 0;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_APP_OPEN_SETTINGS) {
        SettingsUI::Open(GetModuleHandleW(nullptr));
        return 0;
    }

    if (msg == g_injector.GetTaskbarRestartMsg()) {
        g_tray.Create(g_main_hwnd, OnTrayAction);
        g_injector.Reattach(g_main_hwnd);
        return 0;
    }

    // Attach / detach render window on the main thread (window owner), because
    // the IPC server runs on a background thread.
    if (msg == WM_APP_ATTACH) {
        if (g_main_hwnd && !g_injector.IsAttached()) {
            g_inject_ok = g_injector.Attach(g_main_hwnd);
        }
        return 0;
    }

    if (msg == WM_APP_DEATTACH) {
        if (g_main_hwnd) {
            g_injector.Detach();
            ShowWindow(g_main_hwnd, SW_HIDE);
            g_inject_ok = false;
        }
        return 0;
    }

    switch (msg) {
        case WM_WTSSESSION_CHANGE:
            g_governor.HandleSessionChange(wParam);
            return 0;

        case WM_POWERBROADCAST:
            g_governor.HandlePowerChange(wParam);
            return 0;

        case WM_DISPLAYCHANGE: {
            int cx = LOWORD(lParam);
            int cy = HIWORD(lParam);
            g_presenter.Resize(cx, cy);
            return 0;
        }

        case WM_USER + 1: // Tray icon callback
            g_tray.HandleMessage(wParam, lParam);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void OnTrayAction(TrayAction action) {
    switch (action) {
        case TrayAction::PauseResume:
            g_paused = !g_paused;
            if (!g_paused) g_clock.Reset();
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

            // If a fullscreen app is active the resources are suspended; just
            // persist the new path and let ResumeWallpaperFromFullscreen open it.
            bool ok = g_fullscreen_paused;
            if (!g_fullscreen_paused) {
                ok = OpenWallpaperVideo(path);
                TrimWorkingSetMemory();
                if (ok && g_main_hwnd) {
                    PostMessageW(g_main_hwnd, WM_APP_ATTACH, 0, 0);
                }
            }
            return nlohmann::json{{"ok", ok}, {"hw", g_decoder_hw}}.dump();
        }
    } else if (cmd == "test_render") {
        // Diagnostic: flash the render window a solid color so the user can see
        // whether the injection/swapchain is live on the desktop.
        g_flash_until_us = g_clock.GetCurrentTimeMicros() + 1500000;
        return nlohmann::json{{"ok", true}}.dump();
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

            if (g_fullscreen_paused) {
                // Suspended for fullscreen: persist path, open on resume.
                return nlohmann::json{{"ok", true}, {"hw", false}}.dump();
            }

            bool ok = OpenWallpaperVideo(path);

            if (ok) {
                std::lock_guard<std::mutex> lock(g_decoder_mutex);
                if (g_decoder.DecodeNextFrame(g_current_frame) && g_current_frame.texture) {
                    g_lockscreen.CaptureAndSetLockScreen(g_presenter.GetDevice(), g_presenter.GetContext(), g_current_frame.texture, g_current_frame.texture_index);
                    g_presenter.RenderFrame(g_current_frame.texture, g_current_frame.texture_index, cfg.scaling_mode);
                    g_presenter.Present(0);
                }
            }
            TrimWorkingSetMemory();
            if (ok && g_main_hwnd) {
                PostMessageW(g_main_hwnd, WM_APP_ATTACH, 0, 0);
            }
            return nlohmann::json{{"ok", ok}, {"hw", g_decoder_hw}}.dump();
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
        auto& cfg = g_config.Get();
        std::string current_video = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : "";
        return nlohmann::json{
            {"ok", true},
            {"playing", !g_paused && !current_video.empty() && !g_fullscreen_paused},
            {"paused", g_paused},
            {"fullscreen_paused", g_fullscreen_paused},
            {"current_video", current_video},
            {"fps", g_clock.GetTargetFPS()},
            {"video_fps", info.fps},
            {"width", info.width},
            {"height", info.height},
            {"duration", info.duration_seconds},
            {"codec", info.codec_name},
            {"ram_mb", ram},
            {"injected", g_inject_ok},
            {"hw_decode", g_decoder_hw},
            {"frames_rendered", g_frames_rendered},
            {"frames_decoded", g_frames_decoded},
            {"frame_skip", g_frame_skip},
            {"last_error", g_last_error}
        }.dump();
    } else if (cmd == "pause") {
        g_paused = true;
        return "{\"ok\":true}";
    } else if (cmd == "resume") {
        g_paused = false;
        g_clock.Reset();
        if (g_main_hwnd) {
            PostMessageW(g_main_hwnd, WM_APP_ATTACH, 0, 0);
        }
        return "{\"ok\":true}";
    } else if (cmd == "stop") {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        g_decoder.Close();
        g_current_frame = VideoFrame{};
        g_paused = true;
        SetVideoPacing(0.0);
        auto& cfg = g_config.Get();
        if (!cfg.wallpapers.empty()) {
            cfg.wallpapers[0].video_path = "";
            g_config.Save();
        }
        // Detach render window so the original wallpaper is restored
        if (g_main_hwnd) {
            PostMessageW(g_main_hwnd, WM_APP_DEATTACH, 0, 0);
        }
        return "{\"ok\":true}";
    } else if (cmd == "set_volume") {
        float vol = req.value("volume", 0.0f);
        g_audio.SetVolume(vol);
        if (vol > 0.0f && !g_audio.IsRunning()) {
            g_audio.Init();
        }
        return "{\"ok\":true}";
    } else if (cmd == "set_fps") {
        // target_fps is a DISPLAY cap (frame skipping), never changes playback
        // speed. The pacing clock stays at the video's native fps.
        int fps = req.value("fps", 30);
        if (fps < 1) fps = 1;
        g_config.Get().target_fps = fps;
        g_config.Save();
        return "{\"ok\":true}";
    } else if (cmd == "reload_config") {
        g_config.Load();
        auto& cfg = g_config.Get();
        return "{\"ok\":true}";
    }

    return "{\"ok\":true}";
}
