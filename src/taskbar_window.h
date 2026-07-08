#pragma once
// ──────────────────────────────────────────────────────────────────
// taskbar_window.h — 子窗口任务栏独立显示辅助
// ──────────────────────────────────────────────────────────────────

#include <windows.h>
#include "resource.h"

extern HINSTANCE g_instance;

inline HICON LoadAppIcon(int cx = 0, int cy = 0) {
    if (cx <= 0) cx = GetSystemMetrics(SM_CXICON);
    if (cy <= 0) cy = GetSystemMetrics(SM_CYICON);
    return static_cast<HICON>(LoadImageW(
        g_instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
}

inline HICON LoadAppIconSmall() {
    return LoadAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
}

inline void ApplyWindowIcons(HWND hwnd) {
    if (!hwnd) return;
    SendMessageW(hwnd, WM_SETICON, ICON_BIG,
        reinterpret_cast<LPARAM>(LoadAppIcon()));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
        reinterpret_cast<LPARAM>(LoadAppIconSmall()));
}

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
    ApplyWindowIcons(hwnd);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
