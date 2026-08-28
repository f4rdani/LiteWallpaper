#include "thumbnail_manager.h"
#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

namespace fs = std::filesystem;

namespace litewp {

static uint64_t HashString(const std::string& str) {
    uint64_t hash = 14695981039346656037ULL;
    for (char c : str) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

ThumbnailManager& ThumbnailManager::Instance() {
    static ThumbnailManager s_instance;
    return s_instance;
}

ThumbnailManager::ThumbnailManager() {
    m_worker_thread = std::thread(&ThumbnailManager::WorkerLoop, this);
}

ThumbnailManager::~ThumbnailManager() {
    m_stop_worker = true;
    if (m_worker_thread.joinable()) {
        m_worker_thread.join();
    }
    ReleaseTextures();
}

void ThumbnailManager::ReleaseTextures() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_srv_map.clear();
    m_requested.clear();
    m_pending_uploads.clear();
}

std::string ThumbnailManager::GetCacheDirectory() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        std::string dir = std::string(path) + "\\LiteWallpaper\\thumbnails";
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir;
    }
    return "";
}

std::string ThumbnailManager::GetThumbnailPath(const std::string& video_path) {
    std::string cache_dir = GetCacheDirectory();
    if (cache_dir.empty()) return "";

    std::string filename = fs::path(video_path).stem().string();
    uint64_t hash = HashString(video_path);

    std::ostringstream ss;
    ss << cache_dir << "\\" << filename << "_" << std::hex << std::setw(16) << std::setfill('0') << hash << ".thumb";
    return ss.str();
}

void ThumbnailManager::DeleteThumbnailCache(const std::string& video_path) {
    if (video_path.empty()) return;
    std::string cache_dir = GetCacheDirectory();
    if (cache_dir.empty()) return;

    uint64_t hash = HashString(video_path);
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << hash;
    std::string hash_str = ss.str();

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(cache_dir, ec)) {
        if (entry.is_regular_file()) {
            std::string name = entry.path().filename().string();
            if (name.find(hash_str) != std::string::npos) {
                fs::remove(entry.path(), ec);
            }
        }
    }
}

void ThumbnailManager::CleanOrphanThumbnails(const std::vector<std::string>& active_videos) {
    std::string cache_dir = GetCacheDirectory();
    if (cache_dir.empty()) return;

    std::vector<std::string> active_hashes;
    for (const auto& v : active_videos) {
        if (!v.empty()) {
            uint64_t hash = HashString(v);
            std::ostringstream ss;
            ss << std::hex << std::setw(16) << std::setfill('0') << hash;
            active_hashes.push_back(ss.str());
        }
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(cache_dir, ec)) {
        if (entry.is_regular_file()) {
            std::string name = entry.path().filename().string();
            bool is_active = false;
            for (const auto& h : active_hashes) {
                if (name.find(h) != std::string::npos) {
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

bool ThumbnailManager::ExtractFrameToBGRA(const std::string& video_path, std::vector<uint8_t>& out_bgra) {
    out_bgra.assign(THUMB_WIDTH * THUMB_HEIGHT * 4, 0);

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, video_path.c_str(), nullptr, nullptr) < 0) {
        return false;
    }

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        avformat_close_input(&fmt_ctx);
        return false;
    }

    int video_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = (int)i;
            break;
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

    if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0 || avcodec_open2(codec_ctx, decoder, nullptr) < 0) {
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&fmt_ctx);
        return false;
    }

    // Seek to ~1 sec in or 0 for short clips
    av_seek_frame(fmt_ctx, video_stream_idx, 1 * AV_TIME_BASE, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(codec_ctx);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgb_frame = av_frame_alloc();
    rgb_frame->format = AV_PIX_FMT_BGRA;
    rgb_frame->width = THUMB_WIDTH;
    rgb_frame->height = THUMB_HEIGHT;
    av_frame_get_buffer(rgb_frame, 32);

    SwsContext* sws_ctx = nullptr;
    bool found = false;

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_stream_idx) {
            if (avcodec_send_packet(codec_ctx, pkt) >= 0) {
                if (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                    sws_ctx = sws_getContext(
                        frame->width, frame->height, (AVPixelFormat)frame->format,
                        THUMB_WIDTH, THUMB_HEIGHT, AV_PIX_FMT_BGRA,
                        SWS_BILINEAR, nullptr, nullptr, nullptr
                    );
                    if (sws_ctx) {
                        sws_scale(
                            sws_ctx,
                            frame->data, frame->linesize,
                            0, frame->height,
                            rgb_frame->data, rgb_frame->linesize
                        );

                        for (int y = 0; y < THUMB_HEIGHT; ++y) {
                            memcpy(
                                out_bgra.data() + (y * THUMB_WIDTH * 4),
                                rgb_frame->data[0] + (y * rgb_frame->linesize[0]),
                                THUMB_WIDTH * 4
                            );
                        }
                        found = true;
                        sws_freeContext(sws_ctx);
                    }
                    av_frame_unref(frame);
                    av_packet_unref(pkt);
                    break;
                }
            }
        }
        av_packet_unref(pkt);
    }

    av_frame_free(&frame);
    av_frame_free(&rgb_frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return found;
}

void ThumbnailManager::WorkerLoop() {
    while (!m_stop_worker.load()) {
        std::string current_video;
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            if (!m_extract_queue.empty()) {
                current_video = m_extract_queue.front();
                m_extract_queue.pop_front();
            }
        }

        if (current_video.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::string thumb_file = GetThumbnailPath(current_video);
        std::vector<uint8_t> bgra_data;
        bool loaded = false;

        // 1. Try reading from disk cache
        if (!thumb_file.empty() && fs::exists(thumb_file)) {
            std::ifstream fin(thumb_file, std::ios::binary);
            if (fin.is_open()) {
                bgra_data.resize(THUMB_WIDTH * THUMB_HEIGHT * 4);
                fin.read(reinterpret_cast<char*>(bgra_data.data()), bgra_data.size());
                if (fin.gcount() == (std::streamsize)bgra_data.size()) {
                    loaded = true;
                }
            }
        }

        // 2. If not on disk, extract from video file and save
        if (!loaded) {
            if (ExtractFrameToBGRA(current_video, bgra_data)) {
                loaded = true;
                if (!thumb_file.empty()) {
                    std::ofstream fout(thumb_file, std::ios::binary);
                    if (fout.is_open()) {
                        fout.write(reinterpret_cast<const char*>(bgra_data.data()), bgra_data.size());
                    }
                }
            }
        }

        // 3. Queue for D3D11 upload on UI thread
        if (loaded && !bgra_data.empty()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending_uploads.push_back({ current_video, std::move(bgra_data) });
        }
    }
}

void ThumbnailManager::Update(ID3D11Device* device) {
    if (!device) return;

    std::deque<PendingUpload> to_upload;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        to_upload.swap(m_pending_uploads);
    }

    for (const auto& item : to_upload) {
        if (item.bgra_data.size() != (size_t)(THUMB_WIDTH * THUMB_HEIGHT * 4)) continue;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = THUMB_WIDTH;
        desc.Height = THUMB_HEIGHT;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA subData = {};
        subData.pSysMem = item.bgra_data.data();
        subData.SysMemPitch = THUMB_WIDTH * 4;

        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(device->CreateTexture2D(&desc, &subData, &tex))) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = desc.Format;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;

            ComPtr<ID3D11ShaderResourceView> srv;
            if (SUCCEEDED(device->CreateShaderResourceView(tex.Get(), &srvDesc, &srv))) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_srv_map[item.video_path] = srv;
            }
        }
    }
}

ID3D11ShaderResourceView* ThumbnailManager::GetThumbnailSRV(ID3D11Device* device, const std::string& video_path) {
    if (video_path.empty()) return nullptr;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_srv_map.find(video_path);
        if (it != m_srv_map.end()) {
            return it->second.Get();
        }

        if (m_requested.find(video_path) == m_requested.end()) {
            m_requested[video_path] = true;
            std::lock_guard<std::mutex> qlock(m_queue_mutex);
            m_extract_queue.push_back(video_path);
        }
    }

    return nullptr;
}

} // namespace litewp
