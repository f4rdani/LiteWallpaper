#pragma once
#include <d3d11.h>
#include <string>

namespace litewp {

class LockScreenManager {
public:
    LockScreenManager();
    ~LockScreenManager();

    // Capture current frame from D3D11 texture and set as lock screen image
    bool CaptureAndSetLockScreen(
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

private:
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
