#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <mutex>

namespace litewp {

struct EngineState {
    std::atomic<bool>     connected{true};
    std::atomic<bool>     playing{false};
    std::atomic<bool>     paused{false};
    std::atomic<bool>     injected{false};
    std::atomic<bool>     hw_decode{false};
    std::atomic<int>      fps{30};
    std::atomic<double>   video_fps{0.0};
    std::atomic<int>      width{0};
    std::atomic<int>      height{0};
    std::atomic<double>   duration{0.0};
    std::atomic<size_t>   ram_mb{0};
    std::atomic<size_t>   vram_mb{0};
    std::atomic<double>   cpu_percent{0.0};
    std::atomic<uint64_t> frames_rendered{0};
    std::atomic<uint64_t> frames_decoded{0};
    std::atomic<int>      frame_skip{1};
    std::atomic<int>      active_gpu_index{0}; // -1 = CPU (Software), 0 = GPU 1, 1 = GPU 2, etc.
    std::atomic<int>      system_ram_percent{0};
    std::atomic<int>      gpu_vram_percent{0};
    std::atomic<bool>     resource_heavy_sleep{false};

    // Thread-safe string fields (protected by str_mutex)
    mutable std::mutex str_mutex;
    char current_video[512] = {};
    char codec[64] = {};
    char last_error[256] = {};
    char active_renderer_name[128] = {};

    void SetCurrentVideo(const std::string& path) {
        std::lock_guard<std::mutex> lock(str_mutex);
        strncpy_s(current_video, sizeof(current_video), path.c_str(), _TRUNCATE);
    }
    void SetCodec(const std::string& c) {
        std::lock_guard<std::mutex> lock(str_mutex);
        strncpy_s(codec, sizeof(codec), c.c_str(), _TRUNCATE);
    }
    void SetLastError(const std::string& err) {
        std::lock_guard<std::mutex> lock(str_mutex);
        strncpy_s(last_error, sizeof(last_error), err.c_str(), _TRUNCATE);
    }
    void SetActiveRendererName(const std::string& name) {
        std::lock_guard<std::mutex> lock(str_mutex);
        strncpy_s(active_renderer_name, sizeof(active_renderer_name), name.c_str(), _TRUNCATE);
    }
    std::string GetCurrentVideo() const {
        std::lock_guard<std::mutex> lock(str_mutex);
        return std::string(current_video);
    }
    std::string GetCodec() const {
        std::lock_guard<std::mutex> lock(str_mutex);
        return std::string(codec);
    }
    std::string GetLastError() const {
        std::lock_guard<std::mutex> lock(str_mutex);
        return std::string(last_error);
    }
    std::string GetActiveRendererName() const {
        std::lock_guard<std::mutex> lock(str_mutex);
        return std::string(active_renderer_name);
    }
};

extern EngineState g_shared_engine_state;

} // namespace litewp
