#pragma once
// ──────────────────────────────────────────────────────────────────
// app_theme.h — 应用主题颜色定义与运行时切换
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include "app_settings.h"

namespace quickscript {

struct AppTheme {
    const wchar_t* name = L"";

    // 品牌主色
    COLORREF mainColor = RGB(64, 168, 99);
    COLORREF darkColor = RGB(54, 141, 82);
    COLORREF accentColor = RGB(255, 154, 72);
    COLORREF lightBg = RGB(232, 248, 239);

    // 控件 / 卡片
    COLORREF cardColor = RGB(67, 161, 94);
    COLORREF cardHoverColor = RGB(58, 148, 87);
    COLORREF buttonColor = RGB(70, 185, 111);
    COLORREF buttonHoverColor = RGB(54, 153, 88);
    COLORREF lineColor = RGB(197, 220, 205);
    COLORREF indexColor = RGB(75, 166, 103);
    COLORREF batchSelectedRow = RGB(232, 245, 238);
    COLORREF comboHover = RGB(232, 248, 239);
    COLORREF comboMenuHover = RGB(229, 243, 255);
    COLORREF comboMenuSelect = RGB(0, 102, 204);
    COLORREF buttonDisabled = RGB(190, 205, 194);
    COLORREF tabGradientStart = RGB(59, 157, 92);
    COLORREF tabGradientEnd = RGB(44, 128, 75);
    COLORREF closeHover = RGB(90, 190, 125);
    COLORREF navStripColor = RGB(58, 160, 94);   ///< 顶/底导航条：主色略加深
    COLORREF tabActiveColor = RGB(52, 148, 86);   ///< 选中 Tab：在导航条基础上再略加深

    // 主界面底部操作条 / 标签
    COLORREF bannerBg = RGB(255, 244, 138);
    COLORREF bannerTag = RGB(255, 174, 42);
    COLORREF bannerText = RGB(60, 60, 60);
    COLORREF selectedTagBg = RGB(255, 241, 122);

    // 辅助文字 / 滚动条
    COLORREF secondaryText = RGB(220, 245, 225);
    COLORREF footerHint = RGB(210, 245, 215);
    COLORREF scrollTrack = RGB(52, 143, 84);
    COLORREF scrollThumb = RGB(41, 120, 72);

    // 提示弹窗
    COLORREF promptBg = RGB(255, 255, 255);
    COLORREF promptBorder = RGB(204, 204, 204);
    COLORREF promptText = RGB(40, 40, 40);
    COLORREF promptOkFill = RGB(255, 154, 72);
    COLORREF promptOkHover = RGB(255, 174, 95);
    COLORREF promptOkText = RGB(255, 255, 255);
    COLORREF promptCancelBorder = RGB(180, 180, 180);
};

constexpr int kThemeCount = 7;
/// 设置下拉里「自定义」固定为第 0 项，其后才是预设主题
constexpr int kCustomThemeComboIndex = 0;

AppTheme BuildTheme(const wchar_t* name, COLORREF main, COLORREF light, COLORREF accent,
                    bool classicBanner = true);
COLORREF DeriveLightBg(COLORREF main);
void RandomAttractiveThemeColors(COLORREF& mainOut, COLORREF& accentOut);

const AppTheme& CurrentTheme();
const AppTheme& ThemeById(int themeId);
const AppTheme* ThemeCatalog();
void SetCurrentThemeId(int themeId);
void ApplyThemeFromSettings(const AppSettings& settings);

}  // namespace quickscript
