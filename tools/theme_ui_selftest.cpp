// =============================================================================
// ThemeUiSelfTest — 自定义主题 / 取色弹窗布局与配色自检
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:ThemeUiSelfTest
//   build\Release\ThemeUiSelfTest.exe --json
//
// 说明：无法在无头环境里“看见闪烁”，但可回归检测：
//   - 控件是否超出对话框（裁切）
//   - 确定/取消是否落在底栏内
//   - 标签区宽度能否容纳关键文案（字号与设置窗对齐）
//   - 随机主色是否落在可用饱和/明度带
//   - BuildTheme / ApplyTheme 自定义色是否保留
// =============================================================================
#include "selftest_harness.h"

#include "app_settings.h"
#include "app_theme.h"
#include "config.h"
#include "theme_ui_layout.h"
#include "ui_scale.h"

#include <string>

namespace {

using selftest::Emit;
using namespace quickscript;
using namespace quickscript::theme_ui;

const selftest::CaseInfo kCases[] = {
    {L"font_design_matches_settings", L"layout",
        L"Body/title/close font design heights match settings dialog"},
    {L"custom_layout_in_bounds", L"layout",
        L"Custom theme controls stay inside dialog (no clip)"},
    {L"custom_footer_buttons_visible", L"layout",
        L"OK/Cancel fully inside footer band"},
    {L"custom_action_buttons_no_overlap", L"layout",
        L"Pick-main / pick-accent / random do not overlap"},
    {L"custom_label_width_fits_zh", L"layout",
        L"Accent label rect fits 点缀色（标签/强调） at body font"},
    {L"color_picker_layout_in_bounds", L"layout",
        L"Color picker SV/hue/preview/buttons inside dialog"},
    {L"color_picker_parts_separated", L"layout",
        L"SV / hue / preview do not overlap each other"},
    {L"random_main_color_usable", L"theme",
        L"RandomAttractiveThemeColors mains stay in usable sat/lum band"},
    {L"build_custom_theme_keeps_colors", L"theme",
        L"BuildTheme preserves main/accent; classic banner on"},
    {L"apply_custom_theme_from_settings", L"theme",
        L"ApplyThemeFromSettings(useCustom) updates CurrentTheme"},
    {L"theme_catalog_count", L"theme",
        L"ThemeCatalog has kThemeCount presets; classic is id 0"},
};

bool MeasureTextWidth(const wchar_t* text, int fontDesign, int* outW) {
    if (!outW) return false;
    UiScaleInitFromPrimaryMonitor();
    HDC hdc = CreateCompatibleDC(nullptr);
    if (!hdc) return false;
    HFONT font = CreateFontW(UiFontHeight(fontDesign), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
    HGDIOBJ old = SelectObject(hdc, font);
    SIZE sz{};
    const BOOL ok = GetTextExtentPoint32W(hdc, text, static_cast<int>(wcslen(text)), &sz);
    SelectObject(hdc, old);
    DeleteObject(font);
    DeleteDC(hdc);
    if (!ok) return false;
    *outW = sz.cx;
    return true;
}

void CaseFontDesign() {
    const bool ok = kBodyFontDesign == 26
        && kTitleFontDesign == 26
        && kCloseFontDesign == 36;
    Emit(L"font_design_matches_settings", ok,
        ok ? L"" : L"expected body/title=26 close=36");
}

void CaseCustomInBounds() {
    const auto L = MakeCustomThemeLayout();
    const RECT parts[] = {
        L.close, L.mainLabel, L.accentLabel, L.mainSwatch, L.accentSwatch,
        L.pickMain, L.pickAccent, L.randomBtn, L.ok, L.cancel,
    };
    bool ok = true;
    std::wstring detail;
    for (const RECT& rc : parts) {
        // 关闭钮贴右边，pad=0；其它控件留 8px
        const int pad = (rc.right == kCustomDlgW) ? 0 : 8;
        if (!RectInsideDialog(rc, kCustomDlgW, kCustomDlgH, pad)) {
            ok = false;
            detail = L"clipped control ltrb="
                + std::to_wstring(rc.left) + L"," + std::to_wstring(rc.top) + L","
                + std::to_wstring(rc.right) + L"," + std::to_wstring(rc.bottom);
            break;
        }
    }
    // footer 允许贴边
    if (ok && !RectInsideDialog(L.footer, kCustomDlgW, kCustomDlgH, 0)) {
        ok = false;
        detail = L"footer out of bounds";
    }
    Emit(L"custom_layout_in_bounds", ok, detail.c_str());
}

void CaseCustomFooter() {
    const auto L = MakeCustomThemeLayout();
    const bool ok = L.ok.top >= L.footer.top
        && L.cancel.top >= L.footer.top
        && L.ok.bottom <= kCustomDlgH - 8
        && L.cancel.bottom <= kCustomDlgH - 8
        && L.ok.right <= L.cancel.left;
    Emit(L"custom_footer_buttons_visible", ok,
        ok ? L"" : L"OK/Cancel not fully in footer or overlap");
}

void CaseCustomButtonsNoOverlap() {
    const auto L = MakeCustomThemeLayout();
    const bool ok = !RectsOverlap(L.pickMain, L.pickAccent)
        && !RectsOverlap(L.pickAccent, L.randomBtn)
        && !RectsOverlap(L.pickMain, L.randomBtn)
        && !RectsOverlap(L.mainSwatch, L.accentSwatch);
    Emit(L"custom_action_buttons_no_overlap", ok, ok ? L"" : L"buttons overlap");
}

void CaseCustomLabelWidth() {
    int textW = 0;
    const bool measured = MeasureTextWidth(L"点缀色（标签/强调）", kBodyFontDesign, &textW);
    const auto L = MakeCustomThemeLayout();
    // 设计稿宽 → 物理像素：UiLen；再留 4px 余量
    UiScaleInitFromPrimaryMonitor();
    const int boxW = UiLen(RectW(L.accentLabel));
    const bool ok = measured && textW + 4 <= boxW;
    std::wstring detail = L"textW=" + std::to_wstring(textW)
        + L" boxW=" + std::to_wstring(boxW);
    Emit(L"custom_label_width_fits_zh", ok, detail.c_str());
}

void CaseColorPickerInBounds() {
    const auto L = MakeColorPickerLayout();
    const RECT parts[] = {
        L.close, L.sv, L.hue, L.preview, L.previewLabel, L.ok, L.cancel,
    };
    bool ok = true;
    std::wstring detail;
    for (const RECT& rc : parts) {
        const int pad = (rc.right == kColorDlgW || rc.left == 0) ? 0 : 8;
        if (!RectInsideDialog(rc, kColorDlgW, kColorDlgH, pad)) {
            ok = false;
            detail = L"clipped ltrb="
                + std::to_wstring(rc.left) + L"," + std::to_wstring(rc.top) + L","
                + std::to_wstring(rc.right) + L"," + std::to_wstring(rc.bottom);
            break;
        }
    }
    Emit(L"color_picker_layout_in_bounds", ok, detail.c_str());
}

void CaseColorPickerSeparated() {
    const auto L = MakeColorPickerLayout();
    const bool ok = !RectsOverlap(L.sv, L.hue)
        && !RectsOverlap(L.sv, L.preview)
        && !RectsOverlap(L.hue, L.preview)
        && L.ok.bottom <= kColorDlgH - 8
        && L.cancel.bottom <= kColorDlgH - 8;
    Emit(L"color_picker_parts_separated", ok, ok ? L"" : L"overlap or buttons clipped");
}

void CaseRandomUsable() {
    int bad = 0;
    for (int i = 0; i < 48; ++i) {
        COLORREF main = 0, accent = 0;
        RandomAttractiveThemeColors(main, accent);
        if (!MainColorLooksUsable(main)) ++bad;
    }
    const bool ok = bad <= 2;  // 允许极少数 HSL 随机边界
    Emit(L"random_main_color_usable", ok,
        (L"bad=" + std::to_wstring(bad) + L"/48").c_str());
}

void CaseBuildKeepsColors() {
    const COLORREF main = RGB(78, 148, 210);
    const COLORREF accent = RGB(235, 148, 40);
    const AppTheme t = BuildTheme(L"自定义", main, DeriveLightBg(main), accent, true);
    const bool ok = t.mainColor == main
        && t.accentColor == accent
        && t.bannerBg == RGB(255, 244, 138)
        && t.bannerTag == RGB(255, 174, 42);
    Emit(L"build_custom_theme_keeps_colors", ok, ok ? L"" : L"colors/banner mismatch");
}

void CaseApplyCustom() {
    AppSettings s = DefaultAppSettings();
    s.other.useCustomTheme = true;
    s.other.customMainColor = static_cast<int>(RGB(230, 145, 52) & 0xFFFFFF);
    s.other.customAccentColor = static_cast<int>(RGB(20, 168, 188) & 0xFFFFFF);
    ApplyThemeFromSettings(s);
    const AppTheme& cur = CurrentTheme();
    const bool ok = cur.mainColor == RGB(230, 145, 52)
        && cur.accentColor == RGB(20, 168, 188);
    // 恢复默认，避免污染同进程后续用例
    s.other.useCustomTheme = false;
    s.other.themeId = 0;
    ApplyThemeFromSettings(s);
    Emit(L"apply_custom_theme_from_settings", ok, ok ? L"" : L"CurrentTheme not updated");
}

void CaseCatalog() {
    const AppTheme* cat = ThemeCatalog();
    const bool ok = cat
        && kThemeCount == 7
        && cat[0].name && wcscmp(cat[0].name, L"经典绿橙") == 0
        && kCustomThemeComboIndex == 0;
    Emit(L"theme_catalog_count", ok, ok ? L"" : L"catalog/name/combo index mismatch");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--json") == 0) {
            selftest::gJson = true;
            selftest::InitUtf8Stdout();
        } else if (_wcsicmp(argv[i], L"--list") == 0) {
            listOnly = true;
            selftest::InitUtf8Stdout();
        } else if (_wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"-h") == 0) {
            std::fwprintf(stderr,
                L"  ThemeUiSelfTest.exe [--json] [--list] [--help]\n");
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"ThemeUiSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }
    if (!selftest::gJson) {
        std::fwprintf(stderr, L"=== ThemeUiSelfTest ===\n");
    }

    CaseFontDesign();
    CaseCustomInBounds();
    CaseCustomFooter();
    CaseCustomButtonsNoOverlap();
    CaseCustomLabelWidth();
    CaseColorPickerInBounds();
    CaseColorPickerSeparated();
    CaseRandomUsable();
    CaseBuildKeepsColors();
    CaseApplyCustom();
    CaseCatalog();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
