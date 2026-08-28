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
    double effective_fps = (m_speed > 0.05) ? (m_target_fps * m_speed) : m_target_fps;
    m_frame_interval_us = static_cast<int64_t>(1000000.0 / effective_fps);
}

int PlaybackClock::GetTargetFPS() const {
    return m_target_fps;
}

void PlaybackClock::SetSpeedMultiplier(double speed) {
    if (speed < 0.1) speed = 0.1;
    if (speed > 5.0) speed = 5.0;
    m_speed = speed;
    double effective_fps = m_target_fps * m_speed;
    m_frame_interval_us = static_cast<int64_t>(1000000.0 / effective_fps);
}

double PlaybackClock::GetSpeedMultiplier() const {
    return m_speed;
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
    // Use quotient/remainder decomposition to avoid int64 overflow
    // (counter * 1000000) would overflow after ~10.6 days of uptime on 10MHz QPC
    uint64_t counts = static_cast<uint64_t>(counter.QuadPart);
    uint64_t freq = static_cast<uint64_t>(frequency.QuadPart);
    return static_cast<int64_t>((counts / freq) * 1000000ULL + ((counts % freq) * 1000000ULL) / freq);
}

} // namespace litewp
