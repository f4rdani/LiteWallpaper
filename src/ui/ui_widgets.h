#pragma once
#include "ui_internal.h"
#include <string>

namespace miniwp {
namespace ui {

void SetupImGuiStyle();
void ResetUiAnimations();
void ReleaseAppLogoTexture();

ImU32 StAlpha(ImU32 col, float a);
ImU32 StMix(ImU32 a, ImU32 b, float t);
float AnimTowards(ImGuiID key, float target, float speed);

void DrawFloatingShadow(ImVec2 p0, ImVec2 p1, float rounding);
void StGradientBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, ImU32 leftCol, ImU32 rightCol);
void StGradientStarlume(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding);
void DrawGlowRect(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, ImU32 color, float strength);
void StDrawSweep(ImDrawList* dl, ImVec2 a, ImVec2 b, float progress, ImU32 color);

bool HoloButtonEx(const char* label, ImVec2 size, bool danger, bool compact);
inline bool HoloButton(const char* label, ImVec2 size = ImVec2(0, 0)) { return HoloButtonEx(label, size, false, false); }
inline bool HoloDangerButton(const char* label, ImVec2 size = ImVec2(0, 0)) { return HoloButtonEx(label, size, true, false); }
inline bool HoloButtonSmall(const char* label) { return HoloButtonEx(label, ImVec2(0, 0), false, true); }
inline bool HoloDangerButtonSmall(const char* label) { return HoloButtonEx(label, ImVec2(0, 0), true, true); }

bool HoloIconButton(const char* id, const char* icon, float w, float h, float rounding, bool danger, bool active, const char* tooltip);
bool ToggleSwitch(const char* label, bool* v);

bool StarlumeSliderEx(const char* label, float* v, float vmin, float vmax, int decimals, const char* suffix, float width);
bool StarlumeSliderInt(const char* label, int* v, int vmin, int vmax, const char* suffix = "", float width = -1.0f);
bool StarlumeSliderFloat(const char* label, float* v, float vmin, float vmax, int decimals = 1, const char* suffix = "", float width = -1.0f);
inline bool WasStarlumeSliderDeactivated() noexcept { return g_ui.lastSliderDeactivated; }

void StarlumeBar(const char* label, float pct01, const char* valText, ImU32 valCol = ST_GOLD, ImU32 labelCol = ST_PINK_SOFT, float marker01 = -1.0f);
void StarlumeProgress(float pct01, float h, const char* text);

bool BeginModuleCard(const char* id, ImVec2 size, float chamfer = 18.0f, int state = ST_CARD_NORMAL, ImGuiWindowFlags flags = 0, float padX = 16.0f, float padY = 14.0f);
void EndModuleCard();

void BeginGlassCard(
    const char* id,
    ImVec2 size,
    float padX = 20.0f,
    float padY = 18.0f,
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse,
    bool brackets = false
);
float EndGlassCard();
void GlassCardHeader(const char* titleText);

void TextDim(const char* fmt, ...);
std::string StEllipsize(const std::string& s, size_t maxChars);

void TelemetryTile(
    const char* id,
    const char* label,
    float pct01,
    const char* valueText,
    const char* unitText,
    const char* statusText,
    ImU32 statusCol,
    const float* hist5,
    const char* subText,
    ImVec2 size
);

void RenderPageTitle(const char* icon, const char* title, const char* desc);
void RenderFloatingDock();
void RenderCustomTitleBar(float winWidth, float titleBarH);

} // namespace ui
} // namespace miniwp
