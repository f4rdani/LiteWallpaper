#include "lockscreen_manager.h"
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <vector>
#include <fstream>
#include <algorithm>
#include <thread>
#include "core/logger.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.UserProfile.h>
#include <shobjidl.h>

#ifndef CLSID_DesktopWallpaper
static const CLSID CLSID_DesktopWallpaper = {0xC2CF3110, 0x460E, 0x4fc1, {0xB9, 0xD0, 0x8A, 0x1C, 0x0C, 0x9C, 0xC4, 0xBD}};
#endif
#ifndef IID_IDesktopWallpaper
static const IID IID_IDesktopWallpaper = {0xB92B56A9, 0x8B55, 0x4E14, {0x9A, 0x89, 0x01, 0x99, 0xBB, 0xB6, 0xF9, 0x3B}};
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

using Microsoft::WRL::ComPtr;

namespace litewp {

static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], size_needed, NULL, NULL);
    return str;
}

static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    std::wstring wstr(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], size_needed);
    return wstr;
}

LockScreenManager::LockScreenManager() {
    m_worker_running = true;
    m_worker_thread = std::thread(&LockScreenManager::WorkerLoop, this);
}

LockScreenManager::~LockScreenManager() {
    m_worker_running = false;
    m_queue_cv.notify_all();
    if (m_worker_thread.joinable()) {
        m_worker_thread.join();
    }
}

std::wstring LockScreenManager::GetTempImagePathBmp() const {
    wchar_t appDataPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath))) {
        std::wstring dir = std::wstring(appDataPath) + L"\\LiteWallpaper";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir + L"\\lockscreen_capture.bmp";
    }
    return L"lockscreen_capture.bmp";
}

std::wstring LockScreenManager::GetTempImagePathJpg(int slot) const {
    wchar_t appDataPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath))) {
        std::wstring dir = std::wstring(appDataPath) + L"\\LiteWallpaper";
        CreateDirectoryW(dir.c_str(), NULL);
        if (slot >= 0) {
            return dir + L"\\lockscreen_capture_" + std::to_wstring(slot) + L".jpg";
        }
        return dir + L"\\lockscreen_capture.jpg";
    }
    return (slot >= 0) ? (L"lockscreen_capture_" + std::to_wstring(slot) + L".jpg") : L"lockscreen_capture.jpg";
}

std::wstring LockScreenManager::GetDesktopPlaceholderImagePathJpg(int slot) const {
    wchar_t appDataPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath))) {
        std::wstring dir = std::wstring(appDataPath) + L"\\LiteWallpaper";
        CreateDirectoryW(dir.c_str(), NULL);
        if (slot >= 0) {
            return dir + L"\\active_wallpaper_placeholder_" + std::to_wstring(slot) + L".jpg";
        }
        return dir + L"\\active_wallpaper_placeholder.jpg";
    }
    return (slot >= 0) ? (L"active_wallpaper_placeholder_" + std::to_wstring(slot) + L".jpg") : L"active_wallpaper_placeholder.jpg";
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

bool LockScreenManager::SaveRgbAsBmp(
    const std::vector<uint8_t>& rgb,
    int width,
    int height,
    const std::wstring& outputPath
) {
    if (rgb.empty() || width <= 0 || height <= 0) return false;

    size_t rowStride = ((static_cast<size_t>(width) * 3 + 3) / 4) * 4;
    std::vector<uint8_t> bgrData(rowStride * height, 0);

    for (int y = 0; y < height; ++y) {
        size_t rowStart = static_cast<size_t>(height - 1 - y) * rowStride;
        for (int x = 0; x < width; ++x) {
            size_t srcIdx = (static_cast<size_t>(y) * width + x) * 3;
            bgrData[rowStart + x * 3 + 0] = rgb[srcIdx + 2]; // B
            bgrData[rowStart + x * 3 + 1] = rgb[srcIdx + 1]; // G
            bgrData[rowStart + x * 3 + 2] = rgb[srcIdx + 0]; // R
        }
    }

    std::ofstream bmpFile(outputPath, std::ios::binary);
    if (!bmpFile.is_open()) return false;

    BITMAPFILEHEADER bfh = {};
    bfh.bfType = 0x4D42; // "BM"
    bfh.bfSize = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + bgrData.size());
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    BITMAPINFOHEADER bih = {};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = width;
    bih.biHeight = height;
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = static_cast<DWORD>(bgrData.size());

    bmpFile.write(reinterpret_cast<const char*>(&bfh), sizeof(bfh));
    bmpFile.write(reinterpret_cast<const char*>(&bih), sizeof(bih));
    bmpFile.write(reinterpret_cast<const char*>(bgrData.data()), bgrData.size());
    return true;
}

bool LockScreenManager::SetNativeDesktopWallpaperFile(const std::wstring& imagePath) {
    if (imagePath.empty()) return false;

    // 1. Modern Windows 8/10/11 IDesktopWallpaper COM interface
    ComPtr<IDesktopWallpaper> pDesktopWallpaper;
    HRESULT hr = CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pDesktopWallpaper));
    if (SUCCEEDED(hr) && pDesktopWallpaper) {
        pDesktopWallpaper->SetWallpaper(nullptr, imagePath.c_str());
    }

    // 2. Win32 SystemParametersInfoW for boot persistence and legacy fallback
    SystemParametersInfoW(
        SPI_SETDESKWALLPAPER,
        0,
        reinterpret_cast<void*>(const_cast<wchar_t*>(imagePath.c_str())),
        SPIF_UPDATEINIFILE | SPIF_SENDCHANGE
    );

    // 3. Broadcast setting change to notify Explorer immediately
    PostMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, SPI_SETDESKWALLPAPER, 0);
    return true;
}

bool LockScreenManager::SetNativeDesktopWallpaper(
    ID3D11Device* device,
    ID3D11DeviceContext* ctx,
    ID3D11Texture2D* currentFrame,
    int arrayIndex
) {
    if (!device || !ctx || !currentFrame) return false;

    std::wstring placeholderPath = GetDesktopPlaceholderImagePathJpg(-1);
    if (SaveTextureAsJpg(device, ctx, currentFrame, arrayIndex, placeholderPath, 92)) {
        return SetNativeDesktopWallpaperFile(placeholderPath);
    }
    return false;
}

void LockScreenManager::PreCacheLockScreenAsync(
    ID3D11Device* device,
    ID3D11DeviceContext* ctx,
    ID3D11Texture2D* currentFrame,
    int arrayIndex,
    bool syncNativeDesktop
) {
    SyncVisualsAsync(device, ctx, currentFrame, arrayIndex, true, syncNativeDesktop);
}

void LockScreenManager::SyncFromVideoAsync(
    const std::string& video_path,
    int target_w,
    int target_h,
    bool syncLockScreen,
    bool syncNativeDesktop,
    double timestamp_sec
) {
    if (video_path.empty()) return;
    if (!syncLockScreen && !syncNativeDesktop) return;

    if (target_w <= 0 || target_h <= 0) {
        target_w = GetSystemMetrics(SM_CXSCREEN);
        target_h = GetSystemMetrics(SM_CYSCREEN);
        if (target_w <= 0) target_w = 1920;
        if (target_h <= 0) target_h = 1080;
    }

    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        m_queue.push({video_path, target_w, target_h, syncLockScreen, syncNativeDesktop, timestamp_sec});
    }
    m_queue_cv.notify_one();
}

void LockScreenManager::WorkerLoop() {
    while (m_worker_running) {
        VideoSyncTask task;
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_queue_cv.wait(lock, [this]() {
                return !m_worker_running || !m_queue.empty();
            });
            if (!m_worker_running && m_queue.empty()) {
                break;
            }
            task = std::move(m_queue.front());
            m_queue.pop();
        }
        ProcessSyncTask(task);
    }
}

void LockScreenManager::ProcessSyncTask(const VideoSyncTask& task) {
    int deskSlot = m_desktop_slot.fetch_xor(1);
    int lockSlot = m_lock_slot.fetch_xor(1);

    std::wstring desktopSlotPath = GetDesktopPlaceholderImagePathJpg(deskSlot);
    std::wstring desktopCanonical = GetDesktopPlaceholderImagePathJpg(-1);
    std::wstring lockSlotPath = GetTempImagePathJpg(lockSlot);
    std::wstring lockCanonical = GetTempImagePathJpg(-1);
    std::wstring lockBmp = GetTempImagePathBmp();

    std::string primaryJpg = task.syncNativeDesktop ? WideToUtf8(desktopSlotPath) : WideToUtf8(lockSlotPath);
    bool extraction_done = false;

    // 1. Primary: Use ffmpeg CLI (fastest, pristine 100% quality, zero codec/color quirks)
    wchar_t tsBuf[64] = {};
    swprintf_s(tsBuf, L"%.2f", (std::max)(0.0, task.timestamp_sec));
    std::wstring wCmd = L"ffmpeg -y -ss " + std::wstring(tsBuf) + L" -i \"" + Utf8ToWide(task.video_path) + L"\" -vframes 1 -q:v 2 \"" + Utf8ToWide(primaryJpg) + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, wCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 3000);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (exitCode == 0) {
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExW(Utf8ToWide(primaryJpg).c_str(), GetFileExInfoStandard, &fad) && fad.nFileSizeLow > 5000) {
                extraction_done = true;
                Logger::Info("ProcessSyncTask: Frame extracted via ffmpeg CLI successfully (", fad.nFileSizeLow, " bytes)");
            }
        }
    }

    // 2. Fallback: In-process FFmpeg C API with luma validation
    if (!extraction_done) {
        AVFormatContext* fmt_ctx = nullptr;
        if (avformat_open_input(&fmt_ctx, task.video_path.c_str(), nullptr, nullptr) >= 0) {
            if (avformat_find_stream_info(fmt_ctx, nullptr) >= 0) {
                int video_stream_idx = -1;
                for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
                    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                        video_stream_idx = static_cast<int>(i);
                        break;
                    }
                }

                if (video_stream_idx >= 0) {
                    AVCodecParameters* codecpar = fmt_ctx->streams[video_stream_idx]->codecpar;
                    const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
                    if (decoder) {
                        AVCodecContext* codec_ctx = avcodec_alloc_context3(decoder);
                        if (codec_ctx) {
                            if (avcodec_parameters_to_context(codec_ctx, codecpar) >= 0 &&
                                avcodec_open2(codec_ctx, decoder, nullptr) >= 0) {

                                int64_t target_ts = 0;
                                if (fmt_ctx->streams[video_stream_idx]->time_base.den > 0) {
                                    int64_t req_ts = av_rescale_q(static_cast<int64_t>((std::max)(0.0, task.timestamp_sec) * AV_TIME_BASE), AV_TIME_BASE_Q, fmt_ctx->streams[video_stream_idx]->time_base);
                                    int64_t dur = fmt_ctx->streams[video_stream_idx]->duration;
                                    if (dur > 0 && req_ts < dur) {
                                        target_ts = req_ts;
                                    } else if (dur > 0 && req_ts >= dur) {
                                        target_ts = 0;
                                    } else {
                                        target_ts = req_ts;
                                    }
                                }
                                av_seek_frame(fmt_ctx, video_stream_idx, target_ts, AVSEEK_FLAG_BACKWARD);
                                avcodec_flush_buffers(codec_ctx);

                                AVPacket* pkt = av_packet_alloc();
                                AVFrame* frame = av_frame_alloc();
                                AVFrame* rgb_frame = av_frame_alloc();

                                rgb_frame->format = AV_PIX_FMT_RGB24;
                                rgb_frame->width = task.target_w;
                                rgb_frame->height = task.target_h;
                                av_frame_get_buffer(rgb_frame, 32);

                                SwsContext* sws_ctx = nullptr;
                                int frames_received = 0;

                                while (av_read_frame(fmt_ctx, pkt) >= 0) {
                                    if (pkt->stream_index == video_stream_idx) {
                                        if (avcodec_send_packet(codec_ctx, pkt) >= 0) {
                                            while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                                                if (frame->width > 0 && frame->height > 0 && !(frame->flags & AV_FRAME_FLAG_CORRUPT)) {
                                                    frames_received++;

                                                    // Check luma to avoid intro black frames when using default 1.0s auto-sync
                                                    int luma_sum = 0;
                                                    if (frame->data[0]) {
                                                        for (int i = 0; i < 100; ++i) {
                                                            int sx = (i % 10) * (frame->width / 10);
                                                            int sy = (i / 10) * (frame->height / 10);
                                                            luma_sum += frame->data[0][sy * frame->linesize[0] + sx];
                                                        }
                                                    }
                                                    int avg_luma = luma_sum / 100;

                                                    bool accept_frame = (task.timestamp_sec != 1.0) ? (frames_received >= 1) : (avg_luma > 20 || frames_received > 60);
                                                    if (accept_frame) {
                                                        if (!sws_ctx) {
                                                            sws_ctx = sws_getContext(
                                                                frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                                                task.target_w, task.target_h, AV_PIX_FMT_RGB24,
                                                                SWS_BILINEAR, nullptr, nullptr, nullptr
                                                            );
                                                        }
                                                        if (sws_ctx) {
                                                            int scaled = sws_scale(
                                                                sws_ctx,
                                                                frame->data, frame->linesize,
                                                                0, frame->height,
                                                                rgb_frame->data, rgb_frame->linesize
                                                            );
                                                            if (scaled == task.target_h && rgb_frame->data[0]) {
                                                                std::vector<uint8_t> rgbData(static_cast<size_t>(task.target_w) * task.target_h * 3);
                                                                for (int y = 0; y < task.target_h; ++y) {
                                                                    memcpy(
                                                                        rgbData.data() + (static_cast<size_t>(y) * task.target_w * 3),
                                                                        rgb_frame->data[0] + (static_cast<size_t>(y) * rgb_frame->linesize[0]),
                                                                        static_cast<size_t>(task.target_w) * 3
                                                                    );
                                                                }
                                                                if (stbi_write_jpg(primaryJpg.c_str(), task.target_w, task.target_h, 3, rgbData.data(), 92)) {
                                                                    extraction_done = true;
                                                                    Logger::Info("ProcessSyncTask: Frame extracted via in-process FFmpeg successfully");
                                                                }
                                                            }
                                                        }
                                                        av_frame_unref(frame);
                                                        break;
                                                    }
                                                }
                                                av_frame_unref(frame);
                                            }
                                            if (extraction_done) break;
                                        }
                                    }
                                    av_packet_unref(pkt);
                                    if (extraction_done) break;
                                }

                                if (sws_ctx) sws_freeContext(sws_ctx);
                                av_packet_free(&pkt);
                                av_frame_free(&frame);
                                av_frame_free(&rgb_frame);
                            }
                            avcodec_free_context(&codec_ctx);
                        }
                    }
                }
                avformat_close_input(&fmt_ctx);
            }
        }
    }

    if (extraction_done) {
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hrCo)) hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        if (task.syncNativeDesktop) {
            CopyFileW(desktopSlotPath.c_str(), desktopCanonical.c_str(), FALSE);
            SetNativeDesktopWallpaperFile(desktopSlotPath);
        }
        if (task.syncLockScreen) {
            if (task.syncNativeDesktop) {
                CopyFileW(desktopSlotPath.c_str(), lockSlotPath.c_str(), FALSE);
            }
            CopyFileW(lockSlotPath.c_str(), lockCanonical.c_str(), FALSE);
            SetLockScreenImage(lockSlotPath);
            SetLockScreenImageWin7(lockSlotPath);
        }

        if (SUCCEEDED(hrCo)) {
            CoUninitialize();
        }
    }
}

void LockScreenManager::SyncVisualsRGBAsync(
    std::vector<uint8_t> rgbData,
    int width,
    int height,
    bool syncLockScreen,
    bool syncNativeDesktop
) {
    if (rgbData.empty() || width <= 0 || height <= 0) return;
    if (!syncLockScreen && !syncNativeDesktop) return;

    int deskSlot = m_desktop_slot.fetch_xor(1);
    int lockSlot = m_lock_slot.fetch_xor(1);

    std::wstring desktopSlotPath = GetDesktopPlaceholderImagePathJpg(deskSlot);
    std::wstring desktopCanonical = GetDesktopPlaceholderImagePathJpg(-1);
    std::wstring lockSlotPath = GetTempImagePathJpg(lockSlot);
    std::wstring lockCanonical = GetTempImagePathJpg(-1);
    std::wstring lockBmp = GetTempImagePathBmp();

    std::thread([this, rgb = std::move(rgbData), width, height,
                 desktopSlotPath, desktopCanonical,
                 lockSlotPath, lockCanonical, lockBmp,
                 syncLockScreen, syncNativeDesktop]() {

        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        std::string primaryJpg = syncNativeDesktop ? WideToUtf8(desktopSlotPath) : WideToUtf8(lockSlotPath);
        if (stbi_write_jpg(primaryJpg.c_str(), width, height, 3, rgb.data(), 92)) {
            if (syncNativeDesktop) {
                CopyFileW(desktopSlotPath.c_str(), desktopCanonical.c_str(), FALSE);
                SetNativeDesktopWallpaperFile(desktopSlotPath);
            }
            if (syncLockScreen) {
                if (syncNativeDesktop) {
                    CopyFileW(desktopSlotPath.c_str(), lockSlotPath.c_str(), FALSE);
                }
                CopyFileW(lockSlotPath.c_str(), lockCanonical.c_str(), FALSE);
                SaveRgbAsBmp(rgb, width, height, lockBmp);
                SetLockScreenImage(lockSlotPath);
                SetLockScreenImageWin7(lockSlotPath);
            }
        }

        if (SUCCEEDED(hrCo)) {
            CoUninitialize();
        }
    }).detach();
}

void LockScreenManager::SyncVisualsAsync(
    ID3D11Device* device,
    ID3D11DeviceContext* ctx,
    ID3D11Texture2D* currentFrame,
    int arrayIndex,
    bool syncLockScreen,
    bool syncNativeDesktop
) {
    if (!device || !ctx || !currentFrame) return;
    if (!syncLockScreen && !syncNativeDesktop) return;

    D3D11_TEXTURE2D_DESC desc;
    currentFrame->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.ArraySize = 1;
    stagingDesc.MipLevels = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> stagingTexture;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr)) return;

    UINT subresource = D3D11CalcSubresource(0, arrayIndex, desc.MipLevels);
    ctx->CopySubresourceRegion(stagingTexture.Get(), 0, 0, 0, 0, currentFrame, subresource, nullptr);

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = ctx->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return;

    int width = desc.Width;
    int height = desc.Height;
    std::vector<uint8_t> rgbData(static_cast<size_t>(width) * height * 3, 0);

    if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) {
        const uint8_t* srcRow = reinterpret_cast<const uint8_t*>(mapped.pData);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                size_t srcIdx = static_cast<size_t>(x) * 4;
                size_t dstIdx = (static_cast<size_t>(y) * width + x) * 3;
                rgbData[dstIdx + 0] = srcRow[srcIdx + 2]; // R
                rgbData[dstIdx + 1] = srcRow[srcIdx + 1]; // G
                rgbData[dstIdx + 2] = srcRow[srcIdx + 0]; // B
            }
            srcRow += mapped.RowPitch;
        }
    } else {
        const uint8_t* srcRow = reinterpret_cast<const uint8_t*>(mapped.pData);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                size_t srcIdx = static_cast<size_t>(x) * 4;
                size_t dstIdx = (static_cast<size_t>(y) * width + x) * 3;
                rgbData[dstIdx + 0] = srcRow[srcIdx + 0];
                rgbData[dstIdx + 1] = srcRow[srcIdx + 1];
                rgbData[dstIdx + 2] = srcRow[srcIdx + 2];
            }
            srcRow += mapped.RowPitch;
        }
    }

    ctx->Unmap(stagingTexture.Get(), 0);

    SyncVisualsRGBAsync(std::move(rgbData), width, height, syncLockScreen, syncNativeDesktop);
}

bool LockScreenManager::SetLockScreenImage(const std::wstring& imagePath) {
    if (imagePath.empty()) return false;

    // 0. Official Windows 10/11 WinRT LockScreen API
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {}
    try {
        auto file = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(imagePath).get();
        winrt::Windows::System::UserProfile::LockScreen::SetImageFileAsync(file).get();
    } catch (...) {
        // Fallback to registry if WinRT call fails or is restricted
    }

    // Disable Windows Spotlight override on Lock Screen so Picture mode takes precedence
    HKEY hCdm = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager", 0, KEY_SET_VALUE, &hCdm) == ERROR_SUCCESS) {
        DWORD zero = 0;
        RegSetValueExW(hCdm, L"RotatingLockScreenEnabled", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
        RegSetValueExW(hCdm, L"RotatingLockScreenOverlayEnabled", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
        RegSetValueExW(hCdm, L"SubscribedContent-338387Enabled", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
        RegSetValueExW(hCdm, L"SubscribedContent-338388Enabled", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
        RegSetValueExW(hCdm, L"SubscribedContent-338389Enabled", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof(zero));
        RegCloseKey(hCdm);
    }

    // 1. Creative key (Windows 10/11 LockApp.exe)
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Lock Screen\\Creative",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr
    ) == ERROR_SUCCESS) {
        RegSetValueExW(
            hKey, L"LockScreenImage", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(imagePath.c_str()),
            static_cast<DWORD>((imagePath.length() + 1) * sizeof(wchar_t))
        );
        RegSetValueExW(
            hKey, L"LandscapeAssetPath", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(imagePath.c_str()),
            static_cast<DWORD>((imagePath.length() + 1) * sizeof(wchar_t))
        );
        RegSetValueExW(
            hKey, L"PortraitAssetPath", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(imagePath.c_str()),
            static_cast<DWORD>((imagePath.length() + 1) * sizeof(wchar_t))
        );
        RegCloseKey(hKey);
    }

    // 2. PersonalizationCSP key (Windows 10/11 Personalization Settings)
    if (RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\PersonalizationCSP",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr
    ) == ERROR_SUCCESS) {
        DWORD status = 1;
        RegSetValueExW(
            hKey, L"LockScreenImagePath", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(imagePath.c_str()),
            static_cast<DWORD>((imagePath.length() + 1) * sizeof(wchar_t))
        );
        RegSetValueExW(
            hKey, L"LockScreenImageUrl", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(imagePath.c_str()),
            static_cast<DWORD>((imagePath.length() + 1) * sizeof(wchar_t))
        );
        RegSetValueExW(
            hKey, L"LockScreenImageStatus", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&status), sizeof(status)
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

    HKEY hKey = nullptr;
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

bool LockScreenManager::InstallScreensaver(const std::wstring& scrPath, int timeoutSeconds, bool secureOnResume) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    // Set SCRNSAVE.EXE path
    RegSetValueExW(hKey, L"SCRNSAVE.EXE", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(scrPath.c_str()),
        static_cast<DWORD>((scrPath.length() + 1) * sizeof(wchar_t)));

    // Set ScreenSaveActive = "1"
    const wchar_t* active = L"1";
    RegSetValueExW(hKey, L"ScreenSaveActive", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(active), static_cast<DWORD>((wcslen(active) + 1) * sizeof(wchar_t)));

    // Set ScreenSaverIsSecure = "1" (locks session upon wake up)
    const wchar_t* secure = secureOnResume ? L"1" : L"0";
    RegSetValueExW(hKey, L"ScreenSaverIsSecure", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(secure), static_cast<DWORD>((wcslen(secure) + 1) * sizeof(wchar_t)));

    // Set ScreenSaveTimeOut (in seconds)
    std::wstring timeoutStr = std::to_wstring(timeoutSeconds);
    RegSetValueExW(hKey, L"ScreenSaveTimeOut", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(timeoutStr.c_str()), static_cast<DWORD>((timeoutStr.length() + 1) * sizeof(wchar_t)));

    RegCloseKey(hKey);

    SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, TRUE, nullptr, SPIF_SENDCHANGE);
    return true;
}

bool LockScreenManager::UninstallScreensaver() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, L"SCRNSAVE.EXE");
        RegCloseKey(hKey);
    }
    SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, FALSE, nullptr, SPIF_SENDCHANGE);
    return true;
}

bool LockScreenManager::IsScreensaverInstalled(std::wstring* outPath) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t path[MAX_PATH] = {};
    DWORD size = sizeof(path);
    DWORD type = REG_SZ;
    LSTATUS res = RegQueryValueExW(hKey, L"SCRNSAVE.EXE", nullptr, &type, reinterpret_cast<BYTE*>(path), &size);
    RegCloseKey(hKey);

    if (res == ERROR_SUCCESS && wcslen(path) > 0) {
        if (outPath) *outPath = path;
        return true;
    }
    return false;
}

void LockScreenManager::OpenWindowsScreensaverSettings() {
    ShellExecuteW(nullptr, L"open", L"control.exe", L"desk.cpl,,@screensaver", nullptr, SW_SHOWNORMAL);
}

} // namespace litewp
