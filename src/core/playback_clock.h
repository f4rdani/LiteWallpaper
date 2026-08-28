#pragma once
#include <cstdint>

namespace litewp {

// Clock that paces playback at the VIDEO's native frame rate (so playback is
// always real-time / 1x speed), while the target display FPS is handled as a
// frame-skip factor by the caller.
class PlaybackClock {
public:
    PlaybackClock();

    // Set the base pacing rate = video native fps. Playback always advances at
    // this rate regardless of the display target FPS.
    // Set base frame rate (video fps)
    void SetTargetFPS(int fps);
    int  GetTargetFPS() const;

    // Set dynamic speed multiplier (1.0 = normal, 0.75 = 25% slower for speed ramping)
    void SetSpeedMultiplier(double speed);
    double GetSpeedMultiplier() const;
    
    // Call every iteration in main loop.
    // Returns true if it's time to decode+display the next video frame.
    // Returns false if not yet time.
    bool ShouldRenderFrame();
    
    // Get milliseconds to sleep until next frame
    uint32_t GetSleepDurationMs() const;
    
    // Reset clock (called on resume from pause / when video changes)
    void Reset();

    // Current monotonic time in microseconds (QueryPerformanceCounter based)
    static int64_t GetCurrentTimeMicros();

private:
    int m_target_fps = 30;
    double m_speed = 1.0;
    int64_t m_frame_interval_us = 33333; // microseconds per frame (1/30s)
    int64_t m_last_frame_time = 0;       // timestamp in microseconds
};

} // namespace litewp