#pragma once
#include <windows.h>
#include <d3d11.h>
#include <imgui.h>
#include <cstdint>
#include <string>
#include "platform/win32/hardware_info.h"

namespace miniwp {
namespace ui {

// =========================================================================
// STARTORCH UI KIT (MiniWallpaper edition) — palette & card state constants
// =========================================================================
#define ST_BG_BASE     IM_COL32(9, 12, 21, 255)
#define ST_PANEL       IM_COL32(17, 23, 38, 245)
#define ST_PANEL_ELEV  IM_COL32(23, 32, 53, 255)
#define ST_GLASS       IM_COL32(20, 28, 46, 195)
#define ST_GLASS_HOVER IM_COL32(29, 41, 66, 215)
#define ST_CYAN        IM_COL32(56, 232, 255, 255)
#define ST_WHITE       IM_COL32(241, 245, 249, 255)
#define ST_TEXT        IM_COL32(241, 245, 249, 255)
#define ST_TEXT_DIM    IM_COL32(148, 163, 184, 255)
#define ST_TEXT_MUTED  IM_COL32(100, 116, 139, 255)
#define ST_GOLD        IM_COL32(255, 209, 102, 255)
#define ST_RED         IM_COL32(255, 77, 109, 255)
#define ST_CYAN_LINE   IM_COL32(56, 232, 255, 46)
#define ST_CYAN_EDGE   IM_COL32(56, 232, 255, 107)
#define ST_PINK        IM_COL32(255, 148, 178, 255)
#define ST_PINK_SOFT   IM_COL32(255, 196, 214, 255)
#define ST_GREEN       IM_COL32(46, 229, 157, 255)

enum StCardState {
    ST_CARD_NORMAL = 0,
    ST_CARD_SELECTED = 1,
    ST_CARD_ACTIVE = 2
};

struct DaemonStatusSnapshot {
    bool connected = false;
    bool playing = false;
    bool paused = false;
    std::string currentVideo;
    int fps = 0;
    double videoFps = 0.0;
    int width = 0;
    int height = 0;
    double duration = 0.0;
    double currentTimeSec = 0.0;
    std::string codec;
    size_t ramMB = 0;
    size_t ramPrivateMB = 0;
    size_t vramMB = 0;
    double cpuPercent = 0.0;
    bool injected = false;
    bool hwDecode = false;
    int activeGpuIndex = 0;
    std::string gpuName;
    std::string gpuType;
    uint64_t framesRendered = 0;
    std::string lastError;
};

struct UiContext {
    HWND hWnd = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* deviceContext = nullptr;
    ImFont* fontSemibold = nullptr;
    float dpiScale = 1.0f;

    int activePage = 0; // 0=Gallery, 1=Lock&Static, 2=General, 3=Performance
    int selectedGalleryIdx = -1;
    bool detailVisible = false;
    bool dragOverlay = false;

    SystemHardwareInfo hardwareInfo;
    bool hardwareInfoInit = false;

    DaemonStatusSnapshot daemon;

    // Adaptive frame-pacing tracking
    bool animActiveThisFrame = false;
    bool lastSliderDeactivated = false;
    uint64_t lastInteractionTickMs = 0;
};

extern UiContext g_ui;

void MarkUiInteraction() noexcept;
void EnsureHardwareInfoLoaded();

// Video activation & GPU optimization helpers (shared by pages, modals, and drag-drop)
void ApplyAction(const std::string& utf8_path, const std::string& action);
void RequestApplyVideo(const std::string& utf8_path, const std::string& action);
void StartVideoOptimization(
    const std::string& input_path,
    int target_w,
    int target_h,
    const std::string& action,
    int crop_mode = 0
);
void InvalidateGalleryCache() noexcept;

} // namespace ui
} // namespace miniwp
