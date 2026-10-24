#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mimalloc.h>
#include "ffmpeg_hw_decoder.h"
#include <algorithm>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace miniwp {

static AVPixelFormat GetHWFormat(AVCodecContext* ctx, const AVPixelFormat* pix_fmts) {
    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_D3D11) {
            // Allocate explicit AVHWFramesContext with D3D11_BIND_SHADER_RESOURCE
            // and a compact surface pool (4-6 frames) for true zero-copy SRV rendering.
            if (ctx->hw_device_ctx) {
                AVBufferRef* frames_ref = av_hwframe_ctx_alloc(ctx->hw_device_ctx);
                if (frames_ref) {
                    AVHWFramesContext* frames_ctx = reinterpret_cast<AVHWFramesContext*>(frames_ref->data);
                    frames_ctx->format = AV_PIX_FMT_D3D11;
                    frames_ctx->sw_format = AV_PIX_FMT_NV12;

                    int w = ctx->coded_width > 0 ? ctx->coded_width : (ctx->width > 0 ? ctx->width : 1920);
                    int h = ctx->coded_height > 0 ? ctx->coded_height : (ctx->height > 0 ? ctx->height : 1080);
                    frames_ctx->width = FFALIGN(w, 16);
                    frames_ctx->height = FFALIGN(h, 16);

                    int ref_frames = ctx->refs > 0 ? ctx->refs : 2;
                    frames_ctx->initial_pool_size = std::clamp(ref_frames + 2, 4, 6);

                    auto* d3d11_frames = reinterpret_cast<AVD3D11VAFramesContext*>(frames_ctx->hwctx);
                    d3d11_frames->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;

                    if (av_hwframe_ctx_init(frames_ref) >= 0) {
                        ctx->hw_frames_ctx = av_buffer_ref(frames_ref);
                    }
                    av_buffer_unref(&frames_ref);
                }
            }
            return *p;
        }
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

bool FFmpegHWDecoder::EnsureAudioDecoderOpen() {
    if (!m_fmt_ctx || m_audio_stream_idx < 0) return false;
    if (m_audio_codec_ctx && m_swr_ctx && m_audio_frame) {
        m_fmt_ctx->streams[m_audio_stream_idx]->discard = AVDISCARD_DEFAULT;
        return true;
    }

    CloseAudioDecoder();

    AVStream* audio_stream = m_fmt_ctx->streams[m_audio_stream_idx];
    const AVCodec* audio_codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
    if (!audio_codec) return false;

    m_audio_codec_ctx = avcodec_alloc_context3(audio_codec);
    if (!m_audio_codec_ctx) return false;

    if (avcodec_parameters_to_context(m_audio_codec_ctx, audio_stream->codecpar) < 0) {
        CloseAudioDecoder();
        return false;
    }

    m_audio_codec_ctx->thread_count = 1;
    if (avcodec_open2(m_audio_codec_ctx, audio_codec, nullptr) < 0) {
        CloseAudioDecoder();
        return false;
    }

    AVChannelLayout out_ch_layout;
    av_channel_layout_default(&out_ch_layout, 2);

    swr_alloc_set_opts2(
        &m_swr_ctx,
        &out_ch_layout,
        AV_SAMPLE_FMT_FLT,
        48000, // Match standard WASAPI shared-mode mix rate
        &m_audio_codec_ctx->ch_layout,
        m_audio_codec_ctx->sample_fmt,
        m_audio_codec_ctx->sample_rate,
        0,
        nullptr
    );
    av_channel_layout_uninit(&out_ch_layout);

    if (!m_swr_ctx || swr_init(m_swr_ctx) < 0) {
        CloseAudioDecoder();
        return false;
    }

    m_audio_frame = av_frame_alloc();
    if (!m_audio_frame) {
        CloseAudioDecoder();
        return false;
    }

    audio_stream->discard = AVDISCARD_DEFAULT;
    return true;
}

void FFmpegHWDecoder::CloseAudioDecoder() {
    if (m_audio_frame) {
        av_frame_unref(m_audio_frame);
        av_frame_free(&m_audio_frame);
        m_audio_frame = nullptr;
    }
    if (m_swr_ctx) {
        swr_free(&m_swr_ctx);
        m_swr_ctx = nullptr;
    }
    if (m_audio_codec_ctx) {
        avcodec_free_context(&m_audio_codec_ctx);
        m_audio_codec_ctx = nullptr;
    }
    if (m_fmt_ctx && m_audio_stream_idx >= 0 &&
        static_cast<unsigned int>(m_audio_stream_idx) < m_fmt_ctx->nb_streams) {
        m_fmt_ctx->streams[m_audio_stream_idx]->discard = AVDISCARD_ALL;
    }
}

void FFmpegHWDecoder::SetAudioEnabled(bool enabled) {
    if (m_audio_enabled == enabled) {
        if (enabled && m_fmt_ctx && m_audio_stream_idx >= 0 && !m_audio_codec_ctx) {
            EnsureAudioDecoderOpen();
        }
        return;
    }
    m_audio_enabled = enabled;
    if (!m_fmt_ctx || m_audio_stream_idx < 0) return;

    if (enabled) {
        EnsureAudioDecoderOpen();
    } else {
        // Release audio codec & resampler buffers immediately when muted
        CloseAudioDecoder();
        mi_collect(false);
    }
}

bool FFmpegHWDecoder::Open(const char* path, ID3D11Device* d3d_device, int max_width, int max_height) {
    bool keep_audio_enabled = m_audio_enabled;
    Close();
    m_audio_enabled = keep_audio_enabled;

    if (!path || !d3d_device) return false;
    m_d3d_device = d3d_device;
    m_max_width = max_width;
    m_max_height = max_height;

    m_fmt_ctx = avformat_alloc_context();
    if (!m_fmt_ctx) return false;
    m_fmt_ctx->flags |= AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;
    m_fmt_ctx->probesize = 64 * 1024;
    m_fmt_ctx->max_analyze_duration = 500000;

    if (avformat_open_input(&m_fmt_ctx, path, nullptr, nullptr) < 0) {
        m_fmt_ctx = nullptr;
        return false;
    }

    if (avformat_find_stream_info(m_fmt_ctx, nullptr) < 0) {
        Close();
        return false;
    }

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

    // Discard all non-active streams so packets are never buffered in heap memory
    for (unsigned int i = 0; i < m_fmt_ctx->nb_streams; ++i) {
        if (i != static_cast<unsigned int>(m_video_stream_idx) &&
            (i != static_cast<unsigned int>(m_audio_stream_idx) || !m_audio_enabled)) {
            m_fmt_ctx->streams[i]->discard = AVDISCARD_ALL;
        }
    }

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

    m_video_codec_ctx->width = video_stream->codecpar->width;
    m_video_codec_ctx->height = video_stream->codecpar->height;
    m_video_codec_ctx->coded_width = FFALIGN(video_stream->codecpar->width, 16);
    m_video_codec_ctx->coded_height = FFALIGN(video_stream->codecpar->height, 16);

    m_video_codec_ctx->thread_count = 1;
    m_video_codec_ctx->thread_type = FF_THREAD_SLICE;
    m_video_codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_video_codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;

    if (!m_force_software && InitHWDecoder(d3d_device)) {
        m_video_codec_ctx->hw_device_ctx = av_buffer_ref(m_hw_device_ctx);
        m_video_codec_ctx->get_format = GetHWFormat;
        m_is_hw_accelerated = true;
    } else {
        m_is_hw_accelerated = false;
        m_video_codec_ctx->thread_count = 2;
    }

    if (avcodec_open2(m_video_codec_ctx, video_codec, nullptr) < 0) {
        Close();
        return false;
    }

    m_info.width = m_video_codec_ctx->width;
    m_info.height = m_video_codec_ctx->height;
    m_info.fps = (video_stream->avg_frame_rate.den > 0) ? av_q2d(video_stream->avg_frame_rate) : 30.0;
    if (m_fmt_ctx->duration > 0) {
        m_info.duration_seconds = static_cast<double>(m_fmt_ctx->duration) / AV_TIME_BASE;
    }
    m_info.codec_name = video_codec->name ? video_codec->name : "unknown";
    m_info.has_audio = (m_audio_stream_idx >= 0);

    // Lazy audio initialization: only allocate audio codec & resampler when audio is enabled
    if (m_audio_enabled && m_audio_stream_idx >= 0) {
        EnsureAudioDecoderOpen();
    }

    m_hw_frame = av_frame_alloc();
    m_packet = av_packet_alloc();
    if (!m_hw_frame || !m_packet) {
        Close();
        return false;
    }

    return true;
}

bool FFmpegHWDecoder::DecodeNextFrame(VideoFrame& frame) {
    if (!m_video_codec_ctx || !m_fmt_ctx || !m_hw_frame || !m_packet) {
        return false;
    }

    while (true) {
        int ret = avcodec_receive_frame(m_video_codec_ctx, m_hw_frame);
        if (ret == 0) {
            if (m_hw_frame->format == AV_PIX_FMT_D3D11) {
                frame.texture = reinterpret_cast<ID3D11Texture2D*>(m_hw_frame->data[0]);
                frame.texture_index = static_cast<int>(reinterpret_cast<intptr_t>(m_hw_frame->data[1]));
                frame.width = m_hw_frame->width;
                frame.height = m_hw_frame->height;
                frame.pts = m_hw_frame->pts;
                return true;
            }
            return UploadSoftwareFrame(m_hw_frame, frame);
        }

        ret = av_read_frame(m_fmt_ctx, m_packet);
        if (ret < 0) {
            SeekToStart();
            ret = av_read_frame(m_fmt_ctx, m_packet);
            if (ret < 0) {
                return false;
            }
        }

        if (m_packet->stream_index == m_video_stream_idx) {
            avcodec_send_packet(m_video_codec_ctx, m_packet);
            av_packet_unref(m_packet);
        } else if (m_packet->stream_index == m_audio_stream_idx) {
            if (m_audio_enabled && m_audio_codec_ctx && m_swr_ctx) {
                avcodec_send_packet(m_audio_codec_ctx, m_packet);
            }
            av_packet_unref(m_packet);
        } else {
            av_packet_unref(m_packet);
        }
    }
}

bool FFmpegHWDecoder::UploadSoftwareFrame(AVFrame* src, VideoFrame& frame) {
    if (!src || !m_d3d_device || src->width <= 0 || src->height <= 0) {
        return false;
    }

    int src_w = src->width;
    int src_h = src->height;
    AVPixelFormat srcFmt = static_cast<AVPixelFormat>(src->format);

    int target_w = src_w;
    int target_h = src_h;
    if (m_max_width > 0 && m_max_height > 0 && (src_w > m_max_width || src_h > m_max_height)) {
        double scale = (std::min)(static_cast<double>(m_max_width) / src_w, static_cast<double>(m_max_height) / src_h);
        target_w = (static_cast<int>(src_w * scale) / 2) * 2;
        target_h = (static_cast<int>(src_h * scale) / 2) * 2;
        if (target_w < 128) target_w = 128;
        if (target_h < 128) target_h = 128;
    }

    if (!m_sws_ctx || m_sw_width != target_w || m_sw_height != target_h || m_sw_src_format != srcFmt) {
        if (m_sws_ctx) {
            sws_freeContext(m_sws_ctx);
            m_sws_ctx = nullptr;
        }
        m_sws_ctx = sws_getContext(src_w, src_h, srcFmt, target_w, target_h, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
        m_sw_src_format = srcFmt;
        if (!m_sws_ctx) {
            return false;
        }
    }

    if (!m_sw_y || !m_sw_uv || m_sw_width != target_w || m_sw_height != target_h) {
        if (m_sw_y) { av_free(m_sw_y); m_sw_y = nullptr; }
        if (m_sw_uv) { av_free(m_sw_uv); m_sw_uv = nullptr; }
        m_sw_y = static_cast<uint8_t*>(av_malloc(static_cast<size_t>(target_w) * target_h));
        m_sw_uv = static_cast<uint8_t*>(av_malloc(static_cast<size_t>(target_w) * target_h / 2));
        if (!m_sw_y || !m_sw_uv) {
            return false;
        }
    }

    uint8_t* dst[4] = { m_sw_y, m_sw_uv, nullptr, nullptr };
    int dstStride[4] = { target_w, target_w, 0, 0 };
    if (sws_scale(m_sws_ctx, src->data, src->linesize, 0, src_h, dst, dstStride) < 0) {
        return false;
    }

    if (!m_sw_texture || m_sw_width != target_w || m_sw_height != target_h) {
        m_sw_texture.Reset();
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(target_w);
        td.Height = static_cast<UINT>(target_h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_d3d_device->CreateTexture2D(&td, nullptr, &m_sw_texture))) {
            return false;
        }
    }

    m_sw_width = target_w;
    m_sw_height = target_h;

    ComPtr<ID3D11DeviceContext> ctx;
    m_d3d_device->GetImmediateContext(&ctx);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (!ctx || FAILED(ctx->Map(m_sw_texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }

    BYTE* base = static_cast<BYTE*>(mapped.pData);
    const UINT pitch = mapped.RowPitch;
    for (int y = 0; y < target_h; y++) {
        std::memcpy(base + static_cast<size_t>(y) * pitch, m_sw_y + static_cast<size_t>(y) * target_w, static_cast<size_t>(target_w));
    }
    for (int y = 0; y < target_h / 2; y++) {
        std::memcpy(base + static_cast<size_t>(pitch) * target_h + static_cast<size_t>(y) * pitch, m_sw_uv + static_cast<size_t>(y) * target_w, static_cast<size_t>(target_w));
    }
    ctx->Unmap(m_sw_texture.Get(), 0);

    frame.texture = m_sw_texture.Get();
    frame.texture_index = 0;
    frame.width = target_w;
    frame.height = target_h;
    frame.pts = src->pts;
    return true;
}

void FFmpegHWDecoder::SeekToStart() {
    if (!m_fmt_ctx || m_video_stream_idx < 0) return;

    if (m_hw_frame) av_frame_unref(m_hw_frame);
    if (m_audio_frame) av_frame_unref(m_audio_frame);
    if (m_packet) av_packet_unref(m_packet);

    av_seek_frame(m_fmt_ctx, m_video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
    if (m_video_codec_ctx) {
        avcodec_flush_buffers(m_video_codec_ctx);
    }
    if (m_audio_codec_ctx) {
        avcodec_flush_buffers(m_audio_codec_ctx);
    }

    // Reclaim freed GOP heap & working-set pages immediately on loop/seek
    mi_collect(true);
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

void FFmpegHWDecoder::SeekTo(double seconds) {
    if (!m_fmt_ctx || m_video_stream_idx < 0) return;
    if (seconds < 0.0) seconds = 0.0;

    if (m_hw_frame) av_frame_unref(m_hw_frame);
    if (m_audio_frame) av_frame_unref(m_audio_frame);
    if (m_packet) av_packet_unref(m_packet);

    const AVStream* st = m_fmt_ctx->streams[m_video_stream_idx];
    int64_t ts = av_rescale_q(static_cast<int64_t>(seconds * AV_TIME_BASE), AV_TIME_BASE_Q, st->time_base);
    if (st->duration > 0 && ts >= st->duration) {
        ts = (st->duration > 1) ? (st->duration - 1) : 0;
    }
    av_seek_frame(m_fmt_ctx, m_video_stream_idx, ts, AVSEEK_FLAG_BACKWARD);
    if (m_video_codec_ctx) {
        avcodec_flush_buffers(m_video_codec_ctx);
    }
    if (m_audio_codec_ctx) {
        avcodec_flush_buffers(m_audio_codec_ctx);
    }

    mi_collect(true);
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

VideoInfo FFmpegHWDecoder::GetInfo() const {
    return m_info;
}

bool FFmpegHWDecoder::HasAudio() const {
    return m_info.has_audio;
}

int FFmpegHWDecoder::DecodeAudioSamples(float* buffer, int max_samples) {
    if (!m_audio_enabled || !m_audio_codec_ctx || !m_swr_ctx || !m_audio_frame || !buffer || max_samples <= 0) {
        return 0;
    }

    int ret = avcodec_receive_frame(m_audio_codec_ctx, m_audio_frame);
    if (ret < 0) {
        return 0;
    }

    int max_samples_per_channel = max_samples / 2;
    if (max_samples_per_channel <= 0) {
        av_frame_unref(m_audio_frame);
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

    av_frame_unref(m_audio_frame);
    return samples_converted > 0 ? (samples_converted * 2) : 0;
}

void FFmpegHWDecoder::Close() {
    if (m_sws_ctx) {
        sws_freeContext(m_sws_ctx);
        m_sws_ctx = nullptr;
    }
    if (m_sw_y) {
        av_free(m_sw_y);
        m_sw_y = nullptr;
    }
    if (m_sw_uv) {
        av_free(m_sw_uv);
        m_sw_uv = nullptr;
    }
    m_sw_texture.Reset();
    m_sw_width = 0;
    m_sw_height = 0;
    m_sw_src_format = AV_PIX_FMT_NONE;

    if (m_hw_frame) {
        av_frame_unref(m_hw_frame);
        av_frame_free(&m_hw_frame);
        m_hw_frame = nullptr;
    }
    CloseAudioDecoder();

    if (m_packet) {
        av_packet_free(&m_packet);
        m_packet = nullptr;
    }
    if (m_video_codec_ctx) {
        avcodec_free_context(&m_video_codec_ctx);
        m_video_codec_ctx = nullptr;
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
    m_max_width = 0;
    m_max_height = 0;
    m_info = VideoInfo{};
}

} // namespace miniwp
