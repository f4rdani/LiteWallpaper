#include "playback_clock.h"
#include <windows.h>
#include <algorithm>

namespace litewp {

PlaybackClock::PlaybackClock() {
    SetTargetFPS(30);
    Reset();
}

void PlaybackClock::SetTargetFPS(int fps) {
    if (fps < 1) fps = 1;
    if (fps > 240) fps = 240;
    m_target_fps = fps;
    m_frame_interval_us = 1000000 / fps;
}

int PlaybackClock::GetTargetFPS() const {
    return m_target_fps;
}

void PlaybackClock::Reset() {
    m_last_frame_time = GetCurrentTimeMicros();
}

bool PlaybackClock::ShouldRenderFrame() {
    int64_t now = GetCurrentTimeMicros();
    int64_t elapsed = now - m_last_frame_time;
    if (elapsed >= m_frame_interval_us) {
        // If elapsed is way too large (e.g. after a pause), reset to now
        if (elapsed >= m_frame_interval_us * 3) {
            m_last_frame_time = now;
        } else {
            m_last_frame_time += m_frame_interval_us;
        }
        return true;
    }
    return false;
}

uint32_t PlaybackClock::GetSleepDurationMs() const {
    int64_t now = GetCurrentTimeMicros();
    int64_t elapsed = now - m_last_frame_time;
    if (elapsed >= m_frame_interval_us) {
        return 0;
    }
    int64_t remaining_us = m_frame_interval_us - elapsed;
    // Avoid busy-spin: if remaining > 0 but < 1ms, sleep at least 1ms
    uint32_t ms = static_cast<uint32_t>(remaining_us / 1000);
    return (ms == 0 && remaining_us > 0) ? 1 : ms;
}

int64_t PlaybackClock::GetCurrentTimeMicros() {
    static LARGE_INTEGER frequency = []() {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (counter.QuadPart * 1000000LL) / frequency.QuadPart;
}

} // namespace litewp
