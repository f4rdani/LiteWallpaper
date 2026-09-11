#pragma once
#include <d3d11.h>
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>

namespace litewp {

class LockScreenManager {
public:
    LockScreenManager();
    ~LockScreenManager();

    // Capture current frame from D3D11 texture and set as lock screen image (synchronous)
    bool CaptureAndSetLockScreen(
        ID3D11Device* device,
        ID3D11DeviceContext* ctx,
        ID3D11Texture2D* currentFrame,
        int arrayIndex
    );
    
    // Pre-cache lock screen snapshot on background worker thread (asynchronous, non-blocking)
    void PreCacheLockScreenAsync(
        ID3D11Device* device,
        ID3D11DeviceContext* ctx,
        ID3D11Texture2D* currentFrame,
        int arrayIndex,
        bool syncNativeDesktop = false
    );

    // Synchronize visual state to both Lock Screen and native Windows Desktop Wallpaper directly from video file (100% glitch-free)
    void SyncFromVideoAsync(
        const std::string& video_path,
        int target_w = 0,
        int target_h = 0,
        bool syncLockScreen = true,
        bool syncNativeDesktop = true,
        double timestamp_sec = 1.0
    );

    // Synchronize visual state to both Lock Screen and native Windows Desktop Wallpaper asynchronously from pristine RGB buffer
    void SyncVisualsRGBAsync(
        std::vector<uint8_t> rgbData,
        int width,
        int height,
        bool syncLockScreen = true,
        bool syncNativeDesktop = true
    );

    // Synchronize current frame to both Lock Screen and native Windows Desktop Wallpaper asynchronously
    void SyncVisualsAsync(
        ID3D11Device* device,
        ID3D11DeviceContext* ctx,
        ID3D11Texture2D* currentFrame,
        int arrayIndex,
        bool syncLockScreen = true,
        bool syncNativeDesktop = true
    );

    // Set native Windows desktop wallpaper from image file (IDesktopWallpaper COM + SystemParametersInfoW)
    static bool SetNativeDesktopWallpaperFile(const std::wstring& imagePath);

    // Capture current frame from D3D11 texture and set as native Windows desktop wallpaper for instant 0s boot visual
    bool SetNativeDesktopWallpaper(
        ID3D11Device* device,
        ID3D11DeviceContext* ctx,
        ID3D11Texture2D* currentFrame,
        int arrayIndex
    );

    // Set image file as lock screen image (Windows 10/11)
    bool SetLockScreenImage(const std::wstring& imagePath);
    
    // Windows 7 fallback lock screen configuration (requires true JPEG < 256KB)
    bool SetLockScreenImageWin7(const std::wstring& imagePath);

    std::wstring GetTempImagePathBmp() const;
    std::wstring GetTempImagePathJpg(int slot = -1) const;
    std::wstring GetDesktopPlaceholderImagePathJpg(int slot = -1) const;

    // Windows Native Screensaver integration helpers
    static bool InstallScreensaver(const std::wstring& scrPath, int timeoutSeconds = 300, bool secureOnResume = true);
    static bool UninstallScreensaver();
    static bool IsScreensaverInstalled(std::wstring* outPath = nullptr);
    static void OpenWindowsScreensaverSettings();

private:
    std::atomic<bool> m_is_caching{false};
    std::atomic<int>  m_desktop_slot{0};
    std::atomic<int>  m_lock_slot{0};

    static bool SaveRgbAsBmp(
        const std::vector<uint8_t>& rgb,
        int width,
        int height,
        const std::wstring& outputPath
    );

    bool SaveTextureAsBmp(
        ID3D11Device* device,
        ID3D11DeviceContext* ctx,
        ID3D11Texture2D* texture,
        int arrayIndex,
        const std::wstring& outputPath
    );

    bool SaveTextureAsJpg(
        ID3D11Device* device,
        ID3D11DeviceContext* ctx,
        ID3D11Texture2D* texture,
        int arrayIndex,
        const std::wstring& outputPath,
        int quality = 80
    );
};

} // namespace litewp
