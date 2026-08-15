#include "ffmpeg_hw_decoder.h"
#include <iostream>

namespace litewp {

static AVPixelFormat GetHWFormat(AVCodecContext* /*ctx*/, const AVPixelFormat* pix_fmts) {
    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_D3D11) return *p;
    }
    return AV_PIX_FMT_NONE;
}

FFmpegHWDecoder::FFmpegHWDecoder() = default;

FFmpegHWDecoder::~FFmpegHWDecoder() {
    Close();
}

bool FFmpegHWDecoder::InitHWDecoder(ID3D11Device* device) {
    if (!device) return false;

    AVBufferRef* hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!hw_device_ctx) {
        return false;
    }

    auto* device_ctx = reinterpret_cast<AVHWDeviceContext*>(hw_device_ctx->data);
    auto* d3d11_ctx = reinterpret_cast<AVD3D11VADeviceContext*>(device_ctx->hwctx);

    d3d11_ctx->device = device;
    device->AddRef();

    if (av_hwdevice_ctx_init(hw_device_ctx) < 0) {
        av_buffer_unref(&hw_device_ctx);
        return false;
    }

    m_hw_device_ctx = hw_device_ctx;
    return true;
}

bool FFmpegHWDecoder::Open(const char* path, ID3D11Device* d3d_device) {
    Close();

    if (!path || !d3d_device) return false;
    m_d3d_device = d3d_device;

    if (avformat_open_input(&m_fmt_ctx, path, nullptr, nullptr) < 0) {
        return false;
    }

    if (avformat_find_stream_info(m_fmt_ctx, nullptr) < 0) {
        Close();
        return false;
    }

    // Find video and audio streams
    for (unsigned int i = 0; i < m_fmt_ctx->nb_streams; ++i) {
        AVMediaType type = m_fmt_ctx->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && m_video_stream_idx < 0) {
            m_video_stream_idx = static_cast<int>(i);
        } else if (type == AVMEDIA_TYPE_AUDIO && m_audio_stream_idx < 0) {
            m_audio_stream_idx = static_cast<int>(i);
        }
    }

    if (m_video_stream_idx < 0) {
        Close();
        return false;
    }

    // Setup Video Decoder with D3D11VA HW acceleration
    AVStream* video_stream = m_fmt_ctx->streams[m_video_stream_idx];
    const AVCodec* video_codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
    if (!video_codec) {
        Close();
        return false;
    }

    m_video_codec_ctx = avcodec_alloc_context3(video_codec);
    if (!m_video_codec_ctx) {
        Close();
        return false;
    }

    if (avcodec_parameters_to_context(m_video_codec_ctx, video_stream->codecpar) < 0) {
        Close();
        return false;
    }

    if (InitHWDecoder(d3d_device)) {
        m_video_codec_ctx->hw_device_ctx = av_buffer_ref(m_hw_device_ctx);
        m_video_codec_ctx->get_format = GetHWFormat;
        m_is_hw_accelerated = true;
    }

    if (avcodec_open2(m_video_codec_ctx, video_codec, nullptr) < 0) {
        Close();
        return false;
    }

    // Populate VideoInfo
    m_info.width = m_video_codec_ctx->width;
    m_info.height = m_video_codec_ctx->height;
    if (video_stream->avg_frame_rate.den > 0) {
        m_info.fps = av_q2d(video_stream->avg_frame_rate);
    } else {
        m_info.fps = 30.0;
    }
    if (m_fmt_ctx->duration > 0) {
        m_info.duration_seconds = static_cast<double>(m_fmt_ctx->duration) / AV_TIME_BASE;
    }
    m_info.codec_name = video_codec->name ? video_codec->name : "unknown";
    m_info.has_audio = (m_audio_stream_idx >= 0);

    // Setup Audio Decoder if available
    if (m_audio_stream_idx >= 0) {
        AVStream* audio_stream = m_fmt_ctx->streams[m_audio_stream_idx];
        const AVCodec* audio_codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
        if (audio_codec) {
            m_audio_codec_ctx = avcodec_alloc_context3(audio_codec);
            if (m_audio_codec_ctx && avcodec_parameters_to_context(m_audio_codec_ctx, audio_stream->codecpar) >= 0) {
                if (avcodec_open2(m_audio_codec_ctx, audio_codec, nullptr) >= 0) {
                    // Initialize SwrContext to resample to Stereo Float32 @ 44100 Hz
                    AVChannelLayout out_ch_layout;
                    av_channel_layout_default(&out_ch_layout, 2);
                    
                    swr_alloc_set_opts2(
                        &m_swr_ctx,
                        &out_ch_layout,
                        AV_SAMPLE_FMT_FLT,
                        44100,
                        &m_audio_codec_ctx->ch_layout,
                        m_audio_codec_ctx->sample_fmt,
                        m_audio_codec_ctx->sample_rate,
                        0,
                        nullptr
                    );
                    av_channel_layout_uninit(&out_ch_layout);

                    if (m_swr_ctx) {
                        swr_init(m_swr_ctx);
                    }
                }
            }
        }
    }

    m_hw_frame = av_frame_alloc();
    m_audio_frame = av_frame_alloc();
    m_packet = av_packet_alloc();

    return true;
}

bool FFmpegHWDecoder::DecodeNextFrame(VideoFrame& frame) {
    if (!m_video_codec_ctx || !m_fmt_ctx || !m_hw_frame || !m_packet) {
        return false;
    }

    while (true) {
        // Try receiving a frame from the codec
        int ret = avcodec_receive_frame(m_video_codec_ctx, m_hw_frame);
        if (ret == 0) {
            if (m_hw_frame->format == AV_PIX_FMT_D3D11) {
                // Zero-copy texture pointer and slice index from D3D11VA
                frame.texture = reinterpret_cast<ID3D11Texture2D*>(m_hw_frame->data[0]);
                frame.texture_index = static_cast<int>(reinterpret_cast<intptr_t>(m_hw_frame->data[1]));
                frame.width = m_hw_frame->width;
                frame.height = m_hw_frame->height;
                frame.pts = m_hw_frame->pts;
                return true;
            }
            // For other pixel formats, non-HW frame
            frame.texture = nullptr;
            frame.texture_index = 0;
            frame.width = m_hw_frame->width;
            frame.height = m_hw_frame->height;
            frame.pts = m_hw_frame->pts;
            return true;
        }

        // Read next packet from stream
        ret = av_read_frame(m_fmt_ctx, m_packet);
        if (ret < 0) {
            // EOF or error -> loop back to beginning
            SeekToStart();
            ret = av_read_frame(m_fmt_ctx, m_packet);
            if (ret < 0) {
                return false;
            }
        }

        if (m_packet->stream_index == m_video_stream_idx) {
            avcodec_send_packet(m_video_codec_ctx, m_packet);
            av_packet_unref(m_packet);
        } else if (m_packet->stream_index == m_audio_stream_idx && m_audio_codec_ctx) {
            avcodec_send_packet(m_audio_codec_ctx, m_packet);
            av_packet_unref(m_packet);
        } else {
            av_packet_unref(m_packet);
        }
    }
}

void FFmpegHWDecoder::SeekToStart() {
    if (!m_fmt_ctx || m_video_stream_idx < 0) return;

    av_seek_frame(m_fmt_ctx, m_video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
    if (m_video_codec_ctx) {
        avcodec_flush_buffers(m_video_codec_ctx);
    }
    if (m_audio_codec_ctx) {
        avcodec_flush_buffers(m_audio_codec_ctx);
    }
}

VideoInfo FFmpegHWDecoder::GetInfo() const {
    return m_info;
}

bool FFmpegHWDecoder::HasAudio() const {
    return m_info.has_audio;
}

int FFmpegHWDecoder::DecodeAudioSamples(float* buffer, int max_samples) {
    if (!m_audio_codec_ctx || !m_swr_ctx || !m_audio_frame || !buffer || max_samples <= 0) {
        return 0;
    }

    int ret = avcodec_receive_frame(m_audio_codec_ctx, m_audio_frame);
    if (ret < 0) {
        return 0;
    }

    // SwrContext outputs 2 channels (stereo).
    // max_samples is the total capacity in floats.
    // swr_convert requires out_count to be the sample count per channel.
    int max_samples_per_channel = max_samples / 2;
    if (max_samples_per_channel <= 0) {
        return 0;
    }

    uint8_t* out_data[1] = { reinterpret_cast<uint8_t*>(buffer) };
    int samples_converted = swr_convert(
        m_swr_ctx,
        out_data,
        max_samples_per_channel,
        const_cast<const uint8_t**>(m_audio_frame->data),
        m_audio_frame->nb_samples
    );

    // Return total float elements written (samples_converted * 2 for stereo)
    return samples_converted > 0 ? (samples_converted * 2) : 0;
}

void FFmpegHWDecoder::Close() {
    if (m_hw_frame) {
        av_frame_free(&m_hw_frame);
        m_hw_frame = nullptr;
    }
    if (m_audio_frame) {
        av_frame_free(&m_audio_frame);
        m_audio_frame = nullptr;
    }
    if (m_packet) {
        av_packet_free(&m_packet);
        m_packet = nullptr;
    }
    if (m_swr_ctx) {
        swr_free(&m_swr_ctx);
        m_swr_ctx = nullptr;
    }
    if (m_video_codec_ctx) {
        avcodec_free_context(&m_video_codec_ctx);
        m_video_codec_ctx = nullptr;
    }
    if (m_audio_codec_ctx) {
        avcodec_free_context(&m_audio_codec_ctx);
        m_audio_codec_ctx = nullptr;
    }
    if (m_hw_device_ctx) {
        av_buffer_unref(&m_hw_device_ctx);
        m_hw_device_ctx = nullptr;
    }
    if (m_fmt_ctx) {
        avformat_close_input(&m_fmt_ctx);
        m_fmt_ctx = nullptr;
    }

    m_video_stream_idx = -1;
    m_audio_stream_idx = -1;
    m_d3d_device = nullptr;
    m_is_hw_accelerated = false;
    m_info = VideoInfo{};
}

} // namespace litewp
