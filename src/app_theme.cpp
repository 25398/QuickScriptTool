#include "app_theme.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>

namespace quickscript {
namespace {

COLORREF AdjustRgb(COLORREF c, int dr, int dg, int db) {
    return RGB(
        std::clamp(GetRValue(c) + dr, 0, 255),
        std::clamp(GetGValue(c) + dg, 0, 255),
        std::clamp(GetBValue(c) + db, 0, 255));
}

COLORREF BlendColors(COLORREF from, COLORREF to, float t) {
    const float u = 1.0f - t;
    return RGB(
        static_cast<int>(GetRValue(from) * u + GetRValue(to) * t),
        static_cast<int>(GetGValue(from) * u + GetGValue(to) * t),
        static_cast<int>(GetBValue(from) * u + GetBValue(to) * t));
}

float HueToRgb(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

COLORREF HslToRgb(float h, float s, float l) {
    h = h - std::floor(h);
    s = std::clamp(s, 0.0f, 1.0f);
    l = std::clamp(l, 0.0f, 1.0f);
    float r = l, g = l, b = l;
    if (s > 0.0001f) {
        const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
        const float p = 2.0f * l - q;
        r = HueToRgb(p, q, h + 1.0f / 3.0f);
        g = HueToRgb(p, q, h);
        b = HueToRgb(p, q, h - 1.0f / 3.0f);
    }
    return RGB(
        static_cast<int>(std::lround(r * 255.0f)),
        static_cast<int>(std::lround(g * 255.0f)),
        static_cast<int>(std::lround(b * 255.0f)));
}

int RandRange(int lo, int hi) {
    return lo + (std::rand() % (hi - lo + 1));
}

AppTheme MakeTheme(const wchar_t* name, COLORREF main, COLORREF light, COLORREF accent,
                   bool classicBanner = false) {
    AppTheme t{};
    t.name = name;
    t.mainColor = main;
    t.darkColor = AdjustRgb(main, -8, -22, -14);
    t.accentColor = accent;
    t.lightBg = light;

    t.cardColor = AdjustRgb(main, 2, -5, -3);
    t.cardHoverColor = AdjustRgb(main, -5, -16, -10);
    t.buttonColor = AdjustRgb(main, 4, 12, 8);
    t.buttonHoverColor = AdjustRgb(main, -12, -18, -12);
    t.lineColor = BlendColors(light, main, 0.28f);
    t.indexColor = AdjustRgb(main, 8, -3, 5);
    t.batchSelectedRow = BlendColors(light, RGB(255, 255, 255), 0.35f);
    t.comboHover = light;
    t.comboMenuHover = BlendColors(light, RGB(255, 255, 255), 0.45f);
    t.comboMenuSelect = main;
    t.buttonDisabled = BlendColors(light, RGB(190, 190, 190), 0.5f);

    t.tabGradientStart = AdjustRgb(main, 6, 4, 4);
    t.tabGradientEnd = AdjustRgb(main, -10, -16, -10);
    t.closeHover = AdjustRgb(main, 22, 18, 18);
    t.navStripColor = AdjustRgb(main, -3, -8, -5);
    t.tabActiveColor = AdjustRgb(t.navStripColor, -4, -10, -6);

    if (classicBanner) {
        t.bannerBg = RGB(255, 244, 138);
        t.bannerTag = RGB(255, 174, 42);
        t.selectedTagBg = RGB(255, 241, 122);
    } else {
        t.bannerBg = BlendColors(light, RGB(255, 255, 255), 0.25f);
        t.bannerTag = accent;
        t.selectedTagBg = BlendColors(light, accent, 0.22f);
    }
    t.bannerText = RGB(55, 55, 55);

    t.secondaryText = BlendColors(light, main, 0.52f);
    t.footerHint = BlendColors(light, main, 0.42f);
    t.scrollTrack = AdjustRgb(main, -12, -18, -12);
    t.scrollThumb = AdjustRgb(main, -24, -32, -22);

    t.promptBg = RGB(255, 255, 255);
    t.promptBorder = BlendColors(light, main, 0.32f);
    t.promptText = RGB(40, 40, 40);
    t.promptOkFill = t.buttonColor;
    t.promptOkHover = t.buttonHoverColor;
    t.promptOkText = RGB(255, 255, 255);
    t.promptCancelBorder = BlendColors(main, RGB(180, 180, 180), 0.35f);
    return t;
}

const AppTheme kThemes[] = {
    MakeTheme(L"经典绿橙", RGB(64, 168, 99), RGB(232, 248, 239), RGB(255, 154, 72), true),
    MakeTheme(L"晴空蓝", RGB(78, 148, 210), RGB(230, 240, 248), RGB(235, 148, 40), true),
    MakeTheme(L"活力橙", RGB(230, 145, 52), RGB(255, 244, 228), RGB(20, 168, 188), true),
    MakeTheme(L"珊瑚橙", RGB(228, 118, 88), RGB(250, 236, 232), RGB(240, 196, 72), true),
    MakeTheme(L"梦幻紫", RGB(171, 71, 188), RGB(243, 229, 245), RGB(255, 183, 77), true),
    MakeTheme(L"青柠薄荷", RGB(38, 166, 154), RGB(224, 247, 250), RGB(255, 152, 0), true),
    MakeTheme(L"樱花粉", RGB(236, 64, 122), RGB(252, 228, 236), RGB(255, 193, 7), true),
};

AppTheme gResolvedTheme = kThemes[0];
int gCurrentThemeId = 0;
bool gRandSeeded = false;

}  // namespace

AppTheme BuildTheme(const wchar_t* name, COLORREF main, COLORREF light, COLORREF accent,
                    bool classicBanner) {
    return MakeTheme(name, main, light, accent, classicBanner);
}

COLORREF DeriveLightBg(COLORREF main) {
    return BlendColors(RGB(255, 255, 255), main, 0.12f);
}

void RandomAttractiveThemeColors(COLORREF& mainOut, COLORREF& accentOut) {
    if (!gRandSeeded) {
        std::srand(static_cast<unsigned>(std::time(nullptr)) ^ GetTickCount());
        gRandSeeded = true;
    }

    static const COLORREF kPairs[][2] = {
        {RGB(64, 168, 99), RGB(255, 154, 72)},
        {RGB(78, 148, 210), RGB(235, 148, 40)},
        {RGB(230, 145, 52), RGB(20, 168, 188)},
        {RGB(228, 118, 88), RGB(240, 196, 72)},
        {RGB(171, 71, 188), RGB(255, 183, 77)},
        {RGB(38, 166, 154), RGB(255, 152, 0)},
        {RGB(236, 64, 122), RGB(255, 193, 7)},
        {RGB(52, 152, 219), RGB(241, 196, 15)},
        {RGB(155, 89, 182), RGB(46, 204, 113)},
        {RGB(231, 76, 60), RGB(241, 196, 15)},
        {RGB(26, 188, 156), RGB(230, 126, 34)},
        {RGB(52, 73, 94), RGB(52, 152, 219)},
        {RGB(142, 68, 173), RGB(241, 196, 15)},
        {RGB(39, 174, 96), RGB(230, 126, 34)},
        {RGB(41, 128, 185), RGB(231, 76, 60)},
        {RGB(211, 84, 0), RGB(52, 152, 219)},
    };

    if (RandRange(0, 9) < 7) {
        const int idx = RandRange(0, static_cast<int>(sizeof(kPairs) / sizeof(kPairs[0])) - 1);
        mainOut = kPairs[idx][0];
        accentOut = kPairs[idx][1];
        return;
    }

    const float mainH = RandRange(0, 359) / 360.0f;
    const float mainS = RandRange(48, 68) / 100.0f;
    const float mainL = RandRange(42, 55) / 100.0f;
    mainOut = HslToRgb(mainH, mainS, mainL);

    float accentH = mainH + (RandRange(25, 55) / 360.0f);
    if (RandRange(0, 1) == 0) accentH = mainH + 0.5f + (RandRange(-20, 20) / 360.0f);
    const float accentS = RandRange(55, 78) / 100.0f;
    const float accentL = RandRange(48, 60) / 100.0f;
    accentOut = HslToRgb(accentH, accentS, accentL);
}

const AppTheme& CurrentTheme() {
    return gResolvedTheme;
}

const AppTheme& ThemeById(int themeId) {
    if (themeId < 0 || themeId >= kThemeCount) return kThemes[0];
    return kThemes[themeId];
}

const AppTheme* ThemeCatalog() {
    return kThemes;
}

void SetCurrentThemeId(int themeId) {
    gCurrentThemeId = std::clamp(themeId, 0, kThemeCount - 1);
    gResolvedTheme = kThemes[gCurrentThemeId];
}

void ApplyThemeFromSettings(const AppSettings& settings) {
    if (settings.other.useCustomTheme) {
        const COLORREF main = static_cast<COLORREF>(settings.other.customMainColor);
        const COLORREF accent = static_cast<COLORREF>(settings.other.customAccentColor);
        gResolvedTheme = MakeTheme(L"自定义", main, DeriveLightBg(main), accent, true);
        gCurrentThemeId = std::clamp(settings.other.themeId, 0, kThemeCount - 1);
        return;
    }
    SetCurrentThemeId(settings.other.themeId);
}

}  // namespace quickscript
