// MiniWallpaper — Ultra-lightweight animated video wallpaper engine
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <psapi.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <mimalloc.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

#include "core/config.h"
#include "core/engine_controller.h"
#include "core/engine_state.h"
#include "core/ipc_server.h"
#include "core/legacy_migrator.h"
#include "core/logger.h"
#include "core/playback_clock.h"
#include "core/video_optimizer.h"
#include "core/win_utils.h"
#include "decoder/audio_player.h"
#include "decoder/ffmpeg_hw_decoder.h"
#include "diagnostics/memprobe.h"
#include "platform/win32/d3d11_presenter.h"
#include "platform/win32/desktop_injector.h"
#include "platform/win32/hardware_info.h"
#include "platform/win32/lockscreen_manager.h"
#include "platform/win32/power_governor.h"
#include "platform/win32/screensaver_runner.h"
#include "platform/win32/tray_icon.h"
#include "ui/settings_app.h"
#include "ui/thumbnail_manager.h"

#define WM_APP_OPEN_SETTINGS     (WM_APP + 10)
#define WM_APP_ATTACH            (WM_APP + 11)
#define WM_APP_DEATTACH          (WM_APP + 12)
#define WM_APP_SET_RENDER_DEVICE (WM_APP + 13) // wParam = gpu_index + 1 (0 encodes -1)
#define WM_APP_WAKE_MAIN         (WM_APP + 14)

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

using namespace miniwp;

namespace {

// Global engine subsystems (confined to main translation unit)
PlaybackClock     g_clock;
DesktopInjector   g_injector;
D3D11Presenter    g_presenter;
FFmpegHWDecoder   g_decoder;
AudioPlayer       g_audio;
PowerGovernor     g_governor;
TrayIcon          g_tray;
IpcServer         g_ipc;
LockScreenManager g_lockscreen;

std::mutex        g_decoder_mutex; // Guards g_decoder and g_current_frame
std::mutex        g_task_mutex;    // Guards g_main_tasks queue
std::vector<std::function<void()>> g_main_tasks;

DWORD       g_main_thread_id = 0;
bool        g_engine_initialized = false;
bool        g_render_window_shown = false;
bool        g_running = true;
bool        g_paused = false;
HWND        g_main_hwnd = nullptr;
VideoFrame  g_current_frame;
bool        g_inject_ok = false;
bool        g_decoder_hw = false;
std::string g_last_error;
uint64_t    g_frames_rendered = 0;
int64_t     g_flash_until_us = 0;

// Playback pacing state
double      g_video_fps = 0.0;
int         g_frame_skip = 1;
uint64_t    g_frames_decoded = 0;

bool        g_fullscreen_paused = false;
std::string g_loaded_video_path;
double      g_suspend_resume_position_sec = -1.0;
VideoInfo   g_cached_video_info;

bool        g_first_frame_captured = false;
bool        g_initial_frame_trimmed = false;
uint64_t    g_pause_start_us = 0;
bool        g_deep_suspended = false;

// Target display viewport cache (avoids calling EnumDisplayMonitors on every frame)
std::vector<DisplayViewport> s_cached_target_vps;
std::vector<int>             s_last_target_displays;
bool                         s_displays_dirty = true;

// High-resolution frame-pacing timer with automatic fallback for pre-Win10 1803 OS
class FramePacerTimer {
public:
    FramePacerTimer() {
        m_timer = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS
        );
        if (m_timer) {
            m_high_res = true;
        } else {
            m_timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
            m_high_res = false;
        }
    }

    ~FramePacerTimer() {
        SetActivePlayback(false);
        if (m_timer) {
            CloseHandle(m_timer);
            m_timer = nullptr;
        }
    }

    FramePacerTimer(const FramePacerTimer&) = delete;
    FramePacerTimer& operator=(const FramePacerTimer&) = delete;

    // Only raises OS timer interrupt rate on legacy Windows builds lacking high-res waitable timers,
    // and strictly releases it the moment playback pauses, occludes, or sleeps.
    void SetActivePlayback(bool active) noexcept {
        if (m_high_res) return;
        if (active && !m_period_raised) {
            timeBeginPeriod(1);
            m_period_raised = true;
        } else if (!active && m_period_raised) {
            timeEndPeriod(1);
            m_period_raised = false;
        }
    }

    void WaitMicros(uint64_t sleep_us, bool ui_open) {
        if (ui_open) {
            uint64_t ui_budget_us = static_cast<uint64_t>(SettingsUI::GetPreferredWaitMs()) * 1000ULL;
            if (sleep_us > ui_budget_us) {
                sleep_us = ui_budget_us;
            }
        }
        if (sleep_us == 0) return;

        if (m_timer) {
            LARGE_INTEGER due = {};
            due.QuadPart = -static_cast<LONGLONG>(sleep_us * 10ULL); // 100-ns relative units
            if (SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE)) {
                DWORD safetyMs = static_cast<DWORD>(sleep_us / 1000ULL) + 15;
                MsgWaitForMultipleObjects(1, &m_timer, FALSE, safetyMs, QS_ALLINPUT);
                return;
            }
        }

        DWORD sleepMs = static_cast<DWORD>((sleep_us + 500ULL) / 1000ULL);
        if (sleepMs > 0) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, sleepMs, QS_ALLINPUT);
        }
    }

private:
    HANDLE m_timer = nullptr;
    bool   m_high_res = false;
    bool   m_period_raised = false;
};

// Adaptive soft-cap working-set trimmer.
// Reclaims one-time DXGI/NVDEC/font-init scratch pages to Windows Standby List while
// keeping the active steady-state working set (~1 MB background, ~9 MB UI open) untouched.
class SmartWorkingSetTrimmer {
public:
    void TrimNow(uint64_t now_us = 0) {
        mi_collect(true);
        SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
        m_last_trim_us = now_us;
        m_pending_trim = false;
    }

    void ScheduleSettledTrim(uint64_t now_us, uint64_t delay_us = 400000ULL) noexcept {
        m_pending_trim = true;
        m_pending_due_us = now_us + delay_us;
    }

    void NotifyUiClosed(uint64_t now_us) noexcept {
        m_last_trim_us = now_us;
        m_pending_trim = false;
    }

    void Tick(uint64_t now_us, bool ui_open) {
        if (ui_open && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0)) {
            return; // Never interrupt active UI mouse drags or slider scrubs
        }

        if (m_pending_trim && now_us >= m_pending_due_us) {
            TrimNow(now_us);
            return;
        }

        if (now_us - m_last_check_us < 1000000ULL) {
            return;
        }
        m_last_check_us = now_us;

        PROCESS_MEMORY_COUNTERS_EX pmc = {};
        if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            return;
        }
        size_t ws_mb = pmc.WorkingSetSize / (1024 * 1024);
        bool periodic = (now_us - m_last_trim_us >= 30000000ULL);
        if (ws_mb >= 16 || periodic) {
            TrimNow(now_us);
        }
    }

private:
    bool     m_pending_trim = false;
    uint64_t m_pending_due_us = 0;
    uint64_t m_last_trim_us = 0;
    uint64_t m_last_check_us = 0;
};

SmartWorkingSetTrimmer g_trimmer;

// Adaptive boot readiness gate for silent Windows startup (--startup).
// Keeps FFmpeg & D3D11 DLLs completely unloaded while Windows finishes heavy logon work,
// handing off smoothly from the native static wallpaper once system CPU < 40% for 2s (2s..12s window).
class BootReadinessGate {
public:
    explicit BootReadinessGate(uint64_t boot_start_us) noexcept
        : m_boot_start_us(boot_start_us), m_last_sample_us(boot_start_us) {
        SampleCpuPercent();
    }

    bool PollReady(uint64_t now_us) noexcept {
        uint64_t elapsed_us = (now_us >= m_boot_start_us) ? (now_us - m_boot_start_us) : 0;
        if (elapsed_us >= 12000000ULL) {
            return true; // 12s max ceiling
        }
        if (now_us - m_last_sample_us >= 500000ULL) {
            m_last_sample_us = now_us;
            double sys_cpu = SampleCpuPercent();
            if (sys_cpu < 40.0) {
                m_calm_streak++;
            } else {
                m_calm_streak = 0;
            }
        }
        return (elapsed_us >= 2000000ULL && m_calm_streak >= 4);
    }

private:
    double SampleCpuPercent() noexcept {
        FILETIME idle_ft = {}, kernel_ft = {}, user_ft = {};
        if (!GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) {
            return 0.0;
        }
        auto toU64 = [](const FILETIME& ft) noexcept -> uint64_t {
            return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        };
        uint64_t idle = toU64(idle_ft);
        uint64_t kernel = toU64(kernel_ft);
        uint64_t user = toU64(user_ft);

        if (!m_has_baseline) {
            m_prev_idle = idle;
            m_prev_kernel = kernel;
            m_prev_user = user;
            m_has_baseline = true;
            return 100.0;
        }

        uint64_t idle_diff = idle - m_prev_idle;
        uint64_t total_diff = (kernel - m_prev_kernel) + (user - m_prev_user);
        m_prev_idle = idle;
        m_prev_kernel = kernel;
        m_prev_user = user;

        if (total_diff == 0 || total_diff < idle_diff) return 0.0;
        return static_cast<double>(total_diff - idle_diff) * 100.0 / static_cast<double>(total_diff);
    }

    uint64_t m_boot_start_us = 0;
    uint64_t m_last_sample_us = 0;
    uint64_t m_prev_idle = 0;
    uint64_t m_prev_kernel = 0;
    uint64_t m_prev_user = 0;
    int      m_calm_streak = 0;
    bool     m_has_baseline = false;
};

void UpdateTargetViewports(const std::vector<int>& target_displays) {
    if (!s_displays_dirty && target_displays == s_last_target_displays) {
        return;
    }
    s_cached_target_vps.clear();
    s_last_target_displays = target_displays;
    s_displays_dirty = false;

    if (!target_displays.empty()) {
        auto all_displays = HardwareDetector::GetDisplayList();
        int virt_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int virt_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        for (int disp_idx : target_displays) {
            if (disp_idx >= 0 && disp_idx < static_cast<int>(all_displays.size())) {
                const auto& d = all_displays[disp_idx];
                s_cached_target_vps.push_back({
                    static_cast<float>(d.pos_x - virt_x),
                    static_cast<float>(d.pos_y - virt_y),
                    static_cast<float>(d.width),
                    static_cast<float>(d.height)
                });
            }
        }
    }
}

int ComputeFrameSkip(double video_fps, int display_fps) {
    if (video_fps <= 0.0 || video_fps > 240.0) return 1;
    if (display_fps <= 0) return 1;
    if (display_fps >= video_fps) return 1;
    double skip = std::round(video_fps / display_fps);
    if (skip < 1.0) skip = 1.0;
    if (skip > 4.0) skip = 4.0;
    return static_cast<int>(skip);
}

void SetVideoPacing(double video_fps) {
    if (video_fps <= 0.0 || video_fps > 240.0) video_fps = 30.0;
    g_video_fps = video_fps;
    auto& cfg = g_config.Get();
    int fps = (cfg.target_fps > 0) ? cfg.target_fps : static_cast<int>(video_fps + 0.5);
    fps = std::clamp(fps, 1, 120);
    g_clock.SetTargetFPS(fps);
    g_frames_decoded = 0;
}

double GetProcessCpuUsagePercent() {
    static ULARGE_INTEGER lastCPU = {0}, lastSysCPU = {0}, lastUserCPU = {0};
    static int numProcessors = -1;
    static bool first = true;
    static double last_percent = 0.0;

    if (first) {
        SYSTEM_INFO sysInfo;
        FILETIME ftime, fsys, fuser;
        GetSystemInfo(&sysInfo);
        numProcessors = (sysInfo.dwNumberOfProcessors > 0) ? static_cast<int>(sysInfo.dwNumberOfProcessors) : 1;
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
        double percent = static_cast<double>((sys.QuadPart - lastSysCPU.QuadPart) + (user.QuadPart - lastUserCPU.QuadPart));
        percent /= static_cast<double>(now.QuadPart - lastCPU.QuadPart);
        percent /= numProcessors;
        last_percent = std::clamp(percent * 100.0, 0.0, 100.0);
        lastCPU = now;
        lastUserCPU = user;
        lastSysCPU = sys;
    }
    return last_percent;
}

size_t EstimateProcessVramUsageMB(int vid_w, int vid_h, int screen_w, int screen_h, bool hw_decode) {
    if (!hw_decode || vid_w <= 0 || vid_h <= 0) return 12;
    size_t frame_bytes = static_cast<size_t>(vid_w) * vid_h * 3 / 2;
    size_t pool_bytes = 4 * frame_bytes;
    size_t swapchain_bytes = 2 * static_cast<size_t>(screen_w) * screen_h * 4;
    return (pool_bytes + swapchain_bytes) / (1024 * 1024) + 6;
}

bool OpenWallpaperVideo(const std::string& path) {
    auto& cfg = g_config.Get();
    bool audio_on = !cfg.wallpapers.empty() && cfg.wallpapers[0].audio_enabled && (cfg.wallpapers[0].volume > 0.001f);

    std::lock_guard<std::mutex> lock(g_decoder_mutex);
    g_current_frame = VideoFrame{};
    g_presenter.ReleaseVideoResources();
    g_first_frame_captured = false;
    g_initial_frame_trimmed = false;
    g_decoder.Close();
    g_decoder.SetAudioEnabled(audio_on);
    g_decoder.SetForceSoftware(cfg.gpu_device_index == -1);

    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    bool ok = g_decoder.Open(path.c_str(), g_presenter.GetDevice(), vw, vh);
    g_decoder_hw = g_decoder.IsHWAccelerated();
    g_paused = false;
    g_clock.Reset();
    g_loaded_video_path.clear();
    g_shared_engine_state.current_time_sec.store(0.0);

    if (!ok) {
        g_cached_video_info = VideoInfo{};
        g_last_error = "Failed to open video: " + path;
        Logger::Info(g_last_error);
        return false;
    }

    g_loaded_video_path = path;
    auto info = g_decoder.GetInfo();
    g_cached_video_info = info;
    g_last_error.clear();
    SetVideoPacing(info.fps);

    // Sync shared state immediately so UI reads zero-latency dimensions/codec
    g_shared_engine_state.hw_decode.store(g_decoder_hw);
    g_shared_engine_state.fps.store(g_clock.GetTargetFPS());
    g_shared_engine_state.video_fps.store(info.fps);
    g_shared_engine_state.width.store(info.width);
    g_shared_engine_state.height.store(info.height);
    g_shared_engine_state.duration.store(info.duration_seconds);
    g_shared_engine_state.SetCurrentVideo(path);
    g_shared_engine_state.SetCodec(info.codec_name);
    g_shared_engine_state.SetLastError("");

    Logger::Info("Opened video OK: ", path,
                 " codec=", info.codec_name,
                 " ", info.width, "x", info.height,
                 " fps=", info.fps,
                 " hw=", g_decoder_hw ? "yes" : "no");
    return true;
}

void EnterDeepSuspend() {
    if (g_deep_suspended) return;
    g_deep_suspended = true;
    g_audio.Stop();

    double saved_pos = 0.0;
    {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        double pos = g_shared_engine_state.current_time_sec.load();
        if (pos > 0.0 && !g_loaded_video_path.empty()) {
            g_suspend_resume_position_sec = pos;
            saved_pos = pos;
        }
        g_decoder.Close();
        g_current_frame = VideoFrame{};
        g_presenter.ReleaseVideoResources();
        g_first_frame_captured = false;
        g_loaded_video_path.clear();
    }
    g_trimmer.TrimNow(g_clock.GetCurrentTimeMicros());
    if (saved_pos > 0.0) {
        Logger::Info("Deep suspend active: released decoder (VRAM + RAM) & trimmed working set, position=", saved_pos, "s");
    } else {
        Logger::Info("Deep suspend active: released decoder (VRAM + RAM) & trimmed working set");
    }
}

void LeaveDeepSuspend() {
    g_deep_suspended = false;
    g_pause_start_us = 0;

    auto& cfg = g_config.Get();
    bool need_load = false;
    {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        need_load = g_loaded_video_path.empty() && !cfg.wallpapers.empty() && !cfg.wallpapers[0].video_path.empty();
    }
    if (need_load) {
        OpenWallpaperVideo(cfg.wallpapers[0].video_path);
    }
    if (g_suspend_resume_position_sec > 0.0) {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        g_decoder.SeekTo(g_suspend_resume_position_sec);
        g_suspend_resume_position_sec = -1.0;
    }
    if (!cfg.wallpapers.empty() && cfg.wallpapers[0].audio_enabled && cfg.wallpapers[0].volume > 0.001f && !g_audio.IsRunning()) {
        g_audio.Init();
    }
    g_clock.Reset();
    g_trimmer.ScheduleSettledTrim(g_clock.GetCurrentTimeMicros(), 3000000ULL);
    Logger::Info("Resumed from deep suspend: decoder reopened, playback restored");
}

void SuspendWallpaper(const std::string& reason = "Fullscreen app", const std::string& detail = "") {
    if (g_fullscreen_paused) return;
    g_fullscreen_paused = true;
    g_pause_start_us = g_clock.GetCurrentTimeMicros();
    g_audio.Stop();
    if (!detail.empty()) {
        Logger::Info("Playback auto-paused: ", reason, " [", detail, "]");
    } else {
        Logger::Info("Playback auto-paused: ", reason);
    }
}

void ResumeWallpaper() {
    if (!g_fullscreen_paused) return;
    g_fullscreen_paused = false;
    g_shared_engine_state.paused.store(false);
    g_pause_start_us = 0;

    auto& cfg = g_config.Get();
    if (g_deep_suspended) {
        LeaveDeepSuspend();
    } else {
        bool need_load = false;
        {
            std::lock_guard<std::mutex> lock(g_decoder_mutex);
            need_load = g_loaded_video_path.empty() && !cfg.wallpapers.empty() && !cfg.wallpapers[0].video_path.empty();
        }
        if (need_load) {
            OpenWallpaperVideo(cfg.wallpapers[0].video_path);
        }
    }
    if (!cfg.wallpapers.empty() && cfg.wallpapers[0].audio_enabled && cfg.wallpapers[0].volume > 0.001f && !g_audio.IsRunning()) {
        g_audio.Init();
    }
    g_clock.Reset();

    if (g_main_hwnd && (!g_inject_ok || !g_injector.IsAttachedValid())) {
        bool show_now = (g_frames_rendered > 0);
        g_injector.Reattach(g_main_hwnd, show_now);
        g_inject_ok = g_injector.IsAttached();
        g_render_window_shown = g_inject_ok && show_now;
        if (g_inject_ok) {
            RECT rc;
            if (GetClientRect(g_main_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
                g_presenter.Resize(rc.right - rc.left, rc.bottom - rc.top);
            }
        }
    }
    Logger::Info("Playback auto-resumed: Desktop is visible and active");
}

struct ScopedLowPriorityInit {
    bool active = false;
    explicit ScopedLowPriorityInit(bool enable) noexcept {
        if (enable) {
            active = (SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE);
        }
    }
    ~ScopedLowPriorityInit() {
        if (active) {
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        }
    }
    ScopedLowPriorityInit(const ScopedLowPriorityInit&) = delete;
    ScopedLowPriorityInit& operator=(const ScopedLowPriorityInit&) = delete;
};

bool EnsureEngineInitialized(bool low_priority_io) {
    if (g_engine_initialized) return true;
    if (!g_main_hwnd) return false;

    ScopedLowPriorityInit bg_io_guard(low_priority_io);

    auto& cfg = g_config.Get();
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    // Attach hidden initially so Windows' native static wallpaper stays visible until Frame #1 presents
    g_inject_ok = g_injector.Attach(g_main_hwnd, false);
    g_render_window_shown = false;
    g_injector.RegisterExplorerRestart(g_main_hwnd);
    Logger::Info("Attach: ", g_inject_ok ? "OK" : "FAILED",
                 " workerw=0x", reinterpret_cast<size_t>(g_injector.GetWorkerW()),
                 " visible=", IsWindowVisible(g_main_hwnd) ? "yes" : "no");
    if (!g_inject_ok) {
        Logger::Info("Desktop injection failed, will retry periodically");
    }

    if (!g_presenter.Init(g_main_hwnd, vw, vh, cfg.gpu_device_index)) {
        g_last_error = "D3D11 presenter init failed";
        g_shared_engine_state.SetLastError(g_last_error);
        Logger::Info(g_last_error);
        return false;
    }

    g_engine_initialized = true;
    g_frames_rendered = 0;
    g_shared_engine_state.active_gpu_index.store(cfg.gpu_device_index);
    g_shared_engine_state.SetActiveGpuInfo(g_presenter.GetAdapterName(), g_presenter.GetAdapterType());
    Logger::Info("Presenter init OK (gpu_index=", cfg.gpu_device_index, "), swapchain size=", vw, "x", vh);

    g_flash_until_us = 0;

    if (!cfg.wallpapers.empty() && !cfg.wallpapers[0].video_path.empty()) {
        OpenWallpaperVideo(cfg.wallpapers[0].video_path);
    }

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (cfg.update_desktop_wallpaper) {
        std::string d_vid = (cfg.desktop_static_source_mode == 1 && !cfg.desktop_static_video_path.empty())
            ? cfg.desktop_static_video_path
            : (!cfg.wallpapers.empty() ? cfg.wallpapers[0].video_path : "");
        double d_ts = (cfg.desktop_static_source_mode == 1) ? cfg.desktop_static_timestamp : 1.0;
        if (!d_vid.empty()) {
            g_lockscreen.SyncFromVideoAsync(d_vid, sw, sh, false, true, d_ts);
        }
    }
    if (cfg.update_lockscreen) {
        std::string l_vid = (cfg.lockscreen_source_mode == 1 && !cfg.lockscreen_video_path.empty())
            ? cfg.lockscreen_video_path
            : (!cfg.wallpapers.empty() ? cfg.wallpapers[0].video_path : "");
        double l_ts = (cfg.lockscreen_source_mode == 1) ? cfg.lockscreen_timestamp : 1.0;
        if (!l_vid.empty()) {
            g_lockscreen.SyncFromVideoAsync(l_vid, sw, sh, true, false, l_ts);
        }
    }

    if (!cfg.wallpapers.empty()) {
        bool audio_on = cfg.wallpapers[0].audio_enabled && (cfg.wallpapers[0].volume > 0.001f);
        g_audio.SetVolume(cfg.wallpapers[0].volume);
        g_audio.SetMuted(!audio_on);
        if (audio_on) {
            g_audio.Init();
        }
    }

    g_clock.Reset();
    g_trimmer.TrimNow(g_clock.GetCurrentTimeMicros());
    return true;
}

void ReinitRendererDevice(int gpu_idx) {
    bool ui_was_open = SettingsUI::IsOpen();
    SettingsUI::Shutdown();

    auto& cfg = g_config.Get();
    std::string current_video = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : "";

    {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        g_decoder.Close();
        g_current_frame = VideoFrame{};
        g_loaded_video_path.clear();
        g_first_frame_captured = false;

        int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        g_presenter.Cleanup();
        g_presenter.Init(g_main_hwnd, vw, vh, gpu_idx);

        g_decoder.SetForceSoftware(gpu_idx == -1);
        if (!current_video.empty()) {
            bool audio_on = !cfg.wallpapers.empty() && cfg.wallpapers[0].audio_enabled && (cfg.wallpapers[0].volume > 0.001f);
            g_decoder.SetAudioEnabled(audio_on);
            g_decoder.Open(current_video.c_str(), g_presenter.GetDevice(), vw, vh);
            g_loaded_video_path = current_video;
            g_decoder_hw = g_decoder.IsHWAccelerated();
            g_clock.Reset();
        }
        g_shared_engine_state.active_gpu_index.store(gpu_idx);
        g_shared_engine_state.hw_decode.store(g_decoder_hw);
        g_shared_engine_state.SetActiveGpuInfo(g_presenter.GetAdapterName(), g_presenter.GetAdapterType());
        g_shared_engine_state.vram_mb.store(g_presenter.GetVramUsageMB());
    }

    if (ui_was_open) SettingsUI::Open(GetModuleHandleW(nullptr));
    g_trimmer.ScheduleSettledTrim(g_clock.GetCurrentTimeMicros(), 2500000ULL);
}

} // namespace

// ============================================================================
// EngineController Implementation (Direct Main-Thread Control API)
// ============================================================================
namespace miniwp {
namespace EngineController {

void PostToMainThread(std::function<void()> task) {
    if (!task) return;
    {
        std::lock_guard<std::mutex> lock(g_task_mutex);
        g_main_tasks.push_back(std::move(task));
    }
    if (g_main_hwnd) {
        PostMessageW(g_main_hwnd, WM_APP_WAKE_MAIN, 0, 0);
    } else if (HWND uiHwnd = SettingsUI::GetHwnd()) {
        PostMessageW(uiHwnd, WM_NULL, 0, 0);
    }
}

void DrainMainThreadTasks() {
    std::vector<std::function<void()>> local;
    {
        std::lock_guard<std::mutex> lock(g_task_mutex);
        if (g_main_tasks.empty()) return;
        local.swap(g_main_tasks);
    }
    for (auto& fn : local) {
        if (fn) fn();
    }
}

bool SetWallpaper(const std::string& video_path) {
    if (video_path.empty()) return false;

    auto& cfg = g_config.Get();
    if (cfg.wallpapers.empty()) {
        MonitorWallpaper mw;
        mw.video_path = video_path;
        cfg.wallpapers.push_back(mw);
    } else {
        cfg.wallpapers[0].video_path = video_path;
    }
    if (!IsOptimizedCachePath(video_path)) {
        cfg.AddToGallery(video_path);
    }
    g_config.Save();

    if (!g_engine_initialized) {
        if (!g_main_hwnd) {
            return true;
        }
        if (!EnsureEngineInitialized(false)) {
            return false;
        }
        g_shared_engine_state.playing.store(!g_paused && !g_fullscreen_paused && !g_loaded_video_path.empty());
        g_shared_engine_state.paused.store(false);
        return !g_loaded_video_path.empty();
    }

    g_suspend_resume_position_sec = -1.0;
    bool ok = OpenWallpaperVideo(video_path);
    if (ok) {
        if (g_fullscreen_paused) {
            g_deep_suspended = false;
        }
        g_trimmer.ScheduleSettledTrim(g_clock.GetCurrentTimeMicros(), 2500000ULL);
        if (g_main_hwnd) {
            PostMessageW(g_main_hwnd, WM_APP_ATTACH, 0, 0);
        }
        if (cfg.update_desktop_wallpaper || cfg.update_lockscreen) {
            int sw = GetSystemMetrics(SM_CXSCREEN);
            int sh = GetSystemMetrics(SM_CYSCREEN);
            bool sync_desk = cfg.update_desktop_wallpaper && (cfg.desktop_static_source_mode == 0);
            bool sync_lock = cfg.update_lockscreen && (cfg.lockscreen_source_mode == 0);
            if (sync_desk || sync_lock) {
                g_lockscreen.SyncFromVideoAsync(video_path, sw, sh, sync_lock, sync_desk, 1.0);
            }
        }
        g_shared_engine_state.playing.store(!g_paused && !g_fullscreen_paused);
        g_shared_engine_state.paused.store(false);
    }
    return ok;
}

void Pause() {
    g_paused = true;
    g_shared_engine_state.playing.store(false);
    g_shared_engine_state.paused.store(true);
}

void Resume() {
    g_paused = false;
    if (!g_engine_initialized) {
        if (!g_main_hwnd || !EnsureEngineInitialized(false)) return;
    }

    auto& cfg = g_config.Get();
    if (!g_fullscreen_paused) {
        if (g_deep_suspended) {
            LeaveDeepSuspend();
        } else {
            bool need_load = false;
            {
                std::lock_guard<std::mutex> lock(g_decoder_mutex);
                need_load = g_loaded_video_path.empty() && !cfg.wallpapers.empty() && !cfg.wallpapers[0].video_path.empty();
            }
            if (need_load) {
                OpenWallpaperVideo(cfg.wallpapers[0].video_path);
            }
        }
    }
    g_clock.Reset();
    if (g_main_hwnd) {
        PostMessageW(g_main_hwnd, WM_APP_ATTACH, 0, 0);
    }
    bool has_vid = !cfg.wallpapers.empty() && !cfg.wallpapers[0].video_path.empty();
    g_shared_engine_state.paused.store(false);
    g_shared_engine_state.playing.store(has_vid && !g_fullscreen_paused);
}

void TogglePause() {
    if (g_paused) Resume();
    else Pause();
}

void StopToStatic() {
    auto& cfg = g_config.Get();
    if (!cfg.wallpapers.empty()) {
        cfg.wallpapers[0].video_path.clear();
        g_config.Save();
    }

    if (!g_engine_initialized) return;

    g_audio.Stop();
    {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        g_decoder.Close();
        g_current_frame = VideoFrame{};
        g_presenter.ReleaseVideoResources();
        g_first_frame_captured = false;
        g_loaded_video_path.clear();
        g_cached_video_info = VideoInfo{};
    }
    g_suspend_resume_position_sec = -1.0;
    g_shared_engine_state.current_time_sec.store(0.0);
    g_paused = true;
    SetVideoPacing(0.0);

    g_shared_engine_state.playing.store(false);
    g_shared_engine_state.paused.store(false);
    g_shared_engine_state.SetCurrentVideo("");
    g_shared_engine_state.fps.store(0);
    g_shared_engine_state.video_fps.store(0.0);

    if (g_main_hwnd) {
        PostMessageW(g_main_hwnd, WM_APP_DEATTACH, 0, 0);
    }
    g_trimmer.TrimNow(g_clock.GetCurrentTimeMicros());
}

void SetVolume(float volume, bool save_immediately) {
    volume = std::clamp(volume, 0.0f, 1.0f);
    auto& cfg = g_config.Get();
    if (cfg.wallpapers.empty()) {
        cfg.wallpapers.push_back(MonitorWallpaper{});
    }
    bool enable_audio = (volume > 0.001f);
    cfg.wallpapers[0].volume = volume;
    cfg.wallpapers[0].audio_enabled = enable_audio;

    if (save_immediately) g_config.Save();
    else g_config.MarkDirty();

    if (!g_engine_initialized) return;

    g_audio.SetVolume(volume);
    g_audio.SetMuted(!enable_audio);
    if (enable_audio && !g_paused && !g_fullscreen_paused && !g_deep_suspended && !g_audio.IsRunning()) {
        g_audio.Init();
    } else if (!enable_audio && g_audio.IsRunning()) {
        g_audio.Stop();
    }
    {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        g_decoder.SetAudioEnabled(enable_audio);
    }
}

void SetAudioEnabled(bool enabled, bool save_immediately) {
    auto& cfg = g_config.Get();
    if (cfg.wallpapers.empty()) {
        cfg.wallpapers.push_back(MonitorWallpaper{});
    }
    cfg.wallpapers[0].audio_enabled = enabled;
    if (enabled && cfg.wallpapers[0].volume <= 0.001f) {
        cfg.wallpapers[0].volume = 0.5f;
    }

    if (save_immediately) g_config.Save();
    else g_config.MarkDirty();

    if (!g_engine_initialized) return;

    float vol = cfg.wallpapers[0].volume;
    bool active = enabled && (vol > 0.001f);
    g_audio.SetVolume(vol);
    g_audio.SetMuted(!active);
    if (active && !g_paused && !g_fullscreen_paused && !g_deep_suspended && !g_audio.IsRunning()) {
        g_audio.Init();
    } else if (!active && g_audio.IsRunning()) {
        g_audio.Stop();
    }
    {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        g_decoder.SetAudioEnabled(active);
    }
}

void ToggleMute() {
    auto& cfg = g_config.Get();
    bool currently_enabled = !cfg.wallpapers.empty() && cfg.wallpapers[0].audio_enabled && !g_audio.IsMuted();
    SetAudioEnabled(!currently_enabled, true);
}

void SetScalingMode(int mode) {
    g_config.Get().scaling_mode = mode;
    g_config.Save();
}

void SetTargetFps(int fps, bool save_immediately) {
    fps = std::clamp(fps, 1, 240);
    g_config.Get().target_fps = fps;
    if (g_engine_initialized) {
        g_clock.SetTargetFPS(fps);
        g_shared_engine_state.fps.store(fps);
    }
    if (save_immediately) g_config.Save();
    else g_config.MarkDirty();
}

void SetTargetDisplays(const std::vector<int>& displays) {
    g_config.Get().target_displays = displays;
    s_displays_dirty = true;
    g_config.Save();
}

void SetRenderDevice(int gpu_index) {
    g_config.Get().gpu_device_index = gpu_index;
    g_config.Save();
    if (g_main_hwnd) {
        PostMessageW(g_main_hwnd, WM_APP_SET_RENDER_DEVICE, static_cast<WPARAM>(gpu_index + 1), 0);
    }
}

void TriggerTestFlash() {
    if (g_engine_initialized) {
        g_flash_until_us = g_clock.GetCurrentTimeMicros() + 1500000;
    }
}

void SyncVisualWallpaper(
    const std::string& video_path,
    bool sync_desktop,
    bool sync_lockscreen,
    double timestamp_sec,
    int save_mode
) {
    auto& cfg = g_config.Get();
    std::string target_vid = video_path;
    if (target_vid.empty() && !cfg.wallpapers.empty()) {
        target_vid = cfg.wallpapers[0].video_path;
    }

    if (save_mode == 1) {
        if (sync_desktop) {
            cfg.desktop_static_source_mode = 1;
            cfg.desktop_static_video_path = target_vid;
            cfg.desktop_static_timestamp = timestamp_sec;
            cfg.update_desktop_wallpaper = true;
        }
        if (sync_lockscreen) {
            cfg.lockscreen_source_mode = 1;
            cfg.lockscreen_video_path = target_vid;
            cfg.lockscreen_timestamp = timestamp_sec;
            cfg.update_lockscreen = true;
        }
        g_config.Save();
    }

    if (!target_vid.empty() && (sync_desktop || sync_lockscreen)) {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        g_lockscreen.SyncFromVideoAsync(target_vid, sw, sh, sync_lockscreen, sync_desktop, timestamp_sec, true);
    }
}

void ResetDesktopWallpaperAuto() {
    auto& cfg = g_config.Get();
    cfg.desktop_static_source_mode = 0;
    cfg.update_desktop_wallpaper = true;
    g_config.Save();
    std::string cur_vid = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : "";
    if (!cur_vid.empty()) {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        g_lockscreen.SyncFromVideoAsync(cur_vid, sw, sh, false, true, 1.0, true);
    }
}

void ResetLockscreenAuto() {
    auto& cfg = g_config.Get();
    cfg.lockscreen_source_mode = 0;
    cfg.update_lockscreen = true;
    g_config.Save();
    std::string cur_vid = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : "";
    if (!cur_vid.empty()) {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        g_lockscreen.SyncFromVideoAsync(cur_vid, sw, sh, true, false, 1.0, true);
    }
}

void OpenWallpaperDialog() {
    wchar_t filename[1024] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = SettingsUI::IsOpen() ? SettingsUI::GetHwnd() : nullptr;
    ofn.lpstrFilter = L"Video Files (*.mp4;*.webm;*.mkv;*.avi;*.mov)\0*.mp4;*.webm;*.mkv;*.avi;*.mov\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = static_cast<DWORD>(std::size(filename));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        SetWallpaper(WideToUtf8(filename));
    }
}

void RequestExit() {
    g_config.FlushIfDirty(true);
    g_running = false;
    PostQuitMessage(0);
}

} // namespace EngineController
} // namespace miniwp

// ============================================================================
// Win32 Window Proc, Tray Handler & Thread-Safe External IPC Handler
// ============================================================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void OnTrayAction(TrayAction action);
std::string OnIpcRequest(const std::string& json);

static std::string HandleIpcOnMainThread(const nlohmann::json& req) {
    std::string cmd = req.value("cmd", "");

    if (cmd == "open_settings") {
        if (req.contains("page") && req["page"].is_number_integer()) {
            SettingsUI::SetActivePage(req["page"].get<int>());
        }
        if (req.contains("detail") && req["detail"].is_boolean()) {
            SettingsUI::SetGalleryDetailVisible(req["detail"].get<bool>(), req.value("select", 0));
        }
        EnsureEngineInitialized(false);
        SettingsUI::Open(GetModuleHandleW(nullptr));
        return "{\"ok\":true}";
    }
    if (cmd == "capture_ui") {
        if (req.contains("page") && req["page"].is_number_integer()) {
            SettingsUI::SetActivePage(req["page"].get<int>());
        }
        if (req.contains("detail") && req["detail"].is_boolean()) {
            SettingsUI::SetGalleryDetailVisible(req["detail"].get<bool>(), req.value("select", 0));
        }
        std::string path = req.value("path", "");
        if (path.empty()) {
            return "{\"ok\":false,\"error\":\"missing path\"}";
        }
        EnsureEngineInitialized(false);
        if (!SettingsUI::IsOpen()) {
            SettingsUI::Open(GetModuleHandleW(nullptr));
        }
        for (int f = 0; f < 25; ++f) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            SettingsUI::RenderFrame();
            Sleep(16);
        }
        bool ok = SettingsUI::SaveScreenshot(path);
        return nlohmann::json{{"ok", ok}, {"path", path}}.dump();
    }
    if (cmd == "set_wallpaper") {
        std::string path = req.value("path", "");
        if (path.empty()) return "{\"ok\":false,\"error\":\"missing path\"}";
        bool ok = EngineController::SetWallpaper(path);
        return nlohmann::json{{"ok", ok}, {"hw", g_decoder_hw}}.dump();
    }
    if (cmd == "test_render") {
        EngineController::TriggerTestFlash();
        return "{\"ok\":true}";
    }
    if (cmd == "set_scaling") {
        EngineController::SetScalingMode(req.value("mode", 0));
        return "{\"ok\":true}";
    }
    if (cmd == "sync_desktop_wallpaper") {
        if (req.value("reset_desktop_auto", false)) {
            EngineController::ResetDesktopWallpaperAuto();
            return "{\"ok\":true}";
        }
        if (req.value("reset_lockscreen_auto", false)) {
            EngineController::ResetLockscreenAuto();
            return "{\"ok\":true}";
        }
        auto& cfg = g_config.Get();
        EngineController::SyncVisualWallpaper(
            req.value("path", ""),
            req.value("desktop", true),
            req.value("lockscreen", cfg.update_lockscreen),
            req.value("timestamp", 1.0),
            req.value("save_mode", -1)
        );
        return "{\"ok\":true}";
    }
    if (cmd == "get_status") {
        PROCESS_MEMORY_COUNTERS_EX pmc = {};
        size_t ram = 0, ram_priv = 0;
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            ram = pmc.WorkingSetSize / (1024 * 1024);
            ram_priv = pmc.PrivateUsage / (1024 * 1024);
        }
        VideoInfo info;
        if (g_engine_initialized) {
            std::lock_guard<std::mutex> lock(g_decoder_mutex);
            info = g_decoder.GetInfo();
            if (info.duration_seconds <= 0.0 && info.width <= 0) {
                info = g_cached_video_info;
            }
        }
        auto& cfg = g_config.Get();
        std::string current_video = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : "";
        return nlohmann::json{
            {"ok", true},
            {"playing", g_engine_initialized && !g_paused && !current_video.empty() && !g_fullscreen_paused},
            {"paused", g_paused && !current_video.empty()},
            {"fullscreen_paused", g_fullscreen_paused},
            {"current_video", current_video},
            {"fps", g_clock.GetTargetFPS()},
            {"video_fps", info.fps},
            {"width", info.width},
            {"height", info.height},
            {"duration", info.duration_seconds},
            {"current_time_sec", g_shared_engine_state.current_time_sec.load()},
            {"desktop_static_source_mode", cfg.desktop_static_source_mode},
            {"desktop_static_video_path", cfg.desktop_static_video_path},
            {"desktop_static_timestamp", cfg.desktop_static_timestamp},
            {"lockscreen_source_mode", cfg.lockscreen_source_mode},
            {"lockscreen_video_path", cfg.lockscreen_video_path},
            {"lockscreen_timestamp", cfg.lockscreen_timestamp},
            {"codec", info.codec_name},
            {"ram_mb", ram},
            {"ram_private_mb", ram_priv},
            {"vram_mb", g_shared_engine_state.vram_mb.load()},
            {"cpu_percent", g_shared_engine_state.cpu_percent.load()},
            {"active_gpu_name", g_shared_engine_state.GetActiveGpuName()},
            {"active_gpu_type", g_shared_engine_state.GetActiveGpuType()},
            {"injected", g_inject_ok},
            {"hw_decode", g_decoder_hw},
            {"system_ram_pct", g_governor.GetSystemRamPercent()},
            {"gpu_vram_pct", g_governor.GetGpuVramPercent()},
            {"resource_sleep", g_governor.IsInResourceSleep()},
            {"frames_rendered", g_frames_rendered},
            {"frames_decoded", g_frames_decoded},
            {"frame_skip", g_frame_skip},
            {"last_error", g_last_error}
        }.dump();
    }
    if (cmd == "pause") {
        EngineController::Pause();
        return "{\"ok\":true}";
    }
    if (cmd == "resume") {
        EngineController::Resume();
        return "{\"ok\":true}";
    }
    if (cmd == "stop") {
        EngineController::StopToStatic();
        return "{\"ok\":true}";
    }
    if (cmd == "set_volume") {
        EngineController::SetVolume(req.value("volume", 0.0f), true);
        return "{\"ok\":true}";
    }
    if (cmd == "set_fps") {
        EngineController::SetTargetFps(req.value("fps", 30), true);
        return "{\"ok\":true}";
    }
    if (cmd == "set_render_device") {
        EngineController::SetRenderDevice(req.value("gpu_index", 0));
        return "{\"ok\":true}";
    }
    if (cmd == "set_target_displays") {
        std::vector<int> displays;
        if (req.contains("displays") && req["displays"].is_array()) {
            for (const auto& item : req["displays"]) {
                if (item.is_number_integer()) displays.push_back(item.get<int>());
            }
        }
        EngineController::SetTargetDisplays(displays);
        return "{\"ok\":true}";
    }
    if (cmd == "reload_config") {
        g_config.Load();
        auto& cfg = g_config.Get();
        if (cfg.target_fps > 0) g_clock.SetTargetFPS(cfg.target_fps);
        if (!cfg.wallpapers.empty()) {
            EngineController::SetAudioEnabled(cfg.wallpapers[0].audio_enabled, false);
        }
        return "{\"ok\":true}";
    }

    return "{\"ok\":true}";
}

std::string OnIpcRequest(const std::string& request_json) {
    if (!g_running) {
        return "{\"ok\":false,\"error\":\"shutting down\"}";
    }

    auto req = nlohmann::json::parse(request_json, nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        return "{\"ok\":false,\"error\":\"invalid JSON request\"}";
    }

    if (GetCurrentThreadId() == g_main_thread_id) {
        return HandleIpcOnMainThread(req);
    }

    // Marshal external IPC command safely onto the main Win32/D3D11 thread
    auto task = std::make_shared<std::packaged_task<std::string()>>([req = std::move(req)]() {
        return HandleIpcOnMainThread(req);
    });
    std::future<std::string> fut = task->get_future();
    EngineController::PostToMainThread([task]() { (*task)(); });

    if (fut.wait_for(std::chrono::seconds(8)) == std::future_status::ready) {
        return fut.get();
    }
    return "{\"ok\":false,\"error\":\"main thread timeout\"}";
}

void OnTrayAction(TrayAction action) {
    switch (action) {
        case TrayAction::PauseResume:
            EngineController::TogglePause();
            break;
        case TrayAction::MuteUnmute:
            EngineController::ToggleMute();
            break;
        case TrayAction::OpenSettings:
            EnsureEngineInitialized(false);
            SettingsUI::Open(GetModuleHandleW(nullptr));
            break;
        case TrayAction::ChangeWallpaper:
            EnsureEngineInitialized(false);
            EngineController::OpenWallpaperDialog();
            break;
        case TrayAction::Exit:
            EngineController::RequestExit();
            break;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_APP_WAKE_MAIN) {
        EngineController::DrainMainThreadTasks();
        return 0;
    }

    if (msg == WM_APP_OPEN_SETTINGS) {
        EnsureEngineInitialized(false);
        SettingsUI::Open(GetModuleHandleW(nullptr));
        return 0;
    }

    if (msg == WM_APP_SET_RENDER_DEVICE) {
        if (!g_engine_initialized) {
            EnsureEngineInitialized(false);
        } else {
            ReinitRendererDevice(static_cast<int>(wParam) - 1);
        }
        return 0;
    }

    if (msg == g_injector.GetTaskbarRestartMsg()) {
        g_tray.Create(g_main_hwnd, OnTrayAction);
        if (g_engine_initialized && g_main_hwnd) {
            bool show_now = (g_frames_rendered > 0);
            g_injector.Reattach(g_main_hwnd, show_now);
            g_inject_ok = g_injector.IsAttached();
            g_render_window_shown = g_inject_ok && show_now;
            if (g_inject_ok) {
                RECT rc;
                if (GetClientRect(g_main_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
                    g_presenter.Resize(rc.right - rc.left, rc.bottom - rc.top);
                }
            }
        }
        return 0;
    }

    if (msg == WM_APP_ATTACH) {
        if (g_engine_initialized && g_main_hwnd) {
            bool show_now = (g_frames_rendered > 0);
            if (!g_injector.IsAttached() || !g_injector.IsAttachedValid()) {
                g_injector.Reattach(g_main_hwnd, show_now);
                g_inject_ok = g_injector.IsAttached();
                g_render_window_shown = g_inject_ok && show_now;
            } else if (show_now && !g_render_window_shown) {
                ShowWindow(g_main_hwnd, SW_SHOWNOACTIVATE);
                g_render_window_shown = true;
            }
            if (g_inject_ok) {
                RECT rc;
                if (GetClientRect(g_main_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
                    g_presenter.Resize(rc.right - rc.left, rc.bottom - rc.top);
                }
            }
        }
        return 0;
    }

    if (msg == WM_APP_DEATTACH) {
        if (g_main_hwnd) {
            g_injector.Detach();
            ShowWindow(g_main_hwnd, SW_HIDE);
            g_inject_ok = false;
            g_render_window_shown = false;
            g_frames_rendered = 0;
            HWND shell = GetShellWindow();
            if (shell) InvalidateRect(shell, nullptr, TRUE);
            InvalidateRect(GetDesktopWindow(), nullptr, TRUE);
            RedrawWindow(nullptr, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        }
        return 0;
    }

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_WTSSESSION_CHANGE:
            g_governor.HandleSessionChange(wParam);
            return 0;

        case WM_POWERBROADCAST:
            g_governor.HandlePowerChange(wParam);
            return 0;

        case WM_DISPLAYCHANGE: {
            s_displays_dirty = true;
            RECT rc = {};
            int w = 0, h = 0;
            HWND parent = g_injector.GetWorkerW();
            if (parent && GetClientRect(parent, &rc) && rc.right > 0 && rc.bottom > 0) {
                w = rc.right - rc.left;
                h = rc.bottom - rc.top;
            } else {
                w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            }
            if (g_engine_initialized && g_main_hwnd && w > 0 && h > 0) {
                UINT swpFlags = SWP_NOACTIVATE | (g_render_window_shown ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
                SetWindowPos(g_main_hwnd, HWND_BOTTOM, 0, 0, w, h, swpFlags);
                g_presenter.Resize(w, h);
            }
            Logger::Info("WM_DISPLAYCHANGE: adapted resolution to ", w, "x", h);
            return 0;
        }

        case WM_APP_TRAYICON:
            g_tray.HandleMessage(wParam, lParam);
            return 0;

        case WM_DESTROY:
            if (hwnd == g_main_hwnd) {
                Logger::Info("Main render window received WM_DESTROY from shell");
                g_main_hwnd = nullptr;
                g_inject_ok = false;
                g_render_window_shown = false;
            }
            if (!g_running) {
                PostQuitMessage(0);
            }
            return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
// Application Entry Point
// ============================================================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPWSTR lpCmdLine, int /*nCmdShow*/) {
    g_main_thread_id = GetCurrentThreadId();

    // PerMonitorV2 DPI awareness with layered OS fallbacks
    {
        bool dpiSet = false;
        if (HMODULE hU32 = GetModuleHandleW(L"user32.dll")) {
            using SetCtxFn = BOOL(WINAPI*)(HANDLE);
            if (auto fn = reinterpret_cast<SetCtxFn>(GetProcAddress(hU32, "SetProcessDpiAwarenessContext"))) {
                dpiSet = (fn(reinterpret_cast<HANDLE>(-4)) != FALSE) || (fn(reinterpret_cast<HANDLE>(-2)) != FALSE);
            } else {
                using LegacyFn = BOOL(WINAPI*)();
                if (auto legacy = reinterpret_cast<LegacyFn>(GetProcAddress(hU32, "SetProcessDPIAware")))
                    dpiSet = (legacy() != FALSE);
            }
        }
        if (!dpiSet) {
            if (HMODULE hShcore = LoadLibraryW(L"shcore.dll")) {
                using SetProcFn = HRESULT(WINAPI*)(int);
                if (auto fn = reinterpret_cast<SetProcFn>(GetProcAddress(hShcore, "SetProcessDpiAwareness")))
                    fn(2);
                FreeLibrary(hShcore);
            }
        }
    }

    // Ultra-low footprint mimalloc tuning (immediate arena decommit on free)
    mi_option_set(mi_option_arena_eager_commit, 0);
    mi_option_set(mi_option_page_commit_on_demand, 1);
    mi_option_set(mi_option_reserve_huge_os_pages, 0);
    mi_option_set(mi_option_purge_decommits, 1);
    mi_option_set(mi_option_purge_delay, 0);

    // Lock Current Working Directory to executable directory (Unicode safe)
    fs::path exeDirPath = GetExecutableDir();
    if (!exeDirPath.empty()) {
        SetCurrentDirectoryW(exeDirPath.c_str());
    }

    auto MatchArg = [](const wchar_t* cmd, const wchar_t* arg) -> bool {
        if (!cmd || !arg) return false;
        const wchar_t* p = cmd;
        size_t len = wcslen(arg);
        while ((p = wcsstr(p, arg)) != nullptr) {
            bool start_ok = (p == cmd || *(p - 1) == L' ' || *(p - 1) == L'\t');
            wchar_t next = *(p + len);
            bool end_ok = (next == L'\0' || next == L' ' || next == L'\t' || next == L':');
            if (start_ok && end_ok) return true;
            p += len;
        }
        return false;
    };

    // Handle Windows Screensaver & Diagnostic switches (/s, /c, /p, --memprobe)
    if (lpCmdLine) {
        if (MatchArg(lpCmdLine, L"/s") || MatchArg(lpCmdLine, L"/S") ||
            MatchArg(lpCmdLine, L"-s") || MatchArg(lpCmdLine, L"-S")) {
            return RunScreensaverMode(hInstance);
        }
        if (MatchArg(lpCmdLine, L"/c") || MatchArg(lpCmdLine, L"/C") ||
            MatchArg(lpCmdLine, L"-c") || MatchArg(lpCmdLine, L"-C")) {
            IpcClient client;
            std::string resp = client.SendRequest("{\"cmd\":\"open_settings\"}");
            if (!resp.empty()) {
                return 0;
            }
            g_config.Load();
            SettingsUI::Open(hInstance);
            MSG msg = {};
            while (SettingsUI::IsOpen()) {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) break;
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                if (msg.message == WM_QUIT || !SettingsUI::IsOpen()) break;
                SettingsUI::RenderFrame();
                MsgWaitForMultipleObjects(0, nullptr, FALSE, SettingsUI::GetPreferredWaitMs(), QS_ALLINPUT);
            }
            g_config.FlushIfDirty(true);
            SettingsUI::Shutdown();
            return 0;
        }
        if (MatchArg(lpCmdLine, L"/p") || MatchArg(lpCmdLine, L"/P") ||
            MatchArg(lpCmdLine, L"-p") || MatchArg(lpCmdLine, L"-P")) {
            return 0;
        }

        if (MatchArg(lpCmdLine, L"--memprobe") || MatchArg(lpCmdLine, L"-memprobe")) {
            int probe_seconds = 5;
            const wchar_t* ps = wcsstr(lpCmdLine, L"--memprobe");
            if (!ps) ps = wcsstr(lpCmdLine, L"-memprobe");
            if (ps) {
                ps += (wcsstr(ps, L"--memprobe") == ps) ? 10 : 9;
                while (*ps == L' ' || *ps == L'=' || *ps == L':') ps++;
                if (*ps >= L'0' && *ps <= L'9') probe_seconds = _wtoi(ps);
            }
            probe_seconds = std::clamp(probe_seconds, 2, 60);

            auto pacer = std::make_shared<FramePacerTimer>();
            MemProbeHooks hooks;
            hooks.init_engine = [pacer](HWND hwnd, int vw, int vh, int gpu_index) -> bool {
                if (!g_presenter.Init(hwnd, vw, vh, gpu_index)) return false;
                g_engine_initialized = true;
                g_shared_engine_state.active_gpu_index.store(gpu_index);
                g_shared_engine_state.SetActiveGpuInfo(g_presenter.GetAdapterName(), g_presenter.GetAdapterType());
                g_governor.Init(hwnd);
                g_ipc.Start(OnIpcRequest);
                auto& cfg = g_config.Get();
                if (!cfg.wallpapers.empty() && cfg.wallpapers[0].audio_enabled && cfg.wallpapers[0].volume > 0.001f) {
                    g_audio.SetVolume(cfg.wallpapers[0].volume);
                    g_audio.SetMuted(false);
                    g_audio.Init();
                }
                pacer->SetActivePlayback(true);
                return true;
            };
            hooks.open_video = [](const std::string& video_path) {
                return OpenWallpaperVideo(video_path);
            };
            hooks.get_video_stats = []() {
                std::lock_guard<std::mutex> lock(g_decoder_mutex);
                auto info = g_decoder.GetInfo();
                return MemProbeVideoStats{
                    info.width,
                    info.height,
                    info.fps,
                    info.duration_seconds,
                    g_current_frame.pts,
                    g_decoder_hw
                };
            };
            hooks.get_adapter_summary = []() {
                return g_presenter.GetAdapterName() +
                       (g_presenter.IsSoftwareMode() ? "  [SOFTWARE MODE]" : "  [hardware decode]");
            };
            hooks.render_one_frame_now = []() {
                std::lock_guard<std::mutex> lock(g_decoder_mutex);
                if (g_decoder.DecodeNextFrame(g_current_frame) && g_current_frame.texture) {
                    if (HANDLE w = g_presenter.GetWaitableObject()) WaitForSingleObject(w, 0);
                    auto& cfg = g_config.Get();
                    UpdateTargetViewports(cfg.target_displays);
                    g_presenter.RenderFrame(
                        g_current_frame.texture,
                        g_current_frame.texture_index,
                        cfg.scaling_mode,
                        s_cached_target_vps,
                        0.0f,
                        g_current_frame.width,
                        g_current_frame.height
                    );
                    g_presenter.Present(0);
                    g_frames_rendered++;
                    if (!g_initial_frame_trimmed && g_frames_rendered >= 2) {
                        g_initial_frame_trimmed = true;
                        g_trimmer.ScheduleSettledTrim(g_clock.GetCurrentTimeMicros(), 350000ULL);
                    }
                } else {
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, 8, QS_ALLINPUT);
                }
            };
            hooks.tick_wallpaper = [pacer, &hooks](bool ui_open) {
                if (g_deep_suspended) {
                    pacer->SetActivePlayback(false);
                    pacer->WaitMicros(100000ULL, ui_open);
                    return;
                }
                pacer->SetActivePlayback(true);
                if (g_clock.ShouldRenderFrame()) {
                    hooks.render_one_frame_now();
                } else {
                    pacer->WaitMicros(g_clock.GetSleepDurationMicros(), ui_open);
                }
            };
            hooks.get_frames_rendered = []() { return g_frames_rendered; };
            hooks.enter_deep_suspend = [pacer]() {
                pacer->SetActivePlayback(false);
                EnterDeepSuspend();
            };
            hooks.leave_deep_suspend = [pacer]() {
                LeaveDeepSuspend();
                pacer->SetActivePlayback(true);
            };
            hooks.trim_working_set = []() {
                g_trimmer.TrimNow(g_clock.GetCurrentTimeMicros());
            };
            hooks.shutdown_engine = [pacer]() {
                pacer->SetActivePlayback(false);
                SettingsUI::Shutdown();
                g_ipc.Stop();
                g_audio.Stop();
                {
                    std::lock_guard<std::mutex> lock(g_decoder_mutex);
                    g_decoder.Close();
                }
                g_presenter.Cleanup();
                g_governor.Shutdown();
                g_engine_initialized = false;
            };

            return RunMemoryProbe(hInstance, probe_seconds, hooks);
        }
    }

    bool is_silent_boot = (lpCmdLine && (wcsstr(lpCmdLine, L"--startup") != nullptr || wcsstr(lpCmdLine, L"-startup") != nullptr));
    bool show_detail_on_open = (lpCmdLine && (MatchArg(lpCmdLine, L"--detail") || MatchArg(lpCmdLine, L"-detail")));

    int targetPage = -1;
    if (lpCmdLine) {
        const wchar_t* p = wcsstr(lpCmdLine, L"--page");
        if (!p) p = wcsstr(lpCmdLine, L"-page");
        if (p) {
            p += (wcsstr(p, L"--page") == p) ? 6 : 5;
            while (*p == L' ' || *p == L'=' || *p == L':') p++;
            if (*p >= L'0' && *p <= L'9') targetPage = _wtoi(p);
        }
    }
    if (targetPage >= 0) {
        SettingsUI::SetActivePage(targetPage);
    }
    if (show_detail_on_open) {
        SettingsUI::SetGalleryDetailVisible(true, 0);
    }

    std::string screenshotPath;
    if (lpCmdLine) {
        const wchar_t* p = wcsstr(lpCmdLine, L"--screenshot");
        if (!p) p = wcsstr(lpCmdLine, L"-screenshot");
        if (p) {
            p += (wcsstr(p, L"--screenshot") == p) ? 12 : 11;
            while (*p == L' ' || *p == L'=' || *p == L':') p++;
            bool inQuotes = (*p == L'"');
            if (inQuotes) p++;
            std::wstring wsc;
            while (*p && (inQuotes ? (*p != L'"') : (*p != L' '))) { wsc += *p; p++; }
            screenshotPath = WideToUtf8(wsc);
        }
    }

    // Single-Instance Check
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"MiniWallpaper_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!screenshotPath.empty()) {
            IpcClient client;
            nlohmann::json r = {{"cmd", "capture_ui"}, {"path", screenshotPath}};
            if (targetPage >= 0) r["page"] = targetPage;
            if (show_detail_on_open) r["detail"] = true;
            client.SendRequest(r.dump());
            if (hMutex) CloseHandle(hMutex);
            return 0;
        }
        if (!is_silent_boot) {
            IpcClient client;
            nlohmann::json r = {{"cmd", "open_settings"}};
            if (targetPage >= 0) r["page"] = targetPage;
            if (show_detail_on_open) r["detail"] = true;
            client.SendRequest(r.dump());
        }
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    // Standalone Screenshot Capture Mode
    if (!screenshotPath.empty()) {
        int customW = 0, customH = 0;
        if (lpCmdLine) {
            const wchar_t* pw = wcsstr(lpCmdLine, L"--width");
            if (pw) {
                pw += 7;
                while (*pw == L' ' || *pw == L'=' || *pw == L':') pw++;
                customW = _wtoi(pw);
            }
            const wchar_t* ph = wcsstr(lpCmdLine, L"--height");
            if (ph) {
                ph += 8;
                while (*ph == L' ' || *ph == L'=' || *ph == L':') ph++;
                customH = _wtoi(ph);
            }
        }

        g_config.Load();
        if (targetPage >= 0) SettingsUI::SetActivePage(targetPage);
        if (show_detail_on_open) SettingsUI::SetGalleryDetailVisible(true, 0);
        SettingsUI::Open(hInstance);
        if (customW > 0 && customH > 0 && SettingsUI::GetHwnd()) {
            SetWindowPos(SettingsUI::GetHwnd(), nullptr, 0, 0, customW, customH, SWP_NOMOVE | SWP_NOZORDER);
        }
        for (int f = 0; f < 30; ++f) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            SettingsUI::RenderFrame();
            Sleep(16);
        }
        SettingsUI::SaveScreenshot(screenshotPath);
        SettingsUI::Shutdown();
        if (hMutex) {
            ReleaseMutex(hMutex);
            CloseHandle(hMutex);
        }
        return 0;
    }

    // One-time v1 -> v2 rebranding migration (fast-pathed by .migrated_v2 sentinel)
    LegacyMigrator::RunOnce();
    WindowsAutostart::MigrateLegacy();

    // 1. Load Configuration
    g_config.Load();
    auto& cfg = g_config.Get();

    if (cfg.run_on_startup) {
        WindowsAutostart::AutoRepairIfMoved(cfg.startup_priority);
    }

    // 2. Register Background Render Window Class (LR_SHARED avoids HICON leaks)
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    HICON hIconBig = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR | LR_SHARED
    ));
    HICON hIconSm = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED
    ));
    if (!hIconBig) hIconBig = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    if (!hIconSm) hIconSm = hIconBig;

    wc.hIcon = hIconBig;
    wc.hIconSm = hIconSm;
    wc.lpszClassName = L"MiniWallpaper_Daemon";
    RegisterClassExW(&wc);

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    g_main_hwnd = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"MiniWallpaper_Daemon",
        L"MiniWallpaper",
        WS_POPUP,
        vx, vy, vw, vh,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_main_hwnd) {
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 1;
    }

    SendMessageW(g_main_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIconBig));
    SendMessageW(g_main_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIconSm));

    // 3. Setup Power Governor, System Tray, and External IPC Server FIRST (<15 ms, 0 FFmpeg/D3D11 DLLs)
    g_governor.Init(g_main_hwnd);
    g_tray.Create(g_main_hwnd, OnTrayAction);
    g_ipc.Start(OnIpcRequest);
    g_shared_engine_state.connected.store(true);

    // 4. Manual launch initializes engine & UI immediately (0 ms delay);
    //    Silent Windows boot (--startup) defers heavy D3D11/FFmpeg DLL loading via BootReadinessGate
    //    while Windows' native static wallpaper stays visible at t=0s.
    if (!is_silent_boot) {
        if (!EnsureEngineInitialized(false)) {
            g_ipc.Stop();
            g_tray.Destroy();
            g_governor.Shutdown();
            if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
            return 1;
        }
        SettingsUI::Open(hInstance);
    } else {
        Logger::Info("Silent startup mode (--startup): tray & IPC live, deferring D3D11/FFmpeg init via BootReadinessGate");
    }

    FramePacerTimer frame_pacer;
    BootReadinessGate boot_gate(g_clock.GetCurrentTimeMicros());
    g_trimmer.TrimNow(g_clock.GetCurrentTimeMicros());

    // 5. Main Event & Render Loop
    MSG msg = {};
    std::array<float, 1024 * 2> audio_buffer;
    uint64_t last_state_update_us = 0;
    uint64_t last_ui_render_us = 0;
    uint64_t last_inject_check_us = 0;
    bool was_ui_open = SettingsUI::IsOpen();

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

        EngineController::DrainMainThreadTasks();
        g_config.FlushIfDirty(false);

        uint64_t now_us = g_clock.GetCurrentTimeMicros();
        bool ui_open = SettingsUI::IsOpen();
        if (was_ui_open && !ui_open) {
            g_trimmer.NotifyUiClosed(now_us);
        }
        was_ui_open = ui_open;

        // Render Settings UI with adaptive frame pacing (60 FPS active / 30 FPS Perf / 15 FPS idle / 10 FPS unfocused / 0 FPS minimized)
        if (ui_open) {
            HWND uiHwnd = SettingsUI::GetHwnd();
            if (uiHwnd && !IsIconic(uiHwnd)) {
                uint64_t ui_interval_us = static_cast<uint64_t>(SettingsUI::GetPreferredWaitMs()) * 1000ULL;
                if (now_us - last_ui_render_us >= ui_interval_us) {
                    last_ui_render_us = now_us;
                    SettingsUI::RenderFrame();
                    ui_open = SettingsUI::IsOpen();
                }
            }
        }

        // Recreate render window if Explorer destroyed it
        if (!g_main_hwnd && g_running) {
            vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            g_main_hwnd = CreateWindowExW(
                WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                L"MiniWallpaper_Daemon",
                L"MiniWallpaper",
                WS_POPUP,
                vx, vy, vw, vh,
                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr
            );
            g_inject_ok = false;
            g_render_window_shown = false;
            if (g_main_hwnd && g_engine_initialized) {
                bool ui_was_open = SettingsUI::IsOpen();
                SettingsUI::Shutdown();
                g_presenter.Init(g_main_hwnd, vw, vh, cfg.gpu_device_index);
                if (ui_was_open) SettingsUI::Open(GetModuleHandleW(nullptr));
                ui_open = SettingsUI::IsOpen();
            }
        }

        // Deferred engine initialization gate for --startup
        if (!g_engine_initialized) {
            if (ui_open) {
                EnsureEngineInitialized(false);
            } else {
                PowerState boot_power = g_governor.GetCurrentState(false, false, 80, 80, nullptr);
                if (boot_power == PowerState::Sleeping && cfg.pause_on_lock) {
                    g_trimmer.Tick(now_us, false);
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, 500, QS_ALLINPUT);
                    continue;
                }
                if (boot_gate.PollReady(now_us)) {
                    Logger::Info("BootReadinessGate passed — initializing D3D11 & FFmpeg engine");
                    EnsureEngineInitialized(true);
                } else {
                    g_trimmer.Tick(now_us, false);
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
                    continue;
                }
            }
            if (!g_engine_initialized) {
                DWORD waitMs = ui_open ? SettingsUI::GetPreferredWaitMs() : 500;
                MsgWaitForMultipleObjects(0, nullptr, FALSE, waitMs, QS_ALLINPUT);
                continue;
            }
            now_us = g_clock.GetCurrentTimeMicros();
        }

        bool has_active_wallpaper = (!cfg.wallpapers.empty() && !cfg.wallpapers[0].video_path.empty() && !g_paused);

        if (has_active_wallpaper && g_main_hwnd && (now_us - last_inject_check_us >= 1000000ULL)) {
            last_inject_check_us = now_us;
            bool show_now = (g_frames_rendered > 0);

            if (!g_inject_ok) {
                g_inject_ok = g_injector.Attach(g_main_hwnd, show_now);
                g_render_window_shown = g_inject_ok && show_now;
                if (g_inject_ok) {
                    Logger::Info("Injection retry succeeded, workerw=0x",
                                 reinterpret_cast<size_t>(g_injector.GetWorkerW()));
                    g_last_error.clear();
                    RECT rc;
                    if (GetClientRect(g_main_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
                        g_presenter.Resize(rc.right - rc.left, rc.bottom - rc.top);
                    }
                }
            } else if (!g_injector.IsAttachedValid()) {
                Logger::Info("Desktop hierarchy changed, re-attaching wallpaper to active WorkerW");
                g_injector.Reattach(g_main_hwnd, show_now);
                g_inject_ok = g_injector.IsAttached();
                g_render_window_shown = g_inject_ok && show_now;
                if (g_inject_ok && g_main_hwnd) {
                    RECT rc;
                    if (GetClientRect(g_main_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
                        g_presenter.Resize(rc.right - rc.left, rc.bottom - rc.top);
                    }
                }
            }
        }

        PowerState power = g_governor.GetCurrentState(
            cfg.pause_on_maximized,
            cfg.pause_on_resource_heavy,
            cfg.resource_ram_threshold_pct,
            cfg.resource_vram_threshold_pct,
            g_presenter.GetDevice()
        );

        // Update Shared Engine State (4 Hz while UI is open, 1 Hz when hidden)
        uint64_t state_interval_us = ui_open ? 250000ULL : 1000000ULL;
        if (now_us - last_state_update_us >= state_interval_us) {
            last_state_update_us = now_us;
            std::string cur_vid = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : "";
            VideoInfo info;
            {
                std::lock_guard<std::mutex> lock(g_decoder_mutex);
                info = g_decoder.GetInfo();
                if (info.duration_seconds > 0.0 || info.width > 0) {
                    g_cached_video_info = info;
                } else {
                    info = g_cached_video_info;
                }
            }
            PROCESS_MEMORY_COUNTERS_EX pmc = {};
            size_t ram = 0, ram_priv = 0;
            if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
                ram = pmc.WorkingSetSize / (1024 * 1024);
                ram_priv = pmc.PrivateUsage / (1024 * 1024);
            }
            double cpu = GetProcessCpuUsagePercent();
            size_t vram = g_presenter.GetVramUsageMB();
            if (vram == 0 && g_decoder_hw && !g_presenter.IsSoftwareMode() && !g_deep_suspended) {
                vram = EstimateProcessVramUsageMB(info.width, info.height, vw, vh, g_decoder_hw);
            }
            if (g_presenter.IsSoftwareMode()) {
                vram = 0;
            }

            g_shared_engine_state.connected.store(true);
            g_shared_engine_state.playing.store(!g_paused && !cur_vid.empty() && !g_fullscreen_paused && g_inject_ok);
            g_shared_engine_state.paused.store(g_paused && !cur_vid.empty());
            g_shared_engine_state.injected.store(g_inject_ok);
            g_shared_engine_state.hw_decode.store(g_decoder_hw);
            g_shared_engine_state.fps.store(g_clock.GetTargetFPS());
            g_shared_engine_state.video_fps.store(info.fps);
            g_shared_engine_state.width.store(info.width);
            g_shared_engine_state.height.store(info.height);
            g_shared_engine_state.duration.store(info.duration_seconds);
            g_shared_engine_state.ram_mb.store(ram);
            g_shared_engine_state.ram_private_mb.store(ram_priv);
            g_shared_engine_state.vram_mb.store(vram);
            g_shared_engine_state.cpu_percent.store(cpu);
            g_shared_engine_state.system_ram_percent.store(g_governor.GetSystemRamPercent());
            g_shared_engine_state.gpu_vram_percent.store(g_governor.GetGpuVramPercent());
            g_shared_engine_state.resource_heavy_sleep.store(g_governor.IsInResourceSleep());
            g_shared_engine_state.frames_rendered.store(g_frames_rendered);
            g_shared_engine_state.frames_decoded.store(g_frames_decoded);
            g_shared_engine_state.frame_skip.store(g_frame_skip);
            g_shared_engine_state.SetCurrentVideo(cur_vid);
            g_shared_engine_state.SetCodec(info.codec_name);
            g_shared_engine_state.SetLastError(g_last_error);
            g_shared_engine_state.SetActiveGpuInfo(g_presenter.GetAdapterName(), g_presenter.GetAdapterType());
        }

        // Smart Working-Set Trimmer (zero page-fault thrashing during steady playback)
        g_trimmer.Tick(now_us, ui_open);

        // Manual pause deep-suspend transition after 5s grace period
        if (g_paused && !g_fullscreen_paused) {
            if (g_pause_start_us == 0) g_pause_start_us = now_us;
            if (!g_deep_suspended && (now_us - g_pause_start_us >= 5000000ULL)) {
                EnterDeepSuspend();
            }
        } else if (!g_paused && !g_fullscreen_paused && g_deep_suspended) {
            LeaveDeepSuspend();
        }

        // Idle wait when no active wallpaper is playing
        if (!has_active_wallpaper) {
            frame_pacer.SetActivePlayback(false);
            DWORD idleWaitMs = ui_open ? SettingsUI::GetPreferredWaitMs() : 250;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, idleWaitMs, QS_ALLINPUT);
            continue;
        }

        // Guard against rendering to a detached top-level window
        if (!g_inject_ok) {
            frame_pacer.SetActivePlayback(false);
            DWORD idleWaitMs = ui_open ? SettingsUI::GetPreferredWaitMs() : 150;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, idleWaitMs, QS_ALLINPUT);
            continue;
        }

        bool fullscreen_active = (power == PowerState::Paused && cfg.pause_on_fullscreen);
        bool occluded_active = (power == PowerState::Occluded && cfg.pause_on_maximized);
        bool battery_pause_active = (power == PowerState::Reduced && cfg.pause_on_battery);
        bool lock_pause_active = (power == PowerState::Sleeping && cfg.pause_on_lock);
        bool resource_pause_active = (power == PowerState::ResourceHeavy && cfg.pause_on_resource_heavy);

        if (fullscreen_active || occluded_active || battery_pause_active || lock_pause_active || resource_pause_active) {
            frame_pacer.SetActivePlayback(false);
            if (!g_fullscreen_paused) {
                std::string detail = g_governor.GetLastTriggerInfo();
                if (resource_pause_active) {
                    SuspendWallpaper("High system load (Gaming Auto-Sleep)", detail);
                } else if (occluded_active) {
                    SuspendWallpaper("Maximized window / desktop covered", detail);
                } else if (fullscreen_active) {
                    SuspendWallpaper("Fullscreen app / 3D game", detail);
                } else if (lock_pause_active) {
                    SuspendWallpaper("Workstation locked", detail);
                } else {
                    SuspendWallpaper("Battery saver", detail);
                }
            }

            bool trigger_deep_suspend = resource_pause_active || lock_pause_active ||
                                        (g_pause_start_us > 0 && (now_us - g_pause_start_us >= 5000000ULL));
            if (!g_deep_suspended && trigger_deep_suspend) {
                EnterDeepSuspend();
            }

            // True 0.00% CPU infinite sleep when workstation is locked and deep-suspended
            if (lock_pause_active && g_deep_suspended && !ui_open && !g_config.IsDirty()) {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, INFINITE, QS_ALLINPUT);
                continue;
            }

            DWORD idleWaitMs = ui_open ? SettingsUI::GetPreferredWaitMs() : 250;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, idleWaitMs, QS_ALLINPUT);
            continue;
        } else if (g_fullscreen_paused) {
            ResumeWallpaper();
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

        bool should_pause = (power == PowerState::Sleeping) ||
                            (power == PowerState::Reduced && cfg.pause_on_battery) ||
                            g_paused;

        if (should_pause) {
            frame_pacer.SetActivePlayback(false);
            if (g_pause_start_us == 0) {
                g_pause_start_us = now_us;
            }
            if (!g_deep_suspended && (now_us - g_pause_start_us >= 5000000ULL)) {
                EnterDeepSuspend();
            }
            DWORD idleWaitMs = ui_open ? SettingsUI::GetPreferredWaitMs() : 250;
            MsgWaitForMultipleObjects(0, nullptr, FALSE, idleWaitMs, QS_ALLINPUT);
            continue;
        } else if (!g_fullscreen_paused && g_deep_suspended) {
            LeaveDeepSuspend();
        }

        frame_pacer.SetActivePlayback(true);
        int display_fps_cap = (power == PowerState::Reduced) ? cfg.battery_fps : cfg.target_fps;
        g_frame_skip = ComputeFrameSkip(g_video_fps, display_fps_cap);

        if (g_clock.ShouldRenderFrame()) {
            if (static_cast<int64_t>(g_clock.GetCurrentTimeMicros()) < g_flash_until_us) {
                if (HANDLE w = g_presenter.GetWaitableObject()) WaitForSingleObject(w, 0);
                if (!g_render_window_shown && g_inject_ok && g_main_hwnd) {
                    ShowWindow(g_main_hwnd, SW_SHOWNOACTIVATE);
                    g_render_window_shown = true;
                }
                g_presenter.ClearAndPresent(0.22f, 0.91f, 1.00f);
                g_frames_rendered++;
                frame_pacer.WaitMicros(g_clock.GetSleepDurationMicros(), ui_open);
                continue;
            }

            std::lock_guard<std::mutex> lock(g_decoder_mutex);
            bool decode_ok = false;
            int frames_to_decode = (g_frame_skip > 0) ? g_frame_skip : 1;
            for (int step = 0; step < frames_to_decode; ++step) {
                if (g_decoder.DecodeNextFrame(g_current_frame)) {
                    g_frames_decoded++;
                    decode_ok = true;
                } else {
                    break;
                }
            }

            if (decode_ok) {
                if (g_current_frame.texture) {
                    if (cfg.auto_smooth_loop) {
                        if (!g_first_frame_captured) {
                            g_presenter.CaptureStartFrame(g_current_frame.texture, g_current_frame.texture_index);
                            g_first_frame_captured = true;
                        }
                    } else if (g_first_frame_captured) {
                        g_presenter.ResetStartFrame();
                        g_first_frame_captured = false;
                    }

                    auto info = g_decoder.GetInfo();
                    double current_time_sec = 0.0;
                    if (info.fps > 0.0 && info.duration_seconds > 0.0) {
                        double total_frames = info.duration_seconds * info.fps;
                        if (total_frames > 0.0) {
                            double current_frame_pos = static_cast<double>(g_frames_decoded % static_cast<uint64_t>(total_frames + 0.5));
                            current_time_sec = current_frame_pos / info.fps;
                            g_shared_engine_state.current_time_sec.store(current_time_sec);
                        }
                    }

                    // Calculate Auto Smooth Loop crossfade blend alpha & dynamic speed ramp
                    float blend_alpha = 0.0f;
                    if (cfg.auto_smooth_loop && info.duration_seconds > 1.5 && info.fps > 0.0) {
                        float fade_dur = 0.8f;
                        bool enable_speed_ramp = false;
                        float min_speed = 0.75f;
                        int easing_mode = 1;

                        if (cfg.loop_preset == 0) {
                            fade_dur = 1.2f;
                            enable_speed_ramp = true;
                            min_speed = 0.75f;
                            easing_mode = 1;
                        } else if (cfg.loop_preset == 1) {
                            fade_dur = 0.8f;
                            enable_speed_ramp = false;
                            easing_mode = 1;
                        } else if (cfg.loop_preset == 2) {
                            fade_dur = 1.8f;
                            enable_speed_ramp = true;
                            min_speed = 0.85f;
                            easing_mode = 2;
                        } else if (cfg.loop_preset == 3) {
                            fade_dur = 0.4f;
                            enable_speed_ramp = false;
                            easing_mode = 0;
                        } else {
                            fade_dur = std::clamp(cfg.smooth_loop_duration, 0.2f, 2.5f);
                            enable_speed_ramp = cfg.loop_speed_ramp;
                            min_speed = std::clamp(cfg.loop_min_speed, 0.5f, 0.95f);
                            easing_mode = cfg.loop_easing_curve;
                        }

                        double time_remaining = info.duration_seconds - current_time_sec;
                        if (time_remaining >= 0.0 && time_remaining <= fade_dur) {
                            float raw_alpha = static_cast<float>(1.0 - (time_remaining / fade_dur));
                            if (easing_mode == 1) {
                                blend_alpha = raw_alpha * raw_alpha * (3.0f - 2.0f * raw_alpha);
                            } else if (easing_mode == 2) {
                                blend_alpha = 0.5f * (1.0f - cosf(raw_alpha * 3.14159265f));
                            } else if (easing_mode == 3) {
                                blend_alpha = raw_alpha * raw_alpha * raw_alpha * (raw_alpha * (raw_alpha * 6.0f - 15.0f) + 10.0f);
                            } else {
                                blend_alpha = raw_alpha;
                            }

                            if (enable_speed_ramp) {
                                float ramp_factor = sinf(raw_alpha * 3.14159265f);
                                double target_speed = 1.0 - (1.0 - static_cast<double>(min_speed)) * ramp_factor;
                                g_clock.SetSpeedMultiplier(target_speed);
                            } else {
                                g_clock.SetSpeedMultiplier(1.0);
                            }
                        } else {
                            g_clock.SetSpeedMultiplier(1.0);
                        }
                    } else {
                        g_clock.SetSpeedMultiplier(1.0);
                    }

                    if (HANDLE w = g_presenter.GetWaitableObject()) WaitForSingleObject(w, 0);
                    UpdateTargetViewports(cfg.target_displays);
                    g_presenter.RenderFrame(
                        g_current_frame.texture,
                        g_current_frame.texture_index,
                        cfg.scaling_mode,
                        s_cached_target_vps,
                        blend_alpha,
                        g_current_frame.width,
                        g_current_frame.height
                    );

                    if (!g_render_window_shown && g_inject_ok && g_main_hwnd) {
                        ShowWindow(g_main_hwnd, SW_SHOWNOACTIVATE);
                        g_render_window_shown = true;
                    }
                    if (FAILED(g_presenter.Present(0))) {
                        if (g_last_error.empty()) {
                            g_last_error = "Present() failed (DXGI error)";
                            Logger::Info(g_last_error);
                        }
                    }
                    g_frames_rendered++;
                    if (!g_initial_frame_trimmed && g_frames_rendered >= 2) {
                        g_initial_frame_trimmed = true;
                        g_trimmer.ScheduleSettledTrim(g_clock.GetCurrentTimeMicros(), 350000ULL);
                    }
                } else if (g_last_error.empty()) {
                    g_last_error = "Decoded frame has no GPU texture (HW decode inactive)";
                    Logger::Info(g_last_error);
                }

                if (!g_audio.IsMuted() && g_decoder.HasAudio()) {
                    int samples = 0;
                    const int max_frames = static_cast<int>(audio_buffer.size() / 2);
                    while ((samples = g_decoder.DecodeAudioSamples(audio_buffer.data(), max_frames)) > 0) {
                        g_audio.PushSamples(audio_buffer.data(), samples);
                    }
                }
            } else {
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 8, QS_ALLINPUT);
            }
        } else {
            frame_pacer.WaitMicros(g_clock.GetSleepDurationMicros(), ui_open);
        }
    }

    // Clean Shutdown
    frame_pacer.SetActivePlayback(false);
    g_config.FlushIfDirty(true);
    SettingsUI::Shutdown();
    g_video_optimizer.Cancel();
    g_ipc.Stop();
    g_tray.Destroy();
    g_audio.Stop();
    if (g_engine_initialized) {
        std::lock_guard<std::mutex> lock(g_decoder_mutex);
        g_decoder.Close();
    }
    g_presenter.Cleanup();
    g_governor.Shutdown();
    g_injector.Detach();
    if (g_main_hwnd) {
        DestroyWindow(g_main_hwnd);
        g_main_hwnd = nullptr;
    }
    g_engine_initialized = false;

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    return 0;
}
