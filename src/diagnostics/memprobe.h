#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>

namespace miniwp {

struct MemProbeVideoStats {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    double duration_seconds = 0.0;
    int64_t current_pts = 0;
    bool hw_accelerated = false;
};

struct MemProbeHooks {
    std::function<bool(HWND hwnd, int vw, int vh, int gpu_index)> init_engine;
    std::function<bool(const std::string& video_path)> open_video;
    std::function<MemProbeVideoStats()> get_video_stats;
    std::function<std::string()> get_adapter_summary;
    std::function<void(bool ui_open)> tick_wallpaper;
    std::function<void()> render_one_frame_now;
    std::function<uint64_t()> get_frames_rendered;
    std::function<void()> enter_deep_suspend;
    std::function<void()> leave_deep_suspend;
    std::function<void()> trim_working_set;
    std::function<void()> shutdown_engine;
};

// Development diagnostic benchmark: `MiniWallpaper.exe --memprobe [seconds]`
// Boots the real engine without desktop WorkerW injection and writes `memprobe.txt`
// next to the executable across 6 lifecycle phases.
int RunMemoryProbe(HINSTANCE hInstance, int seconds_per_phase, const MemProbeHooks& hooks);

} // namespace miniwp
