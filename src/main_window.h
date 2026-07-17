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
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "action_tree.h"
#include "action_utils.h"
#include "app_settings.h"
#include "app_settings_store.h"
#include "app_theme.h"
#include "config.h"
#include "controls.h"
#include "drawing.h"
#include "hotkey_dialog.h"
#include "image_match.h"
#include "input/mouse_input_backend.h"
#include "input_timeline_scheduler.h"
#include "macro_variables.h"
#include "macro_debug_window.h"
#include "modern_edit.h"
#include "main_features.h"
#include "popup_combo.h"
#include "prompt_modal.h"
#include "crosshair_drag.h"
#include "ocr_install_dialog.h"
#include "ocr_engine.h"
#include "process_utils.h"
#include "recorder.h"
#include "recorder_timeline.h"
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
#include "coord_space.h"
#include "script_action_builder.h"
#include "utils.h"
#include "agent_dialog.h"
#include "agent_conversation_store.h"
#include "agent_ui_notify.h"
#include "ai_action_service.h"
#include "agent_ai_actions.h"
#include "ai_action_runtime.h"
#include "ui_component.h"
#include "ui_scale.h"
#include "editor_param_layout.h"
#include "find_image_ui_debug.h"
#include "taskbar_window.h"
#include "breakout_input.h"
#include "window_mode/window_mode_session.h"
#include "window_mode/window_mode_executor.h"
#include "window_mode/window_mode_log.h"
#include "window_mode/window_mode_json.h"
#include "window_mode/window_pick_dialog.h"
#include "window_mode/window_coords.h"
#include "process_utils.h"

// ── Global-hotkey low-level hook (fallback when RegisterHotKey is unavailable) ──
inline HHOOK ghHotkeyKbHook = nullptr;
inline HHOOK ghHotkeyMouseHook = nullptr;
inline HWND ghHotkeyHwnd = nullptr;
inline bool ghHotkeyPending = false;
/// 已处理本次按下：到 KEYUP 前忽略另一通道（RegisterHotKey + LL 钩子双投递）。
inline bool ghHotkeyNeedKeyUp = false;
/// 正在处理启停热键：防止 StopRecording 保存期间排队的第二通道再次启动录制。
inline bool ghHotkeyHandling = false;
inline UINT ghHotkeyVk = 0;
inline UINT ghHotkeyMods = 0;
inline bool ghHotkeyEnabled = false;
/// 回放/录制/连点进行中：热键停止时放宽修饰键判定。
inline std::atomic<bool> ghHotkeySessionBusy{false};
constexpr UINT_PTR kHotkeyLatchSyncTimerId = 0x48534B31u; // 'HSK1'
/// 仅左键：长按超过阈值后开始连点，松开停止；右键仍为单击切换
constexpr DWORD kMouseHoldHotkeyMs = 200;
inline DWORD ghHotkeyMouseDownTick = 0;
inline bool ghHotkeyMouseHoldArmed = false;  // 已按下、等待达到长按阈值
inline bool ghHotkeyMouseHoldDown = false;   // 已触发开始，等待松开停止
/// WM_GLOBAL_HOTKEY_DETECTED 的 wParam：左键按住启停命令
constexpr WPARAM kHotHoldStart = 1;
constexpr WPARAM kHotHoldStop = 2;

inline bool NeedsMouseHoldHotkey(UINT vk) {
    return vk == VK_LBUTTON;
}

inline bool CheckHotkeyModifiers(UINT required, bool requireNoExtras) {
    const bool alt   = (GetAsyncKeyState(VK_LMENU)    & 0x8000) || (GetAsyncKeyState(VK_RMENU)    & 0x8000);
    const bool ctrl  = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) || (GetAsyncKeyState(VK_RCONTROL) & 0x8000);
    const bool shift = (GetAsyncKeyState(VK_LSHIFT)   & 0x8000) || (GetAsyncKeyState(VK_RSHIFT)   & 0x8000);
    const bool win   = (GetAsyncKeyState(VK_LWIN)     & 0x8000) || (GetAsyncKeyState(VK_RWIN)     & 0x8000);
    if ((required & MOD_ALT)     && !alt)   return false;
    if ((required & MOD_CONTROL) && !ctrl)  return false;
    if ((required & MOD_SHIFT)   && !shift) return false;
    if ((required & MOD_WIN)     && !win)   return false;
    // 启动时要求没有“额外”修饰键，避免误触；停止/切换忙碌态时放宽——
    // 回放可能残留 Shift/Ctrl，或用户正按着 WASD 旁的修饰键。
    if (requireNoExtras && required == 0 && (alt || ctrl || shift || win)) return false;
    if (requireNoExtras) {
        if (!(required & MOD_ALT) && alt) return false;
        if (!(required & MOD_CONTROL) && ctrl) return false;
        if (!(required & MOD_SHIFT) && shift) return false;
        if (!(required & MOD_WIN) && win) return false;
    }
    return true;
}

/// 钩子丢 KEYUP 时，用异步键态清掉闩锁，避免启停热键永久哑火。
/// 注意：左键按住模式禁止用 GetAsyncKeyState——连点 SendInput 的 LEFTUP 会污染键态，导致误停。
inline void SyncHotkeyLatches() {
    if (!ghHotkeyEnabled || ghHotkeyVk == 0) return;
    if (NeedsMouseHoldHotkey(ghHotkeyVk)) return;
    if ((GetAsyncKeyState(static_cast<int>(ghHotkeyVk)) & 0x8000) == 0) {
        ghHotkeyNeedKeyUp = false;
        ghHotkeyPending = false;
    }
}

/// 左键长按：按下后计时，仍按住且达到阈值才开始连点（抬起仅由钩子非注入 UP 取消/停止）
inline void PollMouseHoldHotkey() {
    if (!ghHotkeyEnabled || !ghHotkeyHwnd || !ghHotkeyMouseHoldArmed || ghHotkeyMouseHoldDown) return;
    if (!NeedsMouseHoldHotkey(ghHotkeyVk)) return;
    if (GetTickCount() - ghHotkeyMouseDownTick < kMouseHoldHotkeyMs) return;
    if (ghHotkeyNeedKeyUp || ghHotkeyPending || ghHotkeyHandling) return;
    const bool busy = ghHotkeySessionBusy.load(std::memory_order_relaxed);
    if (!CheckHotkeyModifiers(ghHotkeyMods, !busy)) {
        ghHotkeyMouseHoldArmed = false;
        return;
    }
    ghHotkeyMouseHoldArmed = false;
    ghHotkeyMouseHoldDown = true;
    ghHotkeyPending = true;
    PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, kHotHoldStart, 0);
}

inline void FlushQueuedGlobalHotkeys(HWND hwnd) {
    if (!hwnd) return;
    MSG msg{};
    // 丢掉处理期间排队的第二通道启停，避免「刚停又开」。
    while (PeekMessageW(&msg, hwnd, WM_GLOBAL_HOTKEY_DETECTED, WM_GLOBAL_HOTKEY_DETECTED, PM_REMOVE)) {
    }
    while (PeekMessageW(&msg, hwnd, WM_HOTKEY, WM_HOTKEY, PM_REMOVE)) {
        if (static_cast<int>(msg.wParam) != HOTKEY_GLOBAL_ID) {
            // 脚本热键：塞回队列末尾会乱序，这里用 Post 还原
            PostMessageW(hwnd, WM_HOTKEY, msg.wParam, msg.lParam);
        }
    }
}

inline bool IsMouseVk(UINT vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON
        || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

inline LRESULT CALLBACK HotkeyKbProc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && ghHotkeyEnabled && !IsMouseVk(ghHotkeyVk)) {
        auto* ks = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        // 忽略脚本 SendInput 注入，避免误触发启停热键
        if (!(ks->flags & LLKHF_INJECTED)) {
            const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
            const bool up   = (wp == WM_KEYUP   || wp == WM_SYSKEYUP);
            if (up && ks->vkCode == ghHotkeyVk) {
                ghHotkeyPending = false;
                ghHotkeyNeedKeyUp = false;
            }
            if (down && ks->vkCode == ghHotkeyVk) {
                // 同一次物理按下：RegisterHotKey 已处理后等到抬起再响应。
                if (ghHotkeyNeedKeyUp || ghHotkeyPending || ghHotkeyHandling) {
                    return CallNextHookEx(nullptr, code, wp, lp);
                }
                // 忙碌态（回放/录制/连点）放宽修饰键，确保能停。
                const bool busy = ghHotkeySessionBusy.load(std::memory_order_relaxed);
                if (CheckHotkeyModifiers(ghHotkeyMods, !busy)) {
                    ghHotkeyPending = true;
                    PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, 0, 0);
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

inline LRESULT CALLBACK HotkeyMouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && ghHotkeyEnabled && IsMouseVk(ghHotkeyVk)) {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        if (!(ms->flags & LLMHF_INJECTED)) {
            bool down = false, up = false; UINT btnVk = 0;
            if      (wp == WM_LBUTTONDOWN) { down = true; btnVk = VK_LBUTTON; }
            else if (wp == WM_LBUTTONUP)   { up   = true; btnVk = VK_LBUTTON; }
            else if (wp == WM_RBUTTONDOWN) { down = true; btnVk = VK_RBUTTON; }
            else if (wp == WM_RBUTTONUP)   { up   = true; btnVk = VK_RBUTTON; }
            else if (wp == WM_MBUTTONDOWN) { down = true; btnVk = VK_MBUTTON; }
            else if (wp == WM_MBUTTONUP)   { up   = true; btnVk = VK_MBUTTON; }
            else if (wp == WM_XBUTTONDOWN || wp == WM_XBUTTONUP) {
                btnVk = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
                if (wp == WM_XBUTTONDOWN) down = true; else up = true;
            }
            if (up && btnVk == ghHotkeyVk) {
                if (NeedsMouseHoldHotkey(btnVk)) {
                    const bool wasArmed = ghHotkeyMouseHoldArmed;
                    const bool wasActive = ghHotkeyMouseHoldDown;
                    ghHotkeyMouseHoldArmed = false;
                    ghHotkeyMouseDownTick = 0;
                    ghHotkeyMouseHoldDown = false;
                    // 未达长按阈值的短按：取消，不启停
                    if (wasArmed && !wasActive) {
                        ghHotkeyNeedKeyUp = false;
                        ghHotkeyPending = false;
                        return CallNextHookEx(nullptr, code, wp, lp);
                    }
                    // 已触发连点：松开即停（仅信任非注入物理抬起）
                    if (wasActive) {
                        ghHotkeyNeedKeyUp = false;
                        if (ghHotkeyHandling || ghHotkeyPending) {
                            // 启动处理中：标记，等 Guard 结束再投递停止
                            ghHotkeyMouseHoldDown = false;
                            PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, kHotHoldStop, 0);
                        } else {
                            ghHotkeyPending = true;
                            PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, kHotHoldStop, 0);
                        }
                    } else {
                        ghHotkeyNeedKeyUp = false;
                        ghHotkeyPending = false;
                    }
                } else {
                    ghHotkeyPending = false;
                    ghHotkeyNeedKeyUp = false;
                }
            }
            if (down && btnVk == ghHotkeyVk) {
                if (ghHotkeyPending || ghHotkeyHandling) {
                    return CallNextHookEx(nullptr, code, wp, lp);
                }
                if (NeedsMouseHoldHotkey(btnVk)) {
                    // 左键：武装长按，达阈值后再由 PollMouseHoldHotkey 启动
                    if (ghHotkeyNeedKeyUp) {
                        return CallNextHookEx(nullptr, code, wp, lp);
                    }
                    ghHotkeyMouseDownTick = GetTickCount();
                    ghHotkeyMouseHoldArmed = true;
                    ghHotkeyMouseHoldDown = false;
                    return CallNextHookEx(nullptr, code, wp, lp);
                }
                if (ghHotkeyNeedKeyUp) {
                    return CallNextHookEx(nullptr, code, wp, lp);
                }
                const bool busy = ghHotkeySessionBusy.load(std::memory_order_relaxed);
                if (CheckHotkeyModifiers(ghHotkeyMods, !busy)) {
                    ghHotkeyPending = true;
                    PostMessageW(ghHotkeyHwnd, WM_GLOBAL_HOTKEY_DETECTED, 0, 0);
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

class MainWindow {
public:
    bool Create() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &MainWindow::WndProc;
        wc.hInstance = g_instance;
        wc.lpszClassName = L"QuickScriptToolWindow";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadAppIcon();
        wc.hIconSm = LoadAppIconSmall();
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        int x = (screenW - UiHomeWidth()) / 2;
        int y = (screenH - UiHomeHeight()) / 2;
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"鼠大侠-鼠标宏", WS_POPUP | WS_MINIMIZEBOX, x, y, UiHomeWidth(), UiHomeHeight(), nullptr, nullptr, g_instance, this);
        if (hwnd_) ApplyWindowIcons(hwnd_);
        return hwnd_ != nullptr;
    }

    void Show(int nCmdShow) {
        ShowWindow(hwnd_, nCmdShow);
        UpdateWindow(hwnd_);
        // 托盘须在窗口创建/显示之后添加；WM_CREATE 里 NIM_ADD 常失败
        EnsureTrayIcon();
    }

    LRESULT RouteEditorDropPopup(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT RouteEditorTipPopup(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT RouteClickerDropPopup(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    friend LRESULT CALLBACK EditorDropPopupWndProc(HWND, UINT, WPARAM, LPARAM);
    friend LRESULT CALLBACK EditorTipPopupWndProc(HWND, UINT, WPARAM, LPARAM);
    friend LRESULT CALLBACK ClickerDropPopupWndProc(HWND, UINT, WPARAM, LPARAM);
    enum class Page { Home, Editor };
    enum Id { kScriptName = 1001, kModeCombo, kActionCombo, kAdd, kModify, kClear, kSave, kCancel, kLoad, kBatchExit, kBatchSelectAll, kBatchDeselect, kBatchDelete, kBatchCopy, kMoveX, kMoveY, kMoveRandomX, kMoveRandomY, kMoveFromVar, kMoveVarX, kMoveVarY, kClickButton, kClickCount, kClickWait, kClickRandom, kWaitDuration, kWaitRandom, kRemark, kListRemarkEdit, kClose, kKeyCapture, kClickLWin, kClickRWin, kClickLCtrl, kClickRCtrl, kClickLAlt, kClickRAlt, kClickLShift, kClickRShift, kKeyLWin, kKeyRWin, kKeyLCtrl, kKeyRCtrl, kKeyLAlt, kKeyRAlt, kKeyLShift, kKeyRShift, kCrosshair, kLoopCount, kLoopFromVar, kLoopVarExpr, kLoopVarName, kDefineBlockName, kRunBlockCombo, kKeyPressCapture, kMousePressButton, kMousePressLWin, kMousePressRWin, kMousePressLCtrl, kMousePressRCtrl, kMousePressLAlt, kMousePressRAlt, kMousePressLShift, kMousePressRShift, kKeyPressLWin, kKeyPressRWin, kKeyPressLCtrl, kKeyPressRCtrl, kKeyPressLAlt, kKeyPressRAlt, kKeyPressLShift, kKeyPressRShift, kHotkeyShortcutCombo, kHotkeyShortcutCount, kHotkeyShortcutWait, kHotkeyShortcutRandom, kQuickInputText, kQuickInputVarCombo, kQuickInputInsert, kQuickInputCharInterval, kQuickInputCount, kQuickInputWait, kQuickInputRandom, kRunMacroCombo, kMousePlaybackCombo, kMousePlaybackCount, kMousePlaybackWait, kMousePlaybackRandom, kScrollVertical, kScrollHorizontal, kScrollSteps, kScrollDirection, kScrollCount, kScrollWait, kScrollRandom, kFindFullScreen, kFindSelectRegion, kFindX1, kFindY1, kFindX2, kFindY2, kFindTest, kFindScreenshot, kFindLocalImage, kFindClearImage, kFindImagePreview, kFindMatchThreshold, kFindScaleMin, kFindScaleMax, kFindFollowUp, kFindOffsetX, kFindOffsetY, kFindSelectOffset, kFindUntilFound, kFindMatchVar, kOcrFullScreen, kOcrSelectRegion, kOcrX1, kOcrY1, kOcrX2, kOcrY2, kOcrResultMode, kOcrSearchText, kOcrSearchVarCombo, kOcrSearchVarInsert, kOcrFollowUp, kOcrOffsetX, kOcrOffsetY, kOcrSelectOffset, kOcrUntilFound, kOcrResultVar, kOcrTest, kOcrInstallDep, kOcrRegionByImage, kOcrFindSelectRegion, kOcrFindScreenshot, kOcrFindLocalImage, kOcrFindClearImage, kOcrFindImagePreview, kOcrFindMatchThreshold, kOcrFindScaleMin, kOcrFindScaleMax, kOcrDigitsOnly, kIfVarCombo, kIfOperator, kIfValue, kIfConnector, kIfAddCondition, kIfConditionList, kRunProgramCombo, kRunProgramPath, kRunProgramBrowse, kRunProgramCrosshair, kRunProgramArgs, kCloseProgramPath, kCloseProgramBrowse, kCloseProgramCrosshair, kCloseProgramMatchFileName, kOpenWebpageUrl, kOpenFilePath, kOpenFileBrowse, kTimerVarName, kAiPrompt, kAiInsertVar, kAiVarCombo, kAiModel, kAiContextMode, kAiOutputVar, kAiOutputType, kAiTimeout, kAiFallback, kAiImageScale, kAiRegionByImage, kAiRegionByImage2, kAiFindSelectRegion, kAiFindMatchThreshold, kAiFindScaleMin, kAiFindScaleMax, kAiTargetPreview, kAiTargetScreenshot, kAiTargetLocal, kAiTargetClear, kAiFullScreen, kAiSelectRegion, kAiSearchRegion, kAiSearchX1, kAiSearchY1, kAiSearchX2, kAiSearchY2, kAiMaxSteps, kAiWithImage, kAiConfirm, kAiMaxStepsHint, kCursorPosVarName, kGotoStepExpr, kMoveRelX, kMoveRelY, kMoveRelRandomX, kMoveRelRandomY, kBreakoutTime = 5099, kWmSelectMethod = 5101, kWmSpecifyWindowBtn, kWmTargetPath, kWmTargetBrowse, kWmTargetCrosshair };
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
        // 顶层下拉弹层不会随 owner 自动位移，拖动/移动时必须按锚点重算屏幕坐标。
        case WM_MOVE:
            SyncOwnedDropPopups();
            promptModal_.OnOwnerResize();
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_WINDOWPOSCHANGED: {
            const auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
            if (pos && !(pos->flags & SWP_NOMOVE)) {
                TrySyncUiScaleIfDisplayChanged();
            }
            if (page_ == Page::Editor && IsWindowVisible(hwnd_) && pos
                && !(pos->flags & SWP_NOMOVE)) {
                ApplyParamLayerMasks();
            }
            SyncOwnedDropPopups();
            promptModal_.OnOwnerResize();
            return DefWindowProcW(hwnd_, msg, wp, lp);
        }
        case WM_SHOWWINDOW:
            if (!wp) {
                CloseEditorPopup();
                CloseClickerDropPopup();
                CancelQuickInputTip();
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_DPICHANGED: OnDpiChanged(wp, lp); return 0;
        case WM_DISPLAYCHANGE: RequestUiScaleSync(); return 0;
        case WM_SETTINGCHANGE:
            if (lp) {
                const wchar_t* section = reinterpret_cast<LPCWSTR>(lp);
                if (lstrcmpiW(section, L"Display") == 0
                    || lstrcmpiW(section, L"WindowMetrics") == 0
                    || lstrcmpiW(section, L"Desktop") == 0) {
                    RequestUiScaleSync();
                    return 0;
                }
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) {
                CloseEditorPopup();
                CloseClickerDropPopup();
                CancelQuickInputTip();
            } else {
                if (page_ == Page::Home) UpdateClickerLayout();
                SyncOwnedDropPopups();
            }
            promptModal_.OnOwnerResize();
            if (wp != SIZE_MINIMIZED) InvalidateRect(hwnd_, nullptr, TRUE);
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE
                && !trayMenuOpen_
                && !breakoutTaskbarTransition_
                && breakoutPaused_.load(std::memory_order_relaxed)
                && !breakoutUiVisibleOnScreen_
                && breakoutTaskbarShown_) {
                PostMessageW(hwnd_, WM_APP_RESTORE_INSTANCE, 0, 0);
            } else if (LOWORD(wp) == WA_INACTIVE) {
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
        case WM_ACTIVATEAPP:
            if (wp && !trayMenuOpen_
                && !breakoutTaskbarTransition_
                && breakoutPaused_.load(std::memory_order_relaxed)
                && !breakoutUiVisibleOnScreen_
                && breakoutTaskbarShown_
                && (GetForegroundWindow() == hwnd_ || GetActiveWindow() == hwnd_)) {
                RestoreBreakoutWindowToScreen();
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_SYSCOMMAND:
            if (!trayMenuOpen_
                && !breakoutTaskbarTransition_
                && breakoutPaused_.load(std::memory_order_relaxed)) {
                const UINT cmd = wp & 0xFFF0;
                if (cmd == SC_RESTORE) {
                    RestoreBreakoutWindowToScreen();
                    return 0;
                }
                if (cmd == SC_MINIMIZE && breakoutUiVisibleOnScreen_
                    && breakoutTaskbarShown_) {
                    MinimizeBreakoutWindowForUser();
                    return 0;
                }
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_SETFOCUS:
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_TIMER:
            if (wp == kHotkeyLatchSyncTimerId) {
                SyncHotkeyLatches();
                PollMouseHoldHotkey();
                return 0;
            }
            if (wp == kDisplaySyncTimerId) {
                SyncUiScaleLayout();
                if (displaySyncPass_ < 4) {
                    ++displaySyncPass_;
                    SetTimer(hwnd_, kDisplaySyncTimerId, 200, nullptr);
                } else {
                    displaySyncPass_ = 0;
                    KillTimer(hwnd_, kDisplaySyncTimerId);
                }
                return 0;
            }
            if (wp == kHoverTimerId) { UpdateHoverFromCursor(); return 0; }
            if (wp == kQuickInputTipTimerId) { OnQuickInputTipTimer(); return 0; }
            if (wp == kScheduledTaskTimerId) { scheduledTasks_.Tick(); return 0; }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_HOTKEY: OnHotkey(static_cast<int>(wp)); return 0;
        case WM_GLOBAL_HOTKEY_DETECTED: OnHotkey(HOTKEY_GLOBAL_ID, static_cast<int>(wp)); return 0;
        case WM_GETICON:
            if (breakoutPaused_.load(std::memory_order_relaxed)) {
                const UINT which = static_cast<UINT>(wp);
                if (which == ICON_BIG) {
                    return reinterpret_cast<LRESULT>(LoadBreakoutPauseIcon());
                }
                if (which == ICON_SMALL) {
                    return reinterpret_cast<LRESULT>(LoadBreakoutPauseIconSmall());
                }
                if (which == ICON_SMALL2) {
                    return reinterpret_cast<LRESULT>(LoadBreakoutPauseIcon());
                }
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_RUN_DONE: OnRunDone(); return 0;
        case WM_APP_BREAKOUT_UI: UpdateBreakoutPauseIcons(); return 0;
        case WM_APP_RESTORE_INSTANCE: RestoreMainWindowForUser(); return 0;
        case WM_APP_EDITOR_FINISH_OPEN:
            FinishDeferredEditorOpen(static_cast<int>(wp));
            return 0;
        case WM_APP_EDITOR_PARSE_MORE:
            ContinueEditorProgressiveParse(static_cast<int>(wp));
            return 0;
        case WM_APP_HOME_REFRESH_LISTS:
            if (page_ == Page::Home) {
                // wp!=0：从编辑页返回——主页已揭开，再做表单复位/图片清理（勿挡首帧）
                if (wp) {
                    ResetActionFormSession(true);
                    CleanupNewImages();
                }
                LoadScripts();
                LoadRecordings();
                if (selectedScript_ >= static_cast<int>(scripts_.size())) selectedScript_ = -1;
                if (selectedRecording_ >= static_cast<int>(recordings_.size())) selectedRecording_ = -1;
                ClampHomeScroll();
                ClampRecordingScroll();
                RegisterAllHotkeys();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_FIND_TEST_DONE: OnFindTestDone(static_cast<int>(wp), static_cast<int>(lp)); return 0;
        case WM_OPEN_AGENT_DIALOG: OpenAgentDialog(); return 0;
        case WM_AGENT_SCRIPT_LIBRARY_CHANGED: RefreshScriptLibraryUi(); return 0;
        case WM_APP_UI_SCALE_SYNC:
            SyncUiScaleLayout();
            if (lp) {
                displaySyncPass_ = 1;
                SetTimer(hwnd_, kDisplaySyncTimerId, 200, nullptr);
            }
            return 0;
        case WM_EDITOR_PARAM_CHROME:
            if (page_ == Page::Editor) RepaintParamPanelChrome();
            return 0;
        case WM_OCR_SUBPANEL_REFRESH:
            ocrSubPanelRefreshPosted_ = false;
            if (page_ == Page::Editor && popupAction_.sel == 18)
                RefreshOcrSubPanel();
            UnlockParamViewportRedraw();
            return 0;
        case WM_APP_PROMPT:
            if (!promptPendingMessage_.empty()) {
                CloseEditorPopup();
                CloseClickerDropPopup();
                std::wstring promptMsg = std::move(promptPendingMessage_);
                promptPendingMessage_.clear();
                if (promptModal_.visible()) promptModal_.Close();
                promptModal_.OnOwnerResize();
                promptModal_.ShowInfo(promptMsg);
            }
            return 0;
        case WM_TRAY: return OnTrayMessage(lp);
        case WM_CTLCOLORSTATIC:
            return OnCtlColorStatic(reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp));
        case WM_CTLCOLOREDIT: return OnEditColor(reinterpret_cast<HDC>(wp));
        case WM_CLOSE:
            // 关闭到托盘（设置项）：仅隐藏，进程继续跑。真正退出靠托盘「退出」。
            // 注意：此处不要 Uninstall*Hooks / DestroyWindow，那些路径会卡成幽灵进程。
            if (page_ == Page::Home && appSettings_.other.closeToTray && !clicking_ && !recording_) {
                if (running_) {
                    try { SaveHomeState(); } catch (...) {}
                    CloseWindowDuringRun();
                    return 0;
                }
                try { SaveHomeState(); } catch (...) {}
                HideToTray();
                return 0;
            }
            QuitApplication();
            return 0;
        case WM_DESTROY:
            // 正常不应走到这里（QuitApplication 直接 TerminateProcess）。
            stopFlag_ = true;
            clicking_ = false;
            running_ = false;
            RemoveTrayIcon();
            PostQuitMessage(0);
            TerminateProcess(GetCurrentProcess(), 0);
        case WM_APP_QUIT_APP:
            QuitApplication();
            return 0;
        case WM_APP_ENSURE_TRAY:
            EnsureTrayIcon();
            return 0;
        case WM_QUERYENDSESSION: if (recording_) StopRecording(); return TRUE;
        case WM_ENDSESSION: if (recording_) StopRecordingCleanup(); return 0;
        default:
            if (wmTaskbarCreated_ && msg == wmTaskbarCreated_) {
                // Explorer 重启后托盘图标会丢，强制重新 NIM_ADD
                trayActive_ = false;
                EnsureTrayIcon();
                return 0;
            }
            return DefWindowProcW(hwnd_, msg, wp, lp);
        }
    }

    // ── Initialization & cleanup ────────────────────────────────────
    void CreateUiFonts() {
        font_ = CreateFontW(UiFontHeight(23), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        editorFont_ = CreateFontW(UiFontHeight(26), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        bigFont_ = CreateFontW(UiFontHeight(39), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        titleFont_ = CreateFontW(UiFontHeight(22), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        hotFont_ = CreateFontW(UiFontHeight(25), 0, 0, 0, FW_BOLD, FALSE, TRUE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        closeFont_ = CreateFontW(UiFontHeight(32), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        homeFont_ = CreateFontW(UiFontHeight(25), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        homeTabFont_ = CreateFontW(UiFontHeight(28), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
    }

    void DestroyUiFonts() {
        auto del = [](HFONT& f) {
            if (f) {
                DeleteObject(f);
                f = nullptr;
            }
        };
        del(font_);
        del(editorFont_);
        del(bigFont_);
        del(titleFont_);
        del(hotFont_);
        del(closeFont_);
        del(homeFont_);
        del(homeTabFont_);
    }

    void RecreateUiFonts() {
        DestroyUiFonts();
        CreateUiFonts();
        editorFontsApplied_ = false;
        editorFontsAppliedTo_ = nullptr;
        editorScaleAppliedPct_ = -1;
        ApplyFont(hwnd_, font_);
    }

    void ApplyDpiLayout() {
        UpdateClickerLayout();
        if (page_ == Page::Editor) {
            ApplyEditorControlScale(true);
        }
        if (!editorControls_.empty()) {
            editorFontsApplied_ = false;
            ApplyEditorFonts();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void RefreshOpenAgentDialogs() {
        for (auto& d : agentDialogs_) {
            if (d && d->IsAlive()) d->ApplyDpiLayout();
        }
    }

    void RecreateThemeBrushes() {
        if (lineGreenBrush_) { DeleteObject(lineGreenBrush_); lineGreenBrush_ = nullptr; }
        lineGreenBrush_ = CreateSolidBrush(kLineGreen);
    }

    /// 主题 id 已写入 settings / CurrentTheme 后，立刻刷新主窗与已打开的附属窗
    void ApplyThemeAndRefreshUi() {
        quickscript::ApplyThemeFromSettings(appSettings_);
        RecreateThemeBrushes();
        RedrawWindow(hwnd_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
        for (auto& d : agentDialogs_) {
            if (!d || !d->IsAlive()) continue;
            RedrawWindow(d->Hwnd(), nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
        if (HWND opt = RecordingOptimizeDialog::ActiveHwnd()) {
            RedrawWindow(opt, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
        if (macroDebugWindow_.IsCreated() && IsWindow(macroDebugWindow_.Hwnd())) {
            RedrawWindow(macroDebugWindow_.Hwnd(), nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
        }
    }

    void SyncUiScaleLayout() {
        const int oldPct = UiScalePercent();
        UiScaleInitFromHwnd(hwnd_);
        const bool scaleChanged = (UiScalePercent() != oldPct);
        lastUiScalePercent_ = UiScalePercent();
        lastUiMonitor_ = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monInfo{};
        monInfo.cbSize = sizeof(monInfo);
        if (lastUiMonitor_ && GetMonitorInfoW(lastUiMonitor_, &monInfo)) {
            lastUiScreenW_ = monInfo.rcMonitor.right - monInfo.rcMonitor.left;
            lastUiScreenH_ = monInfo.rcMonitor.bottom - monInfo.rcMonitor.top;
        }
        RecreateUiFonts();
        ApplyDpiLayout();
        const int targetW = page_ == Page::Home ? UiHomeWidth() : UiEditorWidth();
        const int targetH = page_ == Page::Home ? UiHomeHeight() : UiEditorHeight();
        UiResizeWindowClient(hwnd_, targetW, targetH, scaleChanged);
        RefreshOpenAgentDialogs();
        NotifyActiveSettingsDialogRelayout();
        RedrawWindow(hwnd_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }

    void RequestUiScaleSync() {
        PostMessageW(hwnd_, WM_APP_UI_SCALE_SYNC, 0, 1);
    }

    void TrySyncUiScaleIfDisplayChanged() {
        const int oldPct = lastUiScalePercent_;
        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monInfo{};
        monInfo.cbSize = sizeof(monInfo);
        if (!mon || !GetMonitorInfoW(mon, &monInfo)) return;
        const int sw = monInfo.rcMonitor.right - monInfo.rcMonitor.left;
        const int sh = monInfo.rcMonitor.bottom - monInfo.rcMonitor.top;
        UiScaleInitFromHwnd(hwnd_);
        if (mon != lastUiMonitor_ || sw != lastUiScreenW_ || sh != lastUiScreenH_
            || UiScalePercent() != oldPct) {
            SyncUiScaleLayout();
        }
    }

    void OnDpiChanged(WPARAM /*wp*/, LPARAM /*lp*/) {
        SyncUiScaleLayout();
    }

    void Init() {
        InitCommonControls();
        EnsureScriptsDir();
        UiScaleInitFromHwnd(hwnd_);
        CreateUiFonts();
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
        LoadAgentConversations();
        LoadAppSettings(appSettings_);
        quickscript::ApplyThemeFromSettings(appSettings_);
        ApplyDebugWindowSetting();
        CleanOrphanImages();  // 启动时清理孤立图片
        CreateEditorDropPopup();
        CreateClickerDropPopup();
        CreateEditorTipPopup();
        page_ = Page::Home;
        ShowEditorControls(false);
        RestoreHomeState();   // 恢复上次退出时的界面状态
        InvalidateRect(hwnd_, nullptr, TRUE);
        RegisterAllHotkeys();
        InstallGlobalHotkeyHooks();
        scheduledTasks_.Reload();
        scheduledTasks_.SetRunCallback([this](const std::wstring& path) { RunActionsFromPath(path); });
        SetTimer(hwnd_, kScheduledTaskTimerId, 1000, nullptr);
        // 延迟到 Show()/WM_APP_ENSURE_TRAY：WM_CREATE 阶段 NIM_ADD 不可靠
        PostMessageW(hwnd_, WM_APP_ENSURE_TRAY, 0, 0);
        SetAgentUiNotifyHwnd(hwnd_);
        promptModal_.Bind(hwnd_, font_, [this] {
            if (page_ == Page::Editor) ApplyParamLayerMasks();
        });
        outerShadow_.Attach(hwnd_);
        SyncUiScaleLayout();
        if (page_ == Page::Home) ShowEditorControls(false);
    }

    // ── Editor control creation ────────────────────────────────────
    void CreateEditorControls() {
        labelMacro_ = MakeLabel(hwnd_, L"宏名称:", -1, kEditorMacroNameLabelX, kEditorMacroHeaderRowY, kEditorMacroNameLabelW, kEditorMacroHeaderRowH);
        editorControls_.push_back(labelMacro_);
        name_ = MakeEdit(hwnd_, L"", kScriptName, kEditorMacroNameEditX, kEditorMacroHeaderRowY, kEditorMacroNameEditW, kEditorMacroHeaderRowH);
        SendMessageW(name_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
        editorControls_.push_back(name_);
        labelBreakoutTime_ = MakeLabel(hwnd_, L"脱离时间:", -1, kEditorBreakoutLabelX, kEditorBreakoutRowY, kEditorBreakoutLabelW, kEditorMacroHeaderRowH);
        editorControls_.push_back(labelBreakoutTime_);
        breakoutTimeEdit_ = MakeEdit(hwnd_, L"0", kBreakoutTime, kEditorBreakoutEditX, kEditorBreakoutRowY, kEditorBreakoutEditW, kEditorMacroHeaderRowH);
        SendMessageW(breakoutTimeEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
        editorControls_.push_back(breakoutTimeEdit_);
        ShowWindow(labelMacro_, SW_HIDE);
        ShowWindow(name_, SW_HIDE);
        ShowWindow(labelBreakoutTime_, SW_HIDE);
        ShowWindow(breakoutTimeEdit_, SW_HIDE);
        mode_ = MakeLabel(hwnd_, L"默认模式", kModeCombo,
            kEditorComboRight - kEditorModeComboW, kEditorMacroHeaderRowY, kEditorModeComboW, kEditorModeComboH);
        editorControls_.push_back(mode_);
        popupMode_.items = {L"默认模式", L"窗口模式", L"后台窗口模式"}; popupMode_.sel = 0;
        popupWmSelectMethod_.items = {
            L"启动时选择窗口",
            L"启动时获取鼠标位置的窗口",
            L"使用宏编辑时获取到的窗口类名",
            L"不选择窗口"
        };
        popupWmSelectMethod_.sel = 0;
        wmSelectMethod_ = MakeLabel(hwnd_, L"启动时选择窗口", kWmSelectMethod,
            560, kEditorMacroHeaderRowY, kEditorWmSelectMethodComboW, kEditorModeComboH);
        editorControls_.push_back(wmSelectMethod_);
        wmSpecifyWindowBtn_ = MakeGreenButton(hwnd_, L"指定窗口类", kWmSpecifyWindowBtn, 740,
            kEditorMacroHeaderRowY, kEditorWmSpecifyBtnW, kEditorMacroHeaderRowH);
        editorControls_.push_back(wmSpecifyWindowBtn_);
        ShowWindow(wmSelectMethod_, SW_HIDE);
        ShowWindow(wmSpecifyWindowBtn_, SW_HIDE);
        wmTargetPathEdit_ = MakeEdit(hwnd_, L"", kWmTargetPath, 83, 84, 400, kEditorMacroHeaderRowH);
        SendMessageW(wmTargetPathEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
        editorControls_.push_back(wmTargetPathEdit_);
        wmTargetBrowseBtn_ = MakeGrayButton(hwnd_, L"浏览", kWmTargetBrowse, 0, 84, kEditorWmTargetBrowseW, kEditorMacroHeaderRowH);
        editorControls_.push_back(wmTargetBrowseBtn_);
        wmTargetCrosshairBtn_ = MakeGreenButton(hwnd_, L"准星找程序", kWmTargetCrosshair, 0, 84, kEditorWmTargetCrosshairW, kEditorMacroHeaderRowH);
        editorControls_.push_back(wmTargetCrosshairBtn_);
        ShowWindow(wmTargetPathEdit_, SW_HIDE);
        ShowWindow(wmTargetBrowseBtn_, SW_HIDE);
        ShowWindow(wmTargetCrosshairBtn_, SW_HIDE);
        labelList_ = MakeLabel(hwnd_, L"动作列表", -1, kEditorListLabelX, kEditorToolbarLabelY, 80, kEditorMacroHeaderRowH); editorControls_.push_back(labelList_);
        labelBatchCount_ = MakeLabel(hwnd_, L"已选中:0个", -1, 95, kEditorToolbarLabelY, 120, kEditorMacroHeaderRowH); editorControls_.push_back(labelBatchCount_);
        ShowWindow(labelBatchCount_, SW_HIDE);
        loadBtn_ = MakeGreenButton(hwnd_, L"批量编辑", kLoad, 546, kEditorToolbarBtnY, 105, kEditorToolbarBtnH); editorControls_.push_back(loadBtn_);
        clearBtn_ = MakeGreenButton(hwnd_, L"清空列表", kClear, 664, kEditorToolbarBtnY, 105, kEditorToolbarBtnH); editorControls_.push_back(clearBtn_);
        batchExitBtn_ = MakeGreenButton(hwnd_, L"退出批量编辑", kBatchExit, 258, kEditorToolbarBtnY, 118, kEditorToolbarBtnH); editorControls_.push_back(batchExitBtn_);
        batchSelectAllBtn_ = MakeGreenButton(hwnd_, L"全选", kBatchSelectAll, 390, kEditorToolbarBtnY, 68, kEditorToolbarBtnH); editorControls_.push_back(batchSelectAllBtn_);
        batchDeselectBtn_ = MakeGreenButton(hwnd_, L"取消选择", kBatchDeselect, 464, kEditorToolbarBtnY, 88, kEditorToolbarBtnH); editorControls_.push_back(batchDeselectBtn_);
        batchDeleteBtn_ = MakeGreenButton(hwnd_, L"删除所选项", kBatchDelete, 558, kEditorToolbarBtnY, 105, kEditorToolbarBtnH); editorControls_.push_back(batchDeleteBtn_);
        batchCopyBtn_ = MakeGreenButton(hwnd_, L"复制所选项", kBatchCopy, 667, kEditorToolbarBtnY, 105, kEditorToolbarBtnH); editorControls_.push_back(batchCopyBtn_);
        ShowWindow(batchExitBtn_, SW_HIDE);
        ShowWindow(batchSelectAllBtn_, SW_HIDE);
        ShowWindow(batchDeselectBtn_, SW_HIDE);
        ShowWindow(batchDeleteBtn_, SW_HIDE);
        ShowWindow(batchCopyBtn_, SW_HIDE);
        HWND labelNo = MakeLabel(hwnd_, L"序号", -1, 32, kEditorListColumnHeaderY, 60, kEditorMacroHeaderRowH); editorControls_.push_back(labelNo);
        HWND labelAction = MakeLabel(hwnd_, L"动作", -1, 94, kEditorListColumnHeaderY, 80, kEditorMacroHeaderRowH); editorControls_.push_back(labelAction);
        HWND labelRemark = MakeLabel(hwnd_, L"备注", -1, 438, kEditorListColumnHeaderY, 80, kEditorMacroHeaderRowH); editorControls_.push_back(labelRemark);
        HWND labelOp = MakeLabel(hwnd_, L"操作", -1, 631, kEditorListColumnHeaderY, 80, kEditorMacroHeaderRowH); editorControls_.push_back(labelOp);
        actionCombo_ = MakeLabel(hwnd_, L"移动鼠标到", kActionCombo,
            kEditorComboRight - kEditorActionComboW, kEditorActionComboY, kEditorActionComboW, kEditorActionComboH);
        editorControls_.push_back(actionCombo_);
        popupAction_.items = {
            L"移动鼠标到", L"等待", L"鼠标点击", L"鼠标回放", L"运行鼠标宏",
            L"鼠标按下", L"鼠标松开", L"滚动滚轮", L"按键点击", L"键盘按下", L"键盘松开",
            L"快捷按键", L"快捷输入", L"循环", L"跳出循环", L"定义宏指令块", L"运行宏指令块",
            L"找图(返回最匹配的)", L"文字识别", L"条件-如果", L"条件-否则",
            L"锁定截屏", L"解锁截屏", L"结束宏运行", L"运行程序", L"关闭程序",
            L"打开网页", L"打开文件", L"计时器记录时间",
            L"AI文字分析", L"AI图片分析", L"AI动作执行", L"获取当前光标位置", L"跳转",
            L"相对移动鼠标"
        };
        popupAction_.sel = 0;
        // 勿加 WS_CLIPCHILDREN / WS_EX_COMPOSITED：父 DC 需画输入框边框与下拉外观；
        // CLIPCHILDREN 会裁掉边线，COMPOSITED 易导致自绘勾选框走错 DRAWITEM 成绿按钮。
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
            InvalidateParamControlInViewport(h);
            const RECT vp = ParamViewportRect();
            MoveWindow(h, x - vp.left, y - vp.top, w, hgt, repaint);
            if (IsGrayButton(h)) {
                RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_FRAME);
            }
        } else {
            MoveWindow(h, x, y, w, hgt, repaint);
        }
    }

    void SetParamPosAware(HWND h, int x, int y, int w, int hgt, UINT flags) const {
        if (!h) return;
        if (paramViewport_ && GetParent(h) == paramViewport_) {
            InvalidateParamControlInViewport(h);
            const RECT vp = ParamViewportRect();
            SetWindowPos(h, nullptr, x - vp.left, y - vp.top, w, hgt, flags);
        } else {
            SetWindowPos(h, nullptr, x, y, w, hgt, flags);
        }
    }

    void AttachToParamViewport(HWND h) const {
        if (!h || !paramViewport_ || h == paramViewport_) return;
        RECT rc = WindowClientRect(h);
        if (GetParent(h) != paramViewport_) {
            SetParent(h, paramViewport_);
            const RECT vp = ParamViewportRect();
            MoveWindow(h, rc.left - vp.left, rc.top - vp.top, rc.right - rc.left, rc.bottom - rc.top, FALSE);
        }
        SetWindowSubclass(h, EditorChildSubclassProc, 1, reinterpret_cast<DWORD_PTR>(const_cast<MainWindow*>(this)));
        auto* self = const_cast<MainWindow*>(this);
        if (IsMarkedParamCheckbox(h)) {
            LONG style = GetWindowLongW(h, GWL_STYLE);
            if ((style & BS_TYPEMASK) != BS_OWNERDRAW) {
                SetWindowLongW(h, GWL_STYLE, (style & ~BS_TYPEMASK) | BS_OWNERDRAW | BS_AUTOCHECKBOX);
            }
        }
        if (self->IsGrayButton(h)) {
            LONG style = GetWindowLongW(h, GWL_STYLE);
            if (style & WS_TABSTOP) {
                SetWindowLongW(h, GWL_STYLE, style & ~WS_TABSTOP);
            }
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
                if (p.type == UIComponentType::CheckBox) MarkParamCheckbox(p.hwnd);
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

    void RestoreEditorControlLayout(HWND hwnd) {
        if (!hwnd) return;
        const RECT scaled = ScaledEditorLayoutRect(hwnd);
        MoveParamAware(hwnd, scaled.left, scaled.top, scaled.right - scaled.left, scaled.bottom - scaled.top, FALSE);
        SetWindowRgn(hwnd, nullptr, TRUE);
    }

    void RestoreEditorGroupLayout(const std::vector<HWND>& group) {
        std::unordered_set<HWND> groupSet(group.begin(), group.end());
        for (const auto& item : editorLayouts_) {
            if (!groupSet.count(item.hwnd)) continue;
            RestoreEditorControlLayout(item.hwnd);
        }
    }

    // 显示/隐藏指定 action index 的布局
    void ShowParamLayout(int idx, bool visible) {
        auto it = paramLayoutResults_.find(idx);
        if (it == paramLayoutResults_.end()) return;
        if (!visible) {
            for (const auto& p : it->second.placements) {
                if (p.hwnd) ParkParamControl(p.hwnd);
            }
            return;
        }
        for (const auto& p : it->second.placements) {
            if (!p.hwnd) continue;
            if (!UsesRuntimeParamLayout(p.hwnd)) RestoreEditorControlLayout(p.hwnd);
            ShowWindow(p.hwnd, SW_SHOW);
            SetWindowRgn(p.hwnd, nullptr, TRUE);
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
            moveHintLabel_ = FindLayoutTextControl(r, L"移动到(左上角为0,0)");
            moveXLabel_ = FindLayoutTextControl(r, L"X:", 0);
            moveYLabel_ = FindLayoutTextControl(r, L"Y:", 0);
            moveRandomXLabel_ = FindLayoutTextControl(r, L"±随机:", 0);
            moveRandomYLabel_ = FindLayoutTextControl(r, L"±随机:", 1);
            moveVarXLabel_ = FindLayoutTextControl(r, L"X:", 1);
            moveVarYLabel_ = FindLayoutTextControl(r, L"Y:", 1);
            moveHintFooter_ = FindLayoutTextControl(
                r, L"*提示:可使用来自找图、找色，获取颜色，文字识别保存到变量中的值");
            moveX_ = r.HwndForId(EID_MoveX); moveY_ = r.HwndForId(EID_MoveY);
            moveRandomX_ = r.HwndForId(EID_MoveRandomX); moveRandomY_ = r.HwndForId(EID_MoveRandomY);
            crosshairBtn_ = r.HwndForId(EID_Crosshair); moveFromVar_ = r.HwndForId(EID_MoveFromVar);
            moveVarX_ = r.HwndForId(EID_MoveVarX); moveVarY_ = r.HwndForId(EID_MoveVarY);
        }

        // ── 34. 相对移动鼠标 ──
        {
            auto r = BuildAndStoreLayout(MoveMouseRelative(), moveRelControls_, 34);
            moveRelX_ = r.HwndForId(EID_MoveRelX); moveRelY_ = r.HwndForId(EID_MoveRelY);
            moveRelRandomX_ = r.HwndForId(EID_MoveRelRandomX);
            moveRelRandomY_ = r.HwndForId(EID_MoveRelRandomY);
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
            findSelectOffsetBtn_ = r2.HwndForId(EID_FindSelectOffset);

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
            findImageHeaderLabel_ = findLabel(r, L"要查找的图");
            findX1Label_ = findLabel(r, L"X1");
            findY1Label_ = findLabel(r, L"Y1");
            findX2Label_ = findLabel(r, L"X2");
            findY2Label_ = findLabel(r, L"Y2");
            findTimeLabel_ = findLabel(r2, L"时间");
            findTimeEdit_ = r2.HwndForId(EID_FindTime);
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

        // ── 33. 跳转 ──
        {
            auto r = BuildAndStoreLayout(Goto(), gotoControls_, 33);
            gotoStepEdit_ = r.HwndForId(EID_GotoStepExpr);
        }

        // ── AI 公共部分 ──
        popupAiModel_.items = {}; popupAiModel_.sel = -1;
        popupAiContextMode_.items = {L"无上下文", L"宏上下文", L"循环上下文", L"指令块上下文"}; popupAiContextMode_.sel = 1;
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
        if (editorFontsApplied_ && editorFontsAppliedTo_ == editorFont_) return;
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
            // FALSE：避免每个控件立刻重绘，打开编辑页时数百次同步重绘极慢
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(editorFont_), FALSE);
        }
        editorFontsApplied_ = true;
        editorFontsAppliedTo_ = editorFont_;
    }

    void ShowGroup(const std::vector<HWND>& controls, bool visible) { for (HWND h : controls) ShowWindow(h, visible ? SW_SHOW : SW_HIDE); }

    bool IsParamViewportDescendant(HWND h) const {
        if (!h || !paramViewport_) return false;
        if (h == paramViewport_) return true;
        return IsParamViewportChild(h);
    }

    void ParkAllParamPanelControls() const {
        if (!paramViewport_) return;
        for (HWND child = GetWindow(paramViewport_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            ParkParamControl(child);
        }
    }

    void HideEditorMacroHeaderControls() const {
        for (HWND h : {labelMacro_, name_, labelBreakoutTime_, breakoutTimeEdit_,
                wmSelectMethod_, wmSpecifyWindowBtn_, wmTargetPathEdit_,
                wmTargetBrowseBtn_, wmTargetCrosshairBtn_}) {
            if (h) ShowWindow(h, SW_HIDE);
        }
    }

    bool IsBatchToolbarHwnd(HWND hwnd) const {
        return hwnd == loadBtn_ || hwnd == clearBtn_
            || hwnd == batchExitBtn_ || hwnd == batchSelectAllBtn_
            || hwnd == batchDeselectBtn_ || hwnd == batchDeleteBtn_
            || hwnd == batchCopyBtn_ || hwnd == labelBatchCount_;
    }

    // includeParamViewport：打开编辑页首帧可传 false，避免参数区在 ResetActionFormSession 前闪出旧布局
    void ShowEditorControls(bool visible, bool includeParamViewport = true) {
        if (!visible) {
            CloseEditorPopup();
            // 离开编辑页：整组隐藏即可，勿逐个 Park（长脚本参数控件极多，MoveWindow 会卡）
            if (paramViewport_) ShowWindow(paramViewport_, SW_HIDE);
            HideEditorMacroHeaderControls();
            for (HWND h : editorControls_) {
                if (!h || IsParamViewportDescendant(h)) continue;
                ShowWindow(h, SW_HIDE);
            }
            ApplyParamLayerMasks();
            return;
        }
        // 页眉/批量工具栏/备注编辑/自绘下拉锚点由专用函数按状态显隐，不可一律 SW_SHOW。
        // 否则 mode_/actionCombo_ 等 STATIC 会叠在父窗口自绘之上，出现透明/乱字。
        // 添加/修改/备注：须等 ApplyEditorFooterLayout，否则会在设计坐标上闪一下。
        for (HWND h : editorControls_) {
            if (!h || IsParamViewportDescendant(h)) continue;
            if (IsEditorMacroHeaderHwnd(h)) continue;
            if (IsBatchToolbarHwnd(h)) continue;
            if (IsEditorComboAnchorHwnd(h)) continue;
            if (IsFooterControl(h)) continue;
            if (h == listRemarkEdit_ || IsParamMaskControl(h)) continue;
            ShowWindow(h, SW_SHOW);
        }
        for (HWND h : {remarkLabel_, remark_, addBtn_, modifyBtn_}) {
            if (h) ShowWindow(h, SW_HIDE);
        }
        if (includeParamViewport && paramViewport_) ShowWindow(paramViewport_, SW_SHOW);
        else if (paramViewport_) ShowWindow(paramViewport_, SW_HIDE);
        if (page_ == Page::Editor) {
            UpdateEditorWindowModeChrome();
            UpdateBatchToolbar();
            HideEditorComboHwnds();
        }
        ApplyParamLayerMasks();
    }

    bool IsFooterControl(HWND hwnd) const {
        return hwnd == remarkLabel_ || hwnd == remark_ || hwnd == addBtn_ || hwnd == modifyBtn_;
    }

    bool ShouldShowModifyButton() const {
        return !batchEditMode_ && selectedIndex_ >= 0
            && selectedIndex_ < static_cast<int>(actions_.size());
    }

    bool IsParamMaskControl(HWND hwnd) const {
        return hwnd == paramTopMask_ || hwnd == paramBottomMask_ || hwnd == paramRightMask_;
    }

    COLORREF ParamMaskColor(HWND hwnd) const {
        return hwnd == paramBottomMask_ ? kPanel : kWhite;
    }

    /// 打开编辑页时延后显示参数区，避免 UpdateParamViewportGeometry 强制揭开旧控件（绿条闪一下）
    bool ParamViewportAllowed() const {
        return page_ == Page::Editor && !editorOpenPending_;
    }

    void ApplyParamLayerMasks() {
        if (!hwnd_) return;
        UpdateParamViewportGeometry();
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
        if (breakoutTimeEdit_ && IsWindowVisible(breakoutTimeEdit_)) {
            SetWindowPos(breakoutTimeEdit_, HWND_TOP, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }

    void UpdateParamViewportGeometry() {
        if (!paramViewport_) return;
        const RECT visible = ParamScrollContentRect();
        const int w = visible.right - visible.left;
        const int h = visible.bottom - visible.top;
        if (w <= 0 || h <= 0) return;
        const RECT cur = WindowClientRect(paramViewport_);
        const bool sameBox = cur.left == visible.left && cur.top == visible.top
            && (cur.right - cur.left) == w && (cur.bottom - cur.top) == h;
        const bool wantShow = ParamViewportAllowed();
        const bool shown = IsWindowVisible(paramViewport_) != FALSE;
        if (sameBox && wantShow == shown) return;
        SetWindowPos(paramViewport_, HWND_TOP, visible.left, visible.top, w, h,
            SWP_NOACTIVATE | (wantShow ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }

    bool IsParamScrollManagedHwnd(HWND hwnd) const {
        for (const auto& entry : paramScrollLayout_) {
            if (entry.hwnd == hwnd) return true;
        }
        return false;
    }

    RECT EditorComboClientRect(HWND combo) const {
        if (!combo) return RECT{};
        if (IsParamScrollManagedHwnd(combo)) {
            for (const auto& entry : paramScrollLayout_) {
                if (entry.hwnd != combo) continue;
                const int y = entry.baseY - paramScrollY_;
                return RECT{entry.baseX, y, entry.baseX + entry.baseW, y + entry.baseH};
            }
        }
        return WindowClientRect(combo);
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
        // 仅依赖标记位：勿再用 IsParamViewportChild/page_ 门禁，否则 DRAWITEM
        // 路由变化时会落到 DrawOwnerButton，勾选框整块变绿按钮。
        return IsMarkedParamCheckbox(hwnd);
    }

    void PaintParamPanelCheckbox(HWND hwnd, HDC hdc, const RECT* itemRc = nullptr) const {
        if (!hwnd || !hdc) return;
        RECT rc{};
        if (itemRc) rc = *itemRc;
        else GetClientRect(hwnd, &rc);
        FillRectColor(hdc, rc, kWhite);
        wchar_t text[128]{};
        GetWindowTextW(hwnd, text, 128);
        HGDIOBJ oldFont = SelectObject(hdc, editorFont_ ? editorFont_ : font_);
        constexpr int kCbSize = 18;
        const int cbTop = rc.top + (rc.bottom - rc.top - kCbSize) / 2;
        RECT cbRc{rc.left, cbTop, rc.left + kCbSize, cbTop + kCbSize};
        DrawCheckbox(hdc, cbRc, Checked(hwnd));
        RECT textRc{rc.left + kCbSize + 4, rc.top, rc.right, rc.bottom};
        ::DrawTextIn(hdc, text, textRc, kText);
        if (oldFont) SelectObject(hdc, oldFont);
    }

    void DrawParamPanelCheckboxItem(DRAWITEMSTRUCT* dis) const {
        if (!dis) return;
        PaintParamPanelCheckbox(dis->hwndItem, dis->hDC, &dis->rcItem);
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
        case 34: append(moveRelControls_); break;
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
        case 33: append(gotoControls_); break;
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
                    ScaleX(b.left),
                    ScaleX(b.top),
                    ScaleX(b.right),
                    ScaleX(b.bottom)
                };
            }
            if (IsOcrDepHwnd(hwnd) || IsOcrHwnd(hwnd)) {
                return WindowClientRect(hwnd);
            }
            return RECT{
                ScaleX(b.left),
                ScaleY(b.top),
                ScaleX(b.right),
                ScaleY(b.bottom)
            };
        }
        return WindowClientRect(hwnd);
    }

    int ParamScrollEditorRightMargin() const {
        return ScaleX(kParamScrollEditorRightMarginDesign);
    }

    int ParamScrollBarGap() const {
        return ScaleX(kParamScrollBarGapDesign);
    }

    int ParamScrollContentLeft() const {
        return ScaleX(kParamScrollLeftDesign);
    }

    int ParamFieldInset() const {
        return ScaleX(kParamFieldInsetDesign);
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
        return ScaleX(kParamScrollRightDesign);
    }

    int ParamScrollOuterBottom() const {
        return ScaleY(kParamScrollBottomDesign);
    }

    int ParamScrollTrackRightEdge() const {
        return ParamScrollOuterRight();
    }

    RECT ParamScrollTrackRect() const {
        const int right = ParamScrollTrackRightEdge();
        const int left = right - UiLen(kEditorScrollW);
        const int top = ParamPanelContentTopY() + UiLen(2);
        const int bottom = ParamScrollViewportBottomY() - UiLen(2);
        if (bottom <= top || right <= left) return RECT{};
        return RECT{left, top, right, bottom};
    }

    int ParamScrollContentRight() const {
        return std::max(ParamScrollContentLeft() + 1,
            ParamScrollOuterRight() - UiLen(kEditorScrollW) - ParamScrollBarGap());
    }

    int ParamScrollContentWidth() const {
        return std::max(1, ParamScrollContentRight() - ParamScrollContentLeft());
    }

    int ParamPanelContentTopY() const {
        return ScaleY(kParamScrollTopDesign);
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
            rc.top = std::min(rc.top, actionRc.top - ScaleY(34));
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

    void InvalidateParamControlInViewport(HWND h) const {
        if (!h || !paramViewport_) return;
        RECT rc = WindowClientRect(h);
        const RECT vp = ParamViewportRect();
        RECT vr{
            rc.left - vp.left, rc.top - vp.top,
            rc.right - vp.left, rc.bottom - vp.top
        };
        InvalidateRect(paramViewport_, &vr, FALSE);
    }

    void LockParamViewportRedraw() const {
        if (paramViewport_) SendMessageW(paramViewport_, WM_SETREDRAW, FALSE, 0);
    }

    void UnlockParamViewportRedraw() const {
        if (!paramViewport_) return;
        SendMessageW(paramViewport_, WM_SETREDRAW, TRUE, 0);
        // 不带 RDW_ERASE：由 WM_PAINT 一次填白底+子控件，避免先擦后画的闪白
        RedrawWindow(paramViewport_, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_NOERASE);
    }

    void RequestOcrSubPanelRefresh() {
        if (!hwnd_ || ocrSubPanelRefreshPosted_) return;
        ocrSubPanelRefreshPosted_ = true;
        LockParamViewportRedraw();
        PostMessageW(hwnd_, WM_OCR_SUBPANEL_REFRESH, 0, 0);
    }

    void HideInactiveParamControls() {
        const int sel = popupAction_.sel;
        const bool aiFindImage = (sel == 30 && aiRegionByImageCheck_ && Checked(aiRegionByImageCheck_))
            || (sel == 31 && aiWithImageCheck_ && Checked(aiWithImageCheck_)
                && aiRegionByImageCheck2_ && Checked(aiRegionByImageCheck2_));
        auto stashGroup = [&](const std::vector<HWND>& group, bool active) {
            if (active) return;
            ParkParamGroup(group);
        };
        stashGroup(moveControls_, sel == 0);
        stashGroup(moveRelControls_, sel == 34);
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
        stashGroup(gotoControls_, sel == 33);
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
        auto appendHwnd = [&](HWND h) {
            if (!h) return;
            if (std::find(out.begin(), out.end(), h) == out.end()) out.push_back(h);
        };
        switch (sel) {
        case 0: appendGroup(moveControls_); break;
        case 34: appendGroup(moveRelControls_); break;
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
            appendHwnd(ocrResultModeLabel_);
            appendHwnd(ocrFollowUpLabel_);
            appendHwnd(ocrSearchLabel_);
            appendHwnd(ocrSearchVarLabel_);
            appendHwnd(ocrX1Label_);
            appendHwnd(ocrY1Label_);
            appendHwnd(ocrX2Label_);
            appendHwnd(ocrY2Label_);
            appendHwnd(ocrRegionLabel_);
            appendHwnd(ocrFindImageLabel_);
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
        case 33: appendGroup(gotoControls_); break;
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

    struct RemarkFieldMetrics {
        int panelLeft = 0;
        int labelW = 0;
        int fieldH = 0;
        int editX = 0;
        int editW = 0;
    };

    RemarkFieldMetrics ComputeRemarkFieldMetrics() const {
        const int panelLeft = ParamPanelLeft();
        const int maxRight = ParamFieldMaxRight();
        const int labelW = ScaleX(44);
        const int fieldH = ScaleY(22);
        const int labelGap = ScaleX(4);
        const int editX = panelLeft + labelW + labelGap;
        const int editW = std::max(40, maxRight - editX);
        return RemarkFieldMetrics{panelLeft, labelW, fieldH, editX, editW};
    }

    void LayoutRemarkStyleFieldRow(HWND label, HWND edit, int y, const RemarkFieldMetrics& m) const {
        if (label) {
            MoveParamAware(label, m.panelLeft, y, m.labelW, m.fieldH, FALSE);
            ShowWindow(label, SW_SHOW);
        }
        if (edit) {
            MoveParamAware(edit, m.editX, y, m.editW, m.fieldH, FALSE);
            ShowWindow(edit, SW_SHOW);
        }
    }

    int MeasureParamScrollContentBottom(const std::vector<HWND>& controls) const {
        int bottom = ParamPanelContentTopY();
        for (HWND h : controls) {
            if (!h || IsFooterControl(h)) continue;
            if (IsIntentionallyHiddenParamControl(h)) continue;
            if (!IsControlShown(h) && !IsEditorParamComboHwnd(h)) continue;
            const RECT rc = WindowClientRect(h);
            if (rc.top < ParamPanelContentTopY() - 64) continue;
            bottom = std::max(bottom, static_cast<int>(rc.bottom));
        }
        return bottom;
    }

    void ApplyEditorFooterLayout(int contentBottom = -1) {
        if (page_ != Page::Editor) return;

        const RemarkFieldMetrics remarkMetrics = ComputeRemarkFieldMetrics();
        const int panelLeft = remarkMetrics.panelLeft;
        const int maxRight = ParamFieldMaxRight();
        const int labelW = remarkMetrics.labelW;
        const int remarkH = remarkMetrics.fieldH;
        const int btnH = ScaleY(30);
        const int btnW = ScaleX(76);
        const int gap = ScaleY(18);
        const int panelTop = ParamPanelContentTopY();
        const int minRemarkY = ScaleY(kEditorRemarkY);
        const bool dynamicFooter = UsesDynamicParamPanel();
        if (contentBottom < 0) {
            std::vector<HWND> controls;
            CollectParamScrollControls(controls);
            contentBottom = MeasureParamScrollContentBottom(controls);
        }
        if (dynamicFooter && paramLayoutBottomHint_ > panelTop) {
            contentBottom = std::max(contentBottom, paramLayoutBottomHint_);
        }
        if (dynamicFooter && contentBottom < panelTop) return;
        const int measuredBottom = contentBottom > panelTop ? contentBottom + gap : 0;
        int remarkY;
        if (dynamicFooter) {
            remarkY = contentBottom + gap;
        } else {
            remarkY = measuredBottom > 0 ? measuredBottom : minRemarkY;
        }
        const int addY = remarkY + remarkH + gap;
        const int btnGap = ScaleX(10);
        int addX = maxRight - btnW;
        int modifyX = addX - btnGap - btnW;
        if (modifyX < panelLeft) {
            modifyX = panelLeft;
            addX = std::min(maxRight - btnW, modifyX + btnGap + btnW);
        }
        const int labelX = panelLeft;
        const int remarkX = remarkMetrics.editX;
        const int remarkW = remarkMetrics.editW;

        auto moveFooter = [&](HWND h, int x, int y, int w, int hgt) {
            if (!h) return;
            EnsureParamViewportParent(h);
            SetParamPosAware(h, x, y, w, hgt, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        };

        if (remarkLabel_) {
            ShowWindow(remarkLabel_, SW_SHOW);
            moveFooter(remarkLabel_, labelX, remarkY, labelW, remarkH);
        }
        if (remark_) {
            ShowWindow(remark_, SW_SHOW);
            moveFooter(remark_, remarkX, remarkY, remarkW, remarkH);
        }
        if (modifyBtn_) {
            ShowWindow(modifyBtn_, ShouldShowModifyButton() ? SW_SHOW : SW_HIDE);
            moveFooter(modifyBtn_, modifyX, addY, btnW, btnH);
        }
        if (addBtn_) {
            ShowWindow(addBtn_, SW_SHOW);
            moveFooter(addBtn_, addX, addY, btnW, btnH);
        }
        for (HWND h : {remarkLabel_, remark_, modifyBtn_, addBtn_}) {
            if (!h || !IsWindowVisible(h)) continue;
            SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
            RedrawWindow(child, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_FRAME);
        }
        UpdateHoverFromCursor();
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
        if (sel == 0) RefreshMoveParamPanel();
        else if (sel == 17) RefreshFindImageSubPanel();
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
            RedrawWindow(paramViewport_, nullptr, nullptr, RDW_INVALIDATE | RDW_NOCHILDREN);
            RefreshGrayButtonsInParamViewport();
        }
    }

    void ClearParamViewportSurface() {
        if (!paramViewport_ || page_ != Page::Editor) return;
        InvalidateRect(paramViewport_, nullptr, FALSE);
    }

    void RefreshParamViewportChildren() const {
        if (!paramViewport_ || page_ != Page::Editor) return;
        for (HWND child = GetWindow(paramViewport_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!IsWindowVisible(child)) continue;
            RedrawWindow(child, nullptr, nullptr,
                RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_FRAME);
        }
    }

    void FillParamViewportGaps(HDC hdc, HWND viewport, const RECT& rcPaint) const {
        if (!hdc || !viewport) return;
        HRGN fillRgn = CreateRectRgnIndirect(&rcPaint);
        for (HWND child = GetWindow(viewport, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!IsWindowVisible(child)) continue;
            if (IsEditorParamComboHwnd(child)) continue;
            RECT cr = MapRectFromMain(viewport, WindowClientRect(child));
            // Edit 外边框画在 client 外 1px，需避让；灰按钮边框画在 client 内，勿外扩——
            // 否则滚动后按钮外侧 1px 永不刷白，留下「全图/测试」等边沿线残影。
            wchar_t cls[16]{};
            GetClassNameW(child, cls, 16);
            if (lstrcmpW(cls, L"Edit") == 0) InflateRect(&cr, 1, 1);
            RECT inter{};
            if (!IntersectRect(&inter, &cr, &rcPaint)) continue;
            HRGN childRgn = CreateRectRgnIndirect(&inter);
            CombineRgn(fillRgn, fillRgn, childRgn, RGN_DIFF);
            DeleteObject(childRgn);
        }
        SelectClipRgn(hdc, fillRgn);
        FillRectColor(hdc, rcPaint, kWhite);
        SelectClipRgn(hdc, nullptr);
        DeleteObject(fillRgn);
    }

    void PostLayoutParamPanelRedraw() {
        if (!paramViewport_ || page_ != Page::Editor) return;
        RefreshGrayButtonsInParamViewport();
        for (HWND child = GetWindow(paramViewport_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!IsWindowVisible(child)) continue;
            if (IsEditorParamComboHwnd(child)) continue;
            RedrawWindow(child, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        }
    }

    void RaiseAiFindImageHeaderControls() const {
        for (HWND h : {aiFindImageLabel_, aiFindSelectRegionBtn_}) {
            if (!h || !IsWindowVisible(h)) continue;
            SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_FRAME);
        }
    }

    void RaiseOcrFindImageHeaderControls() const {
        for (HWND h : {ocrFindImageLabel_, ocrFindSelectRegionBtn_}) {
            if (!h || !IsWindowVisible(h)) continue;
            SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_FRAME);
        }
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

    void EnsureParamFooterVisible() {
        if (page_ != Page::Editor) return;
        const int maxScroll = MaxParamScroll();
        if (maxScroll <= 0) return;
        int footerBottom = ParamPanelContentTopY();
        for (HWND h : {remarkLabel_, remark_, modifyBtn_, addBtn_}) {
            if (!h || !IsWindowVisible(h)) continue;
            footerBottom = std::max(footerBottom, static_cast<int>(WindowClientRect(h).bottom));
        }
        const int vpBottom = ParamScrollViewportBottomY();
        if (footerBottom <= vpBottom) return;
        paramScrollY_ = std::min(maxScroll, footerBottom - vpBottom + ScaleY(8));
        ApplyParamScrollOffset(false);
    }

    void RestoreStaticParamLayoutsForSel(int sel) {
        auto restoreIdx = [&](int idx) {
            auto it = paramLayoutResults_.find(idx);
            if (it == paramLayoutResults_.end()) return;
            for (const auto& p : it->second.placements) {
                if (!p.hwnd || UsesRuntimeParamLayout(p.hwnd)) continue;
                RestoreEditorControlLayout(p.hwnd);
            }
        };
        if (sel >= 0 && sel <= 34 && sel != 29) restoreIdx(sel);
        if (sel == 5 || sel == 6) { restoreIdx(5); restoreIdx(6); }
        if (sel == 9 || sel == 10) { restoreIdx(9); restoreIdx(10); }
        if (sel == 29 || sel == 30 || sel == 31) restoreIdx(29);
        if (sel == 24) restoreIdx(240);
    }

    bool UsesDynamicParamPanel(int sel = -1) const {
        if (sel < 0) sel = popupAction_.sel;
        return sel == 0 || sel == 17 || sel == 18 || sel == 29 || sel == 30 || sel == 31;
    }

    void SyncParamScrollLayout(int layoutBottomHint = -1) {
        if (layoutBottomHint >= 0) paramLayoutBottomHint_ = layoutBottomHint;
        CaptureParamScrollBaseLayout(layoutBottomHint);
        paramScrollY_ = std::clamp(paramScrollY_, 0, MaxParamScroll());
        ApplyParamScrollOffset(false);
        UpdateParamViewportGeometry();
        RepaintParamPanelChrome();
        PostLayoutParamPanelRedraw();
    }

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

    bool IsMoveHwnd(HWND hwnd) const {
        for (HWND h : moveControls_) if (h == hwnd) return true;
        return false;
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

    bool IsAiLayoutHwnd(HWND hwnd) const {
        const int sel = popupAction_.sel;
        if (sel != 29 && sel != 30 && sel != 31) return false;
        auto contains = [hwnd](const std::vector<HWND>& group) {
            for (HWND h : group) if (h == hwnd) return true;
            return false;
        };
        return contains(aiCommonControls_) || contains(aiImageControls_)
            || contains(aiActionControls_) || contains(aiFindRegionControls_);
    }

    bool IsAiDynamicHwnd(HWND hwnd) const {
        return IsAiLayoutHwnd(hwnd);
    }

    bool UsesRuntimeParamLayout(HWND hwnd) const {
        if (popupAction_.sel == 0 && IsMoveHwnd(hwnd)) return true;
        if (IsOcrDepHwnd(hwnd) && popupAction_.sel == 18) return true;
        if (IsOcrHwnd(hwnd)) return true;
        if (IsAiLayoutHwnd(hwnd)) return true;
        if (popupAction_.sel == 17 && IsFindImageHwnd(hwnd)) return true;
        return false;
    }

    RECT ScaledEditorLayoutRect(HWND hwnd) const {
        for (const auto& item : editorLayouts_) {
            if (item.hwnd != hwnd) continue;
            const int x = ScaleX(item.base.left);
            const int y = ScaleY(item.base.top);
            const int w = std::max(1, ScaleX(item.base.right - item.base.left));
            int h = std::max(1, ScaleY(item.base.bottom - item.base.top));
            if (IsEditorListHeaderLabelBase(item.base)) h = EditorListColumnHeaderHeight(y);
            return RECT{x, y, x + w, y + h};
        }
        return WindowClientRect(hwnd);
    }

    void SyncParamControlScrollPosition(HWND hwnd) const {
        if (!hwnd || UsesRuntimeParamLayout(hwnd)) return;
        const RECT scaled = ScaledEditorLayoutRect(hwnd);
        MoveParamAware(hwnd, scaled.left, scaled.top, scaled.right - scaled.left, scaled.bottom - scaled.top, FALSE);
    }

    bool IsEditorListHeaderLabelBase(const RECT& base) const {
        if (base.top != kEditorListColumnHeaderY) return false;
        return base.left == 32 || base.left == 94 || base.left == 438 || base.left == 631;
    }

    int EditorListColumnHeaderHeight(int scaledHeaderTop) const {
        return std::max(1, UiLen(kListY) - scaledHeaderTop - UiLen(2));
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
        layout.actionX = ScaleX(kFindActionBtnX);
        layout.btnW = std::max(1, ScaleX(kFindBtnW));
        layout.compactBtnH = std::max(1, ScaleY(kFindImageSideBtnH));
        layout.sideGap = std::max(2, ScaleY(kFindImageSideBtnGap));
        layout.stackHeight = layout.compactBtnH * 3 + layout.sideGap * 2;
        return layout;
    }

    void LayoutFindImageSideStack(
        int previewTop, HWND screenshot, HWND local, HWND clear) const {
        LayoutOcrFindImageSideStack(previewTop, screenshot, local, clear);
    }

    int LayoutFindImageMatchScaleRows(int y) const {
        const int left = ScaleX(kFindContentLeft);
        const int fieldH = ScaleY(22);
        const int rowGap = ScaleY(kFindVGap);
        auto show = [](HWND h) { if (h) ShowWindow(h, SW_SHOW); };
        if (findMatchThreshold_) {
            MoveParamAware(findMatchThreshold_, left + ScaleX(91), y,
                ScaleX(40), fieldH, FALSE);
            show(findMatchThreshold_);
        }
        for (HWND h : findImageControls_) {
            if (!h) continue;
            wchar_t buf[16]{};
            GetWindowTextW(h, buf, 16);
            if (wcscmp(buf, L"匹配度大于") == 0) {
                MoveParamAware(h, left, y, ScaleX(90), fieldH, FALSE);
                show(h);
            } else if (wcscmp(buf, L"%") == 0) {
                MoveParamAware(h, left + ScaleX(135), y,
                    ScaleX(24), fieldH, FALSE);
                show(h);
            }
        }
        y += fieldH + rowGap;
        if (findScaleMin_) {
            MoveParamAware(findScaleMin_, left + ScaleX(65), y,
                ScaleX(40), fieldH, FALSE);
            show(findScaleMin_);
        }
        if (findScaleMax_) {
            MoveParamAware(findScaleMax_, left + ScaleX(151), y,
                ScaleX(40), fieldH, FALSE);
            show(findScaleMax_);
        }
        for (HWND h : findImageControls_) {
            if (!h) continue;
            wchar_t buf[16]{};
            GetWindowTextW(h, buf, 16);
            if (wcscmp(buf, L"最小缩放") == 0) {
                MoveParamAware(h, left, y, ScaleX(64), fieldH, FALSE);
                show(h);
            } else if (wcscmp(buf, L"最大") == 0) {
                MoveParamAware(h, left + ScaleX(110), y,
                    ScaleX(40), fieldH, FALSE);
                show(h);
            }
        }
        return y + fieldH + rowGap;
    }

    int LayoutFindImageFollowUpRow(int y) const {
        const int left = ScaleX(kFindContentLeft);
        const int btnH = ScaleY(kFindBtnH);
        const int rowGap = ScaleY(kFindVGap);
        const int labelW = ScaleX(kFindFollowLabelW);
        const int comboW = ScaleX(kFindFollowComboW);
        const int gap = ScaleX(8);
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
        const int rowGap = ScaleY(kFindVGap);
        const int fieldH = ScaleY(22);
        if (findSelectOffsetBtn_) {
            MoveOcrAt(findSelectOffsetBtn_, kFindSelectOffsetLeft, y, kFindSelectOffsetW, kFindImageSideBtnH);
            ShowWindow(findSelectOffsetBtn_, SW_SHOW);
        }
        y += OcrScaleY(kFindImageSideBtnH) + rowGap;
        LayoutFindImageTimeRow(y);
        return y + fieldH + rowGap;
    }

    void LayoutFindImageTimeRow(int y) const {
        LayoutRemarkStyleFieldRow(findTimeLabel_, findTimeEdit_, y, ComputeRemarkFieldMetrics());
    }

    int LayoutFindImageVarBlock(int y) const {
        if (findOffsetXLabel_) ShowWindow(findOffsetXLabel_, SW_HIDE);
        if (findOffsetYLabel_) ShowWindow(findOffsetYLabel_, SW_HIDE);
        if (findSelectOffsetBtn_) ShowWindow(findSelectOffsetBtn_, SW_HIDE);
        if (findTimeLabel_) ShowWindow(findTimeLabel_, SW_HIDE);
        if (findTimeEdit_) ShowWindow(findTimeEdit_, SW_HIDE);
        const int left = ScaleX(kFindContentLeft);
        const int fieldH = ScaleY(22);
        const int rowGap = ScaleY(kFindVGap);
        const int labelW = ScaleX(kFindMatchVarLabelW);
        const int editW = ScaleX(kFindMatchVarEditW);
        const int gap = ScaleX(4);
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
        const int previewX = ScaleX(kFindContentLeft);
        MoveParamAware(findImagePreviewBtn_, previewX, previewY, kFindImageSize, kFindImageSize, FALSE);
        LayoutOcrFindImageSideStack(previewY, findScreenshotBtn_, findLocalImageBtn_, findClearImageBtn_);
        return previewY + kFindImageSize + layout.sideGap;
    }

    int LayoutFindImagePreviewBlock() const {
        if (page_ != Page::Editor || popupAction_.sel != 17) return 0;
        const int previewY = ScaleY(kFindImageRowY);
        const int afterPreview = LayoutFindImagePreviewBlockAt(previewY);
        return LayoutFindImageMatchScaleRows(afterPreview);
    }


    void ApplyEditorControlScale(bool force = false) {
        const int pct = UiScalePercent();
        if (!force && editorScaleAppliedPct_ == pct && page_ == Page::Editor) {
            // 打开过程中参数区尚未就绪：勿刷新子面板，否则会把上次动作的绿按钮画出来
            if (editorOpenPending_) {
                ApplyParamLayerMasks();
                return;
            }
            // 缩放未变：只刷新当前动作类型的动态子面板与页眉
            if (popupAction_.sel == 0) RefreshMoveParamPanel();
            else if (popupAction_.sel == 17) RefreshFindImageSubPanel();
            else if (popupAction_.sel == 18) {
                RefreshOcrDepStatus();
                RefreshOcrSubPanel();
            } else if (popupAction_.sel == 29 || popupAction_.sel == 30 || popupAction_.sel == 31) {
                RefreshAiSubPanel();
            }
            UpdateEditorWindowModeChrome();
            ApplyParamLayerMasks();
            return;
        }
        for (const auto& item : editorLayouts_) {
            if (item.hwnd == paramViewport_) {
                UpdateParamViewportGeometry();
                continue;
            }
            if (IsFooterControl(item.hwnd)) continue;
            if (IsEditorMacroHeaderHwnd(item.hwnd)) continue;
            if (IsFindImageHwnd(item.hwnd)) {
                // 找图控件由 RefreshFindImageSubPanel 统一堆叠布局
            } else if (IsMoveHwnd(item.hwnd)) {
                // 移动鼠标控件由 RefreshMoveParamPanel 统一堆叠布局
            } else if (IsOcrDepHwnd(item.hwnd) || IsOcrHwnd(item.hwnd) || IsAiLayoutHwnd(item.hwnd)) {
                // OCR / AI 动态区由 Refresh*SubPanel 统一堆叠布局
            } else {
                const int x = ScaleX(item.base.left);
                const int y = ScaleY(item.base.top);
                const int w = std::max(1, ScaleX(item.base.right - item.base.left));
                int h = std::max(1, ScaleY(item.base.bottom - item.base.top));
                if (IsEditorListHeaderLabelBase(item.base)) h = EditorListColumnHeaderHeight(y);
                MoveParamAware(item.hwnd, x, y, w, h, FALSE);
            }
        }
        if (!editorOpenPending_) {
            if (page_ == Page::Editor && popupAction_.sel == 0) {
                RefreshMoveParamPanel();
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
            if (page_ == Page::Editor) UpdateEditorWindowModeChrome();
        } else {
            ConfigureEditorCombos();
            if (page_ == Page::Editor) UpdateEditorWindowModeChrome();
        }
        ApplyParamLayerMasks();
        editorScaleAppliedPct_ = pct;
    }

    // ── Page navigation ────────────────────────────────────────────
    void ShowHome() {
        CommitInlineRemark();
        CloseEditorPopup();
        CancelQuickInputTip();
        StopHoverTimer();
        BeginSmoothPageTransition(hwnd_);
        ++editorOpenGeneration_;
        editorOpenPending_ = false;
        editorOpenPhase_ = 0;
        ClearEditorProgressiveState();
        if (batchEditMode_) {
            batchEditMode_ = false;
            batchSelected_.clear();
        }
        // 仅清内存草稿；表单复位/扫盘删图放到揭开后（与录制优化关窗一样：先见主页）
        actionFormDrafts_.clear();
        page_ = Page::Home;
        ShowEditorControls(false);
        if (hasHomeRectBeforeEditor_) {
            SetWindowPos(hwnd_, nullptr,
                homeRectBeforeEditor_.left, homeRectBeforeEditor_.top,
                UiHomeWidth(), UiHomeHeight(),
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS);
            hasHomeRectBeforeEditor_ = false;
        }
        ClampHomeScroll();
        ClampRecordingScroll();
        // 勿用 EndSmooth 的 ALLCHILDREN：隐藏的编辑子控件极多，会拖住揭开
        editorFullClientBlit_ = true;
        RedrawWindow(hwnd_, nullptr, nullptr,
            RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
        editorFullClientBlit_ = false;
        SetWindowCloaked(hwnd_, false);
        BOOL disableTransitions = FALSE;
        DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED,
            &disableTransitions, sizeof(disableTransitions));
        outerShadow_.Sync();
        // wp=1：揭开后再 ResetActionFormSession + CleanupNewImages + 刷列表
        PostMessageW(hwnd_, WM_APP_HOME_REFRESH_LISTS, 1, 0);
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
        ++editorOpenGeneration_;
        const int openGen = editorOpenGeneration_;
        RECT homeRc{};
        if (page_ == Page::Home && GetWindowRect(hwnd_, &homeRc)) {
            homeRectBeforeEditor_ = homeRc;
            hasHomeRectBeforeEditor_ = true;
        } else {
            GetWindowRect(hwnd_, &homeRc);
        }

        // cloak 内：改尺寸 + 首屏列表；参数表单延后，缩短揭开前耗时
        BeginSmoothPageTransition(hwnd_);
        page_ = Page::Editor;
        HideEditorComboHwnds();
        ShowEditorControls(false);
        RECT editorRc = EditorRectFromHome(homeRc);
        SetWindowPos(hwnd_, nullptr, editorRc.left, editorRc.top, UiEditorWidth(), UiEditorHeight(),
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS);

        selectedIndex_ = -1; hoverIndex_ = -1; scrollOffset_ = 0; editingRemarkIndex_ = -1;
        collapsedContainers_.clear();
        batchEditMode_ = false;
        batchSelected_.clear();
        actions_.clear();
        MarkVisibleActionsDirty();
        ShowWindow(listRemarkEdit_, SW_HIDE);

        editorOpenPending_ = true;
        editorOpenCreateNew_ = createNew;
        editorOpenPhase_ = 0;
        editorOpenPath_.clear();
        actionFormDrafts_.clear();

        if (createNew) {
            currentScriptIndex_ = -1;
            currentPath_.clear();
            currentRecordTime_ = NowText();
            SetText(name_, TimestampName());
            loadedCoordMeta_ = StandardScriptCoordMeta();
            ResetEditorScriptChromeDefaults();
            SetPopupSel(popupMode_, mode_, 0);
            SetText(remark_, L"");
        } else if (index >= 0 && index < static_cast<int>(scripts_.size())) {
            currentScriptIndex_ = index;
            currentPath_ = scripts_[static_cast<size_t>(index)].path;
            editorOpenPath_ = currentPath_;
            LoadScriptFileProgressive(currentPath_);
        } else {
            editorOpenPending_ = false;
        }

        ApplyEditorFonts();
        ApplyEditorControlScale(false);
        UpdateBatchToolbar();
        StartHoverTimer();
        FiDbgInit(hwnd_);
        FiDbgSetMainWnd(hwnd_);

        // 首帧只出列表与页眉；参数区与添加/修改等 footer 等 FinishDeferred 布局后再显示
        ShowEditorControls(true, false);
        HideEditorComboHwnds();
        if (mode_) ShowWindow(mode_, SW_HIDE);
        if (actionCombo_) ShowWindow(actionCombo_, SW_HIDE);
        SetPopupSel(popupAction_, actionCombo_, 0);
        ResetEditorTransientFormState();
        for (HWND h : {remarkLabel_, remark_, addBtn_, modifyBtn_}) {
            if (h) ShowWindow(h, SW_HIDE);
        }
        if (paramViewport_) ShowWindow(paramViewport_, SW_HIDE);
        RefreshActionListLayer();
        UncloakEditorAfterReady();

        if (editorOpenPending_) {
            editorOpenPhase_ = 1;
            PostMessageW(hwnd_, WM_APP_EDITOR_FINISH_OPEN, static_cast<WPARAM>(openGen), 0);
        }
        if (editorParsePending_) {
            PostMessageW(hwnd_, WM_APP_EDITOR_PARSE_MORE, static_cast<WPARAM>(openGen), 0);
        }
    }

    void FinishDeferredEditorOpen(int openGen) {
        if (openGen != editorOpenGeneration_ || page_ != Page::Editor) return;
        if (!editorOpenPending_) return;
        LockParamViewportRedraw();
        ResetActionFormSession(editorOpenCreateNew_);
        if (editorOpenCreateNew_) RefreshRunBlockCombo();
        UpdateEditMode();
        if (mode_) ShowWindow(mode_, SW_HIDE);
        if (actionCombo_) ShowWindow(actionCombo_, SW_HIDE);
        editorOpenPending_ = false;
        editorOpenPhase_ = 0;
        if (paramViewport_) ShowWindow(paramViewport_, SW_SHOW);
        // Reset/LoadForm 已 ApplyEditorFooterLayout；再兜一次，避免首开坐标仍是创建时的设计值
        ApplyEditorFooterLayout();
        ApplyParamLayerMasks();
        UnlockParamViewportRedraw();
        DiscardSpuriousEditorInput();
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
        if (!UsesDynamicParamPanel(sel)) paramLayoutBottomHint_ = -1;

        // 先隐藏所有 Combo 标签，再由 ShowParamLayout 显示正确的
        HideEditorComboHwnds();
        HideInactiveParamControls();

        // ── 主面板切换 (基于布局索引) ──
        for (int i = 0; i <= 31; ++i) {
            if (i == 29) continue;
            ShowParamLayout(i, i == sel);
        }
        ShowParamLayout(29, sel == 29 || sel == 30 || sel == 31);
        ShowParamLayout(300, sel == 30);
        ShowParamLayout(310, sel == 31);
        ShowParamLayout(32, sel == 32);
        ShowParamLayout(33, sel == 33);
        ShowParamLayout(34, sel == 34);

        if (sel == 0) RefreshMoveParamPanel();

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
        if (UsesDynamicParamPanel(sel)) {
            ApplyParamScrollOffset(true);
            UpdateParamViewportGeometry();
            if (MaxParamScroll() <= 0 && hwnd_) {
                HDC dc = GetDC(hwnd_);
                ClearParamScrollTrack(dc);
                ReleaseDC(hwnd_, dc);
            }
        } else {
            ApplyParamScroll();
        }
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

    void CaptureParamScrollBaseLayout(int layoutBottomHint = -1) {
        if (UsesDynamicParamPanel()) {
            RevealParamControlsForCapture();
            if (paramViewport_) UpdateWindow(paramViewport_);
        }

        std::unordered_map<HWND, ParamScrollLayoutEntry> prevLayout;
        for (const auto& entry : paramScrollLayout_) {
            if (entry.hwnd) prevLayout[entry.hwnd] = entry;
        }

        paramScrollLayout_.clear();
        paramControlsBottom_ = ParamPanelContentTopY();
        paramContentBottom_ = ParamPanelContentTopY();
        std::vector<HWND> controls;
        CollectParamScrollControls(controls);
        const bool dynamicPanel = UsesDynamicParamPanel();
        if (dynamicPanel && paramScrollY_ != 0) {
            for (const auto& [hwnd, entry] : prevLayout) {
                if (!hwnd) continue;
                SetParamPosAware(hwnd, entry.baseX, entry.baseY, entry.baseW, entry.baseH,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
            }
        }
        for (HWND h : controls) {
            if (!h) continue;
            if (IsIntentionallyHiddenParamControl(h)) continue;
            if (!dynamicPanel) {
                if (!IsControlShown(h) && !IsEditorParamComboHwnd(h)) continue;
            } else if (!IsEditorParamComboHwnd(h)) {
                const RECT probe = WindowClientRect(h);
                if (probe.top < -1000) continue;
            }
            if (IsFooterControl(h)) continue;
            EnsureParamViewportParent(h);
            if (!dynamicPanel) SyncParamControlScrollPosition(h);
            RECT rc = WindowClientRect(h);
            if (dynamicPanel && paramScrollY_ != 0) {
                OffsetRect(&rc, 0, paramScrollY_);
            }
            if (auto it = prevLayout.find(h); it != prevLayout.end()) {
                const auto& old = it->second;
                const int oldW = old.baseW;
                const int oldH = old.baseH;
                const int newW = rc.right - rc.left;
                const int newH = rc.bottom - rc.top;
                if (old.baseX != rc.left || old.baseY != rc.top || oldW != newW || oldH != newH) {
                    const RECT vp = ParamViewportRect();
                    RECT oldVp{
                        old.baseX - vp.left, old.baseY - vp.top,
                        old.baseX + oldW - vp.left, old.baseY + oldH - vp.top
                    };
                    if (paramViewport_) InvalidateRect(paramViewport_, &oldVp, FALSE);
                }
            }
            const int maxRight = ParamFieldMaxRight();
            if (rc.left < maxRight && rc.right > maxRight) {
                SetParamPosAware(h, rc.left, rc.top, maxRight - rc.left, rc.bottom - rc.top,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                rc.right = maxRight;
            }
            paramScrollLayout_.push_back(ParamScrollLayoutEntry{
                h, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top
            });
            paramControlsBottom_ = std::max(paramControlsBottom_, static_cast<int>(rc.bottom));
            paramContentBottom_ = std::max(paramContentBottom_, static_cast<int>(rc.bottom));
        }

        int footerAnchor = paramControlsBottom_;
        if (layoutBottomHint >= 0) {
            footerAnchor = std::max(footerAnchor, layoutBottomHint);
        } else if (UsesDynamicParamPanel() && paramLayoutBottomHint_ >= 0) {
            footerAnchor = std::max(footerAnchor, paramLayoutBottomHint_);
        }
        ApplyEditorFooterLayout(footerAnchor);

        for (HWND h : {remarkLabel_, remark_, modifyBtn_, addBtn_}) {
            if (!h) continue;
            EnsureParamViewportParent(h);
            RECT rc = WindowClientRect(h);
            const int maxRight = ParamFieldMaxRight();
            if (rc.left < maxRight && rc.right > maxRight) {
                SetParamPosAware(h, rc.left, rc.top, maxRight - rc.left, rc.bottom - rc.top,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                rc.right = maxRight;
            }
            paramScrollLayout_.push_back(ParamScrollLayoutEntry{
                h, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top
            });
            paramContentBottom_ = std::max(paramContentBottom_, static_cast<int>(rc.bottom));
        }

        UpdateParamViewportGeometry();

        if (footerAnchor >= ParamPanelContentTopY()) {
            const int gap = ScaleY(18);
            const int remarkH = ScaleY(22);
            const int btnGap = ScaleY(18);
            const int btnH = ScaleY(30);
            const int estimatedBottom = footerAnchor + gap + remarkH + btnGap + btnH;
            paramContentBottom_ = std::max(paramContentBottom_, estimatedBottom);
        }
    }

    bool IsIntentionallyHiddenParamControl(HWND h) const {
        if (!h) return false;
        // ApplyParamScrollOffset 会对布局项强制 ShowWindow；无选中时必须继续隐藏「修改」
        if (h == modifyBtn_ && !ShouldShowModifyButton()) return true;
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
        if (sel == 18) {
            const bool regionByImage = ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_);
            if (regionByImage) {
                if (h == ocrRegionLabel_ || h == ocrFullScreenBtn_ || h == ocrSelectRegionBtn_) {
                    return true;
                }
            } else {
                for (HWND hide : ocrFindRegionControls_) {
                    if (h == hide) return true;
                }
                if (h == ocrFindSelectRegionBtn_ || h == ocrFindImageLabel_) return true;
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

    void ApplyParamScrollOffset(bool repaintChrome = true, bool eraseViewport = true, bool hideOffscreen = true, bool postLayoutRedraw = true) {
        const RECT contentVp = ParamScrollContentRect();
        const bool effectiveHideOffscreen = hideOffscreen && paramScrollY_ > 0;
        // 记录移位前可见控件区域，SWP_NOCOPYBITS 不会自动擦掉旧位置
        std::vector<RECT> staleVpRects;
        if (paramViewport_ && page_ == Page::Editor) {
            const RECT vp = ParamViewportRect();
            for (const auto& entry : paramScrollLayout_) {
                if (!entry.hwnd || IsIntentionallyHiddenParamControl(entry.hwnd)) continue;
                const bool pinFooter = IsFooterControl(entry.hwnd) && !UsesDynamicParamPanel();
                const int scrollY = pinFooter ? entry.baseY : entry.baseY - paramScrollY_;
                const RECT mainRc{entry.baseX, scrollY, entry.baseX + entry.baseW, scrollY + entry.baseH};
                RECT visible{};
                if (!IntersectRect(&visible, &mainRc, &contentVp)) continue;
                staleVpRects.push_back(RECT{
                    mainRc.left - vp.left, mainRc.top - vp.top,
                    mainRc.right - vp.left, mainRc.bottom - vp.top
                });
            }
        }
        // 滚动/重排前仅标记旧区域待重绘（不 erase，避免白底闪一下盖住子控件）
        if (eraseViewport && paramViewport_ && page_ == Page::Editor) {
            for (const RECT& r : staleVpRects)
                InvalidateRect(paramViewport_, &r, FALSE);
        }
        const bool dynamicPanelScroll = UsesDynamicParamPanel();
        HDWP hdwp = (!dynamicPanelScroll && !paramScrollLayout_.empty())
            ? BeginDeferWindowPos(static_cast<UINT>(paramScrollLayout_.size()))
            : nullptr;
        for (const auto& entry : paramScrollLayout_) {
            if (!entry.hwnd) continue;
            if (IsIntentionallyHiddenParamControl(entry.hwnd)) {
                ShowWindow(entry.hwnd, SW_HIDE);
                SetWindowRgn(entry.hwnd, nullptr, FALSE);
                if (dynamicPanelScroll) {
                    SetParamPosAware(entry.hwnd, -5000, -5000, entry.baseW, entry.baseH,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                } else if (hdwp) {
                    hdwp = DeferWindowPos(hdwp, entry.hwnd, nullptr, -5000, -5000, entry.baseW, entry.baseH,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                } else {
                    SetParamPosAware(entry.hwnd, -5000, -5000, entry.baseW, entry.baseH,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                }
                continue;
            }
            EnsureParamViewportParent(entry.hwnd);
            const bool pinFooter = IsFooterControl(entry.hwnd) && !dynamicPanelScroll;
            const int y = pinFooter ? entry.baseY : entry.baseY - paramScrollY_;
            const RECT ctrl{entry.baseX, y, entry.baseX + entry.baseW, y + entry.baseH};
            RECT visible{};
            if (effectiveHideOffscreen && !IsFooterControl(entry.hwnd)
                && !IntersectRect(&visible, &ctrl, &contentVp)) {
                ShowWindow(entry.hwnd, SW_HIDE);
                SetWindowRgn(entry.hwnd, nullptr, FALSE);
                if (dynamicPanelScroll) {
                    SetParamPosAware(entry.hwnd, -5000, -5000, entry.baseW, entry.baseH,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                } else if (hdwp) {
                    hdwp = DeferWindowPos(hdwp, entry.hwnd, nullptr, -5000, -5000, entry.baseW, entry.baseH,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                } else {
                    SetParamPosAware(entry.hwnd, -5000, -5000, entry.baseW, entry.baseH,
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
                }
                continue;
            }
            SetWindowRgn(entry.hwnd, nullptr, FALSE);
            const RECT vp = ParamViewportRect();
            const int targetX = GetParent(entry.hwnd) == paramViewport_ ? entry.baseX - vp.left : entry.baseX;
            const int targetY = GetParent(entry.hwnd) == paramViewport_ ? y - vp.top : y;
            if (dynamicPanelScroll) {
                SetParamPosAware(entry.hwnd, entry.baseX, y, entry.baseW, entry.baseH,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
            } else if (hdwp) {
                hdwp = DeferWindowPos(hdwp, entry.hwnd, nullptr, targetX, targetY, entry.baseW, entry.baseH,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
            } else {
                SetWindowPos(entry.hwnd, nullptr, targetX, targetY, entry.baseW, entry.baseH,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
            }
            if (IsEditorParamComboHwnd(entry.hwnd)) {
                ShowWindow(entry.hwnd, SW_HIDE);
            } else if (entry.hwnd == modifyBtn_) {
                ShowWindow(modifyBtn_, ShouldShowModifyButton() ? SW_SHOWNA : SW_HIDE);
            } else {
                ShowWindow(entry.hwnd, SW_SHOWNA);
            }
        }
        if (hdwp) EndDeferWindowPos(hdwp);
        for (HWND h : {remarkLabel_, remark_, modifyBtn_, addBtn_}) {
            if (!h || !IsWindowVisible(h)) continue;
            SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (paramViewport_ && page_ == Page::Editor) {
            for (const RECT& r : staleVpRects)
                InvalidateRect(paramViewport_, &r, FALSE);
            if (!postLayoutRedraw) {
                for (const auto& entry : paramScrollLayout_) {
                    if (!entry.hwnd || IsIntentionallyHiddenParamControl(entry.hwnd)) continue;
                    const bool pinFooter = IsFooterControl(entry.hwnd) && !UsesDynamicParamPanel();
                    const int y = pinFooter ? entry.baseY : entry.baseY - paramScrollY_;
                    const RECT ctrl{entry.baseX, y, entry.baseX + entry.baseW, y + entry.baseH};
                    RECT visible{};
                    if (!IntersectRect(&visible, &ctrl, &contentVp)) continue;
                    InvalidateParamControlInViewport(entry.hwnd);
                }
            }
        }
        // 滚轮轻量路径（erase/repaint 均为 false）跳过遮罩与取消/保存强制重绘，避免整区闪
        if (eraseViewport || repaintChrome) ApplyParamLayerMasks();
        if (editorPopupOpen_ >= 0) SyncEditorDropPopup();
        if (quickInputTipShown_ != QuickInputTipKind::None) SyncQuickInputTipPopup();
        if (repaintChrome) RepaintParamPanelChrome();
        if (postLayoutRedraw) PostLayoutParamPanelRedraw();
    }

    void RestoreParamPanelLayout() {
        std::vector<HWND> active;
        CollectParamScrollControls(active);
        std::unordered_set<HWND> activeSet(active.begin(), active.end());
        for (const auto& item : editorLayouts_) {
            if (item.hwnd == paramViewport_) {
                UpdateParamViewportGeometry();
                continue;
            }
            if (IsFooterControl(item.hwnd)) continue;
            if (IsOcrDepHwnd(item.hwnd)) continue;
            if (IsOcrHwnd(item.hwnd)) continue;
            if (IsFindImageHwnd(item.hwnd)) continue;
            if (IsMoveHwnd(item.hwnd)) continue;
            if (IsAiLayoutHwnd(item.hwnd)) continue;
            if (!activeSet.count(item.hwnd)) continue;
            const int x = ScaleX(item.base.left);
            const int y = ScaleY(item.base.top);
            const int w = std::max(1, ScaleX(item.base.right - item.base.left));
            int h = std::max(1, ScaleY(item.base.bottom - item.base.top));
            if (IsEditorListHeaderLabelBase(item.base)) h = EditorListColumnHeaderHeight(y);
            MoveParamAware(item.hwnd, x, y, w, h, FALSE);
        }
    }

    void RebuildParamPanelLayout() {
        ClearParamScrollClipping();
        paramScrollY_ = 0;
        HideInactiveParamControls();
        RestoreParamPanelLayout();
        RefreshDynamicParamLayout();
        SyncSharedVarComboVisibility();
        if (!UsesDynamicParamPanel()) {
            RevealParamControlsForCapture();
            CaptureParamScrollBaseLayout();
            paramScrollY_ = std::clamp(paramScrollY_, 0, MaxParamScroll());
            ApplyParamScrollOffset(false);
        }
        ApplyParamLayerMasks();
        RepaintParamPanelChrome();
        PostLayoutParamPanelRedraw();
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
        paramScrollY_ = 0;
        paramLayoutBottomHint_ = -1;
        RefreshDynamicParamLayout();
        RepaintParamPanelChrome();
        PostLayoutParamPanelRedraw();
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
        ClearParamScrollClipping();
        const int sel = popupAction_.sel;
        if (!UsesDynamicParamPanel(sel)) {
            RestoreStaticParamLayoutsForSel(sel);
        }
        CaptureParamScrollBaseLayout();
        paramScrollY_ = std::clamp(paramScrollY_, 0, MaxParamScroll());
        ApplyParamScrollOffset(true);
        UpdateParamViewportGeometry();
        if (MaxParamScroll() <= 0 && hwnd_) {
            HDC dc = GetDC(hwnd_);
            ClearParamScrollTrack(dc);
            ReleaseDC(hwnd_, dc);
        }
    }

    void ScrollParamPanel(int deltaY) {
        const int maxScroll = MaxParamScroll();
        if (maxScroll <= 0) return;
        const int oldScroll = paramScrollY_;
        paramScrollY_ = std::clamp(paramScrollY_ + deltaY, 0, maxScroll);
        if (oldScroll == paramScrollY_) return;
        // 滚轮滚动：冻结重绘→移位→单次合成刷新，避免整区闪烁与按钮边缘残影
        LockParamViewportRedraw();
        ApplyParamScrollOffset(/*repaintChrome*/ false, /*eraseViewport*/ false,
            /*hideOffscreen*/ true, /*postLayoutRedraw*/ false);
        UnlockParamViewportRedraw();
        if (hwnd_) {
            HDC mainDc = GetDC(hwnd_);
            if (mainDc) {
                PaintParamScrollScrollbar(mainDc);
                ReleaseDC(hwnd_, mainDc);
            }
        }
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

    bool IsEditorComboAnchorHwnd(HWND h) const {
        return h == mode_ || h == actionCombo_ || h == mousePressButton_ || h == clickButton_
            || h == loopTypeCombo_ || h == runBlockCombo_ || h == hotkeyShortcutCombo_
            || h == quickInputVarCombo_ || h == aiVarCombo_ || h == runMacroCombo_
            || h == mousePlaybackCombo_ || h == scrollDirectionCombo_ || h == findFollowUpCombo_
            || h == ocrResultModeCombo_ || h == ocrFollowUpCombo_ || h == ocrSearchVarCombo_
            || h == ifVarCombo_ || h == ifOperatorCombo_ || h == ifConnectorCombo_
            || h == runProgramCombo_ || h == aiModelCombo_ || h == aiContextModeCombo_
            || h == aiOutputTypeCombo_ || h == aiSearchRegionCombo_
            || h == wmSelectMethod_;
    }

    void LayoutFindImageRegionButtons(int y) {
        if (!findRegionLabel_ || !findFullScreenBtn_ || !findSelectRegionBtn_) return;
        const int left = ParamPanelLeft();
        const int maxRight = ParamFieldMaxRight();
        const int btnGap = ScaleX(kFindRegionBtnGap);
        const int labelGap = ScaleX(kFindRegionLabelGap);
        const int selectW = kFindBtnW;
        HDC hdc = GetDC(hwnd_);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, editorFont_ ? editorFont_ : font_));
        wchar_t labelBuf[32]{};
        GetWindowTextW(findRegionLabel_, labelBuf, 32);
        SIZE labelSz{}, fullSz{};
        GetTextExtentPoint32W(hdc, labelBuf, static_cast<int>(wcslen(labelBuf)), &labelSz);
        GetTextExtentPoint32W(hdc, L"全图", 2, &fullSz);
        SelectObject(hdc, old);
        ReleaseDC(hwnd_, hdc);
        const int labelW = std::max(ScaleX(kFindRegionLabelW),
            static_cast<int>(labelSz.cx) + 4);
        const int fullWWant = std::max(44, static_cast<int>(fullSz.cx) + ScaleX(18));
        const int selectWScaled = ScaleX(selectW);
        const int availW = maxRight - left;
        const int fullWMax = std::max(44, availW - labelW - labelGap - btnGap - selectWScaled);
        const int fullW = std::max(44, std::min(fullWMax, fullWWant));
        const int totalW = labelW + labelGap + fullW + btnGap + selectWScaled;
        const int startX = left + std::max(0, (availW - totalW) / 2);
        const int labelXDesign = MulDiv(startX, kEditorBaseWidth, UiEditorWidth());
        const int fullXDesign = MulDiv(startX + labelW + labelGap, kEditorBaseWidth, UiEditorWidth());
        const int selectXDesign = MulDiv(startX + labelW + labelGap + fullW + btnGap, kEditorBaseWidth, UiEditorWidth());
        const int fullWDesign = MulDiv(fullW, kEditorBaseWidth, UiEditorWidth());
        MoveOcrAt(findRegionLabel_, labelXDesign, y, MulDiv(labelW, kEditorBaseWidth, UiEditorWidth()), kFindBtnH);
        MoveOcrAt(findFullScreenBtn_, fullXDesign, y, fullWDesign, kFindBtnH);
        MoveOcrAt(findSelectRegionBtn_, selectXDesign, y, selectW, kFindBtnH);
        ShowWindow(findRegionLabel_, SW_SHOW);
        ShowWindow(findFullScreenBtn_, SW_SHOW);
        ShowWindow(findSelectRegionBtn_, SW_SHOW);
    }

    void RaiseFindImageRegionButtons() const {
        for (HWND h : {findRegionLabel_, findFullScreenBtn_, findSelectRegionBtn_}) {
            if (!h || !IsWindowVisible(h)) continue;
            SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_FRAME);
        }
    }

    void RaiseOcrRegionButtons() const {
        for (HWND h : {ocrRegionLabel_, ocrFullScreenBtn_, ocrSelectRegionBtn_}) {
            if (!h || !IsWindowVisible(h)) continue;
            SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_FRAME);
        }
    }

    // 参数区复选框切换会触发 viewport 白底填充；邻近灰按钮须立即重绘，否则上边框被盖住。
    void RefreshOcrNeighborGrayButtons() {
        if (popupAction_.sel != 18) return;
        if (ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_))
            RaiseOcrFindImageHeaderControls();
        else
            RaiseOcrRegionButtons();
        RefreshGrayButtonsInParamViewport();
    }

    int LayoutFindImageHeaderRow(int y) const {
        const FindImageSideButtonLayout layout = ComputeFindImageSideButtonLayout();
        const int rowH = layout.compactBtnH;
        const int left = ScaleX(kFindContentLeft);
        if (findImageHeaderLabel_) {
            MoveParamAware(findImageHeaderLabel_, left, y, ScaleX(90), rowH, FALSE);
            ShowWindow(findImageHeaderLabel_, SW_SHOW);
        }
        PlaceOcrFindImageCompactButton(findTestBtn_, y);
        if (findTestBtn_) ShowWindow(findTestBtn_, SW_SHOW);
        return y + rowH + layout.sideGap;
    }

    void RefreshMoveParamPanel() {
        const int left = ParamPanelLeft();
        const int maxRight = ParamFieldMaxRight();
        const int fieldH = ScaleY(22);
        const int hintH = ScaleY(25);
        const int gap1 = ScaleX(1);
        const int gap5 = ScaleX(5);
        const int gap7 = ScaleY(7);
        const int gap8 = ScaleY(8);
        const int gap9 = ScaleY(9);
        const int gap10 = ScaleY(10);
        const int gap15 = ScaleY(15);
        const int btnH = ScaleY(32);

        int y = ScaleY(180);

        if (moveHintLabel_) {
            MoveParamAware(moveHintLabel_, left, y,
                std::min(ScaleX(190), maxRight - left), hintH, FALSE);
            ShowWindow(moveHintLabel_, SW_SHOW);
        }
        y += hintH + gap9;

        auto layoutCoordRandomRow = [&](HWND xLabel, HWND xEdit, HWND rndLabel, HWND rndEdit) {
            int x = left;
            auto mv = [&](HWND h, int wDesign) {
                if (!h) return;
                const int w = ScaleX(wDesign);
                MoveParamAware(h, x, y, w, fieldH, FALSE);
                ShowWindow(h, SW_SHOW);
                x += w;
            };
            mv(xLabel, 25);
            x += gap1;
            mv(xEdit, 87);
            x += gap1;
            mv(rndLabel, 50);
            x += gap5;
            mv(rndEdit, 25);
        };

        layoutCoordRandomRow(moveXLabel_, moveX_, moveRandomXLabel_, moveRandomX_);
        y += fieldH + gap15;

        layoutCoordRandomRow(moveYLabel_, moveY_, moveRandomYLabel_, moveRandomY_);
        y += fieldH + gap10;

        if (crosshairBtn_) {
            MoveParamAware(crosshairBtn_, left, y, ScaleX(186), btnH, FALSE);
            ShowWindow(crosshairBtn_, SW_SHOW);
        }
        y += btnH + gap7;

        if (moveFromVar_) {
            MoveParamAware(moveFromVar_, left, y, ScaleX(180), hintH, FALSE);
            ShowWindow(moveFromVar_, SW_SHOW);
        }
        y += hintH + gap7;

        auto layoutVarRow = [&](HWND xLabel, HWND xEdit) {
            int x = left;
            auto mv = [&](HWND h, int wDesign) {
                if (!h) return;
                const int w = ScaleX(wDesign);
                MoveParamAware(h, x, y, w, fieldH, FALSE);
                ShowWindow(h, SW_SHOW);
                x += w;
            };
            mv(xLabel, 25);
            x += gap1;
            mv(xEdit, 168);
        };

        layoutVarRow(moveVarXLabel_, moveVarX_);
        y += fieldH + gap15;

        layoutVarRow(moveVarYLabel_, moveVarY_);
        y += fieldH + gap8;

        if (moveHintFooter_) {
            const int hintFooterH = ScaleY(56);
            MoveParamAware(moveHintFooter_, left, y, std::max(1, maxRight - left), hintFooterH, FALSE);
            ShowWindow(moveHintFooter_, SW_SHOW);
            y += hintFooterH;
        }

        SyncParamScrollLayout(y);
    }

    void RefreshFindImageSubPanel() {
        const bool saveVar = popupFindFollowUp_.sel == 2;
        if (saveVar) {
            ParkParamGroup(findImageOffsetControls_);
            for (HWND h : findImageVarControls_) {
                if (h) ShowWindow(h, SW_SHOW);
            }
        } else {
            ParkParamGroup(findImageVarControls_);
            for (HWND h : findImageOffsetControls_) {
                if (h) ShowWindow(h, SW_SHOW);
            }
        }
        HideFindImageFloatingControls();
        const int rowGap = ScaleY(kFindVGap);
        const int btnH = ScaleY(kFindBtnH);
        int y = ScaleY(kFindRegionRowY);
        LayoutFindImageRegionButtons(y);
        RaiseFindImageRegionButtons();
        y += btnH + rowGap;
        y = LayoutParamCoordRows(
            y,
            findX1Label_, findX1_, findY1Label_, findY1_,
            findX2Label_, findX2_, findY2Label_, findY2_);
        y = LayoutFindImageHeaderRow(y);
        y = LayoutFindImagePreviewBlockAt(y);
        if (findImagePreviewBtn_) {
            SetWindowPos(findImagePreviewBtn_, HWND_BOTTOM, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        y = LayoutFindImageMatchScaleRows(y);
        y = LayoutFindImageFollowUpRow(y);
        if (saveVar) y = LayoutFindImageVarBlock(y);
        else y = LayoutFindImageOffsetBlock(y);
        ShowFindImageSideControls(
            findImagePreviewBtn_, findScreenshotBtn_, findLocalImageBtn_, findClearImageBtn_);
        RefreshGrayButtonsInParamViewport();
        SyncParamScrollLayout(y);
        FiDbgLog(L"FIND_PANEL_REFRESH", L"layout snapshot");
        FiDbgLogGrayButtonLayout(L"FIND/screenshot", findScreenshotBtn_);
        FiDbgLogGrayButtonLayout(L"FIND/local", findLocalImageBtn_);
        FiDbgLogGrayButtonLayout(L"FIND/clear", findClearImageBtn_);
        FiDbgLogGrayButtonLayout(L"FIND/test", findTestBtn_);
        FiDbgLogGrayButtonLayout(L"OCR/screenshot", ocrFindScreenshotBtn_);
        FiDbgLogGrayButtonLayout(L"OCR/local", ocrFindLocalImageBtn_);
        FiDbgLogGrayButtonLayout(L"OCR/clear", ocrFindClearImageBtn_);
    }

    const wchar_t* GrayButtonDebugName(HWND hwnd) const {
        if (!hwnd) return L"(null)";
        if (hwnd == findFullScreenBtn_) return L"findFullScreen";
        if (hwnd == findSelectRegionBtn_) return L"findSelectRegion";
        if (hwnd == findTestBtn_) return L"findTest";
        if (hwnd == findImagePreviewBtn_) return L"findImagePreview";
        if (hwnd == findScreenshotBtn_) return L"findScreenshot";
        if (hwnd == findLocalImageBtn_) return L"findLocalImage";
        if (hwnd == findClearImageBtn_) return L"findClearImage";
        if (hwnd == findSelectOffsetBtn_) return L"findSelectOffset";
        if (hwnd == ocrFindScreenshotBtn_) return L"ocrFindScreenshot";
        if (hwnd == ocrFindLocalImageBtn_) return L"ocrFindLocalImage";
        if (hwnd == ocrFindClearImageBtn_) return L"ocrFindClearImage";
        if (hwnd == ocrFindSelectRegionBtn_) return L"ocrFindSelectRegion";
        return L"grayOther";
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
        return ScaleX(designPx);
    }

    static int OcrScaleY(int designPx) {
        return ScaleY(designPx);
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
        if (GetFocus() == ctrl) {
            HWND parent = GetParent(ctrl);
            if (parent) SetFocus(parent);
        }
        InvalidateGrayButton(ctrl);
        UpdateHoverFromCursor();
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
        CrosshairDragBinding wmTargetBinding{};
        wmTargetBinding.mode = CrosshairDragMode::WindowTarget;
        wmTargetBinding.targetEdit = wmTargetPathEdit_;
        wmTargetBinding.onWindowTarget = [this](const WindowInfoFromPoint& info) {
            ApplyWindowModeTargetFromPoint(info);
        };
        crosshairDrag_.RegisterButton(wmTargetCrosshairBtn_, wmTargetBinding);
    }

    void ApplyWindowModeTargetFromPoint(const WindowInfoFromPoint& info) {
        if (!info.processPath.empty()) {
            scriptWindowMode_.targetExePath = info.processPath;
            if (wmTargetPathEdit_) SetText(wmTargetPathEdit_, info.processPath);
        }
        if (!info.windowTitle.empty()) {
            scriptWindowMode_.windowName = info.windowTitle;
            scriptWindowMode_.targetWindowTitle = info.windowTitle;
        }
        if (!info.windowClassName.empty()) scriptWindowMode_.windowClassName = info.windowClassName;
        if (!info.childWindowClassName.empty()) {
            scriptWindowMode_.childWindowClassName = info.childWindowClassName;
        }
        scriptWindowMode_.targetPickX = info.x;
        scriptWindowMode_.targetPickY = info.y;
        scriptWindowMode_.selectMethod = windowmode::WindowSelectMethod::UseEditorWindowClass;
        SyncScriptWindowModeFromEditor();
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
        if (hwnd == wmSelectMethod_ && IsEditorWindowModeActive()) return 23;
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
        if (wmSelectMethod_ && IsEditorWindowModeActive() && PtIn(WindowClientRect(wmSelectMethod_), x, y)) return 23;
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
            if (PtIn(EditorComboClientRect(hit.hwnd), x, y)) return hit.id;
        }
        if (IsParamComboVisible(7)) {
            if (HWND varCombo = ActiveVarComboHwnd()) {
                if (PtIn(EditorComboClientRect(varCombo), x, y)) return 7;
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
        MarkVisibleActionsDirty();
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
        const int btnH = ScaleY(kFindBtnH);
        const int btnGap = ScaleX(kFindRegionBtnGap);
        const int labelGap = ScaleX(kFindRegionLabelGap);
        const int selectW = ScaleX(kFindBtnW);
        HDC hdc = GetDC(hwnd_);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, editorFont_ ? editorFont_ : font_));
        wchar_t labelBuf[32]{};
        GetWindowTextW(regionLabel, labelBuf, 32);
        SIZE labelSz{}, fullSz{};
        GetTextExtentPoint32W(hdc, labelBuf, static_cast<int>(wcslen(labelBuf)), &labelSz);
        GetTextExtentPoint32W(hdc, L"全图", 2, &fullSz);
        SelectObject(hdc, old);
        ReleaseDC(hwnd_, hdc);
        const int labelW = std::max(ScaleX(kFindRegionLabelW),
            static_cast<int>(labelSz.cx) + 4);
        const int fullWWant = std::max(44, static_cast<int>(fullSz.cx) + ScaleX(18));
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
        const int fieldH = ScaleY(22);
        const int rowGap = ScaleY(kFindVGap);
        const int labelW = ScaleX(labelWDesign);
        const int editW = ScaleX(kFindEditW);
        const int labelEditGap = ScaleX(kFindCoordLabelEditGap);
        const int pairGap = ScaleX(kFindCoordPairGap);
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
        return ScaleX(designPx);
    }

    static int AiScaleY(int designPx) {
        return ScaleY(designPx);
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
        HideAiFindRegionFloatingControls();
        const FindImageSideButtonLayout layout = ComputeFindImageSideButtonLayout();
        const int fieldH = OcrScale(22);
        const int rowGap = OcrScale(kFindVGap);

        MoveAiRegionAt(aiFindImagePreviewBtn_, kFindContentLeft, y, kFindImageSize, kFindImageSize);
        LayoutAiFindImageSideStack(y, aiFindScreenshotBtn_, aiFindLocalImageBtn_, aiFindClearImageBtn_);
        if (aiFindImagePreviewBtn_) {
            SetWindowPos(aiFindImagePreviewBtn_, HWND_BOTTOM, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
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

    int LayoutAiCommonStack(int y, int sel) {
        const int left = ParamPanelLeft();
        const int fullW = ParamFieldMaxWidth();
        const int rowGap = ScaleY(8);
        const int labelGap = ScaleY(6);
        const int fieldH = ScaleY(22);
        const int comboH = ScaleY(21);
        const int btnH = ScaleY(28);
        const int textFieldH = ScaleY(EditorParamLayout::kPanelTextFieldH);
        const int hintH = ScaleY(28);
        const int blockGap = ScaleY(12);
        const bool actionExecute = sel == 31;

        const int promptLabelH = ScaleY(kEditorLabelAboveComboH);
        if (actionExecute && aiWithImageCheck_) {
            const int checkW = AiScaleX(52);
            const int sideGap = AiScaleX(8);
            const int labelW = std::max(AiScaleX(80), fullW - checkW - sideGap);
            MoveAiAtVisible(aiPromptLabel_, left, y, labelW, promptLabelH);
            MoveAiAtVisible(aiWithImageCheck_, left + labelW + sideGap, y, checkW, AiScaleY(25));
        } else {
            if (aiWithImageCheck_) ParkParamControl(aiWithImageCheck_);
            MoveAiAtVisible(aiPromptLabel_, left, y, fullW, promptLabelH);
        }
        y += promptLabelH + labelGap;

        MoveAiAtVisible(aiPromptEdit_, left, y, fullW, textFieldH);
        y += textFieldH + rowGap;

        if (actionExecute) {
            MoveAiAtVisible(aiMaxStepsLabel_, left, y, fullW, fieldH);
            y += fieldH + labelGap;
            MoveAiAtVisible(aiMaxStepsEdit_, left, y, fullW, fieldH);
            y += fieldH + ScaleY(4);
            MoveAiAtVisible(aiMaxStepsHint_, left, y, fullW, hintH);
            y += hintH + rowGap;
        } else {
            for (HWND h : {aiMaxStepsLabel_, aiMaxStepsEdit_, aiMaxStepsHint_}) ParkParamControl(h);
        }

        MoveAiAtVisible(aiVarLabel_, left, y, fullW, fieldH);
        y += fieldH + labelGap;
        MoveAiAtVisible(aiVarCombo_, left, y, fullW, comboH);
        y += comboH + rowGap;
        MoveAiAtVisible(aiInsertVarBtn_, left, y, fullW, btnH);
        y += btnH + blockGap;

        MoveAiAtVisible(aiModelLabel_, left, y, fullW, fieldH);
        y += fieldH + labelGap;
        MoveAiAtVisible(aiModelCombo_, left, y, fullW, comboH);
        y += comboH + rowGap;

        MoveAiAtVisible(aiContextLabel_, left, y, fullW, fieldH);
        y += fieldH + labelGap;
        MoveAiAtVisible(aiContextModeCombo_, left, y, fullW, comboH);
        y += comboH + rowGap;

        MoveAiAtVisible(aiTimeoutLabel_, left, y, fullW, fieldH);
        y += fieldH + labelGap;
        MoveAiAtVisible(aiTimeoutEdit_, left, y, fullW, fieldH);
        y += fieldH + rowGap;

        MoveAiAtVisible(aiFallbackLabel_, left, y, fullW, fieldH);
        y += fieldH + labelGap;
        MoveAiAtVisible(aiFallbackEdit_, left, y, fullW, fieldH);
        y += fieldH + rowGap;

        if (!actionExecute) {
            MoveAiAtVisible(aiOutputVarLabel_, left, y, fullW, fieldH);
            y += fieldH + labelGap;
            MoveAiAtVisible(aiOutputVarEdit_, left, y, fullW, fieldH);
            y += fieldH + rowGap;
            MoveAiAtVisible(aiOutputTypeLabel_, left, y, fullW, fieldH);
            y += fieldH + labelGap;
            MoveAiAtVisible(aiOutputTypeCombo_, left, y, fullW, comboH);
            y += comboH;
        } else {
            for (HWND h : {aiOutputVarLabel_, aiOutputVarEdit_, aiOutputTypeLabel_, aiOutputTypeCombo_}) {
                if (h) ParkParamControl(h);
            }
        }
        return y;
    }

    int LayoutAiImageFields(int y) {
        const int left = ParamPanelLeft();
        const int fullW = ParamFieldMaxWidth();
        const int rowGap = ScaleY(8);
        const int labelGap = ScaleY(6);
        const int fieldH = ScaleY(22);

        MoveAiAtVisible(aiImageScaleLabel_, left, y, fullW, fieldH);
        y += fieldH + labelGap;
        MoveAiAtVisible(aiImageScaleEdit_, left, y, fullW, fieldH);
        y += fieldH + rowGap;
        return y;
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
            MoveParamAware(regionByImageCheck, ParamPanelLeft(), y, ParamFieldMaxWidth(), fieldH, FALSE);
            ShowWindow(regionByImageCheck, SW_SHOW);
            y += fieldH + rowGap;
        }

        if (regionByImage) {
            ParkParamControl(regionLabel);
            ParkParamControl(fullBtn);
            ParkParamControl(selectBtn);
            HideAiFindRegionFloatingControls();

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
            RaiseAiFindImageHeaderControls();
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
        ParkParamGroup(aiImageControls_);
        ParkParamGroup(aiActionControls_);

        const int sectionGap = ScaleY(12);
        const int rowGap = ScaleY(8);
        int y = ParamPanelContentTopY();
        y = LayoutAiCommonStack(y, sel);

        if (imageMode) {
            y += sectionGap;
            y = LayoutAiImageFields(y);
            y = LayoutAiRegionSection(
                regionByImage,
                aiRegionByImageCheck_,
                aiRegionLabel_, aiFullScreenBtn_, aiSelectRegionBtn_,
                aiCoordX1Label_, aiSearchX1Edit_, aiCoordY1Label_, aiSearchY1Edit_,
                aiCoordX2Label_, aiSearchX2Edit_, aiCoordY2Label_, aiSearchY2Edit_,
                y);
        } else if (actionExecuteMode) {
            y += sectionGap;
            if (withImage) {
                y = LayoutAiRegionSection(
                    regionByImage,
                    aiRegionByImageCheck2_,
                    aiActionRegionLabel_, aiFullScreenBtn2_, aiSelectRegionBtn2_,
                    aiActCoordX1Label_, aiSearchX1Edit2_, aiActCoordY1Label_, aiSearchY1Edit2_,
                    aiActCoordX2Label_, aiSearchX2Edit2_, aiActCoordY2Label_, aiSearchY2Edit2_,
                    y);
            } else {
                HideAiActionExecuteRegionControls();
            }
            if (aiConfirmExecute_) {
                y += rowGap;
                MoveAiAtVisible(aiConfirmExecute_, ParamPanelLeft(), y, AiScaleX(200), AiScaleY(25));
                y += AiScaleY(25);
            }
        }

        if (regionByImage && (imageMode || withImage)) RefreshGrayButtonsInParamViewport();
        paramScrollY_ = 0;
        SyncParamScrollLayout(y);
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
        HideOcrFindImageFloatingControls();
        const FindImageSideButtonLayout layout = ComputeFindImageSideButtonLayout();
        const int fieldH = OcrScaleY(22);
        const int rowGap = OcrScaleY(kFindVGap);
        auto show = [](HWND h) {
            if (!h) return;
            ShowWindow(h, SW_SHOW);
            SetWindowRgn(h, nullptr, TRUE);
        };
        for (HWND h : {ocrFindImagePreviewBtn_, ocrFindScreenshotBtn_, ocrFindLocalImageBtn_, ocrFindClearImageBtn_}) {
            if (h && IsWindowVisible(h)) InvalidateParamControlInViewport(h);
        }
        MoveOcrAt(ocrFindImagePreviewBtn_, kFindContentLeft, y, kFindImageSize, kFindImageSize);
        show(ocrFindImagePreviewBtn_);
        LayoutOcrFindImageSideStack(y, ocrFindScreenshotBtn_, ocrFindLocalImageBtn_, ocrFindClearImageBtn_);
        show(ocrFindScreenshotBtn_);
        show(ocrFindLocalImageBtn_);
        show(ocrFindClearImageBtn_);
        y += kFindImageSize + layout.sideGap;

        MoveOcrAt(ocrFindMatchThreshold_, kFindContentLeft + 91, y, 40, 22);
        show(ocrFindMatchThreshold_);
        for (HWND h : ocrFindRegionControls_) {
            if (!h) continue;
            wchar_t buf[16]{};
            GetWindowTextW(h, buf, 16);
            if (wcscmp(buf, L"匹配度大于") == 0) {
                MoveOcrAt(h, kFindContentLeft, y, 90, 22);
                show(h);
            } else if (wcscmp(buf, L"%") == 0) {
                MoveOcrAt(h, kFindContentLeft + 135, y, 24, 22);
                show(h);
            }
        }
        y += fieldH + rowGap;

        MoveOcrAt(ocrFindScaleMin_, kFindContentLeft + 65, y, 40, 22);
        MoveOcrAt(ocrFindScaleMax_, kFindContentLeft + 151, y, 40, 22);
        show(ocrFindScaleMin_);
        show(ocrFindScaleMax_);
        for (HWND h : ocrFindRegionControls_) {
            if (!h) continue;
            wchar_t buf[16]{};
            GetWindowTextW(h, buf, 16);
            if (wcscmp(buf, L"最小缩放") == 0) {
                MoveOcrAt(h, kFindContentLeft, y, 64, 22);
                show(h);
            } else if (wcscmp(buf, L"最大") == 0) {
                MoveOcrAt(h, kFindContentLeft + 110, y, 40, 22);
                show(h);
            }
        }
        return y + fieldH + rowGap;
    }

    void HideOcrDynamicRegionControls() const {
        for (HWND h : {ocrRegionLabel_, ocrFullScreenBtn_, ocrSelectRegionBtn_,
                       ocrFindSelectRegionBtn_, ocrFindImageLabel_}) {
            if (!h) continue;
            if (IsWindowVisible(h)) InvalidateParamControlInViewport(h);
            ShowWindow(h, SW_HIDE);
        }
        for (HWND h : ocrFindRegionControls_) {
            if (!h) continue;
            if (IsWindowVisible(h)) InvalidateParamControlInViewport(h);
            ShowWindow(h, SW_HIDE);
        }
    }

    void HideFindImageFloatingControls() const {
        for (HWND h : {findTestBtn_, findImagePreviewBtn_, findScreenshotBtn_,
                       findLocalImageBtn_, findClearImageBtn_}) {
            if (!h) continue;
            if (IsWindowVisible(h)) InvalidateParamControlInViewport(h);
            ShowWindow(h, SW_HIDE);
        }
    }

    void HideAiFindRegionFloatingControls() const {
        for (HWND h : {aiFindImagePreviewBtn_, aiFindScreenshotBtn_,
                       aiFindLocalImageBtn_, aiFindClearImageBtn_}) {
            if (!h) continue;
            if (IsWindowVisible(h)) InvalidateParamControlInViewport(h);
            ShowWindow(h, SW_HIDE);
        }
    }

    void HideOcrFindImageFloatingControls() const {
        for (HWND h : {ocrFindImagePreviewBtn_, ocrFindScreenshotBtn_,
                       ocrFindLocalImageBtn_, ocrFindClearImageBtn_}) {
            if (!h) continue;
            if (IsWindowVisible(h)) InvalidateParamControlInViewport(h);
            ShowWindow(h, SW_HIDE);
        }
    }

    int LayoutOcrFindImageHeaderRow(int y) {
        const FindImageSideButtonLayout sideLayout = ComputeFindImageSideButtonLayout();
        const int headerRowY = y;
        const int headerLabelH = OcrScale(kFindBtnH);
        if (ocrFindImageLabel_) {
            MoveOcrAt(ocrFindImageLabel_, kFindContentLeft,
                headerRowY + std::max(0, (sideLayout.compactBtnH - headerLabelH) / 2),
                90, kFindBtnH);
            ShowWindow(ocrFindImageLabel_, SW_SHOW);
            SetWindowPos(ocrFindImageLabel_, HWND_TOP, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        PlaceOcrFindImageCompactButton(ocrFindSelectRegionBtn_, headerRowY);
        if (ocrFindSelectRegionBtn_) {
            ShowWindow(ocrFindSelectRegionBtn_, SW_SHOW);
            SetWindowPos(ocrFindSelectRegionBtn_, HWND_TOP, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return y + sideLayout.compactBtnH + sideLayout.sideGap;
    }

    int LayoutOcrRegionModeRow(int y) {
        SizeOcrRegionButtonsAt(y);
        if (ocrRegionLabel_) ShowWindow(ocrRegionLabel_, SW_SHOW);
        if (ocrFullScreenBtn_) ShowWindow(ocrFullScreenBtn_, SW_SHOW);
        if (ocrSelectRegionBtn_) ShowWindow(ocrSelectRegionBtn_, SW_SHOW);
        return y + OcrScaleY(kFindBtnH) + OcrScaleY(kFindVGap);
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
        HideOcrDynamicRegionControls();
        const bool searchMode = popupOcrResultMode_.sel == 1;
        const bool saveVar = popupOcrFollowUp_.sel == 2;
        const bool regionByImage = ocrRegionByImageCheck_ && Checked(ocrRegionByImageCheck_);
        if (searchMode) {
            for (HWND h : ocrSearchControls_) {
                if (h) ShowWindow(h, SW_SHOW);
            }
        } else {
            ParkParamGroup(ocrSearchControls_);
        }
        if (saveVar) {
            ParkParamGroup(ocrFollowOffsetControls_);
            for (HWND h : ocrFollowVarControls_) {
                if (h) ShowWindow(h, SW_SHOW);
            }
        } else {
            ParkParamGroup(ocrFollowVarControls_);
            for (HWND h : ocrFollowOffsetControls_) {
                if (h) ShowWindow(h, SW_SHOW);
            }
        }
        // 布局 186 仅用于创建控件；显示/定位由下方动态堆叠负责，避免静态坐标与复选框行重叠
        ShowParamLayout(186, false);
        // ShowParamLayout(18) 会把识别区域/找图控件短暂显示在静态坐标，须再次隐藏后再动态堆叠
        HideOcrDynamicRegionControls();
        if (ocrTestBtn_) ShowWindow(ocrTestBtn_, SW_HIDE);
        if (editorPopupOpen_ == 18) CloseEditorPopup();
        if (ocrUntilFound_) ShowWindow(ocrUntilFound_, searchMode ? SW_SHOW : SW_HIDE);

        const int rowGap = OcrScaleY(kFindVGap);
        const int btnH = OcrScaleY(kFindBtnH);
        const int fieldH = OcrScaleY(22);

        int y = OcrContentStartY();
        if (ocrRegionByImageCheck_) {
            MoveOcrAt(ocrRegionByImageCheck_, kFindContentLeft, y, kFindBlockW, fieldH);
            ShowWindow(ocrRegionByImageCheck_, SW_SHOW);
            y += fieldH + rowGap;
        }
        if (ocrDigitsOnlyCheck_) {
            MoveOcrAt(ocrDigitsOnlyCheck_, kFindContentLeft, y, kFindBlockW, fieldH);
            ShowWindow(ocrDigitsOnlyCheck_, SW_SHOW);
            y += fieldH + rowGap;
        }

        if (regionByImage) {
            y += rowGap;
            y = LayoutOcrFindImageHeaderRow(y);
            y = LayoutOcrFindRegionBlock(y);
            if (ocrFindImagePreviewBtn_) {
                SetWindowPos(ocrFindImagePreviewBtn_, HWND_BOTTOM, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            ShowFindImageSideControls(
                ocrFindImagePreviewBtn_, ocrFindScreenshotBtn_, ocrFindLocalImageBtn_, ocrFindClearImageBtn_);
            RaiseOcrFindImageHeaderControls();
            RefreshGrayButtonsInParamViewport();
        } else {
            y = LayoutOcrRegionModeRow(y);
            RaiseOcrRegionButtons();
            RefreshGrayButtonsInParamViewport();
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
        if (regionByImage && paramViewport_) InvalidateRect(paramViewport_, nullptr, TRUE);
        paramScrollY_ = 0;
        SyncParamScrollLayout(y);
    }

    void InvalidateOcrEditorPanel() {
        InvalidateParamScrollArea();
    }

    int OcrDepRowY() const {
        return ParamPanelContentTopY();
    }

    int OcrContentStartY() const {
        const int depH = ScaleY(kFindBtnH);
        const int depGap = ScaleY(kOcrDepToRegionGap);
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
        const int left = ScaleX(kFindContentLeft);
        const int panelRight = ParamScrollContentRight();
        const int maxW = std::max(1, panelRight - left - ScaleX(4));
        const int gap = ScaleX(8);
        const int depH = ScaleY(kFindBtnH);
        HDC hdc = GetDC(hwnd_);
        const wchar_t* btnText = ready ? L"修复/更新" : L"一键安装";
        const int btnWDesign = OcrTextButtonWidthDesign(hdc, editorFont_ ? editorFont_ : font_, btnText, kFindBtnW, 16);
        ReleaseDC(hwnd_, hdc);
        const int btnW = std::min(OcrScaleX(btnWDesign), maxW - ScaleX(120) - gap);
        const int labelW = std::max(ScaleX(120), maxW - btnW - gap);
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
            ShowPromptInfo(L"请先设置要查找的图片。");
            return;
        }

        HideEditorForScreenCapture();

        int sx1 = ToInt(ocrX1_), sy1 = ToInt(ocrY1_), sx2 = ToInt(ocrX2_), sy2 = ToInt(ocrY2_);
        if (regionByImage) {
            if (!ResolveOcrAbsRegionFromFindImage(sx1, sy1, sx2, sy2, sx1, sy1, sx2, sy2)) {
                RestoreEditorAfterScreenCapture();
                ocrTestRunning_ = false;
                if (ocrTestBtn_) EnableWindow(ocrTestBtn_, TRUE);
                ShowPromptInfo(L"未找到参考图片，无法测试识别。");
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

    bool ValidateDefineBlockName(const std::wstring& name, int excludeIndex = -1) {
        if (!IsValidBlockName(name)) {
            ShowPromptInfo(L"块名称只能以字母开始，后面只能包含字母和数字。");
            return false;
        }
        if (IsBlockNameDuplicate(name, excludeIndex)) {
            ShowPromptInfo(L"块名称不能重复。");
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
        const int addedSel = popupAction_.sel;
        // 添加过程不选中新动作，避免 UpdateEditMode 瞬间露出「修改」按钮
        InsertAction(pos, action, indentOverride, /*selectInserted=*/false);
        selectedIndex_ = -1;
        actionFormDrafts_.erase(addedSel);
        loadingForm_ = true;
        LoadForm(DefaultActionForPopupSel(addedSel));
        SetText(remark_, L"");
        loadingForm_ = false;
        UpdateEditMode();
        if (action.type == ActionType::DefineBlock || action.type == ActionType::RunBlock) RefreshRunBlockCombo();
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
        case ActionType::Goto: return 33;
        case ActionType::MoveMouseRelative: return 34;
        default: return 14;
        }
    }

    bool IsImplementedActionPopup(int idx) const {
        return idx == 0 || idx == 1 || idx == 2 || idx == 3 || idx == 4 || idx == 5 || idx == 6 || idx == 7 || idx == 8 || idx == 9 || idx == 10
            || idx == 11 || idx == 12
            || idx == 13 || idx == 14 || idx == 15 || idx == 16 || idx == 17 || idx == 18 || idx == 19
            || idx == 20 || idx == 21 || idx == 22 || idx == 23 || idx == 24
            || idx == 25 || idx == 26 || idx == 27 || idx == 28
            || idx == 29 || idx == 30 || idx == 31 || idx == 32 || idx == 33 || idx == 34;
    }

    void UpdateEditMode() {
        const bool selected = ShouldShowModifyButton();
        if (selected) LoadForm(actions_[static_cast<size_t>(selectedIndex_)]);
        if (batchEditMode_) SyncBatchSelectedSize();
        scrollOffset_ = std::clamp(scrollOffset_, 0, MaxEditorScroll());
        RefreshActionListLayer();
        ApplyEditorFooterLayout();
    }

    void ClearEditorActions() {
        actions_.clear();
        collapsedContainers_.clear();
        selectedIndex_ = -1;
        if (batchEditMode_) batchSelected_.clear();
        if (!loadingForm_) RestoreActionFormDraftForCurrentType();
        UpdateBatchToolbar();
        UpdateEditMode();
        OnActionsChanged();
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

    RECT ToolbarAreaRect() const {
        RECT rc{};
        bool has = false;
        for (HWND h : {loadBtn_, clearBtn_, batchExitBtn_, batchSelectAllBtn_, batchDeselectBtn_,
                       batchDeleteBtn_, batchCopyBtn_, labelBatchCount_, labelList_}) {
            if (!h) continue;
            const RECT hr = WindowClientRect(h);
            if (hr.right <= hr.left || hr.bottom <= hr.top) continue;
            if (!has) {
                rc = hr;
                has = true;
            } else {
                rc.left = std::min(rc.left, hr.left);
                rc.top = std::min(rc.top, hr.top);
                rc.right = std::max(rc.right, hr.right);
                rc.bottom = std::max(rc.bottom, hr.bottom);
            }
        }
        if (!has) {
            return RECT{
                0,
                ScaleY(90),
                ScaleX(780),
                ScaleY(132)
            };
        }
        InflateRect(&rc, 4, 4);
        return rc;
    }

    void InvalidateToolbarArea() {
        if (!hwnd_) return;
        const RECT toolbarRc = ToolbarAreaRect();
        InvalidateRect(hwnd_, &toolbarRc, TRUE);
    }

    void RedrawEditorSurface() {
        if (!hwnd_ || page_ != Page::Editor) return;
        InvalidateToolbarArea();
        const RECT listRc = ActionListRect();
        InvalidateRect(hwnd_, &listRc, TRUE);
        RefreshActionListLayer();
        if (UsesDynamicParamPanel()) {
            RefreshDynamicParamLayout();
        } else {
            ApplyEditorFooterLayout();
            SyncParamScrollLayout();
        }
        RepaintParamPanelChrome();
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        UpdateWindow(hwnd_);
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
        InvalidateToolbarArea();
    }

    void EnterBatchEditMode() {
        if (batchEditMode_) return;
        CommitInlineRemark();
        batchEditMode_ = true;
        const bool hadSelection = selectedIndex_ >= 0;
        selectedIndex_ = -1;
        hoverIndex_ = -1;
        if (hadSelection && !loadingForm_) RestoreActionFormDraftForCurrentType();
        batchSelected_.assign(actions_.size(), false);
        UpdateBatchToolbar();
        UpdateEditMode();
        RedrawEditorSurface();
    }

    void ExitBatchEditMode() {
        if (!batchEditMode_) return;
        batchEditMode_ = false;
        batchSelected_.clear();
        UpdateBatchToolbar();
        UpdateEditMode();
        RedrawEditorSurface();
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
        if (!loadingForm_) RestoreActionFormDraftForCurrentType();
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

    bool Checked(HWND h) const {
        if (IsMarkedParamCheckbox(h)) return IsParamCheckboxChecked(h);
        return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    void SetChecked(HWND h, bool checked, bool immediateRedraw = true) {
        if (IsMarkedParamCheckbox(h)) {
            SetParamCheckboxChecked(h, checked, immediateRedraw);
            return;
        }
        SendMessageW(h, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    void RedrawParamCheckbox(HWND hwnd) const {
        if (!hwnd) return;
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_FRAME);
    }

    void OnParamCheckboxClicked(HWND ctrl) {
        if (!ctrl) return;
        const int id = GetDlgCtrlID(ctrl);
        if (id == kOcrRegionByImage || ctrl == ocrRegionByImageCheck_) {
            if (popupAction_.sel == 18) RequestOcrSubPanelRefresh();
            else RebuildParamPanelLayout();
            return;
        }
        if (id == kAiRegionByImage || id == kAiRegionByImage2 || id == kAiWithImage
            || ctrl == aiRegionByImageCheck_ || ctrl == aiRegionByImageCheck2_
            || ctrl == aiWithImageCheck_) {
            RebuildParamPanelLayout();
        } else if (id == kMoveFromVar || id == kLoopFromVar
            || ctrl == moveFromVar_ || ctrl == loopFromVar_) {
            UpdateMoveVarControls();
            UpdateLoopVarControls();
        }
    }

    void ToggleParamCheckbox(HWND ctrl) {
        if (!ctrl) return;
        const int id = GetDlgCtrlID(ctrl);
        const bool lockVp = (id == EditorParamLayout::EID_OcrDigitsOnly && paramViewport_);
        if (lockVp) LockParamViewportRedraw();
        if (id == EditorParamLayout::EID_OcrDigitsOnly) RefreshOcrNeighborGrayButtons();
        SetChecked(ctrl, !Checked(ctrl));
        if (!IsMarkedParamCheckbox(ctrl)) RedrawParamCheckbox(ctrl);
        if (lockVp) UnlockParamViewportRedraw();
        OnParamCheckboxClicked(ctrl);
    }

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

    // 各动作类型各自一套「同时按住」控件；LoadForm 只写当前类型时，其它类型会残留勾选
    void WriteModifierHoldsQuiet(const ScriptAction& action, HWND lWin, HWND rWin, HWND lCtrl, HWND rCtrl,
        HWND lAlt, HWND rAlt, HWND lShift, HWND rShift) {
        SetChecked(lWin, action.holdLeftWin, false); SetChecked(rWin, action.holdRightWin, false);
        SetChecked(lCtrl, action.holdLeftCtrl, false); SetChecked(rCtrl, action.holdRightCtrl, false);
        SetChecked(lAlt, action.holdLeftAlt, false); SetChecked(rAlt, action.holdRightAlt, false);
        SetChecked(lShift, action.holdLeftShift, false); SetChecked(rShift, action.holdRightShift, false);
    }

    void ClearAllModifierHoldCheckboxes() {
        ScriptAction empty{};
        WriteModifierHoldsQuiet(empty, clickLWin_, clickRWin_, clickLCtrl_, clickRCtrl_,
            clickLAlt_, clickRAlt_, clickLShift_, clickRShift_);
        WriteModifierHoldsQuiet(empty, mousePressLWin_, mousePressRWin_, mousePressLCtrl_, mousePressRCtrl_,
            mousePressLAlt_, mousePressRAlt_, mousePressLShift_, mousePressRShift_);
        WriteModifierHoldsQuiet(empty, keyLWin_, keyRWin_, keyLCtrl_, keyRCtrl_,
            keyLAlt_, keyRAlt_, keyLShift_, keyRShift_);
        WriteModifierHoldsQuiet(empty, keyPressLWin_, keyPressRWin_, keyPressLCtrl_, keyPressRCtrl_,
            keyPressLAlt_, keyPressRAlt_, keyPressLShift_, keyPressRShift_);
    }

    ActionType ActionTypeForPopupSel(int sel) const {
        switch (sel) {
        case 0: return ActionType::MoveMouse;
        case 1: return ActionType::Wait;
        case 2: return ActionType::MouseClick;
        case 3: return ActionType::MousePlayback;
        case 4: return ActionType::RunMacro;
        case 5: return ActionType::MouseDown;
        case 6: return ActionType::MouseUp;
        case 7: return ActionType::ScrollWheel;
        case 8: return ActionType::KeyClick;
        case 9: return ActionType::KeyDown;
        case 10: return ActionType::KeyUp;
        case 11: return ActionType::HotkeyShortcut;
        case 12: return ActionType::QuickInput;
        case 13: return ActionType::Loop;
        case 14: return ActionType::EndLoop;
        case 15: return ActionType::DefineBlock;
        case 16: return ActionType::RunBlock;
        case 17: return ActionType::FindImage;
        case 18: return ActionType::TextRecognition;
        case 19: return ActionType::If;
        case 20: return ActionType::Else;
        case 21: return ActionType::LockScreenshot;
        case 22: return ActionType::UnlockScreenshot;
        case 23: return ActionType::StopMacro;
        case 24: return ActionType::RunProgram;
        case 25: return ActionType::CloseProgram;
        case 26: return ActionType::OpenWebpage;
        case 27: return ActionType::OpenFile;
        case 28: return ActionType::TimerRecordTime;
        case 29: return ActionType::AiTextAnalysis;
        case 30: return ActionType::AiImageAnalysis;
        case 31: return ActionType::AiActionExecute;
        case 32: return ActionType::GetCursorPos;
        case 33: return ActionType::Goto;
        case 34: return ActionType::MoveMouseRelative;
        default: return ActionType::EndLoop;
        }
    }

    ScriptAction DefaultActionForPopupSel(int sel) const {
        ScriptAction action;
        action.type = ActionTypeForPopupSel(sel);
        switch (sel) {
        case 0:
            action.moveVarExprX = L"0";
            action.moveVarExprY = L"0";
            break;
        case 34:
            action.x = 0;
            action.y = 0;
            break;
        case 1:
            action.duration = 0.5;
            break;
        case 2:
            action.duration = 0.01;
            break;
        case 3:
            action.clickCount = 1;
            action.duration = 0.01;
            break;
        case 7:
            action.scrollVertical = true;
            action.scrollHorizontal = false;
            action.scrollSteps = 1;
            action.scrollDirection = 0;
            action.clickCount = 1;
            action.duration = 0.01;
            break;
        case 8:
            action.keyText.clear();
            action.keyVk = 0;
            action.clickCount = 1;
            action.duration = 0.01;
            break;
        case 9:
        case 10:
            action.keyText.clear();
            action.keyVk = 0;
            break;
        case 11:
            action.clickCount = 1;
            action.duration = 0.01;
            break;
        case 12:
            action.charInterval = 0.01;
            action.clickCount = 1;
            action.duration = 0.01;
            break;
        case 14:
            action.customText = L"跳出循环";
            break;
        case 15:
            action.blockName = L"block1";
            break;
        case 17:
            action.searchFullScreen = true;
            action.matchThreshold = 65.0;
            action.imageScaleMin = 1.0;
            action.imageScaleMax = 1.0;
            action.findImageFollowUp = 0;
            action.findTimeExpr = L"0";
            action.matchVarName = L"matchRet";
            break;
        case 18:
            action.searchFullScreen = true;
            action.ocrResultMode = 0;
            action.ocrFollowUp = 0;
            action.matchVarName = L"a";
            break;
        case 29:
            action.aiOutputVarName = L"aiResult";
            action.aiOutputType = 0;
            action.aiContextMode = 0;
            action.aiTimeoutSec = 30;
            break;
        case 30:
            action.aiOutputVarName = L"aiImgResult";
            action.aiOutputType = 0;
            action.aiContextMode = 0;
            action.aiTimeoutSec = 30;
            action.aiImageScale = 1.0;
            action.searchFullScreen = true;
            break;
        case 31:
            action.aiContextMode = 0;
            action.aiTimeoutSec = 30;
            action.aiImageScale = 0.5;
            action.aiMaxSteps = 10;
            action.aiConfirmExecute = false;
            action.searchFullScreen = true;
            break;
        default:
            break;
        }
        return action;
    }

    // ── 添加表单草稿（仅内存，从不写入脚本文件）────────────────────
    // 生命周期：
    //   · 存在：未点「添加」、未退出编辑时；切换动作类型，或选中/取消选中已添加动作
    //   · 清除：「添加」后清掉该类型草稿；退出编辑 / 打开编辑 ResetActionFormSession 整表清空
    // 脚本 JSON 里的 holdLeftWin 等是已添加动作的字段，不是草稿。
    void SaveCurrentActionFormDraft() {
        if (loadingForm_ || selectedIndex_ >= 0) return;
        actionFormDrafts_[popupAction_.sel] = ActionFromForm();
    }

    void RestoreActionFormDraftForCurrentType() {
        const int sel = popupAction_.sel;
        const auto it = actionFormDrafts_.find(sel);
        if (it != actionFormDrafts_.end()) {
            LoadForm(it->second);
            return;
        }
        LoadForm(DefaultActionForPopupSel(sel));
        // 无草稿：再清修饰键，避免取消选中列表项后勾选残留又被写进草稿
        ClearAllModifierHoldCheckboxes();
    }

    void ClearAllParamCheckboxStates() {
        // 批量静默清勾选，避免每个框 RDW_UPDATENOW 拖慢打开/切类型
        auto clearMarked = [this](HWND root) {
            if (!root) return;
            for (HWND child = GetWindow(root, GW_CHILD); child;
                 child = GetWindow(child, GW_HWNDNEXT)) {
                if (IsMarkedParamCheckbox(child)) SetChecked(child, false, false);
            }
        };
        clearMarked(paramViewport_);
        for (HWND h : editorControls_) {
            if (h && IsMarkedParamCheckbox(h)) SetChecked(h, false, false);
        }
    }

    void ResetEditorScriptChromeDefaults() {
        scriptWindowMode_ = {};
        popupMode_.sel = 0;
        SetPopupSel(popupMode_, mode_, 0);
        popupWmSelectMethod_.sel = 0;
        if (!popupWmSelectMethod_.items.empty()) {
            SetPopupSel(popupWmSelectMethod_, wmSelectMethod_, 0);
        }
        if (breakoutTimeEdit_) SetText(breakoutTimeEdit_, L"0");
        if (wmTargetPathEdit_) SetText(wmTargetPathEdit_, L"");
        SyncWindowModeUiFromScript();
    }

    void ResetEditorTransientFormState() {
        formKeyText_.clear();
        formKeyVk_ = 0;
        formKeyPressText_.clear();
        formKeyPressVk_ = 0;
        if (keyEdit_) SetText(keyEdit_, L"");
        if (keyPressEdit_) SetText(keyPressEdit_, L"");
        ClearAllModifierHoldCheckboxes();
        findImagePath_.clear();
        findImageFullScreen_ = true;
        ocrFindImagePath_.clear();
        ocrFullScreen_ = true;
        aiFindImagePath_.clear();
        if (findImagePreviewBitmap_) {
            DeleteBitmapHandle(findImagePreviewBitmap_);
            findImagePreviewBitmap_ = nullptr;
        }
        if (ocrFindImagePreviewBitmap_) {
            DeleteBitmapHandle(ocrFindImagePreviewBitmap_);
            ocrFindImagePreviewBitmap_ = nullptr;
        }
        if (aiFindImagePreviewBitmap_) {
            DeleteBitmapHandle(aiFindImagePreviewBitmap_);
            aiFindImagePreviewBitmap_ = nullptr;
        }
        ClearAllParamCheckboxStates();
    }

    // resetScriptChrome：退出编辑/新建宏时清模式·脱离时间·窗口模式；
    // 打开已有宏时为 false（脚本头已由 LoadScriptFile 写入）。
    void ResetActionFormSession(bool resetScriptChrome = true) {
        actionFormDrafts_.clear();
        if (resetScriptChrome) ResetEditorScriptChromeDefaults();
        ResetEditorTransientFormState();
        // 子下拉也复位，避免「右键/侧键」等留在上次状态
        if (!popupMouseBtn_.items.empty()) SetPopupSel(popupMouseBtn_, mousePressButton_, 0);
        if (!popupClickBtn_.items.empty()) SetPopupSel(popupClickBtn_, clickButton_, 0);
        if (!popupScrollDir_.items.empty()) SetPopupSel(popupScrollDir_, scrollDirectionCombo_, 0);
        if (!popupHotkeyShortcut_.items.empty()) SetPopupSel(popupHotkeyShortcut_, hotkeyShortcutCombo_, 0);
        if (!popupLoopType_.items.empty()) SetPopupSel(popupLoopType_, loopTypeCombo_, 0);
        loadingForm_ = true;
        SetPopupSel(popupAction_, actionCombo_, 0);
        SetText(remark_, L"");
        if (page_ == Page::Editor) {
            LoadForm(DefaultActionForPopupSel(0));
        } else {
            // 已回主页：勿 RefreshParamPanel（会把参数区 ShowWindow 叠到主页上）
            ClearAllModifierHoldCheckboxes();
            ClearAllParamCheckboxStates();
        }
        loadingForm_ = false;
    }

    void DiscardSpuriousEditorInput() {
        if (!hwnd_) return;
        MSG msg{};
        // Peek(hwnd) 不含子控件；下拉选类型后要把落在勾选框上的 UP 清掉
        while (PeekMessageW(&msg, nullptr, WM_MOUSEFIRST, WM_MOUSELAST, PM_NOREMOVE)) {
            const bool ours = msg.hwnd == hwnd_
                || (msg.hwnd && IsChild(hwnd_, msg.hwnd))
                || (editorDropPopup_ && msg.hwnd == editorDropPopup_);
            if (!ours) break;
            PeekMessageW(&msg, msg.hwnd, msg.message, msg.message, PM_REMOVE);
        }
    }

    void UncloakEditorAfterReady() {
        if (!hwnd_) return;
        // 仍 cloak：整客户区先画一帧再揭开（避免 ExcludeClip 空洞露出主页）
        editorFullClientBlit_ = true;
        RedrawWindow(hwnd_, nullptr, nullptr,
            RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
        editorFullClientBlit_ = false;
        SetWindowCloaked(hwnd_, false);
        BOOL disableTransitions = FALSE;
        DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED,
            &disableTransitions, sizeof(disableTransitions));
        DiscardSpuriousEditorInput();
        outerShadow_.Sync();
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
        else if (sel == 34) {
            action.type = ActionType::MoveMouseRelative;
            action.x = ToInt(moveRelX_);
            action.y = ToInt(moveRelY_);
            action.randomX = std::max(0, ToInt(moveRelRandomX_));
            action.randomY = std::max(0, ToInt(moveRelRandomY_));
            action.coordsAreNormalized = false;
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
        else if (sel == 8) { action.type = ActionType::KeyClick; action.keyText = formKeyText_; action.keyVk = formKeyVk_; action.clickCount = std::max(1, ToInt(keyCount_, 1)); action.duration = std::max(0.0, ToDouble(keyWait_, 0.01)); action.randomDuration = std::max(0.0, ToDouble(keyRandom_)); ReadModifierHolds(action, keyLWin_, keyRWin_, keyLCtrl_, keyRCtrl_, keyLAlt_, keyRAlt_, keyLShift_, keyRShift_); }
        else if (sel == 9) { action.type = ActionType::KeyDown; action.keyText = formKeyPressText_; action.keyVk = formKeyPressVk_; ReadModifierHolds(action, keyPressLWin_, keyPressRWin_, keyPressLCtrl_, keyPressRCtrl_, keyPressLAlt_, keyPressRAlt_, keyPressLShift_, keyPressRShift_); }
        else if (sel == 10) { action.type = ActionType::KeyUp; action.keyText = formKeyPressText_; action.keyVk = formKeyPressVk_; ReadModifierHolds(action, keyPressLWin_, keyPressRWin_, keyPressLCtrl_, keyPressRCtrl_, keyPressLAlt_, keyPressRAlt_, keyPressLShift_, keyPressRShift_); }
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
            action.findTimeExpr = action.findImageFollowUp == 2 ? L"0" : GetText(findTimeEdit_);
            if (action.findTimeExpr.empty()) action.findTimeExpr = L"0";
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
        else if (sel == 33) {
            action.type = ActionType::Goto;
            action.gotoStepExpr = Trim(GetText(gotoStepEdit_));
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
        // 先清空全部勾选/修饰键，再写入当前类型，避免跨类型与退出后残留
        ClearAllModifierHoldCheckboxes();
        ClearAllParamCheckboxStates();
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
        else if (action.type == ActionType::MoveMouseRelative) {
            SetPopupSel(popupAction_, actionCombo_, 34);
            SetText(moveRelX_, std::to_wstring(action.x));
            SetText(moveRelY_, std::to_wstring(action.y));
            SetText(moveRelRandomX_, std::to_wstring(action.randomX));
            SetText(moveRelRandomY_, std::to_wstring(action.randomY));
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
        else if (action.type == ActionType::KeyDown || action.type == ActionType::KeyUp) { SetPopupSel(popupAction_, actionCombo_, ComboSelForType(action.type)); formKeyPressText_ = action.keyText; formKeyPressVk_ = action.keyVk; SetText(keyPressEdit_, formKeyPressText_); WriteModifierHolds(action, keyPressLWin_, keyPressRWin_, keyPressLCtrl_, keyPressRCtrl_, keyPressLAlt_, keyPressRAlt_, keyPressLShift_, keyPressRShift_); }
        else if (action.type == ActionType::KeyClick) { SetPopupSel(popupAction_, actionCombo_, 8); formKeyText_ = action.keyText; formKeyVk_ = action.keyVk; SetText(keyEdit_, formKeyText_); SetText(keyCount_, std::to_wstring(action.clickCount)); SetText(keyWait_, F3(action.duration)); SetText(keyRandom_, F3(action.randomDuration)); WriteModifierHolds(action, keyLWin_, keyRWin_, keyLCtrl_, keyRCtrl_, keyLAlt_, keyRAlt_, keyLShift_, keyRShift_); }
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
            SetText(findTimeEdit_, action.findTimeExpr.empty() ? L"0" : action.findTimeExpr);
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
        else if (action.type == ActionType::Goto) {
            SetPopupSel(popupAction_, actionCombo_, 33);
            SetText(gotoStepEdit_, action.gotoStepExpr);
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
        RECT clientRc = WindowClientRect(hwnd_);
        RECT panelRc{actionRc.left, actionRc.bottom + UiLen(4), clientRc.right,
            clientRc.bottom - UiLen(kBottomH)};
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
        const TemplateScale ts = ComputeTemplateScale(
            ScriptCoordMetaForExecution(loadedCoordMeta_), vsW, vsH);
        HBITMAP tmpl = LoadBitmapFromFile(ocrFindImagePath_);
        if (!tmpl) return false;
        ScriptAction probe{};
        probe.matchThreshold = threshold;
        probe.imageScaleMin = scaleMin;
        probe.imageScaleMax = scaleMax;
        ImageMatchOptions opt = BuildExecutionFindImageOptions(probe, ts);
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
            int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
            GetVirtualScreenRect(vsX, vsY, vsW, vsH);
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
            ShowPromptInfo(L"请先设置要查找的图片。");
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
                ShowPromptInfo(L"请先输入要在结果中查找的文字。");
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
            ShowPromptInfo(L"请先设置要查找的图片。");
            return;
        }

        HideEditorForScreenCapture();

        int sx1 = ToInt(ocrX1_), sy1 = ToInt(ocrY1_), sx2 = ToInt(ocrX2_), sy2 = ToInt(ocrY2_);
        if (regionByImage) {
            if (!ResolveOcrAbsRegionFromFindImage(sx1, sy1, sx2, sy2, sx1, sy1, sx2, sy2)) {
                RestoreEditorAfterScreenCapture();
                ShowPromptInfo(L"未找到参考图片，无法选择偏移位置。");
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
            ShowPromptInfo(L"请先设置要查找的图片。");
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
            int cx = 0, cy = 0;
            FindImageMatchCenter(matchOverlay_->matchResult_, cx, cy);
            const int offsetX = result.offsetX - cx;
            const int offsetY = result.offsetY - cy;
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
            ShowPromptInfo(L"请先设置要查找的图片。");
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
        if (findTestRunning_.exchange(true)) return;
        auto finishTest = [&]() {
            findTestRunning_ = false;
            if (findTestBtn_) {
                EnableWindow(findTestBtn_, TRUE);
                FinishGrayButtonClick(findTestBtn_);
            }
        };
        // 无图时在此直接返回，不调用 MatchOverlay / 屏幕冻结
        if (!HasFindImageTemplate(findImagePath_)) {
            finishTest();
            QueuePromptInfo(L"请先设置要查找的图片。");
            return;
        }
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

        ScriptAction probe{};
        probe.matchThreshold = threshold;
        probe.imageScaleMin = scaleMin;
        probe.imageScaleMax = scaleMax;
        const CoordMeta execMeta = ScriptCoordMetaForExecution(loadedCoordMeta_);
        int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
        GetVirtualScreenRect(vsX, vsY, vsW, vsH);
        const TemplateScale ts = ComputeTemplateScale(execMeta, vsW, vsH);
        const ImageMatchOptions findOpt = BuildExecutionFindImageOptions(probe, ts);

        matchOverlay_->Show(findImagePath_, sx1, sy1, sx2, sy2, findOpt,
                            MatchOverlayMode::Test);

        RestoreEditorAfterScreenOverlay();
        finishTest();
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
                // WM_LBUTTONUP 已在子类过程中复位；此处仅兜底键盘触发的 BN_CLICKED
                if (self && btn && notifyCode == BN_CLICKED && self->IsGrayButton(btn)
                    && GetFocus() == btn) {
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
        if (id == kClear) { ClearEditorActions(); return; }
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
        if (id == kFindTest) {
            FiDbgLogFmt(L"ON_COMMAND_FIND_TEST",
                L"ctrl=%p parent=%p chain=%s",
                ctrl, ctrl ? GetParent(ctrl) : nullptr,
                FiDbgWindowChain(ctrl).c_str());
            TestFindImage();
            return;
        }
        if (id == kFindSelectOffset) { BeginFindOffsetSelect(); return; }
        if (id == kOcrFullScreen) { ApplyOcrFullScreen(); return; }
        if (id == kOcrSelectRegion) { BeginOcrRegionSelect(); return; }
        if (id == kOcrSelectOffset) { BeginOcrOffsetSelect(); return; }
        if (id == kOcrTest) { TestOcr(); return; }
        if (id == kOcrInstallDep) { ShowOcrInstallDialog(); return; }
        if (id == kOcrFindSelectRegion) { BeginOcrFindRegionSelect(); return; }
        if (id == kOcrFindScreenshot) { BeginOcrFindScreenshot(); return; }
        if (id == kOcrFindLocalImage) { LoadOcrFindImageFromFile(); return; }
        if (id == kOcrFindImagePreview) { LoadOcrFindImageFromFile(); return; }
        if (id == kOcrFindClearImage) { ClearOcrFindImage(); return; }
        if (id == kAiInsertVar) { InsertAiPromptVariable(); return; }
        if (id == kAiSelectRegion) { BeginAiRegionSelect(); return; }
        if (id == kAiFullScreen) { ApplyAiFullScreen(); return; }
        if (id == kAiFindSelectRegion) { BeginAiFindRegionSelect(); return; }
        if (id == kAiTargetScreenshot) { BeginAiFindScreenshot(); return; }
        if (id == kAiTargetLocal) { LoadAiFindImageFromFile(); return; }
        if (id == kAiTargetPreview) { LoadAiFindImageFromFile(); return; }
        if (id == kAiTargetClear) { ClearAiFindImage(); return; }
        if (id == kListRemarkEdit && code == EN_KILLFOCUS) { CommitInlineRemark(); return; }
        if (id == kModify) { ModifySelected(); return; }
        if (id == kAdd) { ShowAddMenu(); return; }
        if (id >= kCopyLast && id <= kCopyAfterSelected) { CopyActionByMenu(id); return; }
        if (id >= kAddLast && id <= kAddAsChild) { AddActionByMenu(id); return; }
        if (id >= kHotCustom && id <= kHotSpace) { SetCommonHotkeyFromMenu(id); return; }
        if (id == kWmSpecifyWindowBtn && code == BN_CLICKED) { ShowWindowClassPickDialog(); return; }
        if (id == kWmTargetBrowse && code == BN_CLICKED) { BrowseProgramExecutable(wmTargetPathEdit_); return; }
    }

    void InsertAction(size_t pos, const ScriptAction& src, int indentOverride = -1, bool selectInserted = true) {
        ScriptAction action = src;
        action.originalNo = NextNo();
        if (indentOverride >= 0) action.indent = indentOverride;
        pos = std::min(pos, actions_.size());
        if (action.type == ActionType::EndLoop && !HasLoopParentAt(actions_, pos, action.indent)) {
            ShowPromptInfo(kEndLoopNeedsLoopParentMsg);
            return;
        }
        actions_.insert(actions_.begin() + static_cast<std::ptrdiff_t>(pos), action);
        if (selectInserted) {
            selectedIndex_ = static_cast<int>(pos);
        } else if (selectedIndex_ >= static_cast<int>(pos)) {
            ++selectedIndex_;
        }
        {
            std::set<int> updated;
            for (int idx : collapsedContainers_) {
                updated.insert(idx >= static_cast<int>(pos) ? idx + 1 : idx);
            }
            collapsedContainers_ = std::move(updated);
        }
        if (IsExpandableContainer(action.type)) collapsedContainers_.erase(static_cast<int>(pos));
        RenumberActions();
        if (selectInserted) {
            EnsureSelectedVisible();
            UpdateEditMode();
        } else {
            RefreshActionListLayer();
        }
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
        if (action.type == ActionType::EndLoop
            && !HasLoopParentAt(actions_, static_cast<size_t>(selectedIndex_), actions_[static_cast<size_t>(selectedIndex_)].indent)) {
            ShowPromptInfo(kEndLoopNeedsLoopParentMsg);
            return;
        }
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
        EnsureEditorFullyParsed();
        SyncFormIntoActionsBeforeRun();
        EnsureScriptsDir();
        if (currentPath_.empty()) {
            std::wstring scriptName = GetText(name_);
            if (Trim(scriptName).empty()) scriptName = L"未命名脚本";
            currentPath_ = ScriptsDir() + L"\\" + scriptName + L".json";
        }
        SaveScriptFile(currentPath_);
    }

    void SaveScriptFile(const std::wstring& path) {
        ScriptFileData data{};
        data.scriptName = GetText(name_);
        data.recordTime = currentRecordTime_.empty() ? NowText() : currentRecordTime_;
        data.durationSeconds = saveDurationSeconds_;
        if (IsRecordingScriptPath(path)) {
            data.recordingCaptureMode = currentRecordingCaptureMode_;
            data.inputTimingVersion = currentInputTimingVersion_;
        }
        data.hotkey = saveHotkeyOverride_.has_value() ? *saveHotkeyOverride_
            : (currentScriptIndex_ >= 0 && currentScriptIndex_ < static_cast<int>(scripts_.size())
                ? scripts_[static_cast<size_t>(currentScriptIndex_)].hotkey : Hotkey{0, 0, L"", false});
        SyncScriptWindowModeFromEditor();
        data.windowMode = scriptWindowMode_;
        if (IsRecordingScriptPath(path)) {
            data.windowMode = windowmode::DefaultWindowModeConfig();
            data.breakoutTimeSeconds = 0;
        } else {
            data.breakoutTimeSeconds = data.windowMode.enabled
                ? 0.0 : NormalizeBreakoutTimeSeconds(ParseBreakoutTimeFromEditor());
        }
        data.actions = actions_;
        data.coordsNormalized = false;

        CoordMeta pixelMeta = CaptureCurrentCoordMeta(
            data.windowMode.enabled ? &data.windowMode : nullptr);
        const CoordMeta storeMeta = BuildScriptCoordMetaForSave(pixelMeta);
        data.coordMeta = storeMeta;
        SyncNormFieldsFromPixels(data.actions, pixelMeta);

        if (!SaveScriptFileData(path, data)) {
            ShowPromptInfo(L"保存失败：无法写入文件，请检查磁盘空间和权限。");
            return;
        }
        newImagePaths_.clear();
        CleanOrphanImages();
        loadedCoordMeta_ = storeMeta;
        scriptWindowMode_ = data.windowMode;
    }

    ScriptAction ParseScriptActionBlock(const std::wstring& block, size_t fallbackNo,
        bool coordsNormalized = false) const {
        return ::ParseScriptActionBlock(block, fallbackNo, coordsNormalized);
    }

    std::vector<ScriptAction> ParseActionsFromContent(const std::wstring& content,
        bool coordsNormalized = false) const {
        std::vector<ScriptAction> parsed;
        const auto blocks = ExtractJsonActionBlocks(content);
        for (size_t i = 0; i < blocks.size(); ++i) {
            const auto type = ExtractString(blocks[i], L"type");
            if (!type.empty()) parsed.push_back(ParseScriptActionBlock(blocks[i], i, coordsNormalized));
        }
        return parsed;
    }

    std::vector<ScriptAction> ParseActionsFromFile(const std::wstring& path) const {
        if (path.empty()) return {};
        const auto content = ReadAll(path);
        const bool normalized = HasCoordMetaJson(content);
        std::vector<ScriptAction> actions = ParseActionsFromContent(content, normalized);
        if (normalized && !actions.empty()) {
            DenormalizeScriptToCurrentScreen(actions);
        }
        return actions;
    }

    void LoadScriptFile(const std::wstring& path) {
        ClearEditorProgressiveState();
        const ScriptFileData data = LoadScriptFileData(path, true);
        ApplyScriptFileDataToEditor(data);
    }

    void ClearEditorProgressiveState() {
        editorActionBlocks_.clear();
        editorActionParsed_.clear();
        editorParseCursor_ = 0;
        editorParsePending_ = false;
        editorLoadCoordsNormalized_ = false;
        editorLoadCoordMeta_ = {};
    }

    void ApplyScriptFileChrome(const std::wstring& content, const ScriptFileData* fullData) {
        if (fullData) {
            SetText(name_, fullData->scriptName);
            SetText(breakoutTimeEdit_, FormatBreakoutTimeForEditor(fullData->breakoutTimeSeconds));
            currentRecordTime_ = fullData->recordTime;
            currentRecordingCaptureMode_ = fullData->recordingCaptureMode;
            currentInputTimingVersion_ = fullData->inputTimingVersion;
            scriptWindowMode_ = fullData->windowMode;
            scriptWindowMode_.autoLaunchTarget =
                windowmode::ShouldAutoLaunchTarget(scriptWindowMode_);
            loadedCoordMeta_ = ScriptCoordMetaForExecution(fullData->coordMeta);
        } else {
            SetText(name_, ExtractString(content, L"scriptName"));
            SetText(breakoutTimeEdit_, FormatBreakoutTimeForEditor(
                NormalizeBreakoutTimeSeconds(ExtractNumber(content, L"breakoutTimeSeconds", 0))));
            currentRecordTime_ = ExtractString(content, L"recordTime");
            currentRecordingCaptureMode_ = static_cast<int>(
                ExtractNumber(content, L"recordingCaptureMode", -1));
            currentInputTimingVersion_ = std::max(0, static_cast<int>(
                ExtractNumber(content, L"inputTimingVersion", 0)));
            scriptWindowMode_ = windowmode::ParseWindowModeJson(content);
            scriptWindowMode_.autoLaunchTarget =
                windowmode::ShouldAutoLaunchTarget(scriptWindowMode_);
            if (HasCoordMetaJson(content)) {
                loadedCoordMeta_ = ScriptCoordMetaForExecution(ParseCoordMetaJson(content));
            } else {
                loadedCoordMeta_ = StandardScriptCoordMeta();
            }
        }
        popupMode_.sel = !scriptWindowMode_.enabled ? 0
            : (scriptWindowMode_.executionKind == windowmode::WindowModeExecutionKind::BackgroundWindow ? 2 : 1);
        SyncWindowModeUiFromScript();
    }

    void ApplyScriptFileDataToEditor(const ScriptFileData& data) {
        ApplyScriptFileChrome(L"", &data);
        actions_ = data.actions;
        if (!data.coordsNormalized && !actions_.empty()) {
            MigrateLegacyScriptToNormalized(actions_, StandardScriptCoordMeta());
        }
        NormalizeScriptActionList(actions_);
        collapsedContainers_.clear();
        MarkVisibleActionsDirty();
        RefreshRunBlockCombo();
        OnActionsChanged();
    }

    /// 打开编辑器：只解析首屏动作，其余分块继续（避免长脚本堵在揭开前）
    void LoadScriptFileProgressive(const std::wstring& path) {
        ClearEditorProgressiveState();
        const std::wstring content = ReadAll(path);
        ApplyScriptFileChrome(content, nullptr);

        editorLoadCoordsNormalized_ = HasCoordMetaJson(content);
        if (editorLoadCoordsNormalized_) {
            editorLoadCoordMeta_ = ParseCoordMetaJson(content);
            if (editorLoadCoordMeta_.refWidth <= 0 || editorLoadCoordMeta_.refHeight <= 0)
                editorLoadCoordMeta_ = StandardScriptCoordMeta();
        } else {
            editorLoadCoordMeta_ = StandardScriptCoordMeta();
        }

        editorActionBlocks_ = ExtractJsonActionBlocks(content);
        const int n = static_cast<int>(editorActionBlocks_.size());
        actions_.assign(static_cast<size_t>(n), ScriptAction{});
        editorActionParsed_.assign(static_cast<size_t>(n), 0);
        collapsedContainers_.clear();
        MarkVisibleActionsDirty();

        const int firstPage = std::max(VisibleActionRows() + 2, 24);
        EnsureEditorActionsParsed(0, firstPage);
        editorParseCursor_ = firstPage;
        editorParsePending_ = editorParseCursor_ < n;
        if (!editorParsePending_) {
            FinalizeEditorProgressiveParse();
        }
    }

    void EnsureEditorActionsParsed(int begin, int end) {
        if (actions_.empty()) return;
        begin = std::max(0, begin);
        end = std::min(end, static_cast<int>(actions_.size()));
        if (begin >= end) return;
        if (editorActionParsed_.size() != actions_.size()) {
            editorActionParsed_.assign(actions_.size(), 1);
            return;
        }
        int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
        GetVirtualScreenBounds(vsX, vsY, vsW, vsH);
        const CoordMeta denormMeta = StandardScriptCoordMeta();
        for (int i = begin; i < end; ++i) {
            if (editorActionParsed_[static_cast<size_t>(i)]) continue;
            if (i >= static_cast<int>(editorActionBlocks_.size())) {
                editorActionParsed_[static_cast<size_t>(i)] = 1;
                continue;
            }
            const auto& block = editorActionBlocks_[static_cast<size_t>(i)];
            if (ExtractString(block, L"type").empty()) {
                editorActionParsed_[static_cast<size_t>(i)] = 1;
                continue;
            }
            ScriptAction a = ParseScriptActionBlock(block, static_cast<size_t>(i), editorLoadCoordsNormalized_);
            if (!editorLoadCoordsNormalized_) {
                std::vector<ScriptAction> tmp{std::move(a)};
                MigrateLegacyScriptToNormalized(tmp, StandardScriptCoordMeta());
                a = std::move(tmp[0]);
            }
            DenormalizeActionCoords(a, denormMeta, vsW, vsH);
            actions_[static_cast<size_t>(i)] = std::move(a);
            editorActionParsed_[static_cast<size_t>(i)] = 1;
        }
    }

    void EnsureEditorFullyParsed() {
        if (!editorParsePending_ && editorActionBlocks_.empty()) return;
        EnsureEditorActionsParsed(0, static_cast<int>(actions_.size()));
        FinalizeEditorProgressiveParse();
    }

    void FinalizeEditorProgressiveParse() {
        editorActionBlocks_.clear();
        editorActionParsed_.assign(actions_.size(), 1);
        editorParsePending_ = false;
        editorParseCursor_ = static_cast<int>(actions_.size());
        NormalizeScriptActionList(actions_);
        MarkVisibleActionsDirty();
        if (page_ == Page::Editor) {
            RefreshRunBlockCombo();
            OnActionsChanged();
        }
    }

    void ContinueEditorProgressiveParse(int openGen) {
        if (openGen != editorOpenGeneration_ || page_ != Page::Editor) return;
        if (!editorParsePending_) return;
        constexpr int kChunk = 64;
        const int n = static_cast<int>(actions_.size());
        const int end = std::min(n, editorParseCursor_ + kChunk);
        EnsureEditorActionsParsed(editorParseCursor_, end);
        editorParseCursor_ = end;
        // 若用户已滚到未解析区，补一下当前视口
        if (page_ == Page::Editor) {
            const auto& vis = VisibleActionIndices();
            const int rows = VisibleActionRows();
            const int from = scrollOffset_;
            const int to = std::min(static_cast<int>(vis.size()), scrollOffset_ + rows + 1);
            for (int vi = from; vi < to; ++vi) {
                EnsureEditorActionsParsed(vis[static_cast<size_t>(vi)], vis[static_cast<size_t>(vi)] + 1);
            }
            RefreshActionListLayer();
        }
        if (editorParseCursor_ >= n) {
            FinalizeEditorProgressiveParse();
            if (page_ == Page::Editor) RefreshActionListLayer();
        } else {
            PostMessageW(hwnd_, WM_APP_EDITOR_PARSE_MORE, static_cast<WPARAM>(openGen), 0);
        }
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
        ScriptFileData data = ParseScriptContent(content);
        if (data.scriptName.empty()) {
            ShowPromptInfo(L"导入失败：文件格式不正确，未找到脚本名称。");
            return;
        }
        if (toRecordings) {
            data.windowMode = windowmode::DefaultWindowModeConfig();
            data.breakoutTimeSeconds = 0;
        }
        data.recordTime = NowText();
        EnsureScriptsDir();
        const auto baseDir = ImportTargetDir(toRecordings);
        std::wstring target = baseDir + L"\\" + SafeScriptFileName(data.scriptName) + L".json";
        int suffix = 1;
        while (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            target = baseDir + L"\\" + SafeScriptFileName(data.scriptName)
                + L"-" + std::to_wstring(suffix++) + L".json";
        }
        if (!SaveScriptFileData(target, data)) {
            ShowPromptInfo(L"导入失败：无法写入目标目录。");
            return;
        }
        FinishImport(toRecordings);
    }

    void ImportScriptFromZipFile(const std::wstring& zipPath, bool toRecordings) {
        // 先读取 ZIP 中的 JSON 内容
        std::string jsonUtf8 = ReadTextFromZip(zipPath, "script.json");
        if (jsonUtf8.empty()) {
            ShowPromptInfo(L"导入失败：ZIP 文件中未找到 script.json。");
            return;
        }
        const std::wstring content = FromUtf8(jsonUtf8);
        std::wstring name = ExtractString(content, L"scriptName");
        if (name.empty()) {
            ShowPromptInfo(L"导入失败：文件格式不正确，未找到脚本名称。");
            return;
        }

        // 解压 ZIP 到临时目录
        wchar_t tempDir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempDir);
        const std::wstring extractDir = std::wstring(tempDir) + L"qs_import_" + std::to_wstring(GetTickCount());
        int extracted = ExtractZipFile(zipPath, extractDir);
        if (extracted < 0) {
            ShowPromptInfo(L"导入失败：无法解压 ZIP 文件。");
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
        ScriptFileData importData = ParseScriptContent(modifiedContent);
        if (importData.scriptName.empty()) importData.scriptName = name;
        if (toRecordings) {
            importData.windowMode = windowmode::DefaultWindowModeConfig();
            importData.breakoutTimeSeconds = 0;
        }
        importData.recordTime = NowText();
        EnsureScriptsDir();
        const auto baseDir = ImportTargetDir(toRecordings);
        std::wstring target = baseDir + L"\\" + SafeScriptFileName(importData.scriptName) + L".json";
        int suffix = 1;
        while (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            target = baseDir + L"\\" + SafeScriptFileName(importData.scriptName)
                + L"-" + std::to_wstring(suffix++) + L".json";
        }
        if (!SaveScriptFileData(target, importData)) {
            ShowPromptInfo(L"导入失败：无法写入目标目录。");
            return;
        }
        FinishImport(toRecordings);
    }

    void ExportSelectedScript() {
        if (selectedScript_ < 0 || selectedScript_ >= static_cast<int>(scripts_.size())) {
            ShowPromptInfo(L"请选择要导出的宏");
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
            ShowPromptInfo(L"导出失败：无法写入目标文件。");
        } else {
            ShowPromptInfo(L"导出成功！");
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
                ShowPromptInfo(L"导出成功！图片已一同打包。");
            } else {
                const std::wstring msg = L"导出成功，但有 " + std::to_wstring(zipResult.skippedFiles.size())
                    + L" 张图片未找到已跳过。\n\n对方导入后需要重新截图或选择本地图片。";
                ShowPromptInfo(msg.c_str());
            }
        } else {
            ShowPromptInfo(L"导出失败：无法创建 ZIP 文件，请检查保存路径是否有写入权限。");
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
            homeCenterX - UiEditorWidth() / 2,
            homeCenterY - UiEditorHeight() / 2,
            homeCenterX + UiEditorWidth() / 2,
            homeCenterY + UiEditorHeight() / 2
        };
        return ClampToWorkArea(editorRc);
    }

    // ── Layout helpers / RECT getters ──────────────────────────────
    RECT HomeListRect() const { return UiRect4(kHomeCardX, kHomeListY, kHomeCardX + kHomeCardW, kHomeListBottom); }
    int HomeListHeight() const { RECT r = HomeListRect(); return r.bottom - r.top; }
    int HomeContentHeight() const { return static_cast<int>(scripts_.size()) * UiLen(kHomeCardStep) - UiLen(kHomeCardGap); }
    int MaxHomeScroll() const { return std::max(0, HomeContentHeight() - HomeListHeight()); }
    int AgentConvContentHeight() const { return static_cast<int>(agentConversations_.size()) * UiLen(kHomeCardStep) - UiLen(kHomeCardGap); }
    int MaxAgentConvScroll() const { return std::max(0, AgentConvContentHeight() - HomeListHeight()); }
    int ActiveHomeContentHeight() const {
        if (activeHomeTab_ == quickscript::MainTab::ScriptCustom)
            return std::max(AgentConvContentHeight(), 1);
        if (activeHomeTab_ == quickscript::MainTab::Recorder)
            return std::max(RecordingContentHeight(), 1);
        return std::max(HomeContentHeight(), 1);
    }
    int ActiveHomeListMaxScroll() const {
        if (activeHomeTab_ == quickscript::MainTab::ScriptCustom) return MaxAgentConvScroll();
        if (activeHomeTab_ == quickscript::MainTab::Recorder) return MaxRecordingScroll();
        return MaxHomeScroll();
    }
    void ClampHomeScroll() { homeScrollOffset_ = std::clamp(homeScrollOffset_, 0, MaxHomeScroll()); }
    void ClampAgentConvScroll() { homeScrollOffset_ = std::clamp(homeScrollOffset_, 0, MaxAgentConvScroll()); }
    RECT HomeCardRect(int i) const {
        const int cardX = UiLen(kHomeCardX);
        const int cardW = UiLen(kHomeCardW);
        const int cardH = UiLen(kHomeCardH);
        const int y = UiLen(kHomeListY) + i * UiLen(kHomeCardStep) - homeScrollOffset_;
        return RECT{cardX, y, cardX + cardW, y + cardH};
    }
    int RecordingContentHeight() const { return static_cast<int>(recordings_.size()) * UiLen(kHomeCardStep) - UiLen(kHomeCardGap); }
    int MaxRecordingScroll() const { return std::max(0, RecordingContentHeight() - HomeListHeight()); }
    void ClampRecordingScroll() { homeScrollOffset_ = std::clamp(homeScrollOffset_, 0, MaxRecordingScroll()); }
    RECT RecordingCardRect(int i) const { return HomeCardRect(i); }
    std::wstring RecordingHotkeyText(const ScriptMeta& rec) const { return rec.hotkey.enabled && !rec.hotkey.text.empty() ? rec.hotkey.text : L"设置热键"; }
    RECT RecordingHotkeyRect(int i) const {
        if (i < 0 || i >= static_cast<int>(recordings_.size())) return RECT{};
        RECT r = RecordingCardRect(i);
        const int left = r.left + UiLen(14);
        const int rightLimit = r.right - UiLen(96);
        const int hotW = TextWidth(RecordingHotkeyText(recordings_[static_cast<size_t>(i)]), hotFont_);
        const int nameW = TextWidth(recordings_[static_cast<size_t>(i)].name, homeFont_);
        const int hotLeft = std::min(left + nameW + UiLen(10), rightLimit - hotW);
        return RECT{hotLeft, r.top + UiLen(17), std::min(hotLeft + hotW, rightLimit), r.top + UiLen(48)};
    }
    RECT RecorderBannerKeyRect() const {
        // 与 PaintRecorderHome 黄条居中布局保持一致（命中热区跟着文案走）
        const RECT cr = CreateRect();
        const std::wstring prefix = L"按";
        std::wstring suffix;
        if (recording_) suffix = L"键停止 录制";
        else if (selectedRecording_ >= 0) suffix = L"键开始 回放";
        else suffix = L"键开始 录制";
        const std::wstring hotText = globalHotkey_.text.empty() ? L"F8" : globalHotkey_.text;
        const int hotW = std::max(UiLen(56), TextWidth(hotText, bigFont_) + UiLen(20));
        const int gap = UiLen(20);
        const int prefixW = TextWidth(prefix, bigFont_);
        const int suffixW = TextWidth(suffix, bigFont_);
        const int totalW = prefixW + gap + hotW + gap + suffixW;
        const int left = cr.left + (cr.right - cr.left - totalW) / 2 + prefixW + gap;
        return RECT{left, cr.top + UiLen(21), left + hotW, cr.bottom - UiLen(21)};
    }
    RECT HomeScrollTrackRect() const { RECT list = HomeListRect(); return RECT{list.right + UiLen(5), list.top, list.right + UiLen(5) + UiLen(kHomeScrollW), list.bottom}; }
    RECT HomeScrollThumbRect() const {
        RECT track = HomeScrollTrackRect();
        const int maxScroll = ActiveHomeListMaxScroll();
        if (maxScroll <= 0) return RECT{track.left, track.top, track.right, track.bottom};
        const int trackH = track.bottom - track.top;
        const int contentH = ActiveHomeContentHeight();
        const int thumbH = std::max(UiLen(46), trackH * HomeListHeight() / contentH);
        const int range = std::max(1, trackH - thumbH);
        const int top = track.top + range * homeScrollOffset_ / maxScroll;
        return RECT{track.left, top, track.right, top + thumbH};
    }
    RECT CreateRect() const { return UiRect4(35, 375, 683, 459); }
    RECT CreateWordRect() const {
        RECT cr = CreateRect();
        const int gap = UiLen(12);
        const int padV = UiLen(21);
        const int createBtnW = UiLen(78);
        const int wClick = TextWidth(L"点击", bigFont_);
        const int wSuffix = TextWidth(L"鼠标宏", bigFont_);
        const int total = wClick + gap + createBtnW + gap + wSuffix;
        int x = cr.left + (cr.right - cr.left - total) / 2 + wClick + gap;
        return RECT{x, cr.top + padV, x + createBtnW, cr.bottom - padV};
    }
    std::wstring ScriptHotkeyText(const ScriptMeta& script) const { return script.hotkey.enabled && !script.hotkey.text.empty() ? script.hotkey.text : L"设置热键"; }
    RECT ScriptHotkeyRect(int i) const {
        if (i < 0 || i >= static_cast<int>(scripts_.size())) return RECT{};
        RECT r = HomeCardRect(i);
        const int left = r.left + UiLen(14);
        const int rightLimit = r.right - UiLen(96);
        const int hotW = TextWidth(ScriptHotkeyText(scripts_[static_cast<size_t>(i)]), hotFont_);
        const int nameW = TextWidth(scripts_[static_cast<size_t>(i)].name, homeFont_);
        const int hotLeft = std::min(left + nameW + UiLen(10), rightLimit - hotW);
        return RECT{hotLeft, r.top + UiLen(17), std::min(hotLeft + hotW, rightLimit), r.top + UiLen(48)};
    }
    RECT ImportRect() const { return UiRect4(60, 149, 166, 182); }
    RECT ExportRect() const { return UiRect4(180, 149, 286, 182); }
    RECT TimerRect() const { return UiRect4(300, 149, 406, 182); }
    RECT RecorderModeRect() const {
        // 贴齐「按 XX 键开始录制」黄条右上角，不留缝
        const RECT cr = CreateRect();
        const std::wstring label = RecorderInputModeLabel(recorderSettings_.inputMode);
        const int btnW = std::max(UiLen(88), TextWidth(label, homeFont_) + UiLen(24));
        const int btnH = UiLen(28);
        return RECT{
            cr.right - btnW,
            cr.top,
            cr.right,
            cr.top + btnH};
    }

    static std::wstring RecorderInputModeLabel(quickscript::RecorderInputMode mode) {
        switch (mode) {
        case quickscript::RecorderInputMode::DesktopAbsolute: return L"绝对坐标";
        case quickscript::RecorderInputMode::FpsRelative: return L"相对坐标";
        case quickscript::RecorderInputMode::Auto:
        default: return L"自动识别";
        }
    }

    void CycleRecorderInputMode() {
        // 自动识别 → 相对坐标 → 绝对坐标 → …
        switch (recorderSettings_.inputMode) {
        case quickscript::RecorderInputMode::Auto:
            recorderSettings_.inputMode = quickscript::RecorderInputMode::FpsRelative;
            break;
        case quickscript::RecorderInputMode::FpsRelative:
            recorderSettings_.inputMode = quickscript::RecorderInputMode::DesktopAbsolute;
            break;
        case quickscript::RecorderInputMode::DesktopAbsolute:
        default:
            recorderSettings_.inputMode = quickscript::RecorderInputMode::Auto;
            break;
        }
    }
    RECT HelpRect() const { return UiRect4(576, 149, 686, 182); }
    RECT HomeFooterRect() const { return UiRect4(36, 468, 700, 492); }
    RECT ScriptCustomHeaderRect() const { return UiRect4(36, 125, 684, 182); }
    RECT AgentConvChatRect(int i) const { return UiEdgeRect(HomeCardRect(i), 96, 14, 24, 45); }
    RECT AgentConvDeleteRect(int i) const { return UiEdgeRect(HomeCardRect(i), 96, 58, 24, 88); }

    void LoadAgentConversations() {
        agentConversations_.clear();
        LoadAgentConversationList(agentConversations_);
        ClampAgentConvScroll();
    }

    void OnAgentConversationClosed(AgentConversationSavePayload&& payload) {
        if (payload.shouldSave) SaveAgentConversation(payload);
        LoadAgentConversations();
        if (IsWindow(hwnd_)) InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void ConfirmDeleteAgentConversation(int index) {
        if (index < 0 || index >= static_cast<int>(agentConversations_.size())) return;
        pendingDeleteAgentConv_ = index;
        const std::wstring name = agentConversations_[static_cast<size_t>(index)].name;
        promptModal_.ShowConfirm(L"您确定要删除对话 \"" + name + L"\"\n吗？", [this](bool ok) {
            if (ok && pendingDeleteAgentConv_ >= 0
                && pendingDeleteAgentConv_ < static_cast<int>(agentConversations_.size())) {
                DeleteAgentConversation(agentConversations_[static_cast<size_t>(pendingDeleteAgentConv_)].id);
                LoadAgentConversations();
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
            pendingDeleteAgentConv_ = -1;
            StDiscardSpuriousInputAfterModal(hwnd_);
        });
    }

    void QueuePromptInfo(std::wstring message) {
        promptPendingMessage_ = std::move(message);
        PostMessageW(hwnd_, WM_APP_PROMPT, 0, 0);
    }

    void ShowPromptInfo(const std::wstring& message) {
        CloseEditorPopup();
        CloseClickerDropPopup();
        promptModal_.ShowInfo(message);
    }
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
        const int hotW = std::max(UiLen(80), TextWidth(globalHotkey_.text, bigFont_) + UiLen(28));
        const int prefixW = TextWidth(prefix, bigFont_);
        const int suffixW = TextWidth(suffix, bigFont_);
        const int gap = UiLen(20);
        const int totalW = prefixW + gap + hotW + gap + suffixW;
        const int left = cr.left + (cr.right - cr.left - totalW) / 2 + prefixW + gap;
        return RECT{left, cr.top + UiLen(27), left + hotW, cr.bottom - UiLen(20)};
    }
    RECT RunHintRect() const { return CreateRect(); }
    RECT CloseRect() const { RECT rc{}; GetClientRect(hwnd_, &rc); return RECT{rc.right - UiLen(kCloseBtnW), 0, rc.right, UiLen(kTitleH)}; }
    RECT MinimizeRect() const { RECT close = CloseRect(); return RECT{close.left - UiLen(kTitleBtnW), 0, close.left, UiLen(kTitleH)}; }
    RECT SettingsRect() const { RECT min = MinimizeRect(); return RECT{min.left - UiLen(kTitleBtnW), 0, min.left, UiLen(kTitleH)}; }
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
    int ActionListCheckboxColumnRight() const {
        return UiLen(kListX) + UiLen(kListInnerPad) + UiLen(kColRemarkInList) - UiLen(4);
    }
    RECT CheckboxRect(int index) const {
        RECT list = ActionListRect();
        const int slot = VisibleSlotOf(index);
        if (slot < 0) return RECT{};
        const int rowH = std::max(1, UiLen(kRowH));
        const int localY = UiLen(kListInnerPad) + (slot - scrollOffset_) * rowH;
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
        const int bodyBottom = static_cast<int>(rc.bottom) - (page_ == Page::Editor ? UiLen(kBottomH) : 0);
        const int h = std::max(0, std::min(UiLen(kListH), bodyBottom - UiLen(kListY)));
        return RECT{UiLen(kListX), UiLen(kListY), UiLen(kListX) + UiLen(kListW), UiLen(kListY) + h};
    }
    int VisibleActionRows() const {
        const RECT list = ActionListRect();
        const int rowH = std::max(1, UiLen(kRowH));
        return std::max(0, static_cast<int>((list.bottom - list.top) / rowH));
    }
    int VisibleActionCount() const {
        return static_cast<int>(VisibleActionIndices().size());
    }
    void MarkVisibleActionsDirty() {
        visibleActionIndexCacheDirty_ = true;
    }
    const std::vector<int>& VisibleActionIndices() const {
        if (visibleActionIndexCacheDirty_) {
            visibleActionIndexCache_.clear();
            visibleActionIndexCache_.reserve(actions_.size());
            for (int i = 0; i < static_cast<int>(actions_.size()); ++i) {
                if (IsRowVisible(i)) visibleActionIndexCache_.push_back(i);
            }
            visibleActionIndexCacheDirty_ = false;
        }
        return visibleActionIndexCache_;
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
    int ActionListContentRight() const {
        return UiLen(kListX) + UiLen(kListW)
            - (MaxEditorScroll() > 0 ? UiLen(kEditorScrollW) + UiLen(6) : 0);
    }
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
        const int contentH = std::max(VisibleActionCount() * std::max(1, UiLen(kRowH)), 1);
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

    RECT ClickerTabRect() const { const int tabW = UiHomeWidth() / 4; return RECT{0, UiLen(kTitleH), tabW, UiLen(kHomeContentTop)}; }
    RECT RecorderTabRect() const { const int tabW = UiHomeWidth() / 4; return RECT{tabW, UiLen(kTitleH), tabW * 2, UiLen(kHomeContentTop)}; }
    RECT MacroTabRect() const { const int tabW = UiHomeWidth() / 4; return RECT{tabW * 2, UiLen(kTitleH), tabW * 3, UiLen(kHomeContentTop)}; }
    RECT ScriptCustomTabRect() const { const int tabW = UiHomeWidth() / 4; return RECT{tabW * 3, UiLen(kTitleH), tabW * 4, UiLen(kHomeContentTop)}; }

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
        int x = UiLen(kClickerLabelX) + TextWidth(L"点击类型:", homeFont_) + UiLen(kClickerFieldGap);
        clickerLayout_.leftRadioLeft = x;
        x += UiLen(kClickerRadioSize) + UiLen(kRadioTextGap) + TextWidth(L"鼠标左键", homeFont_) + UiLen(kOptionGap);
        clickerLayout_.middleRadioLeft = x;
        x += UiLen(kClickerRadioSize) + UiLen(kRadioTextGap) + TextWidth(L"鼠标中键", homeFont_) + UiLen(kOptionGap);
        clickerLayout_.rightRadioLeft = x;
        clickerLayout_.intervalComboLeft = UiLen(kClickerLabelX) + TextWidth(L"每次点击间隔时间:", homeFont_) + UiLen(kClickerFieldGap);
        clickerLayout_.hotkeyComboLeft = UiLen(kClickerLabelX) + TextWidth(L"启停的全局热键:", homeFont_) + UiLen(kClickerFieldGap);
    }

    RECT ClickerLeftRadioRect() const {
        return RECT{clickerLayout_.leftRadioLeft, UiLen(171), clickerLayout_.leftRadioLeft + UiLen(kClickerRadioSize), UiLen(191)};
    }
    RECT ClickerMiddleRadioRect() const {
        return RECT{clickerLayout_.middleRadioLeft, UiLen(171), clickerLayout_.middleRadioLeft + UiLen(kClickerRadioSize), UiLen(191)};
    }
    RECT ClickerRightRadioRect() const {
        return RECT{clickerLayout_.rightRadioLeft, UiLen(171), clickerLayout_.rightRadioLeft + UiLen(kClickerRadioSize), UiLen(191)};
    }
    RECT ClickerIntervalRect() const {
        return RECT{clickerLayout_.intervalComboLeft, UiLen(236), UiLen(kClickerComboRight), UiLen(266)};
    }
    RECT ClickerHotkeyRect() const {
        return RECT{clickerLayout_.hotkeyComboLeft, UiLen(303), UiLen(kClickerComboRight), UiLen(333)};
    }
    static constexpr int kClickerDropdownItemH = 68;
    static constexpr int kClickerHotkeyMenuCount = 7;
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
        const int hotW = std::max(UiLen(56), TextWidth(hotText, bigFont_) + UiLen(20));
        return RECT{cr.left + UiLen(204), cr.top + UiLen(21), cr.left + UiLen(204) + hotW, cr.bottom - UiLen(21)};
    }
    // 主界面卡片右侧操作列：宏/录制共用；右缘留白 16（相对上一版翻倍）
    // rightPad=16，列宽=72 → rightStart=88
    RECT HomeCardEditBtn(const RECT& r) const { return UiEdgeRect(r, 88, 14, 16, 45); }
    RECT HomeCardDeleteBtn(const RECT& r) const { return UiEdgeRect(r, 88, 56, 16, 88); }
    RECT HomeCardSelectedTag(const RECT& r) const { return UiEdgeRect(r, 88, 58, 16, 95); }
    RECT HomeCardNameRect(const RECT& r) const { return UiInsetRect(r, 14, 17, 96, 48); }
    RECT HomeCardMetaLeft(const RECT& r) const {
        return RECT{r.left + UiLen(14), r.top + UiLen(58), r.left + UiLen(350), r.top + UiLen(88)};
    }
    RECT HomeCardMetaRight(const RECT& r) const {
        return RECT{r.left + UiLen(344), r.top + UiLen(58), r.left + UiLen(510), r.top + UiLen(88)};
    }
    // 「重命名/删除」与宏「编辑/删除」同列；「优化」同行等高，紧挨其左（右对齐贴齐）
    RECT RecordingOptimizeRect(int i) const { return UiEdgeRect(RecordingCardRect(i), 148, 14, 100, 45); }
    RECT RecordingRenameRect(int i) const { return UiEdgeRect(RecordingCardRect(i), 88, 14, 16, 45); }
    RECT RecordingDeleteRect(int i) const { return UiEdgeRect(RecordingCardRect(i), 88, 56, 16, 88); }
    RECT RecordingDeselectRect(int i) const { return UiEdgeRect(RecordingCardRect(i), 88, 14, 16, 45); }
    RECT RecordingSelectedTagRect(int i) const { return UiEdgeRect(RecordingCardRect(i), 88, 58, 16, 95); }
    RECT RecorderHotkeyRect() const { return UiRect4(267, 397, 359, 438); }
    RECT RecorderScopeRect() const { return UiRect4(556, 369, 692, 412); }

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
            case WM_ERASEBKGND:
                // 背景由 WM_PAINT 统一填充，避免 erase+paint 双次白闪
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC hdc = BeginPaint(hwnd, &ps);
                self->FillParamViewportGaps(hdc, hwnd, ps.rcPaint);
                self->PaintEditorParamChrome(hdc, hwnd);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_MOUSEWHEEL:
                self->OnWheel(GET_WHEEL_DELTA_WPARAM(wp));
                return 0;
            case WM_MOUSEMOVE: {
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ClientToScreen(hwnd, &pt);
                ScreenToClient(self->hwnd_, &pt);
                self->OnMouseMove(pt.x, pt.y);
                return 0;
            }
            case WM_MOUSELEAVE: {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(self->hwnd_, &pt);
                self->OnMouseMove(pt.x, pt.y);
                return 0;
            }
            default:
                break;
            }
        }
        if (self && self->IsParamPanelCheckbox(hwnd)) {
            switch (msg) {
            case WM_ERASEBKGND:
                return 1;
            case WM_PRINTCLIENT:
                self->PaintParamPanelCheckbox(hwnd, reinterpret_cast<HDC>(wp));
                return 0;
            case WM_PAINT:
                return DefSubclassProc(hwnd, msg, wp, lp);
            case WM_LBUTTONDOWN:
                SetFocus(hwnd);
                SetCapture(hwnd);
                return 0;
            case WM_LBUTTONDBLCLK:
                return 0;
            case WM_LBUTTONUP:
                // 必须本控件收到过 DOWN（capture）：下拉在 DOWN 时切类型会露出勾选框，
                // 随后的 UP 若落在「左Win」上，不能当成一次点击。
                if (GetCapture() == hwnd) {
                    ReleaseCapture();
                    self->ToggleParamCheckbox(hwnd);
                }
                return 0;
            case WM_KEYDOWN:
                if (wp == VK_SPACE) {
                    self->ToggleParamCheckbox(hwnd);
                    return 0;
                }
                break;
            case WM_NCDESTROY:
                RemovePropW(hwnd, kParamCheckboxProp);
                RemovePropW(hwnd, kParamCheckboxCheckedProp);
                break;
            default:
                break;
            }
        }
        if (self && self->IsGrayButton(hwnd)) {
            if (msg == WM_LBUTTONDOWN) {
                FiDbgOnGrayClick(hwnd);
            }
            if (msg == WM_LBUTTONUP) {
                FiDbgLogFmt(L"GRAY_LBUTTONUP", L"btn=%p name=%s",
                    hwnd, self->GrayButtonDebugName(hwnd));
                LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
                self->EditorChildMessage(hwnd, msg, wp, lp);
                self->FinishGrayButtonClick(hwnd);
                return r;
            }
            if (msg == WM_CAPTURECHANGED && reinterpret_cast<HWND>(lp) != hwnd) {
                LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
                self->FinishGrayButtonClick(hwnd);
                return r;
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
                FillRectColor(hdc, rc, hwnd == self->paramBottomMask_ ? kPanel : kWhite);
                EndPaint(hwnd, &ps);
                return 0;
            }
            if (self->EditorComboPopupIdForHwnd(hwnd) >= 0) {
                return msg == WM_ERASEBKGND ? 1 : 0;
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
        if (selectedIndex_ < 0 && !loadingForm_) {
            actionFormDrafts_[popupAction_.sel] = ActionFromForm();
        }
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
        RenderBatchScope batch(memDc);
        PaintActionListLocal(memDc, lw, lh);
        SelectObject(memDc, oldFont);
        batch.End();
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

    void DrawExpandTriangle(HDC hdc, RECT rc, bool expanded, COLORREF color) {
        ::DrawExpandTriangle(hdc, rc, expanded, color);
    }

    void ToggleContainerExpand(int index) {
        if (index < 0 || index >= static_cast<int>(actions_.size()) || !IsExpandableContainer(actions_[static_cast<size_t>(index)].type)) return;
        if (IsContainerExpanded(index)) collapsedContainers_.insert(index);
        else collapsedContainers_.erase(index);
        MarkVisibleActionsDirty();
        scrollOffset_ = std::clamp(scrollOffset_, 0, MaxEditorScroll());
        RefreshActionListLayer();
    }

    void PaintActionListLocal(HDC hdc, int width, int height);

    RECT LocalCheckboxRect(int index, int y) const {
        const int pad = UiLen(kListInnerPad);
        const auto& a = actions_[static_cast<size_t>(index)];
        const int rowH = std::max(1, UiLen(kRowH));
        const int cb = std::max(1, UiLen(kBatchCheckboxSize));
        const int top = y + (rowH - cb) / 2;
        const int left = BatchCheckboxLeftLocal(a.indent, a.type, pad);
        return RECT{left, top, left + cb, top + cb};
    }

    RECT LocalCopyRect(int, int y, int contentRight) const {
        const int rowH = std::max(1, UiLen(kRowH));
        return RECT{contentRight - UiLen(104), y + UiLen(6), contentRight - UiLen(62), y + rowH - UiLen(6)};
    }
    RECT LocalDeleteRect(int, int y, int contentRight) const {
        const int rowH = std::max(1, UiLen(kRowH));
        return RECT{contentRight - UiLen(58), y + UiLen(6), contentRight - UiLen(18), y + rowH - UiLen(6)};
    }

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

    void InvalidateAgentConvCard(int index) {
        if (index < 0 || index >= static_cast<int>(agentConversations_.size())) return;
        InvalidateRectClipped(HomeCardRect(index));
    }

    int HitAgentConvCard(int x, int y) const {
        if (activeHomeTab_ != quickscript::MainTab::ScriptCustom) return -1;
        RECT list = HomeListRect();
        if (!PtIn(list, x, y)) return -1;
        for (int i = 0; i < static_cast<int>(agentConversations_.size()); ++i) {
            RECT r = HomeCardRect(i);
            if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) return i;
        }
        return -1;
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
        if (promptModal_.visible()) return;
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
            ApplyParamScrollOffset(true, false);
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
            const int oldAgentConv = agentConvHover_;
            homeHover_ = HitHomeCard(x, y);
            recordingHover_ = HitRecordingCard(x, y);
            agentConvHover_ = HitAgentConvCard(x, y);
            if (oldCard != homeHover_) {
                InvalidateHomeCard(oldCard);
                InvalidateHomeCard(homeHover_);
            }
            if (oldRecording != recordingHover_) {
                InvalidateRecordingCard(oldRecording);
                InvalidateRecordingCard(recordingHover_);
            }
            if (oldAgentConv != agentConvHover_) {
                InvalidateAgentConvCard(oldAgentConv);
                InvalidateAgentConvCard(agentConvHover_);
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
                if (PtIn(RecorderModeRect(), x, y)) return HoverButton::HomeEdit;
                if (PtIn(RecorderBannerKeyRect(), x, y)) return HoverButton::CommonHotkey;
                if (ActiveHomeListMaxScroll() > 0 && PtIn(HomeScrollTrackRect(), x, y)) return HoverButton::HomeScroll;
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
                if (PtIn(CreateRect(), x, y)) return HoverButton::Create;
                if (ActiveHomeListMaxScroll() > 0 && PtIn(HomeScrollTrackRect(), x, y)) return HoverButton::HomeScroll;
                for (int i = 0; i < static_cast<int>(agentConversations_.size()); ++i) {
                    RECT r = HomeCardRect(i);
                    RECT list = HomeListRect();
                    if (r.bottom < list.top || r.top > list.bottom) continue;
                    if (PtIn(AgentConvChatRect(i), x, y)) return HoverButton::HomeEdit;
                    if (PtIn(AgentConvDeleteRect(i), x, y)) return HoverButton::HomeDelete;
                    if (PtIn(r, x, y)) return HoverButton::HomeCard;
                }
                return HoverButton::None;
            }
            if (PtIn(ImportRect(), x, y)) return HoverButton::Import;
            if (PtIn(ExportRect(), x, y)) return HoverButton::Export;
            if (PtIn(TimerRect(), x, y)) return HoverButton::HomeCard;
            if (selectedScript_ >= 0 && PtIn(CommonHotRect(), x, y)) return HoverButton::CommonHotkey;
            if (selectedScript_ < 0 && PtIn(CreateWordRect(), x, y)) return HoverButton::Create;
            if (ActiveHomeListMaxScroll() > 0 && PtIn(HomeScrollTrackRect(), x, y)) return HoverButton::HomeScroll;
            for (int i = 0; i < static_cast<int>(scripts_.size()); ++i) {
                RECT r = HomeCardRect(i);
                RECT list = HomeListRect();
                if (r.bottom < list.top || r.top > list.bottom) continue;
                RECT hot = ScriptHotkeyRect(i);
                RECT edit = HomeCardEditBtn(r);
                RECT del = HomeCardDeleteBtn(r);
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
        if (ShouldShowModifyButton() && PtIn(ModifyButtonRect(), x, y)) return HoverButton::Modify;
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
        const HoverButton hb = HitButton(x, y);
        // 整行可选中，但空白行体不做手型；仅复制/删除/批量勾选等真交互点
        if (hb == HoverButton::None || hb == HoverButton::Row) {
            return HitGrayButton(x, y) != nullptr;
        }
        return true;
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
        if (button == HoverButton::Clear) { ClearEditorActions(); return; }
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
        if (promptModal_.visible()) return;
        if (HitClose(x, y)) { if (page_ == Page::Editor) ShowHome(); else SendMessageW(hwnd_, WM_CLOSE, 0, 0); return; }
        if (HitMinimize(x, y)) { ShowWindow(hwnd_, SW_MINIMIZE); return; }
        if (HitSettings(x, y) && page_ == Page::Home) { ShowSettingsDialog(); return; }
        if (y <= UiLen(kTitleH)) { ReleaseCapture(); SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(x, y)); return; }
        if (page_ == Page::Home && (activeHomeTab_ == quickscript::MainTab::Macro || activeHomeTab_ == quickscript::MainTab::ScriptCustom)
            && ActiveHomeListMaxScroll() > 0 && PtIn(HomeScrollThumbRect(), x, y)) {
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
        if (promptModal_.visible()) return;
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
        homeHover_ = -1;
        recordingHover_ = -1;
        agentConvHover_ = -1;
        if (tab == quickscript::MainTab::Recorder) ClampRecordingScroll();
        if (tab == quickscript::MainTab::Macro) { homeScrollOffset_ = 0; ClampHomeScroll(); }
        if (tab == quickscript::MainTab::ScriptCustom) { homeScrollOffset_ = 0; ClampAgentConvScroll(); }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    // ── 主界面状态缓存（退出时保存，启动时恢复） ──────────────────
    void SaveHomeState() {
        auto& hs = appSettings_.home;
        // 有选中脚本/录制时，保存它所在的 Tab；否则保存当前可见的 Tab
        if (selectedScript_ >= 0)
            hs.activeTab = static_cast<int>(quickscript::MainTab::Macro);
        else if (selectedRecording_ >= 0)
            hs.activeTab = static_cast<int>(quickscript::MainTab::Recorder);
        else
            hs.activeTab = static_cast<int>(activeHomeTab_);
        hs.clickerButton = static_cast<int>(clickerSettings_.button);
        hs.clickerIntervalMode = static_cast<int>(clickerSettings_.intervalMode);
        hs.clickerCustomInterval = clickerSettings_.customIntervalSeconds;
        hs.recorderCaptureScope = static_cast<int>(recorderSettings_.captureScope);
        hs.recorderInputMode = static_cast<int>(recorderSettings_.inputMode);
        hs.selectedScriptPath = (selectedScript_ >= 0 && selectedScript_ < static_cast<int>(scripts_.size()))
            ? scripts_[static_cast<size_t>(selectedScript_)].path : L"";
        hs.selectedRecordingPath = (selectedRecording_ >= 0 && selectedRecording_ < static_cast<int>(recordings_.size()))
            ? recordings_[static_cast<size_t>(selectedRecording_)].path : L"";
        switch (activeHomeTab_) {
        case quickscript::MainTab::Clicker: hs.clickerScrollOffset = homeScrollOffset_; break;
        case quickscript::MainTab::Recorder: hs.recorderScrollOffset = homeScrollOffset_; break;
        case quickscript::MainTab::Macro: hs.macroScrollOffset = homeScrollOffset_; break;
        case quickscript::MainTab::ScriptCustom: hs.scriptCustomScrollOffset = homeScrollOffset_; break;
        }
        SaveAppSettings(appSettings_);

        // 同时写入简单文本文件（UTF-8 编码，规避 wofstream 的区域设置问题）
        std::string utf8;
        utf8 += "\xEF\xBB\xBF"; // UTF-8 BOM
        auto addNum = [&](int v) { utf8 += std::to_string(v) + "\n"; };
        auto addDouble = [&](double v) { utf8 += std::to_string(v) + "\n"; };
        auto addWstr = [&](const std::wstring& s) { utf8 += ToUtf8(s) + "\n"; };
        addNum(hs.activeTab);
        addNum(hs.clickerButton);
        addNum(hs.clickerIntervalMode);
        addDouble(hs.clickerCustomInterval);
        addNum(hs.recorderCaptureScope);
        addWstr(hs.selectedScriptPath);
        addWstr(hs.selectedRecordingPath);
        addNum(hs.clickerScrollOffset);
        addNum(hs.recorderScrollOffset);
        addNum(hs.macroScrollOffset);
        addNum(hs.scriptCustomScrollOffset);
        addNum(hs.recorderInputMode); // 追加字段，保持旧 home_state.txt 行号兼容
        std::ofstream f(AppDir() + L"\\home_state.txt", std::ios::binary | std::ios::trunc);
        if (f.is_open()) f.write(utf8.data(), utf8.size());
    }

    void RestoreHomeState() {
        // 优先从简单文本文件加载（UTF-8 编码，避免区域设置问题）
        const std::wstring stateFile = AppDir() + L"\\home_state.txt";
        const std::wstring content = ReadAll(stateFile);
        if (!content.empty()) {
            // 按行解析
            std::vector<std::wstring> lines;
            size_t start = 0;
            for (size_t i = 0; i <= content.size(); ++i) {
                if (i == content.size() || content[i] == L'\n') {
                    // 去掉末尾可能存在的 \r
                    size_t end = i;
                    if (end > start && content[end - 1] == L'\r') --end;
                    lines.push_back(content.substr(start, end - start));
                    start = i + 1;
                }
            }
            if (lines.size() < 6) return; // 不够字段数，回退到 JSON

            int activeTab = 0;
            if (!lines[0].empty()) activeTab = std::stoi(lines[0]);
            if (activeTab >= 0 && activeTab <= 3)
                activeHomeTab_ = static_cast<quickscript::MainTab>(activeTab);
            if (!lines[1].empty()) {
                int btn = std::stoi(lines[1]);
                if (btn >= 0 && btn <= 2)
                    clickerSettings_.button = static_cast<quickscript::MouseButtonChoice>(btn);
            }
            if (!lines[2].empty()) {
                int mode = std::stoi(lines[2]);
                if (mode >= 0 && mode <= 2)
                    clickerSettings_.intervalMode = static_cast<quickscript::ClickIntervalMode>(mode);
            }
            if (!lines[3].empty()) {
                double val = std::stod(lines[3]);
                if (val > 0) clickerSettings_.customIntervalSeconds = val;
            }
            if (!lines[4].empty()) {
                int scope = std::stoi(lines[4]);
                if (scope >= 0 && scope <= 1)
                    recorderSettings_.captureScope = static_cast<quickscript::RecordCaptureScope>(scope);
            }
            if (lines.size() > 11 && !lines[11].empty()) {
                const int mode = std::stoi(lines[11]);
                if (mode >= 0 && mode <= 2)
                    recorderSettings_.inputMode = static_cast<quickscript::RecorderInputMode>(mode);
            }
            // 恢复选中脚本（与录制互斥，优先恢复有路径的那个）
            selectedScript_ = -1;
            selectedRecording_ = -1;
            if (lines.size() > 6) {
                std::wstring recordingPath = lines[6];
                if (!recordingPath.empty()) {
                    for (int i = 0; i < static_cast<int>(recordings_.size()); ++i) {
                        if (recordings_[static_cast<size_t>(i)].path == recordingPath) {
                            selectedRecording_ = i;
                            break;
                        }
                    }
                }
            }
            // 只有录制没恢复成功时才尝试恢复脚本（与录制互斥）
            if (selectedRecording_ < 0) {
                std::wstring scriptPath = lines[5];
                if (!scriptPath.empty()) {
                    for (int i = 0; i < static_cast<int>(scripts_.size()); ++i) {
                        if (scripts_[static_cast<size_t>(i)].path == scriptPath) {
                            selectedScript_ = i;
                            break;
                        }
                    }
                }
            }
            // 恢复滚动位置
            switch (activeHomeTab_) {
            case quickscript::MainTab::Clicker:
                homeScrollOffset_ = (lines.size() > 7 && !lines[7].empty()) ? std::stoi(lines[7]) : 0;
                break;
            case quickscript::MainTab::Recorder:
                homeScrollOffset_ = (lines.size() > 8 && !lines[8].empty()) ? std::stoi(lines[8]) : 0;
                ClampRecordingScroll();
                break;
            case quickscript::MainTab::Macro:
                homeScrollOffset_ = (lines.size() > 9 && !lines[9].empty()) ? std::stoi(lines[9]) : 0;
                ClampHomeScroll();
                break;
            case quickscript::MainTab::ScriptCustom:
                homeScrollOffset_ = (lines.size() > 10 && !lines[10].empty()) ? std::stoi(lines[10]) : 0;
                ClampAgentConvScroll();
                break;
            }
            return;
        }

        // 回退到 JSON 中读取（首次启动时会进入这里）
        const auto& hs = appSettings_.home;
        // 恢复标签页选中
        if (hs.activeTab >= 0 && hs.activeTab <= 3)
            activeHomeTab_ = static_cast<quickscript::MainTab>(hs.activeTab);
        // 恢复连点设置
        if (hs.clickerButton >= 0 && hs.clickerButton <= 2)
            clickerSettings_.button = static_cast<quickscript::MouseButtonChoice>(hs.clickerButton);
        if (hs.clickerIntervalMode >= 0 && hs.clickerIntervalMode <= 2)
            clickerSettings_.intervalMode = static_cast<quickscript::ClickIntervalMode>(hs.clickerIntervalMode);
        if (hs.clickerCustomInterval > 0)
            clickerSettings_.customIntervalSeconds = hs.clickerCustomInterval;
        // 恢复录制设置
        if (hs.recorderCaptureScope >= 0 && hs.recorderCaptureScope <= 1)
            recorderSettings_.captureScope = static_cast<quickscript::RecordCaptureScope>(hs.recorderCaptureScope);
        if (hs.recorderInputMode >= 0 && hs.recorderInputMode <= 2)
            recorderSettings_.inputMode = static_cast<quickscript::RecorderInputMode>(hs.recorderInputMode);
        // 恢复选中脚本/录制（与录制/脚本互斥，优先恢复有路径的那个）
        selectedScript_ = -1;
        selectedRecording_ = -1;
        if (!hs.selectedRecordingPath.empty()) {
            for (int i = 0; i < static_cast<int>(recordings_.size()); ++i) {
                if (recordings_[static_cast<size_t>(i)].path == hs.selectedRecordingPath) {
                    selectedRecording_ = i;
                    break;
                }
            }
        }
        if (selectedRecording_ < 0 && !hs.selectedScriptPath.empty()) {
            for (int i = 0; i < static_cast<int>(scripts_.size()); ++i) {
                if (scripts_[static_cast<size_t>(i)].path == hs.selectedScriptPath) {
                    selectedScript_ = i;
                    break;
                }
            }
        }
        // 恢复滚动位置
        switch (activeHomeTab_) {
        case quickscript::MainTab::Clicker: homeScrollOffset_ = hs.clickerScrollOffset; break;
        case quickscript::MainTab::Recorder: { homeScrollOffset_ = hs.recorderScrollOffset; ClampRecordingScroll(); break; }
        case quickscript::MainTab::Macro: { homeScrollOffset_ = hs.macroScrollOffset; ClampHomeScroll(); break; }
        case quickscript::MainTab::ScriptCustom: { homeScrollOffset_ = hs.scriptCustomScrollOffset; ClampAgentConvScroll(); break; }
        }
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
            const RECT rc = ClickerIntervalRect();
            const bool onArrow = x >= rc.right - 36;
            if (clickerSettings_.intervalMode == quickscript::ClickIntervalMode::Custom && !onArrow) {
                ShowCustomIntervalDialog();
                return true;
            }
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

    void SyncOwnedDropPopups() {
        if (editorPopupOpen_ >= 0) SyncEditorDropPopup();
        if (clickerDropPopupKind_ >= 0) SyncClickerDropPopup();
        if (quickInputTipShown_ != QuickInputTipKind::None) SyncQuickInputTipPopup();
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
        SetWindowPos(clickerDropPopup_, HWND_TOPMOST, x, y, w, h,
            SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
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
                {kHotLeft, L"鼠标左键", L"长按左键开始连点，松开停止（约0.2秒）"},
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
        UiScaleInitFromHwnd(hwnd_);
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
        const int dlgW = UiLen(420);
        const int dlgH = UiLen(225);
        const wchar_t* clsName = L"QuickScriptCustomIntervalDlg";
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                auto* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                auto cancelRect = []() { return RECT{UiLen(108), UiLen(168), UiLen(198), UiLen(206)}; };
                auto okRect = []() { return RECT{UiLen(222), UiLen(168), UiLen(312), UiLen(206)}; };
                auto closeRect = []() { return RECT{UiLen(380), 0, UiLen(420), UiLen(kTitleH)}; };
                auto editOuterRect = []() {
                    const int bodyTop = UiLen(38);
                    const int bodyBottom = UiLen(168);
                    const int rowH = UiLen(38);
                    const int editW = UiLen(140);
                    const int gap = UiLen(10);
                    const int unitW = UiLen(32);
                    const int groupW = editW + gap + unitW;
                    const int groupLeft = (UiLen(420) - groupW) / 2;
                    const int rowTop = bodyTop + (bodyBottom - bodyTop - rowH) / 2;
                    return RECT{groupLeft, rowTop, groupLeft + editW, rowTop + rowH};
                };
                auto unitRect = [&]() {
                    RECT edit = editOuterRect();
                    return RECT{edit.right + UiLen(10), edit.top, edit.right + UiLen(42), edit.bottom};
                };
                auto centerEditText = [](HWND edit) {
                    CenterModernSingleLineEditText(edit);
                };
                if (msg == WM_CREATE) {
                    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                    st = reinterpret_cast<DialogState*>(cs->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
                    st->titleFont = CreateFontW(UiFontHeight(26), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
                    st->bodyFont = CreateFontW(UiFontHeight(26), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
                    st->closeFont = CreateFontW(UiFontHeight(36), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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
                    RECT client{}; GetClientRect(hwnd, &client);
                    const int th = UiLen(kTitleH);
                    RECT titleRc{0, 0, client.right, th};
                    FillRectColor(hdc, titleRc, kMainGreen);
                    FillRectColor(hdc, RECT{0, th, client.right, client.bottom}, kWhite);
                    if (st->hoverClose) FillRectColor(hdc, closeRect(), kCloseHover);
                    HGDIOBJ oldFont = SelectObject(hdc, st->titleFont);
                    ::DrawTextIn(hdc, L"  鼠大侠-自定义连点间隔", titleRc, kWhite);
                    SelectObject(hdc, st->closeFont);
                    ::DrawTextIn(hdc, L"×", closeRect(), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, st->bodyFont);
                    DrawBorderRect(hdc, editOuterRect(), kComboBorderGray);
                    ::DrawTextIn(hdc, L"秒", unitRect(), kText);
                    const RECT cancel = cancelRect();
                    const RECT ok = okRect();
                    if (st->hoverCancel) FillRectColor(hdc, cancel, kComboHoverGreen);
                    DrawBorderRoundRect(hdc, cancel, kMainGreen, UiLen(6));
                    ::DrawTextIn(hdc, L"取消", cancel, st->hoverCancel ? kDarkGreen : kMainGreen,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
        RECT ownerClient{};
        GetClientRect(hwnd_, &ownerClient);
        POINT ownerTl{0, 0};
        ClientToScreen(hwnd_, &ownerTl);
        const RECT ownerScreen{
            ownerTl.x, ownerTl.y,
            ownerTl.x + ownerClient.right, ownerTl.y + ownerClient.bottom
        };
        const int ownerW = ownerScreen.right - ownerScreen.left;
        const int ownerH = ownerScreen.bottom - ownerScreen.top;
        const int dlgX = ownerScreen.left + (ownerW - dlgW) / 2;
        const int dlgY = ownerScreen.top + (ownerH - dlgH) / 2;

        static constexpr BYTE kOverlayAlpha = 145;
        static constexpr wchar_t kOverlayClass[] = L"QuickScriptCustomIntervalOverlay";
        static bool overlayRegistered = false;
        if (!overlayRegistered) {
            WNDCLASSW owc{};
            owc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                if (msg == WM_ERASEBKGND) return 1;
                if (msg == WM_PAINT) {
                    PAINTSTRUCT ps{};
                    HDC hdc = BeginPaint(hwnd, &ps);
                    RECT rc{};
                    GetClientRect(hwnd, &rc);
                    ::FillAlphaRect(hdc, rc, RGB(0, 0, 0), kOverlayAlpha);
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            };
            owc.hInstance = g_instance;
            owc.lpszClassName = kOverlayClass;
            owc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassW(&owc);
            overlayRegistered = true;
        }
        HWND overlay = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kOverlayClass, L"", WS_POPUP,
            ownerScreen.left, ownerScreen.top, ownerW, ownerH,
            hwnd_, nullptr, g_instance, nullptr);
        ShowWindow(overlay, SW_SHOWNA);
        UpdateWindow(overlay);

        HWND dlg = CreateWindowExW(WS_EX_TOPMOST, clsName, L"", WS_POPUP,
            dlgX, dlgY, dlgW, dlgH, hwnd_, nullptr, g_instance, &state);
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);
        SetForegroundWindow(dlg);
        SetFocus(state.edit);
        MSG msg{};
        while (!state.done && IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        if (IsWindow(overlay)) DestroyWindow(overlay);
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
        UiScaleInitFromHwnd(hwnd_);
        struct DialogState {
            std::wstring name;
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
        state.name = recordings_[static_cast<size_t>(index)].name;
        const int dlgW = UiLen(420);
        const int dlgH = UiLen(225);
        const wchar_t* clsName = L"QuickScriptRenameRecordingDlg";
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                auto* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                auto cancelRect = []() { return RECT{UiLen(108), UiLen(168), UiLen(198), UiLen(206)}; };
                auto okRect = []() { return RECT{UiLen(222), UiLen(168), UiLen(312), UiLen(206)}; };
                auto closeRect = []() { return RECT{UiLen(380), 0, UiLen(420), UiLen(kTitleH)}; };
                auto editOuterRect = []() { return RECT{UiLen(40), UiLen(78), UiLen(380), UiLen(116)}; };
                auto centerEditText = [](HWND edit) {
                    CenterModernSingleLineEditText(edit);
                };
                if (msg == WM_CREATE) {
                    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
                    st = reinterpret_cast<DialogState*>(cs->lpCreateParams);
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
                    st->titleFont = CreateFontW(UiFontHeight(26), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
                    st->bodyFont = CreateFontW(UiFontHeight(26), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
                    st->closeFont = CreateFontW(UiFontHeight(36), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
                    const RECT editOuter = editOuterRect();
                    st->edit = MakeModernSingleLineEdit(hwnd, nullptr, 100,
                        editOuter.left + 1, editOuter.top + 1,
                        editOuter.right - editOuter.left - 2, editOuter.bottom - editOuter.top - 2);
                    ApplyModernEditBehavior(st->edit, false);
                    SendMessageW(st->edit, WM_SETFONT, reinterpret_cast<WPARAM>(st->bodyFont), TRUE);
                    SetWindowTextW(st->edit, st->name.c_str());
                    centerEditText(st->edit);
                    return 0;
                }
                if (!st) return DefWindowProcW(hwnd, msg, wp, lp);
                if (msg == WM_ERASEBKGND) return 1;
                if (msg == WM_SETCURSOR) {
                    if (LOWORD(lp) == HTCLIENT) {
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
                        // 只刷按钮区，避免整窗 Invalidate 导致编辑框一起闪
                        auto dirty = [](HWND hwnd, const RECT& a, const RECT& b) {
                            RECT u{};
                            UnionRect(&u, &a, &b);
                            InvalidateRect(hwnd, &u, FALSE);
                        };
                        if (hc != st->hoverClose) dirty(hwnd, closeR, closeR);
                        if (hcan != st->hoverCancel) dirty(hwnd, cancelR, cancelR);
                        if (hok != st->hoverOk) dirty(hwnd, okR, okR);
                        st->hoverClose = hc;
                        st->hoverCancel = hcan;
                        st->hoverOk = hok;
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
                    HDC windowDc = BeginPaint(hwnd, &ps);
                    RECT client{}; GetClientRect(hwnd, &client);
                    const int cw = client.right - client.left;
                    const int ch = client.bottom - client.top;
                    HDC hdc = CreateCompatibleDC(windowDc);
                    HBITMAP bmp = CreateCompatibleBitmap(windowDc, cw, ch);
                    HGDIOBJ oldBmp = SelectObject(hdc, bmp);
                    const int th = UiLen(kTitleH);
                    RECT titleRc{0, 0, client.right, th};
                    FillRectColor(hdc, titleRc, kMainGreen);
                    FillRectColor(hdc, RECT{0, th, client.right, client.bottom}, kWhite);
                    if (st->hoverClose) FillRectColor(hdc, closeRect(), kCloseHover);
                    HGDIOBJ oldFont = SelectObject(hdc, st->titleFont);
                    ::DrawTextIn(hdc, L"  鼠大侠-重命名", titleRc, kWhite);
                    SelectObject(hdc, st->closeFont);
                    ::DrawTextIn(hdc, L"×", closeRect(), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(hdc, st->bodyFont);
                    DrawBorderRect(hdc, editOuterRect(), kComboBorderGray);
                    const RECT cancel = cancelRect();
                    const RECT ok = okRect();
                    if (st->hoverCancel) FillRectColor(hdc, cancel, kComboHoverGreen);
                    DrawBorderRoundRect(hdc, cancel, kMainGreen, UiLen(6));
                    ::DrawTextIn(hdc, L"取消", cancel, st->hoverCancel ? kDarkGreen : kMainGreen,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    StDrawGreenButton(hdc, st->bodyFont, ok, L"确定", st->hoverOk);
                    SelectObject(hdc, oldFont);
                    const int blitW = ps.rcPaint.right - ps.rcPaint.left;
                    const int blitH = ps.rcPaint.bottom - ps.rcPaint.top;
                    BitBlt(windowDc, ps.rcPaint.left, ps.rcPaint.top, blitW, blitH,
                        hdc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
                    SelectObject(hdc, oldBmp);
                    DeleteObject(bmp);
                    DeleteDC(hdc);
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
                        wchar_t buf[256]{};
                        GetWindowTextW(st->edit, buf, 255);
                        st->name = buf;
                        st->ok = !Trim(st->name).empty();
                        st->done = true;
                        DestroyWindow(hwnd);
                        return 0;
                    }
                }
                if (msg == WM_KEYDOWN && wp == VK_RETURN) {
                    wchar_t buf[256]{};
                    GetWindowTextW(st->edit, buf, 255);
                    st->name = buf;
                    st->ok = !Trim(st->name).empty();
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

        RECT ownerClient{};
        GetClientRect(hwnd_, &ownerClient);
        POINT ownerTl{0, 0};
        ClientToScreen(hwnd_, &ownerTl);
        const RECT ownerScreen{
            ownerTl.x, ownerTl.y,
            ownerTl.x + ownerClient.right, ownerTl.y + ownerClient.bottom
        };
        const int ownerW = ownerScreen.right - ownerScreen.left;
        const int ownerH = ownerScreen.bottom - ownerScreen.top;
        const int dlgX = ownerScreen.left + (ownerW - dlgW) / 2;
        const int dlgY = ownerScreen.top + (ownerH - dlgH) / 2;

        // 与「自定义连点间隔」一致：半透明遮罩，不用 EnableWindow（关窗时主界面会闪）
        static constexpr BYTE kOverlayAlpha = 145;
        static constexpr wchar_t kOverlayClass[] = L"QuickScriptRenameRecordingOverlay";
        static bool overlayRegistered = false;
        if (!overlayRegistered) {
            WNDCLASSW owc{};
            owc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
                if (msg == WM_ERASEBKGND) return 1;
                if (msg == WM_PAINT) {
                    PAINTSTRUCT ps{};
                    HDC hdc = BeginPaint(hwnd, &ps);
                    RECT rc{};
                    GetClientRect(hwnd, &rc);
                    ::FillAlphaRect(hdc, rc, RGB(0, 0, 0), kOverlayAlpha);
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                return DefWindowProcW(hwnd, msg, wp, lp);
            };
            owc.hInstance = g_instance;
            owc.lpszClassName = kOverlayClass;
            owc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassW(&owc);
            overlayRegistered = true;
        }
        HWND overlay = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kOverlayClass, L"", WS_POPUP,
            ownerScreen.left, ownerScreen.top, ownerW, ownerH,
            hwnd_, nullptr, g_instance, nullptr);
        ShowWindow(overlay, SW_SHOWNA);
        UpdateWindow(overlay);

        HWND dlg = CreateWindowExW(WS_EX_TOPMOST, clsName, L"", WS_POPUP | WS_CLIPCHILDREN,
            dlgX, dlgY, dlgW, dlgH, hwnd_, nullptr, g_instance, &state);
        ShowWindow(dlg, SW_SHOW);
        UpdateWindow(dlg);
        SetForegroundWindow(dlg);
        SetFocus(state.edit);
        SendMessageW(state.edit, EM_SETSEL, 0, -1);
        MSG msg{};
        while (!state.done && IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
        if (IsWindow(overlay)) DestroyWindow(overlay);
        SetForegroundWindow(hwnd_);
        StDiscardSpuriousInputAfterModal(hwnd_);
        if (state.ok) {
            PersistRecordingRename(index, state.name);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void OnRecorderHomeClick(int x, int y) {
        if (PtIn(ImportRect(), x, y)) { ImportScript(); return; }
        if (PtIn(ExportRect(), x, y)) { ExportSelectedRecording(); return; }
        if (PtIn(TimerRect(), x, y)) { ShowScheduledTaskDialog(); return; }
        if (PtIn(RecorderModeRect(), x, y) && !recording_) {
            CycleRecorderInputMode();
            SaveHomeState();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (PtIn(CreateRect(), x, y)) {
            if (PtIn(RecorderBannerKeyRect(), x, y)) { CaptureGlobalHotkey(); return; }
            return;
        }
        if (ActiveHomeListMaxScroll() > 0 && PtIn(HomeScrollTrackRect(), x, y)) {
            RECT thumb = HomeScrollThumbRect();
            homeScrollbarDragging_ = true;
            homeScrollbarDragOffset_ = (thumb.bottom - thumb.top) / 2;
            UpdateHomeScrollFromThumb(y - homeScrollbarDragOffset_);
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
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
                    ConfirmDeleteRecording(i);
                    return;
                }
            }
            selectedRecording_ = (selectedRecording_ == i) ? -1 : i;
            if (selectedRecording_ >= 0) selectedScript_ = -1;  // 与鼠标宏选择互斥
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
                    ShowPromptInfo(msg.c_str());
                }
            } else {
                ShowPromptInfo(L"导出失败：无法创建 ZIP 文件，请检查保存路径是否有写入权限。");
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
        if (ActiveHomeListMaxScroll() > 0 && PtIn(HomeScrollTrackRect(), x, y)) {
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
            RECT edit = HomeCardEditBtn(r);
            RECT del = HomeCardDeleteBtn(r);
            if (x >= edit.left && x <= edit.right && y >= edit.top && y <= edit.bottom) { if (selectedScript_ == i) { selectedScript_ = -1; InvalidateRect(hwnd_, nullptr, FALSE); } else { ShowEditorFor(i, false); } return; }
            if (x >= del.left && x <= del.right && y >= del.top && y <= del.bottom) { ConfirmDelete(i); return; }
            if (x >= hot.left && x <= hot.right && y >= hot.top && y <= hot.bottom) { CaptureScriptHotkey(i); return; }
            selectedScript_ = selectedScript_ == i ? -1 : i;
            if (selectedScript_ >= 0) selectedRecording_ = -1;  // 与鼠标录制选择互斥
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    void OnHomeClick(int x, int y) {
        if (HandleHomeNavClick(x, y)) return;
        if (activeHomeTab_ == quickscript::MainTab::Clicker) { OnClickerHomeClick(x, y); return; }
        if (activeHomeTab_ == quickscript::MainTab::Recorder) { OnRecorderHomeClick(x, y); return; }
        if (activeHomeTab_ == quickscript::MainTab::ScriptCustom) {
            OnScriptCustomHomeClick(x, y);
            return;
        }
        OnMacroHomeClick(x, y);
    }

    void OnScriptCustomHomeClick(int x, int y) {
        if (PtIn(CreateRect(), x, y)) {
            OpenAgentDialog(-1);
            return;
        }
        if (ActiveHomeListMaxScroll() > 0 && PtIn(HomeScrollTrackRect(), x, y)) {
            RECT thumb = HomeScrollThumbRect();
            homeScrollbarDragging_ = true;
            homeScrollbarDragOffset_ = (thumb.bottom - thumb.top) / 2;
            UpdateHomeScrollFromThumb(y - homeScrollbarDragOffset_);
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        for (int i = 0; i < static_cast<int>(agentConversations_.size()); ++i) {
            RECT r = HomeCardRect(i);
            RECT list = HomeListRect();
            if (r.bottom < list.top || r.top > list.bottom) continue;
            if (PtIn(AgentConvChatRect(i), x, y)) { OpenAgentDialog(i); return; }
            if (PtIn(AgentConvDeleteRect(i), x, y)) { ConfirmDeleteAgentConversation(i); return; }
        }
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
        const std::wstring name = scripts_[static_cast<size_t>(index)].name;
        promptModal_.ShowConfirm(L"您确定要删除宏 \"" + name + L"\"\n吗？", [this](bool ok) {
            if (ok) ExecutePendingDelete();
            else pendingDeleteIndex_ = -1;
            StDiscardSpuriousInputAfterModal(hwnd_);
        });
    }

    void ExecutePendingDelete() {
        if (pendingDeleteIndex_ >= 0 && pendingDeleteIndex_ < static_cast<int>(scripts_.size())) {
            const auto scriptPath = scripts_[static_cast<size_t>(pendingDeleteIndex_)].path;
            DeleteUnreferencedImagesOfScript(scriptPath);
            DeleteFileW(scriptPath.c_str());
            selectedScript_ = -1;
            pendingDeleteIndex_ = -1;
            LoadScripts();
            ClampHomeScroll();
            RegisterAllHotkeys();
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void ConfirmDeleteRecording(int index) {
        if (index < 0 || index >= static_cast<int>(recordings_.size())) return;
        pendingRecordingDeleteIndex_ = index;
        const std::wstring name = recordings_[static_cast<size_t>(index)].name;
        promptModal_.ShowConfirm(L"您确定要删除录制 \"" + name + L"\"\n吗？", [this](bool ok) {
            if (ok) ExecutePendingRecordingDelete();
            else pendingRecordingDeleteIndex_ = -1;
            StDiscardSpuriousInputAfterModal(hwnd_);
        });
    }

    void ExecutePendingRecordingDelete() {
        if (pendingRecordingDeleteIndex_ >= 0
            && pendingRecordingDeleteIndex_ < static_cast<int>(recordings_.size())) {
            const auto recPath = recordings_[static_cast<size_t>(pendingRecordingDeleteIndex_)].path;
            DeleteUnreferencedImagesOfScript(recPath);
            DeleteFileW(recPath.c_str());
            if (selectedRecording_ == pendingRecordingDeleteIndex_) selectedRecording_ = -1;
            else if (selectedRecording_ > pendingRecordingDeleteIndex_) --selectedRecording_;
            pendingRecordingDeleteIndex_ = -1;
            LoadRecordings();
            ClampRecordingScroll();
            RegisterAllHotkeys();
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void ShowHotkeyMenuAt(RECT anchor) {
        HMENU menu = CreatePopupMenu();
        hotkeyMenuItems_ = {
            {kHotCustom, L"自定义", L"将您指定的按键设为启停热键"},
            {kHotLeft, L"鼠标左键", L"长按左键开始连点，松开停止（约0.2秒）"},
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
        ghHotkeyNeedKeyUp = false;
        ghHotkeyMouseHoldArmed = false;
        ghHotkeyMouseHoldDown = false;
        ghHotkeyMouseDownTick = 0;
        HINSTANCE inst    = GetModuleHandleW(nullptr);
        if (!ghHotkeyKbHook)
            ghHotkeyKbHook = SetWindowsHookExW(WH_KEYBOARD_LL, HotkeyKbProc, inst, 0);
        if (IsMouseVk(ghHotkeyVk) && !ghHotkeyMouseHook)
            ghHotkeyMouseHook = SetWindowsHookExW(WH_MOUSE_LL, HotkeyMouseProc, inst, 0);
        SetTimer(hwnd_, kHotkeyLatchSyncTimerId, 50, nullptr);
    }

    void UninstallGlobalHotkeyHooks() {
        ghHotkeyEnabled = false;
        KillTimer(hwnd_, kHotkeyLatchSyncTimerId);
        if (ghHotkeyKbHook)     { UnhookWindowsHookEx(ghHotkeyKbHook);     ghHotkeyKbHook     = nullptr; }
        if (ghHotkeyMouseHook)  { UnhookWindowsHookEx(ghHotkeyMouseHook);  ghHotkeyMouseHook  = nullptr; }
    }

    void RefreshGlobalHotkeyHooks() {
        ghHotkeyVk      = globalHotkey_.vk;
        ghHotkeyMods    = globalHotkey_.modifiers;
        ghHotkeyEnabled = globalHotkey_.enabled;
        ghHotkeyPending = false;
        ghHotkeyNeedKeyUp = false;
        ghHotkeyMouseHoldArmed = false;
        ghHotkeyMouseHoldDown = false;
        ghHotkeyMouseDownTick = 0;
        HINSTANCE inst  = GetModuleHandleW(nullptr);
        if (IsMouseVk(ghHotkeyVk) && !ghHotkeyMouseHook)
            ghHotkeyMouseHook = SetWindowsHookExW(WH_MOUSE_LL, HotkeyMouseProc, inst, 0);
        else if (!IsMouseVk(ghHotkeyVk) && ghHotkeyMouseHook)
            { UnhookWindowsHookEx(ghHotkeyMouseHook); ghHotkeyMouseHook = nullptr; }
    }

    void OnHotkey(int id, int holdCmd = 0) {
        // RegisterHotKey 与 LL 钩子双通道：用 NeedKeyUp 吃掉同一次物理按下的重复。
        // StopRecording/保存期间 UI 线程不泵消息，返回后队列里的第二通道不能再开录制。
        if (id == HOTKEY_GLOBAL_ID) {
            const bool holdKey = NeedsMouseHoldHotkey(ghHotkeyVk);

            // 左键松开停止：优先处理，不被 Handling 闩锁吞掉
            if (holdKey && holdCmd == static_cast<int>(kHotHoldStop)) {
                ghHotkeyNeedKeyUp = false;
                ghHotkeyPending = false;
                ghHotkeyMouseHoldArmed = false;
                ghHotkeyMouseHoldDown = false;
                if (clicking_) { StopClicking(); return; }
                if (recording_) { StopRecording(); return; }
                if (running_) { StopRun(); return; }
                return;
            }

            if (ghHotkeyHandling) return;
            SyncHotkeyLatches();

            // 仅左键：长按达阈值后启动（右键等仍走下方单击切换）
            if (holdKey && holdCmd == static_cast<int>(kHotHoldStart)) {
                if (ghHotkeyNeedKeyUp) return;
                const DWORD now = GetTickCount();
                if (now - lastHotkeyTick_ < 30u) return;
                lastHotkeyTick_ = now;
                ghHotkeyHandling = true;
                ghHotkeyNeedKeyUp = false;
                ghHotkeyPending = false;
                struct HoldStartGuard {
                    ~HoldStartGuard() {
                        // 不要 Flush 掉可能已排队的 kHotHoldStop；不要用 GetAsyncKeyState 判抬起
                        ghHotkeyNeedKeyUp = false;
                        ghHotkeyPending = false;
                        ghHotkeyHandling = false;
                    }
                } holdGuard;
                if (clicking_ || recording_ || running_) return;
                if (activeHomeTab_ != quickscript::MainTab::Clicker) {
                    if (selectedScript_ >= 0) {
                        RunScriptByIndex(selectedScript_);
                    } else if (selectedRecording_ >= 0 && selectedRecording_ < static_cast<int>(recordings_.size())) {
                        LoadScriptFile(recordings_[static_cast<size_t>(selectedRecording_)].path);
                        RunCurrentActions();
                    } else if (activeHomeTab_ == quickscript::MainTab::Recorder) {
                        ToggleRecording();
                    }
                    return;
                }
                StartClicking();
                return;
            }

            if (ghHotkeyNeedKeyUp) return;
            const DWORD now = GetTickCount();
            const bool busy = clicking_ || recording_ || running_
                || ghHotkeySessionBusy.load(std::memory_order_relaxed);
            const DWORD minGap = busy ? 10u : 60u;
            if (now - lastHotkeyTick_ < minGap) return;
            lastHotkeyTick_ = now;
            ghHotkeyHandling = true;
            ghHotkeyNeedKeyUp = true;
            ghHotkeyPending = false;
            struct HotkeyHandlingGuard {
                HWND hwnd;
                ~HotkeyHandlingGuard() {
                    FlushQueuedGlobalHotkeys(hwnd);
                    // 处理期间 KEYUP 可能已到：若键已松开则允许下次；否则保持到抬起。
                    SyncHotkeyLatches();
                    if ((ghHotkeyVk && (GetAsyncKeyState(static_cast<int>(ghHotkeyVk)) & 0x8000)) == 0) {
                        ghHotkeyNeedKeyUp = false;
                        ghHotkeyPending = false;
                    } else {
                        ghHotkeyNeedKeyUp = true;
                    }
                    ghHotkeyHandling = false;
                }
            } handlingGuard{hwnd_};

            if (clicking_) { StopClicking(); return; }
            if (recording_) { StopRecording(); return; }
            if (running_) { StopRun(); return; }
            if (activeHomeTab_ != quickscript::MainTab::Clicker) {
                if (selectedScript_ >= 0) {
                    RunScriptByIndex(selectedScript_);
                } else if (selectedRecording_ >= 0 && selectedRecording_ < static_cast<int>(recordings_.size())) {
                    LoadScriptFile(recordings_[static_cast<size_t>(selectedRecording_)].path);
                    RunCurrentActions();
                } else if (activeHomeTab_ == quickscript::MainTab::Recorder) {
                    ToggleRecording();
                }
                return;
            }
            ToggleClicker();
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

    void ShowScheduledTaskDialog() {
        ScheduledTaskDialog dlg;
        dlg.Show(hwnd_, scheduledTasks_);
        if (IsWindow(hwnd_)) {
            SetForegroundWindow(hwnd_);
        }
        StDiscardSpuriousInputAfterModal(hwnd_);
    }

    void OpenAgentDialog(int restoreIndex = -1) {
        agentDialogs_.erase(
            std::remove_if(agentDialogs_.begin(), agentDialogs_.end(),
                [](const std::unique_ptr<AgentDialog>& d) { return !d || !d->IsAlive(); }),
            agentDialogs_.end());

        LoadAppSettings(appSettings_);

        AgentDialog::RestoreData restore;
        const AgentDialog::RestoreData* restorePtr = nullptr;
        if (restoreIndex >= 0 && restoreIndex < static_cast<int>(agentConversations_.size())) {
            AgentConversationRecord rec;
            if (LoadAgentConversationRecord(agentConversations_[static_cast<size_t>(restoreIndex)].id, rec)) {
                restore.id = rec.meta.id;
                restore.name = rec.meta.name;
                restore.createdTime = rec.meta.createdTime;
                restore.messages = std::move(rec.messages);
                restore.chatDisplay = std::move(rec.chatDisplay);
                restorePtr = &restore;
            }
        }

        auto dlg = std::make_unique<AgentDialog>();
        if (!dlg->Show(hwnd_, appSettings_.ai, restorePtr,
            [this](AgentConversationSavePayload&& payload) {
                OnAgentConversationClosed(std::move(payload));
            })) return;
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

    bool IsEditorWindowModeActive() const {
        return popupMode_.sel == 1 || popupMode_.sel == 2;
    }

    bool IsEditorMacroHeaderHwnd(HWND hwnd) const {
        return hwnd == labelMacro_ || hwnd == name_ || hwnd == mode_
            || hwnd == labelBreakoutTime_ || hwnd == breakoutTimeEdit_
            || hwnd == wmSelectMethod_ || hwnd == wmSpecifyWindowBtn_
            || hwnd == wmTargetPathEdit_ || hwnd == wmTargetBrowseBtn_ || hwnd == wmTargetCrosshairBtn_;
    }

    RECT wmModeLabelRect_{};
    RECT wmSelectMethodLabelRect_{};

    int ScaleEditorX(int designX) const {
        return ScaleX(designX);
    }

    int ScaleEditorY(int designY) const {
        return ScaleY(designY);
    }

    int ScaleEditorW(int designW) const {
        return std::max(1, ScaleX(designW));
    }

    int ScaleEditorH(int designH) const {
        return std::max(1, ScaleY(designH));
    }

    RECT EditorMacroHeaderTextRect() const {
        if (labelMacro_) return WindowClientRect(labelMacro_);
        const int y = ScaleEditorY(kEditorMacroHeaderRowY);
        const int h = ScaleEditorH(kEditorMacroHeaderRowH);
        return RECT{0, y, 0, y + h};
    }

    RECT EditorRowLabelTextRect(int rowY, int rowH) const {
        const int left = ScaleEditorX(kEditorMacroNameLabelX);
        const int right = ScaleEditorX(kEditorMacroNameEditX) - ScaleEditorX(2);
        return RECT{left, rowY, right, rowY + rowH};
    }

    void UpdateEditorWindowModeChrome() {
        if (page_ != Page::Editor) {
            HideEditorMacroHeaderControls();
            return;
        }
        if (labelMacro_) ShowWindow(labelMacro_, SW_SHOW);
        if (name_) ShowWindow(name_, SW_SHOW);
        // mode_ 必须保持隐藏：仅作命中锚点，外观由 PaintEditor/DrawEditorCombo 自绘
        if (mode_) ShowWindow(mode_, SW_HIDE);
        const bool wm = IsEditorWindowModeActive();
        const bool showBreakout = !wm;
        if (labelBreakoutTime_) ShowWindow(labelBreakoutTime_, SW_HIDE);
        if (breakoutTimeEdit_) ShowWindow(breakoutTimeEdit_, showBreakout ? SW_SHOW : SW_HIDE);
        // 下拉锚点控件仅用于命中测试，外观由父窗口自绘
        if (wmSelectMethod_) ShowWindow(wmSelectMethod_, SW_HIDE);
        if (wmSpecifyWindowBtn_) ShowWindow(wmSpecifyWindowBtn_, SW_HIDE);
        // 目标程序行显隐由下方 wm 分支按选择窗口方式决定。
        if (wmTargetPathEdit_) ShowWindow(wmTargetPathEdit_, SW_HIDE);
        if (wmTargetBrowseBtn_) ShowWindow(wmTargetBrowseBtn_, SW_HIDE);
        if (wmTargetCrosshairBtn_) ShowWindow(wmTargetCrosshairBtn_, SW_HIDE);

        const int rowY = ScaleEditorY(kEditorMacroHeaderRowY);
        const int rowH = ScaleEditorH(kEditorMacroHeaderRowH);
        const int comboH = ScaleEditorH(kEditorModeComboH);
        const int comboY = rowY + std::max(0, (rowH - comboH) / 2);
        const int contentLeft = ScaleEditorX(kEditorMacroNameEditX);
        const int gap = ScaleEditorX(kEditorWmHeaderGap);
        const int btnGap = ScaleEditorX(kEditorWmTargetBtnGap);

        if (labelMacro_) {
            MoveWindow(labelMacro_, ScaleEditorX(kEditorMacroNameLabelX), rowY, ScaleEditorW(kEditorMacroNameLabelW), rowH, FALSE);
        }
        const int breakoutRowY = ScaleEditorY(kEditorBreakoutRowY);
        if (breakoutTimeEdit_) {
            MoveWindow(breakoutTimeEdit_, ScaleEditorX(kEditorBreakoutEditX), breakoutRowY,
                ScaleEditorW(kEditorBreakoutEditW), rowH, FALSE);
        }

        const int nameX = contentLeft;
        const int modeW = ScaleEditorW(kEditorModeComboW);
        const int modeX = ScaleEditorX(kEditorComboRight) - modeW;
        const int comboLabelGap = ScaleEditorX(6);
        const int modeLabelW = ScaleEditorW(50);

        if (wm) {
            const bool showSpec = popupWmSelectMethod_.sel == 2;
            // 启动时选择 / 鼠标位置：运行时再选窗，不显示「目标程序」行。
            const bool showTargetPath = popupWmSelectMethod_.sel == 2
                || popupWmSelectMethod_.sel == 3;
            if (wmSpecifyWindowBtn_) {
                ShowWindow(wmSpecifyWindowBtn_, showSpec ? SW_SHOW : SW_HIDE);
            }
            if (wmTargetPathEdit_) ShowWindow(wmTargetPathEdit_, showTargetPath ? SW_SHOW : SW_HIDE);
            if (wmTargetBrowseBtn_) ShowWindow(wmTargetBrowseBtn_, showTargetPath ? SW_SHOW : SW_HIDE);
            if (wmTargetCrosshairBtn_) {
                ShowWindow(wmTargetCrosshairBtn_, showTargetPath ? SW_SHOW : SW_HIDE);
                if (showTargetPath) {
                    const wchar_t* crossLabel = popupMode_.sel == 2 ? L"准星绑定窗口" : L"准星找程序";
                    SetWindowTextW(wmTargetCrosshairBtn_, crossLabel);
                }
            }

            if (mode_) MoveWindow(mode_, modeX, comboY, modeW, comboH, FALSE);
            const int modeLabelLeft = modeX - comboLabelGap - modeLabelW;
            wmModeLabelRect_ = RECT{modeLabelLeft, rowY, modeX - comboLabelGap, rowY + rowH};

            // 自右向左：模式 | [指定窗口类] | 选择窗口方式 | 宏名称（占满剩余宽度）
            int cursor = modeLabelLeft - gap;
            if (showSpec) {
                const int specW = ScaleEditorW(kEditorWmSpecifyBtnW);
                cursor -= specW;
                if (wmSpecifyWindowBtn_) MoveWindow(wmSpecifyWindowBtn_, cursor, rowY, specW, rowH, FALSE);
                cursor -= gap;
            }

            const int selLabelW = ScaleEditorW(kEditorWmSelectMethodLabelW);
            const int preferredSelW = ScaleEditorW(kEditorWmSelectMethodComboW);
            const int minNameW = ScaleEditorW(72);
            const int minSelW = ScaleEditorW(140);

            // 先保证选择方式文案完整，剩余宽度全部给宏名称。
            int selW = preferredSelW;
            int nameW = cursor - contentLeft - gap - selLabelW - selW;
            if (nameW < minNameW) {
                selW = std::max(minSelW, selW - (minNameW - nameW));
                nameW = cursor - contentLeft - gap - selLabelW - selW;
            }
            nameW = std::max(minNameW, nameW);

            cursor -= selW;
            const int selComboLeft = cursor;
            cursor -= selLabelW;
            wmSelectMethodLabelRect_ = RECT{cursor, rowY, selComboLeft, rowY + rowH};
            if (wmSelectMethod_) MoveWindow(wmSelectMethod_, selComboLeft, comboY, selW, comboH, FALSE);

            cursor -= gap;
            if (name_) MoveWindow(name_, nameX, rowY, nameW, rowH, FALSE);

            if (showTargetPath) {
                const int targetContentRight = ScaleEditorX(kEditorWmContentRight);
                const int targetRowY = rowY + rowH + ScaleEditorY(kEditorWmRowGap);
                const int crossW = ScaleEditorW(kEditorWmTargetCrosshairW);
                const int browseW = ScaleEditorW(kEditorWmTargetBrowseW);
                const int crossX = targetContentRight - crossW;
                const int browseX = crossX - btnGap - browseW;
                const int pathW = std::max(ScaleEditorW(120), browseX - btnGap - contentLeft);

                if (wmTargetPathEdit_) MoveWindow(wmTargetPathEdit_, contentLeft, targetRowY, pathW, rowH, FALSE);
                if (wmTargetBrowseBtn_) MoveWindow(wmTargetBrowseBtn_, browseX, targetRowY, browseW, rowH, FALSE);
                if (wmTargetCrosshairBtn_) MoveWindow(wmTargetCrosshairBtn_, crossX, targetRowY, crossW, rowH, FALSE);
            }
        } else {
            wmModeLabelRect_ = {};
            wmSelectMethodLabelRect_ = {};
            if (wmSpecifyWindowBtn_) ShowWindow(wmSpecifyWindowBtn_, SW_HIDE);
            const int nameW = ScaleEditorW(kEditorMacroNameEditW);
            if (name_) MoveWindow(name_, nameX, rowY, nameW, rowH, FALSE);
            if (mode_) MoveWindow(mode_, modeX, comboY, modeW, comboH, FALSE);
        }
        CenterModernSingleLineEditText(name_);
        CenterModernSingleLineEditText(breakoutTimeEdit_);
        CenterModernSingleLineEditText(wmTargetPathEdit_);
        if (page_ == Page::Editor) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SyncScriptWindowModeFromEditor() {
        if (page_ != Page::Editor) return;
        scriptWindowMode_.enabled = IsEditorWindowModeActive();
        scriptWindowMode_.executionKind = popupMode_.sel == 2
            ? windowmode::WindowModeExecutionKind::BackgroundWindow
            : windowmode::WindowModeExecutionKind::HiddenDesktop;
        if (popupWmSelectMethod_.sel >= 0
            && popupWmSelectMethod_.sel < static_cast<int>(popupWmSelectMethod_.items.size())) {
            scriptWindowMode_.selectMethod =
                static_cast<windowmode::WindowSelectMethod>(popupWmSelectMethod_.sel);
        }
        if (wmTargetPathEdit_) {
            scriptWindowMode_.targetExePath = Trim(GetText(wmTargetPathEdit_));
        }
        // 「不选择窗口」时清掉类名/标题，避免残留身份字段让绑定走错路径。
        if (scriptWindowMode_.selectMethod == windowmode::WindowSelectMethod::NoSelect) {
            scriptWindowMode_.windowName.clear();
            scriptWindowMode_.windowClassName.clear();
            scriptWindowMode_.childWindowClassName.clear();
            scriptWindowMode_.targetWindowTitle.clear();
            scriptWindowMode_.targetPickX = 0;
            scriptWindowMode_.targetPickY = 0;
        }
        // 有目标路径时始终允许自动打开：窗口不存在时由 BeginRun 启动。
        scriptWindowMode_.autoLaunchTarget =
            windowmode::ShouldAutoLaunchTarget(scriptWindowMode_);
        if (!scriptWindowMode_.windowName.empty()) {
            scriptWindowMode_.targetWindowTitle = scriptWindowMode_.windowName;
        }
    }

    void SyncWindowModeUiFromScript() {
        popupMode_.sel = !scriptWindowMode_.enabled ? 0
            : (scriptWindowMode_.executionKind == windowmode::WindowModeExecutionKind::BackgroundWindow ? 2 : 1);
        SetPopupSel(popupMode_, mode_, popupMode_.sel);
        const int methodSel = static_cast<int>(scriptWindowMode_.selectMethod);
        if (methodSel >= 0 && methodSel < static_cast<int>(popupWmSelectMethod_.items.size())) {
            popupWmSelectMethod_.sel = methodSel;
        }
        if (wmSelectMethod_ && popupWmSelectMethod_.sel >= 0
            && popupWmSelectMethod_.sel < static_cast<int>(popupWmSelectMethod_.items.size())) {
            SetText(wmSelectMethod_, popupWmSelectMethod_.items[static_cast<size_t>(popupWmSelectMethod_.sel)]);
        }
        if (wmTargetPathEdit_) SetText(wmTargetPathEdit_, scriptWindowMode_.targetExePath);
        if (page_ == Page::Editor) UpdateEditorWindowModeChrome();
    }

    void ShowWindowClassPickDialog() {
        windowmode::WindowPickResult pick{};
        pick.windowTitle = scriptWindowMode_.windowName;
        pick.windowClassName = scriptWindowMode_.windowClassName;
        pick.childWindowClassName = scriptWindowMode_.childWindowClassName;
        pick.processPath = scriptWindowMode_.targetExePath;
        if (!wmPickDialog_.Show(hwnd_, pick)) return;
        scriptWindowMode_.windowName = pick.windowTitle;
        scriptWindowMode_.windowClassName = pick.windowClassName;
        scriptWindowMode_.childWindowClassName = pick.childWindowClassName;
        scriptWindowMode_.targetWindowTitle = pick.windowTitle;
        scriptWindowMode_.targetPickX = pick.pickX;
        scriptWindowMode_.targetPickY = pick.pickY;
        if (!pick.processPath.empty()) scriptWindowMode_.targetExePath = pick.processPath;
        if (!pick.documentPath.empty()) {
            scriptWindowMode_.launchArgs = L"\"" + pick.documentPath + L"\"";
        }
        if (wmTargetPathEdit_ && !pick.processPath.empty()) SetText(wmTargetPathEdit_, pick.processPath);
        SyncScriptWindowModeFromEditor();
    }

    void FinalizeWindowModeAutoLaunch(windowmode::WindowModeScriptConfig& cfg) const {
        cfg.autoLaunchTarget = windowmode::ShouldAutoLaunchTarget(cfg);
    }

    static void ApplyWindowPickToConfig(windowmode::WindowModeScriptConfig& cfg,
        const windowmode::WindowPickResult& pick) {
        cfg.windowName = pick.windowTitle;
        cfg.windowClassName = pick.windowClassName;
        cfg.childWindowClassName = pick.childWindowClassName;
        cfg.targetWindowTitle = pick.windowTitle;
        cfg.targetPickX = pick.pickX;
        cfg.targetPickY = pick.pickY;
        if (!pick.processPath.empty()) cfg.targetExePath = pick.processPath;
        if (!pick.documentPath.empty()) {
            cfg.launchArgs = L"\"" + pick.documentPath + L"\"";
        } else if (cfg.launchArgs.empty() && !pick.windowTitle.empty()) {
            // 拾取时没有完整路径时，至少把标题里的文件名写入 launchArgs，运行时再按脚本目录解析。
            std::wstring stem = pick.windowTitle;
            const auto dash = stem.find(L" - ");
            if (dash != std::wstring::npos) stem = stem.substr(0, dash);
            while (!stem.empty() && (stem.front() == L'*' || stem.front() == L' ')) stem.erase(stem.begin());
            while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'\t')) stem.pop_back();
            if (!stem.empty() && stem.find(L'.') != std::wstring::npos
                && stem != L"无标题" && _wcsicmp(stem.c_str(), L"Untitled") != 0) {
                cfg.launchArgs = L"\"" + stem + L"\"";
            }
        }
    }

    static void ApplyWindowInfoToConfig(windowmode::WindowModeScriptConfig& cfg,
        const WindowInfoFromPoint& info) {
        if (!info.windowTitle.empty()) cfg.windowName = info.windowTitle;
        if (!info.windowClassName.empty()) cfg.windowClassName = info.windowClassName;
        if (!info.childWindowClassName.empty()) cfg.childWindowClassName = info.childWindowClassName;
        if (!info.processPath.empty()) cfg.targetExePath = info.processPath;
        if (!info.documentPath.empty()) cfg.launchArgs = L"\"" + info.documentPath + L"\"";
        cfg.targetWindowTitle = cfg.windowName;
        cfg.targetPickX = info.x;
        cfg.targetPickY = info.y;
    }

    bool IsOwnProcessWindowAt(int x, int y) const {
        POINT pt{x, y};
        HWND pointHwnd = WindowFromPoint(pt);
        if (!pointHwnd) return false;
        HWND root = GetAncestor(pointHwnd, GA_ROOT);
        if (!root) root = pointHwnd;
        DWORD pid = 0;
        GetWindowThreadProcessId(root, &pid);
        return pid != 0 && pid == GetCurrentProcessId();
    }

    void PumpMessagesFor(DWORD ms) const {
        const DWORD deadline = GetTickCount() + ms;
        while (static_cast<int>(deadline - GetTickCount()) > 0) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(msg.wParam));
                    return;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(30);
        }
    }

    /// 解析「选择窗口方式」：填充 cfg 的窗口身份字段。可在编辑器运行 / 热键 / 定时任务路径复用。
    bool ResolveWindowModeSelectMethod(windowmode::WindowModeScriptConfig& cfg) {
        if (!cfg.enabled) return true;

        using SM = windowmode::WindowSelectMethod;
        const bool background =
            cfg.executionKind == windowmode::WindowModeExecutionKind::BackgroundWindow;

        switch (cfg.selectMethod) {
        case SM::SelectOnStartup: {
            // 不要先隐藏主窗口再弹选窗（owned 弹窗会跟着不显示）。
            windowmode::WindowPickResult pick{};
            pick.processPath = cfg.targetExePath;
            const bool accepted = wmPickDialog_.Show(hwnd_, pick);
            if (!accepted) {
                promptModal_.ShowInfo(L"已取消选择目标窗口，宏未运行。");
                return false;
            }
            if (pick.windowClassName.empty() && pick.windowTitle.empty()
                && pick.pickX == 0 && pick.pickY == 0) {
                promptModal_.ShowInfo(L"未获取到有效窗口，请拖动准星点选目标窗口后再确定。");
                return false;
            }
            ApplyWindowPickToConfig(cfg, pick);
            break;
        }
        case SM::MousePositionOnStartup: {
            // 编辑器可见时先隐藏并等待，避免采到本软件；热键启动时通常已在目标窗上，立即采集。
            const bool wasVisible = hwnd_ && IsWindow(hwnd_) && IsWindowVisible(hwnd_);
            if (wasVisible) {
                ShowWindow(hwnd_, SW_HIDE);
                PumpMessagesFor(1500);
            }
            POINT pt{};
            GetCursorPos(&pt);
            if (IsOwnProcessWindowAt(pt.x, pt.y)) {
                if (wasVisible && hwnd_ && IsWindow(hwnd_)) {
                    ShowWindow(hwnd_, SW_SHOW);
                    SetForegroundWindow(hwnd_);
                }
                promptModal_.ShowInfo(
                    L"鼠标位于本软件窗口上，无法作为目标。\n"
                    L"请将鼠标移到目标窗口后，再通过热键启动宏；或改用「启动时选择窗口」。");
                return false;
            }
            const auto info = GetWindowInfoFromPoint(pt.x, pt.y);
            if (wasVisible && hwnd_ && IsWindow(hwnd_)) {
                ShowWindow(hwnd_, SW_SHOW);
                SetForegroundWindow(hwnd_);
            }
            if (info.windowClassName.empty() && info.windowTitle.empty()) {
                promptModal_.ShowInfo(L"未能根据鼠标位置获取到窗口，请将鼠标移到目标窗口后再试。");
                return false;
            }
            ApplyWindowInfoToConfig(cfg, info);
            break;
        }
        case SM::UseEditorWindowClass: {
            if (cfg.windowClassName.empty()
                && cfg.windowName.empty()
                && cfg.targetWindowTitle.empty()
                && cfg.targetPickX == 0 && cfg.targetPickY == 0) {
                promptModal_.ShowInfo(
                    L"请先点击「指定窗口类」配置目标窗口，或改用其他选择窗口方式。");
                return false;
            }
            break;
        }
        case SM::NoSelect: {
            cfg.windowName.clear();
            cfg.windowClassName.clear();
            cfg.childWindowClassName.clear();
            cfg.targetWindowTitle.clear();
            cfg.targetPickX = 0;
            cfg.targetPickY = 0;
            break;
        }
        }

        // 身份解析完成后再做模式级校验。
        const bool hasIdentity = !cfg.windowClassName.empty()
            || !cfg.windowName.empty()
            || !cfg.targetWindowTitle.empty()
            || cfg.targetPickX != 0 || cfg.targetPickY != 0;

        if (cfg.selectMethod == SM::NoSelect) {
            if (cfg.targetExePath.empty()) {
                promptModal_.ShowInfo(L"请填写目标程序路径，可使用「浏览」或「准星找程序」。");
                return false;
            }
        } else if (cfg.selectMethod == SM::UseEditorWindowClass) {
            // 窗口不存在时要能自动打开，因此需要目标程序路径。
            if (cfg.targetExePath.empty()) {
                promptModal_.ShowInfo(
                    L"请填写目标程序路径（窗口未打开时将自动启动），或用「指定窗口类」拾取带路径的窗口。");
                return false;
            }
        } else if (!background && !hasIdentity && cfg.targetExePath.empty()) {
            promptModal_.ShowInfo(L"未获取到目标窗口信息。");
            return false;
        }

        if (background
            && cfg.targetExePath.empty()
            && !hasIdentity) {
            promptModal_.ShowInfo(
                L"后台窗口模式请用「启动时选择窗口」「准星绑定窗口」或「指定窗口类」选择目标窗口。");
            return false;
        }

        FinalizeWindowModeAutoLaunch(cfg);
        // 再次强制：防止上游 JSON / 旧字段把 autoLaunch 关掉。
        cfg.autoLaunchTarget = windowmode::ShouldAutoLaunchTarget(cfg);
        if (!cfg.allowForegroundInputFallback) {
            cfg.allowForegroundInputFallback = appSettings_.windowMode.allowForegroundInputFallback;
        }
        return true;
    }

    bool PrepareWindowModeRunConfig(windowmode::WindowModeScriptConfig& cfg) {
        SyncScriptWindowModeFromEditor();
        cfg = scriptWindowMode_;
        if (!ResolveWindowModeSelectMethod(cfg)) return false;
        cfg.autoLaunchTarget = windowmode::ShouldAutoLaunchTarget(cfg);
        return true;
    }

    void ShowSettingsDialog() {
        if (settingsDialog_ && settingsDialog_->IsAlive()) {
            SetForegroundWindow(settingsDialog_->Hwnd());
            return;
        }
        settingsDialog_.reset();
        // 始终从磁盘加载最新设置后再打开，确保 AI 助手修改后立即可见
        LoadAppSettings(appSettings_);
        settingsDialog_ = std::make_unique<SettingsDialog>();
        if (!settingsDialog_->Show(hwnd_, appSettings_, [this]() {
            ApplyThemeAndRefreshUi();
            ApplyDebugWindowSetting();
        })) {
            settingsDialog_.reset();
        }
    }

    void RunActionsFromPath(const std::wstring& path) {
        if (running_ || path.empty()) return;
        const ScriptFileData data = LoadScriptFileData(path, false);
        if (data.actions.empty()) return;
        windowmode::WindowModeScriptConfig wmCfg = data.windowMode;
        if (!ResolveWindowModeSelectMethod(wmCfg)) return;

        CoordMeta execMeta = ScriptCoordMetaForExecution(data.coordMeta);
        std::vector<ScriptAction> execActions =
            PrepareScriptActionsForExecution(data.actions, execMeta);

        const double breakoutTime = EffectiveBreakoutTimeSeconds(data);
        StartActionsWorker(execActions, path, wmCfg, execMeta, breakoutTime,
            data.hotkey);
    }

    int RandomInt(int maxValue) { if (maxValue <= 0) return 0; std::uniform_int_distribution<int> dist(-maxValue, maxValue); return dist(rng_); }
    double RandomDelay(double maxValue) { if (maxValue <= 0) return 0; std::uniform_real_distribution<double> dist(0.0, maxValue); return dist(rng_); }
    double ParseBreakoutTimeFromEditor() const {
        if (!breakoutTimeEdit_) return 0;
        const std::wstring text = Trim(GetText(breakoutTimeEdit_));
        if (text.empty()) return 0;
        try {
            size_t consumed = 0;
            const double value = std::stod(text, &consumed);
            if (consumed == 0) return 0;
            return NormalizeBreakoutTimeSeconds(value);
        } catch (...) {
            return 0;
        }
    }

    static std::wstring FormatBreakoutTimeForEditor(double seconds) {
        if (seconds <= 0) return L"0";
        std::wstringstream ss;
        ss << seconds;
        std::wstring out = ss.str();
        if (auto dot = out.find(L'.'); dot != std::wstring::npos) {
            while (!out.empty() && out.back() == L'0') out.pop_back();
            if (!out.empty() && out.back() == L'.') out.pop_back();
        }
        return out.empty() ? L"0" : out;
    }

    bool BreakoutTriggered() const {
        return workerBreakoutTime_ > 0
            && breakoutUserInput_.load(std::memory_order_relaxed);
    }

    void SleepInterruptible(double seconds) {
        if (seconds <= 0.0 || stopFlag_) return;
        using clock = std::chrono::steady_clock;
        const auto end = clock::now()
            + std::chrono::duration_cast<clock::duration>(
                std::chrono::duration<double>(seconds));
        while (!stopFlag_ && !BreakoutTriggered()) {
            const auto now = clock::now();
            if (now >= end) break;
            const auto rem = end - now;
            // 旧实现固定 sleep 10ms：短等待（录制相对移动常见 4~8ms）会被拖糊成台阶感
            if (rem > std::chrono::milliseconds(2)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // rem ≤ 2ms：忙等收尾，保证亚毫秒级对齐
        }
    }

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
        if (action.type == ActionType::EndLoop
            && !HasLoopParentAt(actions_, static_cast<size_t>(selectedIndex_), existing.indent)) {
            return;
        }
        action.originalNo = existing.originalNo;
        action.indent = existing.indent;
        actions_[static_cast<size_t>(selectedIndex_)] = action;
    }

    void RunCurrentActions() {
        if (running_) {
            // 避免「点了没反应」：正在运行时再次点击视为请求停止。
            StopRun();
            if (macroDebugWindow_.IsCreated()) {
                macroDebugWindow_.AppendLog(L"脚本正在运行，已请求停止");
            }
            return;
        }
        SyncFormIntoActionsBeforeRun();
        EnsureEditorFullyParsed();
        windowmode::WindowModeScriptConfig runCfg{};
        if (!PrepareWindowModeRunConfig(runCfg)) return;
        // 不把启动时解析出的临时窗口身份写回编辑器，避免污染「选择窗口方式」与保存内容。
        // 有路径且允许自动打开时，不要因为“当前没有窗口”而拦截运行——真正打开交给 BeginRun。
        if (runCfg.enabled && appSettings_.windowMode.blockRunWhenUnhealthy
            && !windowmode::ShouldAutoLaunchTarget(runCfg)) {
            std::wstring err;
            if (!windowmode::WindowModeExecutor::CheckRunHealth(runCfg, err)) {
                promptModal_.ShowInfo(err.empty() ? L"窗口模式未就绪" : err);
                return;
            }
        }

        CoordMeta execMeta = ScriptCoordMetaForExecution(loadedCoordMeta_);
        std::vector<ScriptAction> execActions = actions_;
        SyncNormFieldsFromPixels(execActions,
            CaptureCurrentCoordMeta(runCfg.enabled ? &runCfg : nullptr));
        execActions = PrepareScriptActionsForExecution(execActions, execMeta);

        const double breakoutTime = runCfg.enabled ? 0.0 : ParseBreakoutTimeFromEditor();
        Hotkey scriptHotkey{};
        if (currentScriptIndex_ >= 0
            && currentScriptIndex_ < static_cast<int>(scripts_.size())) {
            scriptHotkey = scripts_[static_cast<size_t>(currentScriptIndex_)].hotkey;
        }
        StartActionsWorker(execActions, currentPath_, runCfg, execMeta, breakoutTime,
            scriptHotkey);
    }

    void StartActionsWorker(const std::vector<ScriptAction>& actions, const std::wstring& selfPath,
        const windowmode::WindowModeScriptConfig& wmCfg = {},
        const CoordMeta& execCoordMeta = {},
        double breakoutTime = 0,
        const Hotkey& scriptHotkey = {}) {
        if (running_) {
            StopRun();
            if (macroDebugWindow_.IsCreated()) {
                macroDebugWindow_.AppendLog(L"脚本正在运行，已请求停止");
            }
            return;
        }
        running_ = true; stopFlag_ = false; breakoutUserInput_ = false; breakoutPaused_ = false;
        ghHotkeySessionBusy.store(true, std::memory_order_relaxed);
        MouseInputRouter::Instance().Configure();
        BeginHighResTimer(); // 宏内短等待（录制轨迹）需要 1ms 定时器精度
        breakoutTaskbarShown_ = false;
        breakoutUiVisibleOnScreen_ = false;
        breakoutPlacement_ = BreakoutTaskbarPlacement{};
        workerBreakoutTime_ = (!wmCfg.enabled && breakoutTime > 0) ? breakoutTime : 0;
        aiHttpAbort_.Clear();
        wasVisibleBeforeRun_ = IsWindowVisible(hwnd_) == TRUE;
        wasMinimizedBeforeRun_ = IsIconic(hwnd_) == TRUE;
        runSavedRectValid_ = GetWindowRestoredRect(hwnd_, &runSavedRect_);
        runSavedExStyle_ = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        SetWindowCloaked(hwnd_, false);
        CloseEditorPopup(); CancelQuickInputTip();
        if (appSettings_.other.playSoundOnStart) MessageBeep(MB_OK);
        EnsureTrayIcon();
        if (wasMinimizedBeforeRun_) {
            // 运行期间主窗口不保留绿色按钮；辅助窗口不受影响并继续显示绿色按钮。
            ShowWindow(hwnd_, SW_HIDE);
        } else if (appSettings_.other.autoHideMainWindow) {
            // 自动隐藏前让 DWM 保存真实界面，供脱离时的最小化任务栏预览使用。
            PrepareHwndTaskbarLivePreview(hwnd_);
            ShowWindow(hwnd_, SW_HIDE);
        }
        UpdateStatusTip();
        windowmode::SetWindowModeLogSink([this](const std::wstring& line) {
            if (macroDebugWindow_.IsCreated()) macroDebugWindow_.AppendLog(line);
        });
        if (macroDebugWindow_.IsCreated()) macroDebugWindow_.ClearLog();
        breakoutHookState_ = BreakoutHookState{};
        breakoutHookState_.running = &running_;
        breakoutHookState_.simulatingDepth = &simulatingInputDepth_;
        breakoutHookState_.userInput = &breakoutUserInput_;
        if (workerBreakoutTime_ > 0) {
            if (globalHotkey_.enabled && globalHotkey_.vk) {
                breakoutHookState_.ignoreHotkeys.push_back(globalHotkey_);
            }
            for (const auto& script : scripts_) {
                if (script.hotkey.enabled && script.hotkey.vk) {
                    breakoutHookState_.ignoreHotkeys.push_back(script.hotkey);
                }
            }
            if (scriptHotkey.enabled && scriptHotkey.vk) {
                breakoutHookState_.ignoreHotkeys.push_back(scriptHotkey);
            }
            breakout_input::InstallBreakoutHooks(breakoutHookState_);
        }
        worker_ = std::thread([this, actions, selfPath, wmCfg, execCoordMeta]() {
            if (wmCfg.enabled) {
                windowmode::WindowModeLog(
                    wmCfg.executionKind == windowmode::WindowModeExecutionKind::BackgroundWindow
                        ? L"[窗口模式] 后台窗口模式：工作线程已启动"
                        : L"[窗口模式] 窗口模式：工作线程已启动");
            }
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
            UINT heldKeyVk = 0; // 兼容单键跟踪；多键用 heldKeys
            std::unordered_set<UINT> heldKeys;
            HBITMAP lockedScreen_ = nullptr;
            int lockedVirtX_ = 0;
            int lockedVirtY_ = 0;

            // 计算模板缩放比例（用于找图跨分辨率适配，嵌套宏可切换 activeCoordMeta）
            int execTargetW = 0, execTargetH = 0, execVirtX = 0, execVirtY = 0;
            GetVirtualScreenBounds(execVirtX, execVirtY, execTargetW, execTargetH);
            CoordMeta activeCoordMeta = execCoordMeta;
            auto currentTmplScale = [&]() -> TemplateScale {
                if (activeCoordMeta.refWidth <= 0 || activeCoordMeta.refHeight <= 0) {
                    return TemplateScale{};
                }
                return ComputeTemplateScale(activeCoordMeta, execTargetW, execTargetH);
            };

            windowmode::WindowModeExecutor wmExec;
            windowmode::WindowModeExecutor* wmExecPtr = nullptr;
            if (wmCfg.enabled) {
                std::wstring wmErr;
                windowmode::BeginRunOptions wmBeginOpts;
                wmBeginOpts.launchTarget = true;
                wmBeginOpts.cancelFlag = &stopFlag_;
                {
                    const auto slash = selfPath.find_last_of(L"\\/");
                    wmBeginOpts.launchSearchDir = slash == std::wstring::npos ? L"" : selfPath.substr(0, slash);
                }
                if (!wmExec.BeginRun(wmCfg, wmErr, wmBeginOpts)) {
                    if (stopFlag_) {
                        PostMessageW(hwnd_, WM_RUN_DONE, 0, 0);
                        return;
                    }
                    if (hwnd_) {
                        promptPendingMessage_ = wmErr.empty()
                            ? L"窗口模式启动失败" : wmErr;
                        // 先结束运行并恢复主窗口，再弹提示，避免遮罩坐标错位导致「确定」点不到。
                        PostMessageW(hwnd_, WM_RUN_DONE, 0, 0);
                        PostMessageW(hwnd_, WM_APP_PROMPT, 0, 0);
                    } else {
                        PostMessageW(hwnd_, WM_RUN_DONE, 0, 0);
                    }
                    return;
                }
                wmExecPtr = &wmExec;
                wmExec.SetCoordMeta(activeCoordMeta);
                windowmode::WindowModeLog(L"[窗口模式] 已绑定目标，开始运行");
                windowmode::WindowModeLogDesktopSnap(L"绑定后", wmExec.TargetHwnd());
                if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                    HWND th = wmExec.TargetHwnd();
                    wchar_t cls[128]{};
                    if (th) GetClassNameW(th, cls, 128);
                    wchar_t buf[192]{};
                    swprintf_s(buf, L"窗口模式已绑定 hwnd=0x%p class=%s%s",
                        th, cls,
                        wmExec.UsesBackgroundWindow() ? L" [后台]" : L" [鼠标宏桌面]");
                    AppendDebugLog(buf);
                }
            }
            if (usesOcr) holdOcrSession();

            auto wmSetPos = [this, wmExecPtr](int x, int y, int rx, int ry) {
                if (wmExecPtr && wmExecPtr->IsActive()) {
                    wmExecPtr->MoveMouseClient(x, y, rx, ry, [this](int r) { return RandomInt(r); });
                } else {
                    MarkSimulatedInput();
                    SetCursorPos(x + RandomInt(rx), y + RandomInt(ry));
                    UnmarkSimulatedInput();
                }
            };

            auto wmUsesTarget = [wmExecPtr]() {
                return wmExecPtr && wmExecPtr->IsActive();
            };
            auto wmUsesBackground = [wmExecPtr]() {
                return wmExecPtr && wmExecPtr->IsActive() && wmExecPtr->UsesBackgroundWindow();
            };
            auto wmSendKey = [this, wmExecPtr, wmUsesTarget](UINT vk, bool down) {
                if (wmUsesTarget()) wmExecPtr->PostKeyToTarget(vk, down);
                else SendKey(vk, down);
            };
            auto wmSendHeldModifiers = [wmSendKey](const ScriptAction& act, bool down) {
                if (act.holdLeftWin) wmSendKey(VK_LWIN, down);
                if (act.holdRightWin) wmSendKey(VK_RWIN, down);
                if (act.holdLeftCtrl) wmSendKey(VK_LCONTROL, down);
                if (act.holdRightCtrl) wmSendKey(VK_RCONTROL, down);
                if (act.holdLeftAlt) wmSendKey(VK_LMENU, down);
                if (act.holdRightAlt) wmSendKey(VK_RMENU, down);
                if (act.holdLeftShift) wmSendKey(VK_LSHIFT, down);
                if (act.holdRightShift) wmSendKey(VK_RSHIFT, down);
            };
            auto wmMouseButton = [wmExecPtr, wmUsesTarget](int cx, int cy, MouseButtonType btn, bool down) {
                if (wmUsesTarget()) wmExecPtr->PostMouseButtonAtClient(cx, cy, btn, down);
                else MouseButtonEvent(btn, down);
            };
            auto wmMouseClick = [&](int cx, int cy, MouseButtonType btn) {
                if (wmUsesTarget()) {
                    wmExecPtr->PostMouseClickAtClient(cx, cy, btn);
                } else {
                    MouseClick(btn);
                }
            };
            auto wmSendShortcut = [wmSendKey](const ScriptAction& action) {
                ScriptAction tmp = action;
                ApplyShortcutPreset(tmp, action.shortcutPreset);
                if (tmp.holdLeftWin) wmSendKey(VK_LWIN, true);
                if (tmp.holdLeftCtrl) wmSendKey(VK_LCONTROL, true);
                if (tmp.holdLeftAlt) wmSendKey(VK_LMENU, true);
                if (tmp.holdLeftShift) wmSendKey(VK_LSHIFT, true);
                wmSendKey(tmp.keyVk, true);
                wmSendKey(tmp.keyVk, false);
                if (tmp.holdLeftShift) wmSendKey(VK_LSHIFT, false);
                if (tmp.holdLeftAlt) wmSendKey(VK_LMENU, false);
                if (tmp.holdLeftCtrl) wmSendKey(VK_LCONTROL, false);
                if (tmp.holdLeftWin) wmSendKey(VK_LWIN, false);
            };

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

            enum class RunRangeResult { Normal, BreakLoop, GotoPending };
            std::optional<size_t> pendingGoto;
            std::optional<size_t> loopEntryGotoTarget;
            bool pendingBreakLoop = false;
            std::function<RunRangeResult(size_t, size_t)> runRange;
            std::function<RunRangeResult(const std::wstring&)> runBlockByName;
            std::unordered_set<std::wstring> blockCallStack;

            auto notifyBreakoutUi = [&]() {
                HWND h = hwnd_;
                if (!h || !IsWindow(h)) return;
                DWORD_PTR result = 0;
                SendMessageTimeoutW(h, WM_APP_BREAKOUT_UI, 0, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &result);
            };
            auto waitBreakoutCooldown = [&]() {
                if (workerBreakoutTime_ <= 0 || stopFlag_) return;
                breakoutUserInput_ = false;
                breakoutPaused_ = true;
                const double t = workerBreakoutTime_;
                AppendBreakoutDebugLog(L"脱离时间：宏已中断，等待 "
                    + FormatBreakoutTimeForEditor(t) + L" 秒后继续");
                notifyBreakoutUi();
                auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(static_cast<int>(t * 1000.0));
                while (!stopFlag_) {
                    if (std::chrono::steady_clock::now() >= deadline) break;
                    if (breakoutUserInput_.exchange(false, std::memory_order_relaxed)) {
                        deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(static_cast<int>(t * 1000.0));
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                breakoutPaused_ = false;
                if (!stopFlag_) {
                    AppendBreakoutDebugLog(L"脱离时间：等待结束，宏继续运行");
                }
                notifyBreakoutUi();
            };

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
            int aiLoopDepth = 0;
            AiStepBudgetState aiRootBudget{};
            AiStepFrame* aiCurFrame = nullptr;
            const ScriptAction* aiInheritParent = nullptr;

            std::function<void(const ScriptAction&, const ScriptAction*)> runAiActionExecute;
            auto executeOne = std::function<void(const ScriptAction&)>();

            runAiActionExecute = [this, &usesOcr, &holdOcrSession, &heldKeyVk, &runRange, &runningScriptPath,
                &activeActions, &lockedScreen_, &lockedVirtX_, &lockedVirtY_, &clearLockedScreen, &makeVarCtx,
                &executeOne, &runAiActionExecute, &aiSessions, &aiLoopDepth, &aiRootBudget, &aiCurFrame, &aiInheritParent,
                &pendingBreakLoop, wmExecPtr, &wmUsesTarget, &currentTmplScale](
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

                auto propagateAiHistory = [&](AgentCore* core, const ScriptAction& action, size_t histBefore,
                    const std::wstring& sysPrompt, bool withTools, int maxTokens, int timeoutMs) {
                    if (core && action.aiContextMode != 0) {
                        aiSessions.PropagateHistoryAfterCall(
                            action.aiContextMode, aiLoopDepth, core, histBefore,
                            action, sysPrompt, appSettings_, timeoutMs, withTools, maxTokens);
                    }
                };

                auto resolveAiRegion = [&](int& x1, int& y1, int& x2, int& y2) -> bool {
                    if (wmUsesTarget()) {
                        return wmExecPtr->ResolveAiScreenRect(
                            eff, x1, y1, x2, y2, lockedScreen_, lockedVirtX_, lockedVirtY_);
                    }
                    if (eff.aiRegionByImage && !eff.aiTargetImagePath.empty()) {
                        int sx = 0, sy = 0, rsw = 0, rsh = 0;
                        GetVirtualScreenRect(sx, sy, rsw, rsh);
                        const TemplateScale tmplScale = currentTmplScale();
                        HBITMAP tmpl = LoadBitmapFromFile(eff.aiTargetImagePath);
                        if (!tmpl) return false;
                        ImageMatchOptions opt = BuildExecutionFindImageOptions(eff, tmplScale);
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
                            if (stepAction.type == ActionType::EndLoop) {
                                if (aiLoopDepth <= 0) {
                                    AppendAiDebugLog(L"  跳过结束循环：" + std::wstring(kEndLoopNeedsLoopParentMsg));
                                    continue;
                                }
                            }
                            AppendAiDebugLog(L"  执行 " + ActionName(stepAction)
                                + L" (步" + std::to_wstring(stepCount + 1) + L")");
                            if (stepAction.type == ActionType::AiActionExecute) {
                                runAiActionExecute(stepAction, &eff);
                            } else {
                                executeOne(stepAction);
                            }
                            if (pendingBreakLoop) break;
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
                            &aiSessions, prepAction, aiLoopDepth, route, 0, 0, appSettings_, timeoutMs, corePtr);
                        AgentCore* core = corePtr ? corePtr : ownedCore.get();
                        if (!core) {
                            AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：无法创建 AI 客户端");
                        } else {
                            const size_t histBefore = core->GetHistory().size();
                            AiActionResult ar = ExecuteAiActionExecute(
                                core, resolvedPrompt, "", 0, 0, eff.aiContextMode,
                                stopFlag_, eff.aiTimeoutSec, logFn, &aiHttpAbort_, nullptr);
                            if (ar.ok) {
                                propagateAiHistory(core, prepAction, histBefore,
                                    BuildAiActionExecuteTextSystemPrompt(),
                                    route == AiActionRouteKind::ToolExecute
                                        || route == AiActionRouteKind::MultiTurnTools,
                                    1024, timeoutMs);
                            }
                            handleAiActionApiResult(ar);
                        }
                    } catch (...) {
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：执行异常");
                    }
                } else {
                    int capX1 = 0, capY1 = 0, capX2 = 0, capY2 = 0;
                    if (!resolveAiRegion(capX1, capY1, capX2, capY2)) {
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：无法定位分析区域");
                    } else {
                        HBITMAP screenBmp = nullptr;
                        if (wmUsesTarget()) {
                            screenBmp = wmExecPtr->CaptureScreenRegionFromWindow(
                                capX1, capY1, capX2, capY2,
                                lockedScreen_, lockedVirtX_, lockedVirtY_);
                        } else {
                            screenBmp = CaptureScreenRegion(capX1, capY1, capX2, capY2);
                        }
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
                                        &aiSessions, prepAction, aiLoopDepth, route, apiW, apiH,
                                        appSettings_, timeoutMs, corePtr);
                                    AgentCore* core = corePtr ? corePtr : ownedCore.get();
                                    if (!core) {
                                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：无法创建 AI 客户端");
                                    } else {
                                        const size_t histBefore = core->GetHistory().size();
                                        AiActionResult ar = ExecuteAiActionExecute(
                                            core, resolvedPrompt, screenshotB64,
                                            apiW, apiH, eff.aiContextMode,
                                            stopFlag_, eff.aiTimeoutSec, logFn, &aiHttpAbort_, &capMap);
                                        if (ar.ok) {
                                            std::wstring sysPrompt;
                                            if (route == AiActionRouteKind::VisionQuery
                                                || route == AiActionRouteKind::CompositeClick) {
                                                sysPrompt = BuildAiActionVisionQuerySystemPrompt(apiW, apiH);
                                            } else if (route == AiActionRouteKind::MultiTurnTools) {
                                                sysPrompt = BuildAiActionHybridSystemPrompt(apiW, apiH);
                                            } else {
                                                sysPrompt = BuildAiActionExecuteSystemPrompt(apiW, apiH);
                                            }
                                            propagateAiHistory(core, prepAction, histBefore, sysPrompt,
                                                route == AiActionRouteKind::ToolExecute
                                                    || route == AiActionRouteKind::MultiTurnTools,
                                                1024, timeoutMs);
                                        }
                                        handleAiActionApiResult(ar);
                                    }
                                } catch (...) {
                                    AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：执行异常");
                                }
                            }
                        }
                    }
                }

                aiCurFrame = prevFrame;
            };

            // 精密键鼠回放（对齐 LLIR/FLOW 思路）：
            // 1) 绝对时间轴  2) 关闭加速使 SendInput 贴近 Raw 录制值  3) 提高线程优先级  4) 忙等收尾
            struct InputTimelineState {
                bool enabled = false;
                PrecisionInputTimeline precision;
                void Reset() {
                    precision.Reset();
                }
            };
            InputTimelineState inputTimeline;
            inputTimeline.enabled = IsRecordingScriptPath(selfPath)
                || ScriptIsTimedInputSequence(actions);

            // SendInput：准点与迟到均不垫间隔，靠绝对时间轴追赶，避免越追越稀、视角拉飘。
            MouseInputRouter::Instance().SetCatchUpGapUs(0);
            MouseBallisticsGuard ballisticsGuard(inputTimeline.enabled);
            PlaybackProcessPriorityGuard processPriGuard(inputTimeline.enabled);
            PlaybackThreadAffinityGuard affinityGuard(inputTimeline.enabled);
            MultimediaTimerGuard timerPeriodGuard(inputTimeline.enabled);
            struct ThreadPriorityGuard {
                HANDLE thread = nullptr;
                int prev = THREAD_PRIORITY_NORMAL;
                bool active = false;
                explicit ThreadPriorityGuard(bool enable) {
                    if (!enable) return;
                    thread = GetCurrentThread();
                    prev = GetThreadPriority(thread);
                    active = true;
                    // TIME_CRITICAL 尽量压调度抖动（仍可能被更高 IRQL 打断）
                    SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
                }
                ~ThreadPriorityGuard() {
                    if (active) SetThreadPriority(thread, prev);
                }
            } threadPriGuard(inputTimeline.enabled);

            auto waitAbsoluteTimeline = [this, &inputTimeline](double waitSec, uint64_t timingUs = 0) {
                if (timingUs == 0 && waitSec <= 0.0) {
                    MouseInputRouter::Instance().NoteWaitLatenessUs(0);
                    return;
                }
                const auto cancelled = [this] {
                    return stopFlag_.load(std::memory_order_relaxed)
                        || BreakoutTriggered();
                };
                // 绝对时间轴：优先微秒；迟到不回补 sleep，由后续 deadline 自然追赶。
                if (timingUs > 0) {
                    inputTimeline.precision.WaitDeltaUs(timingUs, cancelled);
                } else {
                    inputTimeline.precision.WaitDeltaSeconds(waitSec, cancelled);
                }
                MouseInputRouter::Instance().NoteWaitLatenessUs(
                    inputTimeline.precision.LastLatenessUs());
            };

            executeOne = [this, &usesOcr, &holdOcrSession, &heldKeyVk, &heldKeys, &runRange, &runningScriptPath, &activeActions, &lockedScreen_, &lockedVirtX_, &lockedVirtY_, &clearLockedScreen, &makeVarCtx, &executeOne, &runAiActionExecute, &aiSessions, &aiLoopDepth, &pendingBreakLoop, wmExecPtr, &wmSetPos, &wmSendKey, &wmSendHeldModifiers, &wmMouseButton, &wmMouseClick, &wmSendShortcut, &wmUsesTarget, &wmUsesBackground, &activeCoordMeta, &currentTmplScale, execTargetW, execTargetH, &inputTimeline, &waitAbsoluteTimeline](const ScriptAction& a) {
                // 录制折叠进来的 duration/timingUs=前延迟（Wait/Click/Scroll 等除外）
                if (inputTimeline.enabled && a.randomDuration <= 1e-12) {
                    const bool durationIsInterval =
                        a.type == ActionType::Wait
                        || ActionUsesInterRepeatInterval(a.type);
                    if (!durationIsInterval && (a.timingUs > 0 || a.duration > 1e-9)) {
                        waitAbsoluteTimeline(a.duration, a.timingUs);
                    }
                }

                if (appSettings_.playback.autoOutputKeyFunctionDebug
                    && a.type != ActionType::MoveMouse
                    && a.type != ActionType::MoveMouseRelative
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
                    const int rx = inputTimeline.enabled ? 0 : a.randomX;
                    const int ry = inputTimeline.enabled ? 0 : a.randomY;
                    wmSetPos(x, y, rx, ry);
                }
                else if (a.type == ActionType::MoveMouseRelative) {
                    const int dx = a.x + (inputTimeline.enabled ? 0 : RandomInt(a.randomX));
                    const int dy = a.y + (inputTimeline.enabled ? 0 : RandomInt(a.randomY));
                    if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                        AppendDebugLog(FormatMoveMouseRelativeDebug(a, dx, dy));
                    }
                    MarkSimulatedInput();
                    SendMouseMoveRelative(dx, dy);
                    UnmarkSimulatedInput();
                }
                else if (a.type == ActionType::Wait) {
                    if (a.randomDuration > 1e-12 || !inputTimeline.enabled) {
                        const double waitSec = a.duration + RandomDelay(a.randomDuration);
                        if (waitSec > 0.0) {
                            SleepInterruptible(waitSec);
                            if (inputTimeline.enabled) inputTimeline.Reset();
                        }
                    } else if (a.timingUs > 0) {
                        waitAbsoluteTimeline(0.0, a.timingUs);
                    } else if (a.duration > 0.0) {
                        waitAbsoluteTimeline(a.duration, 0);
                    }
                }
                else if (a.type == ActionType::MouseDown) {
                    MarkSimulatedInput();
                    wmSendHeldModifiers(a, true);
                    wmMouseButton(a.x, a.y, a.button, true);
                    UnmarkSimulatedInput();
                }
                else if (a.type == ActionType::MouseUp) {
                    MarkSimulatedInput();
                    wmMouseButton(a.x, a.y, a.button, false);
                    wmSendHeldModifiers(a, false);
                    UnmarkSimulatedInput();
                }
                else if (a.type == ActionType::MouseClick) for (int i = 0; i < a.clickCount && !stopFlag_; ++i) {
                    int cx = a.x;
                    int cy = a.y;
                    if (a.x != 0 || a.y != 0) {
                        wmSetPos(a.x, a.y, a.randomX, a.randomY);
                        cx = a.x + RandomInt(a.randomX);
                        cy = a.y + RandomInt(a.randomY);
                    }
                    MarkSimulatedInput();
                    wmSendHeldModifiers(a, true);
                    wmMouseClick(cx, cy, a.button);
                    wmSendHeldModifiers(a, false);
                    UnmarkSimulatedInput();
                    // duration=两次重复之间的间隔；执行 1 次时不等待，首前/末后也不插等待
                    if (ShouldWaitAfterRepeat(a, i) && !stopFlag_) {
                        SleepInterruptible(a.duration + RandomDelay(a.randomDuration));
                    }
                }
                else if (a.type == ActionType::KeyDown) {
                    MarkSimulatedInput();
                    wmSendHeldModifiers(a, true);
                    wmSendKey(a.keyVk, true);
                    heldKeyVk = a.keyVk;
                    heldKeys.insert(a.keyVk);
                    UnmarkSimulatedInput();
                }
                else if (a.type == ActionType::KeyUp) {
                    MarkSimulatedInput();
                    wmSendKey(a.keyVk, false);
                    if (heldKeyVk == a.keyVk) heldKeyVk = 0;
                    heldKeys.erase(a.keyVk);
                    wmSendHeldModifiers(a, false);
                    UnmarkSimulatedInput();
                }
                else if (a.type == ActionType::KeyClick) for (int i = 0; i < a.clickCount && !stopFlag_; ++i) {
                    MarkSimulatedInput();
                    wmSendHeldModifiers(a, true);
                    wmSendKey(a.keyVk, true);
                    wmSendKey(a.keyVk, false);
                    wmSendHeldModifiers(a, false);
                    UnmarkSimulatedInput();
                    if (ShouldWaitAfterRepeat(a, i) && !stopFlag_) {
                        SleepInterruptible(a.duration + RandomDelay(a.randomDuration));
                    }
                }
                else if (a.type == ActionType::HotkeyShortcut) for (int i = 0; i < a.clickCount && !stopFlag_; ++i) {
                    MarkSimulatedInput();
                    wmSendShortcut(a);
                    UnmarkSimulatedInput();
                    if (ShouldWaitAfterRepeat(a, i) && !stopFlag_) {
                        SleepInterruptible(a.duration + RandomDelay(a.randomDuration));
                    }
                }
                else if (a.type == ActionType::QuickInput) for (int i = 0; i < a.clickCount && !stopFlag_; ++i) {
                    // 不在整段输入期间持有 MarkSimulatedInput：否则热键停止会被 ShouldIgnoreHotkeyStop 挡住。
                    // 注入键由 LL 钩子过滤 LLKHF_INJECTED；字间轮询 stopFlag_ 以便立即终止。
                    const std::wstring text = ResolveQuickInputText(a.inputText);
                    if (wmExecPtr && wmExecPtr->IsActive()) {
                        wmExecPtr->SendQuickInputToTarget(text, a.charInterval);
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            wchar_t buf[160]{};
                            swprintf_s(buf, L"快捷输入→目标窗口 hwnd=0x%p%s",
                                wmExecPtr->TargetHwnd(),
                                wmUsesBackground() ? L" [后台窗口模式]" : L" [窗口模式]");
                            AppendDebugLog(buf);
                        }
                    } else {
                        SendQuickInputText(text, a.charInterval, &stopFlag_);
                    }
                    if (ShouldWaitAfterRepeat(a, i) && !stopFlag_) {
                        SleepInterruptible(a.duration + RandomDelay(a.randomDuration));
                    }
                }
                else if (a.type == ActionType::ScrollWheel) for (int i = 0; i < a.clickCount && !stopFlag_; ++i) {
                    MarkSimulatedInput();
                    const bool positive = a.scrollDirection == 0;
                    if (wmUsesTarget()) {
                        if (a.scrollVertical) {
                            wmExecPtr->PostScrollWheelAtClient(a.x, a.y, a.scrollSteps, true, positive);
                        }
                        if (a.scrollHorizontal) {
                            wmExecPtr->PostScrollWheelAtClient(a.x, a.y, a.scrollSteps, false, positive);
                        }
                    } else {
                        const int delta = (positive ? 1 : -1) * WHEEL_DELTA;
                        for (int step = 0; step < a.scrollSteps; ++step) {
                            if (a.scrollVertical)
                                MouseInputRouter::Instance().Wheel(delta, false);
                            if (a.scrollHorizontal)
                                MouseInputRouter::Instance().Wheel(delta, true);
                        }
                    }
                    UnmarkSimulatedInput();
                    if (ShouldWaitAfterRepeat(a, i) && !stopFlag_) {
                        SleepInterruptible(a.duration + RandomDelay(a.randomDuration));
                    }
                }
                else if (a.type == ActionType::FindImage) {
                    const TemplateScale findTmplScale = currentTmplScale();
                    // 窗口/后台模式也必须 Prepare：偏移 nOffset 依赖 templateW/H。
                    // 若这里留空，ResolveFindImageClickPoint 会把偏移算成 0（落在中心）。
                    const PreparedFindImageMatch findPrep = PrepareFindImageMatch(a, findTmplScale);
                    int lastFindMs = 0;
                    bool findUsedAnamorphic = false;
                    auto runFind = [&]() -> ImageMatchResult {
                        findUsedAnamorphic = false;
                        if (wmUsesTarget()) {
                            ImageMatchOutput output = wmExecPtr->FindImageClient(
                                a, lockedScreen_, lockedVirtX_, lockedVirtY_);
                            lastFindMs = output.elapsedMs;
                            if (output.matches.empty()) return {};
                            return output.matches.front();
                        }
                        int x1 = a.searchX1, y1 = a.searchY1, x2 = a.searchX2, y2 = a.searchY2;
                        if (a.searchFullScreen) {
                            int sx = 0, sy = 0, sw = 0, sh = 0;
                            GetVirtualScreenRect(sx, sy, sw, sh);
                            x1 = sx; y1 = sy; x2 = sx + sw; y2 = sy + sh;
                        } else {
                            int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
                            GetVirtualScreenRect(vsX, vsY, vsW, vsH);
                            if (x1 <= vsX + 2 && y1 <= vsY + 2
                                && x2 >= vsX + vsW - 2 && y2 >= vsY + vsH - 2) {
                                x1 = vsX; y1 = vsY; x2 = vsX + vsW; y2 = vsY + vsH;
                            }
                        }
                        HBITMAP tmpl = findPrep.bitmap;
                        if (!tmpl) {
                            if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                AppendDebugLog(L"找图失败: 无法加载模板 " + a.imagePath);
                            }
                            return {};
                        }
                        ImageMatchOptions opt = findPrep.options;
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            wchar_t buf[960]{};
                            swprintf_s(buf,
                                L"找图调试 ref=%dx%d cap=%dx%d cur=%dx%d@%ddpi scale=%.3fx%.3f "
                                L"preScale=%d matchScale=%.3f~%.3f tpl=%dx%d search=(%d,%d)-(%d,%d) thr=%.0f locked=%d",
                                activeCoordMeta.refWidth, activeCoordMeta.refHeight,
                                activeCoordMeta.captureWidth > 0 ? activeCoordMeta.captureWidth
                                    : activeCoordMeta.refWidth,
                                activeCoordMeta.captureHeight > 0 ? activeCoordMeta.captureHeight
                                    : activeCoordMeta.refHeight,
                                execTargetW, execTargetH, GetDpiForSystem(),
                                findTmplScale.sx, findTmplScale.sy,
                                findPrep.templatePreScaled ? 1 : 0,
                                opt.scaleMin, opt.scaleMax,
                                findPrep.templateW, findPrep.templateH, x1, y1, x2, y2, opt.thresholdPercent,
                                lockedScreen_ ? 1 : 0);
                            AppendDebugLog(buf);
                        }
                        opt.maxMatches = 20;
                        opt.maxOverlap = 0.5;
                        auto doMatch = [&](HBITMAP bmp, const ImageMatchOptions& matchOpt) -> ImageMatchOutput {
                            if (lockedScreen_) {
                                return FindTemplateInFrozenScreenMulti(
                                    lockedScreen_, lockedVirtX_, lockedVirtY_, x1, y1, x2, y2, bmp, matchOpt);
                            }
                            return FindTemplateOnScreenMulti(x1, y1, x2, y2, bmp, matchOpt);
                        };
                        ImageMatchOutput output = doMatch(tmpl, opt);
                        lastFindMs = output.elapsedMs;

                        // 宽高比变化时：仅当 NCC 还有希望时再试非等比拉伸
                        const bool aspectChanged =
                            std::abs(findTmplScale.sx - findTmplScale.sy) > 0.02;
                        if (output.matches.empty() && aspectChanged
                            && output.debugBestNccPercent >= opt.thresholdPercent * 0.40) {
                            HBITMAP stretched = LoadScaledTemplateBitmap(
                                a.imagePath, findTmplScale.sx, findTmplScale.sy);
                            if (stretched) {
                                ImageMatchOptions stretchOpt = opt;
                                stretchOpt.scaleMin = 1.0;
                                stretchOpt.scaleMax = 1.0;
                                stretchOpt.scaleStep = 1.0;
                                stretchOpt.disablePyramid = true;
                                stretchOpt.crossResolutionMatch = true;
                                ImageMatchOutput stretchOut = doMatch(stretched, stretchOpt);
                                lastFindMs += stretchOut.elapsedMs;
                                if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                    wchar_t buf[320]{};
                                    swprintf_s(buf,
                                        L"找图兜底 非等比拉伸 bestNcc=%.1f%% matches=%d",
                                        stretchOut.debugBestNccPercent,
                                        static_cast<int>(stretchOut.matches.size()));
                                    AppendDebugLog(buf);
                                }
                                if (!stretchOut.matches.empty()) {
                                    findUsedAnamorphic = true;
                                    output = std::move(stretchOut);
                                }
                                DeleteBitmapHandle(stretched);
                            }
                        }

                        if (appSettings_.playback.autoOutputKeyFunctionDebug && output.matches.empty()) {
                            wchar_t buf[320]{};
                            swprintf_s(buf,
                                L"找图诊断 无共识匹配 %dms rawCandidates=%d bestNcc=%.1f%%",
                                output.elapsedMs, output.debugRawCandidates, output.debugBestNccPercent);
                            AppendDebugLog(buf);
                        }
                        if (output.matches.empty()) return {};
                        return output.matches.front();
                    };
                    ImageMatchResult lastRawMatch{};
                    int lastTargetX = 0, lastTargetY = 0;
                    bool lastHadTarget = false;
                    const double findTimeSec = ResolveFindImageTimeSec(a.findTimeExpr, makeVarCtx());
                    const bool loopUntilFound = findTimeSec < 0.0;
                    const auto findStart = std::chrono::steady_clock::now();
                    do {
                        const ImageMatchResult rawMatch = runFind();
                        lastRawMatch = rawMatch;
                        const ImageMatchResult match = NormalizeMatchVarResult(rawMatch, a.matchThreshold);
                        if (a.findImageFollowUp == 2) {
                            const std::wstring varName = a.matchVarName.empty() ? L"matchRet" : a.matchVarName;
                            matchVars_[varName] = match;
                            break;
                        } else if (match.found) {
                            int tx = 0, ty = 0;
                            ResolveFindImageClickPoint(match, findPrep.templateW, findPrep.templateH,
                                a.nOffsetX, a.nOffsetY, findTmplScale,
                                findPrep.templatePreScaled || findUsedAnamorphic, tx, ty);
                            lastTargetX = tx;
                            lastTargetY = ty;
                            lastHadTarget = true;
                            if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                int cx = 0, cy = 0;
                                FindImageMatchCenter(match, cx, cy);
                                const bool scaledTpl = findPrep.templatePreScaled || findUsedAnamorphic;
                                const int scaledOffX = static_cast<int>(std::round(
                                    a.nOffsetX * findPrep.templateW
                                    * (scaledTpl ? findTmplScale.sx : match.scale)));
                                const int scaledOffY = static_cast<int>(std::round(
                                    a.nOffsetY * findPrep.templateH
                                    * (scaledTpl ? findTmplScale.sy : match.scale)));
                                int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
                                GetVirtualScreenBounds(vsX, vsY, vsW, vsH);
                                wchar_t buf[640]{};
                                swprintf_s(buf,
                                    L"找图落点 %dms tl=(%d,%d) box=%dx%d scale=%.3f "
                                    L"center=(%d,%d) norm=(%.4f,%.4f) "
                                    L"offNorm=(%.4f,%.4f) offScaled=(%d,%d) target=(%d,%d)",
                                    lastFindMs,
                                    match.topLeftX, match.topLeftY,
                                    match.bottomRightX - match.topLeftX,
                                    match.bottomRightY - match.topLeftY,
                                    match.scale,
                                    cx, cy,
                                    vsW > 0 ? (cx - vsX) / static_cast<double>(vsW) : 0.0,
                                    vsH > 0 ? (cy - vsY) / static_cast<double>(vsH) : 0.0,
                                    a.nOffsetX, a.nOffsetY, scaledOffX, scaledOffY, tx, ty);
                                AppendDebugLog(buf);
                            }
                            const std::wstring varName = a.matchVarName.empty() ? L"matchRet" : a.matchVarName;
                            matchVars_[varName] = match;
                            if (a.findImageFollowUp == 0) {
                                wmSetPos(tx, ty, 0, 0);
                                MarkSimulatedInput();
                                if (wmUsesTarget()) wmExecPtr->PostMouseClickAtClient(tx, ty, MouseButtonType::Left);
                                else wmMouseClick(tx, ty, MouseButtonType::Left);
                                UnmarkSimulatedInput();
                            } else if (a.findImageFollowUp == 1) {
                                wmSetPos(tx, ty, 0, 0);
                                if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                    POINT cur{};
                                    GetCursorPos(&cur);
                                    wchar_t buf[128]{};
                                    swprintf_s(buf, L"找图光标实际位置=(%d,%d)", cur.x, cur.y);
                                    AppendDebugLog(buf);
                                }
                            }
                            break;
                        } else if (!loopUntilFound) {
                            if (findTimeSec <= 0.0) break;
                            const double elapsed = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - findStart).count();
                            if (elapsed >= findTimeSec) break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    } while (!stopFlag_ && !BreakoutTriggered());
                    if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                        AppendDebugLog(FormatFindImageDebug(a, lastRawMatch, lastHadTarget, lastTargetX, lastTargetY));
                    }
                    if (findPrep.bitmap) {
                        DeleteBitmapHandle(findPrep.bitmap);
                    }
                }
                else if (a.type == ActionType::TextRecognition) {
                    usesOcr = true;
                    workerUsesOcrVars_ = true;
                    holdOcrSession();
                    const std::wstring varName = a.matchVarName.empty() ? L"a" : a.matchVarName;
                    auto resolveOcrRegion = [&](int& x1, int& y1, int& x2, int& y2) -> bool {
                        if (wmUsesTarget()) {
                            if (a.ocrRegionByImage) {
                                ImageMatchOutput output = wmExecPtr->FindImageClient(
                                    a, lockedScreen_, lockedVirtX_, lockedVirtY_);
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
                            if (!wmExecPtr->ResolveClientSearchRect(a, x1, y1, x2, y2)) return false;
                            return wmExecPtr->MapClientRect(x1, y1, x2, y2, x1, y1, x2, y2);
                        }
                        if (a.ocrRegionByImage) {
                            int sx = 0, sy = 0, sw = 0, sh = 0;
                            GetVirtualScreenRect(sx, sy, sw, sh);
                            const TemplateScale tmplScale = currentTmplScale();
                            HBITMAP tmpl = LoadBitmapFromFile(a.imagePath);
                            if (!tmpl) return false;
                            ImageMatchOptions opt = BuildExecutionFindImageOptions(a, tmplScale);
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
                        OcrEngineOutput output;
                        if (wmUsesTarget()) {
                            output = wmExecPtr->RunOcrOnClientRegion(
                                a, lockedScreen_, lockedVirtX_, lockedVirtY_);
                        } else {
                            int x1 = a.searchX1, y1 = a.searchY1, x2 = a.searchX2, y2 = a.searchY2;
                            if (!resolveOcrRegion(x1, y1, x2, y2)) {
                                return a.ocrResultMode == 1
                                    ? MakeOcrSearchVarResult(OcrTextLine{}, false)
                                    : MakeOcrTextVarResult(L"");
                            }
                            output = RunOcrOnScreenRegion(
                                x1, y1, x2, y2, lockedScreen_, lockedVirtX_, lockedVirtY_, a.ocrDigitsOnly);
                        }
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
                        int tx = centerX + a.offsetX;
                        int ty = centerY + a.offsetY;
                        if (wmExecPtr && wmExecPtr->IsActive()) {
                            windowmode::ScreenToClientPoint(
                                wmExecPtr->TargetHwnd(), tx, ty, tx, ty);
                        }
                        if (a.ocrFollowUp == 0) {
                            wmSetPos(tx, ty, 0, 0);
                            MarkSimulatedInput();
                            wmMouseClick(tx, ty, MouseButtonType::Left);
                            UnmarkSimulatedInput();
                        } else if (a.ocrFollowUp == 1) {
                            wmSetPos(tx, ty, 0, 0);
                        }
                    };
                    auto emitOcrDebug = [&](const std::wstring& textContent, bool searchFound) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatOcrDebug(a, textContent, searchFound, makeVarCtx()));
                        }
                    };
                    if (a.ocrResultMode == 0 && a.ocrFollowUp != 2) {
                        OcrEngineOutput output;
                        if (wmUsesTarget()) {
                            output = wmExecPtr->RunOcrOnClientRegion(
                                a, lockedScreen_, lockedVirtX_, lockedVirtY_);
                        } else {
                            int x1 = a.searchX1, y1 = a.searchY1, x2 = a.searchX2, y2 = a.searchY2;
                            if (!resolveOcrRegion(x1, y1, x2, y2)) {
                                ocrVars_[varName] = MakeOcrTextVarResult(L"");
                                emitOcrDebug(L"", false);
                                return;
                            }
                            output = RunOcrOnScreenRegion(
                                x1, y1, x2, y2, lockedScreen_, lockedVirtX_, lockedVirtY_, a.ocrDigitsOnly);
                        }
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
                        } while (!stopFlag_ && !BreakoutTriggered());
                        emitOcrDebug(lastResult.text, lastResult.found != 0);
                    }
                }
                else if (a.type == ActionType::RunMacro) {
                    std::wstring path = a.targetPath;
                    if (path.empty() || path == runningScriptPath) return;
                    const ScriptFileData nestedData = LoadScriptFileData(path, false);
                    if (nestedData.actions.empty()) return;
                    CoordMeta nestedMeta = ScriptCoordMetaForExecution(nestedData.coordMeta);
                    std::vector<ScriptAction> nested =
                        PrepareScriptActionsForExecution(nestedData.actions, nestedMeta);
                    if (!usesOcr && ScriptUsesTextRecognition(nested)) {
                        usesOcr = true;
                        workerUsesOcrVars_ = true;
                    }
                    if (usesOcr) holdOcrSession();
                    const std::vector<ScriptAction>* prevActions = activeActions;
                    const std::wstring prevPath = runningScriptPath;
                    const CoordMeta prevCoordMeta = activeCoordMeta;
                    activeCoordMeta = nestedMeta;
                    if (wmExecPtr) wmExecPtr->SetCoordMeta(activeCoordMeta);
                    activeActions = &nested;
                    runningScriptPath = path;
                    runRange(0, nested.size());
                    activeActions = prevActions;
                    runningScriptPath = prevPath;
                    activeCoordMeta = prevCoordMeta;
                    if (wmExecPtr) wmExecPtr->SetCoordMeta(activeCoordMeta);
                }
                else if (a.type == ActionType::LockScreenshot) {
                    clearLockedScreen();
                    if (wmUsesTarget()) {
                        if (!wmExecPtr->LockWindowCapture(lockedScreen_, lockedVirtX_, lockedVirtY_)) {
                            AppendDebugLog(std::wstring(wmUsesBackground() ? L"后台窗口模式" : L"窗口模式")
                                + L"：锁定窗口截图失败，后续找图将使用实时截图");
                        }
                    } else {
                        lockedScreen_ = CaptureVirtualScreen(lockedVirtX_, lockedVirtY_);
                    }
                }
                else if (a.type == ActionType::UnlockScreenshot) {
                    clearLockedScreen();
                }
                else if (a.type == ActionType::StopMacro) {
                    stopFlag_ = true;
                }
                else if (a.type == ActionType::EndLoop) {
                    if (aiLoopDepth > 0) pendingBreakLoop = true;
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
                    std::wstring path = a.targetPath;
                    if (path.empty()) break;
                    const ScriptFileData nestedData = LoadScriptFileData(path, false);
                    if (nestedData.actions.empty()) break;
                    CoordMeta nestedMeta = ScriptCoordMetaForExecution(nestedData.coordMeta);
                    std::vector<ScriptAction> nested =
                        PrepareScriptActionsForExecution(nestedData.actions, nestedMeta);
                    const std::vector<ScriptAction>* prevActions = activeActions;
                    const std::wstring prevPath = runningScriptPath;
                    const CoordMeta prevCoordMeta = activeCoordMeta;
                    activeCoordMeta = nestedMeta;
                    if (wmExecPtr) wmExecPtr->SetCoordMeta(activeCoordMeta);
                    activeActions = &nested;
                    runningScriptPath = path;
                    runRange(0, nested.size());
                    activeActions = prevActions;
                    runningScriptPath = prevPath;
                    activeCoordMeta = prevCoordMeta;
                    if (wmExecPtr) wmExecPtr->SetCoordMeta(activeCoordMeta);
                    if (ShouldWaitAfterRepeat(a, i) && !stopFlag_) {
                        SleepInterruptible(a.duration + RandomDelay(a.randomDuration));
                    }
                }
                else if (a.type == ActionType::GetCursorPos) {
                    int cx = 0, cy = 0;
                    bool gotPos = false;
                    if (wmExecPtr && wmExecPtr->IsActive()) {
                        gotPos = wmExecPtr->GetCursorClientPos(cx, cy);
                    } else {
                        POINT pt{};
                        gotPos = GetCursorPos(&pt) == TRUE;
                        cx = pt.x;
                        cy = pt.y;
                    }
                    if (gotPos) {
                        const std::wstring varName = a.matchVarName.empty() ? L"cursor" : a.matchVarName;
                        ImageMatchResult match{};
                        match.found = true;
                        match.topLeftX = cx;
                        match.topLeftY = cy;
                        match.bottomRightX = cx;
                        match.bottomRightY = cy;
                        match.x = cx;
                        match.y = cy;
                        match.score = 100.0;
                        matchVars_[varName] = match;
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(L"获取当前光标位置→[" + varName + L"] "
                                + std::to_wstring(cx) + L"," + std::to_wstring(cy));
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
                        const AiActionResult ar = RunAiTextAnalysisForAction(
                            a, resolvedPrompt, &aiSessions, aiLoopDepth);
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
                        if (wmUsesTarget()) {
                            return wmExecPtr->ResolveAiScreenRect(
                                a, x1, y1, x2, y2, lockedScreen_, lockedVirtX_, lockedVirtY_);
                        }
                        if (a.aiRegionByImage && !a.aiTargetImagePath.empty()) {
                            int sx = 0, sy = 0, sw = 0, sh = 0;
                            GetVirtualScreenRect(sx, sy, sw, sh);
                            const TemplateScale tmplScale = currentTmplScale();
                            HBITMAP tmpl = LoadBitmapFromFile(a.aiTargetImagePath);
                            if (!tmpl) return false;
                            ImageMatchOptions opt = BuildExecutionFindImageOptions(a, tmplScale);
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
                        if (wmUsesTarget()) {
                            screenBmp = wmExecPtr->CaptureScreenRegionFromWindow(
                                capX1, capY1, capX2, capY2,
                                lockedScreen_, lockedVirtX_, lockedVirtY_);
                        } else {
                            screenBmp = CaptureScreenRegion(capX1, capY1, capX2, capY2);
                        }
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
                                        a, resolvedPrompt, encoded.base64, &aiSessions, aiLoopDepth);
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

            auto executeWithBreakout = [&](const ScriptAction& action) {
                if (workerBreakoutTime_ <= 0) {
                    executeOne(action);
                    return;
                }
                while (!stopFlag_) {
                    breakoutUserInput_ = false;
                    executeOne(action);
                    if (stopFlag_ || !breakoutUserInput_.load(std::memory_order_relaxed)) break;
                    waitBreakoutCooldown();
                }
            };

            runBlockByName = [&](const std::wstring& name) -> RunRangeResult {
                if (stopFlag_ || name.empty()) return RunRangeResult::Normal;
                aiSessions.ClearBlock();
                std::unordered_map<std::wstring, size_t> blockDefs;
                for (size_t i = 0; i < activeActions->size(); ++i) {
                    if ((*activeActions)[i].type == ActionType::DefineBlock && !(*activeActions)[i].blockName.empty()) {
                        blockDefs[(*activeActions)[i].blockName] = i;
                    }
                }
                const auto it = blockDefs.find(name);
                if (it == blockDefs.end()) return RunRangeResult::Normal;
                if (blockCallStack.count(name)) return RunRangeResult::Normal;
                blockCallStack.insert(name);
                const RunRangeResult result = runRange(it->second + 1, containerBodyEnd(it->second));
                blockCallStack.erase(name);
                return result;
            };
            runRange = [&](size_t start, size_t end) -> RunRangeResult {
                auto isDirectLoopBodyRange = [&](size_t loopIdx) {
                    return start == loopIdx + 1 && end == containerBodyEnd(loopIdx);
                };
                auto resolveGotoCursor = [&](size_t targetIdx, size_t& outCursor) {
                    const int outerLoop = OutermostEnclosingLoop(*activeActions, targetIdx);
                    if (outerLoop < 0) {
                        outCursor = targetIdx;
                        return;
                    }
                    if (isDirectLoopBodyRange(static_cast<size_t>(outerLoop))) {
                        outCursor = targetIdx;
                        return;
                    }
                    loopEntryGotoTarget = targetIdx;
                    outCursor = static_cast<size_t>(outerLoop);
                };
                auto consumePendingGoto = [&](size_t scopeStart, size_t scopeEnd, size_t& cursor) -> RunRangeResult {
                    if (!pendingGoto) return RunRangeResult::Normal;
                    const size_t targetIdx = *pendingGoto;
                    pendingGoto.reset();
                    resolveGotoCursor(targetIdx, cursor);
                    if (cursor < scopeStart || cursor >= scopeEnd) return RunRangeResult::GotoPending;
                    return RunRangeResult::Normal;
                };
                for (size_t i = start; i < end && !stopFlag_; ) {
                    if (pendingGoto) {
                        const RunRangeResult gotoResult = consumePendingGoto(start, end, i);
                        if (gotoResult == RunRangeResult::GotoPending) return RunRangeResult::GotoPending;
                        continue;
                    }
                    const auto& a = (*activeActions)[i];
                    if (a.type == ActionType::EndLoop) return RunRangeResult::BreakLoop;
                    if (SkipsInMainFlow(a.type)) {
                        i = containerBodyEnd(i);
                        continue;
                    }
                    if (a.type == ActionType::Loop) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatGenericActionDebug(a));
                        }
                        const size_t bodyEnd = containerBodyEnd(i);
                        const int thisLoopDepth = aiLoopDepth;
                        ++aiLoopDepth;
                        aiSessions.EnsureLoopDepth(aiLoopDepth);
                        int iter = 1;
                        bool broke = false;
                        const auto loopStartTime = std::chrono::steady_clock::now();
                        while (!stopFlag_ && !broke) {
                            inputTimeline.Reset(); // 每次循环从零重新对齐录制时间轴
                            aiSessions.ClearLoopAt(thisLoopDepth);
                            if (!a.loopVarName.empty()) loopVars_[a.loopVarName] = iter;
                            MacroVariableContext ctx = makeVarCtx();
                            const int maxLoop = ResolveLoopMaxCount(a, ctx, loopStartTime);
                            if (!(maxLoop < 0 || iter <= maxLoop)) break;
                            size_t runFrom = i + 1;
                            if (iter == 1 && loopEntryGotoTarget) {
                                const size_t entryTarget = *loopEntryGotoTarget;
                                if (entryTarget > i && entryTarget < bodyEnd) {
                                    if (EnclosingChildLoopInBody(*activeActions, i, entryTarget) < 0) {
                                        runFrom = entryTarget;
                                        loopEntryGotoTarget.reset();
                                    }
                                }
                            }
                            const RunRangeResult bodyResult = runRange(runFrom, bodyEnd);
                            if (bodyResult == RunRangeResult::BreakLoop) broke = true;
                            else if (bodyResult == RunRangeResult::GotoPending) broke = true;
                            ++iter;
                        }
                        --aiLoopDepth;
                        if (!a.loopVarName.empty()) loopVars_.erase(a.loopVarName);
                        if (pendingGoto) {
                            const RunRangeResult gotoResult = consumePendingGoto(start, end, i);
                            if (gotoResult == RunRangeResult::GotoPending) return RunRangeResult::GotoPending;
                            continue;
                        }
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
                        RunRangeResult branchResult = RunRangeResult::Normal;
                        if (cond) {
                            branchResult = elseIdx >= 0
                                ? runRange(i + 1, static_cast<size_t>(elseIdx))
                                : runRange(i + 1, trueEnd);
                            i = elseIdx >= 0 ? containerBodyEnd(static_cast<size_t>(elseIdx)) : trueEnd;
                        } else if (elseIdx >= 0) {
                            branchResult = runRange(static_cast<size_t>(elseIdx) + 1, containerBodyEnd(static_cast<size_t>(elseIdx)));
                            i = containerBodyEnd(static_cast<size_t>(elseIdx));
                        } else {
                            i = trueEnd;
                        }
                        if (branchResult == RunRangeResult::GotoPending || pendingGoto) {
                            const RunRangeResult gotoResult = consumePendingGoto(start, end, i);
                            if (gotoResult == RunRangeResult::GotoPending) return RunRangeResult::GotoPending;
                            continue;
                        }
                        if (branchResult == RunRangeResult::BreakLoop) return RunRangeResult::BreakLoop;
                    } else if (a.type == ActionType::Else) {
                        i = containerBodyEnd(i);
                    } else if (a.type == ActionType::RunBlock) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatGenericActionDebug(a));
                        }
                        const RunRangeResult blockResult = runBlockByName(a.blockName);
                        if (blockResult == RunRangeResult::GotoPending || pendingGoto) {
                            const RunRangeResult gotoResult = consumePendingGoto(start, end, i);
                            if (gotoResult == RunRangeResult::GotoPending) return RunRangeResult::GotoPending;
                            continue;
                        }
                        ++i;
                    } else if (a.type == ActionType::Goto) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatGenericActionDebug(a));
                        }
                        MacroVariableContext ctx = makeVarCtx();
                        int targetNo = 0;
                        if (TryResolveGotoStepNo(a.gotoStepExpr, ctx, targetNo)) {
                            const size_t targetIdx = FindActionIndexByNo(*activeActions, targetNo);
                            if (targetIdx < activeActions->size()) {
                                if (targetIdx < start || targetIdx >= end) {
                                    pendingGoto = targetIdx;
                                    return RunRangeResult::GotoPending;
                                }
                                resolveGotoCursor(targetIdx, i);
                                continue;
                            }
                        }
                        ++i;
                    } else {
                        executeWithBreakout(a);
                        if (pendingBreakLoop) {
                            pendingBreakLoop = false;
                            return RunRangeResult::BreakLoop;
                        }
                        ++i;
                    }
                }
                return RunRangeResult::Normal;
            };
            while (!stopFlag_) {
                ++curLoops_;
                inputTimeline.Reset();
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
                pendingGoto.reset();
                loopEntryGotoTarget.reset();
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
            if (!heldKeys.empty()) {
                MarkSimulatedInput();
                for (UINT vk : heldKeys) SendKey(vk, false);
                UnmarkSimulatedInput();
                heldKeys.clear();
                heldKeyVk = 0;
            } else if (heldKeyVk != 0) {
                SendKey(heldKeyVk, false);
                heldKeyVk = 0;
            }
            ReleaseAllHeldInputs();
            clearLockedScreen();
            if (ocrSessionHeld) ReleaseOcrSession();
            if (wmCfg.enabled) wmExec.EndRun();
            breakout_input::UninstallBreakoutHooks();
            workerUsesOcrVars_ = false;
            PostMessageW(hwnd_, WM_RUN_DONE, 0, 0);
        });
    }

    void StopRun() {
        stopFlag_ = true;
        aiHttpAbort_.Abort();
        ghHotkeyPending = false;
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
    void ForceEndBreakoutUiState() {
        const bool wasPaused = breakoutPaused_.load(std::memory_order_relaxed);
        const bool hadBreakoutTaskbar = breakoutTaskbarShown_;
        breakoutPaused_ = false;
        breakoutUserInput_ = false;
        if (!hwnd_ || !IsWindow(hwnd_)) {
            breakoutTaskbarShown_ = false;
            breakoutPlacement_ = BreakoutTaskbarPlacement{};
            if (wasPaused || hadBreakoutTaskbar) RestoreIconsAfterBreakout();
            return;
        }
        SetWindowCloaked(hwnd_, false);
        if (breakoutTaskbarShown_) {
            breakoutTaskbarShown_ = false;
            breakoutUiVisibleOnScreen_ = false;
            SetWindowTextW(hwnd_, L"鼠大侠-鼠标宏");
            const LONG_PTR ex = breakoutPlacement_.saved ? breakoutPlacement_.exStyle
                : (runSavedRectValid_ ? runSavedExStyle_ : GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
            if (breakoutPlacement_.saved) {
                const int w = std::max(static_cast<int>(breakoutPlacement_.rect.right - breakoutPlacement_.rect.left), 1);
                const int h = std::max(static_cast<int>(breakoutPlacement_.rect.bottom - breakoutPlacement_.rect.top), 1);
                SetWindowPos(hwnd_, nullptr, breakoutPlacement_.rect.left, breakoutPlacement_.rect.top, w, h,
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                if (wasMinimizedBeforeRun_) {
                    ShowWindow(hwnd_, SW_MINIMIZE);
                }
            }
            breakoutPlacement_.saved = false;
        }
        if (wasPaused || hadBreakoutTaskbar) RestoreIconsAfterBreakout();
    }

    void CloseWindowDuringRun() {
        hiddenToTray_ = true;
        EnsureTrayIcon();
        if (breakoutPaused_.load(std::memory_order_relaxed)) {
            if (!breakoutTaskbarShown_) {
                ShowBreakoutTaskbarPresence();
            } else {
                MinimizeBreakoutWindowForUser();
            }
        } else if (appSettings_.other.autoHideMainWindow) {
            ShowWindow(hwnd_, SW_HIDE);
        } else {
            HideToTray();
        }
        UpdateStatusTip();
    }

    void RestoreMainWindowAfterRun() {
        if (!hwnd_) return;
        SetWindowCloaked(hwnd_, false);
        breakoutTaskbarShown_ = false;
        breakoutUiVisibleOnScreen_ = false;

        const LONG_PTR ex = runSavedRectValid_ ? runSavedExStyle_
            : (breakoutPlacement_.saved ? breakoutPlacement_.exStyle
                : GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);

        RECT rc{};
        if (runSavedRectValid_) rc = runSavedRect_;
        else if (breakoutPlacement_.saved) rc = breakoutPlacement_.rect;
        else GetWindowRect(hwnd_, &rc);

        const int w = std::max(static_cast<int>(rc.right - rc.left), 1);
        const int h = std::max(static_cast<int>(rc.bottom - rc.top), 1);

        if (appSettings_.other.autoHideMainWindow && !wasVisibleBeforeRun_) {
            ShowWindow(hwnd_, SW_HIDE);
        } else if (wasMinimizedBeforeRun_) {
            breakoutTaskbarTransition_ = true;
            if (IsIconic(hwnd_)) {
                ShowWindow(hwnd_, SW_RESTORE);
            }
            SetWindowPos(hwnd_, HWND_BOTTOM, rc.left, rc.top, w, h,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
            PrepareHwndTaskbarLivePreview(hwnd_);
            DwmFlush();
            ShowWindow(hwnd_, SW_MINIMIZE);
            TaskbarYieldMessages();
            breakoutTaskbarTransition_ = false;
        } else {
            SetWindowPos(hwnd_, HWND_TOP, rc.left, rc.top, w, h, SWP_SHOWWINDOW);
            ShowWindow(hwnd_, SW_SHOW);
            SetForegroundWindow(hwnd_);
        }

        breakoutPlacement_ = BreakoutTaskbarPlacement{};
        runSavedRectValid_ = false;
    }

    void OnRunDone() {
        running_ = false;
        ghHotkeySessionBusy.store(recording_ || clicking_, std::memory_order_relaxed);
        ghHotkeyPending = false;
        EndHighResTimer();
        breakout_input::UninstallBreakoutHooks();
        breakoutHookState_ = BreakoutHookState{};
        workerBreakoutTime_ = 0;
        breakoutUserInput_ = false;
        breakoutPaused_ = false;
        if (worker_.joinable()) worker_.detach();
        if (hwnd_ && IsWindow(hwnd_)) {
            SetWindowCloaked(hwnd_, false);
            BOOL off = FALSE;
            DwmSetWindowAttribute(hwnd_, DWMWA_FORCE_ICONIC_REPRESENTATION, &off, sizeof(off));
            DwmSetWindowAttribute(hwnd_, DWMWA_FREEZE_REPRESENTATION, &off, sizeof(off));
        }
        ApplyMainWindowNormalTaskbarPresentation(hwnd_);
        RestoreMainWindowAfterRun();
        if (hwnd_ && IsWindow(hwnd_) && !wasMinimizedBeforeRun_) {
            if (IsWindowVisible(hwnd_)) {
                ApplyMainWindowNormalTaskbarPresentation(hwnd_);
                RestoreWindowTaskbarLivePreview(hwnd_);
            }
        }
        EnsureTrayIcon();
        HideStatusTip();
    }

    bool NeedsBreakoutWindowRestore() const {
        return !breakoutUiVisibleOnScreen_;
    }

    void MinimizeBreakoutWindowForUser() {
        if (!hwnd_ || !breakoutPaused_.load(std::memory_order_relaxed) || !breakoutTaskbarShown_) return;
        breakoutUiVisibleOnScreen_ = false;
        if (!breakoutPlacement_.saved && runSavedRectValid_) {
            breakoutPlacement_.rect = runSavedRect_;
            breakoutPlacement_.exStyle = runSavedExStyle_;
            breakoutPlacement_.saved = true;
        }
        breakoutTaskbarTransition_ = true;
        MinimizeBreakoutWindowOnTaskbar(hwnd_, &breakoutPlacement_);
        breakoutTaskbarTransition_ = false;
    }

    void ReturnBreakoutWindowToTaskbar() {
        MinimizeBreakoutWindowForUser();
    }

    void ShowBreakoutTaskbarPresence() {
        if (!hwnd_) return;
        breakoutTaskbarShown_ = true;
        breakoutUiVisibleOnScreen_ = false;
        SetWindowTextW(hwnd_, L"鼠大侠-鼠标宏脱离中");
        if (!breakoutPlacement_.saved && runSavedRectValid_) {
            breakoutPlacement_.rect = runSavedRect_;
            breakoutPlacement_.exStyle = runSavedExStyle_;
            breakoutPlacement_.saved = true;
        }
        // 所有场景统一使用系统最小化按钮；不再 cloak，确保点击必定走 SC_RESTORE。
        breakoutTaskbarTransition_ = true;
        MinimizeBreakoutWindowOnTaskbar(hwnd_, &breakoutPlacement_);
        breakoutTaskbarTransition_ = false;
    }

    void HideBreakoutTaskbarPresence() {
        if (!hwnd_ || !breakoutTaskbarShown_) return;
        breakoutTaskbarShown_ = false;
        breakoutUiVisibleOnScreen_ = false;
        SetWindowTextW(hwnd_, L"鼠大侠-鼠标宏");
        const bool hide = running_
            && (appSettings_.other.autoHideMainWindow || wasMinimizedBeforeRun_);
        HideMainWindowFromTaskbarAfterBreakout(hwnd_, &breakoutPlacement_, hide);
        if (!hide && breakoutPlacement_.saved) {
            const int w = std::max(static_cast<int>(breakoutPlacement_.rect.right - breakoutPlacement_.rect.left), 1);
            const int h = std::max(static_cast<int>(breakoutPlacement_.rect.bottom - breakoutPlacement_.rect.top), 1);
            SetWindowPos(hwnd_, nullptr, breakoutPlacement_.rect.left, breakoutPlacement_.rect.top, w, h,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            if (wasMinimizedBeforeRun_) {
                ShowWindow(hwnd_, SW_MINIMIZE);
            }
        }
        breakoutPlacement_.saved = false;
    }

    void RestoreBreakoutWindowToScreen() {
        if (!hwnd_) return;
        hiddenToTray_ = false;
        SetWindowCloaked(hwnd_, false);
        if (IsIconic(hwnd_)) {
            ShowWindow(hwnd_, SW_RESTORE);
        }
        RECT rc{};
        if (breakoutPlacement_.saved) rc = breakoutPlacement_.rect;
        else if (runSavedRectValid_) rc = runSavedRect_;
        else GetWindowRect(hwnd_, &rc);
        const int w = std::max(static_cast<int>(rc.right - rc.left), 1);
        const int h = std::max(static_cast<int>(rc.bottom - rc.top), 1);
        if (breakoutPlacement_.saved) {
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, breakoutPlacement_.exStyle);
        } else if (runSavedRectValid_) {
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, runSavedExStyle_);
        }
        SetWindowPos(hwnd_, HWND_TOP, rc.left, rc.top, w, h, SWP_SHOWWINDOW);
        ShowWindow(hwnd_, SW_SHOW);
        SetWindowCloaked(hwnd_, false);
        RestoreWindowTaskbarLivePreview(hwnd_);
        TaskbarYieldMessages();
        SwitchToThisWindow(hwnd_, TRUE);
        SetForegroundWindow(hwnd_);
        breakoutUiVisibleOnScreen_ = true;
        if (breakoutPaused_.load(std::memory_order_relaxed) && breakoutTaskbarShown_) {
            ApplyMainBreakoutTaskbarPresentation(hwnd_);
        }
    }

    void ApplyAllBreakoutIcons() {
        if (!hwnd_) return;
        ApplyMainBreakoutTaskbarPresentation(hwnd_);
        EnsureTrayIcon();
    }

    void RestoreIconsAfterBreakout() {
        if (!hwnd_) return;
        ApplyMainWindowNormalTaskbarPresentation(hwnd_);
        if (IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
            RestoreWindowTaskbarLivePreview(hwnd_);
        } else if (IsIconic(hwnd_)) {
            RefreshWindowTaskbarGrouping(hwnd_);
        }
        EnsureTrayIcon();
    }

    void UpdateBreakoutPauseIcons() {
        if (!hwnd_) return;
        const bool paused = breakoutPaused_.load(std::memory_order_relaxed);
        if (paused) {
            // 脱离状态只改变主窗口；AI、调试等辅助窗口完全保持原任务栏与显示状态。
            ShowBreakoutTaskbarPresence();
            ApplyAllBreakoutIcons();
            UpdateStatusTip();
        } else {
            HideBreakoutTaskbarPresence();
            RestoreIconsAfterBreakout();
            UpdateStatusTip();
        }
    }
    void RemoveTrayIcon() {
        if (!trayActive_) return;
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        trayActive_ = false;
    }
    void EnsureTrayIcon() {
        if (!hwnd_ || !IsWindow(hwnd_)) return;
        if (!wmTaskbarCreated_) {
            wmTaskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        }
        HICON icon = breakoutPaused_.load(std::memory_order_relaxed)
            ? LoadBreakoutPauseIconSmall()
            : (running_ ? LoadTrayRunningIconSmall() : LoadAppIconSmall());
        if (!icon) icon = LoadIconW(nullptr, IDI_APPLICATION);
        if (!icon) return;

        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = 1;
        nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
        nid.uCallbackMessage = WM_TRAY;
        nid.hIcon = icon;
        if (breakoutPaused_.load(std::memory_order_relaxed)) wcscpy_s(nid.szTip, L"鼠大侠-鼠标宏脱离中");
        else if (clicking_) wcscpy_s(nid.szTip, L"鼠大侠-连点运行中");
        else if (recording_) wcscpy_s(nid.szTip, L"鼠大侠-录制中");
        else if (running_) wcscpy_s(nid.szTip, L"鼠大侠-鼠标宏运行中");
        else wcscpy_s(nid.szTip, L"鼠大侠");

        // 仅在系统确认成功后标记 trayActive_；MODIFY 失败则回退 ADD（Explorer 清图标后常见）
        if (trayActive_) {
            if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
                trayActive_ = false;
                if (Shell_NotifyIconW(NIM_ADD, &nid)) trayActive_ = true;
            }
        } else if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            trayActive_ = true;
        } else if (Shell_NotifyIconW(NIM_MODIFY, &nid)) {
            trayActive_ = true;
        }
    }
    void AddTray() { EnsureTrayIcon(); }
    void RemoveTray() {
        // 停止录制/连点后刷新提示；常驻托盘不删除（关闭到托盘依赖它）
        EnsureTrayIcon();
    }
    void RestoreMainWindowForUser() {
        if (!hwnd_) return;
        hiddenToTray_ = false;
        if (breakoutPaused_.load(std::memory_order_relaxed) && breakoutTaskbarShown_) {
            if (!breakoutUiVisibleOnScreen_) {
                RestoreBreakoutWindowToScreen();
            } else {
                SwitchToThisWindow(hwnd_, TRUE);
                SetForegroundWindow(hwnd_);
            }
            return;
        }
        SetWindowCloaked(hwnd_, false);
        if (IsIconic(hwnd_)) {
            ShowWindow(hwnd_, SW_RESTORE);
        } else {
            ShowWindow(hwnd_, SW_SHOW);
        }
        SetForegroundWindow(hwnd_);
        EnsureTrayIcon();
    }

    void QuitApplication() {
        // 可重入保护。
        static std::atomic_bool quitting{false};
        if (quitting.exchange(true)) {
            TerminateProcess(GetCurrentProcess(), 0);
        }

        // ExitProcess 可能卡在 VDA/WinRT 的 DLL_PROCESS_DETACH，进程变幽灵。
        // 看门狗与最终退出一律用 TerminateProcess，保证任务管理器里一定消失。
        std::thread([] {
            Sleep(400);
            TerminateProcess(GetCurrentProcess(), 0);
        }).detach();

        // 1) 立刻摘托盘 + 藏窗口。
        RemoveTrayIcon();
        if (hwnd_ && IsWindow(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
        macroDebugWindow_.Hide();
        if (statusTipWindow_ && IsWindow(statusTipWindow_)) ShowWindow(statusTipWindow_, SW_HIDE);
        for (const auto& dialog : agentDialogs_) {
            if (dialog && dialog->IsAlive()) ShowWindow(dialog->Hwnd(), SW_HIDE);
        }

        // 2) 只发停止信号，不做 join / Unhook / DestroyWindow。
        stopFlag_ = true;
        clicking_ = false;
        running_ = false;
        g_recording = false;
        recording_ = false;
        aiHttpAbort_.Abort();
        ghHotkeyEnabled = false;

        // 3) 尽力保存（卡住由看门狗 TerminateProcess）。
        try {
            SaveHomeState();
        } catch (...) {
        }

        TerminateProcess(GetCurrentProcess(), 0);
    }

    void RestoreFromTray() {
        // 设置 / 录制优化打开时优先激活对应窗口
        if (HWND settings = SettingsDialog::ActiveHwnd()) {
            if (IsIconic(settings)) ShowWindow(settings, SW_RESTORE);
            else ShowWindow(settings, SW_SHOW);
            SwitchToThisWindow(settings, TRUE);
            SetForegroundWindow(settings);
            EnsureTrayIcon();
            return;
        }
        if (HWND opt = RecordingOptimizeDialog::ActiveHwnd()) {
            if (IsIconic(opt)) ShowWindow(opt, SW_RESTORE);
            else ShowWindow(opt, SW_SHOW);
            SwitchToThisWindow(opt, TRUE);
            SetForegroundWindow(opt);
            EnsureTrayIcon();
            return;
        }
        RestoreMainWindowForUser();
    }
    void HideToTray() {
        hiddenToTray_ = true;
        EnsureTrayIcon();
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
            trayMenuOpen_ = true;
            const TrayMenuAction action = TrayMenu::Show(hwnd_, pt);
            trayMenuOpen_ = false;
            switch (action) {
            case TrayMenuAction::ShowWindow:
                RestoreFromTray();
                break;
            case TrayMenuAction::Exit:
                // 菜单嵌套循环已结束，直接硬退出（不要 PostMessage，避免丢失）。
                QuitApplication();
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
        // 键鼠录制始终采屏幕绝对坐标；录制脚本保存时也会强制关闭 windowMode。
        // 勿用编辑器残留的 scriptWindowMode_ 弹「窗口模式无法录制」（易误导）。
        const auto requestedMode = recorderSettings_.inputMode == quickscript::RecorderInputMode::DesktopAbsolute
            ? RecordingCaptureMode::Absolute
            : (recorderSettings_.inputMode == quickscript::RecorderInputMode::FpsRelative
                ? RecordingCaptureMode::Relative : RecordingCaptureMode::Auto);
        SetRecordingCaptureMode(requestedMode);
        BeginHighResTimer();
        if (!InstallRecordingHooks()) {
            UninstallRecordingHooks();
            EndHighResTimer();
            SetRecordingIgnoreHotkey(0, 0, false);
            ShowPromptInfo(requestedMode != RecordingCaptureMode::Absolute
                ? L"高精度录制启动失败：Raw Input 初始化失败。"
                : L"录制钩子初始化失败。");
            return;
        }
        InitRecordingClock();
        {
            std::lock_guard<std::mutex> lock(g_recordMutex);
            g_recordedEvents.clear();
            const bool startsRelative = requestedMode == RecordingCaptureMode::Relative
                || (requestedMode == RecordingCaptureMode::Auto && IsRelativeMouseCaptureActive());
            if (!startsRelative) {
                RecordedEvent initialPos{};
                initialPos.timeOffsetUs = 0;
                initialPos.sequence = 0;
                initialPos.msg = WM_MOUSEMOVE;
                initialPos.source = RecordedEventSource::Synthetic;
                POINT pt{}; GetCursorPos(&pt);
                initialPos.x = pt.x; initialPos.y = pt.y;
                g_recordedEvents.push_back(initialPos);
            }
        }
        g_recording = true;
        recording_ = true;
        ghHotkeySessionBusy.store(true, std::memory_order_relaxed);
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
        recording_ = false;
        ghHotkeySessionBusy.store(clicking_ || running_, std::memory_order_relaxed);
        ghHotkeyPending = false;
        SetRecordingIgnoreHotkey(0, 0, false);
        // 先卸钩并冲刷 Raw 累计（此时仍保持 g_recording，避免丢最后一段相对位移）
        UninstallRecordingHooks();
        g_recording = false;
        EndHighResTimer();
        RemoveTray();
        HideStatusTip();
        if (appSettings_.other.autoHideMainWindow && recordingWasVisible_) ShowWindow(hwnd_, SW_SHOW);
        ConvertRecordedToActions();
        if (!actions_.empty()) {
            SaveRecording();
        } else {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        UpdateStatusTip();
    }

    void ConvertRecordedToActions() {
        std::vector<RecordedEvent> events;
        {
            std::lock_guard<std::mutex> lock(g_recordMutex);
            events = g_recordedEvents;
        }
        RecordingConversionResult converted =
            ConvertRecordedEventsToActions(std::move(events), globalHotkey_);
        actions_ = std::move(converted.actions);
        saveDurationSeconds_ = converted.durationSeconds;
        currentRecordingCaptureMode_ = static_cast<int>(recorderSettings_.inputMode);
        currentInputTimingVersion_ = 1;
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
        ghHotkeySessionBusy.store(true, std::memory_order_relaxed);
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
        ghHotkeySessionBusy.store(recording_ || running_, std::memory_order_relaxed);
        ghHotkeyPending = false;
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
        const int rowH = std::max(1, UiLen(kRowH));
        const int y = list.top + (slot - scrollOffset_) * rowH;
        return RECT{list.left, y, ActionListContentRight(), y + rowH};
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
        const int rowH = std::max(1, UiLen(kRowH));
        const int visibleSlot = scrollOffset_ + (y - list.top) / rowH;
        const auto& vis = VisibleActionIndices();
        if (visibleSlot < 0 || visibleSlot >= static_cast<int>(vis.size())) return -1;
        return vis[static_cast<size_t>(visibleSlot)];
    }
    RECT ExpandToggleRect(int index) const {
        if (index < 0 || index >= static_cast<int>(actions_.size())) return RECT{};
        const auto& a = actions_[static_cast<size_t>(index)];
        if (!IsExpandableContainer(a.type)) return RECT{};
        RECT r = RowRect(index);
        const int pad = UiLen(kListInnerPad);
        const int expandLeft = ExpandToggleLeftLocal(a.indent, batchEditMode_, pad);
        const int toggleW = std::max(1, UiLen(kExpandToggleWidth));
        return RECT{r.left + expandLeft, r.top + UiLen(8), r.left + expandLeft + toggleW, r.bottom - UiLen(8)};
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
        const bool clearedSelection = selectedIndex_ >= row && selectedIndex_ < end;
        collapsedContainers_ = RemapCollapsedAfterDelete(collapsedContainers_, row, end);
        actions_.erase(actions_.begin() + row, actions_.begin() + end);
        if (clearedSelection) selectedIndex_ = -1;
        else if (selectedIndex_ >= end) selectedIndex_ -= count;
        if (hoverIndex_ >= row && hoverIndex_ < end) hoverIndex_ = -1;
        else if (hoverIndex_ >= end) hoverIndex_ -= count;
        RenumberActions();
        RefreshRunBlockCombo();
        // 删除当前选中项后恢复添加草稿（无草稿则该类型默认值）
        if (clearedSelection && !loadingForm_) RestoreActionFormDraftForCurrentType();
        UpdateEditMode();
        OnActionsChanged();
    }

    DragInsertTarget HitInsertTarget(int x, int y) const {
        DragInsertTarget target{};
        const auto vis = VisibleActionIndices();
        if (actions_.empty()) return target;
        RECT list = ActionListRect();
        const int listH = static_cast<int>(list.bottom - list.top);
        const int rowH = std::max(1, UiLen(kRowH));
        const int relativeY = std::clamp(y - static_cast<int>(list.top), 0, listH);
        const int visibleRow = std::clamp(scrollOffset_ + relativeY / rowH, 0, std::max(0, static_cast<int>(vis.size()) - 1));
        const int inRowY = relativeY % rowH;
        const int slot = std::clamp(visibleRow + (inRowY > rowH / 2 ? 1 : 0), 0, static_cast<int>(vis.size()));
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
            const int nextSel = selectedIndex_ == dragIndex_ ? -1 : dragIndex_;
            if (nextSel >= 0 && selectedIndex_ < 0 && !loadingForm_) {
                // 选中已添加动作前：只保存当前「添加表单」草稿（须在 LoadForm 列表项之前）
                SaveCurrentActionFormDraft();
            } else if (nextSel < 0 && selectedIndex_ >= 0 && !loadingForm_) {
                selectedIndex_ = -1;
                RestoreActionFormDraftForCurrentType();
                UpdateEditMode();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            selectedIndex_ = nextSel;
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
        for (auto& action : block) action.indent = std::max(0, action.indent + indentDelta);
        int insertIndex = dragTargetIndex_;
        if (insertIndex > dragIndex_) insertIndex -= count;
        insertIndex = std::clamp(insertIndex, 0, static_cast<int>(actions_.size()));
        {
            std::vector<ScriptAction> trial = actions_;
            trial.erase(trial.begin() + dragIndex_, trial.begin() + dragEnd);
            trial.insert(trial.begin() + insertIndex, block.begin(), block.end());
            if (const std::wstring endLoopErr = ValidateEndLoopPlacements(trial); !endLoopErr.empty()) {
                ShowPromptInfo(kEndLoopNeedsLoopParentMsg);
                dragIndex_ = -1;
                dragMoved_ = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
        }
        actions_.erase(actions_.begin() + dragIndex_, actions_.begin() + dragEnd);
        collapsedContainers_ = RemapCollapsedAfterMove(collapsedContainers_, dragIndex_, dragEnd, insertIndex, count);
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
        const int maxScroll = ActiveHomeListMaxScroll();
        const int trackHeight = static_cast<int>(track.bottom - track.top);
        const int thumbHeight = static_cast<int>(thumb.bottom - thumb.top);
        const int range = std::max(1, trackHeight - thumbHeight);
        const int thumbOffset = thumbTop - static_cast<int>(track.top);
        homeScrollOffset_ = std::clamp(thumbOffset * maxScroll / range, 0, maxScroll);
    }

    void OnWheel(int delta) {
        if (promptModal_.visible()) return;
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
            if (activeHomeTab_ == quickscript::MainTab::Macro
                || activeHomeTab_ == quickscript::MainTab::Recorder
                || activeHomeTab_ == quickscript::MainTab::ScriptCustom) {
                const int maxScroll = ActiveHomeListMaxScroll();
                if (maxScroll > 0) {
                    const int step = std::max(1, UiLen(kHomeCardStep));
                    const int old = homeScrollOffset_;
                    homeScrollOffset_ = std::clamp(
                        homeScrollOffset_ + (delta < 0 ? step : -step), 0, maxScroll);
                    if (old != homeScrollOffset_) {
                        POINT cursorPt{};
                        GetCursorPos(&cursorPt);
                        ScreenToClient(hwnd_, &cursorPt);
                        if (activeHomeTab_ == quickscript::MainTab::Macro)
                            homeHover_ = HitHomeCard(cursorPt.x, cursorPt.y);
                        else if (activeHomeTab_ == quickscript::MainTab::Recorder)
                            recordingHover_ = HitRecordingCard(cursorPt.x, cursorPt.y);
                        else
                            agentConvHover_ = HitAgentConvCard(cursorPt.x, cursorPt.y);
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                }
            }
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
        if (oldScroll != scrollOffset_) {
            // 滚动后按光标位置重算 hover，避免旧行高亮跟着滚走再被新行替换造成闪烁
            POINT cursorPt{};
            GetCursorPos(&cursorPt);
            ScreenToClient(hwnd_, &cursorPt);
            hoverIndex_ = HitRow(cursorPt.x, cursorPt.y);
            RefreshActionListLayer();
        }
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
        DrawTopActionGlyph(hdc, rc, iconType);
        SelectObject(hdc, homeFont_);
        DrawTextIn(hdc, text, RECT{rc.left + UiLen(32), rc.top, rc.right, rc.bottom}, kWhite);
    }

    void DrawMacroCreatePrompt(HDC hdc, const RECT& cr) {
        const wchar_t* partClick = L"点击";
        const wchar_t* partCreate = L"创建";
        const wchar_t* partSuffix = L"鼠标宏";
        const int gap = UiLen(12);
        const int padV = UiLen(21);
        const int createBtnW = UiLen(78);
        const int wClick = TextWidth(partClick, bigFont_);
        const int wSuffix = TextWidth(partSuffix, bigFont_);
        const int total = wClick + gap + createBtnW + gap + wSuffix;
        int x = cr.left + (cr.right - cr.left - total) / 2;
        DrawTextIn(hdc, partClick, RECT{x, cr.top, x + wClick, cr.bottom}, kBannerText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        x += wClick + gap;
        RECT btn{x, cr.top + padV, x + createBtnW, cr.bottom - padV};
        FillRectColor(hdc, btn, kOrange);
        DrawTextIn(hdc, partCreate, btn, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        x += createBtnW + gap;
        DrawTextIn(hdc, partSuffix, RECT{x, cr.top, x + wSuffix, cr.bottom}, kBannerText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    void DrawTitleButtons(HDC hdc) {
        RECT close = CloseRect();
        if (page_ == Page::Home) {
            RECT settings = SettingsRect();
            RECT minimize = MinimizeRect();
            if (hoverButton_ == HoverButton::Settings) FillAlphaRect(hdc, settings, RGB(0, 0, 0), kCloseHoverAlpha);
            if (hoverButton_ == HoverButton::Minimize) FillAlphaRect(hdc, minimize, RGB(0, 0, 0), kCloseHoverAlpha);
            DrawGearGlyph(hdc, settings, kWhite, kNavStripGreen);
            SelectObject(hdc, closeFont_);
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
        SelectObject(hdc, homeTabFont_);
        DrawTextIn(hdc, L"鼠大侠", RECT{UiLen(14), 0, UiLen(120), UiLen(kTitleH)}, kWhite);
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

    std::wstring ClickIntervalComboText() const {
        if (clickerSettings_.intervalMode == quickscript::ClickIntervalMode::Custom) {
            wchar_t buf[64]{};
            swprintf_s(buf, L"间隔%.3f秒点击", clickerSettings_.customIntervalSeconds);
            return buf;
        }
        return ClickIntervalTitle(clickerSettings_.intervalMode);
    }

    RECT ClickerCustomHintRect() const {
        struct Segment { const wchar_t* text; };
        static const Segment kSegments[] = {
            {L"点击"}, {L"修改时间间隔"}, {L"，点击下拉箭头可快速设置"},
            {L"极速模式"}, {L"和"}, {L"高效模式"},
        };
        int textW = UiLen(16);
        for (const auto& seg : kSegments) {
            textW += TextWidth(seg.text, homeFont_);
        }
        return UiRect4(kClickerLabelX, 203, kClickerLabelX + textW, 231);
    }

    void DrawClickerCustomHint(HDC hdc) {
        const RECT rc = ClickerCustomHintRect();
        FillRectColor(hdc, rc, kCreateYellow);
        SelectObject(hdc, homeFont_);
        struct Segment { const wchar_t* text; COLORREF color; };
        static const Segment kSegments[] = {
            {L"点击", kBannerText},
            {L"修改时间间隔", kMainGreen},
            {L"，点击下拉箭头可快速设置", kBannerText},
            {L"极速模式", RGB(220, 60, 60)},
            {L"和", kBannerText},
            {L"高效模式", kMainGreen},
        };
        int x = rc.left + UiLen(8);
        const int y = rc.top;
        const int h = rc.bottom - rc.top;
        for (const auto& seg : kSegments) {
            const int w = TextWidth(seg.text, homeFont_);
            DrawTextIn(hdc, seg.text, RECT{x, y, x + w, y + h}, seg.color, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            x += w;
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
        FillRectColor(hdc, rc, kWhite);
        DrawTextIn(hdc, text, RECT{rc.left + 8, rc.top, rc.right - 36, rc.bottom}, kMainGreen);
        const int arrowCenterX = rc.right - 16;
        const int arrowCenterY = rc.top + 15;
        DrawComboDownArrow(hdc, arrowCenterX, arrowCenterY, kMainGreen);
        DrawBorderRect(hdc, rc, borderColor);
    }

    void DrawClickerCombo(HDC hdc, RECT rc, bool dropped = false) {
        DrawClickerCombo(hdc, rc, ClickIntervalComboText(), dropped);
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
        case 23: return &popupWmSelectMethod_;
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
        case 23: return WindowClientRect(wmSelectMethod_);
        case 1: return WindowClientRect(actionCombo_);
        case 2: return EditorComboClientRect(mousePressButton_);
        case 3: return EditorComboClientRect(clickButton_);
        case 4: return EditorComboClientRect(loopTypeCombo_);
        case 5: return EditorComboClientRect(runBlockCombo_);
        case 6: return EditorComboClientRect(hotkeyShortcutCombo_);
        case 7: {
            HWND combo = ActiveVarComboHwnd();
            return combo ? EditorComboClientRect(combo) : RECT{};
        }
        case 8: return EditorComboClientRect(runMacroCombo_);
        case 9: return EditorComboClientRect(mousePlaybackCombo_);
        case 10: return EditorComboClientRect(scrollDirectionCombo_);
        case 11: return EditorComboClientRect(findFollowUpCombo_);
        case 12: return EditorComboClientRect(ifVarCombo_);
        case 13: return EditorComboClientRect(ifOperatorCombo_);
        case 14: return EditorComboClientRect(ifConnectorCombo_);
        case 15: return EditorComboClientRect(runProgramCombo_);
        case 16: return EditorComboClientRect(ocrResultModeCombo_);
        case 17: return EditorComboClientRect(ocrFollowUpCombo_);
        case 18: return EditorComboClientRect(ocrSearchVarCombo_);
        case 19: return EditorComboClientRect(aiModelCombo_);
        case 20: return EditorComboClientRect(aiContextModeCombo_);
        case 21: return EditorComboClientRect(aiOutputTypeCombo_);
        case 22: return EditorComboClientRect(aiSearchRegionCombo_);
        default: return RECT{};
        }
    }

    RECT EditorComboInvalidateRect(int popupId) const {
        HWND combo = nullptr;
        switch (popupId) {
        case 0: combo = mode_; break;
        case 23: combo = wmSelectMethod_; break;
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
        RECT rc = EditorComboClientRect(combo);
        if (popupId == 0) {
            const RECT header = EditorMacroHeaderTextRect();
            rc.left = std::max<LONG>(0, rc.left - ScaleEditorX(50));
            rc.top = std::min(rc.top, header.top);
            rc.bottom = std::max(rc.bottom, header.bottom);
        } else if (popupId == 23) {
            const RECT header = EditorMacroHeaderTextRect();
            rc.left = std::max<LONG>(0, rc.left - ScaleEditorX(108));
            rc.top = std::min(rc.top, header.top);
            rc.bottom = std::max(rc.bottom, header.bottom);
        } else if (popupId == 1) rc.top = std::max<LONG>(0, rc.top - 28);
        InflateRect(&rc, 3, 3);
        return rc;
    }

    void InvalidateEditorComboArea(int popupId) {
        if (popupId < 0) return;
        const RECT rc = EditorComboInvalidateRect(popupId);
        if (rc.right <= rc.left || rc.bottom <= rc.top) return;
        HWND combo = nullptr;
        switch (popupId) {
        case 0: combo = mode_; break;
        case 23: combo = wmSelectMethod_; break;
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
        default: break;
        }
        if (combo && IsParamViewportChild(combo) && paramViewport_) {
            const RECT vp = ParamViewportRect();
            RECT inv{
                rc.left - vp.left, rc.top - vp.top,
                rc.right - vp.left, rc.bottom - vp.top
            };
            InvalidateRect(paramViewport_, &inv, FALSE);
            RepaintParamPanelChrome();
        } else {
            InvalidateRect(hwnd_, &rc, FALSE);
        }
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
        SetWindowPos(editorDropPopup_, HWND_TOPMOST, x, y, w, h,
            SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
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
            ShowPromptInfo(L"该动作类型暂未实现。");
            CloseEditorPopup();
            return;
        }
        const bool switchingActionType = editorPopupOpen_ == 1;
        const int prevActionSel = switchingActionType ? popupAction_.sel : -1;
        // 未选中列表项时：切换类型先存当前「添加表单」草稿，再恢复目标类型草稿（无则默认）
        // 注意：须在表单已与 selectedIndex_ 一致时调用；取消选中路径必须先 Restore，否则会把列表动作的勾选写进草稿
        if (switchingActionType && !loadingForm_ && selectedIndex_ < 0 && prevActionSel >= 0) {
            actionFormDrafts_[prevActionSel] = ActionFromForm();
        }
        pc->sel = idx;
        HWND label = nullptr;
        switch (editorPopupOpen_) {
        case 0: label = mode_; break;
        case 23: label = wmSelectMethod_; break;
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
        if (switchingActionType) {
            if (!loadingForm_ && selectedIndex_ < 0) {
                RestoreActionFormDraftForCurrentType();
            } else if (!loadingForm_ && selectedIndex_ >= 0) {
                // 已选中列表动作时改类型：用新类型默认值（保留备注），不碰添加草稿
                ScriptAction next = DefaultActionForPopupSel(idx);
                next.remark = GetText(remark_);
                LoadForm(next);
            } else {
                RefreshParamPanel();
            }
            // 下拉在 LBUTTONDOWN 选中；匹配的 UP 可能落在新露出的「同时按住」勾选框上
            DiscardSpuriousEditorInput();
        } else if (editorPopupOpen_ == 0) {
            if (page_ == Page::Editor) UpdateEditorWindowModeChrome();
            SyncScriptWindowModeFromEditor();
        } else if (editorPopupOpen_ == 23) {
            SyncScriptWindowModeFromEditor();
            // 切换「选择窗口方式」后必须刷新，「指定窗口类」按钮显隐依赖 sel==2。
            if (page_ == Page::Editor) UpdateEditorWindowModeChrome();
        } else if (editorPopupOpen_ == 11) RefreshFindImageSubPanel();
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
        ::FillAlphaRect(hdc, rc, color, alpha);
    }

    bool IsHotkeyMenuChecked(int id) const {
        if (id == kHotLeft) return globalHotkey_.vk == VK_LBUTTON && globalHotkey_.modifiers == 0;
        if (id == kHotMiddle) return globalHotkey_.vk == VK_MBUTTON && globalHotkey_.modifiers == 0;
        if (id == kHotRight) return globalHotkey_.vk == VK_RBUTTON && globalHotkey_.modifiers == 0;
        if (id == kHotX1) return globalHotkey_.vk == VK_XBUTTON1 && globalHotkey_.modifiers == 0;
        if (id == kHotX2) return globalHotkey_.vk == VK_XBUTTON2 && globalHotkey_.modifiers == 0;
        if (id == kHotSpace) return globalHotkey_.vk == VK_SPACE && globalHotkey_.modifiers == 0;
        return id == kHotCustom && globalHotkey_.enabled
            && !IsHotkeyMenuChecked(kHotLeft) && !IsHotkeyMenuChecked(kHotMiddle)
            && !IsHotkeyMenuChecked(kHotRight) && !IsHotkeyMenuChecked(kHotX1)
            && !IsHotkeyMenuChecked(kHotX2) && !IsHotkeyMenuChecked(kHotSpace);
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
        FillRectColor(dis->hDC, rc, bg);
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
        const COLORREF bg = (checked || selected) ? kBatchSelectedRow : RGB(255, 255, 255);
        FillRectColor(dis->hDC, rc, bg);

        RECT box{rc.left + 11, rc.top + 12, rc.left + 23, rc.top + 24};
        DrawCheckbox(dis->hDC, box, checked);

        SelectObject(dis->hDC, font_);
        DrawTextIn(dis->hDC, item->title, RECT{rc.left + 46, rc.top + 3, rc.right - 10, rc.top + 22}, RGB(30, 30, 30));
        DrawTextIn(dis->hDC, item->desc, RECT{rc.left + 46, rc.top + 23, rc.right - 10, rc.bottom - 3}, RGB(145, 150, 155));
    }

    void DrawOwnerItem(DRAWITEMSTRUCT* dis) {
        if (!dis) return;
        if (dis->CtlType == ODT_MENU) { DrawHotkeyMenuItem(dis); return; }
        if (dis->CtlType == ODT_COMBOBOX) { DrawComboItem(dis); return; }
        // 勾选框优先：避免被后面的绿按钮分支误吃
        if (IsMarkedParamCheckbox(dis->hwndItem)) { DrawParamPanelCheckboxItem(dis); return; }
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
            || hwnd == aiSelectRegionBtn_ || hwnd == aiSelectRegionBtn2_
            || hwnd == wmTargetBrowseBtn_;
    }

    HWND HitGrayButton(int x, int y) const {
        if (page_ != Page::Editor) return nullptr;
        auto testChild = [&](HWND child) -> HWND {
            if (!IsGrayButton(child) || !IsWindowVisible(child)) return nullptr;
            if (PtIn(WindowClientRect(child), x, y)) return child;
            return nullptr;
        };
        auto scanParent = [&](HWND parent) -> HWND {
            if (!parent) return nullptr;
            for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                if (HWND h = testChild(child)) return h;
            }
            return nullptr;
        };
        if (HWND vpHit = scanParent(paramViewport_)) return vpHit;
        return scanParent(hwnd_);
    }

    void InvalidateGrayButton(HWND btn) {
        if (!btn) return;
        RedrawWindow(btn, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
    }

    void UpdateGrayButtonHover(int x, int y) {
        HWND hit = HitGrayButton(x, y);
        if (hit == hoverGrayBtn_) return;
        HWND old = hoverGrayBtn_;
        hoverGrayBtn_ = hit;
        pendingHoverGrayOld_ = old;
        pendingHoverGrayNew_ = hit;
        FiDbgOnGrayHoverChanged(old, hit);
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
        FiDbgBumpGrayDraw(dis->hwndItem, dis->itemState, hoverGrayBtn_, false);
        HDC hdc = dis->hDC;
        RECT rc = dis->rcItem;
        const bool hovered = dis->hwndItem == hoverGrayBtn_;
        const COLORREF fill = hovered ? kGrayButtonHover : kGrayButton;
        const COLORREF border = hovered ? kMainGreen : kGrayButtonBorder;
        // 先铺面板底色再填按钮，避免滚动后边缘透出旧像素
        FillRectColor(hdc, rc, kWhite);
        FillRectColor(hdc, rc, fill);
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
        SelectObject(hdc, editorFont_ ? editorFont_ : font_);
        DrawTextIn(hdc, text, rc, kGrayButtonText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
        // 先铺白底再画圆角，清掉四角残留（滚动/移位后常见）
        FillRectColor(hdc, rc, kWhite);
        FillRoundRectColor(hdc, rc, fill, 5);
        wchar_t text[64]{};
        GetWindowTextW(dis->hwndItem, text, 64);
        SelectObject(hdc, font_);
        DrawTextIn(hdc, text, rc, disabled ? kButtonDisabledText : kWhite,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // ── WM_PAINT entry point ───────────────────────────────────────
    void Paint() {
        PAINTSTRUCT ps{}; HDC windowDc = BeginPaint(hwnd_, &ps); RECT rc{}; GetClientRect(hwnd_, &rc);
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        HDC hdc = CreateCompatibleDC(windowDc);
        HBITMAP bmp = CreateCompatibleBitmap(windowDc, w, h);
        HGDIOBJ oldBmp = SelectObject(hdc, bmp);
        RenderBatchScope batch(hdc);
        RECT title{0, 0, rc.right, UiLen(kTitleH)};
        RECT body{0, UiLen(kTitleH), rc.right, rc.bottom - (page_ == Page::Editor ? UiLen(kBottomH) : 0)};
        FillRectColor(hdc, title, kMainGreen);
        FillRectColor(hdc, body, page_ == Page::Home ? kMainGreen : kWhite);
        if (page_ == Page::Editor) {
            FillRectColor(hdc, RECT{0, rc.bottom - UiLen(kBottomH), rc.right, rc.bottom}, kPanel);
        }
        SelectObject(hdc, page_ == Page::Editor ? editorFont_ : titleFont_);
        if (page_ == Page::Editor) DrawTextIn(hdc, L"◴ 鼠大侠-鼠标宏", RECT{UiLen(14), 0, UiLen(360), UiLen(kTitleH)}, kWhite);
        SelectObject(hdc, font_);
        if (page_ == Page::Home) {
            PaintHome(hdc, w, h);
        } else {
            PaintEditor(hdc);
        }
        DrawTitleButtons(hdc);
        batch.End();
        const int blitW = ps.rcPaint.right - ps.rcPaint.left;
        const int blitH = ps.rcPaint.bottom - ps.rcPaint.top;
        // 打开过渡整客户区 BitBlt：ExcludeClipRect 会在子控件未画完时露出主页残留
        if (!editorFullClientBlit_ && !editorOpenPending_) {
            for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                if (!IsWindowVisible(child)) continue;
                RECT childRc = PaintExcludeRectForChild(child);
                if (childRc.right <= childRc.left || childRc.bottom <= childRc.top) continue;
                RECT inter{};
                if (!IntersectRect(&inter, &childRc, &ps.rcPaint)) continue;
                ExcludeClipRect(windowDc, childRc.left, childRc.top, childRc.right, childRc.bottom);
            }
        }
        BitBlt(windowDc, ps.rcPaint.left, ps.rcPaint.top, blitW, blitH, hdc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(hdc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(hdc);
        EndPaint(hwnd_, &ps);
        if (page_ == Page::Editor && !promptModal_.visible()) {
            RepaintParamPanelChrome();
            HDC chromeDc = GetDC(hwnd_);
            PaintEditorListHeaderChrome(chromeDc);
            ReleaseDC(hwnd_, chromeDc);
        }
    }

    // ── Home screen painting ───────────────────────────────────────
    void PaintHome(HDC hdc, int clientW, int clientH) {
        FillRectColor(hdc, RECT{0, 0, clientW, UiLen(kHomeContentTop)}, kNavStripGreen);
        FillRectColor(hdc, RECT{0, UiLen(kHomeFooterTop), clientW, clientH}, kNavStripGreen);
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
        DrawTextIn(hdc, L"点击类型:", RECT{UiLen(kClickerLabelX), UiLen(169), clickerLayout_.leftRadioLeft - UiLen(kClickerFieldGap), UiLen(194)}, kWhite);
        DrawRadio(hdc, ClickerLeftRadioRect(), clickerSettings_.button == quickscript::MouseButtonChoice::Left);
        DrawTextIn(hdc, L"鼠标左键", RECT{clickerLayout_.leftRadioLeft + UiLen(kClickerRadioSize) + UiLen(9), UiLen(169),
            clickerLayout_.middleRadioLeft - UiLen(17), UiLen(194)}, kWhite);
        DrawRadio(hdc, ClickerMiddleRadioRect(), clickerSettings_.button == quickscript::MouseButtonChoice::Middle);
        DrawTextIn(hdc, L"鼠标中键", RECT{clickerLayout_.middleRadioLeft + UiLen(kClickerRadioSize) + UiLen(9), UiLen(169),
            clickerLayout_.rightRadioLeft - UiLen(17), UiLen(194)}, kWhite);
        DrawRadio(hdc, ClickerRightRadioRect(), clickerSettings_.button == quickscript::MouseButtonChoice::Right);
        DrawTextIn(hdc, L"鼠标右键", RECT{clickerLayout_.rightRadioLeft + UiLen(kClickerRadioSize) + UiLen(9), UiLen(169), UiLen(kClickerComboRight), UiLen(194)}, kWhite);

        if (clickerSettings_.intervalMode == quickscript::ClickIntervalMode::Custom) {
            DrawClickerCustomHint(hdc);
        }
        DrawTextIn(hdc, L"每次点击间隔时间:", RECT{UiLen(kClickerLabelX), UiLen(238), clickerLayout_.intervalComboLeft - UiLen(kClickerFieldGap), UiLen(263)}, kWhite);
        DrawClickerCombo(hdc, ClickerIntervalRect(), ClickerIntervalPopupOpen());
        DrawTextIn(hdc, L"启停的全局热键:", RECT{UiLen(kClickerLabelX), UiLen(305), clickerLayout_.hotkeyComboLeft - UiLen(kClickerFieldGap), UiLen(330)}, kWhite);
        DrawClickerCombo(hdc, ClickerHotkeyRect(), globalHotkey_.text.empty() ? L"F8" : globalHotkey_.text, ClickerHotkeyPopupOpen());

        RECT hint = CreateRect();
        FillRectColor(hdc, hint, clicking_ ? RGB(255, 200, 200) : kCreateYellow);
        SelectObject(hdc, bigFont_);
        const std::wstring hotText = globalHotkey_.text.empty() ? L"F8" : globalHotkey_.text;
        RECT keyBox = ClickerBannerKeyRect();
        DrawTextIn(hdc, L"按", RECT{hint.left + UiLen(166), hint.top, keyBox.left - UiLen(5), hint.bottom}, kBannerText, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        FillRectColor(hdc, keyBox, clicking_ ? RGB(200, 50, 50) : kOrange);
        DrawTextIn(hdc, hotText, keyBox, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        const std::wstring actionText = clicking_
            ? (L"键停止 连点（当前 " + ClickerButtonTitle() + L" " + ClickIntervalComboText() + L"）")
            : (L"键开始 " + ClickerButtonTitle() + L" 连点");
        DrawTextIn(hdc, actionText, RECT{keyBox.right + UiLen(10), hint.top, hint.right - UiLen(20), hint.bottom}, clicking_ ? RGB(180, 40, 40) : kBannerText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, homeFont_);
    }

    void DrawRecorderEmptyIcon(HDC hdc) {
        ::DrawRecorderEmptyIcon(hdc);
    }

    void PaintRecorderHome(HDC hdc) {
        DrawTopAction(hdc, ImportRect(), L"导入", 0);
        DrawTopAction(hdc, ExportRect(), L"导出", 1);
        DrawTopAction(hdc, TimerRect(), L"定时", 2);

        if (recordings_.empty() && !recording_) {
            DrawRecorderEmptyIcon(hdc);
            SelectObject(hdc, homeFont_);
            DrawTextIn(hdc, L"没有已录制记录，按下面的提示开始录制吧", UiRect4(60, 320, 660, 350), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            int saved = SaveDC(hdc);
            RECT list = HomeListRect();
            IntersectClipRect(hdc, list.left, list.top, list.right, list.bottom);
            for (int i = 0; i < static_cast<int>(recordings_.size()); ++i) {
                RECT r = RecordingCardRect(i);
                if (r.bottom < list.top || r.top > list.bottom) continue;
                COLORREF cardColor = i == selectedRecording_ ? kDarkGreen : (i == recordingHover_ ? kCardHoverGreen : kCardGreen);
                FillRectColor(hdc, r, cardColor);
                RECT hotRc = RecordingHotkeyRect(i);
                SelectObject(hdc, homeFont_);
                DrawTextIn(hdc, recordings_[static_cast<size_t>(i)].name, HomeCardNameRect(r), kWhite, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(hdc, hotFont_);
                DrawTextIn(hdc, RecordingHotkeyText(recordings_[static_cast<size_t>(i)]), hotRc, kWhite);
                SelectObject(hdc, homeFont_);
                DrawTextIn(hdc, L"录制时间: " + recordings_[static_cast<size_t>(i)].recordTime, HomeCardMetaLeft(r), kSecondaryText);
                DrawTextIn(hdc, L"时长: " + FormatDuration(recordings_[static_cast<size_t>(i)].durationSeconds), HomeCardMetaRight(r), kSecondaryText);
                if (i == selectedRecording_) {
                    DrawTextIn(hdc, L"取消选择", RecordingDeselectRect(i), kWhite, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    RECT tag = RecordingSelectedTagRect(i);
                    FillRectColor(hdc, tag, kSelectedYellow);
                    DrawTextIn(hdc, L"已选中⌄", tag, kMainGreen, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                } else {
                    DrawTextIn(hdc, L"优化", RecordingOptimizeRect(i), kWhite, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    // 与宏「编辑/删除」同列右对齐（「重命名」三字、「删除」两字）
                    DrawTextIn(hdc, L"重命名", RecordingRenameRect(i), kWhite, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    DrawTextIn(hdc, L"删除", RecordingDeleteRect(i), kWhite, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                }
            }
            RestoreDC(hdc, saved);
            PaintHomeScrollbar(hdc);
        }

        RECT hint = CreateRect();
        FillRectColor(hdc, hint, recording_ ? RGB(255, 230, 230) : kCreateYellow);
        // 黄条右上：录制坐标模式切换（自动识别 / 相对坐标 / 绝对坐标）
        {
            const RECT modeRc = RecorderModeRect();
            FillRoundRectColor(hdc, modeRc, kOrange, UiLen(4));
            SelectObject(hdc, homeFont_);
            DrawTextIn(hdc, RecorderInputModeLabel(recorderSettings_.inputMode), modeRc,
                kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(hdc, bigFont_);
        const std::wstring prefix = L"按";
        std::wstring suffix;
        if (recording_) suffix = L"键停止 录制";
        else if (selectedRecording_ >= 0) suffix = L"键开始 回放";
        else suffix = L"键开始 录制";
        const std::wstring hotText = globalHotkey_.text.empty() ? L"F8" : globalHotkey_.text;
        const RECT keyBox = RecorderBannerKeyRect();
        const int gap = UiLen(20);
        const int prefixW = TextWidth(prefix, bigFont_);
        RECT prefixRc{keyBox.left - gap - prefixW, hint.top, keyBox.left - gap, hint.bottom};
        RECT suffixRc{keyBox.right + gap, hint.top, keyBox.right + gap + TextWidth(suffix, bigFont_), hint.bottom};
        const COLORREF bannerFg = recording_ ? RGB(180, 40, 40) : kBannerText;
        DrawTextIn(hdc, prefix, prefixRc, bannerFg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        FillRectColor(hdc, keyBox, recording_ ? RGB(220, 60, 60) : kOrange);
        DrawTextIn(hdc, hotText, keyBox, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(hdc, suffix, suffixRc, bannerFg, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, homeFont_);
        DrawTextIn(hdc, L"前往修改全局启停热键   在录制列表，您也可以为您的录制设置单独的热键", HomeFooterRect(), kFooterHint);
    }

    void PaintScriptCustomHome(HDC hdc) {
        const RECT header = ScriptCustomHeaderRect();
        SelectObject(hdc, bigFont_);
        DrawTextIn(hdc, L"AI 脚本助手", RECT{header.left, header.top, header.right, header.top + UiLen(30)},
            kWhite, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, homeFont_);
        DrawTextIn(hdc, L"通过自然语言描述需求，AI 将自动生成或修改自动化脚本。",
            RECT{header.left, header.top + UiLen(30), header.right, header.bottom},
            kFooterHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        int saved = SaveDC(hdc);
        RECT list = HomeListRect();
        IntersectClipRect(hdc, list.left, list.top, list.right, list.bottom);
        for (int i = 0; i < static_cast<int>(agentConversations_.size()); ++i) {
            RECT r = HomeCardRect(i);
            if (r.bottom < list.top || r.top > list.bottom) continue;
            const COLORREF cardColor = i == agentConvHover_ ? kCardHoverGreen : kCardGreen;
            FillRectColor(hdc, r, cardColor);
            const auto& conv = agentConversations_[static_cast<size_t>(i)];
            SelectObject(hdc, homeFont_);
            DrawTextIn(hdc, conv.name, HomeCardNameRect(r),
                kWhite, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            DrawTextIn(hdc, L"对话时间: " + conv.createdTime, HomeCardMetaLeft(r), kSecondaryText);
            DrawTextIn(hdc, L"对话轮数: " + std::to_wstring(conv.roundCount), HomeCardMetaRight(r), kSecondaryText);
            DrawTextIn(hdc, L"对话", AgentConvChatRect(i), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DrawTextIn(hdc, L"删除", AgentConvDeleteRect(i), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        RestoreDC(hdc, saved);
        if (MaxAgentConvScroll() > 0) PaintHomeScrollbar(hdc);

        RECT hint = CreateRect();
        FillRectColor(hdc, hint, kCreateYellow);
        SelectObject(hdc, bigFont_);
        DrawTextIn(hdc, L"开始 AI 对话", UiPadRect(hint, 0, 21, 0, 21),
            kBannerText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, homeFont_);
        DrawTextIn(hdc, L"点击上方按钮启动 AI 脚本助手，支持列表/读取/写入脚本",
            HomeFooterRect(), kFooterHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
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
            FillRectColor(hdc, r, cardColor);
            RECT hotRc = ScriptHotkeyRect(i);
            SelectObject(hdc, homeFont_);
            DrawTextIn(hdc, scripts_[static_cast<size_t>(i)].name, HomeCardNameRect(r), kWhite, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, hotFont_);
            DrawTextIn(hdc, ScriptHotkeyText(scripts_[static_cast<size_t>(i)]), hotRc, kWhite);
            SelectObject(hdc, homeFont_);
            DrawTextIn(hdc, L"录制时间: " + scripts_[static_cast<size_t>(i)].recordTime, HomeCardMetaLeft(r), kSecondaryText);
            DrawTextIn(hdc, L"动作数: " + std::to_wstring(scripts_[static_cast<size_t>(i)].actionCount), HomeCardMetaRight(r), kSecondaryText);
            DrawTextIn(hdc, i == selectedScript_ ? L"取消选择" : L"编辑", HomeCardEditBtn(r), kWhite, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            DrawTextIn(hdc, L"删除", HomeCardDeleteBtn(r), kWhite, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            if (i == selectedScript_) {
                RECT tag = HomeCardSelectedTag(r);
                FillRectColor(hdc, tag, kSelectedYellow);
                DrawTextIn(hdc, L"已选中⌄", tag, kMainGreen, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        RestoreDC(hdc, saved);
        PaintHomeScrollbar(hdc);
        RECT cr = CreateRect();
        FillRectColor(hdc, cr, kCreateYellow);
        SelectObject(hdc, bigFont_);
        if (selectedScript_ >= 0) {
            const std::wstring prefix = L"按";
            const std::wstring suffix = L"键开始 运行宏";
            RECT hot = CommonHotRect();
            const int gap = UiLen(20);
            const int prefixW = TextWidth(prefix, bigFont_);
            const int suffixW = TextWidth(suffix, bigFont_);
            RECT prefixRc{hot.left - gap - prefixW, cr.top, hot.left - gap, cr.bottom};
            RECT suffixRc{hot.right + gap, cr.top, hot.right + gap + suffixW, cr.bottom};
            DrawTextIn(hdc, prefix, prefixRc, kBannerText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            FillRectColor(hdc, hot, kOrange);
            DrawTextIn(hdc, globalHotkey_.text, hot, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            DrawTextIn(hdc, suffix, suffixRc, kBannerText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            DrawMacroCreatePrompt(hdc, cr);
        }
        SelectObject(hdc, homeFont_); DrawTextIn(hdc, L"前往修改全局启停热键   在宏列表中，您也可以为您的宏设置单独热键", HomeFooterRect(), kFooterHint);
    }

    void PaintHomeScrollbar(HDC hdc) {
        if (ActiveHomeListMaxScroll() <= 0) return;
        // 主界面（宏列表 / 录制列表）共用绿圆角滚轮；编辑页细灰轨另绘，不要混用
        FillRoundRectColor(hdc, HomeScrollTrackRect(), kHomeScrollTrack, UiLen(10));
        FillRoundRectColor(hdc, HomeScrollThumbRect(), kHomeScrollThumb, UiLen(10));
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

    void AppendBreakoutDebugLog(const std::wstring& text) {
        if (!macroDebugWindow_.IsCreated()) return;
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

    AiActionResult RunAiTextAnalysisForAction(
        const ScriptAction& a,
        const std::wstring& resolvedPrompt,
        AiSessionStore* sessions = nullptr,
        int loopDepth = 0) {
        AiActionResult result;
        result.errorMessage = L"AI 未启用或未配置模型";
        const std::wstring modelName = EffectiveAiModelName(a);
        if (modelName.empty() || !appSettings_.ai.enabled) return result;

        const int timeoutMs = std::max(5000, a.aiTimeoutSec > 0 ? a.aiTimeoutSec * 1000 : 30000);
        static const wchar_t* kAiTextSystemPrompt =
            L"文本分析助手。只输出用户要求的结果，不要解释或 Markdown，尽量简短。";

        ScriptAction prepAction = a;
        prepAction.aiModelName = modelName;
        AgentCore* corePtr = nullptr;
        std::unique_ptr<AgentCore> ownedCore = PrepareAiAnalysisCore(
            sessions, prepAction, loopDepth, kAiTextSystemPrompt, appSettings_, timeoutMs, 512, corePtr);
        AgentCore* core = corePtr ? corePtr : ownedCore.get();
        if (!core) {
            result.errorMessage = L"无法创建 AI 客户端";
            return result;
        }

        const size_t histBefore = core->GetHistory().size();
        AiMacroLogFn logFn = [this](const std::wstring& line) { AppendAiDebugLog(line); };
        result = ExecuteAiTextAnalysis(
            core, resolvedPrompt, a.aiOutputType, stopFlag_, a.aiTimeoutSec,
            logFn, &aiHttpAbort_, a.aiContextMode);
        if (result.ok && sessions) {
            sessions->PropagateHistoryAfterCall(
                a.aiContextMode, loopDepth, core, histBefore,
                prepAction, kAiTextSystemPrompt, appSettings_, timeoutMs, false, 512);
        }
        return result;
    }

    AiActionResult RunAiImageAnalysisForAction(
        const ScriptAction& a,
        const std::wstring& resolvedPrompt,
        const std::string& screenshotBase64,
        AiSessionStore* sessions = nullptr,
        int loopDepth = 0) {
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

        ScriptAction prepAction = a;
        prepAction.aiModelName = modelName;
        AgentCore* corePtr = nullptr;
        std::unique_ptr<AgentCore> ownedCore = PrepareAiAnalysisCore(
            sessions, prepAction, loopDepth, kAiImageSystemPrompt, appSettings_, timeoutMs, 1024, corePtr);
        AgentCore* core = corePtr ? corePtr : ownedCore.get();
        if (!core) {
            result.errorMessage = L"无法创建 AI 客户端";
            return result;
        }

        const size_t histBefore = core->GetHistory().size();
        AiMacroLogFn logFn = [this](const std::wstring& line) { AppendAiDebugLog(line); };
        result = ExecuteAiImageAnalysis(
            core, resolvedPrompt, screenshotBase64, a.aiOutputType,
            stopFlag_, a.aiTimeoutSec, logFn, &aiHttpAbort_, a.aiContextMode);
        if (result.ok && sessions) {
            sessions->PropagateHistoryAfterCall(
                a.aiContextMode, loopDepth, core, histBefore,
                prepAction, kAiImageSystemPrompt, appSettings_, timeoutMs, false, 1024);
        }
        return result;
    }

    AiActionResult RunAiActionExecuteForAction(
        const ScriptAction& a,
        const std::wstring& resolvedPrompt,
        const std::string& screenshotBase64,
        int captureWidth,
        int captureHeight,
        AiSessionStore* sessions = nullptr,
        int loopDepth = 0,
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
            sessions, prepAction, loopDepth, route, captureWidth, captureHeight,
            appSettings_, timeoutMs, corePtr);
        AgentCore* core = corePtr ? corePtr : ownedCore.get();
        if (!core) {
            result.errorMessage = L"无法创建 AI 客户端";
            return result;
        }

        const size_t histBefore = core->GetHistory().size();
        AiMacroLogFn logFn = [this](const std::wstring& line) { AppendAiDebugLog(line); };
        result = ExecuteAiActionExecute(
            core, resolvedPrompt, screenshotBase64, captureWidth, captureHeight, a.aiContextMode,
            stopFlag_, a.aiTimeoutSec, logFn, &aiHttpAbort_, captureMapping);
        if (result.ok && sessions) {
            std::wstring sysPrompt;
            if (route == AiActionRouteKind::VisionQuery || route == AiActionRouteKind::CompositeClick) {
                sysPrompt = BuildAiActionVisionQuerySystemPrompt(captureWidth, captureHeight);
            } else if (route == AiActionRouteKind::MultiTurnTools) {
                sysPrompt = BuildAiActionHybridSystemPrompt(captureWidth, captureHeight);
            } else if (withImage) {
                sysPrompt = BuildAiActionExecuteSystemPrompt(captureWidth, captureHeight);
            } else {
                sysPrompt = BuildAiActionExecuteTextSystemPrompt();
            }
            const bool useTools = route == AiActionRouteKind::ToolExecute
                || route == AiActionRouteKind::MultiTurnTools;
            sessions->PropagateHistoryAfterCall(
                a.aiContextMode, loopDepth, core, histBefore,
                prepAction, sysPrompt, appSettings_, timeoutMs, useTools, 1024);
        }
        return result;
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
        const wchar_t* text = breakoutPaused_.load(std::memory_order_relaxed) ? L"脱离中..."
            : (clicking_ ? L"连点中..." : (recording_ ? L"录制中..." : (running_ ? L"回放中..." : L"")));
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

    void Cleanup() { SaveHomeState(); outerShadow_.Detach(); FiDbgShutdown(); if (crosshairDrag_.IsActive()) crosshairDrag_.End(); CloseEditorPopup(); CloseClickerDropPopup(); CancelQuickInputTip(); KillTimer(hwnd_, kScheduledTaskTimerId); KillTimer(hwnd_, kHotkeyLatchSyncTimerId); if (editorDropPopup_) { DestroyWindow(editorDropPopup_); editorDropPopup_ = nullptr; } if (clickerDropPopup_) { DestroyWindow(clickerDropPopup_); clickerDropPopup_ = nullptr; } if (editorTipPopup_) { DestroyWindow(editorTipPopup_); editorTipPopup_ = nullptr; } macroDebugWindow_.Destroy(); if (statusTipWindow_) { DestroyWindow(statusTipWindow_); statusTipWindow_ = nullptr; } StopClickerCleanup(); StopRecordingCleanup(); ForceEndBreakoutUiState(); breakout_input::UninstallBreakoutHooks(); stopFlag_ = true; if (worker_.joinable()) worker_.detach(); ReleaseAllHeldInputs(); if (trayActive_) { NOTIFYICONDATAW nid{}; nid.cbSize = sizeof(nid); nid.hWnd = hwnd_; nid.uID = 1; Shell_NotifyIconW(NIM_DELETE, &nid); trayActive_ = false; } UnregisterHotKey(hwnd_, HOTKEY_GLOBAL_ID); for (int i = 0; i < 100; ++i) UnregisterHotKey(hwnd_, HOTKEY_SCRIPT_BASE + i); UninstallGlobalHotkeyHooks(); if (crosshairDragCursor_) { DestroyCursor(crosshairDragCursor_); crosshairDragCursor_ = nullptr; } if (findImagePreviewBitmap_) { DeleteBitmapHandle(findImagePreviewBitmap_); findImagePreviewBitmap_ = nullptr; } if (ocrFindImagePreviewBitmap_) { DeleteBitmapHandle(ocrFindImagePreviewBitmap_); ocrFindImagePreviewBitmap_ = nullptr; } if (aiFindImagePreviewBitmap_) { DeleteBitmapHandle(aiFindImagePreviewBitmap_); aiFindImagePreviewBitmap_ = nullptr; } DeleteObject(font_); DeleteObject(editorFont_); DeleteObject(bigFont_); DeleteObject(titleFont_); DeleteObject(hotFont_); DeleteObject(closeFont_); DeleteObject(homeFont_); DeleteObject(homeTabFont_); DeleteObject(whiteBrush_); DeleteObject(panelBrush_); DeleteObject(lineGreenBrush_); }
    void StopClickerCleanup();
    void StopRecordingCleanup() {
        if (recording_) {
            recording_ = false;
            UninstallRecordingHooks();
            g_recording = false;
            EndHighResTimer();
        }
    }

    HWND hwnd_ = nullptr; HMONITOR lastUiMonitor_ = nullptr; int lastUiScreenW_ = 0; int lastUiScreenH_ = 0;
    int lastUiScalePercent_ = 100; int displaySyncPass_ = 0;
    HFONT font_ = nullptr; HFONT editorFont_ = nullptr; HFONT bigFont_ = nullptr; HFONT titleFont_ = nullptr; HFONT hotFont_ = nullptr; HFONT closeFont_ = nullptr; HFONT homeFont_ = nullptr; HFONT homeTabFont_ = nullptr; HBRUSH whiteBrush_ = nullptr; HBRUSH panelBrush_ = nullptr; HBRUSH lineGreenBrush_ = nullptr;
    HWND labelMacro_ = nullptr; HWND name_ = nullptr; HWND labelBreakoutTime_ = nullptr; HWND breakoutTimeEdit_ = nullptr; HWND mode_ = nullptr; HWND labelList_ = nullptr; HWND labelBatchCount_ = nullptr; HWND actionCombo_ = nullptr; HWND addBtn_ = nullptr; HWND modifyBtn_ = nullptr; HWND clearBtn_ = nullptr; HWND loadBtn_ = nullptr;
    HWND batchExitBtn_ = nullptr; HWND batchSelectAllBtn_ = nullptr; HWND batchDeselectBtn_ = nullptr; HWND batchDeleteBtn_ = nullptr; HWND batchCopyBtn_ = nullptr;
    HWND cancelBtn_ = nullptr; HWND saveBtn_ = nullptr; HWND crosshairBtn_ = nullptr; HWND paramViewport_ = nullptr; HWND paramTopMask_ = nullptr; HWND paramBottomMask_ = nullptr; HWND paramRightMask_ = nullptr;
    HWND runProgramCombo_ = nullptr; HWND runProgramPath_ = nullptr; HWND runProgramBrowseBtn_ = nullptr; HWND runProgramOrLabel_ = nullptr; HWND runProgramCrosshairBtn_ = nullptr; HWND runProgramArgs_ = nullptr;
    HWND closeProgramPath_ = nullptr; HWND closeProgramBrowseBtn_ = nullptr; HWND closeProgramOrLabel_ = nullptr; HWND closeProgramCrosshairBtn_ = nullptr; HWND closeProgramMatchFileName_ = nullptr;
    HWND openWebpageUrl_ = nullptr; HWND openFilePath_ = nullptr; HWND openFileBrowseBtn_ = nullptr; HWND timerVarName_ = nullptr; HWND cursorPosVarName_ = nullptr; HWND gotoStepEdit_ = nullptr;
    HWND moveHintLabel_ = nullptr; HWND moveXLabel_ = nullptr; HWND moveYLabel_ = nullptr;
    HWND moveRandomXLabel_ = nullptr; HWND moveRandomYLabel_ = nullptr;
    HWND moveVarXLabel_ = nullptr; HWND moveVarYLabel_ = nullptr; HWND moveHintFooter_ = nullptr;
    HWND moveX_ = nullptr; HWND moveY_ = nullptr; HWND moveRandomX_ = nullptr; HWND moveRandomY_ = nullptr; HWND moveFromVar_ = nullptr; HWND moveVarX_ = nullptr; HWND moveVarY_ = nullptr; HWND moveRelX_ = nullptr; HWND moveRelY_ = nullptr; HWND moveRelRandomX_ = nullptr; HWND moveRelRandomY_ = nullptr; HWND waitDuration_ = nullptr; HWND waitRandom_ = nullptr; HWND clickButton_ = nullptr; HWND clickCount_ = nullptr; HWND clickWait_ = nullptr; HWND clickRandom_ = nullptr;
    HWND keyEdit_ = nullptr; HWND keyPressEdit_ = nullptr; HWND keyCount_ = nullptr; HWND keyWait_ = nullptr; HWND keyRandom_ = nullptr; HWND loopTypeCombo_ = nullptr; HWND loopCount_ = nullptr; HWND loopFromVar_ = nullptr; HWND loopVarExpr_ = nullptr; HWND loopVarName_ = nullptr; HWND defineBlockName_ = nullptr; HWND runBlockCombo_ = nullptr; HWND remarkLabel_ = nullptr; HWND remark_ = nullptr; HWND listRemarkEdit_ = nullptr;
    HWND clickLWin_ = nullptr; HWND clickRWin_ = nullptr; HWND clickLCtrl_ = nullptr; HWND clickRCtrl_ = nullptr; HWND clickLAlt_ = nullptr; HWND clickRAlt_ = nullptr; HWND clickLShift_ = nullptr; HWND clickRShift_ = nullptr;
    HWND mousePressButton_ = nullptr; HWND mousePressLWin_ = nullptr; HWND mousePressRWin_ = nullptr; HWND mousePressLCtrl_ = nullptr; HWND mousePressRCtrl_ = nullptr; HWND mousePressLAlt_ = nullptr; HWND mousePressRAlt_ = nullptr; HWND mousePressLShift_ = nullptr; HWND mousePressRShift_ = nullptr;
    HWND keyLWin_ = nullptr; HWND keyRWin_ = nullptr; HWND keyLCtrl_ = nullptr; HWND keyRCtrl_ = nullptr; HWND keyLAlt_ = nullptr; HWND keyRAlt_ = nullptr; HWND keyLShift_ = nullptr; HWND keyRShift_ = nullptr;
    HWND keyPressLWin_ = nullptr; HWND keyPressRWin_ = nullptr; HWND keyPressLCtrl_ = nullptr; HWND keyPressRCtrl_ = nullptr; HWND keyPressLAlt_ = nullptr; HWND keyPressRAlt_ = nullptr; HWND keyPressLShift_ = nullptr; HWND keyPressRShift_ = nullptr;
    HWND hotkeyShortcutCombo_ = nullptr; HWND hotkeyShortcutCount_ = nullptr; HWND hotkeyShortcutWait_ = nullptr; HWND hotkeyShortcutRandom_ = nullptr;
    HWND runMacroCombo_ = nullptr; HWND mousePlaybackCombo_ = nullptr; HWND mousePlaybackCount_ = nullptr; HWND mousePlaybackWait_ = nullptr; HWND mousePlaybackRandom_ = nullptr;
    HWND scrollVertical_ = nullptr; HWND scrollHorizontal_ = nullptr; HWND scrollSteps_ = nullptr; HWND scrollDirectionCombo_ = nullptr; HWND scrollCount_ = nullptr; HWND scrollWait_ = nullptr; HWND scrollRandom_ = nullptr;
    HWND findRegionLabel_ = nullptr; HWND findFullScreenBtn_ = nullptr; HWND findSelectRegionBtn_ = nullptr;
    HWND findImageHeaderLabel_ = nullptr;
    HWND findX1Label_ = nullptr; HWND findY1Label_ = nullptr; HWND findX2Label_ = nullptr; HWND findY2Label_ = nullptr;
    HWND findX1_ = nullptr; HWND findY1_ = nullptr; HWND findX2_ = nullptr; HWND findY2_ = nullptr;
    HWND findFollowUpLabel_ = nullptr; HWND findFollowUpCombo_ = nullptr;
    HWND findOffsetXLabel_ = nullptr; HWND findOffsetYLabel_ = nullptr; HWND findMatchVarLabel_ = nullptr;
    HWND findTestBtn_ = nullptr; HWND findImagePreviewBtn_ = nullptr; HWND findScreenshotBtn_ = nullptr; HWND findLocalImageBtn_ = nullptr; HWND findClearImageBtn_ = nullptr;
    HWND findMatchThreshold_ = nullptr; HWND findScaleMin_ = nullptr; HWND findScaleMax_ = nullptr; HWND findOffsetX_ = nullptr; HWND findOffsetY_ = nullptr; HWND findSelectOffsetBtn_ = nullptr; HWND findTimeLabel_ = nullptr; HWND findTimeEdit_ = nullptr; HWND findMatchVar_ = nullptr;
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
    std::vector<HWND> editorControls_, moveControls_, moveRelControls_, waitControls_, mousePressControls_, clickControls_, mousePlaybackControls_, runMacroControls_, keyPressControls_, keyControls_, hotkeyShortcutControls_, quickInputControls_, loopControls_, endLoopControls_, defineBlockControls_, runBlockControls_, scrollWheelControls_, findImageControls_, findImageOffsetControls_, findImageVarControls_, ocrDepControls_, ocrFindRegionToggleControls_, ocrControls_, ocrFindRegionControls_, ocrSearchControls_, ocrFollowControls_, ocrFollowOffsetControls_, ocrFollowVarControls_, ifControls_, elseControls_, lockScreenshotControls_, unlockScreenshotControls_, stopMacroControls_, runProgramControls_, runProgramFileControls_, closeProgramControls_, openWebpageControls_, openFileControls_, timerRecordControls_, getCursorPosControls_, gotoControls_, aiCommonControls_, aiTextControls_, aiImageControls_, aiActionControls_, aiFindRegionControls_;

    // ── 新布局系统: 参数面板布局结果缓存 (索引 = popupAction_.sel) ──
    std::unordered_map<int, UILayoutResult> paramLayoutResults_;
    std::vector<EditorControlLayout> editorLayouts_;
    std::vector<ParamScrollLayoutEntry> paramScrollLayout_;
    int paramContentBottom_ = 0;
    int paramControlsBottom_ = 0;
    int paramLayoutBottomHint_ = -1;
    std::vector<HotkeyMenuItem> hotkeyMenuItems_;
    std::vector<ScriptMeta> scripts_; std::vector<ScriptMeta> recordings_;
    std::vector<AgentConversationMeta> agentConversations_;
    std::vector<ScriptAction> actions_;
    std::set<int> collapsedContainers_;
    mutable std::vector<int> visibleActionIndexCache_;
    mutable bool visibleActionIndexCacheDirty_ = true;
    bool editorOpenPending_ = false;
    bool editorFullClientBlit_ = false;
    bool editorOpenCreateNew_ = false;
    int editorOpenGeneration_ = 0;
    int editorOpenPhase_ = 0;
    std::wstring editorOpenPath_;
    // 编辑器长脚本分步解析（与录制优化首屏策略一致）
    std::vector<std::wstring> editorActionBlocks_;
    std::vector<uint8_t> editorActionParsed_;
    bool editorParsePending_ = false;
    int editorParseCursor_ = 0;
    bool editorLoadCoordsNormalized_ = false;
    CoordMeta editorLoadCoordMeta_{};
    Page page_ = Page::Home; quickscript::MainTab activeHomeTab_ = quickscript::MainTab::Clicker; Hotkey globalHotkey_{0, VK_F8, L"F8", true};
    RECT homeRectBeforeEditor_{};
    int selectedScript_ = -1, selectedRecording_ = -1, currentScriptIndex_ = -1, homeHover_ = -1, recordingHover_ = -1, agentConvHover_ = -1, hoverIndex_ = -1, selectedIndex_ = -1, editingRemarkIndex_ = -1, copySource_ = -1, dragIndex_ = -1, dragTargetIndex_ = -1, dragTargetIndent_ = 0, dragStartX_ = 0, dragStartY_ = 0, scrollOffset_ = 0, homeScrollOffset_ = 0, homeScrollbarDragOffset_ = 0, editorScrollbarDragOffset_ = 0, pendingDeleteIndex_ = -1, pendingRecordingDeleteIndex_ = -1, pendingDeleteAgentConv_ = -1, paramScrollY_ = 0, paramScrollbarDragOffset_ = 0;
    HoverButton hoverButton_ = HoverButton::None;
    HWND hoverGrayBtn_ = nullptr;
    HWND pendingHoverGrayOld_ = nullptr, pendingHoverGrayNew_ = nullptr;
    bool dragging_ = false, dragMoved_ = false, dragTargetNested_ = false, homeScrollbarDragging_ = false, editorScrollbarDragging_ = false, paramScrollbarDragging_ = false, trackingMouse_ = false, hasHomeRectBeforeEditor_ = false, wasVisibleBeforeRun_ = true, wasMinimizedBeforeRun_ = false, loadingForm_ = false, batchEditMode_ = false, findImageFullScreen_ = true, ocrFullScreen_ = true, aiFullScreen_ = true;
    int clickerDropPopupKind_ = -1;
    int clickerPopupHover_ = -1;
    int clickerPopupScroll_ = 0;
    int clickerPopupVisibleCount_ = 0;
    CrosshairDragController crosshairDrag_;
    HWND hoverCrosshairBtn_ = nullptr;
    int editorPopupOpen_ = -1;
    bool ocrSubPanelRefreshPosted_ = false;
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
    std::wstring currentPath_, currentRecordTime_, formKeyText_, formKeyPressText_;
    UINT formKeyVk_ = 0, formKeyPressVk_ = 0;
    quickscript::ClickerSettings clickerSettings_{};
    quickscript::RecorderSettings recorderSettings_{};
    quickscript::AppSettings appSettings_{};
    int currentRecordingCaptureMode_ = -1;
    int currentInputTimingVersion_ = 0;
    bool trayActive_ = false;
    UINT wmTaskbarCreated_ = 0;
    bool editorFontsApplied_ = false;
    HFONT editorFontsAppliedTo_ = nullptr;
    int editorScaleAppliedPct_ = -1;
    bool hiddenToTray_ = false;
    int clickCountDone_ = 0;
    MacroDebugWindow macroDebugWindow_;
    HWND statusTipWindow_ = nullptr;
    PopupCombo popupMode_, popupWmSelectMethod_, popupAction_, popupMouseBtn_, popupClickBtn_, popupLoopType_, popupRunBlock_, popupHotkeyShortcut_, popupQuickInputVar_, popupRunMacro_, popupMousePlayback_, popupScrollDir_, popupFindFollowUp_, popupOcrResultMode_, popupOcrFollowUp_, popupOcrSearchVar_, popupIfVar_, popupIfOperator_, popupIfConnector_, popupRunProgram_, popupAiModel_, popupAiContextMode_, popupAiOutputType_, popupAiSearchRegion_;
    std::vector<QuickInputVarItem> quickInputVarItems_;
    std::vector<std::wstring> runMacroPaths_, mousePlaybackPaths_;
    std::wstring findImagePath_;
    std::wstring ocrFindImagePath_;
    std::wstring aiFindImagePath_;
    std::unordered_map<int, ScriptAction> actionFormDrafts_; // 仅内存；退出/打开编辑清空，不写脚本
    std::unordered_set<std::wstring> newImagePaths_;      // 编辑期间新增的图片路径，用于取消时清理
    std::atomic<bool> findTestRunning_{false};
    std::atomic<bool> ocrTestRunning_{false};
    PromptModal promptModal_;
    WindowOuterShadow outerShadow_;
    std::wstring promptPendingMessage_;
    bool workerUsesOcrVars_ = false;
    HBITMAP findImagePreviewBitmap_ = nullptr;
    HBITMAP ocrFindImagePreviewBitmap_ = nullptr;
    HBITMAP aiFindImagePreviewBitmap_ = nullptr;
    RECT findRegionSavedRect_{};
    std::unique_ptr<MatchOverlay> matchOverlay_;
    std::unique_ptr<OcrOverlay> ocrOverlay_;
    std::unique_ptr<ScreenshotOverlay> screenshotOverlay_;
    std::vector<std::unique_ptr<AgentDialog>> agentDialogs_;
    std::unique_ptr<SettingsDialog> settingsDialog_;
    std::unordered_map<std::wstring, ImageMatchResult> matchVars_;
    std::unordered_map<std::wstring, OcrVarResult> ocrVars_;
    std::unordered_map<std::wstring, std::wstring> aiVars_;
    std::unordered_map<std::wstring, int> loopVars_;
    std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> timerStarts_;
    int curLoops_ = 0;
    std::atomic<int> simulatingInputDepth_{0};
    std::atomic<bool> breakoutUserInput_{false};
    std::atomic<bool> breakoutPaused_{false};
    bool breakoutTaskbarShown_ = false;
    bool breakoutUiVisibleOnScreen_ = false;
    bool breakoutTaskbarTransition_ = false;
    bool trayMenuOpen_ = false;
    BreakoutTaskbarPlacement breakoutPlacement_{};
    RECT runSavedRect_{};
    LONG_PTR runSavedExStyle_ = 0;
    bool runSavedRectValid_ = false;
    double workerBreakoutTime_{0};
    BreakoutHookState breakoutHookState_{};
    DWORD lastHotkeyTick_ = 0;
    std::atomic_bool running_{false}, stopFlag_{false}; AiHttpAbortSlot aiHttpAbort_; std::thread worker_; std::mt19937 rng_{std::random_device{}()};
    windowmode::WindowModeScriptConfig scriptWindowMode_{};
    CoordMeta loadedCoordMeta_{};  // 当前加载脚本的 coordMeta，供保存时复用
    windowmode::WindowPickDialog wmPickDialog_{};
    HWND wmSelectMethod_ = nullptr;
    HWND wmSpecifyWindowBtn_ = nullptr;
    HWND wmTargetPathEdit_ = nullptr;
    HWND wmTargetBrowseBtn_ = nullptr;
    HWND wmTargetCrosshairBtn_ = nullptr;
    ScheduledTaskScheduler scheduledTasks_;
    // Recording
    std::atomic_bool recording_{false};
    bool recordingWasVisible_ = true;
    // Clicker
    std::atomic_bool clicking_{false};
    std::thread clickerThread_;
};

