#pragma once
#include <cstdint>

namespace litewp {

// Adaptive frame-rate clock calculating when the next frame should be rendered
class PlaybackClock {
public:
    PlaybackClock();

    void SetTargetFPS(int fps);     // Set target frame rate
    int  GetTargetFPS() const;
    
    // Call every iteration in main loop.
    // Returns true if it's time to render a new frame.
    // Returns false if not yet time.
    bool ShouldRenderFrame();
    
    // Get milliseconds to sleep until next frame
    uint32_t GetSleepDurationMs() const;
    
    // Reset clock (called on resume from pause)
    void Reset();

private:
    int m_target_fps = 30;
    int64_t m_frame_interval_us = 33333; // microseconds per frame (1/30s)
    int64_t m_last_frame_time = 0;       // timestamp in microseconds
    
    static int64_t GetCurrentTimeMicros();
};

} // namespace litewp
