#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ui_widgets.h"
#include "settings_app.h"
#include "icons_fontawesome6.h"
#include "core/win_utils.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace miniwp {
namespace ui {

static std::unordered_map<ImGuiID, float> g_animFloats;
static float g_dockAnimPage = -1.0f;
static ComPtr<ID3D11ShaderResourceView> g_appLogoSRV;
static bool g_appLogoAttempted = false;

struct StGlassCardPending {
    ImGuiID uid;
    bool brackets;
    bool autoY;
    float padY;
};
static StGlassCardPending g_stGlassPending[8];
static int g_stGlassPendingN = 0;

void ResetUiAnimations() {
    g_animFloats.clear();
    g_dockAnimPage = -1.0f;
    g_stGlassPendingN = 0;
}

void ReleaseAppLogoTexture() {
    g_appLogoSRV.Reset();
    g_appLogoAttempted = false;
}

void SetupImGuiStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    style.WindowRounding    = 12.0f;
    style.ChildRounding     = 14.0f;
    style.FrameRounding     = 10.0f;
    style.PopupRounding     = 12.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding      = 8.0f;
    style.TabRounding       = 10.0f;

    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.ChildBorderSize   = 1.0f;

    style.WindowPadding     = ImVec2(18.0f, 14.0f);
    style.FramePadding      = ImVec2(10.0f, 7.0f);
    style.ItemSpacing       = ImVec2(12.0f, 10.0f);
    style.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize     = 10.0f;

    colors[ImGuiCol_Text]                  = ImVec4(0.945f, 0.961f, 0.976f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.580f, 0.639f, 0.722f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.032f, 0.045f, 0.078f, 0.50f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.055f, 0.075f, 0.125f, 0.50f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.067f, 0.090f, 0.149f, 0.95f);
    colors[ImGuiCol_Border]                = ImVec4(0.220f, 0.910f, 1.000f, 0.18f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.090f, 0.126f, 0.208f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.122f, 0.165f, 0.267f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.150f, 0.200f, 0.320f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.035f, 0.047f, 0.082f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.055f, 0.075f, 0.125f, 1.00f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.067f, 0.090f, 0.149f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.035f, 0.047f, 0.082f, 0.35f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.220f, 0.910f, 1.000f, 0.45f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.220f, 0.910f, 1.000f, 0.65f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.220f, 0.910f, 1.000f, 0.90f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.220f, 0.910f, 1.000f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.220f, 0.910f, 1.000f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.945f, 0.961f, 0.976f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.220f, 0.910f, 1.000f, 0.12f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.220f, 0.910f, 1.000f, 0.22f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.220f, 0.910f, 1.000f, 0.32f);
    colors[ImGuiCol_Header]                = ImVec4(0.220f, 0.910f, 1.000f, 0.10f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.220f, 0.910f, 1.000f, 0.16f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.220f, 0.910f, 1.000f, 0.26f);
    colors[ImGuiCol_Separator]             = ImVec4(0.220f, 0.910f, 1.000f, 0.14f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.220f, 0.910f, 1.000f, 0.55f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.220f, 0.910f, 1.000f, 0.90f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.067f, 0.090f, 0.149f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.122f, 0.165f, 0.267f, 1.00f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.100f, 0.140f, 0.230f, 1.00f);
    colors[ImGuiCol_TabUnfocused]          = ImVec4(0.045f, 0.060f, 0.100f, 1.00f);
    colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.067f, 0.090f, 0.149f, 1.00f);
    colors[ImGuiCol_PlotLines]             = ImVec4(0.220f, 0.910f, 1.000f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]      = ImVec4(1.000f, 0.820f, 0.400f, 1.00f);
    colors[ImGuiCol_PlotHistogram]         = ImVec4(0.220f, 0.910f, 1.000f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(1.000f, 0.820f, 0.400f, 1.00f);
    colors[ImGuiCol_NavHighlight]          = ImVec4(0.220f, 0.910f, 1.000f, 0.80f);
    colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.020f, 0.030f, 0.060f, 0.55f);
    colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.020f, 0.030f, 0.060f, 0.62f);
}

ImU32 StAlpha(ImU32 col, float a) {
    ImVec4 f = ImGui::ColorConvertU32ToFloat4(col);
    f.w = a;
    return ImGui::ColorConvertFloat4ToU32(f);
}

ImU32 StMix(ImU32 a, ImU32 b, float t) {
    ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a);
    ImVec4 cb = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(
        ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t,
        ca.z + (cb.z - ca.z) * t, ca.w + (cb.w - ca.w) * t));
}

// Animasi nilai per-widget (hover/toggle lerp) berbasis ID widget.
float AnimTowards(ImGuiID key, float target, float speed) {
    auto [it, inserted] = g_animFloats.try_emplace(key, target);
    float& v = it->second;
    if (inserted) return v;

    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f || dt > 0.2f) dt = 1.0f / 60.0f;
    float step = speed * dt;
    if (target > v) {
        v = (target - v < step) ? target : v + step;
        if (v != target) g_ui.animActiveThisFrame = true;
    } else if (target < v) {
        v = (v - target < step) ? target : v - step;
        if (v != target) g_ui.animActiveThisFrame = true;
    }
    return v;
}

// Bayangan lembut berlapis utk elemen melayang (efek z-index: terlihat ngambang, bukan ditempel).
void DrawFloatingShadow(ImVec2 p0, ImVec2 p1, float rounding) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p0.x - 12.0f, p0.y - 6.0f), ImVec2(p1.x + 12.0f, p1.y + 14.0f), IM_COL32(0, 0, 0, 26), rounding + 12.0f);
    dl->AddRectFilled(ImVec2(p0.x - 7.0f, p0.y - 3.0f), ImVec2(p1.x + 7.0f, p1.y + 9.0f), IM_COL32(0, 0, 0, 34), rounding + 7.0f);
    dl->AddRectFilled(ImVec2(p0.x - 3.0f, p0.y - 1.0f), ImVec2(p1.x + 3.0f, p1.y + 5.0f), IM_COL32(0, 0, 0, 42), rounding + 3.0f);
}

// Bar gradien horizontal dgn ujung membulat (cap solid + body gradien, seamless).
void StGradientBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, ImU32 leftCol, ImU32 rightCol) {
    float w = b.x - a.x;
    if (w <= 1.0f) return;
    float r = rounding;
    if (w < r * 3.0f) {
        dl->AddRectFilled(a, b, leftCol, r);
        return;
    }
    dl->AddRectFilled(a, ImVec2(a.x + r * 2.0f, b.y), leftCol, r, ImDrawFlags_RoundCornersLeft);
    dl->AddRectFilled(ImVec2(b.x - r * 2.0f, a.y), b, rightCol, r, ImDrawFlags_RoundCornersRight);
    dl->AddRectFilledMultiColor(ImVec2(a.x + r, a.y), ImVec2(b.x - r, b.y), leftCol, rightCol, rightCol, leftCol);
}

// Bar gradien starlume 3-stop (cyan -> pink -> gold, sesuai kit) dgn ujung membulat.
void StGradientStarlume(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding) {
    float w = b.x - a.x;
    if (w <= 1.5f) return;
    float midX = a.x + w * 0.52f;
    dl->AddRectFilledMultiColor(a, ImVec2(midX, b.y), ST_CYAN, ST_PINK, ST_PINK, ST_CYAN);
    dl->AddRectFilledMultiColor(ImVec2(midX, a.y), b, ST_PINK, ST_GOLD, ST_GOLD, ST_PINK);
    float cap = (std::min)(rounding * 2.0f, w * 0.5f);
    dl->AddRectFilled(a, ImVec2(a.x + cap, b.y), ST_CYAN, rounding, ImDrawFlags_RoundCornersLeft);
    dl->AddRectFilled(ImVec2(b.x - cap, a.y), b, ST_GOLD, rounding, ImDrawFlags_RoundCornersRight);
}

// Glow lembut utk rounded-rect (digambar di belakang shape).
void DrawGlowRect(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, ImU32 color, float strength) {
    if (strength <= 0.01f) return;
    for (int i = 3; i >= 1; --i) {
        float off = static_cast<float>(i) * 2.2f;
        dl->AddRectFilled(ImVec2(a.x - off, a.y - off), ImVec2(b.x + off, b.y + off),
                          StAlpha(color, 0.06f * strength / static_cast<float>(i)), rounding + off);
    }
}

// Sweep shine (Startorch Holo): band menyala melintas sekali saat hover.
void StDrawSweep(ImDrawList* dl, ImVec2 a, ImVec2 b, float progress, ImU32 color) {
    float w = b.x - a.x;
    if (w <= 2.0f || progress < 0.0f || progress > 1.0f) return;
    float bw = w * 0.42f;
    float bx = a.x - bw + (w + 2.0f * bw) * progress;
    dl->PushClipRect(a, b, true);
    ImU32 peak = StAlpha(color, 0.32f);
    ImU32 none = StAlpha(color, 0.0f);
    dl->AddRectFilledMultiColor(ImVec2(bx, a.y), ImVec2(bx + bw * 0.5f, b.y), none, peak, peak, none);
    dl->AddRectFilledMultiColor(ImVec2(bx + bw * 0.5f, a.y), ImVec2(bx + bw, b.y), peak, none, none, peak);
    dl->PopClipRect();
}

// ---- Tombol Holo (Startorch Variant 2: Secondary Holo + Sweep) ----
bool HoloButtonEx(const char* label, ImVec2 size, bool danger, bool compact) {
    std::string txt(label ? label : "");
    for (auto& c : txt) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);

    ImFont* font = ImGui::GetFont();
    float fs = ImGui::GetFontSize() + (compact ? 0.0f : 0.5f);
    ImVec2 tsz = font->CalcTextSizeA(fs, 10000.0f, 0.0f, txt.c_str());
    float h = compact ? 27.0f : 36.0f;
    float padX = compact ? 11.0f : 17.0f;
    if (size.x > 0.0f) size.x = (std::max)(size.x, tsz.x + padX * 2.0f);
    else size.x = tsz.x + padX * 2.0f;
    if (size.y > 0.0f) h = size.y;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGuiID id = ImGui::GetID(label ? label : "");
    ImGui::InvisibleButton(label ? label : "##btn", ImVec2(size.x, h));
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    float ht = AnimTowards(id ^ 0x4857u, hovered ? 1.0f : 0.0f, 10.0f);

    float& sw = g_animFloats[id ^ 0x5357u];
    if (!hovered) sw = -1.0f;
    else if (sw < 0.0f) sw = 0.0f;
    if (sw >= 0.0f && sw < 1.0f) {
        sw += ImGui::GetIO().DeltaTime / 0.55f;
        if (sw > 1.0f) sw = 1.0f;
        else g_ui.animActiveThisFrame = true;
    }

    float lift = ht * 2.0f;
    ImVec2 a = ImVec2(pos.x, pos.y - lift);
    ImVec2 b = ImVec2(pos.x + size.x, pos.y + h - lift);
    ImU32 accent = danger ? ST_RED : ST_CYAN;
    ImU32 fill = StAlpha(accent, (danger ? 0.15f : 0.09f) + (danger ? 0.22f : 0.10f) * ht);
    ImU32 border = StAlpha(accent, 0.42f + 0.58f * ht);
    ImU32 txtCol = StMix(accent, ST_WHITE, 0.35f + 0.45f * ht);
    float round = compact ? 8.0f : 10.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ht > 0.01f) DrawGlowRect(dl, a, b, round, accent, ht);
    dl->AddRectFilled(a, b, fill, round);
    if (sw >= 0.0f && sw <= 1.0f) StDrawSweep(dl, a, b, sw, accent);
    dl->AddRect(a, b, border, round, 0, ht > 0.45f ? 1.5f : 1.1f);

    ImVec2 tp = ImVec2(a.x + (size.x - tsz.x) * 0.5f, a.y + (h - tsz.y) * 0.5f);
    dl->AddText(font, fs, tp, txtCol, txt.c_str());
    return clicked;
}

// ---- Slot ikon (dock & pill): gaya segmented-pill, tanpa teks ----
bool HoloIconButton(const char* id, const char* icon, float w, float h, float rounding, bool danger, bool active, const char* tooltip) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGuiID uid = ImGui::GetID(id);
    ImGui::InvisibleButton(id, ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    float ht = AnimTowards(uid ^ 0x1C07u, hovered ? 1.0f : 0.0f, 12.0f);

    ImU32 accent = danger ? ST_RED : ST_CYAN;
    ImVec2 a = pos, b = ImVec2(pos.x + w, pos.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (active) {
        DrawGlowRect(dl, a, b, rounding, accent, 0.9f);
        dl->AddRectFilled(a, b, StAlpha(accent, 0.22f), rounding);
        dl->AddRect(a, b, StAlpha(accent, 0.60f), rounding, 0, 1.2f);
        StGradientBar(dl, ImVec2(a.x + w * 0.28f, b.y - 4.0f), ImVec2(b.x - w * 0.28f, b.y - 1.5f), 1.25f, StAlpha(accent, 0.55f), ST_WHITE);
    } else if (ht > 0.01f) {
        dl->AddRectFilled(a, b, StAlpha(accent, 0.10f * ht), rounding);
        dl->AddRect(a, b, StAlpha(accent, 0.35f * ht), rounding, 0, 1.0f);
    }

    ImU32 fg = active ? ST_WHITE : StMix(ST_TEXT_DIM, accent, ht);
    ImVec2 tsz = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(a.x + (w - tsz.x) * 0.5f, a.y + (h - tsz.y) * 0.5f), fg, icon);
    if (hovered && tooltip) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

// ---- Toggle switch (pengganti checkbox) ----
bool ToggleSwitch(const char* label, bool* v) {
    const float w = 48.0f, h = 26.0f, gap = 10.0f;
    bool hasLabel = (label && label[0] != '\0');
    ImVec2 tsz = hasLabel ? ImGui::CalcTextSize(label) : ImVec2(0, 0);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float totalW = w + (hasLabel ? gap + tsz.x : 0.0f);
    ImGuiID id = ImGui::GetID(hasLabel ? label : "##tgl");
    ImGui::InvisibleButton(hasLabel ? label : "##tgl", ImVec2(totalW, h));
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    if (clicked) *v = !(*v);
    float t = AnimTowards(id ^ 0x70B5u, *v ? 1.0f : 0.0f, 13.0f);
    float hov = AnimTowards(id ^ 0x70B6u, hovered ? 1.0f : 0.0f, 10.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = pos, b = ImVec2(pos.x + w, pos.y + h);
    if (t > 0.02f) DrawGlowRect(dl, a, b, h * 0.5f, ST_CYAN, t * 0.7f);
    dl->AddRectFilled(a, b, StMix(ST_PANEL_ELEV, StAlpha(ST_CYAN, 0.20f), t), h * 0.5f);
    ImU32 border = StMix(StAlpha(ST_CYAN, 0.16f + 0.24f * hov), StAlpha(ST_CYAN, 0.85f), t);
    dl->AddRect(a, b, border, h * 0.5f, 0, 1.1f);

    float thumbX = a.x + 12.0f + t * (w - 24.0f);
    float cy = a.y + h * 0.5f;
    if (t > 0.02f) dl->AddCircleFilled(ImVec2(thumbX, cy), 11.0f, StAlpha(ST_CYAN, 0.16f * t));
    dl->AddCircleFilled(ImVec2(thumbX, cy), 9.0f, StMix(ST_TEXT_MUTED, ST_CYAN, t));
    dl->AddCircle(ImVec2(thumbX, cy), 9.0f, IM_COL32(9, 12, 21, 170), 0, 1.2f);

    if (hasLabel) {
        dl->AddText(ImVec2(a.x + w + gap, pos.y + (h - tsz.y) * 0.5f), ST_TEXT, label);
    }
    return clicked;
}

// ---- Slider Starlume (Variant 6): track gradien biru->putih, thumb diamond gold ----
bool StarlumeSliderEx(const char* label, float* v, float vmin, float vmax, int decimals, const char* suffix, float width) {
    g_ui.lastSliderDeactivated = false;
    if (vmax <= vmin) return false;
    ImGui::PushID(label);
    float availW = ImGui::GetContentRegionAvail().x;
    float w = (width > 0.0f) ? (std::min)(width, availW) : availW;
    if (w < 40.0f) w = 40.0f;

    char valBuf[64];
    snprintf(valBuf, sizeof(valBuf), "%.*f%s", decimals, static_cast<double>(*v), suffix ? suffix : "");

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(pos, ST_TEXT_DIM, label);
    ImVec2 vtsz = ImGui::CalcTextSize(valBuf);
    dl->AddText(ImVec2(pos.x + w - vtsz.x, pos.y), ST_GOLD, valBuf);
    ImGui::Dummy(ImVec2(w, ImGui::GetFontSize() + 5.0f));

    float trackH = 24.0f;
    ImVec2 tpos = ImGui::GetCursorScreenPos();
    ImGuiID id = ImGui::GetID("##slider");
    ImGui::InvisibleButton("##slider", ImVec2(w, trackH));
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    g_ui.lastSliderDeactivated = ImGui::IsItemDeactivated();
    bool changed = false;
    if (active) {
        g_ui.animActiveThisFrame = true;
        float nx = (ImGui::GetIO().MousePos.x - tpos.x) / w;
        nx = (std::min)(1.0f, (std::max)(0.0f, nx));
        float nv = vmin + nx * (vmax - vmin);
        if (nv != *v) { *v = nv; changed = true; }
    }
    float frac = (*v - vmin) / (vmax - vmin);
    frac = (std::min)(1.0f, (std::max)(0.0f, frac));
    float ht = AnimTowards(id ^ 0x5A17u, (hovered || active) ? 1.0f : 0.0f, 12.0f);

    ImVec2 a = ImVec2(tpos.x, tpos.y + (trackH - 7.0f) * 0.5f);
    ImVec2 b = ImVec2(tpos.x + w, a.y + 7.0f);
    dl->AddRectFilled(a, b, ST_BG_BASE, 3.5f);
    float fx = a.x + (b.x - a.x) * frac;
    if (fx > a.x + 1.5f) StGradientBar(dl, a, ImVec2(fx, b.y), 3.5f, ST_CYAN, ST_WHITE);
    dl->AddRect(a, b, ST_CYAN_LINE, 3.5f, 0, 1.0f);

    float cx = (std::min)(b.x - 3.0f, (std::max)(a.x + 3.0f, fx));
    float cyy = (a.y + b.y) * 0.5f;
    float r = 8.0f + 2.0f * ht;
    if (ht > 0.01f) {
        for (int i = 3; i >= 1; --i) {
            float gr = r + static_cast<float>(i) * 2.0f;
            ImVec2 gp[4] = { ImVec2(cx, cyy - gr), ImVec2(cx + gr, cyy), ImVec2(cx, cyy + gr), ImVec2(cx - gr, cyy) };
            dl->AddConvexPolyFilled(gp, 4, StAlpha(ST_GOLD, 0.06f * ht / static_cast<float>(i)));
        }
    }
    ImVec2 dp[4] = { ImVec2(cx, cyy - r), ImVec2(cx + r, cyy), ImVec2(cx, cyy + r), ImVec2(cx - r, cyy) };
    dl->AddConvexPolyFilled(dp, 4, ST_GOLD);
    dl->AddPolyline(dp, 4, ST_BG_BASE, 2.0f, ImDrawFlags_Closed);
    ImGui::Dummy(ImVec2(w, 2.0f));
    ImGui::PopID();
    return changed;
}

bool StarlumeSliderInt(const char* label, int* v, int vmin, int vmax, const char* suffix, float width) {
    float fv = static_cast<float>(*v);
    if (StarlumeSliderEx(label, &fv, static_cast<float>(vmin), static_cast<float>(vmax), 0, suffix, width)) {
        int nv = static_cast<int>(std::lround(fv));
        nv = (std::min)(vmax, (std::max)(vmin, nv));
        if (nv != *v) { *v = nv; return true; }
    }
    return false;
}

bool StarlumeSliderFloat(const char* label, float* v, float vmin, float vmax, int decimals, const char* suffix, float width) {
    return StarlumeSliderEx(label, v, vmin, vmax, decimals, suffix, width);
}

// ---- Resource usage bars (Startorch Variant 1: STARLUME HARMONY) ----
static void StBarHeader(const char* label, const char* valText, ImU32 valCol, ImU32 labelCol) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(pos, labelCol, label);
    ImVec2 vtsz = ImGui::CalcTextSize(valText);
    dl->AddText(ImVec2(pos.x + w - vtsz.x, pos.y), valCol, valText);
    ImGui::Dummy(ImVec2(w, ImGui::GetFontSize() + 4.0f));
}

static void StarlumeBarInternal(const char* valText, float pct01, bool withHeader, const char* label, ImU32 valCol, ImU32 labelCol, float marker01) {
    pct01 = (std::min)(1.0f, (std::max)(0.0f, pct01));
    float w = ImGui::GetContentRegionAvail().x;
    if (withHeader) StBarHeader(label, valText, valCol, labelCol);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 a = pos, b = ImVec2(pos.x + w, pos.y + 10.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, ST_BG_BASE, 5.0f);
    float fx = a.x + (b.x - a.x) * pct01;
    if (fx > a.x + 2.0f) {
        StGradientStarlume(dl, a, ImVec2(fx, b.y), 5.0f);
        if (fx > a.x + 8.0f) {
            dl->PushClipRect(a, ImVec2(fx - 1.0f, b.y), true);
            float bw = (b.x - a.x) * 0.30f;
            float phase = fmodf(static_cast<float>(ImGui::GetTime()) * 0.55f, 1.0f);
            float bx = a.x - bw + ((b.x - a.x) + 2.0f * bw) * phase;
            ImU32 peak = StAlpha(ST_WHITE, 0.30f);
            ImU32 none = StAlpha(ST_WHITE, 0.0f);
            dl->AddRectFilledMultiColor(ImVec2(bx, a.y), ImVec2(bx + bw * 0.5f, b.y), none, peak, peak, none);
            dl->AddRectFilledMultiColor(ImVec2(bx + bw * 0.5f, a.y), ImVec2(bx + bw, b.y), peak, none, none, peak);
            dl->PopClipRect();
        }
    }
    dl->AddRect(a, b, StAlpha(ST_CYAN, 0.25f), 5.0f, 0, 1.0f);
    if (marker01 >= 0.0f) {
        float mx = a.x + (b.x - a.x) * (std::min)(1.0f, (std::max)(0.0f, marker01));
        dl->AddLine(ImVec2(mx, a.y - 2.0f), ImVec2(mx, b.y + 2.0f), StAlpha(ST_GOLD, 0.85f), 1.6f);
    }
    ImGui::Dummy(ImVec2(w, 10.0f));
}

void StarlumeBar(const char* label, float pct01, const char* valText, ImU32 valCol, ImU32 labelCol, float marker01) {
    StarlumeBarInternal(valText, pct01, true, label, valCol, labelCol, marker01);
}

// ---- Progress bar Starlume (utk progress optimisasi) ----
void StarlumeProgress(float pct01, float h, const char* text) {
    g_ui.animActiveThisFrame = true;
    pct01 = (std::min)(1.0f, (std::max)(0.0f, pct01));
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 a = pos, b = ImVec2(pos.x + w, pos.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = h * 0.5f;
    dl->AddRectFilled(a, b, ST_BG_BASE, r);
    float fx = a.x + (b.x - a.x) * pct01;
    if (fx > a.x + 1.5f) {
        StGradientBar(dl, a, ImVec2(fx, b.y), r, ST_CYAN, ST_WHITE);
        dl->PushClipRect(a, ImVec2(fx, b.y), true);
        float bw = (b.x - a.x) * 0.25f;
        float phase = fmodf(static_cast<float>(ImGui::GetTime()) * 0.7f, 1.0f);
        float bx = a.x - bw + ((b.x - a.x) + 2.0f * bw) * phase;
        ImU32 peak = StAlpha(ST_WHITE, 0.35f);
        ImU32 none = StAlpha(ST_WHITE, 0.0f);
        dl->AddRectFilledMultiColor(ImVec2(bx, a.y), ImVec2(bx + bw * 0.5f, b.y), none, peak, peak, none);
        dl->AddRectFilledMultiColor(ImVec2(bx + bw * 0.5f, a.y), ImVec2(bx + bw, b.y), peak, none, none, peak);
        dl->PopClipRect();
    }
    dl->AddRect(a, b, ST_CYAN_LINE, r, 0, 1.0f);
    if (text && text[0]) {
        ImVec2 tsz = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(a.x + (w - tsz.x) * 0.5f, a.y + (h - tsz.y) * 0.5f), ST_TEXT, text);
    }
    ImGui::Dummy(ImVec2(w, h));
}

// ---- Module card (Startorch Variant 4: Chamfered Module) ----
static void StChamferPath(ImVec2 a, ImVec2 b, float chamfer, float inflate, ImVec2* p) {
    p[0] = ImVec2(a.x - inflate, a.y - inflate);
    p[1] = ImVec2(b.x - chamfer - inflate, a.y - inflate);
    p[2] = ImVec2(b.x + inflate, a.y + chamfer + inflate);
    p[3] = ImVec2(b.x + inflate, b.y + inflate);
    p[4] = ImVec2(a.x + chamfer + inflate, b.y + inflate);
    p[5] = ImVec2(a.x - inflate, b.y - chamfer - inflate);
}

bool BeginModuleCard(const char* id, ImVec2 size, float chamfer, int state, ImGuiWindowFlags flags, float padX, float padY) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 sz = size;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (sz.x <= 0.0f) sz.x = avail.x;
    if (sz.y <= 0.0f) sz.y = avail.y;
    ImVec2 a = pos, b = ImVec2(pos.x + sz.x, pos.y + sz.y);

    ImGuiID uid = ImGui::GetID(id);
    bool hovered = ImGui::IsMouseHoveringRect(a, b);
    float ht = AnimTowards(uid ^ 0x0C0Du, (hovered && state == ST_CARD_NORMAL) ? 1.0f : 0.0f, 9.0f);

    ImU32 accent = (state == ST_CARD_ACTIVE) ? ST_GOLD : ST_CYAN;
    float glow = (state == ST_CARD_SELECTED) ? 0.55f : (state == ST_CARD_ACTIVE ? 0.5f : ht * 0.45f);
    ImU32 fill = ST_GLASS;
    if (state == ST_CARD_NORMAL && hovered) fill = ST_GLASS_HOVER;
    else if (state == ST_CARD_SELECTED) fill = IM_COL32(24, 38, 60, 240);
    else if (state == ST_CARD_ACTIVE) fill = IM_COL32(38, 34, 22, 240);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p[6];
    if (glow > 0.02f) {
        for (int i = 3; i >= 1; --i) {
            float off = static_cast<float>(i) * 2.4f;
            StChamferPath(a, b, chamfer, off, p);
            dl->AddConvexPolyFilled(p, 6, StAlpha(accent, 0.045f * glow / static_cast<float>(i)));
        }
    }
    StChamferPath(a, b, chamfer, 0.0f, p);
    dl->AddConvexPolyFilled(p, 6, fill);
    ImU32 border = (state != ST_CARD_NORMAL) ? StAlpha(accent, 0.85f) : StMix(ST_CYAN_LINE, ST_CYAN_EDGE, ht);
    dl->AddPolyline(p, 6, border, (state == ST_CARD_NORMAL) ? 1.2f : 1.6f, ImDrawFlags_Closed);

    ImGuiChildFlags cf = (padX > 0.0f || padY > 0.0f) ? ImGuiChildFlags_AlwaysUseWindowPadding : ImGuiChildFlags_None;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild(id, sz, cf, flags | ImGuiWindowFlags_NoBringToFrontOnFocus);
    return hovered;
}

void EndModuleCard() {
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

// ---- Glass card (Startorch Variant 1: Exo-Genesis Standard Glass + L-brackets) ----
static void StDrawLBrackets(ImDrawList* dl, ImVec2 a, ImVec2 b, float t) {
    float L = 12.0f + 10.0f * t;
    ImU32 cyan = StMix(ST_CYAN, ST_WHITE, 0.35f * t);
    ImU32 pink = StMix(ST_PINK, ST_WHITE, 0.35f * t);
    if (t > 0.05f) {
        ImU32 gcy = StAlpha(ST_CYAN, 0.16f * t);
        ImU32 gpk = StAlpha(ST_PINK, 0.16f * t);
        dl->AddLine(ImVec2(a.x + 2.0f, a.y + 2.0f + L), ImVec2(a.x + 2.0f, a.y + 2.0f), gcy, 5.0f);
        dl->AddLine(ImVec2(a.x + 2.0f, a.y + 2.0f), ImVec2(a.x + 2.0f + L, a.y + 2.0f), gcy, 5.0f);
        dl->AddLine(ImVec2(b.x - 2.0f, b.y - 2.0f - L), ImVec2(b.x - 2.0f, b.y - 2.0f), gpk, 5.0f);
        dl->AddLine(ImVec2(b.x - 2.0f, b.y - 2.0f), ImVec2(b.x - 2.0f - L, b.y - 2.0f), gpk, 5.0f);
    }
    dl->AddLine(ImVec2(a.x + 2.0f, a.y + 2.0f + L), ImVec2(a.x + 2.0f, a.y + 2.0f), cyan, 2.0f);
    dl->AddLine(ImVec2(a.x + 2.0f, a.y + 2.0f), ImVec2(a.x + 2.0f + L, a.y + 2.0f), cyan, 2.0f);
    dl->AddLine(ImVec2(b.x - 2.0f, b.y - 2.0f - L), ImVec2(b.x - 2.0f, b.y - 2.0f), pink, 2.0f);
    dl->AddLine(ImVec2(b.x - 2.0f, b.y - 2.0f), ImVec2(b.x - 2.0f - L, b.y - 2.0f), pink, 2.0f);
}

void BeginGlassCard(const char* id, ImVec2 size, float padX, float padY, ImGuiWindowFlags flags, bool brackets) {
    ImVec2 sz = size;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (sz.x <= 0.0f) sz.x = avail.x;

    bool autoY = (sz.y <= 0.0f);
    ImGuiChildFlags cf = ImGuiChildFlags_AlwaysUseWindowPadding;
    if (autoY) cf |= ImGuiChildFlags_AutoResizeY;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild(id, ImVec2(sz.x, autoY ? 0.0f : sz.y), cf, flags);

    if (g_stGlassPendingN < IM_ARRAYSIZE(g_stGlassPending)) {
        g_stGlassPending[g_stGlassPendingN].uid = ImGui::GetID(id);
        g_stGlassPending[g_stGlassPendingN].brackets = brackets;
        g_stGlassPending[g_stGlassPendingN].autoY = autoY;
        g_stGlassPending[g_stGlassPendingN].padY = padY;
        g_stGlassPendingN++;
    }
}

float EndGlassCard() {
    ImGuiID uid = 0;
    bool brackets = false;
    bool autoY = false;
    float padY = 18.0f;
    if (g_stGlassPendingN > 0) {
        g_stGlassPendingN--;
        uid = g_stGlassPending[g_stGlassPendingN].uid;
        brackets = g_stGlassPending[g_stGlassPendingN].brackets;
        autoY = g_stGlassPending[g_stGlassPendingN].autoY;
        padY = g_stGlassPending[g_stGlassPendingN].padY;
    }

    if (autoY) {
        ImGui::Dummy(ImVec2(0.0f, padY));
    }

    ImGui::EndChild();
    ImVec2 a = ImGui::GetItemRectMin();
    ImVec2 b = ImGui::GetItemRectMax();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    bool hovered = ImGui::IsMouseHoveringRect(a, b);
    float ht = AnimTowards(uid ^ 0x61A5u, hovered ? 1.0f : 0.0f, 8.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, hovered ? ST_GLASS_HOVER : ST_GLASS, 12.0f);
    dl->AddRect(a, b, StMix(ST_CYAN_LINE, ST_CYAN_EDGE, ht), 12.0f, 0, 1.0f);
    if (brackets) StDrawLBrackets(dl, a, b, ht);
    return b.y - a.y;
}

void GlassCardHeader(const char* titleText) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* headerFont = g_ui.fontSemibold ? g_ui.fontSemibold : ImGui::GetFont();
    float fsize = ImGui::GetFontSize();

    dl->AddText(headerFont, fsize, pos, ST_WHITE, titleText);
    ImGui::Dummy(ImVec2(w, fsize + 12.0f));
}

void TextDim(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ST_TEXT_MUTED));
    ImGui::TextV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

std::string StEllipsize(const std::string& s, size_t maxChars) {
    if (s.size() <= maxChars || maxChars < 4) return s;
    return s.substr(0, maxChars - 3) + "...";
}

// ---- Telemetry metric tile (Startorch Variant 3: Telemetry Metric) ----
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
) {
    (void)statusText;
    (void)statusCol;
    pct01 = (std::min)(1.0f, (std::max)(0.0f, pct01));
    BeginGlassCard(id, size, 22.0f, 20.0f, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, false);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x;

    {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        dl->AddText(pos, ST_TEXT_DIM, label);
        ImGui::Dummy(ImVec2(w, ImGui::GetFontSize() + 6.0f));
    }

    {
        ImVec2 vpos = ImGui::GetCursorScreenPos();
        ImFont* bigFont = g_ui.fontSemibold ? g_ui.fontSemibold : ImGui::GetFont();
        float bigSize = ImGui::GetFontSize() * 1.8f;
        dl->AddText(bigFont, bigSize, vpos, ST_WHITE, valueText);
        ImVec2 vsz = bigFont->CalcTextSizeA(bigSize, 10000.0f, 0.0f, valueText);
        if (unitText && unitText[0]) {
            float uSize = ImGui::GetFontSize() * 0.95f;
            ImVec2 usz = bigFont->CalcTextSizeA(uSize, 10000.0f, 0.0f, unitText);
            dl->AddText(bigFont, uSize, ImVec2(vpos.x + vsz.x + 4.0f, vpos.y + vsz.y - usz.y - 2.0f), ST_CYAN, unitText);
        }

        const float barW = 6.0f, barGap = 4.0f;
        float areaH = (std::max)(22.0f, vsz.y);
        float ox = vpos.x + w - (barW * 5.0f + barGap * 4.0f);
        for (int i = 0; i < 5; ++i) {
            float v01 = (std::min)(1.0f, (std::max)(0.0f, hist5 ? hist5[i] * 0.01f : 0.0f));
            float hh = 7.0f + v01 * (areaH - 8.0f);
            ImVec2 ba = ImVec2(ox + static_cast<float>(i) * (barW + barGap), vpos.y + areaH - hh);
            ImVec2 bb = ImVec2(ba.x + barW, vpos.y + areaH);
            ImU32 col = (i >= 3) ? ((i == 4) ? ST_PINK : StAlpha(ST_PINK, 0.80f))
                                 : StAlpha(ST_CYAN, (i == 1) ? 0.60f : 0.42f);
            if (i == 4) dl->AddRectFilled(ImVec2(ba.x - 2.5f, ba.y - 2.5f), ImVec2(bb.x + 2.5f, bb.y + 2.5f), StAlpha(ST_PINK, 0.20f), 4.0f);
            dl->AddRectFilled(ba, bb, col, 2.5f);
        }
        ImGui::Dummy(ImVec2(w, areaH + 8.0f));
    }

    if (subText && subText[0]) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ST_TEXT_MUTED), "%s", subText);
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }
    {
        ImVec2 a = ImGui::GetCursorScreenPos();
        ImVec2 b = ImVec2(a.x + w, a.y + 6.0f);
        dl->AddRectFilled(a, b, IM_COL32(17, 22, 34, 255), 3.0f);
        float fx = a.x + (b.x - a.x) * pct01;
        if (fx > a.x + 2.0f) {
            StGradientStarlume(dl, a, ImVec2(fx, b.y), 3.0f);
            if (fx > a.x + 8.0f) {
                dl->PushClipRect(a, ImVec2(fx - 1.0f, b.y), true);
                float bw = (b.x - a.x) * 0.35f;
                float phase = fmodf(static_cast<float>(ImGui::GetTime()) * 0.7f, 1.0f);
                float bx = a.x - bw + ((b.x - a.x) + 2.0f * bw) * phase;
                ImU32 peak = StAlpha(ST_WHITE, 0.28f);
                ImU32 none = StAlpha(ST_WHITE, 0.0f);
                dl->AddRectFilledMultiColor(ImVec2(bx, a.y), ImVec2(bx + bw * 0.5f, b.y), none, peak, peak, none);
                dl->AddRectFilledMultiColor(ImVec2(bx + bw * 0.5f, a.y), ImVec2(bx + bw, b.y), peak, none, none, peak);
                dl->PopClipRect();
            }
        }
        dl->AddRect(a, b, StAlpha(ST_CYAN, 0.20f), 3.0f, 0, 1.0f);
        ImGui::Dummy(ImVec2(w, 8.0f));
    }
    EndGlassCard();
}

// Header bar modern & cantik: strip kaca ramping, badge ikon menyala, judul tebal + subjudul inline.
void RenderPageTitle(const char* icon, const char* title, const char* desc) {
    float availW = ImGui::GetContentRegionAvail().x;
    const float headerH = 34.0f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec2 b0 = pos;
    ImVec2 b1 = ImVec2(pos.x + availW, pos.y + headerH);
    dl->AddRectFilled(b0, b1, IM_COL32(14, 20, 34, 150), 8.0f);
    dl->AddRect(b0, b1, IM_COL32(56, 232, 255, 38), 8.0f, 0, 1.0f);

    const float badgeSz = 24.0f;
    ImVec2 badge0 = ImVec2(pos.x + 5.0f, pos.y + 5.0f);
    ImVec2 badge1 = ImVec2(badge0.x + badgeSz, badge0.y + badgeSz);
    dl->AddRectFilled(badge0, badge1, IM_COL32(56, 232, 255, 32), 6.0f);
    dl->AddRect(badge0, badge1, IM_COL32(56, 232, 255, 75), 6.0f, 0, 1.0f);

    ImVec2 icoSz = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(badge0.x + (badgeSz - icoSz.x) * 0.5f, badge0.y + (badgeSz - icoSz.y) * 0.5f), IM_COL32(56, 232, 255, 255), icon);

    ImFont* font = g_ui.fontSemibold ? g_ui.fontSemibold : ImGui::GetFont();
    float fsize = ImGui::GetFontSize();
    float textY = pos.y + (headerH - fsize) * 0.5f;
    float textX = badge1.x + 10.0f;

    dl->AddText(font, fsize, ImVec2(textX, textY), IM_COL32(245, 248, 252, 255), title);
    float titleW = font->CalcTextSizeA(fsize, 1000.0f, 0.0f, title).x;

    if (desc && desc[0] != '\0') {
        float dotX = textX + titleW + 8.0f;
        const char* dotStr = "•";
        dl->AddText(ImVec2(dotX, textY), IM_COL32(56, 232, 255, 140), dotStr);
        float dotW = ImGui::CalcTextSize(dotStr).x;

        float descX = dotX + dotW + 8.0f;
        float remainingW = (b1.x - 12.0f) - descX;
        if (remainingW > 60.0f) {
            dl->PushClipRect(ImVec2(descX, pos.y), ImVec2(b1.x - 12.0f, b1.y), true);
            dl->AddText(ImVec2(descX, textY), IM_COL32(148, 163, 184, 240), desc);
            dl->PopClipRect();
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + headerH + 12.0f));
}

void RenderFloatingDock() {
    const float btnW = 44.0f;
    const float btnH = 34.0f;
    const float btnGap = 6.0f;
    const float padX = 6.0f;
    const float padY = 5.0f;
    const float dockW = padX * 2.0f + 4.0f * btnW + 3.0f * btnGap; // 206.0f
    const float dockH = padY * 2.0f + btnH;                          // 44.0f
    const float dockRounding = dockH * 0.5f;                         // 22.0f
    const float pillRounding = 12.0f;

    ImVec2 winPos = ImGui::GetWindowPos();
    ImVec2 winSize = ImGui::GetWindowSize();
    ImVec2 dockPos = ImVec2(winPos.x + (winSize.x - dockW) * 0.5f, winPos.y + winSize.y - dockH - 12.0f);
    DrawFloatingShadow(dockPos, ImVec2(dockPos.x + dockW, dockPos.y + dockH), 22.0f);
    ImGui::SetCursorScreenPos(dockPos);

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, dockRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX, padY));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.065f, 0.090f, 0.155f, 0.65f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.220f, 0.910f, 1.000f, 0.35f));
    ImGui::BeginChild("FloatingDock", ImVec2(dockW, dockH), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        static const char* icons[4] = { ICON_FA_IMAGES, ICON_FA_LOCK, ICON_FA_SLIDERS, ICON_FA_GAUGE_HIGH };
        static const char* tips[4] = { "Wallpaper Gallery", "Lock Screen & Static", "General Settings", "Performance & Advanced" };
        ImVec2 dockOrigin = ImGui::GetCursorScreenPos();

        if (g_dockAnimPage < 0.0f) {
            g_dockAnimPage = static_cast<float>(g_ui.activePage);
        } else {
            float dt = ImGui::GetIO().DeltaTime;
            if (dt > 0.05f) dt = 0.05f;
            float target = static_cast<float>(g_ui.activePage);
            g_dockAnimPage += (target - g_dockAnimPage) * (1.0f - expf(-18.0f * dt));
            if (fabsf(g_dockAnimPage - target) < 0.001f) {
                g_dockAnimPage = target;
            } else {
                g_ui.animActiveThisFrame = true;
            }
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();

        float curPillX = dockOrigin.x + g_dockAnimPage * (btnW + btnGap);
        float curPillY = dockOrigin.y;
        ImVec2 a = ImVec2(curPillX, curPillY);
        ImVec2 b = ImVec2(curPillX + btnW, curPillY + btnH);

        DrawGlowRect(dl, a, b, pillRounding, ST_CYAN, 0.70f);
        dl->AddRectFilled(a, b, StAlpha(ST_CYAN, 0.22f), pillRounding);
        dl->AddRect(a, b, StAlpha(ST_CYAN, 0.65f), pillRounding, 0, 1.2f);

        for (int i = 0; i < 4; ++i) {
            ImVec2 bp0 = ImVec2(dockOrigin.x + i * (btnW + btnGap), dockOrigin.y);
            ImGui::SetCursorScreenPos(bp0);
            char btnId[32];
            snprintf(btnId, sizeof(btnId), "##dock%d", i);

            ImGui::InvisibleButton(btnId, ImVec2(btnW, btnH));
            bool hov = ImGui::IsItemHovered();
            bool clk = ImGui::IsItemClicked();

            float dist = (std::min)(1.0f, fabsf(g_dockAnimPage - static_cast<float>(i)));
            ImU32 iconCol = StMix(ST_WHITE, (hov ? ST_CYAN : ST_TEXT_DIM), dist);

            if (hov && dist > 0.45f) {
                dl->AddRectFilled(bp0, ImVec2(bp0.x + btnW, bp0.y + btnH), StAlpha(ST_CYAN, 0.08f), pillRounding);
            }

            ImVec2 isz = ImGui::CalcTextSize(icons[i]);
            dl->AddText(ImVec2(bp0.x + (btnW - isz.x) * 0.5f, bp0.y + (btnH - isz.y) * 0.5f), iconCol, icons[i]);
            if (hov && tips[i]) ImGui::SetTooltip("%s", tips[i]);

            if (clk) {
                g_ui.activePage = i;
                g_ui.animActiveThisFrame = true;
            }
        }
    }
    ImGui::EndChild();
    {
        ImDrawList* gdl = ImGui::GetWindowDrawList();
        ImVec2 dMin = ImGui::GetItemRectMin(), dMax = ImGui::GetItemRectMax();
        gdl->AddLine(ImVec2(dMin.x + 20.0f, dMin.y + 1.5f), ImVec2(dMax.x - 20.0f, dMin.y + 1.5f), IM_COL32(255, 255, 255, 45), 1.0f);
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

// Loads miniwallpaper-logo.png once per panel open, WIC-downscaled directly to 64x64
// into an immutable D3D11 texture (~16 KB instead of ~1.5 MB full-res render-target mip chain).
static void LoadAppLogoTexture() {
    if (g_appLogoSRV || g_appLogoAttempted || !g_ui.device) return;
    g_appLogoAttempted = true;

    fs::path baseDir = GetExecutableDir();
    std::vector<fs::path> candidates = {
        baseDir / L"assets" / L"miniwallpaper-logo.png",
        baseDir.parent_path() / L"assets" / L"miniwallpaper-logo.png",
        fs::path(L"assets") / L"miniwallpaper-logo.png"
    };

    fs::path iconPath;
    std::error_code ec;
    for (const auto& p : candidates) {
        if (fs::exists(p, ec)) {
            iconPath = p;
            break;
        }
    }
    if (iconPath.empty()) return;

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ComPtr<IWICImagingFactory> factory;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        ComPtr<IWICBitmapDecoder> decoder;
        std::wstring wpath = iconPath.wstring();
        if (SUCCEEDED(factory->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) {
            ComPtr<IWICBitmapFrameDecode> frame;
            if (SUCCEEDED(decoder->GetFrame(0, &frame))) {
                constexpr UINT kLogoSize = 64;
                ComPtr<IWICBitmapScaler> scaler;
                ComPtr<IWICBitmapSource> scaledSource = frame;
                if (SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
                    SUCCEEDED(scaler->Initialize(frame.Get(), kLogoSize, kLogoSize, WICBitmapInterpolationModeHighQualityCubic))) {
                    scaledSource = scaler;
                }

                ComPtr<IWICFormatConverter> converter;
                if (SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
                    SUCCEEDED(converter->Initialize(scaledSource.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
                    UINT width = 0, height = 0;
                    converter->GetSize(&width, &height);
                    if (width > 0 && height > 0) {
                        UINT stride = width * 4;
                        std::vector<uint8_t> pixels(static_cast<size_t>(stride) * height);
                        if (SUCCEEDED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data()))) {
                            D3D11_TEXTURE2D_DESC desc = {};
                            desc.Width = width;
                            desc.Height = height;
                            desc.MipLevels = 1;
                            desc.ArraySize = 1;
                            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                            desc.SampleDesc.Count = 1;
                            desc.Usage = D3D11_USAGE_IMMUTABLE;
                            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                            D3D11_SUBRESOURCE_DATA initData = {};
                            initData.pSysMem = pixels.data();
                            initData.SysMemPitch = stride;

                            ComPtr<ID3D11Texture2D> tex;
                            if (SUCCEEDED(g_ui.device->CreateTexture2D(&desc, &initData, &tex))) {
                                g_ui.device->CreateShaderResourceView(tex.Get(), nullptr, &g_appLogoSRV);
                            }
                        }
                    }
                }
            }
        }
    }

    if (SUCCEEDED(hrCo)) CoUninitialize();
}

// Custom Glass Title Bar dengan Minimize, Maximize/Restore, dan Close
void RenderCustomTitleBar(float winWidth, float titleBarH) {
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec2 b0 = origin;
    ImVec2 b1 = ImVec2(origin.x + winWidth, origin.y + titleBarH);
    dl->AddRectFilled(b0, b1, IM_COL32(11, 15, 26, 128));
    dl->AddLine(ImVec2(b0.x, b1.y - 0.5f), ImVec2(b1.x, b1.y - 0.5f), IM_COL32(56, 232, 255, 38), 1.0f);

    float leftPad = 14.0f;
    float textY = origin.y + (titleBarH - ImGui::GetFontSize()) * 0.5f;
    ImFont* semiboldFont = g_ui.fontSemibold ? g_ui.fontSemibold : ImGui::GetFont();

    LoadAppLogoTexture();
    float iconW = 24.0f;
    if (g_appLogoSRV) {
        float iconSz = 24.0f;
        float iconY = origin.y + (titleBarH - iconSz) * 0.5f;
        dl->AddImageRounded((ImTextureID)g_appLogoSRV.Get(),
            ImVec2(origin.x + leftPad, iconY),
            ImVec2(origin.x + leftPad + iconSz, iconY + iconSz),
            ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 5.0f);
        iconW = iconSz + 10.0f;
    } else {
        dl->AddText(semiboldFont, ImGui::GetFontSize() * 1.10f, ImVec2(origin.x + leftPad, textY - 1.0f), IM_COL32(56, 232, 255, 255), ICON_FA_DESKTOP);
        iconW = ImGui::CalcTextSize(ICON_FA_DESKTOP).x + 10.0f;
    }

    float titleX = origin.x + leftPad + iconW;
    dl->AddText(semiboldFont, ImGui::GetFontSize() * 1.05f, ImVec2(titleX, textY - 1.0f), IM_COL32(245, 248, 252, 255), "MiniWallpaper");
    float titleW = semiboldFont->CalcTextSizeA(ImGui::GetFontSize() * 1.05f, 1000.0f, 0.0f, "MiniWallpaper").x;

    float badgeX = titleX + titleW + 10.0f;
    const char* verStr = "v2.0.0";
    ImVec2 verSz = ImGui::CalcTextSize(verStr);
    ImVec2 vp0 = ImVec2(badgeX, textY - 1.0f);
    ImVec2 vp1 = ImVec2(badgeX + verSz.x + 12.0f, vp0.y + verSz.y + 3.0f);
    dl->AddRectFilled(vp0, vp1, IM_COL32(56, 232, 255, 28), 5.0f);
    dl->AddRect(vp0, vp1, IM_COL32(56, 232, 255, 75), 5.0f, 0, 1.0f);
    dl->AddText(ImVec2(vp0.x + 6.0f, vp0.y + 1.0f), IM_COL32(56, 232, 255, 240), verStr);

    float leftTotalW = (vp1.x - origin.x) + 20.0f;

    float btnW = 46.0f;
    float totalBtnsW = btnW * 3.0f;
    float btnsStartX = origin.x + winWidth - totalBtnsW;

    float dragW = btnsStartX - (origin.x + leftTotalW);
    if (dragW > 20.0f) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + leftTotalW, origin.y));
        ImGui::InvisibleButton("##titlebar_drag", ImVec2(dragW, titleBarH));
        if (ImGui::IsItemActive() && ImGui::IsMouseClicked(0)) {
            ReleaseCapture();
            SendMessageW(g_ui.hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            if (IsZoomed(g_ui.hWnd)) ShowWindow(g_ui.hWnd, SW_RESTORE);
            else ShowWindow(g_ui.hWnd, SW_MAXIMIZE);
        }
    }

    bool isMax = IsZoomed(g_ui.hWnd);
    const char* btnIds[3] = { "##win_min", "##win_max", "##win_close" };

    for (int i = 0; i < 3; ++i) {
        ImVec2 bp0 = ImVec2(btnsStartX + i * btnW, origin.y);
        ImVec2 bp1 = ImVec2(bp0.x + btnW, origin.y + titleBarH);
        ImGui::SetCursorScreenPos(bp0);
        ImGui::InvisibleButton(btnIds[i], ImVec2(btnW, titleBarH));
        bool hov = ImGui::IsItemHovered();
        bool act = ImGui::IsItemActive();
        bool clk = ImGui::IsItemClicked();

        bool isClose = (i == 2);
        ImU32 bgFill = 0;
        if (isClose) {
            if (act) bgFill = IM_COL32(200, 28, 44, 255);
            else if (hov) bgFill = IM_COL32(232, 28, 48, 225);
        } else {
            if (act) bgFill = IM_COL32(255, 255, 255, 36);
            else if (hov) bgFill = IM_COL32(255, 255, 255, 18);
        }
        if (bgFill != 0) {
            dl->AddRectFilled(bp0, bp1, bgFill);
        }

        ImU32 iconColor = (isClose && hov) ? IM_COL32_WHITE : (hov ? IM_COL32(255, 255, 255, 240) : IM_COL32(180, 192, 208, 210));
        ImVec2 center = ImVec2(bp0.x + btnW * 0.5f, origin.y + titleBarH * 0.5f);

        if (i == 0) {
            dl->AddLine(ImVec2(center.x - 5.0f, center.y), ImVec2(center.x + 5.0f, center.y), iconColor, 1.3f);
        } else if (i == 1) {
            if (isMax) {
                dl->AddRect(ImVec2(center.x - 3.0f, center.y - 5.0f), ImVec2(center.x + 5.0f, center.y + 3.0f), iconColor, 0.0f, 0, 1.1f);
                dl->AddRectFilled(ImVec2(center.x - 5.0f, center.y - 3.0f), ImVec2(center.x + 3.0f, center.y + 5.0f), bgFill != 0 ? bgFill : IM_COL32(11, 15, 26, 255));
                dl->AddRect(ImVec2(center.x - 5.0f, center.y - 3.0f), ImVec2(center.x + 3.0f, center.y + 5.0f), iconColor, 0.0f, 0, 1.1f);
            } else {
                dl->AddRect(ImVec2(center.x - 5.0f, center.y - 5.0f), ImVec2(center.x + 5.0f, center.y + 5.0f), iconColor, 0.0f, 0, 1.3f);
            }
        } else if (i == 2) {
            dl->AddLine(ImVec2(center.x - 4.5f, center.y - 4.5f), ImVec2(center.x + 4.5f, center.y + 4.5f), iconColor, 1.4f);
            dl->AddLine(ImVec2(center.x + 4.5f, center.y - 4.5f), ImVec2(center.x - 4.5f, center.y + 4.5f), iconColor, 1.4f);
        }

        if (clk) {
            if (i == 0) SettingsUI::Close();
            else if (i == 1) {
                if (isMax) ShowWindow(g_ui.hWnd, SW_RESTORE);
                else ShowWindow(g_ui.hWnd, SW_MAXIMIZE);
            } else if (i == 2) {
                SettingsUI::Close();
            }
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + titleBarH));
}

} // namespace ui
} // namespace miniwp
