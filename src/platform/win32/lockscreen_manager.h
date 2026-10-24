#pragma once
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace miniwp {

class LockScreenManager {
public:
    LockScreenManager();
    ~LockScreenManager();

    // Synchronize visual state to Lock Screen and/or native Windows Desktop Wallpaper asynchronously.
    // Spawns an on-demand worker thread that exits immediately once queued tasks finish (0 idle threads).
    void SyncFromVideoAsync(
        const std::string& video_path,
        int target_w = 0,
        int target_h = 0,
        bool syncLockScreen = true,
        bool syncNativeDesktop = true,
        double timestamp_sec = 1.0,
        bool force = false
    );

    // Apply image file as native Windows desktop wallpaper (IDesktopWallpaper COM + SystemParametersInfoW)
    static bool SetNativeDesktopWallpaperFile(const std::wstring& imagePath);

    // Apply image file as Windows 10/11 Lock Screen wallpaper
    bool SetLockScreenImage(const std::wstring& imagePath);

    std::wstring GetTempImagePathJpg(int slot = -1) const;
    std::wstring GetDesktopPlaceholderImagePathJpg(int slot = -1) const;

    // Windows Native Screensaver integration helpers
    static bool InstallScreensaver(const std::wstring& scrPath, int timeoutSeconds = 300, bool secureOnResume = true);
    static bool UninstallScreensaver();
    static bool IsScreensaverInstalled(std::wstring* outPath = nullptr);
    static void OpenWindowsScreensaverSettings();

private:
    struct VideoSyncTask {
        std::string video_path;
        int target_w = 0;
        int target_h = 0;
        bool syncLockScreen = false;
        bool syncNativeDesktop = false;
        double timestamp_sec = 1.0;
        std::string sync_key;
    };

    std::atomic<int>            m_desktop_slot{0};
    std::atomic<int>            m_lock_slot{0};
    std::mutex                  m_queue_mutex;
    std::deque<VideoSyncTask>   m_queue;
    bool                        m_worker_active = false;
    std::atomic<bool>           m_shutting_down{false};
    std::thread                 m_worker_thread;

    void WorkerLoop();
    void ProcessSyncTask(const VideoSyncTask& task);
    bool SetLockScreenImageWin7(const std::wstring& imagePath);
};

} // namespace miniwp
