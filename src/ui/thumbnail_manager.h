#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace miniwp {

class ThumbnailManager {
public:
    static constexpr int THUMB_WIDTH = 160;
    static constexpr int THUMB_HEIGHT = 90;

    // Hard cap on resident GPU thumbnails. Each thumbnail is 160x90 BGRA
    // (~56 KB); without a cap a large gallery keeps every preview forever.
    static constexpr size_t MAX_RESIDENT_THUMBNAILS = 24;

    static ThumbnailManager& Instance();

    // Call once per frame on UI render thread with active D3D11 device
    void Update(ID3D11Device* device);

    // True while background extraction or GPU texture upload is in flight
    bool HasPendingWork() const noexcept;

    // Get D3D11 Shader Resource View for video thumbnail (non-blocking)
    // Returns nullptr if not loaded yet and queues on-demand background extraction
    ID3D11ShaderResourceView* GetThumbnailSRV(ID3D11Device* device, const std::string& video_path);

    // Delete cached thumbnail for a specific video
    static void DeleteThumbnailCache(const std::string& video_path);

    // Clean up orphaned thumbnails not in active list
    static void CleanOrphanThumbnails(const std::vector<std::string>& active_videos);

    // Extract a specific frame at timestamp_sec to BGRA buffer
    static bool ExtractFrameToBGRA(
        const std::string& video_path,
        std::vector<uint8_t>& out_bgra,
        int width = THUMB_WIDTH,
        int height = THUMB_HEIGHT,
        double timestamp_sec = 1.0
    );

    // Helper to create a D3D11 SRV from raw BGRA buffer
    static ComPtr<ID3D11ShaderResourceView> CreateSRVFromBGRA(
        ID3D11Device* device,
        const uint8_t* bgra_data,
        int width,
        int height
    );

    // Release all in-memory textures and stop any active worker thread
    void ReleaseTextures();

    ~ThumbnailManager();

private:
    ThumbnailManager() = default;

    struct PendingUpload {
        std::string video_path;
        std::vector<uint8_t> bgra_data; // 160 * 90 * 4
    };

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> m_srv_map;
    std::list<std::string> m_srv_lru; // most-recently-used first
    std::unordered_map<std::string, bool> m_requested;
    std::deque<PendingUpload> m_pending_uploads;

    // On-demand background worker thread (0 threads when idle or UI closed)
    std::atomic<bool> m_stop_worker{false};
    std::atomic<bool> m_worker_running{false};
    std::atomic<size_t> m_pending_upload_count{0};
    std::deque<std::string> m_extract_queue;
    std::mutex m_queue_mutex;
    std::thread m_worker_thread;

    void StopWorker();
    void WorkerLoop();
    static std::filesystem::path GetThumbnailPath(const std::string& video_path);
    static std::filesystem::path GetCacheDirectory();
};

} // namespace miniwp
