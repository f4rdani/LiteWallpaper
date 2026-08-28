#pragma once
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>

namespace litewp {

struct VideoProbeResult {
    bool valid = false;
    int width = 0;
    int height = 0;
    double fps = 30.0;
    double duration = 0.0;
    std::string codec_name;
    bool has_audio = false;
};

class VideoOptimizer {
public:
    using ProgressCallback = std::function<void(float progress, const std::string& status)>;
    using CompleteCallback = std::function<void(bool success, const std::string& output_path)>;

    VideoOptimizer();
    ~VideoOptimizer();

    // Probe video dimensions and basic stream info quickly without full decode
    static VideoProbeResult Probe(const std::string& input_path);

    // Calculate aspect-ratio preserved target dimensions fitting within max bounding box
    static std::pair<int, int> CalculateTargetDimensions(int src_w, int src_h, int max_w, int max_h);

    // Get deterministic cached output path for a downscaled version
    static std::string GetOptimizedPath(const std::string& input_path, int target_w, int target_h);

    // Check if an optimized version already exists in cache
    static bool HasOptimizedCache(const std::string& input_path, int target_w, int target_h);

    // Delete all cached optimized video files for a given input video
    static void DeleteOptimizedCache(const std::string& input_path);

    // Automatically clean up any orphaned cache files not present in the active gallery list
    static void CleanOrphanCaches(const std::vector<std::string>& active_videos);

    // Start background transcode to downscale video
    bool StartOptimizeAsync(
        const std::string& input_path,
        int target_w,
        int target_h,
        ProgressCallback on_progress = nullptr,
        CompleteCallback on_complete = nullptr
    );

    void Cancel();
    bool IsRunning() const;
    float GetProgress() const;
    std::string GetCurrentStatus() const;

private:
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancel{false};
    std::atomic<float> m_progress{0.0f};
    mutable std::mutex m_status_mutex;
    std::string m_current_status;
    std::thread m_worker_thread;

    void TranscodeWorker(
        std::string input_path,
        int target_w,
        int target_h,
        ProgressCallback on_progress,
        CompleteCallback on_complete
    );
};

extern VideoOptimizer g_video_optimizer;

} // namespace litewp
