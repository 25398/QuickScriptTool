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
#include "config.h"
#include "controls.h"
#include "drawing.h"
#include "hotkey_dialog.h"
#include "image_match.h"
#include "macro_variables.h"
#include "main_features.h"
#include "popup_combo.h"
#include "crosshair_drag.h"
#include "ocr_install_dialog.h"
#include "ocr_engine.h"
#include "process_utils.h"
#include "recorder.h"
#include "match_overlay.h"
#include "ocr_overlay.h"
#include "screenshot_overlay.h"
#include "editor_dropdown.h"
#include "script_types.h"
#include "utils.h"

extern HINSTANCE g_instance;

// ── Global-hotkey low-level hook (fallback when RegisterHotKey is unavailable) ──
namespace {
    HHOOK ghHotkeyKbHook = nullptr;
    HHOOK ghHotkeyMouseHook = nullptr;
    HWND ghHotkeyHwnd = nullptr;
    bool ghHotkeyPending = false;
    UINT ghHotkeyVk = 0;
    UINT ghHotkeyMods = 0;
    bool ghHotkeyEnabled = false;

    bool CheckHotkeyModifiers(UINT required) {
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

    bool IsMouseVk(UINT vk) {
        return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON
            || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
    }

    LRESULT CALLBACK HotkeyKbProc(int code, WPARAM wp, LPARAM lp) {
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

    LRESULT CALLBACK HotkeyMouseProc(int code, WPARAM wp, LPARAM lp) {
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
} // anonymous namespace

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

private:
    friend LRESULT CALLBACK EditorDropPopupWndProc(HWND, UINT, WPARAM, LPARAM);
    friend LRESULT CALLBACK EditorTipPopupWndProc(HWND, UINT, WPARAM, LPARAM);
    enum class Page { Home, Editor };
    enum Id { kScriptName = 1001, kModeCombo, kActionCombo, kAdd, kModify, kClear, kSave, kCancel, kLoad, kBatchExit, kBatchSelectAll, kBatchDeselect, kBatchDelete, kBatchCopy, kMoveX, kMoveY, kMoveRandomX, kMoveRandomY, kMoveFromVar, kMoveVarX, kMoveVarY, kClickButton, kClickCount, kClickWait, kClickRandom, kWaitDuration, kWaitRandom, kRemark, kListRemarkEdit, kClose, kKeyCapture, kClickLWin, kClickRWin, kClickLCtrl, kClickRCtrl, kClickLAlt, kClickRAlt, kClickLShift, kClickRShift, kKeyLWin, kKeyRWin, kKeyLCtrl, kKeyRCtrl, kKeyLAlt, kKeyRAlt, kKeyLShift, kKeyRShift, kCrosshair, kLoopCount, kLoopFromVar, kLoopVarExpr, kLoopVarName, kDefineBlockName, kRunBlockCombo, kKeyPressCapture, kMousePressButton, kMousePressLWin, kMousePressRWin, kMousePressLCtrl, kMousePressRCtrl, kMousePressLAlt, kMousePressRAlt, kMousePressLShift, kMousePressRShift, kKeyPressLWin, kKeyPressRWin, kKeyPressLCtrl, kKeyPressRCtrl, kKeyPressLAlt, kKeyPressRAlt, kKeyPressLShift, kKeyPressRShift, kHotkeyShortcutCombo, kHotkeyShortcutCount, kHotkeyShortcutWait, kHotkeyShortcutRandom, kQuickInputText, kQuickInputVarCombo, kQuickInputInsert, kQuickInputCharInterval, kQuickInputCount, kQuickInputWait, kQuickInputRandom, kRunMacroCombo, kMousePlaybackCombo, kMousePlaybackCount, kMousePlaybackWait, kMousePlaybackRandom, kScrollVertical, kScrollHorizontal, kScrollSteps, kScrollDirection, kScrollCount, kScrollWait, kScrollRandom, kFindFullScreen, kFindSelectRegion, kFindX1, kFindY1, kFindX2, kFindY2, kFindTest, kFindScreenshot, kFindLocalImage, kFindClearImage, kFindImagePreview, kFindMatchThreshold, kFindScaleMin, kFindScaleMax, kFindFollowUp, kFindOffsetX, kFindOffsetY, kFindSelectOffset, kFindUntilFound, kFindMatchVar, kOcrFullScreen, kOcrSelectRegion, kOcrX1, kOcrY1, kOcrX2, kOcrY2, kOcrResultMode, kOcrSearchText, kOcrSearchVarCombo, kOcrSearchVarInsert, kOcrFollowUp, kOcrOffsetX, kOcrOffsetY, kOcrSelectOffset, kOcrUntilFound, kOcrResultVar, kOcrTest, kOcrInstallDep, kOcrRegionByImage, kOcrFindSelectRegion, kOcrFindScreenshot, kOcrFindLocalImage, kOcrFindClearImage, kOcrFindImagePreview, kOcrFindMatchThreshold, kOcrFindScaleMin, kOcrFindScaleMax, kIfVarCombo, kIfOperator, kIfValue, kIfConnector, kIfAddCondition, kIfConditionList, kRunProgramCombo, kRunProgramPath, kRunProgramBrowse, kRunProgramCrosshair, kRunProgramArgs, kCloseProgramPath, kCloseProgramBrowse, kCloseProgramCrosshair, kCloseProgramMatchFileName, kOpenWebpageUrl, kOpenFilePath, kOpenFileBrowse, kTimerVarName };
    enum class HoverButton { None, Import, Export, Load, Clear, Add, Modify, Cancel, Save, Close, Minimize, HomeCard, HomeScroll, EditorScroll, Create, CommonHotkey, HomeEdit, HomeDelete, ScriptHotkey, Row, RowCopy, RowDelete, RowCheckbox, BatchExit, BatchSelectAll, BatchDeselect, BatchDelete, BatchCopy, Crosshair, ClickerInterval, ClickerHotkey, RecorderHotkey };
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
        case WM_WINDOWPOSCHANGED:
            if (EditorDropPopupVisible()) SyncEditorDropPopup();
            if (quickInputTipShown_ != QuickInputTipKind::None) SyncQuickInputTipPopup();
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_SHOWWINDOW:
            if (!wp) {
                CloseEditorPopup();
                CancelQuickInputTip();
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                HWND fg = GetForegroundWindow();
                if (fg != hwnd_ && fg != editorDropPopup_ && fg != editorTipPopup_) {
                    CloseEditorPopup();
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
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_HOTKEY: OnHotkey(static_cast<int>(wp)); return 0;
        case WM_GLOBAL_HOTKEY_DETECTED: OnHotkey(HOTKEY_GLOBAL_ID); return 0;
        case WM_RUN_DONE: OnRunDone(); return 0;
        case WM_FIND_TEST_DONE: OnFindTestDone(static_cast<int>(wp), static_cast<int>(lp)); return 0;
        case WM_TRAY: if (LOWORD(lp) == WM_LBUTTONUP) RestoreFromTray(); return 0;
        case WM_CTLCOLORSTATIC:
            return OnCtlColor(reinterpret_cast<HDC>(wp));
        case WM_CTLCOLOREDIT: return OnEditColor(reinterpret_cast<HDC>(wp));
        case WM_CLOSE: StopRun(); DestroyWindow(hwnd_); return 0;
        case WM_DESTROY: Cleanup(); PostQuitMessage(0); return 0;
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
        whiteBrush_ = CreateSolidBrush(kWhite);
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
        CleanOrphanImages();  // 启动时清理孤立图片
        CreateEditorDropPopup();
        CreateEditorTipPopup();
        ShowHome();
        RegisterAllHotkeys();
        InstallGlobalHotkeyHooks();
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
            L"打开网页", L"打开文件", L"计时器记录时间"
        };
        popupAction_.sel = 0;
        CreateParamControls();
        cancelBtn_ = MakeGreenButton(hwnd_, L"取消", kCancel, 775, 702, 104, 34); editorControls_.push_back(cancelBtn_);
        saveBtn_ = MakeGreenButton(hwnd_, L"保存", kSave, 891, 702, 104, 34); editorControls_.push_back(saveBtn_);
        listRemarkEdit_ = MakeEdit(hwnd_, L"", kListRemarkEdit, kColRemarkClient + 1, kListY + 2, kRemarkEditW, kRemarkEditH);
        ShowWindow(listRemarkEdit_, SW_HIDE);
        editorControls_.push_back(listRemarkEdit_);
    }

    void AddEditorControl(HWND h) { editorControls_.push_back(h); }
    void AddGroup(std::vector<HWND>& group, HWND h) { group.push_back(h); editorControls_.push_back(h); }

    void CreateParamControls() {
        AddGroup(moveControls_, MakeLabel(hwnd_, L"移动到(左上角为0,0)", -1, 807, 180, 190, 25));
        AddGroup(moveControls_, MakeLabel(hwnd_, L"X:", -1, 807, 214, 25, 22));
        AddGroup(moveControls_, moveX_ = MakeEdit(hwnd_, L"0", kMoveX, 833, 214, 87, 22));
        AddGroup(moveControls_, MakeLabel(hwnd_, L"±随机:", -1, 921, 214, 50, 22));
        AddGroup(moveControls_, moveRandomX_ = MakeEdit(hwnd_, L"0", kMoveRandomX, 976, 214, 25, 22));
        AddGroup(moveControls_, MakeLabel(hwnd_, L"Y:", -1, 807, 251, 25, 22));
        AddGroup(moveControls_, moveY_ = MakeEdit(hwnd_, L"0", kMoveY, 833, 251, 87, 22));
        AddGroup(moveControls_, MakeLabel(hwnd_, L"±随机:", -1, 921, 251, 50, 22));
        AddGroup(moveControls_, moveRandomY_ = MakeEdit(hwnd_, L"0", kMoveRandomY, 976, 251, 25, 22));
        AddGroup(moveControls_, crosshairBtn_ = MakeGreenButton(hwnd_, L"拖动准星获取坐标", kCrosshair, 807, 283, 186, 32));
        AddGroup(moveControls_, moveFromVar_ = MakeCheckBox(hwnd_, L"来自变量表达式", kMoveFromVar, 807, 322, 180, 25));
        AddGroup(moveControls_, MakeLabel(hwnd_, L"X:", -1, 807, 354, 25, 22));
        AddGroup(moveControls_, moveVarX_ = MakeEdit(hwnd_, L"0", kMoveVarX, 833, 354, 168, 22));
        AddGroup(moveControls_, MakeLabel(hwnd_, L"Y:", -1, 807, 391, 25, 22));
        AddGroup(moveControls_, moveVarY_ = MakeEdit(hwnd_, L"0", kMoveVarY, 833, 391, 168, 22));
        AddGroup(moveControls_, MakeHint(hwnd_, L"*提示:可使用来自找图、找色，获取颜色，文字识别保存到变量中的值", 807, 421, 198, 56));
        AddGroup(waitControls_, MakeLabel(hwnd_, L"等待时间", -1, 807, 185, 90, 25));
        AddGroup(waitControls_, waitDuration_ = MakeEdit(hwnd_, L"0.500", kWaitDuration, 818, 216, 76, 22));
        AddGroup(waitControls_, MakeLabel(hwnd_, L"秒", -1, 901, 216, 35, 22));
        AddGroup(waitControls_, MakeLabel(hwnd_, L"最大随机时间", -1, 807, 258, 120, 25));
        AddGroup(waitControls_, waitRandom_ = MakeEdit(hwnd_, L"0.000", kWaitRandom, 818, 289, 76, 22));
        AddGroup(waitControls_, MakeLabel(hwnd_, L"秒", -1, 901, 289, 35, 22));
        AddGroup(waitControls_, MakeHint(hwnd_, L"*提示:等待总时间=等待时间+随机(0~最大随机时间)", 807, 319, 195, 40));
        AddGroup(mousePressControls_, MakeEditorLabel(hwnd_, L"选择鼠标键", -1, kEditorPanelLeft, kEditorLabelAboveComboY, kEditorActionComboW, kEditorLabelAboveComboH));
        AddGroup(mousePressControls_, mousePressButton_ = MakeLabel(hwnd_, L"左键", kMousePressButton, kEditorPanelLeft, kEditorParamComboY, kEditorActionComboW, kEditorActionComboH));
        popupMouseBtn_.items = {L"左键", L"右键", L"中键", L"侧键1", L"侧键2"}; popupMouseBtn_.sel = 0;
        AddGroup(mousePressControls_, MakeLabel(hwnd_, L"同时按住", -1, 817, 257, 120, 25));
        AddGroup(mousePressControls_, mousePressLWin_ = MakeCheckBox(hwnd_, L"左Win", kMousePressLWin, 817, 288, 80, 25));
        AddGroup(mousePressControls_, mousePressRWin_ = MakeCheckBox(hwnd_, L"右Win", kMousePressRWin, 916, 288, 80, 25));
        AddGroup(mousePressControls_, mousePressLCtrl_ = MakeCheckBox(hwnd_, L"左Ctrl", kMousePressLCtrl, 817, 319, 80, 25));
        AddGroup(mousePressControls_, mousePressRCtrl_ = MakeCheckBox(hwnd_, L"右Ctrl", kMousePressRCtrl, 916, 319, 80, 25));
        AddGroup(mousePressControls_, mousePressLAlt_ = MakeCheckBox(hwnd_, L"左Alt", kMousePressLAlt, 817, 350, 80, 25));
        AddGroup(mousePressControls_, mousePressRAlt_ = MakeCheckBox(hwnd_, L"右Alt", kMousePressRAlt, 916, 350, 80, 25));
        AddGroup(mousePressControls_, mousePressLShift_ = MakeCheckBox(hwnd_, L"左Shift", kMousePressLShift, 817, 381, 86, 25));
        AddGroup(mousePressControls_, mousePressRShift_ = MakeCheckBox(hwnd_, L"右Shift", kMousePressRShift, 916, 381, 86, 25));
        AddGroup(clickControls_, MakeEditorLabel(hwnd_, L"选择鼠标键", -1, kEditorPanelLeft, kEditorLabelAboveComboY, kEditorActionComboW, kEditorLabelAboveComboH));
        AddGroup(clickControls_, clickButton_ = MakeLabel(hwnd_, L"左键", kClickButton, kEditorPanelLeft, kEditorParamComboY, kEditorActionComboW, kEditorActionComboH));
        popupClickBtn_.items = {L"左键", L"右键", L"中键", L"侧键1", L"侧键2"}; popupClickBtn_.sel = 0;
        AddGroup(clickControls_, MakeLabel(hwnd_, L"同时按住", -1, 817, 257, 120, 25));
        AddGroup(clickControls_, clickLWin_ = MakeCheckBox(hwnd_, L"左Win", kClickLWin, 817, 288, 80, 25));
        AddGroup(clickControls_, clickRWin_ = MakeCheckBox(hwnd_, L"右Win", kClickRWin, 916, 288, 80, 25));
        AddGroup(clickControls_, clickLCtrl_ = MakeCheckBox(hwnd_, L"左Ctrl", kClickLCtrl, 817, 319, 80, 25));
        AddGroup(clickControls_, clickRCtrl_ = MakeCheckBox(hwnd_, L"右Ctrl", kClickRCtrl, 916, 319, 80, 25));
        AddGroup(clickControls_, clickLAlt_ = MakeCheckBox(hwnd_, L"左Alt", kClickLAlt, 817, 350, 80, 25));
        AddGroup(clickControls_, clickRAlt_ = MakeCheckBox(hwnd_, L"右Alt", kClickRAlt, 916, 350, 80, 25));
        AddGroup(clickControls_, clickLShift_ = MakeCheckBox(hwnd_, L"左Shift", kClickLShift, 817, 381, 86, 25));
        AddGroup(clickControls_, clickRShift_ = MakeCheckBox(hwnd_, L"右Shift", kClickRShift, 916, 381, 86, 25));
        AddGroup(clickControls_, MakeLabel(hwnd_, L"循环次数", -1, 817, 412, 75, 22));
        AddGroup(clickControls_, clickCount_ = MakeEdit(hwnd_, L"0", kClickCount, 908, 412, 54, 22));
        AddGroup(clickControls_, MakeLabel(hwnd_, L"等待时间", -1, 817, 449, 75, 22));
        AddGroup(clickControls_, clickWait_ = MakeEdit(hwnd_, L"0.010", kClickWait, 908, 449, 54, 22));
        AddGroup(clickControls_, MakeLabel(hwnd_, L"秒", -1, 968, 450, 32, 25));
        AddGroup(clickControls_, MakeLabel(hwnd_, L"随机时间", -1, 817, 487, 75, 22));
        AddGroup(clickControls_, clickRandom_ = MakeEdit(hwnd_, L"0.000", kClickRandom, 908, 487, 54, 22));
        AddGroup(clickControls_, MakeLabel(hwnd_, L"秒", -1, 968, 488, 32, 25));
        AddGroup(mousePlaybackControls_, MakeEditorLabel(hwnd_, L"请选择用于回放的鼠标录制", -1, kEditorPanelLeft, kEditorLabelAboveComboY, kEditorActionComboW, kEditorLabelAboveComboH));
        AddGroup(mousePlaybackControls_, mousePlaybackCombo_ = MakeLabel(hwnd_, L"", kMousePlaybackCombo, kEditorPanelLeft, kEditorParamComboY, kEditorActionComboW, kEditorActionComboH));
        popupMousePlayback_.items.clear(); popupMousePlayback_.sel = -1;
        AddGroup(mousePlaybackControls_, MakeLabel(hwnd_, L"循环次数", -1, 817, 244, 75, 22));
        AddGroup(mousePlaybackControls_, mousePlaybackCount_ = MakeEdit(hwnd_, L"0", kMousePlaybackCount, 908, 244, 54, 22));
        AddGroup(mousePlaybackControls_, MakeLabel(hwnd_, L"等待时间", -1, 817, 281, 75, 22));
        AddGroup(mousePlaybackControls_, mousePlaybackWait_ = MakeEdit(hwnd_, L"0.010", kMousePlaybackWait, 908, 281, 54, 22));
        AddGroup(mousePlaybackControls_, MakeLabel(hwnd_, L"秒", -1, 968, 281, 32, 25));
        AddGroup(mousePlaybackControls_, MakeLabel(hwnd_, L"随机时间", -1, 817, 318, 75, 22));
        AddGroup(mousePlaybackControls_, mousePlaybackRandom_ = MakeEdit(hwnd_, L"0.000", kMousePlaybackRandom, 908, 318, 54, 22));
        AddGroup(mousePlaybackControls_, MakeLabel(hwnd_, L"秒", -1, 968, 318, 32, 25));
        AddGroup(runMacroControls_, MakeEditorLabel(hwnd_, L"请选择用于运行的鼠标宏", -1, kEditorPanelLeft, kEditorLabelAboveComboY, kEditorActionComboW, kEditorLabelAboveComboH));
        AddGroup(runMacroControls_, runMacroCombo_ = MakeLabel(hwnd_, L"", kRunMacroCombo, kEditorPanelLeft, kEditorParamComboY, kEditorActionComboW, kEditorActionComboH));
        popupRunMacro_.items.clear(); popupRunMacro_.sel = -1;

        AddGroup(scrollWheelControls_, MakeLabel(hwnd_, L"滚动类型", -1, 807, 180, 90, 25));
        AddGroup(scrollWheelControls_, scrollVertical_ = MakeCheckBox(hwnd_, L"垂直", kScrollVertical, 817, 210, 70, 25));
        AddGroup(scrollWheelControls_, scrollHorizontal_ = MakeCheckBox(hwnd_, L"水平", kScrollHorizontal, 916, 210, 70, 25));
        SetChecked(scrollVertical_, true);
        AddGroup(scrollWheelControls_, MakeLabel(hwnd_, L"滚动步数", -1, 807, 244, 75, 22));
        AddGroup(scrollWheelControls_, scrollSteps_ = MakeEdit(hwnd_, L"1", kScrollSteps, 908, 244, 54, 22));
        AddGroup(scrollWheelControls_, MakeEditorLabel(hwnd_, L"滚动方向", -1, kEditorPanelLeft, 270, kEditorActionComboW, kEditorLabelAboveComboH));
        AddGroup(scrollWheelControls_, scrollDirectionCombo_ = MakeLabel(hwnd_, L"向上/左", kScrollDirection, kEditorPanelLeft, 302, kEditorActionComboW, kEditorActionComboH));
        popupScrollDir_.items = {L"向上/左", L"向下/右"}; popupScrollDir_.sel = 0;
        AddGroup(scrollWheelControls_, MakeLabel(hwnd_, L"循环次数", -1, 817, 412, 75, 22));
        AddGroup(scrollWheelControls_, scrollCount_ = MakeEdit(hwnd_, L"0", kScrollCount, 908, 412, 54, 22));
        AddGroup(scrollWheelControls_, MakeLabel(hwnd_, L"等待时间", -1, 817, 449, 75, 22));
        AddGroup(scrollWheelControls_, scrollWait_ = MakeEdit(hwnd_, L"0.010", kScrollWait, 908, 449, 54, 22));
        AddGroup(scrollWheelControls_, MakeLabel(hwnd_, L"秒", -1, 968, 450, 32, 25));
        AddGroup(scrollWheelControls_, MakeLabel(hwnd_, L"随机时间", -1, 817, 487, 75, 22));
        AddGroup(scrollWheelControls_, scrollRandom_ = MakeEdit(hwnd_, L"0.000", kScrollRandom, 908, 487, 54, 22));
        AddGroup(scrollWheelControls_, MakeLabel(hwnd_, L"秒", -1, 968, 488, 32, 25));

        AddGroup(findImageControls_, findRegionLabel_ = MakeLabel(hwnd_, L"找图区域", -1, kFindContentLeft, kFindRegionRowY, kFindRegionLabelW, kFindBtnH));
        AddGroup(findImageControls_, findFullScreenBtn_ = MakeGrayButton(hwnd_, L"全图", kFindFullScreen, kFindContentLeft + kFindRegionLabelW + 8, kFindRegionRowY, 44, kFindBtnH));
        AddGroup(findImageControls_, findSelectRegionBtn_ = MakeGrayButton(hwnd_, L"选取区域", kFindSelectRegion, kFindContentLeft + kFindRegionLabelW + 58, kFindRegionRowY, kFindBtnW, kFindBtnH));
        AddGroup(findImageControls_, MakeLabel(hwnd_, L"X1", -1, kFindXLabelX, kFindCoordRow1Y, kFindCoordLabelW, 22));
        AddGroup(findImageControls_, findX1_ = MakeFieldEdit(hwnd_, L"0", kFindX1, kFindXEditX, kFindCoordRow1Y, kFindEditW, 22));
        AddGroup(findImageControls_, MakeLabel(hwnd_, L"Y1", -1, kFindYLabelX, kFindCoordRow1Y, kFindCoordLabelW, 22));
        AddGroup(findImageControls_, findY1_ = MakeFieldEdit(hwnd_, L"0", kFindY1, kFindYEditX, kFindCoordRow1Y, kFindEditW, 22));
        AddGroup(findImageControls_, MakeLabel(hwnd_, L"X2", -1, kFindXLabelX, kFindCoordRow2Y, kFindCoordLabelW, 22));
        AddGroup(findImageControls_, findX2_ = MakeFieldEdit(hwnd_, L"0", kFindX2, kFindXEditX, kFindCoordRow2Y, kFindEditW, 22));
        AddGroup(findImageControls_, MakeLabel(hwnd_, L"Y2", -1, kFindYLabelX, kFindCoordRow2Y, kFindCoordLabelW, 22));
        AddGroup(findImageControls_, findY2_ = MakeFieldEdit(hwnd_, L"0", kFindY2, kFindYEditX, kFindCoordRow2Y, kFindEditW, 22));
        AddGroup(findImageControls_, MakeLabel(hwnd_, L"要查找的图", -1, kFindContentLeft, kFindImageLabelY, 90, kFindBtnH));
        AddGroup(findImageControls_, findTestBtn_ = MakeGrayButton(hwnd_, L"测试", kFindTest, kFindActionBtnX, kFindImageLabelY, kFindBtnW, kFindBtnH));
        AddGroup(findImageControls_, findImagePreviewBtn_ = MakeGrayButton(hwnd_, L"", kFindImagePreview, kFindContentLeft, kFindImageRowY, kFindImageSize, kFindImageSize));
        AddGroup(findImageControls_, findScreenshotBtn_ = MakeGrayButton(hwnd_, L"屏幕截图", kFindScreenshot, kFindActionBtnX, kFindImageRowY, kFindBtnW, kFindBtnH));
        AddGroup(findImageControls_, findLocalImageBtn_ = MakeGrayButton(hwnd_, L"本地图片", kFindLocalImage, kFindActionBtnX, kFindImageRowY + kFindBtnH + kFindBtnStackGap, kFindBtnW, kFindBtnH));
        AddGroup(findImageControls_, findClearImageBtn_ = MakeGrayButton(hwnd_, L"清除图片", kFindClearImage, kFindActionBtnX, kFindImageRowY + (kFindBtnH + kFindBtnStackGap) * 2, kFindBtnW, kFindBtnH));
        AddGroup(findImageControls_, MakeLabel(hwnd_, L"匹配度大于", -1, kFindContentLeft, kFindMatchY, 90, 22));
        AddGroup(findImageControls_, findMatchThreshold_ = MakeFieldEdit(hwnd_, L"65", kFindMatchThreshold, kFindContentLeft + 91, kFindMatchY, 40, 22));
        AddGroup(findImageControls_, MakeLabel(hwnd_, L"%", -1, kFindContentLeft + 135, kFindMatchY, 24, 22));
        AddGroup(findImageControls_, MakeLabel(hwnd_, L"最小缩放", -1, kFindContentLeft, kFindScaleY, 64, 22));
        AddGroup(findImageControls_, findScaleMin_ = MakeFieldEdit(hwnd_, L"0.9", kFindScaleMin, kFindContentLeft + 65, kFindScaleY, 40, 22));
        AddGroup(findImageControls_, MakeLabel(hwnd_, L"最大", -1, kFindContentLeft + 110, kFindScaleY, 40, 22));
        AddGroup(findImageControls_, findScaleMax_ = MakeFieldEdit(hwnd_, L"1.1", kFindScaleMax, kFindContentLeft + 151, kFindScaleY, 40, 22));
        AddGroup(findImageControls_, findFollowUpLabel_ = MakeLabel(hwnd_, L"后续操作", -1, kFindContentLeft, kFindFollowRowY, kFindFollowLabelW, kFindBtnH));
        AddGroup(findImageControls_, findFollowUpCombo_ = MakeLabel(hwnd_, L"点击", kFindFollowUp, kFindContentLeft + kFindFollowLabelW + 8, kFindFollowRowY, kFindFollowComboW, kFindBtnH));
        popupFindFollowUp_.items = {L"点击", L"鼠标移动到", L"保存到变量"}; popupFindFollowUp_.sel = 0;
        AddGroup(findImageOffsetControls_, MakeLabel(hwnd_, L"X偏", -1, kFindOffsetXLabelX, kFindOffsetRowY, kFindOffsetLabelW, 22));
        AddGroup(findImageOffsetControls_, findOffsetX_ = MakeFieldEdit(hwnd_, L"0", kFindOffsetX, kFindOffsetXEditX, kFindOffsetRowY, kFindEditW, 22));
        AddGroup(findImageOffsetControls_, MakeLabel(hwnd_, L"Y偏", -1, kFindOffsetYLabelX, kFindOffsetRowY, kFindOffsetLabelW, 22));
        AddGroup(findImageOffsetControls_, findOffsetY_ = MakeFieldEdit(hwnd_, L"0", kFindOffsetY, kFindYEditX, kFindOffsetRowY, kFindEditW, 22));
        AddGroup(findImageOffsetControls_, findSelectOffsetBtn_ = MakeGrayButton(hwnd_, L"选择偏移点击位置", kFindSelectOffset, kFindSelectOffsetLeft, kFindSelectOffsetY, kFindSelectOffsetW, kFindBtnH));
        AddGroup(findImageOffsetControls_, findUntilFound_ = MakeCheckBox(hwnd_, L"直到找到为止", kFindUntilFound, kFindContentLeft, kFindUntilFoundY, 140, 22));
        AddGroup(findImageVarControls_, MakeLabel(hwnd_, L"匹配度保存到", -1, kFindContentLeft, kFindOffsetRowY, 100, 22));
        AddGroup(findImageVarControls_, findMatchVar_ = MakeFieldEdit(hwnd_, L"matchRet", kFindMatchVar, kFindContentLeft + 91, kFindOffsetRowY, 80, 22));
        SizeFindFullScreenButton();
        ApplyFindImageFullScreen();

        AddGroup(ocrDepControls_, ocrDepStatusLabel_ = MakeLabel(hwnd_, L"文字识别未安装", -1, kFindContentLeft, kOcrDepRowY, 130, kFindBtnH));
        AddGroup(ocrDepControls_, ocrDepInstallBtn_ = MakeGrayButton(hwnd_, L"一键安装", kOcrInstallDep, kFindActionBtnX, kOcrDepRowY, kFindBtnW, kFindBtnH));
        AddGroup(ocrFindRegionToggleControls_, ocrRegionByImageCheck_ = MakeCheckBox(hwnd_, L"根据图片选取区域", kOcrRegionByImage, kFindContentLeft, kOcrRegionRowY, 180, 22));
        AddGroup(ocrControls_, ocrRegionLabel_ = MakeLabel(hwnd_, L"识别区域", -1, kFindContentLeft, kOcrRegionRowY, kFindRegionLabelW, kFindBtnH));
        AddGroup(ocrControls_, ocrFullScreenBtn_ = MakeGrayButton(hwnd_, L"全图", kOcrFullScreen, kFindContentLeft + kFindRegionLabelW + 8, kOcrRegionRowY, 44, kFindBtnH));
        AddGroup(ocrControls_, ocrSelectRegionBtn_ = MakeGrayButton(hwnd_, L"选取区域", kOcrSelectRegion, kFindContentLeft + kFindRegionLabelW + 58, kOcrRegionRowY, kFindBtnW, kFindBtnH));
        AddGroup(ocrControls_, MakeLabel(hwnd_, L"X1", -1, kFindXLabelX, kOcrCoordRow1Y, kFindCoordLabelW, 22));
        AddGroup(ocrControls_, ocrX1_ = MakeFieldEdit(hwnd_, L"0", kOcrX1, kFindXEditX, kOcrCoordRow1Y, kFindEditW, 22));
        AddGroup(ocrControls_, MakeLabel(hwnd_, L"Y1", -1, kFindYLabelX, kOcrCoordRow1Y, kFindCoordLabelW, 22));
        AddGroup(ocrControls_, ocrY1_ = MakeFieldEdit(hwnd_, L"0", kOcrY1, kFindYEditX, kOcrCoordRow1Y, kFindEditW, 22));
        AddGroup(ocrControls_, MakeLabel(hwnd_, L"X2", -1, kFindXLabelX, kOcrCoordRow2Y, kFindCoordLabelW, 22));
        AddGroup(ocrControls_, ocrX2_ = MakeFieldEdit(hwnd_, L"0", kOcrX2, kFindXEditX, kOcrCoordRow2Y, kFindEditW, 22));
        AddGroup(ocrControls_, MakeLabel(hwnd_, L"Y2", -1, kFindYLabelX, kOcrCoordRow2Y, kFindCoordLabelW, 22));
        AddGroup(ocrControls_, ocrY2_ = MakeFieldEdit(hwnd_, L"0", kOcrY2, kFindYEditX, kOcrCoordRow2Y, kFindEditW, 22));
        AddGroup(ocrControls_, ocrResultModeLabel_ = MakeLabel(hwnd_, L"结果处理", -1, kFindContentLeft, kOcrResultModeY, kFindFollowLabelW, kFindBtnH));
        AddGroup(ocrControls_, ocrResultModeCombo_ = MakeLabel(hwnd_, L"获取文字", kOcrResultMode, kFindContentLeft + kFindFollowLabelW + 8, kOcrResultModeY, kFindFollowComboW, kFindBtnH));
        popupOcrResultMode_.items = {L"获取文字", L"文字查找"}; popupOcrResultMode_.sel = 0;
        AddGroup(ocrControls_, ocrTestBtn_ = MakeGrayButton(hwnd_, L"测试", kOcrTest, kFindContentLeft, kOcrTestBtnYCompact, kFindBtnW, kFindBtnH));
        AddGroup(ocrFindRegionControls_, ocrFindImageLabel_ = MakeLabel(hwnd_, L"要查找的图", -1, kFindContentLeft, kFindImageLabelY, 90, kFindBtnH));
        AddGroup(ocrFindRegionControls_, ocrFindSelectRegionBtn_ = MakeGrayButton(hwnd_, L"选取区域", kOcrFindSelectRegion, kFindSelectRegionX, kFindImageLabelY, kFindBtnW, kFindBtnH));
        AddGroup(ocrFindRegionControls_, ocrFindImagePreviewBtn_ = MakeGrayButton(hwnd_, L"", kOcrFindImagePreview, kFindContentLeft, kFindImageRowY, kFindImageSize, kFindImageSize));
        AddGroup(ocrFindRegionControls_, ocrFindScreenshotBtn_ = MakeGrayButton(hwnd_, L"屏幕截图", kOcrFindScreenshot, kFindActionBtnX, kFindImageRowY, kFindBtnW, kFindBtnH));
        AddGroup(ocrFindRegionControls_, ocrFindLocalImageBtn_ = MakeGrayButton(hwnd_, L"本地图片", kOcrFindLocalImage, kFindActionBtnX, kFindImageRowY + kFindBtnH + kFindBtnStackGap, kFindBtnW, kFindBtnH));
        AddGroup(ocrFindRegionControls_, ocrFindClearImageBtn_ = MakeGrayButton(hwnd_, L"清除图片", kOcrFindClearImage, kFindActionBtnX, kFindImageRowY + (kFindBtnH + kFindBtnStackGap) * 2, kFindBtnW, kFindBtnH));
        AddGroup(ocrFindRegionControls_, MakeLabel(hwnd_, L"匹配度大于", -1, kFindContentLeft, kFindMatchY, 90, 22));
        AddGroup(ocrFindRegionControls_, ocrFindMatchThreshold_ = MakeFieldEdit(hwnd_, L"65", kOcrFindMatchThreshold, kFindContentLeft + 91, kFindMatchY, 40, 22));
        AddGroup(ocrFindRegionControls_, MakeLabel(hwnd_, L"%", -1, kFindContentLeft + 135, kFindMatchY, 24, 22));
        AddGroup(ocrFindRegionControls_, MakeLabel(hwnd_, L"最小缩放", -1, kFindContentLeft, kFindScaleY, 64, 22));
        AddGroup(ocrFindRegionControls_, ocrFindScaleMin_ = MakeFieldEdit(hwnd_, L"0.9", kOcrFindScaleMin, kFindContentLeft + 65, kFindScaleY, 40, 22));
        AddGroup(ocrFindRegionControls_, MakeLabel(hwnd_, L"最大", -1, kFindContentLeft + 110, kFindScaleY, 40, 22));
        AddGroup(ocrFindRegionControls_, ocrFindScaleMax_ = MakeFieldEdit(hwnd_, L"1.1", kOcrFindScaleMax, kFindContentLeft + 151, kFindScaleY, 40, 22));
        AddGroup(ocrSearchControls_, ocrSearchLabel_ = MakeLabel(hwnd_, L"在结果中查找的文字", -1, kFindContentLeft, kOcrSearchLabelY, kFindBlockW, 22));
        AddGroup(ocrSearchControls_, ocrSearchEdit_ = MakeEdit(hwnd_, L"", kOcrSearchText, kFindContentLeft, kOcrSearchEditY, kFindBlockW, 22));
        AddGroup(ocrSearchControls_, ocrSearchVarLabel_ = MakeLabel(hwnd_, L"变量:", -1, kFindContentLeft, kOcrSearchVarY, 50, 22));
        AddGroup(ocrSearchControls_, ocrSearchVarCombo_ = MakeLabel(hwnd_, L"", kOcrSearchVarCombo, kFindContentLeft + 56, kOcrSearchVarY - 2, kFindFollowComboW, kFindBtnH));
        AddGroup(ocrSearchControls_, ocrSearchVarInsertBtn_ = MakeButton(hwnd_, L"插入选择的变量", kOcrSearchVarInsert, kFindContentLeft, kOcrSearchVarY + kFindBtnH + 4, kFindBlockW, 28));
        popupOcrSearchVar_.items.clear(); popupOcrSearchVar_.sel = -1;
        AddGroup(ocrFollowControls_, ocrFollowUpLabel_ = MakeLabel(hwnd_, L"后续操作", -1, kFindContentLeft, kOcrFollowRowY, kFindFollowLabelW, kFindBtnH));
        AddGroup(ocrFollowControls_, ocrFollowUpCombo_ = MakeLabel(hwnd_, L"点击", kOcrFollowUp, kFindContentLeft + kFindFollowLabelW + 8, kOcrFollowRowY, kFindFollowComboW, kFindBtnH));
        popupOcrFollowUp_.items = {L"点击", L"鼠标移动到", L"保存到变量"}; popupOcrFollowUp_.sel = 0;
        AddGroup(ocrFollowControls_, ocrUntilFound_ = MakeCheckBox(hwnd_, L"直到找到为止", kOcrUntilFound, kFindContentLeft, kOcrUntilFoundY, 140, 22));
        AddGroup(ocrFollowOffsetControls_, ocrOffsetXLabel_ = MakeLabel(hwnd_, L"X偏", -1, kFindOffsetXLabelX, kOcrOffsetRowY, kFindOffsetLabelW, 22));
        AddGroup(ocrFollowOffsetControls_, ocrOffsetX_ = MakeFieldEdit(hwnd_, L"0", kOcrOffsetX, kFindOffsetXEditX, kOcrOffsetRowY, kFindEditW, 22));
        AddGroup(ocrFollowOffsetControls_, ocrOffsetYLabel_ = MakeLabel(hwnd_, L"Y偏", -1, kFindOffsetYLabelX, kOcrOffsetRowY, kFindOffsetLabelW, 22));
        AddGroup(ocrFollowOffsetControls_, ocrOffsetY_ = MakeFieldEdit(hwnd_, L"0", kOcrOffsetY, kFindYEditX, kOcrOffsetRowY, kFindEditW, 22));
        AddGroup(ocrFollowOffsetControls_, ocrSelectOffsetBtn_ = MakeGrayButton(hwnd_, L"选择偏移位置", kOcrSelectOffset, kFindSelectOffsetLeft, kFindSelectOffsetY, kFindSelectOffsetW, kFindBtnH));
        AddGroup(ocrFollowVarControls_, ocrResultVarLabel_ = MakeLabel(hwnd_, L"结果保存到", -1, kFindContentLeft, kOcrResultVarY, 100, 22));
        AddGroup(ocrFollowVarControls_, ocrResultVar_ = MakeEdit(hwnd_, L"a", kOcrResultVar, kFindContentLeft + 91, kOcrResultVarY, kOcrResultVarEditW, 22));
        SizeOcrRegionButtons();
        ApplyOcrFullScreen();
        RefreshOcrSubPanel();
        RefreshOcrDepStatus();

        AddGroup(keyControls_, MakeEditorLabel(hwnd_, L"选择键盘按键", -1, kEditorPanelLeft, kEditorLabelAboveComboY, kEditorActionComboW, kEditorLabelAboveComboH));
        AddGroup(keyControls_, keyEdit_ = MakeCaptureField(hwnd_, L"点击修改", kKeyCapture, kEditorPanelLeft, kEditorParamComboY, kEditorActionComboW, kEditorActionComboH));
        AddGroup(keyControls_, MakeLabel(hwnd_, L"同时按住", -1, 817, 257, 120, 25));
        AddGroup(keyControls_, keyLWin_ = MakeCheckBox(hwnd_, L"左Win", kKeyLWin, 817, 288, 80, 25));
        AddGroup(keyControls_, keyRWin_ = MakeCheckBox(hwnd_, L"右Win", kKeyRWin, 916, 288, 80, 25));
        AddGroup(keyControls_, keyLCtrl_ = MakeCheckBox(hwnd_, L"左Ctrl", kKeyLCtrl, 817, 319, 80, 25));
        AddGroup(keyControls_, keyRCtrl_ = MakeCheckBox(hwnd_, L"右Ctrl", kKeyRCtrl, 916, 319, 80, 25));
        AddGroup(keyControls_, keyLAlt_ = MakeCheckBox(hwnd_, L"左Alt", kKeyLAlt, 817, 350, 80, 25));
        AddGroup(keyControls_, keyRAlt_ = MakeCheckBox(hwnd_, L"右Alt", kKeyRAlt, 916, 350, 80, 25));
        AddGroup(keyControls_, keyLShift_ = MakeCheckBox(hwnd_, L"左Shift", kKeyLShift, 817, 381, 86, 25));
        AddGroup(keyControls_, keyRShift_ = MakeCheckBox(hwnd_, L"右Shift", kKeyRShift, 916, 381, 86, 25));
        AddGroup(keyControls_, MakeLabel(hwnd_, L"循环次数", -1, 817, 412, 75, 22));
        AddGroup(keyControls_, keyCount_ = MakeEdit(hwnd_, L"0", 0, 908, 412, 54, 22));
        AddGroup(keyControls_, MakeLabel(hwnd_, L"等待时间", -1, 817, 449, 75, 22));
        AddGroup(keyControls_, keyWait_ = MakeEdit(hwnd_, L"0.010", 0, 908, 449, 54, 22));
        AddGroup(keyControls_, MakeLabel(hwnd_, L"秒", -1, 968, 450, 32, 25));
        AddGroup(keyControls_, MakeLabel(hwnd_, L"随机时间", -1, 817, 487, 75, 22));
        AddGroup(keyControls_, keyRandom_ = MakeEdit(hwnd_, L"0.000", 0, 908, 487, 54, 22));
        AddGroup(keyControls_, MakeLabel(hwnd_, L"秒", -1, 968, 488, 32, 25));
        AddGroup(keyPressControls_, MakeEditorLabel(hwnd_, L"选择键盘按键", -1, kEditorPanelLeft, kEditorLabelAboveComboY, kEditorActionComboW, kEditorLabelAboveComboH));
        AddGroup(keyPressControls_, keyPressEdit_ = MakeCaptureField(hwnd_, L"点击修改", kKeyPressCapture, kEditorPanelLeft, kEditorParamComboY, kEditorActionComboW, kEditorActionComboH));
        AddGroup(keyPressControls_, MakeLabel(hwnd_, L"同时按住", -1, 817, 257, 120, 25));
        AddGroup(keyPressControls_, keyPressLWin_ = MakeCheckBox(hwnd_, L"左Win", kKeyPressLWin, 817, 288, 80, 25));
        AddGroup(keyPressControls_, keyPressRWin_ = MakeCheckBox(hwnd_, L"右Win", kKeyPressRWin, 916, 288, 80, 25));
        AddGroup(keyPressControls_, keyPressLCtrl_ = MakeCheckBox(hwnd_, L"左Ctrl", kKeyPressLCtrl, 817, 319, 80, 25));
        AddGroup(keyPressControls_, keyPressRCtrl_ = MakeCheckBox(hwnd_, L"右Ctrl", kKeyPressRCtrl, 916, 319, 80, 25));
        AddGroup(keyPressControls_, keyPressLAlt_ = MakeCheckBox(hwnd_, L"左Alt", kKeyPressLAlt, 817, 350, 80, 25));
        AddGroup(keyPressControls_, keyPressRAlt_ = MakeCheckBox(hwnd_, L"右Alt", kKeyPressRAlt, 916, 350, 80, 25));
        AddGroup(keyPressControls_, keyPressLShift_ = MakeCheckBox(hwnd_, L"左Shift", kKeyPressLShift, 817, 381, 86, 25));
        AddGroup(keyPressControls_, keyPressRShift_ = MakeCheckBox(hwnd_, L"右Shift", kKeyPressRShift, 916, 381, 86, 25));
        AddGroup(hotkeyShortcutControls_, MakeEditorLabel(hwnd_, L"要使用的快捷按键", -1, kEditorPanelLeft, kEditorLabelAboveComboY, kEditorActionComboW, kEditorLabelAboveComboH));
        AddGroup(hotkeyShortcutControls_, hotkeyShortcutCombo_ = MakeLabel(hwnd_, L"Ctrl+C(拷贝)", kHotkeyShortcutCombo, kEditorPanelLeft, kEditorParamComboY, kEditorActionComboW, kEditorActionComboH));
        AddGroup(hotkeyShortcutControls_, MakeLabel(hwnd_, L"循环次数", -1, 817, 412, 75, 22));
        AddGroup(hotkeyShortcutControls_, hotkeyShortcutCount_ = MakeEdit(hwnd_, L"0", kHotkeyShortcutCount, 908, 412, 54, 22));
        AddGroup(hotkeyShortcutControls_, MakeLabel(hwnd_, L"等待时间", -1, 817, 449, 75, 22));
        AddGroup(hotkeyShortcutControls_, hotkeyShortcutWait_ = MakeEdit(hwnd_, L"0.010", kHotkeyShortcutWait, 908, 449, 54, 22));
        AddGroup(hotkeyShortcutControls_, MakeLabel(hwnd_, L"秒", -1, 968, 450, 32, 25));
        AddGroup(hotkeyShortcutControls_, MakeLabel(hwnd_, L"随机时间", -1, 817, 487, 75, 22));
        AddGroup(hotkeyShortcutControls_, hotkeyShortcutRandom_ = MakeEdit(hwnd_, L"0.000", kHotkeyShortcutRandom, 908, 487, 54, 22));
        AddGroup(hotkeyShortcutControls_, MakeLabel(hwnd_, L"秒", -1, 968, 488, 32, 25));
        AddGroup(quickInputControls_, MakeEditorLabel(hwnd_, L"要输入的文字", -1, kEditorPanelLeft, kEditorLabelAboveComboY, kEditorActionComboW, kEditorLabelAboveComboH));
        AddGroup(quickInputControls_, quickInputEdit_ = MakeMultilineEdit(hwnd_, L"", kQuickInputText, kEditorPanelLeft, kEditorParamComboY, kEditorActionComboW, 80));
        AddGroup(quickInputControls_, MakeLabel(hwnd_, L"变量:", -1, kEditorPanelLeft, 292, 50, 22));
        AddGroup(quickInputControls_, quickInputVarCombo_ = MakeLabel(hwnd_, L"a", kQuickInputVarCombo, kEditorPanelLeft + 56, 290, kEditorActionComboW - 56, kEditorActionComboH));
        AddGroup(quickInputControls_, quickInputInsertBtn_ = MakeButton(hwnd_, L"插入选择的变量", kQuickInputInsert, kEditorPanelLeft, 318, kEditorActionComboW, 28));
        AddGroup(quickInputControls_, MakeLabel(hwnd_, L"字输入间隔", -1, 817, 358, 90, 22));
        AddGroup(quickInputControls_, quickInputCharInterval_ = MakeEdit(hwnd_, L"0.010", kQuickInputCharInterval, 908, 358, 54, 22));
        AddGroup(quickInputControls_, MakeLabel(hwnd_, L"秒", -1, 968, 358, 32, 25));
        AddGroup(quickInputControls_, MakeHint(hwnd_, L"*提示:直接输入文字或变量中的值: 使用来自变量中的", 817, 386, 198, 56));
        AddGroup(quickInputControls_, MakeLabel(hwnd_, L"循环次数", -1, 817, 458, 75, 22));
        AddGroup(quickInputControls_, quickInputCount_ = MakeEdit(hwnd_, L"0", kQuickInputCount, 908, 458, 54, 22));
        AddGroup(quickInputControls_, MakeLabel(hwnd_, L"等待时间", -1, 817, 490, 75, 22));
        AddGroup(quickInputControls_, quickInputWait_ = MakeEdit(hwnd_, L"0.010", kQuickInputWait, 908, 490, 54, 22));
        AddGroup(quickInputControls_, MakeLabel(hwnd_, L"秒", -1, 968, 490, 32, 25));
        AddGroup(quickInputControls_, MakeLabel(hwnd_, L"随机时间", -1, 817, 522, 75, 22));
        AddGroup(quickInputControls_, quickInputRandom_ = MakeEdit(hwnd_, L"0.000", kQuickInputRandom, 908, 522, 54, 22));
        AddGroup(quickInputControls_, MakeLabel(hwnd_, L"秒", -1, 968, 522, 32, 25));
        AddGroup(loopControls_, MakeEditorLabel(hwnd_, L"循环类型", -1, kEditorPanelLeft, kEditorLabelAboveComboY, kEditorActionComboW, kEditorLabelAboveComboH));
        AddGroup(loopControls_, loopTypeCombo_ = MakeLabel(hwnd_, L"次数循环", 0, kEditorPanelLeft, kEditorParamComboY, kEditorActionComboW, kEditorActionComboH));
        popupLoopType_.items = {L"次数循环", L"变量"}; popupLoopType_.sel = 0;
        AddGroup(loopControls_, MakeLabel(hwnd_, L"循环次数", -1, 817, 244, 75, 22));
        AddGroup(loopControls_, loopCount_ = MakeEdit(hwnd_, L"-1", kLoopCount, 908, 244, 54, 22));
        AddGroup(loopControls_, MakeHint(hwnd_, L"*提示:-1表示无限循环", 817, 272, 195, 28));
        AddGroup(loopControls_, loopFromVar_ = MakeCheckBox(hwnd_, L"来自变量表达式", kLoopFromVar, 817, 308, 180, 25));
        AddGroup(loopControls_, loopVarExpr_ = MakeEdit(hwnd_, L"", kLoopVarExpr, 817, 340, 184, 22));
        AddGroup(loopControls_, MakeLabel(hwnd_, L"循环变量命名", -1, 817, 378, 120, 25));
        AddGroup(loopControls_, loopVarName_ = MakeEdit(hwnd_, L"", kLoopVarName, 817, 404, 184, 22));
        AddGroup(loopControls_, MakeHint(hwnd_, L"*提示:可用于标识当前循环次数，或作为数据索引", 807, 432, 195, 40));
        AddGroup(endLoopControls_, MakeHint(hwnd_, L"*提示:仅可添加到循环子节点，用于提前结束循环", 807, 183, 195, 48));
        AddGroup(defineBlockControls_, MakeEditorLabel(hwnd_, L"定义的宏指令块名称:", -1, kEditorPanelLeft, kEditorLabelAboveComboY, kEditorActionComboW, kEditorLabelAboveComboH));
        AddGroup(defineBlockControls_, defineBlockName_ = MakeEdit(hwnd_, L"block1", kDefineBlockName, kEditorPanelLeft, kEditorParamComboY, kEditorActionComboW, kEditorActionComboH));
        AddGroup(defineBlockControls_, MakeHint(hwnd_, L"*提示:块名称不能重复;\r\n块名称只能以字母开始, 后面可以加数字和字母如 'DaKai001';\r\n添加时宏模块自动添加到顶部。", kEditorPanelLeft, kEditorParamComboY + kEditorActionComboH + kEditorLabelGap, kEditorActionComboW, 96));
        AddGroup(runBlockControls_, MakeEditorLabel(hwnd_, L"要运行的宏指令块名称:", -1, kEditorPanelLeft, kEditorLabelAboveComboY, kEditorActionComboW, kEditorLabelAboveComboH));
        AddGroup(runBlockControls_, runBlockCombo_ = MakeLabel(hwnd_, L"", kRunBlockCombo, kEditorPanelLeft, kEditorParamComboY, kEditorActionComboW, kEditorActionComboH));
        popupRunBlock_.items.clear(); popupRunBlock_.sel = -1;
        AddGroup(ifControls_, MakeLabel(hwnd_, L"如果:", -1, kEditorPanelLeft, 180, 60, 22));
        AddGroup(ifControls_, MakeLabel(hwnd_, L"变量:", -1, kEditorPanelLeft, 214, 50, 22));
        AddGroup(ifControls_, ifVarCombo_ = MakeLabel(hwnd_, L"", kIfVarCombo, kEditorPanelLeft + 56, 212, kEditorActionComboW - 56, kEditorActionComboH));
        AddGroup(ifControls_, MakeLabel(hwnd_, L"判断:", -1, kEditorPanelLeft, 248, 50, 22));
        AddGroup(ifControls_, ifOperatorCombo_ = MakeLabel(hwnd_, L"等于", kIfOperator, kEditorPanelLeft + 56, 246, kEditorActionComboW - 56, kEditorActionComboH));
        popupIfOperator_.items = {L"等于", L"不等于", L"小于", L"小于等于", L"大于", L"大于等于", L"包含"}; popupIfOperator_.sel = 0;
        AddGroup(ifControls_, MakeLabel(hwnd_, L"值:", -1, kEditorPanelLeft, 282, 50, 22));
        AddGroup(ifControls_, ifValueEdit_ = MakeEdit(hwnd_, L"0", kIfValue, kEditorPanelLeft + 56, 280, kEditorActionComboW - 56, 22));
        AddGroup(ifControls_, MakeLabel(hwnd_, L"多条件连接方式:", -1, kEditorPanelLeft, 316, 120, 22));
        AddGroup(ifControls_, ifConnectorCombo_ = MakeLabel(hwnd_, L"并且(and)", kIfConnector, kEditorPanelLeft, 346, kEditorActionComboW, kEditorActionComboH));
        popupIfConnector_.items = {L"并且(and)", L"或者(or)", L"非(not)"}; popupIfConnector_.sel = 0;
        AddGroup(ifControls_, ifAddConditionBtn_ = MakeButton(hwnd_, L"添加判断条件", kIfAddCondition, kEditorPanelLeft, 378, kEditorActionComboW, 28));
        AddGroup(ifControls_, MakeLabel(hwnd_, L"判断条件:", -1, kEditorPanelLeft, 412, 80, 22));
        AddGroup(ifControls_, ifConditionList_ = MakeMultilineEdit(hwnd_, L"", kIfConditionList, kEditorPanelLeft, 436, kEditorActionComboW, 72));
        AddGroup(ifControls_, MakeHint(hwnd_, L"*提示:如需要更复杂的条件判断，请导出脚本操作", kEditorPanelLeft, 512, kEditorActionComboW, 28));
        AddGroup(elseControls_, MakeHint(hwnd_, L"*提示:必须和如果成对出现，作为如果节点的下方兄弟节点 (非子节点)！", 807, 183, 195, 48));
        AddGroup(lockScreenshotControls_, MakeHint(hwnd_, L"*提示:锁定屏幕截图，锁定后后续的找图、找色、颜色匹配、文字识别不会再次截图，而是使用锁定时的屏幕截图进行处理", 807, 183, 195, 96));
        AddGroup(unlockScreenshotControls_, MakeHint(hwnd_, L"*提示:解锁定屏幕截图，与锁定截屏成对出现，取消锁定后找图、找色、颜色匹配、文字识别每次使用都会再次截图进行处理", 807, 183, 195, 96));
        AddGroup(stopMacroControls_, MakeHint(hwnd_, L"*提示:会结束宏的运行，与您按下快捷键停止是一样的", 807, 183, 195, 48));

        AddGroup(runProgramControls_, MakeEditorLabel(hwnd_, L"要运行的程序:", -1, kEditorPanelLeft, kEditorLabelAboveComboY, kEditorActionComboW, kEditorLabelAboveComboH));
        AddGroup(runProgramControls_, runProgramCombo_ = MakeLabel(hwnd_, L"选择文件", kRunProgramCombo, kEditorPanelLeft, kEditorParamComboY, kEditorActionComboW, kEditorActionComboH));
        AddGroup(runProgramFileControls_, runProgramPath_ = MakeMultilineEdit(hwnd_, L"", kRunProgramPath, kEditorPanelLeft, 232, kEditorActionComboW, 64));
        AddGroup(runProgramFileControls_, runProgramBrowseBtn_ = MakeButton(hwnd_, L"选择要启动的程序", kRunProgramBrowse, kEditorPanelLeft, 304, kEditorActionComboW, 28));
        AddGroup(runProgramFileControls_, runProgramOrLabel_ = MakeLabel(hwnd_, L"或", -1, kEditorPanelLeft, 340, 20, 22));
        AddGroup(runProgramFileControls_, runProgramCrosshairBtn_ = MakeGreenButton(hwnd_, L"拖动准星查找程序", kRunProgramCrosshair, kEditorPanelLeft + 24, 336, kEditorActionComboW - 24, 32));
        AddGroup(runProgramControls_, MakeLabel(hwnd_, L"运行参数", -1, kEditorPanelLeft, 378, kEditorActionComboW, 22));
        AddGroup(runProgramControls_, runProgramArgs_ = MakeMultilineEdit(hwnd_, L"", kRunProgramArgs, kEditorPanelLeft, 404, kEditorActionComboW, 56));

        AddGroup(closeProgramControls_, MakeEditorLabel(hwnd_, L"要关闭的程序:", -1, kEditorPanelLeft, kEditorLabelAboveComboY, kEditorActionComboW, kEditorLabelAboveComboH));
        AddGroup(closeProgramControls_, closeProgramPath_ = MakeMultilineEdit(hwnd_, L"", kCloseProgramPath, kEditorPanelLeft, kEditorParamComboY, kEditorActionComboW, 64));
        AddGroup(closeProgramControls_, closeProgramBrowseBtn_ = MakeButton(hwnd_, L"选择要关闭的程序", kCloseProgramBrowse, kEditorPanelLeft, kEditorParamComboY + 64 + kEditorLabelGap, kEditorActionComboW, 28));
        AddGroup(closeProgramControls_, closeProgramOrLabel_ = MakeLabel(hwnd_, L"或", -1, kEditorPanelLeft, kEditorParamComboY + 64 + kEditorLabelGap + 32, 20, 22));
        AddGroup(closeProgramControls_, closeProgramCrosshairBtn_ = MakeGreenButton(hwnd_, L"拖动准星查找程序", kCloseProgramCrosshair, kEditorPanelLeft + 24, kEditorParamComboY + 64 + kEditorLabelGap + 28, kEditorActionComboW - 24, 32));
        AddGroup(closeProgramControls_, closeProgramMatchFileName_ = MakeCheckBox(hwnd_, L"匹配全路径", kCloseProgramMatchFileName, kEditorPanelLeft, kEditorParamComboY + 64 + kEditorLabelGap + 28 + 32 + 4, 120, 25));
        AddGroup(closeProgramControls_, MakeHint(hwnd_, L"*提示:选中只匹配文件名，否则匹配全路径", kEditorPanelLeft, kEditorParamComboY + 64 + kEditorLabelGap + 28 + 32 + 4 + 25 + kEditorLabelGap, kEditorActionComboW, kEditorParamHintH));

        AddGroup(openWebpageControls_, MakeEditorLabel(hwnd_, L"要打开的网页:", -1, kEditorPanelLeft, kEditorLabelAboveComboY, kEditorActionComboW, kEditorLabelAboveComboH));
        AddGroup(openWebpageControls_, openWebpageUrl_ = MakeMultilineEdit(hwnd_, L"", kOpenWebpageUrl, kEditorPanelLeft, kEditorParamComboY, kEditorActionComboW, 64));
        AddGroup(openWebpageControls_, MakeHint(hwnd_, L"*提示：可仅支持http:// https://开头的网址", kEditorPanelLeft, kEditorParamComboY + 64 + kEditorLabelGap, kEditorActionComboW, kEditorParamHintH));

        AddGroup(openFileControls_, MakeEditorLabel(hwnd_, L"要打开的文件:", -1, kEditorPanelLeft, kEditorLabelAboveComboY, kEditorActionComboW, kEditorLabelAboveComboH));
        AddGroup(openFileControls_, openFilePath_ = MakeMultilineEdit(hwnd_, L"", kOpenFilePath, kEditorPanelLeft, kEditorParamComboY, kEditorActionComboW, 64));
        AddGroup(openFileControls_, openFileBrowseBtn_ = MakeButton(hwnd_, L"选择要打开的文件", kOpenFileBrowse, kEditorPanelLeft, kEditorParamComboY + 72, kEditorActionComboW, 28));
        AddGroup(openFileControls_, MakeHint(hwnd_, L"*提示：请确保文件扩展名已关联默认程序，否则可能出现提示窗口，导致打开文件失败", kEditorPanelLeft, kEditorParamComboY + 108, kEditorActionComboW, 48));

        AddGroup(timerRecordControls_, MakeEditorLabel(hwnd_, L"计时器变量命名", -1, kEditorPanelLeft, kEditorLabelAboveComboY, kEditorActionComboW, kEditorLabelAboveComboH));
        AddGroup(timerRecordControls_, timerVarName_ = MakeEdit(hwnd_, L"", kTimerVarName, kEditorPanelLeft, kEditorParamComboY, kEditorActionComboW, kEditorActionComboH));
        AddGroup(timerRecordControls_, MakeHint(hwnd_, L"*提示: 记录从此动作到引用该变量时经过的秒数(向下取整); 同名变量会重置计时", kEditorPanelLeft, kEditorParamComboY + kEditorActionComboH + kEditorLabelGap, kEditorActionComboW, 40));

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
    }

    bool IsFooterControl(HWND hwnd) const {
        return hwnd == remarkLabel_ || hwnd == remark_ || hwnd == addBtn_ || hwnd == modifyBtn_;
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
                return RECT{
                    MulDiv(b.left, kEditorWidth, kEditorBaseWidth),
                    MulDiv(b.top, kEditorWidth, kEditorBaseWidth),
                    MulDiv(b.right, kEditorWidth, kEditorBaseWidth),
                    MulDiv(b.bottom, kEditorWidth, kEditorBaseWidth)
                };
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

    void ApplyEditorFooterLayout() {
        if (page_ != Page::Editor) return;
        const int panelBottom = ParamPanelBottomClient();
        const int gap = MulDiv(kEditorFooterGap, kEditorWidth, kEditorBaseWidth);
        int remarkY = panelBottom + gap;
        const int remarkH = MulDiv(22, kEditorHeight, kEditorBaseHeight);
        const int addGap = MulDiv(8, kEditorWidth, kEditorBaseWidth);
        int addY = remarkY + remarkH + addGap;
        const auto sx = [](int x) { return MulDiv(x, kEditorWidth, kEditorBaseWidth); };
        const auto sw = [](int w) { return std::max(1, MulDiv(w, kEditorWidth, kEditorBaseWidth)); };
        const auto sh = [](int h) { return std::max(1, MulDiv(h, kEditorHeight, kEditorBaseHeight)); };
        const int btnH = sh(30);
        const int cancelTop = cancelBtn_ ? WindowClientRect(cancelBtn_).top : MulDiv(702, kEditorHeight, kEditorBaseHeight);
        const int maxAddBottom = cancelTop - sh(6);
        if (addY + btnH > maxAddBottom) {
            addY = maxAddBottom - btnH;
            remarkY = addY - addGap - remarkH;
        }
        if (remarkLabel_) MoveWindow(remarkLabel_, sx(807), remarkY, sw(44), remarkH, FALSE);
        if (remark_) MoveWindow(remark_, sx(857), remarkY, sw(117), remarkH, FALSE);
        if (modifyBtn_) MoveWindow(modifyBtn_, sx(837), addY, sw(76), btnH, FALSE);
        if (addBtn_) MoveWindow(addBtn_, sx(927), addY, sw(76), btnH, FALSE);
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


    void ApplyEditorControlScale() {
        for (const auto& item : editorLayouts_) {
            if (IsFooterControl(item.hwnd)) continue;
            if (IsFindImageHwnd(item.hwnd)) {
                const int x = MulDiv(item.base.left, kEditorWidth, kEditorBaseWidth);
                const int y = MulDiv(item.base.top, kEditorWidth, kEditorBaseWidth);
                const int w = std::max(1, MulDiv(item.base.right - item.base.left, kEditorWidth, kEditorBaseWidth));
                const int h = std::max(1, MulDiv(item.base.bottom - item.base.top, kEditorWidth, kEditorBaseWidth));
                MoveWindow(item.hwnd, x, y, w, h, FALSE);
            } else if (IsOcrDepHwnd(item.hwnd) || IsOcrHwnd(item.hwnd)) {
                // OCR 控件由 RefreshOcrSubPanel 统一堆叠布局，不在此按初始坐标缩放
            } else {
                const int x = MulDiv(item.base.left, kEditorWidth, kEditorBaseWidth);
                const int y = MulDiv(item.base.top, kEditorHeight, kEditorBaseHeight);
                const int w = std::max(1, MulDiv(item.base.right - item.base.left, kEditorWidth, kEditorBaseWidth));
                const int h = std::max(1, MulDiv(item.base.bottom - item.base.top, kEditorHeight, kEditorBaseHeight));
                MoveWindow(item.hwnd, x, y, w, h, FALSE);
            }
        }
        ConfigureEditorCombos();
        if (page_ == Page::Editor && popupAction_.sel == 18) {
            RefreshOcrDepStatus();
            RefreshOcrSubPanel();
        }
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
        ShowEditorControls(true);
        HideEditorComboHwnds();
        RECT editorRc = EditorRectFromHome(homeRc);
        MoveWindow(hwnd_, editorRc.left, editorRc.top, kEditorWidth, kEditorHeight, FALSE);
        ApplyEditorControlScale();
        ApplyEditorFonts();
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
        RefreshParamPanel();
        RefreshRunBlockCombo();
        UpdateBatchToolbar();
        UpdateEditMode();
        InvalidateToolbarArea();
        StartHoverTimer();
        InvalidateRect(hwnd_, nullptr, TRUE);
        ShowWindow(hwnd_, SW_SHOW);
        ApplyEditorFooterLayout();
        InvalidateRect(hwnd_, nullptr, FALSE);
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
        ShowGroup(moveControls_, sel == 0);
        ShowGroup(waitControls_, sel == 1);
        ShowGroup(clickControls_, sel == 2);
        ShowGroup(mousePlaybackControls_, sel == 3);
        ShowGroup(runMacroControls_, sel == 4);
        ShowGroup(mousePressControls_, sel == 5 || sel == 6);
        ShowGroup(scrollWheelControls_, sel == 7);
        ShowGroup(keyPressControls_, sel == 9 || sel == 10);
        ShowGroup(keyControls_, sel == 8);
        ShowGroup(hotkeyShortcutControls_, sel == 11);
        ShowGroup(quickInputControls_, sel == 12);
        ShowGroup(loopControls_, sel == 13);
        ShowGroup(endLoopControls_, sel == 14);
        ShowGroup(defineBlockControls_, sel == 15);
        ShowGroup(runBlockControls_, sel == 16);
        ShowGroup(findImageControls_, sel == 17);
        if (sel == 17) {
            RefreshFindImageSubPanel();
        } else {
            ShowGroup(findImageOffsetControls_, false);
            ShowGroup(findImageVarControls_, false);
        }
        ShowGroup(ocrDepControls_, sel == 18);
        ShowGroup(ocrFindRegionToggleControls_, sel == 18);
        ShowGroup(ocrControls_, sel == 18);
        ShowGroup(ocrFollowControls_, sel == 18);
        if (sel == 18) {
            RefreshOcrDepStatus();
            RefreshOcrSubPanel();
        } else {
            ShowGroup(ocrSearchControls_, false);
            ShowGroup(ocrFollowOffsetControls_, false);
            ShowGroup(ocrFollowVarControls_, false);
            ShowGroup(ocrFindRegionToggleControls_, false);
            ShowGroup(ocrFindRegionControls_, false);
        }
        ShowGroup(ifControls_, sel == 19);
        ShowGroup(elseControls_, sel == 20);
        ShowGroup(lockScreenshotControls_, sel == 21);
        ShowGroup(unlockScreenshotControls_, sel == 22);
        ShowGroup(stopMacroControls_, sel == 23);
        ShowGroup(runProgramControls_, sel == 24);
        ShowGroup(runProgramFileControls_, sel == 24 && popupRunProgram_.sel <= 0);
        ShowGroup(closeProgramControls_, sel == 25);
        ShowGroup(openWebpageControls_, sel == 26);
        ShowGroup(openFileControls_, sel == 27);
        ShowGroup(timerRecordControls_, sel == 28);
        if (sel != 17 && sel != 18) ClearGrayButtonHover();
        if (sel == 17) { /* handled above */ }
        else if (sel == 18) { /* handled above */ }
        ApplyEditorFooterLayout();
        if (sel == 3) RefreshMousePlaybackCombo();
        if (sel == 4) RefreshRunMacroCombo();
        if (sel == 16) RefreshRunBlockCombo();
        if (sel == 12) RefreshQuickInputVarCombo();
        if (sel == 18) RefreshOcrSearchVarCombo();
        if (sel == 19) RefreshIfVarCombo();
        HideEditorComboHwnds();
        UpdateMoveVarControls();
        UpdateLoopVarControls();
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
        for (HWND h : {mode_, actionCombo_, mousePressButton_, clickButton_, loopTypeCombo_, runBlockCombo_, hotkeyShortcutCombo_, quickInputVarCombo_, runMacroCombo_, mousePlaybackCombo_, scrollDirectionCombo_, findFollowUpCombo_, ocrResultModeCombo_, ocrFollowUpCombo_, ocrSearchVarCombo_, ifVarCombo_, ifOperatorCombo_, ifConnectorCombo_, runProgramCombo_}) {
            if (h) ShowWindow(h, SW_HIDE);
        }
    }

    void RefreshFindImageSubPanel() {
        const bool saveVar = popupFindFollowUp_.sel == 2;
        ShowGroup(findImageOffsetControls_, !saveVar);
        ShowGroup(findImageVarControls_, saveVar);
        ApplyEditorFooterLayout();
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
        MoveWindow(findRegionLabel_, kFindContentLeft, kFindRegionRowY, labelW, kFindBtnH, FALSE);
        MoveWindow(findFullScreenBtn_, fullX, kFindRegionRowY, fullW, kFindBtnH, FALSE);
        MoveWindow(findSelectRegionBtn_, selectX, kFindRegionRowY, kFindBtnW, kFindBtnH, FALSE);
    }

    static int OcrScale(int designPx) {
        return MulDiv(designPx, kEditorWidth, kEditorBaseWidth);
    }

    void MoveOcrAt(HWND hwnd, int xDesign, int yClient, int wDesign, int hDesign) const {
        if (!hwnd) return;
        MoveWindow(hwnd,
            OcrScale(xDesign),
            yClient,
            std::max(1, OcrScale(wDesign)),
            std::max(1, OcrScale(hDesign)),
            FALSE);
    }

    void MoveOcrCoordLabel(const wchar_t* text, int yClient) const {
        const int x = (text[0] == L'X') ? kFindXLabelX : kFindYLabelX;
        for (HWND h : ocrControls_) {
            if (!h) continue;
            wchar_t buf[8]{};
            GetWindowTextW(h, buf, 8);
            if (wcscmp(buf, text) != 0) continue;
            wchar_t cls[16]{};
            GetClassNameW(h, cls, 16);
            if (wcscmp(cls, L"Static") == 0) {
                MoveOcrAt(h, x, yClient, kFindCoordLabelW, 22);
                return;
            }
        }
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
        MoveWindow(ocrRegionLabel_, OcrScale(kFindContentLeft), yClient,
            OcrScale(labelW), btnH, FALSE);
        MoveWindow(ocrFullScreenBtn_, OcrScale(fullX), yClient,
            OcrScale(fullW), btnH, FALSE);
        MoveWindow(ocrSelectRegionBtn_, OcrScale(kFindSelectRegionX), yClient,
            OcrScale(kFindBtnW), btnH, FALSE);
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
        case 7: return sel == 12;
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
        if (hwnd == quickInputVarCombo_ && IsParamComboVisible(7)) return 7;
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
        return -1;
    }

    int EditorComboPopupIdAtPoint(int x, int y) const {
        if (mode_ && PtIn(WindowClientRect(mode_), x, y)) return 0;
        if (actionCombo_ && PtIn(WindowClientRect(actionCombo_), x, y)) return 1;
        struct ComboHit { int id; HWND hwnd; };
        const ComboHit hits[] = {
            {2, mousePressButton_}, {3, clickButton_}, {4, loopTypeCombo_}, {5, runBlockCombo_},
            {6, hotkeyShortcutCombo_}, {7, quickInputVarCombo_}, {8, runMacroCombo_}, {9, mousePlaybackCombo_},
            {10, scrollDirectionCombo_}, {11, findFollowUpCombo_},
            {12, ifVarCombo_}, {13, ifOperatorCombo_}, {14, ifConnectorCombo_}, {15, runProgramCombo_},
            {16, ocrResultModeCombo_}, {17, ocrFollowUpCombo_}, {18, ocrSearchVarCombo_},
        };
        for (const auto& hit : hits) {
            if (!hit.hwnd || !IsParamComboVisible(hit.id)) continue;
            if (PtIn(WindowClientRect(hit.hwnd), x, y)) return hit.id;
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

    void RefreshQuickInputVarCombo() {
        quickInputVarItems_ = BuildQuickInputVarItems(actions_);
        popupQuickInputVar_.items.clear();
        for (const auto& item : quickInputVarItems_) {
            popupQuickInputVar_.items.push_back(item.display);
        }
        if (popupQuickInputVar_.sel < 0 || popupQuickInputVar_.sel >= static_cast<int>(popupQuickInputVar_.items.size())) {
            popupQuickInputVar_.sel = popupQuickInputVar_.items.empty() ? -1 : 0;
        }
        SetText(quickInputVarCombo_, popupQuickInputVar_.sel >= 0 && popupQuickInputVar_.sel < static_cast<int>(popupQuickInputVar_.items.size())
            ? popupQuickInputVar_.items[static_cast<size_t>(popupQuickInputVar_.sel)] : L"");
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
        if (popupAction_.sel == 12) RefreshQuickInputVarCombo();
        if (popupAction_.sel == 18) RefreshOcrSearchVarCombo();
        if (popupAction_.sel == 19) RefreshIfVarCombo();
    }

    void InsertQuickInputVariable() {
        if (!quickInputEdit_) return;
        const int sel = popupQuickInputVar_.sel;
        if (sel < 0 || sel >= static_cast<int>(quickInputVarItems_.size())) return;
        const std::wstring insert = quickInputVarItems_[static_cast<size_t>(sel)].insertText;
        DWORD start = 0, end = 0;
        SendMessageW(quickInputEdit_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
        const std::wstring current = GetText(quickInputEdit_);
        const std::wstring updated = current.substr(0, start) + insert + current.substr(end);
        SetText(quickInputEdit_, updated);
        const DWORD pos = start + static_cast<DWORD>(insert.size());
        SendMessageW(quickInputEdit_, EM_SETSEL, pos, pos);
        SetFocus(quickInputEdit_);
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
        if (popupAction_.sel == 18) RefreshOcrSubPanel();
    }

    int LayoutOcrSearchBlock(int y, int rowGap, int btnH, int fieldH, int tightGap, int insertBtnH) {
        MoveOcrAt(ocrSearchLabel_, kFindContentLeft, y, kFindBlockW, 22);
        y += fieldH + tightGap;
        MoveOcrAt(ocrSearchEdit_, kFindContentLeft, y, kFindBlockW, 22);
        y += fieldH + rowGap;
        MoveOcrAt(ocrSearchVarLabel_, kFindContentLeft, y + (btnH - fieldH) / 2, 50, 22);
        MoveOcrAt(ocrSearchVarCombo_, kFindContentLeft + 56, y, kFindFollowComboW, kFindBtnH);
        y += btnH + tightGap;
        MoveOcrAt(ocrSearchVarInsertBtn_, kFindContentLeft, y, kFindBlockW, 28);
        return y + insertBtnH + rowGap;
    }

    int LayoutOcrFindRegionBlock(int y) {
        const int rowGap = OcrScale(kFindVGap);
        const int btnH = OcrScale(kFindBtnH);
        const int fieldH = OcrScale(22);
        const int imageSize = OcrScale(kFindImageSize);
        const int sideBtnStep = OcrScale(kFindBtnH + kFindBtnStackGap);
        MoveOcrAt(ocrFindImageLabel_, kFindContentLeft, y, 90, kFindBtnH);
        MoveOcrAt(ocrFindSelectRegionBtn_, kFindSelectRegionX, y, kFindBtnW, kFindBtnH);
        y += btnH + rowGap;
        MoveOcrAt(ocrFindImagePreviewBtn_, kFindContentLeft, y, kFindImageSize, kFindImageSize);
        MoveOcrAt(ocrFindScreenshotBtn_, kFindActionBtnX, y, kFindBtnW, kFindBtnH);
        MoveOcrAt(ocrFindLocalImageBtn_, kFindActionBtnX, y + sideBtnStep, kFindBtnW, kFindBtnH);
        MoveOcrAt(ocrFindClearImageBtn_, kFindActionBtnX, y + sideBtnStep * 2, kFindBtnW, kFindBtnH);
        y += imageSize + rowGap;
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
        ShowGroup(ocrSearchControls_, searchMode);
        ShowGroup(ocrFollowOffsetControls_, !saveVar);
        ShowGroup(ocrFollowVarControls_, saveVar);
        ShowGroup(ocrFindRegionToggleControls_, true);
        ShowGroup(ocrFindRegionControls_, regionByImage);
        if (ocrRegionLabel_) ShowWindow(ocrRegionLabel_, regionByImage ? SW_HIDE : SW_SHOW);
        if (ocrFullScreenBtn_) ShowWindow(ocrFullScreenBtn_, regionByImage ? SW_HIDE : SW_SHOW);
        if (ocrSelectRegionBtn_) ShowWindow(ocrSelectRegionBtn_, regionByImage ? SW_HIDE : SW_SHOW);
        if (editorPopupOpen_ == 18) CloseEditorPopup();
        if (ocrUntilFound_) ShowWindow(ocrUntilFound_, searchMode ? SW_SHOW : SW_HIDE);

        const int depGap = OcrScale(kOcrDepToRegionGap);
        const int rowGap = OcrScale(kFindVGap);
        const int btnH = OcrScale(kFindBtnH);
        const int fieldH = OcrScale(22);
        const int tightGap = OcrScale(4);
        const int insertBtnH = OcrScale(28);

        int y = (ocrDepStatusLabel_
            ? WindowClientRect(ocrDepStatusLabel_).bottom
            : OcrScale(kOcrRegionRowY)) + depGap;

        if (ocrRegionByImageCheck_) {
            MoveOcrAt(ocrRegionByImageCheck_, kFindContentLeft, y, 180, 22);
            y += fieldH + rowGap;
        }

        if (regionByImage) {
            y = LayoutOcrFindRegionBlock(y);
        } else {
            SizeOcrRegionButtonsAt(y);
            y += btnH + rowGap;
        }

        MoveOcrCoordLabel(L"X1", y);
        MoveOcrAt(ocrX1_, kFindXEditX, y, kFindEditW, 22);
        MoveOcrCoordLabel(L"Y1", y);
        MoveOcrAt(ocrY1_, kFindYEditX, y, kFindEditW, 22);
        y += fieldH + rowGap;

        MoveOcrCoordLabel(L"X2", y);
        MoveOcrAt(ocrX2_, kFindXEditX, y, kFindEditW, 22);
        MoveOcrCoordLabel(L"Y2", y);
        MoveOcrAt(ocrY2_, kFindYEditX, y, kFindEditW, 22);
        y += fieldH + rowGap;

        MoveOcrAt(ocrResultModeLabel_, kFindContentLeft, y, kFindFollowLabelW, kFindBtnH);
        MoveOcrAt(ocrResultModeCombo_, kFindContentLeft + kFindFollowLabelW + 8, y, kFindFollowComboW, kFindBtnH);
        y += btnH + rowGap;

        if (searchMode) {
            y = LayoutOcrSearchBlock(y, rowGap, btnH, fieldH, tightGap, insertBtnH);
        }

        MoveOcrAt(ocrFollowUpLabel_, kFindContentLeft, y, kFindFollowLabelW, kFindBtnH);
        MoveOcrAt(ocrFollowUpCombo_, kFindContentLeft + kFindFollowLabelW + 8, y, kFindFollowComboW, kFindBtnH);
        y += btnH + rowGap;

        if (!saveVar) {
            MoveOcrAt(ocrOffsetXLabel_, kFindOffsetXLabelX, y, kFindOffsetLabelW, 22);
            MoveOcrAt(ocrOffsetX_, kFindOffsetXEditX, y, kFindEditW, 22);
            MoveOcrAt(ocrOffsetYLabel_, kFindOffsetYLabelX, y, kFindOffsetLabelW, 22);
            MoveOcrAt(ocrOffsetY_, kFindYEditX, y, kFindEditW, 22);
            y += fieldH + rowGap;
        } else {
            MoveOcrAt(ocrResultVarLabel_, kFindContentLeft, y, 100, 22);
            MoveOcrAt(ocrResultVar_, kFindContentLeft + 91, y, kOcrResultVarEditW, 22);
            y += fieldH + rowGap;
        }

        if (searchMode && ocrUntilFound_) {
            MoveOcrAt(ocrUntilFound_, kFindContentLeft, y, 140, 22);
            y += fieldH + rowGap;
        }

        const bool showSelectOffset = searchMode && !saveVar && !OcrSearchTextContainsVariable();
        MoveOcrAt(ocrTestBtn_, kFindContentLeft, y, kFindBtnW, kFindBtnH);
        if (ocrSelectOffsetBtn_) {
            if (showSelectOffset) {
                ShowWindow(ocrSelectOffsetBtn_, SW_SHOW);
                MoveOcrAt(ocrSelectOffsetBtn_, kOcrTestRowOffsetBtnX, y, kOcrTestRowOffsetBtnW, kFindBtnH);
            } else {
                ShowWindow(ocrSelectOffsetBtn_, SW_HIDE);
            }
        }
        y += btnH + rowGap;
        ApplyEditorFooterLayout();
        InvalidateOcrEditorPanel();
    }

    void InvalidateOcrEditorPanel() {
        if (!hwnd_) return;
        const RECT rc{
            MulDiv(kFindZoneLeft, kEditorWidth, kEditorBaseWidth),
            0,
            kEditorWidth,
            kEditorHeight - kBottomH
        };
        InvalidateRect(hwnd_, &rc, TRUE);
    }

    void RefreshOcrDepStatus() {
        if (!ocrDepStatusLabel_ || !ocrDepInstallBtn_) return;
        const OcrEnvStatus env = CheckOcrEnvironment(false);
        const bool ready = env.state == OcrEnvState::Ready;
        SetText(ocrDepStatusLabel_, ready ? L"文字识别已安装" : L"文字识别未安装");
        SetWindowTextW(ocrDepInstallBtn_, ready ? L"修复/更新" : L"一键安装");
        EnableWindow(ocrDepInstallBtn_, TRUE);
        if (actionCombo_) {
            const int gap = OcrScale(kEditorLabelGap);
            const int depY = WindowClientRect(actionCombo_).bottom + gap;
            const int x = OcrScale(kFindContentLeft);
            const int labelW = OcrScale(130);
            const int btnX = OcrScale(kFindActionBtnX);
            const int btnW = OcrScale(kFindBtnW);
            const int btnH = OcrScale(kFindBtnH);
            MoveWindow(ocrDepStatusLabel_, x, depY, labelW, btnH, FALSE);
            MoveWindow(ocrDepInstallBtn_, btnX, depY, btnW, btnH, FALSE);
        }
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
    }

    void BeginOcrRegionSelect() {
        if (!hwnd_) return;
        if (!screenshotOverlay_) screenshotOverlay_ = std::make_unique<ScreenshotOverlay>();
        screenshotOverlay_->SetTitle(L"选取区域");
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);
        screenshotOverlay_->Show([this](RECT sel) {
            if (sel.left == 0 && sel.top == 0 && sel.right == 0 && sel.bottom == 0) {
                SetWindowPos(hwnd_, HWND_TOP,
                    findRegionSavedRect_.left, findRegionSavedRect_.top,
                    findRegionSavedRect_.right - findRegionSavedRect_.left,
                    findRegionSavedRect_.bottom - findRegionSavedRect_.top,
                    SWP_SHOWWINDOW);
                return;
            }
            const int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            ocrFullScreen_ = false;
            SetText(ocrX1_, std::to_wstring(sel.left + vsX));
            SetText(ocrY1_, std::to_wstring(sel.top + vsY));
            SetText(ocrX2_, std::to_wstring(sel.right + vsX));
            SetText(ocrY2_, std::to_wstring(sel.bottom + vsY));
            SetWindowPos(hwnd_, HWND_TOP,
                findRegionSavedRect_.left, findRegionSavedRect_.top,
                findRegionSavedRect_.right - findRegionSavedRect_.left,
                findRegionSavedRect_.bottom - findRegionSavedRect_.top,
                SWP_SHOWWINDOW);
            SetForegroundWindow(hwnd_);
            InvalidateRect(hwnd_, nullptr, TRUE);
            UpdateWindow(hwnd_);
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
        if (!hwnd_) return;
        SetWindowPos(hwnd_, HWND_TOP,
            findRegionSavedRect_.left, findRegionSavedRect_.top,
            findRegionSavedRect_.right - findRegionSavedRect_.left,
            findRegionSavedRect_.bottom - findRegionSavedRect_.top,
            SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd_);
        InvalidateRect(hwnd_, nullptr, TRUE);
        UpdateWindow(hwnd_);
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
        ocrOverlay_->Show(sx1, sy1, sx2, sy2, searchTarget);
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
        default: return 14;
        }
    }

    bool IsImplementedActionPopup(int idx) const {
        return idx == 0 || idx == 1 || idx == 2 || idx == 3 || idx == 4 || idx == 5 || idx == 6 || idx == 7 || idx == 8 || idx == 9 || idx == 10
            || idx == 11 || idx == 12
            || idx == 13 || idx == 14 || idx == 15 || idx == 16 || idx == 17 || idx == 18 || idx == 19
            || idx == 20 || idx == 21 || idx == 22 || idx == 23 || idx == 24
            || idx == 25 || idx == 26 || idx == 27 || idx == 28;
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
            action.findUntilFound = Checked(findUntilFound_);
            action.matchVarName = Trim(GetText(findMatchVar_));
            if (action.matchVarName.empty()) action.matchVarName = L"matchRet";
        }
        else if (sel == 18) {
            action.type = ActionType::TextRecognition;
            const bool regionByImage = ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_);
            action.ocrRegionByImage = regionByImage;
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
            SetText(findX1_, std::to_wstring(action.searchX1));
            SetText(findY1_, std::to_wstring(action.searchY1));
            SetText(findX2_, std::to_wstring(action.searchX2));
            SetText(findY2_, std::to_wstring(action.searchY2));
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
            ocrFindImagePath_ = action.ocrRegionByImage ? action.imagePath : L"";
            if (action.ocrRegionByImage) {
                SetText(ocrFindMatchThreshold_, std::to_wstring(static_cast<int>(action.matchThreshold)));
                SetText(ocrFindScaleMin_, F3(action.imageScaleMin > 0.0 ? action.imageScaleMin : action.imageScale));
                SetText(ocrFindScaleMax_, F3(action.imageScaleMax > 0.0 ? action.imageScaleMax : action.imageScale));
                UpdateOcrFindImagePreview();
            } else {
                UpdateOcrFindImagePreview();
            }
            SetText(ocrX1_, std::to_wstring(action.searchX1));
            SetText(ocrY1_, std::to_wstring(action.searchY1));
            SetText(ocrX2_, std::to_wstring(action.searchX2));
            SetText(ocrY2_, std::to_wstring(action.searchY2));
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
        if (findImagePreviewBtn_) InvalidateRect(findImagePreviewBtn_, nullptr, FALSE);
    }

    void UpdateOcrFindImagePreview() {
        if (ocrFindImagePreviewBitmap_) {
            DeleteBitmapHandle(ocrFindImagePreviewBitmap_);
            ocrFindImagePreviewBitmap_ = nullptr;
        }
        if (!ocrFindImagePath_.empty()) {
            ocrFindImagePreviewBitmap_ = LoadBitmapFromFile(ocrFindImagePath_);
        }
        if (ocrFindImagePreviewBtn_) InvalidateRect(ocrFindImagePreviewBtn_, nullptr, FALSE);
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

    void ApplyFindImageFullScreen() {
        findImageFullScreen_ = true;
        int x = 0, y = 0, w = 0, h = 0;
        GetVirtualScreenRect(x, y, w, h);
        SetText(findX1_, std::to_wstring(x));
        SetText(findY1_, std::to_wstring(y));
        SetText(findX2_, std::to_wstring(x + w));
        SetText(findY2_, std::to_wstring(y + h));
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
                // Cancelled — restore main window
                SetWindowPos(hwnd_, HWND_TOP,
                    findRegionSavedRect_.left, findRegionSavedRect_.top,
                    findRegionSavedRect_.right - findRegionSavedRect_.left,
                    findRegionSavedRect_.bottom - findRegionSavedRect_.top,
                    SWP_SHOWWINDOW);
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
            // Restore main window and force repaint
            SetWindowPos(hwnd_, HWND_TOP,
                findRegionSavedRect_.left, findRegionSavedRect_.top,
                findRegionSavedRect_.right - findRegionSavedRect_.left,
                findRegionSavedRect_.bottom - findRegionSavedRect_.top,
                SWP_SHOWWINDOW);
            SetForegroundWindow(hwnd_);
            // Force full repaint so the preview and coordinates appear immediately
            InvalidateRect(hwnd_, nullptr, TRUE);
            UpdateWindow(hwnd_);
        });
    }

    void BeginOcrFindRegionSelect() {
        if (!ocrRegionByImageCheck_ || !Checked(ocrRegionByImageCheck_)) return;
        if (ocrFindImagePath_.empty()) {
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

        SetWindowPos(hwnd_, HWND_TOP,
            findRegionSavedRect_.left, findRegionSavedRect_.top,
            findRegionSavedRect_.right - findRegionSavedRect_.left,
            findRegionSavedRect_.bottom - findRegionSavedRect_.top,
            SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd_);
        if (popupAction_.sel == 18) {
            RefreshOcrDepStatus();
            RefreshOcrSubPanel();
        } else {
            InvalidateRect(hwnd_, nullptr, TRUE);
            UpdateWindow(hwnd_);
        }

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
            SetWindowPos(hwnd_, HWND_TOP,
                findRegionSavedRect_.left, findRegionSavedRect_.top,
                findRegionSavedRect_.right - findRegionSavedRect_.left,
                findRegionSavedRect_.bottom - findRegionSavedRect_.top,
                SWP_SHOWWINDOW);
            if (sel.left == 0 && sel.top == 0 && sel.right == 0 && sel.bottom == 0) return;
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
            SetForegroundWindow(hwnd_);
            InvalidateRect(hwnd_, nullptr, TRUE);
            UpdateWindow(hwnd_);
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
    }

    void BeginOcrOffsetSelect() {
        if (popupOcrResultMode_.sel != 1) return;
        if (OcrSearchTextContainsVariable()) return;
        if (Trim(GetText(ocrSearchEdit_)).empty()) {
            MessageBoxW(hwnd_, L"请先输入要在结果中查找的文字。", L"选择偏移", MB_OK | MB_ICONINFORMATION);
            return;
        }
        int sx1 = ToInt(ocrX1_), sy1 = ToInt(ocrY1_), sx2 = ToInt(ocrX2_), sy2 = ToInt(ocrY2_);
        if (ocrFullScreen_) {
            int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
            GetVirtualScreenRect(vsX, vsY, vsW, vsH);
            sx1 = vsX; sy1 = vsY; sx2 = vsX + vsW; sy2 = vsY + vsH;
        }
        std::wstring searchTarget;
        if (popupOcrResultMode_.sel == 1) {
            MacroVariableContext ctx;
            ctx.matchVars = &matchVars_;
            ctx.ocrVars = &ocrVars_;
            ctx.loopVars = &loopVars_;
            ctx.timerStarts = &timerStarts_;
            searchTarget = ResolveMacroVariables(GetText(ocrSearchEdit_), ctx);
        }

        if (!ocrOverlay_) ocrOverlay_ = std::make_unique<OcrOverlay>();
        GetWindowRect(hwnd_, &findRegionSavedRect_);
        ShowWindow(hwnd_, SW_HIDE);

        EnsureOcrSession();
        const auto result = ocrOverlay_->Show(sx1, sy1, sx2, sy2, searchTarget, OcrOverlayMode::OffsetPick);
        ReleaseOcrSession();

        SetWindowPos(hwnd_, HWND_TOP,
            findRegionSavedRect_.left, findRegionSavedRect_.top,
            findRegionSavedRect_.right - findRegionSavedRect_.left,
            findRegionSavedRect_.bottom - findRegionSavedRect_.top,
            SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd_);
        if (popupAction_.sel == 18) {
            RefreshOcrDepStatus();
            RefreshOcrSubPanel();
        } else {
            InvalidateRect(hwnd_, nullptr, TRUE);
            UpdateWindow(hwnd_);
        }

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

        // Restore main window
        SetWindowPos(hwnd_, HWND_TOP,
            findRegionSavedRect_.left, findRegionSavedRect_.top,
            findRegionSavedRect_.right - findRegionSavedRect_.left,
            findRegionSavedRect_.bottom - findRegionSavedRect_.top,
            SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd_);
        InvalidateRect(hwnd_, nullptr, TRUE);
        UpdateWindow(hwnd_);

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

        // Restore main window
        SetWindowPos(hwnd_, HWND_TOP,
            findRegionSavedRect_.left, findRegionSavedRect_.top,
            findRegionSavedRect_.right - findRegionSavedRect_.left,
            findRegionSavedRect_.bottom - findRegionSavedRect_.top,
            SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd_);
        InvalidateRect(hwnd_, nullptr, TRUE);
        UpdateWindow(hwnd_);

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
        if (id == kOcrRegionByImage && code == BN_CLICKED) { RefreshOcrSubPanel(); return; }
        if (id == kOcrFindSelectRegion) { BeginOcrFindRegionSelect(); return; }
        if (id == kOcrFindScreenshot) { BeginOcrFindScreenshot(); return; }
        if (id == kOcrFindLocalImage) { LoadOcrFindImageFromFile(); return; }
        if (id == kOcrFindImagePreview) { LoadOcrFindImageFromFile(); return; }
        if (id == kOcrFindClearImage) { ClearOcrFindImage(); return; }
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

    void CaptureKeyPress() {
        HotkeyCapture cap;
        Hotkey oldValue{};
        oldValue.vk = formKeyPressVk_;
        oldValue.text = formKeyPressText_.empty() ? VkName(formKeyPressVk_) : formKeyPressText_;
        oldValue.enabled = oldValue.vk != 0;
        Hotkey out;
        if (!cap.Show(hwnd_, oldValue, false, out) || !out.enabled || out.vk == 0) return;
        formKeyPressVk_ = out.vk;
        formKeyPressText_ = VkName(out.vk);
        SetText(keyPressEdit_, formKeyPressText_);
        SetChecked(keyPressLCtrl_, (out.modifiers & MOD_CONTROL) != 0);
        SetChecked(keyPressLAlt_, (out.modifiers & MOD_ALT) != 0);
        SetChecked(keyPressLShift_, (out.modifiers & MOD_SHIFT) != 0);
        SetChecked(keyPressLWin_, (out.modifiers & MOD_WIN) != 0);
        SetChecked(keyPressRCtrl_, false); SetChecked(keyPressRAlt_, false); SetChecked(keyPressRShift_, false); SetChecked(keyPressRWin_, false);
    }

    void CaptureActionKey() {
        HotkeyCapture cap;
        Hotkey oldValue{};
        oldValue.vk = formKeyVk_;
        oldValue.text = formKeyText_.empty() ? VkName(formKeyVk_) : formKeyText_;
        oldValue.enabled = oldValue.vk != 0;
        Hotkey out;
        if (!cap.Show(hwnd_, oldValue, false, out) || !out.enabled || out.vk == 0) return;
        formKeyVk_ = out.vk;
        formKeyText_ = VkName(out.vk);
        SetText(keyEdit_, formKeyText_);
        SetChecked(keyLCtrl_, (out.modifiers & MOD_CONTROL) != 0);
        SetChecked(keyLAlt_, (out.modifiers & MOD_ALT) != 0);
        SetChecked(keyLShift_, (out.modifiers & MOD_SHIFT) != 0);
        SetChecked(keyLWin_, (out.modifiers & MOD_WIN) != 0);
        SetChecked(keyRCtrl_, false); SetChecked(keyRAlt_, false); SetChecked(keyRShift_, false); SetChecked(keyRWin_, false);
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
            const auto& a = actions_[i];
            file << L"    {\n";
            file << L"      \"type\": \"" << JsonType(a.type) << L"\",\n";
            file << L"      \"text\": \"" << EscapeJson(a.customText) << L"\",\n";
            file << L"      \"remark\": \"" << EscapeJson(a.remark) << L"\",\n";
            file << L"      \"no\": " << a.originalNo << L",\n";
            file << L"      \"indent\": " << a.indent << L",\n";
            file << L"      \"x\": " << a.x << L",\n";
            file << L"      \"y\": " << a.y << L",\n";
            file << L"      \"randomX\": " << a.randomX << L",\n";
            file << L"      \"randomY\": " << a.randomY << L",\n";
            file << L"      \"moveFromVar\": " << (a.moveFromVar ? 1 : 0) << L",\n";
            file << L"      \"moveVarExprX\": \"" << EscapeJson(a.moveVarExprX) << L"\",\n";
            file << L"      \"moveVarExprY\": \"" << EscapeJson(a.moveVarExprY) << L"\",\n";
            file << L"      \"button\": \"" << JsonButton(a.button) << L"\",\n";
            file << L"      \"keyText\": \"" << EscapeJson(a.keyText) << L"\",\n";
            file << L"      \"keyVk\": " << a.keyVk << L",\n";
            file << L"      \"holdLeftWin\": " << (a.holdLeftWin ? 1 : 0) << L",\n";
            file << L"      \"holdRightWin\": " << (a.holdRightWin ? 1 : 0) << L",\n";
            file << L"      \"holdLeftCtrl\": " << (a.holdLeftCtrl ? 1 : 0) << L",\n";
            file << L"      \"holdRightCtrl\": " << (a.holdRightCtrl ? 1 : 0) << L",\n";
            file << L"      \"holdLeftAlt\": " << (a.holdLeftAlt ? 1 : 0) << L",\n";
            file << L"      \"holdRightAlt\": " << (a.holdRightAlt ? 1 : 0) << L",\n";
            file << L"      \"holdLeftShift\": " << (a.holdLeftShift ? 1 : 0) << L",\n";
            file << L"      \"holdRightShift\": " << (a.holdRightShift ? 1 : 0) << L",\n";
            file << L"      \"clickCount\": " << a.clickCount << L",\n";
            file << L"      \"duration\": " << a.duration << L",\n";
            file << L"      \"randomDuration\": " << a.randomDuration << L",\n";
            file << L"      \"loopCount\": " << a.loopCount << L",\n";
            file << L"      \"loopVarName\": \"" << EscapeJson(a.loopVarName) << L"\",\n";
            file << L"      \"loopFromVar\": " << (a.loopFromVar ? 1 : 0) << L",\n";
            file << L"      \"loopVarExpr\": \"" << EscapeJson(a.loopVarExpr) << L"\",\n";
            file << L"      \"blockName\": \"" << EscapeJson(a.blockName) << L"\",\n";
            file << L"      \"targetPath\": \"" << EscapeJson(a.targetPath) << L"\",\n";
            file << L"      \"shortcutPreset\": " << a.shortcutPreset << L",\n";
            file << L"      \"inputText\": \"" << EscapeJson(a.inputText) << L"\",\n";
            file << L"      \"charInterval\": " << a.charInterval << L",\n";
            file << L"      \"scrollVertical\": " << (a.scrollVertical ? 1 : 0) << L",\n";
            file << L"      \"scrollHorizontal\": " << (a.scrollHorizontal ? 1 : 0) << L",\n";
            file << L"      \"scrollSteps\": " << a.scrollSteps << L",\n";
            file << L"      \"scrollDirection\": " << a.scrollDirection << L",\n";
            file << L"      \"searchX1\": " << a.searchX1 << L",\n";
            file << L"      \"searchY1\": " << a.searchY1 << L",\n";
            file << L"      \"searchX2\": " << a.searchX2 << L",\n";
            file << L"      \"searchY2\": " << a.searchY2 << L",\n";
            file << L"      \"searchFullScreen\": " << (a.searchFullScreen ? 1 : 0) << L",\n";
            const std::wstring savedImagePath = [&]() -> std::wstring {
                if (a.type == ActionType::FindImage && !a.imagePath.empty()) {
                    return ImagePathForJson(EnsureImageInLibrary(a.imagePath));
                }
                if (a.type == ActionType::TextRecognition && a.ocrRegionByImage && !a.imagePath.empty()) {
                    return ImagePathForJson(EnsureImageInLibrary(a.imagePath));
                }
                return a.imagePath;
            }();
            file << L"      \"imagePath\": \"" << EscapeJson(savedImagePath) << L"\",\n";
            file << L"      \"matchThreshold\": " << a.matchThreshold << L",\n";
            file << L"      \"imageScale\": " << a.imageScale << L",\n";
            file << L"      \"imageScaleMin\": " << a.imageScaleMin << L",\n";
            file << L"      \"imageScaleMax\": " << a.imageScaleMax << L",\n";
            file << L"      \"findImageFollowUp\": " << a.findImageFollowUp << L",\n";
            file << L"      \"offsetX\": " << a.offsetX << L",\n";
            file << L"      \"offsetY\": " << a.offsetY << L",\n";
            file << L"      \"findUntilFound\": " << (a.findUntilFound ? 1 : 0) << L",\n";
            file << L"      \"matchVarName\": \"" << EscapeJson(a.matchVarName) << L"\",\n";
            file << L"      \"ocrResultMode\": " << a.ocrResultMode << L",\n";
            file << L"      \"ocrRegionByImage\": " << (a.ocrRegionByImage ? 1 : 0) << L",\n";
            file << L"      \"ocrSearchText\": \"" << EscapeJson(a.ocrSearchText) << L"\",\n";
            file << L"      \"ocrFollowUp\": " << a.ocrFollowUp << L",\n";
            file << L"      \"conditionExpr\": \"" << EscapeJson(a.conditionExpr) << L"\",\n";
            file << L"      \"matchFileNameOnly\": " << (a.matchFileNameOnly ? 1 : 0) << L"\n";
            file << L"    }" << (i + 1 == actions_.size() ? L"\n" : L",\n");
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
        ScriptAction a{};
        const auto type = ExtractString(block, L"type");
        if (type.empty()) return a;
        if (type == L"moveMouse") a.type = ActionType::MoveMouse;
        else if (type == L"mouseDown") a.type = ActionType::MouseDown;
        else if (type == L"mouseUp") a.type = ActionType::MouseUp;
        else if (type == L"mouseClick") a.type = ActionType::MouseClick;
        else if (type == L"mousePlayback") a.type = ActionType::MousePlayback;
        else if (type == L"runMacro") a.type = ActionType::RunMacro;
        else if (type == L"keyDown") a.type = ActionType::KeyDown;
        else if (type == L"keyUp") a.type = ActionType::KeyUp;
        else if (type == L"keyClick") a.type = ActionType::KeyClick;
        else if (type == L"hotkeyShortcut") a.type = ActionType::HotkeyShortcut;
        else if (type == L"quickInput") a.type = ActionType::QuickInput;
        else if (type == L"scrollWheel") a.type = ActionType::ScrollWheel;
        else if (type == L"findImage") a.type = ActionType::FindImage;
        else if (type == L"textRecognition") a.type = ActionType::TextRecognition;
        else if (type == L"wait") a.type = ActionType::Wait;
        else if (type == L"loop") a.type = ActionType::Loop;
        else if (type == L"endLoop") a.type = ActionType::EndLoop;
        else if (type == L"defineBlock") a.type = ActionType::DefineBlock;
        else if (type == L"runBlock") a.type = ActionType::RunBlock;
        else if (type == L"if") a.type = ActionType::If;
        else if (type == L"else") a.type = ActionType::Else;
        else if (type == L"lockScreenshot") a.type = ActionType::LockScreenshot;
        else if (type == L"unlockScreenshot") a.type = ActionType::UnlockScreenshot;
        else if (type == L"stopMacro") a.type = ActionType::StopMacro;
        else if (type == L"runProgram") a.type = ActionType::RunProgram;
        else if (type == L"closeProgram") a.type = ActionType::CloseProgram;
        else if (type == L"openWebpage") a.type = ActionType::OpenWebpage;
        else if (type == L"openFile") a.type = ActionType::OpenFile;
        else if (type == L"timerRecordTime") a.type = ActionType::TimerRecordTime;
        else a.type = ActionType::CustomText;
        a.customText = ExtractString(block, L"text");
        a.remark = ExtractString(block, L"remark");
        a.originalNo = static_cast<int>(ExtractNumber(block, L"no", static_cast<double>(fallbackNo + 1)));
        a.indent = static_cast<int>(ExtractNumber(block, L"indent", 0));
        a.x = static_cast<int>(ExtractNumber(block, L"x", 0));
        a.y = static_cast<int>(ExtractNumber(block, L"y", 0));
        a.randomX = static_cast<int>(ExtractNumber(block, L"randomX", 0));
        a.randomY = static_cast<int>(ExtractNumber(block, L"randomY", 0));
        a.moveFromVar = ExtractNumber(block, L"moveFromVar", 0) != 0;
        a.moveVarExprX = ExtractString(block, L"moveVarExprX");
        a.moveVarExprY = ExtractString(block, L"moveVarExprY");
        const auto button = ExtractString(block, L"button");
        a.button = button == L"right" ? MouseButtonType::Right : button == L"middle" ? MouseButtonType::Middle : MouseButtonType::Left;
        a.keyText = ExtractString(block, L"keyText");
        if (a.keyText.empty()) a.keyText = L"7";
        a.keyVk = static_cast<UINT>(ExtractNumber(block, L"keyVk", a.keyText.size() == 1 ? towupper(a.keyText[0]) : '7'));
        a.holdLeftWin = ExtractNumber(block, L"holdLeftWin", 0) != 0;
        a.holdRightWin = ExtractNumber(block, L"holdRightWin", 0) != 0;
        a.holdLeftCtrl = ExtractNumber(block, L"holdLeftCtrl", 0) != 0;
        a.holdRightCtrl = ExtractNumber(block, L"holdRightCtrl", 0) != 0;
        a.holdLeftAlt = ExtractNumber(block, L"holdLeftAlt", 0) != 0;
        a.holdRightAlt = ExtractNumber(block, L"holdRightAlt", 0) != 0;
        a.holdLeftShift = ExtractNumber(block, L"holdLeftShift", 0) != 0;
        a.holdRightShift = ExtractNumber(block, L"holdRightShift", 0) != 0;
        a.clickCount = static_cast<int>(ExtractNumber(block, L"clickCount", 1));
        a.duration = ExtractNumber(block, L"duration", 0.1);
        a.randomDuration = ExtractNumber(block, L"randomDuration", 0);
        a.loopCount = static_cast<int>(ExtractNumber(block, L"loopCount", -1));
        a.loopVarName = ExtractString(block, L"loopVarName");
        a.loopFromVar = ExtractNumber(block, L"loopFromVar", 0) != 0;
        a.loopVarExpr = ExtractString(block, L"loopVarExpr");
        a.blockName = ExtractString(block, L"blockName");
        a.targetPath = ExtractString(block, L"targetPath");
        a.shortcutPreset = static_cast<int>(ExtractNumber(block, L"shortcutPreset", 0));
        a.inputText = ExtractString(block, L"inputText");
        a.charInterval = ExtractNumber(block, L"charInterval", 0.01);
        a.scrollVertical = ExtractNumber(block, L"scrollVertical", 1) != 0;
        a.scrollHorizontal = ExtractNumber(block, L"scrollHorizontal", 0) != 0;
        a.scrollSteps = static_cast<int>(ExtractNumber(block, L"scrollSteps", 1));
        a.scrollDirection = static_cast<int>(ExtractNumber(block, L"scrollDirection", 0));
        a.searchX1 = static_cast<int>(ExtractNumber(block, L"searchX1", 0));
        a.searchY1 = static_cast<int>(ExtractNumber(block, L"searchY1", 0));
        a.searchX2 = static_cast<int>(ExtractNumber(block, L"searchX2", 0));
        a.searchY2 = static_cast<int>(ExtractNumber(block, L"searchY2", 0));
        a.searchFullScreen = ExtractNumber(block, L"searchFullScreen", 1) != 0;
        a.imagePath = ExtractString(block, L"imagePath");
        if (!a.imagePath.empty()) a.imagePath = ResolveImagePath(a.imagePath);
        a.matchThreshold = ExtractNumber(block, L"matchThreshold", 65.0);
        a.imageScale = ExtractNumber(block, L"imageScale", 1.0);
        a.imageScaleMin = ExtractNumber(block, L"imageScaleMin", a.imageScale);
        a.imageScaleMax = ExtractNumber(block, L"imageScaleMax", a.imageScale);
        a.findImageFollowUp = static_cast<int>(ExtractNumber(block, L"findImageFollowUp", 0));
        a.offsetX = static_cast<int>(ExtractNumber(block, L"offsetX", 0));
        a.offsetY = static_cast<int>(ExtractNumber(block, L"offsetY", 0));
        a.findUntilFound = ExtractNumber(block, L"findUntilFound", 0) != 0;
        a.matchVarName = ExtractString(block, L"matchVarName");
        if (a.matchVarName.empty()) {
            a.matchVarName = a.type == ActionType::TextRecognition ? L"a" : L"matchRet";
        }
        a.ocrResultMode = static_cast<int>(ExtractNumber(block, L"ocrResultMode", 0));
        a.ocrRegionByImage = ExtractNumber(block, L"ocrRegionByImage", 0) != 0;
        a.ocrSearchText = ExtractString(block, L"ocrSearchText");
        a.ocrFollowUp = static_cast<int>(ExtractNumber(block, L"ocrFollowUp", 0));
        a.conditionExpr = ExtractString(block, L"conditionExpr");
        a.matchFileNameOnly = ExtractNumber(block, L"matchFileNameOnly", 0) != 0;
        return a;
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
        const int rightLimit = r.right - 168;
        const int hotW = TextWidth(ScriptHotkeyText(scripts_[static_cast<size_t>(i)]), hotFont_);
        const int nameW = TextWidth(scripts_[static_cast<size_t>(i)].name, font_);
        const int hotLeft = std::min(left + nameW + 10, rightLimit - hotW);
        const int hotRight = std::min(hotLeft + hotW, rightLimit);
        return RECT{hotLeft, r.top + 17, hotRight, r.top + 48};
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
    RECT ClickerLeftRadioRect() const { return RECT{167, 171, 187, 191}; }
    RECT ClickerMiddleRadioRect() const { return RECT{303, 171, 323, 191}; }
    RECT ClickerRightRadioRect() const { return RECT{437, 171, 457, 191}; }
    RECT ClickerIntervalRect() const { return RECT{245, 236, 575, 266}; }
    RECT ClickerHotkeyRect() const { return RECT{245, 303, 575, 333}; }
    RECT ClickerIntervalPopupRect() const { RECT rc = ClickerIntervalRect(); return RECT{rc.left, rc.bottom, rc.right, rc.bottom + 231}; }
    RECT ClickerIntervalOptionRect(int index) const {
        RECT popup = ClickerIntervalPopupRect();
        return RECT{popup.left, popup.top + index * 77, popup.right, popup.top + (index + 1) * 77};
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
        if (self && msg == WM_ERASEBKGND) {
            // Suppress background erase for owner-draw buttons and static labels
            // to prevent flickering between erase and parent paint
            wchar_t clsName[32];
            GetClassNameW(hwnd, clsName, 32);
            if (lstrcmpW(clsName, L"STATIC") == 0 || self->IsGrayButton(hwnd)) return 1;
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
        if (self && msg == WM_LBUTTONDOWN) {
            const int popupId = self->EditorComboPopupIdForHwnd(hwnd);
            if (popupId >= 0) { self->ToggleEditorPopup(popupId); return 0; }
        }
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
        return RECT{kColRemarkClient + 1, r.top + (kRowH - kRemarkEditH) / 2, kColRemarkClient + 1 + kRemarkEditW, r.top + (kRowH - kRemarkEditH) / 2 + kRemarkEditH};
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
        if (btn == HoverButton::Close || btn == HoverButton::Minimize) {
            RECT rc = btn == HoverButton::Close ? CloseRect() : MinimizeRect();
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
            if (activeHomeTab_ == quickscript::MainTab::Clicker && clickerIntervalOpen_) {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
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
        if (page_ == Page::Home) {
            if (PtIn(ClickerTabRect(), x, y) || PtIn(RecorderTabRect(), x, y) || PtIn(MacroTabRect(), x, y) || PtIn(ScriptCustomTabRect(), x, y)) return HoverButton::HomeCard;
            if (activeHomeTab_ == quickscript::MainTab::Clicker) {
                if (clickerIntervalOpen_ && PtIn(ClickerIntervalPopupRect(), x, y)) return HoverButton::ClickerInterval;
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
            if (activeHomeTab_ == quickscript::MainTab::ScriptCustom) return HoverButton::None;
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
                if (!PtIn(drop, pt.x, pt.y)) CloseEditorPopup();
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
        clickerIntervalOpen_ = false;
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
        if (clickerIntervalOpen_) {
            const quickscript::ClickIntervalMode modes[3] = {
                quickscript::ClickIntervalMode::Custom,
                quickscript::ClickIntervalMode::Efficient,
                quickscript::ClickIntervalMode::Extreme
            };
            for (int i = 0; i < 3; ++i) {
                if (PtIn(ClickerIntervalOptionRect(i), x, y)) {
                    if (modes[i] == quickscript::ClickIntervalMode::Custom) {
                        clickerIntervalOpen_ = false;
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        ShowCustomIntervalDialog();
                        return true;
                    }
                    clickerSettings_.intervalMode = modes[i];
                    clickerIntervalOpen_ = false;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return true;
                }
            }
            if (!PtIn(ClickerIntervalRect(), x, y)) {
                clickerIntervalOpen_ = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return true;
            }
        }
        if (PtIn(ClickerIntervalRect(), x, y)) {
            clickerIntervalOpen_ = !clickerIntervalOpen_;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
        return false;
    }

    void ShowCustomIntervalDialog() {
        struct DialogState { double val; bool ok; bool done; HWND edit; };
        DialogState state{clickerSettings_.customIntervalSeconds, false, false, nullptr};
        const wchar_t* clsName = L"QuickScriptCustomIntervalDlg";
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                auto* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                auto cancelRect = []() { return RECT{88, 132, 158, 166}; };
                auto okRect = []() { return RECT{182, 132, 252, 166}; };
                auto closeRect = []() { return RECT{296, 4, 336, 36}; };
                auto editRect = []() { return RECT{108, 72, 208, 100}; };
                if (msg == WM_CREATE) {
                    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                    st = reinterpret_cast<DialogState*>(cs->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
                    st->edit = CreateWindowExW(0, L"EDIT", nullptr,
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_CENTER,
                        editRect().left, editRect().top,
                        editRect().right - editRect().left, editRect().bottom - editRect().top,
                        hwnd, reinterpret_cast<HMENU>(100), g_instance, nullptr);
                    wchar_t buf[32]{};
                    swprintf_s(buf, L"%.3f", st->val);
                    SetWindowTextW(st->edit, buf);
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
                    DrawTextW(hdc, L"  鼠大侠-自定义连点间隔", -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, closeFont);
                    RECT close = closeRect();
                    DrawTextW(hdc, L"×", -1, &close, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SetTextColor(hdc, RGB(50, 50, 50));
                    RECT unitRc{212, 76, 240, 100};
                    SelectObject(hdc, btnFont);
                    DrawTextW(hdc, L"秒", -1, &unitRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
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
        if (PtIn(ClickerBannerKeyRect(), x, y)) { CaptureGlobalHotkey(); return; }
        if (PtIn(ClickerHotkeyRect(), x, y)) { ShowHotkeyMenuAt(ClickerHotkeyRect()); return; }
        if (PtIn(ClickerLeftRadioRect(), x, y)) { clickerSettings_.button = quickscript::MouseButtonChoice::Left; RegisterAllHotkeys(); }
        else if (PtIn(ClickerMiddleRadioRect(), x, y)) { clickerSettings_.button = quickscript::MouseButtonChoice::Middle; RegisterAllHotkeys(); }
        else if (PtIn(ClickerRightRadioRect(), x, y)) { clickerSettings_.button = quickscript::MouseButtonChoice::Right; RegisterAllHotkeys(); }
        else if (PtIn(CreateRect(), x, y)) return;
        else if (clickerIntervalOpen_) {
            clickerIntervalOpen_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
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
                    st->edit = CreateWindowExW(0, L"EDIT", nullptr,
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                        editRect().left, editRect().top,
                        editRect().right - editRect().left, editRect().bottom - editRect().top,
                        hwnd, reinterpret_cast<HMENU>(100), g_instance, nullptr);
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
        if (PtIn(TimerRect(), x, y)) { MessageBoxW(hwnd_, L"定时录制功能暂未开放。", L"录制", MB_OK | MB_ICONINFORMATION); return; }
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
                    MessageBoxW(hwnd_, L"优化功能暂未开放。", L"录制", MB_OK | MB_ICONINFORMATION); return;
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
        if (PtIn(TimerRect(), x, y)) { MessageBoxW(hwnd_, L"定时功能暂未开放。", L"提示", MB_OK | MB_ICONINFORMATION); return; }
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
        if (activeHomeTab_ == quickscript::MainTab::ScriptCustom) return;
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
        }
        if (running_ && ShouldIgnoreHotkeyStop()) return;
        if (id == HOTKEY_GLOBAL_ID) {
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

    int RandomInt(int maxValue) { if (maxValue <= 0) return 0; std::uniform_int_distribution<int> dist(-maxValue, maxValue); return dist(rng_); }
    double RandomDelay(double maxValue) { if (maxValue <= 0) return 0; std::uniform_real_distribution<double> dist(0.0, maxValue); return dist(rng_); }
    void SleepInterruptible(double seconds) { const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000.0)); while (!stopFlag_ && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(10)); }

    std::wstring ResolveQuickInputText(const std::wstring& text) {
        MacroVariableContext ctx;
        ctx.matchVars = &matchVars_;
        ctx.ocrVars = workerUsesOcrVars_ ? &ocrVars_ : nullptr;
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
        running_ = true; stopFlag_ = false; wasVisibleBeforeRun_ = IsWindowVisible(hwnd_) == TRUE; wasMinimizedBeforeRun_ = IsIconic(hwnd_) == TRUE;
        CloseEditorPopup(); CancelQuickInputTip(); AddTray(); ShowWindow(hwnd_, SW_HIDE);
        const auto actions = actions_;
        const std::wstring selfPath = currentPath_;
        worker_ = std::thread([this, actions, selfPath]() {
            bool usesOcr = ScriptUsesTextRecognition(actions);
            workerUsesOcrVars_ = usesOcr;
            matchVars_.clear();
            if (usesOcr) ocrVars_.clear();
            loopVars_.clear();
            timerStarts_.clear();
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
                ctx.loopVars = &loopVars_;
                ctx.timerStarts = &timerStarts_;
                ctx.curLoops = curLoops_;
                return ctx;
            };

            auto executeOne = [this, &usesOcr, &holdOcrSession, &heldKeyVk, &runRange, &runningScriptPath, &activeActions, &lockedScreen_, &lockedVirtX_, &lockedVirtY_, &clearLockedScreen, &makeVarCtx](const ScriptAction& a) {
                if (a.type == ActionType::MoveMouse) {
                    int x = a.x;
                    int y = a.y;
                    if (a.moveFromVar) {
                        MacroVariableContext ctx = makeVarCtx();
                        if (!TryResolveIntOperand(a.moveVarExprX, ctx, x)) x = 0;
                        if (!TryResolveIntOperand(a.moveVarExprY, ctx, y)) y = 0;
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
                else if (a.type == ActionType::MouseClick) for (int i = 0; i < a.clickCount && !stopFlag_; ++i) { SleepInterruptible(a.duration + RandomDelay(a.randomDuration)); if (!stopFlag_) { MarkSimulatedInput(); SendHeldModifiers(a, true); MouseClick(a.button); SendHeldModifiers(a, false); UnmarkSimulatedInput(); } }
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
                    do {
                        const ImageMatchResult match = runFind();
                        if (a.findImageFollowUp == 2) {
                            matchVars_[a.matchVarName] = match;
                            if (match.found) break;
                            if (!a.findUntilFound) break;
                        } else if (match.found) {
                            const int tx = match.x + a.offsetX;
                            const int ty = match.y + a.offsetY;
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
                            x1, y1, x2, y2, lockedScreen_, lockedVirtX_, lockedVirtY_);
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
                    if (a.ocrResultMode == 0 && a.ocrFollowUp != 2) {
                        int x1 = a.searchX1, y1 = a.searchY1, x2 = a.searchX2, y2 = a.searchY2;
                        if (!resolveOcrRegion(x1, y1, x2, y2)) {
                            ocrVars_[varName] = MakeOcrTextVarResult(L"");
                            return;
                        }
                        const OcrEngineOutput output = RunOcrOnScreenRegion(
                            x1, y1, x2, y2, lockedScreen_, lockedVirtX_, lockedVirtY_);
                        ocrVars_[varName] = output.success
                            ? MakeOcrTextVarResult(ConcatOcrLines(output))
                            : MakeOcrTextVarResult(L"");
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
                        ocrVars_[varName] = runOcrAction();
                    } else {
                        do {
                            const OcrVarResult result = runOcrAction();
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
            };

            runBlockByName = [&](const std::wstring& name) {
                if (stopFlag_ || name.empty()) return;
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
                        const size_t bodyEnd = containerBodyEnd(i);
                        int iter = 1;
                        bool broke = false;
                        const auto loopStartTime = std::chrono::steady_clock::now();
                        while (!stopFlag_ && !broke) {
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
                matchVars_.clear();
                if (usesOcr) ocrVars_.clear();
                loopVars_.clear();
                timerStarts_.clear();
                runRange(0, actions.size());
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

    void StopRun() { stopFlag_ = true; }
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
    void OnRunDone() { running_ = false; if (worker_.joinable()) worker_.join(); RemoveTray(); if (wasVisibleBeforeRun_) ShowWindow(hwnd_, wasMinimizedBeforeRun_ ? SW_MINIMIZE : SW_SHOW); }
    void AddTray() { NOTIFYICONDATAW nid{}; nid.cbSize = sizeof(nid); nid.hWnd = hwnd_; nid.uID = 1; nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON; nid.uCallbackMessage = WM_TRAY; nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION); wcscpy_s(nid.szTip, L"鼠大侠-鼠标宏运行中"); Shell_NotifyIconW(NIM_ADD, &nid); }
    void RemoveTray() { NOTIFYICONDATAW nid{}; nid.cbSize = sizeof(nid); nid.hWnd = hwnd_; nid.uID = 1; Shell_NotifyIconW(NIM_DELETE, &nid); }
    void RestoreFromTray() { ShowWindow(hwnd_, SW_SHOW); SetForegroundWindow(hwnd_); }

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
        }
        g_recordStartTick.store(GetTickCount(), std::memory_order_relaxed);
        g_lastMouseMoveTick.store(g_recordStartTick.load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_recording = true;
        recording_ = true;
        recordingWasVisible_ = IsWindowVisible(hwnd_) == TRUE;
        CloseEditorPopup(); CancelQuickInputTip();
        AddTray();
        ShowWindow(hwnd_, SW_HIDE);
    }

    void StopRecording() {
        g_recording = false;
        recording_ = false;
        SetRecordingIgnoreHotkey(0, 0, false);
        UninstallRecordingHooks();
        RemoveTray();
        if (recordingWasVisible_) ShowWindow(hwnd_, SW_SHOW);
        ConvertRecordedToActions();
        if (!actions_.empty()) SaveRecording();
    }

    static bool RecordedEventMatchesHotkey(const RecordedEvent& e, const Hotkey& hk) {
        if (!hk.enabled || !hk.vk) return false;
        if (hk.vk == VK_LBUTTON) return e.msg == WM_LBUTTONDOWN || e.msg == WM_LBUTTONUP;
        if (hk.vk == VK_RBUTTON) return e.msg == WM_RBUTTONDOWN || e.msg == WM_RBUTTONUP;
        if (hk.vk == VK_MBUTTON) return e.msg == WM_MBUTTONDOWN || e.msg == WM_MBUTTONUP;
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

        bool isKeyHeld = false;    // track key-hold state to avoid dupes
        bool isMouseHeld[3] = {};  // L, R, M

        for (size_t i = 0; i < events.size(); ++i) {
            const auto& e = events[i];
            double timeSec = e.timeOffsetMs / 1000.0;

            // Move mouse
            if (e.msg == WM_MOUSEMOVE) {
                // Add the time since the last event as a Wait
                double prevSec = (i > 0) ? (events[i - 1].timeOffsetMs / 1000.0) : 0.0;
                double gap = timeSec - prevSec;
                if (gap > 0.001) {
                    ScriptAction wa{};
                    wa.type = ActionType::Wait;
                    wa.duration = gap;
                    wa.remark = L"";
                    actions_.push_back(wa);
                }

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
                double gap = timeSec - prevSec;
                if (gap > 0.001) {
                    ScriptAction wa{};
                    wa.type = ActionType::Wait;
                    wa.duration = gap;
                    actions_.push_back(wa);
                }

                ScriptAction kd{};
                kd.type = ActionType::KeyDown;
                kd.keyVk = static_cast<UINT>(e.vkOrButton);
                kd.keyText = VkName(kd.keyVk);
                actions_.push_back(kd);
                isKeyHeld = true;
                continue;
            }
            if (e.msg == WM_KEYUP || e.msg == WM_SYSKEYUP) {
                double prevSec = (i > 0) ? (events[i - 1].timeOffsetMs / 1000.0) : 0.0;
                double gap = timeSec - prevSec;
                if (gap > 0.001) {
                    ScriptAction wa{};
                    wa.type = ActionType::Wait;
                    wa.duration = gap;
                    actions_.push_back(wa);
                }

                ScriptAction ku{};
                ku.type = ActionType::KeyUp;
                ku.keyVk = static_cast<UINT>(e.vkOrButton);
                ku.keyText = VkName(ku.keyVk);
                actions_.push_back(ku);
                isKeyHeld = false;
                continue;
            }

            // Mouse button down / up
            int mbtnIdx = -1;
            if (e.vkOrButton == VK_LBUTTON) mbtnIdx = 0;
            else if (e.vkOrButton == VK_RBUTTON) mbtnIdx = 1;
            else if (e.vkOrButton == VK_MBUTTON) mbtnIdx = 2;

            if (mbtnIdx >= 0 && (e.msg == WM_LBUTTONDOWN || e.msg == WM_RBUTTONDOWN || e.msg == WM_MBUTTONDOWN)) {
                double prevSec = (i > 0) ? (events[i - 1].timeOffsetMs / 1000.0) : 0.0;
                double gap = timeSec - prevSec;
                if (gap > 0.001) {
                    ScriptAction wa{};
                    wa.type = ActionType::Wait;
                    wa.duration = gap;
                    actions_.push_back(wa);
                }

                ScriptAction md{};
                md.type = ActionType::MouseDown;
                md.button = static_cast<MouseButtonType>(mbtnIdx);
                actions_.push_back(md);
                isMouseHeld[mbtnIdx] = true;
                continue;
            }
            if (mbtnIdx >= 0 && (e.msg == WM_LBUTTONUP || e.msg == WM_RBUTTONUP || e.msg == WM_MBUTTONUP)) {
                double prevSec = (i > 0) ? (events[i - 1].timeOffsetMs / 1000.0) : 0.0;
                double gap = timeSec - prevSec;
                if (gap > 0.001) {
                    ScriptAction wa{};
                    wa.type = ActionType::Wait;
                    wa.duration = gap;
                    actions_.push_back(wa);
                }

                ScriptAction mu{};
                mu.type = ActionType::MouseUp;
                mu.button = static_cast<MouseButtonType>(mbtnIdx);
                actions_.push_back(mu);
                isMouseHeld[mbtnIdx] = false;
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
        {
            std::lock_guard<std::mutex> lock(g_recordMutex);
            saveDurationSeconds_ = g_recordedEvents.empty() ? 0.0 : g_recordedEvents.back().timeOffsetMs / 1000.0;
        }
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
        AddTray();
        ShowWindow(hwnd_, SW_HIDE);
        clickerThread_ = std::thread([this]() {
            while (clicking_ && !stopFlag_) {
                // Determine the click interval
                double interval = 0.1; // default
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

                // Send mouse click
                DWORD downFlag, upFlag;
                if (clickerSettings_.button == quickscript::MouseButtonChoice::Left) {
                    downFlag = MOUSEEVENTF_LEFTDOWN; upFlag = MOUSEEVENTF_LEFTUP;
                } else if (clickerSettings_.button == quickscript::MouseButtonChoice::Right) {
                    downFlag = MOUSEEVENTF_RIGHTDOWN; upFlag = MOUSEEVENTF_RIGHTUP;
                } else {
                    downFlag = MOUSEEVENTF_MIDDLEDOWN; upFlag = MOUSEEVENTF_MIDDLEUP;
                }

                INPUT inputs[2]{};
                inputs[0].type = INPUT_MOUSE;
                inputs[0].mi.dwFlags = downFlag;
                inputs[1].type = INPUT_MOUSE;
                inputs[1].mi.dwFlags = upFlag;
                SendInput(2, inputs, sizeof(INPUT));

                // Sleep with stop check (polling)
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
        ShowWindow(hwnd_, SW_SHOW);
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
        RECT minimize = MinimizeRect();
        RECT close = CloseRect();
        if (hoverButton_ == HoverButton::Minimize) FillAlphaRect(hdc, minimize, RGB(0, 0, 0), kCloseHoverAlpha);
        if (hoverButton_ == HoverButton::Close) FillAlphaRect(hdc, close, RGB(0, 0, 0), kCloseHoverAlpha);
        SelectObject(hdc, closeFont_);
        DrawTextIn(hdc, L"−", minimize, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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

    void DrawClickerCombo(HDC hdc, RECT rc, const std::wstring& text) {
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
    }

    void DrawClickerCombo(HDC hdc, RECT rc) { DrawClickerCombo(hdc, rc, ClickIntervalTitle(clickerSettings_.intervalMode)); }

    void PaintClickerIntervalPopup(HDC hdc);

    // ── Editor popup combo drawing ─────────────────────────────────
    PopupCombo* GetEditorPopup() {
        switch (editorPopupOpen_) {
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
        default: return nullptr;
        }
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
        case 7: return WindowClientRect(quickInputVarCombo_);
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
        case 7: combo = quickInputVarCombo_; break;
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
        const RECT actionRc = WindowClientRect(actionCombo_);
        const RECT panelRc{actionRc.left, std::max<LONG>(0, actionRc.top - 28), kEditorWidth, kEditorHeight - kBottomH};
        InvalidateRect(hwnd_, &panelRc, FALSE);
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
        else if (id == 7) RefreshQuickInputVarCombo();
        else if (id == 8) RefreshRunMacroCombo();
        else if (id == 9) RefreshMousePlaybackCombo();
        else if (id == 12) RefreshIfVarCombo();
        else if (id == 16) RefreshOcrSubPanel();
        else if (id == 17) RefreshOcrSubPanel();
        else if (id == 18) RefreshOcrSearchVarCombo();
        PopupCombo* pc = GetEditorPopup();
        if (pc) pc->open = true;
        SyncEditorDropPopup();
        if (prevId >= 0) InvalidateEditorComboArea(prevId);
        InvalidateEditorComboArea(id);
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
        case 7: label = quickInputVarCombo_; break;
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
        }
        if (label) SetText(label, pc->items[static_cast<size_t>(idx)]);
        if (editorPopupOpen_ == 1) RefreshParamPanel();
        else if (editorPopupOpen_ == 11) RefreshFindImageSubPanel();
        else if (editorPopupOpen_ == 16) RefreshOcrSubPanel();
        else if (editorPopupOpen_ == 17) RefreshOcrSubPanel();
        else if (editorPopupOpen_ == 15) UpdateRunProgramSubPanel();
        const int closedId = editorPopupOpen_;
        pc->open = false;
        editorPopupOpen_ = -1;
        editorPopupHover_ = -1;
        SyncEditorDropPopup();
        InvalidateEditorComboArea(closedId);
        if (closedId == 1) InvalidateEditorParamPanel();
        else if (closedId == 16) InvalidateOcrEditorPanel();
        else if (closedId == 17) InvalidateOcrEditorPanel();
        else if (closedId == 18) InvalidateOcrEditorPanel();
        else if (closedId == 11) InvalidateEditorParamPanel();
        else if (closedId == 15) InvalidateEditorParamPanel();
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
        if (IsGrayButton(dis->hwndItem)) { DrawGrayButton(dis); return; }
        DrawOwnerButton(dis);
    }

    bool IsGrayButton(HWND hwnd) const {
        return hwnd == findFullScreenBtn_ || hwnd == findSelectRegionBtn_ || hwnd == findTestBtn_
            || hwnd == findImagePreviewBtn_
            || hwnd == findScreenshotBtn_ || hwnd == findLocalImageBtn_ || hwnd == findClearImageBtn_
            || hwnd == findSelectOffsetBtn_
            || hwnd == ocrFullScreenBtn_ || hwnd == ocrSelectRegionBtn_ || hwnd == ocrTestBtn_
            || hwnd == ocrSelectOffsetBtn_
            || hwnd == ocrFindSelectRegionBtn_ || hwnd == ocrFindImagePreviewBtn_
            || hwnd == ocrFindScreenshotBtn_ || hwnd == ocrFindLocalImageBtn_ || hwnd == ocrFindClearImageBtn_;
    }

    HWND HitGrayButton(int x, int y) const {
        if (popupAction_.sel == 17) {
            const HWND buttons[] = {
                findFullScreenBtn_, findSelectRegionBtn_, findTestBtn_, findImagePreviewBtn_,
                findScreenshotBtn_, findLocalImageBtn_, findClearImageBtn_,
                findSelectOffsetBtn_
            };
            for (HWND btn : buttons) {
                if (!btn || !IsWindowVisible(btn)) continue;
                if (PtIn(WindowClientRect(btn), x, y)) return btn;
            }
            return nullptr;
        }
        if (popupAction_.sel == 18) {
            const HWND buttons[] = {
                ocrFullScreenBtn_, ocrSelectRegionBtn_, ocrTestBtn_, ocrSelectOffsetBtn_,
                ocrFindSelectRegionBtn_, ocrFindImagePreviewBtn_, ocrFindScreenshotBtn_,
                ocrFindLocalImageBtn_, ocrFindClearImageBtn_
            };
            for (HWND btn : buttons) {
                if (!btn || !IsWindowVisible(btn)) continue;
                if (PtIn(WindowClientRect(btn), x, y)) return btn;
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
        const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
        const bool hovered = dis->hwndItem == hoverGrayBtn_;
        const COLORREF fill = pressed ? kGrayButtonHover : (hovered ? kGrayButtonHover : kGrayButton);
        HBRUSH brush = CreateSolidBrush(fill);
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);
        if (dis->hwndItem == findImagePreviewBtn_ || dis->hwndItem == ocrFindImagePreviewBtn_) {
            HBITMAP preview = dis->hwndItem == findImagePreviewBtn_
                ? findImagePreviewBitmap_ : ocrFindImagePreviewBitmap_;
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
            return;
        }
        wchar_t text[64]{};
        GetWindowTextW(dis->hwndItem, text, 64);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, kGrayButtonText);
        SelectObject(hdc, editorFont_ ? editorFont_ : font_);
        DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
        BitBlt(windowDc, ps.rcPaint.left, ps.rcPaint.top, blitW, blitH, hdc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        if (page_ == Page::Home && activeHomeTab_ == quickscript::MainTab::Clicker) {
            if (clickerIntervalOpen_) PaintClickerIntervalPopup(windowDc);
        }
        SelectObject(hdc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(hdc);
        DeleteObject(green); DeleteObject(white); DeleteObject(panel); EndPaint(hwnd_, &ps);
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
        SelectObject(hdc, homeFont_);
        DrawTextIn(hdc, L"点击类型:", RECT{61, 169, 152, 194}, kWhite);
        DrawRadio(hdc, ClickerLeftRadioRect(), clickerSettings_.button == quickscript::MouseButtonChoice::Left);
        DrawTextIn(hdc, L"鼠标左键", RECT{196, 169, 286, 194}, kWhite);
        DrawRadio(hdc, ClickerMiddleRadioRect(), clickerSettings_.button == quickscript::MouseButtonChoice::Middle);
        DrawTextIn(hdc, L"鼠标中键", RECT{332, 169, 422, 194}, kWhite);
        DrawRadio(hdc, ClickerRightRadioRect(), clickerSettings_.button == quickscript::MouseButtonChoice::Right);
        DrawTextIn(hdc, L"鼠标右键", RECT{466, 169, 556, 194}, kWhite);

        DrawTextIn(hdc, L"每次点击间隔时间:", RECT{61, 238, 220, 263}, kWhite);
        DrawClickerCombo(hdc, ClickerIntervalRect());
        DrawTextIn(hdc, L"启停的全局热键:", RECT{61, 305, 220, 330}, kWhite);
        DrawClickerCombo(hdc, ClickerHotkeyRect(), globalHotkey_.text.empty() ? L"F8" : globalHotkey_.text);

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
        SelectObject(hdc, homeFont_);
        DrawTextIn(hdc, L"脚本定制功能接口已预留", RECT{0, 220, kHomeWidth, 255}, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(hdc, L"后续可在这里接入脚本模板、定制任务和自动化向导。", RECT{0, 258, kHomeWidth, 292}, RGB(220, 245, 225), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        HBRUSH y = CreateSolidBrush(kCreateYellow);
        RECT hint = CreateRect();
        FillRect(hdc, &hint, y);
        DeleteObject(y);
        SelectObject(hdc, bigFont_);
        DrawTextIn(hdc, L"点击", RECT{hint.left + 190, hint.top, hint.left + 270, hint.bottom}, RGB(60, 60, 60), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT word{hint.left + 280, hint.top + 21, hint.left + 390, hint.bottom - 21};
        HBRUSH orange = CreateSolidBrush(kOrange);
        FillRect(hdc, &word, orange);
        DeleteObject(orange);
        DrawTextIn(hdc, L"定制", word, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(hdc, L"脚本", RECT{word.right + 10, hint.top, hint.right - 160, hint.bottom}, RGB(60, 60, 60), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
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
            DrawTextIn(hdc, scripts_[static_cast<size_t>(i)].name, RECT{r.left + 14, r.top + 17, std::max(r.left + 14, hotRc.left - 10), r.top + 48}, kWhite);
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

    void PaintEditorTipPopupContent(HDC hdc, HWND popupHwnd);

    SIZE MeasureQuickInputTipSize() const;

    void DrawEditorFieldBorder(HDC hdc, HWND ctrl);

    void PaintActionList(HDC hdc);

    void PaintDragMarker(HDC hdc);

    LRESULT OnCtlColor(HDC hdc) { SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, kText); return reinterpret_cast<LRESULT>(whiteBrush_); }
    LRESULT OnEditColor(HDC hdc) { SetBkMode(hdc, OPAQUE); SetTextColor(hdc, kText); SetBkColor(hdc, kWhite); return reinterpret_cast<LRESULT>(whiteBrush_); }
    void Cleanup() { if (crosshairDrag_.IsActive()) crosshairDrag_.End(); CloseEditorPopup(); CancelQuickInputTip(); if (editorDropPopup_) { DestroyWindow(editorDropPopup_); editorDropPopup_ = nullptr; } if (editorTipPopup_) { DestroyWindow(editorTipPopup_); editorTipPopup_ = nullptr; } StopClickerCleanup(); StopRecordingCleanup(); stopFlag_ = true; if (worker_.joinable()) worker_.join(); ReleaseAllHeldInputs(); RemoveTray(); UnregisterHotKey(hwnd_, HOTKEY_GLOBAL_ID); for (int i = 0; i < 100; ++i) UnregisterHotKey(hwnd_, HOTKEY_SCRIPT_BASE + i); UninstallGlobalHotkeyHooks(); if (crosshairDragCursor_) { DestroyCursor(crosshairDragCursor_); crosshairDragCursor_ = nullptr; } if (findImagePreviewBitmap_) { DeleteBitmapHandle(findImagePreviewBitmap_); findImagePreviewBitmap_ = nullptr; } if (ocrFindImagePreviewBitmap_) { DeleteBitmapHandle(ocrFindImagePreviewBitmap_); ocrFindImagePreviewBitmap_ = nullptr; } DeleteObject(font_); DeleteObject(editorFont_); DeleteObject(bigFont_); DeleteObject(titleFont_); DeleteObject(hotFont_); DeleteObject(closeFont_); DeleteObject(homeFont_); DeleteObject(homeTabFont_); DeleteObject(whiteBrush_); DeleteObject(lineGreenBrush_); }
    void StopClickerCleanup();
    void StopRecordingCleanup() { if (recording_) { g_recording = false; recording_ = false; UninstallRecordingHooks(); } }

    HWND hwnd_ = nullptr; HFONT font_ = nullptr; HFONT editorFont_ = nullptr; HFONT bigFont_ = nullptr; HFONT titleFont_ = nullptr; HFONT hotFont_ = nullptr; HFONT closeFont_ = nullptr; HFONT homeFont_ = nullptr; HFONT homeTabFont_ = nullptr; HBRUSH whiteBrush_ = nullptr; HBRUSH lineGreenBrush_ = nullptr;
    HWND name_ = nullptr; HWND mode_ = nullptr; HWND labelList_ = nullptr; HWND labelBatchCount_ = nullptr; HWND actionCombo_ = nullptr; HWND addBtn_ = nullptr; HWND modifyBtn_ = nullptr; HWND clearBtn_ = nullptr; HWND loadBtn_ = nullptr;
    HWND batchExitBtn_ = nullptr; HWND batchSelectAllBtn_ = nullptr; HWND batchDeselectBtn_ = nullptr; HWND batchDeleteBtn_ = nullptr; HWND batchCopyBtn_ = nullptr;
    HWND cancelBtn_ = nullptr; HWND saveBtn_ = nullptr; HWND crosshairBtn_ = nullptr;
    HWND runProgramCombo_ = nullptr; HWND runProgramPath_ = nullptr; HWND runProgramBrowseBtn_ = nullptr; HWND runProgramOrLabel_ = nullptr; HWND runProgramCrosshairBtn_ = nullptr; HWND runProgramArgs_ = nullptr;
    HWND closeProgramPath_ = nullptr; HWND closeProgramBrowseBtn_ = nullptr; HWND closeProgramOrLabel_ = nullptr; HWND closeProgramCrosshairBtn_ = nullptr; HWND closeProgramMatchFileName_ = nullptr;
    HWND openWebpageUrl_ = nullptr; HWND openFilePath_ = nullptr; HWND openFileBrowseBtn_ = nullptr; HWND timerVarName_ = nullptr;
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
    HWND findTestBtn_ = nullptr; HWND findImagePreviewBtn_ = nullptr; HWND findScreenshotBtn_ = nullptr; HWND findLocalImageBtn_ = nullptr; HWND findClearImageBtn_ = nullptr;
    HWND findMatchThreshold_ = nullptr; HWND findScaleMin_ = nullptr; HWND findScaleMax_ = nullptr; HWND findOffsetX_ = nullptr; HWND findOffsetY_ = nullptr; HWND findSelectOffsetBtn_ = nullptr; HWND findUntilFound_ = nullptr; HWND findMatchVar_ = nullptr;
    HWND ocrDepStatusLabel_ = nullptr; HWND ocrDepInstallBtn_ = nullptr;
    HWND ocrRegionLabel_ = nullptr; HWND ocrFullScreenBtn_ = nullptr; HWND ocrSelectRegionBtn_ = nullptr;
    HWND ocrX1_ = nullptr; HWND ocrY1_ = nullptr; HWND ocrX2_ = nullptr; HWND ocrY2_ = nullptr;
    HWND ocrResultModeLabel_ = nullptr; HWND ocrResultModeCombo_ = nullptr;
    HWND ocrSearchLabel_ = nullptr; HWND ocrSearchEdit_ = nullptr; HWND ocrSearchVarLabel_ = nullptr; HWND ocrSearchVarCombo_ = nullptr; HWND ocrSearchVarInsertBtn_ = nullptr;
    HWND ocrFollowUpLabel_ = nullptr; HWND ocrFollowUpCombo_ = nullptr;
    HWND ocrOffsetXLabel_ = nullptr; HWND ocrOffsetX_ = nullptr; HWND ocrOffsetYLabel_ = nullptr; HWND ocrOffsetY_ = nullptr;
    HWND ocrSelectOffsetBtn_ = nullptr;
    HWND ocrResultVarLabel_ = nullptr; HWND ocrUntilFound_ = nullptr; HWND ocrResultVar_ = nullptr; HWND ocrTestBtn_ = nullptr;
    HWND ocrRegionByImageCheck_ = nullptr;
    HWND ocrFindImageLabel_ = nullptr; HWND ocrFindSelectRegionBtn_ = nullptr; HWND ocrFindImagePreviewBtn_ = nullptr;
    HWND ocrFindScreenshotBtn_ = nullptr; HWND ocrFindLocalImageBtn_ = nullptr; HWND ocrFindClearImageBtn_ = nullptr;
    HWND ocrFindMatchThreshold_ = nullptr; HWND ocrFindScaleMin_ = nullptr; HWND ocrFindScaleMax_ = nullptr;
    HWND quickInputEdit_ = nullptr; HWND quickInputVarCombo_ = nullptr; HWND quickInputInsertBtn_ = nullptr; HWND quickInputCharInterval_ = nullptr; HWND quickInputCount_ = nullptr; HWND quickInputWait_ = nullptr; HWND quickInputRandom_ = nullptr;
    HWND ifVarCombo_ = nullptr; HWND ifOperatorCombo_ = nullptr; HWND ifValueEdit_ = nullptr; HWND ifConnectorCombo_ = nullptr; HWND ifAddConditionBtn_ = nullptr; HWND ifConditionList_ = nullptr;
    std::vector<HWND> editorControls_, moveControls_, waitControls_, mousePressControls_, clickControls_, mousePlaybackControls_, runMacroControls_, keyPressControls_, keyControls_, hotkeyShortcutControls_, quickInputControls_, loopControls_, endLoopControls_, defineBlockControls_, runBlockControls_, scrollWheelControls_, findImageControls_, findImageOffsetControls_, findImageVarControls_, ocrDepControls_, ocrFindRegionToggleControls_, ocrControls_, ocrFindRegionControls_, ocrSearchControls_, ocrFollowControls_, ocrFollowOffsetControls_, ocrFollowVarControls_, ifControls_, elseControls_, lockScreenshotControls_, unlockScreenshotControls_, stopMacroControls_, runProgramControls_, runProgramFileControls_, closeProgramControls_, openWebpageControls_, openFileControls_, timerRecordControls_;
    std::vector<EditorControlLayout> editorLayouts_;
    std::vector<HotkeyMenuItem> hotkeyMenuItems_;
    std::vector<ScriptMeta> scripts_; std::vector<ScriptMeta> recordings_; std::vector<ScriptAction> actions_;
    std::set<int> collapsedContainers_;
    Page page_ = Page::Home; quickscript::MainTab activeHomeTab_ = quickscript::MainTab::Clicker; Hotkey globalHotkey_{0, VK_F8, L"F8", true};
    RECT homeRectBeforeEditor_{};
    int selectedScript_ = -1, selectedRecording_ = -1, currentScriptIndex_ = -1, homeHover_ = -1, recordingHover_ = -1, hoverIndex_ = -1, selectedIndex_ = -1, editingRemarkIndex_ = -1, copySource_ = -1, dragIndex_ = -1, dragTargetIndex_ = -1, dragTargetIndent_ = 0, dragStartX_ = 0, dragStartY_ = 0, scrollOffset_ = 0, homeScrollOffset_ = 0, homeScrollbarDragOffset_ = 0, editorScrollbarDragOffset_ = 0, pendingDeleteIndex_ = -1;
    HoverButton hoverButton_ = HoverButton::None;
    HWND hoverGrayBtn_ = nullptr;
    HWND pendingHoverGrayOld_ = nullptr, pendingHoverGrayNew_ = nullptr;
    bool dragging_ = false, dragMoved_ = false, dragTargetNested_ = false, homeScrollbarDragging_ = false, editorScrollbarDragging_ = false, trackingMouse_ = false, deleteConfirmVisible_ = false, hasHomeRectBeforeEditor_ = false, wasVisibleBeforeRun_ = true, wasMinimizedBeforeRun_ = false, loadingForm_ = false, batchEditMode_ = false, clickerIntervalOpen_ = false, findImageFullScreen_ = true, ocrFullScreen_ = true;
    CrosshairDragController crosshairDrag_;
    HWND hoverCrosshairBtn_ = nullptr;
    int editorPopupOpen_ = -1;
    int editorPopupHover_ = -1;
    int editorPopupScroll_ = 0;
    HWND editorDropPopup_ = nullptr;
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
    PopupCombo popupMode_, popupAction_, popupMouseBtn_, popupClickBtn_, popupLoopType_, popupRunBlock_, popupHotkeyShortcut_, popupQuickInputVar_, popupRunMacro_, popupMousePlayback_, popupScrollDir_, popupFindFollowUp_, popupOcrResultMode_, popupOcrFollowUp_, popupOcrSearchVar_, popupIfVar_, popupIfOperator_, popupIfConnector_, popupRunProgram_;
    std::vector<QuickInputVarItem> quickInputVarItems_;
    std::vector<std::wstring> runMacroPaths_, mousePlaybackPaths_;
    std::wstring findImagePath_;
    std::wstring ocrFindImagePath_;
    std::unordered_set<std::wstring> newImagePaths_;      // 编辑期间新增的图片路径，用于取消时清理
    std::atomic<bool> findTestRunning_{false};
    std::atomic<bool> ocrTestRunning_{false};
    bool workerUsesOcrVars_ = false;
    HBITMAP findImagePreviewBitmap_ = nullptr;
    HBITMAP ocrFindImagePreviewBitmap_ = nullptr;
    RECT findRegionSavedRect_{};
    std::unique_ptr<MatchOverlay> matchOverlay_;
    std::unique_ptr<OcrOverlay> ocrOverlay_;
    std::unique_ptr<ScreenshotOverlay> screenshotOverlay_;
    std::unordered_map<std::wstring, ImageMatchResult> matchVars_;
    std::unordered_map<std::wstring, OcrVarResult> ocrVars_;
    std::unordered_map<std::wstring, int> loopVars_;
    std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> timerStarts_;
    int curLoops_ = 0;
    std::atomic<int> simulatingInputDepth_{0};
    DWORD lastHotkeyTick_ = 0;
    std::atomic_bool running_{false}, stopFlag_{false}; std::thread worker_; std::mt19937 rng_{std::random_device{}()};
    // Recording
    std::atomic_bool recording_{false};
    bool recordingWasVisible_ = true;
    // Clicker
    std::atomic_bool clicking_{false};
    std::thread clickerThread_;
};

