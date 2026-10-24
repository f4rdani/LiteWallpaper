#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "lockscreen_manager.h"
#include "core/logger.h"
#include "core/win_utils.h"
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <versionhelpers.h>
#include <wrl/client.h>
#include <mimalloc.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.UserProfile.h>

#ifndef CLSID_DesktopWallpaper
static const CLSID CLSID_DesktopWallpaper = {0xC2CF3110, 0x460E, 0x4fc1, {0xB9, 0xD0, 0x8A, 0x1C, 0x0C, 0x9C, 0xC4, 0xBD}};
#endif
#ifndef IID_IDesktopWallpaper
static const IID IID_IDesktopWallpaper = {0xB92B56A9, 0x8B55, 0x4E14, {0x9A, 0x89, 0x01, 0x99, 0xBB, 0xB6, 0xF9, 0x3B}};
#endif

#define STBIW_WINDOWS_UTF8
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

using Microsoft::WRL::ComPtr;

namespace miniwp {

namespace {

std::wstring GetDesktopStampPath() {
    return GetAppDataDirWide() + L"\\desktop_sync.stamp";
}

std::wstring GetLockScreenStampPath() {
    return GetAppDataDirWide() + L"\\lockscreen_sync.stamp";
}

bool HasValidImageFile(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        return false;
    }
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        return false;
    }
    uint64_t sz = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    return sz > 2048;
}

std::string ReadStampFile(const std::wstring& path) {
    std::ifstream fin(std::filesystem::path(path), std::ios::binary);
    if (!fin.is_open()) return {};
    std::string val;
    std::getline(fin, val);
    while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ')) {
        val.pop_back();
    }
    return val;
}

void WriteStampFile(const std::wstring& path, const std::string& key) {
    if (key.empty()) return;
    std::ofstream fout(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (fout.is_open()) {
        fout.write(key.data(), static_cast<std::streamsize>(key.size()));
    }
}

} // namespace

LockScreenManager::LockScreenManager() = default;

LockScreenManager::~LockScreenManager() {
    m_shutting_down.store(true);
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        m_queue.clear();
    }
    if (m_worker_thread.joinable()) {
        m_worker_thread.join();
    }
}

std::wstring LockScreenManager::GetTempImagePathJpg(int slot) const {
    const std::wstring& dir = GetAppDataDirWide();
    if (slot >= 0) {
        return dir + L"\\lockscreen_capture_" + std::to_wstring(slot) + L".jpg";
    }
    return dir + L"\\lockscreen_capture.jpg";
}

std::wstring LockScreenManager::GetDesktopPlaceholderImagePathJpg(int slot) const {
    const std::wstring& dir = GetAppDataDirWide();
    if (slot >= 0) {
        return dir + L"\\active_wallpaper_placeholder_" + std::to_wstring(slot) + L".jpg";
    }
    return dir + L"\\active_wallpaper_placeholder.jpg";
}

bool LockScreenManager::SetNativeDesktopWallpaperFile(const std::wstring& imagePath) {
    if (imagePath.empty()) return false;

    ComPtr<IDesktopWallpaper> pDesktopWallpaper;
    HRESULT hr = CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pDesktopWallpaper));
    if (SUCCEEDED(hr) && pDesktopWallpaper) {
        if (SUCCEEDED(pDesktopWallpaper->SetWallpaper(nullptr, imagePath.c_str()))) {
            return true;
        }
    }

    SystemParametersInfoW(
        SPI_SETDESKWALLPAPER,
        0,
        reinterpret_cast<void*>(const_cast<wchar_t*>(imagePath.c_str())),
        SPIF_UPDATEINIFILE
    );
    return true;
}

void LockScreenManager::SyncFromVideoAsync(
    const std::string& video_path,
    int target_w,
    int target_h,
    bool syncLockScreen,
    bool syncNativeDesktop,
    double timestamp_sec,
    bool force
) {
    if (video_path.empty() || m_shutting_down.load()) return;
    if (!syncLockScreen && !syncNativeDesktop) return;

    if (target_w <= 0 || target_h <= 0) {
        target_w = GetSystemMetrics(SM_CXSCREEN);
        target_h = GetSystemMetrics(SM_CYSCREEN);
        if (target_w <= 0) target_w = 1920;
        if (target_h <= 0) target_h = 1080;
    }

    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    std::wstring wide_video = Utf8ToWide(video_path);
    if (wide_video.empty() || !GetFileAttributesExW(wide_video.c_str(), GetFileExInfoStandard, &fad)) {
        return;
    }
    uint64_t file_size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    int64_t ts_ms = static_cast<int64_t>((std::max)(0.0, timestamp_sec) * 1000.0 + 0.5);
    std::string sync_key = Fnv1a64Hex(
        NormalizePathKey(video_path) + "|" +
        std::to_string(file_size) + "|" +
        std::to_string(ts_ms) + "|" +
        std::to_string(target_w) + "x" + std::to_string(target_h)
    );

    if (!force) {
        if (syncNativeDesktop &&
            ReadStampFile(GetDesktopStampPath()) == sync_key &&
            HasValidImageFile(GetDesktopPlaceholderImagePathJpg(-1))) {
            syncNativeDesktop = false;
        }
        if (syncLockScreen &&
            ReadStampFile(GetLockScreenStampPath()) == sync_key &&
            HasValidImageFile(GetTempImagePathJpg(-1))) {
            syncLockScreen = false;
        }
        if (!syncLockScreen && !syncNativeDesktop) {
            return; // Already up-to-date on disk: 0 threads, 0 decode RAM, 0.00% CPU
        }
    }

    std::thread stale_thread;
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        // Keep only the latest pending task for identical target flags so rapid wallpaper switches never pile up
        m_queue.erase(
            std::remove_if(m_queue.begin(), m_queue.end(), [&](const VideoSyncTask& t) {
                return t.syncLockScreen == syncLockScreen && t.syncNativeDesktop == syncNativeDesktop;
            }),
            m_queue.end()
        );
        m_queue.push_back({ video_path, target_w, target_h, syncLockScreen, syncNativeDesktop, timestamp_sec, sync_key });

        if (!m_worker_active) {
            if (m_worker_thread.joinable()) {
                stale_thread = std::move(m_worker_thread);
            }
            m_worker_active = true;
            m_worker_thread = std::thread(&LockScreenManager::WorkerLoop, this);
        }
    }
    if (stale_thread.joinable()) {
        stale_thread.join();
    }
}

void LockScreenManager::WorkerLoop() {
    while (!m_shutting_down.load()) {
        VideoSyncTask task;
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            if (m_queue.empty()) {
                m_worker_active = false;
                break; // On-demand exit: 0 idle background thread when finished!
            }
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }
        ProcessSyncTask(task);
    }

    mi_collect(true);
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
}

void LockScreenManager::ProcessSyncTask(const VideoSyncTask& task) {
    int deskSlot = m_desktop_slot.fetch_xor(1);
    int lockSlot = m_lock_slot.fetch_xor(1);

    std::wstring desktopSlotPath = GetDesktopPlaceholderImagePathJpg(deskSlot);
    std::wstring desktopCanonical = GetDesktopPlaceholderImagePathJpg(-1);
    std::wstring lockSlotPath = GetTempImagePathJpg(lockSlot);
    std::wstring lockCanonical = GetTempImagePathJpg(-1);

    std::string primaryJpg = task.syncNativeDesktop ? WideToUtf8(desktopSlotPath) : WideToUtf8(lockSlotPath);
    bool extraction_done = false;

    AVFormatContext* fmt_ctx = avformat_alloc_context();
    if (fmt_ctx) {
        fmt_ctx->flags |= AVFMT_FLAG_NOBUFFER;
        fmt_ctx->probesize = 64 * 1024;
        fmt_ctx->max_analyze_duration = 500000;
    }

    if (fmt_ctx && avformat_open_input(&fmt_ctx, task.video_path.c_str(), nullptr, nullptr) >= 0) {
        if (avformat_find_stream_info(fmt_ctx, nullptr) >= 0) {
            int video_stream_idx = -1;
            for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
                if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_idx < 0) {
                    video_stream_idx = static_cast<int>(i);
                } else {
                    fmt_ctx->streams[i]->discard = AVDISCARD_ALL;
                }
            }

            if (video_stream_idx >= 0) {
                AVCodecParameters* codecpar = fmt_ctx->streams[video_stream_idx]->codecpar;
                const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
                if (decoder) {
                    AVCodecContext* codec_ctx = avcodec_alloc_context3(decoder);
                    if (codec_ctx) {
                        codec_ctx->thread_count = 1; // Minimal single-thread pool for background snapshot
                        codec_ctx->thread_type = FF_THREAD_SLICE;
                        if (avcodec_parameters_to_context(codec_ctx, codecpar) >= 0 &&
                            avcodec_open2(codec_ctx, decoder, nullptr) >= 0) {

                            AVRational stream_tb = fmt_ctx->streams[video_stream_idx]->time_base;
                            AVRational frame_rate = fmt_ctx->streams[video_stream_idx]->avg_frame_rate;
                            if (frame_rate.num <= 0 || frame_rate.den <= 0) {
                                frame_rate = fmt_ctx->streams[video_stream_idx]->r_frame_rate;
                            }
                            double fps = (frame_rate.num > 0 && frame_rate.den > 0)
                                ? (static_cast<double>(frame_rate.num) / frame_rate.den)
                                : 30.0;
                            if (fps <= 0.0) fps = 30.0;

                            int64_t target_ts = 0;
                            int64_t near_window_ts = 0;
                            if (stream_tb.den > 0) {
                                int64_t req_ts = av_rescale_q(
                                    static_cast<int64_t>((std::max)(0.0, task.timestamp_sec) * AV_TIME_BASE),
                                    AV_TIME_BASE_Q,
                                    stream_tb
                                );
                                int64_t dur = fmt_ctx->streams[video_stream_idx]->duration;
                                target_ts = (dur > 0 && req_ts >= dur) ? (dur - 1) : req_ts;
                                near_window_ts = av_rescale_q(AV_TIME_BASE / 4, AV_TIME_BASE_Q, stream_tb);
                            }
                            int64_t half_frame_ts = (stream_tb.den > 0 && stream_tb.num > 0)
                                ? static_cast<int64_t>((0.5 / fps) * stream_tb.den / stream_tb.num)
                                : 0;

                            av_seek_frame(fmt_ctx, video_stream_idx, target_ts, AVSEEK_FLAG_BACKWARD);
                            avcodec_flush_buffers(codec_ctx);
                            if (target_ts > near_window_ts) {
                                codec_ctx->skip_frame = AVDISCARD_NONREF; // Skip non-ref B-frames while fast-forwarding GOP
                            }

                            AVPacket* pkt = av_packet_alloc();
                            AVFrame* frame = av_frame_alloc();
                            AVFrame* last_good = av_frame_alloc();
                            bool has_last_good = false;
                            int frames_received = 0;

                            auto SaveFrameToJpg = [&](AVFrame* src) -> bool {
                                int out_w = (task.target_w > 0) ? task.target_w : src->width;
                                int out_h = (task.target_h > 0) ? task.target_h : src->height;
                                out_w = (std::max)(128, (out_w / 2) * 2);
                                out_h = (std::max)(128, (out_h / 2) * 2);

                                SwsContext* sws = sws_getContext(
                                    src->width, src->height, static_cast<AVPixelFormat>(src->format),
                                    out_w, out_h, AV_PIX_FMT_RGB24,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr
                                );
                                if (!sws) return false;

                                int cs = (src->colorspace != AVCOL_SPC_UNSPECIFIED)
                                    ? src->colorspace
                                    : ((src->height >= 720) ? SWS_CS_ITU709 : SWS_CS_ITU601);
                                const int* inv_table = sws_getCoefficients(cs);
                                const int* table = sws_getCoefficients(SWS_CS_DEFAULT);
                                int srcRange = (src->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
                                sws_setColorspaceDetails(sws, inv_table, srcRange, table, 1, 0, 1 << 16, 1 << 16);

                                std::vector<uint8_t> rgbData(static_cast<size_t>(out_w) * out_h * 3);
                                uint8_t* dstSlice[4] = { rgbData.data(), nullptr, nullptr, nullptr };
                                int dstStride[4] = { out_w * 3, 0, 0, 0 };

                                int scaled = sws_scale(sws, src->data, src->linesize, 0, src->height, dstSlice, dstStride);
                                sws_freeContext(sws);

                                if (scaled != out_h) return false;
                                return stbi_write_jpg(primaryJpg.c_str(), out_w, out_h, 3, rgbData.data(), 92) != 0;
                            };

                            while (!m_shutting_down.load() && av_read_frame(fmt_ctx, pkt) >= 0) {
                                if (pkt->stream_index == video_stream_idx) {
                                    if (avcodec_send_packet(codec_ctx, pkt) >= 0) {
                                        while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                                            if (frame->width > 0 && frame->height > 0 && !(frame->flags & AV_FRAME_FLAG_CORRUPT)) {
                                                frames_received++;
                                                int64_t pts = (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                                    ? frame->best_effort_timestamp
                                                    : frame->pts;

                                                if (pts != AV_NOPTS_VALUE && pts + near_window_ts >= target_ts) {
                                                    codec_ctx->skip_frame = AVDISCARD_DEFAULT;
                                                }

                                                if (pts != AV_NOPTS_VALUE && pts + half_frame_ts < target_ts && frames_received < 600) {
                                                    av_frame_unref(last_good);
                                                    av_frame_move_ref(last_good, frame);
                                                    has_last_good = true;
                                                    continue;
                                                }

                                                int avg_luma = 128;
                                                if (frame->data[0]) {
                                                    int luma_sum = 0;
                                                    for (int i = 0; i < 100; ++i) {
                                                        int sx = (i % 10) * (frame->width / 10);
                                                        int sy = (i / 10) * (frame->height / 10);
                                                        luma_sum += frame->data[0][sy * frame->linesize[0] + sx];
                                                    }
                                                    avg_luma = luma_sum / 100;
                                                }

                                                bool accept = (task.timestamp_sec != 1.0) || (avg_luma > 20 || frames_received > 60);
                                                if (accept) {
                                                    av_frame_unref(last_good);
                                                    has_last_good = false;
                                                    extraction_done = SaveFrameToJpg(frame);
                                                    av_frame_unref(frame);
                                                    break;
                                                }
                                                av_frame_unref(last_good);
                                                av_frame_move_ref(last_good, frame);
                                                has_last_good = true;
                                                continue;
                                            }
                                            av_frame_unref(frame);
                                        }
                                        if (extraction_done) {
                                            av_packet_unref(pkt);
                                            break;
                                        }
                                    }
                                }
                                av_packet_unref(pkt);
                            }

                            if (!extraction_done && has_last_good && !m_shutting_down.load()) {
                                extraction_done = SaveFrameToJpg(last_good);
                            }

                            av_frame_free(&last_good);
                            av_frame_free(&frame);
                            av_packet_free(&pkt);
                        }
                        avcodec_free_context(&codec_ctx);
                    }
                }
            }
        }
        avformat_close_input(&fmt_ctx);
    }

    if (extraction_done && !m_shutting_down.load()) {
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hrCo)) hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        if (task.syncNativeDesktop) {
            CopyFileW(desktopSlotPath.c_str(), desktopCanonical.c_str(), FALSE);
            if (SetNativeDesktopWallpaperFile(desktopSlotPath)) {
                WriteStampFile(GetDesktopStampPath(), task.sync_key);
            }
        }
        if (task.syncLockScreen) {
            if (task.syncNativeDesktop) {
                CopyFileW(desktopSlotPath.c_str(), lockSlotPath.c_str(), FALSE);
            }
            CopyFileW(lockSlotPath.c_str(), lockCanonical.c_str(), FALSE);
            if (SetLockScreenImage(lockSlotPath)) {
                WriteStampFile(GetLockScreenStampPath(), task.sync_key);
            }
            if (!IsWindows8OrGreater()) {
                SetLockScreenImageWin7(lockSlotPath);
            }
        }

        if (SUCCEEDED(hrCo)) {
            CoUninitialize();
        }
    }
}

bool LockScreenManager::SetLockScreenImage(const std::wstring& imagePath) {
    if (imagePath.empty()) return false;

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {}
    try {
        auto file = winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(imagePath).get();
        winrt::Windows::System::UserProfile::LockScreen::SetImageFileAsync(file).get();
    } catch (...) {}

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

    HKEY hKey = nullptr;
    if (RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Lock Screen\\Creative",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr
    ) == ERROR_SUCCESS) {
        DWORD cb = static_cast<DWORD>((imagePath.length() + 1) * sizeof(wchar_t));
        const auto* bytes = reinterpret_cast<const BYTE*>(imagePath.c_str());
        RegSetValueExW(hKey, L"LockScreenImage", 0, REG_SZ, bytes, cb);
        RegSetValueExW(hKey, L"LandscapeAssetPath", 0, REG_SZ, bytes, cb);
        RegSetValueExW(hKey, L"PortraitAssetPath", 0, REG_SZ, bytes, cb);
        RegCloseKey(hKey);
    }

    if (RegCreateKeyExW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\PersonalizationCSP",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr
    ) == ERROR_SUCCESS) {
        DWORD status = 1;
        DWORD cb = static_cast<DWORD>((imagePath.length() + 1) * sizeof(wchar_t));
        const auto* bytes = reinterpret_cast<const BYTE*>(imagePath.c_str());
        RegSetValueExW(hKey, L"LockScreenImagePath", 0, REG_SZ, bytes, cb);
        RegSetValueExW(hKey, L"LockScreenImageUrl", 0, REG_SZ, bytes, cb);
        RegSetValueExW(hKey, L"LockScreenImageStatus", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&status), sizeof(status));
        RegCloseKey(hKey);
    }

    return true;
}

bool LockScreenManager::SetLockScreenImageWin7(const std::wstring& imagePath) {
    std::wstring oobeDir = L"C:\\Windows\\System32\\oobe\\info\\backgrounds";
    CreateDirectoryW(L"C:\\Windows\\System32\\oobe\\info", nullptr);
    CreateDirectoryW(oobeDir.c_str(), nullptr);

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

    RegSetValueExW(hKey, L"SCRNSAVE.EXE", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(scrPath.c_str()),
        static_cast<DWORD>((scrPath.length() + 1) * sizeof(wchar_t)));

    const wchar_t* active = L"1";
    RegSetValueExW(hKey, L"ScreenSaveActive", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(active), static_cast<DWORD>((wcslen(active) + 1) * sizeof(wchar_t)));

    const wchar_t* secure = secureOnResume ? L"1" : L"0";
    RegSetValueExW(hKey, L"ScreenSaverIsSecure", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(secure), static_cast<DWORD>((wcslen(secure) + 1) * sizeof(wchar_t)));

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

} // namespace miniwp
