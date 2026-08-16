#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "audio_player.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <algorithm>

namespace litewp {

// Max 200ms buffer capacity for 48000Hz stereo (48000 * 2 * 0.2 = ~19200 floats)
static constexpr size_t MAX_SAMPLE_BUFFER_FLOATS = 19200;

AudioPlayer::AudioPlayer() = default;

AudioPlayer::~AudioPlayer() {
    Stop();
}

bool AudioPlayer::Init() {
    if (m_running.load()) return true;

    m_running.store(true);
    m_thread = std::thread(&AudioPlayer::AudioThread, this);
    return true;
}

void AudioPlayer::PushSamples(const float* data, int num_samples) {
    if (!data || num_samples <= 0 || m_muted.load() || m_volume.load() <= 0.0f) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_buffer_mutex);
    // If incoming samples would exceed limit, drop oldest to maintain low latency and prevent memory bloat
    if (m_sample_buffer.size() + num_samples > MAX_SAMPLE_BUFFER_FLOATS) {
        size_t overflow = (m_sample_buffer.size() + num_samples) - MAX_SAMPLE_BUFFER_FLOATS;
        if (overflow < m_sample_buffer.size()) {
            m_sample_buffer.erase(m_sample_buffer.begin(), m_sample_buffer.begin() + overflow);
        } else {
            m_sample_buffer.clear();
        }
    }
    m_sample_buffer.insert(m_sample_buffer.end(), data, data + num_samples);
}

void AudioPlayer::SetVolume(float volume) {
    m_volume.store(std::clamp(volume, 0.0f, 1.0f));
}

float AudioPlayer::GetVolume() const {
    return m_volume.load();
}

void AudioPlayer::SetMuted(bool muted) {
    m_muted.store(muted);
}

bool AudioPlayer::IsMuted() const {
    return m_muted.load();
}

bool AudioPlayer::IsRunning() const {
    return m_running.load();
}

void AudioPlayer::Stop() {
    if (!m_running.load()) return;
    m_running.store(false);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    std::lock_guard<std::mutex> lock(m_buffer_mutex);
    m_sample_buffer.clear();
}

void AudioPlayer::AudioThread() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool co_initialized = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audio_client = nullptr;
    IAudioRenderClient* render_client = nullptr;
    WAVEFORMATEX* mix_format = nullptr;

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator)
    );

    if (SUCCEEDED(hr) && enumerator) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    }

    if (SUCCEEDED(hr) && device) {
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audio_client));
    }

    if (SUCCEEDED(hr) && audio_client) {
        hr = audio_client->GetMixFormat(&mix_format);
    }

    REFERENCE_TIME hnsRequestedDuration = 1000000; // 100 ms buffer
    if (SUCCEEDED(hr) && audio_client && mix_format) {
        hr = audio_client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            0,
            hnsRequestedDuration,
            0,
            mix_format,
            nullptr
        );
    }

    UINT32 bufferFrameCount = 0;
    if (SUCCEEDED(hr) && audio_client) {
        hr = audio_client->GetBufferSize(&bufferFrameCount);
    }

    if (SUCCEEDED(hr) && audio_client) {
        hr = audio_client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_client));
    }

    if (SUCCEEDED(hr) && audio_client) {
        audio_client->Start();
    }

    while (m_running.load()) {
        if (!audio_client || !render_client || !mix_format) {
            Sleep(50);
            continue;
        }

        UINT32 numPaddingFrames = 0;
        hr = audio_client->GetCurrentPadding(&numPaddingFrames);
        if (FAILED(hr)) {
            Sleep(20);
            continue;
        }

        UINT32 numAvailableFrames = bufferFrameCount - numPaddingFrames;
        if (numAvailableFrames == 0) {
            Sleep(10);
            continue;
        }

        BYTE* pData = nullptr;
        UINT32 framesToRequest = std::min(numAvailableFrames, static_cast<UINT32>(4800));
        hr = render_client->GetBuffer(framesToRequest, &pData);
        if (FAILED(hr) || !pData) {
            Sleep(10);
            continue;
        }

        float current_vol = m_muted.load() ? 0.0f : m_volume.load();
        int channels = mix_format->nChannels;

        // Pop stereo frame pairs using stack buffer (zero heap allocations)
        static constexpr size_t STACK_BUF_FRAMES = 4800; // 100ms @ 48kHz
        float stereoPairs[STACK_BUF_FRAMES * 2] = {};
        UINT32 framesToProcess = std::min(numAvailableFrames, static_cast<UINT32>(STACK_BUF_FRAMES));
        {
            std::lock_guard<std::mutex> lock(m_buffer_mutex);
            size_t availableFloats = m_sample_buffer.size();
            size_t availableStereoPairs = availableFloats / 2;
            size_t pairsToCopy = std::min(static_cast<size_t>(framesToProcess), availableStereoPairs);
            
            if (pairsToCopy > 0) {
                size_t floatsToCopy = pairsToCopy * 2;
                std::copy(m_sample_buffer.begin(), m_sample_buffer.begin() + floatsToCopy, stereoPairs);
                m_sample_buffer.erase(m_sample_buffer.begin(), m_sample_buffer.begin() + floatsToCopy);
            }
        }

        // Write to device buffer handling any channel count and format
        bool isFloat = (mix_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                       (mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                        reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix_format)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

        if (isFloat) {
            float* out = reinterpret_cast<float*>(pData);
            for (UINT32 i = 0; i < framesToProcess; ++i) {
                float l = stereoPairs[i * 2 + 0] * current_vol;
                float r = stereoPairs[i * 2 + 1] * current_vol;

                if (channels == 1) {
                    out[i] = (l + r) * 0.5f;
                } else if (channels == 2) {
                    out[i * 2 + 0] = l;
                    out[i * 2 + 1] = r;
                } else {
                    // Multi-channel (5.1, 7.1, etc.): L, R on channels 0 and 1, silence on others
                    out[i * channels + 0] = l;
                    out[i * channels + 1] = r;
                    for (int c = 2; c < channels; ++c) {
                        out[i * channels + c] = 0.0f;
                    }
                }
            }
        } else if (mix_format->wBitsPerSample == 16) {
            int16_t* out = reinterpret_cast<int16_t*>(pData);
            for (UINT32 i = 0; i < framesToProcess; ++i) {
                float l = std::clamp(stereoPairs[i * 2 + 0] * current_vol, -1.0f, 1.0f);
                float r = std::clamp(stereoPairs[i * 2 + 1] * current_vol, -1.0f, 1.0f);

                if (channels == 1) {
                    out[i] = static_cast<int16_t>(((l + r) * 0.5f) * 32767.0f);
                } else if (channels == 2) {
                    out[i * 2 + 0] = static_cast<int16_t>(l * 32767.0f);
                    out[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
                } else {
                    out[i * channels + 0] = static_cast<int16_t>(l * 32767.0f);
                    out[i * channels + 1] = static_cast<int16_t>(r * 32767.0f);
                    for (int c = 2; c < channels; ++c) {
                        out[i * channels + c] = 0;
                    }
                }
            }
        } else {
            std::memset(pData, 0, framesToProcess * mix_format->nBlockAlign);
        }

        render_client->ReleaseBuffer(framesToProcess, 0);
        Sleep(10);
    }

    if (audio_client) {
        audio_client->Stop();
        audio_client->Release();
    }
    if (render_client) {
        render_client->Release();
    }
    if (mix_format) {
        CoTaskMemFree(mix_format);
    }
    if (device) {
        device->Release();
    }
    if (enumerator) {
        enumerator->Release();
    }
    if (co_initialized) {
        CoUninitialize();
    }
}

} // namespace litewp
