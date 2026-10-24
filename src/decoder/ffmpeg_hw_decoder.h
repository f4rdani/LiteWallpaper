#pragma once
#include "video_decoder.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstring>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace miniwp {

class FFmpegHWDecoder : public IVideoDecoder {
public:
    FFmpegHWDecoder();
    ~FFmpegHWDecoder() override;

    bool Open(const char* path, ID3D11Device* d3d_device, int max_width = 0, int max_height = 0) override;
    bool DecodeNextFrame(VideoFrame& frame) override;
    void SeekToStart() override;
    void SeekTo(double seconds) override;
    VideoInfo GetInfo() const override;
    bool HasAudio() const override;
    int DecodeAudioSamples(float* buffer, int max_samples) override;
    void Close() override;

    bool IsHWAccelerated() const noexcept { return m_is_hw_accelerated; }

    // Dynamically enables/disables audio decoding; lazy-initializes audio codec on demand
    void SetAudioEnabled(bool enabled);
    void SetForceSoftware(bool force_sw) noexcept { m_force_software = force_sw; }

private:
    AVFormatContext* m_fmt_ctx = nullptr;
    AVCodecContext*  m_video_codec_ctx = nullptr;
    AVCodecContext*  m_audio_codec_ctx = nullptr;
    AVBufferRef*     m_hw_device_ctx = nullptr;
    AVFrame*         m_hw_frame = nullptr;
    AVFrame*         m_audio_frame = nullptr;
    AVPacket*        m_packet = nullptr;
    SwrContext*      m_swr_ctx = nullptr;

    int m_video_stream_idx = -1;
    int m_audio_stream_idx = -1;

    ID3D11Device* m_d3d_device = nullptr;
    bool m_is_hw_accelerated = false;
    bool m_force_software = false;
    bool m_audio_enabled = false;
    int  m_max_width = 0;
    int  m_max_height = 0;

    VideoInfo m_info;

    bool InitHWDecoder(ID3D11Device* device);
    bool EnsureAudioDecoderOpen();
    void CloseAudioDecoder();
    bool UploadSoftwareFrame(AVFrame* src, VideoFrame& frame);

    // Software decode fallback resources
    SwsContext*                             m_sws_ctx = nullptr;
    uint8_t*                                m_sw_y = nullptr;
    uint8_t*                                m_sw_uv = nullptr;
    int                                     m_sw_width = 0;
    int                                     m_sw_height = 0;
    AVPixelFormat                           m_sw_src_format = AV_PIX_FMT_NONE;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_sw_texture;
};

} // namespace miniwp
