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

// ── 找图面板布局（1024×768 设计稿，统一按宽比缩放以保持 120×120 等比例）────
constexpr int kFindZoneLeft = 780;
constexpr int kFindZoneRight = kEditorBaseWidth - 10;
constexpr int kFindPanelRightMargin = 10;
constexpr int kFindBtnW = 90;
constexpr int kFindBtnH = 30;
constexpr int kFindImageSize = 120;
constexpr int kFindContentGap = 8;
constexpr int kFindVGap = 8;
constexpr int kFindBlockW = kFindImageSize + kFindContentGap + kFindBtnW;
constexpr int kFindContentLeft = kFindZoneLeft + (kFindZoneRight - kFindZoneLeft - kFindBlockW) / 2;
constexpr int kFindActionBtnX = kFindContentLeft + kFindImageSize + kFindContentGap;
constexpr int kFindPanelRight = kFindZoneRight;
constexpr int kFindRegionLabelW = 64;
constexpr int kFindFollowLabelW = 64;
constexpr int kFindFollowComboW = 145;
constexpr int kFindSelectOffsetW = 155;
constexpr int kFindActionBtnRight = kFindActionBtnX + kFindBtnW;
constexpr int kFindEditW = 48;
constexpr int kFindRegionLabelGap = 8;
constexpr int kFindRegionBtnGap = 6;
constexpr int kFindSelectRegionX = kFindActionBtnX;
constexpr int kFindCoordLabelW = 22;
constexpr int kFindOffsetLabelW = 30;
constexpr int kFindCoordLabelEditGap = 4;
constexpr int kFindCoordPairGap = 10;
constexpr int kFindYEditRight = kFindActionBtnRight;
constexpr int kFindYEditX = kFindYEditRight - kFindEditW;
constexpr int kFindYLabelX = kFindYEditX - kFindCoordLabelEditGap - kFindCoordLabelW;
constexpr int kFindXEditX = kFindYLabelX - kFindCoordPairGap - kFindEditW;
constexpr int kFindXLabelX = kFindXEditX - kFindCoordLabelEditGap - kFindCoordLabelW;
constexpr int kFindOffsetYLabelX = kFindYEditX - kFindCoordLabelEditGap - kFindOffsetLabelW;
constexpr int kFindOffsetXEditX = kFindOffsetYLabelX - kFindCoordPairGap - kFindEditW;
constexpr int kFindOffsetXLabelX = kFindOffsetXEditX - kFindCoordLabelEditGap - kFindOffsetLabelW;
constexpr int kFindSelectOffsetLeft = kFindOffsetXLabelX;
constexpr int kFindPanelLeft = kFindContentLeft;
constexpr int kOcrDepRowY = 118 + kEditorLabelAboveComboH + kEditorLabelGap + kEditorActionComboH + kEditorLabelGap;
constexpr int kFindRegionRowY = 180;
constexpr int kFindCoordRow1Y = kFindRegionRowY + kFindBtnH + kFindVGap;
constexpr int kFindCoordRow2Y = kFindCoordRow1Y + 22 + kFindVGap;
constexpr int kFindBtnStackGap = 8;
constexpr int kFindImageLabelY = kFindCoordRow2Y + 22 + kFindVGap;
constexpr int kFindImageRowY = kFindImageLabelY + kFindBtnH + kFindVGap;
constexpr int kFindMatchY = kFindImageRowY + kFindImageSize + 12;
constexpr int kFindScaleY = kFindMatchY + 22 + kFindVGap;
constexpr int kFindFollowRowY = kFindScaleY + 22 + kFindVGap;
constexpr int kFindOffsetRowY = kFindFollowRowY + kFindBtnH + kFindVGap;
constexpr int kFindSelectOffsetY = kFindOffsetRowY + 22 + kFindVGap;
constexpr int kFindUntilFoundY = kFindSelectOffsetY + kFindBtnH + kFindVGap;
constexpr int kFindMatchVarLabelW = 108;
constexpr int kFindMatchVarEditX = kFindContentLeft + kFindMatchVarLabelW + 1;
constexpr int kFindMatchVarEditW = 80;

// ── 文字识别面板布局（运行时由 RefreshOcrSubPanel 自 dep 行起堆叠排列）──
constexpr int kOcrDepToRegionGap = 4;
constexpr int kOcrRegionRowY = kOcrDepRowY + kFindBtnH + kOcrDepToRegionGap;
constexpr int kOcrCoordRow1Y = kOcrRegionRowY + kFindBtnH + kFindVGap;
constexpr int kOcrCoordRow2Y = kOcrCoordRow1Y + 22 + kFindVGap;
constexpr int kOcrResultModeY = kOcrCoordRow2Y + 22 + kFindVGap;
constexpr int kOcrResultVarEditW = kFindBlockW - 91;
constexpr int kOcrSearchLabelY = kOcrResultModeY + kFindBtnH + kFindVGap;
constexpr int kOcrSearchEditY = kOcrSearchLabelY + 22 + 4;
constexpr int kOcrSearchVarY = kOcrSearchEditY + 28 + kFindVGap;
constexpr int kOcrFollowRowY = kOcrSearchVarY + 28 + kFindVGap;
constexpr int kOcrFollowRowYCompact = kOcrResultModeY + kFindBtnH + kFindVGap;
constexpr int kOcrOffsetRowY = kOcrFollowRowY + kFindBtnH + kFindVGap;
constexpr int kOcrOffsetRowYCompact = kOcrFollowRowYCompact + kFindBtnH + kFindVGap;
constexpr int kOcrResultVarY = kOcrOffsetRowY + 22 + kFindVGap;
constexpr int kOcrResultVarYCompact = kOcrOffsetRowYCompact + 22 + kFindVGap;
constexpr int kOcrUntilFoundY = kOcrResultVarY + 22 + kFindVGap;
constexpr int kOcrTestBtnY = kOcrUntilFoundY + 22 + kFindVGap;
constexpr int kOcrTestBtnYCompact = kOcrResultVarYCompact + 22 + kFindVGap;
// 文字查找：测试与「选择偏移位置」同一行，偏移按钮在测试右侧留空，右缘对齐内容块
constexpr int kOcrTestToOffsetGap = 12;
constexpr int kOcrTestRowOffsetBtnX = kFindContentLeft + kFindBtnW + kOcrTestToOffsetGap;
constexpr int kOcrTestRowOffsetBtnW = kFindContentLeft + kFindBlockW - kOcrTestRowOffsetBtnX;
constexpr int kOcrRegionByImageW = 140;
constexpr int kOcrDigitsOnlyX = kFindContentLeft + kOcrRegionByImageW + 4;
constexpr int kOcrDigitsOnlyW = 64;
constexpr int kOcrSearchVarLabelW = 36;
constexpr int kOcrVarComboGap = 4;
constexpr int kOcrVarInsertGap = 6;
constexpr int kOcrPanelRight = kFindContentLeft + kFindBlockW;
constexpr int kOcrSearchEditX = kFindContentLeft + kOcrSearchVarLabelW + kOcrVarComboGap;
constexpr int kOcrSearchEditW = kOcrPanelRight - kOcrSearchEditX;
constexpr int kOcrCompactBtnH = 26;
constexpr int kOcrTestBtnW = 56;
constexpr int kOcrInsertBtnW = 44;

constexpr int kEditorRemarkY = 516;
constexpr int kEditorAddY = 552;
constexpr int kEditorFooterGap = 32;
constexpr int kEditorParamHintH = 40;

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
constexpr int HOTKEY_COMMON_ID = 701;
constexpr int HOTKEY_GLOBAL_ID = 702;
constexpr int HOTKEY_SCRIPT_BASE = 800;
constexpr BYTE kUiFontQuality = CLEARTYPE_NATURAL_QUALITY;
constexpr UINT kHoverTimerId = 9001;
constexpr UINT kQuickInputTipTimerId = 9002;
constexpr UINT kScheduledTaskTimerId = 9003;
constexpr int kQuickInputTipDelayMs = 500;
