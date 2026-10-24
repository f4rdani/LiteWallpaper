#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "audio_player.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace miniwp {

// 200ms circular ring capacity @ 48 kHz stereo (48000 * 2 * 0.2 = 19200 floats)
static constexpr size_t MAX_SAMPLE_BUFFER_FLOATS = 19200;

AudioPlayer::AudioPlayer() = default;

AudioPlayer::~AudioPlayer() {
    Stop();
}

bool AudioPlayer::Init() {
    if (m_running.load()) return true;

    if (!m_wake_event) {
        m_wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_wake_event) return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_buffer_mutex);
        m_ring.assign(MAX_SAMPLE_BUFFER_FLOATS, 0.0f);
        m_read_pos = 0;
        m_write_pos = 0;
        m_count = 0;
    }

    m_running.store(true);
    m_thread = std::thread(&AudioPlayer::AudioThread, this);
    return true;
}

void AudioPlayer::PushSamples(const float* data, int num_samples) {
    if (!data || num_samples <= 0 || !m_running.load() || m_muted.load() || m_volume.load() <= 0.0f) {
        return;
    }

    size_t incoming = static_cast<size_t>(num_samples) & ~static_cast<size_t>(1); // Ensure stereo pairs
    if (incoming == 0) return;

    bool was_empty = false;
    {
        std::lock_guard<std::mutex> lock(m_buffer_mutex);
        if (m_ring.empty()) return;
        was_empty = (m_count == 0);

        if (incoming >= MAX_SAMPLE_BUFFER_FLOATS) {
            data += (incoming - MAX_SAMPLE_BUFFER_FLOATS);
            incoming = MAX_SAMPLE_BUFFER_FLOATS;
            m_read_pos = 0;
            m_write_pos = 0;
            m_count = 0;
        } else if (m_count + incoming > MAX_SAMPLE_BUFFER_FLOATS) {
            // Advance read cursor to drop oldest samples (O(1), zero memmove)
            size_t overflow = (m_count + incoming) - MAX_SAMPLE_BUFFER_FLOATS;
            m_read_pos = (m_read_pos + overflow) % MAX_SAMPLE_BUFFER_FLOATS;
            m_count -= overflow;
        }

        size_t first_chunk = (std::min)(incoming, MAX_SAMPLE_BUFFER_FLOATS - m_write_pos);
        std::memcpy(&m_ring[m_write_pos], data, first_chunk * sizeof(float));
        if (incoming > first_chunk) {
            std::memcpy(&m_ring[0], data + first_chunk, (incoming - first_chunk) * sizeof(float));
        }
        m_write_pos = (m_write_pos + incoming) % MAX_SAMPLE_BUFFER_FLOATS;
        m_count += incoming;
    }

    if (was_empty && m_wake_event) {
        SetEvent(static_cast<HANDLE>(m_wake_event));
    }
}

void AudioPlayer::SetVolume(float volume) {
    float clamped = std::clamp(volume, 0.0f, 1.0f);
    float prev = m_volume.exchange(clamped);
    if ((prev <= 0.0f && clamped > 0.0f) || (prev > 0.0f && clamped <= 0.0f)) {
        if (m_wake_event) SetEvent(static_cast<HANDLE>(m_wake_event));
    }
}

float AudioPlayer::GetVolume() const {
    return m_volume.load();
}

void AudioPlayer::SetMuted(bool muted) {
    bool prev = m_muted.exchange(muted);
    if (prev != muted) {
        if (muted) {
            std::lock_guard<std::mutex> lock(m_buffer_mutex);
            m_read_pos = 0;
            m_write_pos = 0;
            m_count = 0;
        }
        if (m_wake_event) SetEvent(static_cast<HANDLE>(m_wake_event));
    }
}

bool AudioPlayer::IsMuted() const {
    return m_muted.load();
}

bool AudioPlayer::IsRunning() const {
    return m_running.load();
}

void AudioPlayer::Stop() {
    if (m_running.exchange(false)) {
        if (m_wake_event) {
            SetEvent(static_cast<HANDLE>(m_wake_event));
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }
    if (m_wake_event) {
        CloseHandle(static_cast<HANDLE>(m_wake_event));
        m_wake_event = nullptr;
    }
    std::lock_guard<std::mutex> lock(m_buffer_mutex);
    std::vector<float>().swap(m_ring); // Release ring buffer memory completely
    m_read_pos = 0;
    m_write_pos = 0;
    m_count = 0;
}

void AudioPlayer::AudioThread() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_initialized = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audio_client = nullptr;
    IAudioRenderClient* render_client = nullptr;
    WAVEFORMATEX* mix_format = nullptr;
    HANDLE hBufferEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool event_driven = false;
    bool client_started = false;

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

    const REFERENCE_TIME hnsRequestedDuration = 500000; // 50 ms shared buffer
    if (SUCCEEDED(hr) && audio_client && mix_format) {
        if (hBufferEvent) {
            hr = audio_client->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                hnsRequestedDuration,
                0,
                mix_format,
                nullptr
            );
            if (SUCCEEDED(hr) && SUCCEEDED(audio_client->SetEventHandle(hBufferEvent))) {
                event_driven = true;
            }
        }
        if (!event_driven) {
            hr = audio_client->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                0,
                hnsRequestedDuration,
                0,
                mix_format,
                nullptr
            );
        }
    }

    UINT32 bufferFrameCount = 0;
    if (SUCCEEDED(hr) && audio_client) {
        hr = audio_client->GetBufferSize(&bufferFrameCount);
    }
    if (SUCCEEDED(hr) && audio_client) {
        hr = audio_client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_client));
    }

    const bool ready = SUCCEEDED(hr) && audio_client && render_client && mix_format && bufferFrameCount > 0;
    bool isFloat = false;
    int channels = 2;
    if (ready) {
        channels = mix_format->nChannels;
        isFloat = (mix_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                  (mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                   reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix_format)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }

    HANDLE hWake = static_cast<HANDLE>(m_wake_event);
    UINT32 empty_polls = 0;

    while (m_running.load()) {
        if (!ready) {
            WaitForSingleObject(hWake, 250);
            continue;
        }

        const float current_vol = m_muted.load() ? 0.0f : m_volume.load();
        size_t queued_floats = 0;
        {
            std::lock_guard<std::mutex> lock(m_buffer_mutex);
            queued_floats = m_count;
        }

        // Suspend WASAPI stream completely when muted or idle (0% CPU in app & audiodg.exe)
        if (current_vol <= 0.0f || (queued_floats == 0 && empty_polls > 12)) {
            if (client_started) {
                audio_client->Stop();
                audio_client->Reset();
                client_started = false;
            }
            WaitForSingleObject(hWake, INFINITE);
            empty_polls = 0;
            continue;
        }

        if (!client_started) {
            if (SUCCEEDED(audio_client->Start())) {
                client_started = true;
            }
        }

        HANDLE waitHandles[2] = { hWake, hBufferEvent };
        DWORD waitCount = (event_driven && hBufferEvent) ? 2 : 1;
        DWORD waitTimeout = event_driven ? 50 : 12;
        WaitForMultipleObjects(waitCount, waitHandles, FALSE, waitTimeout);
        if (!m_running.load()) break;

        UINT32 numPaddingFrames = 0;
        if (FAILED(audio_client->GetCurrentPadding(&numPaddingFrames))) {
            continue;
        }

        UINT32 numAvailableFrames = (bufferFrameCount > numPaddingFrames) ? (bufferFrameCount - numPaddingFrames) : 0;
        if (numAvailableFrames == 0) {
            continue;
        }

        BYTE* pData = nullptr;
        UINT32 framesToWrite = (std::min)(numAvailableFrames, static_cast<UINT32>(4800));
        if (FAILED(render_client->GetBuffer(framesToWrite, &pData)) || !pData) {
            continue;
        }

        DWORD bufferFlags = 0;
        {
            std::lock_guard<std::mutex> lock(m_buffer_mutex);
            const size_t availablePairs = m_count / 2;
            const UINT32 copyPairs = static_cast<UINT32>((std::min)(static_cast<size_t>(framesToWrite), availablePairs));

            if (copyPairs == 0 || current_vol <= 0.0f) {
                bufferFlags = AUDCLNT_BUFFERFLAGS_SILENT;
                empty_polls++;
            } else {
                empty_polls = 0;
                if (isFloat) {
                    float* out = reinterpret_cast<float*>(pData);
                    for (UINT32 i = 0; i < framesToWrite; ++i) {
                        float l = 0.0f, r = 0.0f;
                        if (i < copyPairs) {
                            l = m_ring[m_read_pos] * current_vol;
                            r = m_ring[(m_read_pos + 1) % MAX_SAMPLE_BUFFER_FLOATS] * current_vol;
                            m_read_pos = (m_read_pos + 2) % MAX_SAMPLE_BUFFER_FLOATS;
                        }
                        if (channels == 2) {
                            out[i * 2 + 0] = l;
                            out[i * 2 + 1] = r;
                        } else if (channels == 1) {
                            out[i] = (l + r) * 0.5f;
                        } else {
                            out[i * channels + 0] = l;
                            out[i * channels + 1] = r;
                            for (int c = 2; c < channels; ++c) out[i * channels + c] = 0.0f;
                        }
                    }
                } else if (mix_format->wBitsPerSample == 16) {
                    int16_t* out = reinterpret_cast<int16_t*>(pData);
                    for (UINT32 i = 0; i < framesToWrite; ++i) {
                        float l = 0.0f, r = 0.0f;
                        if (i < copyPairs) {
                            l = std::clamp(m_ring[m_read_pos] * current_vol, -1.0f, 1.0f);
                            r = std::clamp(m_ring[(m_read_pos + 1) % MAX_SAMPLE_BUFFER_FLOATS] * current_vol, -1.0f, 1.0f);
                            m_read_pos = (m_read_pos + 2) % MAX_SAMPLE_BUFFER_FLOATS;
                        }
                        if (channels == 2) {
                            out[i * 2 + 0] = static_cast<int16_t>(l * 32767.0f);
                            out[i * 2 + 1] = static_cast<int16_t>(r * 32767.0f);
                        } else if (channels == 1) {
                            out[i] = static_cast<int16_t>(((l + r) * 0.5f) * 32767.0f);
                        } else {
                            out[i * channels + 0] = static_cast<int16_t>(l * 32767.0f);
                            out[i * channels + 1] = static_cast<int16_t>(r * 32767.0f);
                            for (int c = 2; c < channels; ++c) out[i * channels + c] = 0;
                        }
                    }
                } else {
                    bufferFlags = AUDCLNT_BUFFERFLAGS_SILENT;
                }
                m_count -= static_cast<size_t>(copyPairs) * 2;
            }
        }

        render_client->ReleaseBuffer(framesToWrite, bufferFlags);
    }

    if (audio_client) {
        if (client_started) audio_client->Stop();
        audio_client->Release();
    }
    if (render_client) render_client->Release();
    if (mix_format) CoTaskMemFree(mix_format);
    if (device) device->Release();
    if (enumerator) enumerator->Release();
    if (hBufferEvent) CloseHandle(hBufferEvent);
    if (co_initialized) CoUninitialize();
}

} // namespace miniwp
