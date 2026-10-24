#pragma once
#include <functional>
#include <string>
#include <vector>

namespace miniwp {

// Direct main-thread controller API for SettingsUI, TrayIcon, and marshaled IPC.
// Replaces legacy self-IPC Named Pipe roundtrips and eliminates multi-thread data races.
namespace EngineController {

// Queue an arbitrary callback to execute safely on the main Win32 UI/engine thread
void PostToMainThread(std::function<void()> task);
void DrainMainThreadTasks();

// Activate and play a wallpaper video (also updates gallery history & auto-syncs static visuals if enabled)
bool SetWallpaper(const std::string& video_path);

// Pause / Resume / Stop live wallpaper playback
void Pause();
void Resume();
void TogglePause();
void StopToStatic();

// Audio control (updates config, lazy-opens or disables audio decoder & WASAPI stream)
void SetVolume(float volume, bool save_immediately = false);
void SetAudioEnabled(bool enabled, bool save_immediately = true);
void ToggleMute();

// Display & performance controls
void SetScalingMode(int mode);
void SetTargetFps(int fps, bool save_immediately = false);
void SetTargetDisplays(const std::vector<int>& displays);
void SetRenderDevice(int gpu_index); // Deferred safely via PostMessage so UI context isn't torn down mid-frame
void TriggerTestFlash();

// Desktop static & Lock screen visual synchronization
void SyncVisualWallpaper(
    const std::string& video_path,
    bool sync_desktop,
    bool sync_lockscreen,
    double timestamp_sec = 1.0,
    int save_mode = -1
);
void ResetDesktopWallpaperAuto();
void ResetLockscreenAuto();

// Native Win32 file picker & clean shutdown
void OpenWallpaperDialog();
void RequestExit();

} // namespace EngineController

} // namespace miniwp
