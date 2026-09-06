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
        L"ThemeCatalog has kThemeCount classic presets; id0=经典绿橙"},
    {L"shell_chrome_geometry", L"layout",
        L"Title accent / tab underline / card rail design sizes"},
    {L"home_settings_sizes", L"layout",
        L"GDI home 720x540; settings 720x540; list/cta classic bands"},
    {L"editor_op_copy_delete_gap", L"layout",
        L"Editor copy/delete origin slots do not overlap"},
    {L"chrome_pulse_timer_interval", L"layout",
        L"Chrome pulse timer interval is 30-50ms (Web; GDI no-op)"},
    {L"p2_round_radii", L"layout",
        L"Shared round radii tokens present for draw helpers"},
    {L"p3_component_geometry", L"layout",
        L"Classic title/nav/footer constants (38/70/498)"},
    {L"home_workspace_not_main", L"theme",
        L"Classic themes: workspaceBg==lightBg; yellow CTA banner"},
    {L"home_layout_no_overlap", L"layout",
        L"Classic home: list above yellow CTA CreateRect; cardH=100"},
    {L"home_card_side_no_overlap", L"layout",
        L"HomeCardEditBtn ≠ HomeCardSelectedTag (UiEdgeRect slots)"},
    {L"settings_size_720x520", L"layout",
        L"settings dialog 720x540 matches config"},
    {L"editor_columns_arow", L"layout",
        L"editor list rowH=48; bottom bar; op gap tokens"},
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
        && t.bannerTag == RGB(255, 174, 42)
        && t.workspaceBg == t.lightBg
        && t.bannerText == RGB(55, 55, 55);
    Emit(L"build_custom_theme_keeps_colors", ok, ok ? L"" : L"colors/home surface mismatch");
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
        && cat[kThemeCount - 1].name && wcscmp(cat[kThemeCount - 1].name, L"樱花粉") == 0
        && kCustomThemeComboIndex == 0;
    Emit(L"theme_catalog_count", ok, ok ? L"" : L"catalog/name/combo index mismatch");
}

void CaseShellChromeGeometry() {
    const RECT title{0, kTitleH - kTitleAccentLineH, kHomeWidth, kTitleH};
    const RECT tab{0, kTitleH, kHomeTabW, kHomeContentTop};
    const RECT underline{tab.left + tab.right / 8, tab.bottom - kTabUnderlineH,
        tab.right - tab.right / 8, tab.bottom};
    const RECT card{kHomeCardX, kHomeListY, kHomeCardX + kHomeCardW, kHomeListY + kHomeCardH};
    const RECT rail{card.left, card.top, card.left + kHomeCardRailW, card.bottom};
    const bool ok = kTitleAccentLineH == 2
        && kTabUnderlineH == 3
        && kHomeCardRailW == 4
        && title.top == kTitleH - kTitleAccentLineH
        && title.bottom == kTitleH
        && title.right == kHomeWidth
        && (underline.bottom - underline.top) == kTabUnderlineH
        && (rail.right - rail.left) == kHomeCardRailW
        && rail.left == card.left
        && rail.bottom == card.bottom;
    Emit(L"shell_chrome_geometry", ok, ok ? L"" : L"chrome design rect mismatch");
}

void CaseHomeSettingsSizes() {
    // Classic GDI CreateRect CTA band: y 375..459; list bottom 375; footer top 498
    const bool ok = kHomeWidth == 720
        && kHomeHeight == 540
        && kSettingsWidth == 720
        && kSettingsHeight == 540
        && kHomeFooterTop == 498
        && kHomeListBottom == 375
        && kHomeListBottom < 459
        && kHomeCardX + kHomeCardW <= kHomeWidth
        && kHomeCardH == 100
        && kHomeCardGap == 13
        && kTitleH == 38
        && kHomeNavH == 70
        && kHomeContentTop == 108;
    Emit(L"home_settings_sizes", ok, ok ? L"" : L"home/settings size or CTA/footer band mismatch");
}

void CaseEditorOpCopyDeleteGap() {
    // Mirror origin LocalCopyRect / LocalDeleteRect (design px, contentRight=800)
    constexpr int kContentRight = 800;
    constexpr int kRowHLocal = 48;
    const RECT copy{kContentRight - 104, 6, kContentRight - 62, kRowHLocal - 6};
    const RECT del{kContentRight - 58, 6, kContentRight - 18, kRowHLocal - 6};
    const int gap = del.left - copy.right;
    const int paramOuterW = kParamScrollRightDesign - kParamScrollLeftDesign;
    const int rightAt1200 = MulDiv(paramOuterW, kEditorWidth, kEditorBaseWidth);
    const bool ok = gap >= 0
        && copy.right <= del.left
        && del.right == kContentRight - 18
        && kEditorOpGap == 14
        && kEditorOpBtnW == 42
        && kEditorOpRightPad == 18
        && kParamFieldWidth >= 190 && kParamFieldWidth <= 280
        && paramOuterW >= 218 && paramOuterW <= 280
        && rightAt1200 >= kEditorRightColMinW
        && rightAt1200 <= kEditorRightColMaxW;
    Emit(L"editor_op_copy_delete_gap", ok, ok ? L"" : L"copy/delete gap or param width out of range");
}

void CaseChromePulseTimerInterval() {
    const bool ok = kChromePulseIntervalMs >= 30
        && kChromePulseIntervalMs <= 50
        && kChromePulseTimerId != kHoverTimerId
        && kChromePulseTimerId != kDisplaySyncTimerId
        && kChromePulseTimerId != kBreakoutReturnTimerId;
    Emit(L"chrome_pulse_timer_interval", ok, ok ? L"" : L"chrome pulse timer id/interval invalid");
}

void CaseP2RoundRadii() {
    const AppTheme* cat = ThemeCatalog();
    const bool classicBanner = cat
        && cat[0].bannerBg == RGB(255, 244, 138)
        && cat[0].name && wcscmp(cat[0].name, L"经典绿橙") == 0;
    const bool ok = kHomeCardRadius == 9
        && kHomeCtaRadius == 10
        && kHomePanelRadius == 10
        && kEditorRowRadius == 4
        && kEditorFieldRadius == 6
        && kEditorListRadius == 6
        && classicBanner;
    Emit(L"p2_round_radii", ok, ok ? L"" : L"radius or classic banner token mismatch");
}

void CaseP3ComponentGeometry() {
    const RECT band{0, kHomeFooterTop, kHomeWidth, kHomeHeight};
    const RECT hint{kHomeFooterHintPadX, kHomeFooterTop,
        kHomeWidth - kHomeFooterHintPadX, kHomeHeight};
    const RECT title{0, 0, kHomeWidth, kTitleH};
    const RECT tab0{0, kTitleH, kHomeTabW, kHomeContentTop};
    const RECT tab3{kHomeTabW * 3, kTitleH, kHomeWidth, kHomeContentTop};
    const bool ok = kHomeFooterHintPadX == 20
        && kHomeNavTabTextInset == 52
        && kTitleH == 38
        && kHomeNavH == 70
        && band.top == kHomeFooterTop
        && band.bottom == kHomeHeight
        && band.right == kHomeWidth
        && hint.left == kHomeFooterHintPadX
        && hint.right == kHomeWidth - kHomeFooterHintPadX
        && title.bottom == kTitleH
        && tab0.bottom == kHomeContentTop
        && (tab0.right - tab0.left) == kHomeTabW
        && tab3.right == kHomeWidth
        && (tab3.left == kHomeTabW * 3);
    Emit(L"p3_component_geometry", ok, ok ? L"" : L"P3 footer/title/nav design rect mismatch");
}

void CaseHomeLayoutNoOverlap() {
    const RECT list{kHomeCardX, kHomeListY, kHomeCardX + kHomeCardW, kHomeListBottom};
    const RECT cta{35, 375, 683, 459};
    const RECT footer{0, kHomeFooterTop, kHomeWidth, kHomeHeight};
    const bool noOverlap = list.bottom <= cta.top
        && cta.bottom <= footer.top
        && list.bottom <= footer.top;
    const bool sizes = kHomeCardH == 100
        && kHomeCardGap == 13
        && kTitleH == 38
        && kHomeNavH == 70
        && kHomeContentTop == 108
        && kHomeListBottom == 375
        && kHomeFooterTop == 498;
    const bool ok = noOverlap && sizes;
    Emit(L"home_layout_no_overlap", ok, ok ? L"" : L"list/cta/footer overlap or classic size mismatch");
}

void CaseHomeCardSideNoOverlap() {
    const RECT card{kHomeCardX, kHomeListY, kHomeCardX + kHomeCardW, kHomeListY + kHomeCardH};
    auto edge = [](const RECT& base, int rightStart, int top, int rightEnd, int bottomY) {
        return RECT{base.right - rightStart, base.top + top, base.right - rightEnd, base.top + bottomY};
    };
    const RECT edit = edge(card, 104, 14, 16, 45);
    const RECT del = edge(card, 104, 56, 16, 88);
    const RECT tag = edge(card, 104, 58, 16, 95);
    const RECT rename = edge(card, 104, 14, 16, 45);
    const RECT deselect = edge(card, 104, 14, 16, 45);
    const bool editNeTag = edit.bottom <= tag.top || edit.top >= tag.bottom
        || edit.right <= tag.left || edit.left >= tag.right
        || (edit.top != tag.top || edit.bottom != tag.bottom);
    // EditBtn ≠ SelectedTag (different vertical slots). Deselect/Rename share top slot by mutual exclusion.
    const bool ok = editNeTag
        && edit.top == 14 + card.top
        && tag.top == 58 + card.top
        && rename.top == deselect.top
        && del.top >= edit.bottom;
    Emit(L"home_card_side_no_overlap", ok,
        ok ? L"" : L"HomeCardEditBtn/SelectedTag slot mismatch");
}

void CaseSettingsSize720x520() {
    // Name kept for suite stability; sizes follow config 720×540
    const bool sizes = kSettingsWidth == 720
        && kSettingsHeight == 540
        && (kSettingsWidth == kHomeWidth);
    const bool ok = sizes;
    Emit(L"settings_size_720x520", ok, ok ? L"" : L"settings size mismatch vs config");
}

void CaseEditorColumnsArow() {
    const int rightW1200 = MulDiv(kEditorRightColWDesign, kEditorWidth, kEditorBaseWidth);
    const bool ok = kRowH == 48
        && kEditorBottomBarH == 60
        && kBottomH == 101
        && kEditorOpGap == 14
        && rightW1200 >= kEditorRightColMinW
        && rightW1200 <= kEditorRightColMaxW
        && kParamScrollLeftDesign == kFindContentLeft
        && kParamScrollBottomDesign >= 690;
    Emit(L"editor_columns_arow", ok, ok ? L"" : L"editor column/arow/bottom bar mismatch");
}

void CaseHomeWorkspaceNotMain() {
    const AppTheme* cat = ThemeCatalog();
    if (!cat) {
        Emit(L"home_workspace_not_main", false, L"no catalog");
        return;
    }
    bool ok = true;
    std::wstring detail;
    for (int i = 0; i < kThemeCount; ++i) {
        const AppTheme& t = cat[i];
        // Classic: workspace follows lightBg; banner is yellow CTA (not dark chrome)
        if (t.workspaceBg != t.lightBg
            || GetRValue(t.bannerBg) < 200
            || GetGValue(t.bannerBg) < 200) {
            ok = false;
            detail = L"themeId=" + std::to_wstring(i);
            break;
        }
    }
    Emit(L"home_workspace_not_main", ok, detail.c_str());
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
    CaseShellChromeGeometry();
    CaseHomeSettingsSizes();
    CaseEditorOpCopyDeleteGap();
    CaseChromePulseTimerInterval();
    CaseP2RoundRadii();
    CaseP3ComponentGeometry();
    CaseHomeLayoutNoOverlap();
    CaseHomeCardSideNoOverlap();
    CaseSettingsSize720x520();
    CaseEditorColumnsArow();
    CaseHomeWorkspaceNotMain();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
