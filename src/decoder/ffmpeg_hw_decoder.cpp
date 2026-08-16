#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ffmpeg_hw_decoder.h"
#include <iostream>
#include <algorithm>

namespace litewp {

static AVPixelFormat GetHWFormat(AVCodecContext* ctx, const AVPixelFormat* pix_fmts) {
    for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_D3D11) {
            // Allocate an explicit AVHWFramesContext with D3D11_BIND_SHADER_RESOURCE
            // and a minimal surface pool (4 frames) to drastically reduce VRAM (from 500MB -> <50MB)
            // and enable 100% zero-copy direct texture rendering!
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
                    // Pool must be large enough for codec reference frames + decode-in-progress surfaces.
                    // Too small = FFmpeg ignores our pool and allocates its own (20+ frames, no SHADER_RESOURCE flag).
                    int ref_frames = ctx->refs > 0 ? ctx->refs : 4;
                    frames_ctx->initial_pool_size = std::clamp(ref_frames + 4, 8, 16);

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

bool FFmpegHWDecoder::Open(const char* path, ID3D11Device* d3d_device, int max_width, int max_height) {
    Close();

    if (!path || !d3d_device) return false;
    m_d3d_device = d3d_device;
    m_max_width = max_width;
    m_max_height = max_height;

    if (avformat_open_input(&m_fmt_ctx, path, nullptr, nullptr) < 0) {
        return false;
    }

    // Fast demuxer probing settings to minimize RAM buffers
    m_fmt_ctx->probesize = 64 * 1024;
    m_fmt_ctx->max_analyze_duration = 500000;

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

    // Explicitly set width & coded_width to stream dimensions before opening
    m_video_codec_ctx->width = video_stream->codecpar->width;
    m_video_codec_ctx->height = video_stream->codecpar->height;
    m_video_codec_ctx->coded_width = FFALIGN(video_stream->codecpar->width, 16);
    m_video_codec_ctx->coded_height = FFALIGN(video_stream->codecpar->height, 16);

    // Limit thread count and buffer footprint
    m_video_codec_ctx->thread_count = 1; // HW decoding is done on GPU; 1 CPU thread minimizes thread pool RAM!
    m_video_codec_ctx->thread_type = FF_THREAD_SLICE;
    m_video_codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_video_codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;

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
                m_audio_codec_ctx->thread_count = 1;
                if (avcodec_open2(m_audio_codec_ctx, audio_codec, nullptr) >= 0) {
                    // Initialize SwrContext to resample to Stereo Float32 @ 44100 Hz
                    AVChannelLayout out_ch_layout;
                    av_channel_layout_default(&out_ch_layout, 2);
                    
                    swr_alloc_set_opts2(
                        &m_swr_ctx,
                        &out_ch_layout,
                        AV_SAMPLE_FMT_FLT,
                        48000, // Match WASAPI default mix format
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
            // For other pixel formats, non-HW frame: convert & upload to GPU
            return UploadSoftwareFrame(m_hw_frame, frame);
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
        } else if (m_packet->stream_index == m_audio_stream_idx) {
            // Only feed audio packets if audio is enabled
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

    // Auto-downscale to monitor bounds if specified (e.g. 4K -> 1080p saves 75% memory!)
    int target_w = src_w;
    int target_h = src_h;
    if (m_max_width > 0 && m_max_height > 0 && (src_w > m_max_width || src_h > m_max_height)) {
        double scale = (std::min)(static_cast<double>(m_max_width) / src_w, static_cast<double>(m_max_height) / src_h);
        target_w = (static_cast<int>(src_w * scale) / 2) * 2; // Even dimensions for NV12
        target_h = (static_cast<int>(src_h * scale) / 2) * 2;
        if (target_w < 128) target_w = 128;
        if (target_h < 128) target_h = 128;
    }

    // (Re)create swscale context when the source format/size or target size changes
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

    // CPU NV12 staging buffers (reallocate on dimension change)
    if (!m_sw_y || !m_sw_uv || m_sw_width != target_w || m_sw_height != target_h) {
        if (m_sw_y) { av_free(m_sw_y); m_sw_y = nullptr; }
        if (m_sw_uv) { av_free(m_sw_uv); m_sw_uv = nullptr; }
        m_sw_y = static_cast<uint8_t*>(av_malloc((size_t)target_w * target_h));
        m_sw_uv = static_cast<uint8_t*>(av_malloc((size_t)target_w * target_h / 2));
        if (!m_sw_y || !m_sw_uv) {
            return false;
        }
    }

    // Convert and downscale to NV12 (Y plane + interleaved UV plane)
    uint8_t* dst[4] = { m_sw_y, m_sw_uv, nullptr, nullptr };
    int dstStride[4] = { target_w, target_w, 0, 0 };
    if (sws_scale(m_sws_ctx, src->data, src->linesize, 0, src_h, dst, dstStride) < 0) {
        return false;
    }

    // (Re)create the GPU texture when dimensions change
    if (!m_sw_texture || m_sw_width != target_w || m_sw_height != target_h) {
        m_sw_texture.Reset();
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (UINT)target_w;
        td.Height = (UINT)target_h;
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

    // Update tracked dimensions after all checks
    m_sw_width = target_w;
    m_sw_height = target_h;

    // Upload Y and UV planes into the mapped NV12 texture
    ID3D11DeviceContext* ctx = nullptr;
    m_d3d_device->GetImmediateContext(&ctx);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (!ctx || FAILED(ctx->Map(m_sw_texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        if (ctx) ctx->Release();
        return false;
    }

    BYTE* base = static_cast<BYTE*>(mapped.pData);
    const UINT pitch = mapped.RowPitch;
    for (int y = 0; y < target_h; y++) {
        memcpy(base + (size_t)y * pitch, m_sw_y + (size_t)y * target_w, (size_t)target_w);
    }
    for (int y = 0; y < target_h / 2; y++) {
        memcpy(base + (size_t)pitch * target_h + (size_t)y * pitch, m_sw_uv + (size_t)y * target_w, (size_t)target_w);
    }
    ctx->Unmap(m_sw_texture.Get(), 0);
    ctx->Release();

    frame.texture = m_sw_texture.Get();
    frame.texture_index = 0;
    frame.width = target_w;
    frame.height = target_h;
    frame.pts = src->pts;
    return true;
}

void FFmpegHWDecoder::SeekToStart() {
    if (!m_fmt_ctx || m_video_stream_idx < 0) return;

    // Release VRAM slice held by last decoded frame before flushing
    if (m_hw_frame) av_frame_unref(m_hw_frame);
    if (m_audio_frame) av_frame_unref(m_audio_frame);

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
    if (m_audio_frame) {
        av_frame_unref(m_audio_frame);
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
    m_audio_enabled = false;
    m_max_width = 0;
    m_max_height = 0;
    m_info = VideoInfo{};
}

} // namespace litewp
