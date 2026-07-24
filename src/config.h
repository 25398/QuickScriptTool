#pragma once
// ──────────────────────────────────────────────────────────────────
// config.h — 全局颜色常量、布局尺寸和应用级配置常量定义
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

// ── 窗口尺寸 ──────────────────────────────────────────────────────
constexpr int kHomeWidth = 720;
constexpr int kHomeHeight = 540;
constexpr int kEditorBaseWidth = 1024;
constexpr int kEditorBaseHeight = 768;
constexpr int kEditorWidth = 1200;
constexpr int kEditorHeight = 1080;
constexpr int kTitleH = 38;
constexpr int kHomeNavH = 70;
constexpr int kHomeContentTop = kTitleH + kHomeNavH;
constexpr int kHomeFooterTop = 498;
constexpr int kHomeTabW = kHomeWidth / 4;
constexpr int kBottomH = 101;
constexpr int kListInnerPad = 1;
constexpr int kListX = 28;
constexpr int kColRemarkClient = 513;
constexpr int kColOpClient = 739;
constexpr int kColRemarkInList = kColRemarkClient - kListX;
constexpr int kColOpInList = kColOpClient - kListX;
constexpr int kRemarkEditW = 138;
constexpr int kRemarkEditH = 31;
constexpr int kColNoInList = 38 - kListX;
constexpr int kColActionInList = 110 - kListX;
constexpr int kListY = 240;
constexpr int kListW = 872;
constexpr int kListH = 934;
constexpr int kRowH = 48;
constexpr int kIndentStep = 21;
constexpr int kHomeCardX = 59;
constexpr int kHomeCardW = 600;
constexpr int kHomeCardH = 100;
constexpr int kHomeListY = 193;
constexpr int kHomeListBottom = 375;
constexpr int kHomeCardGap = 13;
constexpr int kHomeCardStep = kHomeCardH + kHomeCardGap;
constexpr int kHomeScrollW = 16;
constexpr int kEditorScrollW = 12;

// ── 固定颜色（不随主题变化） ──────────────────────────────────────
constexpr COLORREF kHoverGray = RGB(205, 205, 205);
constexpr COLORREF kScrollTrackGray = RGB(245, 245, 245);
constexpr COLORREF kScrollThumbGray = RGB(210, 210, 210);
constexpr COLORREF kComboScrollTrackGray = RGB(238, 238, 238);
constexpr COLORREF kComboScrollThumbGray = RGB(142, 142, 142);
constexpr COLORREF kComboPopupBorderGray = RGB(154, 154, 154);
constexpr COLORREF kText = RGB(28, 28, 28);
constexpr COLORREF kHint = RGB(165, 165, 165);
constexpr COLORREF kWhite = RGB(255, 255, 255);
constexpr COLORREF kCrosshairBlue = RGB(0, 120, 215);
constexpr COLORREF kPanel = RGB(246, 246, 246);
constexpr COLORREF kButtonDisabledText = RGB(235, 235, 235);
constexpr COLORREF kComboBorderGray = RGB(204, 204, 204);
constexpr COLORREF kComboMenuSelectText = RGB(255, 255, 255);
constexpr int kEditorPopupItemH = 32;
constexpr int kEditorPopupMaxHeight = 450;
constexpr int kEditorPopupMaxVisible = 10;
/// 主界面窗口外扩透明阴影（贴窗口一侧最深，向外渐浅）
constexpr int kWindowEdgeShadowSize = 4;
constexpr int kWindowEdgeShadowMaxAlpha = 48;
// 编辑界面下拉框基准尺寸（1024×768 设计稿，缩放后在 1200×1080 下约为 150×30 / 240×30）
constexpr int kEditorModeComboW = 128;
constexpr int kEditorModeComboH = 21;
constexpr int kEditorWmSelectMethodComboW = 250;
constexpr int kEditorWmSpecifyBtnW = 92;
// 编辑页顶部宏名称行 — 设计稿坐标（紧贴标题栏下方），布局时经 MulDiv 缩放
constexpr int kEditorChromeIndent = 10;
constexpr int kEditorMacroHeaderRowY = 40;
constexpr int kEditorMacroHeaderRowH = 26;
constexpr int kEditorMacroNameLabelX = 15 + kEditorChromeIndent;
constexpr int kEditorMacroNameLabelW = 66;
constexpr int kEditorMacroNameEditX = 83 + kEditorChromeIndent;
constexpr int kEditorMacroNameEditW = 690;
constexpr int kEditorListLabelX = 13 + kEditorChromeIndent;
// 动作列表工具栏（批量编辑等）— 位于宏名称/窗口模式行下方
constexpr int kEditorToolbarBtnY = 108;
constexpr int kEditorToolbarBtnH = 31;
constexpr int kEditorToolbarLabelY = 112;
constexpr int kEditorListColumnHeaderY = 148;
constexpr int kEditorActionComboY = 126;
// 窗口模式顶部两行：左缘与宏名称输入对齐，右缘止于动作列表区域
constexpr int kEditorWmContentRight = 776;
constexpr int kEditorWmHeaderGap = 8;
constexpr int kEditorWmNameEditW = 160;
constexpr int kEditorWmModeComboW = 100;
constexpr int kEditorWmModeLabelW = 40;
constexpr int kEditorWmSelectMethodLabelW = 100;
constexpr int kEditorWmRowGap = 8;
// 默认模式脱离时间行 — 紧贴宏名称行下方，左缘与宏名称标签对齐
constexpr int kEditorBreakoutRowY = kEditorMacroHeaderRowY + kEditorMacroHeaderRowH + kEditorWmRowGap;
constexpr int kEditorBreakoutLabelX = kEditorMacroNameLabelX;
constexpr int kEditorBreakoutLabelW = 68;
constexpr int kEditorBreakoutEditX = kEditorMacroNameEditX;
constexpr int kEditorBreakoutEditW = 80;
constexpr int kEditorWmTargetBrowseW = 50;
constexpr int kEditorWmTargetCrosshairW = 136;
constexpr int kEditorWmTargetBtnGap = 6;
constexpr int kEditorWmFakeFocusW = 72;  // 「聚焦」勾选，紧挨准星右侧
constexpr int kEditorActionComboW = 205;
constexpr int kEditorActionComboH = 21;
constexpr int kEditorComboRight = 995;
constexpr int kEditorPanelLeft = kEditorComboRight - kEditorActionComboW;
constexpr int kEditorParamComboY = 212;
constexpr int kEditorLabelGap = 8;
constexpr int kEditorLabelAboveComboH = 22;
constexpr int kEditorLabelAboveComboY = kEditorParamComboY - kEditorLabelGap - kEditorLabelAboveComboH;
constexpr COLORREF kGrayButton = RGB(240, 240, 240);
constexpr COLORREF kGrayButtonHover = RGB(220, 220, 220);
constexpr COLORREF kGrayButtonBorder = RGB(173, 173, 173);
constexpr COLORREF kGrayButtonText = RGB(51, 51, 51);

// ── 找图/文字识别面板公共尺寸（1024×768 设计稿，统一按宽比缩放）────
constexpr int kFindBtnW = 90;
constexpr int kFindBtnH = 30;
constexpr int kFindImageSize = 120;
constexpr int kFindImageSideBtnH = 26;
constexpr int kFindImageSideBtnGap = 4;
constexpr int kFindVGap = 8;
constexpr int kFindContentLeft = 785;           // 1200x1080 下对应 x=920
constexpr int kFindActionBtnX = 913;            // kFindContentLeft + kFindImageSize + 8
constexpr int kFindRegionLabelW = 64;
constexpr int kFindFollowLabelW = 64;
constexpr int kFindFollowComboW = 145;
constexpr int kFindActionBtnRight = 1003;       // kFindActionBtnX + kFindBtnW
constexpr int kFindEditW = 48;
constexpr int kFindRegionLabelGap = 8;
constexpr int kFindRegionBtnGap = 6;
constexpr int kFindSelectRegionX = 913;         // kFindActionBtnX
constexpr int kFindCoordLabelW = 22;
constexpr int kFindOffsetLabelW = 30;
constexpr int kFindCoordLabelEditGap = 4;
constexpr int kFindCoordPairGap = 10;
constexpr int kFindYEditX = 955;                // kFindActionBtnRight - kFindEditW
constexpr int kFindYLabelX = 929;               // kFindYEditX - kFindCoordLabelEditGap - kFindCoordLabelW
constexpr int kFindXEditX = 871;                // kFindYLabelX - kFindCoordPairGap - kFindEditW
constexpr int kFindXLabelX = 845;               // kFindXEditX - kFindCoordLabelEditGap - kFindCoordLabelW
constexpr int kFindOffsetYLabelX = 921;         // kFindYEditX - kFindCoordLabelEditGap - kFindOffsetLabelW
constexpr int kFindOffsetXEditX = 863;          // kFindOffsetYLabelX - kFindCoordPairGap - kFindEditW
constexpr int kFindOffsetXLabelX = 829;         // kFindOffsetXEditX - kFindCoordLabelEditGap - kFindOffsetLabelW
constexpr int kFindRegionRowY = 174;            // 与其他动作参数首行对齐
constexpr int kFindBtnStackGap = 8;
constexpr int kFindBlockW = 218;               // kFindImageSize + kFindContentGap + kFindBtnW
constexpr int kFindContentGap = 8;
constexpr int kFindSelectOffsetW = 155;
constexpr int kFindSelectOffsetLeft = 829;     // kFindOffsetXLabelX
constexpr int kFindSelectOffsetY = 552;
constexpr int kFindUntilFoundY = 578;
constexpr int kFindMatchVarLabelW = 108;
constexpr int kFindMatchVarEditX = 897;       // kFindContentLeft + kFindMatchVarLabelW + 4
constexpr int kFindMatchVarEditW = 80;
constexpr int kFindImageLabelY = 278;
constexpr int kFindImageRowY = 316;
constexpr int kFindMatchY = 448;
constexpr int kFindScaleY = 478;
constexpr int kFindFollowRowY = 508;
constexpr int kFindOffsetRowY = 546;

// ── 文字识别面板公共尺寸 ──
constexpr int kOcrDepToRegionGap = 4;
constexpr int kOcrDepRowY = 174;                // 与其他动作参数首行对齐
constexpr int kOcrToggleRowY = 208;             // 174 + kFindBtnH + kOcrDepToRegionGap
constexpr int kOcrRegionRowY = 238;             // 208 + 22 + kFindVGap（复选框行下方）
constexpr int kOcrResultVarEditW = 127;         // 218 - 91
constexpr int kOcrRegionByImageW = 140;
constexpr int kOcrDigitsOnlyX = 929;            // kFindContentLeft + kOcrRegionByImageW + 4
constexpr int kOcrDigitsOnlyW = 64;
constexpr int kOcrSearchVarLabelW = 36;
constexpr int kOcrVarComboGap = 4;
constexpr int kOcrVarInsertGap = 6;
constexpr int kOcrPanelRight = 1003;            // kFindContentLeft + 218
constexpr int kOcrSearchEditX = 825;            // kFindContentLeft + kOcrSearchVarLabelW + kOcrVarComboGap
constexpr int kOcrSearchEditW = 178;            // kOcrPanelRight - kOcrSearchEditX
constexpr int kOcrCompactBtnH = 26;
constexpr int kOcrTestBtnW = 56;
constexpr int kOcrInsertBtnW = 44;
constexpr int kOcrSearchLabelY = 347;
constexpr int kOcrFollowRowY = 445;
constexpr int kOcrOffsetRowY = 483;
constexpr int kOcrResultVarY = 513;

constexpr int kEditorRemarkY = 516;
constexpr int kEditorAddY = 552;

// 参数面板滚动区（设计稿 1024×768；1200×1080 下约为 left=920 top=225 right=1175 bottom=980）
constexpr int kParamScrollLeftDesign = 785;
constexpr int kParamScrollTopDesign = 168;
constexpr int kParamScrollRightDesign = 1003;
constexpr int kParamScrollBottomDesign = 697;
constexpr int kParamScrollEditorRightMarginDesign = 6;
constexpr int kParamScrollBarGapDesign = 7;
// 参数区输入框：扣除滚动条列与左右绿边绘制余量（设计稿像素）
constexpr int kParamFieldInsetDesign = 2;
constexpr int kParamScrollContentRightDesign =
    kParamScrollRightDesign - kParamScrollBarGapDesign - kEditorScrollW;
constexpr int kParamPanelLeftDesign = kParamScrollLeftDesign + kParamFieldInsetDesign;
constexpr int kParamFieldWidth =
    kParamScrollContentRightDesign - kParamPanelLeftDesign - kParamFieldInsetDesign;

// ── 控件尺寸 ──────────────────────────────────────────────────────
constexpr int kComboArrowW = 24;
constexpr int kComboItemH = 38;
constexpr int kComboScrollW = 10;
constexpr int kCloseBtnW = 46;
constexpr int kTitleBtnW = 46;
constexpr int kBatchCheckboxSize = 18;
constexpr int kExpandToggleWidth = 14;
constexpr int kExpandToggleSlot = 16;
constexpr int kBatchItemGap = 4;
constexpr int kDragThreshold = 5;
constexpr BYTE kCloseHoverAlpha = 55;

// ── 消息与ID ──────────────────────────────────────────────────────
constexpr UINT WM_RUN_DONE = WM_APP + 11;
constexpr UINT WM_TRAY = WM_APP + 12;
constexpr UINT WM_FIND_TEST_DONE = WM_APP + 13;
constexpr UINT WM_GLOBAL_HOTKEY_DETECTED = WM_APP + 14;
constexpr UINT WM_OPEN_AGENT_DIALOG = WM_APP + 15;
constexpr UINT WM_AGENT_SCRIPT_LIBRARY_CHANGED = WM_APP + 20;
constexpr UINT WM_EDITOR_PARAM_CHROME = WM_APP + 21;
constexpr UINT WM_APP_PROMPT = WM_APP + 22;
constexpr UINT WM_OCR_SUBPANEL_REFRESH = WM_APP + 23;
constexpr UINT WM_WINDOW_MODE_TEST_DONE = WM_APP + 24;
constexpr UINT WM_APP_UI_SCALE_SYNC = WM_APP + 25;
constexpr UINT WM_APP_UI_LAYOUT_REFRESH = WM_APP + 26;
constexpr UINT WM_APP_BREAKOUT_UI = WM_APP + 27;
constexpr UINT WM_APP_RESTORE_INSTANCE = WM_APP + 28;
constexpr UINT WM_APP_QUIT_APP = WM_APP + 29;
constexpr UINT WM_APP_ENSURE_TRAY = WM_APP + 30;
constexpr UINT WM_APP_EDITOR_FINISH_OPEN = WM_APP + 31;
constexpr UINT WM_APP_HOME_REFRESH_LISTS = WM_APP + 32;
constexpr UINT WM_APP_OPTIMIZE_LOAD = WM_APP + 33;
constexpr UINT WM_APP_OPTIMIZE_LOADED = WM_APP + 34;
constexpr UINT WM_APP_OPTIMIZE_PRERENDER = WM_APP + 35;
constexpr UINT WM_APP_EDITOR_PARSE_MORE = WM_APP + 36;
constexpr UINT WM_APP_EXT_RUN_SCRIPT = WM_APP + 37;
constexpr UINT WM_APP_EXT_STOP_SCRIPT = WM_APP + 38;
constexpr UINT WM_SETTINGS_EXTERNAL_SYNC = WM_APP + 19;

// ── 主题感知颜色（运行时随 CurrentTheme() 变化） ──────────────────
#include "app_theme.h"
#define kMainGreen (quickscript::CurrentTheme().mainColor)
#define kDarkGreen (quickscript::CurrentTheme().darkColor)
#define kCardGreen (quickscript::CurrentTheme().cardColor)
#define kCardHoverGreen (quickscript::CurrentTheme().cardHoverColor)
#define kButtonGreen (quickscript::CurrentTheme().buttonColor)
#define kButtonGreenHover (quickscript::CurrentTheme().buttonHoverColor)
#define kOrange (quickscript::CurrentTheme().accentColor)
#define kLineGreen (quickscript::CurrentTheme().lineColor)
#define kIndexGreen (quickscript::CurrentTheme().indexColor)
#define kButtonDisabledGreen (quickscript::CurrentTheme().buttonDisabled)
#define kBatchSelectedRow (quickscript::CurrentTheme().batchSelectedRow)
#define kComboHoverGreen (quickscript::CurrentTheme().comboHover)
#define kComboMenuHoverBlue (quickscript::CurrentTheme().comboMenuHover)
#define kComboMenuSelectBlue (quickscript::CurrentTheme().comboMenuSelect)
#define kTabGradientStart (quickscript::CurrentTheme().tabGradientStart)
#define kTabGradientEnd (quickscript::CurrentTheme().tabGradientEnd)
#define kNavStripGreen (quickscript::CurrentTheme().navStripColor)
#define kTabActiveGreen (quickscript::CurrentTheme().tabActiveColor)
#define kCloseHover (quickscript::CurrentTheme().closeHover)
#define kCreateYellow (quickscript::CurrentTheme().bannerBg)
#define kSelectedYellow (quickscript::CurrentTheme().selectedTagBg)
#define kBannerTag (quickscript::CurrentTheme().bannerTag)
#define kBannerText (quickscript::CurrentTheme().bannerText)
#define kSecondaryText (quickscript::CurrentTheme().secondaryText)
#define kFooterHint (quickscript::CurrentTheme().footerHint)
#define kHomeScrollTrack (quickscript::CurrentTheme().scrollTrack)
#define kHomeScrollThumb (quickscript::CurrentTheme().scrollThumb)
#define kPromptDialogBg (quickscript::CurrentTheme().promptBg)
#define kPromptDialogBorder (quickscript::CurrentTheme().promptBorder)
#define kPromptText (quickscript::CurrentTheme().promptText)
#define kPromptOkFill (quickscript::CurrentTheme().promptOkFill)
#define kPromptOkHover (quickscript::CurrentTheme().promptOkHover)
#define kPromptOkText (quickscript::CurrentTheme().promptOkText)
#define kPromptCancelBorder (quickscript::CurrentTheme().promptCancelBorder)
constexpr int HOTKEY_COMMON_ID = 701;
constexpr int HOTKEY_GLOBAL_ID = 702;
constexpr int HOTKEY_SCRIPT_BASE = 800;
constexpr BYTE kUiFontQuality = CLEARTYPE_NATURAL_QUALITY;
constexpr UINT kHoverTimerId = 9001;
constexpr UINT kQuickInputTipTimerId = 9002;
constexpr UINT kScheduledTaskTimerId = 9003;
constexpr UINT kDisplaySyncTimerId = 9005;
constexpr UINT kWindowModePreviewTimerId = 9004;
constexpr UINT kBreakoutReturnTimerId = 9006;
constexpr int kQuickInputTipDelayMs = 500;
