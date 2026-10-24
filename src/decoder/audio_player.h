#pragma once
#include <atomic>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace miniwp {

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    // Initialize event-driven WASAPI shared-mode audio output
    bool Init();

    // Push stereo float audio samples to internal circular ring buffer
    void PushSamples(const float* data, int num_samples);

    // Volume (0.0 to 1.0) & Mute controls
    void SetVolume(float volume);
    float GetVolume() const;

    void SetMuted(bool muted);
    bool IsMuted() const;

    bool IsRunning() const;
    void Stop();

private:
    std::atomic<float> m_volume{0.0f};
    std::atomic<bool>  m_muted{true};
    std::atomic<bool>  m_running{false};
    std::thread        m_thread;
    void*              m_wake_event = nullptr; // HANDLE signaled on state change / sample push / stop

    std::mutex         m_buffer_mutex;
    std::vector<float> m_ring;        // Circular buffer of interleaved stereo floats (48 kHz)
    size_t             m_read_pos = 0;
    size_t             m_write_pos = 0;
    size_t             m_count = 0;

    void AudioThread();
};

} // namespace miniwp
