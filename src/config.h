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
constexpr int kListY = 228;
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

// ── 品牌颜色 ──────────────────────────────────────────────────────
constexpr COLORREF kMainGreen = RGB(64, 168, 99);
constexpr COLORREF kDarkGreen = RGB(54, 141, 82);
constexpr COLORREF kCardGreen = RGB(67, 161, 94);
constexpr COLORREF kCardHoverGreen = RGB(58, 148, 87);
constexpr COLORREF kButtonGreen = RGB(70, 185, 111);
constexpr COLORREF kButtonGreenHover = RGB(54, 153, 88);
constexpr COLORREF kSelectedYellow = RGB(255, 241, 122);
constexpr COLORREF kCreateYellow = RGB(255, 244, 138);
constexpr COLORREF kOrange = RGB(255, 154, 72);
constexpr COLORREF kHoverGray = RGB(205, 205, 205);
constexpr COLORREF kScrollTrackGray = RGB(245, 245, 245);
constexpr COLORREF kScrollThumbGray = RGB(210, 210, 210);
constexpr COLORREF kComboScrollTrackGray = RGB(238, 238, 238);
constexpr COLORREF kComboScrollThumbGray = RGB(142, 142, 142);
constexpr COLORREF kComboPopupBorderGray = RGB(154, 154, 154);
constexpr COLORREF kLineGreen = RGB(197, 220, 205);
constexpr COLORREF kText = RGB(28, 28, 28);
constexpr COLORREF kHint = RGB(165, 165, 165);
constexpr COLORREF kIndexGreen = RGB(75, 166, 103);
constexpr COLORREF kWhite = RGB(255, 255, 255);
constexpr COLORREF kCrosshairBlue = RGB(0, 120, 215);
constexpr COLORREF kPanel = RGB(246, 246, 246);
constexpr COLORREF kButtonDisabledGreen = RGB(190, 205, 194);
constexpr COLORREF kButtonDisabledText = RGB(235, 235, 235);
constexpr COLORREF kBatchSelectedRow = RGB(232, 245, 238);
constexpr COLORREF kComboBorderGray = RGB(204, 204, 204);
constexpr COLORREF kComboMenuHoverBlue = RGB(229, 243, 255);
constexpr COLORREF kComboMenuSelectBlue = RGB(0, 102, 204);
constexpr COLORREF kComboMenuSelectText = RGB(255, 255, 255);
constexpr int kEditorPopupItemH = 32;
constexpr int kEditorPopupMaxHeight = 450;
constexpr int kEditorPopupMaxVisible = 10;
// 编辑界面下拉框基准尺寸（1024×768 设计稿，缩放后在 1200×1080 下约为 150×30 / 240×30）
constexpr int kEditorModeComboW = 128;
constexpr int kEditorModeComboH = 21;
constexpr int kEditorActionComboW = 205;
constexpr int kEditorActionComboH = 21;
constexpr int kEditorComboRight = 995;
constexpr int kEditorPanelLeft = kEditorComboRight - kEditorActionComboW;
constexpr int kEditorParamComboY = 204;
constexpr int kEditorLabelGap = 8;
constexpr int kEditorLabelAboveComboH = 22;
constexpr int kEditorLabelAboveComboY = kEditorParamComboY - kEditorLabelGap - kEditorLabelAboveComboH;
constexpr COLORREF kComboHoverGreen = RGB(232, 248, 239);
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
constexpr int kParamScrollTopDesign = 160;
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
constexpr UINT WM_SETTINGS_EXTERNAL_SYNC = WM_APP + 19;
constexpr int HOTKEY_COMMON_ID = 701;
constexpr int HOTKEY_GLOBAL_ID = 702;
constexpr int HOTKEY_SCRIPT_BASE = 800;
constexpr BYTE kUiFontQuality = CLEARTYPE_NATURAL_QUALITY;
constexpr UINT kHoverTimerId = 9001;
constexpr UINT kQuickInputTipTimerId = 9002;
constexpr UINT kScheduledTaskTimerId = 9003;
constexpr int kQuickInputTipDelayMs = 500;
