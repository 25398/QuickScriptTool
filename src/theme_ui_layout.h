#pragma once
// ──────────────────────────────────────────────────────────────────
// theme_ui_layout.h — 自定义主题 / 取色弹窗布局（设计稿像素，100% 缩放）
// 供 theme_custom_dialog 与 ThemeUiSelfTest 共用，避免 UI 裁切回归。
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "config.h"

namespace quickscript {
namespace theme_ui {

// 与 settings_dialog 正文/关闭钮字号设计值对齐
constexpr int kBodyFontDesign = 26;
constexpr int kTitleFontDesign = 26;
constexpr int kCloseFontDesign = 36;

constexpr int kCustomDlgW = 480;
constexpr int kCustomDlgH = 340;
constexpr int kCustomTitleH = 40;

constexpr int kColorDlgW = 420;
constexpr int kColorDlgH = 460;
constexpr int kColorTitleH = 40;

inline int RectW(const RECT& rc) { return rc.right - rc.left; }
inline int RectH(const RECT& rc) { return rc.bottom - rc.top; }

inline bool RectNonEmpty(const RECT& rc) {
    return rc.right > rc.left && rc.bottom > rc.top;
}

inline bool RectInsideDialog(const RECT& rc, int dlgW, int dlgH, int pad = 0) {
    return rc.left >= pad && rc.top >= pad
        && rc.right <= dlgW - pad && rc.bottom <= dlgH - pad
        && RectNonEmpty(rc);
}

inline bool RectsOverlap(const RECT& a, const RECT& b) {
    return a.left < b.right && a.right > b.left
        && a.top < b.bottom && a.bottom > b.top;
}

struct CustomThemeLayout {
    RECT close{};
    RECT mainLabel{};
    RECT accentLabel{};
    RECT mainSwatch{};
    RECT accentSwatch{};
    RECT pickMain{};
    RECT pickAccent{};
    RECT randomBtn{};
    RECT footer{};
    RECT ok{};
    RECT cancel{};
};

inline CustomThemeLayout MakeCustomThemeLayout() {
    CustomThemeLayout L{};
    L.close = {kCustomDlgW - kCloseBtnW, 0, kCustomDlgW, kCustomTitleH};
    L.mainLabel = {28, 56, 220, 92};
    L.accentLabel = {252, 56, 452, 92};
    L.mainSwatch = {28, 100, 140, 168};
    L.accentSwatch = {252, 100, 364, 168};
    L.pickMain = {28, 184, 168, 232};
    L.pickAccent = {180, 184, 332, 232};
    L.randomBtn = {344, 184, 452, 232};
    L.footer = {0, 256, kCustomDlgW, kCustomDlgH};
    L.ok = {268, 272, 360, 320};
    L.cancel = {372, 272, 452, 320};
    return L;
}

struct ColorPickerLayout {
    RECT close{};
    RECT sv{};
    RECT hue{};
    RECT preview{};
    RECT previewLabel{};
    RECT ok{};
    RECT cancel{};
};

inline ColorPickerLayout MakeColorPickerLayout() {
    ColorPickerLayout L{};
    L.close = {kColorDlgW - kCloseBtnW, 0, kColorDlgW, kColorTitleH};
    L.sv = {24, 60, 300, 336};
    L.hue = {316, 60, 344, 336};
    L.preview = {360, 60, 400, 132};
    L.previewLabel = {360, 140, 400, 172};
    L.ok = {232, 380, 320, 428};
    L.cancel = {332, 380, 400, 428};
    return L;
}

inline void RgbToHsl(COLORREF c, float& h, float& s, float& l) {
    const float r = GetRValue(c) / 255.0f;
    const float g = GetGValue(c) / 255.0f;
    const float b = GetBValue(c) / 255.0f;
    const float maxv = (std::max)({r, g, b});
    const float minv = (std::min)({r, g, b});
    l = (maxv + minv) * 0.5f;
    if (maxv == minv) {
        h = 0.0f;
        s = 0.0f;
        return;
    }
    const float d = maxv - minv;
    s = l > 0.5f ? d / (2.0f - maxv - minv) : d / (maxv + minv);
    if (maxv == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (maxv == g) h = (b - r) / d + 2.0f;
    else h = (r - g) / d + 4.0f;
    h /= 6.0f;
}

/// 整窗铺底主色：需能压白字，且不能过暗/过灰
inline bool MainColorLooksUsable(COLORREF main) {
    float h = 0, s = 0, l = 0;
    RgbToHsl(main, h, s, l);
    const int lum = (299 * GetRValue(main) + 587 * GetGValue(main) + 114 * GetBValue(main)) / 1000;
    (void)h;
    // 白字可读：不宜过亮；整窗铺底：不宜过暗；观感：饱和度不能太低
    return lum >= 55 && lum <= 210 && s >= 0.28f && l >= 0.22f && l <= 0.72f;
}

}  // namespace theme_ui
}  // namespace quickscript
