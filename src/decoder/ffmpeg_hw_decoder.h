#pragma once
#include "video_decoder.h"
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include <d3d11.h>

namespace litewp {

class FFmpegHWDecoder : public IVideoDecoder {
public:
    FFmpegHWDecoder();
    ~FFmpegHWDecoder() override;

    bool Open(const char* path, ID3D11Device* d3d_device) override;
    bool DecodeNextFrame(VideoFrame& frame) override;
    void SeekToStart() override;
    VideoInfo GetInfo() const override;
    bool HasAudio() const override;
    int DecodeAudioSamples(float* buffer, int max_samples) override;
    void Close() override;

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

    VideoInfo m_info;

    bool InitHWDecoder(ID3D11Device* device);
};

} // namespace litewp
