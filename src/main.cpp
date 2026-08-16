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
#include "core/engine_state.h"
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

// Playback pacing state
static double g_video_fps = 0.0;
static int    g_frame_skip = 1;
static uint64_t g_frames_decoded = 0;

static bool g_fullscreen_paused = false;

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
    auto& cfg = g_config.Get();
    int fps = (cfg.target_fps > 0) ? cfg.target_fps : ((video_fps > 0.0) ? static_cast<int>(video_fps + 0.5) : 30);
    if (fps < 1) fps = 1;
    g_clock.SetTargetFPS(fps);
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

static double GetProcessCpuUsagePercent() {
    static ULARGE_INTEGER lastCPU = {0}, lastSysCPU = {0}, lastUserCPU = {0};
    static int numProcessors = -1;
    static bool first = true;
    static double last_percent = 0.0;

    if (first) {
        SYSTEM_INFO sysInfo;
        FILETIME ftime, fsys, fuser;
        GetSystemInfo(&sysInfo);
        numProcessors = (sysInfo.dwNumberOfProcessors > 0) ? sysInfo.dwNumberOfProcessors : 1;
        GetSystemTimeAsFileTime(&ftime);
        memcpy(&lastCPU, &ftime, sizeof(FILETIME));
        GetProcessTimes(GetCurrentProcess(), &ftime, &ftime, &fsys, &fuser);
        memcpy(&lastSysCPU, &fsys, sizeof(FILETIME));
        memcpy(&lastUserCPU, &fuser, sizeof(FILETIME));
        first = false;
        return 0.0;
    }

    FILETIME ftime, fsys, fuser;
    ULARGE_INTEGER now, sys, user;
    GetSystemTimeAsFileTime(&ftime);
    memcpy(&now, &ftime, sizeof(FILETIME));
    GetProcessTimes(GetCurrentProcess(), &ftime, &ftime, &fsys, &fuser);
    memcpy(&sys, &fsys, sizeof(FILETIME));
    memcpy(&user, &fuser, sizeof(FILETIME));

    if (now.QuadPart > lastCPU.QuadPart) {
        double percent = (double)((sys.QuadPart - lastSysCPU.QuadPart) + (user.QuadPart - lastUserCPU.QuadPart));
        percent /= (now.QuadPart - lastCPU.QuadPart);
        percent /= numProcessors;
        last_percent = percent * 100.0;
        if (last_percent < 0.0) last_percent = 0.0;
        if (last_percent > 100.0) last_percent = 100.0;
        lastCPU = now;
        lastUserCPU = user;
        lastSysCPU = sys;
    }
    return last_percent;
}

static size_t GetProcessVramUsageMB(int vid_w, int vid_h, int screen_w, int screen_h, bool hw_decode) {
    if (!hw_decode || vid_w <= 0 || vid_h <= 0) return 12;
    size_t frame_bytes = (size_t)vid_w * vid_h * 3 / 2;
    size_t pool_bytes = 4 * frame_bytes; // 4 surfaces in minimal hardware pool
    size_t swapchain_bytes = 2 * (size_t)screen_w * screen_h * 4; // 2 backbuffers
    return (pool_bytes + swapchain_bytes) / (1024 * 1024) + 6; // +6MB driver/shader overhead
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
            bool audio_on = !cfg.wallpapers.empty() && cfg.wallpapers[0].audio_enabled && (cfg.wallpapers[0].volume > 0.0f);
            g_decoder.SetAudioEnabled(audio_on);
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (g_decoder.Open(utf8_path.c_str(), g_presenter.GetDevice(), vw, vh)) {
                SetVideoPacing(g_decoder.GetInfo().fps);
            }
            g_paused = false;
            g_clock.Reset();
        }

        TrimWorkingSetMemory();
    }
}

static bool OpenWallpaperVideo(const std::string& path) {
    auto& cfg = g_config.Get();
    bool audio_on = !cfg.wallpapers.empty() && cfg.wallpapers[0].audio_enabled && (cfg.wallpapers[0].volume > 0.0f);

    std::lock_guard<std::mutex> lock(g_decoder_mutex);
    g_current_frame = VideoFrame{};
    g_decoder.Close();
    g_decoder.SetAudioEnabled(audio_on);
    g_decoder.SetForceSoftware(cfg.gpu_device_index == -1);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    bool ok = g_decoder.Open(path.c_str(), g_presenter.GetDevice(), vw, vh);
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

static void SuspendWallpaperForFullscreen() {
    if (g_fullscreen_paused) return;
    g_fullscreen_paused = true;
    g_audio.Stop();
    Logger::Info("Fullscreen app detected: wallpaper playback paused");
}

static void ResumeWallpaperFromFullscreen() {
    if (!g_fullscreen_paused) return;
    g_fullscreen_paused = false;

    auto& cfg = g_config.Get();
    if (!cfg.wallpapers.empty() && cfg.wallpapers[0].audio_enabled && cfg.wallpapers[0].volume > 0.0f) {
        g_audio.Init();
    }
    g_clock.Reset();
    Logger::Info("Fullscreen app closed: wallpaper playback resumed");
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPWSTR /*lpCmdLine*/, int /*nCmdShow*/) {
    // Single-Instance Check
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"LiteWallpaper_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        IpcClient client;
        client.SendRequest("{\"cmd\":\"open_settings\"}");
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

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
        // Show the window as a hidden popup; injection will be retried in the main loop
        Logger::Info("Desktop injection failed, will retry periodically");
    }

    // 4. Initialize Direct3D 11 Presenter
    if (!g_presenter.Init(g_main_hwnd, vw, vh, cfg.gpu_device_index)) {
        g_last_error = "D3D11 presenter init failed";
        Logger::Info(g_last_error);
        timeEndPeriod(1);
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 1;
    }
    g_shared_engine_state.active_gpu_index.store(cfg.gpu_device_index);
    Logger::Info("Presenter init OK (gpu_index=", cfg.gpu_device_index, "), swapchain size=", vw, "x", vh);

    g_flash_until_us = 0;

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

    // 9. Start IPC Server
    g_ipc.Start(OnIpcRequest);

    // 10. Open Settings UI on first startup
    SettingsUI::Open(hInstance);

    TrimWorkingSetMemory();

    // 11. Main Event & Render Loop
    MSG msg = {};
    float audio_buffer[4096 * 2];
    uint64_t last_trim_us = 0;
    uint64_t last_state_update_us = 0;

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
        bool battery_pause_active = (power == PowerState::Reduced && cfg.pause_on_battery);
        bool lock_pause_active = (power == PowerState::Sleeping && cfg.pause_on_lock);

        if (fullscreen_active || battery_pause_active || lock_pause_active) {
            if (!g_fullscreen_paused) {
                SuspendWallpaperForFullscreen();
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
            continue;
        } else if (g_fullscreen_paused) {
            ResumeWallpaperFromFullscreen();
        }

        // Apply battery vs AC FPS throttling
        if (power == PowerState::Reduced && cfg.battery_fps > 0) {
            if (g_clock.GetTargetFPS() != static_cast<uint32_t>(cfg.battery_fps)) {
                g_clock.SetTargetFPS(cfg.battery_fps);
            }
        } else if (power == PowerState::Active && cfg.target_fps > 0) {
            if (g_clock.GetTargetFPS() != static_cast<uint32_t>(cfg.target_fps)) {
                g_clock.SetTargetFPS(cfg.target_fps);
            }
        }

        // Re-attach if desktop was rebuilt, or retry injection if it initially failed
        static uint64_t last_inject_check_us = 0;
        uint64_t now_us = g_clock.GetCurrentTimeMicros();
        if (!g_fullscreen_paused && g_main_hwnd &&
            (now_us - last_inject_check_us >= 500000)) { // Check every 500ms
            last_inject_check_us = now_us;

            if (!g_inject_ok) {
                // Retry initial injection
                g_inject_ok = g_injector.Attach(g_main_hwnd);
                if (g_inject_ok) {
                    Logger::Info("Injection retry succeeded, workerw=0x",
                                 reinterpret_cast<size_t>(g_injector.GetWorkerW()));
                    g_last_error.clear();
                    ShowWindow(g_main_hwnd, SW_SHOW);
                    // Resize swap chain to match desktop dimensions
                    RECT rc;
                    if (GetClientRect(g_main_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
                        g_presenter.Resize(rc.right - rc.left, rc.bottom - rc.top);
                    }
                }
            } else if (!g_injector.IsAttachedValid()) {
                // Desktop hierarchy changed, re-attach
                Logger::Info("Desktop hierarchy changed, re-attaching wallpaper");
                g_injector.Reattach(g_main_hwnd);
                g_inject_ok = g_injector.IsAttached();
                if (g_inject_ok && g_main_hwnd) {
                    ShowWindow(g_main_hwnd, SW_SHOW);
                    RECT rc;
                    if (GetClientRect(g_main_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
                        g_presenter.Resize(rc.right - rc.left, rc.bottom - rc.top);
                    }
                }
            }
        }

        // Update Shared Engine State for instant zero-latency UI reads
        if (now_us - last_state_update_us >= 250000) { // Every 250ms
            last_state_update_us = now_us;
            std::string cur_vid = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : "";
            auto info = g_decoder.GetInfo();
            size_t ram = GetProcessMemoryUsageMB();
            double cpu = GetProcessCpuUsagePercent();
            size_t vram = GetProcessVramUsageMB(info.width, info.height, vw, vh, g_decoder_hw);

            g_shared_engine_state.connected.store(true);
            g_shared_engine_state.playing.store(!g_paused && !cur_vid.empty() && !g_fullscreen_paused);
            g_shared_engine_state.paused.store(g_paused);
            g_shared_engine_state.injected.store(g_inject_ok);
            g_shared_engine_state.hw_decode.store(g_decoder_hw);
            g_shared_engine_state.fps.store(g_clock.GetTargetFPS());
            g_shared_engine_state.video_fps.store(info.fps);
            g_shared_engine_state.width.store(info.width);
            g_shared_engine_state.height.store(info.height);
            g_shared_engine_state.duration.store(info.duration_seconds);
            g_shared_engine_state.ram_mb.store(ram);
            g_shared_engine_state.vram_mb.store(vram);
            g_shared_engine_state.cpu_percent.store(cpu);
            g_shared_engine_state.frames_rendered.store(g_frames_rendered);
            g_shared_engine_state.frames_decoded.store(g_frames_decoded);
            g_shared_engine_state.frame_skip.store(g_frame_skip);
            g_shared_engine_state.SetCurrentVideo(cur_vid);
            g_shared_engine_state.SetCodec(info.codec_name);
            g_shared_engine_state.SetLastError(g_last_error);
        }

        // Periodic process working set memory trimming every 5 seconds
        if (now_us - last_trim_us >= 5000000) {
            last_trim_us = now_us;
            TrimWorkingSetMemory();
        }

        bool should_pause = (power == PowerState::Sleeping) ||
                            (power == PowerState::Reduced && cfg.pause_on_battery) ||
                            g_paused;

        if (should_pause) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
            continue;
        }

        int display_fps_cap = (power == PowerState::Reduced) ? cfg.battery_fps : cfg.target_fps;
        g_frame_skip = ComputeFrameSkip(g_video_fps, display_fps_cap);

        if (g_clock.ShouldRenderFrame()) {
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
                } else if (!g_current_frame.texture && g_last_error.empty()) {
                    g_last_error = "Decoded frame has no GPU texture (HW decode inactive)";
                    Logger::Info(g_last_error);
                }

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

        case WM_USER + 1:
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
        g_flash_until_us = g_clock.GetCurrentTimeMicros() + 1500000;
        return nlohmann::json{{"ok", true}}.dump();
    } else if (cmd == "set_lockscreen") {
        std::string path = req.value("path", "");
        if (!path.empty()) {
            auto& cfg = g_config.Get();
            cfg.AddToGallery(path);
            g_config.Save();

            std::lock_guard<std::mutex> lock(g_decoder_mutex);
            FFmpegHWDecoder tempDecoder;
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (tempDecoder.Open(path.c_str(), g_presenter.GetDevice(), vw, vh)) {
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
        if (g_main_hwnd) {
            PostMessageW(g_main_hwnd, WM_APP_DEATTACH, 0, 0);
        }
        TrimWorkingSetMemory();
        return "{\"ok\":true}";
    } else if (cmd == "set_volume") {
        float vol = req.value("volume", 0.0f);
        g_audio.SetVolume(vol);
        if (vol > 0.0f && !g_audio.IsRunning()) {
            g_audio.Init();
        }
        g_decoder.SetAudioEnabled(vol > 0.0f && !g_audio.IsMuted());
        return "{\"ok\":true}";
    } else if (cmd == "set_fps") {
        int fps = req.value("fps", 30);
        if (fps < 1) fps = 1;
        g_config.Get().target_fps = fps;
        g_clock.SetTargetFPS(fps);
        g_config.Save();
        return "{\"ok\":true}";
    } else if (cmd == "set_render_device") {
        int gpu_idx = req.value("gpu_index", 0);
        auto& cfg = g_config.Get();
        cfg.gpu_device_index = gpu_idx;
        g_config.Save();

        std::string current_video = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : "";

        {
            std::lock_guard<std::mutex> lock(g_decoder_mutex);
            g_decoder.Close();
            g_current_frame = VideoFrame{};

            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            g_presenter.Cleanup();
            g_presenter.Init(g_main_hwnd, vw, vh, gpu_idx);

            g_decoder.SetForceSoftware(gpu_idx == -1);
            if (!current_video.empty()) {
                bool audio_on = !cfg.wallpapers.empty() && cfg.wallpapers[0].audio_enabled && (cfg.wallpapers[0].volume > 0.0f);
                g_decoder.SetAudioEnabled(audio_on);
                g_decoder.Open(current_video.c_str(), g_presenter.GetDevice(), vw, vh);
                g_decoder_hw = g_decoder.IsHWAccelerated();
                g_clock.Reset();
            }
            g_shared_engine_state.active_gpu_index.store(gpu_idx);
            g_shared_engine_state.hw_decode.store(g_decoder_hw);
        }
        return "{\"ok\":true}";
    } else if (cmd == "reload_config") {
        g_config.Load();
        auto& cfg = g_config.Get();
        if (cfg.target_fps > 0) {
            g_clock.SetTargetFPS(cfg.target_fps);
        }
        if (!cfg.wallpapers.empty()) {
            float vol = cfg.wallpapers[0].volume;
            g_audio.SetVolume(vol);
            g_decoder.SetAudioEnabled(vol > 0.0f && !g_audio.IsMuted());
        }
        return "{\"ok\":true}";
    }

    return "{\"ok\":true}";
}
