#pragma once
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>

namespace litewp {

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    // Initialize WASAPI shared-mode audio output
    bool Init();
    
    // Push stereo float audio samples to internal buffer (called by decoder thread)
    void PushSamples(const float* data, int num_samples);
    
    // Set volume (0.0 to 1.0)
    void SetVolume(float volume);
    float GetVolume() const;
    
    // Mute / Unmute
    void SetMuted(bool muted);
    bool IsMuted() const;
    
    // Check if audio thread is actively running
    bool IsRunning() const;

    void Stop();

private:
    std::atomic<float> m_volume{0.0f};
    std::atomic<bool>  m_muted{true};  // Muted by default!
    std::atomic<bool>  m_running{false};
    std::thread        m_thread;

    std::mutex         m_buffer_mutex;
    std::vector<float> m_sample_buffer; // Stereo interleaved float samples

    void AudioThread();
};

} // namespace litewp
