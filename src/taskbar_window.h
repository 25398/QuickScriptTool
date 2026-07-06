#pragma once
// ──────────────────────────────────────────────────────────────────
// taskbar_window.h — 子窗口任务栏独立显示辅助
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

/// 使窗口在任务栏中独立显示（支持最小化后从任务栏恢复）。
/// @param detachOwner 为 true 时解除 owner 关联，窗口与主界面完全独立。
inline void ApplyTaskbarWindowStyle(HWND hwnd, const wchar_t* taskbarTitle,
                                    bool detachOwner = false) {
    if (!hwnd) return;
    if (detachOwner) {
        SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, 0);
    }
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex &= ~WS_EX_TOOLWINDOW;
    ex |= WS_EX_APPWINDOW;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    if (taskbarTitle && taskbarTitle[0]) {
        SetWindowTextW(hwnd, taskbarTitle);
    }
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
