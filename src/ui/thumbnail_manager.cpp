#include "thumbnail_manager.h"
#include "core/win_utils.h"
#include <windows.h>
#include <mimalloc.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace fs = std::filesystem;

namespace miniwp {

ThumbnailManager& ThumbnailManager::Instance() {
    static ThumbnailManager s_instance;
    return s_instance;
}

ThumbnailManager::~ThumbnailManager() {
    ReleaseTextures();
}

void ThumbnailManager::StopWorker() {
    m_stop_worker.store(true, std::memory_order_release);
    std::thread to_join;
    {
        std::lock_guard<std::mutex> qlock(m_queue_mutex);
        m_extract_queue.clear();
        if (m_worker_thread.joinable()) {
            to_join = std::move(m_worker_thread);
        }
        m_worker_running.store(false, std::memory_order_release);
    }
    if (to_join.joinable()) {
        to_join.join();
    }
    m_stop_worker.store(false, std::memory_order_release);
}

void ThumbnailManager::ReleaseTextures() {
    StopWorker();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_srv_map.clear();
    m_srv_lru.clear();
    m_requested.clear();
    m_pending_uploads.clear();
    m_pending_upload_count.store(0, std::memory_order_relaxed);
}

bool ThumbnailManager::HasPendingWork() const noexcept {
    return m_worker_running.load(std::memory_order_relaxed) ||
           m_pending_upload_count.load(std::memory_order_relaxed) > 0;
}

fs::path ThumbnailManager::GetCacheDirectory() {
    std::string dir_utf8 = GetAppDataSubDirUtf8(L"thumbnails");
    if (dir_utf8.empty()) return {};
    return PathFromUtf8(dir_utf8);
}

fs::path ThumbnailManager::GetThumbnailPath(const std::string& video_path) {
    fs::path cache_dir = GetCacheDirectory();
    if (cache_dir.empty()) return {};

    std::wstring stem = PathFromUtf8(video_path).stem().wstring();
    std::wstring hash_w = Utf8ToWide(Fnv1a64Hex(video_path));
    return cache_dir / (stem + L"_" + hash_w + L".thumb");
}

void ThumbnailManager::DeleteThumbnailCache(const std::string& video_path) {
    if (video_path.empty()) return;
    fs::path cache_dir = GetCacheDirectory();
    if (cache_dir.empty()) return;

    std::wstring hash_w = Utf8ToWide(Fnv1a64Hex(video_path));
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(cache_dir, ec)) {
        if (entry.is_regular_file(ec)) {
            std::wstring name = entry.path().filename().wstring();
            if (name.find(hash_w) != std::wstring::npos) {
                fs::remove(entry.path(), ec);
            }
        }
    }
}

void ThumbnailManager::CleanOrphanThumbnails(const std::vector<std::string>& active_videos) {
    fs::path cache_dir = GetCacheDirectory();
    if (cache_dir.empty()) return;

    std::vector<std::wstring> active_hashes;
    active_hashes.reserve(active_videos.size());
    for (const auto& v : active_videos) {
        if (!v.empty()) {
            active_hashes.push_back(Utf8ToWide(Fnv1a64Hex(v)));
        }
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(cache_dir, ec)) {
        if (entry.is_regular_file(ec)) {
            std::wstring name = entry.path().filename().wstring();
            bool is_active = false;
            for (const auto& h : active_hashes) {
                if (name.find(h) != std::wstring::npos) {
                    is_active = true;
                    break;
                }
            }
            if (!is_active) {
                fs::remove(entry.path(), ec);
            }
        }
    }
}

bool ThumbnailManager::ExtractFrameToBGRA(
    const std::string& video_path,
    std::vector<uint8_t>& out_bgra,
    int width,
    int height,
    double timestamp_sec
) {
    if (width <= 0) width = THUMB_WIDTH;
    if (height <= 0) height = THUMB_HEIGHT;
    out_bgra.assign(static_cast<size_t>(width) * height * 4, 0);

    AVFormatContext* fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) {
        return false;
    }
    fmt_ctx->flags |= AVFMT_FLAG_NOBUFFER;
    fmt_ctx->probesize = 64 * 1024;
    fmt_ctx->max_analyze_duration = 500000;

    if (avformat_open_input(&fmt_ctx, video_path.c_str(), nullptr, nullptr) < 0) {
        return false;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx);
        return false;
    }

    int video_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (video_stream_idx < 0 && fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = static_cast<int>(i);
        } else {
            fmt_ctx->streams[i]->discard = AVDISCARD_ALL;
        }
    }

    if (video_stream_idx < 0) {
        avformat_close_input(&fmt_ctx);
        return false;
    }

    AVCodecParameters* codecpar = fmt_ctx->streams[video_stream_idx]->codecpar;
    const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
    if (!decoder) {
        avformat_close_input(&fmt_ctx);
        return false;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(decoder);
    if (!codec_ctx) {
        avformat_close_input(&fmt_ctx);
        return false;
    }

    if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return false;
    }

    // Bound decoder worker threads to 1 to minimize temporary heap & thread churn
    codec_ctx->thread_count = 1;
    codec_ctx->thread_type = FF_THREAD_SLICE;

    if (avcodec_open2(codec_ctx, decoder, nullptr) < 0) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return false;
    }

    // Calculate stream frame rate and tolerance
    AVRational stream_tb = fmt_ctx->streams[video_stream_idx]->time_base;
    AVRational frame_rate = fmt_ctx->streams[video_stream_idx]->avg_frame_rate;
    if (frame_rate.num <= 0 || frame_rate.den <= 0) {
        frame_rate = fmt_ctx->streams[video_stream_idx]->r_frame_rate;
    }
    double fps = (frame_rate.num > 0 && frame_rate.den > 0)
                     ? (static_cast<double>(frame_rate.num) / frame_rate.den)
                     : 30.0;
    if (fps <= 0.0) fps = 30.0;

    int64_t target_ts = 0;
    if (stream_tb.den > 0) {
        int64_t req_ts = av_rescale_q(
            static_cast<int64_t>((std::max)(0.0, timestamp_sec) * AV_TIME_BASE),
            AV_TIME_BASE_Q,
            stream_tb
        );
        int64_t dur = fmt_ctx->streams[video_stream_idx]->duration;
        if (dur > 0 && req_ts >= dur) {
            target_ts = dur - 1;
        } else {
            target_ts = req_ts;
        }
    }

    // Half frame duration tolerance in stream timebase
    int64_t half_frame_ts = (stream_tb.den > 0 && stream_tb.num > 0)
                                ? static_cast<int64_t>((0.5 / fps) * stream_tb.den / stream_tb.num)
                                : 0;

    av_seek_frame(fmt_ctx, video_stream_idx, target_ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(codec_ctx);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgb_frame = av_frame_alloc();
    rgb_frame->format = AV_PIX_FMT_BGRA;
    rgb_frame->width = width;
    rgb_frame->height = height;
    av_frame_get_buffer(rgb_frame, 32);

    bool found = false;
    int frames_decoded = 0;

    auto ConvertToBGRA = [&](AVFrame* src_frame) {
        SwsContext* sws_ctx = sws_getContext(
            src_frame->width, src_frame->height, static_cast<AVPixelFormat>(src_frame->format),
            width, height, AV_PIX_FMT_BGRA,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        if (sws_ctx) {
            sws_scale(
                sws_ctx,
                src_frame->data, src_frame->linesize,
                0, src_frame->height,
                rgb_frame->data, rgb_frame->linesize
            );

            for (int y = 0; y < height; ++y) {
                std::memcpy(
                    out_bgra.data() + (static_cast<size_t>(y) * width * 4),
                    rgb_frame->data[0] + (static_cast<size_t>(y) * rgb_frame->linesize[0]),
                    static_cast<size_t>(width) * 4
                );
            }
            sws_freeContext(sws_ctx);
            found = true;
        }
    };

    AVFrame* last_decoded_frame = av_frame_alloc();
    bool has_last_frame = false;

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_stream_idx) {
            if (avcodec_send_packet(codec_ctx, pkt) >= 0) {
                while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                    if (frame->width > 0 && frame->height > 0 && !(frame->flags & AV_FRAME_FLAG_CORRUPT)) {
                        frames_decoded++;

                        av_frame_unref(last_decoded_frame);
                        av_frame_ref(last_decoded_frame, frame);
                        has_last_frame = true;

                        int64_t pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                          ? frame->best_effort_timestamp
                                          : frame->pts;

                        // Accurate seek: keep decoding forward toward target timestamp
                        if (pts != AV_NOPTS_VALUE) {
                            if (pts + half_frame_ts < target_ts && frames_decoded < 600) {
                                av_frame_unref(frame);
                                continue;
                            }
                        }

                        ConvertToBGRA(frame);
                        av_frame_unref(frame);
                        break;
                    }
                    av_frame_unref(frame);
                }
                if (found) {
                    av_packet_unref(pkt);
                    break;
                }
            }
        }
        av_packet_unref(pkt);
    }

    if (!found && has_last_frame) {
        ConvertToBGRA(last_decoded_frame);
    }

    av_frame_free(&last_decoded_frame);
    av_frame_free(&frame);
    av_frame_free(&rgb_frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);

    return found;
}

ComPtr<ID3D11ShaderResourceView> ThumbnailManager::CreateSRVFromBGRA(
    ID3D11Device* device,
    const uint8_t* bgra_data,
    int width,
    int height
) {
    if (!device || !bgra_data || width <= 0 || height <= 0) return nullptr;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = bgra_data;
    initData.SysMemPitch = static_cast<UINT>(width * 4);

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(device->CreateTexture2D(&desc, &initData, &tex))) return nullptr;

    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return nullptr;

    return srv;
}

void ThumbnailManager::WorkerLoop() {
    for (;;) {
        std::string current_video;
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            if (m_stop_worker.load(std::memory_order_relaxed) || m_extract_queue.empty()) {
                m_worker_running.store(false, std::memory_order_release);
                break;
            }
            current_video = std::move(m_extract_queue.front());
            m_extract_queue.pop_front();
        }

        if (current_video.empty()) continue;

        fs::path thumb_file = GetThumbnailPath(current_video);
        std::vector<uint8_t> bgra_data;
        bool loaded = false;

        // 1. Try reading from disk cache
        std::error_code ec;
        if (!thumb_file.empty() && fs::exists(thumb_file, ec)) {
            std::ifstream fin(thumb_file, std::ios::binary);
            if (fin.is_open()) {
                bgra_data.resize(THUMB_WIDTH * THUMB_HEIGHT * 4);
                fin.read(reinterpret_cast<char*>(bgra_data.data()), static_cast<std::streamsize>(bgra_data.size()));
                if (fin.gcount() == static_cast<std::streamsize>(bgra_data.size())) {
                    loaded = true;
                }
            }
        }

        // 2. If not on disk, extract from video file and save
        if (!loaded && !m_stop_worker.load(std::memory_order_relaxed)) {
            if (ExtractFrameToBGRA(current_video, bgra_data)) {
                loaded = true;
                if (!thumb_file.empty()) {
                    std::ofstream fout(thumb_file, std::ios::binary);
                    if (fout.is_open()) {
                        fout.write(reinterpret_cast<const char*>(bgra_data.data()), static_cast<std::streamsize>(bgra_data.size()));
                    }
                }
            }
        }

        // 3. Queue for D3D11 upload on UI thread
        if (loaded && !bgra_data.empty() && !m_stop_worker.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending_uploads.push_back({ std::move(current_video), std::move(bgra_data) });
            m_pending_upload_count.store(m_pending_uploads.size(), std::memory_order_relaxed);
        }
    }

    mi_collect(true);
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

void ThumbnailManager::Update(ID3D11Device* device) {
    if (!device) return;

    // Reap completed worker thread handle once queue has drained
    if (!m_worker_running.load(std::memory_order_acquire)) {
        std::thread finished;
        {
            std::lock_guard<std::mutex> qlock(m_queue_mutex);
            if (!m_worker_running.load(std::memory_order_relaxed) && m_worker_thread.joinable()) {
                finished = std::move(m_worker_thread);
            }
        }
        if (finished.joinable()) {
            finished.join();
        }
    }

    std::deque<PendingUpload> to_upload;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pending_uploads.empty()) return;
        to_upload.swap(m_pending_uploads);
        m_pending_upload_count.store(0, std::memory_order_relaxed);
    }

    for (const auto& item : to_upload) {
        if (item.bgra_data.size() != static_cast<size_t>(THUMB_WIDTH * THUMB_HEIGHT * 4)) continue;

        ComPtr<ID3D11ShaderResourceView> srv = CreateSRVFromBGRA(
            device,
            item.bgra_data.data(),
            THUMB_WIDTH,
            THUMB_HEIGHT
        );
        if (srv) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_srv_map[item.video_path] = srv;
            m_srv_lru.remove(item.video_path);
            m_srv_lru.push_front(item.video_path);

            // Evict least-recently-used thumbnails beyond the cap.
            while (m_srv_lru.size() > MAX_RESIDENT_THUMBNAILS) {
                const std::string& victim = m_srv_lru.back();
                m_srv_map.erase(victim);
                m_requested.erase(victim);
                m_srv_lru.pop_back();
            }
        }
    }
}

ID3D11ShaderResourceView* ThumbnailManager::GetThumbnailSRV(ID3D11Device* /*device*/, const std::string& video_path) {
    if (video_path.empty()) return nullptr;

    bool should_enqueue = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_srv_map.find(video_path);
        if (it != m_srv_map.end()) {
            m_srv_lru.remove(video_path);
            m_srv_lru.push_front(video_path);
            return it->second.Get();
        }

        if (m_requested.find(video_path) == m_requested.end()) {
            m_requested[video_path] = true;
            should_enqueue = true;
        }
    }

    if (should_enqueue) {
        std::thread old_finished;
        {
            std::lock_guard<std::mutex> qlock(m_queue_mutex);
            m_extract_queue.push_back(video_path);
            if (!m_worker_running.load(std::memory_order_relaxed)) {
                if (m_worker_thread.joinable()) {
                    old_finished = std::move(m_worker_thread);
                }
                m_stop_worker.store(false, std::memory_order_relaxed);
                m_worker_running.store(true, std::memory_order_release);
                m_worker_thread = std::thread(&ThumbnailManager::WorkerLoop, this);
            }
        }
        if (old_finished.joinable()) {
            old_finished.join();
        }
    }

    return nullptr;
}

} // namespace miniwp
