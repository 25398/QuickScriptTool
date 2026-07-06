#pragma once
// ──────────────────────────────────────────────────────────────────
// main_window.h — 主窗口类声明
// 提供 MainWindow 类的完整声明，供其他编译单元引用
// ──────────────────────────────────────────────────────────────────

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "action_tree.h"
#include "action_utils.h"
#include "app_settings.h"
#include "app_settings_store.h"
#include "config.h"
#include "controls.h"
#include "drawing.h"
#include "hotkey_dialog.h"
#include "image_match.h"
#include "macro_variables.h"
#include "macro_debug_window.h"
#include "modern_edit.h"
#include "main_features.h"
#include "popup_combo.h"
#include "crosshair_drag.h"
#include "ocr_install_dialog.h"
#include "ocr_engine.h"
#include "process_utils.h"
#include "recorder.h"
#include "recording_optimize_dialog.h"
#include "scheduled_task_dialog.h"
#include "scheduled_task_scheduler.h"
#include "scheduled_task_ui.h"
#include "settings_dialog.h"
#include "tray_menu.h"
#include "match_overlay.h"
#include "ocr_overlay.h"
#include "screenshot_overlay.h"
#include "editor_dropdown.h"
#include "script_types.h"
#include "script_io.h"
#include "script_action_builder.h"
#include "utils.h"
#include "agent_dialog.h"
#include "agent_ui_notify.h"
#include "ai_action_service.h"
#include "agent_ai_actions.h"
#include "ai_action_runtime.h"
#include "ui_component.h"
#include "editor_param_layout.h"

extern HINSTANCE g_instance;

// ── Global-hotkey low-level hook (fallback when RegisterHotKey is unavailable) ──
inline HHOOK ghHotkeyKbHook = nullptr;
inline HHOOK ghHotkeyMouseHook = nullptr;
inline HWND ghHotkeyHwnd = nullptr;
inline bool ghHotkeyPending = false;
inline UINT ghHotkeyVk = 0;
inline UINT ghHotkeyMods = 0;
inline bool ghHotkeyEnabled = false;

inline bool CheckHotkeyModifiers(UINT required) {
    const bool alt   = (GetAsyncKeyState(VK_LMENU)    & 0x8000) || (GetAsyncKeyState(VK_RMENU)    & 0x8000);
    const bool ctrl  = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) || (GetAsyncKeyState(VK_RCONTROL) & 0x8000);
    const bool shift = (GetAsyncKeyState(VK_LSHIFT)   & 0x8000) || (GetAsyncKeyState(VK_RSHIFT)   & 0x8000);
    const bool win   = (GetAsyncKeyState(VK_LWIN)     & 0x8000) || (GetAsyncKeyState(VK_RWIN)     & 0x8000);
    if (required == 0) return !alt && !ctrl && !shift && !win;
    if ((required & MOD_ALT)     && !alt)   return false;
    if ((required & MOD_CONTROL) && !ctrl)  return false;
    if ((required & MOD_SHIFT)   && !shift) return false;
    if ((required & MOD_WIN)     && !win)   return false;
    return true;
}

inline bool IsMouseVk(UINT vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON
        || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

inline LRESULT CALLBACK HotkeyKbProc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && ghHotkeyEnabled && !ghHotkeyPending && !IsMouseVk(ghHotkeyVk)) {
        const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        const bool up   = (wp == WM_KEYUP   || wp == WM_SYSKEYUP);
        auto* ks = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        if (down && ks->vkCode == ghHotkeyVk && CheckHotkeyModifiers(ghHotkeyMods)) {
            ghHotkeyPending = true;
            PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, 0, 0);
        }
        if (up && ks->vkCode == ghHotkeyVk)
            ghHotkeyPending = false;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

inline LRESULT CALLBACK HotkeyMouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && ghHotkeyEnabled && !ghHotkeyPending && IsMouseVk(ghHotkeyVk)) {
        bool down = false, up = false; UINT btnVk = 0;
        if      (wp == WM_LBUTTONDOWN) { down = true; btnVk = VK_LBUTTON; }
        else if (wp == WM_LBUTTONUP)   { up   = true; btnVk = VK_LBUTTON; }
        else if (wp == WM_RBUTTONDOWN) { down = true; btnVk = VK_RBUTTON; }
        else if (wp == WM_RBUTTONUP)   { up   = true; btnVk = VK_RBUTTON; }
        else if (wp == WM_MBUTTONDOWN) { down = true; btnVk = VK_MBUTTON; }
        else if (wp == WM_MBUTTONUP)   { up   = true; btnVk = VK_MBUTTON; }
        else if (wp == WM_XBUTTONDOWN || wp == WM_XBUTTONUP) {
            auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
            btnVk = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            if (wp == WM_XBUTTONDOWN) down = true; else up = true;
        }
        if (down && btnVk == ghHotkeyVk && CheckHotkeyModifiers(ghHotkeyMods)) {
            ghHotkeyPending = true;
            PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, 0, 0);
        }
        if (up && btnVk == ghHotkeyVk) ghHotkeyPending = false;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

class MainWindow {
public:
    bool Create() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &MainWindow::WndProc;
        wc.hInstance = g_instance;
        wc.lpszClassName = L"QuickScriptToolWindow";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        int x = (screenW - kHomeWidth) / 2;
        int y = (screenH - kHomeHeight) / 2;
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"鼠大侠-鼠标宏", WS_POPUP | WS_MINIMIZEBOX, x, y, kHomeWidth, kHomeHeight, nullptr, nullptr, g_instance, this);
        return hwnd_ != nullptr;
    }

    void Show(int nCmdShow) {
        ShowWindow(hwnd_, nCmdShow);
        UpdateWindow(hwnd_);
    }

    LRESULT RouteEditorDropPopup(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT RouteEditorTipPopup(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT RouteClickerDropPopup(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    friend LRESULT CALLBACK EditorDropPopupWndProc(HWND, UINT, WPARAM, LPARAM);
    friend LRESULT CALLBACK EditorTipPopupWndProc(HWND, UINT, WPARAM, LPARAM);
    friend LRESULT CALLBACK ClickerDropPopupWndProc(HWND, UINT, WPARAM, LPARAM);
    enum class Page { Home, Editor };
    enum Id { kScriptName = 1001, kModeCombo, kActionCombo, kAdd, kModify, kClear, kSave, kCancel, kLoad, kBatchExit, kBatchSelectAll, kBatchDeselect, kBatchDelete, kBatchCopy, kMoveX, kMoveY, kMoveRandomX, kMoveRandomY, kMoveFromVar, kMoveVarX, kMoveVarY, kClickButton, kClickCount, kClickWait, kClickRandom, kWaitDuration, kWaitRandom, kRemark, kListRemarkEdit, kClose, kKeyCapture, kClickLWin, kClickRWin, kClickLCtrl, kClickRCtrl, kClickLAlt, kClickRAlt, kClickLShift, kClickRShift, kKeyLWin, kKeyRWin, kKeyLCtrl, kKeyRCtrl, kKeyLAlt, kKeyRAlt, kKeyLShift, kKeyRShift, kCrosshair, kLoopCount, kLoopFromVar, kLoopVarExpr, kLoopVarName, kDefineBlockName, kRunBlockCombo, kKeyPressCapture, kMousePressButton, kMousePressLWin, kMousePressRWin, kMousePressLCtrl, kMousePressRCtrl, kMousePressLAlt, kMousePressRAlt, kMousePressLShift, kMousePressRShift, kKeyPressLWin, kKeyPressRWin, kKeyPressLCtrl, kKeyPressRCtrl, kKeyPressLAlt, kKeyPressRAlt, kKeyPressLShift, kKeyPressRShift, kHotkeyShortcutCombo, kHotkeyShortcutCount, kHotkeyShortcutWait, kHotkeyShortcutRandom, kQuickInputText, kQuickInputVarCombo, kQuickInputInsert, kQuickInputCharInterval, kQuickInputCount, kQuickInputWait, kQuickInputRandom, kRunMacroCombo, kMousePlaybackCombo, kMousePlaybackCount, kMousePlaybackWait, kMousePlaybackRandom, kScrollVertical, kScrollHorizontal, kScrollSteps, kScrollDirection, kScrollCount, kScrollWait, kScrollRandom, kFindFullScreen, kFindSelectRegion, kFindX1, kFindY1, kFindX2, kFindY2, kFindTest, kFindScreenshot, kFindLocalImage, kFindClearImage, kFindImagePreview, kFindMatchThreshold, kFindScaleMin, kFindScaleMax, kFindFollowUp, kFindOffsetX, kFindOffsetY, kFindSelectOffset, kFindUntilFound, kFindMatchVar, kOcrFullScreen, kOcrSelectRegion, kOcrX1, kOcrY1, kOcrX2, kOcrY2, kOcrResultMode, kOcrSearchText, kOcrSearchVarCombo, kOcrSearchVarInsert, kOcrFollowUp, kOcrOffsetX, kOcrOffsetY, kOcrSelectOffset, kOcrUntilFound, kOcrResultVar, kOcrTest, kOcrInstallDep, kOcrRegionByImage, kOcrFindSelectRegion, kOcrFindScreenshot, kOcrFindLocalImage, kOcrFindClearImage, kOcrFindImagePreview, kOcrFindMatchThreshold, kOcrFindScaleMin, kOcrFindScaleMax, kOcrDigitsOnly, kIfVarCombo, kIfOperator, kIfValue, kIfConnector, kIfAddCondition, kIfConditionList, kRunProgramCombo, kRunProgramPath, kRunProgramBrowse, kRunProgramCrosshair, kRunProgramArgs, kCloseProgramPath, kCloseProgramBrowse, kCloseProgramCrosshair, kCloseProgramMatchFileName, kOpenWebpageUrl, kOpenFilePath, kOpenFileBrowse, kTimerVarName, kAiPrompt, kAiInsertVar, kAiVarCombo, kAiModel, kAiContextMode, kAiOutputVar, kAiOutputType, kAiTimeout, kAiFallback, kAiImageScale, kAiRegionByImage, kAiRegionByImage2, kAiFindSelectRegion, kAiFindMatchThreshold, kAiFindScaleMin, kAiFindScaleMax, kAiTargetPreview, kAiTargetScreenshot, kAiTargetLocal, kAiTargetClear, kAiFullScreen, kAiSelectRegion, kAiSearchRegion, kAiSearchX1, kAiSearchY1, kAiSearchX2, kAiSearchY2, kAiMaxSteps, kAiWithImage, kAiConfirm };
    enum class HoverButton { None, Import, Export, Load, Clear, Add, Modify, Cancel, Save, Close, Minimize, Settings, HomeCard, HomeScroll, EditorScroll, Create, CommonHotkey, HomeEdit, HomeDelete, ScriptHotkey, Row, RowCopy, RowDelete, RowCheckbox, BatchExit, BatchSelectAll, BatchDeselect, BatchDelete, BatchCopy, Crosshair, ClickerInterval, ClickerHotkey, RecorderHotkey };
    enum MenuId { kCopyLast = 3001, kCopyFirst, kCopyBeforeSelected, kCopyAfterSelected, kAddLast, kAddFirst, kAddBeforeSelected, kAddAfterSelected, kAddAsChild, kHotCustom = 3101, kHotF8, kHotF10, kHotLeft, kHotMiddle, kHotRight, kHotX1, kHotX2, kHotSpace };
    struct HotkeyMenuItem { int id; const wchar_t* title; const wchar_t* desc; };
    struct EditorControlLayout { HWND hwnd = nullptr; RECT base{}; };
    struct PopupCombo { bool open = false; std::vector<std::wstring> items; int sel = 0; };
    struct ModifierHolds { HWND lWin = nullptr; HWND rWin = nullptr; HWND lCtrl = nullptr; HWND rCtrl = nullptr; HWND lAlt = nullptr; HWND rAlt = nullptr; HWND lShift = nullptr; HWND rShift = nullptr; };
    enum class QuickInputTipKind { None, TextExample, VariableHelp };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        MainWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<MainWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        } else self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        if (crosshairDrag_.IsActive()) {
            if (msg == WM_HOTKEY) { OnHotkey(static_cast<int>(wp)); return 0; }
            if (crosshairDrag_.HandleMessage(msg, wp, lp,
                [this](int x, int y) {
                    if (moveX_) SetText(moveX_, std::to_wstring(x));
                    if (moveY_) SetText(moveY_, std::to_wstring(y));
                },
                nullptr)) {
                return 0;
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        }
        switch (msg) {
        case WM_CREATE: Init(); return 0;
        case WM_PAINT: Paint(); return 0;
        case WM_COMMAND: OnCommand(LOWORD(wp), HIWORD(wp), reinterpret_cast<HWND>(lp)); return 0;
        case WM_DRAWITEM: DrawOwnerItem(reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE;
        case WM_MEASUREITEM: MeasureOwnerItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lp)); return TRUE;
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_MOUSELEAVE: OnMouseLeave(); return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt{}; GetCursorPos(&pt); ScreenToClient(hwnd_, &pt);
                if (page_ == Page::Editor && PtInRemark(pt.x, pt.y)) SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                else if (HitGrayButton(pt.x, pt.y)) SetCursor(LoadCursorW(nullptr, IDC_HAND));
                else SetCursor(LoadCursorW(nullptr, IsClickablePoint(pt.x, pt.y) ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_LBUTTONDOWN: OnMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_LBUTTONUP: OnMouseUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_MOUSEWHEEL: OnWheel(GET_WHEEL_DELTA_WPARAM(wp)); return 0;
        case WM_WINDOWPOSCHANGED: {
            const auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
            if (page_ == Page::Editor && IsWindowVisible(hwnd_) && pos
                && !(pos->flags & SWP_NOMOVE)) {
                ApplyParamLayerMasks();
            }
            if (EditorDropPopupVisible()) SyncEditorDropPopup();
            if (quickInputTipShown_ != QuickInputTipKind::None) SyncQuickInputTipPopup();
            return DefWindowProcW(hwnd_, msg, wp, lp);
        }
        case WM_SHOWWINDOW:
            if (!wp) {
                CloseEditorPopup();
                CloseClickerDropPopup();
                CancelQuickInputTip();
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) CloseClickerDropPopup();
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                HWND fg = GetForegroundWindow();
                if (fg != hwnd_ && fg != editorDropPopup_ && fg != editorTipPopup_ && fg != clickerDropPopup_) {
                    CloseEditorPopup();
                    CloseClickerDropPopup();
                    CancelQuickInputTip();
                    // 窗口失焦时取消拖拽状态，避免拖拽卡死
                    if (crosshairDrag_.IsActive()) crosshairDrag_.End();
                    if (dragging_) {
                        dragging_ = false;
                        dragIndex_ = -1;
                        ReleaseCapture();
                    }
                }
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_TIMER:
            if (wp == kHoverTimerId) { UpdateHoverFromCursor(); return 0; }
            if (wp == kQuickInputTipTimerId) { OnQuickInputTipTimer(); return 0; }
            if (wp == kScheduledTaskTimerId) { scheduledTasks_.Tick(); return 0; }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_HOTKEY: OnHotkey(static_cast<int>(wp)); return 0;
        case WM_GLOBAL_HOTKEY_DETECTED: OnHotkey(HOTKEY_GLOBAL_ID); return 0;
        case WM_RUN_DONE: OnRunDone(); return 0;
        case WM_FIND_TEST_DONE: OnFindTestDone(static_cast<int>(wp), static_cast<int>(lp)); return 0;
        case WM_OPEN_AGENT_DIALOG: OpenAgentDialog(); return 0;
        case WM_AGENT_SCRIPT_LIBRARY_CHANGED: RefreshScriptLibraryUi(); return 0;
        case WM_EDITOR_PARAM_CHROME:
            if (page_ == Page::Editor) RepaintParamPanelChrome();
            return 0;
        case WM_TRAY: return OnTrayMessage(lp);
        case WM_CTLCOLORSTATIC:
            return OnCtlColorStatic(reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp));
        case WM_CTLCOLOREDIT: return OnEditColor(reinterpret_cast<HDC>(wp));
        case WM_CLOSE:
            if (recording_) StopRecording();
            StopRun();
            if (page_ == Page::Home && appSettings_.other.closeToTray && !clicking_ && !recording_ && !running_) {
                HideToTray();
                return 0;
            }
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY: Cleanup(); PostQuitMessage(0); return 0;
        case WM_QUERYENDSESSION: if (recording_) StopRecording(); return TRUE;
        case WM_ENDSESSION: if (recording_) StopRecordingCleanup(); return 0;
        default: return DefWindowProcW(hwnd_, msg, wp, lp);
        }
    }

    // ── Initialization & cleanup ────────────────────────────────────
    void Init() {
        InitCommonControls();
        EnsureScriptsDir();
        font_ = CreateFontW(23, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        editorFont_ = CreateFontW(26, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        bigFont_ = CreateFontW(39, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        titleFont_ = CreateFontW(22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        hotFont_ = CreateFontW(25, 0, 0, 0, FW_BOLD, FALSE, TRUE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        closeFont_ = CreateFontW(32, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        homeFont_ = CreateFontW(25, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        homeTabFont_ = CreateFontW(28, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        UpdateClickerLayout();
        whiteBrush_ = CreateSolidBrush(kWhite);
        panelBrush_ = CreateSolidBrush(kPanel);
        lineGreenBrush_ = CreateSolidBrush(kLineGreen);
        crosshairDragCursor_ = CreateCrosshairDragCursor(kCrosshairBlue);
        CreateEditorControls();
        CaptureEditorControlLayout();
        AttachEditorChildSubclass();
        ApplyFont(hwnd_, font_);
        ShowWindow(mode_, SW_HIDE);
        ShowWindow(actionCombo_, SW_HIDE);
        ShowWindow(mousePressButton_, SW_HIDE);
        ShowWindow(clickButton_, SW_HIDE);
        ShowWindow(loopTypeCombo_, SW_HIDE);
        ShowWindow(runBlockCombo_, SW_HIDE);
        ShowWindow(hotkeyShortcutCombo_, SW_HIDE);
        ShowWindow(quickInputVarCombo_, SW_HIDE);
        ShowWindow(runMacroCombo_, SW_HIDE);
        ShowWindow(mousePlaybackCombo_, SW_HIDE);
        InitHotkeyShortcutPresets();
        LoadScripts();
        LoadRecordings();
        LoadAppSettings(appSettings_);
        ApplyDebugWindowSetting();
        CleanOrphanImages();  // 启动时清理孤立图片
        CreateEditorDropPopup();
        CreateClickerDropPopup();
        CreateEditorTipPopup();
        ShowHome();
        RegisterAllHotkeys();
        InstallGlobalHotkeyHooks();
        scheduledTasks_.Reload();
        scheduledTasks_.SetRunCallback([this](const std::wstring& path) { RunActionsFromPath(path); });
        SetTimer(hwnd_, kScheduledTaskTimerId, 1000, nullptr);
        SetAgentUiNotifyHwnd(hwnd_);
    }

    // ── Editor control creation ────────────────────────────────────
    void CreateEditorControls() {
        HWND labelMacro = MakeLabel(hwnd_, L"宏名称:", -1, 15, 52, 66, 26); editorControls_.push_back(labelMacro);
        name_ = MakeEdit(hwnd_, L"", kScriptName, 83, 54, 690, 24);
        SendMessageW(name_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
        editorControls_.push_back(name_);
        mode_ = MakeLabel(hwnd_, L"默认模式", kModeCombo,
            kEditorComboRight - kEditorModeComboW, 52, kEditorModeComboW, kEditorModeComboH);
        editorControls_.push_back(mode_);
        popupMode_.items = {L"默认模式", L"窗口模式", L"后台窗口模式"}; popupMode_.sel = 0;
        labelList_ = MakeLabel(hwnd_, L"动作列表", -1, 13, 101, 80, 26); editorControls_.push_back(labelList_);
        labelBatchCount_ = MakeLabel(hwnd_, L"已选中:0个", -1, 95, 101, 120, 26); editorControls_.push_back(labelBatchCount_);
        ShowWindow(labelBatchCount_, SW_HIDE);
        loadBtn_ = MakeGreenButton(hwnd_, L"批量编辑", kLoad, 546, 94, 105, 31); editorControls_.push_back(loadBtn_);
        clearBtn_ = MakeGreenButton(hwnd_, L"清空列表", kClear, 664, 94, 105, 31); editorControls_.push_back(clearBtn_);
        batchExitBtn_ = MakeGreenButton(hwnd_, L"退出批量编辑", kBatchExit, 258, 94, 118, 31); editorControls_.push_back(batchExitBtn_);
        batchSelectAllBtn_ = MakeGreenButton(hwnd_, L"全选", kBatchSelectAll, 390, 94, 68, 31); editorControls_.push_back(batchSelectAllBtn_);
        batchDeselectBtn_ = MakeGreenButton(hwnd_, L"取消选择", kBatchDeselect, 464, 94, 88, 31); editorControls_.push_back(batchDeselectBtn_);
        batchDeleteBtn_ = MakeGreenButton(hwnd_, L"删除所选项", kBatchDelete, 558, 94, 105, 31); editorControls_.push_back(batchDeleteBtn_);
        batchCopyBtn_ = MakeGreenButton(hwnd_, L"复制所选项", kBatchCopy, 667, 94, 105, 31); editorControls_.push_back(batchCopyBtn_);
        ShowWindow(batchExitBtn_, SW_HIDE);
        ShowWindow(batchSelectAllBtn_, SW_HIDE);
        ShowWindow(batchDeselectBtn_, SW_HIDE);
        ShowWindow(batchDeleteBtn_, SW_HIDE);
        ShowWindow(batchCopyBtn_, SW_HIDE);
        HWND labelNo = MakeLabel(hwnd_, L"序号", -1, 32, 137, 60, 26); editorControls_.push_back(labelNo);
        HWND labelAction = MakeLabel(hwnd_, L"动作", -1, 94, 137, 80, 26); editorControls_.push_back(labelAction);
        HWND labelRemark = MakeLabel(hwnd_, L"备注", -1, 438, 137, 80, 26); editorControls_.push_back(labelRemark);
        HWND labelOp = MakeLabel(hwnd_, L"操作", -1, 631, 137, 80, 26); editorControls_.push_back(labelOp);
        actionCombo_ = MakeLabel(hwnd_, L"移动鼠标到", kActionCombo,
            kEditorComboRight - kEditorActionComboW, 118, kEditorActionComboW, kEditorActionComboH);
        editorControls_.push_back(actionCombo_);
        popupAction_.items = {
            L"移动鼠标到", L"等待", L"鼠标点击", L"鼠标回放", L"运行鼠标宏",
            L"鼠标按下", L"鼠标松开", L"滚动滚轮", L"按键点击", L"键盘按下", L"键盘松开",
            L"快捷按键", L"快捷输入", L"循环", L"结束循环", L"定义宏指令块", L"运行宏指令块",
            L"找图(返回最匹配的)", L"文字识别", L"条件-如果", L"条件-否则",
            L"锁定截屏", L"解锁截屏", L"结束宏运行", L"运行程序", L"关闭程序",
            L"打开网页", L"打开文件", L"计时器记录时间",
            L"AI文字分析", L"AI图片分析", L"AI动作执行", L"获取当前光标位置"
        };
        popupAction_.sel = 0;
        paramViewport_ = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE,
            920, 225, 255, 755, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        editorControls_.push_back(paramViewport_);
        CreateParamControls();
        cancelBtn_ = MakeGreenButton(hwnd_, L"取消", kCancel, 775, 702, 104, 34); editorControls_.push_back(cancelBtn_);
        saveBtn_ = MakeGreenButton(hwnd_, L"保存", kSave, 891, 702, 104, 34); editorControls_.push_back(saveBtn_);
        listRemarkEdit_ = MakeEdit(hwnd_, L"", kListRemarkEdit, kColRemarkClient + 1, kListY + 2, kRemarkEditW, kRemarkEditH);
        ShowWindow(listRemarkEdit_, SW_HIDE);
        editorControls_.push_back(listRemarkEdit_);
        paramTopMask_ = MakeLabel(hwnd_, L"", -1, 0, 0, 1, 1); editorControls_.push_back(paramTopMask_);
        paramBottomMask_ = MakeLabel(hwnd_, L"", -1, 0, 0, 1, 1); editorControls_.push_back(paramBottomMask_);
        paramRightMask_ = MakeLabel(hwnd_, L"", -1, 0, 0, 1, 1); editorControls_.push_back(paramRightMask_);
    }

    void AddEditorControl(HWND h) { editorControls_.push_back(h); }
    void AddGroup(std::vector<HWND>& group, HWND h) { group.push_back(h); editorControls_.push_back(h); }

    RECT ParamViewportRect() const {
        return ParamScrollContentRect();
    }

    void MoveParamAware(HWND h, int x, int y, int w, int hgt, BOOL repaint = FALSE) const {
        if (!h) return;
        if (paramViewport_ && GetParent(h) == paramViewport_) {
            const RECT vp = ParamViewportRect();
            MoveWindow(h, x - vp.left, y - vp.top, w, hgt, repaint);
        } else {
            MoveWindow(h, x, y, w, hgt, repaint);
        }
    }

    void SetParamPosAware(HWND h, int x, int y, int w, int hgt, UINT flags) const {
        if (!h) return;
        if (paramViewport_ && GetParent(h) == paramViewport_) {
            const RECT vp = ParamViewportRect();
            SetWindowPos(h, nullptr, x - vp.left, y - vp.top, w, hgt, flags);
        } else {
            SetWindowPos(h, nullptr, x, y, w, hgt, flags);
        }
    }

    void AttachToParamViewport(HWND h) const {
        if (!h || !paramViewport_ || h == paramViewport_) return;
        if (GetParent(h) == paramViewport_) return;
        RECT rc = WindowClientRect(h);
        SetParent(h, paramViewport_);
        SetWindowSubclass(h, EditorChildSubclassProc, 1, reinterpret_cast<DWORD_PTR>(const_cast<MainWindow*>(this)));
        const RECT vp = ParamViewportRect();
        MoveWindow(h, rc.left - vp.left, rc.top - vp.top, rc.right - rc.left, rc.bottom - rc.top, FALSE);
        auto* self = const_cast<MainWindow*>(this);
        if (self->IsGrayButton(h)) {
            RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        }
    }

    // 使用新布局系统构建参数面板布局
    // group: 输出参数，布局中的 HWND 会被添加到该 group
    // idx: 布局索引 (对应 popupAction_.sel 的值)
    UILayoutResult BuildAndStoreLayout(UILayout layout, std::vector<HWND>& group, int idx) {
        auto result = BuildLayout(hwnd_, layout);
        for (const auto& p : result.placements) {
            if (p.hwnd) {
                AttachToParamViewport(p.hwnd);
                group.push_back(p.hwnd);
                editorControls_.push_back(p.hwnd);
            }
        }
        paramLayoutResults_[idx] = result;
        return result;
    }

    // 获取指定 action index 对应的布局结果中的 HWND
    HWND LayoutHwnd(int idx, int id) const {
        auto it = paramLayoutResults_.find(idx);
        if (it != paramLayoutResults_.end()) return it->second.HwndForId(id);
        return nullptr;
    }

    static HWND FindLayoutTextControl(const UILayoutResult& result, const wchar_t* text, int occurrence = 0) {
        int seen = 0;
        for (const auto& p : result.placements) {
            if (!p.hwnd) continue;
            if (p.type != UIComponentType::Label
                && p.type != UIComponentType::EditorLabel
                && p.type != UIComponentType::Hint) {
                continue;
            }
            wchar_t buf[128]{};
            GetWindowTextW(p.hwnd, buf, 128);
            if (wcscmp(buf, text) != 0) continue;
            if (seen++ == occurrence) return p.hwnd;
        }
        return nullptr;
    }

    // 显示/隐藏指定 action index 的布局
    void ShowParamLayout(int idx, bool visible) {
        auto it = paramLayoutResults_.find(idx);
        if (it != paramLayoutResults_.end()) {
            ShowLayoutGroup(it->second, visible);
        }
    }

    // 收集当前动作类型所有可见参数面板中需要 GDI 绘制的控件，按 layer 排序
    // 返回: (hwnd, type, layer) 的排序列表 (低 layer → 高 layer, 即先绘制的在前)
    struct ParamDrawItem {
        HWND hwnd = nullptr;
        UIComponentType type = UIComponentType::Label;
        int layer = 0;
    };
    std::vector<ParamDrawItem> CollectParamDrawItems() const {
        std::vector<ParamDrawItem> items;
        const int sel = popupAction_.sel;
        for (const auto& [idx, result] : paramLayoutResults_) {
            // 只收集当前可见的 action 相关布局
            if (idx != sel && !IsSubPanelIdxVisible(sel, idx)) continue;
            for (const auto& p : result.placements) {
                if (!p.hwnd || !IsWindowVisible(p.hwnd)) continue;
                // 只收集需要自定义 GDI 绘制的类型
                if (p.type != UIComponentType::Edit
                    && p.type != UIComponentType::FieldEdit
                    && p.type != UIComponentType::ComboLabel) continue;
                items.push_back({p.hwnd, p.type, p.layer});
            }
        }
        std::sort(items.begin(), items.end(),
            [](const ParamDrawItem& a, const ParamDrawItem& b) { return a.layer < b.layer; });
        return items;
    }
    bool IsSubPanelIdxVisible(int sel, int idx) const {
        // 判断子面板索引是否对应当前 sel 可见
        if (idx == 170 || idx == 171) return sel == 17;                          // 找图子面板
        if (idx >= 180 && idx <= 186) return sel == 18;                          // OCR 子面板
        if (idx == 240) return sel == 24 && popupRunProgram_.sel <= 0;           // 打开程序文件子面板
        if (idx == 29) return sel == 29 || sel == 30 || sel == 31;             // AI 共用面板
        if (idx == 300) return sel == 30;                                        // AI 图片子面板
        if (idx == 310) return sel == 31;                                        // AI 动作子面板
        if (idx == 320) return sel == 29 || sel == 30 || sel == 31;             // AI 找图子区域
        return false;
    }

    void CreateParamControls() {
        using namespace EditorParamLayout;
        namespace EID = EditorParamLayout;  // for ID constants

        // ── 0. 移动鼠标到 ──
        {
            auto r = BuildAndStoreLayout(MoveMouse(), moveControls_, 0);
            moveX_ = r.HwndForId(EID_MoveX); moveY_ = r.HwndForId(EID_MoveY);
            moveRandomX_ = r.HwndForId(EID_MoveRandomX); moveRandomY_ = r.HwndForId(EID_MoveRandomY);
            crosshairBtn_ = r.HwndForId(EID_Crosshair); moveFromVar_ = r.HwndForId(EID_MoveFromVar);
            moveVarX_ = r.HwndForId(EID_MoveVarX); moveVarY_ = r.HwndForId(EID_MoveVarY);
        }

        // ── 1. 等待 ──
        {
            auto r = BuildAndStoreLayout(Wait(), waitControls_, 1);
            waitDuration_ = r.HwndForId(EID_WaitDuration); waitRandom_ = r.HwndForId(EID_WaitRandom);
        }

        // ── 2. 鼠标点击 ──
        {
            auto r = BuildAndStoreLayout(MouseClick(), clickControls_, 2);
            clickButton_ = r.HwndForId(EID_ClickButton);
            clickLWin_ = r.HwndForId(EID_ClickLWin); clickRWin_ = r.HwndForId(EID_ClickRWin);
            clickLCtrl_ = r.HwndForId(EID_ClickLCtrl); clickRCtrl_ = r.HwndForId(EID_ClickRCtrl);
            clickLAlt_ = r.HwndForId(EID_ClickLAlt); clickRAlt_ = r.HwndForId(EID_ClickRAlt);
            clickLShift_ = r.HwndForId(EID_ClickLShift); clickRShift_ = r.HwndForId(EID_ClickRShift);
            clickCount_ = r.HwndForId(EID_ClickCount); clickWait_ = r.HwndForId(EID_ClickWait);
            clickRandom_ = r.HwndForId(EID_ClickRandom);
            popupClickBtn_.items = {L"左键", L"右键", L"中键", L"侧键1", L"侧键2"}; popupClickBtn_.sel = 0;
        }

        // ── 3. 鼠标回放 ──
        {
            auto r = BuildAndStoreLayout(MousePlayback(), mousePlaybackControls_, 3);
            mousePlaybackCombo_ = r.HwndForId(EID_MousePlaybackCombo);
            mousePlaybackCount_ = r.HwndForId(EID_MousePlaybackCount);
            mousePlaybackWait_ = r.HwndForId(EID_MousePlaybackWait);
            mousePlaybackRandom_ = r.HwndForId(EID_MousePlaybackRandom);
            popupMousePlayback_.items.clear(); popupMousePlayback_.sel = -1;
        }

        // ── 4. 运行鼠标宏 ──
        {
            auto r = BuildAndStoreLayout(RunMacro(), runMacroControls_, 4);
            runMacroCombo_ = r.HwndForId(EID_RunMacroCombo);
            popupRunMacro_.items.clear(); popupRunMacro_.sel = -1;
        }

        // ── 5/6. 鼠标按下/松开 (共用) ──
        {
            auto r = BuildAndStoreLayout(MousePress(), mousePressControls_, 5);
            mousePressButton_ = r.HwndForId(EID_MousePressButton);
            mousePressLWin_ = r.HwndForId(EID_MousePressLWin); mousePressRWin_ = r.HwndForId(EID_MousePressRWin);
            mousePressLCtrl_ = r.HwndForId(EID_MousePressLCtrl); mousePressRCtrl_ = r.HwndForId(EID_MousePressRCtrl);
            mousePressLAlt_ = r.HwndForId(EID_MousePressLAlt); mousePressRAlt_ = r.HwndForId(EID_MousePressRAlt);
            mousePressLShift_ = r.HwndForId(EID_MousePressLShift); mousePressRShift_ = r.HwndForId(EID_MousePressRShift);
            popupMouseBtn_.items = {L"左键", L"右键", L"中键", L"侧键1", L"侧键2"}; popupMouseBtn_.sel = 0;
            // 共享: 将同样的布局注册到 idx 6
            paramLayoutResults_[6] = r;
            for (HWND h : mousePressControls_) { if (h) editorControls_.push_back(h); }
        }

        // ── 7. 滚动滚轮 ──
        {
            auto r = BuildAndStoreLayout(ScrollWheel(), scrollWheelControls_, 7);
            scrollVertical_ = r.HwndForId(EID_ScrollVertical); scrollHorizontal_ = r.HwndForId(EID_ScrollHorizontal);
            scrollSteps_ = r.HwndForId(EID_ScrollSteps); scrollDirectionCombo_ = r.HwndForId(EID_ScrollDirection);
            scrollCount_ = r.HwndForId(EID_ScrollCount); scrollWait_ = r.HwndForId(EID_ScrollWait);
            scrollRandom_ = r.HwndForId(EID_ScrollRandom);
            SetChecked(scrollVertical_, true);
            popupScrollDir_.items = {L"向上/左", L"向下/右"}; popupScrollDir_.sel = 0;
        }

        // ── 8. 按键点击 ──
        {
            auto r = BuildAndStoreLayout(KeyClick(), keyControls_, 8);
            keyEdit_ = r.HwndForId(EID_KeyCapture);
            keyLWin_ = r.HwndForId(EID_KeyLWin); keyRWin_ = r.HwndForId(EID_KeyRWin);
            keyLCtrl_ = r.HwndForId(EID_KeyLCtrl); keyRCtrl_ = r.HwndForId(EID_KeyRCtrl);
            keyLAlt_ = r.HwndForId(EID_KeyLAlt); keyRAlt_ = r.HwndForId(EID_KeyRAlt);
            keyLShift_ = r.HwndForId(EID_KeyLShift); keyRShift_ = r.HwndForId(EID_KeyRShift);
            // 循环次数等通过动态 ID 0 查找 (遍历 placements 按顺序取)
            for (auto& p : r.placements) {
                if (p.id == 0 && !keyCount_ && p.hwnd) keyCount_ = p.hwnd;
                else if (p.id == 0 && keyCount_ && !keyWait_ && p.hwnd) keyWait_ = p.hwnd;
                else if (p.id == 0 && keyCount_ && keyWait_ && !keyRandom_ && p.hwnd) keyRandom_ = p.hwnd;
            }
        }

        // ── 9/10. 键盘按下/松开 (共用) ──
        {
            auto r = BuildAndStoreLayout(KeyPress(), keyPressControls_, 9);
            keyPressEdit_ = r.HwndForId(EID_KeyPressCapture);
            keyPressLWin_ = r.HwndForId(EID_KeyPressLWin); keyPressRWin_ = r.HwndForId(EID_KeyPressRWin);
            keyPressLCtrl_ = r.HwndForId(EID_KeyPressLCtrl); keyPressRCtrl_ = r.HwndForId(EID_KeyPressRCtrl);
            keyPressLAlt_ = r.HwndForId(EID_KeyPressLAlt); keyPressRAlt_ = r.HwndForId(EID_KeyPressRAlt);
            keyPressLShift_ = r.HwndForId(EID_KeyPressLShift); keyPressRShift_ = r.HwndForId(EID_KeyPressRShift);
            paramLayoutResults_[10] = r;
            for (HWND h : keyPressControls_) { if (h) editorControls_.push_back(h); }
        }

        // ── 11. 快捷按键 ──
        {
            auto r = BuildAndStoreLayout(HotkeyShortcut(), hotkeyShortcutControls_, 11);
            hotkeyShortcutCombo_ = r.HwndForId(EID_HotkeyShortcutCombo);
            hotkeyShortcutCount_ = r.HwndForId(EID_HotkeyShortcutCount);
            hotkeyShortcutWait_ = r.HwndForId(EID_HotkeyShortcutWait);
            hotkeyShortcutRandom_ = r.HwndForId(EID_HotkeyShortcutRandom);
        }

        // ── 12. 快捷输入 ──
        {
            auto r = BuildAndStoreLayout(QuickInput(), quickInputControls_, 12);
            quickInputEdit_ = r.HwndForId(EID_QuickInputText);
            quickInputVarCombo_ = r.HwndForId(EID_QuickInputVarCombo);
            quickInputInsertBtn_ = r.HwndForId(EID_QuickInputInsert);
            quickInputCharInterval_ = r.HwndForId(EID_QuickInputCharInterval);
            quickInputCount_ = r.HwndForId(EID_QuickInputCount);
            quickInputWait_ = r.HwndForId(EID_QuickInputWait);
            quickInputRandom_ = r.HwndForId(EID_QuickInputRandom);
        }

        // ── 13. 循环 ──
        {
            auto r = BuildAndStoreLayout(Loop(), loopControls_, 13);
            loopTypeCombo_ = r.HwndForId(EID_LoopTypeCombo);
            loopCount_ = r.HwndForId(EID_LoopCount); loopFromVar_ = r.HwndForId(EID_LoopFromVar);
            loopVarExpr_ = r.HwndForId(EID_LoopVarExpr); loopVarName_ = r.HwndForId(EID_LoopVarName);
            popupLoopType_.items = {L"次数循环", L"变量"}; popupLoopType_.sel = 0;
        }

        // ── 14. 结束循环 ──
        { BuildAndStoreLayout(EndLoop(), endLoopControls_, 14); }

        // ── 15. 定义宏指令块 ──
        {
            auto r = BuildAndStoreLayout(DefineBlock(), defineBlockControls_, 15);
            defineBlockName_ = r.HwndForId(EID_DefineBlockName);
        }

        // ── 16. 运行宏指令块 ──
        {
            auto r = BuildAndStoreLayout(RunBlock(), runBlockControls_, 16);
            runBlockCombo_ = r.HwndForId(EID_RunBlockCombo);
            popupRunBlock_.items.clear(); popupRunBlock_.sel = -1;
        }

        // ── 17. 找图 (基础 + 子面板) ──
        {
            auto r = BuildAndStoreLayout(FindImageBase(), findImageControls_, 17);
            findFullScreenBtn_ = r.HwndForId(EID_FindFullScreen); findSelectRegionBtn_ = r.HwndForId(EID_FindSelectRegion);
            findX1_ = r.HwndForId(EID_FindX1); findY1_ = r.HwndForId(EID_FindY1);
            findX2_ = r.HwndForId(EID_FindX2); findY2_ = r.HwndForId(EID_FindY2);
            findTestBtn_ = r.HwndForId(EID_FindTest);
            findImagePreviewBtn_ = r.HwndForId(EID_FindImagePreview);
            findScreenshotBtn_ = r.HwndForId(EID_FindScreenshot); findLocalImageBtn_ = r.HwndForId(EID_FindLocalImage);
            findClearImageBtn_ = r.HwndForId(EID_FindClearImage);
            findMatchThreshold_ = r.HwndForId(EID_FindMatchThreshold);
            findScaleMin_ = r.HwndForId(EID_FindScaleMin); findScaleMax_ = r.HwndForId(EID_FindScaleMax);
            findFollowUpCombo_ = r.HwndForId(EID_FindFollowUp);
            popupFindFollowUp_.items = {L"点击", L"鼠标移动到", L"保存到变量"}; popupFindFollowUp_.sel = 0;

            auto r2 = BuildAndStoreLayout(FindImageOffset(), findImageOffsetControls_, 170);
            findOffsetX_ = r2.HwndForId(EID_FindOffsetX); findOffsetY_ = r2.HwndForId(EID_FindOffsetY);
            findSelectOffsetBtn_ = r2.HwndForId(EID_FindSelectOffset); findUntilFound_ = r2.HwndForId(EID_FindUntilFound);

            auto r3 = BuildAndStoreLayout(FindImageVar(), findImageVarControls_, 171);
            findMatchVar_ = r3.HwndForId(EID_FindMatchVar);

            findRegionLabel_ = r.HwndForId(-1);  // first label in base layout
            auto findLabel = [](const UILayoutResult& lr, const wchar_t* text) -> HWND {
                for (const auto& p : lr.placements) {
                    if (!p.hwnd) continue;
                    if (p.type != UIComponentType::Label && p.type != UIComponentType::EditorLabel) continue;
                    wchar_t buf[64]{};
                    GetWindowTextW(p.hwnd, buf, 64);
                    if (wcscmp(buf, text) == 0) return p.hwnd;
                }
                return nullptr;
            };
            findFollowUpLabel_ = findLabel(r, L"后续操作");
            findOffsetXLabel_ = findLabel(r2, L"X偏");
            findOffsetYLabel_ = findLabel(r2, L"Y偏");
            findMatchVarLabel_ = findLabel(r3, L"匹配度保存到");
        }

        // ── 18. 文字识别 (基础 + 子面板) ──
        {
            auto r = BuildAndStoreLayout(OcrDepStatus(), ocrDepControls_, 180);
            ocrDepStatusLabel_ = r.HwndForId(-1);
            ocrDepInstallBtn_ = r.HwndForId(EID_OcrInstallDep);

            auto rt = BuildAndStoreLayout(OcrFindRegionToggle(), ocrFindRegionToggleControls_, 181);
            ocrRegionByImageCheck_ = rt.HwndForId(EID_OcrRegionByImage);
            ocrDigitsOnlyCheck_ = rt.HwndForId(EID_OcrDigitsOnly);

            auto rb = BuildAndStoreLayout(OcrBase(), ocrControls_, 18);
            ocrFullScreenBtn_ = rb.HwndForId(EID_OcrFullScreen); ocrSelectRegionBtn_ = rb.HwndForId(EID_OcrSelectRegion);
            ocrX1_ = rb.HwndForId(EID_OcrX1); ocrY1_ = rb.HwndForId(EID_OcrY1);
            ocrX2_ = rb.HwndForId(EID_OcrX2); ocrY2_ = rb.HwndForId(EID_OcrY2);
            ocrResultModeCombo_ = rb.HwndForId(EID_OcrResultMode); ocrTestBtn_ = rb.HwndForId(EID_OcrTest);
            popupOcrResultMode_.items = {L"获取文字", L"文字查找"}; popupOcrResultMode_.sel = 0;

            auto rs = BuildAndStoreLayout(OcrSearch(), ocrSearchControls_, 182);
            ocrSearchEdit_ = rs.HwndForId(EID_OcrSearchText);
            ocrSearchVarCombo_ = rs.HwndForId(EID_OcrSearchVarCombo); ocrSearchVarInsertBtn_ = rs.HwndForId(EID_OcrSearchVarInsert);
            popupOcrSearchVar_.items.clear(); popupOcrSearchVar_.sel = -1;

            auto rf = BuildAndStoreLayout(OcrFollowUp(), ocrFollowControls_, 183);
            ocrFollowUpCombo_ = rf.HwndForId(EID_OcrFollowUp); ocrUntilFound_ = rf.HwndForId(EID_OcrUntilFound);
            popupOcrFollowUp_.items = {L"点击", L"鼠标移动到", L"保存到变量"}; popupOcrFollowUp_.sel = 0;

            auto ro = BuildAndStoreLayout(OcrFollowOffset(), ocrFollowOffsetControls_, 184);
            ocrOffsetX_ = ro.HwndForId(EID_OcrOffsetX); ocrOffsetY_ = ro.HwndForId(EID_OcrOffsetY);
            ocrSelectOffsetBtn_ = ro.HwndForId(EID_OcrSelectOffset);

            auto rv = BuildAndStoreLayout(OcrFollowVar(), ocrFollowVarControls_, 185);
            ocrResultVar_ = rv.HwndForId(EID_OcrResultVar);

            auto rfr = BuildAndStoreLayout(OcrFindRegion(), ocrFindRegionControls_, 186);
            ocrFindImagePreviewBtn_ = rfr.HwndForId(EID_OcrFindImagePreview);
            ocrFindScreenshotBtn_ = rfr.HwndForId(EID_OcrFindScreenshot); ocrFindLocalImageBtn_ = rfr.HwndForId(EID_OcrFindLocalImage);
            ocrFindClearImageBtn_ = rfr.HwndForId(EID_OcrFindClearImage);
            ocrFindMatchThreshold_ = rfr.HwndForId(EID_OcrFindMatchThreshold);
            ocrFindScaleMin_ = rfr.HwndForId(EID_OcrFindScaleMin); ocrFindScaleMax_ = rfr.HwndForId(EID_OcrFindScaleMax);
            ocrFindSelectRegionBtn_ = rfr.HwndForId(EID_OcrFindSelectRegion);
            ocrFindImageLabel_ = rfr.HwndForId(-1);

            ocrRegionLabel_ = rb.HwndForId(-1);
            auto findLabel = [](const UILayoutResult& r, const wchar_t* text) -> HWND {
                for (const auto& p : r.placements) {
                    if (!p.hwnd) continue;
                    if (p.type != UIComponentType::Label && p.type != UIComponentType::EditorLabel) continue;
                    wchar_t buf[64]{};
                    GetWindowTextW(p.hwnd, buf, 64);
                    if (wcscmp(buf, text) == 0) return p.hwnd;
                }
                return nullptr;
            };
            ocrResultModeLabel_ = findLabel(rb, L"结果处理");
            ocrFollowUpLabel_ = findLabel(rf, L"后续操作");
            ocrSearchLabel_ = findLabel(rs, L"查找:");
            ocrSearchVarLabel_ = findLabel(rs, L"变量:");
            ocrX1Label_ = findLabel(rb, L"X1");
            ocrY1Label_ = findLabel(rb, L"Y1");
            ocrX2Label_ = findLabel(rb, L"X2");
            ocrY2Label_ = findLabel(rb, L"Y2");
            ocrOffsetXLabel_ = findLabel(ro, L"X偏");
            ocrOffsetYLabel_ = findLabel(ro, L"Y偏");
            ocrResultVarLabel_ = findLabel(rv, L"结果保存到");
            SetPopupSel(popupOcrResultMode_, ocrResultModeCombo_, 0);
            SetPopupSel(popupOcrFollowUp_, ocrFollowUpCombo_, 0);
            RefreshOcrSubPanel();
            RefreshOcrDepStatus();
        }

        // ── 19. 条件-如果 ──
        {
            auto r = BuildAndStoreLayout(IfCondition(), ifControls_, 19);
            ifVarCombo_ = r.HwndForId(EID_IfVarCombo); ifOperatorCombo_ = r.HwndForId(EID_IfOperator);
            ifValueEdit_ = r.HwndForId(EID_IfValue); ifConnectorCombo_ = r.HwndForId(EID_IfConnector);
            ifAddConditionBtn_ = r.HwndForId(EID_IfAddCondition); ifConditionList_ = r.HwndForId(EID_IfConditionList);
            popupIfOperator_.items = {L"等于", L"不等于", L"小于", L"小于等于", L"大于", L"大于等于", L"包含"}; popupIfOperator_.sel = 0;
            popupIfConnector_.items = {L"并且(and)", L"或者(or)", L"非(not)"}; popupIfConnector_.sel = 0;
        }

        // ── 20. 条件-否则 ──
        { BuildAndStoreLayout(ElseCondition(), elseControls_, 20); }

        // ── 21. 锁定截屏 ──
        { BuildAndStoreLayout(LockScreenshot(), lockScreenshotControls_, 21); }

        // ── 22. 解锁截屏 ──
        { BuildAndStoreLayout(UnlockScreenshot(), unlockScreenshotControls_, 22); }

        // ── 23. 结束宏运行 ──
        { BuildAndStoreLayout(StopMacro(), stopMacroControls_, 23); }

        // ── 24. 打开程序 ──
        {
            auto r = BuildAndStoreLayout(RunProgram(), runProgramControls_, 24);
            runProgramCombo_ = r.HwndForId(EID_RunProgramCombo);

            auto rf = BuildAndStoreLayout(RunProgramFile(), runProgramFileControls_, 240);
            runProgramPath_ = rf.HwndForId(EID_RunProgramPath); runProgramBrowseBtn_ = rf.HwndForId(EID_RunProgramBrowse);
            runProgramCrosshairBtn_ = rf.HwndForId(EID_RunProgramCrosshair); runProgramArgs_ = rf.HwndForId(EID_RunProgramArgs);
        }

        // ── 25. 关闭程序 ──
        {
            auto r = BuildAndStoreLayout(CloseProgram(), closeProgramControls_, 25);
            closeProgramPath_ = r.HwndForId(EID_CloseProgramPath); closeProgramBrowseBtn_ = r.HwndForId(EID_CloseProgramBrowse);
            closeProgramCrosshairBtn_ = r.HwndForId(EID_CloseProgramCrosshair);
            closeProgramMatchFileName_ = r.HwndForId(EID_CloseProgramMatchFileName);
        }

        // ── 26. 打开网页 ──
        {
            auto r = BuildAndStoreLayout(OpenWebpage(), openWebpageControls_, 26);
            openWebpageUrl_ = r.HwndForId(EID_OpenWebpageUrl);
        }

        // ── 27. 打开文件 ──
        {
            auto r = BuildAndStoreLayout(OpenFile(), openFileControls_, 27);
            openFilePath_ = r.HwndForId(EID_OpenFilePath); openFileBrowseBtn_ = r.HwndForId(EID_OpenFileBrowse);
        }

        // ── 28. 计时器记录时间 ──
        {
            auto r = BuildAndStoreLayout(TimerRecord(), timerRecordControls_, 28);
            timerVarName_ = r.HwndForId(EID_TimerVarName);
        }

        // ── 32. 获取当前光标位置 ──
        {
            auto r = BuildAndStoreLayout(GetCursorPos(), getCursorPosControls_, 32);
            cursorPosVarName_ = r.HwndForId(EID_CursorPosVarName);
        }

        // ── AI 公共部分 ──
        popupAiModel_.items = {}; popupAiModel_.sel = -1;
        popupAiContextMode_.items = {L"无上下文", L"宏上下文", L"循环上下文", L"指令块上下文"}; popupAiContextMode_.sel = 0;
        popupAiOutputType_.items = {L"文本", L"整数"}; popupAiOutputType_.sel = 0;

        // ── 29. AI文字分析 ──
        {
            auto r = BuildAndStoreLayout(AiCommon(), aiCommonControls_, 29);
            aiPromptEdit_ = r.HwndForId(EID_AiPrompt); aiInsertVarBtn_ = r.HwndForId(EID_AiInsertVar);
            aiVarCombo_ = r.HwndForId(EID_AiVarCombo);
            aiModelCombo_ = r.HwndForId(EID_AiModel); aiContextModeCombo_ = r.HwndForId(EID_AiContextMode);
            aiTimeoutEdit_ = r.HwndForId(EID_AiTimeout); aiFallbackEdit_ = r.HwndForId(EID_AiFallback);
            aiOutputVarEdit_ = r.HwndForId(EID_AiOutputVar); aiOutputTypeCombo_ = r.HwndForId(EID_AiOutputType);
            aiPromptLabel_ = FindLayoutTextControl(r, L"提示词 (Prompt)");
            aiVarLabel_ = FindLayoutTextControl(r, L"变量");
            aiModelLabel_ = FindLayoutTextControl(r, L"AI 模型");
            aiContextLabel_ = FindLayoutTextControl(r, L"上下文模式");
            aiTimeoutLabel_ = FindLayoutTextControl(r, L"超时(秒)");
            aiFallbackLabel_ = FindLayoutTextControl(r, L"降级值(失败时使用)");
            aiOutputVarLabel_ = FindLayoutTextControl(r, L"输出变量名");
            aiOutputTypeLabel_ = FindLayoutTextControl(r, L"输出类型");
            aiMaxStepsEdit_ = r.HwndForId(EID_AiMaxSteps);
            aiWithImageCheck_ = r.HwndForId(EID_AiWithImage);
            aiMaxStepsLabel_ = FindLayoutTextControl(r, L"最大步骤数");
            aiMaxStepsHint_ = FindLayoutTextControl(r, L"*提示:-1表示不限制步数");
        }

        // ── 30. AI图片分析专用 ──
        {
            auto r = BuildAndStoreLayout(AiImage(), aiImageControls_, 300);
            aiImageScaleEdit_ = r.HwndForId(EID_AiImageScale); aiRegionByImageCheck_ = r.HwndForId(EID_AiRegionByImage);
            aiFullScreenBtn_ = r.HwndForId(EID_AiFullScreen); aiSelectRegionBtn_ = r.HwndForId(EID_AiSelectRegion);
            aiSearchX1Edit_ = r.HwndForId(EID_AiSearchX1); aiSearchY1Edit_ = r.HwndForId(EID_AiSearchY1);
            aiSearchX2Edit_ = r.HwndForId(EID_AiSearchX2); aiSearchY2Edit_ = r.HwndForId(EID_AiSearchY2);
            aiImageScaleLabel_ = FindLayoutTextControl(r, L"截屏缩放(0.1-1.0)");
            aiRegionLabel_ = FindLayoutTextControl(r, L"识别区域");
            aiCoordX1Label_ = FindLayoutTextControl(r, L"X1");
            aiCoordY1Label_ = FindLayoutTextControl(r, L"Y1");
            aiCoordX2Label_ = FindLayoutTextControl(r, L"X2");
            aiCoordY2Label_ = FindLayoutTextControl(r, L"Y2");
        }

        // ── 31. AI动作执行专用 ──
        {
            auto r = BuildAndStoreLayout(AiAction(), aiActionControls_, 310);
            aiRegionByImageCheck2_ = r.HwndForId(EID_AiRegionByImage2);
            aiFullScreenBtn2_ = r.HwndForId(EID_AiFullScreen); aiSelectRegionBtn2_ = r.HwndForId(EID_AiSelectRegion);
            aiSearchX1Edit2_ = r.HwndForId(EID_AiSearchX1); aiSearchY1Edit2_ = r.HwndForId(EID_AiSearchY1);
            aiSearchX2Edit2_ = r.HwndForId(EID_AiSearchX2); aiSearchY2Edit2_ = r.HwndForId(EID_AiSearchY2);
            aiConfirmExecute_ = r.HwndForId(EID_AiConfirm);
            aiActionRegionLabel_ = FindLayoutTextControl(r, L"识别区域");
            aiActCoordX1Label_ = FindLayoutTextControl(r, L"X1");
            aiActCoordY1Label_ = FindLayoutTextControl(r, L"Y1");
            aiActCoordX2Label_ = FindLayoutTextControl(r, L"X2");
            aiActCoordY2Label_ = FindLayoutTextControl(r, L"Y2");
        }

        // ── AI 根据图片选取区域 (共用) ──
        {
            auto r = BuildAndStoreLayout(AiFindRegion(), aiFindRegionControls_, 320);
            aiFindImagePreviewBtn_ = r.HwndForId(EID_AiTargetPreview);
            aiFindScreenshotBtn_ = r.HwndForId(EID_AiTargetScreenshot); aiFindLocalImageBtn_ = r.HwndForId(EID_AiTargetLocal);
            aiFindClearImageBtn_ = r.HwndForId(EID_AiTargetClear);
            aiFindMatchThreshold_ = r.HwndForId(EID_AiFindMatchThreshold);
            aiFindScaleMin_ = r.HwndForId(EID_AiFindScaleMin); aiFindScaleMax_ = r.HwndForId(EID_AiFindScaleMax);
            aiFindSelectRegionBtn_ = r.HwndForId(EID_AiFindSelectRegion);
            aiFindImageLabel_ = FindLayoutTextControl(r, L"要查找的图");
            aiFindMatchLabel_ = FindLayoutTextControl(r, L"匹配度大于");
            aiFindMatchPctLabel_ = FindLayoutTextControl(r, L"%");
            aiFindScaleMinLabel_ = FindLayoutTextControl(r, L"最小缩放");
            aiFindScaleMaxLabel_ = FindLayoutTextControl(r, L"最大");
        }

        InitRunProgramPresets();
        InitCrosshairDrag();

        AddEditorControl(remarkLabel_ = MakeLabel(hwnd_, L"备注", -1, 807, kEditorRemarkY, 44, 22));
        AddEditorControl(remark_ = MakeEdit(hwnd_, L"", kRemark, 857, kEditorRemarkY, 117, 22));
        AddEditorControl(modifyBtn_ = MakeGreenButton(hwnd_, L"修改", kModify, 837, kEditorAddY, 76, 30));
        AddEditorControl(addBtn_ = MakeGreenButton(hwnd_, L"添加", kAdd, 927, kEditorAddY, 76, 30));
    }

    void ApplyEditorFonts() {
        for (HWND h : editorControls_) {
            if (!h) continue;
            wchar_t cls[16]{};
            GetClassNameW(h, cls, 16);
            if (wcscmp(cls, L"Button") == 0) {
                LONG style = GetWindowLongW(h, GWL_STYLE);
                if (style & BS_OWNERDRAW) continue;
            }
            wchar_t text[256]{};
            GetWindowTextW(h, text, 256);
            if (text[0] == L'*') continue;
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(editorFont_), TRUE);
        }
    }

    void ShowGroup(const std::vector<HWND>& controls, bool visible) { for (HWND h : controls) ShowWindow(h, visible ? SW_SHOW : SW_HIDE); }
    void ShowEditorControls(bool visible) {
        if (!visible) CloseEditorPopup();
        for (HWND h : editorControls_) ShowWindow(h, visible ? SW_SHOW : SW_HIDE);
        ApplyParamLayerMasks();
    }

    bool IsFooterControl(HWND hwnd) const {
        return hwnd == remarkLabel_ || hwnd == remark_ || hwnd == addBtn_ || hwnd == modifyBtn_;
    }

    bool IsParamMaskControl(HWND hwnd) const {
        return hwnd == paramTopMask_ || hwnd == paramBottomMask_ || hwnd == paramRightMask_;
    }

    COLORREF ParamMaskColor(HWND hwnd) const {
        return hwnd == paramBottomMask_ ? kPanel : kWhite;
    }

    void ApplyParamLayerMasks() {
        if (!hwnd_) return;
        if (paramViewport_) {
            const RECT vp = ParamViewportRect();
            SetWindowPos(paramViewport_, HWND_TOP, vp.left, vp.top, vp.right - vp.left, vp.bottom - vp.top,
                SWP_NOACTIVATE | (page_ == Page::Editor ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        }
        for (HWND h : {paramTopMask_, paramBottomMask_, paramRightMask_}) {
            if (h) ShowWindow(h, SW_HIDE);
        }
        if (page_ != Page::Editor) return;

        for (HWND h : {cancelBtn_, saveBtn_}) {
            if (h && IsWindowVisible(h)) {
                SetWindowPos(h, HWND_TOP, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            }
        }
        if (name_ && IsWindowVisible(name_)) {
            SetWindowPos(name_, HWND_TOP, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }

    bool IsParamScrollManagedHwnd(HWND hwnd) const {
        for (const auto& entry : paramScrollLayout_) {
            if (entry.hwnd == hwnd) return true;
        }
        return false;
    }

    RECT PaintExcludeRectForChild(HWND hwnd) const {
        RECT rc = WindowClientRect(hwnd);
        if (IsParamScrollManagedHwnd(hwnd)) {
            RECT content = ParamScrollContentRect();
            RECT clipped{};
            if (IntersectRect(&clipped, &rc, &content)) return clipped;
            return RECT{};
        }
        return rc;
    }

    bool IsParamPanelCheckbox(HWND hwnd) const {
        if (!hwnd || page_ != Page::Editor) return false;
        wchar_t cls[16]{};
        GetClassNameW(hwnd, cls, 16);
        if (lstrcmpW(cls, L"BUTTON") != 0) return false;
        if ((GetWindowLongW(hwnd, GWL_STYLE) & BS_AUTOCHECKBOX) == 0) return false;
        if (IsGrayButton(hwnd) || IsFooterControl(hwnd)) return false;
        RECT rc = WindowClientRect(hwnd);
        return ParamRectIntersectsContent(rc);
    }

    bool IsFindImageParamEdit(HWND hwnd) const {
        return hwnd == findX1_ || hwnd == findY1_ || hwnd == findX2_ || hwnd == findY2_
            || hwnd == findMatchThreshold_ || hwnd == findScaleMin_ || hwnd == findScaleMax_
            || hwnd == findOffsetX_ || hwnd == findOffsetY_ || hwnd == findMatchVar_;
    }

    bool IsOcrParamEdit(HWND hwnd) const {
        return hwnd == ocrX1_ || hwnd == ocrY1_ || hwnd == ocrX2_ || hwnd == ocrY2_
            || hwnd == ocrSearchEdit_ || hwnd == ocrOffsetX_ || hwnd == ocrOffsetY_
            || hwnd == ocrResultVar_
            || hwnd == ocrFindMatchThreshold_ || hwnd == ocrFindScaleMin_ || hwnd == ocrFindScaleMax_;
    }

    bool IsControlShown(HWND h) const {
        if (!h) return false;
        if (!hwnd_ || !IsWindowVisible(hwnd_)) {
            return (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0;
        }
        return IsWindowVisible(h);
    }

    void CollectVisibleParamControls(std::vector<HWND>& out) const {
        out.clear();
        auto append = [&](const std::vector<HWND>& group) {
            for (HWND h : group) if (IsControlShown(h)) out.push_back(h);
        };
        const int sel = popupAction_.sel;
        switch (sel) {
        case 0: append(moveControls_); break;
        case 1: append(waitControls_); break;
        case 2: append(clickControls_); break;
        case 3: append(mousePlaybackControls_); break;
        case 4: append(runMacroControls_); break;
        case 5: case 6: append(mousePressControls_); break;
        case 7: append(scrollWheelControls_); break;
        case 8: append(keyControls_); break;
        case 9: case 10: append(keyPressControls_); break;
        case 11: append(hotkeyShortcutControls_); break;
        case 12: append(quickInputControls_); break;
        case 13: append(loopControls_); break;
        case 14: append(endLoopControls_); break;
        case 15: append(defineBlockControls_); break;
        case 16: append(runBlockControls_); break;
        case 17:
            append(findImageControls_);
            if (popupFindFollowUp_.sel == 2) append(findImageVarControls_);
            else append(findImageOffsetControls_);
            break;
        case 18:
            append(ocrDepControls_);
            append(ocrFindRegionToggleControls_);
            append(ocrControls_);
            if (ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_)) {
                append(ocrFindRegionControls_);
            }
            append(ocrFollowControls_);
            if (popupOcrResultMode_.sel == 1) append(ocrSearchControls_);
            if (popupOcrFollowUp_.sel == 2) append(ocrFollowVarControls_);
            else append(ocrFollowOffsetControls_);
            break;
        case 19: append(ifControls_); break;
        case 20: append(elseControls_); break;
        case 21: append(lockScreenshotControls_); break;
        case 22: append(unlockScreenshotControls_); break;
        case 23: append(stopMacroControls_); break;
        case 24:
            append(runProgramControls_);
            if (popupRunProgram_.sel <= 0) append(runProgramFileControls_);
            break;
        case 25: append(closeProgramControls_); break;
        case 26: append(openWebpageControls_); break;
        case 27: append(openFileControls_); break;
        case 28: append(timerRecordControls_); break;
        case 32: append(getCursorPosControls_); break;
        case 29: append(aiCommonControls_); append(aiTextControls_); break;
        case 30: append(aiCommonControls_); append(aiImageControls_);
            if (aiRegionByImageCheck_ && Checked(aiRegionByImageCheck_)) append(aiFindRegionControls_);
            break;
        case 31: append(aiCommonControls_); append(aiActionControls_);
            if (aiWithImageCheck_ && Checked(aiWithImageCheck_)
                && aiRegionByImageCheck2_ && Checked(aiRegionByImageCheck2_)) append(aiFindRegionControls_);
            break;
        default: break;
        }
    }

    int ParamPanelBottomClient() const {
        int bottom = 0;
        if (actionCombo_) {
            bottom = std::max(bottom, static_cast<int>(ScaledLayoutClientRect(actionCombo_).bottom));
        }
        std::vector<HWND> controls;
        CollectVisibleParamControls(controls);
        for (HWND h : controls) {
            bottom = std::max(bottom, static_cast<int>(WindowClientRect(h).bottom));
        }
        return bottom;
    }

    RECT ScaledLayoutClientRect(HWND hwnd) const {
        for (const auto& item : editorLayouts_) {
            if (item.hwnd != hwnd) continue;
            const RECT& b = item.base;
            if (IsFindImageHwnd(hwnd)) {
                return RECT{
                    MulDiv(b.left, kEditorWidth, kEditorBaseWidth),
                    MulDiv(b.top, kEditorWidth, kEditorBaseWidth),
                    MulDiv(b.right, kEditorWidth, kEditorBaseWidth),
                    MulDiv(b.bottom, kEditorWidth, kEditorBaseWidth)
                };
            }
            if (IsOcrDepHwnd(hwnd) || IsOcrHwnd(hwnd)) {
                return WindowClientRect(hwnd);
            }
            return RECT{
                MulDiv(b.left, kEditorWidth, kEditorBaseWidth),
                MulDiv(b.top, kEditorHeight, kEditorBaseHeight),
                MulDiv(b.right, kEditorWidth, kEditorBaseWidth),
                MulDiv(b.bottom, kEditorHeight, kEditorBaseHeight)
            };
        }
        return WindowClientRect(hwnd);
    }

    int ParamScrollEditorRightMargin() const {
        return MulDiv(kParamScrollEditorRightMarginDesign, kEditorWidth, kEditorBaseWidth);
    }

    int ParamScrollBarGap() const {
        return MulDiv(kParamScrollBarGapDesign, kEditorWidth, kEditorBaseWidth);
    }

    int ParamScrollContentLeft() const {
        return 920;
    }

    int ParamFieldInset() const {
        return MulDiv(kParamFieldInsetDesign, kEditorWidth, kEditorBaseWidth);
    }

    int ParamPanelLeft() const {
        return ParamScrollContentLeft() + ParamFieldInset();
    }

    int ParamFieldMaxRight() const {
        return ParamScrollContentRight() - ParamFieldInset();
    }

    int ParamFieldMaxWidth() const {
        return std::max(1, ParamFieldMaxRight() - ParamPanelLeft());
    }

    int ParamScrollOuterRight() const {
        return 1175;
    }

    int ParamScrollOuterBottom() const {
        return 980;
    }

    int ParamScrollTrackRightEdge() const {
        return ParamScrollOuterRight();
    }

    RECT ParamScrollTrackRect() const {
        const int right = ParamScrollTrackRightEdge();
        const int left = right - kEditorScrollW;
        const int top = ParamPanelContentTopY() + 2;
        const int bottom = ParamScrollViewportBottomY() - 2;
        if (bottom <= top || right <= left) return RECT{};
        return RECT{left, top, right, bottom};
    }

    int ParamScrollContentRight() const {
        return std::max(ParamScrollContentLeft() + 1,
            ParamScrollOuterRight() - kEditorScrollW - ParamScrollBarGap());
    }

    int ParamScrollContentWidth() const {
        return std::max(1, ParamScrollContentRight() - ParamScrollContentLeft());
    }

    int ParamPanelContentTopY() const {
        return 225;
    }

    int ParamScrollViewportBottomY() const {
        return ParamScrollOuterBottom();
    }

    RECT ParamScrollOuterRect() const {
        return RECT{
            ParamScrollContentLeft(),
            ParamPanelContentTopY(),
            ParamScrollOuterRight(),
            ParamScrollOuterBottom()
        };
    }

    RECT ParamScrollContentRect() const {
        return RECT{
            ParamScrollContentLeft(),
            ParamPanelContentTopY(),
            ParamScrollContentRight(),
            ParamScrollViewportBottomY()
        };
    }

    RECT ParamScrollViewportRect() const {
        return ParamScrollContentRect();
    }

    RECT ParamEditorRedrawRect() const {
        RECT rc = ParamScrollViewportRect();
        RECT track = ParamScrollTrackRect();
        if (track.right > track.left) rc.right = std::max(rc.right, track.right);

        if (actionCombo_) {
            RECT actionRc = WindowClientRect(actionCombo_);
            rc.left = std::min(rc.left, actionRc.left);
            rc.top = std::min(rc.top, actionRc.top - MulDiv(34, kEditorHeight, kEditorBaseHeight));
            rc.right = std::max(rc.right, actionRc.right);
        }
        rc.bottom = std::max(rc.bottom, static_cast<LONG>(ParamScrollViewportBottomY()));
        return rc;
    }

    bool ParamRectIntersectsContent(const RECT& rc) const {
        RECT content = ParamScrollContentRect();
        RECT vis{};
        return IntersectRect(&vis, &rc, &content) != FALSE;
    }

    bool ParamRectIntersectsViewport(const RECT& rc) const {
        return ParamRectIntersectsContent(rc);
    }

    void InvalidateParamScrollChrome() {
        if (!hwnd_) return;
        InvalidateParamPanelArea();
    }

    void InvalidateParamPanelArea() {
        if (!hwnd_) return;
        RECT area = ParamEditorRedrawRect();
        InvalidateRect(hwnd_, &area, FALSE);
        if (paramViewport_ && IsWindowVisible(paramViewport_))
            InvalidateRect(paramViewport_, nullptr, FALSE);
    }

    void HideInactiveParamControls() {
        const int sel = popupAction_.sel;
        const bool aiFindImage = (sel == 30 && aiRegionByImageCheck_ && Checked(aiRegionByImageCheck_))
            || (sel == 31 && aiWithImageCheck_ && Checked(aiWithImageCheck_)
                && aiRegionByImageCheck2_ && Checked(aiRegionByImageCheck2_));
        auto stashGroup = [&](const std::vector<HWND>& group, bool active) {
            if (active) return;
            for (HWND h : group) {
                if (!h) continue;
                ShowWindow(h, SW_HIDE);
                SetWindowRgn(h, nullptr, TRUE);
            }
        };
        stashGroup(moveControls_, sel == 0);
        stashGroup(waitControls_, sel == 1);
        stashGroup(clickControls_, sel == 2);
        stashGroup(mousePlaybackControls_, sel == 3);
        stashGroup(runMacroControls_, sel == 4);
        stashGroup(mousePressControls_, sel == 5 || sel == 6);
        stashGroup(scrollWheelControls_, sel == 7);
        stashGroup(keyControls_, sel == 8);
        stashGroup(keyPressControls_, sel == 9 || sel == 10);
        stashGroup(hotkeyShortcutControls_, sel == 11);
        stashGroup(quickInputControls_, sel == 12);
        stashGroup(loopControls_, sel == 13);
        stashGroup(endLoopControls_, sel == 14);
        stashGroup(defineBlockControls_, sel == 15);
        stashGroup(runBlockControls_, sel == 16);
        stashGroup(findImageControls_, sel == 17);
        stashGroup(findImageOffsetControls_, sel == 17 && popupFindFollowUp_.sel != 2);
        stashGroup(findImageVarControls_, sel == 17 && popupFindFollowUp_.sel == 2);
        stashGroup(ocrDepControls_, sel == 18);
        stashGroup(ocrFindRegionToggleControls_, sel == 18);
        stashGroup(ocrControls_, sel == 18);
        stashGroup(ocrFindRegionControls_, sel == 18 && ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_));
        stashGroup(ocrFollowControls_, sel == 18);
        stashGroup(ocrSearchControls_, sel == 18 && popupOcrResultMode_.sel == 1);
        stashGroup(ocrFollowOffsetControls_, sel == 18 && popupOcrFollowUp_.sel != 2);
        stashGroup(ocrFollowVarControls_, sel == 18 && popupOcrFollowUp_.sel == 2);
        stashGroup(ifControls_, sel == 19);
        stashGroup(elseControls_, sel == 20);
        stashGroup(lockScreenshotControls_, sel == 21);
        stashGroup(unlockScreenshotControls_, sel == 22);
        stashGroup(stopMacroControls_, sel == 23);
        stashGroup(runProgramControls_, sel == 24);
        stashGroup(runProgramFileControls_, sel == 24 && popupRunProgram_.sel <= 0);
        stashGroup(closeProgramControls_, sel == 25);
        stashGroup(openWebpageControls_, sel == 26);
        stashGroup(openFileControls_, sel == 27);
        stashGroup(timerRecordControls_, sel == 28);
        stashGroup(getCursorPosControls_, sel == 32);
        stashGroup(aiCommonControls_, sel == 29 || sel == 30 || sel == 31);
        stashGroup(aiTextControls_, sel == 29);
        stashGroup(aiImageControls_, sel == 30);
        stashGroup(aiActionControls_, sel == 31);
        stashGroup(aiFindRegionControls_, aiFindImage);
    }

    bool IsEditorParamComboHwnd(HWND hwnd) const {
        for (HWND h : {mousePressButton_, clickButton_, loopTypeCombo_, runBlockCombo_, hotkeyShortcutCombo_,
            quickInputVarCombo_, aiVarCombo_, runMacroCombo_, mousePlaybackCombo_, scrollDirectionCombo_, findFollowUpCombo_,
            ocrResultModeCombo_, ocrFollowUpCombo_, ocrSearchVarCombo_, ifVarCombo_, ifOperatorCombo_,
            ifConnectorCombo_, runProgramCombo_, aiModelCombo_, aiContextModeCombo_, aiOutputTypeCombo_,
            aiSearchRegionCombo_}) {
            if (h == hwnd) return true;
        }
        return false;
    }

    void CollectParamScrollControls(std::vector<HWND>& out) const {
        out.clear();
        const int sel = popupAction_.sel;
        auto appendGroup = [&](const std::vector<HWND>& group) {
            for (HWND h : group) {
                if (!h) continue;
                if (std::find(out.begin(), out.end(), h) == out.end()) out.push_back(h);
            }
        };
        switch (sel) {
        case 0: appendGroup(moveControls_); break;
        case 1: appendGroup(waitControls_); break;
        case 2: appendGroup(clickControls_); break;
        case 3: appendGroup(mousePlaybackControls_); break;
        case 4: appendGroup(runMacroControls_); break;
        case 5: case 6: appendGroup(mousePressControls_); break;
        case 7: appendGroup(scrollWheelControls_); break;
        case 8: appendGroup(keyControls_); break;
        case 9: case 10: appendGroup(keyPressControls_); break;
        case 11: appendGroup(hotkeyShortcutControls_); break;
        case 12: appendGroup(quickInputControls_); break;
        case 13: appendGroup(loopControls_); break;
        case 14: appendGroup(endLoopControls_); break;
        case 15: appendGroup(defineBlockControls_); break;
        case 16: appendGroup(runBlockControls_); break;
        case 17:
            appendGroup(findImageControls_);
            if (popupFindFollowUp_.sel == 2) appendGroup(findImageVarControls_);
            else appendGroup(findImageOffsetControls_);
            break;
        case 18:
            appendGroup(ocrDepControls_);
            appendGroup(ocrFindRegionToggleControls_);
            appendGroup(ocrControls_);
            if (ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_)) appendGroup(ocrFindRegionControls_);
            appendGroup(ocrFollowControls_);
            if (popupOcrResultMode_.sel == 1) appendGroup(ocrSearchControls_);
            if (popupOcrFollowUp_.sel == 2) appendGroup(ocrFollowVarControls_);
            else appendGroup(ocrFollowOffsetControls_);
            break;
        case 19: appendGroup(ifControls_); break;
        case 20: appendGroup(elseControls_); break;
        case 21: appendGroup(lockScreenshotControls_); break;
        case 22: appendGroup(unlockScreenshotControls_); break;
        case 23: appendGroup(stopMacroControls_); break;
        case 24:
            appendGroup(runProgramControls_);
            if (popupRunProgram_.sel <= 0) appendGroup(runProgramFileControls_);
            break;
        case 25: appendGroup(closeProgramControls_); break;
        case 26: appendGroup(openWebpageControls_); break;
        case 27: appendGroup(openFileControls_); break;
        case 28: appendGroup(timerRecordControls_); break;
        case 32: appendGroup(getCursorPosControls_); break;
        case 29: appendGroup(aiCommonControls_); appendGroup(aiTextControls_); break;
        case 30:
            appendGroup(aiCommonControls_);
            appendGroup(aiImageControls_);
            if (aiRegionByImageCheck_ && Checked(aiRegionByImageCheck_)) appendGroup(aiFindRegionControls_);
            break;
        case 31:
            appendGroup(aiCommonControls_);
            appendGroup(aiActionControls_);
            if (aiWithImageCheck_ && Checked(aiWithImageCheck_)
                && aiRegionByImageCheck2_ && Checked(aiRegionByImageCheck2_)) appendGroup(aiFindRegionControls_);
            break;
        default: break;
        }
        struct ComboEntry { HWND hwnd; int id; };
        static const ComboEntry kParamCombos[] = {
            {mousePressButton_, 2}, {clickButton_, 3}, {loopTypeCombo_, 4}, {runBlockCombo_, 5},
            {hotkeyShortcutCombo_, 6}, {runMacroCombo_, 8},
            {mousePlaybackCombo_, 9}, {scrollDirectionCombo_, 10}, {findFollowUpCombo_, 11},
            {ifVarCombo_, 12}, {ifOperatorCombo_, 13}, {ifConnectorCombo_, 14},
            {runProgramCombo_, 15}, {ocrResultModeCombo_, 16}, {ocrFollowUpCombo_, 17},
            {ocrSearchVarCombo_, 18}, {aiModelCombo_, 19}, {aiContextModeCombo_, 20},
            {aiOutputTypeCombo_, 21}, {aiSearchRegionCombo_, 22},
        };
        for (const auto& entry : kParamCombos) {
            if (!entry.hwnd || !IsParamComboVisible(entry.id)) continue;
            if (std::find(out.begin(), out.end(), entry.hwnd) == out.end()) out.push_back(entry.hwnd);
        }
        if (IsParamComboVisible(7)) {
            if (HWND varCombo = ActiveVarComboHwnd()) {
                if (std::find(out.begin(), out.end(), varCombo) == out.end()) out.push_back(varCombo);
            }
        }
    }

    void ApplyEditorFooterLayout() {
        if (page_ != Page::Editor) return;

        const int panelLeft = ParamPanelLeft();
        const int maxRight = ParamFieldMaxRight();
        const int labelW = MulDiv(44, kEditorWidth, kEditorBaseWidth);
        const int remarkH = MulDiv(22, kEditorHeight, kEditorBaseHeight);
        const int btnH = MulDiv(30, kEditorHeight, kEditorBaseHeight);
        const int btnW = MulDiv(76, kEditorWidth, kEditorBaseWidth);
        const int gap = MulDiv(12, kEditorHeight, kEditorBaseHeight);
        const int panelTop = ParamPanelContentTopY();
        const int minRemarkY = MulDiv(kEditorRemarkY, kEditorHeight, kEditorBaseHeight);
        const int measuredBottom = paramControlsBottom_ > panelTop ? paramControlsBottom_ + gap : 0;
        const int remarkY = measuredBottom > 0 ? measuredBottom : minRemarkY;
        const int addY = remarkY + remarkH + gap;
        const int btnGap = MulDiv(10, kEditorWidth, kEditorBaseWidth);
        const int addX = maxRight - btnW;
        const int modifyX = addX - btnGap - btnW;
        const int labelX = panelLeft;
        const int remarkX = labelX + labelW + MulDiv(4, kEditorWidth, kEditorBaseWidth);
        const int remarkW = std::max(40, maxRight - remarkX);

        auto moveMaybeViewport = [&](HWND h, int x, int y, int w, int hgt) {
            if (!h) return;
            if (paramViewport_ && GetParent(h) == paramViewport_) {
                MoveWindow(h, x - ParamScrollContentLeft(), y - ParamPanelContentTopY(), w, hgt, FALSE);
            } else {
                MoveWindow(h, x, y, w, hgt, FALSE);
            }
        };

        if (remarkLabel_) {
            ShowWindow(remarkLabel_, SW_SHOW);
            moveMaybeViewport(remarkLabel_, labelX, remarkY, labelW, remarkH);
        }
        if (remark_) {
            ShowWindow(remark_, SW_SHOW);
            moveMaybeViewport(remark_, remarkX, remarkY, remarkW, remarkH);
        }
        if (modifyBtn_) {
            const bool selected = !batchEditMode_ && selectedIndex_ >= 0
                && selectedIndex_ < static_cast<int>(actions_.size());
            ShowWindow(modifyBtn_, selected ? SW_SHOW : SW_HIDE);
            if (selected) moveMaybeViewport(modifyBtn_, modifyX, addY, btnW, btnH);
        }
        if (addBtn_) {
            ShowWindow(addBtn_, SW_SHOW);
            moveMaybeViewport(addBtn_, addX, addY, btnW, btnH);
        }
    }

    void RefreshEditorEdits() {
        if (!hwnd_ || page_ != Page::Editor) return;
        auto refreshEdit = [](HWND child) {
            if (!child || !IsWindowVisible(child)) return;
            wchar_t cls[16]{};
            GetClassNameW(child, cls, 16);
            if (lstrcmpW(cls, L"Edit") != 0) return;
            RedrawWindow(child, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        };
        for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            refreshEdit(child);
        if (paramViewport_) {
            for (HWND child = GetWindow(paramViewport_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
                refreshEdit(child);
        }
    }

    void RefreshGrayButtonsInParamViewport() {
        if (!paramViewport_ || page_ != Page::Editor) return;
        for (HWND child = GetWindow(paramViewport_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!IsGrayButton(child) || !IsWindowVisible(child)) continue;
            RedrawWindow(child, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        }
    }

    void ClearParamScrollTrack(HDC hdc) const {
        RECT track = ParamScrollTrackRect();
        if (track.right <= track.left || track.bottom <= track.top) return;
        FillRectColor(hdc, track, kWhite);
    }

    // OCR / 找图 / AI 等依赖运行时堆叠布局的面板，须在 Reveal 之后刷新，
    // 否则 RevealParamControlsForCapture 会把控件恢复到静态坐标。
    void RefreshDynamicParamLayout() {
        const int sel = popupAction_.sel;
        if (sel == 17) RefreshFindImageSubPanel();
        else if (sel == 18) {
            RefreshOcrDepStatus();
            RefreshOcrSubPanel();
        } else if (sel == 29 || sel == 30 || sel == 31) RefreshAiSubPanel();
    }

    // 参数面板装饰（绿边/下拉框/复选框）统一在 paramViewport_ WM_PAINT 中绘制；
    // 此处仅触发 viewport 重绘，并在主窗口 DC 上画滚动条。
    void RepaintParamPanelChrome() {
        if (!hwnd_ || page_ != Page::Editor) return;
        HDC mainDc = GetDC(hwnd_);
        if (MaxParamScroll() > 0) PaintParamScrollScrollbar(mainDc);
        else ClearParamScrollTrack(mainDc);
        ReleaseDC(hwnd_, mainDc);
        if (paramViewport_) {
            RedrawWindow(paramViewport_, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        }
    }

    void ClearParamViewportSurface() {
        if (!paramViewport_ || page_ != Page::Editor) return;
        InvalidateRect(paramViewport_, nullptr, TRUE);
    }

    void RefreshEditorChildWindows(const RECT* updateRect) {
        if (!hwnd_ || page_ != Page::Editor) return;
        auto refreshChild = [&](HWND child) {
            if (!child || !IsWindowVisible(child)) return;
            if (updateRect) {
                wchar_t cls[16]{};
                GetClassNameW(child, cls, 16);
                if (lstrcmpW(cls, L"Edit") != 0) {
                    RECT rc = WindowClientRect(child);
                    RECT inter{};
                    if (!IntersectRect(&inter, &rc, updateRect)) return;
                }
            }
            RedrawWindow(child, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        };
        for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            refreshChild(child);
        if (paramViewport_) {
            for (HWND child = GetWindow(paramViewport_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
                refreshChild(child);
        }
    }

    void SyncParamScrollLayout() {
        CaptureParamScrollBaseLayout();
        paramScrollY_ = std::clamp(paramScrollY_, 0, MaxParamScroll());
        ApplyParamScrollOffset(false);
        RepaintParamPanelChrome();
    }

    void ConfigureCombo(HWND, int) {}
    void ConfigureEditorCombos() {}

    void CaptureEditorControlLayout() {
        editorLayouts_.clear();
        std::unordered_set<HWND> seen;
        for (HWND h : editorControls_) {
            if (!h || !seen.insert(h).second) continue;
            RECT rc = WindowClientRect(h);
            editorLayouts_.push_back(EditorControlLayout{h, rc});
        }
    }

    bool IsFindImageHwnd(HWND hwnd) const {
        auto contains = [hwnd](const std::vector<HWND>& group) {
            for (HWND h : group) if (h == hwnd) return true;
            return false;
        };
        return contains(findImageControls_) || contains(findImageOffsetControls_) || contains(findImageVarControls_);
    }

    bool IsOcrDepHwnd(HWND hwnd) const {
        return hwnd == ocrDepStatusLabel_ || hwnd == ocrDepInstallBtn_;
    }

    bool IsOcrHwnd(HWND hwnd) const {
        if (IsOcrDepHwnd(hwnd)) return false;
        auto contains = [hwnd](const std::vector<HWND>& group) {
            for (HWND h : group) if (h == hwnd) return true;
            return false;
        };
        return contains(ocrControls_) || contains(ocrSearchControls_) || contains(ocrFollowControls_)
            || contains(ocrFollowOffsetControls_) || contains(ocrFollowVarControls_)
            || contains(ocrFindRegionToggleControls_) || contains(ocrFindRegionControls_);
    }

    bool IsAiDynamicHwnd(HWND hwnd) const {
        const int sel = popupAction_.sel;
        if (sel != 30 && sel != 31) return false;
        auto contains = [hwnd](const std::vector<HWND>& group) {
            for (HWND h : group) if (h == hwnd) return true;
            return false;
        };
        return contains(aiImageControls_) || contains(aiActionControls_)
            || contains(aiFindRegionControls_);
    }

    bool IsEditorListHeaderLabelBase(const RECT& base) const {
        if (base.top != 137) return false;
        return base.left == 32 || base.left == 94 || base.left == 438 || base.left == 631;
    }

    struct FindImageSideButtonLayout {
        int actionX = 0;
        int btnW = 0;
        int compactBtnH = 0;
        int sideGap = 0;
        int stackHeight = 0;

        int StackTop(int previewTop) const {
            return previewTop + std::max(0, (kFindImageSize - stackHeight) / 2);
        }

        int SideBtnY(int previewTop, int index) const {
            return StackTop(previewTop) + index * (compactBtnH + sideGap);
        }
    };

    FindImageSideButtonLayout ComputeFindImageSideButtonLayout() const {
        FindImageSideButtonLayout layout;
        layout.actionX = MulDiv(kFindActionBtnX, kEditorWidth, kEditorBaseWidth);
        layout.btnW = std::max(1, MulDiv(kFindBtnW, kEditorWidth, kEditorBaseWidth));
        layout.compactBtnH = std::max(1, MulDiv(kFindImageSideBtnH, kEditorHeight, kEditorBaseHeight));
        layout.sideGap = std::max(2, MulDiv(kFindImageSideBtnGap, kEditorHeight, kEditorBaseHeight));
        layout.stackHeight = layout.compactBtnH * 3 + layout.sideGap * 2;
        return layout;
    }

    void LayoutFindImageSideStack(
        int previewTop, const FindImageSideButtonLayout& layout,
        HWND screenshot, HWND local, HWND clear) const {
        auto place = [&](HWND h, int index) {
            if (!h) return;
            MoveParamAware(h, layout.actionX, layout.SideBtnY(previewTop, index),
                layout.btnW, layout.compactBtnH, FALSE);
        };
        place(screenshot, 0);
        place(local, 1);
        place(clear, 2);
    }

    void PlaceFindImageCompactButton(HWND btn, int y) const {
        if (!btn) return;
        const FindImageSideButtonLayout layout = ComputeFindImageSideButtonLayout();
        MoveParamAware(btn, layout.actionX, y, layout.btnW, layout.compactBtnH, FALSE);
    }

    int LayoutFindImageMatchScaleRows(int y) const {
        const int left = MulDiv(kFindContentLeft, kEditorWidth, kEditorBaseWidth);
        const int fieldH = MulDiv(22, kEditorHeight, kEditorBaseHeight);
        const int rowGap = MulDiv(kFindVGap, kEditorHeight, kEditorBaseHeight);
        MoveParamAware(findMatchThreshold_, left + MulDiv(91, kEditorWidth, kEditorBaseWidth), y,
            MulDiv(40, kEditorWidth, kEditorBaseWidth), fieldH, FALSE);
        for (HWND h : findImageControls_) {
            if (!h) continue;
            wchar_t buf[16]{};
            GetWindowTextW(h, buf, 16);
            if (wcscmp(buf, L"匹配度大于") == 0) {
                MoveParamAware(h, left, y, MulDiv(90, kEditorWidth, kEditorBaseWidth), fieldH, FALSE);
            } else if (wcscmp(buf, L"%") == 0) {
                MoveParamAware(h, left + MulDiv(135, kEditorWidth, kEditorBaseWidth), y,
                    MulDiv(24, kEditorWidth, kEditorBaseWidth), fieldH, FALSE);
            }
        }
        y += fieldH + rowGap;
        MoveParamAware(findScaleMin_, left + MulDiv(65, kEditorWidth, kEditorBaseWidth), y,
            MulDiv(40, kEditorWidth, kEditorBaseWidth), fieldH, FALSE);
        MoveParamAware(findScaleMax_, left + MulDiv(151, kEditorWidth, kEditorBaseWidth), y,
            MulDiv(40, kEditorWidth, kEditorBaseWidth), fieldH, FALSE);
        for (HWND h : findImageControls_) {
            if (!h) continue;
            wchar_t buf[16]{};
            GetWindowTextW(h, buf, 16);
            if (wcscmp(buf, L"最小缩放") == 0) {
                MoveParamAware(h, left, y, MulDiv(64, kEditorWidth, kEditorBaseWidth), fieldH, FALSE);
            } else if (wcscmp(buf, L"最大") == 0) {
                MoveParamAware(h, left + MulDiv(110, kEditorWidth, kEditorBaseWidth), y,
                    MulDiv(40, kEditorWidth, kEditorBaseWidth), fieldH, FALSE);
            }
        }
        return y + fieldH + rowGap;
    }

    int LayoutFindImageFollowUpRow(int y) const {
        const int left = MulDiv(kFindContentLeft, kEditorWidth, kEditorBaseWidth);
        const int btnH = MulDiv(kFindBtnH, kEditorHeight, kEditorBaseHeight);
        const int rowGap = MulDiv(kFindVGap, kEditorHeight, kEditorBaseHeight);
        const int labelW = MulDiv(kFindFollowLabelW, kEditorWidth, kEditorBaseWidth);
        const int comboW = MulDiv(kFindFollowComboW, kEditorWidth, kEditorBaseWidth);
        const int gap = MulDiv(8, kEditorWidth, kEditorBaseWidth);
        if (findFollowUpLabel_) {
            MoveParamAware(findFollowUpLabel_, left, y, labelW, btnH, FALSE);
            ShowWindow(findFollowUpLabel_, SW_SHOW);
        }
        if (findFollowUpCombo_) MoveParamAware(findFollowUpCombo_, left + labelW + gap, y, comboW, btnH, FALSE);
        return y + btnH + rowGap;
    }

    int LayoutFindImageOffsetBlock(int y) const {
        if (findOffsetXLabel_) ShowWindow(findOffsetXLabel_, SW_SHOW);
        if (findOffsetYLabel_) ShowWindow(findOffsetYLabel_, SW_SHOW);
        if (findMatchVarLabel_) ShowWindow(findMatchVarLabel_, SW_HIDE);
        y = LayoutParamCoordPairRow(y, findOffsetXLabel_, findOffsetX_, findOffsetYLabel_, findOffsetY_, kFindOffsetLabelW);
        const int left = MulDiv(kFindContentLeft, kEditorWidth, kEditorBaseWidth);
        const int btnH = MulDiv(kFindBtnH, kEditorHeight, kEditorBaseHeight);
        const int rowGap = MulDiv(kFindVGap, kEditorHeight, kEditorBaseHeight);
        const int offsetW = MulDiv(kFindSelectOffsetW, kEditorWidth, kEditorBaseWidth);
        const int selectLeft = MulDiv(kFindSelectOffsetLeft - kFindContentLeft, kEditorWidth, kEditorBaseWidth);
        if (findSelectOffsetBtn_) {
            MoveParamAware(findSelectOffsetBtn_, left + selectLeft, y, offsetW, btnH, FALSE);
            ShowWindow(findSelectOffsetBtn_, SW_SHOW);
        }
        y += btnH + rowGap;
        const int fieldH = MulDiv(22, kEditorHeight, kEditorBaseHeight);
        if (findUntilFound_) MoveParamAware(findUntilFound_, left, y, MulDiv(140, kEditorWidth, kEditorBaseWidth), fieldH, FALSE);
        return y + fieldH + rowGap;
    }

    int LayoutFindImageVarBlock(int y) const {
        if (findOffsetXLabel_) ShowWindow(findOffsetXLabel_, SW_HIDE);
        if (findOffsetYLabel_) ShowWindow(findOffsetYLabel_, SW_HIDE);
        if (findSelectOffsetBtn_) ShowWindow(findSelectOffsetBtn_, SW_HIDE);
        if (findUntilFound_) ShowWindow(findUntilFound_, SW_HIDE);
        const int left = MulDiv(kFindContentLeft, kEditorWidth, kEditorBaseWidth);
        const int fieldH = MulDiv(22, kEditorHeight, kEditorBaseHeight);
        const int rowGap = MulDiv(kFindVGap, kEditorHeight, kEditorBaseHeight);
        const int labelW = MulDiv(kFindMatchVarLabelW, kEditorWidth, kEditorBaseWidth);
        const int editW = MulDiv(kFindMatchVarEditW, kEditorWidth, kEditorBaseWidth);
        const int gap = MulDiv(4, kEditorWidth, kEditorBaseWidth);
        if (findMatchVarLabel_) {
            MoveParamAware(findMatchVarLabel_, left, y, labelW, fieldH, FALSE);
            ShowWindow(findMatchVarLabel_, SW_SHOW);
        }
        if (findMatchVar_) MoveParamAware(findMatchVar_, left + labelW + gap, y, editW, fieldH, FALSE);
        return y + fieldH + rowGap;
    }

    int LayoutFindImagePreviewBlockAt(int previewY) const {
        if (!findImagePreviewBtn_) return previewY;
        const FindImageSideButtonLayout layout = ComputeFindImageSideButtonLayout();
        const int previewX = MulDiv(kFindContentLeft, kEditorWidth, kEditorBaseWidth);
        MoveParamAware(findImagePreviewBtn_, previewX, previewY, kFindImageSize, kFindImageSize, FALSE);
        LayoutFindImageSideStack(previewY, layout, findScreenshotBtn_, findLocalImageBtn_, findClearImageBtn_);
        return previewY + kFindImageSize + layout.sideGap;
    }

    int LayoutFindImagePreviewBlock() const {
        if (page_ != Page::Editor || popupAction_.sel != 17) return 0;
        const int previewY = MulDiv(kFindImageRowY, kEditorHeight, kEditorBaseHeight);
        const int afterPreview = LayoutFindImagePreviewBlockAt(previewY);
        return LayoutFindImageMatchScaleRows(afterPreview);
    }


    void ApplyEditorControlScale() {
        for (const auto& item : editorLayouts_) {
            if (item.hwnd == paramViewport_) {
                const RECT vp = ParamViewportRect();
                MoveWindow(paramViewport_, vp.left, vp.top, vp.right - vp.left, vp.bottom - vp.top, FALSE);
                continue;
            }
            if (IsFooterControl(item.hwnd)) continue;
            if (IsFindImageHwnd(item.hwnd)) {
                const int x = MulDiv(item.base.left, kEditorWidth, kEditorBaseWidth);
                const int y = MulDiv(item.base.top, kEditorHeight, kEditorBaseHeight);
                const int w = std::max(1, MulDiv(item.base.right - item.base.left, kEditorWidth, kEditorBaseWidth));
                const int h = std::max(1, MulDiv(item.base.bottom - item.base.top, kEditorHeight, kEditorBaseHeight));
                MoveParamAware(item.hwnd, x, y, w, h, FALSE);
            } else if (IsOcrHwnd(item.hwnd) || IsAiDynamicHwnd(item.hwnd)) {
                // OCR 内容区 / AI 控件由 RefreshOcrSubPanel / RefreshAiSubPanel 统一堆叠布局
            } else {
                const int x = MulDiv(item.base.left, kEditorWidth, kEditorBaseWidth);
                const int y = MulDiv(item.base.top, kEditorHeight, kEditorBaseHeight);
                const int w = std::max(1, MulDiv(item.base.right - item.base.left, kEditorWidth, kEditorBaseWidth));
                int h = std::max(1, MulDiv(item.base.bottom - item.base.top, kEditorHeight, kEditorBaseHeight));
                if (IsEditorListHeaderLabelBase(item.base)) h = std::max(1, kListY - y - 2);
                MoveParamAware(item.hwnd, x, y, w, h, FALSE);
            }
        }
        if (page_ == Page::Editor && popupAction_.sel == 17) {
            RefreshFindImageSubPanel();
        }
        ConfigureEditorCombos();
        if (page_ == Page::Editor && (popupAction_.sel == 18)) {
            RefreshOcrDepStatus();
            RefreshOcrSubPanel();
        }
        if (page_ == Page::Editor && (popupAction_.sel == 29 || popupAction_.sel == 30 || popupAction_.sel == 31)) {
            RefreshAiSubPanel();
        }
        ApplyParamLayerMasks();
    }

    // ── Page navigation ────────────────────────────────────────────
    void ShowHome() {
        CommitInlineRemark();
        ExitBatchEditMode();
        CloseEditorPopup();
        CancelQuickInputTip();
        StopHoverTimer();
        // 清理编辑期间新增但未被任何脚本引用的图片（取消编辑时）
        CleanupNewImages();
        ShowWindow(hwnd_, SW_HIDE);
        page_ = Page::Home;
        ShowEditorControls(false);
        if (hasHomeRectBeforeEditor_) {
            MoveWindow(hwnd_, homeRectBeforeEditor_.left, homeRectBeforeEditor_.top, kHomeWidth, kHomeHeight, FALSE);
            hasHomeRectBeforeEditor_ = false;
        }
        LoadScripts();
        LoadRecordings();
        if (selectedScript_ >= static_cast<int>(scripts_.size())) selectedScript_ = -1;
        if (selectedRecording_ >= static_cast<int>(recordings_.size())) selectedRecording_ = -1;
        ClampHomeScroll();
        ClampRecordingScroll();
        RegisterAllHotkeys();
        InvalidateRect(hwnd_, nullptr, TRUE);
        ShowWindow(hwnd_, SW_SHOW);
    }

    /// 清理编辑期间新增但未被任何脚本引用的图片（取消编辑或退出时调用）
    void CleanupNewImages() {
        if (newImagePaths_.empty()) return;
        const auto allRefs = CollectAllReferencedImages();
        for (const auto& path : newImagePaths_) {
            if (allRefs.find(path) == allRefs.end()) {
                DeleteFileW(path.c_str());
            }
        }
        newImagePaths_.clear();
    }

    void ShowEditorFor(int index, bool createNew) {
        CloseEditorPopup();
        RECT homeRc{};
        if (page_ == Page::Home && GetWindowRect(hwnd_, &homeRc)) {
            homeRectBeforeEditor_ = homeRc;
            hasHomeRectBeforeEditor_ = true;
        } else {
            GetWindowRect(hwnd_, &homeRc);
        }
        ShowWindow(hwnd_, SW_HIDE);
        page_ = Page::Editor;
        HideEditorComboHwnds();
        RECT editorRc = EditorRectFromHome(homeRc);
        MoveWindow(hwnd_, editorRc.left, editorRc.top, kEditorWidth, kEditorHeight, FALSE);
        ShowEditorControls(true);
        if (createNew) {
            currentScriptIndex_ = -1;
            currentPath_.clear();
            currentRecordTime_ = NowText();
            SetText(name_, TimestampName());
            actions_.clear();
        } else if (index >= 0 && index < static_cast<int>(scripts_.size())) {
            currentScriptIndex_ = index;
            currentPath_ = scripts_[static_cast<size_t>(index)].path;
            LoadScriptFile(currentPath_);
        }
        selectedIndex_ = -1; hoverIndex_ = -1; scrollOffset_ = 0; editingRemarkIndex_ = -1;
        collapsedContainers_.clear();
        batchEditMode_ = false;
        batchSelected_.clear();
        ShowWindow(listRemarkEdit_, SW_HIDE);
        ApplyEditorControlScale();
        ApplyEditorFonts();
        RefreshParamPanel();
        RefreshRunBlockCombo();
        UpdateBatchToolbar();
        UpdateEditMode();
        InvalidateToolbarArea();
        StartHoverTimer();
        InvalidateRect(hwnd_, nullptr, TRUE);
        ShowWindow(hwnd_, SW_SHOW);
        ApplyParamLayerMasks();
        CaptureParamScrollBaseLayout();
        ApplyParamScrollOffset(false);
        RepaintParamPanelChrome();
        UpdateWindow(hwnd_);
    }

    // ── Action form ↔ UI binding ───────────────────────────────────
    void SetPopupSel(PopupCombo& pc, HWND label, int sel) {
        pc.sel = sel;
        SetText(label, sel >= 0 && sel < static_cast<int>(pc.items.size()) ? pc.items[static_cast<size_t>(sel)] : L"");
    }

    void SyncEditorPopupLayer() {
        // 下拉展开时保持参数面板布局不变，弹层在 Paint 中叠加绘制
    }

    void RefreshParamPanel() {
        const int sel = popupAction_.sel;
        if (sel != 12) CancelQuickInputTip();

        // 先隐藏所有 Combo 标签，再由 ShowParamLayout 显示正确的
        HideEditorComboHwnds();

        // ── 主面板切换 (基于布局索引) ──
        for (int i = 0; i <= 31; ++i) {
            if (i == 29) continue;
            ShowParamLayout(i, i == sel);
        }
        ShowParamLayout(29, sel == 29 || sel == 30 || sel == 31);
        ShowParamLayout(300, sel == 30);
        ShowParamLayout(310, sel == 31);

        // ── 共享布局 (sel 5/6 共用; 9/10 共用) ──
        ShowParamLayout(5, sel == 5 || sel == 6);
        ShowParamLayout(6, sel == 5 || sel == 6);
        ShowParamLayout(9, sel == 9 || sel == 10);
        ShowParamLayout(10, sel == 9 || sel == 10);

        // ── 找图子面板 (位置 170/171) ──
        if (sel == 17) {
            RefreshFindImageSubPanel();
            if (!loadingForm_ && selectedIndex_ < 0) EnsureFindImageRegionDefaults();
        } else {
            ShowParamLayout(170, false);
            ShowParamLayout(171, false);
        }

        // ── 文字识别子面板 (位置 180-186) ──
        ShowParamLayout(180, sel == 18);
        ShowParamLayout(181, sel == 18);
        ShowParamLayout(18, sel == 18);
        ShowParamLayout(183, sel == 18);
        if (sel == 18) {
            SetPopupSel(popupOcrResultMode_, ocrResultModeCombo_,
                std::clamp(popupOcrResultMode_.sel, 0, static_cast<int>(popupOcrResultMode_.items.size()) - 1));
            SetPopupSel(popupOcrFollowUp_, ocrFollowUpCombo_,
                std::clamp(popupOcrFollowUp_.sel, 0, static_cast<int>(popupOcrFollowUp_.items.size()) - 1));
            RefreshOcrDepStatus();
            RefreshOcrSubPanel();
            if (!loadingForm_ && selectedIndex_ < 0) EnsureOcrRegionDefaults();
        } else {
            ShowParamLayout(182, false);
            ShowParamLayout(184, false);
            ShowParamLayout(185, false);
            ShowParamLayout(186, false);
        }

        // ── 打开程序文件子面板 ──
        ShowParamLayout(240, sel == 24 && popupRunProgram_.sel <= 0);

        // ── AI 子面板 ──
        if (sel == 29 || sel == 30 || sel == 31) {
            RefreshAiModelCombo();
            SetPopupSel(popupAiContextMode_, aiContextModeCombo_, std::clamp(popupAiContextMode_.sel, 0, 3));
            if (sel != 31) {
                SetPopupSel(popupAiOutputType_, aiOutputTypeCombo_, std::clamp(popupAiOutputType_.sel, 0, 1));
            }
            RefreshAiSubPanel();
            if (!loadingForm_ && selectedIndex_ < 0) EnsureAiRegionDefaults();
        } else {
            ParkParamGroup(aiCommonControls_);
            ParkParamGroup(aiImageControls_);
            ParkParamGroup(aiActionControls_);
            HideAiFindRegionControls();
        }

        // ── 底部操作（备注/保存/取消） ──
        if (sel != 17 && sel != 18 && sel != 30 && sel != 31) ClearGrayButtonHover();
        if (sel == 3) RefreshMousePlaybackCombo();
        if (sel == 4) RefreshRunMacroCombo();
        if (sel == 16) RefreshRunBlockCombo();
        if (sel == 12 || sel == 29 || sel == 30 || sel == 31) {
            SyncSharedVarComboVisibility();
            RefreshActiveVarCombo();
        }
        if (sel == 18) RefreshOcrSearchVarCombo();
        if (sel == 19) RefreshIfVarCombo();

        UpdateMoveVarControls();
        UpdateLoopVarControls();
        paramScrollY_ = 0;
        ApplyParamScroll();
        if (hwnd_) {
            RECT listRc = ActionListRect();
            InvalidateRect(hwnd_, &listRc, TRUE);
            RedrawWindow(hwnd_, &listRc, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        }
    }

    struct ParamScrollLayoutEntry {
        HWND hwnd = nullptr;
        int baseX = 0;
        int baseY = 0;
        int baseW = 0;
        int baseH = 0;
    };

    void EnsureParamViewportParent(HWND h) const {
        AttachToParamViewport(h);
    }

    void ClearParamScrollClipping() {
        for (const auto& entry : paramScrollLayout_) {
            if (entry.hwnd) SetWindowRgn(entry.hwnd, nullptr, TRUE);
        }
    }

    RECT ParamScrollThumbRect() const {
        RECT track = ParamScrollTrackRect();
        const int maxScroll = MaxParamScroll();
        if (maxScroll <= 0 || track.bottom <= track.top) return track;
        const int trackH = track.bottom - track.top;
        const int vpTop = static_cast<int>(ParamScrollViewportRect().top);
        const int contentH = std::max(1, paramContentBottom_ - vpTop);
        const int viewH = std::max(1, static_cast<int>(ParamScrollViewportRect().bottom - ParamScrollViewportRect().top));
        const int thumbH = std::max(32, trackH * viewH / contentH);
        const int range = std::max(1, trackH - thumbH);
        const int top = track.top + range * paramScrollY_ / maxScroll;
        return RECT{track.left, top, track.right, top + thumbH};
    }

    void UpdateParamScrollFromThumb(int thumbTop) {
        RECT track = ParamScrollTrackRect();
        RECT thumb = ParamScrollThumbRect();
        const int maxScroll = MaxParamScroll();
        const int trackHeight = static_cast<int>(track.bottom - track.top);
        const int thumbHeight = static_cast<int>(thumb.bottom - thumb.top);
        const int range = std::max(1, trackHeight - thumbHeight);
        const int thumbOffset = thumbTop - static_cast<int>(track.top);
        paramScrollY_ = std::clamp(thumbOffset * maxScroll / range, 0, maxScroll);
    }

    void InvalidateParamScrollArea() {
        InvalidateParamScrollChrome();
    }

    void CaptureParamScrollBaseLayout() {
        paramScrollLayout_.clear();
        paramControlsBottom_ = ParamPanelContentTopY();
        paramContentBottom_ = ParamPanelContentTopY();
        std::vector<HWND> controls;
        CollectParamScrollControls(controls);
        for (HWND h : controls) {
            if (!h) continue;
            if (!IsControlShown(h) && !IsEditorParamComboHwnd(h)) continue;
            RECT rc = WindowClientRect(h);
            if (IsFooterControl(h)) continue;
            const int maxRight = ParamFieldMaxRight();
            if (rc.left < maxRight && rc.right > maxRight) {
                SetParamPosAware(h, rc.left, rc.top, maxRight - rc.left, rc.bottom - rc.top,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                rc.right = maxRight;
            }
            EnsureParamViewportParent(h);
            paramScrollLayout_.push_back(ParamScrollLayoutEntry{
                h, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top
            });
            paramControlsBottom_ = std::max(paramControlsBottom_, static_cast<int>(rc.bottom));
            paramContentBottom_ = std::max(paramContentBottom_, static_cast<int>(rc.bottom));
        }

        ApplyEditorFooterLayout();

        for (HWND h : {remarkLabel_, remark_, modifyBtn_, addBtn_}) {
            if (!h || !IsControlShown(h)) continue;
            RECT rc = WindowClientRect(h);
            const int maxRight = ParamFieldMaxRight();
            if (rc.left < maxRight && rc.right > maxRight) {
                SetParamPosAware(h, rc.left, rc.top, maxRight - rc.left, rc.bottom - rc.top,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                rc.right = maxRight;
            }
            EnsureParamViewportParent(h);
            paramScrollLayout_.push_back(ParamScrollLayoutEntry{
                h, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top
            });
            paramContentBottom_ = std::max(paramContentBottom_, static_cast<int>(rc.bottom));
        }
    }

    bool IsIntentionallyHiddenParamControl(HWND h) const {
        if (!h) return false;
        const int sel = popupAction_.sel;
        if (h == quickInputVarCombo_ && sel != 12) return true;
        if (h == aiVarCombo_ && sel != 29 && sel != 30 && sel != 31) return true;
        if (sel == 31) {
            if (h == aiOutputVarLabel_ || h == aiOutputVarEdit_
                || h == aiOutputTypeLabel_ || h == aiOutputTypeCombo_) {
                return true;
            }
        }
        if (sel != 31) {
            if (h == aiWithImageCheck_ || h == aiMaxStepsLabel_ || h == aiMaxStepsEdit_
                || h == aiMaxStepsHint_) {
                return true;
            }
        }
        const bool aiFindImage = (sel == 30 && aiRegionByImageCheck_ && Checked(aiRegionByImageCheck_))
            || (sel == 31 && aiWithImageCheck_ && Checked(aiWithImageCheck_)
                && aiRegionByImageCheck2_ && Checked(aiRegionByImageCheck2_));
        if (aiFindImage) {
            if (h == aiRegionLabel_ || h == aiFullScreenBtn_ || h == aiSelectRegionBtn_
                || h == aiActionRegionLabel_ || h == aiFullScreenBtn2_ || h == aiSelectRegionBtn2_) {
                return true;
            }
        }
        return false;
    }

    void RevealParamControlsForCapture() {
        std::vector<HWND> controls;
        CollectParamScrollControls(controls);
        for (HWND h : controls) {
            if (!h || IsIntentionallyHiddenParamControl(h)) continue;
            ShowWindow(h, SW_SHOW);
            SetWindowRgn(h, nullptr, TRUE);
        }
    }

    void ApplyParamScrollOffset(bool repaintChrome = true, bool eraseViewport = true, bool hideOffscreen = true) {
        const RECT contentVp = ParamScrollContentRect();
        // 滚动/重排前先擦掉 viewport 底色，避免 SWP_NOCOPYBITS 移位后在原位置留下 GDI 残留
        if (eraseViewport && paramViewport_ && page_ == Page::Editor && !paramScrollLayout_.empty()) {
            RedrawWindow(paramViewport_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ERASENOW | RDW_NOCHILDREN);
        }
        HDWP hdwp = paramScrollLayout_.empty()
            ? nullptr
            : BeginDeferWindowPos(static_cast<UINT>(paramScrollLayout_.size()));
        for (const auto& entry : paramScrollLayout_) {
            if (!entry.hwnd) continue;
            EnsureParamViewportParent(entry.hwnd);
            const int y = entry.baseY - paramScrollY_;
            const RECT ctrl{entry.baseX, y, entry.baseX + entry.baseW, y + entry.baseH};
            RECT visible{};
            if (hideOffscreen && !IntersectRect(&visible, &ctrl, &contentVp)) {
                ShowWindow(entry.hwnd, SW_HIDE);
                SetWindowRgn(entry.hwnd, nullptr, TRUE);
                if (hdwp) {
                    hdwp = DeferWindowPos(hdwp, entry.hwnd, nullptr, -5000, -5000, entry.baseW, entry.baseH,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                } else {
                    SetParamPosAware(entry.hwnd, -5000, -5000, entry.baseW, entry.baseH,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                }
                continue;
            }
            SetWindowRgn(entry.hwnd, nullptr, TRUE);
            const RECT vp = ParamViewportRect();
            const int targetX = GetParent(entry.hwnd) == paramViewport_ ? entry.baseX - vp.left : entry.baseX;
            const int targetY = GetParent(entry.hwnd) == paramViewport_ ? y - vp.top : y;
            if (hdwp) {
                hdwp = DeferWindowPos(hdwp, entry.hwnd, nullptr, targetX, targetY, entry.baseW, entry.baseH,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
            } else {
                SetWindowPos(entry.hwnd, nullptr, targetX, targetY, entry.baseW, entry.baseH,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
            }
            ShowWindow(entry.hwnd, SW_SHOWNA);
        }
        if (hdwp) EndDeferWindowPos(hdwp);
        ApplyParamLayerMasks();
        if (editorPopupOpen_ >= 0) SyncEditorDropPopup();
        if (quickInputTipShown_ != QuickInputTipKind::None) SyncQuickInputTipPopup();
        if (repaintChrome) RepaintParamPanelChrome();
    }

    void RestoreParamPanelLayout() {
        for (const auto& item : editorLayouts_) {
            if (item.hwnd == paramViewport_) {
                const RECT vp = ParamViewportRect();
                MoveWindow(paramViewport_, vp.left, vp.top, vp.right - vp.left, vp.bottom - vp.top, FALSE);
                continue;
            }
            if (IsFooterControl(item.hwnd)) continue;
            if (IsOcrHwnd(item.hwnd)) continue;
            if (IsAiDynamicHwnd(item.hwnd)) continue;
            const int x = MulDiv(item.base.left, kEditorWidth, kEditorBaseWidth);
            const int y = MulDiv(item.base.top, kEditorHeight, kEditorBaseHeight);
            const int w = std::max(1, MulDiv(item.base.right - item.base.left, kEditorWidth, kEditorBaseWidth));
            int h = std::max(1, MulDiv(item.base.bottom - item.base.top, kEditorHeight, kEditorBaseHeight));
            if (IsEditorListHeaderLabelBase(item.base)) h = std::max(1, kListY - y - 2);
            MoveParamAware(item.hwnd, x, y, w, h, FALSE);
        }
    }

    void RebuildParamPanelLayout() {
        ClearParamScrollClipping();
        HideInactiveParamControls();
        RestoreParamPanelLayout();
        RevealParamControlsForCapture();
        RefreshDynamicParamLayout();
        SyncSharedVarComboVisibility();
        CaptureParamScrollBaseLayout();
        paramScrollY_ = std::clamp(paramScrollY_, 0, MaxParamScroll());
        ApplyParamScrollOffset(false);
        ApplyParamLayerMasks();
        RepaintParamPanelChrome();
    }

    void RestoreEditorAfterScreenOverlay() {
        SetWindowPos(hwnd_, HWND_TOP,
            findRegionSavedRect_.left, findRegionSavedRect_.top,
            findRegionSavedRect_.right - findRegionSavedRect_.left,
            findRegionSavedRect_.bottom - findRegionSavedRect_.top,
            SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd_);
        if (page_ != Page::Editor) {
            InvalidateRect(hwnd_, nullptr, TRUE);
            UpdateWindow(hwnd_);
            return;
        }
        RebuildParamPanelLayout();
        InvalidateRect(hwnd_, nullptr, TRUE);
        if (paramViewport_) InvalidateRect(paramViewport_, nullptr, TRUE);
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }

    void ParkParamControl(HWND h) const {
        if (!h) return;
        ShowWindow(h, SW_HIDE);
        SetWindowRgn(h, nullptr, TRUE);
        MoveParamAware(h, -5000, -5000, 1, 1, FALSE);
    }

    void ParkParamGroup(const std::vector<HWND>& group) const {
        for (HWND h : group) ParkParamControl(h);
    }

    void HideAiFindRegionControls() const {
        ParkParamGroup(aiFindRegionControls_);
    }

    void ApplyParamScroll() {
        RebuildParamPanelLayout();
    }

    void ScrollParamPanel(int deltaY) {
        const int maxScroll = MaxParamScroll();
        if (maxScroll <= 0) return;
        const int oldScroll = paramScrollY_;
        paramScrollY_ = std::clamp(paramScrollY_ + deltaY, 0, maxScroll);
        if (oldScroll != paramScrollY_) ApplyParamScrollOffset(true);
    }

    int MaxParamScroll() const {
        if (paramContentBottom_ <= 0) return 0;
        const int vpBottom = static_cast<int>(ParamScrollContentRect().bottom);
        return std::max(0, paramContentBottom_ - vpBottom);
    }

    void UpdateMoveVarControls() {
        const bool fromVar = moveFromVar_ && Checked(moveFromVar_);
        if (moveX_) EnableWindow(moveX_, fromVar ? FALSE : TRUE);
        if (moveY_) EnableWindow(moveY_, fromVar ? FALSE : TRUE);
        if (moveVarX_) EnableWindow(moveVarX_, fromVar ? TRUE : FALSE);
        if (moveVarY_) EnableWindow(moveVarY_, fromVar ? TRUE : FALSE);
    }

    void UpdateLoopVarControls() {
        const bool fromVar = loopFromVar_ && Checked(loopFromVar_);
        if (loopCount_) EnableWindow(loopCount_, fromVar ? FALSE : TRUE);
        if (loopVarExpr_) EnableWindow(loopVarExpr_, fromVar ? TRUE : FALSE);
    }

    void HideEditorComboHwnds() {
        for (HWND h : {mode_, actionCombo_, mousePressButton_, clickButton_, loopTypeCombo_, runBlockCombo_, hotkeyShortcutCombo_, quickInputVarCombo_, aiVarCombo_, runMacroCombo_, mousePlaybackCombo_, scrollDirectionCombo_, findFollowUpCombo_, ocrResultModeCombo_, ocrFollowUpCombo_, ocrSearchVarCombo_, ifVarCombo_, ifOperatorCombo_, ifConnectorCombo_, runProgramCombo_, aiModelCombo_, aiContextModeCombo_, aiOutputTypeCombo_, aiSearchRegionCombo_}) {
            if (h) ShowWindow(h, SW_HIDE);
        }
    }

    void RefreshFindImageSubPanel() {
        const bool saveVar = popupFindFollowUp_.sel == 2;
        ShowParamLayout(170, !saveVar);
        ShowParamLayout(171, saveVar);
        const int rowY = MulDiv(kFindRegionRowY, kEditorHeight, kEditorBaseHeight);
        LayoutParamRegionButtons(findRegionLabel_, findFullScreenBtn_, findSelectRegionBtn_, rowY);
        int y = LayoutFindImagePreviewBlock();
        y = LayoutFindImageFollowUpRow(y);
        if (saveVar) LayoutFindImageVarBlock(y);
        else LayoutFindImageOffsetBlock(y);
        ShowFindImageSideControls(
            findImagePreviewBtn_, findScreenshotBtn_, findLocalImageBtn_, findClearImageBtn_);
        RefreshGrayButtonsInParamViewport();
        SyncParamScrollLayout();
    }

    void SizeFindFullScreenButton() {
        if (!findFullScreenBtn_ || !findSelectRegionBtn_ || !findRegionLabel_) return;
        HDC hdc = GetDC(hwnd_);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, editorFont_ ? editorFont_ : font_));
        SIZE labelSz{}, fullSz{};
        GetTextExtentPoint32W(hdc, L"找图区域", 4, &labelSz);
        GetTextExtentPoint32W(hdc, L"全图", 2, &fullSz);
        SelectObject(hdc, old);
        ReleaseDC(hwnd_, hdc);
        const int labelW = std::max(kFindRegionLabelW, static_cast<int>(labelSz.cx) + 4);
        const int selectX = kFindSelectRegionX;
        const int fullX = kFindContentLeft + labelW + kFindRegionLabelGap;
        const int fullWMax = selectX - kFindRegionBtnGap - fullX;
        const int fullWWant = std::max(32, static_cast<int>(fullSz.cx) + 10);
        const int fullW = std::max(32, std::min(fullWMax, fullWWant));
        MoveParamAware(findRegionLabel_, kFindContentLeft, kFindRegionRowY, labelW, kFindBtnH, FALSE);
        MoveParamAware(findFullScreenBtn_, fullX, kFindRegionRowY, fullW, kFindBtnH, FALSE);
        MoveParamAware(findSelectRegionBtn_, selectX, kFindRegionRowY, kFindBtnW, kFindBtnH, FALSE);
    }

    static int OcrScaleX(int designPx) {
        return MulDiv(designPx, kEditorWidth, kEditorBaseWidth);
    }

    static int OcrScaleY(int designPx) {
        return MulDiv(designPx, kEditorHeight, kEditorBaseHeight);
    }

    static int OcrScale(int designPx) { return OcrScaleX(designPx); }

    void MoveOcrAt(HWND hwnd, int xDesign, int yClient, int wDesign, int hDesign) const {
        if (!hwnd) return;
        EnsureParamViewportParent(hwnd);
        int w = std::max(1, OcrScaleX(wDesign));
        int h = std::max(1, OcrScaleY(hDesign));
        const int x = OcrScaleX(xDesign);
        if (wDesign == kFindImageSize && hDesign == kFindImageSize) {
            w = h = kFindImageSize;
        }
        const int maxRight = ParamScrollContentRight();
        if (x < maxRight && x + w > maxRight) {
            w = std::max(1, maxRight - x);
            if (wDesign == kFindImageSize && hDesign == kFindImageSize) h = w;
        }
        SetParamPosAware(hwnd, x, yClient, w, h,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    }

    int OcrTextButtonWidthDesign(HDC hdc, HFONT font, const wchar_t* text, int minDesignPx, int padDesignPx) const {
        HFONT old = static_cast<HFONT>(SelectObject(hdc, font));
        SIZE sz{};
        GetTextExtentPoint32W(hdc, text, static_cast<int>(wcslen(text)), &sz);
        SelectObject(hdc, old);
        return std::max(minDesignPx, static_cast<int>(sz.cx) + padDesignPx);
    }

    void LayoutOcrFindImageSideStack(int previewTop, HWND screenshot, HWND local, HWND clear) const {
        const FindImageSideButtonLayout layout = ComputeFindImageSideButtonLayout();
        auto place = [&](HWND h, int index) {
            if (!h) return;
            MoveOcrAt(h, kFindActionBtnX, layout.SideBtnY(previewTop, index), kFindBtnW, kFindImageSideBtnH);
        };
        place(screenshot, 0);
        place(local, 1);
        place(clear, 2);
    }

    void PlaceOcrFindImageCompactButton(HWND btn, int yClient) const {
        if (!btn) return;
        MoveOcrAt(btn, kFindActionBtnX, yClient, kFindBtnW, kFindImageSideBtnH);
    }

    void PlaceAiFindImageCompactButton(HWND btn, int yClient) const {
        if (!btn) return;
        MoveAiRegionAt(btn, kFindActionBtnX, yClient, kFindBtnW, kFindImageSideBtnH);
    }

    void ShowFindImageSideControls(HWND preview, HWND screenshot, HWND local, HWND clear,
                                   HWND selectRegion = nullptr) const {
        auto show = [](HWND h) {
            if (!h) return;
            ShowWindow(h, SW_SHOW);
            SetWindowRgn(h, nullptr, TRUE);
        };
        show(selectRegion);
        show(preview);
        show(screenshot);
        show(local);
        show(clear);
    }

    void FinishGrayButtonClick(HWND ctrl) {
        if (!ctrl || !IsGrayButton(ctrl)) return;
        SendMessageW(ctrl, BM_SETSTATE, FALSE, 0);
        InvalidateGrayButton(ctrl);
    }

    void LayoutAiFindImageSideStack(int previewTop, HWND screenshot, HWND local, HWND clear) const {
        const FindImageSideButtonLayout layout = ComputeFindImageSideButtonLayout();
        auto place = [&](HWND h, int index) {
            if (!h) return;
            MoveAiRegionAt(h, kFindActionBtnX, layout.SideBtnY(previewTop, index), kFindBtnW, kFindImageSideBtnH);
        };
        place(screenshot, 0);
        place(local, 1);
        place(clear, 2);
    }

    void SizeOcrRegionButtonsAt(int yClient) {
        if (!ocrFullScreenBtn_ || !ocrSelectRegionBtn_ || !ocrRegionLabel_) return;
        HDC hdc = GetDC(hwnd_);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, editorFont_ ? editorFont_ : font_));
        SIZE labelSz{}, fullSz{};
        GetTextExtentPoint32W(hdc, L"识别区域", 4, &labelSz);
        GetTextExtentPoint32W(hdc, L"全图", 2, &fullSz);
        SelectObject(hdc, old);
        ReleaseDC(hwnd_, hdc);
        const int labelW = std::max(kFindRegionLabelW, static_cast<int>(labelSz.cx) + 4);
        const int fullX = kFindContentLeft + labelW + kFindRegionLabelGap;
        const int fullWMax = kFindSelectRegionX - kFindRegionBtnGap - fullX;
        const int fullWWant = std::max(32, static_cast<int>(fullSz.cx) + 10);
        const int fullW = std::max(32, std::min(fullWMax, fullWWant));
        const int btnH = OcrScale(kFindBtnH);
        MoveOcrAt(ocrRegionLabel_, kFindContentLeft, yClient, labelW, kFindBtnH);
        MoveOcrAt(ocrFullScreenBtn_, fullX, yClient, fullW, kFindBtnH);
        MoveOcrAt(ocrSelectRegionBtn_, kFindSelectRegionX, yClient, kFindBtnW, kFindBtnH);
        (void)btnH;
    }

    void SizeOcrRegionButtons() {
        SizeOcrRegionButtonsAt(OcrScale(kOcrRegionRowY));
    }

    bool IsParamComboVisible(int comboId) const {
        const int sel = popupAction_.sel;
        switch (comboId) {
        case 2: return sel == 5 || sel == 6;
        case 3: return sel == 2;
        case 4: return sel == 13;
        case 5: return sel == 16;
        case 6: return sel == 11;
        case 7: return sel == 12 || sel == 29 || sel == 30 || sel == 31;
        case 8: return sel == 4;
        case 9: return sel == 3;
        case 10: return sel == 7;
        case 11: return sel == 17;
        case 12: return sel == 19;
        case 13: return sel == 19;
        case 14: return sel == 19;
        case 15: return sel == 24;
        case 16: return sel == 18;
        case 17: return sel == 18;
        case 18: return sel == 18 && popupOcrResultMode_.sel == 1;
        case 19: return sel == 29 || sel == 30 || sel == 31;
        case 20: return sel == 29 || sel == 30 || sel == 31;
        case 21: return sel == 29 || sel == 30;
        case 22: return false; // region combo removed in favor of region selection button
        default: return false;
        }
    }

    void InitCrosshairDrag() {
        crosshairDrag_.SetOwner(hwnd_);
        crosshairDrag_.SetDragCursor(crosshairDragCursor_);
        crosshairDrag_.ClearButtons();
        crosshairDrag_.RegisterButton(crosshairBtn_, {CrosshairDragMode::Coordinates, nullptr});
        crosshairDrag_.RegisterButton(runProgramCrosshairBtn_, {CrosshairDragMode::ProgramPath, runProgramPath_});
        crosshairDrag_.RegisterButton(closeProgramCrosshairBtn_, {CrosshairDragMode::ProgramPath, closeProgramPath_});
    }

    void InitRunProgramPresets() {
        popupRunProgram_.items.clear();
        for (int i = 0; i < RunProgramPresetCount(); ++i) {
            popupRunProgram_.items.push_back(RunProgramPresetAt(i).label);
        }
        popupRunProgram_.sel = 0;
        if (runProgramCombo_) {
            SetText(runProgramCombo_, popupRunProgram_.items.empty() ? L"选择文件" : popupRunProgram_.items[0]);
        }
    }

    void UpdateRunProgramSubPanel() {
        RefreshParamPanel();
    }

    void RefreshRunBlockCombo() {
        if (!runBlockCombo_) return;
        std::wstring prevText = popupRunBlock_.sel >= 0 && popupRunBlock_.sel < static_cast<int>(popupRunBlock_.items.size())
            ? popupRunBlock_.items[static_cast<size_t>(popupRunBlock_.sel)] : GetText(runBlockCombo_);
        popupRunBlock_.items.clear();
        for (const auto& a : actions_) {
            if (a.type != ActionType::DefineBlock || a.blockName.empty()) continue;
            popupRunBlock_.items.push_back(a.blockName);
        }
        if (!prevText.empty()) {
            int idx = -1;
            for (int i = 0; i < static_cast<int>(popupRunBlock_.items.size()); ++i) {
                if (popupRunBlock_.items[static_cast<size_t>(i)] == prevText) { idx = i; break; }
            }
            popupRunBlock_.sel = std::max(-1, idx);
        } else if (!popupRunBlock_.items.empty()) {
            popupRunBlock_.sel = 0;
        } else {
            popupRunBlock_.sel = -1;
        }
        SetText(runBlockCombo_, popupRunBlock_.sel >= 0 && popupRunBlock_.sel < static_cast<int>(popupRunBlock_.items.size())
            ? popupRunBlock_.items[static_cast<size_t>(popupRunBlock_.sel)] : L"");
    }

    void RefreshRunMacroCombo() {
        if (!runMacroCombo_) return;
        LoadScripts();
        const std::wstring currentName = Trim(GetText(name_));
        const std::wstring prevText = popupRunMacro_.sel >= 0 && popupRunMacro_.sel < static_cast<int>(popupRunMacro_.items.size())
            ? popupRunMacro_.items[static_cast<size_t>(popupRunMacro_.sel)] : GetText(runMacroCombo_);
        popupRunMacro_.items.clear();
        runMacroPaths_.clear();
        for (const auto& script : scripts_) {
            if (script.name.empty()) continue;
            if (!currentName.empty() && script.name == currentName) continue;
            popupRunMacro_.items.push_back(script.name);
            runMacroPaths_.push_back(script.path);
        }
        if (!prevText.empty()) {
            int idx = -1;
            for (int i = 0; i < static_cast<int>(popupRunMacro_.items.size()); ++i) {
                if (popupRunMacro_.items[static_cast<size_t>(i)] == prevText) { idx = i; break; }
            }
            popupRunMacro_.sel = idx;
        } else if (!popupRunMacro_.items.empty()) {
            popupRunMacro_.sel = 0;
        } else {
            popupRunMacro_.sel = -1;
        }
        SetText(runMacroCombo_, popupRunMacro_.sel >= 0 && popupRunMacro_.sel < static_cast<int>(popupRunMacro_.items.size())
            ? popupRunMacro_.items[static_cast<size_t>(popupRunMacro_.sel)] : L"");
    }

    void RefreshMousePlaybackCombo() {
        if (!mousePlaybackCombo_) return;
        LoadRecordings();
        const std::wstring prevText = popupMousePlayback_.sel >= 0 && popupMousePlayback_.sel < static_cast<int>(popupMousePlayback_.items.size())
            ? popupMousePlayback_.items[static_cast<size_t>(popupMousePlayback_.sel)] : GetText(mousePlaybackCombo_);
        popupMousePlayback_.items.clear();
        mousePlaybackPaths_.clear();
        for (const auto& rec : recordings_) {
            if (rec.name.empty()) continue;
            popupMousePlayback_.items.push_back(rec.name);
            mousePlaybackPaths_.push_back(rec.path);
        }
        if (!prevText.empty()) {
            int idx = -1;
            for (int i = 0; i < static_cast<int>(popupMousePlayback_.items.size()); ++i) {
                if (popupMousePlayback_.items[static_cast<size_t>(i)] == prevText) { idx = i; break; }
            }
            popupMousePlayback_.sel = idx;
        } else if (!popupMousePlayback_.items.empty()) {
            popupMousePlayback_.sel = 0;
        } else {
            popupMousePlayback_.sel = -1;
        }
        SetText(mousePlaybackCombo_, popupMousePlayback_.sel >= 0 && popupMousePlayback_.sel < static_cast<int>(popupMousePlayback_.items.size())
            ? popupMousePlayback_.items[static_cast<size_t>(popupMousePlayback_.sel)] : L"");
    }

    int EditorComboPopupIdForHwnd(HWND hwnd) const {
        if (hwnd == mode_) return 0;
        if (hwnd == actionCombo_) return 1;
        if (hwnd == mousePressButton_ && IsParamComboVisible(2)) return 2;
        if (hwnd == clickButton_ && IsParamComboVisible(3)) return 3;
        if (hwnd == loopTypeCombo_ && IsParamComboVisible(4)) return 4;
        if (hwnd == runBlockCombo_ && IsParamComboVisible(5)) return 5;
        if (hwnd == hotkeyShortcutCombo_ && IsParamComboVisible(6)) return 6;
        if (hwnd == quickInputVarCombo_ || hwnd == aiVarCombo_) {
            return hwnd == ActiveVarComboHwnd() ? 7 : -1;
        }
        if (hwnd == runMacroCombo_ && IsParamComboVisible(8)) return 8;
        if (hwnd == mousePlaybackCombo_ && IsParamComboVisible(9)) return 9;
        if (hwnd == scrollDirectionCombo_ && IsParamComboVisible(10)) return 10;
        if (hwnd == findFollowUpCombo_ && IsParamComboVisible(11)) return 11;
        if (hwnd == ifVarCombo_ && IsParamComboVisible(12)) return 12;
        if (hwnd == ifOperatorCombo_ && IsParamComboVisible(13)) return 13;
        if (hwnd == ifConnectorCombo_ && IsParamComboVisible(14)) return 14;
        if (hwnd == runProgramCombo_ && IsParamComboVisible(15)) return 15;
        if (hwnd == ocrResultModeCombo_ && IsParamComboVisible(16)) return 16;
        if (hwnd == ocrFollowUpCombo_ && IsParamComboVisible(17)) return 17;
        if (hwnd == ocrSearchVarCombo_ && IsParamComboVisible(18)) return 18;
        if (hwnd == aiModelCombo_ && IsParamComboVisible(19)) return 19;
        if (hwnd == aiContextModeCombo_ && IsParamComboVisible(20)) return 20;
        if (hwnd == aiOutputTypeCombo_ && IsParamComboVisible(21)) return 21;
        if (hwnd == aiSearchRegionCombo_ && IsParamComboVisible(22)) return 22;
        return -1;
    }

    int EditorComboPopupIdAtPoint(int x, int y) const {
        if (mode_ && PtIn(WindowClientRect(mode_), x, y)) return 0;
        if (actionCombo_ && PtIn(WindowClientRect(actionCombo_), x, y)) return 1;
        struct ComboHit { int id; HWND hwnd; };
        const ComboHit hits[] = {
            {2, mousePressButton_}, {3, clickButton_}, {4, loopTypeCombo_}, {5, runBlockCombo_},
            {6, hotkeyShortcutCombo_}, {8, runMacroCombo_}, {9, mousePlaybackCombo_},
            {10, scrollDirectionCombo_}, {11, findFollowUpCombo_},
            {12, ifVarCombo_}, {13, ifOperatorCombo_}, {14, ifConnectorCombo_}, {15, runProgramCombo_},
            {16, ocrResultModeCombo_}, {17, ocrFollowUpCombo_}, {18, ocrSearchVarCombo_},
            {19, aiModelCombo_}, {20, aiContextModeCombo_}, {21, aiOutputTypeCombo_}, {22, aiSearchRegionCombo_},
        };
        for (const auto& hit : hits) {
            if (!hit.hwnd || !IsParamComboVisible(hit.id)) continue;
            if (PtIn(WindowClientRect(hit.hwnd), x, y)) return hit.id;
        }
        if (IsParamComboVisible(7)) {
            if (HWND varCombo = ActiveVarComboHwnd()) {
                if (PtIn(WindowClientRect(varCombo), x, y)) return 7;
            }
        }
        return -1;
    }

    void InitHotkeyShortcutPresets() {
        popupHotkeyShortcut_.items.clear();
        for (int i = 0; i < ShortcutPresetCount(); ++i) {
            popupHotkeyShortcut_.items.push_back(ShortcutPresetAt(i).label);
        }
        popupHotkeyShortcut_.sel = 0;
        if (hotkeyShortcutCombo_) SetText(hotkeyShortcutCombo_, popupHotkeyShortcut_.items.empty() ? L"" : popupHotkeyShortcut_.items[0]);
    }

    void RebuildQuickInputVarPopup() {
        quickInputVarItems_ = BuildQuickInputVarItems(actions_);
        popupQuickInputVar_.items.clear();
        for (const auto& item : quickInputVarItems_) {
            popupQuickInputVar_.items.push_back(item.display);
        }
        if (popupQuickInputVar_.sel < 0 || popupQuickInputVar_.sel >= static_cast<int>(popupQuickInputVar_.items.size())) {
            popupQuickInputVar_.sel = popupQuickInputVar_.items.empty() ? -1 : 0;
        }
    }

    std::wstring QuickInputVarPopupDisplayText() const {
        return popupQuickInputVar_.sel >= 0 && popupQuickInputVar_.sel < static_cast<int>(popupQuickInputVar_.items.size())
            ? popupQuickInputVar_.items[static_cast<size_t>(popupQuickInputVar_.sel)] : L"";
    }

    void SyncSharedVarComboVisibility() {
        const int sel = popupAction_.sel;
        if (quickInputVarCombo_) {
            ShowWindow(quickInputVarCombo_, sel == 12 ? SW_SHOW : SW_HIDE);
        }
        if (aiVarCombo_) {
            ShowWindow(aiVarCombo_, (sel == 29 || sel == 30 || sel == 31) ? SW_SHOW : SW_HIDE);
        }
    }

    void RefreshActiveVarCombo() {
        RebuildQuickInputVarPopup();
        HWND active = ActiveVarComboHwnd();
        if (!active) return;
        SetText(active, QuickInputVarPopupDisplayText());
        InvalidateEditorComboArea(7);
    }

    void RefreshQuickInputVarCombo() {
        RefreshActiveVarCombo();
    }

    void RefreshIfVarCombo() {
        quickInputVarItems_ = BuildQuickInputVarItems(actions_);
        popupIfVar_.items.clear();
        for (const auto& item : quickInputVarItems_) {
            popupIfVar_.items.push_back(item.display);
        }
        if (popupIfVar_.sel < 0 || popupIfVar_.sel >= static_cast<int>(popupIfVar_.items.size())) {
            popupIfVar_.sel = popupIfVar_.items.empty() ? -1 : 0;
        }
        SetText(ifVarCombo_, popupIfVar_.sel >= 0 && popupIfVar_.sel < static_cast<int>(popupIfVar_.items.size())
            ? popupIfVar_.items[static_cast<size_t>(popupIfVar_.sel)] : L"");
    }

    void AppendIfCondition() {
        if (!ifConditionList_) return;
        const int varSel = popupIfVar_.sel;
        if (varSel < 0 || varSel >= static_cast<int>(quickInputVarItems_.size())) return;
        static const wchar_t* opSymbols[] = { L"==", L"!=", L"<", L"<=", L">", L">=", L">>" };
        const int opSel = std::clamp(popupIfOperator_.sel, 0, 6);
        static const wchar_t* connectors[] = { L"and", L"or", L"not" };
        const int connSel = std::clamp(popupIfConnector_.sel, 0, 2);
        const std::wstring varCode = quickInputVarItems_[static_cast<size_t>(varSel)].codeHint;
        const std::wstring value = Trim(GetText(ifValueEdit_));
        const std::wstring clause = varCode + opSymbols[opSel] + value;
        std::wstring current = GetText(ifConditionList_);
        if (Trim(current).empty()) {
            SetText(ifConditionList_, clause);
        } else {
            while (!current.empty() && (current.back() == L'\r' || current.back() == L'\n')) current.pop_back();
            current += L" ";
            current += connectors[connSel];
            current += L"\r\n";
            current += clause;
            SetText(ifConditionList_, current);
        }
        SetFocus(ifConditionList_);
    }

    void OnActionsChanged() {
        if (editorPopupOpen_ == 7) CloseEditorPopup();
        const int sel = popupAction_.sel;
        if (sel == 12 || sel == 29 || sel == 30 || sel == 31) RefreshActiveVarCombo();
        if (sel == 18) RefreshOcrSearchVarCombo();
        if (sel == 19) RefreshIfVarCombo();
        if (page_ == Page::Editor) {
            SyncSharedVarComboVisibility();
            RepaintParamPanelChrome();
        }
    }

    HWND ActiveVarComboHwnd() const {
        const int sel = popupAction_.sel;
        if (sel == 12) return quickInputVarCombo_;
        if (sel == 29 || sel == 30 || sel == 31) return aiVarCombo_;
        return nullptr;
    }

    void InsertTextAtEditSelection(HWND edit, const std::wstring& insert) {
        if (!edit || insert.empty()) return;
        SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(insert.c_str()));
        SetFocus(edit);
    }

    void InsertQuickInputVariable() {
        if (!quickInputEdit_) return;
        const int sel = popupQuickInputVar_.sel;
        if (sel < 0 || sel >= static_cast<int>(quickInputVarItems_.size())) return;
        InsertTextAtEditSelection(quickInputEdit_, quickInputVarItems_[static_cast<size_t>(sel)].insertText);
    }

    void InsertAiPromptVariable() {
        if (!aiPromptEdit_) return;
        const int sel = popupQuickInputVar_.sel;
        if (sel < 0 || sel >= static_cast<int>(quickInputVarItems_.size())) return;
        InsertTextAtEditSelection(aiPromptEdit_, quickInputVarItems_[static_cast<size_t>(sel)].insertText);
    }

    void RefreshAiVarCombo() {
        RefreshActiveVarCombo();
    }

    void RefreshOcrSearchVarCombo() {
        quickInputVarItems_ = BuildQuickInputVarItems(actions_);
        popupOcrSearchVar_.items.clear();
        for (const auto& item : quickInputVarItems_) {
            popupOcrSearchVar_.items.push_back(item.display);
        }
        if (popupOcrSearchVar_.sel < 0 || popupOcrSearchVar_.sel >= static_cast<int>(popupOcrSearchVar_.items.size())) {
            popupOcrSearchVar_.sel = popupOcrSearchVar_.items.empty() ? -1 : 0;
        }
        SetText(ocrSearchVarCombo_, popupOcrSearchVar_.sel >= 0 && popupOcrSearchVar_.sel < static_cast<int>(popupOcrSearchVar_.items.size())
            ? popupOcrSearchVar_.items[static_cast<size_t>(popupOcrSearchVar_.sel)] : L"");
    }

    void RefreshAiModelCombo() {
        popupAiModel_.items.clear();
        const auto& models = appSettings_.ai.savedModels;
        for (const auto& m : models) {
            popupAiModel_.items.push_back(m.modelName);
        }
        if (popupAiModel_.items.empty()) {
            popupAiModel_.items.push_back(appSettings_.ai.modelName);
        }
        if (popupAiModel_.sel < 0 || popupAiModel_.sel >= static_cast<int>(popupAiModel_.items.size())) {
            popupAiModel_.sel = popupAiModel_.items.empty() ? -1 : 0;
        }
        SetText(aiModelCombo_, popupAiModel_.sel >= 0 && popupAiModel_.sel < static_cast<int>(popupAiModel_.items.size())
            ? popupAiModel_.items[static_cast<size_t>(popupAiModel_.sel)] : L"");
    }

    void MoveAiAt(HWND hwnd, int x, int y, int w, int h) const {
        if (hwnd) {
            EnsureParamViewportParent(hwnd);
            SetParamPosAware(hwnd, x, y, w, h,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        }
    }

    void MoveAiAtVisible(HWND hwnd, int x, int y, int w, int h) const {
        MoveAiAt(hwnd, x, y, w, h);
        if (hwnd) ShowWindow(hwnd, SW_SHOW);
    }

    void LayoutParamRegionButtons(HWND regionLabel, HWND fullBtn, HWND selectBtn, int y) {
        if (!regionLabel || !fullBtn || !selectBtn) return;
        const int left = ParamPanelLeft();
        const int maxRight = ParamFieldMaxRight();
        const int btnH = MulDiv(kFindBtnH, kEditorHeight, kEditorBaseHeight);
        const int btnGap = MulDiv(kFindRegionBtnGap, kEditorWidth, kEditorBaseWidth);
        const int labelGap = MulDiv(kFindRegionLabelGap, kEditorWidth, kEditorBaseWidth);
        const int selectW = MulDiv(kFindBtnW, kEditorWidth, kEditorBaseWidth);
        HDC hdc = GetDC(hwnd_);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, editorFont_ ? editorFont_ : font_));
        wchar_t labelBuf[32]{};
        GetWindowTextW(regionLabel, labelBuf, 32);
        SIZE labelSz{}, fullSz{};
        GetTextExtentPoint32W(hdc, labelBuf, static_cast<int>(wcslen(labelBuf)), &labelSz);
        GetTextExtentPoint32W(hdc, L"全图", 2, &fullSz);
        SelectObject(hdc, old);
        ReleaseDC(hwnd_, hdc);
        const int labelW = std::max(MulDiv(kFindRegionLabelW, kEditorWidth, kEditorBaseWidth),
            static_cast<int>(labelSz.cx) + 4);
        const int fullWWant = std::max(44, static_cast<int>(fullSz.cx) + MulDiv(18, kEditorWidth, kEditorBaseWidth));
        const int availW = maxRight - left;
        const int fullWMax = std::max(44, availW - labelW - labelGap - btnGap - selectW);
        const int fullW = std::max(44, std::min(fullWMax, fullWWant));
        const int totalW = labelW + labelGap + fullW + btnGap + selectW;
        const int startX = left + std::max(0, (availW - totalW) / 2);
        MoveParamAware(regionLabel, startX, y, labelW, btnH, FALSE);
        MoveParamAware(fullBtn, startX + labelW + labelGap, y, fullW, btnH, FALSE);
        MoveParamAware(selectBtn, startX + labelW + labelGap + fullW + btnGap, y, selectW, btnH, FALSE);
        ShowWindow(regionLabel, SW_SHOW);
        ShowWindow(fullBtn, SW_SHOW);
        ShowWindow(selectBtn, SW_SHOW);
    }

    int LayoutParamCoordPairRow(
        int y,
        HWND xLabel, HWND xEdit, HWND yLabel, HWND yEdit,
        int labelWDesign = kFindCoordLabelW) const {
        const int left = ParamPanelLeft();
        const int maxRight = ParamFieldMaxRight();
        const int fieldH = MulDiv(22, kEditorHeight, kEditorBaseHeight);
        const int rowGap = MulDiv(kFindVGap, kEditorHeight, kEditorBaseHeight);
        const int labelW = MulDiv(labelWDesign, kEditorWidth, kEditorBaseWidth);
        const int editW = MulDiv(kFindEditW, kEditorWidth, kEditorBaseWidth);
        const int labelEditGap = MulDiv(kFindCoordLabelEditGap, kEditorWidth, kEditorBaseWidth);
        const int pairGap = MulDiv(kFindCoordPairGap, kEditorWidth, kEditorBaseWidth);
        const int pairW = labelW + labelEditGap + editW;
        const int totalW = pairW * 2 + pairGap;
        const int startX = left + std::max(0, (maxRight - left - totalW) / 2);

        int x = startX;
        auto mv = [&](HWND h, int w) {
            if (!h) return;
            MoveParamAware(h, x, y, w, fieldH, FALSE);
            ShowWindow(h, SW_SHOW);
            x += w;
        };
        mv(xLabel, labelW);
        x += labelEditGap;
        mv(xEdit, editW);
        x += pairGap;
        mv(yLabel, labelW);
        x += labelEditGap;
        mv(yEdit, editW);
        return y + fieldH + rowGap;
    }

    int LayoutParamCoordRows(
        int y,
        HWND x1Label, HWND x1Edit, HWND y1Label, HWND y1Edit,
        HWND x2Label, HWND x2Edit, HWND y2Label, HWND y2Edit) {
        y = LayoutParamCoordPairRow(y, x1Label, x1Edit, y1Label, y1Edit);
        y = LayoutParamCoordPairRow(y, x2Label, x2Edit, y2Label, y2Edit);
        return y;
    }

    static int AiScaleX(int designPx) {
        return MulDiv(designPx, kEditorWidth, kEditorBaseWidth);
    }

    static int AiScaleY(int designPx) {
        return MulDiv(designPx, kEditorHeight, kEditorBaseHeight);
    }

    int AiPanelLeft() const { return ParamPanelLeft(); }
    int AiPanelWidth() const { return ParamFieldMaxWidth(); }

    static int AiRegionXDesign(int xFromFindContent) {
        return xFromFindContent + (kParamScrollLeftDesign - kFindContentLeft);
    }

    void MoveAiRegionAt(HWND hwnd, int xDesign, int yClient, int wDesign, int hDesign) const {
        MoveOcrAt(hwnd, AiRegionXDesign(xDesign), yClient, wDesign, hDesign);
    }

    void SizeAiRegionButtonsAt(HWND regionLabel, HWND fullBtn, HWND selectBtn, int yClient) {
        if (!regionLabel || !fullBtn || !selectBtn) return;
        HDC hdc = GetDC(hwnd_);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, editorFont_ ? editorFont_ : font_));
        wchar_t labelBuf[32]{};
        GetWindowTextW(regionLabel, labelBuf, 32);
        SIZE labelSz{}, fullSz{};
        GetTextExtentPoint32W(hdc, labelBuf, static_cast<int>(wcslen(labelBuf)), &labelSz);
        GetTextExtentPoint32W(hdc, L"全图", 2, &fullSz);
        SelectObject(hdc, old);
        ReleaseDC(hwnd_, hdc);
        const int labelW = std::max(kFindRegionLabelW, static_cast<int>(labelSz.cx) + 4);
        const int fullX = kFindContentLeft + labelW + kFindRegionLabelGap;
        const int fullWMax = kFindSelectRegionX - kFindRegionBtnGap - fullX;
        const int fullWWant = std::max(32, static_cast<int>(fullSz.cx) + 10);
        const int fullW = std::max(32, std::min(fullWMax, fullWWant));
        MoveAiRegionAt(regionLabel, kFindContentLeft, yClient, labelW, kFindBtnH);
        MoveAiRegionAt(fullBtn, fullX, yClient, fullW, kFindBtnH);
        MoveAiRegionAt(selectBtn, kFindSelectRegionX, yClient, kFindBtnW, kFindBtnH);
    }

    int LayoutAiFindRegionBlock(int y) {
        const FindImageSideButtonLayout layout = ComputeFindImageSideButtonLayout();
        const int fieldH = OcrScale(22);
        const int rowGap = OcrScale(kFindVGap);

        MoveAiRegionAt(aiFindImagePreviewBtn_, kFindContentLeft, y, kFindImageSize, kFindImageSize);
        LayoutAiFindImageSideStack(y, aiFindScreenshotBtn_, aiFindLocalImageBtn_, aiFindClearImageBtn_);
        ShowFindImageSideControls(
            aiFindImagePreviewBtn_, aiFindScreenshotBtn_, aiFindLocalImageBtn_, aiFindClearImageBtn_);
        y += kFindImageSize + layout.sideGap;

        MoveAiRegionAt(aiFindMatchThreshold_, kFindContentLeft + 91, y, 40, 22);
        if (aiFindMatchLabel_) {
            MoveAiRegionAt(aiFindMatchLabel_, kFindContentLeft, y, 90, 22);
            ShowWindow(aiFindMatchLabel_, SW_SHOW);
        }
        if (aiFindMatchPctLabel_) {
            MoveAiRegionAt(aiFindMatchPctLabel_, kFindContentLeft + 135, y, 24, 22);
            ShowWindow(aiFindMatchPctLabel_, SW_SHOW);
        }
        if (aiFindMatchThreshold_) ShowWindow(aiFindMatchThreshold_, SW_SHOW);
        y += fieldH + rowGap;

        MoveAiRegionAt(aiFindScaleMin_, kFindContentLeft + 65, y, 40, 22);
        MoveAiRegionAt(aiFindScaleMax_, kFindContentLeft + 151, y, 40, 22);
        if (aiFindScaleMinLabel_) {
            MoveAiRegionAt(aiFindScaleMinLabel_, kFindContentLeft, y, 64, 22);
            ShowWindow(aiFindScaleMinLabel_, SW_SHOW);
        }
        if (aiFindScaleMaxLabel_) {
            MoveAiRegionAt(aiFindScaleMaxLabel_, kFindContentLeft + 110, y, 40, 22);
            ShowWindow(aiFindScaleMaxLabel_, SW_SHOW);
        }
        if (aiFindScaleMin_) ShowWindow(aiFindScaleMin_, SW_SHOW);
        if (aiFindScaleMax_) ShowWindow(aiFindScaleMax_, SW_SHOW);
        return y + fieldH + rowGap;
    }

    void LayoutAiPromptHeader(bool actionExecuteMode) {
        if (!aiPromptLabel_) return;
        const int left = ParamPanelLeft();
        const int fullW = ParamFieldMaxWidth();
        const int labelH = MulDiv(kEditorLabelAboveComboH, kEditorHeight, kEditorBaseHeight);
        const int checkW = AiScaleX(52);
        const int sideGap = AiScaleX(8);
        const int top = static_cast<int>(WindowClientRect(aiPromptLabel_).top);
        if (actionExecuteMode) {
            const int labelW = std::max(AiScaleX(80), fullW - checkW - sideGap);
            MoveParamAware(aiPromptLabel_, left, top, labelW, labelH, FALSE);
            if (aiWithImageCheck_) {
                MoveParamAware(aiWithImageCheck_, left + labelW + sideGap, top, checkW, AiScaleY(25), FALSE);
                ShowWindow(aiWithImageCheck_, SW_SHOW);
            }
        } else {
            MoveParamAware(aiPromptLabel_, left, top, fullW, labelH, FALSE);
        }
    }

    void ParkAiActionOnlyCommonFields() {
        for (HWND h : {aiWithImageCheck_, aiMaxStepsLabel_, aiMaxStepsEdit_, aiMaxStepsHint_}) {
            if (h) ParkParamControl(h);
        }
    }

    void EnsureAiCommonEditsVisible() {
        for (HWND h : {aiPromptEdit_, aiFallbackEdit_, aiOutputVarEdit_, aiImageScaleEdit_}) {
            if (!h || IsIntentionallyHiddenParamControl(h)) continue;
            ShowWindow(h, SW_SHOW);
            SetWindowRgn(h, nullptr, TRUE);
        }
    }

    void CompactAiCommonBelowPrompt() {
        if (!aiVarLabel_ || !aiPromptEdit_) return;
        if (!IsWindowVisible(aiPromptEdit_)) return;
        const RECT promptRc = WindowClientRect(aiPromptEdit_);
        if (promptRc.bottom <= promptRc.top) return;
        const int targetTop = static_cast<int>(WindowClientRect(aiPromptEdit_).bottom)
            + MulDiv(8, kEditorHeight, kEditorBaseHeight);
        const int currentTop = static_cast<int>(WindowClientRect(aiVarLabel_).top);
        const int delta = targetTop - currentTop;
        if (delta == 0) return;
        const HWND shiftOrder[] = {
            aiVarLabel_, aiVarCombo_, aiInsertVarBtn_, aiModelLabel_, aiModelCombo_,
            aiContextLabel_, aiContextModeCombo_, aiTimeoutLabel_, aiTimeoutEdit_,
            aiFallbackLabel_, aiFallbackEdit_, aiOutputVarLabel_, aiOutputVarEdit_,
            aiOutputTypeLabel_, aiOutputTypeCombo_
        };
        for (HWND h : shiftOrder) {
            if (!h) continue;
            RECT rc = WindowClientRect(h);
            MoveParamAware(h, rc.left, rc.top + delta, rc.right - rc.left, rc.bottom - rc.top, FALSE);
            ShowWindow(h, SW_SHOW);
        }
    }

    int AiCommonBlockBottomY() const {
        int bottom = ParamPanelContentTopY();
        for (HWND h : aiCommonControls_) {
            if (!h || !IsWindowVisible(h)) continue;
            if (IsIntentionallyHiddenParamControl(h)) continue;
            bottom = std::max(bottom, static_cast<int>(WindowClientRect(h).bottom));
        }
        return bottom;
    }

    void StackParamGroupBelow(const std::vector<HWND>& group, int anchorBottom, int gap) {
        int minTop = INT_MAX;
        for (HWND h : group) {
            if (!h || IsIntentionallyHiddenParamControl(h)) continue;
            RECT rc = WindowClientRect(h);
            if (rc.top < -1000) continue;
            minTop = std::min(minTop, static_cast<int>(rc.top));
        }
        if (minTop == INT_MAX) return;
        const int delta = anchorBottom + gap - minTop;
        if (delta == 0) return;
        for (HWND h : group) {
            if (!h || IsIntentionallyHiddenParamControl(h)) continue;
            RECT rc = WindowClientRect(h);
            if (rc.top < -1000) continue;
            MoveParamAware(h, rc.left, rc.top + delta, rc.right - rc.left, rc.bottom - rc.top, FALSE);
            ShowWindow(h, SW_SHOW);
            SetWindowRgn(h, nullptr, TRUE);
        }
    }

    int LayoutAiRegionSection(
        bool regionByImage,
        HWND regionByImageCheck,
        HWND regionLabel,
        HWND fullBtn,
        HWND selectBtn,
        HWND x1Label, HWND x1Edit, HWND y1Label, HWND y1Edit,
        HWND x2Label, HWND x2Edit, HWND y2Label, HWND y2Edit,
        int y) {
        const int rowGap = OcrScale(kFindVGap);
        const int btnH = OcrScale(kFindBtnH);
        const int fieldH = OcrScale(22);

        if (regionByImageCheck) {
            MoveParamAware(regionByImageCheck, ParamPanelLeft(), y, MulDiv(kOcrRegionByImageW, kEditorWidth, kEditorBaseWidth), fieldH, FALSE);
            ShowWindow(regionByImageCheck, SW_SHOW);
            y += fieldH + rowGap;
        }

        if (regionByImage) {
            ParkParamControl(regionLabel);
            ParkParamControl(fullBtn);
            ParkParamControl(selectBtn);
            ShowParamLayout(320, true);

            const FindImageSideButtonLayout sideLayout = ComputeFindImageSideButtonLayout();
            const int headerRowY = y;
            const int headerLabelH = OcrScale(kFindBtnH);
            if (aiFindImageLabel_) {
                MoveAiRegionAt(aiFindImageLabel_, kFindContentLeft,
                    headerRowY + std::max(0, (sideLayout.compactBtnH - headerLabelH) / 2), 90, kFindBtnH);
                ShowWindow(aiFindImageLabel_, SW_SHOW);
            }
            if (aiFindSelectRegionBtn_) {
                PlaceAiFindImageCompactButton(aiFindSelectRegionBtn_, headerRowY);
                ShowWindow(aiFindSelectRegionBtn_, SW_SHOW);
            }
            y += sideLayout.compactBtnH + sideLayout.sideGap;
            y = LayoutAiFindRegionBlock(y);
        } else {
            ShowParamLayout(320, false);
            HideAiFindRegionControls();
            LayoutParamRegionButtons(regionLabel, fullBtn, selectBtn, y);
            y += btnH + rowGap;
        }

        y = LayoutParamCoordRows(
            y,
            x1Label, x1Edit, y1Label, y1Edit,
            x2Label, x2Edit, y2Label, y2Edit);
        return y;
    }

    void HideAiActionExecuteRegionControls() {
        ShowParamLayout(320, false);
        HideAiFindRegionControls();
        for (HWND h : {
            aiRegionByImageCheck2_, aiActionRegionLabel_, aiFullScreenBtn2_, aiSelectRegionBtn2_,
            aiActCoordX1Label_, aiSearchX1Edit2_, aiActCoordY1Label_, aiSearchY1Edit2_,
            aiActCoordX2Label_, aiSearchX2Edit2_, aiActCoordY2Label_, aiSearchY2Edit2_}) {
            if (h) ParkParamControl(h);
        }
    }

    void RefreshAiSubPanel() {
        const int sel = popupAction_.sel;
        if (sel != 29 && sel != 30 && sel != 31) {
            HideAiFindRegionControls();
            return;
        }

        const bool imageMode = sel == 30;
        const bool actionExecuteMode = sel == 31;
        const bool withImage = actionExecuteMode && aiWithImageCheck_ && Checked(aiWithImageCheck_);
        const bool regionByImage = imageMode
            ? (aiRegionByImageCheck_ && Checked(aiRegionByImageCheck_))
            : (withImage && aiRegionByImageCheck2_ && Checked(aiRegionByImageCheck2_));

        HideAiFindRegionControls();
        const int rowGap = MulDiv(12, kEditorHeight, kEditorBaseHeight);
        EnsureAiCommonEditsVisible();
        LayoutAiPromptHeader(actionExecuteMode);

        if (sel == 29) {
            ParkParamGroup(aiImageControls_);
            ParkParamGroup(aiActionControls_);
            ParkAiActionOnlyCommonFields();
            CompactAiCommonBelowPrompt();
            SyncParamScrollLayout();
            return;
        }

        if (imageMode) {
            ParkParamGroup(aiActionControls_);
            ParkAiActionOnlyCommonFields();
            CompactAiCommonBelowPrompt();
            if (aiRegionByImageCheck_) ShowWindow(aiRegionByImageCheck_, SW_SHOW);
            if (aiRegionByImageCheck2_) ShowWindow(aiRegionByImageCheck2_, SW_HIDE);
            StackParamGroupBelow(aiImageControls_, AiCommonBlockBottomY(), rowGap);
            int y = aiRegionByImageCheck_
                ? static_cast<int>(WindowClientRect(aiRegionByImageCheck_).top)
                : AiCommonBlockBottomY() + rowGap;
            LayoutAiRegionSection(
                regionByImage,
                aiRegionByImageCheck_,
                aiRegionLabel_, aiFullScreenBtn_, aiSelectRegionBtn_,
                aiCoordX1Label_, aiSearchX1Edit_, aiCoordY1Label_, aiSearchY1Edit_,
                aiCoordX2Label_, aiSearchX2Edit_, aiCoordY2Label_, aiSearchY2Edit_,
                y);
        } else {
            ParkParamGroup(aiImageControls_);
            if (aiRegionByImageCheck2_) ShowWindow(aiRegionByImageCheck2_, SW_SHOW);
            if (aiRegionByImageCheck_) ShowWindow(aiRegionByImageCheck_, SW_HIDE);
            StackParamGroupBelow(aiActionControls_, AiCommonBlockBottomY(), rowGap);
            if (withImage) {
                int y = aiRegionByImageCheck2_
                    ? static_cast<int>(WindowClientRect(aiRegionByImageCheck2_).top)
                    : AiCommonBlockBottomY() + rowGap;
                LayoutAiRegionSection(
                    regionByImage,
                    aiRegionByImageCheck2_,
                    aiActionRegionLabel_, aiFullScreenBtn2_, aiSelectRegionBtn2_,
                    aiActCoordX1Label_, aiSearchX1Edit2_, aiActCoordY1Label_, aiSearchY1Edit2_,
                    aiActCoordX2Label_, aiSearchX2Edit2_, aiActCoordY2Label_, aiSearchY2Edit2_,
                    y);
            } else {
                HideAiActionExecuteRegionControls();
                for (HWND h : {
                    aiRegionByImageCheck2_, aiActionRegionLabel_, aiFullScreenBtn2_, aiSelectRegionBtn2_,
                    aiActCoordX1Label_, aiSearchX1Edit2_, aiActCoordY1Label_, aiSearchY1Edit2_,
                    aiActCoordX2Label_, aiSearchX2Edit2_, aiActCoordY2Label_, aiSearchY2Edit2_}) {
                    if (h) ParkParamControl(h);
                }
            }
        }
        if (regionByImage && (imageMode || withImage)) RefreshGrayButtonsInParamViewport();
        SyncParamScrollLayout();
    }

    void InsertOcrSearchVariable() {
        if (!ocrSearchEdit_) return;
        const int sel = popupOcrSearchVar_.sel;
        if (sel < 0 || sel >= static_cast<int>(quickInputVarItems_.size())) return;
        const std::wstring insert = quickInputVarItems_[static_cast<size_t>(sel)].insertText;
        DWORD start = 0, end = 0;
        SendMessageW(ocrSearchEdit_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
        const std::wstring current = GetText(ocrSearchEdit_);
        const std::wstring updated = current.substr(0, start) + insert + current.substr(end);
        SetText(ocrSearchEdit_, updated);
        const DWORD pos = start + static_cast<DWORD>(insert.size());
        SendMessageW(ocrSearchEdit_, EM_SETSEL, pos, pos);
        SetFocus(ocrSearchEdit_);
        if (popupAction_.sel == 18) { RefreshOcrSubPanel(); SyncParamScrollLayout(); }
    }

    int LayoutOcrSearchBlock(int y, int rowGap, int btnH, int fieldH) {
        (void)btnH;
        MoveOcrAt(ocrSearchLabel_, kFindContentLeft, y, kOcrSearchVarLabelW, 22);
        MoveOcrAt(ocrSearchEdit_, kOcrSearchEditX, y, kOcrSearchEditW, 22);
        y += fieldH + rowGap;

        HDC hdc = GetDC(hwnd_);
        const int insertW = OcrTextButtonWidthDesign(hdc, editorFont_ ? editorFont_ : font_, L"插入", kOcrInsertBtnW, 16);
        ReleaseDC(hwnd_, hdc);
        const int insertX = kOcrPanelRight - insertW;
        const int comboX = kFindContentLeft + kOcrSearchVarLabelW + kOcrVarComboGap;
        const int comboW = std::max(40, insertX - kOcrVarInsertGap - comboX);
        const int rowH = OcrScale(kOcrCompactBtnH);
        MoveOcrAt(ocrSearchVarLabel_, kFindContentLeft, y + (rowH - fieldH) / 2, kOcrSearchVarLabelW, 22);
        MoveOcrAt(ocrSearchVarCombo_, comboX, y + (rowH - OcrScale(kFindBtnH)) / 2, comboW, kFindBtnH);
        MoveOcrAt(ocrSearchVarInsertBtn_, insertX, y, insertW, kOcrCompactBtnH);
        return y + rowH + rowGap;
    }

    int LayoutOcrFindRegionBlock(int y) {
        const FindImageSideButtonLayout layout = ComputeFindImageSideButtonLayout();
        const int fieldH = OcrScaleY(22);
        const int rowGap = OcrScaleY(kFindVGap);
        if (ocrFindImageLabel_) ShowWindow(ocrFindImageLabel_, SW_HIDE);
        MoveOcrAt(ocrFindImagePreviewBtn_, kFindContentLeft, y, kFindImageSize, kFindImageSize);
        LayoutOcrFindImageSideStack(y, ocrFindScreenshotBtn_, ocrFindLocalImageBtn_, ocrFindClearImageBtn_);
        y += kFindImageSize + layout.sideGap;

        MoveOcrAt(ocrFindMatchThreshold_, kFindContentLeft + 91, y, 40, 22);
        for (HWND h : ocrFindRegionControls_) {
            if (!h) continue;
            wchar_t buf[16]{};
            GetWindowTextW(h, buf, 16);
            if (wcscmp(buf, L"匹配度大于") == 0) MoveOcrAt(h, kFindContentLeft, y, 90, 22);
            else if (wcscmp(buf, L"%") == 0) MoveOcrAt(h, kFindContentLeft + 135, y, 24, 22);
        }
        y += fieldH + rowGap;

        MoveOcrAt(ocrFindScaleMin_, kFindContentLeft + 65, y, 40, 22);
        MoveOcrAt(ocrFindScaleMax_, kFindContentLeft + 151, y, 40, 22);
        for (HWND h : ocrFindRegionControls_) {
            if (!h) continue;
            wchar_t buf[16]{};
            GetWindowTextW(h, buf, 16);
            if (wcscmp(buf, L"最小缩放") == 0) MoveOcrAt(h, kFindContentLeft, y, 64, 22);
            else if (wcscmp(buf, L"最大") == 0) MoveOcrAt(h, kFindContentLeft + 110, y, 40, 22);
        }
        return y + fieldH + rowGap;
    }

    bool OcrSearchTextContainsVariable() const {
        if (!ocrSearchEdit_) return false;
        const std::wstring text = GetText(ocrSearchEdit_);
        const size_t open = text.find(L'{');
        if (open == std::wstring::npos) return false;
        const size_t close = text.find(L'}', open + 1);
        return close != std::wstring::npos;
    }

    void RefreshOcrSubPanel() {
        const bool searchMode = popupOcrResultMode_.sel == 1;
        const bool saveVar = popupOcrFollowUp_.sel == 2;
        const bool regionByImage = ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_);
        ShowParamLayout(18, true);
        ShowParamLayout(183, true);
        ShowParamLayout(182, searchMode);
        ShowParamLayout(184, !saveVar);
        ShowParamLayout(185, saveVar);
        ShowParamLayout(181, true);
        ShowParamLayout(186, regionByImage);
        if (!regionByImage) {
            for (HWND h : ocrFindRegionControls_) {
                if (h) ShowWindow(h, SW_HIDE);
            }
        }
        if (editorPopupOpen_ == 18) CloseEditorPopup();
        if (ocrUntilFound_) ShowWindow(ocrUntilFound_, searchMode ? SW_SHOW : SW_HIDE);

        const int rowGap = OcrScaleY(kFindVGap);
        const int btnH = OcrScaleY(kFindBtnH);
        const int fieldH = OcrScaleY(22);

        int y = OcrContentStartY();
        if (ocrRegionByImageCheck_) {
            MoveOcrAt(ocrRegionByImageCheck_, kFindContentLeft, y, kOcrRegionByImageW, 22);
            ShowWindow(ocrRegionByImageCheck_, SW_SHOW);
            y += fieldH + rowGap;
        }

        if (regionByImage) {
            const FindImageSideButtonLayout sideLayout = ComputeFindImageSideButtonLayout();
            const int comboRowY = y;
            if (ocrDigitsOnlyCheck_) {
                MoveOcrAt(ocrDigitsOnlyCheck_, kFindContentLeft,
                    comboRowY + std::max(0, (sideLayout.compactBtnH - fieldH) / 2), kOcrDigitsOnlyW, 22);
                ShowWindow(ocrDigitsOnlyCheck_, SW_SHOW);
            }
            if (ocrRegionLabel_) ShowWindow(ocrRegionLabel_, SW_HIDE);
            if (ocrFullScreenBtn_) ShowWindow(ocrFullScreenBtn_, SW_HIDE);
            if (ocrSelectRegionBtn_) ShowWindow(ocrSelectRegionBtn_, SW_HIDE);
            PlaceOcrFindImageCompactButton(ocrFindSelectRegionBtn_, comboRowY);
            if (ocrFindSelectRegionBtn_) ShowWindow(ocrFindSelectRegionBtn_, SW_SHOW);
            y += sideLayout.compactBtnH + sideLayout.sideGap;
            y = LayoutOcrFindRegionBlock(y);
            ShowFindImageSideControls(
                ocrFindImagePreviewBtn_, ocrFindScreenshotBtn_, ocrFindLocalImageBtn_, ocrFindClearImageBtn_);
        } else {
            if (ocrDigitsOnlyCheck_) {
                MoveOcrAt(ocrDigitsOnlyCheck_, kFindContentLeft, y, kOcrDigitsOnlyW, 22);
                ShowWindow(ocrDigitsOnlyCheck_, SW_SHOW);
                y += fieldH + rowGap;
            }
            if (ocrFindSelectRegionBtn_) ShowWindow(ocrFindSelectRegionBtn_, SW_HIDE);
            SizeOcrRegionButtonsAt(y);
            y += btnH + rowGap;
        }

        y = LayoutParamCoordRows(
            y,
            ocrX1Label_, ocrX1_, ocrY1Label_, ocrY1_,
            ocrX2Label_, ocrX2_, ocrY2Label_, ocrY2_);

        MoveOcrAt(ocrResultModeLabel_, kFindContentLeft, y, kFindFollowLabelW, kFindBtnH);
        MoveOcrAt(ocrResultModeCombo_, kFindContentLeft + kFindFollowLabelW + 8, y, kFindFollowComboW, kFindBtnH);
        if (ocrResultModeLabel_) ShowWindow(ocrResultModeLabel_, SW_SHOW);
        y += btnH + rowGap;

        if (searchMode) {
            y = LayoutOcrSearchBlock(y, rowGap, btnH, fieldH);
        }

        MoveOcrAt(ocrFollowUpLabel_, kFindContentLeft, y, kFindFollowLabelW, kFindBtnH);
        MoveOcrAt(ocrFollowUpCombo_, kFindContentLeft + kFindFollowLabelW + 8, y, kFindFollowComboW, kFindBtnH);
        if (ocrFollowUpLabel_) ShowWindow(ocrFollowUpLabel_, SW_SHOW);
        y += btnH + rowGap;

        if (!saveVar) {
            if (ocrOffsetXLabel_) ShowWindow(ocrOffsetXLabel_, SW_SHOW);
            if (ocrOffsetYLabel_) ShowWindow(ocrOffsetYLabel_, SW_SHOW);
            if (ocrResultVarLabel_) ShowWindow(ocrResultVarLabel_, SW_HIDE);
            y = LayoutParamCoordPairRow(y, ocrOffsetXLabel_, ocrOffsetX_, ocrOffsetYLabel_, ocrOffsetY_, kFindOffsetLabelW);
        } else {
            if (ocrOffsetXLabel_) ShowWindow(ocrOffsetXLabel_, SW_HIDE);
            if (ocrOffsetYLabel_) ShowWindow(ocrOffsetYLabel_, SW_HIDE);
            if (ocrResultVarLabel_) ShowWindow(ocrResultVarLabel_, SW_SHOW);
            if (ocrSelectOffsetBtn_) ShowWindow(ocrSelectOffsetBtn_, SW_HIDE);
            MoveOcrAt(ocrResultVarLabel_, kFindContentLeft, y, 100, 22);
            MoveOcrAt(ocrResultVar_, kFindContentLeft + 91, y, kOcrResultVarEditW, 22);
            y += fieldH + rowGap;
        }

        if (searchMode) {
            const int compactRowH = std::max(fieldH, OcrScale(kOcrCompactBtnH));
            const int testX = kOcrPanelRight - kOcrTestBtnW;
            if (ocrUntilFound_) {
                MoveOcrAt(ocrUntilFound_, kFindContentLeft, y + (compactRowH - fieldH) / 2, 140, 22);
            }
            if (ocrSelectOffsetBtn_) ShowWindow(ocrSelectOffsetBtn_, SW_HIDE);
            MoveOcrAt(ocrTestBtn_, testX, y + (compactRowH - OcrScale(kOcrCompactBtnH)) / 2, kOcrTestBtnW, kOcrCompactBtnH);
            y += compactRowH + rowGap;
        } else if (!saveVar) {
            const int rowY = y;
            const int left = OcrScaleX(kFindContentLeft);
            const int maxRight = ParamScrollContentRight() - OcrScaleX(4);
            const int gap = OcrScaleX(8);
            const int testW = std::min(OcrScaleX(kOcrTestBtnW), std::max(OcrScaleX(48), (maxRight - left) / 4));
            const int offsetW = std::min(OcrScaleX(kFindSelectOffsetW), std::max(OcrScaleX(48), maxRight - left - testW - gap));
            const int rowH = btnH;
            if (ocrSelectOffsetBtn_) {
                ShowWindow(ocrSelectOffsetBtn_, SW_SHOW);
                SetParamPosAware(ocrSelectOffsetBtn_, left, rowY, offsetW, rowH, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (ocrTestBtn_) {
                ShowWindow(ocrTestBtn_, SW_SHOW);
                SetParamPosAware(ocrTestBtn_, left + offsetW + gap, rowY, std::min(testW, maxRight - left - offsetW - gap), rowH,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            y += rowH + rowGap;
        } else {
            MoveOcrAt(ocrTestBtn_, kFindContentLeft, y, kFindBtnW, kFindBtnH);
            y += btnH + rowGap;
        }
        if (regionByImage) RefreshGrayButtonsInParamViewport();
    }

    void InvalidateOcrEditorPanel() {
        InvalidateParamScrollArea();
    }

    int OcrDepRowY() const {
        return ParamPanelContentTopY();
    }

    int OcrContentStartY() const {
        const int depH = MulDiv(kFindBtnH, kEditorHeight, kEditorBaseHeight);
        const int depGap = MulDiv(kOcrDepToRegionGap, kEditorHeight, kEditorBaseHeight);
        return OcrDepRowY() + depH + depGap;
    }

    void RefreshOcrDepStatus() {
        if (!ocrDepStatusLabel_ || !ocrDepInstallBtn_) return;
        const OcrEnvStatus env = CheckOcrEnvironment(false);
        const bool ready = env.state == OcrEnvState::Ready;
        SetText(ocrDepStatusLabel_, ready ? L"文字识别已安装" : L"文字识别未安装");
        SetWindowTextW(ocrDepInstallBtn_, ready ? L"修复/更新" : L"一键安装");
        EnableWindow(ocrDepInstallBtn_, TRUE);
        EnsureParamViewportParent(ocrDepStatusLabel_);
        EnsureParamViewportParent(ocrDepInstallBtn_);
        const int depY = OcrDepRowY();
        const int left = MulDiv(kFindContentLeft, kEditorWidth, kEditorBaseWidth);
        const int panelRight = ParamScrollContentRight();
        const int maxW = std::max(1, panelRight - left - MulDiv(4, kEditorWidth, kEditorBaseWidth));
        const int gap = MulDiv(8, kEditorWidth, kEditorBaseWidth);
        const int depH = MulDiv(kFindBtnH, kEditorHeight, kEditorBaseHeight);
        HDC hdc = GetDC(hwnd_);
        const wchar_t* btnText = ready ? L"修复/更新" : L"一键安装";
        const int btnWDesign = OcrTextButtonWidthDesign(hdc, editorFont_ ? editorFont_ : font_, btnText, kFindBtnW, 16);
        ReleaseDC(hwnd_, hdc);
        const int btnW = std::min(OcrScaleX(btnWDesign), maxW - MulDiv(120, kEditorWidth, kEditorBaseWidth) - gap);
        const int labelW = std::max(MulDiv(120, kEditorWidth, kEditorBaseWidth), maxW - btnW - gap);
        const int btnX = left + labelW + gap;
        SetParamPosAware(ocrDepStatusLabel_, left, depY, labelW, depH, SWP_NOZORDER | SWP_NOACTIVATE);
        SetParamPosAware(ocrDepInstallBtn_, btnX, depY, btnW, depH, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(ocrDepStatusLabel_, SW_SHOW);
        ShowWindow(ocrDepInstallBtn_, SW_SHOW);
    }

    void ShowOcrInstallDialog() {
        const OcrEnvStatus env = CheckOcrEnvironment(false);
        const bool repair = env.state == OcrEnvState::Ready;
        OcrInstallDialog dlg;
        dlg.Show(hwnd_, repair);
        RefreshOcrDepStatus();
    }

    void ApplyOcrFullScreen() {
        ocrFullScreen_ = true;
        int x = 0, y = 0, w = 0, h = 0;
        GetVirtualScreenRect(x, y, w, h);
        SetText(ocrX1_, std::to_wstring(x));
        SetText(ocrY1_, std::to_wstring(y));
        SetText(ocrX2_, std::to_wstring(x + w));
        SetText(ocrY2_, std::to_wstring(y + h));
        RefreshCoordFieldEdits({ocrX1_, ocrY1_, ocrX2_, ocrY2_});
    }

    void ApplyAiFullScreen() {
        aiFullScreen_ = true;
        int x = 0, y = 0, w = 0, h = 0;
        GetVirtualScreenRect(x, y, w, h);
        if (popupAction_.sel == 30) {
            SetText(aiSearchX1Edit_, std::to_wstring(x));
            SetText(aiSearchY1Edit_, std::to_wstring(y));
            SetText(aiSearchX2Edit_, std::to_wstring(x + w));
            SetText(aiSearchY2Edit_, std::to_wstring(y + h));
            RefreshCoordFieldEdits({aiSearchX1Edit_, aiSearchY1Edit_, aiSearchX2Edit_, aiSearchY2Edit_});
        } else if (popupAction_.sel == 31) {
            SetText(aiSearchX1Edit2_, std::to_wstring(x));
            SetText(aiSearchY1Edit2_, std::to_wstring(y));
            SetText(aiSearchX2Edit2_, std::to_wstring(x + w));
            SetText(aiSearchY2Edit2_, std::to_wstring(y + h));
            RefreshCoordFieldEdits({aiSearchX1Edit2_, aiSearchY1Edit2_, aiSearchX2Edit2_, aiSearchY2Edit2_});
        }
    }

    void BeginOcrRegionSelect() {
        if (!hwnd_) return;
        if (!screenshotOverlay_) screenshotOverlay_ = std::make_unique<ScreenshotOverlay>();
        screenshotOverlay_->SetTitle(L"选取区域");
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);
        screenshotOverlay_->Show([this](RECT sel) {
            if (sel.left == 0 && sel.top == 0 && sel.right == 0 && sel.bottom == 0) {
                RestoreEditorAfterScreenOverlay();
                return;
            }
            const int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            ocrFullScreen_ = false;
            SetText(ocrX1_, std::to_wstring(sel.left + vsX));
            SetText(ocrY1_, std::to_wstring(sel.top + vsY));
            SetText(ocrX2_, std::to_wstring(sel.right + vsX));
            SetText(ocrY2_, std::to_wstring(sel.bottom + vsY));
            RestoreEditorAfterScreenOverlay();
        });
    }

    void FlushPendingUiMessages() const {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    void HideEditorForScreenCapture() {
        if (!hwnd_) return;
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);
        UpdateWindow(hwnd_);
        FlushPendingUiMessages();
        Sleep(150);
    }

    void RestoreEditorAfterScreenCapture() {
        RestoreEditorAfterScreenOverlay();
    }

    void TestOcr() {
        if (ocrTestRunning_.exchange(true)) return;
        const OcrEnvStatus env = CheckOcrEnvironment(false);
        if (env.state != OcrEnvState::Ready) {
            ocrTestRunning_ = false;
            ShowOcrInstallDialog();
            return;
        }
        if (ocrTestBtn_) EnableWindow(ocrTestBtn_, FALSE);

        const bool regionByImage = ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_);
        const bool digitsOnly = ocrDigitsOnlyCheck_ && Checked(ocrDigitsOnlyCheck_);
        if (regionByImage && ocrFindImagePath_.empty()) {
            ocrTestRunning_ = false;
            if (ocrTestBtn_) EnableWindow(ocrTestBtn_, TRUE);
            MessageBoxW(hwnd_, L"请先设置要查找的图片。", L"测试文字识别", MB_OK | MB_ICONINFORMATION);
            return;
        }

        HideEditorForScreenCapture();

        int sx1 = ToInt(ocrX1_), sy1 = ToInt(ocrY1_), sx2 = ToInt(ocrX2_), sy2 = ToInt(ocrY2_);
        if (regionByImage) {
            if (!ResolveOcrAbsRegionFromFindImage(sx1, sy1, sx2, sy2, sx1, sy1, sx2, sy2)) {
                RestoreEditorAfterScreenCapture();
                ocrTestRunning_ = false;
                if (ocrTestBtn_) EnableWindow(ocrTestBtn_, TRUE);
                MessageBoxW(hwnd_, L"未找到参考图片，无法测试识别。", L"测试文字识别", MB_OK | MB_ICONINFORMATION);
                return;
            }
        } else if (ocrFullScreen_) {
            int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
            GetVirtualScreenRect(vsX, vsY, vsW, vsH);
            sx1 = vsX; sy1 = vsY; sx2 = vsX + vsW; sy2 = vsY + vsH;
        }

        if (!ocrOverlay_) ocrOverlay_ = std::make_unique<OcrOverlay>();

        std::wstring searchTarget;
        if (popupOcrResultMode_.sel == 1) {
            MacroVariableContext ctx;
            ctx.matchVars = &matchVars_;
            ctx.ocrVars = &ocrVars_;
            ctx.loopVars = &loopVars_;
            ctx.timerStarts = &timerStarts_;
            ctx.curLoops = curLoops_;
            searchTarget = ResolveMacroVariables(GetText(ocrSearchEdit_), ctx);
        }
        EnsureOcrSession();
        ocrOverlay_->Show(sx1, sy1, sx2, sy2, searchTarget, OcrOverlayMode::Test, digitsOnly);
        ReleaseOcrSession();

        RestoreEditorAfterScreenCapture();

        ocrTestRunning_ = false;
        if (ocrTestBtn_) EnableWindow(ocrTestBtn_, TRUE);
        if (popupAction_.sel == 18) {
            RefreshOcrDepStatus();
            RefreshOcrSubPanel();
        }
    }

    void CancelQuickInputTip() {
        quickInputTipPending_ = QuickInputTipKind::None;
        quickInputTipPendingVarIndex_ = -1;
        quickInputTipShown_ = QuickInputTipKind::None;
        if (hwnd_) KillTimer(hwnd_, kQuickInputTipTimerId);
        if (editorTipPopup_) ShowWindow(editorTipPopup_, SW_HIDE);
    }

    bool IsPointInQuickInputEdit(int x, int y) const {
        if (!quickInputEdit_ || popupAction_.sel != 12 || !IsWindowVisible(quickInputEdit_)) return false;
        const RECT rc = WindowClientRect(quickInputEdit_);
        return PtInRect(&rc, POINT{x, y});
    }

    void BeginQuickInputTextTipHover(int x, int y) {
        if (popupAction_.sel != 12) { CancelQuickInputTip(); return; }
        if (quickInputTipPending_ == QuickInputTipKind::TextExample
            && quickInputTipAnchor_.x == x && quickInputTipAnchor_.y == y) return;
        quickInputTipPending_ = QuickInputTipKind::TextExample;
        quickInputTipPendingVarIndex_ = -1;
        quickInputTipAnchor_ = POINT{x, y};
        quickInputTipHoverStart_ = GetTickCount();
        quickInputTipShown_ = QuickInputTipKind::None;
        if (editorTipPopup_) ShowWindow(editorTipPopup_, SW_HIDE);
        if (hwnd_) SetTimer(hwnd_, kQuickInputTipTimerId, 50, nullptr);
    }

    void BeginQuickInputVarTipHover(int varIndex) {
        if (varIndex < 0 || varIndex >= static_cast<int>(quickInputVarItems_.size())) {
            CancelQuickInputTip();
            return;
        }
        if (quickInputTipPending_ == QuickInputTipKind::VariableHelp && quickInputTipPendingVarIndex_ == varIndex) return;
        quickInputTipPending_ = QuickInputTipKind::VariableHelp;
        quickInputTipPendingVarIndex_ = varIndex;
        GetCursorPos(&quickInputTipAnchor_);
        quickInputTipHoverStart_ = GetTickCount();
        quickInputTipShown_ = QuickInputTipKind::None;
        if (editorTipPopup_) ShowWindow(editorTipPopup_, SW_HIDE);
        if (hwnd_) SetTimer(hwnd_, kQuickInputTipTimerId, 50, nullptr);
    }

    void OnQuickInputTipTimer() {
        if (quickInputTipPending_ == QuickInputTipKind::None) {
            KillTimer(hwnd_, kQuickInputTipTimerId);
            return;
        }
        if (GetTickCount() - quickInputTipHoverStart_ < static_cast<DWORD>(kQuickInputTipDelayMs)) return;
        KillTimer(hwnd_, kQuickInputTipTimerId);
        if (quickInputTipPending_ == QuickInputTipKind::TextExample) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            if (!IsPointInQuickInputEdit(pt.x, pt.y)) {
                CancelQuickInputTip();
                return;
            }
            quickInputTipShown_ = QuickInputTipKind::TextExample;
            quickInputTipAnchor_ = pt;
        } else if (quickInputTipPending_ == QuickInputTipKind::VariableHelp) {
            if (editorPopupOpen_ != 7 || editorPopupHover_ != quickInputTipPendingVarIndex_) {
                CancelQuickInputTip();
                return;
            }
            quickInputTipShown_ = QuickInputTipKind::VariableHelp;
            GetCursorPos(&quickInputTipAnchor_);
        }
        SyncQuickInputTipPopup();
    }

    void UpdateQuickInputTextTip(int x, int y) {
        if (IsPointInQuickInputEdit(x, y)) BeginQuickInputTextTipHover(x, y);
        else if (quickInputTipPending_ == QuickInputTipKind::TextExample || quickInputTipShown_ == QuickInputTipKind::TextExample) CancelQuickInputTip();
    }

    bool IsBlockNameDuplicate(const std::wstring& name, int excludeIndex = -1) const {
        for (int i = 0; i < static_cast<int>(actions_.size()); ++i) {
            if (i == excludeIndex) continue;
            if (actions_[static_cast<size_t>(i)].type == ActionType::DefineBlock && actions_[static_cast<size_t>(i)].blockName == name) return true;
        }
        return false;
    }

    bool ValidateDefineBlockName(const std::wstring& name, int excludeIndex = -1) const {
        if (!IsValidBlockName(name)) {
            MessageBoxW(hwnd_, L"块名称只能以字母开始，后面只能包含字母和数字。", L"定义宏指令块", MB_OK | MB_ICONWARNING);
            return false;
        }
        if (IsBlockNameDuplicate(name, excludeIndex)) {
            MessageBoxW(hwnd_, L"块名称不能重复。", L"定义宏指令块", MB_OK | MB_ICONWARNING);
            return false;
        }
        return true;
    }

    bool TryInsertActionFromForm(size_t pos, int indentOverride = -1) {
        ScriptAction action = ActionFromForm();
        if (action.type == ActionType::DefineBlock) {
            if (!ValidateDefineBlockName(action.blockName)) return false;
            pos = 0;
            indentOverride = 0;
        }
        InsertAction(pos, action, indentOverride);
        if (action.type == ActionType::DefineBlock || action.type == ActionType::RunBlock) RefreshRunBlockCombo();
        OnActionsChanged();
        return true;
    }

    int ComboSelForType(ActionType type) const {
        switch (type) {
        case ActionType::MoveMouse: return 0;
        case ActionType::Wait: return 1;
        case ActionType::MouseClick: return 2;
        case ActionType::MousePlayback: return 3;
        case ActionType::RunMacro: return 4;
        case ActionType::MouseDown: return 5;
        case ActionType::MouseUp: return 6;
        case ActionType::KeyClick: return 8;
        case ActionType::KeyDown: return 9;
        case ActionType::KeyUp: return 10;
        case ActionType::HotkeyShortcut: return 11;
        case ActionType::QuickInput: return 12;
        case ActionType::Loop: return 13;
        case ActionType::EndLoop: return 14;
        case ActionType::DefineBlock: return 15;
        case ActionType::RunBlock: return 16;
        case ActionType::ScrollWheel: return 7;
        case ActionType::FindImage: return 17;
        case ActionType::TextRecognition: return 18;
        case ActionType::If: return 19;
        case ActionType::Else: return 20;
        case ActionType::LockScreenshot: return 21;
        case ActionType::UnlockScreenshot: return 22;
        case ActionType::StopMacro: return 23;
        case ActionType::RunProgram: return 24;
        case ActionType::CloseProgram: return 25;
        case ActionType::OpenWebpage: return 26;
        case ActionType::OpenFile: return 27;
        case ActionType::TimerRecordTime: return 28;
        case ActionType::AiTextAnalysis: return 29;
        case ActionType::AiImageAnalysis: return 30;
        case ActionType::AiActionExecute: return 31;
        case ActionType::GetCursorPos: return 32;
        default: return 14;
        }
    }

    bool IsImplementedActionPopup(int idx) const {
        return idx == 0 || idx == 1 || idx == 2 || idx == 3 || idx == 4 || idx == 5 || idx == 6 || idx == 7 || idx == 8 || idx == 9 || idx == 10
            || idx == 11 || idx == 12
            || idx == 13 || idx == 14 || idx == 15 || idx == 16 || idx == 17 || idx == 18 || idx == 19
            || idx == 20 || idx == 21 || idx == 22 || idx == 23 || idx == 24
            || idx == 25 || idx == 26 || idx == 27 || idx == 28
            || idx == 29 || idx == 30 || idx == 31 || idx == 32;
    }

    void UpdateEditMode() {
        const bool selected = !batchEditMode_ && selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(actions_.size());
        ShowWindow(modifyBtn_, selected ? SW_SHOW : SW_HIDE);
        if (selected) LoadForm(actions_[static_cast<size_t>(selectedIndex_)]);
        if (batchEditMode_) SyncBatchSelectedSize();
        scrollOffset_ = std::clamp(scrollOffset_, 0, MaxEditorScroll());
        RefreshActionListLayer();
    }

    int BatchSelectedCount() const {
        int count = 0;
        for (bool checked : batchSelected_) if (checked) ++count;
        return count;
    }

    void SyncBatchSelectedSize() {
        if (!batchEditMode_) return;
        if (batchSelected_.size() < actions_.size()) batchSelected_.resize(actions_.size(), false);
        else if (batchSelected_.size() > actions_.size()) batchSelected_.resize(actions_.size());
    }

    void InvalidateToolbarArea() {
        RECT rc{0, 90, 780, 132};
        InvalidateRect(hwnd_, &rc, FALSE);
    }

    void UpdateBatchToolbar() {
        ShowWindow(loadBtn_, batchEditMode_ ? SW_HIDE : SW_SHOW);
        ShowWindow(clearBtn_, batchEditMode_ ? SW_HIDE : SW_SHOW);
        ShowWindow(batchExitBtn_, batchEditMode_ ? SW_SHOW : SW_HIDE);
        ShowWindow(batchSelectAllBtn_, batchEditMode_ ? SW_SHOW : SW_HIDE);
        ShowWindow(batchDeselectBtn_, batchEditMode_ ? SW_SHOW : SW_HIDE);
        ShowWindow(batchDeleteBtn_, batchEditMode_ ? SW_SHOW : SW_HIDE);
        ShowWindow(batchCopyBtn_, batchEditMode_ ? SW_SHOW : SW_HIDE);
        ShowWindow(labelBatchCount_, batchEditMode_ ? SW_SHOW : SW_HIDE);
        if (labelList_) SetText(labelList_, L"动作列表");
        if (labelBatchCount_) SetText(labelBatchCount_, L"已选中:" + std::to_wstring(BatchSelectedCount()) + L"个");
        const bool hasSelection = BatchSelectedCount() > 0;
        EnableWindow(batchDeleteBtn_, hasSelection ? TRUE : FALSE);
        EnableWindow(batchCopyBtn_, hasSelection ? TRUE : FALSE);
        if (batchDeleteBtn_) InvalidateRect(batchDeleteBtn_, nullptr, FALSE);
        if (batchCopyBtn_) InvalidateRect(batchCopyBtn_, nullptr, FALSE);
        InvalidateToolbarArea();
    }

    void EnterBatchEditMode() {
        if (batchEditMode_) return;
        CommitInlineRemark();
        batchEditMode_ = true;
        selectedIndex_ = -1;
        hoverIndex_ = -1;
        batchSelected_.assign(actions_.size(), false);
        UpdateBatchToolbar();
        UpdateEditMode();
    }

    void ExitBatchEditMode() {
        if (!batchEditMode_) return;
        batchEditMode_ = false;
        batchSelected_.clear();
        UpdateBatchToolbar();
        UpdateEditMode();
    }

    void ToggleBatchSelection(int index) {
        if (!batchEditMode_ || index < 0 || index >= static_cast<int>(actions_.size())) return;
        SyncBatchSelectedSize();
        batchSelected_[static_cast<size_t>(index)] = !batchSelected_[static_cast<size_t>(index)];
        UpdateBatchToolbar();
        RefreshActionListLayer();
    }

    void BatchSelectAll() {
        if (!batchEditMode_) return;
        batchSelected_.assign(actions_.size(), true);
        UpdateBatchToolbar();
        RefreshActionListLayer();
    }

    void BatchDeselectAll() {
        if (!batchEditMode_) return;
        batchSelected_.assign(actions_.size(), false);
        UpdateBatchToolbar();
        RefreshActionListLayer();
    }

    void BatchDeleteSelected() {
        if (!batchEditMode_ || BatchSelectedCount() == 0) return;
        CommitInlineRemark();
        for (int i = static_cast<int>(actions_.size()) - 1; i >= 0; --i) {
            if (i < static_cast<int>(batchSelected_.size()) && batchSelected_[static_cast<size_t>(i)]) {
                DeleteActionAt(i);
            }
        }
        batchSelected_.assign(actions_.size(), false);
        selectedIndex_ = -1;
        hoverIndex_ = -1;
        RenumberActions();
        UpdateBatchToolbar();
        UpdateEditMode();
    }

    void BatchCopySelected() {
        if (!batchEditMode_ || BatchSelectedCount() == 0) return;
        std::vector<ScriptAction> copies;
        for (size_t i = 0; i < actions_.size(); ++i) {
            if (i < batchSelected_.size() && batchSelected_[i]) {
                ScriptAction copy = actions_[i];
                copy.originalNo = NextNo();
                copies.push_back(copy);
            }
        }
        for (const auto& copy : copies) actions_.push_back(copy);
        batchSelected_.assign(actions_.size(), false);
        RenumberActions();
        UpdateBatchToolbar();
        UpdateEditMode();
        OnActionsChanged();
    }

    bool Checked(HWND h) const { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }
    void SetChecked(HWND h, bool checked) { SendMessageW(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0); }

    void ReadModifierHolds(ScriptAction& action, HWND lWin, HWND rWin, HWND lCtrl, HWND rCtrl, HWND lAlt, HWND rAlt, HWND lShift, HWND rShift) {
        action.holdLeftWin = Checked(lWin); action.holdRightWin = Checked(rWin);
        action.holdLeftCtrl = Checked(lCtrl); action.holdRightCtrl = Checked(rCtrl);
        action.holdLeftAlt = Checked(lAlt); action.holdRightAlt = Checked(rAlt);
        action.holdLeftShift = Checked(lShift); action.holdRightShift = Checked(rShift);
    }

    void WriteModifierHolds(const ScriptAction& action, HWND lWin, HWND rWin, HWND lCtrl, HWND rCtrl, HWND lAlt, HWND rAlt, HWND lShift, HWND rShift) {
        SetChecked(lWin, action.holdLeftWin); SetChecked(rWin, action.holdRightWin);
        SetChecked(lCtrl, action.holdLeftCtrl); SetChecked(rCtrl, action.holdRightCtrl);
        SetChecked(lAlt, action.holdLeftAlt); SetChecked(rAlt, action.holdRightAlt);
        SetChecked(lShift, action.holdLeftShift); SetChecked(rShift, action.holdRightShift);
    }

    ScriptAction ActionFromForm() {
        ScriptAction action{};
        action.remark = GetText(remark_);
        const int sel = popupAction_.sel;
        if (sel == 0) {
            action.type = ActionType::MoveMouse;
            action.moveFromVar = Checked(moveFromVar_);
            action.moveVarExprX = Trim(GetText(moveVarX_));
            action.moveVarExprY = Trim(GetText(moveVarY_));
            action.x = ToInt(moveX_);
            action.y = ToInt(moveY_);
            action.randomX = std::max(0, ToInt(moveRandomX_));
            action.randomY = std::max(0, ToInt(moveRandomY_));
        }
        else if (sel == 1) { action.type = ActionType::Wait; action.duration = std::max(0.0, ToDouble(waitDuration_, 0.5)); action.randomDuration = std::max(0.0, ToDouble(waitRandom_)); }
        else if (sel == 2) { action.type = ActionType::MouseClick; action.button = static_cast<MouseButtonType>(std::max(0, popupClickBtn_.sel)); action.clickCount = std::max(1, ToInt(clickCount_, 1)); action.duration = std::max(0.0, ToDouble(clickWait_, 0.01)); action.randomDuration = std::max(0.0, ToDouble(clickRandom_)); ReadModifierHolds(action, clickLWin_, clickRWin_, clickLCtrl_, clickRCtrl_, clickLAlt_, clickRAlt_, clickLShift_, clickRShift_); }
        else if (sel == 3) {
            action.type = ActionType::MousePlayback;
            action.blockName = popupMousePlayback_.sel >= 0 && popupMousePlayback_.sel < static_cast<int>(popupMousePlayback_.items.size())
                ? popupMousePlayback_.items[static_cast<size_t>(popupMousePlayback_.sel)] : Trim(GetText(mousePlaybackCombo_));
            action.targetPath = popupMousePlayback_.sel >= 0 && popupMousePlayback_.sel < static_cast<int>(mousePlaybackPaths_.size())
                ? mousePlaybackPaths_[static_cast<size_t>(popupMousePlayback_.sel)] : L"";
            action.clickCount = std::max(1, ToInt(mousePlaybackCount_, 1));
            action.duration = std::max(0.0, ToDouble(mousePlaybackWait_, 0.01));
            action.randomDuration = std::max(0.0, ToDouble(mousePlaybackRandom_));
        }
        else if (sel == 4) {
            action.type = ActionType::RunMacro;
            action.blockName = popupRunMacro_.sel >= 0 && popupRunMacro_.sel < static_cast<int>(popupRunMacro_.items.size())
                ? popupRunMacro_.items[static_cast<size_t>(popupRunMacro_.sel)] : Trim(GetText(runMacroCombo_));
            action.targetPath = popupRunMacro_.sel >= 0 && popupRunMacro_.sel < static_cast<int>(runMacroPaths_.size())
                ? runMacroPaths_[static_cast<size_t>(popupRunMacro_.sel)] : L"";
        }
        else if (sel == 5) { action.type = ActionType::MouseDown; action.button = static_cast<MouseButtonType>(std::max(0, popupMouseBtn_.sel)); ReadModifierHolds(action, mousePressLWin_, mousePressRWin_, mousePressLCtrl_, mousePressRCtrl_, mousePressLAlt_, mousePressRAlt_, mousePressLShift_, mousePressRShift_); }
        else if (sel == 6) { action.type = ActionType::MouseUp; action.button = static_cast<MouseButtonType>(std::max(0, popupMouseBtn_.sel)); ReadModifierHolds(action, mousePressLWin_, mousePressRWin_, mousePressLCtrl_, mousePressRCtrl_, mousePressLAlt_, mousePressRAlt_, mousePressLShift_, mousePressRShift_); }
        else if (sel == 7) {
            action.type = ActionType::ScrollWheel;
            action.scrollVertical = Checked(scrollVertical_);
            action.scrollHorizontal = Checked(scrollHorizontal_);
            if (!action.scrollVertical && !action.scrollHorizontal) action.scrollVertical = true;
            action.scrollSteps = std::max(1, ToInt(scrollSteps_, 1));
            action.scrollDirection = std::clamp(popupScrollDir_.sel, 0, 1);
            action.clickCount = std::max(1, ToInt(scrollCount_, 1));
            action.duration = std::max(0.0, ToDouble(scrollWait_, 0.01));
            action.randomDuration = std::max(0.0, ToDouble(scrollRandom_));
        }
        else if (sel == 8) { action.type = ActionType::KeyClick; action.keyText = formKeyText_.empty() ? L"7" : formKeyText_; action.keyVk = formKeyVk_ == 0 ? '7' : formKeyVk_; action.clickCount = std::max(1, ToInt(keyCount_, 1)); action.duration = std::max(0.0, ToDouble(keyWait_, 0.01)); action.randomDuration = std::max(0.0, ToDouble(keyRandom_)); ReadModifierHolds(action, keyLWin_, keyRWin_, keyLCtrl_, keyRCtrl_, keyLAlt_, keyRAlt_, keyLShift_, keyRShift_); }
        else if (sel == 9) { action.type = ActionType::KeyDown; action.keyText = formKeyPressText_.empty() ? L"7" : formKeyPressText_; action.keyVk = formKeyPressVk_ == 0 ? '7' : formKeyPressVk_; ReadModifierHolds(action, keyPressLWin_, keyPressRWin_, keyPressLCtrl_, keyPressRCtrl_, keyPressLAlt_, keyPressRAlt_, keyPressLShift_, keyPressRShift_); }
        else if (sel == 10) { action.type = ActionType::KeyUp; action.keyText = formKeyPressText_.empty() ? L"7" : formKeyPressText_; action.keyVk = formKeyPressVk_ == 0 ? '7' : formKeyPressVk_; ReadModifierHolds(action, keyPressLWin_, keyPressRWin_, keyPressLCtrl_, keyPressRCtrl_, keyPressLAlt_, keyPressRAlt_, keyPressLShift_, keyPressRShift_); }
        else if (sel == 11) {
            action.type = ActionType::HotkeyShortcut;
            action.shortcutPreset = std::clamp(popupHotkeyShortcut_.sel, 0, ShortcutPresetCount() - 1);
            ApplyShortcutPreset(action, action.shortcutPreset);
            action.clickCount = std::max(1, ToInt(hotkeyShortcutCount_, 1));
            action.duration = std::max(0.0, ToDouble(hotkeyShortcutWait_, 0.01));
            action.randomDuration = std::max(0.0, ToDouble(hotkeyShortcutRandom_));
        }
        else if (sel == 12) {
            action.type = ActionType::QuickInput;
            action.inputText = GetText(quickInputEdit_);
            action.charInterval = std::max(0.0, ToDouble(quickInputCharInterval_, 0.01));
            action.clickCount = std::max(1, ToInt(quickInputCount_, 1));
            action.duration = std::max(0.0, ToDouble(quickInputWait_, 0.01));
            action.randomDuration = std::max(0.0, ToDouble(quickInputRandom_));
        }
        else if (sel == 13) { action.type = ActionType::Loop; action.loopCount = ToInt(loopCount_, -1); action.loopFromVar = Checked(loopFromVar_); action.loopVarExpr = GetText(loopVarExpr_); action.loopVarName = GetText(loopVarName_); }
        else if (sel == 14) { action.type = ActionType::EndLoop; action.customText = L"跳出循环"; }
        else if (sel == 15) { action.type = ActionType::DefineBlock; action.blockName = Trim(GetText(defineBlockName_)); }
        else if (sel == 16) { action.type = ActionType::RunBlock; action.blockName = Trim(GetText(runBlockCombo_)); }
        else if (sel == 17) {
            action.type = ActionType::FindImage;
            action.searchX1 = ToInt(findX1_);
            action.searchY1 = ToInt(findY1_);
            action.searchX2 = ToInt(findX2_);
            action.searchY2 = ToInt(findY2_);
            action.searchFullScreen = findImageFullScreen_;
            action.imagePath = findImagePath_;
            action.matchThreshold = std::clamp(ToDouble(findMatchThreshold_, 65.0), 1.0, 100.0);
            action.imageScaleMin = std::max(0.1, ToDouble(findScaleMin_, 1.0));
            action.imageScaleMax = std::max(action.imageScaleMin, ToDouble(findScaleMax_, action.imageScaleMin));
            action.imageScale = (action.imageScaleMin + action.imageScaleMax) * 0.5;
            action.findImageFollowUp = std::clamp(popupFindFollowUp_.sel, 0, 2);
            action.offsetX = ToInt(findOffsetX_);
            action.offsetY = ToInt(findOffsetY_);
            action.findUntilFound = action.findImageFollowUp == 2 ? false : Checked(findUntilFound_);
            action.matchVarName = Trim(GetText(findMatchVar_));
            if (action.matchVarName.empty()) action.matchVarName = L"matchRet";
        }
        else if (sel == 18) {
            action.type = ActionType::TextRecognition;
            const bool regionByImage = ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_);
            const bool digitsOnly = ocrDigitsOnlyCheck_ && Checked(ocrDigitsOnlyCheck_);
            action.ocrRegionByImage = regionByImage;
            action.ocrDigitsOnly = digitsOnly;
            action.searchX1 = ToInt(ocrX1_);
            action.searchY1 = ToInt(ocrY1_);
            action.searchX2 = ToInt(ocrX2_);
            action.searchY2 = ToInt(ocrY2_);
            action.searchFullScreen = regionByImage ? false : ocrFullScreen_;
            action.imagePath = ocrFindImagePath_;
            if (regionByImage) {
                action.matchThreshold = std::clamp(ToDouble(ocrFindMatchThreshold_, 65.0), 1.0, 100.0);
                action.imageScaleMin = std::max(0.1, ToDouble(ocrFindScaleMin_, 1.0));
                action.imageScaleMax = std::max(action.imageScaleMin, ToDouble(ocrFindScaleMax_, action.imageScaleMin));
                action.imageScale = (action.imageScaleMin + action.imageScaleMax) * 0.5;
            }
            action.ocrResultMode = std::clamp(popupOcrResultMode_.sel, 0, 1);
            action.ocrSearchText = GetText(ocrSearchEdit_);
            action.ocrFollowUp = std::clamp(popupOcrFollowUp_.sel, 0, 2);
            action.offsetX = ToInt(ocrOffsetX_);
            action.offsetY = ToInt(ocrOffsetY_);
            action.findUntilFound = Checked(ocrUntilFound_);
            action.matchVarName = Trim(GetText(ocrResultVar_));
            if (action.matchVarName.empty()) action.matchVarName = L"a";
        }
        else if (sel == 19) {
            action.type = ActionType::If;
            action.conditionExpr = GetText(ifConditionList_);
        }
        else if (sel == 20) {
            action.type = ActionType::Else;
        }
        else if (sel == 21) {
            action.type = ActionType::LockScreenshot;
        }
        else if (sel == 22) {
            action.type = ActionType::UnlockScreenshot;
        }
        else if (sel == 23) {
            action.type = ActionType::StopMacro;
        }
        else if (sel == 24) {
            action.type = ActionType::RunProgram;
            action.shortcutPreset = std::clamp(popupRunProgram_.sel, 0, RunProgramPresetCount() - 1);
            action.targetPath = Trim(GetText(runProgramPath_));
            action.inputText = GetText(runProgramArgs_);
            action.blockName = RunProgramDisplayName(action.shortcutPreset, action.targetPath);
        }
        else if (sel == 25) {
            action.type = ActionType::CloseProgram;
            action.targetPath = Trim(GetText(closeProgramPath_));
            action.matchFileNameOnly = closeProgramMatchFileName_ && Checked(closeProgramMatchFileName_);
        }
        else if (sel == 26) {
            action.type = ActionType::OpenWebpage;
            action.targetPath = Trim(GetText(openWebpageUrl_));
        }
        else if (sel == 27) {
            action.type = ActionType::OpenFile;
            action.targetPath = Trim(GetText(openFilePath_));
        }
        else if (sel == 28) {
            action.type = ActionType::TimerRecordTime;
            action.loopVarName = Trim(GetText(timerVarName_));
        }
        else if (sel == 32) {
            action.type = ActionType::GetCursorPos;
            action.matchVarName = Trim(GetText(cursorPosVarName_));
        }
        else if (sel == 29) {
            action.type = ActionType::AiTextAnalysis;
            action.aiPrompt = GetText(aiPromptEdit_);
            action.aiOutputVarName = Trim(GetText(aiOutputVarEdit_));
            if (action.aiOutputVarName.empty()) action.aiOutputVarName = L"aiResult";
            action.aiOutputType = std::clamp(popupAiOutputType_.sel, 0, 1);
            action.aiModelName = popupAiModel_.sel >= 0 && popupAiModel_.sel < static_cast<int>(popupAiModel_.items.size())
                ? popupAiModel_.items[static_cast<size_t>(popupAiModel_.sel)] : L"";
            action.aiContextMode = std::clamp(popupAiContextMode_.sel, 0, 3);
            action.aiTimeoutSec = std::max(5, ToInt(aiTimeoutEdit_, 30));
            action.aiFallbackValue = Trim(GetText(aiFallbackEdit_));
        }
        else if (sel == 30) {
            action.type = ActionType::AiImageAnalysis;
            action.aiPrompt = GetText(aiPromptEdit_);
            action.aiOutputVarName = Trim(GetText(aiOutputVarEdit_));
            if (action.aiOutputVarName.empty()) action.aiOutputVarName = L"aiImgResult";
            action.aiOutputType = std::clamp(popupAiOutputType_.sel, 0, 1);
            action.aiModelName = popupAiModel_.sel >= 0 && popupAiModel_.sel < static_cast<int>(popupAiModel_.items.size())
                ? popupAiModel_.items[static_cast<size_t>(popupAiModel_.sel)] : L"";
            action.aiContextMode = std::clamp(popupAiContextMode_.sel, 0, 3);
            action.aiTimeoutSec = std::max(5, ToInt(aiTimeoutEdit_, 30));
            action.aiFallbackValue = Trim(GetText(aiFallbackEdit_));
            action.aiImageScale = std::clamp(ToDouble(aiImageScaleEdit_, 1.0), 0.1, 1.0);
            action.aiRegionByImage = aiRegionByImageCheck_ && Checked(aiRegionByImageCheck_);
            action.aiTargetImagePath = aiFindImagePath_;
            action.aiSearchX1 = ToInt(aiSearchX1Edit_);
            action.aiSearchY1 = ToInt(aiSearchY1Edit_);
            action.aiSearchX2 = ToInt(aiSearchX2Edit_);
            action.aiSearchY2 = ToInt(aiSearchY2Edit_);
            action.searchFullScreen = action.aiRegionByImage ? false : aiFullScreen_;
            if (action.aiRegionByImage) {
                action.matchThreshold = std::clamp(ToDouble(aiFindMatchThreshold_, 65.0), 1.0, 100.0);
                action.imageScaleMin = std::max(0.1, ToDouble(aiFindScaleMin_, 1.0));
                action.imageScaleMax = std::max(action.imageScaleMin, ToDouble(aiFindScaleMax_, action.imageScaleMin));
                action.imageScale = (action.imageScaleMin + action.imageScaleMax) * 0.5;
            }
        }
        else if (sel == 31) {
            action.type = ActionType::AiActionExecute;
            action.aiPrompt = GetText(aiPromptEdit_);
            action.aiModelName = popupAiModel_.sel >= 0 && popupAiModel_.sel < static_cast<int>(popupAiModel_.items.size())
                ? popupAiModel_.items[static_cast<size_t>(popupAiModel_.sel)] : L"";
            action.aiContextMode = std::clamp(popupAiContextMode_.sel, 0, 3);
            action.aiTimeoutSec = std::max(5, ToInt(aiTimeoutEdit_, 30));
            action.aiFallbackValue = Trim(GetText(aiFallbackEdit_));
            action.aiImageScale = 0.5;
            action.aiWithImage = aiWithImageCheck_ && Checked(aiWithImageCheck_);
            action.aiRegionByImage = action.aiWithImage && aiRegionByImageCheck2_ && Checked(aiRegionByImageCheck2_);
            action.aiTargetImagePath = aiFindImagePath_;
            action.aiSearchX1 = ToInt(aiSearchX1Edit2_);
            action.aiSearchY1 = ToInt(aiSearchY1Edit2_);
            action.aiSearchX2 = ToInt(aiSearchX2Edit2_);
            action.aiSearchY2 = ToInt(aiSearchY2Edit2_);
            action.searchFullScreen = action.aiRegionByImage ? false : aiFullScreen_;
            if (action.aiRegionByImage) {
                action.matchThreshold = std::clamp(ToDouble(aiFindMatchThreshold_, 65.0), 1.0, 100.0);
                action.imageScaleMin = std::max(0.1, ToDouble(aiFindScaleMin_, 1.0));
                action.imageScaleMax = std::max(action.imageScaleMin, ToDouble(aiFindScaleMax_, action.imageScaleMin));
                action.imageScale = (action.imageScaleMin + action.imageScaleMax) * 0.5;
            }
            action.aiMaxSteps = ToInt(aiMaxStepsEdit_, 10);
            action.aiConfirmExecute = Checked(aiConfirmExecute_);
        }
        else { action.type = ActionType::MoveMouse; }
        return action;
    }

    void LoadForm(const ScriptAction& action) {
        loadingForm_ = true;
        SetText(remark_, action.remark);
        if (action.type == ActionType::MoveMouse) {
            SetPopupSel(popupAction_, actionCombo_, 0);
            SetText(moveX_, std::to_wstring(action.x));
            SetText(moveY_, std::to_wstring(action.y));
            SetText(moveRandomX_, std::to_wstring(action.randomX));
            SetText(moveRandomY_, std::to_wstring(action.randomY));
            SetChecked(moveFromVar_, action.moveFromVar);
            SetText(moveVarX_, action.moveVarExprX.empty() ? L"0" : action.moveVarExprX);
            SetText(moveVarY_, action.moveVarExprY.empty() ? L"0" : action.moveVarExprY);
        }
        else if (action.type == ActionType::Wait) { SetPopupSel(popupAction_, actionCombo_, 1); SetText(waitDuration_, F3(action.duration)); SetText(waitRandom_, F3(action.randomDuration)); }
        else if (action.type == ActionType::MouseDown || action.type == ActionType::MouseUp) { SetPopupSel(popupAction_, actionCombo_, ComboSelForType(action.type)); SetPopupSel(popupMouseBtn_, mousePressButton_, static_cast<int>(action.button)); WriteModifierHolds(action, mousePressLWin_, mousePressRWin_, mousePressLCtrl_, mousePressRCtrl_, mousePressLAlt_, mousePressRAlt_, mousePressLShift_, mousePressRShift_); }
        else if (action.type == ActionType::MouseClick) { SetPopupSel(popupAction_, actionCombo_, 2); SetPopupSel(popupClickBtn_, clickButton_, static_cast<int>(action.button)); SetText(clickCount_, std::to_wstring(action.clickCount)); SetText(clickWait_, F3(action.duration)); SetText(clickRandom_, F3(action.randomDuration)); WriteModifierHolds(action, clickLWin_, clickRWin_, clickLCtrl_, clickRCtrl_, clickLAlt_, clickRAlt_, clickLShift_, clickRShift_); }
        else if (action.type == ActionType::MousePlayback) {
            SetPopupSel(popupAction_, actionCombo_, 3);
            RefreshMousePlaybackCombo();
            if (!action.blockName.empty()) {
                int idx = -1;
                for (size_t i = 0; i < popupMousePlayback_.items.size(); ++i) {
                    if (popupMousePlayback_.items[i] == action.blockName) { idx = static_cast<int>(i); break; }
                }
                SetPopupSel(popupMousePlayback_, mousePlaybackCombo_, idx);
            }
            SetText(mousePlaybackCount_, std::to_wstring(action.clickCount));
            SetText(mousePlaybackWait_, F3(action.duration));
            SetText(mousePlaybackRandom_, F3(action.randomDuration));
        }
        else if (action.type == ActionType::RunMacro) {
            SetPopupSel(popupAction_, actionCombo_, 4);
            RefreshRunMacroCombo();
            if (!action.blockName.empty()) {
                int idx = -1;
                for (size_t i = 0; i < popupRunMacro_.items.size(); ++i) {
                    if (popupRunMacro_.items[i] == action.blockName) { idx = static_cast<int>(i); break; }
                }
                SetPopupSel(popupRunMacro_, runMacroCombo_, idx);
            }
        }
        else if (action.type == ActionType::KeyDown || action.type == ActionType::KeyUp) { SetPopupSel(popupAction_, actionCombo_, ComboSelForType(action.type)); formKeyPressText_ = action.keyText.empty() ? L"7" : action.keyText; formKeyPressVk_ = action.keyVk == 0 ? '7' : action.keyVk; SetText(keyPressEdit_, formKeyPressText_); WriteModifierHolds(action, keyPressLWin_, keyPressRWin_, keyPressLCtrl_, keyPressRCtrl_, keyPressLAlt_, keyPressRAlt_, keyPressLShift_, keyPressRShift_); }
        else if (action.type == ActionType::KeyClick) { SetPopupSel(popupAction_, actionCombo_, 8); formKeyText_ = action.keyText.empty() ? L"7" : action.keyText; formKeyVk_ = action.keyVk == 0 ? '7' : action.keyVk; SetText(keyEdit_, formKeyText_); SetText(keyCount_, std::to_wstring(action.clickCount)); SetText(keyWait_, F3(action.duration)); SetText(keyRandom_, F3(action.randomDuration)); WriteModifierHolds(action, keyLWin_, keyRWin_, keyLCtrl_, keyRCtrl_, keyLAlt_, keyRAlt_, keyLShift_, keyRShift_); }
        else if (action.type == ActionType::HotkeyShortcut) {
            SetPopupSel(popupAction_, actionCombo_, 11);
            SetPopupSel(popupHotkeyShortcut_, hotkeyShortcutCombo_, std::clamp(action.shortcutPreset, 0, ShortcutPresetCount() - 1));
            SetText(hotkeyShortcutCount_, std::to_wstring(action.clickCount));
            SetText(hotkeyShortcutWait_, F3(action.duration));
            SetText(hotkeyShortcutRandom_, F3(action.randomDuration));
        }
        else if (action.type == ActionType::QuickInput) {
            SetPopupSel(popupAction_, actionCombo_, 12);
            RefreshQuickInputVarCombo();
            SetText(quickInputEdit_, action.inputText);
            SetText(quickInputCharInterval_, F3(action.charInterval));
            SetText(quickInputCount_, std::to_wstring(action.clickCount));
            SetText(quickInputWait_, F3(action.duration));
            SetText(quickInputRandom_, F3(action.randomDuration));
        }
        else if (action.type == ActionType::Loop) { SetPopupSel(popupAction_, actionCombo_, 13); SetText(loopCount_, std::to_wstring(action.loopCount)); SetChecked(loopFromVar_, action.loopFromVar); SetText(loopVarExpr_, action.loopVarExpr); SetText(loopVarName_, action.loopVarName); SetPopupSel(popupLoopType_, loopTypeCombo_, 0); }
        else if (action.type == ActionType::EndLoop) { SetPopupSel(popupAction_, actionCombo_, 14); }
        else if (action.type == ActionType::DefineBlock) { SetPopupSel(popupAction_, actionCombo_, 15); SetText(defineBlockName_, action.blockName.empty() ? L"block1" : action.blockName); }
        else if (action.type == ActionType::RunBlock) { SetPopupSel(popupAction_, actionCombo_, 16); RefreshRunBlockCombo(); if (!action.blockName.empty()) { int idx = -1; for (size_t i = 0; i < popupRunBlock_.items.size(); ++i) { if (popupRunBlock_.items[i] == action.blockName) { idx = static_cast<int>(i); break; } } SetPopupSel(popupRunBlock_, runBlockCombo_, idx); } }
        else if (action.type == ActionType::ScrollWheel) {
            SetPopupSel(popupAction_, actionCombo_, 7);
            SetChecked(scrollVertical_, action.scrollVertical);
            SetChecked(scrollHorizontal_, action.scrollHorizontal);
            SetText(scrollSteps_, std::to_wstring(action.scrollSteps));
            SetPopupSel(popupScrollDir_, scrollDirectionCombo_, std::clamp(action.scrollDirection, 0, 1));
            SetText(scrollCount_, std::to_wstring(action.clickCount));
            SetText(scrollWait_, F3(action.duration));
            SetText(scrollRandom_, F3(action.randomDuration));
        }
        else if (action.type == ActionType::FindImage) {
            SetPopupSel(popupAction_, actionCombo_, 17);
            findImageFullScreen_ = action.searchFullScreen;
            findImagePath_ = action.imagePath;
            if (action.searchFullScreen) {
                ApplyFindImageFullScreen();
            } else {
                SetText(findX1_, std::to_wstring(action.searchX1));
                SetText(findY1_, std::to_wstring(action.searchY1));
                SetText(findX2_, std::to_wstring(action.searchX2));
                SetText(findY2_, std::to_wstring(action.searchY2));
                RefreshCoordFieldEdits({findX1_, findY1_, findX2_, findY2_});
            }
            SetText(findMatchThreshold_, std::to_wstring(static_cast<int>(action.matchThreshold)));
            SetText(findScaleMin_, F3(action.imageScaleMin > 0.0 ? action.imageScaleMin : action.imageScale));
            SetText(findScaleMax_, F3(action.imageScaleMax > 0.0 ? action.imageScaleMax : action.imageScale));
            SetPopupSel(popupFindFollowUp_, findFollowUpCombo_, std::clamp(action.findImageFollowUp, 0, 2));
            SetText(findOffsetX_, std::to_wstring(action.offsetX));
            SetText(findOffsetY_, std::to_wstring(action.offsetY));
            SetChecked(findUntilFound_, action.findUntilFound);
            SetText(findMatchVar_, action.matchVarName.empty() ? L"matchRet" : action.matchVarName);
            UpdateFindImagePreview();
        }
        else if (action.type == ActionType::TextRecognition) {
            SetPopupSel(popupAction_, actionCombo_, 18);
            ocrFullScreen_ = action.searchFullScreen;
            SetChecked(ocrRegionByImageCheck_, action.ocrRegionByImage);
            SetChecked(ocrDigitsOnlyCheck_, action.ocrDigitsOnly);
            ocrFindImagePath_ = action.ocrRegionByImage ? action.imagePath : L"";
            if (action.ocrRegionByImage) {
                SetText(ocrFindMatchThreshold_, std::to_wstring(static_cast<int>(action.matchThreshold)));
                SetText(ocrFindScaleMin_, F3(action.imageScaleMin > 0.0 ? action.imageScaleMin : action.imageScale));
                SetText(ocrFindScaleMax_, F3(action.imageScaleMax > 0.0 ? action.imageScaleMax : action.imageScale));
                UpdateOcrFindImagePreview();
            } else {
                UpdateOcrFindImagePreview();
            }
            if (!action.ocrRegionByImage && action.searchFullScreen) {
                ApplyOcrFullScreen();
            } else {
                SetText(ocrX1_, std::to_wstring(action.searchX1));
                SetText(ocrY1_, std::to_wstring(action.searchY1));
                SetText(ocrX2_, std::to_wstring(action.searchX2));
                SetText(ocrY2_, std::to_wstring(action.searchY2));
                RefreshCoordFieldEdits({ocrX1_, ocrY1_, ocrX2_, ocrY2_});
            }
            SetPopupSel(popupOcrResultMode_, ocrResultModeCombo_, std::clamp(action.ocrResultMode, 0, 1));
            SetText(ocrSearchEdit_, action.ocrSearchText);
            SetPopupSel(popupOcrFollowUp_, ocrFollowUpCombo_, std::clamp(action.ocrFollowUp, 0, 2));
            SetText(ocrOffsetX_, std::to_wstring(action.offsetX));
            SetText(ocrOffsetY_, std::to_wstring(action.offsetY));
            SetChecked(ocrUntilFound_, action.findUntilFound);
            SetText(ocrResultVar_, action.matchVarName.empty() ? L"a" : action.matchVarName);
            RefreshOcrSearchVarCombo();
        }
        else if (action.type == ActionType::If) {
            SetPopupSel(popupAction_, actionCombo_, 19);
            RefreshIfVarCombo();
            SetText(ifConditionList_, action.conditionExpr);
            SetText(ifValueEdit_, L"0");
            SetPopupSel(popupIfOperator_, ifOperatorCombo_, 0);
            SetPopupSel(popupIfConnector_, ifConnectorCombo_, 0);
        }
        else if (action.type == ActionType::Else) {
            SetPopupSel(popupAction_, actionCombo_, 20);
        }
        else if (action.type == ActionType::LockScreenshot) {
            SetPopupSel(popupAction_, actionCombo_, 21);
        }
        else if (action.type == ActionType::UnlockScreenshot) {
            SetPopupSel(popupAction_, actionCombo_, 22);
        }
        else if (action.type == ActionType::StopMacro) {
            SetPopupSel(popupAction_, actionCombo_, 23);
        }
        else if (action.type == ActionType::RunProgram) {
            SetPopupSel(popupAction_, actionCombo_, 24);
            SetPopupSel(popupRunProgram_, runProgramCombo_, std::clamp(action.shortcutPreset, 0, RunProgramPresetCount() - 1));
            SetText(runProgramPath_, action.targetPath);
            SetText(runProgramArgs_, action.inputText);
        }
        else if (action.type == ActionType::CloseProgram) {
            SetPopupSel(popupAction_, actionCombo_, 25);
            SetText(closeProgramPath_, action.targetPath);
            SetChecked(closeProgramMatchFileName_, action.matchFileNameOnly);
        }
        else if (action.type == ActionType::OpenWebpage) {
            SetPopupSel(popupAction_, actionCombo_, 26);
            SetText(openWebpageUrl_, action.targetPath);
        }
        else if (action.type == ActionType::OpenFile) {
            SetPopupSel(popupAction_, actionCombo_, 27);
            SetText(openFilePath_, action.targetPath);
        }
        else if (action.type == ActionType::TimerRecordTime) {
            SetPopupSel(popupAction_, actionCombo_, 28);
            SetText(timerVarName_, action.loopVarName);
        }
        else if (action.type == ActionType::GetCursorPos) {
            SetPopupSel(popupAction_, actionCombo_, 32);
            SetText(cursorPosVarName_, action.matchVarName);
        }
        else if (action.type == ActionType::AiTextAnalysis) {
            SetPopupSel(popupAction_, actionCombo_, 29);
            RefreshAiModelCombo();
            SetText(aiPromptEdit_, action.aiPrompt);
            SetText(aiOutputVarEdit_, action.aiOutputVarName.empty() ? L"aiResult" : action.aiOutputVarName);
            SetPopupSel(popupAiOutputType_, aiOutputTypeCombo_, std::clamp(action.aiOutputType, 0, 1));
            if (!action.aiModelName.empty()) {
                int idx = -1;
                for (size_t i = 0; i < popupAiModel_.items.size(); ++i) {
                    if (popupAiModel_.items[i] == action.aiModelName) { idx = static_cast<int>(i); break; }
                }
                SetPopupSel(popupAiModel_, aiModelCombo_, idx);
            } else if (!popupAiModel_.items.empty()) {
                SetPopupSel(popupAiModel_, aiModelCombo_, popupAiModel_.sel >= 0 ? popupAiModel_.sel : 0);
            }
            SetPopupSel(popupAiContextMode_, aiContextModeCombo_, std::clamp(action.aiContextMode, 0, 3));
            SetText(aiTimeoutEdit_, std::to_wstring(action.aiTimeoutSec));
            SetText(aiFallbackEdit_, action.aiFallbackValue);
        }
        else if (action.type == ActionType::AiImageAnalysis) {
            SetPopupSel(popupAction_, actionCombo_, 30);
            RefreshAiModelCombo();
            SetText(aiPromptEdit_, action.aiPrompt);
            SetText(aiOutputVarEdit_, action.aiOutputVarName.empty() ? L"aiImgResult" : action.aiOutputVarName);
            SetPopupSel(popupAiOutputType_, aiOutputTypeCombo_, std::clamp(action.aiOutputType, 0, 1));
            if (!action.aiModelName.empty()) {
                int idx = -1;
                for (size_t i = 0; i < popupAiModel_.items.size(); ++i) {
                    if (popupAiModel_.items[i] == action.aiModelName) { idx = static_cast<int>(i); break; }
                }
                SetPopupSel(popupAiModel_, aiModelCombo_, idx);
            } else if (!popupAiModel_.items.empty()) {
                SetPopupSel(popupAiModel_, aiModelCombo_, popupAiModel_.sel >= 0 ? popupAiModel_.sel : 0);
            }
            SetPopupSel(popupAiContextMode_, aiContextModeCombo_, std::clamp(action.aiContextMode, 0, 3));
            SetText(aiTimeoutEdit_, std::to_wstring(action.aiTimeoutSec));
            SetText(aiFallbackEdit_, action.aiFallbackValue);
            SetChecked(aiRegionByImageCheck_, action.aiRegionByImage);
            aiFindImagePath_ = action.aiTargetImagePath;
            if (action.aiRegionByImage) {
                SetText(aiFindMatchThreshold_, F3(action.matchThreshold));
                SetText(aiFindScaleMin_, F3(action.imageScaleMin > 0.0 ? action.imageScaleMin : 0.9));
                SetText(aiFindScaleMax_, F3(action.imageScaleMax > 0.0 ? action.imageScaleMax : 1.1));
            }
            UpdateAiFindImagePreview();
            SetText(aiImageScaleEdit_, F3(action.aiImageScale));
            if (!action.aiRegionByImage && action.searchFullScreen) {
                ApplyAiFullScreen();
            } else {
                SetText(aiSearchX1Edit_, std::to_wstring(action.aiSearchX1));
                SetText(aiSearchY1Edit_, std::to_wstring(action.aiSearchY1));
                SetText(aiSearchX2Edit_, std::to_wstring(action.aiSearchX2));
                SetText(aiSearchY2Edit_, std::to_wstring(action.aiSearchY2));
                RefreshCoordFieldEdits({aiSearchX1Edit_, aiSearchY1Edit_, aiSearchX2Edit_, aiSearchY2Edit_});
            }
        }
        else if (action.type == ActionType::AiActionExecute) {
            SetPopupSel(popupAction_, actionCombo_, 31);
            RefreshAiModelCombo();
            SetText(aiPromptEdit_, action.aiPrompt);
            if (!action.aiModelName.empty()) {
                int idx = -1;
                for (size_t i = 0; i < popupAiModel_.items.size(); ++i) {
                    if (popupAiModel_.items[i] == action.aiModelName) { idx = static_cast<int>(i); break; }
                }
                SetPopupSel(popupAiModel_, aiModelCombo_, idx);
            } else if (!popupAiModel_.items.empty()) {
                SetPopupSel(popupAiModel_, aiModelCombo_, popupAiModel_.sel >= 0 ? popupAiModel_.sel : 0);
            }
            SetPopupSel(popupAiContextMode_, aiContextModeCombo_, std::clamp(action.aiContextMode, 0, 3));
            SetText(aiTimeoutEdit_, std::to_wstring(action.aiTimeoutSec));
            SetText(aiFallbackEdit_, action.aiFallbackValue);
            SetChecked(aiWithImageCheck_, action.aiWithImage);
            SetChecked(aiRegionByImageCheck2_, action.aiRegionByImage);
            aiFindImagePath_ = action.aiTargetImagePath;
            if (action.aiRegionByImage) {
                SetText(aiFindMatchThreshold_, F3(action.matchThreshold));
                SetText(aiFindScaleMin_, F3(action.imageScaleMin > 0.0 ? action.imageScaleMin : 0.9));
                SetText(aiFindScaleMax_, F3(action.imageScaleMax > 0.0 ? action.imageScaleMax : 1.1));
            }
            UpdateAiFindImagePreview();
            if (!action.aiRegionByImage && action.searchFullScreen) {
                ApplyAiFullScreen();
            } else {
                SetText(aiSearchX1Edit2_, std::to_wstring(action.aiSearchX1));
                SetText(aiSearchY1Edit2_, std::to_wstring(action.aiSearchY1));
                SetText(aiSearchX2Edit2_, std::to_wstring(action.aiSearchX2));
                SetText(aiSearchY2Edit2_, std::to_wstring(action.aiSearchY2));
                RefreshCoordFieldEdits({aiSearchX1Edit2_, aiSearchY1Edit2_, aiSearchX2Edit2_, aiSearchY2Edit2_});
            }
            SetText(aiMaxStepsEdit_, std::to_wstring(action.aiMaxSteps));
            SetChecked(aiConfirmExecute_, action.aiConfirmExecute);
        }
        else { SetPopupSel(popupAction_, actionCombo_, 14); }
        RefreshParamPanel();
        loadingForm_ = false;
        RECT actionRc = WindowClientRect(actionCombo_);
        RECT panelRc{actionRc.left, actionRc.bottom + 4, kEditorWidth, kEditorHeight - kBottomH};
        InvalidateRect(hwnd_, &panelRc, FALSE);
    }

    // findImagePath_ now stored in ScriptAction, above functions use utils::FindImagesDir()/EnsureFindImagesDir()

    void UpdateFindImagePreview() {
        if (findImagePreviewBitmap_) {
            DeleteBitmapHandle(findImagePreviewBitmap_);
            findImagePreviewBitmap_ = nullptr;
        }
        if (!findImagePath_.empty()) {
            findImagePreviewBitmap_ = LoadBitmapFromFile(findImagePath_);
        }
        if (findImagePreviewBtn_) {
            RedrawWindow(findImagePreviewBtn_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
        }
    }

    void UpdateOcrFindImagePreview() {
        if (ocrFindImagePreviewBitmap_) {
            DeleteBitmapHandle(ocrFindImagePreviewBitmap_);
            ocrFindImagePreviewBitmap_ = nullptr;
        }
        if (!ocrFindImagePath_.empty()) {
            ocrFindImagePreviewBitmap_ = LoadBitmapFromFile(ocrFindImagePath_);
        }
        if (ocrFindImagePreviewBtn_) {
            RedrawWindow(ocrFindImagePreviewBtn_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
        }
    }

    void UpdateAiFindImagePreview() {
        if (aiFindImagePreviewBitmap_) {
            DeleteBitmapHandle(aiFindImagePreviewBitmap_);
            aiFindImagePreviewBitmap_ = nullptr;
        }
        if (!aiFindImagePath_.empty()) {
            aiFindImagePreviewBitmap_ = LoadBitmapFromFile(aiFindImagePath_);
        }
        if (aiFindImagePreviewBtn_) {
            RedrawWindow(aiFindImagePreviewBtn_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
        }
    }

    void SetAiRegionCoords(int x1, int y1, int x2, int y2) {
        const std::wstring sx1 = std::to_wstring(x1);
        const std::wstring sy1 = std::to_wstring(y1);
        const std::wstring sx2 = std::to_wstring(x2);
        const std::wstring sy2 = std::to_wstring(y2);
        if (popupAction_.sel == 30) {
            SetText(aiSearchX1Edit_, sx1);
            SetText(aiSearchY1Edit_, sy1);
            SetText(aiSearchX2Edit_, sx2);
            SetText(aiSearchY2Edit_, sy2);
        } else if (popupAction_.sel == 31) {
            SetText(aiSearchX1Edit2_, sx1);
            SetText(aiSearchY1Edit2_, sy1);
            SetText(aiSearchX2Edit2_, sx2);
            SetText(aiSearchY2Edit2_, sy2);
        } else {
            if (aiSearchX1Edit_) { SetText(aiSearchX1Edit_, sx1); SetText(aiSearchY1Edit_, sy1); SetText(aiSearchX2Edit_, sx2); SetText(aiSearchY2Edit_, sy2); }
            if (aiSearchX1Edit2_) { SetText(aiSearchX1Edit2_, sx1); SetText(aiSearchY1Edit2_, sy1); SetText(aiSearchX2Edit2_, sx2); SetText(aiSearchY2Edit2_, sy2); }
        }
    }

    bool ResolveOcrAbsRegionFromFindImage(int relX1, int relY1, int relX2, int relY2,
                                          int& outX1, int& outY1, int& outX2, int& outY2) const {
        if (ocrFindImagePath_.empty()) return false;
        int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
        GetVirtualScreenRect(vsX, vsY, vsW, vsH);
        const double threshold = std::clamp(ToDouble(ocrFindMatchThreshold_, 65.0), 1.0, 100.0);
        const double scaleMin = std::max(0.1, ToDouble(ocrFindScaleMin_, 1.0));
        const double scaleMax = std::max(scaleMin, ToDouble(ocrFindScaleMax_, scaleMin));
        HBITMAP tmpl = LoadBitmapFromFile(ocrFindImagePath_);
        if (!tmpl) return false;
        ImageMatchOptions opt;
        opt.thresholdPercent = threshold;
        opt.scaleMin = scaleMin;
        opt.scaleMax = scaleMax;
        opt.scaleStep = 0.05;
        opt.maxMatches = 20;
        opt.maxOverlap = 0.5;
        const ImageMatchOutput output = FindTemplateOnScreenMulti(
            vsX, vsY, vsX + vsW, vsY + vsH, tmpl, opt);
        DeleteBitmapHandle(tmpl);
        if (output.matches.empty()) return false;
        const ImageMatchResult& match = output.matches.front();
        outX1 = match.topLeftX + relX1;
        outY1 = match.topLeftY + relY1;
        outX2 = match.topLeftX + relX2;
        outY2 = match.topLeftY + relY2;
        return true;
    }

    void RefreshCoordFieldEdits(std::initializer_list<HWND> edits) {
        for (HWND h : edits) {
            if (!h) continue;
            InvalidateRect(h, nullptr, TRUE);
            RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        }
        RepaintParamPanelChrome();
    }

    void EnsureFindImageRegionDefaults() {
        findImageFullScreen_ = true;
        ApplyFindImageFullScreen();
    }

    void EnsureOcrRegionDefaults() {
        if (ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_)) return;
        ocrFullScreen_ = true;
        ApplyOcrFullScreen();
    }

    void EnsureAiRegionDefaults() {
        const int sel = popupAction_.sel;
        const bool regionByImage = (sel == 30 && aiRegionByImageCheck_ && Checked(aiRegionByImageCheck_))
            || (sel == 31 && aiWithImageCheck_ && Checked(aiWithImageCheck_)
                && aiRegionByImageCheck2_ && Checked(aiRegionByImageCheck2_));
        if (regionByImage) return;
        aiFullScreen_ = true;
        ApplyAiFullScreen();
    }

    void ApplyFindImageFullScreen() {
        findImageFullScreen_ = true;
        int x = 0, y = 0, w = 0, h = 0;
        GetVirtualScreenRect(x, y, w, h);
        SetText(findX1_, std::to_wstring(x));
        SetText(findY1_, std::to_wstring(y));
        SetText(findX2_, std::to_wstring(x + w));
        SetText(findY2_, std::to_wstring(y + h));
        RefreshCoordFieldEdits({findX1_, findY1_, findX2_, findY2_});
    }

    void BeginFindRegionSelect(bool captureTemplate) {
        if (!hwnd_) return;
        if (!screenshotOverlay_) screenshotOverlay_ = std::make_unique<ScreenshotOverlay>();
        screenshotOverlay_->SetTitle(captureTemplate ? L"屏幕截图" : L"选取区域");
        // Store previous window state and hide it
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);
        screenshotOverlay_->Show([this, captureTemplate](RECT sel) {
            if (sel.left == 0 && sel.top == 0 && sel.right == 0 && sel.bottom == 0) {
                RestoreEditorAfterScreenOverlay();
                return;
            }
            // Client coords from overlay → screen coords (add virtual screen origin)
            const int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const int x1 = sel.left + vsX, y1 = sel.top + vsY;
            const int x2 = sel.right + vsX, y2 = sel.bottom + vsY;
            if (captureTemplate) {
                // Screenshot mode: capture template image ONLY — do NOT touch search region
                EnsureFindImagesDir();
                const std::wstring path = FindImagesDir() + L"\\template_"
                    + std::to_wstring(GetTickCount()) + L".bmp";
                HBITMAP bmp = CaptureScreenRegion(x1, y1, x2, y2);
            if (bmp && SaveBitmapToFile(bmp, path)) {
                findImagePath_ = path;
                newImagePaths_.insert(path);
                findImageFullScreen_ = false;
                if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(actions_.size())) {
                    actions_[static_cast<size_t>(selectedIndex_)].imagePath = path;
                }
                UpdateFindImagePreview();
                }
                DeleteBitmapHandle(bmp);
            } else {
                // Region select mode: update coordinates only
                findImageFullScreen_ = false;
                SetText(findX1_, std::to_wstring(x1));
                SetText(findY1_, std::to_wstring(y1));
                SetText(findX2_, std::to_wstring(x2));
                SetText(findY2_, std::to_wstring(y2));
            }
            RestoreEditorAfterScreenOverlay();
        });
    }

    bool HasFindImageTemplate(const std::wstring& path) const {
        return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    void BeginOcrFindRegionSelect() {
        if (!ocrRegionByImageCheck_ || !Checked(ocrRegionByImageCheck_)) return;
        if (!HasFindImageTemplate(ocrFindImagePath_)) {
            MessageBoxW(hwnd_, L"请先设置要查找的图片。", L"选取区域", MB_OK | MB_ICONINFORMATION);
            return;
        }
        int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
        GetVirtualScreenRect(vsX, vsY, vsW, vsH);
        const double threshold = std::clamp(ToDouble(ocrFindMatchThreshold_, 65.0), 1.0, 100.0);
        const double scaleMin = std::max(0.1, ToDouble(ocrFindScaleMin_, 1.0));
        const double scaleMax = std::max(scaleMin, ToDouble(ocrFindScaleMax_, scaleMin));

        if (!matchOverlay_) matchOverlay_ = std::make_unique<MatchOverlay>();
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);

        const auto result = matchOverlay_->Show(
            ocrFindImagePath_, vsX, vsY, vsX + vsW, vsY + vsH,
            threshold, scaleMin, scaleMax, MatchOverlayMode::RelativeRegionPick);

        RestoreEditorAfterScreenOverlay();

        if (!result.cancelled && result.regionValid && matchOverlay_->matchResult_.found) {
            const int anchorX = matchOverlay_->matchResult_.topLeftX;
            const int anchorY = matchOverlay_->matchResult_.topLeftY;
            ocrFullScreen_ = false;
            SetText(ocrX1_, std::to_wstring(result.regionX1 - anchorX));
            SetText(ocrY1_, std::to_wstring(result.regionY1 - anchorY));
            SetText(ocrX2_, std::to_wstring(result.regionX2 - anchorX));
            SetText(ocrY2_, std::to_wstring(result.regionY2 - anchorY));
        }
    }

    void BeginOcrFindScreenshot() {
        if (!hwnd_) return;
        if (!screenshotOverlay_) screenshotOverlay_ = std::make_unique<ScreenshotOverlay>();
        screenshotOverlay_->SetTitle(L"屏幕截图");
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);
        screenshotOverlay_->Show([this](RECT sel) {
            const bool cancelled = sel.left == 0 && sel.top == 0 && sel.right == 0 && sel.bottom == 0;
            if (!cancelled) {
                const int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
                const int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
                const int x1 = sel.left + vsX, y1 = sel.top + vsY;
                const int x2 = sel.right + vsX, y2 = sel.bottom + vsY;
                EnsureFindImagesDir();
                const std::wstring path = FindImagesDir() + L"\\template_"
                    + std::to_wstring(GetTickCount()) + L".bmp";
                HBITMAP bmp = CaptureScreenRegion(x1, y1, x2, y2);
                if (bmp && SaveBitmapToFile(bmp, path)) {
                    ocrFindImagePath_ = path;
                    newImagePaths_.insert(path);
                    if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(actions_.size())) {
                        actions_[static_cast<size_t>(selectedIndex_)].imagePath = path;
                    }
                    UpdateOcrFindImagePreview();
                }
                DeleteBitmapHandle(bmp);
            }
            RestoreEditorAfterScreenOverlay();
        });
    }

    void LoadOcrFindImageFromFile() {
        wchar_t fileName[MAX_PATH]{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"图片文件\0*.bmp;*.png;*.jpg;*.jpeg\0所有文件\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return;
        const std::wstring copied = EnsureImageInLibrary(fileName);
        ocrFindImagePath_ = copied;
        if (IsPathInImageDir(copied)) newImagePaths_.insert(copied);
        if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(actions_.size())) {
            actions_[static_cast<size_t>(selectedIndex_)].imagePath = copied;
        }
        UpdateOcrFindImagePreview();
    }

    void ClearOcrFindImage() {
        if (!ocrFindImagePath_.empty() && IsPathInImageDir(ocrFindImagePath_)) {
            auto it = newImagePaths_.find(ocrFindImagePath_);
            if (it != newImagePaths_.end()) {
                const auto allRefs = CollectAllReferencedImages();
                if (allRefs.find(ocrFindImagePath_) == allRefs.end()) {
                    DeleteFileW(ocrFindImagePath_.c_str());
                }
                newImagePaths_.erase(it);
            }
        }
        ocrFindImagePath_.clear();
        if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(actions_.size())) {
            actions_[static_cast<size_t>(selectedIndex_)].imagePath.clear();
        }
        UpdateOcrFindImagePreview();
        RefreshGrayButtonsInParamViewport();
    }

    void BeginOcrOffsetSelect() {
        if (popupOcrFollowUp_.sel == 2) return;

        const bool searchMode = popupOcrResultMode_.sel == 1;
        if (searchMode) {
            if (OcrSearchTextContainsVariable()) return;
            if (Trim(GetText(ocrSearchEdit_)).empty()) {
                MessageBoxW(hwnd_, L"请先输入要在结果中查找的文字。", L"选择偏移", MB_OK | MB_ICONINFORMATION);
                return;
            }
        }

        const OcrEnvStatus env = CheckOcrEnvironment(false);
        if (env.state != OcrEnvState::Ready) {
            ShowOcrInstallDialog();
            return;
        }

        const bool regionByImage = ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_);
        if (regionByImage && ocrFindImagePath_.empty()) {
            MessageBoxW(hwnd_, L"请先设置要查找的图片。", L"选择偏移", MB_OK | MB_ICONINFORMATION);
            return;
        }

        HideEditorForScreenCapture();

        int sx1 = ToInt(ocrX1_), sy1 = ToInt(ocrY1_), sx2 = ToInt(ocrX2_), sy2 = ToInt(ocrY2_);
        if (regionByImage) {
            if (!ResolveOcrAbsRegionFromFindImage(sx1, sy1, sx2, sy2, sx1, sy1, sx2, sy2)) {
                RestoreEditorAfterScreenCapture();
                MessageBoxW(hwnd_, L"未找到参考图片，无法选择偏移位置。", L"选择偏移", MB_OK | MB_ICONINFORMATION);
                return;
            }
        } else if (ocrFullScreen_) {
            int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
            GetVirtualScreenRect(vsX, vsY, vsW, vsH);
            sx1 = vsX; sy1 = vsY; sx2 = vsX + vsW; sy2 = vsY + vsH;
        }

        std::wstring searchTarget;
        if (searchMode) {
            MacroVariableContext ctx;
            ctx.matchVars = &matchVars_;
            ctx.ocrVars = &ocrVars_;
            ctx.loopVars = &loopVars_;
            ctx.timerStarts = &timerStarts_;
            ctx.curLoops = curLoops_;
            searchTarget = ResolveMacroVariables(GetText(ocrSearchEdit_), ctx);
        }

        if (!ocrOverlay_) ocrOverlay_ = std::make_unique<OcrOverlay>();
        EnsureOcrSession();
        const bool digitsOnly = ocrDigitsOnlyCheck_ && Checked(ocrDigitsOnlyCheck_);
        const auto result = ocrOverlay_->Show(sx1, sy1, sx2, sy2, searchTarget, OcrOverlayMode::OffsetPick, digitsOnly);
        ReleaseOcrSession();

        RestoreEditorAfterScreenCapture();

        if (!result.cancelled && result.anchorValid) {
            SetText(ocrOffsetX_, std::to_wstring(result.offsetX));
            SetText(ocrOffsetY_, std::to_wstring(result.offsetY));
        }
    }

    void BeginFindOffsetSelect() {
        if (findImagePath_.empty()) {
            MessageBoxW(hwnd_, L"请先设置要查找的图片。", L"选择偏移", MB_OK | MB_ICONINFORMATION);
            return;
        }
        int sx1 = ToInt(findX1_), sy1 = ToInt(findY1_), sx2 = ToInt(findX2_), sy2 = ToInt(findY2_);
        if (findImageFullScreen_) {
            int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
            GetVirtualScreenRect(vsX, vsY, vsW, vsH);
            sx1 = vsX; sy1 = vsY; sx2 = vsX + vsW; sy2 = vsY + vsH;
        }
        const double threshold = std::clamp(ToDouble(findMatchThreshold_, 65.0), 1.0, 100.0);
        const double scaleMin = std::max(0.1, ToDouble(findScaleMin_, 1.0));
        const double scaleMax = std::max(scaleMin, ToDouble(findScaleMax_, scaleMin));

        if (!matchOverlay_) matchOverlay_ = std::make_unique<MatchOverlay>();
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);

        auto result = matchOverlay_->Show(findImagePath_, sx1, sy1, sx2, sy2, threshold, scaleMin, scaleMax,
                                          MatchOverlayMode::OffsetPick);

        RestoreEditorAfterScreenOverlay();

        if (!result.cancelled && matchOverlay_->matchResult_.found) {
            // Offset = click position - match center
            const int offsetX = result.offsetX - matchOverlay_->matchResult_.x;
            const int offsetY = result.offsetY - matchOverlay_->matchResult_.y;
            SetText(findOffsetX_, std::to_wstring(offsetX));
            SetText(findOffsetY_, std::to_wstring(offsetY));
        }
    }

    void EndFindOffsetSelect() {
        // No-op: offset selection is now handled by MatchOverlay in BeginFindOffsetSelect()
    }

    void LoadFindImageFromFile() {
        wchar_t fileName[MAX_PATH]{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"图片文件\0*.bmp;*.png;*.jpg;*.jpeg\0所有文件\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return;
        const std::wstring copied = EnsureImageInLibrary(fileName);
        findImagePath_ = copied;
        if (IsPathInImageDir(copied)) newImagePaths_.insert(copied);
        if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(actions_.size())) {
            actions_[static_cast<size_t>(selectedIndex_)].imagePath = copied;
        }
        UpdateFindImagePreview();
    }

    void BeginAiRegionSelect() {
        if (!hwnd_) return;
        if (!screenshotOverlay_) screenshotOverlay_ = std::make_unique<ScreenshotOverlay>();
        screenshotOverlay_->SetTitle(L"选取分析区域");
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);
        screenshotOverlay_->Show([this](RECT sel) {
            if (sel.left == 0 && sel.top == 0 && sel.right == 0 && sel.bottom == 0) {
                RestoreEditorAfterScreenOverlay();
                return;
            }
            const int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            aiFullScreen_ = false;
            SetAiRegionCoords(
                sel.left + vsX, sel.top + vsY,
                sel.right + vsX, sel.bottom + vsY);
            RestoreEditorAfterScreenOverlay();
        });
    }

    void BeginAiFindRegionSelect() {
        const bool regionByImage = (popupAction_.sel == 30 && aiRegionByImageCheck_ && Checked(aiRegionByImageCheck_))
            || (popupAction_.sel == 31 && aiWithImageCheck_ && Checked(aiWithImageCheck_)
                && aiRegionByImageCheck2_ && Checked(aiRegionByImageCheck2_));
        if (!regionByImage) return;
        if (!HasFindImageTemplate(aiFindImagePath_)) {
            MessageBoxW(hwnd_, L"请先设置要查找的图片。", L"选取区域", MB_OK | MB_ICONINFORMATION);
            return;
        }
        int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
        GetVirtualScreenRect(vsX, vsY, vsW, vsH);
        const double threshold = std::clamp(ToDouble(aiFindMatchThreshold_, 65.0), 1.0, 100.0);
        const double scaleMin = std::max(0.1, ToDouble(aiFindScaleMin_, 1.0));
        const double scaleMax = std::max(scaleMin, ToDouble(aiFindScaleMax_, scaleMin));

        if (!matchOverlay_) matchOverlay_ = std::make_unique<MatchOverlay>();
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);

        const auto result = matchOverlay_->Show(
            aiFindImagePath_, vsX, vsY, vsX + vsW, vsY + vsH,
            threshold, scaleMin, scaleMax, MatchOverlayMode::RelativeRegionPick);

        RestoreEditorAfterScreenOverlay();

        if (!result.cancelled && result.regionValid && matchOverlay_->matchResult_.found) {
            const int anchorX = matchOverlay_->matchResult_.topLeftX;
            const int anchorY = matchOverlay_->matchResult_.topLeftY;
            SetAiRegionCoords(
                result.regionX1 - anchorX, result.regionY1 - anchorY,
                result.regionX2 - anchorX, result.regionY2 - anchorY);
        }
    }

    void BeginAiFindScreenshot() {
        if (!hwnd_) return;
        if (!screenshotOverlay_) screenshotOverlay_ = std::make_unique<ScreenshotOverlay>();
        screenshotOverlay_->SetTitle(L"屏幕截图");
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);
        screenshotOverlay_->Show([this](RECT sel) {
            const bool cancelled = sel.left == 0 && sel.top == 0 && sel.right == 0 && sel.bottom == 0;
            if (!cancelled) {
                const int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
                const int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
                const int x1 = sel.left + vsX, y1 = sel.top + vsY;
                const int x2 = sel.right + vsX, y2 = sel.bottom + vsY;
                EnsureFindImagesDir();
                const std::wstring path = FindImagesDir() + L"\\template_"
                    + std::to_wstring(GetTickCount()) + L".bmp";
                HBITMAP bmp = CaptureScreenRegion(x1, y1, x2, y2);
                if (bmp && SaveBitmapToFile(bmp, path)) {
                    aiFindImagePath_ = path;
                    newImagePaths_.insert(path);
                    UpdateAiFindImagePreview();
                }
                DeleteBitmapHandle(bmp);
            }
            RestoreEditorAfterScreenOverlay();
        });
    }

    void LoadAiFindImageFromFile() {
        wchar_t fileName[MAX_PATH]{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"图片文件\0*.bmp;*.png;*.jpg;*.jpeg\0所有文件\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return;
        const std::wstring copied = EnsureImageInLibrary(fileName);
        aiFindImagePath_ = copied;
        if (IsPathInImageDir(copied)) newImagePaths_.insert(copied);
        UpdateAiFindImagePreview();
        if (popupAction_.sel == 30 || popupAction_.sel == 31) RebuildParamPanelLayout();
    }

    void ClearAiFindImage() {
        if (!aiFindImagePath_.empty() && IsPathInImageDir(aiFindImagePath_)) {
            auto it = newImagePaths_.find(aiFindImagePath_);
            if (it != newImagePaths_.end()) {
                const auto allRefs = CollectAllReferencedImages();
                if (allRefs.find(aiFindImagePath_) == allRefs.end()) {
                    DeleteFileW(aiFindImagePath_.c_str());
                }
                newImagePaths_.erase(it);
            }
        }
        aiFindImagePath_.clear();
        UpdateAiFindImagePreview();
        if (popupAction_.sel == 30 || popupAction_.sel == 31) RebuildParamPanelLayout();
    }

    void BrowseProgramExecutable(HWND targetEdit) {
        if (!targetEdit) return;
        wchar_t fileName[MAX_PATH]{};
        const std::wstring current = Trim(GetText(targetEdit));
        if (!current.empty()) wcsncpy_s(fileName, current.c_str(), _TRUNCATE);
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"可执行文件 (*.exe;*.msc;*.bat;*.cmd)\0*.exe;*.msc;*.bat;*.cmd\0所有文件 (*.*)\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return;
        SetText(targetEdit, fileName);
    }

    void BrowseOpenFile(HWND targetEdit) {
        if (!targetEdit) return;
        wchar_t fileName[MAX_PATH]{};
        const std::wstring current = Trim(GetText(targetEdit));
        if (!current.empty()) wcsncpy_s(fileName, current.c_str(), _TRUNCATE);
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"所有文件 (*.*)\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return;
        SetText(targetEdit, fileName);
    }

    void ClearFindImage() {
        // 如果是编辑期间新截图的图片且未保存到脚本，删除残留文件
        if (!findImagePath_.empty() && IsPathInImageDir(findImagePath_)) {
            auto it = newImagePaths_.find(findImagePath_);
            if (it != newImagePaths_.end()) {
                // 检查是否被任何已有脚本引用
                const auto allRefs = CollectAllReferencedImages();
                if (allRefs.find(findImagePath_) == allRefs.end()) {
                    DeleteFileW(findImagePath_.c_str());
                }
                newImagePaths_.erase(it);
            }
        }
        findImagePath_.clear();
        if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(actions_.size())) {
            actions_[static_cast<size_t>(selectedIndex_)].imagePath.clear();
        }
        UpdateFindImagePreview();
        RefreshGrayButtonsInParamViewport();
    }

    void TestFindImage() {
        if (findImagePath_.empty()) {
            MessageBoxW(hwnd_, L"请先设置要查找的图片。", L"测试找图", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (findTestRunning_.exchange(true)) return;
        if (findTestBtn_) EnableWindow(findTestBtn_, FALSE);

        if (!matchOverlay_) matchOverlay_ = std::make_unique<MatchOverlay>();
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);

        int sx1 = ToInt(findX1_), sy1 = ToInt(findY1_), sx2 = ToInt(findX2_), sy2 = ToInt(findY2_);
        if (findImageFullScreen_) {
            int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
            GetVirtualScreenRect(vsX, vsY, vsW, vsH);
            sx1 = vsX; sy1 = vsY; sx2 = vsX + vsW; sy2 = vsY + vsH;
        }
        const double threshold = std::clamp(ToDouble(findMatchThreshold_, 65.0), 1.0, 100.0);
        const double scaleMin = std::max(0.1, ToDouble(findScaleMin_, 1.0));
        const double scaleMax = std::max(scaleMin, ToDouble(findScaleMax_, scaleMin));

        matchOverlay_->Show(findImagePath_, sx1, sy1, sx2, sy2, threshold, scaleMin, scaleMax,
                            MatchOverlayMode::Test);

        RestoreEditorAfterScreenOverlay();

        findTestRunning_ = false;
        if (findTestBtn_) EnableWindow(findTestBtn_, TRUE);
    }

    void OnFindTestDone(int /*found*/, int /*lParam*/) {
        // Kept for backward compatibility; current TestFindImage uses MatchOverlay inline.
        findTestRunning_ = false;
        if (findTestBtn_) EnableWindow(findTestBtn_, TRUE);
    }

    // ── WM_COMMAND dispatch ────────────────────────────────────────
    void OnCommand(int id, int code, HWND ctrl) {
        struct GrayButtonClickReset {
            MainWindow* self = nullptr;
            HWND btn = nullptr;
            int notifyCode = 0;
            ~GrayButtonClickReset() {
                if (self && btn && notifyCode == BN_CLICKED && self->IsGrayButton(btn)) {
                    self->FinishGrayButtonClick(btn);
                }
            }
        } grayClickReset{this, ctrl, code};
        if ((code == CBN_DROPDOWN || code == CBN_CLOSEUP) && ctrl) {
            if (code == CBN_DROPDOWN) StyleComboDropdownList(ctrl);
            InvalidateRect(ctrl, nullptr, FALSE);
        }
        if (id == kClose) { if (page_ == Page::Editor) ShowHome(); else SendMessageW(hwnd_, WM_CLOSE, 0, 0); return; }
        if (id == kActionCombo && code == CBN_SELCHANGE) { if (!loadingForm_) { RefreshParamPanel(); InvalidateRect(hwnd_, nullptr, FALSE); } return; }
        if (id == kCancel) { ShowHome(); return; }
        if (id == kSave) { SaveIfEditor(); ShowHome(); return; }
        if (id == kClear) {
            actions_.clear();
            collapsedContainers_.clear();
            selectedIndex_ = -1;
            if (batchEditMode_) batchSelected_.clear();
            UpdateBatchToolbar();
            UpdateEditMode();
            OnActionsChanged();
            return;
        }
        if (id == kLoad) { EnterBatchEditMode(); return; }
        if (id == kBatchExit) { ExitBatchEditMode(); return; }
        if (id == kBatchSelectAll) { BatchSelectAll(); return; }
        if (id == kBatchDeselect) { BatchDeselectAll(); return; }
        if (id == kBatchDelete) { BatchDeleteSelected(); return; }
        if (id == kBatchCopy) { BatchCopySelected(); return; }
        if (id == kKeyCapture) { CaptureActionKey(); return; }
        if (id == kKeyPressCapture) { CaptureKeyPress(); return; }
        if (id == kQuickInputInsert) { InsertQuickInputVariable(); return; }
        if (id == kOcrSearchVarInsert) { InsertOcrSearchVariable(); return; }
        if (id == kOcrSearchText && code == EN_CHANGE && !loadingForm_ && popupAction_.sel == 18) {
            RefreshOcrSubPanel();
            return;
        }
        if (id == kIfAddCondition) { AppendIfCondition(); return; }
        if (id == kRunProgramBrowse || id == kCloseProgramBrowse) { BrowseProgramExecutable(id == kRunProgramBrowse ? runProgramPath_ : closeProgramPath_); return; }
        if (id == kOpenFileBrowse) { BrowseOpenFile(openFilePath_); return; }
        if (id == kFindFullScreen) { ApplyFindImageFullScreen(); return; }
        if (id == kFindSelectRegion) { BeginFindRegionSelect(false); return; }
        if (id == kFindScreenshot) { BeginFindRegionSelect(true); return; }
        if (id == kFindLocalImage) { LoadFindImageFromFile(); return; }
        if (id == kFindImagePreview) { LoadFindImageFromFile(); return; }
        if (id == kFindClearImage) { ClearFindImage(); return; }
        if (id == kFindTest) { TestFindImage(); return; }
        if (id == kFindSelectOffset) { BeginFindOffsetSelect(); return; }
        if (id == kOcrFullScreen) { ApplyOcrFullScreen(); return; }
        if (id == kOcrSelectRegion) { BeginOcrRegionSelect(); return; }
        if (id == kOcrSelectOffset) { BeginOcrOffsetSelect(); return; }
        if (id == kOcrTest) { TestOcr(); return; }
        if (id == kOcrInstallDep) { ShowOcrInstallDialog(); return; }
        if (id == kOcrRegionByImage && code == BN_CLICKED) { RebuildParamPanelLayout(); return; }
        if (id == kOcrFindSelectRegion) { BeginOcrFindRegionSelect(); return; }
        if (id == kOcrFindScreenshot) { BeginOcrFindScreenshot(); return; }
        if (id == kOcrFindLocalImage) { LoadOcrFindImageFromFile(); return; }
        if (id == kOcrFindImagePreview) { LoadOcrFindImageFromFile(); return; }
        if (id == kOcrFindClearImage) { ClearOcrFindImage(); return; }
        if (id == kAiInsertVar) { InsertAiPromptVariable(); return; }
        if (id == kAiSelectRegion) { BeginAiRegionSelect(); return; }
        if (id == kAiFullScreen) { ApplyAiFullScreen(); return; }
        if (id == kAiRegionByImage && code == BN_CLICKED) { RebuildParamPanelLayout(); return; }
        if (id == kAiRegionByImage2 && code == BN_CLICKED) { RebuildParamPanelLayout(); return; }
        if (id == kAiWithImage && code == BN_CLICKED) { RebuildParamPanelLayout(); return; }
        if (id == kAiFindSelectRegion) { BeginAiFindRegionSelect(); return; }
        if (id == kAiTargetScreenshot) { BeginAiFindScreenshot(); return; }
        if (id == kAiTargetLocal) { LoadAiFindImageFromFile(); return; }
        if (id == kAiTargetPreview) { LoadAiFindImageFromFile(); return; }
        if (id == kAiTargetClear) { ClearAiFindImage(); return; }
        if ((id == kMoveFromVar || id == kLoopFromVar) && code == BN_CLICKED) {
            UpdateMoveVarControls();
            UpdateLoopVarControls();
            return;
        }
        if (id == kListRemarkEdit && code == EN_KILLFOCUS) { CommitInlineRemark(); return; }
        if (id == kModify) { ModifySelected(); return; }
        if (id == kAdd) { ShowAddMenu(); return; }
        if (id >= kCopyLast && id <= kCopyAfterSelected) { CopyActionByMenu(id); return; }
        if (id >= kAddLast && id <= kAddAsChild) { AddActionByMenu(id); return; }
        if (id >= kHotCustom && id <= kHotSpace) { SetCommonHotkeyFromMenu(id); return; }
    }

    void InsertAction(size_t pos, const ScriptAction& src, int indentOverride = -1) {
        ScriptAction action = src;
        action.originalNo = NextNo();
        if (indentOverride >= 0) action.indent = indentOverride;
        pos = std::min(pos, actions_.size());
        actions_.insert(actions_.begin() + static_cast<std::ptrdiff_t>(pos), action);
        selectedIndex_ = static_cast<int>(pos);
        {
            std::set<int> updated;
            for (int idx : collapsedContainers_) {
                updated.insert(idx >= static_cast<int>(pos) ? idx + 1 : idx);
            }
            collapsedContainers_ = std::move(updated);
        }
        if (IsExpandableContainer(action.type)) collapsedContainers_.erase(static_cast<int>(pos));
        RenumberActions();
        EnsureSelectedVisible();
        UpdateEditMode();
        OnActionsChanged();
    }

    void ShowAddMenu() { ShowAddMenuAt(0, 0); }

    void ShowAddMenuAt(int x, int y) {
        if (selectedIndex_ < 0) { TryInsertActionFromForm(actions_.size()); return; }
        const ScriptAction preview = ActionFromForm();
        if (preview.type == ActionType::DefineBlock) { TryInsertActionFromForm(0, 0); return; }
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kAddLast, L"添加到最后");
        AppendMenuW(menu, MF_STRING, kAddFirst, L"插入到最前");
        AppendMenuW(menu, MF_STRING, kAddBeforeSelected, L"插入到选择项前");
        AppendMenuW(menu, MF_STRING, kAddAfterSelected, L"插入到选择项后");
        if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(actions_.size()) && IsSubtreeContainer(actions_[static_cast<size_t>(selectedIndex_)].type)) {
            AppendMenuW(menu, MF_STRING, kAddAsChild, L"添加为子节点");
        }
        POINT pt{x, y};
        if (x == 0 && y == 0) {
            GetCursorPos(&pt);
        } else {
            ClientToScreen(hwnd_, &pt);
        }
        TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
    }

    void AddActionByMenu(int id) {
        size_t pos = actions_.size();
        int indent = -1;
        if (id == kAddFirst) pos = 0;
        if (id == kAddBeforeSelected && selectedIndex_ >= 0) {
            pos = static_cast<size_t>(selectedIndex_);
            indent = actions_[static_cast<size_t>(selectedIndex_)].indent;
        }
        if (id == kAddAfterSelected && selectedIndex_ >= 0) {
            pos = static_cast<size_t>(SubtreeEnd(selectedIndex_));
            indent = actions_[static_cast<size_t>(selectedIndex_)].indent;
        }
        if (id == kAddAsChild && selectedIndex_ >= 0 && IsSubtreeContainer(actions_[static_cast<size_t>(selectedIndex_)].type)) {
            pos = static_cast<size_t>(ContainerBodyEndIndex(selectedIndex_));
            indent = actions_[static_cast<size_t>(selectedIndex_)].indent + 1;
        }
        TryInsertActionFromForm(pos, indent);
    }

    void CopyActionByMenu(int id) {
        if (copySource_ < 0 || copySource_ >= static_cast<int>(actions_.size())) return;
        ScriptAction action = actions_[static_cast<size_t>(copySource_)];
        size_t pos = actions_.size();
        if (id == kCopyFirst) pos = 0;
        if (id == kCopyBeforeSelected && selectedIndex_ >= 0) pos = static_cast<size_t>(selectedIndex_);
        if (id == kCopyAfterSelected && selectedIndex_ >= 0) pos = static_cast<size_t>(selectedIndex_ + 1);
        InsertAction(pos, action);
    }

    void ApplyCapturedKeyToForm(Hotkey& out, UINT& formVk, std::wstring& formText, HWND keyEdit,
                                HWND lWin, HWND rWin, HWND lCtrl, HWND rCtrl,
                                HWND lAlt, HWND rAlt, HWND lShift, HWND rShift) {
        LockWindowUpdate(hwnd_);
        formVk = out.vk;
        formText = VkName(out.vk);
        SetText(keyEdit, formText);
        SetChecked(lCtrl, (out.modifiers & MOD_CONTROL) != 0);
        SetChecked(lAlt, (out.modifiers & MOD_ALT) != 0);
        SetChecked(lShift, (out.modifiers & MOD_SHIFT) != 0);
        SetChecked(lWin, (out.modifiers & MOD_WIN) != 0);
        SetChecked(rCtrl, false);
        SetChecked(rAlt, false);
        SetChecked(rShift, false);
        SetChecked(rWin, false);
        LockWindowUpdate(nullptr);
    }

    void CaptureKeyPress() {
        HotkeyCapture cap;
        Hotkey oldValue{};
        oldValue.vk = formKeyPressVk_;
        oldValue.text = formKeyPressText_.empty() ? VkName(formKeyPressVk_) : formKeyPressText_;
        oldValue.enabled = oldValue.vk != 0;
        Hotkey out;
        if (!cap.Show(hwnd_, oldValue, false, out) || !out.enabled || out.vk == 0) return;
        ApplyCapturedKeyToForm(out, formKeyPressVk_, formKeyPressText_, keyPressEdit_,
            keyPressLWin_, keyPressRWin_, keyPressLCtrl_, keyPressRCtrl_,
            keyPressLAlt_, keyPressRAlt_, keyPressLShift_, keyPressRShift_);
    }

    void CaptureActionKey() {
        HotkeyCapture cap;
        Hotkey oldValue{};
        oldValue.vk = formKeyVk_;
        oldValue.text = formKeyText_.empty() ? VkName(formKeyVk_) : formKeyText_;
        oldValue.enabled = oldValue.vk != 0;
        Hotkey out;
        if (!cap.Show(hwnd_, oldValue, false, out) || !out.enabled || out.vk == 0) return;
        ApplyCapturedKeyToForm(out, formKeyVk_, formKeyText_, keyEdit_,
            keyLWin_, keyRWin_, keyLCtrl_, keyRCtrl_,
            keyLAlt_, keyRAlt_, keyLShift_, keyRShift_);
    }

    void ModifySelected() {
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(actions_.size())) return;
        ScriptAction action = ActionFromForm();
        if (action.type == ActionType::DefineBlock && !ValidateDefineBlockName(action.blockName, selectedIndex_)) return;
        action.originalNo = actions_[static_cast<size_t>(selectedIndex_)].originalNo;
        action.indent = actions_[static_cast<size_t>(selectedIndex_)].indent;
        actions_[static_cast<size_t>(selectedIndex_)] = action;
        if (action.type == ActionType::DefineBlock || action.type == ActionType::RunBlock) RefreshRunBlockCombo();
        OnActionsChanged();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    int NextNo() const { int maxNo = 0; for (const auto& a : actions_) maxNo = std::max(maxNo, a.originalNo); return maxNo + 1; }

    // ── Script file I/O ────────────────────────────────────────────
    void LoadScripts() {
        scripts_.clear(); EnsureScriptsDir();
        WIN32_FIND_DATAW fd{};
        const auto pattern = ScriptsDir() + L"\\*.json";
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    ScriptMeta meta{};
                    meta.path = ScriptsDir() + L"\\" + fd.cFileName;
                    const auto content = ReadAll(meta.path);
                    meta.name = ExtractString(content, L"scriptName");
                    if (meta.name.empty()) meta.name = fd.cFileName;
                    meta.recordTime = ExtractString(content, L"recordTime");
                    if (meta.recordTime.empty()) meta.recordTime = L"未知";
                    meta.actionCount = CountActionsInJson(content);
                    meta.hotkey.text = ExtractString(content, L"hotkeyText");
                    meta.hotkey.vk = static_cast<UINT>(ExtractNumber(content, L"hotkeyVk", 0));
                    meta.hotkey.modifiers = static_cast<UINT>(ExtractNumber(content, L"hotkeyModifiers", 0));
                    meta.hotkey.enabled = meta.hotkey.vk != 0;
                    scripts_.push_back(meta);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        ClampHomeScroll();
    }

    void LoadRecordings() {
        recordings_.clear();
        WIN32_FIND_DATAW fd{};
        const auto pattern = RecordingsDir() + L"\\*.json";
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    ScriptMeta meta{};
                    meta.path = RecordingsDir() + L"\\" + fd.cFileName;
                    const auto content = ReadAll(meta.path);
                    meta.name = ExtractString(content, L"scriptName");
                    if (meta.name.empty()) meta.name = fd.cFileName;
                    meta.recordTime = ExtractString(content, L"recordTime");
                    if (meta.recordTime.empty()) meta.recordTime = L"未知";
                    meta.actionCount = CountActionsInJson(content);
                    meta.durationSeconds = ExtractNumber(content, L"durationSeconds", 0);
                    meta.hotkey.text = ExtractString(content, L"hotkeyText");
                    meta.hotkey.vk = static_cast<UINT>(ExtractNumber(content, L"hotkeyVk", 0));
                    meta.hotkey.modifiers = static_cast<UINT>(ExtractNumber(content, L"hotkeyModifiers", 0));
                    meta.hotkey.enabled = meta.hotkey.vk != 0;
                    recordings_.push_back(meta);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    void SaveIfEditor() {
        if (page_ != Page::Editor) return;
        SyncFormIntoActionsBeforeRun();
        EnsureScriptsDir();
        if (currentPath_.empty()) {
            std::wstring scriptName = GetText(name_);
            if (scriptName.empty()) scriptName = L"未命名脚本";
            currentPath_ = ScriptsDir() + L"\\" + scriptName + L".json";
        }
        SaveScriptFile(currentPath_);
    }

    void SaveScriptFile(const std::wstring& path) {
        std::wstringstream file;
        std::ofstream out(path, std::ios::binary);
        if (!out) return;
        out.write("\xEF\xBB\xBF", 3);
        file << L"{\n";
        file << L"  \"scriptName\": \"" << EscapeJson(GetText(name_)) << L"\",\n";
        file << L"  \"recordTime\": \"" << EscapeJson(currentRecordTime_.empty() ? NowText() : currentRecordTime_) << L"\",\n";
        if (saveDurationSeconds_ > 0) file << L"  \"durationSeconds\": " << saveDurationSeconds_ << L",\n";
        Hotkey hk = saveHotkeyOverride_.has_value() ? *saveHotkeyOverride_
            : (currentScriptIndex_ >= 0 && currentScriptIndex_ < static_cast<int>(scripts_.size()) ? scripts_[static_cast<size_t>(currentScriptIndex_)].hotkey : Hotkey{0, 0, L"", false});
        file << L"  \"hotkeyText\": \"" << EscapeJson(hk.text) << L"\",\n";
        file << L"  \"hotkeyVk\": " << hk.vk << L",\n";
        file << L"  \"hotkeyModifiers\": " << hk.modifiers << L",\n";
        file << L"  \"actions\": [\n";
        for (size_t i = 0; i < actions_.size(); ++i) {
            WriteActionJson(file, actions_[i], i + 1 == actions_.size());
        }
        file << L"  ]\n}\n";
        const auto bytes = ToUtf8(file.str());
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        const bool writeOk = out.good();
        out.close();
        if (!writeOk) {
            MessageBoxW(hwnd_, L"保存失败：无法写入文件，请检查磁盘空间和权限。", L"保存", MB_OK | MB_ICONERROR);
        } else {
            newImagePaths_.clear();
            CleanOrphanImages();
        }
    }

    ScriptAction ParseScriptActionBlock(const std::wstring& block, size_t fallbackNo) const {
        return ::ParseScriptActionBlock(block, fallbackNo);
    }

    std::vector<ScriptAction> ParseActionsFromContent(const std::wstring& content) const {
        std::vector<ScriptAction> parsed;
        const auto blocks = ExtractJsonActionBlocks(content);
        for (size_t i = 0; i < blocks.size(); ++i) {
            const auto type = ExtractString(blocks[i], L"type");
            if (!type.empty()) parsed.push_back(ParseScriptActionBlock(blocks[i], i));
        }
        return parsed;
    }

    std::vector<ScriptAction> ParseActionsFromFile(const std::wstring& path) const {
        if (path.empty()) return {};
        return ParseActionsFromContent(ReadAll(path));
    }

    void LoadScriptFile(const std::wstring& path) {
        const auto content = ReadAll(path);
        SetText(name_, ExtractString(content, L"scriptName"));
        currentRecordTime_ = ExtractString(content, L"recordTime");
        actions_ = ParseActionsFromContent(content);
        collapsedContainers_.clear();
        RefreshRunBlockCombo();
        OnActionsChanged();
    }

    std::wstring ChooseScriptPath(bool save, const std::wstring& defaultName = L"鼠标宏.json") {
        wchar_t fileName[MAX_PATH]{};
        wcsncpy_s(fileName, defaultName.c_str(), _TRUNCATE);
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"JSON 脚本 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        ofn.lpstrDefExt = L"json";
        const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
        return ok ? std::wstring(fileName) : L"";
    }

    std::wstring EnsureJsonExtension(std::wstring path) const {
        const auto slash = path.find_last_of(L"\\/");
        const auto dot = path.find_last_of(L'.');
        if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) {
            std::wstring ext = path.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
            if (ext == L".json") return path;
            path.erase(dot);
        }
        return path + L".json";
    }

    std::wstring SafeScriptFileName(std::wstring name) const {
        if (Trim(name).empty()) name = TimestampName();
        for (wchar_t& ch : name) {
            if (wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
        }
        return name;
    }

    void ImportScript() {
        const bool toRecordings = ImportTargetsRecordings();
        wchar_t fileName[MAX_PATH]{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"脚本文件 (*.zip;*.json)\0*.zip;*.json\0ZIP 脚本包 (*.zip)\0*.zip\0JSON 脚本 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return;

        const std::wstring path(fileName);
        std::wstring lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });

        if (lower.size() >= 4 && lower.substr(lower.size() - 4) == L".zip") {
            ImportScriptFromZipFile(path, toRecordings);
        } else {
            ImportScriptFromJsonFile(path, toRecordings);
        }
    }

    bool ImportTargetsRecordings() const {
        return activeHomeTab_ == quickscript::MainTab::Recorder;
    }

    std::wstring ImportTargetDir(bool toRecordings) const {
        return toRecordings ? RecordingsDir() : ScriptsDir();
    }

    bool WriteImportedJson(const std::wstring& target, std::wstring content) {
        content = UpdateJsonStringField(content, L"recordTime", NowText());
        if (content.find(L"\"recordTime\"") == std::wstring::npos) {
            const auto brace = content.find(L'{');
            if (brace != std::wstring::npos) {
                const std::wstring insert = L"\n  \"recordTime\": \"" + EscapeJson(NowText()) + L"\",";
                content.insert(brace + 1, insert);
            }
        }
        std::ofstream out(target, std::ios::binary);
        if (!out) return false;
        out.write("\xEF\xBB\xBF", 3);
        const auto bytes = ToUtf8(content);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return out.good();
    }

    void FinishImport(bool toRecordings) {
        if (toRecordings) {
            LoadRecordings();
            ClampRecordingScroll();
        } else {
            LoadScripts();
            ClampHomeScroll();
            RegisterAllHotkeys();
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void ImportScriptFromJsonFile(const std::wstring& path, bool toRecordings) {
        const auto content = ReadAll(path);
        std::wstring name = ExtractString(content, L"scriptName");
        if (name.empty()) {
            MessageBoxW(hwnd_, L"导入失败：文件格式不正确，未找到脚本名称。", L"导入", MB_OK | MB_ICONWARNING);
            return;
        }
        EnsureScriptsDir();
        const auto baseDir = ImportTargetDir(toRecordings);
        std::wstring target = baseDir + L"\\" + SafeScriptFileName(name) + L".json";
        int suffix = 1;
        while (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            target = baseDir + L"\\" + SafeScriptFileName(name) + L"-" + std::to_wstring(suffix++) + L".json";
        }
        if (!WriteImportedJson(target, content)) {
            MessageBoxW(hwnd_, L"导入失败：无法写入目标目录。", L"导入", MB_OK | MB_ICONERROR);
            return;
        }
        FinishImport(toRecordings);
    }

    void ImportScriptFromZipFile(const std::wstring& zipPath, bool toRecordings) {
        // 先读取 ZIP 中的 JSON 内容
        std::string jsonUtf8 = ReadTextFromZip(zipPath, "script.json");
        if (jsonUtf8.empty()) {
            MessageBoxW(hwnd_, L"导入失败：ZIP 文件中未找到 script.json。", L"导入", MB_OK | MB_ICONWARNING);
            return;
        }
        const std::wstring content = FromUtf8(jsonUtf8);
        std::wstring name = ExtractString(content, L"scriptName");
        if (name.empty()) {
            MessageBoxW(hwnd_, L"导入失败：文件格式不正确，未找到脚本名称。", L"导入", MB_OK | MB_ICONWARNING);
            return;
        }

        // 解压 ZIP 到临时目录
        wchar_t tempDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDir);
        const std::wstring extractDir = std::wstring(tempDir) + L"qs_import_" + std::to_wstring(GetTickCount());
        int extracted = ExtractZipFile(zipPath, extractDir);
        if (extracted < 0) {
            MessageBoxW(hwnd_, L"导入失败：无法解压 ZIP 文件。", L"导入", MB_OK | MB_ICONWARNING);
            return;
        }

        // 复制图片到 images 目录并重映射
        EnsureFindImagesDir();
        std::wstring modifiedContent = content;
        const auto imgDir = FindImagesDir();

        // 收集 ZIP 中解压出的图片文件名（排除 script.json）
        WIN32_FIND_DATAW fd{};
        const std::wstring pattern = extractDir + L"\\*";
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring extractedFile = extractDir + L"\\" + fd.cFileName;
                std::wstring fileName(fd.cFileName);
                // 跳过 script.json
                if (_wcsicmp(fileName.c_str(), L"script.json") == 0) continue;
                // 检查是否为图片（.bmp, .png, .jpg）
                auto dotPos = fileName.find_last_of(L'.');
                if (dotPos == std::wstring::npos) continue;
                std::wstring ext = fileName.substr(dotPos);
                std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
                if (ext != L".bmp" && ext != L".png" && ext != L".jpg" && ext != L".jpeg") continue;

                // 复制到 images 目录
                std::wstring destPath = imgDir + L"\\" + fileName;
                // 如果目标已存在，生成新文件名
                if (GetFileAttributesW(destPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    const auto nameNoExt = fileName.substr(0, dotPos);
                    destPath = imgDir + L"\\" + nameNoExt + L"_" + std::to_wstring(GetTickCount()) + ext;
                }
                if (!CopyFileW(extractedFile.c_str(), destPath.c_str(), FALSE)) continue;

                const std::wstring relPath = ImagePathForJson(destPath);
                const auto blocks = ExtractJsonActionBlocks(modifiedContent);
                for (const auto& block : blocks) {
                    const auto type = ExtractString(block, L"type");
                    const bool usesImage = type == L"findImage"
                        || (type == L"textRecognition"
                            && ExtractNumber(block, L"ocrRegionByImage", 0) != 0);
                    if (!usesImage) continue;
                    const auto oldImgPath = ExtractString(block, L"imagePath");
                    if (oldImgPath.empty()) continue;
                    const auto oldSlash = oldImgPath.find_last_of(L"\\/");
                    const std::wstring oldFileName = (oldSlash == std::wstring::npos)
                        ? oldImgPath : oldImgPath.substr(oldSlash + 1);
                    const auto destSlash = destPath.find_last_of(L"\\/");
                    const std::wstring destFileName = (destSlash == std::wstring::npos)
                        ? destPath : destPath.substr(destSlash + 1);
                    if (_wcsicmp(oldFileName.c_str(), fileName.c_str()) == 0 ||
                        _wcsicmp(oldFileName.c_str(), destFileName.c_str()) == 0) {
                        const auto key = L"\"imagePath\": \"" + EscapeJson(oldImgPath) + L"\"";
                        const auto pos = modifiedContent.find(key);
                        if (pos != std::wstring::npos) {
                            modifiedContent.replace(pos + 14, EscapeJson(oldImgPath).size(), EscapeJson(relPath));
                        }
                    }
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }

        // 清理临时目录
        RemoveDirectoryW(extractDir.c_str());
        // 也删除临时文件
        WIN32_FIND_DATAW fd2{};
        const std::wstring cleanPattern = extractDir + L"\\*";
        HANDLE hFind2 = FindFirstFileW(cleanPattern.c_str(), &fd2);
        if (hFind2 != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    DeleteFileW((extractDir + L"\\" + fd2.cFileName).c_str());
                }
            } while (FindNextFileW(hFind2, &fd2));
            FindClose(hFind2);
        }
        RemoveDirectoryW(extractDir.c_str());

        // 保存修改后的 JSON 到目标目录
        EnsureScriptsDir();
        const auto baseDir = ImportTargetDir(toRecordings);
        std::wstring target = baseDir + L"\\" + SafeScriptFileName(name) + L".json";
        int suffix = 1;
        while (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            target = baseDir + L"\\" + SafeScriptFileName(name) + L"-" + std::to_wstring(suffix++) + L".json";
        }
        if (!WriteImportedJson(target, modifiedContent)) {
            MessageBoxW(hwnd_, L"导入失败：无法写入目标目录。", L"导入", MB_OK | MB_ICONERROR);
            return;
        }
        FinishImport(toRecordings);
    }

    void ExportSelectedScript() {
        if (selectedScript_ < 0 || selectedScript_ >= static_cast<int>(scripts_.size())) {
            MessageBoxW(hwnd_, L"请选择要导出的宏", L"导出", MB_OK | MB_ICONINFORMATION);
            return;
        }
        const auto& script = scripts_[static_cast<size_t>(selectedScript_)];
        const auto content = ReadAll(script.path);
        const auto imgPaths = CollectImagePathsFromJson(content);

        // 根据是否包含图片选择导出格式
        if (!imgPaths.empty()) {
            ExportScriptAsZip(script);
        } else {
            ExportScriptAsJson(script);
        }
    }

    void ExportScriptAsJson(const ScriptMeta& script) {
        const auto chosenPath = ChooseScriptPath(true, SafeScriptFileName(script.name) + L".json");
        if (chosenPath.empty()) return;
        const auto path = EnsureJsonExtension(chosenPath);
        if (!CopyFileW(script.path.c_str(), path.c_str(), FALSE)) {
            MessageBoxW(hwnd_, L"导出失败：无法写入目标文件。", L"导出", MB_OK | MB_ICONERROR);
        } else {
            MessageBoxW(hwnd_, L"导出成功！", L"导出", MB_OK | MB_ICONINFORMATION);
        }
    }

    void ExportScriptAsZip(const ScriptMeta& script) {
        wchar_t fileName[MAX_PATH]{};
        const auto defaultName = SafeScriptFileName(script.name) + L".zip";
        wcsncpy_s(fileName, defaultName.c_str(), _TRUNCATE);
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"ZIP 脚本包 (*.zip)\0*.zip\0所有文件 (*.*)\0*.*\0";
        ofn.lpstrFile = fileName;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&ofn)) return;

        // 确保扩展名为 .zip
        std::wstring zipPath(fileName);
        if (zipPath.size() < 4 || _wcsicmp(zipPath.substr(zipPath.size() - 4).c_str(), L".zip") != 0) {
            zipPath += L".zip";
        }

        // 收集文件列表：JSON + 所有引用的图片
        const auto content = ReadAll(script.path);
        const auto imgPaths = CollectImagePathsFromJson(content);
        std::vector<std::pair<std::wstring, std::wstring>> files;
        // JSON 文件在 ZIP 中固定命名为 script.json
        files.push_back({L"script.json", script.path});
        for (const auto& imgPath : imgPaths) {
            // 图片在 ZIP 中用原始文件名
            const auto slashPos = imgPath.find_last_of(L"\\/");
            std::wstring imgName = (slashPos == std::wstring::npos) ? imgPath : imgPath.substr(slashPos + 1);
            files.push_back({imgName, imgPath});
        }
        const auto zipResult = CreateZipFile(zipPath, files, script.path);
        if (zipResult.success) {
            if (zipResult.skippedFiles.empty()) {
                MessageBoxW(hwnd_, L"导出成功！图片已一同打包。", L"导出", MB_OK | MB_ICONINFORMATION);
            } else {
                const std::wstring msg = L"导出成功，但有 " + std::to_wstring(zipResult.skippedFiles.size())
                    + L" 张图片未找到已跳过。\n\n对方导入后需要重新截图或选择本地图片。";
                MessageBoxW(hwnd_, msg.c_str(), L"导出", MB_OK | MB_ICONWARNING);
            }
        } else {
            MessageBoxW(hwnd_, L"导出失败：无法创建 ZIP 文件，请检查保存路径是否有写入权限。", L"导出", MB_OK | MB_ICONERROR);
        }
    }

    RECT ClampToWorkArea(RECT rc) const {
        HMONITOR monitor = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) return rc;
        const int width = rc.right - rc.left;
        const int height = rc.bottom - rc.top;
        if (rc.left < info.rcWork.left) { rc.left = info.rcWork.left; rc.right = rc.left + width; }
        if (rc.top < info.rcWork.top) { rc.top = info.rcWork.top; rc.bottom = rc.top + height; }
        if (rc.right > info.rcWork.right) { rc.right = info.rcWork.right; rc.left = rc.right - width; }
        if (rc.bottom > info.rcWork.bottom) { rc.bottom = info.rcWork.bottom; rc.top = rc.bottom - height; }
        return rc;
    }

    RECT EditorRectFromHome(RECT homeRc) const {
        const int homeCenterX = homeRc.left + (homeRc.right - homeRc.left) / 2;
        const int homeCenterY = homeRc.top + (homeRc.bottom - homeRc.top) / 2;
        RECT editorRc{
            homeCenterX - kEditorWidth / 2,
            homeCenterY - kEditorHeight / 2,
            homeCenterX + kEditorWidth / 2,
            homeCenterY + kEditorHeight / 2
        };
        return ClampToWorkArea(editorRc);
    }

    // ── Layout helpers / RECT getters ──────────────────────────────
    RECT HomeListRect() const { return RECT{kHomeCardX, kHomeListY, kHomeCardX + kHomeCardW, kHomeListBottom}; }
    int HomeListHeight() const { RECT r = HomeListRect(); return r.bottom - r.top; }
    int HomeContentHeight() const { return static_cast<int>(scripts_.size()) * kHomeCardStep - kHomeCardGap; }
    int MaxHomeScroll() const { return std::max(0, HomeContentHeight() - HomeListHeight()); }
    void ClampHomeScroll() { homeScrollOffset_ = std::clamp(homeScrollOffset_, 0, MaxHomeScroll()); }
    RECT HomeCardRect(int i) const { const int y = kHomeListY + i * kHomeCardStep - homeScrollOffset_; return RECT{kHomeCardX, y, kHomeCardX + kHomeCardW, y + kHomeCardH}; }
    int RecordingContentHeight() const { return static_cast<int>(recordings_.size()) * kHomeCardStep - kHomeCardGap; }
    int MaxRecordingScroll() const { return std::max(0, RecordingContentHeight() - HomeListHeight()); }
    void ClampRecordingScroll() { homeScrollOffset_ = std::clamp(homeScrollOffset_, 0, MaxRecordingScroll()); }
    RECT RecordingCardRect(int i) const { return HomeCardRect(i); }
    std::wstring RecordingHotkeyText(const ScriptMeta& rec) const { return rec.hotkey.enabled && !rec.hotkey.text.empty() ? rec.hotkey.text : L"设置热键"; }
    RECT RecordingHotkeyRect(int i) const {
        if (i < 0 || i >= static_cast<int>(recordings_.size())) return RECT{};
        RECT r = RecordingCardRect(i);
        const int left = r.left + 14;
        const int rightLimit = r.right - 100;
        const int hotW = TextWidth(RecordingHotkeyText(recordings_[static_cast<size_t>(i)]), hotFont_);
        const int nameW = TextWidth(recordings_[static_cast<size_t>(i)].name, homeFont_);
        const int hotLeft = std::min(left + nameW + 10, rightLimit - hotW);
        return RECT{hotLeft, r.top + 17, std::min(hotLeft + hotW, rightLimit), r.top + 48};
    }
    RECT RecorderBannerKeyRect() const {
        RECT cr = CreateRect();
        const int hotW = std::max(56, TextWidth(globalHotkey_.text.empty() ? L"F8" : globalHotkey_.text, bigFont_) + 20);
        return RECT{cr.left + 204, cr.top + 21, cr.left + 204 + hotW, cr.bottom - 21};
    }
    RECT RecorderWindowModeRect() const { RECT cr = CreateRect(); return RECT{cr.right - 150, cr.top, cr.right, cr.top + 35}; }
    RECT HomeScrollTrackRect() const { RECT list = HomeListRect(); return RECT{list.right + 5, list.top, list.right + 5 + kHomeScrollW, list.bottom}; }
    RECT HomeScrollThumbRect() const {
        RECT track = HomeScrollTrackRect();
        const int maxScroll = MaxHomeScroll();
        if (maxScroll <= 0) return RECT{track.left, track.top, track.right, track.bottom};
        const int trackH = track.bottom - track.top;
        const int contentH = std::max(HomeContentHeight(), 1);
        const int thumbH = std::max(46, trackH * HomeListHeight() / contentH);
        const int range = std::max(1, trackH - thumbH);
        const int top = track.top + range * homeScrollOffset_ / maxScroll;
        return RECT{track.left, top, track.right, top + thumbH};
    }
    RECT CreateRect() const { return RECT{35, 375, 683, 459}; }
    RECT CreateWordRect() const { RECT cr = CreateRect(); return RECT{cr.left + 270, cr.top + 21, cr.left + 348, cr.bottom - 21}; }
    std::wstring ScriptHotkeyText(const ScriptMeta& script) const { return script.hotkey.enabled && !script.hotkey.text.empty() ? script.hotkey.text : L"设置热键"; }
    RECT ScriptHotkeyRect(int i) const {
        if (i < 0 || i >= static_cast<int>(scripts_.size())) return RECT{};
        RECT r = HomeCardRect(i);
        const int left = r.left + 14;
        const int rightLimit = r.right - 100;
        const int hotW = TextWidth(ScriptHotkeyText(scripts_[static_cast<size_t>(i)]), hotFont_);
        const int nameW = TextWidth(scripts_[static_cast<size_t>(i)].name, homeFont_);
        const int hotLeft = std::min(left + nameW + 10, rightLimit - hotW);
        return RECT{hotLeft, r.top + 17, std::min(hotLeft + hotW, rightLimit), r.top + 48};
    }
    RECT ImportRect() const { return RECT{60, 149, 166, 182}; }
    RECT ExportRect() const { return RECT{180, 149, 286, 182}; }
    RECT TimerRect() const { return RECT{300, 149, 406, 182}; }
    RECT HelpRect() const { return RECT{576, 149, 686, 182}; }
    RECT DeleteDialogRect() const { RECT rc{}; GetClientRect(hwnd_, &rc); const int w = 450, h = 252; const int x = (rc.right - w) / 2; const int y = (rc.bottom - h) / 2; return RECT{x, y, x + w, y + h}; }
    RECT DeleteOkRect() const { RECT d = DeleteDialogRect(); return RECT{d.left + 32, d.top + 138, d.left + 216, d.top + 208}; }
    RECT DeleteCancelRect() const { RECT d = DeleteDialogRect(); return RECT{d.left + 236, d.top + 138, d.right - 32, d.top + 208}; }
    int TextWidth(const std::wstring& text, HFONT font) const {
        if (!hwnd_ || !font) return static_cast<int>(text.size()) * 18;
        HDC hdc = GetDC(hwnd_);
        HGDIOBJ oldFont = SelectObject(hdc, font);
        SIZE size{};
        GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &size);
        SelectObject(hdc, oldFont);
        ReleaseDC(hwnd_, hdc);
        return size.cx;
    }

    RECT CommonHotRect() const {
        const RECT cr = CreateRect();
        const std::wstring prefix = L"按";
        const std::wstring suffix = L"键开始 运行宏";
        const int hotW = std::max(80, TextWidth(globalHotkey_.text, bigFont_) + 28);
        const int prefixW = TextWidth(prefix, bigFont_);
        const int suffixW = TextWidth(suffix, bigFont_);
        const int gap = 20;
        const int totalW = prefixW + gap + hotW + gap + suffixW;
        const int left = cr.left + (cr.right - cr.left - totalW) / 2 + prefixW + gap;
        return RECT{left, cr.top + 27, left + hotW, cr.bottom - 20};
    }
    RECT RunHintRect() const { return CreateRect(); }
    RECT CloseRect() const { RECT rc{}; GetClientRect(hwnd_, &rc); return RECT{rc.right - kCloseBtnW, 0, rc.right, kTitleH}; }
    RECT MinimizeRect() const { RECT close = CloseRect(); return RECT{close.left - kTitleBtnW, 0, close.left, kTitleH}; }
    RECT SettingsRect() const { RECT min = MinimizeRect(); return RECT{min.left - kTitleBtnW, 0, min.left, kTitleH}; }
    RECT LoadButtonRect() const { return WindowClientRect(loadBtn_); }
    RECT ClearButtonRect() const { return WindowClientRect(clearBtn_); }
    RECT BatchExitButtonRect() const { return WindowClientRect(batchExitBtn_); }
    RECT BatchSelectAllButtonRect() const { return WindowClientRect(batchSelectAllBtn_); }
    RECT BatchDeselectButtonRect() const { return WindowClientRect(batchDeselectBtn_); }
    RECT BatchDeleteButtonRect() const { return WindowClientRect(batchDeleteBtn_); }
    RECT BatchCopyButtonRect() const { return WindowClientRect(batchCopyBtn_); }
    RECT AddButtonRect() const { return WindowClientRect(addBtn_); }
    RECT ModifyButtonRect() const { return WindowClientRect(modifyBtn_); }
    RECT CancelButtonRect() const { return WindowClientRect(cancelBtn_); }
    RECT SaveButtonRect() const { return WindowClientRect(saveBtn_); }
    RECT CrosshairButtonRect() const { return WindowClientRect(crosshairBtn_); }
    int ActionListCheckboxColumnRight() const { return kListX + kListInnerPad + kColRemarkInList - 4; }
    RECT CheckboxRect(int index) const {
        RECT list = ActionListRect();
        const int slot = VisibleSlotOf(index);
        if (slot < 0) return RECT{};
        const int localY = kListInnerPad + (slot - scrollOffset_) * kRowH;
        RECT local = LocalCheckboxRect(index, localY);
        OffsetRect(&local, list.left, list.top);
        return local;
    }
    bool HitClose(int x, int y) const { return PtIn(CloseRect(), x, y); }
    bool HitMinimize(int x, int y) const { return PtIn(MinimizeRect(), x, y); }
    bool HitSettings(int x, int y) const { return PtIn(SettingsRect(), x, y); }
    RECT ActionListRect() const {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const int bodyBottom = static_cast<int>(rc.bottom) - (page_ == Page::Editor ? kBottomH : 0);
        const int h = std::max(0, std::min(kListH, bodyBottom - kListY));
        return RECT{kListX, kListY, kListX + kListW, kListY + h};
    }
    int VisibleActionRows() const {
        const RECT list = ActionListRect();
        return std::max(0, static_cast<int>((list.bottom - list.top) / kRowH));
    }
    int VisibleActionCount() const {
        int count = 0;
        for (int i = 0; i < static_cast<int>(actions_.size()); ++i) if (IsRowVisible(i)) ++count;
        return count;
    }
    std::vector<int> VisibleActionIndices() const {
        std::vector<int> out;
        out.reserve(actions_.size());
        for (int i = 0; i < static_cast<int>(actions_.size()); ++i) if (IsRowVisible(i)) out.push_back(i);
        return out;
    }
    int VisibleSlotOf(int index) const {
        int slot = 0;
        for (int i = 0; i < static_cast<int>(actions_.size()); ++i) {
            if (!IsRowVisible(i)) continue;
            if (i == index) return slot;
            ++slot;
        }
        return -1;
    }
    int ContainerBodyEndIndex(int containerIndex) const { return ContainerBodyEnd(actions_, containerIndex); }
    int LoopBodyEnd(int loopIndex) const { return ContainerBodyEndIndex(loopIndex); }
    int SubtreeEnd(int index) const { return ::SubtreeEnd(actions_, index); }
    bool IsContainerExpanded(int index) const { return ::IsContainerExpanded(collapsedContainers_, index); }
    bool IsLoopExpanded(int index) const { return IsContainerExpanded(index); }
    bool IsRowVisible(int index) const { return IsRowVisibleInTree(actions_, collapsedContainers_, index); }
    int ActionLeftClient(int indent) const { return kListX + kListInnerPad + kColActionInList + indent * kIndentStep; }
    int ActionContentLeftLocal(int indent, int pad = kListInnerPad) const { return pad + kColActionInList + indent * kIndentStep; }
    int BatchCheckboxLeftLocal(int indent, ActionType type, int pad = kListInnerPad) const {
        const int contentLeft = ActionContentLeftLocal(indent, pad);
        return IsExpandableContainer(type) ? contentLeft + kExpandToggleSlot : contentLeft;
    }
    int ActionTextLeftLocal(int indent, ActionType type, bool batchMode, int pad = kListInnerPad) const {
        const int contentLeft = ActionContentLeftLocal(indent, pad);
        if (!batchMode) return contentLeft;
        if (IsExpandableContainer(type)) return contentLeft + kExpandToggleSlot + kBatchCheckboxSize + kBatchItemGap;
        return contentLeft + kBatchCheckboxSize + kBatchItemGap;
    }
    int ExpandToggleLeftLocal(int indent, bool batchMode, int pad = kListInnerPad) const {
        const int contentLeft = ActionContentLeftLocal(indent, pad);
        return batchMode ? contentLeft : contentLeft - kExpandToggleSlot;
    }
    int IndentFromX(int x) const { return std::max(0, (x - ActionLeftClient(0)) / kIndentStep); }
    struct DragInsertTarget { int insertIndex = 0; int targetIndent = 0; bool nested = false; };
    int ActionListContentRight() const { return kListX + kListW - (MaxEditorScroll() > 0 ? kEditorScrollW + 6 : 0); }
    int MaxEditorScroll() const { return std::max(0, VisibleActionCount() - VisibleActionRows()); }
    RECT EditorScrollTrackRect() const {
        RECT list = ActionListRect();
        return RECT{list.right - kEditorScrollW - 4, list.top + 2, list.right - 4, list.bottom - 2};
    }
    RECT EditorScrollThumbRect() const {
        RECT track = EditorScrollTrackRect();
        const int maxScroll = MaxEditorScroll();
        if (maxScroll <= 0) return track;
        const int trackH = track.bottom - track.top;
        const int contentH = std::max(VisibleActionCount() * kRowH, 1);
        RECT list = ActionListRect();
        const int visibleH = std::max(1, static_cast<int>(list.bottom - list.top));
        const int thumbH = std::max(32, trackH * visibleH / contentH);
        const int range = std::max(1, trackH - thumbH);
        const int top = track.top + range * scrollOffset_ / maxScroll;
        return RECT{track.left, top, track.right, top + thumbH};
    }
    void UpdateEditorScrollFromThumb(int thumbTop) {
        RECT track = EditorScrollTrackRect();
        RECT thumb = EditorScrollThumbRect();
        const int maxScroll = MaxEditorScroll();
        const int trackHeight = static_cast<int>(track.bottom - track.top);
        const int thumbHeight = static_cast<int>(thumb.bottom - thumb.top);
        const int range = std::max(1, trackHeight - thumbHeight);
        const int thumbOffset = thumbTop - static_cast<int>(track.top);
        scrollOffset_ = std::clamp(thumbOffset * maxScroll / range, 0, maxScroll);
    }

    RECT ClickerTabRect() const { return RECT{0, kTitleH, kHomeTabW, kHomeContentTop}; }
    RECT RecorderTabRect() const { return RECT{kHomeTabW, kTitleH, kHomeTabW * 2, kHomeContentTop}; }
    RECT MacroTabRect() const { return RECT{kHomeTabW * 2, kTitleH, kHomeTabW * 3, kHomeContentTop}; }
    RECT ScriptCustomTabRect() const { return RECT{kHomeTabW * 3, kTitleH, kHomeTabW * 4, kHomeContentTop}; }

    static constexpr int kClickerLabelX = 61;
    static constexpr int kClickerFieldGap = 8;
    static constexpr int kClickerComboRight = 575;
    static constexpr int kClickerRadioSize = 20;

    struct ClickerHomeLayout {
        int leftRadioLeft = 167;
        int middleRadioLeft = 303;
        int rightRadioLeft = 437;
        int intervalComboLeft = 245;
        int hotkeyComboLeft = 245;
    };
    ClickerHomeLayout clickerLayout_{};

    void UpdateClickerLayout() {
        static constexpr int kRadioTextGap = 9;
        static constexpr int kOptionGap = 17;
        int x = kClickerLabelX + TextWidth(L"点击类型:", homeFont_) + kClickerFieldGap;
        clickerLayout_.leftRadioLeft = x;
        x += kClickerRadioSize + kRadioTextGap + TextWidth(L"鼠标左键", homeFont_) + kOptionGap;
        clickerLayout_.middleRadioLeft = x;
        x += kClickerRadioSize + kRadioTextGap + TextWidth(L"鼠标中键", homeFont_) + kOptionGap;
        clickerLayout_.rightRadioLeft = x;
        clickerLayout_.intervalComboLeft = kClickerLabelX + TextWidth(L"每次点击间隔时间:", homeFont_) + kClickerFieldGap;
        clickerLayout_.hotkeyComboLeft = kClickerLabelX + TextWidth(L"启停的全局热键:", homeFont_) + kClickerFieldGap;
    }

    RECT ClickerLeftRadioRect() const {
        return RECT{clickerLayout_.leftRadioLeft, 171, clickerLayout_.leftRadioLeft + kClickerRadioSize, 191};
    }
    RECT ClickerMiddleRadioRect() const {
        return RECT{clickerLayout_.middleRadioLeft, 171, clickerLayout_.middleRadioLeft + kClickerRadioSize, 191};
    }
    RECT ClickerRightRadioRect() const {
        return RECT{clickerLayout_.rightRadioLeft, 171, clickerLayout_.rightRadioLeft + kClickerRadioSize, 191};
    }
    RECT ClickerIntervalRect() const {
        return RECT{clickerLayout_.intervalComboLeft, 236, kClickerComboRight, 266};
    }
    RECT ClickerHotkeyRect() const {
        return RECT{clickerLayout_.hotkeyComboLeft, 303, kClickerComboRight, 333};
    }
    static constexpr int kClickerDropdownItemH = 68;
    static constexpr int kClickerHotkeyMenuCount = 9;
    bool ClickerIntervalPopupOpen() const { return clickerDropPopupKind_ == 0; }
    bool ClickerHotkeyPopupOpen() const { return clickerDropPopupKind_ == 1; }
    int ClickerPopupItemCount() const {
        if (clickerDropPopupKind_ == 0) return 3;
        if (clickerDropPopupKind_ == 1) return kClickerHotkeyMenuCount;
        return 0;
    }
    int ClickerPopupVisibleCount() const {
        if (clickerPopupVisibleCount_ > 0) return clickerPopupVisibleCount_;
        return ClickerPopupItemCount();
    }
    RECT ClickerBannerKeyRect() const {
        RECT cr = CreateRect();
        const std::wstring hotText = globalHotkey_.text.empty() ? L"F8" : globalHotkey_.text;
        const int hotW = std::max(56, TextWidth(hotText, bigFont_) + 20);
        return RECT{cr.left + 204, cr.top + 21, cr.left + 204 + hotW, cr.bottom - 21};
    }
    RECT RecordingOptimizeRect(int i) const {
        RECT r = RecordingCardRect(i);
        return RECT{r.right - 150, r.top + 14, r.right - 82, r.top + 34};
    }
    RECT RecordingRenameRect(int i) const {
        RECT r = RecordingCardRect(i);
        return RECT{r.right - 74, r.top + 14, r.right - 14, r.top + 34};
    }
    RECT RecordingDeleteRect(int i) const {
        RECT r = RecordingCardRect(i);
        return RECT{r.right - 74, r.top + 56, r.right - 14, r.top + 88};
    }
    RECT RecordingDeselectRect(int i) const {
        RECT r = RecordingCardRect(i);
        return RECT{r.right - 96, r.top + 14, r.right - 24, r.top + 45};
    }
    RECT RecordingSelectedTagRect(int i) const {
        RECT r = RecordingCardRect(i);
        return RECT{r.right - 96, r.top + 58, r.right - 18, r.top + 95};
    }
    RECT RecorderHotkeyRect() const { return RECT{267, 397, 359, 438}; }
    RECT RecorderScopeRect() const { return RECT{556, 369, 692, 412}; }

    static LRESULT CALLBACK EditorChildSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR refData) {
        auto* self = reinterpret_cast<MainWindow*>(refData);
        if (self && hwnd == self->paramViewport_) {
            switch (msg) {
            case WM_COMMAND:
                self->OnCommand(LOWORD(wp), HIWORD(wp), reinterpret_cast<HWND>(lp));
                return 0;
            case WM_DRAWITEM:
                self->DrawOwnerItem(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
                return TRUE;
            case WM_MEASUREITEM:
                self->MeasureOwnerItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lp));
                return TRUE;
            case WM_CTLCOLORSTATIC:
                return self->OnCtlColorStatic(reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp));
            case WM_CTLCOLOREDIT:
                return self->OnEditColor(reinterpret_cast<HDC>(wp));
            case WM_ERASEBKGND: {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                FillRect(reinterpret_cast<HDC>(wp), &rc, self->whiteBrush_);
                return 1;
            }
            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(hwnd, &ps);
                RECT full{};
                GetClientRect(hwnd, &full);
                FillRect(hdc, &full, self->whiteBrush_);
                self->PaintEditorParamChrome(hdc, hwnd);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_MOUSEWHEEL:
                self->OnWheel(GET_WHEEL_DELTA_WPARAM(wp));
                return 0;
            default:
                break;
            }
        }
        wchar_t clsName[32]{};
        GetClassNameW(hwnd, clsName, 32);
        if (lstrcmpW(clsName, L"EDIT") == 0 && ModernEditHandleShortcutMessage(hwnd, msg, wp, lp))
            return 0;
        if (self && (msg == WM_ERASEBKGND || msg == WM_PAINT)) {
            if (self->IsParamMaskControl(hwnd)) {
                if (msg == WM_ERASEBKGND) return 1;
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(hwnd, &ps);
                RECT rc{};
                GetClientRect(hwnd, &rc);
                FillRect(hdc, &rc, hwnd == self->paramBottomMask_ ? self->panelBrush_ : self->whiteBrush_);
                EndPaint(hwnd, &ps);
                return 0;
            }
            if (self->EditorComboPopupIdForHwnd(hwnd) >= 0) {
                return msg == WM_ERASEBKGND ? 1 : 0;
            }
            if (self->IsParamPanelCheckbox(hwnd)) {
                if (msg == WM_ERASEBKGND) return 1;
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(hwnd, &ps);
                RECT rc{};
                GetClientRect(hwnd, &rc);
                FillRect(hdc, &rc, self->whiteBrush_);
                wchar_t text[128]{};
                GetWindowTextW(hwnd, text, 128);
                SelectObject(hdc, self->editorFont_ ? self->editorFont_ : self->font_);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, kText);
                constexpr int kCbSize = 18;
                const int cbTop = rc.top + (rc.bottom - rc.top - kCbSize) / 2;
                RECT cbRc{rc.left, cbTop, rc.left + kCbSize, cbTop + kCbSize};
                FillRectColor(hdc, cbRc, kWhite);
                StDrawCheckbox(hdc, cbRc, self->Checked(hwnd));
                RECT textRc{rc.left + kCbSize + 4, rc.top, rc.right, rc.bottom};
                DrawTextW(hdc, text, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                EndPaint(hwnd, &ps);
                return 0;
            }
        }
        if (self && msg == WM_ERASEBKGND && self->IsGrayButton(hwnd)) {
            return 1;
        }
        if (self && msg == WM_MOUSEWHEEL && self->page_ == Page::Editor) {
            self->OnWheel(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }
        if (self && msg == WM_KEYDOWN && hwnd == self->listRemarkEdit_) {
            if (wp == VK_RETURN) { self->CommitInlineRemark(); return 0; }
            if (wp == VK_ESCAPE) { self->CancelInlineRemark(); return 0; }
        }
        if (self && msg == WM_LBUTTONDOWN) {
            CrosshairDragBinding binding{};
            if (self->crosshairDrag_.TryGetBinding(hwnd, binding)) {
                self->crosshairDrag_.Begin(binding);
                return 0;
            }
        }
        // Combo toggling is handled in parent OnMouseDown via EditorComboHitTest.
        if (self && hwnd == self->quickInputEdit_ && msg == WM_MOUSEMOVE) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &pt);
            ScreenToClient(self->hwnd_, &pt);
            self->UpdateQuickInputTextTip(pt.x, pt.y);
        }
        if (self && hwnd == self->quickInputEdit_ && msg == WM_MOUSELEAVE) {
            self->CancelQuickInputTip();
        }
        if (self) self->EditorChildMessage(hwnd, msg, wp, lp);
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    void EditorChildMessage(HWND hwnd, UINT msg, WPARAM, LPARAM lp) {
        if (page_ != Page::Editor) return;
        if (msg == WM_MOUSEMOVE) EnsureChildMouseTracking(hwnd);
        if (msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE) {
            POINT pt{};
            if (msg == WM_MOUSEMOVE) {
                pt.x = GET_X_LPARAM(lp);
                pt.y = GET_Y_LPARAM(lp);
                ClientToScreen(hwnd, &pt);
            } else {
                GetCursorPos(&pt);
            }
            ScreenToClient(hwnd_, &pt);
            OnMouseMove(pt.x, pt.y);
        }
    }

    void AttachEditorChildSubclass() {
        for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            SetWindowSubclass(child, EditorChildSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
        }
    }

    RECT WindowClientRect(HWND child) const {
        RECT rc{};
        if (!child) return rc;
        GetWindowRect(child, &rc);
        MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&rc), 2);
        return rc;
    }

    void UpdateHoverFromCursor() {
        if (page_ != Page::Editor || !hwnd_) return;
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        if (pt.x < rc.left || pt.y < rc.top || pt.x >= rc.right || pt.y >= rc.bottom) return;
        OnMouseMove(pt.x, pt.y);
    }

    void StartHoverTimer() { if (hwnd_) SetTimer(hwnd_, kHoverTimerId, 30, nullptr); }
    void StopHoverTimer() { if (hwnd_) KillTimer(hwnd_, kHoverTimerId); }

    // ── Inline remark editing ──────────────────────────────────────
    void CommitInlineRemark() {
        if (editingRemarkIndex_ < 0 || editingRemarkIndex_ >= static_cast<int>(actions_.size()) || !listRemarkEdit_) return;
        if (IsWindowVisible(listRemarkEdit_)) {
            actions_[static_cast<size_t>(editingRemarkIndex_)].remark = GetText(listRemarkEdit_);
            ShowWindow(listRemarkEdit_, SW_HIDE);
        }
        editingRemarkIndex_ = -1;
        RefreshActionListLayer();
    }

    void CancelInlineRemark() {
        if (editingRemarkIndex_ < 0 || !listRemarkEdit_) return;
        ShowWindow(listRemarkEdit_, SW_HIDE);
        editingRemarkIndex_ = -1;
        RefreshActionListLayer();
    }

    void BeginInlineRemarkEdit(int row) {
        if (row < 0 || row >= static_cast<int>(actions_.size())) return;
        if (editingRemarkIndex_ != row) CommitInlineRemark();
        editingRemarkIndex_ = row;
        selectedIndex_ = row;
        LoadForm(actions_[static_cast<size_t>(row)]);
        RECT rc = RemarkRect(row);
        MoveWindow(listRemarkEdit_, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
        SetText(listRemarkEdit_, actions_[static_cast<size_t>(row)].remark);
        ShowWindow(listRemarkEdit_, SW_SHOW);
        SetWindowPos(listRemarkEdit_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetFocus(listRemarkEdit_);
        SendMessageW(listRemarkEdit_, EM_SETSEL, 0, -1);
        RefreshActionListLayer();
    }

    RECT RemarkRect(int index) const {
        RECT r = RowRect(index);
        const int left = kColRemarkClient + 1;
        const int right = std::max(left + 40, std::min(kColOpClient - 4, ActionListContentRight() - 2));
        return RECT{left, r.top + (kRowH - kRemarkEditH) / 2, right, r.top + (kRowH - kRemarkEditH) / 2 + kRemarkEditH};
    }

    bool PtInRemark(int x, int y) const {
        int row = HitRow(x, y);
        return row >= 0 && PtIn(RemarkRect(row), x, y);
    }

    void UpdateInlineRemarkEditorPosition() {
        if (editingRemarkIndex_ < 0 || !listRemarkEdit_ || !IsWindowVisible(listRemarkEdit_)) return;
        const int visible = VisibleActionRows();
        const int slot = VisibleSlotOf(editingRemarkIndex_);
        if (slot < 0 || slot < scrollOffset_ || slot >= scrollOffset_ + visible) {
            CommitInlineRemark();
            return;
        }
        RECT rc = RemarkRect(editingRemarkIndex_);
        MoveWindow(listRemarkEdit_, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, TRUE);
    }

    void EnsureMouseTracking() {
        if (trackingMouse_) return;
        TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd_, 0};
        if (TrackMouseEvent(&tme)) trackingMouse_ = true;
    }

    void EnsureChildMouseTracking(HWND hwnd) {
        TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
    }

    void OnMouseLeave() {
        trackingMouse_ = false;
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        RECT client{};
        GetClientRect(hwnd_, &client);
        if (PtInRect(&client, pt)) {
            UpdateGrayButtonHover(pt.x, pt.y);
            FlushGrayButtonHover();
            return;
        }
        ClearGrayButtonHover();
        pendingHoverGrayOld_ = nullptr;
        pendingHoverGrayNew_ = nullptr;
        if (page_ == Page::Editor) {
            const int oldRow = hoverIndex_;
            const HoverButton oldButton = hoverButton_;
            hoverIndex_ = -1;
            hoverButton_ = HoverButton::None;
            if (oldRow != -1) RefreshActionListLayer();
            if (oldButton != HoverButton::None) InvalidateHoverButton(oldButton);
        } else if (page_ == Page::Home) {
            const int oldCard = homeHover_;
            const int oldRecording = recordingHover_;
            const HoverButton oldButton = hoverButton_;
            homeHover_ = -1;
            recordingHover_ = -1;
            hoverButton_ = HoverButton::None;
            if (oldCard != -1) InvalidateHomeCard(oldCard);
            if (oldRecording != -1) InvalidateRecordingCard(oldRecording);
            if (oldButton != HoverButton::None) InvalidateHoverButton(oldButton);
        }
    }

    // ── Action list off-screen rendering ───────────────────────────
    void RefreshActionListLayer() {
        if (page_ != Page::Editor || !hwnd_) return;
        scrollOffset_ = std::clamp(scrollOffset_, 0, MaxEditorScroll());
        RECT list = ActionListRect();
        const int lw = list.right - list.left;
        const int lh = list.bottom - list.top;
        if (lw <= 0 || lh <= 0) return;
        HDC wndDc = GetDC(hwnd_);
        HDC memDc = CreateCompatibleDC(wndDc);
        HBITMAP memBmp = CreateCompatibleBitmap(wndDc, lw, lh);
        HGDIOBJ oldBmp = SelectObject(memDc, memBmp);
        HGDIOBJ oldFont = SelectObject(memDc, editorFont_);
        PaintActionListLocal(memDc, lw, lh);
        SelectObject(memDc, oldFont);
        if (IsWindowVisible(listRemarkEdit_)) {
            RECT erc = WindowClientRect(listRemarkEdit_);
            ExcludeClipRect(wndDc, erc.left, erc.top, erc.right, erc.bottom);
        }
        BitBlt(wndDc, list.left, list.top, lw, lh, memDc, 0, 0, SRCCOPY);
        SelectObject(memDc, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDc);
        PaintDragMarker(wndDc);
        UpdateInlineRemarkEditorPosition();
        ReleaseDC(hwnd_, wndDc);
    }

    void DrawListCheckbox(HDC hdc, RECT rc, bool checked) {
        HPEN boxPen = CreatePen(PS_SOLID, 1, checked ? kMainGreen : RGB(190, 190, 190));
        HBRUSH boxBrush = CreateSolidBrush(checked ? kMainGreen : kWhite);
        HGDIOBJ oldPen = SelectObject(hdc, boxPen);
        HGDIOBJ oldBrush = SelectObject(hdc, boxBrush);
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 3, 3);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(boxPen);
        DeleteObject(boxBrush);
        if (checked) DrawTextIn(hdc, L"✓", rc, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    void DrawExpandTriangle(HDC hdc, RECT rc, bool expanded, COLORREF color) {
        POINT pts[3]{};
        const int cx = (rc.left + rc.right) / 2;
        const int cy = (rc.top + rc.bottom) / 2;
        if (expanded) {
            pts[0] = {cx - 4, cy - 2}; pts[1] = {cx + 4, cy - 2}; pts[2] = {cx, cy + 4};
        } else {
            pts[0] = {cx - 2, cy - 4}; pts[1] = {cx - 2, cy + 4}; pts[2] = {cx + 4, cy};
        }
        HPEN pen = CreatePen(PS_SOLID, 1, color);
        HBRUSH brush = CreateSolidBrush(color);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, brush);
        Polygon(hdc, pts, 3);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
    }

    void ToggleContainerExpand(int index) {
        if (index < 0 || index >= static_cast<int>(actions_.size()) || !IsExpandableContainer(actions_[static_cast<size_t>(index)].type)) return;
        if (IsContainerExpanded(index)) collapsedContainers_.insert(index);
        else collapsedContainers_.erase(index);
        scrollOffset_ = std::clamp(scrollOffset_, 0, MaxEditorScroll());
        RefreshActionListLayer();
    }

    void PaintActionListLocal(HDC hdc, int width, int height);

    RECT LocalCheckboxRect(int index, int y) const {
        const int pad = kListInnerPad;
        const auto& a = actions_[static_cast<size_t>(index)];
        const int top = y + (kRowH - kBatchCheckboxSize) / 2;
        const int left = BatchCheckboxLeftLocal(a.indent, a.type, pad);
        return RECT{left, top, left + kBatchCheckboxSize, top + kBatchCheckboxSize};
    }

    RECT LocalCopyRect(int, int y, int contentRight) const { return RECT{contentRight - 104, y + 6, contentRight - 62, y + kRowH - 6}; }
    RECT LocalDeleteRect(int, int y, int contentRight) const { return RECT{contentRight - 58, y + 6, contentRight - 18, y + kRowH - 6}; }

    void PaintEditorScrollbarLocal(HDC hdc, int width, int height);
    void PaintParamScrollScrollbar(HDC hdc);

    void InvalidateRectClipped(RECT rc) {
        if (rc.right > rc.left && rc.bottom > rc.top) InvalidateRect(hwnd_, &rc, FALSE);
    }

    void InvalidateHomeCard(int index) {
        if (index < 0 || index >= static_cast<int>(scripts_.size())) return;
        InvalidateRectClipped(HomeCardRect(index));
    }

    void InvalidateRecordingCard(int index) {
        if (index < 0 || index >= static_cast<int>(recordings_.size())) return;
        InvalidateRectClipped(RecordingCardRect(index));
    }

    int HitRecordingCard(int x, int y) const {
        if (activeHomeTab_ != quickscript::MainTab::Recorder) return -1;
        RECT list = HomeListRect();
        if (!PtIn(list, x, y)) return -1;
        for (int i = 0; i < static_cast<int>(recordings_.size()); ++i) {
            RECT r = RecordingCardRect(i);
            if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return i;
        }
        return -1;
    }

    void InvalidateRow(int index) {
        if (page_ == Page::Editor) { RefreshActionListLayer(); return; }
        if (index < 0 || index >= static_cast<int>(actions_.size())) return;
        RECT r = RowRect(index);
        if (r.bottom < kListY || r.top > kListY + kListH) return;
        InvalidateRectClipped(r);
    }

    HWND HoverButtonHwnd(HoverButton btn) const {
        switch (btn) {
        case HoverButton::Load: return loadBtn_;
        case HoverButton::Clear: return clearBtn_;
        case HoverButton::Add: return addBtn_;
        case HoverButton::Modify: return modifyBtn_;
        case HoverButton::Cancel: return cancelBtn_;
        case HoverButton::Save: return saveBtn_;
        case HoverButton::Crosshair: return hoverCrosshairBtn_ ? hoverCrosshairBtn_ : crosshairBtn_;
        case HoverButton::BatchExit: return batchExitBtn_;
        case HoverButton::BatchSelectAll: return batchSelectAllBtn_;
        case HoverButton::BatchDeselect: return batchDeselectBtn_;
        case HoverButton::BatchDelete: return batchDeleteBtn_;
        case HoverButton::BatchCopy: return batchCopyBtn_;
        default: return nullptr;
        }
    }

    void InvalidateHoverButton(HoverButton btn) {
        if (btn == HoverButton::Close || btn == HoverButton::Minimize || btn == HoverButton::Settings) {
            RECT rc = btn == HoverButton::Close ? CloseRect()
                : (btn == HoverButton::Minimize ? MinimizeRect() : SettingsRect());
            InvalidateRect(hwnd_, &rc, FALSE);
            return;
        }
        HWND btnHwnd = HoverButtonHwnd(btn);
        if (btnHwnd) InvalidateRect(btnHwnd, nullptr, FALSE);
    }

    // ── Mouse input ────────────────────────────────────────────────
    void OnMouseMove(int x, int y) {
        if (deleteConfirmVisible_) {
            SetCursor(LoadCursorW(nullptr, (PtIn(DeleteOkRect(), x, y) || PtIn(DeleteCancelRect(), x, y)) ? IDC_HAND : IDC_ARROW));
            return;
        }
        if (page_ == Page::Editor && editorPopupOpen_ >= 0) {
            if (EditorDropPopupVisible()) return;
        }
        if (page_ == Page::Editor) UpdateQuickInputTextTip(x, y);
        if (homeScrollbarDragging_) {
            UpdateHomeScrollFromThumb(y - homeScrollbarDragOffset_);
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (editorScrollbarDragging_) {
            UpdateEditorScrollFromThumb(y - editorScrollbarDragOffset_);
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            RefreshActionListLayer();
            return;
        }
        if (paramScrollbarDragging_) {
            UpdateParamScrollFromThumb(y - paramScrollbarDragOffset_);
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            ApplyParamScrollOffset(false, false);
            InvalidateRectClipped(ParamScrollTrackRect());
            return;
        }
        HoverButton oldButton = hoverButton_;
        hoverButton_ = HitButton(x, y);
        hoverCrosshairBtn_ = (hoverButton_ == HoverButton::Crosshair) ? CrosshairButtonAtPoint(x, y) : nullptr;
        UpdateGrayButtonHover(x, y);
        if (page_ == Page::Editor && PtInRemark(x, y)) SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
        else if (hoverGrayBtn_) SetCursor(LoadCursorW(nullptr, IDC_HAND));
        else SetCursor(LoadCursorW(nullptr, IsClickablePoint(x, y) ? IDC_HAND : IDC_ARROW));
        EnsureMouseTracking();
        if (page_ == Page::Home) {
            const int oldCard = homeHover_;
            const int oldRecording = recordingHover_;
            homeHover_ = HitHomeCard(x, y);
            recordingHover_ = HitRecordingCard(x, y);
            if (oldCard != homeHover_) {
                InvalidateHomeCard(oldCard);
                InvalidateHomeCard(homeHover_);
            }
            if (oldRecording != recordingHover_) {
                InvalidateRecordingCard(oldRecording);
                InvalidateRecordingCard(recordingHover_);
            }
            if (oldButton != hoverButton_) {
                InvalidateHoverButton(oldButton);
                InvalidateHoverButton(hoverButton_);
            }
            FlushGrayButtonHover();
            return;
        }
        const int oldRow = hoverIndex_;
        hoverIndex_ = HitRow(x, y);
        if (dragging_) {
            if (abs(x - dragStartX_) >= kDragThreshold || abs(y - dragStartY_) >= kDragThreshold) {
                MoveDragged(x, y);
                RefreshActionListLayer();
            }
        } else {
            if (oldRow != hoverIndex_) RefreshActionListLayer();
            // Defer all button invalidations until after RefreshActionListLayer
            // to avoid races between direct screen painting and WM_DRAWITEM
            HoverButton oldHb = (oldButton != hoverButton_) ? oldButton : HoverButton::None;
            HoverButton newHb = (oldButton != hoverButton_) ? hoverButton_ : HoverButton::None;
            if (oldHb != HoverButton::None) InvalidateHoverButton(oldHb);
            if (newHb != HoverButton::None) InvalidateHoverButton(newHb);
            FlushGrayButtonHover();
        }
    }

    HWND CrosshairButtonAtPoint(int x, int y) const {
        return crosshairDrag_.HitButton(x, y, [this](HWND hwnd) { return WindowClientRect(hwnd); });
    }

    HoverButton HitButton(int x, int y) const {
        if (deleteConfirmVisible_) {
            if (PtIn(DeleteOkRect(), x, y) || PtIn(DeleteCancelRect(), x, y)) return HoverButton::HomeDelete;
            return HoverButton::None;
        }
        if (HitClose(x, y)) return HoverButton::Close;
        if (HitMinimize(x, y)) return HoverButton::Minimize;
        if (page_ == Page::Home && HitSettings(x, y)) return HoverButton::Settings;
        if (page_ == Page::Home) {
            if (PtIn(ClickerTabRect(), x, y) || PtIn(RecorderTabRect(), x, y) || PtIn(MacroTabRect(), x, y) || PtIn(ScriptCustomTabRect(), x, y)) return HoverButton::HomeCard;
            if (activeHomeTab_ == quickscript::MainTab::Clicker) {
                if (ClickerDropPopupVisible()) return HoverButton::ClickerInterval;
                if (PtIn(ClickerIntervalRect(), x, y)) return HoverButton::ClickerInterval;
                if (PtIn(ClickerHotkeyRect(), x, y)) return HoverButton::ClickerHotkey;
                if (PtIn(ClickerBannerKeyRect(), x, y)) return HoverButton::CommonHotkey;
                if (PtIn(ClickerLeftRadioRect(), x, y) || PtIn(ClickerMiddleRadioRect(), x, y) || PtIn(ClickerRightRadioRect(), x, y)) return HoverButton::HomeCard;
                return HoverButton::None;
            }
            if (activeHomeTab_ == quickscript::MainTab::Recorder) {
                if (PtIn(ImportRect(), x, y)) return HoverButton::Import;
                if (PtIn(ExportRect(), x, y)) return HoverButton::Export;
                if (PtIn(TimerRect(), x, y)) return HoverButton::HomeCard;
                if (PtIn(RecorderWindowModeRect(), x, y)) return HoverButton::HomeCard;
                if (PtIn(RecorderBannerKeyRect(), x, y)) return HoverButton::CommonHotkey;
                for (int i = 0; i < static_cast<int>(recordings_.size()); ++i) {
                    RECT r = RecordingCardRect(i);
                    RECT list = HomeListRect();
                    if (r.bottom < list.top || r.top > list.bottom) continue;
                    if (i == selectedRecording_) {
                        if (PtIn(RecordingDeselectRect(i), x, y)) return HoverButton::HomeEdit;
                        if (PtIn(RecordingSelectedTagRect(i), x, y)) return HoverButton::HomeCard;
                    } else {
                        if (PtIn(RecordingOptimizeRect(i), x, y)) return HoverButton::HomeEdit;
                        if (PtIn(RecordingRenameRect(i), x, y)) return HoverButton::HomeEdit;
                        if (PtIn(RecordingDeleteRect(i), x, y)) return HoverButton::HomeDelete;
                    }
                    if (PtIn(RecordingHotkeyRect(i), x, y)) return HoverButton::ScriptHotkey;
                    if (PtIn(r, x, y)) return HoverButton::HomeCard;
                }
                return HoverButton::None;
            }
            if (activeHomeTab_ == quickscript::MainTab::ScriptCustom) {
                if (PtIn(CreateRect(), x, y)) return HoverButton::HomeCard;
                return HoverButton::None;
            }
            if (PtIn(ImportRect(), x, y)) return HoverButton::Import;
            if (PtIn(ExportRect(), x, y)) return HoverButton::Export;
            if (PtIn(TimerRect(), x, y)) return HoverButton::HomeCard;
            if (selectedScript_ >= 0 && PtIn(CommonHotRect(), x, y)) return HoverButton::CommonHotkey;
            if (selectedScript_ < 0 && PtIn(CreateWordRect(), x, y)) return HoverButton::Create;
            if (MaxHomeScroll() > 0 && PtIn(HomeScrollTrackRect(), x, y)) return HoverButton::HomeScroll;
            for (int i = 0; i < static_cast<int>(scripts_.size()); ++i) {
                RECT r = HomeCardRect(i);
                RECT list = HomeListRect();
                if (r.bottom < list.top || r.top > list.bottom) continue;
                RECT hot = ScriptHotkeyRect(i);
                RECT edit{r.right - 96, r.top + 16, r.right - 24, r.top + 45};
                RECT del{r.right - 96, r.top + 58, r.right - 24, r.top + 88};
                if (PtIn(hot, x, y)) return HoverButton::ScriptHotkey;
                if (PtIn(edit, x, y)) return HoverButton::HomeEdit;
                if (PtIn(del, x, y)) return HoverButton::HomeDelete;
            }
            return HoverButton::None;
        }
        if (PtIn(LoadButtonRect(), x, y) && !batchEditMode_) return HoverButton::Load;
        if (PtIn(ClearButtonRect(), x, y) && !batchEditMode_) return HoverButton::Clear;
        if (batchEditMode_) {
            if (PtIn(BatchExitButtonRect(), x, y)) return HoverButton::BatchExit;
            if (PtIn(BatchSelectAllButtonRect(), x, y)) return HoverButton::BatchSelectAll;
            if (PtIn(BatchDeselectButtonRect(), x, y)) return HoverButton::BatchDeselect;
            if (BatchSelectedCount() > 0) {
                if (PtIn(BatchDeleteButtonRect(), x, y)) return HoverButton::BatchDelete;
                if (PtIn(BatchCopyButtonRect(), x, y)) return HoverButton::BatchCopy;
            }
        }
        if (PtIn(AddButtonRect(), x, y)) return HoverButton::Add;
        if (PtIn(ModifyButtonRect(), x, y)) return HoverButton::Modify;
        if (PtIn(CancelButtonRect(), x, y)) return HoverButton::Cancel;
        if (PtIn(SaveButtonRect(), x, y)) return HoverButton::Save;
        if (CrosshairButtonAtPoint(x, y)) return HoverButton::Crosshair;
        if (MaxEditorScroll() > 0 && PtIn(EditorScrollTrackRect(), x, y)) return HoverButton::EditorScroll;
        int row = HitRow(x, y);
        if (row >= 0) {
            if (batchEditMode_) {
                if (IsExpandableContainer(actions_[static_cast<size_t>(row)].type) && PtIn(ExpandToggleRect(row), x, y)) return HoverButton::Row;
                if (PtIn(CheckboxRect(row), x, y)) return HoverButton::RowCheckbox;
                return HoverButton::None;
            }
            if (PtIn(CopyRect(row), x, y)) return HoverButton::RowCopy;
            if (PtIn(DeleteRect(row), x, y)) return HoverButton::RowDelete;
            return HoverButton::Row;
        }
        return HoverButton::None;
    }

    bool IsClickablePoint(int x, int y) const {
        if (page_ == Page::Editor && PtInRemark(x, y)) return true;
        return HitButton(x, y) != HoverButton::None || HitGrayButton(x, y) != nullptr;
    }

    void ClickButton(HoverButton button, int x, int y) {
        if (button == HoverButton::Close) { if (page_ == Page::Editor) ShowHome(); else SendMessageW(hwnd_, WM_CLOSE, 0, 0); return; }
        if (button == HoverButton::Minimize) { ShowWindow(hwnd_, SW_MINIMIZE); return; }
        if (button == HoverButton::Settings && page_ == Page::Home) { ShowSettingsDialog(); return; }
        if (button == HoverButton::Import) { ImportScript(); return; }
        if (button == HoverButton::Export) { ExportSelectedScript(); return; }
        if (button == HoverButton::Create) { ShowEditorFor(-1, true); return; }
        if (button == HoverButton::CommonHotkey) { CaptureGlobalHotkey(); return; }
        if (button == HoverButton::Load) { EnterBatchEditMode(); return; }
        if (button == HoverButton::Clear) {
            actions_.clear();
            collapsedContainers_.clear();
            selectedIndex_ = -1;
            if (batchEditMode_) batchSelected_.clear();
            UpdateBatchToolbar();
            UpdateEditMode();
            return;
        }
        if (button == HoverButton::BatchExit) { ExitBatchEditMode(); return; }
        if (button == HoverButton::BatchSelectAll) { BatchSelectAll(); return; }
        if (button == HoverButton::BatchDeselect) { BatchDeselectAll(); return; }
        if (button == HoverButton::BatchDelete) { BatchDeleteSelected(); return; }
        if (button == HoverButton::BatchCopy) { BatchCopySelected(); return; }
        if (button == HoverButton::Add) { ShowAddMenuAt(x, y); return; }
        if (button == HoverButton::Modify) { ModifySelected(); return; }
        if (button == HoverButton::Cancel) { ShowHome(); return; }
        if (button == HoverButton::Save) { SaveIfEditor(); ShowHome(); return; }
    }

    int HitHomeCard(int x, int y) const {
        if (activeHomeTab_ != quickscript::MainTab::Macro) return -1;
        RECT list = HomeListRect();
        if (!PtIn(list, x, y)) return -1;
        for (int i = 0; i < static_cast<int>(scripts_.size()); ++i) {
            RECT r = HomeCardRect(i);
            if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return i;
        }
        return -1;
    }

    void OnMouseDown(int x, int y) {
        if (deleteConfirmVisible_) { OnDeleteConfirmClick(x, y); return; }
        if (HitClose(x, y)) { if (page_ == Page::Editor) ShowHome(); else SendMessageW(hwnd_, WM_CLOSE, 0, 0); return; }
        if (HitMinimize(x, y)) { ShowWindow(hwnd_, SW_MINIMIZE); return; }
        if (HitSettings(x, y) && page_ == Page::Home) { ShowSettingsDialog(); return; }
        if (y <= kTitleH) { ReleaseCapture(); SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(x, y)); return; }
        if (page_ == Page::Home && activeHomeTab_ == quickscript::MainTab::Macro && MaxHomeScroll() > 0 && PtIn(HomeScrollThumbRect(), x, y)) {
            RECT thumb = HomeScrollThumbRect();
            homeScrollbarDragging_ = true;
            homeScrollbarDragOffset_ = y - thumb.top;
            SetCapture(hwnd_);
            return;
        }
        if (page_ == Page::Editor && EditorComboHitTest(x, y)) return;
        if (page_ == Page::Editor && editorPopupOpen_ >= 0) {
            if (EditorDropPopupVisible()) {
                POINT pt{x, y};
                ClientToScreen(hwnd_, &pt);
                RECT drop{};
                GetWindowRect(editorDropPopup_, &drop);
                if (!PtIn(drop, pt.x, pt.y)) {
                    const int comboHit = EditorComboPopupIdAtPoint(x, y);
                    if (comboHit == editorPopupOpen_) ToggleEditorPopup(comboHit);
                    else CloseEditorPopup();
                }
            } else if (HandleEditorPopupClick(x, y)) return;
        }
        if (page_ == Page::Home) { OnHomeClick(x, y); return; }
        if (MaxEditorScroll() > 0 && PtIn(EditorScrollThumbRect(), x, y)) {
            RECT thumb = EditorScrollThumbRect();
            editorScrollbarDragging_ = true;
            editorScrollbarDragOffset_ = y - thumb.top;
            SetCapture(hwnd_);
            return;
        }
        if (MaxEditorScroll() > 0 && PtIn(EditorScrollTrackRect(), x, y)) {
            RECT thumb = EditorScrollThumbRect();
            editorScrollbarDragging_ = true;
            editorScrollbarDragOffset_ = (thumb.bottom - thumb.top) / 2;
            UpdateEditorScrollFromThumb(y - editorScrollbarDragOffset_);
            SetCapture(hwnd_);
            RefreshActionListLayer();
            return;
        }
        if (MaxParamScroll() > 0 && PtIn(ParamScrollThumbRect(), x, y)) {
            RECT thumb = ParamScrollThumbRect();
            paramScrollbarDragging_ = true;
            paramScrollbarDragOffset_ = y - thumb.top;
            SetCapture(hwnd_);
            return;
        }
        if (MaxParamScroll() > 0 && PtIn(ParamScrollTrackRect(), x, y)) {
            RECT thumb = ParamScrollThumbRect();
            paramScrollbarDragging_ = true;
            paramScrollbarDragOffset_ = (thumb.bottom - thumb.top) / 2;
            UpdateParamScrollFromThumb(y - paramScrollbarDragOffset_);
            SetCapture(hwnd_);
            ApplyParamScrollOffset();
            return;
        }
        HoverButton hit = HitButton(x, y);
        if (hit == HoverButton::Row || hit == HoverButton::RowCopy || hit == HoverButton::RowDelete || hit == HoverButton::RowCheckbox) { OnEditorClick(x, y); return; }
        if (hit != HoverButton::None) { ClickButton(hit, x, y); return; }
        OnEditorClick(x, y);
    }

    void OnMouseUp(int, int) {
        if (homeScrollbarDragging_) {
            homeScrollbarDragging_ = false;
            ReleaseCapture();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (editorScrollbarDragging_) {
            editorScrollbarDragging_ = false;
            ReleaseCapture();
            RefreshActionListLayer();
            return;
        }
        if (paramScrollbarDragging_) {
            paramScrollbarDragging_ = false;
            ReleaseCapture();
            ApplyParamScrollOffset(true);
            return;
        }
        if (page_ != Page::Editor || !dragging_) { dragging_ = false; dragIndex_ = -1; dragTargetIndex_ = -1; dragTargetIndent_ = 0; dragTargetNested_ = false; dragStartX_ = 0; dragStartY_ = 0; ReleaseCapture(); return; }
        CompleteDrag();
        dragging_ = false;
        dragIndex_ = -1;
        dragTargetIndex_ = -1;
        dragTargetIndent_ = 0;
        dragTargetNested_ = false;
        dragStartX_ = 0;
        dragStartY_ = 0;
        dragMoved_ = false;
        ReleaseCapture();
        RefreshActionListLayer();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SetActiveHomeTab(quickscript::MainTab tab) {
        if (activeHomeTab_ == tab) return;
        activeHomeTab_ = tab;
        CloseClickerDropPopup();
        CloseEditorPopup();
        hoverButton_ = HoverButton::None;
        if (tab == quickscript::MainTab::Recorder) ClampRecordingScroll();
        if (tab == quickscript::MainTab::Macro) ClampHomeScroll();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    bool HandleHomeNavClick(int x, int y) {
        if (PtIn(ClickerTabRect(), x, y)) { SetActiveHomeTab(quickscript::MainTab::Clicker); return true; }
        if (PtIn(RecorderTabRect(), x, y)) { SetActiveHomeTab(quickscript::MainTab::Recorder); return true; }
        if (PtIn(MacroTabRect(), x, y)) { SetActiveHomeTab(quickscript::MainTab::Macro); return true; }
        if (PtIn(ScriptCustomTabRect(), x, y)) { SetActiveHomeTab(quickscript::MainTab::ScriptCustom); return true; }
        return false;
    }

    void CaptureGlobalHotkey() {
        HotkeyCapture cap;
        Hotkey out;
        if (!cap.Show(hwnd_, globalHotkey_, false, out, true) || !out.enabled || out.vk == 0) return;
        globalHotkey_ = out;
        RegisterAllHotkeys();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool HandleClickerIntervalClick(int x, int y) {
        if (PtIn(ClickerIntervalRect(), x, y)) {
            if (clickerDropPopupKind_ == 0) CloseClickerDropPopup();
            else OpenClickerDropPopup(0);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
        if (clickerDropPopupKind_ == 0) {
            if (PtIn(ClickerHotkeyRect(), x, y)) return false;
            CloseClickerDropPopup();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
        return false;
    }

    bool HandleClickerHotkeyClick(int x, int y) {
        if (PtIn(ClickerHotkeyRect(), x, y)) {
            if (clickerDropPopupKind_ == 1) CloseClickerDropPopup();
            else OpenClickerDropPopup(1);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
        if (clickerDropPopupKind_ == 1) {
            if (PtIn(ClickerIntervalRect(), x, y)) return false;
            CloseClickerDropPopup();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
        return false;
    }

    void OpenClickerDropPopup(int kind) {
        clickerDropPopupKind_ = kind;
        clickerPopupHover_ = -1;
        clickerPopupScroll_ = 0;
        clickerPopupVisibleCount_ = 0;
        SyncClickerDropPopup();
    }

    void CloseClickerDropPopup() {
        if (clickerDropPopupKind_ < 0) return;
        clickerDropPopupKind_ = -1;
        clickerPopupHover_ = -1;
        clickerPopupScroll_ = 0;
        clickerPopupVisibleCount_ = 0;
        if (clickerDropPopup_) ShowWindow(clickerDropPopup_, SW_HIDE);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    bool ClickerDropPopupVisible() const {
        return clickerDropPopup_ && IsWindowVisible(clickerDropPopup_) == TRUE;
    }

    void CreateClickerDropPopup() {
        RegisterClickerDropPopupClass();
        clickerDropPopup_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            L"QSClickerDropPopup", L"",
            WS_POPUP,
            0, 0, 0, 0,
            hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (clickerDropPopup_) {
            SetWindowLongPtrW(clickerDropPopup_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            ShowWindow(clickerDropPopup_, SW_HIDE);
        }
    }

    void SyncClickerDropPopup() {
        if (!clickerDropPopup_) return;
        if (clickerDropPopupKind_ < 0 || page_ != Page::Home
            || activeHomeTab_ != quickscript::MainTab::Clicker
            || !IsWindowVisible(hwnd_) || IsIconic(hwnd_)) {
            ShowWindow(clickerDropPopup_, SW_HIDE);
            return;
        }
        const RECT anchor = clickerDropPopupKind_ == 0 ? ClickerIntervalRect() : ClickerHotkeyRect();
        const int itemCount = ClickerPopupItemCount();
        const int w = anchor.right - anchor.left;
        const int totalH = itemCount * kClickerDropdownItemH + 2;

        POINT anchorBottomScreen{anchor.left, anchor.bottom};
        ClientToScreen(hwnd_, &anchorBottomScreen);

        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int spaceBelow = static_cast<int>(work.bottom) - anchorBottomScreen.y;

        int h = totalH;
        const int x = anchorBottomScreen.x;
        int y = anchorBottomScreen.y;
        if (h > spaceBelow) {
            clickerPopupVisibleCount_ = std::max(1, spaceBelow / kClickerDropdownItemH);
            h = clickerPopupVisibleCount_ * kClickerDropdownItemH + 2;
            clickerPopupScroll_ = std::clamp(clickerPopupScroll_, 0,
                std::max(0, itemCount - clickerPopupVisibleCount_));
        } else {
            clickerPopupVisibleCount_ = itemCount;
            clickerPopupScroll_ = 0;
        }
        y = std::max(static_cast<int>(work.top),
            std::min(y, static_cast<int>(work.bottom) - h));

        RECT existing{};
        GetWindowRect(clickerDropPopup_, &existing);
        const bool samePos = existing.left == x && existing.top == y
            && (existing.right - existing.left) == w && (existing.bottom - existing.top) == h;
        if (samePos && ClickerDropPopupVisible()) return;
        SetWindowPos(clickerDropPopup_, HWND_TOP, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
    }

    void InvalidateClickerPopupRow(int idx) {
        if (!clickerDropPopup_ || idx < 0) return;
        RECT client{};
        GetClientRect(clickerDropPopup_, &client);
        const int vis = idx - clickerPopupScroll_;
        if (vis < 0 || vis >= ClickerPopupVisibleCount()) return;
        RECT row{client.left + 1, client.top + 1 + vis * kClickerDropdownItemH,
            client.right - 1, client.top + 1 + (vis + 1) * kClickerDropdownItemH};
        InvalidateRect(clickerDropPopup_, &row, FALSE);
    }

    int HitClickerPopupItemLocal(int, int y) const {
        if (clickerDropPopupKind_ < 0) return -1;
        const int visible = ClickerPopupVisibleCount();
        const int rel = (y - 1) / kClickerDropdownItemH;
        if (rel < 0 || rel >= visible) return -1;
        const int idx = rel + clickerPopupScroll_;
        if (idx >= ClickerPopupItemCount()) return -1;
        return idx;
    }

    void OnClickerDropPopupWheel(int delta) {
        if (clickerDropPopupKind_ < 0) return;
        const int itemCount = ClickerPopupItemCount();
        const int scrollMax = std::max(0, itemCount - ClickerPopupVisibleCount());
        if (scrollMax <= 0) return;
        const int oldScroll = clickerPopupScroll_;
        clickerPopupScroll_ = std::clamp(clickerPopupScroll_ + (delta > 0 ? -1 : 1), 0, scrollMax);
        if (oldScroll != clickerPopupScroll_ && clickerDropPopup_) {
            InvalidateRect(clickerDropPopup_, nullptr, FALSE);
        }
    }

    void SelectClickerPopupItem(int idx) {
        if (clickerDropPopupKind_ == 0) {
            const quickscript::ClickIntervalMode modes[3] = {
                quickscript::ClickIntervalMode::Custom,
                quickscript::ClickIntervalMode::Efficient,
                quickscript::ClickIntervalMode::Extreme
            };
            if (idx < 0 || idx >= 3) return;
            CloseClickerDropPopup();
            if (modes[idx] == quickscript::ClickIntervalMode::Custom) {
                ShowCustomIntervalDialog();
                return;
            }
            clickerSettings_.intervalMode = modes[idx];
        } else if (clickerDropPopupKind_ == 1) {
            static const HotkeyMenuItem kItems[] = {
                {kHotCustom, L"自定义", L"将您指定的按键设为启停热键"},
                {kHotF8, L"F8", L"将F8设为启停热键"},
                {kHotF10, L"F10", L"将F10设为启停热键"},
                {kHotLeft, L"鼠标左键", L"将长按左键设为启停热键"},
                {kHotMiddle, L"鼠标中键", L"将点击中键设为启停热键"},
                {kHotRight, L"鼠标右键", L"将点击右键设为启停热键"},
                {kHotX1, L"鼠标侧键1", L"一般为鼠标左侧后部的键"},
                {kHotX2, L"鼠标侧键2", L"一般为鼠标左侧前部的键"},
                {kHotSpace, L"空格键", L"将空格键设为启停热键"},
            };
            if (idx < 0 || idx >= kClickerHotkeyMenuCount) return;
            CloseClickerDropPopup();
            SetCommonHotkeyFromMenu(kItems[idx].id);
            return;
        }
        CloseClickerDropPopup();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void ShowCustomIntervalDialog() {
        CloseClickerDropPopup();
        struct DialogState {
            double val = 0.0;
            bool ok = false;
            bool done = false;
            HWND edit = nullptr;
            bool hoverClose = false;
            bool hoverCancel = false;
            bool hoverOk = false;
            HFONT titleFont = nullptr;
            HFONT bodyFont = nullptr;
            HFONT closeFont = nullptr;
        };
        DialogState state{};
        state.val = clickerSettings_.customIntervalSeconds;
        static constexpr int kDlgW = 420;
        static constexpr int kDlgH = 225;
        static constexpr int kDlgTitleH = kTitleH;
        const wchar_t* clsName = L"QuickScriptCustomIntervalDlg";
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                auto* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                auto cancelRect = []() { return RECT{108, 168, 198, 206}; };
                auto okRect = []() { return RECT{222, 168, 312, 206}; };
                auto closeRect = []() { return RECT{380, 0, 420, kTitleH}; };
                auto editOuterRect = []() {
                    static constexpr int kBodyTop = 38;
                    static constexpr int kBodyBottom = 168;
                    static constexpr int kCustomRowH = 38;
                    static constexpr int kEditW = 140;
                    static constexpr int kUnitW = 32;
                    static constexpr int kGap = 10;
                    static constexpr int kGroupW = kEditW + kGap + kUnitW;
                    static constexpr int kGroupLeft = (420 - kGroupW) / 2;
                    static constexpr int kRowTop = kBodyTop + (kBodyBottom - kBodyTop - kCustomRowH) / 2;
                    return RECT{kGroupLeft, kRowTop, kGroupLeft + kEditW, kRowTop + kCustomRowH};
                };
                auto unitRect = [&]() {
                    RECT edit = editOuterRect();
                    return RECT{edit.right + 10, edit.top, edit.right + 42, edit.bottom};
                };
                auto centerEditText = [](HWND edit) {
                    if (!edit) return;
                    RECT rc{};
                    GetClientRect(edit, &rc);
                    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
                    HDC hdc = GetDC(edit);
                    HFONT oldFont = font ? reinterpret_cast<HFONT>(SelectObject(hdc, font)) : nullptr;
                    TEXTMETRICW tm{};
                    GetTextMetricsW(hdc, &tm);
                    if (oldFont) SelectObject(hdc, oldFont);
                    ReleaseDC(edit, hdc);
                    const int textH = tm.tmHeight;
                    const int pad = std::max(0, static_cast<int>((rc.bottom - rc.top - textH) / 2)) + 2;
                    rc.top = pad;
                    rc.bottom = rc.top + textH + 2;
                    SendMessageW(edit, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&rc));
                };
                if (msg == WM_CREATE) {
                    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                    st = reinterpret_cast<DialogState*>(cs->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
                    st->titleFont = CreateFontW(26, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
                    st->bodyFont = CreateFontW(26, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
                    st->closeFont = CreateFontW(36, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
                    const RECT editOuter = editOuterRect();
                    st->edit = MakeModernSingleLineEdit(hwnd, nullptr, 100,
                        editOuter.left + 1, editOuter.top + 1,
                        editOuter.right - editOuter.left - 2, editOuter.bottom - editOuter.top - 2,
                        ES_CENTER);
                    ApplyModernEditBehavior(st->edit, false);
                    SendMessageW(st->edit, WM_SETFONT, reinterpret_cast<WPARAM>(st->bodyFont), TRUE);
                    wchar_t buf[32]{};
                    swprintf_s(buf, L"%.3f", st->val);
                    SetWindowTextW(st->edit, buf);
                    centerEditText(st->edit);
                    return 0;
                }
                if (!st) return DefWindowProcW(hwnd, msg, wp, lp);
                if (msg == WM_ERASEBKGND) return 1;
                if (msg == WM_SETCURSOR) {
                    if (LOWORD(lp) == HTCLIENT) {
                        POINT pt{};
                        GetCursorPos(&pt);
                        ScreenToClient(hwnd, &pt);
                        const bool hand = st->hoverClose || st->hoverCancel || st->hoverOk;
                        SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                        return TRUE;
                    }
                    return DefWindowProcW(hwnd, msg, wp, lp);
                }
                if (msg == WM_MOUSEMOVE) {
                    const int x = GET_X_LPARAM(lp);
                    const int y = GET_Y_LPARAM(lp);
                    const RECT closeR = closeRect();
                    const RECT cancelR = cancelRect();
                    const RECT okR = okRect();
                    const bool hc = PtInRect(&closeR, POINT{x, y});
                    const bool hcan = PtInRect(&cancelR, POINT{x, y});
                    const bool hok = PtInRect(&okR, POINT{x, y});
                    if (hc != st->hoverClose || hcan != st->hoverCancel || hok != st->hoverOk) {
                        st->hoverClose = hc;
                        st->hoverCancel = hcan;
                        st->hoverOk = hok;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return 0;
                }
                if (msg == WM_CTLCOLOREDIT) {
                    HDC hdc = reinterpret_cast<HDC>(wp);
                    SetBkColor(hdc, kWhite);
                    SetTextColor(hdc, kText);
                    static HBRUSH brush = CreateSolidBrush(kWhite);
                    return reinterpret_cast<LRESULT>(brush);
                }
                if (msg == WM_PAINT) {
                    PAINTSTRUCT ps{};
                    HDC hdc = BeginPaint(hwnd, &ps);
                    RECT titleRc{0, 0, 420, kTitleH};
                    FillRectColor(hdc, titleRc, kMainGreen);
                    FillRectColor(hdc, RECT{0, kTitleH, 420, 225}, kWhite);
                    if (st->hoverClose) FillRectColor(hdc, closeRect(), RGB(90, 190, 125));
                    SetBkMode(hdc, TRANSPARENT);
                    SetTextColor(hdc, kWhite);
                    HGDIOBJ oldFont = SelectObject(hdc, st->titleFont);
                    DrawTextW(hdc, L"  鼠大侠-自定义连点间隔", -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, st->closeFont);
                    RECT closeR = closeRect();
                    DrawTextW(hdc, L"×", -1, &closeR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SetTextColor(hdc, kText);
                    SelectObject(hdc, st->bodyFont);
                    DrawBorderRect(hdc, editOuterRect(), kComboBorderGray);
                    RECT unit = unitRect();
                    DrawTextW(hdc, L"秒", -1, &unit, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    const RECT cancel = cancelRect();
                    const RECT ok = okRect();
                    DrawBorderRoundRect(hdc, cancel, kMainGreen, 6);
                    SetTextColor(hdc, st->hoverCancel ? kDarkGreen : kMainGreen);
                    DrawTextW(hdc, L"取消", -1, const_cast<RECT*>(&cancel), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    StDrawGreenButton(hdc, st->bodyFont, ok, L"确定", st->hoverOk);
                    SelectObject(hdc, oldFont);
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                if (msg == WM_LBUTTONDOWN) {
                    const int x = GET_X_LPARAM(lp);
                    const int y = GET_Y_LPARAM(lp);
                    const RECT closeR = closeRect();
                    const RECT cancelR = cancelRect();
                    const RECT okR = okRect();
                    if (PtInRect(&closeR, POINT{x, y}) || PtInRect(&cancelR, POINT{x, y})) {
                        st->done = true;
                        DestroyWindow(hwnd);
                        return 0;
                    }
                    if (PtInRect(&okR, POINT{x, y})) {
                        wchar_t buf[64]{};
                        GetWindowTextW(st->edit, buf, 63);
                        try { st->val = std::stod(buf); st->ok = st->val > 0.0; } catch (...) { st->ok = false; }
                        st->done = true;
                        DestroyWindow(hwnd);
                        return 0;
                    }
                }
                if (msg == WM_KEYDOWN && wp == VK_RETURN) {
                    wchar_t buf[64]{};
                    GetWindowTextW(st->edit, buf, 63);
                    try { st->val = std::stod(buf); st->ok = st->val > 0.0; } catch (...) { st->ok = false; }
                    st->done = true;
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (msg == WM_DESTROY) {
                    if (st->titleFont) { DeleteObject(st->titleFont); st->titleFont = nullptr; }
                    if (st->bodyFont) { DeleteObject(st->bodyFont); st->bodyFont = nullptr; }
                    if (st->closeFont) { DeleteObject(st->closeFont); st->closeFont = nullptr; }
                    st->done = true;
                    return 0;
                }
                if (msg == WM_CLOSE) { st->done = true; DestroyWindow(hwnd); return 0; }
                return DefWindowProcW(hwnd, msg, wp, lp);
            };
            wc.hInstance = g_instance;
            wc.lpszClassName = clsName;
            wc.hbrBackground = nullptr;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassW(&wc);
            registered = true;
        }
        EnableWindow(hwnd_, FALSE);
        HWND dlg = CreateWindowExW(WS_EX_TOPMOST, clsName, L"", WS_POPUP,
            (GetSystemMetrics(SM_CXSCREEN) - kDlgW) / 2, (GetSystemMetrics(SM_CYSCREEN) - kDlgH) / 2,
            kDlgW, kDlgH, hwnd_, nullptr, g_instance, &state);
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);
        SetFocus(state.edit);
        MSG msg{};
        while (!state.done && IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        EnableWindow(hwnd_, TRUE);
        SetForegroundWindow(hwnd_);
        if (state.ok) {
            clickerSettings_.intervalMode = quickscript::ClickIntervalMode::Custom;
            clickerSettings_.customIntervalSeconds = state.val;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void OnClickerHomeClick(int x, int y) {
        if (HandleClickerIntervalClick(x, y)) return;
        if (HandleClickerHotkeyClick(x, y)) return;
        if (PtIn(ClickerBannerKeyRect(), x, y)) { CaptureGlobalHotkey(); return; }
        if (PtIn(ClickerLeftRadioRect(), x, y)) { clickerSettings_.button = quickscript::MouseButtonChoice::Left; RegisterAllHotkeys(); }
        else if (PtIn(ClickerMiddleRadioRect(), x, y)) { clickerSettings_.button = quickscript::MouseButtonChoice::Middle; RegisterAllHotkeys(); }
        else if (PtIn(ClickerRightRadioRect(), x, y)) { clickerSettings_.button = quickscript::MouseButtonChoice::Right; RegisterAllHotkeys(); }
        else if (PtIn(CreateRect(), x, y)) return;
        else if (clickerDropPopupKind_ >= 0) {
            CloseClickerDropPopup();
            return;
        }
        else return;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void CaptureRecordingHotkey(int index) {
        if (index < 0 || index >= static_cast<int>(recordings_.size())) return;
        HotkeyCapture cap; Hotkey out;
        if (cap.Show(hwnd_, recordings_[static_cast<size_t>(index)].hotkey, true, out)) {
            recordings_[static_cast<size_t>(index)].hotkey = out;
            PersistRecordingHotkey(index);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void PersistRecordingHotkey(int index) {
        if (index < 0 || index >= static_cast<int>(recordings_.size())) return;
        const ScriptMeta backup = recordings_[static_cast<size_t>(index)];
        const auto oldActions = actions_;
        const auto oldPath = currentPath_;
        const auto oldName = GetText(name_);
        const auto oldTime = currentRecordTime_;
        const int oldIndex = currentScriptIndex_;
        currentPath_ = backup.path;
        LoadScriptFile(currentPath_);
        SetText(name_, backup.name);
        currentRecordTime_ = backup.recordTime;
        saveDurationSeconds_ = backup.durationSeconds;
        saveHotkeyOverride_ = backup.hotkey;
        SaveScriptFile(currentPath_);
        saveHotkeyOverride_.reset();
        saveDurationSeconds_ = 0;
        actions_ = oldActions;
        currentPath_ = oldPath;
        SetText(name_, oldName);
        currentRecordTime_ = oldTime;
        currentScriptIndex_ = oldIndex;
        LoadRecordings();
    }

    void PersistRecordingRename(int index, const std::wstring& newName) {
        if (index < 0 || index >= static_cast<int>(recordings_.size()) || newName.empty()) return;
        ScriptMeta backup = recordings_[static_cast<size_t>(index)];
        const auto oldActions = actions_;
        const auto oldPath = currentPath_;
        const auto oldName = GetText(name_);
        const auto oldTime = currentRecordTime_;
        const int oldIndex = currentScriptIndex_;
        currentPath_ = backup.path;
        LoadScriptFile(currentPath_);
        SetText(name_, newName);
        currentRecordTime_ = backup.recordTime;
        saveDurationSeconds_ = backup.durationSeconds;
        saveHotkeyOverride_ = backup.hotkey;
        SaveScriptFile(currentPath_);
        saveHotkeyOverride_.reset();
        saveDurationSeconds_ = 0;
        const std::wstring newPath = RecordingsDir() + L"\\" + newName + L".json";
        if (_wcsicmp(currentPath_.c_str(), newPath.c_str()) != 0) {
            if (GetFileAttributesW(newPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                MoveFileW(currentPath_.c_str(), newPath.c_str());
            }
        }
        actions_ = oldActions;
        currentPath_ = oldPath;
        SetText(name_, oldName);
        currentRecordTime_ = oldTime;
        currentScriptIndex_ = oldIndex;
        LoadRecordings();
    }

    void ShowRenameRecordingDialog(int index) {
        if (index < 0 || index >= static_cast<int>(recordings_.size())) return;
        struct DialogState { std::wstring name; bool ok; bool done; HWND edit; };
        DialogState state{recordings_[static_cast<size_t>(index)].name, false, false, nullptr};
        const wchar_t* clsName = L"QuickScriptRenameRecordingDlg";
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                auto* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                auto cancelRect = []() { return RECT{88, 132, 158, 166}; };
                auto okRect = []() { return RECT{182, 132, 252, 166}; };
                auto closeRect = []() { return RECT{296, 4, 336, 36}; };
                auto editRect = []() { return RECT{40, 72, 300, 100}; };
                if (msg == WM_CREATE) {
                    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                    st = reinterpret_cast<DialogState*>(cs->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
                    st->edit = MakeModernSingleLineEdit(hwnd, nullptr, 100,
                        editRect().left, editRect().top,
                        editRect().right - editRect().left, editRect().bottom - editRect().top);
                    ApplyModernEditBehavior(st->edit, false, 0, false);
                    SetWindowTextW(st->edit, st->name.c_str());
                    SendMessageW(st->edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
                    SendMessageW(st->edit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
                    return 0;
                }
                if (!st) return DefWindowProcW(hwnd, msg, wp, lp);
                if (msg == WM_ERASEBKGND) return 1;
                if (msg == WM_PAINT) {
                    PAINTSTRUCT ps{};
                    HDC hdc = BeginPaint(hwnd, &ps);
                    RECT titleRc{0, 0, 340, 40};
                    FillRectColor(hdc, titleRc, kMainGreen);
                    RECT bodyRc{0, 40, 340, 180};
                    FillRectColor(hdc, bodyRc, kWhite);
                    RECT edit = editRect();
                    DrawBorderRect(hdc, edit, kComboBorderGray);
                    SetBkMode(hdc, TRANSPARENT);
                    SetTextColor(hdc, RGB(255, 255, 255));
                    HFONT titleFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
                    HFONT btnFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
                    HFONT closeFont = CreateFontW(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
                    HGDIOBJ oldFont = SelectObject(hdc, titleFont);
                    DrawTextW(hdc, L"  鼠大侠-重命名", -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, closeFont);
                    RECT close = closeRect();
                    DrawTextW(hdc, L"×", -1, &close, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, btnFont);
                    RECT cancel = cancelRect();
                    RECT ok = okRect();
                    DrawBorderRoundRect(hdc, cancel, kMainGreen, 6);
                    SetTextColor(hdc, kMainGreen);
                    DrawTextW(hdc, L"取消", -1, &cancel, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    HBRUSH okBrush = CreateSolidBrush(kMainGreen);
                    SelectObject(hdc, okBrush);
                    HGDIOBJ oldPen3 = SelectObject(hdc, GetStockObject(NULL_PEN));
                    RoundRect(hdc, ok.left, ok.top, ok.right, ok.bottom, 6, 6);
                    SetTextColor(hdc, RGB(255, 255, 255));
                    DrawTextW(hdc, L"确定", -1, &ok, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, oldPen3);
                    DeleteObject(okBrush);
                    SelectObject(hdc, oldFont);
                    DeleteObject(titleFont);
                    DeleteObject(btnFont);
                    DeleteObject(closeFont);
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                if (msg == WM_LBUTTONDOWN) {
                    const int x = GET_X_LPARAM(lp);
                    const int y = GET_Y_LPARAM(lp);
                    RECT close = closeRect();
                    if (x >= close.left && x <= close.right && y >= close.top && y <= close.bottom) {
                        st->done = true;
                        DestroyWindow(hwnd);
                        return 0;
                    }
                    RECT cancel = cancelRect();
                    if (x >= cancel.left && x <= cancel.right && y >= cancel.top && y <= cancel.bottom) {
                        st->done = true;
                        DestroyWindow(hwnd);
                        return 0;
                    }
                    RECT ok = okRect();
                    if (x >= ok.left && x <= ok.right && y >= ok.top && y <= ok.bottom) {
                        wchar_t buf[256]{};
                        GetWindowTextW(st->edit, buf, 255);
                        st->name = buf;
                        st->ok = !st->name.empty();
                        st->done = true;
                        DestroyWindow(hwnd);
                        return 0;
                    }
                }
                if (msg == WM_KEYDOWN && wp == VK_RETURN) {
                    wchar_t buf[256]{};
                    GetWindowTextW(st->edit, buf, 255);
                    st->name = buf;
                    st->ok = !st->name.empty();
                    st->done = true;
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (msg == WM_CLOSE) { st->done = true; DestroyWindow(hwnd); return 0; }
                return DefWindowProcW(hwnd, msg, wp, lp);
            };
            wc.hInstance = g_instance;
            wc.lpszClassName = clsName;
            wc.hbrBackground = nullptr;
            RegisterClassW(&wc);
            registered = true;
        }
        EnableWindow(hwnd_, FALSE);
        HWND dlg = CreateWindowExW(WS_EX_TOPMOST, clsName, L"", WS_POPUP,
            (GetSystemMetrics(SM_CXSCREEN) - 340) / 2, (GetSystemMetrics(SM_CYSCREEN) - 180) / 2,
            340, 180, hwnd_, nullptr, g_instance, &state);
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);
        SetFocus(state.edit);
        SendMessageW(state.edit, EM_SETSEL, 0, -1);
        MSG msg{};
        while (!state.done && IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        EnableWindow(hwnd_, TRUE);
        SetForegroundWindow(hwnd_);
        if (state.ok) {
            PersistRecordingRename(index, state.name);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void OnRecorderHomeClick(int x, int y) {
        if (PtIn(ImportRect(), x, y)) { ImportScript(); return; }
        if (PtIn(ExportRect(), x, y)) { ExportSelectedRecording(); return; }
        if (PtIn(TimerRect(), x, y)) { ShowScheduledTaskDialog(); return; }
        if (PtIn(RecorderWindowModeRect(), x, y)) {
            recorderSettings_.captureScope = recorderSettings_.captureScope == quickscript::RecordCaptureScope::Window
                ? quickscript::RecordCaptureScope::Global : quickscript::RecordCaptureScope::Window;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (PtIn(CreateRect(), x, y)) {
            if (PtIn(RecorderBannerKeyRect(), x, y)) { CaptureGlobalHotkey(); return; }
            return;
        }
        for (int i = 0; i < static_cast<int>(recordings_.size()); ++i) {
            RECT r = RecordingCardRect(i);
            RECT list = HomeListRect();
            if (r.bottom < list.top || r.top > list.bottom) continue;
            if (!PtIn(r, x, y)) continue;
            if (x >= RecordingHotkeyRect(i).left && x <= RecordingHotkeyRect(i).right && y >= RecordingHotkeyRect(i).top && y <= RecordingHotkeyRect(i).bottom) { CaptureRecordingHotkey(i); return; }
            if (i != selectedRecording_) {
                if (PtIn(RecordingOptimizeRect(i), x, y)) {
                    RecordingOptimizeDialog optimizeDlg;
                    if (optimizeDlg.Show(hwnd_, recordings_[static_cast<size_t>(i)]).saved) {
                        LoadRecordings();
                    }
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
                if (PtIn(RecordingRenameRect(i), x, y)) {
                    ShowRenameRecordingDialog(i);
                    return;
                }
                if (PtIn(RecordingDeleteRect(i), x, y)) {
                    const auto recPath = recordings_[static_cast<size_t>(i)].path;
                    DeleteUnreferencedImagesOfScript(recPath);
                    DeleteFileW(recPath.c_str());
                    LoadRecordings();
                    ClampRecordingScroll();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
            }
            selectedRecording_ = (selectedRecording_ == i) ? -1 : i;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    void ExportSelectedRecording() {
        if (selectedRecording_ < 0 || selectedRecording_ >= static_cast<int>(recordings_.size())) return;
        auto& meta = recordings_[static_cast<size_t>(selectedRecording_)];
        const auto content = ReadAll(meta.path);
        const auto imgPaths = CollectImagePathsFromJson(content);

        // 根据是否包含图片选择导出格式
        if (!imgPaths.empty()) {
            // ZIP 格式导出
            wchar_t fileBuffer[MAX_PATH + 128]{};
            const auto defaultName = meta.name + L".zip";
            wcsncpy_s(fileBuffer, defaultName.c_str(), defaultName.size());
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd_;
            ofn.lpstrFile = fileBuffer;
            ofn.nMaxFile = MAX_PATH + 128;
            ofn.lpstrFilter = L"ZIP 脚本包 (*.zip)\0*.zip\0所有文件 (*.*)\0*.*\0";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (!GetSaveFileNameW(&ofn)) return;

            std::wstring zipPath(fileBuffer);
            if (zipPath.size() < 4 || _wcsicmp(zipPath.substr(zipPath.size() - 4).c_str(), L".zip") != 0) {
                zipPath += L".zip";
            }
            std::vector<std::pair<std::wstring, std::wstring>> files;
            files.push_back({L"script.json", meta.path});
            for (const auto& imgPath : imgPaths) {
                const auto slashPos = imgPath.find_last_of(L"\\/");
                std::wstring imgName = (slashPos == std::wstring::npos) ? imgPath : imgPath.substr(slashPos + 1);
                files.push_back({imgName, imgPath});
            }
            const auto zipResult = CreateZipFile(zipPath, files, meta.path);
            if (zipResult.success) {
                if (zipResult.skippedFiles.empty()) {
                    MessageBoxW(hwnd_, L"录制已导出。图片已一同打包。可在\"鼠标宏\"中导入此文件。", L"导出", MB_OK | MB_ICONINFORMATION);
                } else {
                    const std::wstring msg = L"录制已导出，但有 " + std::to_wstring(zipResult.skippedFiles.size())
                        + L" 张图片未找到已跳过。\n\n可在\"鼠标宏\"中导入此文件，导入后需重新设置图片。";
                    MessageBoxW(hwnd_, msg.c_str(), L"导出", MB_OK | MB_ICONWARNING);
                }
            } else {
                MessageBoxW(hwnd_, L"导出失败：无法创建 ZIP 文件，请检查保存路径是否有写入权限。", L"导出", MB_OK | MB_ICONERROR);
            }
        } else {
            // 纯 JSON 导出
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd_;
            std::wstring name = meta.name + L".json";
            std::wstring ext = L"JSON 文件 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
            wchar_t fileBuffer[MAX_PATH + 128]{};
            wcsncpy_s(fileBuffer, name.c_str(), name.size());
            ofn.lpstrFile = fileBuffer;
            ofn.nMaxFile = MAX_PATH + 128;
            ofn.lpstrFilter = ext.c_str();
            ofn.nFilterIndex = 1;
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (!GetSaveFileNameW(&ofn)) return;
            CopyFileW(meta.path.c_str(), fileBuffer, FALSE);
            MessageBoxW(hwnd_, L"录制已导出。可在\"鼠标宏\"中导入此文件。", L"导出", MB_OK | MB_ICONINFORMATION);
        }
    }

    void OnMacroHomeClick(int x, int y) {
        if (PtIn(ImportRect(), x, y)) { ImportScript(); return; }
        if (PtIn(ExportRect(), x, y)) { ExportSelectedScript(); return; }
        if (PtIn(TimerRect(), x, y)) { ShowScheduledTaskDialog(); return; }
        if (selectedScript_ >= 0 && PtIn(CommonHotRect(), x, y)) { CaptureGlobalHotkey(); return; }
        if (selectedScript_ < 0 && PtIn(CreateWordRect(), x, y)) { ShowEditorFor(-1, true); return; }
        if (MaxHomeScroll() > 0 && PtIn(HomeScrollTrackRect(), x, y)) {
            RECT thumb = HomeScrollThumbRect();
            homeScrollbarDragging_ = true;
            homeScrollbarDragOffset_ = (thumb.bottom - thumb.top) / 2;
            UpdateHomeScrollFromThumb(y - homeScrollbarDragOffset_);
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        for (int i = 0; i < static_cast<int>(scripts_.size()); ++i) {
            RECT r = HomeCardRect(i);
            RECT list = HomeListRect();
            if (r.bottom < list.top || r.top > list.bottom) continue;
            if (x < r.left || x > r.right || y < r.top || y > r.bottom) continue;
            RECT hot = ScriptHotkeyRect(i);
            RECT edit{r.right - 96, r.top + 16, r.right - 24, r.top + 45};
            RECT del{r.right - 96, r.top + 58, r.right - 24, r.top + 88};
            if (x >= edit.left && x <= edit.right && y >= edit.top && y <= edit.bottom) { if (selectedScript_ == i) { selectedScript_ = -1; InvalidateRect(hwnd_, nullptr, FALSE); } else { ShowEditorFor(i, false); } return; }
            if (x >= del.left && x <= del.right && y >= del.top && y <= del.bottom) { ConfirmDelete(i); return; }
            if (x >= hot.left && x <= hot.right && y >= hot.top && y <= hot.bottom) { CaptureScriptHotkey(i); return; }
            selectedScript_ = selectedScript_ == i ? -1 : i;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    void OnHomeClick(int x, int y) {
        if (HandleHomeNavClick(x, y)) return;
        if (activeHomeTab_ == quickscript::MainTab::Clicker) { OnClickerHomeClick(x, y); return; }
        if (activeHomeTab_ == quickscript::MainTab::Recorder) { OnRecorderHomeClick(x, y); return; }
        if (activeHomeTab_ == quickscript::MainTab::ScriptCustom) {
            if (PtIn(CreateRect(), x, y)) {
                PostMessageW(hwnd_, WM_OPEN_AGENT_DIALOG, 0, 0);
            }
            return;
        }
        OnMacroHomeClick(x, y);
    }

    void CaptureScriptHotkey(int index) {
        if (index < 0 || index >= static_cast<int>(scripts_.size())) return;
        HotkeyCapture cap; Hotkey out;
        if (cap.Show(hwnd_, scripts_[static_cast<size_t>(index)].hotkey, true, out)) {
            scripts_[static_cast<size_t>(index)].hotkey = out;
            PersistScriptHotkey(index);
            RegisterAllHotkeys();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void PersistScriptHotkey(int index) {
        if (index < 0 || index >= static_cast<int>(scripts_.size())) return;
        auto oldActions = actions_; auto oldPath = currentPath_; auto oldName = GetText(name_); auto oldTime = currentRecordTime_; int oldIndex = currentScriptIndex_;
        currentScriptIndex_ = index; currentPath_ = scripts_[static_cast<size_t>(index)].path; LoadScriptFile(currentPath_); SaveScriptFile(currentPath_);
        actions_ = oldActions; currentPath_ = oldPath; SetText(name_, oldName); currentRecordTime_ = oldTime; currentScriptIndex_ = oldIndex;
    }

    void ConfirmDelete(int index) {
        if (index < 0 || index >= static_cast<int>(scripts_.size())) return;
        pendingDeleteIndex_ = index;
        deleteConfirmVisible_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void ExecutePendingDelete() {
        if (pendingDeleteIndex_ >= 0 && pendingDeleteIndex_ < static_cast<int>(scripts_.size())) {
            const auto scriptPath = scripts_[static_cast<size_t>(pendingDeleteIndex_)].path;
            // 删除脚本引用的孤立图片（不被其他脚本引用才删）
            DeleteUnreferencedImagesOfScript(scriptPath);
            DeleteFileW(scriptPath.c_str());
            selectedScript_ = -1;
            pendingDeleteIndex_ = -1;
            LoadScripts();
            ClampHomeScroll();
            RegisterAllHotkeys();
        }
        deleteConfirmVisible_ = false;
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void OnDeleteConfirmClick(int x, int y) {
        if (PtIn(DeleteOkRect(), x, y)) { ExecutePendingDelete(); return; }
        if (PtIn(DeleteCancelRect(), x, y)) {
            deleteConfirmVisible_ = false;
            pendingDeleteIndex_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void ShowHotkeyMenuAt(RECT anchor) {
        HMENU menu = CreatePopupMenu();
        hotkeyMenuItems_ = {
            {kHotCustom, L"自定义", L"将您指定的按键设为启停热键"},
            {kHotF8, L"F8", L"将F8设为启停热键"},
            {kHotF10, L"F10", L"将F10设为启停热键"},
            {kHotLeft, L"鼠标左键", L"将长按左键设为启停热键"},
            {kHotMiddle, L"鼠标中键", L"将点击中键设为启停热键"},
            {kHotRight, L"鼠标右键", L"将点击右键设为启停热键"},
            {kHotX1, L"鼠标侧键1", L"一般为鼠标左侧后部的键"},
            {kHotX2, L"鼠标侧键2", L"一般为鼠标左侧前部的键"},
            {kHotSpace, L"空格键", L"将空格键设为启停热键"}
        };
        for (size_t i = 0; i < hotkeyMenuItems_.size(); ++i) {
            MENUITEMINFOW item{};
            item.cbSize = sizeof(item);
            item.fMask = MIIM_ID | MIIM_FTYPE | MIIM_DATA;
            item.fType = MFT_OWNERDRAW;
            item.wID = static_cast<UINT>(hotkeyMenuItems_[i].id);
            item.dwItemData = reinterpret_cast<ULONG_PTR>(&hotkeyMenuItems_[i]);
            InsertMenuItemW(menu, static_cast<UINT>(i), TRUE, &item);
        }
        POINT pt{anchor.left, anchor.bottom};
        ClientToScreen(hwnd_, &pt);
        TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
    }

    void ShowCommonHotkeyMenu() { ShowHotkeyMenuAt(CommonHotRect()); }

    void SetCommonHotkeyFromMenu(int id) {
        if (id == kHotCustom) { HotkeyCapture cap; Hotkey out; if (cap.Show(hwnd_, globalHotkey_, false, out, true)) globalHotkey_ = out; }
        else if (id == kHotF8) globalHotkey_ = Hotkey{0, VK_F8, L"F8", true};
        else if (id == kHotF10) globalHotkey_ = Hotkey{0, VK_F10, L"F10", true};
        else if (id == kHotLeft) globalHotkey_ = Hotkey{0, VK_LBUTTON, L"鼠标左键", true};
        else if (id == kHotMiddle) globalHotkey_ = Hotkey{0, VK_MBUTTON, L"鼠标中键", true};
        else if (id == kHotRight) globalHotkey_ = Hotkey{0, VK_RBUTTON, L"鼠标右键", true};
        else if (id == kHotX1) globalHotkey_ = Hotkey{0, VK_XBUTTON1, L"鼠标侧键1", true};
        else if (id == kHotX2) globalHotkey_ = Hotkey{0, VK_XBUTTON2, L"鼠标侧键2", true};
        else if (id == kHotSpace) globalHotkey_ = Hotkey{0, VK_SPACE, L"空格键", true};
        RegisterAllHotkeys(); InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void RegisterAllHotkeys() {
        UnregisterHotKey(hwnd_, HOTKEY_GLOBAL_ID);
        if (globalHotkey_.enabled && globalHotkey_.vk) RegisterHotKey(hwnd_, HOTKEY_GLOBAL_ID, globalHotkey_.modifiers, globalHotkey_.vk);
        for (int i = 0; i < 100; ++i) UnregisterHotKey(hwnd_, HOTKEY_SCRIPT_BASE + i);
        for (int i = 0; i < static_cast<int>(scripts_.size()) && i < 100; ++i) {
            const auto& hk = scripts_[static_cast<size_t>(i)].hotkey;
            if (hk.enabled && hk.vk) RegisterHotKey(hwnd_, HOTKEY_SCRIPT_BASE + i, hk.modifiers, hk.vk);
        }
        RefreshGlobalHotkeyHooks();
    }

    void InstallGlobalHotkeyHooks() {
        ghHotkeyHwnd      = hwnd_;
        ghHotkeyEnabled   = true;
        ghHotkeyVk        = globalHotkey_.vk;
        ghHotkeyMods      = globalHotkey_.modifiers;
        ghHotkeyPending   = false;
        HINSTANCE inst    = GetModuleHandleW(nullptr);
        if (!ghHotkeyKbHook)
            ghHotkeyKbHook = SetWindowsHookExW(WH_KEYBOARD_LL, HotkeyKbProc, inst, 0);
        if (IsMouseVk(ghHotkeyVk) && !ghHotkeyMouseHook)
            ghHotkeyMouseHook = SetWindowsHookExW(WH_MOUSE_LL, HotkeyMouseProc, inst, 0);
    }

    void UninstallGlobalHotkeyHooks() {
        ghHotkeyEnabled = false;
        if (ghHotkeyKbHook)     { UnhookWindowsHookEx(ghHotkeyKbHook);     ghHotkeyKbHook     = nullptr; }
        if (ghHotkeyMouseHook)  { UnhookWindowsHookEx(ghHotkeyMouseHook);  ghHotkeyMouseHook  = nullptr; }
    }

    void RefreshGlobalHotkeyHooks() {
        ghHotkeyVk      = globalHotkey_.vk;
        ghHotkeyMods    = globalHotkey_.modifiers;
        ghHotkeyEnabled = globalHotkey_.enabled;
        ghHotkeyPending = false;
        HINSTANCE inst  = GetModuleHandleW(nullptr);
        if (IsMouseVk(ghHotkeyVk) && !ghHotkeyMouseHook)
            ghHotkeyMouseHook = SetWindowsHookExW(WH_MOUSE_LL, HotkeyMouseProc, inst, 0);
        else if (!IsMouseVk(ghHotkeyVk) && ghHotkeyMouseHook)
            { UnhookWindowsHookEx(ghHotkeyMouseHook); ghHotkeyMouseHook = nullptr; }
    }

    void OnHotkey(int id) {
        // Debounce: suppress duplicate global-hotkey triggers that arrive within 300 ms
        // (happens when both RegisterHotKey and the low-level fallback hook fire for the same event)
        if (id == HOTKEY_GLOBAL_ID) {
            DWORD now = GetTickCount();
            if (now - lastHotkeyTick_ < 300) return;
            lastHotkeyTick_ = now;
            if (clicking_) { StopClicking(); return; }
            if (recording_) { StopRecording(); return; }
            if (running_) { StopRun(); return; }
            switch (activeHomeTab_) {
            case quickscript::MainTab::Clicker:
                ToggleClicker();
                break;
            case quickscript::MainTab::Recorder:
                if (selectedRecording_ >= 0 && selectedRecording_ < static_cast<int>(recordings_.size())) {
                    LoadScriptFile(recordings_[static_cast<size_t>(selectedRecording_)].path);
                    RunCurrentActions();
                } else {
                    ToggleRecording();
                }
                break;
            case quickscript::MainTab::Macro:
                if (selectedScript_ >= 0) RunScriptByIndex(selectedScript_);
                break;
            default:
                break;
            }
            return;
        }
        if (clicking_ || recording_) return;
        if (running_) { StopRun(); return; }
        if (running_ && ShouldIgnoreHotkeyStop()) return;
        if (id >= HOTKEY_SCRIPT_BASE) RunScriptByIndex(id - HOTKEY_SCRIPT_BASE);
    }

    void RunScriptByIndex(int index) {
        if (index < 0 || index >= static_cast<int>(scripts_.size())) return;
        const std::wstring path = scripts_[static_cast<size_t>(index)].path;
        if (!(page_ == Page::Editor && path == currentPath_)) {
            LoadScriptFile(path);
        }
        RunCurrentActions();
    }

    void ShowScheduledTaskDialog() {
        ScheduledTaskDialog dlg;
        dlg.Show(hwnd_, scheduledTasks_);
        if (IsWindow(hwnd_)) {
            SetForegroundWindow(hwnd_);
        }
        StDiscardSpuriousInputAfterModal(hwnd_);
    }

    void OpenAgentDialog() {
        agentDialogs_.erase(
            std::remove_if(agentDialogs_.begin(), agentDialogs_.end(),
                [](const std::unique_ptr<AgentDialog>& d) { return !d || !d->IsAlive(); }),
            agentDialogs_.end());

        LoadAppSettings(appSettings_);
        auto dlg = std::make_unique<AgentDialog>();
        if (!dlg->Show(hwnd_, appSettings_.ai)) return;
        agentDialogs_.push_back(std::move(dlg));
    }

    void RefreshScriptLibraryUi() {
        LoadScripts();
        LoadRecordings();
        scheduledTasks_.Reload();
        if (selectedScript_ >= static_cast<int>(scripts_.size())) selectedScript_ = -1;
        if (selectedRecording_ >= static_cast<int>(recordings_.size())) selectedRecording_ = -1;
        if (currentScriptIndex_ >= static_cast<int>(scripts_.size())) currentScriptIndex_ = -1;
        ClampHomeScroll();
        ClampRecordingScroll();
        RegisterAllHotkeys();
        if (IsWindow(hwnd_)) InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void ShowSettingsDialog() {
        // 始终从磁盘加载最新设置后再打开，确保 AI 助手修改后立即可见
        LoadAppSettings(appSettings_);
        SettingsDialog dlg;
        if (dlg.Show(hwnd_, appSettings_)) {
            SaveAppSettings(appSettings_);
            ApplyDebugWindowSetting();
        }
        if (IsWindow(hwnd_)) {
            SetForegroundWindow(hwnd_);
        }
        StDiscardSpuriousInputAfterModal(hwnd_);
    }

    void RunActionsFromPath(const std::wstring& path) {
        if (running_ || path.empty()) return;
        const auto actions = ParseActionsFromFile(path);
        if (actions.empty()) return;
        StartActionsWorker(actions, path);
    }

    int RandomInt(int maxValue) { if (maxValue <= 0) return 0; std::uniform_int_distribution<int> dist(-maxValue, maxValue); return dist(rng_); }
    double RandomDelay(double maxValue) { if (maxValue <= 0) return 0; std::uniform_real_distribution<double> dist(0.0, maxValue); return dist(rng_); }
    void SleepInterruptible(double seconds) { const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000.0)); while (!stopFlag_ && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(10)); }

    std::wstring ResolveQuickInputText(const std::wstring& text) {
        MacroVariableContext ctx;
        ctx.matchVars = &matchVars_;
        ctx.ocrVars = workerUsesOcrVars_ ? &ocrVars_ : nullptr;
        ctx.aiVars = &aiVars_;
        ctx.loopVars = &loopVars_;
        ctx.timerStarts = &timerStarts_;
        ctx.curLoops = curLoops_;
        return DecodeQuickInputEscapes(ResolveMacroVariables(text, ctx));
    }

    void SendKey(UINT vk, bool down) {
        if (running_) MarkSimulatedInput();
        SendKeyboardKey(vk, down);
        if (running_) UnmarkSimulatedInput();
    }

    void MarkSimulatedInput() {
        simulatingInputDepth_.fetch_add(1, std::memory_order_relaxed);
    }

    void UnmarkSimulatedInput() {
        simulatingInputDepth_.fetch_sub(1, std::memory_order_relaxed);
    }

    bool ShouldIgnoreHotkeyStop() const {
        return simulatingInputDepth_.load(std::memory_order_relaxed) > 0;
    }

    void SendHeldModifiers(const ScriptAction& a, bool down) {
        if (a.holdLeftWin) SendKey(VK_LWIN, down);
        if (a.holdRightWin) SendKey(VK_RWIN, down);
        if (a.holdLeftCtrl) SendKey(VK_LCONTROL, down);
        if (a.holdRightCtrl) SendKey(VK_RCONTROL, down);
        if (a.holdLeftAlt) SendKey(VK_LMENU, down);
        if (a.holdRightAlt) SendKey(VK_RMENU, down);
        if (a.holdLeftShift) SendKey(VK_LSHIFT, down);
        if (a.holdRightShift) SendKey(VK_RSHIFT, down);
    }

    // ── Script execution (worker thread) ───────────────────────────
    void SyncFormIntoActionsBeforeRun() {
        if (page_ != Page::Editor) return;
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(actions_.size())) return;
        const ScriptAction& existing = actions_[static_cast<size_t>(selectedIndex_)];
        if (ComboSelForType(existing.type) != popupAction_.sel) return;
        ScriptAction action = ActionFromForm();
        if (action.type == ActionType::DefineBlock && !IsValidBlockName(action.blockName)) return;
        action.originalNo = existing.originalNo;
        action.indent = existing.indent;
        actions_[static_cast<size_t>(selectedIndex_)] = action;
    }

    void RunCurrentActions() {
        if (running_) return;
        SyncFormIntoActionsBeforeRun();
        StartActionsWorker(actions_, currentPath_);
    }

    void StartActionsWorker(const std::vector<ScriptAction>& actions, const std::wstring& selfPath) {
        if (running_) return;
        running_ = true; stopFlag_ = false; aiHttpAbort_.Clear(); wasVisibleBeforeRun_ = IsWindowVisible(hwnd_) == TRUE; wasMinimizedBeforeRun_ = IsIconic(hwnd_) == TRUE;
        CloseEditorPopup(); CancelQuickInputTip();
        if (appSettings_.other.playSoundOnStart) MessageBeep(MB_OK);
        if (appSettings_.other.autoHideMainWindow) {
            AddTray();
            ShowWindow(hwnd_, SW_HIDE);
        }
        UpdateStatusTip();
        if (appSettings_.playback.enableDebugOutputWindow) ShowDebugWindow();
        if (macroDebugWindow_.IsCreated()) macroDebugWindow_.ClearLog();
        worker_ = std::thread([this, actions, selfPath]() {
            bool usesOcr = ScriptUsesTextRecognition(actions);
            workerUsesOcrVars_ = usesOcr;
            matchVars_.clear();
            if (usesOcr) ocrVars_.clear();
            loopVars_.clear();
            timerStarts_.clear();
            aiVars_.clear();
            curLoops_ = 0;
            bool ocrSessionHeld = false;
            auto holdOcrSession = [&ocrSessionHeld]() {
                if (ocrSessionHeld) return;
                EnsureOcrSession();
                ocrSessionHeld = true;
            };
            if (usesOcr) holdOcrSession();
            UINT heldKeyVk = 0;
            HBITMAP lockedScreen_ = nullptr;
            int lockedVirtX_ = 0;
            int lockedVirtY_ = 0;

            auto clearLockedScreen = [&]() {
                if (lockedScreen_) {
                    DeleteBitmapHandle(lockedScreen_);
                    lockedScreen_ = nullptr;
                }
            };

            const std::vector<ScriptAction>* activeActions = &actions;
            std::wstring runningScriptPath = selfPath;

            auto containerBodyEnd = [&activeActions](size_t containerIndex) -> size_t {
                return static_cast<size_t>(ContainerBodyEnd(*activeActions, static_cast<int>(containerIndex)));
            };

            std::function<bool(size_t, size_t)> runRange;
            std::function<void(const std::wstring&)> runBlockByName;
            std::unordered_set<std::wstring> blockCallStack;

            auto makeVarCtx = [&]() {
                MacroVariableContext ctx;
                ctx.matchVars = &matchVars_;
                ctx.ocrVars = usesOcr ? &ocrVars_ : nullptr;
                ctx.aiVars = &aiVars_;
                ctx.loopVars = &loopVars_;
                ctx.timerStarts = &timerStarts_;
                ctx.curLoops = curLoops_;
                return ctx;
            };

            AiSessionStore aiSessions;
            AiStepBudgetState aiRootBudget{};
            AiStepFrame* aiCurFrame = nullptr;
            const ScriptAction* aiInheritParent = nullptr;

            std::function<void(const ScriptAction&, const ScriptAction*)> runAiActionExecute;
            auto executeOne = std::function<void(const ScriptAction&)>();

            runAiActionExecute = [this, &usesOcr, &holdOcrSession, &heldKeyVk, &runRange, &runningScriptPath,
                &activeActions, &lockedScreen_, &lockedVirtX_, &lockedVirtY_, &clearLockedScreen, &makeVarCtx,
                &executeOne, &runAiActionExecute, &aiSessions, &aiRootBudget, &aiCurFrame, &aiInheritParent](
                const ScriptAction& action, const ScriptAction* inheritFrom) {
                if (stopFlag_) return;
                ScriptAction eff = inheritFrom ? InheritAiActionFields(action, *inheritFrom) : action;
                aiInheritParent = &eff;

                AiStepFrame childFrame{};
                childFrame.localMax = eff.aiMaxSteps;
                if (aiCurFrame && aiCurFrame->shared) {
                    childFrame.shared = aiCurFrame->shared;
                } else {
                    aiRootBudget = AiStepBudgetState{};
                    aiRootBudget.maxSteps = eff.aiMaxSteps;
                    childFrame.shared = &aiRootBudget;
                }
                AiStepFrame* prevFrame = aiCurFrame;
                aiCurFrame = &childFrame;

                if (!inheritFrom && eff.aiContextMode == 0) {
                    aiSessions.ephemeralSession.reset();
                }

                auto resolveAiRegion = [&](int& x1, int& y1, int& x2, int& y2) -> bool {
                    if (eff.aiRegionByImage && !eff.aiTargetImagePath.empty()) {
                        int sx = 0, sy = 0, rsw = 0, rsh = 0;
                        GetVirtualScreenRect(sx, sy, rsw, rsh);
                        HBITMAP tmpl = LoadBitmapFromFile(eff.aiTargetImagePath);
                        if (!tmpl) return false;
                        ImageMatchOptions opt;
                        opt.thresholdPercent = eff.matchThreshold;
                        opt.scaleMin = eff.imageScaleMin > 0.0 ? eff.imageScaleMin : eff.imageScale;
                        opt.scaleMax = eff.imageScaleMax > 0.0 ? eff.imageScaleMax : eff.imageScale;
                        opt.scaleStep = 0.05;
                        opt.maxMatches = 20;
                        opt.maxOverlap = 0.5;
                        ImageMatchOutput output;
                        if (lockedScreen_) {
                            output = FindTemplateInFrozenScreenMulti(
                                lockedScreen_, lockedVirtX_, lockedVirtY_, sx, sy, sx + rsw, sy + rsh, tmpl, opt);
                        } else {
                            output = FindTemplateOnScreenMulti(sx, sy, sx + rsw, sy + rsh, tmpl, opt);
                        }
                        DeleteBitmapHandle(tmpl);
                        if (output.matches.empty()) return false;
                        const ImageMatchResult& match = output.matches.front();
                        if (eff.aiSearchX2 > eff.aiSearchX1 && eff.aiSearchY2 > eff.aiSearchY1) {
                            x1 = match.topLeftX + eff.aiSearchX1;
                            y1 = match.topLeftY + eff.aiSearchY1;
                            x2 = match.topLeftX + eff.aiSearchX2;
                            y2 = match.topLeftY + eff.aiSearchY2;
                        } else {
                            x1 = match.topLeftX;
                            y1 = match.topLeftY;
                            x2 = match.bottomRightX;
                            y2 = match.bottomRightY;
                        }
                        return x2 > x1 && y2 > y1;
                    }
                    if (eff.aiSearchX2 > eff.aiSearchX1 && eff.aiSearchY2 > eff.aiSearchY1) {
                        x1 = eff.aiSearchX1; y1 = eff.aiSearchY1; x2 = eff.aiSearchX2; y2 = eff.aiSearchY2;
                        return true;
                    }
                    int sx = 0, sy = 0, vw = 0, vh = 0;
                    GetVirtualScreenRect(sx, sy, vw, vh);
                    x1 = sx; y1 = sy; x2 = sx + vw; y2 = sy + vh;
                    return true;
                };

                MacroVariableContext ctx = makeVarCtx();
                const std::wstring resolvedPrompt = ResolveMacroVariables(eff.aiPrompt, ctx);
                const std::wstring effModel = EffectiveAiModelName(eff);
                ScriptAction prepAction = eff;
                prepAction.aiModelName = effModel;

                auto handleAiActionApiResult = [&](const AiActionResult& ar) {
                    if (stopFlag_) return;
                    if (!ar.ok) {
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：API 调用失败"
                            + (ar.errorMessage.empty() ? L"" : L" (" + ar.errorMessage + L")"));
                        return;
                    }
                    if (ar.visionQueryText) {
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]："
                            + AiActionRouteLabel(ar.routeKind) + L" → "
                            + Trim(ar.textResult));
                        return;
                    }
                    std::wstring jsonStr = Trim(ar.textResult);
                    const size_t bracketStart = jsonStr.find(L'[');
                    const size_t bracketEnd = jsonStr.rfind(L']');
                    if (bracketStart != std::wstring::npos && bracketEnd != std::wstring::npos && bracketEnd > bracketStart) {
                        jsonStr = jsonStr.substr(bracketStart, bracketEnd - bracketStart + 1);
                    }
                    std::wstring preview = jsonStr;
                    if (preview.size() > 240) preview = preview.substr(0, 240) + L"...";
                    AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：收到动作序列 → " + preview);
                    try {
                        nlohmann::json steps = nlohmann::json::parse(ToUtf8(jsonStr));
                        if (!steps.is_array()) {
                            AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：返回内容不是 JSON 数组");
                            return;
                        }
                        bool shouldExecute = true;
                        if (eff.aiConfirmExecute) {
                            std::wstring confirmPreview = jsonStr;
                            if (confirmPreview.size() > 1200) confirmPreview = confirmPreview.substr(0, 1200) + L"\n...";
                            const int confirm = MessageBoxW(hwnd_,
                                (L"AI 已生成以下动作序列，是否执行？\n\n" + confirmPreview).c_str(),
                                L"确认执行 AI 动作",
                                MB_OKCANCEL | MB_ICONQUESTION | MB_TOPMOST);
                            shouldExecute = confirm == IDOK;
                            if (!shouldExecute) {
                                AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：用户取消执行");
                            }
                        }
                        int stepCount = 0;
                        for (const auto& step : steps) {
                            if (stopFlag_ || !shouldExecute) break;
                            if (!ConsumeAiStep(childFrame)) {
                                AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：步骤预算已用尽（共享 "
                                    + std::to_wstring(childFrame.shared ? childFrame.shared->usedSteps : 0)
                                    + L"/" + (childFrame.shared && childFrame.shared->maxSteps >= 0
                                        ? std::to_wstring(childFrame.shared->maxSteps) : L"∞")
                                    + L"，本地 " + std::to_wstring(childFrame.localUsed)
                                    + L"/" + (childFrame.localMax >= 0 ? std::to_wstring(childFrame.localMax) : L"∞")
                                    + L"）");
                                break;
                            }
                            if (!step.is_object()) continue;

                            nlohmann::json params;
                            std::wstring actionType;
                            if (step.contains("action")) {
                                actionType = FromUtf8(step["action"].get<std::string>());
                                if (actionType == L"mouseMove") actionType = L"moveMouse";
                                params = step.value("params", nlohmann::json::object());
                                if (!params.is_object()) params = nlohmann::json::object();
                                params["type"] = ToUtf8(actionType);
                            } else if (step.contains("type")) {
                                params = step;
                                actionType = FromUtf8(step["type"].get<std::string>());
                            } else {
                                continue;
                            }

                            auto built = BuildScriptActionFromJson(params);
                            if (!built.ok) {
                                AppendAiDebugLog(L"  跳过无效动作：" + built.error);
                                continue;
                            }
                            ScriptAction stepAction = InheritAiActionFields(built.action, eff);
                            AppendAiDebugLog(L"  执行 " + ActionName(stepAction)
                                + L" (步" + std::to_wstring(stepCount + 1) + L")");
                            if (stepAction.type == ActionType::AiActionExecute) {
                                runAiActionExecute(stepAction, &eff);
                            } else {
                                executeOne(stepAction);
                            }
                            ++stepCount;
                        }
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：完成，共执行 "
                            + std::to_wstring(stepCount) + L" 步");
                    } catch (...) {
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：JSON 解析失败");
                    }
                };

                if (effModel.empty() || !appSettings_.ai.enabled) {
                    AppendAiDebugLog(L"AI动作执行：未配置模型或 AI 未启用");
                    aiCurFrame = prevFrame;
                    return;
                }

                AiMacroLogFn logFn = [this](const std::wstring& line) { AppendAiDebugLog(line); };

                if (!eff.aiWithImage) {
                    AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：发送 prompt…");
                    try {
                        const AiActionRouteKind route = ClassifyAiActionRoute(resolvedPrompt, false);
                        const int timeoutMs = ResolveAiActionExecuteTimeoutSec(
                            eff.aiTimeoutSec, false) * 1000;
                        AgentCore* corePtr = nullptr;
                        std::unique_ptr<AgentCore> ownedCore = PrepareAiActionExecuteCore(
                            &aiSessions, prepAction, route, 0, 0, appSettings_, timeoutMs, corePtr);
                        AgentCore* core = corePtr ? corePtr : ownedCore.get();
                        if (!core) {
                            AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：无法创建 AI 客户端");
                        } else {
                            handleAiActionApiResult(ExecuteAiActionExecute(
                                core, resolvedPrompt, "", 0, 0, eff.aiContextMode,
                                stopFlag_, eff.aiTimeoutSec, logFn, &aiHttpAbort_, nullptr));
                        }
                    } catch (...) {
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：执行异常");
                    }
                } else {
                    int capX1 = 0, capY1 = 0, capX2 = 0, capY2 = 0;
                    if (!resolveAiRegion(capX1, capY1, capX2, capY2)) {
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：无法定位分析区域");
                    } else {
                        HBITMAP screenBmp = CaptureScreenRegion(capX1, capY1, capX2, capY2);
                        int sw = 0, sh = 0;
                        if (screenBmp) {
                            BITMAP bm{};
                            if (GetObject(screenBmp, sizeof(bm), &bm)) { sw = bm.bmWidth; sh = bm.bmHeight; }
                        }
                        if (!screenBmp || sw <= 0 || sh <= 0) {
                            AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：截屏失败");
                            if (screenBmp) DeleteBitmapHandle(screenBmp);
                        } else {
                            AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：截屏完成("
                                + std::to_wstring(sw) + L"×" + std::to_wstring(sh) + L")，发送中…");
                            const double scale = std::clamp(
                                eff.aiImageScale > 0.0 ? eff.aiImageScale : 0.5, 0.1, 1.0);
                            const AiImageEncodeResult enc = EncodeBitmapForAiAnalysis(screenBmp, scale);
                            DeleteBitmapHandle(screenBmp);
                            if (enc.base64.empty()) {
                                AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：图片编码失败");
                            } else {
                                AppendAiDebugLog(L"  图片 " + std::to_wstring(enc.srcWidth) + L"×"
                                    + std::to_wstring(enc.srcHeight) + L" → 上传 "
                                    + std::to_wstring(enc.outWidth) + L"×" + std::to_wstring(enc.outHeight)
                                    + L"（" + std::to_wstring(enc.base64.size()) + L" 字节 base64）");
                                const std::string& screenshotB64 = enc.base64;
                                try {
                                    const AiActionRouteKind route = ClassifyAiActionRoute(resolvedPrompt, true);
                                    const int effectiveTimeoutSec = ResolveAiActionExecuteTimeoutSec(
                                        eff.aiTimeoutSec, true);
                                    const int timeoutMs = effectiveTimeoutSec * 1000;
                                    const int apiW = enc.outWidth;
                                    const int apiH = enc.outHeight;
                                    AiCaptureMapping capMap{};
                                    capMap.capX1 = capX1;
                                    capMap.capY1 = capY1;
                                    capMap.capX2 = capX2;
                                    capMap.capY2 = capY2;
                                    capMap.srcWidth = enc.srcWidth;
                                    capMap.srcHeight = enc.srcHeight;
                                    capMap.apiWidth = apiW;
                                    capMap.apiHeight = apiH;
                                    AgentCore* corePtr = nullptr;
                                    std::unique_ptr<AgentCore> ownedCore = PrepareAiActionExecuteCore(
                                        &aiSessions, prepAction, route, apiW, apiH,
                                        appSettings_, timeoutMs, corePtr);
                                    AgentCore* core = corePtr ? corePtr : ownedCore.get();
                                    if (!core) {
                                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：无法创建 AI 客户端");
                                    } else {
                                        handleAiActionApiResult(ExecuteAiActionExecute(
                                            core, resolvedPrompt, screenshotB64,
                                            apiW, apiH, eff.aiContextMode,
                                            stopFlag_, eff.aiTimeoutSec, logFn, &aiHttpAbort_, &capMap));
                                    }
                                } catch (...) {
                                    AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：执行异常");
                                }
                            }
                        }
                    }
                }

                aiCurFrame = prevFrame;
                if (!inheritFrom && eff.aiContextMode == 0) {
                    aiSessions.ephemeralSession.reset();
                }
            };

            executeOne = [this, &usesOcr, &holdOcrSession, &heldKeyVk, &runRange, &runningScriptPath, &activeActions, &lockedScreen_, &lockedVirtX_, &lockedVirtY_, &clearLockedScreen, &makeVarCtx, &executeOne, &runAiActionExecute](const ScriptAction& a) {
                if (appSettings_.playback.autoOutputKeyFunctionDebug
                    && a.type != ActionType::MoveMouse
                    && a.type != ActionType::FindImage
                    && a.type != ActionType::TextRecognition) {
                    AppendDebugLog(FormatGenericActionDebug(a));
                }
                if (a.type == ActionType::MoveMouse) {
                    int x = a.x;
                    int y = a.y;
                    if (a.moveFromVar) {
                        MacroVariableContext ctx = makeVarCtx();
                        if (!TryResolveIntOperand(a.moveVarExprX, ctx, x)) x = 0;
                        if (!TryResolveIntOperand(a.moveVarExprY, ctx, y)) y = 0;
                    }
                    if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                        AppendDebugLog(FormatMoveMouseDebug(a, x, y));
                    }
                    SetCursorPos(x + RandomInt(a.randomX), y + RandomInt(a.randomY));
                }
                else if (a.type == ActionType::Wait) {
                    const double totalSec = a.duration + RandomDelay(a.randomDuration);
                    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(totalSec * 1000.0));
                    while (!stopFlag_ && std::chrono::steady_clock::now() < end) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                }
                else if (a.type == ActionType::MouseDown) { MarkSimulatedInput(); SendHeldModifiers(a, true); MouseButtonEvent(a.button, true); UnmarkSimulatedInput(); }
                else if (a.type == ActionType::MouseUp) { MarkSimulatedInput(); MouseButtonEvent(a.button, false); SendHeldModifiers(a, false); UnmarkSimulatedInput(); }
                else if (a.type == ActionType::MouseClick) for (int i = 0; i < a.clickCount && !stopFlag_; ++i) {
                    SleepInterruptible(a.duration + RandomDelay(a.randomDuration));
                    if (!stopFlag_) {
                        if (a.x != 0 || a.y != 0) {
                            SetCursorPos(a.x + RandomInt(a.randomX), a.y + RandomInt(a.randomY));
                        }
                        MarkSimulatedInput();
                        SendHeldModifiers(a, true);
                        MouseClick(a.button);
                        SendHeldModifiers(a, false);
                        UnmarkSimulatedInput();
                    }
                }
                else if (a.type == ActionType::KeyDown) {
                    MarkSimulatedInput();
                    SendHeldModifiers(a, true);
                    SendKey(a.keyVk, true);
                    heldKeyVk = a.keyVk;
                    UnmarkSimulatedInput();
                }
                else if (a.type == ActionType::KeyUp) {
                    MarkSimulatedInput();
                    SendKey(a.keyVk, false);
                    if (heldKeyVk == a.keyVk) heldKeyVk = 0;
                    SendHeldModifiers(a, false);
                    UnmarkSimulatedInput();
                }
                else if (a.type == ActionType::KeyClick) for (int i = 0; i < a.clickCount && !stopFlag_; ++i) { SleepInterruptible(a.duration + RandomDelay(a.randomDuration)); if (!stopFlag_) { MarkSimulatedInput(); SendHeldModifiers(a, true); SendKey(a.keyVk, true); SendKey(a.keyVk, false); SendHeldModifiers(a, false); UnmarkSimulatedInput(); } }
                else if (a.type == ActionType::HotkeyShortcut) for (int i = 0; i < a.clickCount && !stopFlag_; ++i) { SleepInterruptible(a.duration + RandomDelay(a.randomDuration)); if (!stopFlag_) { MarkSimulatedInput(); SendShortcutCombo(a); UnmarkSimulatedInput(); } }
                else if (a.type == ActionType::QuickInput) for (int i = 0; i < a.clickCount && !stopFlag_; ++i) { SleepInterruptible(a.duration + RandomDelay(a.randomDuration)); if (!stopFlag_) { MarkSimulatedInput(); SendQuickInputText(ResolveQuickInputText(a.inputText), a.charInterval); UnmarkSimulatedInput(); } }
                else if (a.type == ActionType::ScrollWheel) for (int i = 0; i < a.clickCount && !stopFlag_; ++i) {
                    SleepInterruptible(a.duration + RandomDelay(a.randomDuration));
                    if (!stopFlag_) {
                        MarkSimulatedInput();
                        const bool positive = a.scrollDirection == 0;
                        if (a.scrollVertical) SendMouseWheel(a.scrollSteps, true, false, positive);
                        if (a.scrollHorizontal) SendMouseWheel(a.scrollSteps, false, true, positive);
                        UnmarkSimulatedInput();
                    }
                }
                else if (a.type == ActionType::FindImage) {
                    auto runFind = [&]() -> ImageMatchResult {
                        int x1 = a.searchX1, y1 = a.searchY1, x2 = a.searchX2, y2 = a.searchY2;
                        if (a.searchFullScreen) {
                            int sx = 0, sy = 0, sw = 0, sh = 0;
                            GetVirtualScreenRect(sx, sy, sw, sh);
                            x1 = sx; y1 = sy; x2 = sx + sw; y2 = sy + sh;
                        }
                        HBITMAP tmpl = LoadBitmapFromFile(a.imagePath);
                        if (!tmpl) return {};
                        ImageMatchOptions opt;
                        opt.thresholdPercent = a.matchThreshold;
                        opt.scaleMin = a.imageScaleMin > 0.0 ? a.imageScaleMin : a.imageScale;
                        opt.scaleMax = a.imageScaleMax > 0.0 ? a.imageScaleMax : opt.scaleMin;
                        opt.scaleStep = 0.05;
                        opt.maxMatches = 20;
                        opt.maxOverlap = 0.5;
                        ImageMatchOutput output;
                        if (lockedScreen_) {
                            output = FindTemplateInFrozenScreenMulti(
                                lockedScreen_, lockedVirtX_, lockedVirtY_, x1, y1, x2, y2, tmpl, opt);
                        } else {
                            output = FindTemplateOnScreenMulti(x1, y1, x2, y2, tmpl, opt);
                        }
                        DeleteBitmapHandle(tmpl);
                        if (output.matches.empty()) return {};
                        return output.matches.front();
                    };
                    ImageMatchResult lastRawMatch{};
                    do {
                        const ImageMatchResult rawMatch = runFind();
                        lastRawMatch = rawMatch;
                        const ImageMatchResult match = NormalizeMatchVarResult(rawMatch, a.matchThreshold);
                        if (a.findImageFollowUp == 2) {
                            const std::wstring varName = a.matchVarName.empty() ? L"matchRet" : a.matchVarName;
                            matchVars_[varName] = match;
                            break;
                        } else if (match.found) {
                            const int tx = match.x + a.offsetX;
                            const int ty = match.y + a.offsetY;
                            const std::wstring varName = a.matchVarName.empty() ? L"matchRet" : a.matchVarName;
                            matchVars_[varName] = match;
                            if (a.findImageFollowUp == 0) {
                                SetCursorPos(tx, ty);
                                MarkSimulatedInput();
                                MouseClick(MouseButtonType::Left);
                                UnmarkSimulatedInput();
                            } else if (a.findImageFollowUp == 1) {
                                SetCursorPos(tx, ty);
                            }
                            break;
                        } else if (!a.findUntilFound) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    } while (!stopFlag_);
                    if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                        AppendDebugLog(FormatFindImageDebug(a, lastRawMatch));
                    }
                }
                else if (a.type == ActionType::TextRecognition) {
                    usesOcr = true;
                    workerUsesOcrVars_ = true;
                    holdOcrSession();
                    const std::wstring varName = a.matchVarName.empty() ? L"a" : a.matchVarName;
                    auto resolveOcrRegion = [&](int& x1, int& y1, int& x2, int& y2) -> bool {
                        if (a.ocrRegionByImage) {
                            int sx = 0, sy = 0, sw = 0, sh = 0;
                            GetVirtualScreenRect(sx, sy, sw, sh);
                            HBITMAP tmpl = LoadBitmapFromFile(a.imagePath);
                            if (!tmpl) return false;
                            ImageMatchOptions opt;
                            opt.thresholdPercent = a.matchThreshold;
                            opt.scaleMin = a.imageScaleMin > 0.0 ? a.imageScaleMin : a.imageScale;
                            opt.scaleMax = a.imageScaleMax > 0.0 ? a.imageScaleMax : opt.scaleMin;
                            opt.scaleStep = 0.05;
                            opt.maxMatches = 20;
                            opt.maxOverlap = 0.5;
                            ImageMatchOutput output;
                            if (lockedScreen_) {
                                output = FindTemplateInFrozenScreenMulti(
                                    lockedScreen_, lockedVirtX_, lockedVirtY_, sx, sy, sx + sw, sy + sh, tmpl, opt);
                            } else {
                                output = FindTemplateOnScreenMulti(sx, sy, sx + sw, sy + sh, tmpl, opt);
                            }
                            DeleteBitmapHandle(tmpl);
                            if (output.matches.empty()) return false;
                            const ImageMatchResult& match = output.matches.front();
                            const int relX1 = a.searchX1, relY1 = a.searchY1;
                            const int relX2 = a.searchX2, relY2 = a.searchY2;
                            x1 = match.topLeftX + relX1;
                            y1 = match.topLeftY + relY1;
                            x2 = match.topLeftX + relX2;
                            y2 = match.topLeftY + relY2;
                            return true;
                        }
                        if (a.searchFullScreen) {
                            int sx = 0, sy = 0, sw = 0, sh = 0;
                            GetVirtualScreenRect(sx, sy, sw, sh);
                            x1 = sx; y1 = sy; x2 = sx + sw; y2 = sy + sh;
                        }
                        return true;
                    };
                    auto runOcrAction = [&]() -> OcrVarResult {
                        int x1 = a.searchX1, y1 = a.searchY1, x2 = a.searchX2, y2 = a.searchY2;
                        if (!resolveOcrRegion(x1, y1, x2, y2)) {
                            return a.ocrResultMode == 1
                                ? MakeOcrSearchVarResult(OcrTextLine{}, false)
                                : MakeOcrTextVarResult(L"");
                        }
                        const OcrEngineOutput output = RunOcrOnScreenRegion(
                            x1, y1, x2, y2, lockedScreen_, lockedVirtX_, lockedVirtY_, a.ocrDigitsOnly);
                        if (!output.success) {
                            return a.ocrResultMode == 1
                                ? MakeOcrSearchVarResult(OcrTextLine{}, false)
                                : MakeOcrTextVarResult(L"");
                        }
                        if (a.ocrResultMode == 0) {
                            return MakeOcrTextVarResult(ConcatOcrLines(output));
                        }
                        MacroVariableContext ctx = makeVarCtx();
                        const std::wstring target = ResolveMacroVariables(a.ocrSearchText, ctx);
                        const auto found = FindTextInOcrLines(output, target);
                        if (found.has_value()) return MakeOcrSearchVarResult(*found, true);
                        return MakeOcrSearchVarResult(OcrTextLine{}, false);
                    };
                    auto applyFollowUpAt = [&](int centerX, int centerY) {
                        if (a.ocrFollowUp == 2) return;
                        const int tx = centerX + a.offsetX;
                        const int ty = centerY + a.offsetY;
                        if (a.ocrFollowUp == 0) {
                            SetCursorPos(tx, ty);
                            MarkSimulatedInput();
                            MouseClick(MouseButtonType::Left);
                            UnmarkSimulatedInput();
                        } else if (a.ocrFollowUp == 1) {
                            SetCursorPos(tx, ty);
                        }
                    };
                    auto emitOcrDebug = [&](const std::wstring& textContent, bool searchFound) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatOcrDebug(a, textContent, searchFound, makeVarCtx()));
                        }
                    };
                    if (a.ocrResultMode == 0 && a.ocrFollowUp != 2) {
                        int x1 = a.searchX1, y1 = a.searchY1, x2 = a.searchX2, y2 = a.searchY2;
                        if (!resolveOcrRegion(x1, y1, x2, y2)) {
                            ocrVars_[varName] = MakeOcrTextVarResult(L"");
                            emitOcrDebug(L"", false);
                            return;
                        }
                        const OcrEngineOutput output = RunOcrOnScreenRegion(
                            x1, y1, x2, y2, lockedScreen_, lockedVirtX_, lockedVirtY_, a.ocrDigitsOnly);
                        const std::wstring text = output.success ? ConcatOcrLines(output) : L"";
                        ocrVars_[varName] = MakeOcrTextVarResult(text);
                        emitOcrDebug(text, false);
                        if (output.success && !output.lines.empty()) {
                            int minX = output.lines.front().x1, minY = output.lines.front().y1;
                            int maxX = output.lines.front().x2, maxY = output.lines.front().y2;
                            for (const auto& line : output.lines) {
                                minX = std::min(minX, line.x1);
                                minY = std::min(minY, line.y1);
                                maxX = std::max(maxX, line.x2);
                                maxY = std::max(maxY, line.y2);
                            }
                            applyFollowUpAt((minX + maxX) / 2, (minY + maxY) / 2);
                        }
                    } else if (a.ocrResultMode == 0) {
                        const OcrVarResult result = runOcrAction();
                        ocrVars_[varName] = result;
                        emitOcrDebug(result.text, false);
                    } else {
                        OcrVarResult lastResult{};
                        do {
                            const OcrVarResult result = runOcrAction();
                            lastResult = result;
                            ocrVars_[varName] = result;
                            if (result.found && a.ocrFollowUp != 2) {
                                const int centerX = (result.topLeftX + result.bottomRightX) / 2;
                                const int centerY = (result.topLeftY + result.bottomRightY) / 2;
                                applyFollowUpAt(centerX, centerY);
                                break;
                            }
                            if (result.found || !a.findUntilFound) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        } while (!stopFlag_);
                        emitOcrDebug(lastResult.text, lastResult.found != 0);
                    }
                }
                else if (a.type == ActionType::RunMacro) {
                    std::wstring path = a.targetPath;
                    if (path.empty() || path == runningScriptPath) return;
                    std::vector<ScriptAction> nested = ParseActionsFromFile(path);
                    if (nested.empty()) return;
                    if (!usesOcr && ScriptUsesTextRecognition(nested)) {
                        usesOcr = true;
                        workerUsesOcrVars_ = true;
                    }
                    if (usesOcr) holdOcrSession();
                    const std::vector<ScriptAction>* prevActions = activeActions;
                    const std::wstring prevPath = runningScriptPath;
                    activeActions = &nested;
                    runningScriptPath = path;
                    runRange(0, nested.size());
                    activeActions = prevActions;
                    runningScriptPath = prevPath;
                }
                else if (a.type == ActionType::LockScreenshot) {
                    clearLockedScreen();
                    lockedScreen_ = CaptureVirtualScreen(lockedVirtX_, lockedVirtY_);
                }
                else if (a.type == ActionType::UnlockScreenshot) {
                    clearLockedScreen();
                }
                else if (a.type == ActionType::StopMacro) {
                    stopFlag_ = true;
                }
                else if (a.type == ActionType::RunProgram) {
                    const std::wstring path = ResolveRunProgramPath(a.shortcutPreset, a.targetPath);
                    if (!path.empty()) LaunchProgram(path, a.inputText);
                }
                else if (a.type == ActionType::CloseProgram) {
                    if (!a.targetPath.empty()) CloseProgramsByTarget(a.targetPath, a.matchFileNameOnly);
                }
                else if (a.type == ActionType::OpenWebpage) {
                    if (!a.targetPath.empty()) LaunchProgram(a.targetPath, L"");
                }
                else if (a.type == ActionType::OpenFile) {
                    if (!a.targetPath.empty()) LaunchProgram(a.targetPath, L"");
                }
                else if (a.type == ActionType::TimerRecordTime) {
                    if (!a.loopVarName.empty()) {
                        timerStarts_[a.loopVarName] = std::chrono::steady_clock::now();
                    }
                }
                else if (a.type == ActionType::MousePlayback) for (int i = 0; i < a.clickCount && !stopFlag_; ++i) {
                    SleepInterruptible(a.duration + RandomDelay(a.randomDuration));
                    if (stopFlag_) break;
                    std::wstring path = a.targetPath;
                    if (path.empty()) break;
                    std::vector<ScriptAction> nested = ParseActionsFromFile(path);
                    if (nested.empty()) break;
                    const std::vector<ScriptAction>* prevActions = activeActions;
                    const std::wstring prevPath = runningScriptPath;
                    activeActions = &nested;
                    runningScriptPath = path;
                    runRange(0, nested.size());
                    activeActions = prevActions;
                    runningScriptPath = prevPath;
                }
                else if (a.type == ActionType::GetCursorPos) {
                    POINT pt{};
                    if (GetCursorPos(&pt)) {
                        const std::wstring varName = a.matchVarName.empty() ? L"cursor" : a.matchVarName;
                        ImageMatchResult match{};
                        match.found = true;
                        match.topLeftX = pt.x;
                        match.topLeftY = pt.y;
                        match.bottomRightX = pt.x;
                        match.bottomRightY = pt.y;
                        match.x = pt.x;
                        match.y = pt.y;
                        match.score = 100.0;
                        matchVars_[varName] = match;
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(L"获取当前光标位置→[" + varName + L"] "
                                + std::to_wstring(pt.x) + L"," + std::to_wstring(pt.y));
                        }
                    }
                }
                else if (a.type == ActionType::AiTextAnalysis) {
                    if (stopFlag_) return;
                    MacroVariableContext ctx = makeVarCtx();
                    const std::wstring resolvedPrompt = ResolveMacroVariables(a.aiPrompt, ctx);
                    const std::wstring outputVarName = a.aiOutputVarName.empty() ? L"aiResult" : a.aiOutputVarName;
                    const std::wstring fallback = a.aiFallbackValue.empty()
                        ? (a.aiOutputType == 1 ? L"0" : L"") : a.aiFallbackValue;
                    const std::wstring modelLabel = EffectiveAiModelName(a);
                    AppendAiDebugLog(L"AI文字分析 [" + modelLabel + L"]：发送 prompt…");
                    try {
                        const AiActionResult ar = RunAiTextAnalysisForAction(a, resolvedPrompt);
                        if (ar.ok) {
                            StoreAiOutputVar(outputVarName, a.aiOutputType, ar.textResult, fallback);
                            AppendAiDebugLog(L"AI文字分析 [" + modelLabel + L"]：完成 → "
                                + outputVarName + L" = " + aiVars_[outputVarName]);
                        } else {
                            StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                            AppendAiDebugLog(L"AI文字分析 [" + modelLabel + L"]：失败，使用降级值："
                                + fallback + (ar.errorMessage.empty() ? L"" : L" (" + ar.errorMessage + L")"));
                        }
                    } catch (...) {
                        StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                        AppendAiDebugLog(L"AI文字分析 [" + modelLabel + L"]：执行异常，使用降级值：" + fallback);
                    }
                }
                else if (a.type == ActionType::AiImageAnalysis) {
                    if (stopFlag_) return;
                    MacroVariableContext ctx = makeVarCtx();
                    const std::wstring resolvedPrompt = ResolveMacroVariables(a.aiPrompt, ctx);
                    const std::wstring outputVarName = a.aiOutputVarName.empty() ? L"aiImgResult" : a.aiOutputVarName;
                    const std::wstring fallback = a.aiFallbackValue.empty()
                        ? (a.aiOutputType == 1 ? L"0" : L"") : a.aiFallbackValue;
                    const std::wstring modelLabel = EffectiveAiModelName(a);
                    const double scale = std::clamp(a.aiImageScale, 0.1, 1.0);
                    HBITMAP screenBmp = nullptr;
                    int sw = 0, sh = 0;
                    int capX1 = 0, capY1 = 0, capX2 = 0, capY2 = 0;
                    auto resolveAiRegion = [&](int& x1, int& y1, int& x2, int& y2) -> bool {
                        if (a.aiRegionByImage && !a.aiTargetImagePath.empty()) {
                            int sx = 0, sy = 0, sw = 0, sh = 0;
                            GetVirtualScreenRect(sx, sy, sw, sh);
                            HBITMAP tmpl = LoadBitmapFromFile(a.aiTargetImagePath);
                            if (!tmpl) return false;
                            ImageMatchOptions opt;
                            opt.thresholdPercent = a.matchThreshold;
                            opt.scaleMin = a.imageScaleMin > 0.0 ? a.imageScaleMin : a.imageScale;
                            opt.scaleMax = a.imageScaleMax > 0.0 ? a.imageScaleMax : opt.scaleMin;
                            opt.scaleStep = 0.05;
                            opt.maxMatches = 20;
                            opt.maxOverlap = 0.5;
                            ImageMatchOutput output;
                            if (lockedScreen_) {
                                output = FindTemplateInFrozenScreenMulti(
                                    lockedScreen_, lockedVirtX_, lockedVirtY_, sx, sy, sx + sw, sy + sh, tmpl, opt);
                            } else {
                                output = FindTemplateOnScreenMulti(sx, sy, sx + sw, sy + sh, tmpl, opt);
                            }
                            DeleteBitmapHandle(tmpl);
                            if (output.matches.empty()) return false;
                            const ImageMatchResult& match = output.matches.front();
                            if (a.aiSearchX2 > a.aiSearchX1 && a.aiSearchY2 > a.aiSearchY1) {
                                x1 = match.topLeftX + a.aiSearchX1;
                                y1 = match.topLeftY + a.aiSearchY1;
                                x2 = match.topLeftX + a.aiSearchX2;
                                y2 = match.topLeftY + a.aiSearchY2;
                            } else {
                                x1 = match.topLeftX;
                                y1 = match.topLeftY;
                                x2 = match.bottomRightX;
                                y2 = match.bottomRightY;
                            }
                            return x2 > x1 && y2 > y1;
                        }
                        if (a.aiSearchX2 > a.aiSearchX1 && a.aiSearchY2 > a.aiSearchY1) {
                            x1 = a.aiSearchX1; y1 = a.aiSearchY1; x2 = a.aiSearchX2; y2 = a.aiSearchY2;
                            return true;
                        }
                        int sx = 0, sy = 0, vw = 0, vh = 0;
                        GetVirtualScreenRect(sx, sy, vw, vh);
                        x1 = sx; y1 = sy; x2 = sx + vw; y2 = sy + vh;
                        return true;
                    };
                    if (!resolveAiRegion(capX1, capY1, capX2, capY2)) {
                        StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                        AppendAiDebugLog(L"AI图片分析 [" + modelLabel + L"]：无法定位分析区域，使用降级值：" + fallback);
                    } else {
                        screenBmp = CaptureScreenRegion(capX1, capY1, capX2, capY2);
                        if (screenBmp) {
                            BITMAP bm{};
                            if (GetObject(screenBmp, sizeof(bm), &bm)) { sw = bm.bmWidth; sh = bm.bmHeight; }
                        }
                        if (!screenBmp || sw <= 0 || sh <= 0) {
                            StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                            AppendAiDebugLog(L"AI图片分析 [" + modelLabel + L"]：截屏失败，使用降级值：" + fallback);
                        } else {
                            const AiImageEncodeResult encoded = EncodeBitmapForAiAnalysis(screenBmp, scale);
                            DeleteBitmapHandle(screenBmp);
                            std::wstring captureInfo = L"AI图片分析 [" + modelLabel + L"]：截屏完成("
                                + std::to_wstring(encoded.srcWidth) + L"×" + std::to_wstring(encoded.srcHeight);
                            if (encoded.effectiveScale < 0.999
                                || encoded.outWidth != encoded.srcWidth
                                || encoded.outHeight != encoded.srcHeight) {
                                captureInfo += L"→" + std::to_wstring(encoded.outWidth)
                                    + L"×" + std::to_wstring(encoded.outHeight);
                            }
                            captureInfo += L")，发送中…";
                            AppendAiDebugLog(captureInfo);

                            if (encoded.base64.empty()) {
                                StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                                AppendAiDebugLog(L"AI图片分析 [" + modelLabel + L"]：图片编码失败，使用降级值：" + fallback);
                            } else {
                                AppendAiDebugLog(L"  图片数据 " + std::to_wstring(encoded.base64.size())
                                    + L" 字节(base64)，调用 API…");
                                try {
                                    const AiActionResult ar = RunAiImageAnalysisForAction(
                                        a, resolvedPrompt, encoded.base64);
                                    if (ar.ok) {
                                        StoreAiOutputVar(outputVarName, a.aiOutputType, ar.textResult, fallback);
                                        AppendAiDebugLog(L"AI图片分析 [" + modelLabel + L"]：完成 → "
                                            + outputVarName + L" = " + aiVars_[outputVarName]);
                                    } else {
                                        StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                                        AppendAiDebugLog(L"AI图片分析 [" + modelLabel + L"]：失败，使用降级值："
                                            + fallback + (ar.errorMessage.empty() ? L"" : L" (" + ar.errorMessage + L")"));
                                    }
                                } catch (...) {
                                    StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                                    AppendAiDebugLog(L"AI图片分析 [" + modelLabel + L"]：执行异常，使用降级值：" + fallback);
                                }
                            }
                        }
                    }
                }
                else if (a.type == ActionType::AiActionExecute) {
                    runAiActionExecute(a, nullptr);
                }
            };

            runBlockByName = [&](const std::wstring& name) {
                if (stopFlag_ || name.empty()) return;
                aiSessions.ClearBlock();
                std::unordered_map<std::wstring, size_t> blockDefs;
                for (size_t i = 0; i < activeActions->size(); ++i) {
                    if ((*activeActions)[i].type == ActionType::DefineBlock && !(*activeActions)[i].blockName.empty()) {
                        blockDefs[(*activeActions)[i].blockName] = i;
                    }
                }
                const auto it = blockDefs.find(name);
                if (it == blockDefs.end()) return;
                if (blockCallStack.count(name)) return;
                blockCallStack.insert(name);
                runRange(it->second + 1, containerBodyEnd(it->second));
                blockCallStack.erase(name);
            };
            runRange = [&](size_t start, size_t end) -> bool {
                for (size_t i = start; i < end && !stopFlag_; ) {
                    const auto& a = (*activeActions)[i];
                    if (a.type == ActionType::EndLoop) return true;
                    if (SkipsInMainFlow(a.type)) {
                        i = containerBodyEnd(i);
                        continue;
                    }
                    if (a.type == ActionType::Loop) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatGenericActionDebug(a));
                        }
                        const size_t bodyEnd = containerBodyEnd(i);
                        int iter = 1;
                        bool broke = false;
                        const auto loopStartTime = std::chrono::steady_clock::now();
                        while (!stopFlag_ && !broke) {
                            aiSessions.ClearLoop();
                            if (!a.loopVarName.empty()) loopVars_[a.loopVarName] = iter;
                            MacroVariableContext ctx = makeVarCtx();
                            const int maxLoop = ResolveLoopMaxCount(a, ctx, loopStartTime);
                            if (!(maxLoop < 0 || iter <= maxLoop)) break;
                            broke = runRange(i + 1, bodyEnd);
                            ++iter;
                        }
                        if (!a.loopVarName.empty()) loopVars_.erase(a.loopVarName);
                        i = bodyEnd;
                    } else if (a.type == ActionType::If) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatGenericActionDebug(a));
                        }
                        MacroVariableContext ctx = makeVarCtx();
                        const bool cond = EvaluateConditionExpr(a.conditionExpr, ctx);
                        const int level = a.indent;
                        const size_t trueEnd = containerBodyEnd(i);
                        int elseIdx = -1;
                        for (size_t j = trueEnd; j < activeActions->size(); ++j) {
                            if ((*activeActions)[j].indent < level) break;
                            if ((*activeActions)[j].indent == level && (*activeActions)[j].type == ActionType::If) break;
                            if ((*activeActions)[j].indent == level && (*activeActions)[j].type == ActionType::Else) {
                                elseIdx = static_cast<int>(j);
                                break;
                            }
                        }
                        if (cond) {
                            if (elseIdx >= 0) runRange(i + 1, static_cast<size_t>(elseIdx));
                            else runRange(i + 1, trueEnd);
                            i = elseIdx >= 0 ? containerBodyEnd(static_cast<size_t>(elseIdx)) : trueEnd;
                        } else if (elseIdx >= 0) {
                            runRange(static_cast<size_t>(elseIdx) + 1, containerBodyEnd(static_cast<size_t>(elseIdx)));
                            i = containerBodyEnd(static_cast<size_t>(elseIdx));
                        } else {
                            i = trueEnd;
                        }
                    } else if (a.type == ActionType::Else) {
                        i = containerBodyEnd(i);
                    } else if (a.type == ActionType::RunBlock) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatGenericActionDebug(a));
                        }
                        runBlockByName(a.blockName);
                        ++i;
                    } else {
                        executeOne(a);
                        ++i;
                    }
                }
                return false;
            };
            while (!stopFlag_) {
                ++curLoops_;
                aiSessions.ClearMacro();
                aiRootBudget = AiStepBudgetState{};
                aiCurFrame = nullptr;
                if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                    AppendDebugLog(FormatMacroLoopDebug(curLoops_));
                }
                matchVars_.clear();
                if (usesOcr) ocrVars_.clear();
                loopVars_.clear();
                timerStarts_.clear();
                aiVars_.clear();
                runRange(0, actions.size());
                const auto& ps = appSettings_.playback;
                if (ps.enablePlaybackCount && ps.playbackCount > 0 && curLoops_ >= ps.playbackCount) break;
                if (stopFlag_) break;
                if (ps.enablePlaybackInterval) {
                    const double span = std::max(0.0, ps.playbackIntervalMaxSeconds - ps.playbackIntervalMinSeconds);
                    const double wait = ps.playbackIntervalMinSeconds + RandomDelay(span);
                    SleepInterruptible(wait);
                }
            }
            // Final release (for clean state regardless of stop path)
            if (heldKeyVk != 0) {
                SendKey(heldKeyVk, false);
                heldKeyVk = 0;
            }
            if (stopFlag_) {
                ReleaseAllHeldInputs();
            }
            clearLockedScreen();
            if (ocrSessionHeld) ReleaseOcrSession();
            workerUsesOcrVars_ = false;
            PostMessageW(hwnd_, WM_RUN_DONE, 0, 0);
        });
    }

    void StopRun() {
        stopFlag_ = true;
        aiHttpAbort_.Abort();
    }
    void ReleaseAllHeldInputs() {
        if (running_) MarkSimulatedInput();
        const UINT modKeys[] = { VK_LWIN, VK_RWIN, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LSHIFT, VK_RSHIFT };
        for (UINT vk : modKeys) {
            if (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) SendKey(vk, false);
        }
        struct { int vk; DWORD flag; } mouseBtns[] = {
            { VK_LBUTTON, MOUSEEVENTF_LEFTUP },
            { VK_RBUTTON, MOUSEEVENTF_RIGHTUP },
            { VK_MBUTTON, MOUSEEVENTF_MIDDLEUP },
        };
        for (const auto& btn : mouseBtns) {
            if (GetAsyncKeyState(btn.vk) & 0x8000) {
                INPUT input{};
                input.type = INPUT_MOUSE;
                input.mi.dwFlags = btn.flag;
                SendInput(1, &input, sizeof(INPUT));
            }
        }
        if (running_) UnmarkSimulatedInput();
    }
    void OnRunDone() {
        running_ = false;
        if (worker_.joinable()) worker_.join();
        RemoveTray();
        HideStatusTip();
        if (appSettings_.other.autoHideMainWindow && wasVisibleBeforeRun_) {
            ShowWindow(hwnd_, wasMinimizedBeforeRun_ ? SW_MINIMIZE : SW_SHOW);
        }
    }
    void AddTray() {
        NOTIFYICONDATAW nid{}; nid.cbSize = sizeof(nid); nid.hWnd = hwnd_; nid.uID = 1;
        nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON; nid.uCallbackMessage = WM_TRAY;
        nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        if (clicking_) wcscpy_s(nid.szTip, L"鼠大侠-连点运行中");
        else if (recording_) wcscpy_s(nid.szTip, L"鼠大侠-录制中");
        else wcscpy_s(nid.szTip, L"鼠大侠-鼠标宏运行中");
        if (trayActive_) Shell_NotifyIconW(NIM_MODIFY, &nid);
        else { Shell_NotifyIconW(NIM_ADD, &nid); trayActive_ = true; }
    }
    void RemoveTray() {
        if (!trayActive_) return;
        if (hiddenToTray_) {
            NOTIFYICONDATAW nid{}; nid.cbSize = sizeof(nid); nid.hWnd = hwnd_; nid.uID = 1;
            nid.uFlags = NIF_TIP; nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
            wcscpy_s(nid.szTip, L"鼠大侠");
            Shell_NotifyIconW(NIM_MODIFY, &nid);
            return;
        }
        NOTIFYICONDATAW nid{}; nid.cbSize = sizeof(nid); nid.hWnd = hwnd_; nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        trayActive_ = false;
    }
    void RestoreFromTray() {
        if (hiddenToTray_) {
            hiddenToTray_ = false;
            NOTIFYICONDATAW nid{}; nid.cbSize = sizeof(nid); nid.hWnd = hwnd_; nid.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            trayActive_ = false;
        }
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
    }
    void HideToTray() {
        hiddenToTray_ = true;
        NOTIFYICONDATAW nid{}; nid.cbSize = sizeof(nid); nid.hWnd = hwnd_; nid.uID = 1;
        nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON; nid.uCallbackMessage = WM_TRAY;
        nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(nid.szTip, L"鼠大侠");
        Shell_NotifyIconW(NIM_ADD, &nid);
        trayActive_ = true;
        ShowWindow(hwnd_, SW_HIDE);
    }
    LRESULT OnTrayMessage(LPARAM lp) {
        const UINT evt = LOWORD(lp);
        if (evt == WM_RBUTTONUP || evt == WM_CONTEXTMENU) {
            POINT pt{};
            if (evt == WM_CONTEXTMENU) {
                pt.x = GET_X_LPARAM(lp);
                pt.y = GET_Y_LPARAM(lp);
            } else {
                GetCursorPos(&pt);
            }
            switch (TrayMenu::Show(hwnd_, pt)) {
            case TrayMenuAction::ShowWindow:
                RestoreFromTray();
                break;
            case TrayMenuAction::Exit:
                if (recording_) StopRecording();
                StopRun();
                DestroyWindow(hwnd_);
                break;
            default:
                break;
            }
            return 0;
        }
        if (evt == WM_LBUTTONUP) {
            RestoreFromTray();
            return 0;
        }
        return 0;
    }

    // ── Recording ────────────────────────────────────────────────────
    void ToggleRecording() {
        if (recording_) { StopRecording(); } else { StartRecording(); }
    }

    void StartRecording() {
        if (globalHotkey_.enabled && globalHotkey_.vk) {
            SetRecordingIgnoreHotkey(globalHotkey_.modifiers, globalHotkey_.vk, true);
        }
        InstallRecordingHooks();
        {
            std::lock_guard<std::mutex> lock(g_recordMutex);
            g_recordedEvents.clear();
            RecordedEvent initialPos{};
            initialPos.timeOffsetMs = 0;
            initialPos.msg = WM_MOUSEMOVE;
            POINT pt{}; GetCursorPos(&pt);
            initialPos.x = pt.x; initialPos.y = pt.y;
            g_recordedEvents.push_back(initialPos);
        }
        g_recordStartTick.store(GetTickCount(), std::memory_order_relaxed);
        g_lastMouseMoveTick.store(g_recordStartTick.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_recording = true;
        recording_ = true;
        recordingWasVisible_ = IsWindowVisible(hwnd_) == TRUE;
        CloseEditorPopup(); CancelQuickInputTip();
        if (appSettings_.other.playSoundOnStart) MessageBeep(MB_OK);
        if (appSettings_.other.autoHideMainWindow) {
            AddTray();
            ShowWindow(hwnd_, SW_HIDE);
        }
        UpdateStatusTip();
    }

    void StopRecording() {
        g_recording = false;
        recording_ = false;
        SetRecordingIgnoreHotkey(0, 0, false);
        UninstallRecordingHooks();
        RemoveTray();
        HideStatusTip();
        if (appSettings_.other.autoHideMainWindow && recordingWasVisible_) ShowWindow(hwnd_, SW_SHOW);
        ConvertRecordedToActions();
        if (!actions_.empty()) SaveRecording();
    }

    static bool RecordedEventMatchesHotkey(const RecordedEvent& e, const Hotkey& hk) {
        if (!hk.enabled || !hk.vk) return false;
        if (hk.vk == VK_LBUTTON) return e.msg == WM_LBUTTONDOWN || e.msg == WM_LBUTTONUP;
        if (hk.vk == VK_RBUTTON) return e.msg == WM_RBUTTONDOWN || e.msg == WM_RBUTTONUP;
        if (hk.vk == VK_MBUTTON) return e.msg == WM_MBUTTONDOWN || e.msg == WM_MBUTTONUP;
        if (hk.vk == VK_XBUTTON1 || hk.vk == VK_XBUTTON2) return e.msg == WM_XBUTTONDOWN || e.msg == WM_XBUTTONUP;
        if (e.msg == WM_KEYDOWN || e.msg == WM_KEYUP) return static_cast<UINT>(e.vkOrButton) == hk.vk;
        return false;
    }

    static void TrimStopHotkeyTail(std::vector<RecordedEvent>& events, const Hotkey& hk) {
        while (!events.empty() && RecordedEventMatchesHotkey(events.back(), hk)) {
            events.pop_back();
        }
    }

    void ConvertRecordedToActions() {
        actions_.clear();
        // 持锁复制录制事件列表，避免在遍历期间钩子线程修改数据
        std::vector<RecordedEvent> events;
        {
            std::lock_guard<std::mutex> lock(g_recordMutex);
            events = g_recordedEvents;
        }
        TrimStopHotkeyTail(events, globalHotkey_);
        if (events.empty()) return;
        saveDurationSeconds_ = events.back().timeOffsetMs / 1000.0;

        auto emitWait = [&](double curSec, double prevSec) {
            double gap = curSec - prevSec;
            if (gap > 0.001) {
                ScriptAction wa{};
                wa.type = ActionType::Wait;
                wa.duration = gap;
                actions_.push_back(wa);
            }
        };

        for (size_t i = 0; i < events.size(); ++i) {
            const auto& e = events[i];
            double timeSec = e.timeOffsetMs / 1000.0;

            // Move mouse
            if (e.msg == WM_MOUSEMOVE) {
                double prevSec = (i > 0) ? (events[i - 1].timeOffsetMs / 1000.0) : 0.0;
                emitWait(timeSec, prevSec);

                ScriptAction ma{};
                ma.type = ActionType::MoveMouse;
                ma.x = e.x;
                ma.y = e.y;
                ma.randomX = 0;
                ma.randomY = 0;
                actions_.push_back(ma);
                continue;
            }

            // Key down / up (including system keys like LWin/RWin)
            if (e.msg == WM_KEYDOWN || e.msg == WM_SYSKEYDOWN) {
                double prevSec = (i > 0) ? (events[i - 1].timeOffsetMs / 1000.0) : 0.0;
                emitWait(timeSec, prevSec);

                ScriptAction kd{};
                kd.type = ActionType::KeyDown;
                kd.keyVk = static_cast<UINT>(e.vkOrButton);
                kd.keyText = VkName(kd.keyVk);
                actions_.push_back(kd);
                continue;
            }
            if (e.msg == WM_KEYUP || e.msg == WM_SYSKEYUP) {
                double prevSec = (i > 0) ? (events[i - 1].timeOffsetMs / 1000.0) : 0.0;
                emitWait(timeSec, prevSec);

                ScriptAction ku{};
                ku.type = ActionType::KeyUp;
                ku.keyVk = static_cast<UINT>(e.vkOrButton);
                ku.keyText = VkName(ku.keyVk);
                actions_.push_back(ku);
                continue;
            }

            // Mouse button down / up
            int mbtnIdx = -1;
            if (e.vkOrButton == VK_LBUTTON) mbtnIdx = 0;
            else if (e.vkOrButton == VK_RBUTTON) mbtnIdx = 1;
            else if (e.vkOrButton == VK_MBUTTON) mbtnIdx = 2;

            if (mbtnIdx >= 0 && (e.msg == WM_LBUTTONDOWN || e.msg == WM_RBUTTONDOWN || e.msg == WM_MBUTTONDOWN)) {
                double prevSec = (i > 0) ? (events[i - 1].timeOffsetMs / 1000.0) : 0.0;
                emitWait(timeSec, prevSec);

                ScriptAction md{};
                md.type = ActionType::MouseDown;
                md.button = static_cast<MouseButtonType>(mbtnIdx);
                actions_.push_back(md);
                continue;
            }
            if (mbtnIdx >= 0 && (e.msg == WM_LBUTTONUP || e.msg == WM_RBUTTONUP || e.msg == WM_MBUTTONUP)) {
                double prevSec = (i > 0) ? (events[i - 1].timeOffsetMs / 1000.0) : 0.0;
                emitWait(timeSec, prevSec);

                ScriptAction mu{};
                mu.type = ActionType::MouseUp;
                mu.button = static_cast<MouseButtonType>(mbtnIdx);
                actions_.push_back(mu);
                continue;
            }

            // XButton (X1/X2) down / up
            if (e.msg == WM_XBUTTONDOWN || e.msg == WM_XBUTTONUP) {
                int xbtnIdx = (e.vkOrButton == VK_XBUTTON1) ? 3 : 4;
                double prevSec = (i > 0) ? (events[i - 1].timeOffsetMs / 1000.0) : 0.0;
                emitWait(timeSec, prevSec);
                ScriptAction mb{};
                mb.type = (e.msg == WM_XBUTTONDOWN) ? ActionType::MouseDown : ActionType::MouseUp;
                mb.button = static_cast<MouseButtonType>(xbtnIdx);
                actions_.push_back(mb);
                continue;
            }

            // Scroll wheel vertical / horizontal
            if (e.msg == WM_MOUSEWHEEL || e.msg == WM_MOUSEHWHEEL) {
                double prevSec = (i > 0) ? (events[i - 1].timeOffsetMs / 1000.0) : 0.0;
                emitWait(timeSec, prevSec);
                ScriptAction sw{};
                sw.type = ActionType::ScrollWheel;
                sw.scrollVertical = (e.msg == WM_MOUSEWHEEL);
                sw.scrollHorizontal = (e.msg == WM_MOUSEHWHEEL);
                sw.scrollSteps = std::max(1, std::abs(e.wheelDelta) / WHEEL_DELTA);
                sw.scrollDirection = (e.wheelDelta > 0) ? 0 : 1;
                sw.clickCount = 1;
                sw.duration = 0.01;
                actions_.push_back(sw);
                continue;
            }
        }
    }

    void SaveRecording() {
        EnsureScriptsDir();
        std::wstring name = L"鼠标录制-" + TimestampName();
        std::wstring path = RecordingsDir() + L"\\" + name + L".json";
        currentPath_ = path;
        currentRecordTime_ = NowText();
        SetText(name_, name);
        SaveScriptFile(path);
        saveDurationSeconds_ = 0;
        RefreshRunBlockCombo();
        LoadRecordings();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // ── Clicker ──────────────────────────────────────────────────────
    void ToggleClicker();

    void StartClicking() {
        if (clicking_) return;
        clicking_ = true;
        clickCountDone_ = 0;
        if (appSettings_.other.playSoundOnStart) MessageBeep(MB_OK);
        if (appSettings_.other.autoHideMainWindow) {
            AddTray();
            ShowWindow(hwnd_, SW_HIDE);
        }
        UpdateStatusTip();
        clickerThread_ = std::thread([this]() {
            const auto& cs = appSettings_.click;
            while (clicking_ && !stopFlag_) {
                double interval = 0.1;
                switch (clickerSettings_.intervalMode) {
                case quickscript::ClickIntervalMode::Custom:
                    interval = std::max(0.001, clickerSettings_.customIntervalSeconds);
                    break;
                case quickscript::ClickIntervalMode::Efficient:
                    interval = 0.1;
                    break;
                case quickscript::ClickIntervalMode::Extreme:
                    interval = 0.01;
                    break;
                }
                if (cs.enableRandomInterval) interval += RandomDelay(cs.randomIntervalMaxSeconds);

                int clickX = 0, clickY = 0;
                if (cs.enableFixedCoordinates) {
                    clickX = cs.fixedX;
                    clickY = cs.fixedY;
                } else {
                    POINT pt{}; GetCursorPos(&pt);
                    clickX = pt.x;
                    clickY = pt.y;
                }
                if (cs.enableCoordinateJitter) {
                    clickX += RandomInt(cs.jitterX);
                    clickY += RandomInt(cs.jitterY);
                }
                SetCursorPos(clickX, clickY);

                DWORD downFlag, upFlag;
                if (clickerSettings_.button == quickscript::MouseButtonChoice::Left) {
                    downFlag = MOUSEEVENTF_LEFTDOWN; upFlag = MOUSEEVENTF_LEFTUP;
                } else if (clickerSettings_.button == quickscript::MouseButtonChoice::Right) {
                    downFlag = MOUSEEVENTF_RIGHTDOWN; upFlag = MOUSEEVENTF_RIGHTUP;
                } else {
                    downFlag = MOUSEEVENTF_MIDDLEDOWN; upFlag = MOUSEEVENTF_MIDDLEUP;
                }

                INPUT downInput{};
                downInput.type = INPUT_MOUSE;
                downInput.mi.dwFlags = downFlag;
                SendInput(1, &downInput, sizeof(INPUT));

                if (cs.enablePressReleaseInterval) {
                    SleepInterruptible(cs.pressReleaseIntervalSeconds);
                }

                if (clicking_ && !stopFlag_) {
                    INPUT upInput{};
                    upInput.type = INPUT_MOUSE;
                    upInput.mi.dwFlags = upFlag;
                    SendInput(1, &upInput, sizeof(INPUT));
                }

                ++clickCountDone_;
                if (cs.enableClickCountLimit && cs.clickCountLimit > 0 && clickCountDone_ >= cs.clickCountLimit) {
                    PostMessageW(hwnd_, WM_HOTKEY, HOTKEY_GLOBAL_ID, 0);
                    break;
                }

                const auto end = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(static_cast<int>(interval * 1000.0));
                while (clicking_ && !stopFlag_ && std::chrono::steady_clock::now() < end) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
        });
    }

    void StopClicking() {
        clicking_ = false;
        if (clickerThread_.joinable()) {
            clickerThread_.join();
        }
        RemoveTray();
        HideStatusTip();
        if (appSettings_.other.autoHideMainWindow) {
            ShowWindow(hwnd_, SW_SHOW);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // ── Row layout helpers ─────────────────────────────────────────
    RECT RowRect(int index) const {
        const int slot = VisibleSlotOf(index);
        if (slot < 0) return RECT{};
        RECT list = ActionListRect();
        const int y = list.top + (slot - scrollOffset_) * kRowH;
        return RECT{list.left, y, ActionListContentRight(), y + kRowH};
    }
    bool EditorComboHitTest(int x, int y) {
        const int id = EditorComboPopupIdAtPoint(x, y);
        if (id < 0) return false;
        ToggleEditorPopup(id);
        return true;
    }

    int HitRow(int x, int y) const {
        RECT list = ActionListRect();
        if (x < list.left || x > ActionListContentRight() || y < list.top || y > list.bottom) return -1;
        const int visibleSlot = scrollOffset_ + (y - list.top) / kRowH;
        const auto vis = VisibleActionIndices();
        if (visibleSlot < 0 || visibleSlot >= static_cast<int>(vis.size())) return -1;
        return vis[static_cast<size_t>(visibleSlot)];
    }
    RECT ExpandToggleRect(int index) const {
        if (index < 0 || index >= static_cast<int>(actions_.size())) return RECT{};
        const auto& a = actions_[static_cast<size_t>(index)];
        if (!IsExpandableContainer(a.type)) return RECT{};
        RECT r = RowRect(index);
        const int left = kListX + ExpandToggleLeftLocal(a.indent, batchEditMode_);
        return RECT{left, r.top + 8, left + kExpandToggleWidth, r.bottom - 8};
    }
    RECT CopyRect(int i) const { RECT r = RowRect(i); return RECT{r.right - 104, r.top + 6, r.right - 62, r.bottom - 6}; }
    RECT DeleteRect(int i) const { RECT r = RowRect(i); return RECT{r.right - 58, r.top + 6, r.right - 18, r.bottom - 6}; }
    bool PtIn(RECT r, int x, int y) const { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; }

    void OnEditorClick(int x, int y) {
        int row = HitRow(x, y); if (row < 0) return;
        if (batchEditMode_) {
            if (IsExpandableContainer(actions_[static_cast<size_t>(row)].type) && PtIn(ExpandToggleRect(row), x, y)) {
                ToggleContainerExpand(row);
                return;
            }
            if (PtIn(CheckboxRect(row), x, y)) ToggleBatchSelection(row);
            return;
        }
        if (PtIn(DeleteRect(row), x, y)) { CommitInlineRemark(); DeleteActionAt(row); return; }
        if (PtIn(CopyRect(row), x, y)) { copySource_ = row; ShowCopyMenu(x, y); return; }
        if (PtIn(RemarkRect(row), x, y)) { BeginInlineRemarkEdit(row); return; }
        if (IsExpandableContainer(actions_[static_cast<size_t>(row)].type) && PtIn(ExpandToggleRect(row), x, y)) { ToggleContainerExpand(row); return; }
        CommitInlineRemark();
        dragIndex_ = row;
        dragTargetIndex_ = row;
        dragTargetIndent_ = actions_[static_cast<size_t>(row)].indent;
        dragTargetNested_ = false;
        dragStartX_ = x;
        dragStartY_ = y;
        dragMoved_ = false;
        dragging_ = true;
        SetCapture(hwnd_);
    }

    void DeleteActionAt(int row) {
        if (row < 0 || row >= static_cast<int>(actions_.size())) return;
        const int end = SubtreeEnd(row);
        const int count = end - row;
        collapsedContainers_ = RemapCollapsedAfterDelete(collapsedContainers_, row, end);
        actions_.erase(actions_.begin() + row, actions_.begin() + end);
        if (selectedIndex_ >= row && selectedIndex_ < end) selectedIndex_ = -1;
        else if (selectedIndex_ >= end) selectedIndex_ -= count;
        if (hoverIndex_ >= row && hoverIndex_ < end) hoverIndex_ = -1;
        else if (hoverIndex_ >= end) hoverIndex_ -= count;
        RenumberActions();
        RefreshRunBlockCombo();
        UpdateEditMode();
        OnActionsChanged();
    }

    DragInsertTarget HitInsertTarget(int x, int y) const {
        DragInsertTarget target{};
        const auto vis = VisibleActionIndices();
        if (actions_.empty()) return target;
        RECT list = ActionListRect();
        const int listH = static_cast<int>(list.bottom - list.top);
        const int relativeY = std::clamp(y - static_cast<int>(list.top), 0, listH);
        const int visibleRow = std::clamp(scrollOffset_ + relativeY / kRowH, 0, std::max(0, static_cast<int>(vis.size()) - 1));
        const int inRowY = relativeY % kRowH;
        const int slot = std::clamp(visibleRow + (inRowY > kRowH / 2 ? 1 : 0), 0, static_cast<int>(vis.size()));
        target.insertIndex = slot >= static_cast<int>(vis.size()) ? static_cast<int>(actions_.size()) : vis[static_cast<size_t>(slot)];
        const int xIndent = IndentFromX(x);
        if (target.insertIndex > 0) {
            int prevIndex = target.insertIndex - 1;
            if (dragging_ && prevIndex >= dragIndex_ && prevIndex < SubtreeEnd(dragIndex_)) {
                prevIndex = dragIndex_ - 1;
            }
            if (prevIndex >= 0) {
                const auto& prev = actions_[static_cast<size_t>(prevIndex)];
                if (IsSubtreeContainer(prev.type) && xIndent > prev.indent) {
                    target.targetIndent = prev.indent + 1;
                    target.nested = true;
                } else if (prev.indent > 0) {
                    const int parentLeft = ActionLeftClient(prev.indent - 1);
                    const int childLeft = ActionLeftClient(prev.indent);
                    const int threshold = (parentLeft + childLeft) / 2;
                    target.targetIndent = x < threshold ? prev.indent - 1 : prev.indent;
                    target.nested = target.targetIndent > 0;
                } else {
                    target.targetIndent = prev.indent;
                }
            }
        }
        // Prevent inserting a peer/reduced-indent item inside a container body (would split the container from its children)
        bool adjusted;
        do {
            adjusted = false;
            for (int i = target.insertIndex - 1; i >= 0; --i) {
                if (IsSubtreeContainer(actions_[i].type)) {
                    const int bodyEnd = ContainerBodyEndIndex(i);
                    if (target.insertIndex > i && target.insertIndex < bodyEnd && target.targetIndent <= actions_[i].indent) {
                        target.insertIndex = bodyEnd;
                        target.targetIndent = actions_[i].indent;
                        target.nested = false;
                        adjusted = true;
                    }
                    break;
                }
            }
        } while (adjusted);
        return target;
    }

    void MoveDragged(int x, int y) {
        if (dragIndex_ < 0) return;
        const DragInsertTarget target = HitInsertTarget(x, y);
        int insertIndex = target.insertIndex;
        const int dragEnd = SubtreeEnd(dragIndex_);
        if (insertIndex > dragIndex_ && insertIndex < dragEnd) insertIndex = dragEnd;
        if ((insertIndex == dragIndex_ || insertIndex == dragEnd) &&
            target.targetIndent == actions_[static_cast<size_t>(dragIndex_)].indent) {
            return;
        }
        if (target.targetIndent != dragTargetIndent_ || insertIndex != dragTargetIndex_ || target.nested != dragTargetNested_) {
            dragTargetIndex_ = insertIndex;
            dragTargetIndent_ = target.targetIndent;
            dragTargetNested_ = target.nested;
            dragMoved_ = true;
        }
    }

    void CompleteDrag() {
        if (dragIndex_ < 0 || dragIndex_ >= static_cast<int>(actions_.size())) return;
        if (!dragMoved_) {
            selectedIndex_ = selectedIndex_ == dragIndex_ ? -1 : dragIndex_;
            UpdateEditMode();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        const int dragEnd = SubtreeEnd(dragIndex_);
        const int count = dragEnd - dragIndex_;
        if (dragTargetIndex_ > dragIndex_ && dragTargetIndex_ < dragEnd) {
            dragTargetIndex_ = dragEnd;
        }
        std::vector<ScriptAction> block(actions_.begin() + dragIndex_, actions_.begin() + dragEnd);
        const int indentDelta = dragTargetIndent_ - block.front().indent;
        actions_.erase(actions_.begin() + dragIndex_, actions_.begin() + dragEnd);
        int insertIndex = dragTargetIndex_;
        if (insertIndex > dragIndex_) insertIndex -= count;
        insertIndex = std::clamp(insertIndex, 0, static_cast<int>(actions_.size()));
        collapsedContainers_ = RemapCollapsedAfterMove(collapsedContainers_, dragIndex_, dragEnd, insertIndex, count);
        for (auto& action : block) action.indent = std::max(0, action.indent + indentDelta);
        actions_.insert(actions_.begin() + insertIndex, block.begin(), block.end());
        selectedIndex_ = insertIndex;
        RenumberActions();
        EnsureSelectedVisible();
        UpdateEditMode();
        OnActionsChanged();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void RenumberActions() { for (size_t i = 0; i < actions_.size(); ++i) actions_[i].originalNo = static_cast<int>(i + 1); }
    void UpdateHomeScrollFromThumb(int thumbTop) {
        RECT track = HomeScrollTrackRect();
        RECT thumb = HomeScrollThumbRect();
        const int maxScroll = MaxHomeScroll();
        const int trackHeight = static_cast<int>(track.bottom - track.top);
        const int thumbHeight = static_cast<int>(thumb.bottom - thumb.top);
        const int range = std::max(1, trackHeight - thumbHeight);
        const int thumbOffset = thumbTop - static_cast<int>(track.top);
        homeScrollOffset_ = std::clamp(thumbOffset * maxScroll / range, 0, maxScroll);
    }

    void OnWheel(int delta) {
        if (deleteConfirmVisible_) return;
        if (page_ == Page::Editor && editorPopupOpen_ >= 0 && !EditorDropPopupVisible()) {
            PopupCombo* pc = GetEditorPopup();
            if (pc && pc->open && !pc->items.empty()) {
                const int total = static_cast<int>(pc->items.size());
                const int visible = EditorPopupVisibleCount();
                const int scrollMax = std::max(0, total - visible);
                if (scrollMax > 0) {
                    const int oldScroll = editorPopupScroll_;
                    editorPopupScroll_ = std::clamp(editorPopupScroll_ + (delta < 0 ? 1 : -1), 0, scrollMax);
                    if (oldScroll != editorPopupScroll_) InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
            return;
        }
        if (page_ == Page::Home) {
            if (activeHomeTab_ != quickscript::MainTab::Macro) return;
            homeScrollOffset_ = std::clamp(homeScrollOffset_ + (delta < 0 ? kHomeCardStep : -kHomeCardStep), 0, MaxHomeScroll());
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (page_ != Page::Editor) return;
        // Scroll parameter panel when cursor is inside the scroll viewport (below action combo, above cancel/save)
        {
            POINT cursorPt;
            GetCursorPos(&cursorPt);
            ScreenToClient(hwnd_, &cursorPt);
            RECT paramVp = ParamScrollViewportRect();
            if (PtIn(paramVp, cursorPt.x, cursorPt.y)) {
                ScrollParamPanel(delta < 0 ? 40 : -40);
                return;
            }
        }
        if (MaxEditorScroll() <= 0) return;
        const int oldScroll = scrollOffset_;
        const int maxOffset = MaxEditorScroll();
        scrollOffset_ = std::clamp(scrollOffset_ + (delta < 0 ? 3 : -3), 0, maxOffset);
        if (oldScroll != scrollOffset_) RefreshActionListLayer();
    }
    void EnsureSelectedVisible() {
        if (selectedIndex_ < 0) return;
        const int slot = VisibleSlotOf(selectedIndex_);
        if (slot < 0) return;
        const int visible = VisibleActionRows();
        if (visible <= 0) { scrollOffset_ = 0; return; }
        if (slot < scrollOffset_) scrollOffset_ = slot;
        if (slot >= scrollOffset_ + visible) scrollOffset_ = slot - visible + 1;
        scrollOffset_ = std::clamp(scrollOffset_, 0, MaxEditorScroll());
    }

    void ShowCopyMenu(int x, int y) { HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING, kCopyLast, L"复制到最后"); AppendMenuW(menu, MF_STRING, kCopyFirst, L"复制到最前"); AppendMenuW(menu, MF_STRING, kCopyBeforeSelected, L"复制到当前项前"); AppendMenuW(menu, MF_STRING, kCopyAfterSelected, L"复制到当前项后"); POINT pt{x, y}; ClientToScreen(hwnd_, &pt); TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd_, nullptr); DestroyMenu(menu); }

    void DrawTextIn(HDC hdc, const std::wstring& text, RECT rc, COLORREF color, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    void DrawTopAction(HDC hdc, RECT rc, const std::wstring& text, int iconType) {
        HPEN pen = CreatePen(PS_SOLID, 2, kWhite);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        const int x = rc.left + 8;
        const int y = rc.top + 9;
        if (iconType == 0 || iconType == 1) {
            MoveToEx(hdc, x + 10, y + (iconType == 0 ? 0 : 14), nullptr);
            LineTo(hdc, x + 10, y + (iconType == 0 ? 14 : 0));
            LineTo(hdc, x + 5, y + (iconType == 0 ? 9 : 5));
            MoveToEx(hdc, x + 10, y + (iconType == 0 ? 14 : 0), nullptr);
            LineTo(hdc, x + 15, y + (iconType == 0 ? 9 : 5));
            MoveToEx(hdc, x + 2, y + 15, nullptr);
            LineTo(hdc, x + 2, y + 20);
            LineTo(hdc, x + 18, y + 20);
            LineTo(hdc, x + 18, y + 15);
        } else {
            Ellipse(hdc, x + 1, y + 1, x + 21, y + 21);
            MoveToEx(hdc, x + 11, y + 5, nullptr);
            LineTo(hdc, x + 11, y + 11);
            LineTo(hdc, x + 16, y + 11);
        }
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
        SelectObject(hdc, homeFont_);
        DrawTextIn(hdc, text, RECT{rc.left + 32, rc.top, rc.right, rc.bottom}, kWhite);
    }

    void FillGradientRect(HDC hdc, RECT rc, COLORREF start, COLORREF end, bool vertical) {
        TRIVERTEX vertex[2]{};
        vertex[0].x = rc.left;
        vertex[0].y = rc.top;
        vertex[0].Red = static_cast<COLOR16>(GetRValue(start) << 8);
        vertex[0].Green = static_cast<COLOR16>(GetGValue(start) << 8);
        vertex[0].Blue = static_cast<COLOR16>(GetBValue(start) << 8);
        vertex[0].Alpha = 0;
        vertex[1].x = rc.right;
        vertex[1].y = rc.bottom;
        vertex[1].Red = static_cast<COLOR16>(GetRValue(end) << 8);
        vertex[1].Green = static_cast<COLOR16>(GetGValue(end) << 8);
        vertex[1].Blue = static_cast<COLOR16>(GetBValue(end) << 8);
        vertex[1].Alpha = 0;
        GRADIENT_RECT gr{0, 1};
        GradientFill(hdc, vertex, 2, &gr, 1, vertical ? GRADIENT_FILL_RECT_V : GRADIENT_FILL_RECT_H);
    }

    void DrawHomeLogo(HDC hdc) {
        HPEN pen = CreatePen(PS_SOLID, 2, kWhite);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        Ellipse(hdc, 11, 10, 28, 28);
        MoveToEx(hdc, 19, 7, nullptr);
        LineTo(hdc, 19, 15);
        MoveToEx(hdc, 12, 7, nullptr);
        LineTo(hdc, 26, 7);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }

    void DrawNavIcon(HDC hdc, RECT rc, int iconType) {
        const int cx = rc.left + 29;
        const int cy = rc.top + 35;
        HPEN pen = CreatePen(PS_SOLID, 2, kWhite);
        HBRUSH brush = CreateSolidBrush(kWhite);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, brush);
        if (iconType == 0) {
            POINT arrow[3] = {{cx - 7, cy - 12}, {cx - 7, cy + 13}, {cx + 10, cy + 3}};
            Polygon(hdc, arrow, 3);
            MoveToEx(hdc, cx + 2, cy + 5, nullptr);
            LineTo(hdc, cx + 9, cy + 15);
        } else if (iconType == 1) {
            SelectObject(hdc, GetStockObject(NULL_BRUSH));
            RoundRect(hdc, cx - 10, cy - 10, cx + 10, cy + 10, 5, 5);
            SelectObject(hdc, brush);
            Ellipse(hdc, cx - 4, cy - 4, cx + 4, cy + 4);
        } else if (iconType == 2) {
            SelectObject(hdc, homeTabFont_);
            DrawTextIn(hdc, L"宏", RECT{cx - 13, cy - 14, cx + 15, cy + 14}, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            SelectObject(hdc, GetStockObject(NULL_BRUSH));
            RoundRect(hdc, cx - 13, cy - 10, cx + 13, cy + 10, 5, 5);
            MoveToEx(hdc, cx - 5, cy - 4, nullptr);
            LineTo(hdc, cx - 10, cy);
            LineTo(hdc, cx - 5, cy + 4);
            MoveToEx(hdc, cx + 5, cy - 4, nullptr);
            LineTo(hdc, cx + 10, cy);
            LineTo(hdc, cx + 5, cy + 4);
        }
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
    }

    void DrawTitleButtons(HDC hdc) {
        RECT close = CloseRect();
        if (page_ == Page::Home) {
            RECT settings = SettingsRect();
            RECT minimize = MinimizeRect();
            if (hoverButton_ == HoverButton::Settings) FillAlphaRect(hdc, settings, RGB(0, 0, 0), kCloseHoverAlpha);
            if (hoverButton_ == HoverButton::Minimize) FillAlphaRect(hdc, minimize, RGB(0, 0, 0), kCloseHoverAlpha);
            SelectObject(hdc, closeFont_);
            DrawTextIn(hdc, L"⚙", settings, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextIn(hdc, L"−", minimize, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            RECT minimize = MinimizeRect();
            if (hoverButton_ == HoverButton::Minimize) FillAlphaRect(hdc, minimize, RGB(0, 0, 0), kCloseHoverAlpha);
            SelectObject(hdc, closeFont_);
            DrawTextIn(hdc, L"−", minimize, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (hoverButton_ == HoverButton::Close) FillAlphaRect(hdc, close, RGB(0, 0, 0), kCloseHoverAlpha);
        SelectObject(hdc, closeFont_);
        DrawTextIn(hdc, L"×", close, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    void DrawHomeShell(HDC hdc) {
        DrawHomeLogo(hdc);
        SelectObject(hdc, homeTabFont_);
        DrawTextIn(hdc, L"鼠大侠", RECT{33, 0, 120, kTitleH}, kWhite);
        DrawNavTab(hdc, ClickerTabRect(), quickscript::MainTab::Clicker, L"鼠标连点", 0);
        DrawNavTab(hdc, RecorderTabRect(), quickscript::MainTab::Recorder, L"鼠标录制", 1);
        DrawNavTab(hdc, MacroTabRect(), quickscript::MainTab::Macro, L"鼠标宏", 2);
        DrawNavTab(hdc, ScriptCustomTabRect(), quickscript::MainTab::ScriptCustom, L"脚本定制", 3);
    }

    void DrawNavTab(HDC hdc, RECT rc, quickscript::MainTab tab, const std::wstring& text, int iconType);

    void DrawRadio(HDC hdc, RECT rc, bool checked);

    std::wstring ClickIntervalTitle(quickscript::ClickIntervalMode mode) const {
        switch (mode) {
        case quickscript::ClickIntervalMode::Custom: return L"自定义";
        case quickscript::ClickIntervalMode::Efficient: return L"高效模式(每秒10次点击)";
        case quickscript::ClickIntervalMode::Extreme: return L"极速模式(每秒100次)";
        default: return L"高效模式(每秒10次点击)";
        }
    }

    std::wstring ClickerButtonTitle() const {
        switch (clickerSettings_.button) {
        case quickscript::MouseButtonChoice::Left: return L"左键";
        case quickscript::MouseButtonChoice::Middle: return L"中键";
        case quickscript::MouseButtonChoice::Right: return L"右键";
        default: return L"左键";
        }
    }

    void DrawClickerCombo(HDC hdc, RECT rc, const std::wstring& text, bool dropped = false) {
        const COLORREF borderColor = dropped ? kMainGreen : kComboBorderGray;
        HBRUSH white = CreateSolidBrush(kWhite);
        FillRect(hdc, &rc, white);
        DeleteObject(white);
        DrawTextIn(hdc, text, RECT{rc.left + 8, rc.top, rc.right - 36, rc.bottom}, kMainGreen);
        POINT arrow[3] = {{rc.right - 22, rc.top + 11}, {rc.right - 10, rc.top + 11}, {rc.right - 16, rc.top + 20}};
        HBRUSH arrowBrush = CreateSolidBrush(kMainGreen);
        HGDIOBJ oldBrush = SelectObject(hdc, arrowBrush);
        HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
        Polygon(hdc, arrow, 3);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(arrowBrush);
        DrawBorderRect(hdc, rc, borderColor);
    }

    void DrawClickerCombo(HDC hdc, RECT rc, bool dropped = false) {
        DrawClickerCombo(hdc, rc, ClickIntervalTitle(clickerSettings_.intervalMode), dropped);
    }

    void DrawClickerPopupMenuItem(HDC hdc, const RECT& row, const wchar_t* title, const wchar_t* desc, bool checked, bool hovered);

    void PaintClickerDropPopupContent(HDC hdc, HWND popupHwnd);

    // ── Editor popup combo drawing ─────────────────────────────────
    PopupCombo* PopupComboForEditorId(int id) {
        switch (id) {
        case 0: return &popupMode_;
        case 1: return &popupAction_;
        case 2: return &popupMouseBtn_;
        case 3: return &popupClickBtn_;
        case 4: return &popupLoopType_;
        case 5: return &popupRunBlock_;
        case 6: return &popupHotkeyShortcut_;
        case 7: return &popupQuickInputVar_;
        case 8: return &popupRunMacro_;
        case 9: return &popupMousePlayback_;
        case 10: return &popupScrollDir_;
        case 11: return &popupFindFollowUp_;
        case 12: return &popupIfVar_;
        case 13: return &popupIfOperator_;
        case 14: return &popupIfConnector_;
        case 15: return &popupRunProgram_;
        case 16: return &popupOcrResultMode_;
        case 17: return &popupOcrFollowUp_;
        case 18: return &popupOcrSearchVar_;
        case 19: return &popupAiModel_;
        case 20: return &popupAiContextMode_;
        case 21: return &popupAiOutputType_;
        case 22: return &popupAiSearchRegion_;
        default: return nullptr;
        }
    }

    std::wstring EditorComboDisplayText(HWND label) const {
        std::wstring text = GetText(label);
        if (!text.empty()) return text;
        const int id = const_cast<MainWindow*>(this)->EditorComboPopupIdForHwnd(label);
        if (id < 0) return text;
        PopupCombo* pc = const_cast<MainWindow*>(this)->PopupComboForEditorId(id);
        if (!pc || pc->sel < 0 || pc->sel >= static_cast<int>(pc->items.size())) return text;
        return pc->items[static_cast<size_t>(pc->sel)];
    }

    PopupCombo* GetEditorPopup() {
        return PopupComboForEditorId(editorPopupOpen_);
    }

    int EditorPopupVisibleCount() const {
        PopupCombo* pc = const_cast<MainWindow*>(this)->GetEditorPopup();
        if (!pc || pc->items.empty()) return 0;
        const int maxVisible = kEditorPopupMaxHeight / kEditorPopupItemH;
        return std::min(maxVisible, static_cast<int>(pc->items.size()));
    }

    RECT EditorPopupListRect() const {
        RECT base = EditorPopupRect();
        const int visible = EditorPopupVisibleCount();
        return RECT{base.left, base.bottom, base.right, base.bottom + visible * kEditorPopupItemH + 2};
    }

    RECT EditorPopupRect() const {
        switch (editorPopupOpen_) {
        case 0: return WindowClientRect(mode_);
        case 1: return WindowClientRect(actionCombo_);
        case 2: return WindowClientRect(mousePressButton_);
        case 3: return WindowClientRect(clickButton_);
        case 4: return WindowClientRect(loopTypeCombo_);
        case 5: return WindowClientRect(runBlockCombo_);
        case 6: return WindowClientRect(hotkeyShortcutCombo_);
        case 7: {
            HWND combo = ActiveVarComboHwnd();
            return combo ? WindowClientRect(combo) : RECT{};
        }
        case 8: return WindowClientRect(runMacroCombo_);
        case 9: return WindowClientRect(mousePlaybackCombo_);
        case 10: return WindowClientRect(scrollDirectionCombo_);
        case 11: return WindowClientRect(findFollowUpCombo_);
        case 12: return WindowClientRect(ifVarCombo_);
        case 13: return WindowClientRect(ifOperatorCombo_);
        case 14: return WindowClientRect(ifConnectorCombo_);
        case 15: return WindowClientRect(runProgramCombo_);
        case 16: return WindowClientRect(ocrResultModeCombo_);
        case 17: return WindowClientRect(ocrFollowUpCombo_);
        case 18: return WindowClientRect(ocrSearchVarCombo_);
        case 19: return WindowClientRect(aiModelCombo_);
        case 20: return WindowClientRect(aiContextModeCombo_);
        case 21: return WindowClientRect(aiOutputTypeCombo_);
        case 22: return WindowClientRect(aiSearchRegionCombo_);
        default: return RECT{};
        }
    }

    RECT EditorComboInvalidateRect(int popupId) const {
        HWND combo = nullptr;
        switch (popupId) {
        case 0: combo = mode_; break;
        case 1: combo = actionCombo_; break;
        case 2: combo = mousePressButton_; break;
        case 3: combo = clickButton_; break;
        case 4: combo = loopTypeCombo_; break;
        case 5: combo = runBlockCombo_; break;
        case 6: combo = hotkeyShortcutCombo_; break;
        case 7: combo = ActiveVarComboHwnd(); break;
        case 8: combo = runMacroCombo_; break;
        case 9: combo = mousePlaybackCombo_; break;
        case 10: combo = scrollDirectionCombo_; break;
        case 11: combo = findFollowUpCombo_; break;
        case 12: combo = ifVarCombo_; break;
        case 13: combo = ifOperatorCombo_; break;
        case 14: combo = ifConnectorCombo_; break;
        case 15: combo = runProgramCombo_; break;
        case 16: combo = ocrResultModeCombo_; break;
        case 17: combo = ocrFollowUpCombo_; break;
        case 18: combo = ocrSearchVarCombo_; break;
        case 19: combo = aiModelCombo_; break;
        case 20: combo = aiContextModeCombo_; break;
        case 21: combo = aiOutputTypeCombo_; break;
        case 22: combo = aiSearchRegionCombo_; break;
        default: return RECT{};
        }
        if (!combo) return RECT{};
        RECT rc = WindowClientRect(combo);
        if (popupId == 0) rc.left = std::max<LONG>(0, rc.left - 52);
        else if (popupId == 1) rc.top = std::max<LONG>(0, rc.top - 28);
        InflateRect(&rc, 3, 3);
        return rc;
    }

    void InvalidateEditorComboArea(int popupId) {
        if (popupId < 0) return;
        const RECT rc = EditorComboInvalidateRect(popupId);
        if (rc.right > rc.left && rc.bottom > rc.top) InvalidateRect(hwnd_, &rc, FALSE);
    }

    void InvalidateEditorParamPanel() {
        InvalidateParamScrollArea();
    }

    void DrawEditorCombo(HDC hdc, HWND label, RECT rc, bool dropped = false);

    void CreateEditorDropPopup() {
        RegisterEditorDropPopupClass();
        editorDropPopup_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            L"QSEditorDropPopup", L"",
            WS_POPUP,
            0, 0, 0, 0,
            hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (editorDropPopup_) {
            SetWindowLongPtrW(editorDropPopup_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            ShowWindow(editorDropPopup_, SW_HIDE);
        }
    }

    bool EditorDropPopupVisible() const {
        return editorDropPopup_ && IsWindowVisible(editorDropPopup_) == TRUE;
    }

    void SyncEditorDropPopup() {
        if (!editorDropPopup_) return;
        if (editorPopupOpen_ < 0 || page_ != Page::Editor || !IsWindowVisible(hwnd_)) {
            ShowWindow(editorDropPopup_, SW_HIDE);
            return;
        }
        PopupCombo* pc = GetEditorPopup();
        if (!pc || !pc->open || pc->items.empty()) {
            ShowWindow(editorDropPopup_, SW_HIDE);
            return;
        }
        RECT list = EditorPopupListRect();
        const int w = list.right - list.left;
        const int h = list.bottom - list.top;
        POINT screenTop{list.left, list.top};
        ClientToScreen(hwnd_, &screenTop);
        const int x = static_cast<int>(screenTop.x);
        int y = static_cast<int>(screenTop.y);
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        if (y + h > work.bottom) {
            RECT anchor = EditorPopupRect();
            POINT screenAnchor{anchor.left, anchor.top};
            ClientToScreen(hwnd_, &screenAnchor);
            y = static_cast<int>(screenAnchor.y) - h;
        }
        y = std::max(static_cast<int>(work.top), std::min(y, static_cast<int>(work.bottom) - h));
        RECT existing{};
        GetWindowRect(editorDropPopup_, &existing);
        const bool samePos = existing.left == x && existing.top == y
            && (existing.right - existing.left) == w && (existing.bottom - existing.top) == h;
        if (samePos && EditorDropPopupVisible()) return;
        SetWindowPos(editorDropPopup_, HWND_TOP, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
    }

    void InvalidateEditorPopupRow(int idx) {
        if (!editorDropPopup_ || idx < 0) return;
        RECT client{};
        GetClientRect(editorDropPopup_, &client);
        const int vis = idx - editorPopupScroll_;
        if (vis < 0 || vis >= EditorPopupVisibleCount()) return;
        RECT row{client.left + 1, client.top + 1 + vis * kEditorPopupItemH, client.right - 1, client.top + 1 + (vis + 1) * kEditorPopupItemH};
        InvalidateRect(editorDropPopup_, &row, FALSE);
    }

    void PaintEditorDropPopupContent(HDC hdc, HWND popupHwnd);

    void CreateEditorTipPopup() {
        RegisterEditorTipPopupClass();
        editorTipPopup_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            L"QSEditorTipPopup", L"",
            WS_POPUP,
            0, 0, 0, 0,
            hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (editorTipPopup_) {
            SetWindowLongPtrW(editorTipPopup_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            ShowWindow(editorTipPopup_, SW_HIDE);
        }
    }

    void SyncQuickInputTipPopup() {
        if (!editorTipPopup_) return;
        if (quickInputTipShown_ == QuickInputTipKind::None || page_ != Page::Editor) {
            ShowWindow(editorTipPopup_, SW_HIDE);
            return;
        }
        const SIZE size = MeasureQuickInputTipSize();
        if (size.cx <= 0 || size.cy <= 0) {
            ShowWindow(editorTipPopup_, SW_HIDE);
            return;
        }
        POINT screenPt{};
        if (quickInputTipShown_ == QuickInputTipKind::TextExample) {
            POINT clientPt = quickInputTipAnchor_;
            ClientToScreen(hwnd_, &clientPt);
            screenPt.x = clientPt.x + 12;
            screenPt.y = clientPt.y + 16;
        } else if (quickInputTipShown_ == QuickInputTipKind::VariableHelp) {
            if (editorDropPopup_ && EditorDropPopupVisible()) {
                RECT popupRc{};
                GetWindowRect(editorDropPopup_, &popupRc);
                const int vis = editorPopupHover_ - editorPopupScroll_;
                if (vis >= 0 && vis < EditorPopupVisibleCount()) {
                    screenPt.x = popupRc.right + 4;
                    screenPt.y = popupRc.top + 1 + vis * kEditorPopupItemH;
                } else {
                    GetCursorPos(&screenPt);
                }
            } else {
                GetCursorPos(&screenPt);
            }
        }
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int x = screenPt.x;
        int y = screenPt.y;
        const int tipW = static_cast<int>(size.cx);
        const int tipH = static_cast<int>(size.cy);
        const int workLeft = static_cast<int>(work.left);
        const int workTop = static_cast<int>(work.top);
        const int workRight = static_cast<int>(work.right);
        const int workBottom = static_cast<int>(work.bottom);
        if (x + tipW > workRight) x = std::max(workLeft, workRight - tipW);
        if (y + tipH > workBottom) y = std::max(workTop, workBottom - tipH);
        x = std::max(workLeft, x);
        y = std::max(workTop, y);
        SetWindowPos(editorTipPopup_, HWND_TOPMOST, x, y, size.cx, size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
        InvalidateRect(editorTipPopup_, nullptr, FALSE);
    }

    void CloseEditorPopup() {
        const int closedId = editorPopupOpen_;
        if (editorPopupOpen_ >= 0) {
            PopupCombo* pc = GetEditorPopup();
            if (pc) pc->open = false;
        }
        if (closedId == 7) CancelQuickInputTip();
        editorPopupOpen_ = -1;
        editorPopupHover_ = -1;
        editorPopupScroll_ = 0;
        SyncEditorDropPopup();
        InvalidateEditorComboArea(closedId);
        RepaintParamPanelChrome();
    }

    void ToggleEditorPopup(int id) {
        if (editorPopupOpen_ == id) { CloseEditorPopup(); return; }
        const int prevId = editorPopupOpen_;
        if (prevId == 7 || id == 7) CancelQuickInputTip();
        if (prevId >= 0) {
            PopupCombo* prevPc = GetEditorPopup();
            if (prevPc) prevPc->open = false;
            editorPopupOpen_ = -1;
            editorPopupHover_ = -1;
            editorPopupScroll_ = 0;
        }
        editorPopupOpen_ = id;
        editorPopupHover_ = -1;
        editorPopupScroll_ = 0;
        if (id == 5) RefreshRunBlockCombo();
        else if (id == 7) RefreshActiveVarCombo();
        else if (id == 8) RefreshRunMacroCombo();
        else if (id == 9) RefreshMousePlaybackCombo();
        else if (id == 12) RefreshIfVarCombo();
        else if (id == 16) { RefreshOcrSubPanel(); SyncParamScrollLayout(); }
        else if (id == 17) { RefreshOcrSubPanel(); SyncParamScrollLayout(); }
        else if (id == 18) RefreshOcrSearchVarCombo();
        else if (id == 19) RefreshAiModelCombo();
        PopupCombo* pc = GetEditorPopup();
        if (pc) pc->open = true;
        SyncEditorDropPopup();
        if (prevId >= 0) InvalidateEditorComboArea(prevId);
        InvalidateEditorComboArea(id);
        RepaintParamPanelChrome();
    }

    int HitEditorPopupItem(int x, int y) const {
        if (editorPopupOpen_ < 0 || EditorDropPopupVisible()) return -1;
        PopupCombo* pc = const_cast<MainWindow*>(this)->GetEditorPopup();
        if (!pc || !pc->open || pc->items.empty()) return -1;
        RECT popup = EditorPopupListRect();
        if (!PtIn(popup, x, y)) return -1;
        return HitEditorPopupItemLocal(x - popup.left, y - popup.top);
    }

    int HitEditorPopupItemLocal(int, int y) const {
        if (editorPopupOpen_ < 0) return -1;
        PopupCombo* pc = const_cast<MainWindow*>(this)->GetEditorPopup();
        if (!pc || pc->items.empty()) return -1;
        const int visible = EditorPopupVisibleCount();
        const int rel = (y - 1) / kEditorPopupItemH;
        if (rel < 0 || rel >= visible) return -1;
        const int idx = rel + editorPopupScroll_;
        const int total = static_cast<int>(pc->items.size());
        return idx < total ? idx : -1;
    }

    void OnEditorDropPopupWheel(int delta) {
        PopupCombo* pc = GetEditorPopup();
        if (!pc || !pc->open || pc->items.empty()) return;
        const int total = static_cast<int>(pc->items.size());
        const int visible = EditorPopupVisibleCount();
        const int scrollMax = std::max(0, total - visible);
        if (scrollMax <= 0) return;
        const int oldScroll = editorPopupScroll_;
        editorPopupScroll_ = std::clamp(editorPopupScroll_ + (delta < 0 ? 1 : -1), 0, scrollMax);
        if (oldScroll != editorPopupScroll_ && editorDropPopup_) {
            InvalidateRect(editorDropPopup_, nullptr, FALSE);
            if (quickInputTipShown_ == QuickInputTipKind::VariableHelp) SyncQuickInputTipPopup();
            else if (editorPopupOpen_ == 7 && editorPopupHover_ >= 0) BeginQuickInputVarTipHover(editorPopupHover_);
        }
    }

    void SelectEditorPopupItem(int idx) {
        if (editorPopupOpen_ < 0) return;
        PopupCombo* pc = GetEditorPopup();
        if (!pc) return;
        if (idx < 0 || idx >= static_cast<int>(pc->items.size())) { CloseEditorPopup(); return; }
        if (editorPopupOpen_ == 1 && !IsImplementedActionPopup(idx)) {
            MessageBoxW(hwnd_, L"该动作类型暂未实现。", L"提示", MB_OK | MB_ICONINFORMATION);
            CloseEditorPopup();
            return;
        }
        pc->sel = idx;
        HWND label = nullptr;
        switch (editorPopupOpen_) {
        case 0: label = mode_; break;
        case 1: label = actionCombo_; break;
        case 2: label = mousePressButton_; break;
        case 3: label = clickButton_; break;
        case 4: label = loopTypeCombo_; break;
        case 5: label = runBlockCombo_; break;
        case 6: label = hotkeyShortcutCombo_; break;
        case 7: label = ActiveVarComboHwnd(); break;
        case 8: label = runMacroCombo_; break;
        case 9: label = mousePlaybackCombo_; break;
        case 10: label = scrollDirectionCombo_; break;
        case 11: label = findFollowUpCombo_; break;
        case 12: label = ifVarCombo_; break;
        case 13: label = ifOperatorCombo_; break;
        case 14: label = ifConnectorCombo_; break;
        case 15: label = runProgramCombo_; break;
        case 16: label = ocrResultModeCombo_; break;
        case 17: label = ocrFollowUpCombo_; break;
        case 18: label = ocrSearchVarCombo_; break;
        case 19: label = aiModelCombo_; break;
        case 20: label = aiContextModeCombo_; break;
        case 21: label = aiOutputTypeCombo_; break;
        case 22: label = aiSearchRegionCombo_; break;
        }
        if (label) SetText(label, pc->items[static_cast<size_t>(idx)]);
        if (editorPopupOpen_ == 1) RefreshParamPanel();
        else if (editorPopupOpen_ == 11) RefreshFindImageSubPanel();
        else if (editorPopupOpen_ == 16) { RefreshOcrSubPanel(); SyncParamScrollLayout(); }
        else if (editorPopupOpen_ == 17) { RefreshOcrSubPanel(); SyncParamScrollLayout(); }
        else if (editorPopupOpen_ == 15) UpdateRunProgramSubPanel();
        const int closedId = editorPopupOpen_;
        pc->open = false;
        editorPopupOpen_ = -1;
        editorPopupHover_ = -1;
        SyncEditorDropPopup();
        InvalidateEditorComboArea(closedId);
        RepaintParamPanelChrome();
    }

    bool HandleEditorPopupClick(int x, int y) {
        if (editorPopupOpen_ < 0) return false;
        int idx = HitEditorPopupItem(x, y);
        if (idx >= 0) { SelectEditorPopupItem(idx); return true; }
        RECT anchor = EditorPopupRect();
        if (!PtIn(anchor, x, y)) {
            CloseEditorPopup();
            return true;
        }
        return false;
    }

    void FillAlphaRect(HDC hdc, RECT rc, COLORREF color, BYTE alpha) {
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return;
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        RECT local{0, 0, w, h};
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(mem, &local, brush);
        DeleteObject(brush);
        BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, 0};
        AlphaBlend(hdc, rc.left, rc.top, w, h, mem, 0, 0, w, h, blend);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    bool IsHotkeyMenuChecked(int id) const {
        if (id == kHotF8) return globalHotkey_.vk == VK_F8 && globalHotkey_.modifiers == 0;
        if (id == kHotF10) return globalHotkey_.vk == VK_F10 && globalHotkey_.modifiers == 0;
        if (id == kHotLeft) return globalHotkey_.vk == VK_LBUTTON && globalHotkey_.modifiers == 0;
        if (id == kHotMiddle) return globalHotkey_.vk == VK_MBUTTON && globalHotkey_.modifiers == 0;
        if (id == kHotRight) return globalHotkey_.vk == VK_RBUTTON && globalHotkey_.modifiers == 0;
        if (id == kHotX1) return globalHotkey_.vk == VK_XBUTTON1 && globalHotkey_.modifiers == 0;
        if (id == kHotX2) return globalHotkey_.vk == VK_XBUTTON2 && globalHotkey_.modifiers == 0;
        if (id == kHotSpace) return globalHotkey_.vk == VK_SPACE && globalHotkey_.modifiers == 0;
        return id == kHotCustom && globalHotkey_.enabled && !IsHotkeyMenuChecked(kHotF8) && !IsHotkeyMenuChecked(kHotF10) && !IsHotkeyMenuChecked(kHotLeft) && !IsHotkeyMenuChecked(kHotMiddle) && !IsHotkeyMenuChecked(kHotRight) && !IsHotkeyMenuChecked(kHotX1) && !IsHotkeyMenuChecked(kHotX2) && !IsHotkeyMenuChecked(kHotSpace);
    }

    void MeasureOwnerItem(MEASUREITEMSTRUCT* mis) {
        if (!mis) return;
        if (mis->CtlType == ODT_COMBOBOX) {
            mis->itemHeight = kComboItemH;
            return;
        }
        if (mis->CtlType == ODT_MENU) {
            mis->itemWidth = 293;
            mis->itemHeight = 45;
        }
    }

    void DrawComboItem(DRAWITEMSTRUCT* dis) {
        if (!dis || dis->itemID == static_cast<UINT>(-1)) return;
        const bool inList = (dis->itemState & ODS_COMBOBOXEDIT) == 0;
        const int curSel = ComboBox_GetCurSel(dis->hwndItem);
        const bool isCurSel = inList && static_cast<int>(dis->itemID) == curSel;
        bool highlighted = false;
        if (inList && !isCurSel) {
            COMBOBOXINFO cbi{sizeof(cbi)};
            if (GetComboBoxInfo(dis->hwndItem, &cbi) && cbi.hwndList) {
                auto it = g_comboListHover.find(cbi.hwndList);
                highlighted = (it != g_comboListHover.end() && it->second == static_cast<int>(dis->itemID));
            }
        }
        COLORREF bg = kWhite;
        COLORREF fg = inList ? kText : kMainGreen;
        if (isCurSel) {
            bg = kComboMenuSelectBlue;
            fg = kComboMenuSelectText;
        } else if (highlighted) {
            bg = kComboMenuHoverBlue;
            fg = kText;
        }
        RECT rc = dis->rcItem;
        HBRUSH brush = CreateSolidBrush(bg);
        FillRect(dis->hDC, &rc, brush);
        DeleteObject(brush);
        wchar_t text[256]{};
        SendMessageW(dis->hwndItem, CB_GETLBTEXT, dis->itemID, reinterpret_cast<LPARAM>(text));
        SelectObject(dis->hDC, font_);
        DrawTextIn(dis->hDC, text, RECT{rc.left + 8, rc.top, rc.right - 8, rc.bottom}, fg);
    }

    void DrawHotkeyMenuItem(DRAWITEMSTRUCT* dis) {
        if (!dis) return;
        auto* item = reinterpret_cast<HotkeyMenuItem*>(dis->itemData);
        if (!item) return;
        const bool checked = IsHotkeyMenuChecked(item->id);
        const bool selected = (dis->itemState & ODS_SELECTED) != 0;
        RECT rc = dis->rcItem;
        HBRUSH bg = CreateSolidBrush((checked || selected) ? RGB(232, 245, 238) : RGB(255, 255, 255));
        FillRect(dis->hDC, &rc, bg);
        DeleteObject(bg);

        RECT box{rc.left + 11, rc.top + 12, rc.left + 23, rc.top + 24};
        HPEN boxPen = CreatePen(PS_SOLID, 1, checked ? kMainGreen : RGB(190, 190, 190));
        HBRUSH boxBrush = CreateSolidBrush(checked ? kMainGreen : RGB(255, 255, 255));
        HGDIOBJ oldPen = SelectObject(dis->hDC, boxPen);
        HGDIOBJ oldBrush = SelectObject(dis->hDC, boxBrush);
        RoundRect(dis->hDC, box.left, box.top, box.right, box.bottom, 3, 3);
        SelectObject(dis->hDC, oldPen);
        SelectObject(dis->hDC, oldBrush);
        DeleteObject(boxPen);
        DeleteObject(boxBrush);
        if (checked) DrawTextIn(dis->hDC, L"✓", box, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(dis->hDC, font_);
        DrawTextIn(dis->hDC, item->title, RECT{rc.left + 46, rc.top + 3, rc.right - 10, rc.top + 22}, RGB(30, 30, 30));
        DrawTextIn(dis->hDC, item->desc, RECT{rc.left + 46, rc.top + 23, rc.right - 10, rc.bottom - 3}, RGB(145, 150, 155));
    }

    void DrawOwnerItem(DRAWITEMSTRUCT* dis) {
        if (!dis) return;
        if (dis->CtlType == ODT_MENU) { DrawHotkeyMenuItem(dis); return; }
        if (dis->CtlType == ODT_COMBOBOX) { DrawComboItem(dis); return; }
        if (crosshairDrag_.IsCrosshairButton(dis->hwndItem)) {
            crosshairDrag_.DrawButton(dis, font_, hoverCrosshairBtn_);
            return;
        }
        if (EditorComboPopupIdForHwnd(dis->hwndItem) >= 0) {
            FillRectColor(dis->hDC, dis->rcItem, kWhite);
            return;
        }
        if (IsGrayButton(dis->hwndItem)) { DrawGrayButton(dis); return; }
        DrawOwnerButton(dis);
    }

    bool IsGrayButton(HWND hwnd) const {
        return hwnd == findFullScreenBtn_ || hwnd == findSelectRegionBtn_ || hwnd == findTestBtn_
            || hwnd == findImagePreviewBtn_
            || hwnd == findScreenshotBtn_ || hwnd == findLocalImageBtn_ || hwnd == findClearImageBtn_
            || hwnd == findSelectOffsetBtn_
            || hwnd == ocrDepInstallBtn_
            || hwnd == ocrFullScreenBtn_ || hwnd == ocrSelectRegionBtn_ || hwnd == ocrTestBtn_
            || hwnd == ocrSelectOffsetBtn_
            || hwnd == ocrFindSelectRegionBtn_ || hwnd == ocrFindImagePreviewBtn_
            || hwnd == ocrFindScreenshotBtn_ || hwnd == ocrFindLocalImageBtn_ || hwnd == ocrFindClearImageBtn_
            || hwnd == aiFindSelectRegionBtn_ || hwnd == aiFindImagePreviewBtn_
            || hwnd == aiFindScreenshotBtn_ || hwnd == aiFindLocalImageBtn_ || hwnd == aiFindClearImageBtn_
            || hwnd == aiFullScreenBtn_ || hwnd == aiFullScreenBtn2_
            || hwnd == aiSelectRegionBtn_ || hwnd == aiSelectRegionBtn2_;
    }

    HWND HitGrayButton(int x, int y) const {
        if (page_ != Page::Editor) return nullptr;
        auto testChild = [&](HWND child) -> HWND {
            if (!IsGrayButton(child) || !IsWindowVisible(child)) return nullptr;
            if (PtIn(WindowClientRect(child), x, y)) return child;
            return nullptr;
        };
        for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (HWND hit = testChild(child)) return hit;
        }
        if (paramViewport_) {
            for (HWND child = GetWindow(paramViewport_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                if (HWND hit = testChild(child)) return hit;
            }
        }
        return nullptr;
    }

    void InvalidateGrayButton(HWND btn) {
        if (!btn) return;
        InvalidateRect(btn, nullptr, FALSE);
    }

    void UpdateGrayButtonHover(int x, int y) {
        HWND hit = HitGrayButton(x, y);
        if (hit == hoverGrayBtn_) return;
        HWND old = hoverGrayBtn_;
        hoverGrayBtn_ = hit;
        pendingHoverGrayOld_ = old;
        pendingHoverGrayNew_ = hit;
    }

    void FlushGrayButtonHover() {
        if (!pendingHoverGrayOld_ && !pendingHoverGrayNew_) return;
        InvalidateGrayButton(pendingHoverGrayOld_);
        InvalidateGrayButton(pendingHoverGrayNew_);
        pendingHoverGrayOld_ = nullptr;
        pendingHoverGrayNew_ = nullptr;
    }

    void ClearGrayButtonHover() {
        if (!hoverGrayBtn_) return;
        HWND old = hoverGrayBtn_;
        hoverGrayBtn_ = nullptr;
        InvalidateGrayButton(old);
    }

    bool IsGreenButtonHovered(HWND hwnd) const {
        if (IsGrayButton(hwnd)) return hwnd == hoverGrayBtn_;
        if (crosshairDrag_.IsCrosshairButton(hwnd)) return false;
        if (hwnd == batchDeleteBtn_ || hwnd == batchCopyBtn_) {
            if (BatchSelectedCount() == 0) return false;
            return hwnd == batchDeleteBtn_ && hoverButton_ == HoverButton::BatchDelete ||
                   hwnd == batchCopyBtn_ && hoverButton_ == HoverButton::BatchCopy;
        }
        return hwnd == loadBtn_ && hoverButton_ == HoverButton::Load ||
               hwnd == clearBtn_ && hoverButton_ == HoverButton::Clear ||
               hwnd == addBtn_ && hoverButton_ == HoverButton::Add ||
               hwnd == modifyBtn_ && hoverButton_ == HoverButton::Modify ||
               hwnd == cancelBtn_ && hoverButton_ == HoverButton::Cancel ||
               hwnd == saveBtn_ && hoverButton_ == HoverButton::Save ||
               hwnd == batchExitBtn_ && hoverButton_ == HoverButton::BatchExit ||
               hwnd == batchSelectAllBtn_ && hoverButton_ == HoverButton::BatchSelectAll ||
               hwnd == batchDeselectBtn_ && hoverButton_ == HoverButton::BatchDeselect;
    }

    bool IsOwnerDrawButtonDisabled(HWND hwnd) const {
        return (hwnd == batchDeleteBtn_ || hwnd == batchCopyBtn_) && BatchSelectedCount() == 0;
    }

    void DrawGrayButton(DRAWITEMSTRUCT* dis) {
        if (!dis) return;
        HDC hdc = dis->hDC;
        RECT rc = dis->rcItem;
        const bool hovered = dis->hwndItem == hoverGrayBtn_;
        const COLORREF fill = hovered ? kGrayButtonHover : kGrayButton;
        const COLORREF border = hovered ? kMainGreen : kGrayButtonBorder;
        HBRUSH brush = CreateSolidBrush(fill);
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);
        if (dis->hwndItem == findImagePreviewBtn_ || dis->hwndItem == ocrFindImagePreviewBtn_
            || dis->hwndItem == aiFindImagePreviewBtn_) {
            HBITMAP preview = dis->hwndItem == findImagePreviewBtn_
                ? findImagePreviewBitmap_
                : (dis->hwndItem == ocrFindImagePreviewBtn_ ? ocrFindImagePreviewBitmap_ : aiFindImagePreviewBitmap_);
            if (preview) {
                BITMAP bm{};
                GetObjectW(preview, sizeof(bm), &bm);
                if (bm.bmWidth > 0 && bm.bmHeight > 0) {
                    const int pad = 4;
                    RECT imgRc = rc;
                    imgRc.left += pad;
                    imgRc.top += pad;
                    imgRc.right -= pad;
                    imgRc.bottom -= pad;
                    HDC mem = CreateCompatibleDC(hdc);
                    HGDIOBJ oldBmp = SelectObject(mem, preview);
                    SetStretchBltMode(hdc, HALFTONE);
                    StretchBlt(hdc, imgRc.left, imgRc.top, imgRc.right - imgRc.left, imgRc.bottom - imgRc.top,
                        mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                    SelectObject(mem, oldBmp);
                    DeleteDC(mem);
                }
            }
            DrawBorderRect(hdc, rc, border);
            return;
        }
        wchar_t text[64]{};
        GetWindowTextW(dis->hwndItem, text, 64);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kGrayButtonText);
        SelectObject(hdc, editorFont_ ? editorFont_ : font_);
        DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawBorderRect(hdc, rc, border);
    }

    void DrawOwnerButton(DRAWITEMSTRUCT* dis) {
        if (!dis) return;
        HDC hdc = dis->hDC;
        RECT rc = dis->rcItem;
        const bool disabled = IsOwnerDrawButtonDisabled(dis->hwndItem);
        const bool pressed = !disabled && (dis->itemState & ODS_SELECTED) != 0;
        const bool hovered = !disabled && IsGreenButtonHovered(dis->hwndItem);
        const COLORREF fill = disabled ? kButtonDisabledGreen : (pressed || hovered ? kButtonGreenHover : kButtonGreen);
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, fill);
        HGDIOBJ oldBrush = SelectObject(hdc, brush);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 5, 5);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(brush);
        DeleteObject(pen);
        wchar_t text[64]{};
        GetWindowTextW(dis->hwndItem, text, 64);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, disabled ? kButtonDisabledText : kWhite);
        SelectObject(hdc, font_);
        DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // ── WM_PAINT entry point ───────────────────────────────────────
    void Paint() {
        PAINTSTRUCT ps{}; HDC windowDc = BeginPaint(hwnd_, &ps); RECT rc{}; GetClientRect(hwnd_, &rc);
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        HDC hdc = CreateCompatibleDC(windowDc);
        HBITMAP bmp = CreateCompatibleBitmap(windowDc, w, h);
        HGDIOBJ oldBmp = SelectObject(hdc, bmp);
        HBRUSH green = CreateSolidBrush(kMainGreen); HBRUSH white = CreateSolidBrush(kWhite); HBRUSH panel = CreateSolidBrush(kPanel);
        RECT title{0, 0, rc.right, kTitleH}; FillRect(hdc, &title, green);
        RECT body{0, kTitleH, rc.right, rc.bottom - (page_ == Page::Editor ? kBottomH : 0)}; FillRect(hdc, &body, page_ == Page::Home ? green : white);
        if (page_ == Page::Editor) { RECT bottom{0, rc.bottom - kBottomH, rc.right, rc.bottom}; FillRect(hdc, &bottom, panel); }
        SelectObject(hdc, page_ == Page::Editor ? editorFont_ : titleFont_);
        if (page_ == Page::Editor) DrawTextIn(hdc, L"◴ 鼠大侠-鼠标宏", RECT{14, 0, 360, kTitleH}, kWhite);
        SelectObject(hdc, font_);
        if (page_ == Page::Home) {
            PaintHome(hdc);
            if (deleteConfirmVisible_) PaintDeleteConfirm(hdc);
        } else {
            PaintEditor(hdc);
        }
        DrawTitleButtons(hdc);
        const int blitW = ps.rcPaint.right - ps.rcPaint.left;
        const int blitH = ps.rcPaint.bottom - ps.rcPaint.top;
        // 不 BitBlt 到子控件区域，避免覆盖 EDIT/COMBO 等已绘制内容
        for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!IsWindowVisible(child)) continue;
            RECT childRc = PaintExcludeRectForChild(child);
            if (childRc.right <= childRc.left || childRc.bottom <= childRc.top) continue;
            RECT inter{};
            if (!IntersectRect(&inter, &childRc, &ps.rcPaint)) continue;
            ExcludeClipRect(windowDc, childRc.left, childRc.top, childRc.right, childRc.bottom);
        }
        BitBlt(windowDc, ps.rcPaint.left, ps.rcPaint.top, blitW, blitH, hdc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(hdc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(hdc);
        DeleteObject(green); DeleteObject(white); DeleteObject(panel);
        EndPaint(hwnd_, &ps);
        if (page_ == Page::Editor) {
            RepaintParamPanelChrome();
            HDC chromeDc = GetDC(hwnd_);
            PaintEditorListHeaderChrome(chromeDc);
            ReleaseDC(hwnd_, chromeDc);
        }
    }

    // ── Home screen painting ───────────────────────────────────────
    void PaintHome(HDC hdc) {
        DrawHomeShell(hdc);
        if (activeHomeTab_ == quickscript::MainTab::Clicker) {
            PaintClickerHome(hdc);
        } else if (activeHomeTab_ == quickscript::MainTab::Recorder) {
            PaintRecorderHome(hdc);
        } else if (activeHomeTab_ == quickscript::MainTab::Macro) {
            PaintMacroHome(hdc);
        } else {
            PaintScriptCustomHome(hdc);
        }
    }

    void PaintClickerHome(HDC hdc) {
        UpdateClickerLayout();
        SelectObject(hdc, homeFont_);
        DrawTextIn(hdc, L"点击类型:", RECT{kClickerLabelX, 169, clickerLayout_.leftRadioLeft - kClickerFieldGap, 194}, kWhite);
        DrawRadio(hdc, ClickerLeftRadioRect(), clickerSettings_.button == quickscript::MouseButtonChoice::Left);
        DrawTextIn(hdc, L"鼠标左键", RECT{clickerLayout_.leftRadioLeft + kClickerRadioSize + 9, 169,
            clickerLayout_.middleRadioLeft - 17, 194}, kWhite);
        DrawRadio(hdc, ClickerMiddleRadioRect(), clickerSettings_.button == quickscript::MouseButtonChoice::Middle);
        DrawTextIn(hdc, L"鼠标中键", RECT{clickerLayout_.middleRadioLeft + kClickerRadioSize + 9, 169,
            clickerLayout_.rightRadioLeft - 17, 194}, kWhite);
        DrawRadio(hdc, ClickerRightRadioRect(), clickerSettings_.button == quickscript::MouseButtonChoice::Right);
        DrawTextIn(hdc, L"鼠标右键", RECT{clickerLayout_.rightRadioLeft + kClickerRadioSize + 9, 169, kClickerComboRight, 194}, kWhite);

        DrawTextIn(hdc, L"每次点击间隔时间:", RECT{kClickerLabelX, 238, clickerLayout_.intervalComboLeft - kClickerFieldGap, 263}, kWhite);
        DrawClickerCombo(hdc, ClickerIntervalRect(), ClickerIntervalPopupOpen());
        DrawTextIn(hdc, L"启停的全局热键:", RECT{kClickerLabelX, 305, clickerLayout_.hotkeyComboLeft - kClickerFieldGap, 330}, kWhite);
        DrawClickerCombo(hdc, ClickerHotkeyRect(), globalHotkey_.text.empty() ? L"F8" : globalHotkey_.text, ClickerHotkeyPopupOpen());

        HBRUSH y = CreateSolidBrush(clicking_ ? RGB(255, 200, 200) : kCreateYellow);
        RECT hint = CreateRect();
        FillRect(hdc, &hint, y);
        DeleteObject(y);
        HBRUSH tagBrush = CreateSolidBrush(clicking_ ? RGB(220, 60, 60) : RGB(255, 174, 42));
        RECT tag{557, hint.top, hint.right, hint.top + 35};
        FillRect(hdc, &tag, tagBrush);
        DeleteObject(tagBrush);
        SelectObject(hdc, bigFont_);
        const std::wstring hotText = globalHotkey_.text.empty() ? L"F8" : globalHotkey_.text;
        RECT keyBox = ClickerBannerKeyRect();
        DrawTextIn(hdc, L"按", RECT{hint.left + 166, hint.top, keyBox.left - 5, hint.bottom}, RGB(60, 60, 60), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        HBRUSH orange = CreateSolidBrush(clicking_ ? RGB(200, 50, 50) : kOrange);
        FillRect(hdc, &keyBox, orange);
        DeleteObject(orange);
        DrawTextIn(hdc, hotText, keyBox, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        const std::wstring actionText = clicking_
            ? (L"键停止 连点（当前 " + ClickerButtonTitle() + L" " + ClickIntervalTitle(clickerSettings_.intervalMode) + L"）")
            : (L"键开始 " + ClickerButtonTitle() + L" 连点");
        DrawTextIn(hdc, actionText, RECT{keyBox.right + 10, hint.top, hint.right - 20, hint.bottom}, clicking_ ? RGB(180, 40, 40) : RGB(60, 60, 60), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, titleFont_);
        DrawTextIn(hdc, clicking_ ? L"连点中…" : L"超级连点", tag, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, homeFont_);
    }

    void DrawRecorderEmptyIcon(HDC hdc) {
        HPEN pen = CreatePen(PS_SOLID, 8, kWhite);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, 288, 226, 430, 308, 40, 40);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
        HBRUSH dot = CreateSolidBrush(RGB(222, 245, 229));
        oldBrush = SelectObject(hdc, dot);
        oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, 344, 252, 374, 282);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(dot);
    }

    void PaintRecorderHome(HDC hdc) {
        DrawTopAction(hdc, ImportRect(), L"导入", 0);
        DrawTopAction(hdc, ExportRect(), L"导出", 1);
        DrawTopAction(hdc, TimerRect(), L"定时", 2);

        if (recordings_.empty() && !recording_) {
            DrawRecorderEmptyIcon(hdc);
            SelectObject(hdc, homeFont_);
            DrawTextIn(hdc, L"没有已录制记录，按下面的提示开始录制吧", RECT{60, 320, 660, 350}, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            int saved = SaveDC(hdc);
            RECT list = HomeListRect();
            IntersectClipRect(hdc, list.left, list.top, list.right, list.bottom);
            for (int i = 0; i < static_cast<int>(recordings_.size()); ++i) {
                RECT r = RecordingCardRect(i);
                if (r.bottom < list.top || r.top > list.bottom) continue;
                COLORREF cardColor = i == selectedRecording_ ? kDarkGreen : (i == recordingHover_ ? kCardHoverGreen : kCardGreen);
                HBRUSH b = CreateSolidBrush(cardColor); FillRect(hdc, &r, b); DeleteObject(b);
                RECT hotRc = RecordingHotkeyRect(i);
                SelectObject(hdc, homeFont_);
                DrawTextIn(hdc, recordings_[static_cast<size_t>(i)].name, RECT{r.left + 14, r.top + 17, r.right - 100, r.top + 48}, kWhite, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(hdc, hotFont_);
                DrawTextIn(hdc, RecordingHotkeyText(recordings_[static_cast<size_t>(i)]), hotRc, kWhite);
                SelectObject(hdc, homeFont_);
                DrawTextIn(hdc, L"录制时间: " + recordings_[static_cast<size_t>(i)].recordTime, RECT{r.left + 14, r.top + 58, r.left + 350, r.top + 88}, RGB(220, 245, 225));
                DrawTextIn(hdc, L"时长: " + FormatDuration(recordings_[static_cast<size_t>(i)].durationSeconds), RECT{r.left + 344, r.top + 58, r.left + 510, r.top + 88}, RGB(220, 245, 225));
                if (i == selectedRecording_) {
                    DrawTextIn(hdc, L"取消选择", RecordingDeselectRect(i), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    HBRUSH selBrush = CreateSolidBrush(kSelectedYellow);
                    RECT tag = RecordingSelectedTagRect(i);
                    FillRect(hdc, &tag, selBrush);
                    DeleteObject(selBrush);
                    DrawTextIn(hdc, L"已选中⌄", tag, kMainGreen, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                } else {
                    DrawTextIn(hdc, L"优化", RecordingOptimizeRect(i), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    DrawTextIn(hdc, L"重命名", RecordingRenameRect(i), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    DrawTextIn(hdc, L"删除", RecordingDeleteRect(i), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
            }
            RestoreDC(hdc, saved);
            if (MaxRecordingScroll() > 0) PaintHomeScrollbar(hdc);
        }

        HBRUSH y = CreateSolidBrush(recording_ ? RGB(255, 230, 230) : kCreateYellow);
        RECT hint = CreateRect();
        FillRect(hdc, &hint, y);
        DeleteObject(y);
        HBRUSH tagBrush = CreateSolidBrush(RGB(255, 174, 42));
        RECT modeTag = RecorderWindowModeRect();
        FillRect(hdc, &modeTag, tagBrush);
        DeleteObject(tagBrush);
        SelectObject(hdc, titleFont_);
        DrawTextIn(hdc, recorderSettings_.captureScope == quickscript::RecordCaptureScope::Window ? L"窗口模式 ○" : L"全局模式 ○", modeTag, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, bigFont_);
        const std::wstring hotText = globalHotkey_.text.empty() ? L"F8" : globalHotkey_.text;
        RECT keyBox = RecorderBannerKeyRect();
        DrawTextIn(hdc, L"按", RECT{hint.left + 166, hint.top, keyBox.left - 5, hint.bottom}, recording_ ? RGB(180, 40, 40) : RGB(60, 60, 60), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        HBRUSH orange = CreateSolidBrush(recording_ ? RGB(220, 60, 60) : kOrange);
        FillRect(hdc, &keyBox, orange);
        DeleteObject(orange);
        DrawTextIn(hdc, hotText, keyBox, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        std::wstring actionText;
        if (recording_) actionText = L"键停止 录制";
        else if (selectedRecording_ >= 0) actionText = L"键开始 回放";
        else actionText = L"键开始 录制";
        DrawTextIn(hdc, actionText, RECT{keyBox.right + 10, hint.top, hint.right - 160, hint.bottom}, recording_ ? RGB(180, 40, 40) : RGB(60, 60, 60), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, homeFont_);
        DrawTextIn(hdc, L"前往修改全局启停热键   在录制列表，您也可以为您的录制设置单独的热键", RECT{36, 468, 700, 492}, RGB(210, 245, 215));
    }

    void PaintScriptCustomHome(HDC hdc) {
        // 标题
        SelectObject(hdc, bigFont_);
        DrawTextIn(hdc, L"AI 脚本助手", RECT{0, 170, kHomeWidth, 210}, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, homeFont_);
        DrawTextIn(hdc, L"通过自然语言描述需求，AI 将自动生成或修改自动化脚本。", RECT{0, 218, kHomeWidth, 248}, RGB(210, 245, 215), DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // AI 入口按钮（黄底黑字，与鼠标宏主界面一致）
        RECT hint = CreateRect();
        HBRUSH y = CreateSolidBrush(kCreateYellow);
        FillRect(hdc, &hint, y);
        DeleteObject(y);

        SelectObject(hdc, bigFont_);
        DrawTextIn(hdc, L"开始 AI 对话", RECT{hint.left, hint.top + 21, hint.right, hint.bottom - 21},
            RGB(60, 60, 60), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        // 底部提示
        SelectObject(hdc, homeFont_);
        DrawTextIn(hdc, L"点击上方按钮启动 AI 脚本助手，支持列表/读取/写入脚本", RECT{0, hint.bottom + 16, kHomeWidth, hint.bottom + 44}, RGB(210, 245, 215), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    void PaintMacroHome(HDC hdc) {
        DrawTopAction(hdc, ImportRect(), L"导入", 0);
        DrawTopAction(hdc, ExportRect(), L"导出", 1);
        DrawTopAction(hdc, TimerRect(), L"定时", 2);
        int saved = SaveDC(hdc);
        RECT list = HomeListRect();
        IntersectClipRect(hdc, list.left, list.top, list.right, list.bottom);
        for (int i = 0; i < static_cast<int>(scripts_.size()); ++i) {
            RECT r = HomeCardRect(i);
            if (r.bottom < list.top || r.top > list.bottom) continue;
            COLORREF cardColor = i == selectedScript_ ? kDarkGreen : (i == homeHover_ ? kCardHoverGreen : kCardGreen);
            HBRUSH b = CreateSolidBrush(cardColor); FillRect(hdc, &r, b); DeleteObject(b);
            RECT hotRc = ScriptHotkeyRect(i);
            SelectObject(hdc, homeFont_);
            DrawTextIn(hdc, scripts_[static_cast<size_t>(i)].name, RECT{r.left + 14, r.top + 17, r.right - 100, r.top + 48}, kWhite, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, hotFont_);
            DrawTextIn(hdc, ScriptHotkeyText(scripts_[static_cast<size_t>(i)]), hotRc, kWhite);
            SelectObject(hdc, homeFont_);
            DrawTextIn(hdc, L"录制时间: " + scripts_[static_cast<size_t>(i)].recordTime, RECT{r.left + 14, r.top + 58, r.left + 350, r.top + 88}, RGB(220, 245, 225));
            DrawTextIn(hdc, L"动作数: " + std::to_wstring(scripts_[static_cast<size_t>(i)].actionCount), RECT{r.left + 344, r.top + 58, r.left + 510, r.top + 88}, RGB(220, 245, 225));
            DrawTextIn(hdc, i == selectedScript_ ? L"取消选择" : L"编辑", RECT{r.right - 96, r.top + 14, r.right - 24, r.top + 45}, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextIn(hdc, L"删除", RECT{r.right - 96, r.top + 56, r.right - 24, r.top + 88}, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (i == selectedScript_) { HBRUSH y = CreateSolidBrush(kSelectedYellow); RECT tag{r.right - 96, r.top + 58, r.right - 18, r.top + 95}; FillRect(hdc, &tag, y); DeleteObject(y); DrawTextIn(hdc, L"已选中⌄", tag, kMainGreen, DT_CENTER | DT_VCENTER | DT_SINGLELINE); }
        }
        RestoreDC(hdc, saved);
        PaintHomeScrollbar(hdc);
        HBRUSH y = CreateSolidBrush(kCreateYellow); RECT cr = CreateRect(); FillRect(hdc, &cr, y); DeleteObject(y);
        SelectObject(hdc, bigFont_);
        if (selectedScript_ >= 0) {
            const std::wstring prefix = L"按";
            const std::wstring suffix = L"键开始 运行宏";
            RECT hot = CommonHotRect();
            const int gap = 20;
            const int prefixW = TextWidth(prefix, bigFont_);
            const int suffixW = TextWidth(suffix, bigFont_);
            RECT prefixRc{hot.left - gap - prefixW, cr.top, hot.left - gap, cr.bottom};
            RECT suffixRc{hot.right + gap, cr.top, hot.right + gap + suffixW, cr.bottom};
            DrawTextIn(hdc, prefix, prefixRc, RGB(60, 60, 60), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            HBRUSH o = CreateSolidBrush(kOrange); FillRect(hdc, &hot, o); DeleteObject(o);
            DrawTextIn(hdc, globalHotkey_.text, hot, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            DrawTextIn(hdc, suffix, suffixRc, RGB(60, 60, 60), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            DrawTextIn(hdc, L"点击", RECT{cr.left + 205, cr.top, cr.left + 270, cr.bottom}, RGB(60, 60, 60), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            RECT createWord = CreateWordRect(); HBRUSH o = CreateSolidBrush(kOrange); FillRect(hdc, &createWord, o); DeleteObject(o); DrawTextIn(hdc, L"创建", createWord, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextIn(hdc, L"鼠标宏", RECT{cr.left + 348, cr.top, cr.left + 455, cr.bottom}, RGB(60, 60, 60), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(hdc, homeFont_); DrawTextIn(hdc, L"前往修改全局启停热键   在宏列表中，您也可以为您的宏设置单独热键", RECT{36, 468, 700, 492}, RGB(210, 245, 215));
    }

    void PaintHomeScrollbar(HDC hdc) {
        if (MaxHomeScroll() <= 0) return;
        RECT track = HomeScrollTrackRect();
        HBRUSH trackBrush = CreateSolidBrush(RGB(52, 143, 84));
        HGDIOBJ oldBrush = SelectObject(hdc, trackBrush);
        HPEN nullPen = CreatePen(PS_NULL, 0, 0);
        HGDIOBJ oldPen = SelectObject(hdc, nullPen);
        RoundRect(hdc, track.left, track.top, track.right, track.bottom, 10, 10);
        SelectObject(hdc, oldBrush);
        DeleteObject(trackBrush);
        RECT thumb = HomeScrollThumbRect();
        HBRUSH thumbBrush = CreateSolidBrush(RGB(41, 120, 72));
        oldBrush = SelectObject(hdc, thumbBrush);
        RoundRect(hdc, thumb.left, thumb.top, thumb.right, thumb.bottom, 10, 10);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(thumbBrush);
        DeleteObject(nullPen);
    }

    void PaintDeleteConfirm(HDC hdc) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        FillAlphaRect(hdc, rc, RGB(0, 0, 0), 145);
        RECT d = DeleteDialogRect();
        FillRectColor(hdc, d, RGB(18, 18, 18));
        DrawBorderRoundRect(hdc, d, RGB(70, 70, 70), 6);

        std::wstring name = pendingDeleteIndex_ >= 0 && pendingDeleteIndex_ < static_cast<int>(scripts_.size()) ? scripts_[static_cast<size_t>(pendingDeleteIndex_)].name : L"";
        SelectObject(hdc, titleFont_);
        DrawTextIn(hdc, L"您确定要删除宏 \"" + name + L"\"\n吗？", RECT{d.left + 32, d.top + 34, d.right - 32, d.top + 106}, RGB(255, 226, 110), DT_LEFT | DT_TOP | DT_WORDBREAK);

        RECT ok = DeleteOkRect();
        RECT cancel = DeleteCancelRect();
        FillRectColor(hdc, ok, RGB(255, 188, 75));
        DrawBorderRect(hdc, cancel, RGB(255, 205, 95));
        SelectObject(hdc, font_);
        DrawTextIn(hdc, L"确定", ok, RGB(70, 45, 10), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(hdc, L"取消", cancel, RGB(255, 226, 110), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // ── Editor screen painting ─────────────────────────────────────
    void PaintEditor(HDC hdc);
    void PaintEditorParamChrome(HDC hdc, HWND hdcWindow = nullptr);
    void PaintEditorListHeaderChrome(HDC hdc);

    void PaintEditorTipPopupContent(HDC hdc, HWND popupHwnd);

    SIZE MeasureQuickInputTipSize() const;

    void DrawEditorFieldBorder(HDC hdc, HWND ctrl, HWND hdcWindow = nullptr);
    bool IsParamViewportChild(HWND h) const { return paramViewport_ && h && GetParent(h) == paramViewport_; }
    RECT MapRectFromMain(HWND to, const RECT& rc) const {
        if (!to || to == hwnd_) return rc;
        RECT r = rc;
        MapWindowPoints(hwnd_, to, reinterpret_cast<POINT*>(&r), 2);
        return r;
    }

    void PaintActionList(HDC hdc);

    void PaintDragMarker(HDC hdc);

    LRESULT OnCtlColor(HDC hdc) { SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, kText); return reinterpret_cast<LRESULT>(whiteBrush_); }

    LRESULT OnCtlColorStatic(HDC hdc, HWND ctrl) {
        if (ctrl == paramBottomMask_) {
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, kPanel);
            return reinterpret_cast<LRESULT>(panelBrush_);
        }
        if (ctrl == paramTopMask_ || ctrl == paramRightMask_) {
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, kWhite);
            return reinterpret_cast<LRESULT>(whiteBrush_);
        }
        if (page_ == Page::Editor && ctrl && EditorComboPopupIdForHwnd(ctrl) < 0) {
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, kWhite);
            SetTextColor(hdc, kText);
            return reinterpret_cast<LRESULT>(whiteBrush_);
        }
        return OnCtlColor(hdc);
    }
    LRESULT OnEditColor(HDC hdc) { SetBkMode(hdc, OPAQUE); SetTextColor(hdc, kText); SetBkColor(hdc, kWhite); return reinterpret_cast<LRESULT>(whiteBrush_); }

    void AppendDebugLog(const std::wstring& text) {
        if (!appSettings_.playback.autoOutputKeyFunctionDebug || !macroDebugWindow_.IsCreated()) return;
        macroDebugWindow_.AppendLog(text);
    }

    void AppendAiDebugLog(const std::wstring& text) {
        if (!macroDebugWindow_.IsCreated()) return;
        macroDebugWindow_.AppendLog(text);
    }

    void StoreAiOutputVar(const std::wstring& outputVarName, int outputType,
        const std::wstring& textResult, const std::wstring& fallback) {
        if (!textResult.empty()) {
            if (outputType == 1) {
                double num = 0;
                try { num = std::stod(textResult); }
                catch (...) { num = 0; }
                aiVars_[outputVarName] = std::to_wstring(static_cast<int>(num));
            } else {
                aiVars_[outputVarName] = textResult;
            }
        } else {
            aiVars_[outputVarName] = fallback;
        }
    }

    std::wstring EffectiveAiModelName(const ScriptAction& a) const {
        return ResolveActionAiModelName(a, appSettings_.ai);
    }

    AiActionResult RunAiTextAnalysisForAction(const ScriptAction& a, const std::wstring& resolvedPrompt) {
        AiActionResult result;
        result.errorMessage = L"AI 未启用或未配置模型";
        const std::wstring modelName = EffectiveAiModelName(a);
        if (modelName.empty() || !appSettings_.ai.enabled) return result;

        const int timeoutMs = std::max(5000, a.aiTimeoutSec > 0 ? a.aiTimeoutSec * 1000 : 30000);
        static const wchar_t* kAiTextSystemPrompt =
            L"文本分析助手。只输出用户要求的结果，不要解释或 Markdown，尽量简短。";
        auto core = CreateAiActionCore(
            modelName, appSettings_.ai.savedModels,
            appSettings_.ai.apiUrl, appSettings_.ai.apiKey, kAiTextSystemPrompt,
            timeoutMs, -1.0, 512);
        if (!core) {
            result.errorMessage = L"无法创建 AI 客户端";
            return result;
        }

        AiMacroLogFn logFn = [this](const std::wstring& line) { AppendAiDebugLog(line); };
        return ExecuteAiTextAnalysis(
            core.get(), resolvedPrompt, a.aiOutputType, stopFlag_, a.aiTimeoutSec, logFn, &aiHttpAbort_);
    }

    AiActionResult RunAiImageAnalysisForAction(
        const ScriptAction& a,
        const std::wstring& resolvedPrompt,
        const std::string& screenshotBase64) {
        AiActionResult result;
        result.errorMessage = L"AI 未启用或未配置模型";
        const std::wstring modelName = EffectiveAiModelName(a);
        if (modelName.empty() || !appSettings_.ai.enabled) return result;
        if (screenshotBase64.empty()) {
            result.errorMessage = L"截屏编码失败";
            return result;
        }

        const int timeoutMs = ResolveAiImageAnalysisTimeoutSec(a.aiTimeoutSec, resolvedPrompt.size()) * 1000;
        static const wchar_t* kAiImageSystemPrompt =
            L"截图分析助手。只输出用户要求的结果，不要解释或 Markdown，尽量简短。";
        auto core = CreateAiActionCore(
            modelName, appSettings_.ai.savedModels,
            appSettings_.ai.apiUrl, appSettings_.ai.apiKey, kAiImageSystemPrompt,
            timeoutMs, -1.0, 1024);
        if (!core) {
            result.errorMessage = L"无法创建 AI 客户端";
            return result;
        }

        AiMacroLogFn logFn = [this](const std::wstring& line) { AppendAiDebugLog(line); };
        return ExecuteAiImageAnalysis(
            core.get(), resolvedPrompt, screenshotBase64, a.aiOutputType,
            stopFlag_, a.aiTimeoutSec, logFn, &aiHttpAbort_);
    }

    AiActionResult RunAiActionExecuteForAction(
        const ScriptAction& a,
        const std::wstring& resolvedPrompt,
        const std::string& screenshotBase64,
        int captureWidth,
        int captureHeight,
        AiSessionStore* sessions = nullptr,
        const AiCaptureMapping* captureMapping = nullptr) {
        AiActionResult result;
        result.errorMessage = L"AI 未启用或未配置模型";
        const std::wstring modelName = EffectiveAiModelName(a);
        if (modelName.empty() || !appSettings_.ai.enabled) return result;

        const bool withImage = !screenshotBase64.empty();
        const AiActionRouteKind route = ClassifyAiActionRoute(resolvedPrompt, withImage);
        const int effectiveTimeoutSec = ResolveAiActionExecuteTimeoutSec(a.aiTimeoutSec, withImage);
        const int timeoutMs = effectiveTimeoutSec * 1000;

        ScriptAction prepAction = a;
        prepAction.aiModelName = modelName;
        AgentCore* corePtr = nullptr;
        std::unique_ptr<AgentCore> ownedCore = PrepareAiActionExecuteCore(
            sessions, prepAction, route, captureWidth, captureHeight,
            appSettings_, timeoutMs, corePtr);
        AgentCore* core = corePtr ? corePtr : ownedCore.get();
        if (!core) {
            result.errorMessage = L"无法创建 AI 客户端";
            return result;
        }

        AiMacroLogFn logFn = [this](const std::wstring& line) { AppendAiDebugLog(line); };
        return ExecuteAiActionExecute(
            core, resolvedPrompt, screenshotBase64, captureWidth, captureHeight, a.aiContextMode,
            stopFlag_, a.aiTimeoutSec, logFn, &aiHttpAbort_, captureMapping);
    }

    void OnDebugWindowClosedByUser() {
        if (!appSettings_.playback.enableDebugOutputWindow) return;
        appSettings_.playback.enableDebugOutputWindow = false;
        SaveAppSettings(appSettings_);
        NotifyActiveSettingsDialogSync();
    }

    void ShowDebugWindow() {
        if (!macroDebugWindow_.IsCreated()) {
            macroDebugWindow_.Create(font_, titleFont_, closeFont_, [this]() {
                OnDebugWindowClosedByUser();
            });
        }
        macroDebugWindow_.Show();
    }

    void HideDebugWindow() {
        macroDebugWindow_.Hide();
    }

    void ApplyDebugWindowSetting() {
        if (appSettings_.playback.enableDebugOutputWindow) ShowDebugWindow();
        else HideDebugWindow();
    }

    void UpdateStatusTip() {
        if (appSettings_.other.hideBottomRightTip) { HideStatusTip(); return; }
        const wchar_t* text = clicking_ ? L"连点中..." : (recording_ ? L"录制中..." : (running_ ? L"回放中..." : L""));
        if (!text[0]) { HideStatusTip(); return; }
        if (!statusTipWindow_) {
            statusTipWindow_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                L"STATIC", text, WS_POPUP | SS_CENTER | SS_CENTERIMAGE,
                0, 0, 160, 36, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(statusTipWindow_, WM_SETFONT, reinterpret_cast<WPARAM>(homeFont_), TRUE);
        }
        SetWindowTextW(statusTipWindow_, text);
        const int sw = GetSystemMetrics(SM_CXSCREEN);
        const int sh = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(statusTipWindow_, HWND_TOPMOST, sw - 180, sh - 80, 160, 36, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }

    void HideStatusTip() {
        if (statusTipWindow_) ShowWindow(statusTipWindow_, SW_HIDE);
    }

    void Cleanup() { if (crosshairDrag_.IsActive()) crosshairDrag_.End(); CloseEditorPopup(); CloseClickerDropPopup(); CancelQuickInputTip(); KillTimer(hwnd_, kScheduledTaskTimerId); if (editorDropPopup_) { DestroyWindow(editorDropPopup_); editorDropPopup_ = nullptr; } if (clickerDropPopup_) { DestroyWindow(clickerDropPopup_); clickerDropPopup_ = nullptr; } if (editorTipPopup_) { DestroyWindow(editorTipPopup_); editorTipPopup_ = nullptr; } macroDebugWindow_.Destroy(); if (statusTipWindow_) { DestroyWindow(statusTipWindow_); statusTipWindow_ = nullptr; } StopClickerCleanup(); StopRecordingCleanup(); stopFlag_ = true; if (worker_.joinable()) worker_.join(); ReleaseAllHeldInputs(); if (trayActive_) { NOTIFYICONDATAW nid{}; nid.cbSize = sizeof(nid); nid.hWnd = hwnd_; nid.uID = 1; Shell_NotifyIconW(NIM_DELETE, &nid); trayActive_ = false; } UnregisterHotKey(hwnd_, HOTKEY_GLOBAL_ID); for (int i = 0; i < 100; ++i) UnregisterHotKey(hwnd_, HOTKEY_SCRIPT_BASE + i); UninstallGlobalHotkeyHooks(); if (crosshairDragCursor_) { DestroyCursor(crosshairDragCursor_); crosshairDragCursor_ = nullptr; } if (findImagePreviewBitmap_) { DeleteBitmapHandle(findImagePreviewBitmap_); findImagePreviewBitmap_ = nullptr; } if (ocrFindImagePreviewBitmap_) { DeleteBitmapHandle(ocrFindImagePreviewBitmap_); ocrFindImagePreviewBitmap_ = nullptr; } if (aiFindImagePreviewBitmap_) { DeleteBitmapHandle(aiFindImagePreviewBitmap_); aiFindImagePreviewBitmap_ = nullptr; } DeleteObject(font_); DeleteObject(editorFont_); DeleteObject(bigFont_); DeleteObject(titleFont_); DeleteObject(hotFont_); DeleteObject(closeFont_); DeleteObject(homeFont_); DeleteObject(homeTabFont_); DeleteObject(whiteBrush_); DeleteObject(panelBrush_); DeleteObject(lineGreenBrush_); }
    void StopClickerCleanup();
    void StopRecordingCleanup() { if (recording_) { g_recording = false; recording_ = false; UninstallRecordingHooks(); } }

    HWND hwnd_ = nullptr; HFONT font_ = nullptr; HFONT editorFont_ = nullptr; HFONT bigFont_ = nullptr; HFONT titleFont_ = nullptr; HFONT hotFont_ = nullptr; HFONT closeFont_ = nullptr; HFONT homeFont_ = nullptr; HFONT homeTabFont_ = nullptr; HBRUSH whiteBrush_ = nullptr; HBRUSH panelBrush_ = nullptr; HBRUSH lineGreenBrush_ = nullptr;
    HWND name_ = nullptr; HWND mode_ = nullptr; HWND labelList_ = nullptr; HWND labelBatchCount_ = nullptr; HWND actionCombo_ = nullptr; HWND addBtn_ = nullptr; HWND modifyBtn_ = nullptr; HWND clearBtn_ = nullptr; HWND loadBtn_ = nullptr;
    HWND batchExitBtn_ = nullptr; HWND batchSelectAllBtn_ = nullptr; HWND batchDeselectBtn_ = nullptr; HWND batchDeleteBtn_ = nullptr; HWND batchCopyBtn_ = nullptr;
    HWND cancelBtn_ = nullptr; HWND saveBtn_ = nullptr; HWND crosshairBtn_ = nullptr; HWND paramViewport_ = nullptr; HWND paramTopMask_ = nullptr; HWND paramBottomMask_ = nullptr; HWND paramRightMask_ = nullptr;
    HWND runProgramCombo_ = nullptr; HWND runProgramPath_ = nullptr; HWND runProgramBrowseBtn_ = nullptr; HWND runProgramOrLabel_ = nullptr; HWND runProgramCrosshairBtn_ = nullptr; HWND runProgramArgs_ = nullptr;
    HWND closeProgramPath_ = nullptr; HWND closeProgramBrowseBtn_ = nullptr; HWND closeProgramOrLabel_ = nullptr; HWND closeProgramCrosshairBtn_ = nullptr; HWND closeProgramMatchFileName_ = nullptr;
    HWND openWebpageUrl_ = nullptr; HWND openFilePath_ = nullptr; HWND openFileBrowseBtn_ = nullptr; HWND timerVarName_ = nullptr; HWND cursorPosVarName_ = nullptr;
    HWND moveX_ = nullptr; HWND moveY_ = nullptr; HWND moveRandomX_ = nullptr; HWND moveRandomY_ = nullptr; HWND moveFromVar_ = nullptr; HWND moveVarX_ = nullptr; HWND moveVarY_ = nullptr; HWND waitDuration_ = nullptr; HWND waitRandom_ = nullptr; HWND clickButton_ = nullptr; HWND clickCount_ = nullptr; HWND clickWait_ = nullptr; HWND clickRandom_ = nullptr;
    HWND keyEdit_ = nullptr; HWND keyPressEdit_ = nullptr; HWND keyCount_ = nullptr; HWND keyWait_ = nullptr; HWND keyRandom_ = nullptr; HWND loopTypeCombo_ = nullptr; HWND loopCount_ = nullptr; HWND loopFromVar_ = nullptr; HWND loopVarExpr_ = nullptr; HWND loopVarName_ = nullptr; HWND defineBlockName_ = nullptr; HWND runBlockCombo_ = nullptr; HWND remarkLabel_ = nullptr; HWND remark_ = nullptr; HWND listRemarkEdit_ = nullptr;
    HWND clickLWin_ = nullptr; HWND clickRWin_ = nullptr; HWND clickLCtrl_ = nullptr; HWND clickRCtrl_ = nullptr; HWND clickLAlt_ = nullptr; HWND clickRAlt_ = nullptr; HWND clickLShift_ = nullptr; HWND clickRShift_ = nullptr;
    HWND mousePressButton_ = nullptr; HWND mousePressLWin_ = nullptr; HWND mousePressRWin_ = nullptr; HWND mousePressLCtrl_ = nullptr; HWND mousePressRCtrl_ = nullptr; HWND mousePressLAlt_ = nullptr; HWND mousePressRAlt_ = nullptr; HWND mousePressLShift_ = nullptr; HWND mousePressRShift_ = nullptr;
    HWND keyLWin_ = nullptr; HWND keyRWin_ = nullptr; HWND keyLCtrl_ = nullptr; HWND keyRCtrl_ = nullptr; HWND keyLAlt_ = nullptr; HWND keyRAlt_ = nullptr; HWND keyLShift_ = nullptr; HWND keyRShift_ = nullptr;
    HWND keyPressLWin_ = nullptr; HWND keyPressRWin_ = nullptr; HWND keyPressLCtrl_ = nullptr; HWND keyPressRCtrl_ = nullptr; HWND keyPressLAlt_ = nullptr; HWND keyPressRAlt_ = nullptr; HWND keyPressLShift_ = nullptr; HWND keyPressRShift_ = nullptr;
    HWND hotkeyShortcutCombo_ = nullptr; HWND hotkeyShortcutCount_ = nullptr; HWND hotkeyShortcutWait_ = nullptr; HWND hotkeyShortcutRandom_ = nullptr;
    HWND runMacroCombo_ = nullptr; HWND mousePlaybackCombo_ = nullptr; HWND mousePlaybackCount_ = nullptr; HWND mousePlaybackWait_ = nullptr; HWND mousePlaybackRandom_ = nullptr;
    HWND scrollVertical_ = nullptr; HWND scrollHorizontal_ = nullptr; HWND scrollSteps_ = nullptr; HWND scrollDirectionCombo_ = nullptr; HWND scrollCount_ = nullptr; HWND scrollWait_ = nullptr; HWND scrollRandom_ = nullptr;
    HWND findRegionLabel_ = nullptr; HWND findFullScreenBtn_ = nullptr; HWND findSelectRegionBtn_ = nullptr;
    HWND findX1_ = nullptr; HWND findY1_ = nullptr; HWND findX2_ = nullptr; HWND findY2_ = nullptr;
    HWND findFollowUpLabel_ = nullptr; HWND findFollowUpCombo_ = nullptr;
    HWND findOffsetXLabel_ = nullptr; HWND findOffsetYLabel_ = nullptr; HWND findMatchVarLabel_ = nullptr;
    HWND findTestBtn_ = nullptr; HWND findImagePreviewBtn_ = nullptr; HWND findScreenshotBtn_ = nullptr; HWND findLocalImageBtn_ = nullptr; HWND findClearImageBtn_ = nullptr;
    HWND findMatchThreshold_ = nullptr; HWND findScaleMin_ = nullptr; HWND findScaleMax_ = nullptr; HWND findOffsetX_ = nullptr; HWND findOffsetY_ = nullptr; HWND findSelectOffsetBtn_ = nullptr; HWND findUntilFound_ = nullptr; HWND findMatchVar_ = nullptr;
    HWND ocrDepStatusLabel_ = nullptr; HWND ocrDepInstallBtn_ = nullptr;
    HWND ocrRegionLabel_ = nullptr; HWND ocrFullScreenBtn_ = nullptr; HWND ocrSelectRegionBtn_ = nullptr;
    HWND ocrX1_ = nullptr; HWND ocrY1_ = nullptr; HWND ocrX2_ = nullptr; HWND ocrY2_ = nullptr;
    HWND ocrX1Label_ = nullptr; HWND ocrY1Label_ = nullptr; HWND ocrX2Label_ = nullptr; HWND ocrY2Label_ = nullptr;
    HWND ocrResultModeLabel_ = nullptr; HWND ocrResultModeCombo_ = nullptr;
    HWND ocrSearchLabel_ = nullptr; HWND ocrSearchEdit_ = nullptr; HWND ocrSearchVarLabel_ = nullptr; HWND ocrSearchVarCombo_ = nullptr; HWND ocrSearchVarInsertBtn_ = nullptr;
    HWND ocrFollowUpLabel_ = nullptr; HWND ocrFollowUpCombo_ = nullptr;
    HWND ocrOffsetXLabel_ = nullptr; HWND ocrOffsetX_ = nullptr; HWND ocrOffsetYLabel_ = nullptr; HWND ocrOffsetY_ = nullptr;
    HWND ocrSelectOffsetBtn_ = nullptr;
    HWND ocrResultVarLabel_ = nullptr; HWND ocrUntilFound_ = nullptr; HWND ocrResultVar_ = nullptr; HWND ocrTestBtn_ = nullptr;
    HWND ocrRegionByImageCheck_ = nullptr;
    HWND ocrDigitsOnlyCheck_ = nullptr;
    HWND ocrFindImageLabel_ = nullptr; HWND ocrFindSelectRegionBtn_ = nullptr; HWND ocrFindImagePreviewBtn_ = nullptr;
    HWND ocrFindScreenshotBtn_ = nullptr; HWND ocrFindLocalImageBtn_ = nullptr; HWND ocrFindClearImageBtn_ = nullptr;
    HWND ocrFindMatchThreshold_ = nullptr; HWND ocrFindScaleMin_ = nullptr; HWND ocrFindScaleMax_ = nullptr;
    // ── AI 动作控制 ──
    HWND aiPromptLabel_ = nullptr;
    HWND aiPromptEdit_ = nullptr; HWND aiVarLabel_ = nullptr; HWND aiVarCombo_ = nullptr; HWND aiInsertVarBtn_ = nullptr;
    HWND aiModelLabel_ = nullptr;
    HWND aiModelCombo_ = nullptr;
    HWND aiContextLabel_ = nullptr;
    HWND aiContextModeCombo_ = nullptr;
    HWND aiTimeoutLabel_ = nullptr;
    HWND aiOutputVarLabel_ = nullptr;
    HWND aiOutputVarEdit_ = nullptr;
    HWND aiOutputTypeLabel_ = nullptr;
    HWND aiOutputTypeCombo_ = nullptr;
    HWND aiTimeoutEdit_ = nullptr; HWND aiFallbackLabel_ = nullptr; HWND aiFallbackEdit_ = nullptr;
    HWND aiImageScaleLabel_ = nullptr;
    HWND aiImageScaleEdit_ = nullptr;
    HWND aiRegionByImageCheck_ = nullptr;
    HWND aiRegionLabel_ = nullptr;
    HWND aiFullScreenBtn_ = nullptr;
    HWND aiSelectRegionBtn_ = nullptr;
    HWND aiCoordX1Label_ = nullptr; HWND aiCoordY1Label_ = nullptr;
    HWND aiCoordX2Label_ = nullptr; HWND aiCoordY2Label_ = nullptr;
    HWND aiSearchX1Edit_ = nullptr; HWND aiSearchY1Edit_ = nullptr;
    HWND aiSearchX2Edit_ = nullptr; HWND aiSearchY2Edit_ = nullptr;
    HWND aiFindImageLabel_ = nullptr; HWND aiFindSelectRegionBtn_ = nullptr; HWND aiFindImagePreviewBtn_ = nullptr;
    HWND aiFindScreenshotBtn_ = nullptr; HWND aiFindLocalImageBtn_ = nullptr; HWND aiFindClearImageBtn_ = nullptr;
    HWND aiFindMatchLabel_ = nullptr; HWND aiFindMatchThreshold_ = nullptr; HWND aiFindMatchPctLabel_ = nullptr;
    HWND aiFindScaleMinLabel_ = nullptr; HWND aiFindScaleMin_ = nullptr;
    HWND aiFindScaleMaxLabel_ = nullptr; HWND aiFindScaleMax_ = nullptr;
    HWND aiRegionByImageCheck2_ = nullptr;
    HWND aiActionRegionLabel_ = nullptr;
    HWND aiFullScreenBtn2_ = nullptr;
    HWND aiSelectRegionBtn2_ = nullptr;
    HWND aiActCoordX1Label_ = nullptr; HWND aiActCoordY1Label_ = nullptr;
    HWND aiActCoordX2Label_ = nullptr; HWND aiActCoordY2Label_ = nullptr;
    HWND aiSearchX1Edit2_ = nullptr; HWND aiSearchY1Edit2_ = nullptr;
    HWND aiSearchX2Edit2_ = nullptr; HWND aiSearchY2Edit2_ = nullptr;
    HWND aiMaxStepsLabel_ = nullptr;
    HWND aiMaxStepsEdit_ = nullptr;
    HWND aiWithImageCheck_ = nullptr;
    HWND aiMaxStepsHint_ = nullptr;
    HWND aiConfirmExecute_ = nullptr;
    HWND aiSearchRegionCombo_ = nullptr;
    HWND quickInputEdit_ = nullptr; HWND quickInputVarCombo_ = nullptr; HWND quickInputInsertBtn_ = nullptr; HWND quickInputCharInterval_ = nullptr; HWND quickInputCount_ = nullptr; HWND quickInputWait_ = nullptr; HWND quickInputRandom_ = nullptr;
    HWND ifVarCombo_ = nullptr; HWND ifOperatorCombo_ = nullptr; HWND ifValueEdit_ = nullptr; HWND ifConnectorCombo_ = nullptr; HWND ifAddConditionBtn_ = nullptr; HWND ifConditionList_ = nullptr;
    std::vector<HWND> editorControls_, moveControls_, waitControls_, mousePressControls_, clickControls_, mousePlaybackControls_, runMacroControls_, keyPressControls_, keyControls_, hotkeyShortcutControls_, quickInputControls_, loopControls_, endLoopControls_, defineBlockControls_, runBlockControls_, scrollWheelControls_, findImageControls_, findImageOffsetControls_, findImageVarControls_, ocrDepControls_, ocrFindRegionToggleControls_, ocrControls_, ocrFindRegionControls_, ocrSearchControls_, ocrFollowControls_, ocrFollowOffsetControls_, ocrFollowVarControls_, ifControls_, elseControls_, lockScreenshotControls_, unlockScreenshotControls_, stopMacroControls_, runProgramControls_, runProgramFileControls_, closeProgramControls_, openWebpageControls_, openFileControls_, timerRecordControls_, getCursorPosControls_, aiCommonControls_, aiTextControls_, aiImageControls_, aiActionControls_, aiFindRegionControls_;

    // ── 新布局系统: 参数面板布局结果缓存 (索引 = popupAction_.sel) ──
    std::unordered_map<int, UILayoutResult> paramLayoutResults_;
    std::vector<EditorControlLayout> editorLayouts_;
    std::vector<ParamScrollLayoutEntry> paramScrollLayout_;
    int paramContentBottom_ = 0;
    int paramControlsBottom_ = 0;
    std::vector<HotkeyMenuItem> hotkeyMenuItems_;
    std::vector<ScriptMeta> scripts_; std::vector<ScriptMeta> recordings_; std::vector<ScriptAction> actions_;
    std::set<int> collapsedContainers_;
    Page page_ = Page::Home; quickscript::MainTab activeHomeTab_ = quickscript::MainTab::Clicker; Hotkey globalHotkey_{0, VK_F8, L"F8", true};
    RECT homeRectBeforeEditor_{};
    int selectedScript_ = -1, selectedRecording_ = -1, currentScriptIndex_ = -1, homeHover_ = -1, recordingHover_ = -1, hoverIndex_ = -1, selectedIndex_ = -1, editingRemarkIndex_ = -1, copySource_ = -1, dragIndex_ = -1, dragTargetIndex_ = -1, dragTargetIndent_ = 0, dragStartX_ = 0, dragStartY_ = 0, scrollOffset_ = 0, homeScrollOffset_ = 0, homeScrollbarDragOffset_ = 0, editorScrollbarDragOffset_ = 0, pendingDeleteIndex_ = -1, paramScrollY_ = 0, paramScrollbarDragOffset_ = 0;
    HoverButton hoverButton_ = HoverButton::None;
    HWND hoverGrayBtn_ = nullptr;
    HWND pendingHoverGrayOld_ = nullptr, pendingHoverGrayNew_ = nullptr;
    bool dragging_ = false, dragMoved_ = false, dragTargetNested_ = false, homeScrollbarDragging_ = false, editorScrollbarDragging_ = false, paramScrollbarDragging_ = false, trackingMouse_ = false, deleteConfirmVisible_ = false, hasHomeRectBeforeEditor_ = false, wasVisibleBeforeRun_ = true, wasMinimizedBeforeRun_ = false, loadingForm_ = false, batchEditMode_ = false, findImageFullScreen_ = true, ocrFullScreen_ = true, aiFullScreen_ = true;
    int clickerDropPopupKind_ = -1;
    int clickerPopupHover_ = -1;
    int clickerPopupScroll_ = 0;
    int clickerPopupVisibleCount_ = 0;
    CrosshairDragController crosshairDrag_;
    HWND hoverCrosshairBtn_ = nullptr;
    int editorPopupOpen_ = -1;
    int editorPopupHover_ = -1;
    int editorPopupScroll_ = 0;
    HWND editorDropPopup_ = nullptr;
    HWND clickerDropPopup_ = nullptr;
    HWND editorTipPopup_ = nullptr;
    QuickInputTipKind quickInputTipPending_ = QuickInputTipKind::None;
    QuickInputTipKind quickInputTipShown_ = QuickInputTipKind::None;
    int quickInputTipPendingVarIndex_ = -1;
    POINT quickInputTipAnchor_{};
    DWORD quickInputTipHoverStart_ = 0;
    double saveDurationSeconds_ = 0;
    std::optional<Hotkey> saveHotkeyOverride_;
    std::vector<bool> batchSelected_;
    HCURSOR crosshairDragCursor_ = nullptr;
    std::wstring currentPath_, currentRecordTime_, formKeyText_ = L"7", formKeyPressText_ = L"7";
    UINT formKeyVk_ = '7', formKeyPressVk_ = '7';
    quickscript::ClickerSettings clickerSettings_{};
    quickscript::RecorderSettings recorderSettings_{};
    quickscript::AppSettings appSettings_{};
    bool trayActive_ = false;
    bool hiddenToTray_ = false;
    int clickCountDone_ = 0;
    MacroDebugWindow macroDebugWindow_;
    HWND statusTipWindow_ = nullptr;
    PopupCombo popupMode_, popupAction_, popupMouseBtn_, popupClickBtn_, popupLoopType_, popupRunBlock_, popupHotkeyShortcut_, popupQuickInputVar_, popupRunMacro_, popupMousePlayback_, popupScrollDir_, popupFindFollowUp_, popupOcrResultMode_, popupOcrFollowUp_, popupOcrSearchVar_, popupIfVar_, popupIfOperator_, popupIfConnector_, popupRunProgram_, popupAiModel_, popupAiContextMode_, popupAiOutputType_, popupAiSearchRegion_;
    std::vector<QuickInputVarItem> quickInputVarItems_;
    std::vector<std::wstring> runMacroPaths_, mousePlaybackPaths_;
    std::wstring findImagePath_;
    std::wstring ocrFindImagePath_;
    std::wstring aiFindImagePath_;
    std::unordered_set<std::wstring> newImagePaths_;      // 编辑期间新增的图片路径，用于取消时清理
    std::atomic<bool> findTestRunning_{false};
    std::atomic<bool> ocrTestRunning_{false};
    bool workerUsesOcrVars_ = false;
    HBITMAP findImagePreviewBitmap_ = nullptr;
    HBITMAP ocrFindImagePreviewBitmap_ = nullptr;
    HBITMAP aiFindImagePreviewBitmap_ = nullptr;
    RECT findRegionSavedRect_{};
    std::unique_ptr<MatchOverlay> matchOverlay_;
    std::unique_ptr<OcrOverlay> ocrOverlay_;
    std::unique_ptr<ScreenshotOverlay> screenshotOverlay_;
    std::vector<std::unique_ptr<AgentDialog>> agentDialogs_;
    std::unordered_map<std::wstring, ImageMatchResult> matchVars_;
    std::unordered_map<std::wstring, OcrVarResult> ocrVars_;
    std::unordered_map<std::wstring, std::wstring> aiVars_;
    std::unordered_map<std::wstring, int> loopVars_;
    std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> timerStarts_;
    int curLoops_ = 0;
    std::atomic<int> simulatingInputDepth_{0};
    DWORD lastHotkeyTick_ = 0;
    std::atomic_bool running_{false}, stopFlag_{false}; AiHttpAbortSlot aiHttpAbort_; std::thread worker_; std::mt19937 rng_{std::random_device{}()};
    ScheduledTaskScheduler scheduledTasks_;
    // Recording
    std::atomic_bool recording_{false};
    bool recordingWasVisible_ = true;
    // Clicker
    std::atomic_bool clicking_{false};
    std::thread clickerThread_;
};

