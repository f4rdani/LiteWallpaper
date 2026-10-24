#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui_pages.h"
#include "ui_internal.h"
#include "ui_modals.h"
#include "ui_widgets.h"
#include "settings_app.h"
#include "thumbnail_manager.h"
#include "icons_fontawesome6.h"
#include "core/config.h"
#include "core/engine_controller.h"
#include "core/engine_state.h"
#include "core/video_optimizer.h"
#include "core/win_utils.h"
#include "platform/win32/hardware_info.h"
#include "platform/win32/lockscreen_manager.h"

#include <windows.h>
#include <commdlg.h>
#include <dxgi.h>
#if __has_include(<dxgi1_4.h>)
#include <dxgi1_4.h>
#endif
#include <psapi.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <mimalloc.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace miniwp {
namespace ui {

namespace {

struct GalleryCardCacheEntry {
    std::string filename;
    VideoProbeResult probe;
    int target_w = 1920;
    int target_h = 1080;
    std::string opt_path;
    bool has_opt = false;
    bool is_already_optimal = false;
    std::string size_str;
    std::string meta_line;
    std::string meta_line_compact;
};

static std::unordered_map<std::string, GalleryCardCacheEntry> g_galleryCardCache;
static int g_cachedScreenW = 0;
static int g_cachedScreenH = 0;
static int g_cachedCropMode = -1;
static bool g_wasOptimizerRunning = false;

const GalleryCardCacheEntry& GetGalleryCardMetadata(
    const std::string& path,
    int screen_w,
    int screen_h,
    int crop_mode
) {
    if (g_cachedScreenW != screen_w || g_cachedScreenH != screen_h || g_cachedCropMode != crop_mode) {
        g_galleryCardCache.clear();
        g_cachedScreenW = screen_w;
        g_cachedScreenH = screen_h;
        g_cachedCropMode = crop_mode;
    }

    auto it = g_galleryCardCache.find(path);
    if (it != g_galleryCardCache.end()) {
        return it->second;
    }

    GalleryCardCacheEntry entry;
    fs::path p = PathFromUtf8(path);
    entry.filename = PathToUtf8(p.filename());
    if (entry.filename.empty()) entry.filename = path;

    entry.target_w = screen_w;
    entry.target_h = screen_h;
    entry.probe = VideoOptimizer::Probe(path);
    if (entry.probe.valid) {
        auto [pw, ph] = VideoOptimizer::CalculateTargetDimensions(
            entry.probe.width, entry.probe.height, screen_w, screen_h, crop_mode
        );
        entry.target_w = pw;
        entry.target_h = ph;
    }

    entry.opt_path = VideoOptimizer::GetOptimizedPath(path, entry.target_w, entry.target_h);
    entry.has_opt = VideoOptimizer::HasOptimizedCache(path, entry.target_w, entry.target_h);
    entry.is_already_optimal = (entry.probe.valid &&
                                entry.probe.width <= screen_w &&
                                entry.probe.height <= screen_h &&
                                entry.probe.fps <= 60.5);

    std::error_code fsec;
    if (fs::exists(p, fsec)) {
        uintmax_t bytes = fs::file_size(p, fsec);
        char size_buf[32] = {};
        if (bytes >= 1024ull * 1024ull * 1024ull) {
            snprintf(size_buf, sizeof(size_buf), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
        } else if (bytes >= 1024ull * 1024ull) {
            snprintf(size_buf, sizeof(size_buf), "%.1f MB", bytes / (1024.0 * 1024.0));
        } else if (bytes > 0) {
            snprintf(size_buf, sizeof(size_buf), "%.0f KB", bytes / 1024.0);
        }
        entry.size_str = size_buf;
    }

    if (entry.probe.valid) {
        char meta_buf[128];
        snprintf(meta_buf, sizeof(meta_buf), "%dx%d • %.0f FPS • %s",
                 entry.probe.width, entry.probe.height, entry.probe.fps,
                 entry.probe.codec_name.empty() ? "video" : entry.probe.codec_name.c_str());
        entry.meta_line = meta_buf;
        if (!entry.size_str.empty()) {
            entry.meta_line += " • ";
            entry.meta_line += entry.size_str;
        }

        char compact_buf[128];
        snprintf(compact_buf, sizeof(compact_buf), "%dx%d • %.0ffps",
                 entry.probe.width, entry.probe.height, entry.probe.fps);
        entry.meta_line_compact = compact_buf;
        if (!entry.size_str.empty()) {
            entry.meta_line_compact += " • ";
            entry.meta_line_compact += entry.size_str;
        }
    }

    auto [ins_it, _] = g_galleryCardCache.emplace(path, std::move(entry));
    mi_collect(true);
    return ins_it->second;
}

void PushStylishComboStyle() {
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.078f, 0.118f, 0.196f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.110f, 0.165f, 0.270f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.130f, 0.200f, 0.320f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.220f, 0.910f, 1.000f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.140f, 0.220f, 0.350f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.220f, 0.910f, 1.000f, 0.40f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.220f, 0.910f, 1.000f, 0.70f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.067f, 0.090f, 0.150f, 0.98f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.2f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 8.0f));
}

void PopStylishComboStyle() {
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(8);
}

void RenderOriginalTooltip(const VideoProbeResult& probe, const char* size_str) {
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        if (g_ui.fontSemibold) ImGui::PushFont(g_ui.fontSemibold);
        ImGui::TextColored(ImVec4(0.94f, 0.95f, 0.97f, 1.0f), ICON_FA_CIRCLE_INFO "  Original Video Details");
        if (g_ui.fontSemibold) ImGui::PopFont();
        ImGui::Separator();
        if (probe.valid) {
            const char* resCategory = (probe.width >= 3840) ? "4K Ultra HD" :
                                      (probe.width >= 2560) ? "1440p Quad HD" :
                                      (probe.width >= 1920) ? "1080p Full HD" : "Standard HD";
            ImGui::Text("Resolution : %d x %d (%s)", probe.width, probe.height, resCategory);
            ImGui::Text("Frame Rate : %.1f FPS", probe.fps);
            ImGui::Text("Codec      : %s", probe.codec_name.empty() ? "H.264 / AVC" : probe.codec_name.c_str());
            if (probe.duration > 0.0) {
                int totalSec = static_cast<int>(probe.duration);
                int mm = totalSec / 60;
                int ss = totalSec % 60;
                ImGui::Text("Duration   : %02d:%02d", mm, ss);
            }
        } else {
            ImGui::Text("Resolution : Probing on playback...");
        }
        if (size_str && size_str[0] != '\0') {
            ImGui::Text("File Size  : %s", size_str);
        }
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.65f, 0.70f, 0.80f, 1.00f), "Direct playback of original source with hardware acceleration.");
        ImGui::EndTooltip();
    }
}

void RenderGalleryDetailPanel(
    const std::string& path,
    const GalleryCardCacheEntry& meta,
    bool is_playing_opt,
    bool is_current,
    float detailW,
    float detailH
) {
    auto& cfg = g_config.Get();
    const char* size_str = meta.size_str.c_str();

    BeginModuleCard("GalleryDetail", ImVec2(detailW, detailH), 18.0f, ST_CARD_NORMAL, ImGuiWindowFlags_AlwaysVerticalScrollbar);
    {
        if (g_ui.fontSemibold) ImGui::PushFont(g_ui.fontSemibold);
        ImGui::TextColored(ImVec4(0.945f, 0.961f, 0.976f, 1.0f), ICON_FA_CIRCLE_INFO "  Detail Video");
        if (g_ui.fontSemibold) ImGui::PopFont();
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 38.0f);
        if (HoloButtonSmall(ICON_FA_XMARK)) { g_ui.detailVisible = false; }
        ImGui::Separator();
        ImGui::Spacing();

        float pvW = ImGui::GetContentRegionAvail().x;
        float pvH = pvW * 9.0f / 16.0f;
        ImVec2 pp0 = ImGui::GetCursorScreenPos();
        ImVec2 pp1 = ImVec2(pp0.x + pvW, pp0.y + pvH);
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        ID3D11ShaderResourceView* big_srv = ThumbnailManager::Instance().GetThumbnailSRV(g_ui.device, path);
        if (big_srv) {
            pdl->AddImageRounded((ImTextureID)big_srv, pp0, pp1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 12.0f);
        } else {
            pdl->AddRectFilled(pp0, pp1, IM_COL32(20, 22, 28, 255), 12.0f);
            pdl->AddRect(pp0, pp1, IM_COL32(45, 50, 62, 255), 12.0f);
            const char* lt = "Loading preview...";
            ImVec2 lts = ImGui::CalcTextSize(lt);
            pdl->AddText(ImVec2(pp0.x + (pvW - lts.x) * 0.5f, pp0.y + (pvH - lts.y) * 0.5f), IM_COL32(140, 145, 160, 255), lt);
        }
        pdl->AddRect(pp0, pp1, IM_COL32(56, 232, 255, 46), 12.0f);
        if (is_current) {
            pdl->AddRect(pp0, pp1, IM_COL32(255, 209, 102, 220), 12.0f, 0, 2.0f);
        }
        ImGui::Dummy(ImVec2(pvW, pvH + 4.0f));

        std::string badgeText;
        ImU32 bBg, bFg;
        if (is_current) {
            if (is_playing_opt) { badgeText = ICON_FA_BOLT "  ACTIVE (OPTIMIZED)"; bBg = IM_COL32(255, 209, 102, 38); bFg = IM_COL32(255, 209, 102, 255); }
            else { badgeText = ICON_FA_CIRCLE_PLAY "  ACTIVE (ORIGINAL)"; bBg = IM_COL32(56, 232, 255, 34); bFg = IM_COL32(56, 232, 255, 255); }
        } else if (meta.has_opt) { badgeText = ICON_FA_CHECK "  Optimized Ready"; bBg = IM_COL32(56, 232, 255, 26); bFg = IM_COL32(140, 220, 245, 255); }
        else if (meta.is_already_optimal) { badgeText = "1080p Optimal"; bBg = IM_COL32(255, 209, 102, 26); bFg = IM_COL32(255, 209, 102, 235); }
        else { badgeText = ICON_FA_FILM "  Video File"; bBg = IM_COL32(255, 255, 255, 16); bFg = IM_COL32(148, 163, 184, 255); }
        ImVec2 btsz = ImGui::CalcTextSize(badgeText.c_str());
        ImVec2 bcur = ImGui::GetCursorScreenPos();
        pdl->AddRectFilled(bcur, ImVec2(bcur.x + btsz.x + 18.0f, bcur.y + btsz.y + 8.0f), bBg, 8.0f);
        pdl->AddText(ImVec2(bcur.x + 9.0f, bcur.y + 4.0f), bFg, badgeText.c_str());
        ImGui::Dummy(ImVec2(btsz.x + 18.0f, btsz.y + 10.0f));

        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(meta.filename.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nPath: %s", meta.filename.c_str(), path.c_str());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(ImVec4(0.50f, 0.55f, 0.64f, 1.0f), "%s", path.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        int metaLines = meta.probe.valid ? 3 : 1;
        if (size_str && size_str[0] != '\0') metaLines++;
        float metaH = metaLines * ImGui::GetTextLineHeightWithSpacing() + 12.0f * 2.0f;
        BeginModuleCard("DetailMeta", ImVec2(0, metaH), 10.0f, ST_CARD_NORMAL, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, 12.0f, 12.0f);
        if (meta.probe.valid) {
            ImGui::Text("Resolusi  : %d x %d", meta.probe.width, meta.probe.height);
            ImGui::Text("Framerate : %.0f FPS", meta.probe.fps);
            ImGui::Text("Codec     : %s", meta.probe.codec_name.empty() ? "H.264 / AVC" : meta.probe.codec_name.c_str());
            if (size_str && size_str[0]) ImGui::Text("Ukuran    : %s", size_str);
        } else {
            ImGui::TextColored(ImVec4(0.58f, 0.64f, 0.72f, 1.0f), "Probing video info...");
            if (size_str && size_str[0]) ImGui::Text("Ukuran : %s", size_str);
        }
        EndModuleCard();
        ImGui::Spacing();

        float fullW = ImGui::GetContentRegionAvail().x;
        if (is_current) {
            if (HoloDangerButton(ICON_FA_STOP "  Stop Wallpaper", ImVec2(fullW, 0))) ApplyAction("", "stop");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop video wallpaper and return to Windows static desktop");
            ImGui::Spacing();
            if (is_playing_opt) {
                if (HoloButton(ICON_FA_PLAY "  Play Original", ImVec2(fullW, 0))) ApplyAction(path, "wallpaper");
                RenderOriginalTooltip(meta.probe, size_str);
            } else {
                if (meta.has_opt) {
                    if (HoloButton(ICON_FA_BOLT "  Switch to Optimized", ImVec2(fullW, 0))) ApplyAction(meta.opt_path, "wallpaper");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Switch to pre-rendered optimized version to save ~70% GPU");
                } else if (!meta.is_already_optimal) {
                    if (HoloButton(ICON_FA_DOWNLOAD "  Optimize Video", ImVec2(fullW, 0))) StartVideoOptimization(path, meta.target_w, meta.target_h, "wallpaper", cfg.optimizer_crop_mode);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pre-render hardware-optimized 1080p copy (Low GPU load)");
                }
            }
        } else {
            if (meta.has_opt) {
                if (HoloButton(ICON_FA_BOLT "  Play Optimized", ImVec2(fullW, 0))) ApplyAction(meta.opt_path, "wallpaper");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play pre-rendered optimized version (Low GPU usage)");
                if (HoloButton(ICON_FA_PLAY "  Play Original", ImVec2(fullW, 0))) ApplyAction(path, "wallpaper");
                RenderOriginalTooltip(meta.probe, size_str);
            } else if (meta.is_already_optimal) {
                if (HoloButton(ICON_FA_PLAY "  Play Wallpaper", ImVec2(fullW, 0))) RequestApplyVideo(path, "wallpaper");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play 1080p native video directly");
            } else {
                if (HoloButton(ICON_FA_PLAY "  Play Original", ImVec2(fullW, 0))) RequestApplyVideo(path, "wallpaper");
                RenderOriginalTooltip(meta.probe, size_str);
                if (HoloButton(ICON_FA_DOWNLOAD "  Optimize Video", ImVec2(fullW, 0))) StartVideoOptimization(path, meta.target_w, meta.target_h, "wallpaper", cfg.optimizer_crop_mode);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pre-render hardware-optimized 1080p copy (Low GPU load)");
            }
        }
        ImGui::Spacing();
        if (HoloButton(ICON_FA_CAMERA "  Capture Frame", ImVec2(fullW, 0))) OpenCaptureModal(path);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            if (g_ui.fontSemibold) ImGui::PushFont(g_ui.fontSemibold);
            ImGui::TextColored(ImVec4(0.94f, 0.95f, 0.97f, 1.0f), ICON_FA_CAMERA "  Capture Frame (Visual Wallpaper)");
            if (g_ui.fontSemibold) ImGui::PopFont();
            ImGui::Separator();
            ImGui::Text("Ambil gambar frame statis dari video ini untuk:");
            ImGui::BulletText("Desktop Wallpaper (0s instant boot visual)");
            ImGui::BulletText("Windows Lock Screen (Win+L seamless transition)");
            ImGui::EndTooltip();
        }
        if (HoloDangerButton(ICON_FA_TRASH_CAN "  Remove from Gallery", ImVec2(fullW, 0))) {
            if (is_current) ApplyAction("", "stop");
            ThumbnailManager::DeleteThumbnailCache(path);
            VideoOptimizer::DeleteOptimizedCache(path);
            cfg.RemoveFromGallery(path);
            g_config.Save();
            InvalidateGalleryCache();
            g_ui.detailVisible = false;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove video from gallery");
        ImGui::Dummy(ImVec2(1.0f, 56.0f));
    }
    EndModuleCard();
}

// Dedicated left action rail (Add / Refresh / Clear) — positioned in its own non-overlapping gutter.
void RenderSidePillRail(float pillX, float pillW, float rowTopY, float rowH) {
    auto& cfg = g_config.Get();
    const float sideBtn = pillW - 8.0f;
    const float sideGap = 8.0f;
    const float sidePillH = sideBtn * 3.0f + sideGap * 2.0f + 16.0f;
    float sidePillY = (rowH - sidePillH) * 0.5f;
    if (sidePillY < 4.0f) sidePillY = 4.0f;
    ImVec2 pillPos = ImVec2(pillX, rowTopY + sidePillY);
    DrawFloatingShadow(pillPos, ImVec2(pillPos.x + pillW, pillPos.y + sidePillH), pillW * 0.5f);
    ImGui::SetCursorScreenPos(pillPos);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, pillW * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.070f, 0.102f, 0.168f, 0.88f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.220f, 0.910f, 1.000f, 0.42f));
    ImGui::BeginChild(
        "SideMenuPill",
        ImVec2(pillW, sidePillH),
        true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus
    );
    {
        ImGui::PushID("SideMenu");
        ImGui::SetWindowFontScale(1.25f);
        if (HoloIconButton("##pill_add", ICON_FA_PLUS, sideBtn, sideBtn, 12.0f, false, false, "Tambah video")) {
            wchar_t filename[1024] = L"";
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = g_ui.hWnd;
            ofn.lpstrFilter = L"Video Files (*.mp4;*.webm;*.mkv;*.avi;*.mov)\0*.mp4;*.webm;*.mkv;*.avi;*.mov\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = filename;
            ofn.nMaxFile = static_cast<DWORD>(IM_ARRAYSIZE(filename));
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

            if (GetOpenFileNameW(&ofn)) {
                RequestApplyVideo(WideToUtf8(filename), "wallpaper");
            }
        }
        if (HoloIconButton("##pill_refresh", ICON_FA_ROTATE, sideBtn, sideBtn, 12.0f, false, false, "Refresh galeri")) {
            g_config.Load();
            InvalidateGalleryCache();
        }
        if (HoloIconButton("##pill_clear", ICON_FA_TRASH_CAN, sideBtn, sideBtn, 12.0f, true, false, "Hapus semua video")) {
            for (const auto& pp2 : cfg.gallery_history) {
                ThumbnailManager::DeleteThumbnailCache(pp2);
                VideoOptimizer::DeleteOptimizedCache(pp2);
            }
            cfg.gallery_history.clear();
            g_config.Save();
            InvalidateGalleryCache();
        }
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopID();
    }
    ImGui::EndChild();
    {
        ImDrawList* pdl = ImGui::GetWindowDrawList();
        ImVec2 pmn = ImGui::GetItemRectMin(), pmx = ImGui::GetItemRectMax();
        pdl->AddLine(ImVec2(pmn.x + 13.0f, pmn.y + 1.5f), ImVec2(pmx.x - 13.0f, pmn.y + 1.5f), IM_COL32(56, 232, 255, 75), 1.0f);
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

} // namespace

void InvalidateGalleryCache() noexcept {
    g_galleryCardCache.clear();
}

void ClearGalleryCardCache() {
    std::unordered_map<std::string, GalleryCardCacheEntry>().swap(g_galleryCardCache);
}

void RenderGalleryTab() {
    bool optRunning = g_video_optimizer.IsRunning();
    if (g_wasOptimizerRunning && !optRunning) {
        InvalidateGalleryCache();
    }
    g_wasOptimizerRunning = optRunning;

    auto& cfg = g_config.Get();
    auto galleryCopy = cfg.gallery_history;

    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    if (screen_w <= 0) screen_w = 1920;
    if (screen_h <= 0) screen_h = 1080;

    // Dedicated left rail geometry so SideMenuPill never overlaps video cards
    const float pillLeftPad = 14.0f;
    const float pillW = 46.0f;
    const float railGap = 14.0f;
    const float marginRight = 18.0f;
    const float marginTop = 18.0f;
    const float marginBottom = 20.0f;
    const float detailW = 284.0f;
    const float detailGap = 12.0f;

    ImVec2 winOrigin = ImGui::GetWindowPos();
    ImVec2 winSize = ImGui::GetWindowSize();
    const float pillX = winOrigin.x + pillLeftPad;
    const float contentLeftX = pillX + pillW + railGap;
    float usableW = (winOrigin.x + winSize.x) - contentLeftX - marginRight;
    if (usableW < 200.0f) usableW = 200.0f;

    ImVec2 gridRowTop = ImVec2(contentLeftX, winOrigin.y + marginTop);

    EnsureHardwareInfoLoaded();

    if (g_ui.hardwareInfo.displays.size() > 1) {
        ImGui::SetCursorScreenPos(gridRowTop);
        BeginModuleCard("MultiMonitorBanner", ImVec2(usableW, 54.0f), 14.0f, ST_CARD_NORMAL, ImGuiWindowFlags_NoScrollbar, 16.0f, 12.0f);

        if (g_ui.fontSemibold) ImGui::PushFont(g_ui.fontSemibold);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.945f, 0.961f, 0.976f, 1.0f), ICON_FA_TV " Target Desktops:");
        if (g_ui.fontSemibold) ImGui::PopFont();
        ImGui::SameLine(0, 16.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);

        bool all_checked = cfg.target_displays.empty() || (cfg.target_displays.size() == g_ui.hardwareInfo.displays.size());
        if (ToggleSwitch("All Monitors", &all_checked)) {
            if (all_checked) cfg.target_displays.clear();
            else cfg.target_displays = { 0 };
            EngineController::SetTargetDisplays(cfg.target_displays);
        }

        for (size_t d = 0; d < g_ui.hardwareInfo.displays.size(); ++d) {
            ImGui::SameLine(0, 16.0f);
            bool is_d_checked = cfg.IsDisplayEnabled(static_cast<int>(d));
            std::string disp_label = "Display " + std::to_string(d + 1) + (g_ui.hardwareInfo.displays[d].is_primary ? " (Primary)" : "");
            if (ToggleSwitch(disp_label.c_str(), &is_d_checked)) {
                cfg.SetDisplayEnabled(static_cast<int>(d), is_d_checked);
                EngineController::SetTargetDisplays(cfg.target_displays);
            }
        }

        EndModuleCard();
        gridRowTop.y += 54.0f + 12.0f;
    }

    bool isGalleryEmpty = galleryCopy.empty();

    if (!isGalleryEmpty && (g_ui.selectedGalleryIdx < 0 || g_ui.selectedGalleryIdx >= static_cast<int>(galleryCopy.size()))) {
        g_ui.selectedGalleryIdx = -1;
        for (size_t k = 0; k < galleryCopy.size(); ++k) {
            const std::string& pp = galleryCopy[k];
            if (!g_ui.daemon.currentVideo.empty() && PathsEqualNormalized(g_ui.daemon.currentVideo, pp)) {
                g_ui.selectedGalleryIdx = static_cast<int>(k);
                break;
            }
        }
        if (g_ui.selectedGalleryIdx < 0 && !cfg.wallpapers.empty()) {
            for (size_t k = 0; k < galleryCopy.size(); ++k) {
                if (PathsEqualNormalized(cfg.wallpapers[0].video_path, galleryCopy[k])) {
                    g_ui.selectedGalleryIdx = static_cast<int>(k);
                    break;
                }
            }
        }
    }
    bool showDetail = !isGalleryEmpty && g_ui.detailVisible && g_ui.selectedGalleryIdx >= 0 && g_ui.selectedGalleryIdx < static_cast<int>(galleryCopy.size());

    float gridH = (winOrigin.y + winSize.y) - gridRowTop.y - marginBottom;
    if (gridH < 160.0f) gridH = 160.0f;

    float gridAvailW = usableW - (showDetail ? (detailW + detailGap) : 0.0f);
    if (gridAvailW < 180.0f) gridAvailW = 180.0f;

    RenderSidePillRail(pillX, pillW, gridRowTop.y, gridH);

    if (isGalleryEmpty) {
        ImGui::SetCursorScreenPos(gridRowTop);
        BeginModuleCard("GalleryEmpty", ImVec2(gridAvailW, gridH), 18.0f);
        ImVec2 ec = ImGui::GetCursorScreenPos();
        ImVec2 esz = ImGui::GetContentRegionAvail();
        const char* et = "Belum ada video - klik + atau drag & drop ke sini";
        ImVec2 ets = ImGui::CalcTextSize(et);
        ImGui::SetCursorScreenPos(ImVec2(ec.x + (esz.x - ets.x) * 0.5f, ec.y + (esz.y - ets.y) * 0.5f));
        ImGui::TextColored(ImVec4(0.580f, 0.639f, 0.722f, 1.0f), "%s", et);
        EndModuleCard();
        return;
    }

    ImGui::SetCursorScreenPos(gridRowTop);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("GalleryGrid", ImVec2(gridAvailW, gridH), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus);

    float availW = ImGui::GetContentRegionAvail().x;
    const float spacingX = 12.0f;
    const float spacingY = 14.0f;

    // Reference card width when detail sidebar is closed — prevents cards from ever inflating when sidebar opens
    const float baseCardMinW = 216.0f;
    int baseCols = std::clamp(static_cast<int>((usableW + spacingX) / (baseCardMinW + spacingX)), 1, 4);
    float baseCardW = (usableW - spacingX * static_cast<float>(baseCols - 1)) / static_cast<float>(baseCols);

    const float cardMinW = 198.0f;
    int numCols = std::clamp(static_cast<int>((availW + spacingX) / (cardMinW + spacingX)), 1, 4);
    float rawCardW = (availW - spacingX * static_cast<float>(numCols - 1)) / static_cast<float>(numCols);
    float cardWidth = showDetail ? (std::min)(rawCardW, baseCardW) : baseCardW;
    if (cardWidth < 165.0f) cardWidth = availW;
    float thumbH = cardWidth * 9.0f / 16.0f;
    float footerH = 62.0f;
    float cardHeight = thumbH + footerH + 20.0f;

    for (size_t i = 0; i < galleryCopy.size(); ++i) {
        const auto& path = galleryCopy[i];

        // Scroll culling: off-screen cards only advance layout (0 probes, 0 SRV lookups)
        {
            const float cull_scroll_y = ImGui::GetScrollY();
            const float cull_view_h = ImGui::GetWindowHeight();
            const float cull_card_y = ImGui::GetCursorPosY();
            const float cull_pad = 80.0f;
            if (cull_card_y + cardHeight < cull_scroll_y - cull_pad ||
                cull_card_y > cull_scroll_y + cull_view_h + cull_pad) {
                ImGui::Dummy(ImVec2(cardWidth, cardHeight));
                bool cull_last_in_row = ((i + 1) % numCols == 0) || (i + 1 >= galleryCopy.size());
                if (!cull_last_in_row) ImGui::SameLine(0, spacingX);
                else ImGui::Dummy(ImVec2(1.0f, spacingY));
                continue;
            }
        }

        const auto& meta = GetGalleryCardMetadata(path, screen_w, screen_h, cfg.optimizer_crop_mode);

        bool is_playing_opt = (!g_ui.daemon.currentVideo.empty() && PathsEqualNormalized(g_ui.daemon.currentVideo, meta.opt_path));
        bool is_playing_ori = (!g_ui.daemon.currentVideo.empty() && PathsEqualNormalized(g_ui.daemon.currentVideo, path));
        bool is_current = is_playing_opt || is_playing_ori ||
                          (!cfg.wallpapers.empty() &&
                           (PathsEqualNormalized(cfg.wallpapers[0].video_path, path) ||
                            PathsEqualNormalized(cfg.wallpapers[0].video_path, meta.opt_path)));

        ImGui::PushID(static_cast<int>(i));
        bool is_selected = (g_ui.selectedGalleryIdx == static_cast<int>(i));
        int cardState = is_current ? ST_CARD_ACTIVE : ((is_selected && showDetail) ? ST_CARD_SELECTED : ST_CARD_NORMAL);

        BeginModuleCard("VCard", ImVec2(cardWidth, cardHeight), 14.0f, cardState, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, 0.0f, 0.0f);
        {
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 cardPos = ImGui::GetCursorScreenPos();
            float innerW = cardWidth;

            ImVec2 t0 = ImVec2(cardPos.x + 6.0f, cardPos.y + 6.0f);
            ImVec2 t1 = ImVec2(cardPos.x + innerW - 10.0f, cardPos.y + 6.0f + thumbH);
            ID3D11ShaderResourceView* thumb_srv = ThumbnailManager::Instance().GetThumbnailSRV(g_ui.device, path);
            if (thumb_srv) {
                draw_list->AddImageRounded((ImTextureID)thumb_srv, t0, t1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 10.0f);
            } else {
                draw_list->AddRectFilled(t0, t1, IM_COL32(17, 23, 38, 255), 10.0f);
                draw_list->AddRect(t0, t1, IM_COL32(56, 232, 255, 40), 10.0f);
                const char* lt = "Loading...";
                ImVec2 lts = ImGui::CalcTextSize(lt);
                draw_list->AddText(ImVec2(t0.x + (innerW - 16.0f - lts.x) * 0.5f, t0.y + (thumbH - lts.y) * 0.5f), IM_COL32(140, 152, 168, 255), lt);
            }
            draw_list->AddRect(t0, t1, IM_COL32(56, 232, 255, 40), 10.0f);
            if (is_current) draw_list->AddRect(t0, t1, IM_COL32(255, 209, 102, 210), 10.0f, 0, 1.6f);

            bool thumbHovered = ImGui::IsMouseHoveringRect(t0, t1);
            if (is_current || thumbHovered) {
                ImVec2 cc = ImVec2((t0.x + t1.x) * 0.5f, (t0.y + t1.y) * 0.5f);
                draw_list->AddCircleFilled(cc, 20.0f, IM_COL32(9, 12, 21, 178));
                draw_list->AddCircle(cc, 20.0f, IM_COL32(56, 232, 255, 90), 0, 1.2f);
                const char* pic = is_current ? ICON_FA_PAUSE : ICON_FA_PLAY;
                ImVec2 psz = ImGui::CalcTextSize(pic);
                draw_list->AddText(ImVec2(cc.x - psz.x * 0.5f, cc.y - psz.y * 0.5f), is_current ? IM_COL32(255, 209, 102, 255) : IM_COL32(241, 245, 249, 255), pic);
            }
            ImGui::Dummy(ImVec2(innerW, thumbH + 12.0f));

            ImGui::SetCursorPosX(12.0f);
            ImGui::BeginGroup();
            float footW = innerW - 24.0f;
            std::string verBadge;
            ImU32 verBg, verFg;
            if (is_playing_opt || (is_current && meta.has_opt)) { verBadge = "OPTIMIZED"; verBg = IM_COL32(56, 232, 255, 30); verFg = IM_COL32(56, 232, 255, 255); }
            else if (meta.has_opt) { verBadge = "ORI + OPT"; verBg = IM_COL32(255, 255, 255, 20); verFg = IM_COL32(241, 245, 249, 220); }
            else if (meta.is_already_optimal) { verBadge = "ORI - Optimal"; verBg = IM_COL32(255, 209, 102, 28); verFg = IM_COL32(255, 209, 102, 235); }
            else { verBadge = "ORIGINAL"; verBg = IM_COL32(255, 255, 255, 14); verFg = IM_COL32(148, 163, 184, 255); }
            if (is_current) { verBadge = std::string(ICON_FA_CIRCLE_PLAY "  ") + verBadge; }

            std::string titleStr = meta.filename;
            ImVec2 verSz = ImGui::CalcTextSize(verBadge.c_str());
            float verW = verSz.x + 16.0f;
            float maxTitleW = footW - verW - 10.0f;
            if (maxTitleW < 60.0f) maxTitleW = 60.0f;
            while (titleStr.length() > 6 && ImGui::CalcTextSize((titleStr + "...").c_str()).x > maxTitleW) {
                titleStr.pop_back();
            }
            if (titleStr != meta.filename) titleStr += "...";

            ImVec2 fpos = ImGui::GetCursorScreenPos();
            ImGui::TextUnformatted(titleStr.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nPath: %s", meta.filename.c_str(), path.c_str());
            ImVec2 bp0 = ImVec2(fpos.x + footW - verW, fpos.y);
            ImVec2 bp1 = ImVec2(bp0.x + verW, bp0.y + verSz.y + 6.0f);
            draw_list->AddRectFilled(bp0, bp1, verBg, 6.0f);
            draw_list->AddText(ImVec2(bp0.x + 8.0f, bp0.y + 3.0f), verFg, verBadge.c_str());

            if (!meta.meta_line.empty()) {
                const std::string& sub = (ImGui::CalcTextSize(meta.meta_line.c_str()).x <= footW)
                    ? meta.meta_line
                    : meta.meta_line_compact;
                ImGui::TextColored(ImVec4(0.580f, 0.639f, 0.722f, 1.0f), "%s", sub.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.520f, 0.575f, 0.660f, 1.0f), "Video source ready");
            }
            ImGui::EndGroup();
        }
        EndModuleCard();

        bool cardHovered = ImGui::IsItemHovered();
        if (cardHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (g_ui.selectedGalleryIdx == static_cast<int>(i) && g_ui.detailVisible) g_ui.detailVisible = false;
            else { g_ui.selectedGalleryIdx = static_cast<int>(i); g_ui.detailVisible = true; }
        }
        if (cardHovered) ImGui::SetTooltip("Klik untuk %s detail", (g_ui.detailVisible && is_selected) ? "tutup" : "buka");
        ImGui::PopID();

        bool isLastInRow = ((i + 1) % numCols == 0) || (i + 1 >= galleryCopy.size());
        if (!isLastInRow) {
            ImGui::SameLine(0, spacingX);
        } else {
            ImGui::Dummy(ImVec2(1.0f, spacingY));
        }
    }

    ImGui::Dummy(ImVec2(1.0f, 54.0f));

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    if (showDetail) {
        ImGui::SetCursorScreenPos(ImVec2(gridRowTop.x + gridAvailW + detailGap, gridRowTop.y));
        const std::string& selPath = galleryCopy[static_cast<size_t>(g_ui.selectedGalleryIdx)];
        const auto& selMeta = GetGalleryCardMetadata(selPath, screen_w, screen_h, cfg.optimizer_crop_mode);
        bool selPlayOpt = (!g_ui.daemon.currentVideo.empty() && PathsEqualNormalized(g_ui.daemon.currentVideo, selMeta.opt_path));
        bool selPlayOri = (!g_ui.daemon.currentVideo.empty() && PathsEqualNormalized(g_ui.daemon.currentVideo, selPath));
        bool selCur = selPlayOpt || selPlayOri ||
                      (!cfg.wallpapers.empty() &&
                       (PathsEqualNormalized(cfg.wallpapers[0].video_path, selPath) ||
                        PathsEqualNormalized(cfg.wallpapers[0].video_path, selMeta.opt_path)));
        RenderGalleryDetailPanel(selPath, selMeta, selPlayOpt, selCur, detailW, gridH);
    }
}

void RenderLockscreenTab() {
    auto& cfg = g_config.Get();

    RenderPageTitle(ICON_FA_LOCK, "Lock Screen & Static", "Configure instant 0s Windows boot visuals and lock screen transitions with zero continuous CPU load.");

    float availW = ImGui::GetContentRegionAvail().x;
    ImVec2 pageStart = ImGui::GetCursorScreenPos();
    bool isNarrow = (availW < 680.0f);
    float colGap = 16.0f;
    float colW = isNarrow ? availW : (availW - colGap) * 0.5f;
    const float cardPadX = 22.0f;
    const float cardPadY = 20.0f;
    float contentW = colW - cardPadX * 2.0f;

    float leftX = pageStart.x;
    float rightX = isNarrow ? pageStart.x : (pageStart.x + colW + colGap);
    float colY = pageStart.y;
    float colY2 = isNarrow ? 0.0f : pageStart.y;

    // ================= KOLOM KIRI =================

    ImGui::SetCursorScreenPos(ImVec2(leftX, colY));
    BeginGlassCard("DesktopStaticCard", ImVec2(colW, 0.0f), cardPadX, cardPadY);
    GlassCardHeader(ICON_FA_DESKTOP "  DESKTOP STATIC WALLPAPER");

    if (ToggleSwitch("Enable Desktop Static Wallpaper", &cfg.update_desktop_wallpaper)) {
        g_config.Save();
        if (cfg.update_desktop_wallpaper) {
            if (cfg.desktop_static_source_mode == 1 && !cfg.desktop_static_video_path.empty()) {
                EngineController::SyncVisualWallpaper(cfg.desktop_static_video_path, true, false, cfg.desktop_static_timestamp);
            } else {
                EngineController::SyncVisualWallpaper("", true, false, 1.0);
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Injects a high-definition static frame directly into Windows Desktop Wallpaper.\nWhen Windows boots, your desktop appears in 0.0s with no black screen or loading delay.");
    }

    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    if (cfg.update_desktop_wallpaper) {
        if (cfg.desktop_static_source_mode == 0) {
            ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.0f), ICON_FA_ROTATE " Active: Auto-Sync (Live Video)");
        } else {
            std::string vidName = PathToUtf8(PathFromUtf8(cfg.desktop_static_video_path).filename());
            if (vidName.empty()) vidName = cfg.desktop_static_video_path;
            int curMin = static_cast<int>(cfg.desktop_static_timestamp) / 60;
            float curSec = static_cast<float>(cfg.desktop_static_timestamp) - curMin * 60.0f;
            ImGui::TextColored(ImVec4(1.00f, 0.82f, 0.40f, 1.0f), ICON_FA_IMAGE " Custom: %s [%02d:%05.2f]", StEllipsize(vidName, 22).c_str(), curMin, curSec);

            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            if (HoloButtonSmall("Reset to Auto-Sync")) {
                EngineController::ResetDesktopWallpaperAuto();
            }
        }

        std::string cur_vid = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : g_ui.daemon.currentVideo;
        if (cfg.desktop_static_source_mode == 1 && !cfg.desktop_static_video_path.empty()) {
            cur_vid = cfg.desktop_static_video_path;
        }
        if (cur_vid.empty() && !cfg.gallery_history.empty()) {
            cur_vid = cfg.gallery_history[0];
        }

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        if (HoloButton(ICON_FA_CAMERA "  Choose Custom Frame...", ImVec2(contentW, 0))) {
            if (!cur_vid.empty()) {
                OpenCaptureModal(cur_vid, 1);
            }
        }
    } else {
        ImGui::TextDisabled("Desktop static wallpaper is disabled.");
    }
    colY += EndGlassCard() + colGap;

    // KARTU 2: Windows Screensaver Integration
    ImGui::SetCursorScreenPos(ImVec2(leftX, colY));
    BeginGlassCard("ScreensaverCard", ImVec2(colW, 0.0f), cardPadX, cardPadY);
    GlassCardHeader(ICON_FA_TV "  SCREENSAVER INTEGRATION");

    static bool scr_checked = false;
    static bool scr_installed = false;
    if (!scr_checked) {
        scr_installed = LockScreenManager::IsScreensaverInstalled();
        scr_checked = true;
    }

    if (ToggleSwitch("Enable as Windows Screensaver", &scr_installed)) {
        if (scr_installed) {
            wchar_t exePathBuf[1024] = {};
            GetModuleFileNameW(nullptr, exePathBuf, static_cast<DWORD>(IM_ARRAYSIZE(exePathBuf)));
            std::wstring exePath(exePathBuf);
            size_t dotPos = exePath.find_last_of(L'.');
            std::wstring scrPath = (dotPos != std::wstring::npos ? exePath.substr(0, dotPos) : exePath) + L".scr";
            std::error_code ec;
            if (!fs::exists(scrPath, ec)) {
                CopyFileW(exePath.c_str(), scrPath.c_str(), FALSE);
            }
            LockScreenManager::InstallScreensaver(scrPath, 300, true);
        } else {
            LockScreenManager::UninstallScreensaver();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Plays your live video wallpaper at full 60 FPS when your computer is idle!\nWhen waking the PC, it transitions seamlessly back.");
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    if (HoloButton(ICON_FA_GEAR "  Configure in Windows...", ImVec2(contentW, 0))) {
        LockScreenManager::OpenWindowsScreensaverSettings();
    }

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 3.0f));
    ImGui::TextColored(ImVec4(0.580f, 0.639f, 0.722f, 1.0f), "Plays live wallpaper when idle, waking up immediately on mouse move.");
    ImGui::PopStyleVar();

    colY += EndGlassCard() + colGap;

    // ================= KOLOM KANAN =================

    if (isNarrow) colY2 = colY;
    ImGui::SetCursorScreenPos(ImVec2(rightX, colY2));
    BeginGlassCard("LockscreenCard", ImVec2(colW, 0.0f), cardPadX, cardPadY);
    GlassCardHeader(ICON_FA_LOCK "  LOCK SCREEN WALLPAPER");

    if (ToggleSwitch("Enable Windows Lock Screen Wallpaper", &cfg.update_lockscreen)) {
        g_config.Save();
        if (cfg.update_lockscreen) {
            if (cfg.lockscreen_source_mode == 1 && !cfg.lockscreen_video_path.empty()) {
                EngineController::SyncVisualWallpaper(cfg.lockscreen_video_path, false, true, cfg.lockscreen_timestamp);
            } else {
                EngineController::SyncVisualWallpaper("", false, true, 1.0);
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pre-caches a pristine high-definition frame for the Windows Lock Screen.\nWhen pressing Win+L, it displays immediately with 0ms visual delay and 0% background load.");
    }

    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    if (cfg.update_lockscreen) {
        if (cfg.lockscreen_source_mode == 0) {
            ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.0f), ICON_FA_ROTATE " Active: Auto-Sync (Live Video)");
        } else {
            std::string vidName = PathToUtf8(PathFromUtf8(cfg.lockscreen_video_path).filename());
            if (vidName.empty()) vidName = cfg.lockscreen_video_path;
            int curMin = static_cast<int>(cfg.lockscreen_timestamp) / 60;
            float curSec = static_cast<float>(cfg.lockscreen_timestamp) - curMin * 60.0f;
            ImGui::TextColored(ImVec4(1.00f, 0.82f, 0.40f, 1.0f), ICON_FA_IMAGE " Custom: %s [%02d:%05.2f]", StEllipsize(vidName, 22).c_str(), curMin, curSec);

            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            if (HoloButtonSmall("Reset to Auto-Sync")) {
                EngineController::ResetLockscreenAuto();
            }
        }

        std::string cur_vid = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].video_path : g_ui.daemon.currentVideo;
        if (cfg.lockscreen_source_mode == 1 && !cfg.lockscreen_video_path.empty()) {
            cur_vid = cfg.lockscreen_video_path;
        }
        if (cur_vid.empty() && !cfg.gallery_history.empty()) {
            cur_vid = cfg.gallery_history[0];
        }

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        if (HoloButton(ICON_FA_CAMERA "  Choose Custom Frame...", ImVec2(contentW, 0))) {
            if (!cur_vid.empty()) {
                OpenCaptureModal(cur_vid, 2);
            }
        }
    } else {
        ImGui::TextDisabled("Lock Screen wallpaper integration is disabled.");
    }
    colY2 += EndGlassCard() + colGap;

    // KARTU 4: Status & Transition Guide
    ImGui::SetCursorScreenPos(ImVec2(rightX, colY2));
    BeginGlassCard("LockscreenInfoCard", ImVec2(colW, 0.0f), cardPadX, cardPadY);
    GlassCardHeader(ICON_FA_CIRCLE_INFO "  SEAMLESS TRANSITIONS STATUS");

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));
    ImGui::TextColored(cfg.update_desktop_wallpaper ? ImVec4(0.22f, 0.91f, 1.00f, 1.0f) : ImVec4(0.58f, 0.64f, 0.72f, 1.0f),
        "• Desktop Static: %s", cfg.update_desktop_wallpaper ? "ACTIVE (0s Instant Boot)" : "Disabled");
    ImGui::TextColored(cfg.update_lockscreen ? ImVec4(0.22f, 0.91f, 1.00f, 1.0f) : ImVec4(0.58f, 0.64f, 0.72f, 1.0f),
        "• Lock Screen: %s", cfg.update_lockscreen ? "ACTIVE (Win + L Seamless)" : "Disabled");
    ImGui::TextColored(scr_installed ? ImVec4(0.22f, 0.91f, 1.00f, 1.0f) : ImVec4(0.58f, 0.64f, 0.72f, 1.0f),
        "• Screensaver: %s", scr_installed ? "ACTIVE (60 FPS Idle)" : "Disabled");
    ImGui::PopStyleVar();

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    if (HoloButton(ICON_FA_ROTATE "  Refresh Static Frames", ImVec2(contentW, 0))) {
        if (cfg.update_desktop_wallpaper) {
            std::string d_vid = (cfg.desktop_static_source_mode == 1 && !cfg.desktop_static_video_path.empty())
                                    ? cfg.desktop_static_video_path
                                    : "";
            double d_ts = (cfg.desktop_static_source_mode == 1) ? cfg.desktop_static_timestamp : 1.0;
            EngineController::SyncVisualWallpaper(d_vid, true, false, d_ts);
        }
        if (cfg.update_lockscreen) {
            std::string l_vid = (cfg.lockscreen_source_mode == 1 && !cfg.lockscreen_video_path.empty())
                                    ? cfg.lockscreen_video_path
                                    : "";
            double l_ts = (cfg.lockscreen_source_mode == 1) ? cfg.lockscreen_timestamp : 1.0;
            EngineController::SyncVisualWallpaper(l_vid, false, true, l_ts);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Immediately re-extracts and refreshes desktop and lock screen images from active video.");
    }

    colY2 += EndGlassCard() + colGap;

    float finalH = (std::max)(colY, colY2) + 40.0f;
    ImGui::SetCursorScreenPos(ImVec2(leftX, finalH));
    ImGui::Dummy(ImVec2(1.0f, 1.0f));
}

void RenderGeneralSettingsTab() {
    auto& cfg = g_config.Get();

    RenderPageTitle(ICON_FA_SLIDERS, "General Settings", "Configure display scaling, master volume, Windows autostart, and power-saving rules.");

    float availW = ImGui::GetContentRegionAvail().x;
    ImVec2 pageStart = ImGui::GetCursorScreenPos();
    bool isNarrow = (availW < 680.0f);
    float colGap = 16.0f;
    float colW = isNarrow ? availW : (availW - colGap) * 0.5f;
    const float cardPadX = 22.0f;
    const float cardPadY = 18.0f;
    float contentW = colW - cardPadX * 2.0f;

    float leftX = pageStart.x;
    float rightX = isNarrow ? pageStart.x : (pageStart.x + colW + colGap);

    static float s_genRow1H = 215.0f;
    static float s_genRow2H = 250.0f;
    float row1TargetH = isNarrow ? 0.0f : s_genRow1H;
    float row2TargetH = isNarrow ? 0.0f : s_genRow2H;

    // ROW 1: DISPLAY & SCALING + POWER & OCCLUSION RULES
    float row1Top = pageStart.y;
    ImGui::SetCursorScreenPos(ImVec2(leftX, row1Top));
    BeginGlassCard("DisplayCard", ImVec2(colW, row1TargetH), cardPadX, cardPadY);
    GlassCardHeader(ICON_FA_EXPAND "  DISPLAY & SCALING");

    static const char* scalingModes[] = {
        "Auto Aspect Fill (Cover - Smart Crop)",
        "Aspect Fit (Letterbox - Black Bars)",
        "Stretch to Screen (Ignore Ratio)"
    };
    int currentMode = cfg.scaling_mode;
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ST_TEXT_DIM), "Video Scaling & Placement");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::SetNextItemWidth(contentW);

    PushStylishComboStyle();
    if (ImGui::Combo("##scalingMode", &currentMode, scalingModes, IM_ARRAYSIZE(scalingModes))) {
        EngineController::SetScalingMode(currentMode);
    }
    PopStylishComboStyle();

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Changes how video fits the screen when aspect ratio differs from display.");
    }

    EnsureHardwareInfoLoaded();

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    if (g_ui.hardwareInfo.displays.size() > 1) {
        TextDim("Target Desktop Display(s):");
        ImGui::Dummy(ImVec2(0.0f, 2.0f));

        bool all_checked = cfg.target_displays.empty() || (cfg.target_displays.size() == g_ui.hardwareInfo.displays.size());
        if (ToggleSwitch("All Monitors", &all_checked)) {
            if (all_checked) cfg.target_displays.clear();
            else cfg.target_displays = { 0 };
            EngineController::SetTargetDisplays(cfg.target_displays);
        }

        for (size_t d = 0; d < g_ui.hardwareInfo.displays.size(); ++d) {
            const auto& disp = g_ui.hardwareInfo.displays[d];
            bool is_d_checked = cfg.IsDisplayEnabled(static_cast<int>(d));
            std::string label = "Display " + std::to_string(d + 1) + (disp.is_primary ? " (Primary)" : " (Secondary)");
            if (ToggleSwitch(label.c_str(), &is_d_checked)) {
                cfg.SetDisplayEnabled(static_cast<int>(d), is_d_checked);
                EngineController::SetTargetDisplays(cfg.target_displays);
            }
        }
    } else {
        if (!g_ui.hardwareInfo.displays.empty()) {
            const auto& d = g_ui.hardwareInfo.displays[0];
            ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 0.95f), ICON_FA_DESKTOP "  Active Display: %dx%d (%d Hz)", d.width, d.height, d.refresh_rate > 0 ? d.refresh_rate : 60);
        }
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        TextDim("Wallpaper automatically fills your primary desktop screen.");
    }
    float contentH1 = ImGui::GetCursorPosY() + cardPadY;
    float h1 = EndGlassCard();

    float row1RightY = isNarrow ? (row1Top + h1 + colGap) : row1Top;
    ImGui::SetCursorScreenPos(ImVec2(rightX, row1RightY));
    BeginGlassCard("PowerCard", ImVec2(colW, row1TargetH), cardPadX, cardPadY);
    GlassCardHeader(ICON_FA_BATTERY_HALF "  POWER & OCCLUSION RULES");

    if (ToggleSwitch("Auto-Pause when Fullscreen App/Game is active", &cfg.pause_on_fullscreen)) {
        g_config.Save();
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    if (ToggleSwitch("Auto-Pause when Window is Maximized", &cfg.pause_on_maximized)) {
        g_config.Save();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Pauses wallpaper rendering to release GPU/CPU when desktop is covered.");
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    if (ToggleSwitch("Auto-Pause on Battery Power (Laptops)", &cfg.pause_on_battery)) {
        g_config.Save();
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    TextDim("%s", "Releases 100% GPU/CPU processing when wallpaper is covered.");
    float contentH2 = ImGui::GetCursorPosY() + cardPadY;
    float h2 = EndGlassCard();

    if (!isNarrow) {
        s_genRow1H = (std::max)(contentH1, contentH2);
    }

    // ROW 2: AUDIO & SEAMLESS LOOPING + STARTUP & SYSTEM TRAY
    float row2Top = isNarrow ? (row1RightY + h2 + colGap) : (row1Top + (std::max)(h1, h2) + colGap);

    ImGui::SetCursorScreenPos(ImVec2(leftX, row2Top));
    BeginGlassCard("AudioLoopCard", ImVec2(colW, row2TargetH), cardPadX, cardPadY);
    GlassCardHeader(ICON_FA_VOLUME_HIGH "  AUDIO & SEAMLESS LOOPING");

    float volume = (!cfg.wallpapers.empty()) ? cfg.wallpapers[0].volume : 0.0f;
    if (StarlumeSliderFloat("Master Volume", &volume, 0.0f, 1.0f, 2, "", contentW)) {
        EngineController::SetVolume(volume, false);
    }
    if (WasStarlumeSliderDeactivated()) {
        g_config.FlushIfDirty(true);
    }
    TextDim("Controls background video audio volume level.");

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    if (ToggleSwitch("Enable Auto Smooth Looping (Seamless Crossfade)", &cfg.auto_smooth_loop)) {
        g_config.Save();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Applies a smooth crossfade transition and time-warp deceleration at loop seams for a seamless continuous loop without hard cuts.");
    }

    if (cfg.auto_smooth_loop) {
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        const char* loop_presets[] = {
            "Cinematic Speed Ramp (1.2s)",
            "Smoothstep S-Curve (0.8s)",
            "Gentle Flow (1.8s Ambient)",
            "Instant Snap (0.4s Fast)",
            "Custom Tuning..."
        };

        ImGui::SetNextItemWidth(contentW);
        PushStylishComboStyle();
        if (ImGui::Combo("##loop_preset", &cfg.loop_preset, loop_presets, IM_ARRAYSIZE(loop_presets))) {
            g_config.Save();
        }
        PopStylishComboStyle();

        if (cfg.loop_preset == 4) {
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            if (StarlumeSliderFloat("Transition Duration", &cfg.smooth_loop_duration, 0.2f, 2.5f, 1, "s", contentW)) {
                g_config.MarkDirty();
            }
            if (WasStarlumeSliderDeactivated()) {
                g_config.FlushIfDirty(true);
            }

            const char* easing_curves[] = {
                "Linear (Constant)",
                "Smoothstep (S-Curve Hermite)",
                "Sine Wave (Smooth Harmonic)",
                "Smootherstep (Perlin Ultra-Smooth)"
            };
            ImGui::SetNextItemWidth(contentW);
            if (ImGui::Combo("##loop_easing", &cfg.loop_easing_curve, easing_curves, IM_ARRAYSIZE(easing_curves))) {
                g_config.Save();
            }

            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            if (ToggleSwitch("Enable Speed Ramping", &cfg.loop_speed_ramp)) {
                g_config.Save();
            }

            if (cfg.loop_speed_ramp) {
                ImGui::Dummy(ImVec2(0.0f, 4.0f));
                if (StarlumeSliderFloat("Min Speed at Seam", &cfg.loop_min_speed, 0.50f, 0.95f, 2, "x", contentW)) {
                    g_config.MarkDirty();
                }
                if (WasStarlumeSliderDeactivated()) {
                    g_config.FlushIfDirty(true);
                }
            }
        }
    } else {
        ImGui::Dummy(ImVec2(0.0f, 2.0f));
        TextDim("Seamless crossfade eliminates hard cuts at video loop seams.");
    }
    float contentH3 = ImGui::GetCursorPosY() + cardPadY;
    float h3 = EndGlassCard();

    float row2RightY = isNarrow ? (row2Top + h3 + colGap) : row2Top;
    ImGui::SetCursorScreenPos(ImVec2(rightX, row2RightY));
    BeginGlassCard("StartupTrayCard", ImVec2(colW, row2TargetH), cardPadX, cardPadY);
    GlassCardHeader(ICON_FA_DESKTOP "  STARTUP & SYSTEM TRAY");

    static bool startup_inited = false;
    static bool startup_enabled = false;
    if (!startup_inited) {
        startup_enabled = WindowsAutostart::IsEnabled();
        startup_inited = true;
    }

    if (ToggleSwitch("Start MiniWallpaper on Windows Boot", &startup_enabled)) {
        WindowsAutostart::SetEnabled(startup_enabled, 0);
        cfg.run_on_startup = startup_enabled;
        g_config.Save();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Launches MiniWallpaper silently on Windows boot to instantly resume wallpaper.");
    }
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    TextDim("Runs silently in the system tray when Windows boots.");

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f), ICON_FA_CIRCLE_CHECK "  Auto-Saved in Real-Time");
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    TextDim("All preference changes take effect immediately without restart.");

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    if (HoloButton(ICON_FA_EYE_SLASH "  Hide Window to Tray", ImVec2(contentW, 0))) {
        SettingsUI::Close();
    }
    float contentH4 = ImGui::GetCursorPosY() + cardPadY;
    float h4 = EndGlassCard();

    if (!isNarrow) {
        s_genRow2H = (std::max)(contentH3, contentH4);
    }

    float finalH = (isNarrow ? (row2RightY + h4) : (row2Top + (std::max)(h3, h4))) + 40.0f;
    ImGui::SetCursorScreenPos(ImVec2(leftX, finalH));
    ImGui::Dummy(ImVec2(1.0f, 1.0f));
}

void RenderPerformancePanel() {
    auto& cfg = g_config.Get();

    EnsureHardwareInfoLoaded();

    RenderPageTitle(ICON_FA_GAUGE_HIGH, "Performance & Advanced", "GPU hardware acceleration engine, frame pacing, gamer auto-sleep governor, and live telemetry.");

    float availW = ImGui::GetContentRegionAvail().x;
    ImVec2 pageStart = ImGui::GetCursorScreenPos();
    bool isNarrow = (availW < 680.0f);
    float colGap = 16.0f;
    float colW = isNarrow ? availW : (availW - colGap) * 0.5f;
    const float cardPadX = 22.0f;
    const float cardPadY = 20.0f;
    float contentW = colW - cardPadX * 2.0f;

    static float telemCpu[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    static float telemRam[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    static float telemVram[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    static double telemLastT = 0.0;
    static bool telemInit = false;

    float curCpu = g_ui.daemon.connected ? static_cast<float>(g_ui.daemon.cpuPercent) : 0.0f;
    size_t curAppRam = g_ui.daemon.ramMB;
    size_t curAppPriv = g_ui.daemon.ramPrivateMB;
    size_t curAppVram = g_ui.daemon.vramMB;
    int sys_ram_pct = g_shared_engine_state.system_ram_percent.load();
    bool is_sleeping = g_shared_engine_state.resource_heavy_sleep.load();

    if (curAppRam == 0) {
        PROCESS_MEMORY_COUNTERS_EX pmc = {};
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            curAppRam = pmc.WorkingSetSize / (1024 * 1024);
            curAppPriv = pmc.PrivateUsage / (1024 * 1024);
        }
    }

    if (sys_ram_pct == 0) {
        MEMORYSTATUSEX memInfo = {};
        memInfo.dwLength = sizeof(MEMORYSTATUSEX);
        if (GlobalMemoryStatusEx(&memInfo)) {
            sys_ram_pct = static_cast<int>(memInfo.dwMemoryLoad);
        }
    }

    if (curCpu == 0.0f) {
        static ULARGE_INTEGER lastCPU = {0}, lastSysCPU = {0}, lastUserCPU = {0};
        static int numProcessors = -1;
        static bool s_cpuFirst = true;
        static float s_lastUiCpu = 0.0f;
        if (s_cpuFirst) {
            SYSTEM_INFO sysInfo;
            FILETIME ftime, fsys, fuser;
            GetSystemInfo(&sysInfo);
            numProcessors = static_cast<int>(sysInfo.dwNumberOfProcessors);
            GetSystemTimeAsFileTime(&ftime);
            std::memcpy(&lastCPU, &ftime, sizeof(FILETIME));
            GetProcessTimes(GetCurrentProcess(), &ftime, &ftime, &fsys, &fuser);
            std::memcpy(&lastSysCPU, &fsys, sizeof(FILETIME));
            std::memcpy(&lastUserCPU, &fuser, sizeof(FILETIME));
            s_cpuFirst = false;
        } else {
            FILETIME ftime, fsys, fuser;
            ULARGE_INTEGER now, sys, user;
            GetSystemTimeAsFileTime(&ftime);
            std::memcpy(&now, &ftime, sizeof(FILETIME));
            GetProcessTimes(GetCurrentProcess(), &ftime, &ftime, &fsys, &fuser);
            std::memcpy(&sys, &fsys, sizeof(FILETIME));
            std::memcpy(&user, &fuser, sizeof(FILETIME));
            ULONGLONG deltaSys = sys.QuadPart - lastSysCPU.QuadPart;
            ULONGLONG deltaUser = user.QuadPart - lastUserCPU.QuadPart;
            ULONGLONG deltaTotal = now.QuadPart - lastCPU.QuadPart;
            if (deltaTotal > 0 && numProcessors > 0) {
                double pct = ((deltaSys + deltaUser) * 100.0) / (deltaTotal * numProcessors);
                s_lastUiCpu = static_cast<float>((pct > 100.0) ? 100.0 : pct);
            }
            lastCPU = now;
            lastSysCPU = sys;
            lastUserCPU = user;
        }
        if (s_lastUiCpu > 0.0f) curCpu = s_lastUiCpu;
    }

    int activeGpuIdx = (g_ui.daemon.activeGpuIndex != 0 || cfg.gpu_device_index == -1) ? g_ui.daemon.activeGpuIndex : cfg.gpu_device_index;
    bool isCpuDecode = (activeGpuIdx == -1 || cfg.gpu_device_index == -1);

    std::string activeGpuName = g_ui.daemon.gpuName;
    if (isCpuDecode) {
        activeGpuName = "CPU Software (swscale)";
        curAppVram = 0;
    } else if (activeGpuName.empty() || activeGpuName == "CPU (Software Decode)") {
        if (cfg.gpu_device_index >= 0 && cfg.gpu_device_index < static_cast<int>(g_ui.hardwareInfo.gpus.size())) {
            activeGpuName = g_ui.hardwareInfo.gpus[cfg.gpu_device_index].name;
        } else if (!g_ui.hardwareInfo.gpus.empty()) {
            activeGpuName = g_ui.hardwareInfo.gpus[0].name;
        } else {
            activeGpuName = "DirectX 11 GPU";
        }
    }

    // Query DXGI VRAM at most ~2 Hz when standalone UI needs fallback
    static size_t s_fallbackVramMB = 0;
    static int s_fallbackVramPct = 0;
    static uint64_t s_lastDxgiPollTick = 0;
    if ((curAppVram == 0 || g_shared_engine_state.gpu_vram_percent.load() == 0) && !isCpuDecode && g_ui.device) {
        uint64_t nowTick = GetTickCount64();
        if (nowTick - s_lastDxgiPollTick >= 500) {
            s_lastDxgiPollTick = nowTick;
            ComPtr<IDXGIDevice> dxgiDevice;
            if (SUCCEEDED(g_ui.device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
                ComPtr<IDXGIAdapter> adapter;
                if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
#if __has_include(<dxgi1_4.h>)
                    ComPtr<IDXGIAdapter3> adapter3;
                    if (SUCCEEDED(adapter.As(&adapter3))) {
                        DXGI_QUERY_VIDEO_MEMORY_INFO memInfo = {};
                        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo))) {
                            s_fallbackVramMB = static_cast<size_t>(memInfo.CurrentUsage / (1024 * 1024));
                            if (memInfo.Budget > 0) {
                                s_fallbackVramPct = static_cast<int>(static_cast<double>(memInfo.CurrentUsage) * 100.0 / static_cast<double>(memInfo.Budget));
                            }
                        }
                        if (s_fallbackVramMB == 0 && SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &memInfo))) {
                            s_fallbackVramMB = static_cast<size_t>(memInfo.CurrentUsage / (1024 * 1024));
                        }
                    }
#endif
                }
            }
        }
        if (curAppVram == 0) curAppVram = s_fallbackVramMB;
    }

    double nowT = ImGui::GetTime();
    if (!telemInit) {
        telemInit = true;
        telemLastT = nowT;
        for (int i = 0; i < 5; ++i) {
            telemCpu[i] = curCpu;
            telemRam[i] = static_cast<float>(curAppRam);
            telemVram[i] = static_cast<float>(curAppVram);
        }
    } else if (nowT - telemLastT >= 1.0) {
        telemLastT = nowT;
        for (int i = 0; i < 4; ++i) {
            telemCpu[i] = telemCpu[i + 1];
            telemRam[i] = telemRam[i + 1];
            telemVram[i] = telemVram[i + 1];
        }
        telemCpu[4] = curCpu;
        telemRam[4] = static_cast<float>(curAppRam);
        telemVram[4] = static_cast<float>(curAppVram);
    }

    const float tileH = 144.0f;
    float colTopY = pageStart.y;
    {
        char cpuVal[32];
        char cpuSub[96];
        const char* cpuUnit = "%";
        const char* cpuStatus;
        ImU32 cpuCol;
        if (!g_ui.daemon.connected && curCpu <= 0.0f) {
            snprintf(cpuVal, sizeof(cpuVal), "--");
            snprintf(cpuSub, sizeof(cpuSub), "Background engine connecting...");
            cpuUnit = "";
            cpuStatus = "STANDBY";
            cpuCol = ST_TEXT_MUTED;
        } else {
            snprintf(cpuVal, sizeof(cpuVal), "%.1f", static_cast<double>(curCpu));
            snprintf(cpuSub, sizeof(cpuSub), "MiniWallpaper process • Multi-thread decode");
            if (curCpu < 4.0f) { cpuStatus = "OPTIMAL"; cpuCol = ST_GREEN; }
            else if (curCpu < 12.0f) { cpuStatus = "ELEVATED"; cpuCol = ST_GOLD; }
            else { cpuStatus = "HIGH"; cpuCol = ST_RED; }
        }
        float cpuPct01 = (std::min)(1.0f, (std::max)(0.0f, curCpu / 15.0f));

        char ramVal[32];
        char ramSub[110];
        snprintf(ramVal, sizeof(ramVal), "%zu", curAppRam);
        if (curAppPriv > 0) {
            snprintf(ramSub, sizeof(ramSub), "Working Set: %zu MB (Priv: %zu MB) • Sys: %d%%", curAppRam, curAppPriv, sys_ram_pct);
        } else {
            snprintf(ramSub, sizeof(ramSub), "Working Set: %zu MB • Windows 11 Load: %d%%", curAppRam, sys_ram_pct);
        }
        const char* ramStatus;
        ImU32 ramCol;
        if (curAppRam < 60) { ramStatus = "OPTIMAL"; ramCol = ST_GREEN; }
        else if (curAppRam < 120) { ramStatus = "ELEVATED"; ramCol = ST_GOLD; }
        else { ramStatus = is_sleeping ? "AUTO-SLEEP" : "HIGH"; ramCol = ST_RED; }
        float ramPct01 = (std::min)(1.0f, (std::max)(0.0f, static_cast<float>(curAppRam) / 100.0f));

        char vramVal[32];
        char vramSub[128];
        const char* vramUnit = "MB";
        float vramPct = 0.0f;
        const char* vramStatus;
        ImU32 vramCol;

        if (isCpuDecode) {
            snprintf(vramVal, sizeof(vramVal), "0");
            snprintf(vramSub, sizeof(vramSub), "CPU swscale • 0 MB dedicated VRAM (RAM only)");
            vramPct = 0.0f;
            vramStatus = "CPU MODE";
            vramCol = ST_CYAN;
        } else {
            snprintf(vramVal, sizeof(vramVal), "%zu", curAppVram);
            if (activeGpuName.find("NVIDIA") != std::string::npos) {
                snprintf(vramSub, sizeof(vramSub), "NVIDIA RTX • %zu MB dedicated VRAM", curAppVram);
            } else if (activeGpuName.find("Intel") != std::string::npos) {
                snprintf(vramSub, sizeof(vramSub), "Intel iGPU • %zu MB video memory", curAppVram);
            } else if (activeGpuName.find("AMD") != std::string::npos || activeGpuName.find("Radeon") != std::string::npos) {
                snprintf(vramSub, sizeof(vramSub), "AMD Radeon • %zu MB video memory", curAppVram);
            } else {
                snprintf(vramSub, sizeof(vramSub), "%s • %zu MB", activeGpuName.c_str(), curAppVram);
            }
            vramPct = (std::min)(1.0f, (std::max)(0.0f, static_cast<float>(curAppVram) / 80.0f));
            if (curAppVram < 40) { vramStatus = "OPTIMAL"; vramCol = ST_GREEN; }
            else if (curAppVram < 80) { vramStatus = "ELEVATED"; vramCol = ST_GOLD; }
            else { vramStatus = is_sleeping ? "AUTO-SLEEP" : "HIGH"; vramCol = ST_RED; }
        }

        if (availW >= 640.0f) {
            const float tileGap = 14.0f;
            float tileW = (availW - tileGap * 2.0f) / 3.0f;
            TelemetryTile("TelemCPU", "APP CPU LOAD", cpuPct01, cpuVal, cpuUnit, cpuStatus, cpuCol, telemCpu, cpuSub, ImVec2(tileW, tileH));
            ImGui::SetCursorScreenPos(ImVec2(pageStart.x + tileW + tileGap, pageStart.y));
            TelemetryTile("TelemRAM", "APP RAM USAGE", ramPct01, ramVal, "MB", ramStatus, ramCol, telemRam, ramSub, ImVec2(tileW, tileH));
            ImGui::SetCursorScreenPos(ImVec2(pageStart.x + (tileW + tileGap) * 2.0f, pageStart.y));
            TelemetryTile("TelemVRAM", "APP VRAM USAGE", vramPct, vramVal, vramUnit, vramStatus, vramCol, telemVram, vramSub, ImVec2(tileW, tileH));
            colTopY = pageStart.y + tileH + 18.0f;
        } else {
            TelemetryTile("TelemCPU", "APP CPU LOAD", cpuPct01, cpuVal, cpuUnit, cpuStatus, cpuCol, telemCpu, cpuSub, ImVec2(availW, tileH));
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            TelemetryTile("TelemRAM", "APP RAM USAGE", ramPct01, ramVal, "MB", ramStatus, ramCol, telemRam, ramSub, ImVec2(availW, tileH));
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            TelemetryTile("TelemVRAM", "APP VRAM USAGE", vramPct, vramVal, vramUnit, vramStatus, vramCol, telemVram, vramSub, ImVec2(availW, tileH));
            colTopY = ImGui::GetCursorScreenPos().y + 14.0f;
        }
    }

    float leftX = pageStart.x;
    float rightX = isNarrow ? pageStart.x : (pageStart.x + colW + colGap);
    float colY = colTopY;

    // Kartu 1: Rendering Engine & Frame Rate
    ImGui::SetCursorScreenPos(ImVec2(leftX, colY));
    BeginGlassCard("PerfEngineCard", ImVec2(colW, 0.0f), cardPadX, cardPadY, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    GlassCardHeader(ICON_FA_MICROCHIP "  RENDERING ENGINE");

    std::vector<std::string> deviceNames;
    std::vector<int> deviceValues;
    deviceNames.reserve(g_ui.hardwareInfo.gpus.size() + 1);
    deviceValues.reserve(g_ui.hardwareInfo.gpus.size() + 1);

    for (size_t g = 0; g < g_ui.hardwareInfo.gpus.size(); ++g) {
        std::string label = "GPU " + std::to_string(g + 1) + ": " + g_ui.hardwareInfo.gpus[g].name + " (DirectX 11 HW)";
        deviceNames.push_back(label);
        deviceValues.push_back(static_cast<int>(g));
    }
    deviceNames.push_back("CPU: Software Processor Decode (swscale)");
    deviceValues.push_back(-1);

    int currentDeviceCombo = 0;
    for (size_t i = 0; i < deviceValues.size(); ++i) {
        if (deviceValues[i] == cfg.gpu_device_index) {
            currentDeviceCombo = static_cast<int>(i);
            break;
        }
    }

    std::vector<const char*> comboItems;
    comboItems.reserve(deviceNames.size());
    for (const auto& name : deviceNames) {
        comboItems.push_back(name.c_str());
    }

    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ST_TEXT_DIM), "Video Rendering Engine");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::SetNextItemWidth(contentW);

    PushStylishComboStyle();
    if (ImGui::Combo("##render_engine", &currentDeviceCombo, comboItems.data(), static_cast<int>(comboItems.size()))) {
        int newGpuIdx = deviceValues[currentDeviceCombo];
        EngineController::SetRenderDevice(newGpuIdx);

        if (newGpuIdx == -1) {
            g_ui.daemon.activeGpuIndex = -1;
            g_ui.daemon.gpuName = "CPU Software (swscale)";
            g_ui.daemon.gpuType = "CPU Software";
            g_ui.daemon.vramMB = 0;
            g_shared_engine_state.vram_mb.store(0);
            g_shared_engine_state.active_gpu_index.store(-1);
            g_shared_engine_state.SetActiveGpuInfo("CPU Software", "CPU Software");
        } else if (newGpuIdx >= 0 && newGpuIdx < static_cast<int>(g_ui.hardwareInfo.gpus.size())) {
            g_ui.daemon.activeGpuIndex = newGpuIdx;
            g_ui.daemon.gpuName = g_ui.hardwareInfo.gpus[newGpuIdx].name;
            g_ui.daemon.gpuType = g_ui.hardwareInfo.gpus[newGpuIdx].is_discrete ? "Dedicated GPU" : "Integrated GPU";
            g_shared_engine_state.active_gpu_index.store(newGpuIdx);
            g_shared_engine_state.SetActiveGpuInfo(g_ui.daemon.gpuName, g_ui.daemon.gpuType);
        }
    }
    PopStylishComboStyle();

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Choose whether video decoding & rendering runs on GPU 1, GPU 2, or CPU Software.");
    }

    ImGui::Dummy(ImVec2(0.0f, 14.0f));

    if (StarlumeSliderInt("Target Render FPS", &cfg.target_fps, 15, 60, "", contentW)) {
        EngineController::SetTargetFps(cfg.target_fps, false);
    }
    if (WasStarlumeSliderDeactivated()) {
        g_config.FlushIfDirty(true);
    }

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f), ICON_FA_CIRCLE_CHECK "  Active: %d FPS", g_ui.daemon.fps);

    ImGui::Dummy(ImVec2(0.0f, 12.0f));

    if (StarlumeSliderInt("Battery Saver FPS", &cfg.battery_fps, 10, 30, "", contentW)) {
        g_config.MarkDirty();
    }
    if (WasStarlumeSliderDeactivated()) {
        g_config.FlushIfDirty(true);
    }
    colY += EndGlassCard() + colGap;

    // Kartu 2: Smart Resource Governor
    ImGui::SetCursorScreenPos(ImVec2(leftX, colY));
    BeginGlassCard("PerfGovernorCard", ImVec2(colW, 0.0f), cardPadX, cardPadY, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    GlassCardHeader(ICON_FA_GAUGE_HIGH "  RESOURCE GOVERNOR");

    if (ToggleSwitch("Auto-Sleep on heavy RAM / VRAM load", &cfg.pause_on_resource_heavy)) {
        g_config.Save();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Essential for windowed gaming! When a heavy game or app pushes RAM or VRAM above the threshold, wallpaper enters Deep Sleep (0% CPU, 0 MB VRAM) to yield maximum performance to your game.");
    }

    if (cfg.pause_on_resource_heavy) {
        ImGui::Dummy(ImVec2(0.0f, 14.0f));
        if (StarlumeSliderInt("System RAM Threshold", &cfg.resource_ram_threshold_pct, 60, 95, "%", contentW)) {
            g_config.MarkDirty();
        }
        if (WasStarlumeSliderDeactivated()) {
            g_config.FlushIfDirty(true);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Triggers auto-sleep when overall PC system RAM usage hits this percentage (default 80%).");
        }

        ImGui::Dummy(ImVec2(0.0f, 12.0f));
        if (StarlumeSliderInt("GPU VRAM Threshold", &cfg.resource_vram_threshold_pct, 60, 95, "%", contentW)) {
            g_config.MarkDirty();
        }
        if (WasStarlumeSliderDeactivated()) {
            g_config.FlushIfDirty(true);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Triggers auto-sleep when GPU dedicated video memory usage hits this percentage (default 80%).");
        }

        int cur_ram = sys_ram_pct;
        int cur_vram = g_shared_engine_state.gpu_vram_percent.load();
        if (cur_vram == 0 && !isCpuDecode) {
            cur_vram = s_fallbackVramPct;
        }
        bool ram_high = (cur_ram >= cfg.resource_ram_threshold_pct);
        bool vram_high = (cur_vram >= cfg.resource_vram_threshold_pct);

        char ram_buf[64], vram_buf[64];
        snprintf(ram_buf, sizeof(ram_buf), "%d%%  (limit %d%%)", cur_ram, cfg.resource_ram_threshold_pct);
        snprintf(vram_buf, sizeof(vram_buf), "%d%%  (limit %d%%)", cur_vram, cfg.resource_vram_threshold_pct);

        ImGui::Dummy(ImVec2(0.0f, 14.0f));
        StarlumeBar("System RAM Usage", static_cast<float>(cur_ram) / 100.0f, ram_buf, ram_high ? ST_RED : ST_GOLD,
                    ram_high ? ST_RED : ST_PINK_SOFT, static_cast<float>(cfg.resource_ram_threshold_pct) / 100.0f);

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        if (cur_vram > 0) {
            StarlumeBar("GPU VRAM Usage", static_cast<float>(cur_vram) / 100.0f, vram_buf, vram_high ? ST_RED : ST_GOLD,
                        vram_high ? ST_RED : ST_PINK_SOFT, static_cast<float>(cfg.resource_vram_threshold_pct) / 100.0f);
        } else {
            TextDim("GPU VRAM readout unavailable (software decode).");
        }

        if (is_sleeping) {
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            ImGui::TextColored(ImVec4(1.00f, 0.82f, 0.40f, 1.00f), ICON_FA_EYE_SLASH "  Auto-Sleep Active: Releasing 100% VRAM & pausing rendering for active game");
        }
    }
    colY += EndGlassCard() + colGap;

    // Kartu 3: Video Optimization Transcoding Mode
    ImGui::SetCursorScreenPos(ImVec2(leftX, colY));
    BeginGlassCard("PerfOptimizerCard", ImVec2(colW, 0.0f), cardPadX, cardPadY, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    GlassCardHeader(ICON_FA_EXPAND "  TRANSCODING MODE");

    if (ImGui::RadioButton("Aspect Fit (Proportional - keeps entire frame)", &cfg.optimizer_crop_mode, 0)) {
        g_config.Save();
        InvalidateGalleryCache();
    }
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    TextDim("  Letterboxes the frame if its ratio differs from your screen.");

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    if (ImGui::RadioButton("Aspect Fill / Center Crop (zero black bars)", &cfg.optimizer_crop_mode, 1)) {
        g_config.Save();
        InvalidateGalleryCache();
    }
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    TextDim("  Fills the screen exactly, cropping overflow edges.");
    colY += EndGlassCard() + colGap;

    // ================= Kolom kanan: status & informasi sistem =================

    float colY2 = isNarrow ? colY : colTopY;
    ImGui::SetCursorScreenPos(ImVec2(rightX, colY2));
    BeginGlassCard("PerfStatusCard", ImVec2(colW, 0.0f), cardPadX, cardPadY, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    GlassCardHeader(ICON_FA_CHART_LINE "  ENGINE STATUS");

    if (!g_ui.daemon.connected) {
        ImGui::TextColored(ImVec4(1.00f, 0.30f, 0.43f, 1.0f), "Background Engine Initializing...");
    } else {
        if (g_ui.daemon.playing && !g_ui.daemon.paused) {
            ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f), ICON_FA_CIRCLE_CHECK "  Playing (Active)");
        } else if (g_ui.daemon.paused) {
            ImGui::TextColored(ImVec4(1.00f, 0.82f, 0.40f, 1.00f), ICON_FA_PAUSE "  Paused");
        } else {
            ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.75f, 1.00f), ICON_FA_STOP "  Idle");
        }

        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        std::string vidName = g_ui.daemon.currentVideo.empty()
                                  ? std::string("(None)")
                                  : PathToUtf8(PathFromUtf8(g_ui.daemon.currentVideo).filename());
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));
        ImGui::Text("Active Video: %s", StEllipsize(vidName, 34).c_str());
        ImGui::Text("Render: %d FPS  |  Source: %.1f FPS", g_ui.daemon.fps, g_ui.daemon.videoFps);
        ImGui::Text("Resolution: %dx%d (%s)", g_ui.daemon.width, g_ui.daemon.height, g_ui.daemon.codec.c_str());
        ImGui::Text("Duration: %.1f seconds", g_ui.daemon.duration);
        ImGui::PopStyleVar();
    }
    colY2 += EndGlassCard() + colGap;

    // Kartu 5: Connected Monitors Topology
    size_t dispCount = g_ui.hardwareInfo.displays.size();
    ImGui::SetCursorScreenPos(ImVec2(rightX, colY2));
    BeginGlassCard("PerfMonitorsCard", ImVec2(colW, 0.0f), cardPadX, cardPadY, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    GlassCardHeader(ICON_FA_TV "  CONNECTED MONITORS");

    if (HoloButtonSmall(ICON_FA_ROTATE "  RE-DETECT DISPLAYS")) {
        g_ui.hardwareInfo.displays = HardwareDetector::GetDisplayList();
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    if (dispCount == 0) {
        TextDim("No displays enumerated.");
    } else {
        for (size_t idx = 0; idx < dispCount; ++idx) {
            if (idx > 0) ImGui::Dummy(ImVec2(0.0f, 6.0f));
            const auto& disp = g_ui.hardwareInfo.displays[idx];
            if (disp.is_primary) {
                ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f),
                    "• Display %zu [PRIMARY]: %s", idx + 1, disp.name.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.62f, 0.88f, 1.00f, 1.00f),
                    "• Display %zu [SECONDARY]: %s", idx + 1, disp.name.c_str());
            }
            TextDim("   %d x %d  |  %d Hz  |  pos (%d, %d)",
                    disp.width, disp.height, disp.refresh_rate, disp.pos_x, disp.pos_y);
        }
    }
    colY2 += EndGlassCard() + colGap;

    // Kartu 6: Hardware Topology
    size_t gpuCount = g_ui.hardwareInfo.gpus.size();
    ImGui::SetCursorScreenPos(ImVec2(rightX, colY2));
    BeginGlassCard("PerfHardwareCard", ImVec2(colW, 0.0f), cardPadX, cardPadY, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    GlassCardHeader(ICON_FA_HARD_DRIVE "  HARDWARE TOPOLOGY");

    if (HoloButtonSmall(ICON_FA_ROTATE "  RE-DETECT HARDWARE")) {
        g_ui.hardwareInfo = HardwareDetector::QuerySystemInfo();
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    if (g_ui.daemon.activeGpuIndex == -1) {
        ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f),
            "CPU: %s [SOFTWARE RENDER]", StEllipsize(g_ui.hardwareInfo.cpu.model_name, 26).c_str());
    } else {
        ImGui::TextColored(ImVec4(0.92f, 0.93f, 0.95f, 1.00f),
            "CPU: %s", StEllipsize(g_ui.hardwareInfo.cpu.model_name, 36).c_str());
    }
    TextDim("   %d physical cores / %d logical threads",
            g_ui.hardwareInfo.cpu.physical_cores, g_ui.hardwareInfo.cpu.logical_cores);

    if (gpuCount == 0) {
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        TextDim("GPU: Standard Display Adapter");
    } else {
        for (size_t g_idx = 0; g_idx < gpuCount; ++g_idx) {
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            const auto& gpu = g_ui.hardwareInfo.gpus[g_idx];
            bool is_this_gpu_active = (g_ui.daemon.activeGpuIndex == static_cast<int>(g_idx));
            if (is_this_gpu_active) {
                ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f),
                    "GPU %zu: %s [ACTIVE D3D11]",
                    g_idx + 1, StEllipsize(gpu.name, 26).c_str());
            } else {
                ImGui::TextColored(ImVec4(0.80f, 0.82f, 0.88f, 1.00f),
                    "GPU %zu: %s [%s]",
                    g_idx + 1, StEllipsize(gpu.name, 30).c_str(), gpu.is_discrete ? "dGPU" : "iGPU");
            }
            TextDim("   VRAM: %zu MB (%.1f GB)  |  Shared: %zu MB",
                    gpu.dedicated_vram_mb, static_cast<float>(gpu.dedicated_vram_mb) / 1024.0f,
                    gpu.shared_vram_mb);
        }
    }
    colY2 += EndGlassCard() + colGap;

    // Kartu 7: Diagnostics & Activity Logs
    ImGui::SetCursorScreenPos(ImVec2(rightX, colY2));
    BeginGlassCard("PerfDiagCard", ImVec2(colW, 0.0f), cardPadX, cardPadY, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    GlassCardHeader(ICON_FA_CIRCLE_INFO "  DIAGNOSTICS & LOGS");

    if (g_ui.daemon.injected) {
        ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f), "Desktop Injection (WorkerW): OK");
    } else {
        ImGui::TextColored(ImVec4(1.00f, 0.30f, 0.43f, 1.00f), "Desktop Injection (WorkerW): FAILED");
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    if (g_ui.daemon.activeGpuIndex == -1 || !g_ui.daemon.hwDecode) {
        ImGui::Text("Decoder: Software (CPU swscale)");
    } else {
        ImGui::Text("Decoder: Hardware D3D11VA (HW decode)");
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::Text("Frames Rendered: %llu", static_cast<unsigned long long>(g_ui.daemon.framesRendered));
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    if (!g_ui.daemon.lastError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Last Error: %s", StEllipsize(g_ui.daemon.lastError, 28).c_str());
    } else {
        ImGui::TextColored(ImVec4(0.22f, 0.91f, 1.00f, 1.00f), "Last Error: (none)");
    }

    ImGui::Dummy(ImVec2(0.0f, 12.0f));

    {
        float btnW = (contentW - 8.0f) * 0.5f;
        if (HoloButtonEx(ICON_FA_EXPAND "  FLASH RENDER", ImVec2(btnW, 27.0f), false, true)) {
            EngineController::TriggerTestFlash();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Flashes the injected render window cyan for diagnostics.");
        }
        ImGui::SameLine(0.0f, 8.0f);
        if (HoloButtonEx(ICON_FA_FOLDER_OPEN "  OPEN LOG", ImVec2(btnW, 27.0f), false, true)) {
            const std::wstring& appDataW = GetAppDataDirWide();
            if (!appDataW.empty()) {
                std::wstring logPathW = appDataW + L"\\engine.log";
                ShellExecuteW(nullptr, L"open", L"notepad.exe", logPathW.c_str(), nullptr, SW_SHOW);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Opens the background engine log in Notepad.");
        }
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    TextDim("*Cyan flash on the desktop = injection works.");
    colY2 += EndGlassCard() + colGap;

    ImGui::SetCursorScreenPos(ImVec2(pageStart.x, (std::max)(colY, colY2)));
    ImGui::Dummy(ImVec2(1.0f, 40.0f));
}

} // namespace ui
} // namespace miniwp
