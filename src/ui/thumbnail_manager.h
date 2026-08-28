#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <deque>

using Microsoft::WRL::ComPtr;

namespace litewp {

class ThumbnailManager {
public:
    static constexpr int THUMB_WIDTH = 160;
    static constexpr int THUMB_HEIGHT = 90;

    static ThumbnailManager& Instance();

    // Call once per frame on UI render thread with active D3D11 device
    void Update(ID3D11Device* device);

    // Get D3D11 Shader Resource View for video thumbnail (non-blocking)
    // Returns nullptr if not loaded yet and queues background extraction
    ID3D11ShaderResourceView* GetThumbnailSRV(ID3D11Device* device, const std::string& video_path);

    // Delete cached thumbnail for a specific video
    static void DeleteThumbnailCache(const std::string& video_path);

    // Clean up orphaned thumbnails not in active list
    static void CleanOrphanThumbnails(const std::vector<std::string>& active_videos);

    // Release all in-memory textures
    void ReleaseTextures();

    ~ThumbnailManager();

private:
    ThumbnailManager();

    struct PendingUpload {
        std::string video_path;
        std::vector<uint8_t> bgra_data; // 160 * 90 * 4
    };

    std::mutex m_mutex;
    std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> m_srv_map;
    std::unordered_map<std::string, bool> m_requested;
    std::deque<PendingUpload> m_pending_uploads;
    
    // Background worker thread
    std::atomic<bool> m_stop_worker{false};
    std::deque<std::string> m_extract_queue;
    std::mutex m_queue_mutex;
    std::thread m_worker_thread;

    void WorkerLoop();
    static std::string GetThumbnailPath(const std::string& video_path);
    static std::string GetCacheDirectory();
    static bool ExtractFrameToBGRA(const std::string& video_path, std::vector<uint8_t>& out_bgra);
};

} // namespace litewp
