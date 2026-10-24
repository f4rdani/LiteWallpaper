#pragma once
#include <cstdint>

namespace miniwp {

// Clock that paces playback at the target frame rate with microsecond QPC accuracy.
class PlaybackClock {
public:
    PlaybackClock();

    void SetTargetFPS(int fps);
    int GetTargetFPS() const noexcept;

    // Set dynamic speed multiplier (1.0 = normal, 0.75 = 25% slower for speed ramping)
    void SetSpeedMultiplier(double speed);
    double GetSpeedMultiplier() const noexcept;

    // Returns true if it's time to decode + display the next video frame.
    bool ShouldRenderFrame();

    // Milliseconds / microseconds remaining until next frame
    uint32_t GetSleepDurationMs() const;
    int64_t GetSleepDurationMicros() const;

    // Reset clock (called on resume from pause / when video changes)
    void Reset();

    // Current monotonic time in microseconds (QueryPerformanceCounter based)
    static int64_t GetCurrentTimeMicros();

private:
    int m_target_fps = 30;
    double m_speed = 1.0;
    int64_t m_frame_interval_us = 33333;
    int64_t m_last_frame_time = 0;
};

} // namespace miniwp
