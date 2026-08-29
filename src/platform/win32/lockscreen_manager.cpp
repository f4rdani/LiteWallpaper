#include "lockscreen_manager.h"
#include <windows.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <vector>
#include <fstream>
#include <algorithm>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

using Microsoft::WRL::ComPtr;

namespace litewp {

static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], size_needed, NULL, NULL);
    return str;
}

LockScreenManager::LockScreenManager() = default;

LockScreenManager::~LockScreenManager() = default;

std::wstring LockScreenManager::GetTempImagePathBmp() const {
    wchar_t appDataPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath))) {
        std::wstring dir = std::wstring(appDataPath) + L"\\LiteWallpaper";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir + L"\\lockscreen_capture.bmp";
    }
    return L"lockscreen_capture.bmp";
}

std::wstring LockScreenManager::GetTempImagePathJpg() const {
    wchar_t appDataPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath))) {
        std::wstring dir = std::wstring(appDataPath) + L"\\LiteWallpaper";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir + L"\\lockscreen_capture.jpg";
    }
    return L"lockscreen_capture.jpg";
}

std::wstring LockScreenManager::GetDesktopPlaceholderImagePathJpg() const {
    wchar_t appDataPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath))) {
        std::wstring dir = std::wstring(appDataPath) + L"\\LiteWallpaper";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir + L"\\active_wallpaper_placeholder.jpg";
    }
    return L"active_wallpaper_placeholder.jpg";
}

bool LockScreenManager::SaveTextureAsBmp(
    ID3D11Device* device,
    ID3D11DeviceContext* ctx,
    ID3D11Texture2D* texture,
    int arrayIndex,
    const std::wstring& outputPath
) {
    if (!device || !ctx || !texture) return false;

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.ArraySize = 1;
    stagingDesc.MipLevels = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> stagingTexture;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr)) return false;

    UINT subresource = D3D11CalcSubresource(0, arrayIndex, desc.MipLevels);
    ctx->CopySubresourceRegion(stagingTexture.Get(), 0, 0, 0, 0, texture, subresource, nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ctx->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    int width = desc.Width;
    int height = desc.Height;

    // Standard BMP row stride must be a multiple of 4 bytes
    size_t rowStride = ((static_cast<size_t>(width) * 3 + 3) / 4) * 4;
    std::vector<uint8_t> rgbData(rowStride * height, 0);

    if (desc.Format == DXGI_FORMAT_NV12) {
        const uint8_t* yPlane = reinterpret_cast<const uint8_t*>(mapped.pData);
        const uint8_t* uvPlane = yPlane + (mapped.RowPitch * height);

        for (int y = 0; y < height; ++y) {
            size_t rowStart = static_cast<size_t>(height - 1 - y) * rowStride;
            for (int x = 0; x < width; ++x) {
                float yVal = static_cast<float>(yPlane[y * mapped.RowPitch + x]);
                int uvX = (x / 2) * 2;
                int uvY = (y / 2);
                float uVal = static_cast<float>(uvPlane[uvY * mapped.RowPitch + uvX]) - 128.0f;
                float vVal = static_cast<float>(uvPlane[uvY * mapped.RowPitch + uvX + 1]) - 128.0f;

                float r = yVal + 1.5748f * vVal;
                float g = yVal - 0.1873f * uVal - 0.4681f * vVal;
                float b = yVal + 1.8556f * uVal;

                // BMP stores BGR
                rgbData[rowStart + x * 3 + 0] = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
                rgbData[rowStart + x * 3 + 1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
                rgbData[rowStart + x * 3 + 2] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
            }
        }
    } else {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(mapped.pData);
        for (int y = 0; y < height; ++y) {
            size_t rowStart = static_cast<size_t>(height - 1 - y) * rowStride;
            for (int x = 0; x < width; ++x) {
                int srcIdx = y * mapped.RowPitch + x * 4;
                rgbData[rowStart + x * 3 + 0] = src[srcIdx + 0]; // B
                rgbData[rowStart + x * 3 + 1] = src[srcIdx + 1]; // G
                rgbData[rowStart + x * 3 + 2] = src[srcIdx + 2]; // R
            }
        }
    }

    ctx->Unmap(stagingTexture.Get(), 0);

    // Write BMP File
    std::ofstream bmpFile(outputPath, std::ios::binary);
    if (!bmpFile.is_open()) return false;

    BITMAPFILEHEADER bfh = {};
    bfh.bfType = 0x4D42; // "BM"
    bfh.bfSize = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + rgbData.size());
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    BITMAPINFOHEADER bih = {};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = width;
    bih.biHeight = height;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = static_cast<DWORD>(rgbData.size());

    bmpFile.write(reinterpret_cast<const char*>(&bfh), sizeof(bfh));
    bmpFile.write(reinterpret_cast<const char*>(&bih), sizeof(bih));
    bmpFile.write(reinterpret_cast<const char*>(rgbData.data()), rgbData.size());

    return true;
}

bool LockScreenManager::SaveTextureAsJpg(
    ID3D11Device* device,
    ID3D11DeviceContext* ctx,
    ID3D11Texture2D* texture,
    int arrayIndex,
    const std::wstring& outputPath,
    int quality
) {
    if (!device || !ctx || !texture) return false;

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.ArraySize = 1;
    stagingDesc.MipLevels = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> stagingTexture;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr)) return false;

    UINT subresource = D3D11CalcSubresource(0, arrayIndex, desc.MipLevels);
    ctx->CopySubresourceRegion(stagingTexture.Get(), 0, 0, 0, 0, texture, subresource, nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ctx->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    int width = desc.Width;
    int height = desc.Height;

    // Top-to-bottom RGB buffer for JPEG
    std::vector<uint8_t> rgbData(static_cast<size_t>(width) * height * 3);

    if (desc.Format == DXGI_FORMAT_NV12) {
        const uint8_t* yPlane = reinterpret_cast<const uint8_t*>(mapped.pData);
        const uint8_t* uvPlane = yPlane + (mapped.RowPitch * height);

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                float yVal = static_cast<float>(yPlane[y * mapped.RowPitch + x]);
                int uvX = (x / 2) * 2;
                int uvY = (y / 2);
                float uVal = static_cast<float>(uvPlane[uvY * mapped.RowPitch + uvX]) - 128.0f;
                float vVal = static_cast<float>(uvPlane[uvY * mapped.RowPitch + uvX + 1]) - 128.0f;

                float r = yVal + 1.5748f * vVal;
                float g = yVal - 0.1873f * uVal - 0.4681f * vVal;
                float b = yVal + 1.8556f * uVal;

                size_t dstIdx = (static_cast<size_t>(y) * width + x) * 3;
                rgbData[dstIdx + 0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
                rgbData[dstIdx + 1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
                rgbData[dstIdx + 2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));
            }
        }
    } else {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(mapped.pData);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                int srcIdx = y * mapped.RowPitch + x * 4;
                size_t dstIdx = (static_cast<size_t>(y) * width + x) * 3;
                rgbData[dstIdx + 0] = src[srcIdx + 2]; // R
                rgbData[dstIdx + 1] = src[srcIdx + 1]; // G
                rgbData[dstIdx + 2] = src[srcIdx + 0]; // B
            }
        }
    }

    ctx->Unmap(stagingTexture.Get(), 0);

    std::string utf8Path = WideToUtf8(outputPath);
    return stbi_write_jpg(utf8Path.c_str(), width, height, 3, rgbData.data(), quality) != 0;
}

bool LockScreenManager::CaptureAndSetLockScreen(
    ID3D11Device* device,
    ID3D11DeviceContext* ctx,
    ID3D11Texture2D* currentFrame,
    int arrayIndex
) {
    if (!device || !ctx || !currentFrame) return false;

    // Generate both BMP and true JPG
    std::wstring imgPathBmp = GetTempImagePathBmp();
    std::wstring imgPathJpg = GetTempImagePathJpg();

    SaveTextureAsBmp(device, ctx, currentFrame, arrayIndex, imgPathBmp);
    SaveTextureAsJpg(device, ctx, currentFrame, arrayIndex, imgPathJpg, 80);

    // Apply to Windows 10/11 lock screen registry
    SetLockScreenImage(imgPathBmp);

    // Also attempt Win7 LogonUI if applicable
    SetLockScreenImageWin7(imgPathJpg);

    return true;
}

bool LockScreenManager::SetNativeDesktopWallpaper(
    ID3D11Device* device,
    ID3D11DeviceContext* ctx,
    ID3D11Texture2D* currentFrame,
    int arrayIndex
) {
    if (!device || !ctx || !currentFrame) return false;

    std::wstring placeholderPath = GetDesktopPlaceholderImagePathJpg();
    if (SaveTextureAsJpg(device, ctx, currentFrame, arrayIndex, placeholderPath, 92)) {
        // Set native Windows desktop wallpaper for instant 0s boot visual
        SystemParametersInfoW(
            SPI_SETDESKWALLPAPER,
            0,
            reinterpret_cast<void*>(const_cast<wchar_t*>(placeholderPath.c_str())),
            SPIF_UPDATEINIFILE | SPIF_SENDCHANGE
        );
        return true;
    }
    return false;
}

bool LockScreenManager::SetLockScreenImage(const std::wstring& imagePath) {
    HKEY hKey;
    if (RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Lock Screen\\Creative",
        0,
        KEY_SET_VALUE,
        &hKey
    ) == ERROR_SUCCESS) {
        RegSetValueExW(
            hKey,
            L"LockScreenImage",
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(imagePath.c_str()),
            static_cast<DWORD>((imagePath.length() + 1) * sizeof(wchar_t))
        );
        RegCloseKey(hKey);
    }

    return true;
}

bool LockScreenManager::SetLockScreenImageWin7(const std::wstring& imagePath) {
    // Windows 7 OEM background wallpaper requires true JPEG (< 256 KB)
    std::wstring oobeDir = L"C:\\Windows\\System32\\oobe\\info\\backgrounds";
    CreateDirectoryW(L"C:\\Windows\\System32\\oobe\\info", NULL);
    CreateDirectoryW(oobeDir.c_str(), NULL);

    std::wstring oobeFile = oobeDir + L"\\backgroundDefault.jpg";
    CopyFileW(imagePath.c_str(), oobeFile.c_str(), FALSE);

    HKEY hKey;
    if (RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\LogonUI\\Background",
        0,
        KEY_SET_VALUE,
        &hKey
    ) == ERROR_SUCCESS) {
        DWORD enable = 1;
        RegSetValueExW(hKey, L"OEMBackground", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&enable), sizeof(enable));
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

} // namespace litewp
