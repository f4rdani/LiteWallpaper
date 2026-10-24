#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui_modals.h"
#include "ui_internal.h"
#include "ui_widgets.h"
#include "thumbnail_manager.h"
#include "icons_fontawesome6.h"
#include "core/config.h"
#include "core/engine_controller.h"
#include "core/video_optimizer.h"
#include "core/win_utils.h"
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace miniwp {
namespace ui {

// Modal state for video optimization prompt
static bool g_showOptimizeModal = false;
static std::string g_pendingOptimizePath;
static std::string g_pendingOptimizeAction;
static int g_pendingSourceW = 0;
static int g_pendingSourceH = 0;
static int g_pendingTargetW = 1920;
static int g_pendingTargetH = 1080;
static bool g_rememberDownscaleChoice = false;

// Modal state for Static Frame Capture & Target Selector
static bool g_showCaptureModal = false;
static std::string g_captureModalPath;
static double g_captureModalDuration = 10.0;
static double g_captureModalFps = 30.0;
static int g_captureModalSourceW = 1920;
static int g_captureModalSourceH = 1080;
static float g_captureModalTimeSec = 1.0f;
static int g_captureModalFrameNum = 30;
static bool g_captureModalTargetDesktop = true;
static bool g_captureModalTargetLockscreen = true;
static std::string g_captureModalStatus;

// Thread-safe preview state in capture modal (joinable worker, mutex-protected BGRA buffer)
static ComPtr<ID3D11ShaderResourceView> g_captureModalPreviewSRV;
static float g_captureModalPreviewSec = -1.0f;
static constexpr int kPreviewW = 320;
static constexpr int kPreviewH = 180;

static std::mutex g_previewMutex;
static std::vector<uint8_t> g_previewReadyBgra;
static float g_previewReadySec = -1.0f;
static bool g_previewUploadReady = false;
static bool g_previewQueued = false;
static std::string g_previewQueuedPath;
static float g_previewQueuedSec = 0.0f;
static std::atomic<bool> g_previewWorkerRunning{false};
static std::atomic<bool> g_previewStopRequested{false};
static std::thread g_previewWorkerThread;

static bool g_captureModalAutoPreviewRequested = false;
static uint64_t g_captureModalLastChangeTick = 0;

bool IsAnyModalActive() noexcept {
    return g_showOptimizeModal ||
           g_showCaptureModal ||
           g_previewWorkerRunning.load(std::memory_order_relaxed) ||
           g_captureModalAutoPreviewRequested;
}

void OpenOptimizeModal(
    const std::string& path,
    const std::string& action,
    int srcW,
    int srcH,
    int targetW,
    int targetH
) {
    g_pendingOptimizePath = path;
    g_pendingOptimizeAction = action;
    g_pendingSourceW = srcW;
    g_pendingSourceH = srcH;
    g_pendingTargetW = targetW;
    g_pendingTargetH = targetH;
    g_showOptimizeModal = true;
    MarkUiInteraction();
}

static void PreviewWorkerLoop(std::string initialPath, float initialSec) {
    std::string currentPath = std::move(initialPath);
    float currentSec = initialSec;

    while (!g_previewStopRequested.load(std::memory_order_acquire)) {
        std::vector<uint8_t> bgra;
        if (ThumbnailManager::ExtractFrameToBGRA(currentPath, bgra, kPreviewW, kPreviewH, static_cast<double>(currentSec))) {
            std::lock_guard<std::mutex> lock(g_previewMutex);
            if (!g_previewStopRequested.load(std::memory_order_relaxed)) {
                g_previewReadyBgra = std::move(bgra);
                g_previewReadySec = currentSec;
                g_previewUploadReady = true;
            }
        }

        std::lock_guard<std::mutex> lock(g_previewMutex);
        if (g_previewStopRequested.load(std::memory_order_relaxed) || !g_previewQueued) {
            g_previewWorkerRunning.store(false, std::memory_order_release);
            return;
        }
        currentPath = g_previewQueuedPath;
        currentSec = g_previewQueuedSec;
        g_previewQueued = false;
    }

    g_previewWorkerRunning.store(false, std::memory_order_release);
}

static void RequestCaptureModalPreview() {
    if (g_captureModalPath.empty()) return;

    std::thread oldFinished;
    {
        std::lock_guard<std::mutex> lock(g_previewMutex);
        if (g_previewWorkerRunning.load(std::memory_order_relaxed)) {
            g_previewQueuedPath = g_captureModalPath;
            g_previewQueuedSec = g_captureModalTimeSec;
            g_previewQueued = true;
            return;
        }
        if (g_previewWorkerThread.joinable()) {
            oldFinished = std::move(g_previewWorkerThread);
        }
        g_previewQueued = false;
        g_previewStopRequested.store(false, std::memory_order_relaxed);
        g_previewWorkerRunning.store(true, std::memory_order_release);
        g_previewWorkerThread = std::thread(PreviewWorkerLoop, g_captureModalPath, g_captureModalTimeSec);
    }
    if (oldFinished.joinable()) {
        oldFinished.join();
    }
}

void ShutdownCaptureModalWorker() {
    g_previewStopRequested.store(true, std::memory_order_release);
    std::thread toJoin;
    {
        std::lock_guard<std::mutex> lock(g_previewMutex);
        g_previewQueued = false;
        if (g_previewWorkerThread.joinable()) {
            toJoin = std::move(g_previewWorkerThread);
        }
    }
    if (toJoin.joinable()) {
        toJoin.join();
    }
    g_previewWorkerRunning.store(false, std::memory_order_relaxed);
    g_previewStopRequested.store(false, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(g_previewMutex);
        g_previewUploadReady = false;
        std::vector<uint8_t>().swap(g_previewReadyBgra);
    }

    g_captureModalPreviewSRV.Reset();
    g_captureModalAutoPreviewRequested = false;
    g_showCaptureModal = false;
    g_showOptimizeModal = false;
}

void OpenCaptureModal(const std::string& video_path, int targetFilter) {
    if (video_path.empty()) return;

    g_captureModalPath = video_path;
    auto probe = VideoOptimizer::Probe(video_path);
    if (probe.valid) {
        g_captureModalDuration = (probe.duration > 0.0) ? probe.duration : 10.0;
        g_captureModalFps = (probe.fps > 0.0) ? probe.fps : 30.0;
        g_captureModalSourceW = probe.width > 0 ? probe.width : 1920;
        g_captureModalSourceH = probe.height > 0 ? probe.height : 1080;
    } else {
        g_captureModalDuration = (g_ui.daemon.duration > 0.0) ? g_ui.daemon.duration : 10.0;
        g_captureModalFps = (g_ui.daemon.videoFps > 0.0) ? g_ui.daemon.videoFps : 30.0;
        g_captureModalSourceW = g_ui.daemon.width > 0 ? g_ui.daemon.width : 1920;
        g_captureModalSourceH = g_ui.daemon.height > 0 ? g_ui.daemon.height : 1080;
    }

    auto& cfg = g_config.Get();
    if (targetFilter == 1) {
        g_captureModalTargetDesktop = true;
        g_captureModalTargetLockscreen = false;
    } else if (targetFilter == 2) {
        g_captureModalTargetDesktop = false;
        g_captureModalTargetLockscreen = true;
    } else {
        g_captureModalTargetDesktop = cfg.update_desktop_wallpaper;
        g_captureModalTargetLockscreen = cfg.update_lockscreen;
        if (!g_captureModalTargetDesktop && !g_captureModalTargetLockscreen) {
            g_captureModalTargetDesktop = true;
        }
    }

    std::string stemUtf8 = PathToUtf8(PathFromUtf8(video_path).stem());
    if (targetFilter == 1 && cfg.desktop_static_source_mode == 1 && PathsEqualNormalized(cfg.desktop_static_video_path, video_path)) {
        g_captureModalTimeSec = static_cast<float>(cfg.desktop_static_timestamp);
    } else if (targetFilter == 2 && cfg.lockscreen_source_mode == 1 && PathsEqualNormalized(cfg.lockscreen_video_path, video_path)) {
        g_captureModalTimeSec = static_cast<float>(cfg.lockscreen_timestamp);
    } else if (!g_ui.daemon.currentVideo.empty() &&
               (PathsEqualNormalized(g_ui.daemon.currentVideo, video_path) ||
                (!stemUtf8.empty() && g_ui.daemon.currentVideo.find(stemUtf8) != std::string::npos))) {
        g_captureModalTimeSec = static_cast<float>(g_ui.daemon.currentTimeSec);
    } else {
        g_captureModalTimeSec = 1.0f;
    }

    if (g_captureModalTimeSec > g_captureModalDuration && g_captureModalDuration > 0.0) {
        g_captureModalTimeSec = static_cast<float>(g_captureModalDuration * 0.5);
    }
    g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
    g_captureModalStatus.clear();
    g_captureModalPreviewSRV.Reset();
    g_captureModalPreviewSec = -1.0f;
    g_captureModalAutoPreviewRequested = false;
    g_captureModalLastChangeTick = 0;
    {
        std::lock_guard<std::mutex> lock(g_previewMutex);
        g_previewQueued = false;
        g_previewUploadReady = false;
    }
    g_showCaptureModal = true;
    MarkUiInteraction();
}

void RenderCaptureFrameModal() {
    if (g_showCaptureModal) {
        ImGui::OpenPopup("Capture Static Wallpaper / Lock Screen Frame");
        if (!g_captureModalPreviewSRV && !g_previewWorkerRunning.load(std::memory_order_relaxed) && g_captureModalPreviewSec < 0.0f) {
            RequestCaptureModalPreview();
        }

        if (g_captureModalAutoPreviewRequested) {
            uint64_t nowTick = GetTickCount64();
            if (nowTick >= g_captureModalLastChangeTick + 150) {
                g_captureModalAutoPreviewRequested = false;
                RequestCaptureModalPreview();
            }
        }
    }

    // Reap finished preview worker & upload pending BGRA texture safely on UI thread
    {
        std::vector<uint8_t> readyBgra;
        float readySec = -1.0f;
        bool shouldUpload = false;
        std::thread finishedThread;
        {
            std::lock_guard<std::mutex> lock(g_previewMutex);
            if (g_previewUploadReady) {
                readyBgra = std::move(g_previewReadyBgra);
                readySec = g_previewReadySec;
                g_previewUploadReady = false;
                shouldUpload = true;
            }
            if (!g_previewWorkerRunning.load(std::memory_order_relaxed) && g_previewWorkerThread.joinable()) {
                finishedThread = std::move(g_previewWorkerThread);
            }
        }
        if (finishedThread.joinable()) {
            finishedThread.join();
        }
        if (shouldUpload && g_ui.device && !readyBgra.empty()) {
            g_captureModalPreviewSRV = ThumbnailManager::CreateSRVFromBGRA(
                g_ui.device,
                readyBgra.data(),
                kPreviewW,
                kPreviewH
            );
            g_captureModalPreviewSec = readySec;
        }
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(ImVec2(center.x, 26.0f), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(680, 580));
    ImGui::SetNextWindowSizeConstraints(ImVec2(560, 300), ImVec2(680, 700));

    if (ImGui::BeginPopupModal("Capture Static Wallpaper / Lock Screen Frame", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        if (g_ui.fontSemibold) ImGui::PushFont(g_ui.fontSemibold);
        ImGui::TextColored(ImVec4(0.94f, 0.95f, 0.97f, 1.0f), ICON_FA_CAMERA "  Select Frame & Injection Destination");
        if (g_ui.fontSemibold) ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        std::string filename = PathToUtf8(PathFromUtf8(g_captureModalPath).filename());
        if (filename.empty()) filename = g_captureModalPath;

        auto& cfg = g_config.Get();
        if (!cfg.gallery_history.empty()) {
            int currentIdx = -1;
            for (size_t i = 0; i < cfg.gallery_history.size(); ++i) {
                if (PathsEqualNormalized(cfg.gallery_history[i], g_captureModalPath)) {
                    currentIdx = static_cast<int>(i);
                    break;
                }
            }
            std::string currentLabel = (currentIdx >= 0)
                                           ? PathToUtf8(PathFromUtf8(cfg.gallery_history[currentIdx]).filename())
                                           : filename;
            ImGui::SetNextItemWidth(450.0f);
            if (ImGui::BeginCombo("Select Video Source", currentLabel.c_str())) {
                for (size_t i = 0; i < cfg.gallery_history.size(); ++i) {
                    bool isSel = (static_cast<int>(i) == currentIdx);
                    std::string itemLabel = PathToUtf8(PathFromUtf8(cfg.gallery_history[i]).filename());
                    if (ImGui::Selectable(itemLabel.c_str(), isSel)) {
                        int filter = (g_captureModalTargetDesktop && !g_captureModalTargetLockscreen) ? 1 :
                                     (!g_captureModalTargetDesktop && g_captureModalTargetLockscreen) ? 2 : 0;
                        OpenCaptureModal(cfg.gallery_history[i], filter);
                    }
                    if (isSel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        } else {
            ImGui::TextColored(ImVec4(0.92f, 0.93f, 0.95f, 1.0f), "Video Source: %s", filename.c_str());
        }

        int durMin = static_cast<int>(g_captureModalDuration) / 60;
        float durSec = static_cast<float>(g_captureModalDuration) - durMin * 60.0f;
        int totalFrames = static_cast<int>(g_captureModalDuration * g_captureModalFps);
        ImGui::TextColored(ImVec4(0.65f, 0.68f, 0.75f, 1.0f), "Resolution: %dx%d  |  Framerate: %.1f FPS  |  Duration: %02d:%05.2f (%d frames)",
            g_captureModalSourceW, g_captureModalSourceH, g_captureModalFps, durMin, durSec, totalFrames);
        ImGui::Spacing();

        if (g_ui.fontSemibold) ImGui::PushFont(g_ui.fontSemibold);
        ImGui::TextColored(ImVec4(0.94f, 0.95f, 0.97f, 1.0f), ICON_FA_DESKTOP "  Target Destination (Where to inject this frame):");
        if (g_ui.fontSemibold) ImGui::PopFont();
        ToggleSwitch("Apply to Windows Desktop Wallpaper (0s Instant Boot Visual)", &g_captureModalTargetDesktop);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Sets this frame directly into the native Windows Desktop Wallpaper.\nIt appears instantly in 0.0s upon Windows boot without black screen or delay.");
        }

        ToggleSwitch("Apply to Windows Lock Screen (Win + L Seamless Transition)", &g_captureModalTargetLockscreen);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Pre-caches this frame for the Windows Lock Screen.\nWhen pressing Win+L or locking the PC, this image is shown seamlessly.");
        }

        if (!g_captureModalTargetDesktop && !g_captureModalTargetLockscreen) {
            ImGui::TextColored(ImVec4(1.00f, 0.30f, 0.43f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " Please select at least one destination (Desktop, Lock Screen, or Both).");
        }

        ImGui::Spacing();
        ImGui::Separator();

        float maxTime = static_cast<float>((std::max)(0.1, g_captureModalDuration));

        if (StarlumeSliderFloat("Preview Time", &g_captureModalTimeSec, 0.0f, maxTime, 2, " s", 520.0f)) {
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            g_captureModalAutoPreviewRequested = true;
            g_captureModalLastChangeTick = GetTickCount64();
        }
        if (WasStarlumeSliderDeactivated()) {
            g_captureModalAutoPreviewRequested = false;
            RequestCaptureModalPreview();
        }

        ImGui::PushItemWidth(140.0f);
        if (ImGui::InputFloat("Time (seconds)", &g_captureModalTimeSec, 0.1f, 1.0f, "%.2f s")) {
            g_captureModalTimeSec = std::clamp(g_captureModalTimeSec, 0.0f, maxTime);
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            g_captureModalAutoPreviewRequested = true;
            g_captureModalLastChangeTick = GetTickCount64();
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            g_captureModalAutoPreviewRequested = false;
            RequestCaptureModalPreview();
        }
        ImGui::SameLine();
        if (ImGui::InputInt("Frame Number", &g_captureModalFrameNum, 1, 10)) {
            g_captureModalFrameNum = std::clamp(g_captureModalFrameNum, 0, (std::max)(1, totalFrames));
            g_captureModalTimeSec = static_cast<float>(g_captureModalFrameNum / g_captureModalFps);
            g_captureModalTimeSec = std::clamp(g_captureModalTimeSec, 0.0f, maxTime);
            g_captureModalAutoPreviewRequested = true;
            g_captureModalLastChangeTick = GetTickCount64();
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            g_captureModalAutoPreviewRequested = false;
            RequestCaptureModalPreview();
        }
        ImGui::PopItemWidth();

        ImGui::SameLine();
        if (HoloButton(ICON_FA_ROTATE " Preview Frame")) {
            RequestCaptureModalPreview();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Render a thumbnail preview of the selected frame");
        }

        ImGui::Spacing();
        if (HoloButtonSmall("0s (Start)")) {
            g_captureModalTimeSec = 0.0f;
            g_captureModalFrameNum = 0;
            RequestCaptureModalPreview();
        }
        ImGui::SameLine();
        if (HoloButtonSmall("25%")) {
            g_captureModalTimeSec = maxTime * 0.25f;
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            RequestCaptureModalPreview();
        }
        ImGui::SameLine();
        if (HoloButtonSmall("50% (Middle)")) {
            g_captureModalTimeSec = maxTime * 0.50f;
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            RequestCaptureModalPreview();
        }
        ImGui::SameLine();
        if (HoloButtonSmall("75%")) {
            g_captureModalTimeSec = maxTime * 0.75f;
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            RequestCaptureModalPreview();
        }
        ImGui::SameLine();
        if (HoloButtonSmall("End")) {
            g_captureModalTimeSec = (std::max)(0.0f, maxTime - 0.1f);
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            RequestCaptureModalPreview();
        }
        ImGui::SameLine();
        if (HoloButtonSmall(ICON_FA_CROSSHAIRS " Snap to Live Video")) {
            g_captureModalTimeSec = static_cast<float>(g_ui.daemon.currentTimeSec);
            g_captureModalTimeSec = std::clamp(g_captureModalTimeSec, 0.0f, maxTime);
            g_captureModalFrameNum = static_cast<int>(g_captureModalTimeSec * g_captureModalFps);
            RequestCaptureModalPreview();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Snap time to the exact position currently playing on your desktop");
        }

        ImGui::Spacing();
        float previewBoxW = 300.0f;
        float previewBoxH = 169.0f;
        bool isLoadingPreview = g_previewWorkerRunning.load(std::memory_order_relaxed) || g_captureModalAutoPreviewRequested;

        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + previewBoxW, p0.y + previewBoxH);

        if (g_captureModalPreviewSRV) {
            ImGui::Image((ImTextureID)g_captureModalPreviewSRV.Get(), ImVec2(previewBoxW, previewBoxH));
            if (isLoadingPreview) {
                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                draw_list->AddRectFilled(p0, p1, IM_COL32(9, 12, 21, 150), 4.0f);
                draw_list->AddRect(p0, p1, IM_COL32(56, 232, 255, 180), 4.0f);
                draw_list->AddText(ImVec2(p0.x + 95.0f, p0.y + 80.0f), IM_COL32(56, 232, 255, 255), ICON_FA_ROTATE " Loading Frame...");
            }
        } else {
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            draw_list->AddRectFilled(p0, p1, IM_COL32(17, 23, 38, 255), 4.0f);
            draw_list->AddRect(p0, p1, IM_COL32(56, 232, 255, 46), 4.0f);
            if (isLoadingPreview) {
                draw_list->AddText(ImVec2(p0.x + 80.0f, p0.y + 80.0f), IM_COL32(56, 232, 255, 255), ICON_FA_ROTATE " Extracting Preview...");
            } else {
                draw_list->AddText(ImVec2(p0.x + 65.0f, p0.y + 80.0f), IM_COL32(148, 163, 184, 255), "Drag slider to preview frame");
            }
            ImGui::Dummy(ImVec2(previewBoxW, previewBoxH));
        }

        ImGui::SameLine(0, 18.0f);
        ImGui::BeginGroup();
        {
            bool canApply = (g_captureModalTargetDesktop || g_captureModalTargetLockscreen);
            if (!canApply) ImGui::BeginDisabled();
            if (HoloButton(ICON_FA_CHECK "  Capture & Apply Static Wallpaper", ImVec2(320, 40))) {
                EngineController::SyncVisualWallpaper(
                    g_captureModalPath,
                    g_captureModalTargetDesktop,
                    g_captureModalTargetLockscreen,
                    static_cast<double>(g_captureModalTimeSec),
                    1
                );

                std::string destStr = (g_captureModalTargetDesktop && g_captureModalTargetLockscreen) ? "Desktop & Lock Screen" :
                                      (g_captureModalTargetDesktop ? "Desktop Wallpaper" : "Lock Screen");
                char statusMsg[256];
                sprintf_s(statusMsg, "Pristine static frame at %.2fs successfully injected to %s!", g_captureModalTimeSec, destStr.c_str());
                g_captureModalStatus = statusMsg;

                g_showCaptureModal = false;
                ImGui::CloseCurrentPopup();
            }
            if (!canApply) ImGui::EndDisabled();

            ImGui::Spacing();
            if (HoloButton("Cancel", ImVec2(320, 36))) {
                g_showCaptureModal = false;
                ImGui::CloseCurrentPopup();
            }

            if (!g_captureModalStatus.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.0f), "%s", g_captureModalStatus.c_str());
            }
        }
        ImGui::EndGroup();

        ImGui::EndPopup();
    }
}

void RenderOptimizeModal() {
    if (g_showOptimizeModal) {
        ImGui::OpenPopup("Optimize Video for Display?");
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(620, 310));

    if (ImGui::BeginPopupModal("Optimize Video for Display?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        if (g_ui.fontSemibold) ImGui::PushFont(g_ui.fontSemibold);
        ImGui::TextColored(ImVec4(0.94f, 0.95f, 0.97f, 1.0f), ICON_FA_COMPACT_DISC "  High-Resolution Video Detected");
        if (g_ui.fontSemibold) ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        int screen_w = GetSystemMetrics(SM_CXSCREEN);
        int screen_h = GetSystemMetrics(SM_CYSCREEN);
        if (screen_w <= 0) screen_w = 1920;
        if (screen_h <= 0) screen_h = 1080;

        auto& cfg = g_config.Get();

        ImGui::Text("Source Video Resolution : %dx%d", g_pendingSourceW, g_pendingSourceH);

        ImGui::Spacing();
        if (g_ui.fontSemibold) ImGui::PushFont(g_ui.fontSemibold);
        ImGui::TextColored(ImVec4(0.94f, 0.95f, 0.97f, 1.0f), ICON_FA_EXPAND "  Optimization Scaling Mode:");
        if (g_ui.fontSemibold) ImGui::PopFont();

        if (ImGui::RadioButton("Aspect Fit (Proportional - keeps entire frame intact)", &cfg.optimizer_crop_mode, 0)) {
            g_config.Save();
            InvalidateGalleryCache();
        }
        if (ImGui::RadioButton("Aspect Fill / Center Crop (Exact Full Screen 1080p - zero black bars)", &cfg.optimizer_crop_mode, 1)) {
            g_config.Save();
            InvalidateGalleryCache();
        }

        auto [target_w, target_h] = VideoOptimizer::CalculateTargetDimensions(
            g_pendingSourceW, g_pendingSourceH, screen_w, screen_h, cfg.optimizer_crop_mode
        );
        g_pendingTargetW = target_w;
        g_pendingTargetH = target_h;

        ImGui::Spacing();
        if (cfg.optimizer_crop_mode == 0) {
            ImGui::Text("Target Resolution : %dx%d (Proportional Letterbox)", g_pendingTargetW, g_pendingTargetH);
        } else {
            ImGui::Text("Target Resolution : %dx%d (Full Screen Center-Crop)", g_pendingTargetW, g_pendingTargetH);
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f),
            "*Optimizing saves up to 75%% GPU & VRAM with crystal-clear Lanczos downscaling!");

        ImGui::Spacing();
        ToggleSwitch("Remember my choice (Auto-downscale in the future)", &g_rememberDownscaleChoice);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (HoloButton(ICON_FA_DOWNLOAD "  Optimize Video (Recommended)", ImVec2(260, 36))) {
            if (g_rememberDownscaleChoice) {
                cfg.auto_downscale_highres = true;
                cfg.prompt_downscale = false;
                g_config.Save();
            }
            g_showOptimizeModal = false;
            ImGui::CloseCurrentPopup();
            StartVideoOptimization(g_pendingOptimizePath, g_pendingTargetW, g_pendingTargetH, g_pendingOptimizeAction, cfg.optimizer_crop_mode);
        }

        ImGui::SameLine();
        if (HoloButton("Play Original", ImVec2(130, 36))) {
            if (g_rememberDownscaleChoice) {
                cfg.auto_downscale_highres = false;
                cfg.prompt_downscale = false;
                g_config.Save();
            }
            g_showOptimizeModal = false;
            ImGui::CloseCurrentPopup();
            ApplyAction(g_pendingOptimizePath, g_pendingOptimizeAction);
        }

        ImGui::SameLine();
        if (HoloButton("Cancel", ImVec2(100, 36))) {
            g_showOptimizeModal = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void RenderOptimizationProgress() {
    if (!g_video_optimizer.IsRunning()) return;

    BeginModuleCard("OptProgressBanner", ImVec2(0, 100), 14.0f, ST_CARD_NORMAL, ImGuiWindowFlags_NoScrollbar);

    float prog = g_video_optimizer.GetProgress();
    std::string status = g_video_optimizer.GetCurrentStatus();

    ImGui::TextColored(ImVec4(0.945f, 0.961f, 0.976f, 1.00f), ICON_FA_ROTATE "  %s", status.c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - 110);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 5.0f);
    if (HoloButtonSmall("Cancel")) {
        g_video_optimizer.Cancel();
    }

    char progText[32];
    snprintf(progText, sizeof(progText), "%.0f%%", prog * 100.0f);
    StarlumeProgress(prog, 14.0f, progText);

    EndModuleCard();
    ImGui::Spacing();
}

} // namespace ui
} // namespace miniwp
