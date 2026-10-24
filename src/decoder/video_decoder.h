#pragma once
#include <cstdint>
#include <string>

struct ID3D11Texture2D;
struct ID3D11Device;

namespace miniwp {

struct VideoFrame {
    ID3D11Texture2D* texture = nullptr;  // GPU texture (NV12 format in VRAM)
    int texture_index = 0;               // Array slice index inside texture array
    int64_t pts = 0;                     // Presentation timestamp
    int width = 0;
    int height = 0;
};

struct VideoInfo {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    double duration_seconds = 0.0;
    bool has_audio = false;
    std::string codec_name;
};

class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    // Open video file. d3d_device is used for hardware decoding.
    virtual bool Open(const char* path, ID3D11Device* d3d_device, int max_width = 0, int max_height = 0) = 0;

    // Decode next frame. Returns true if a new frame is available.
    virtual bool DecodeNextFrame(VideoFrame& frame) = 0;

    // Seek to beginning of video for seamless looping
    virtual void SeekToStart() = 0;

    // Seek to an absolute position (seconds) inside the current stream
    virtual void SeekTo(double seconds) { (void)seconds; }

    virtual VideoInfo GetInfo() const = 0;
    virtual bool HasAudio() const = 0;

    // Decode next audio samples (interleaved float, stereo 48000 Hz)
    virtual int DecodeAudioSamples(float* buffer, int max_samples) = 0;

    virtual void Close() = 0;
};

} // namespace miniwp
