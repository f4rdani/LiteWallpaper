#pragma once
#include <d3d11.h>
#include <string>
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
        int arrayIndex
    );

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
    std::wstring GetTempImagePathJpg() const;
    std::wstring GetDesktopPlaceholderImagePathJpg() const;

    // Windows Native Screensaver integration helpers
    static bool InstallScreensaver(const std::wstring& scrPath, int timeoutSeconds = 300, bool secureOnResume = true);
    static bool UninstallScreensaver();
    static bool IsScreensaverInstalled(std::wstring* outPath = nullptr);
    static void OpenWindowsScreensaverSettings();

private:
    std::atomic<bool> m_is_caching{false};

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
