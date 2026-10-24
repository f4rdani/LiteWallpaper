#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "video_optimizer.h"
#include "win_utils.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <unordered_map>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace fs = std::filesystem;

namespace miniwp {

VideoOptimizer g_video_optimizer;

VideoOptimizer::VideoOptimizer() = default;

VideoOptimizer::~VideoOptimizer() {
    Cancel();
}

namespace {
struct ProbeCacheEntry {
    uintmax_t file_size = 0;
    fs::file_time_type mtime{};
    VideoProbeResult result;
};

std::mutex g_probe_cache_mutex;
std::unordered_map<std::string, ProbeCacheEntry> g_probe_cache;

const std::string& GetCacheDirectory() {
    static const std::string s_dir = GetAppDataSubDirUtf8(L"optimized");
    return s_dir;
}
} // namespace

VideoProbeResult VideoOptimizer::Probe(const std::string& input_path) {
    VideoProbeResult res;
    if (input_path.empty()) return res;

    fs::path fspath = PathFromUtf8(input_path);
    std::error_code fs_ec;
    uintmax_t file_size = fs::file_size(fspath, fs_ec);
    if (fs_ec) return res;
    fs::file_time_type mtime = fs::last_write_time(fspath, fs_ec);
    if (fs_ec) return res;

    {
        std::lock_guard<std::mutex> lock(g_probe_cache_mutex);
        auto it = g_probe_cache.find(input_path);
        if (it != g_probe_cache.end() &&
            it->second.file_size == file_size && it->second.mtime == mtime) {
            return it->second.result;
        }
    }

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, input_path.c_str(), nullptr, nullptr) >= 0) {
        fmt_ctx->flags |= AVFMT_FLAG_NOBUFFER;
        fmt_ctx->probesize = 64 * 1024;
        fmt_ctx->max_analyze_duration = 500000;
        if (avformat_find_stream_info(fmt_ctx, nullptr) >= 0) {
            for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
                AVStream* st = fmt_ctx->streams[i];
                if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && res.width == 0) {
                    res.width = st->codecpar->width;
                    res.height = st->codecpar->height;
                    if (st->avg_frame_rate.den > 0) {
                        res.fps = av_q2d(st->avg_frame_rate);
                    }
                    const AVCodec* c = avcodec_find_decoder(st->codecpar->codec_id);
                    res.codec_name = (c && c->name) ? c->name : "unknown";
                    res.valid = (res.width > 0 && res.height > 0);
                } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                    res.has_audio = true;
                }
            }

            if (fmt_ctx->duration > 0) {
                res.duration = static_cast<double>(fmt_ctx->duration) / AV_TIME_BASE;
            }
        }
        avformat_close_input(&fmt_ctx);
    }

    {
        std::lock_guard<std::mutex> lock(g_probe_cache_mutex);
        g_probe_cache[input_path] = ProbeCacheEntry{ file_size, mtime, res };
    }
    return res;
}

std::pair<int, int> VideoOptimizer::CalculateTargetDimensions(int src_w, int src_h, int max_w, int max_h, int crop_mode) {
    if (src_w <= 0 || src_h <= 0) return { (max_w > 0 ? (max_w / 2) * 2 : 1920), (max_h > 0 ? (max_h / 2) * 2 : 1080) };
    if (max_w <= 0) max_w = 1920;
    if (max_h <= 0) max_h = 1080;

    max_w = (max_w / 2) * 2;
    max_h = (max_h / 2) * 2;

    if (crop_mode == 1) {
        return { max_w, max_h };
    }

    double scale = (std::min)(static_cast<double>(max_w) / src_w, static_cast<double>(max_h) / src_h);
    if (scale > 1.0) scale = 1.0;

    int target_w = (static_cast<int>(src_w * scale) / 2) * 2;
    int target_h = (static_cast<int>(src_h * scale) / 2) * 2;
    if (target_w < 128) target_w = 128;
    if (target_h < 128) target_h = 128;
    return { target_w, target_h };
}

std::string VideoOptimizer::GetOptimizedPath(const std::string& input_path, int target_w, int target_h) {
    const std::string& cache_dir = GetCacheDirectory();
    if (cache_dir.empty()) return "";

    std::string filename = PathToUtf8(PathFromUtf8(input_path).stem());
    std::string hash_hex = Fnv1a64Hex(input_path);

    target_w = (target_w / 2) * 2;
    target_h = (target_h / 2) * 2;

    return cache_dir + "\\" + filename + "_" + std::to_string(target_w) + "x" +
           std::to_string(target_h) + "_" + hash_hex + ".mp4";
}

bool VideoOptimizer::HasOptimizedCache(const std::string& input_path, int target_w, int target_h) {
    std::string out_path = GetOptimizedPath(input_path, target_w, target_h);
    if (out_path.empty()) return false;
    fs::path fspath = PathFromUtf8(out_path);
    std::error_code ec;
    return fs::exists(fspath, ec) && !ec && fs::file_size(fspath, ec) > 1024 && !ec;
}

void VideoOptimizer::DeleteOptimizedCache(const std::string& input_path) {
    if (input_path.empty()) return;
    const std::string& cache_dir = GetCacheDirectory();
    if (cache_dir.empty()) return;

    std::string hash_str = Fnv1a64Hex(input_path);
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(PathFromUtf8(cache_dir), ec)) {
        if (entry.is_regular_file(ec)) {
            std::string name = PathToUtf8(entry.path().filename());
            if (name.find(hash_str) != std::string::npos) {
                fs::remove(entry.path(), ec);
            }
        }
    }
}

void VideoOptimizer::CleanOrphanCaches(const std::vector<std::string>& active_videos) {
    const std::string& cache_dir = GetCacheDirectory();
    if (cache_dir.empty()) return;

    std::vector<std::string> active_hashes;
    active_hashes.reserve(active_videos.size());
    for (const auto& v : active_videos) {
        if (!v.empty()) {
            active_hashes.push_back(Fnv1a64Hex(v));
        }
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(PathFromUtf8(cache_dir), ec)) {
        if (entry.is_regular_file(ec)) {
            std::string name = PathToUtf8(entry.path().filename());
            bool is_active = false;
            for (const auto& h : active_hashes) {
                if (name.find(h) != std::string::npos || PathsEqualNormalized(PathToUtf8(entry.path()), name)) {
                    is_active = true;
                    break;
                }
            }
            // Also preserve if the cached .mp4 path itself is directly in active_videos
            if (!is_active) {
                std::string full_utf8 = PathToUtf8(entry.path());
                for (const auto& v : active_videos) {
                    if (PathsEqualNormalized(full_utf8, v)) {
                        is_active = true;
                        break;
                    }
                }
            }
            if (!is_active) {
                fs::remove(entry.path(), ec);
            }
        }
    }
}

bool VideoOptimizer::StartOptimizeAsync(
    const std::string& input_path,
    int target_w,
    int target_h,
    int crop_mode,
    ProgressCallback on_progress,
    CompleteCallback on_complete
) {
    if (m_running.load()) {
        return false;
    }

    target_w = (target_w / 2) * 2;
    target_h = (target_h / 2) * 2;
    if (target_w < 128) target_w = 128;
    if (target_h < 128) target_h = 128;

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
        crop_mode,
        std::move(on_progress),
        std::move(on_complete)
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
    int crop_mode,
    ProgressCallback on_progress,
    CompleteCallback on_complete
) {
    std::string output_path = GetOptimizedPath(input_path, target_w, target_h);
    std::string temp_output_path = output_path + ".tmp.mp4";

    auto report = [this, &on_progress](float p, const std::string& msg) {
        m_progress.store(p);
        {
            std::lock_guard<std::mutex> lock(m_status_mutex);
            m_current_status = msg;
        }
        if (on_progress) on_progress(p, msg);
    };

    auto finish = [this, &on_complete, &temp_output_path](bool success, const std::string& path) {
        std::error_code ec;
        if (!success) {
            fs::remove(PathFromUtf8(temp_output_path), ec);
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
    int hw_threads = static_cast<int>(std::thread::hardware_concurrency());
    dec_ctx->thread_count = std::clamp(hw_threads / 2, 2, 6);
    if (avcodec_open2(dec_ctx, vdec, nullptr) < 0) {
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt);
        report(0.0f, "Failed to initialize decoder");
        finish(false, "");
        return;
    }

    AVFormatContext* out_fmt = nullptr;
    if (avformat_alloc_output_context2(&out_fmt, nullptr, nullptr, temp_output_path.c_str()) < 0 || !out_fmt) {
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt);
        report(0.0f, "Failed to allocate output context");
        finish(false, "");
        return;
    }

    int src_w = in_vstream->codecpar->width;
    int src_h = in_vstream->codecpar->height;
    auto [out_w, out_h] = CalculateTargetDimensions(src_w, src_h, target_w, target_h, crop_mode);

    int crop_w = src_w;
    int crop_h = src_h;
    int crop_x = 0;
    int crop_y = 0;

    if (crop_mode == 1 && src_w > 0 && src_h > 0 && out_w > 0 && out_h > 0) {
        double scale = (std::max)(static_cast<double>(out_w) / src_w, static_cast<double>(out_h) / src_h);
        crop_w = (static_cast<int>(out_w / scale) / 2) * 2;
        crop_h = (static_cast<int>(out_h / scale) / 2) * 2;
        if (crop_w > src_w) crop_w = src_w;
        if (crop_h > src_h) crop_h = src_h;
        crop_x = ((src_w - crop_w) / 4) * 2;
        crop_y = ((src_h - crop_h) / 4) * 2;
    }

    double in_fps = (in_vstream->avg_frame_rate.den > 0) ? av_q2d(in_vstream->avg_frame_rate) : 30.0;
    if (in_fps <= 0.0) in_fps = 30.0;
    double out_fps = (in_fps > 60.0) ? 60.0 : in_fps;
    int out_fps_int = static_cast<int>(out_fps + 0.5);
    if (out_fps_int < 1) out_fps_int = 30;

    struct EncoderCandidate {
        const char* name;
        const char* display_name;
    };

    static constexpr EncoderCandidate kCandidates[] = {
        { "h264_nvenc", "NVIDIA NVENC (GPU)" },
        { "h264_qsv",   "Intel QuickSync (GPU)" },
        { "h264_amf",   "AMD AMF (GPU)" },
        { "h264_mf",    "MediaFoundation (GPU)" },
        { "libx264",    "Lanczos High-Fidelity Engine (CPU)" }
    };

    const AVCodec* venc = nullptr;
    AVCodecContext* enc_ctx = nullptr;
    std::string active_encoder_display = "High-Fidelity Engine";

    for (const auto& cand : kCandidates) {
        const AVCodec* candidate_codec = avcodec_find_encoder_by_name(cand.name);
        if (!candidate_codec) continue;

        AVCodecContext* test_ctx = avcodec_alloc_context3(candidate_codec);
        if (!test_ctx) continue;

        test_ctx->width = out_w;
        test_ctx->height = out_h;
        test_ctx->sample_aspect_ratio = AVRational{1, 1};
        test_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        test_ctx->time_base = AVRational{1, out_fps_int};
        test_ctx->framerate = AVRational{out_fps_int, 1};
        test_ctx->gop_size = out_fps_int * 2;
        test_ctx->max_b_frames = 2;
        test_ctx->thread_count = (std::strcmp(cand.name, "libx264") == 0) ? std::clamp(hw_threads, 2, 8) : 1;

        if (out_fmt->oformat->flags & AVFMT_GLOBALHEADER) {
            test_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        if (std::strcmp(cand.name, "h264_nvenc") == 0) {
            av_opt_set(test_ctx->priv_data, "preset", "p6", 0);
            av_opt_set(test_ctx->priv_data, "tune", "hq", 0);
            av_opt_set(test_ctx->priv_data, "rc", "vbr", 0);
            av_opt_set_int(test_ctx->priv_data, "cq", 19, 0);
            av_opt_set(test_ctx->priv_data, "profile", "high", 0);
        } else if (std::strcmp(cand.name, "h264_qsv") == 0) {
            av_opt_set(test_ctx->priv_data, "preset", "veryfast", 0);
            test_ctx->global_quality = 20 * FF_QP2LAMBDA;
        } else if (std::strcmp(cand.name, "h264_amf") == 0) {
            av_opt_set(test_ctx->priv_data, "quality", "quality", 0);
            av_opt_set(test_ctx->priv_data, "rc", "cqp", 0);
            av_opt_set_int(test_ctx->priv_data, "qp_i", 19, 0);
            av_opt_set_int(test_ctx->priv_data, "qp_p", 20, 0);
        } else if (std::strcmp(cand.name, "h264_mf") == 0) {
            test_ctx->bit_rate = static_cast<int64_t>(out_w) * out_h * 4;
            if (test_ctx->bit_rate < 4000000) test_ctx->bit_rate = 4000000;
        } else if (std::strcmp(cand.name, "libx264") == 0) {
            av_opt_set(test_ctx->priv_data, "preset", "fast", 0);
            av_opt_set(test_ctx->priv_data, "crf", "19", 0);
            av_opt_set(test_ctx->priv_data, "tune", "film", 0);
        }

        if (avcodec_open2(test_ctx, candidate_codec, nullptr) >= 0) {
            venc = candidate_codec;
            enc_ctx = test_ctx;
            active_encoder_display = cand.display_name;
            break;
        }
        avcodec_free_context(&test_ctx);
    }

    if (!enc_ctx || !venc) {
        avformat_free_context(out_fmt);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_fmt);
        report(0.0f, "Failed to initialize any H.264 video encoder");
        finish(false, "");
        return;
    }

    AVStream* out_vstream = avformat_new_stream(out_fmt, nullptr);
    avcodec_parameters_from_context(out_vstream->codecpar, enc_ctx);
    out_vstream->time_base = enc_ctx->time_base;
    out_vstream->r_frame_rate = enc_ctx->framerate;
    out_vstream->avg_frame_rate = enc_ctx->framerate;

    AVStream* out_astream = nullptr;
    if (in_audio_idx >= 0) {
        out_astream = avformat_new_stream(out_fmt, nullptr);
        avcodec_parameters_copy(out_astream->codecpar, in_fmt->streams[in_audio_idx]->codecpar);
        out_astream->codecpar->codec_tag = 0;
        out_astream->time_base = in_fmt->streams[in_audio_idx]->time_base;
    }

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

    report(0.05f, "Optimizing [" + active_encoder_display + "]...");

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
                            crop_w, crop_h, static_cast<AVPixelFormat>(in_frame->format),
                            out_w, out_h, enc_ctx->pix_fmt,
                            SWS_LANCZOS | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT, nullptr, nullptr, nullptr
                        );
                    }

                    if (sws_ctx) {
                        uint8_t* in_data[4] = { nullptr, nullptr, nullptr, nullptr };
                        int in_linesize[4] = { in_frame->linesize[0], in_frame->linesize[1], in_frame->linesize[2], in_frame->linesize[3] };

                        if (crop_mode == 1 && (crop_x > 0 || crop_y > 0)) {
                            if (in_frame->format == AV_PIX_FMT_NV12) {
                                in_data[0] = in_frame->data[0] + crop_y * in_frame->linesize[0] + crop_x;
                                in_data[1] = in_frame->data[1] + (crop_y / 2) * in_frame->linesize[1] + crop_x;
                            } else if (in_frame->format == AV_PIX_FMT_YUV420P) {
                                in_data[0] = in_frame->data[0] + crop_y * in_frame->linesize[0] + crop_x;
                                in_data[1] = in_frame->data[1] + (crop_y / 2) * in_frame->linesize[1] + (crop_x / 2);
                                in_data[2] = in_frame->data[2] + (crop_y / 2) * in_frame->linesize[2] + (crop_x / 2);
                            } else {
                                in_data[0] = in_frame->data[0];
                                in_data[1] = in_frame->data[1];
                                in_data[2] = in_frame->data[2];
                                in_data[3] = in_frame->data[3];
                            }
                        } else {
                            in_data[0] = in_frame->data[0];
                            in_data[1] = in_frame->data[1];
                            in_data[2] = in_frame->data[2];
                            in_data[3] = in_frame->data[3];
                        }

                        sws_scale(
                            sws_ctx,
                            in_data, in_linesize,
                            0, crop_h,
                            out_frame->data, out_frame->linesize
                        );

                        out_frame->pts = out_frame_idx++;
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
                report(pct, "Optimizing [" + active_encoder_display + "] (" + std::to_string(static_cast<int>(pct * 100)) + "%)...");
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
        avcodec_send_frame(enc_ctx, nullptr);
        while (avcodec_receive_packet(enc_ctx, out_pkt) >= 0) {
            av_packet_rescale_ts(out_pkt, enc_ctx->time_base, out_vstream->time_base);
            out_pkt->stream_index = out_vstream->index;
            av_interleaved_write_frame(out_fmt, out_pkt);
            av_packet_unref(out_pkt);
        }
        av_write_trailer(out_fmt);
    }

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
        fs::path src_tmp = PathFromUtf8(temp_output_path);
        fs::path dst_final = PathFromUtf8(output_path);
        fs::remove(dst_final, ec);
        ec.clear();
        fs::rename(src_tmp, dst_final, ec);
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

} // namespace miniwp
