#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "video_optimizer.h"
#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <iomanip>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace fs = std::filesystem;

namespace litewp {

VideoOptimizer g_video_optimizer;

VideoOptimizer::VideoOptimizer() = default;

VideoOptimizer::~VideoOptimizer() {
    Cancel();
}

VideoProbeResult VideoOptimizer::Probe(const std::string& input_path) {
    VideoProbeResult res;
    if (input_path.empty()) return res;

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, input_path.c_str(), nullptr, nullptr) < 0) {
        return res;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx);
        return res;
    }

    for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
        AVStream* st = fmt_ctx->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && res.width == 0) {
            res.width = st->codecpar->width;
            res.height = st->codecpar->height;
            if (st->avg_frame_rate.den > 0) {
                res.fps = av_q2d(st->avg_frame_rate);
            }
            const AVCodec* c = avcodec_find_decoder(st->codecpar->codec_id);
            res.codec_name = c ? c->name : "unknown";
            res.valid = (res.width > 0 && res.height > 0);
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            res.has_audio = true;
        }
    }

    if (fmt_ctx->duration > 0) {
        res.duration = static_cast<double>(fmt_ctx->duration) / AV_TIME_BASE;
    }

    avformat_close_input(&fmt_ctx);
    return res;
}

static std::string GetCacheDirectory() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        std::string dir = std::string(path) + "\\LiteWallpaper\\optimized";
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir;
    }
    return "";
}

static uint64_t HashString(const std::string& str) {
    uint64_t hash = 14695981039346656037ULL;
    for (char c : str) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::pair<int, int> VideoOptimizer::CalculateTargetDimensions(int src_w, int src_h, int max_w, int max_h) {
    if (src_w <= 0 || src_h <= 0) return { (max_w > 0 ? (max_w / 2) * 2 : 1920), (max_h > 0 ? (max_h / 2) * 2 : 1080) };
    if (max_w <= 0) max_w = 1920;
    if (max_h <= 0) max_h = 1080;

    double scale = (std::min)(static_cast<double>(max_w) / src_w, static_cast<double>(max_h) / src_h);
    if (scale > 1.0) scale = 1.0;

    int target_w = (static_cast<int>(src_w * scale) / 2) * 2;
    int target_h = (static_cast<int>(src_h * scale) / 2) * 2;
    if (target_w < 128) target_w = 128;
    if (target_h < 128) target_h = 128;
    return { target_w, target_h };
}

std::string VideoOptimizer::GetOptimizedPath(const std::string& input_path, int target_w, int target_h) {
    std::string cache_dir = GetCacheDirectory();
    if (cache_dir.empty()) return "";

    std::string filename = fs::path(input_path).stem().string();
    uint64_t hash = HashString(input_path);

    target_w = (target_w / 2) * 2;
    target_h = (target_h / 2) * 2;

    std::ostringstream ss;
    ss << cache_dir << "\\" << filename << "_" << target_w << "x" << target_h << "_" 
       << std::hex << std::setw(16) << std::setfill('0') << hash << ".mp4";
    return ss.str();
}

bool VideoOptimizer::HasOptimizedCache(const std::string& input_path, int target_w, int target_h) {
    std::string out_path = GetOptimizedPath(input_path, target_w, target_h);
    if (out_path.empty()) return false;
    std::error_code ec;
    return fs::exists(out_path, ec) && fs::file_size(out_path, ec) > 1024;
}

void VideoOptimizer::DeleteOptimizedCache(const std::string& input_path) {
    if (input_path.empty()) return;
    std::string cache_dir = GetCacheDirectory();
    if (cache_dir.empty()) return;

    uint64_t hash = HashString(input_path);
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << hash;
    std::string hash_str = ss.str();

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(cache_dir, ec)) {
        if (entry.is_regular_file()) {
            std::string name = entry.path().filename().string();
            // Strict match: only delete cache files containing the unique hash of this specific video
            if (name.find(hash_str) != std::string::npos) {
                fs::remove(entry.path(), ec);
            }
        }
    }
}

bool VideoOptimizer::StartOptimizeAsync(
    const std::string& input_path,
    int target_w,
    int target_h,
    ProgressCallback on_progress,
    CompleteCallback on_complete
) {
    if (m_running.load()) {
        return false;
    }

    // Align target dimensions to even numbers
    target_w = (target_w / 2) * 2;
    target_h = (target_h / 2) * 2;
    if (target_w < 128) target_w = 128;
    if (target_h < 128) target_h = 128;

    // If cached already exists, immediately complete
    std::string cached = GetOptimizedPath(input_path, target_w, target_h);
    if (HasOptimizedCache(input_path, target_w, target_h)) {
        if (on_progress) on_progress(1.0f, "Using cached optimized video");
        if (on_complete) on_complete(true, cached);
        return true;
    }

    if (m_worker_thread.joinable()) {
        m_worker_thread.join();
    }

    m_running.store(true);
    m_cancel.store(false);
    m_progress.store(0.0f);

    {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        m_current_status = "Starting optimization...";
    }

    m_worker_thread = std::thread(
        &VideoOptimizer::TranscodeWorker,
        this,
        input_path,
        target_w,
        target_h,
        on_progress,
        on_complete
    );

    return true;
}

void VideoOptimizer::Cancel() {
    m_cancel.store(true);
    if (m_worker_thread.joinable()) {
        m_worker_thread.join();
    }
    m_running.store(false);
}

bool VideoOptimizer::IsRunning() const {
    return m_running.load();
}

float VideoOptimizer::GetProgress() const {
    return m_progress.load();
}

std::string VideoOptimizer::GetCurrentStatus() const {
    std::lock_guard<std::mutex> lock(m_status_mutex);
    return m_current_status;
}

void VideoOptimizer::TranscodeWorker(
    std::string input_path,
    int target_w,
    int target_h,
    ProgressCallback on_progress,
    CompleteCallback on_complete
) {
    std::string output_path = GetOptimizedPath(input_path, target_w, target_h);
    std::string temp_output_path = output_path + ".tmp.mp4";

    auto report = [this, on_progress](float p, const std::string& msg) {
        m_progress.store(p);
        {
            std::lock_guard<std::mutex> lock(m_status_mutex);
            m_current_status = msg;
        }
        if (on_progress) on_progress(p, msg);
    };

    auto finish = [this, on_complete, &temp_output_path](bool success, const std::string& path) {
        std::error_code ec;
        if (!success) {
            fs::remove(temp_output_path, ec);
        }
        m_running.store(false);
        if (on_complete) on_complete(success, path);
    };

    report(0.02f, "Opening source video...");

    AVFormatContext* in_fmt = nullptr;
    if (avformat_open_input(&in_fmt, input_path.c_str(), nullptr, nullptr) < 0) {
        report(0.0f, "Failed to open source file");
        finish(false, "");
        return;
    }

    if (avformat_find_stream_info(in_fmt, nullptr) < 0) {
        avformat_close_input(&in_fmt);
        report(0.0f, "Failed to find stream info");
        finish(false, "");
        return;
    }

    int in_video_idx = -1;
    int in_audio_idx = -1;
    for (unsigned int i = 0; i < in_fmt->nb_streams; ++i) {
        if (in_fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && in_video_idx < 0) {
            in_video_idx = static_cast<int>(i);
        } else if (in_fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && in_audio_idx < 0) {
            in_audio_idx = static_cast<int>(i);
        }
    }

    if (in_video_idx < 0) {
        avformat_close_input(&in_fmt);
        report(0.0f, "No video stream found");
        finish(false, "");
        return;
    }

    AVStream* in_vstream = in_fmt->streams[in_video_idx];
    const AVCodec* vdec = avcodec_find_decoder(in_vstream->codecpar->codec_id);
    if (!vdec) {
        avformat_close_input(&in_fmt);
        report(0.0f, "Decoder not found");
        finish(false, "");
        return;
    }

    AVCodecContext* dec_ctx = avcodec_alloc_context3(vdec);
    avcodec_parameters_to_context(dec_ctx, in_vstream->codecpar);
    dec_ctx->thread_count = 0; // Use all available CPU cores for fast transcode
    if (avcodec_open2(dec_ctx, vdec, nullptr) < 0) {
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt);
        report(0.0f, "Failed to initialize decoder");
        finish(false, "");
        return;
    }

    // Setup Output Format
    AVFormatContext* out_fmt = nullptr;
    if (avformat_alloc_output_context2(&out_fmt, nullptr, nullptr, temp_output_path.c_str()) < 0 || !out_fmt) {
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt);
        report(0.0f, "Failed to allocate output context");
        finish(false, "");
        return;
    }

    // Find H.264 Encoder
    const AVCodec* venc = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!venc) {
        venc = avcodec_find_encoder_by_name("libx264");
    }
    if (!venc) {
        avformat_free_context(out_fmt);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt);
        report(0.0f, "H.264 encoder not found");
        finish(false, "");
        return;
    }

    AVStream* out_vstream = avformat_new_stream(out_fmt, nullptr);
    AVCodecContext* enc_ctx = avcodec_alloc_context3(venc);

    int src_w = in_vstream->codecpar->width;
    int src_h = in_vstream->codecpar->height;
    auto [out_w, out_h] = CalculateTargetDimensions(src_w, src_h, target_w, target_h);

    double in_fps = (in_vstream->avg_frame_rate.den > 0) ? av_q2d(in_vstream->avg_frame_rate) : 30.0;
    if (in_fps <= 0.0) in_fps = 30.0;
    double out_fps = (in_fps > 60.0) ? 60.0 : in_fps;
    int out_fps_int = static_cast<int>(out_fps + 0.5);
    if (out_fps_int < 1) out_fps_int = 30;

    enc_ctx->width = out_w;
    enc_ctx->height = out_h;
    enc_ctx->sample_aspect_ratio = AVRational{1, 1};
    enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    enc_ctx->time_base = AVRational{1, out_fps_int};
    enc_ctx->framerate = AVRational{out_fps_int, 1};
    
    // Balanced bitrate for wallpaper quality at low file size (e.g. 5 Mbps for 1080p)
    enc_ctx->bit_rate = static_cast<int64_t>(out_w) * out_h * 2;
    if (enc_ctx->bit_rate < 2000000) enc_ctx->bit_rate = 2000000;
    if (enc_ctx->bit_rate > 8000000) enc_ctx->bit_rate = 8000000;

    enc_ctx->gop_size = static_cast<int>(out_fps_int * 2);
    enc_ctx->max_b_frames = 2;
    enc_ctx->thread_count = 0;

    if (out_fmt->oformat->flags & AVFMT_GLOBALHEADER) {
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    av_opt_set(enc_ctx->priv_data, "preset", "veryfast", 0);
    av_opt_set(enc_ctx->priv_data, "tune", "film", 0);

    if (avcodec_open2(enc_ctx, venc, nullptr) < 0) {
        avcodec_free_context(&enc_ctx);
        avformat_free_context(out_fmt);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt);
        report(0.0f, "Failed to open video encoder");
        finish(false, "");
        return;
    }

    avcodec_parameters_from_context(out_vstream->codecpar, enc_ctx);
    out_vstream->time_base = enc_ctx->time_base;
    out_vstream->r_frame_rate = enc_ctx->framerate;
    out_vstream->avg_frame_rate = enc_ctx->framerate;

    // Copy Audio Stream if available
    AVStream* out_astream = nullptr;
    if (in_audio_idx >= 0) {
        out_astream = avformat_new_stream(out_fmt, nullptr);
        avcodec_parameters_copy(out_astream->codecpar, in_fmt->streams[in_audio_idx]->codecpar);
        out_astream->codecpar->codec_tag = 0;
        out_astream->time_base = in_fmt->streams[in_audio_idx]->time_base;
    }

    // Open Output File
    if (!(out_fmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_fmt->pb, temp_output_path.c_str(), AVIO_FLAG_WRITE) < 0) {
            avcodec_free_context(&enc_ctx);
            avformat_free_context(out_fmt);
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&in_fmt);
            report(0.0f, "Failed to create output file");
            finish(false, "");
            return;
        }
    }

    if (avformat_write_header(out_fmt, nullptr) < 0) {
        if (out_fmt->pb) avio_closep(&out_fmt->pb);
        avcodec_free_context(&enc_ctx);
        avformat_free_context(out_fmt);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt);
        report(0.0f, "Failed to write header");
        finish(false, "");
        return;
    }

    // Allocate conversion frames & scaler
    AVFrame* in_frame = av_frame_alloc();
    AVFrame* out_frame = av_frame_alloc();
    out_frame->format = enc_ctx->pix_fmt;
    out_frame->width = out_w;
    out_frame->height = out_h;
    av_frame_get_buffer(out_frame, 32);

    AVPacket* in_pkt = av_packet_alloc();
    AVPacket* out_pkt = av_packet_alloc();
    SwsContext* sws_ctx = nullptr;

    int64_t total_duration = in_fmt->duration;
    int64_t current_pts = 0;
    int64_t out_frame_idx = 0;
    double next_frame_time = 0.0;
    double out_frame_interval = 1.0 / out_fps;

    report(0.05f, "Optimizing video...");

    bool success = true;
    while (!m_cancel.load() && av_read_frame(in_fmt, in_pkt) >= 0) {
        if (in_pkt->stream_index == in_video_idx) {
            if (avcodec_send_packet(dec_ctx, in_pkt) >= 0) {
                while (avcodec_receive_frame(dec_ctx, in_frame) >= 0) {
                    double cur_pts_sec = (in_frame->best_effort_timestamp != AV_NOPTS_VALUE ? in_frame->best_effort_timestamp : in_frame->pts) * av_q2d(in_vstream->time_base);
                    if (in_fps > out_fps && cur_pts_sec < (next_frame_time - (out_frame_interval * 0.45))) {
                        av_frame_unref(in_frame);
                        continue;
                    }

                    if (!sws_ctx) {
                        sws_ctx = sws_getContext(
                            in_frame->width, in_frame->height, static_cast<AVPixelFormat>(in_frame->format),
                            out_w, out_h, enc_ctx->pix_fmt,
                            SWS_BILINEAR, nullptr, nullptr, nullptr
                        );
                    }

                    if (sws_ctx) {
                        sws_scale(
                            sws_ctx,
                            in_frame->data, in_frame->linesize,
                            0, in_frame->height,
                            out_frame->data, out_frame->linesize
                        );

                        out_frame->pts = out_frame_idx;
                        out_frame_idx++;
                        next_frame_time += out_frame_interval;
                        current_pts = in_frame->pts;

                        if (avcodec_send_frame(enc_ctx, out_frame) >= 0) {
                            while (avcodec_receive_packet(enc_ctx, out_pkt) >= 0) {
                                av_packet_rescale_ts(out_pkt, enc_ctx->time_base, out_vstream->time_base);
                                out_pkt->stream_index = out_vstream->index;
                                av_interleaved_write_frame(out_fmt, out_pkt);
                                av_packet_unref(out_pkt);
                            }
                        }
                    }
                    av_frame_unref(in_frame);
                }
            }

            if (total_duration > 0 && current_pts > 0) {
                double pts_sec = current_pts * av_q2d(in_vstream->time_base);
                double total_sec = static_cast<double>(total_duration) / AV_TIME_BASE;
                float pct = static_cast<float>(std::clamp(pts_sec / (total_sec > 0 ? total_sec : 1.0), 0.05, 0.95));
                report(pct, "Optimizing video (" + std::to_string(static_cast<int>(pct * 100)) + "%)...");
            }
        } else if (in_pkt->stream_index == in_audio_idx && out_astream) {
            av_packet_rescale_ts(in_pkt, in_fmt->streams[in_audio_idx]->time_base, out_astream->time_base);
            in_pkt->stream_index = out_astream->index;
            av_interleaved_write_frame(out_fmt, in_pkt);
        }
        av_packet_unref(in_pkt);
    }

    if (m_cancel.load()) {
        success = false;
    } else {
        // Flush Encoder
        avcodec_send_frame(enc_ctx, nullptr);
        while (avcodec_receive_packet(enc_ctx, out_pkt) >= 0) {
            av_packet_rescale_ts(out_pkt, enc_ctx->time_base, out_vstream->time_base);
            out_pkt->stream_index = out_vstream->index;
            av_interleaved_write_frame(out_fmt, out_pkt);
            av_packet_unref(out_pkt);
        }
        av_write_trailer(out_fmt);
    }

    // Cleanup resources
    if (sws_ctx) sws_freeContext(sws_ctx);
    av_frame_free(&in_frame);
    av_frame_free(&out_frame);
    av_packet_free(&in_pkt);
    av_packet_free(&out_pkt);

    if (out_fmt->pb) avio_closep(&out_fmt->pb);
    avcodec_free_context(&enc_ctx);
    avformat_free_context(out_fmt);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&in_fmt);

    if (success) {
        std::error_code ec;
        fs::rename(temp_output_path, output_path, ec);
        if (!ec) {
            report(1.0f, "Optimization complete!");
            finish(true, output_path);
        } else {
            report(0.0f, "Failed to save optimized video");
            finish(false, "");
        }
    } else {
        report(0.0f, "Optimization cancelled");
        finish(false, "");
    }
}

} // namespace litewp
