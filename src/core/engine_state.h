#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

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

    char current_video[512] = {};
    char codec[64] = {};
    char last_error[256] = {};

    void SetCurrentVideo(const std::string& path) {
        strncpy_s(current_video, path.c_str(), sizeof(current_video) - 1);
    }
    void SetCodec(const std::string& c) {
        strncpy_s(codec, c.c_str(), sizeof(codec) - 1);
    }
    void SetLastError(const std::string& err) {
        strncpy_s(last_error, err.c_str(), sizeof(last_error) - 1);
    }
};

extern EngineState g_shared_engine_state;

} // namespace litewp
