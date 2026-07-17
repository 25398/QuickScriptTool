#pragma once
// ──────────────────────────────────────────────────────────────────
// scheduled_task_ui.h — 定时任务对话框共用 UI 绘制
// ──────────────────────────────────────────────────────────────────

#include <algorithm>

#include "config.h"
#include "drawing.h"

inline void StDrawRadio(HDC hdc, const RECT& rc, bool checked) {
    DrawRadioButton(hdc, rc, checked);
}

inline void StDrawCheckbox(HDC hdc, const RECT& rc, bool checked) {
    DrawCheckbox(hdc, rc, checked);
}

inline void StDrawPanelCombo(HDC hdc, HFONT font, const RECT& rc, const wchar_t* text, bool open) {
    FillRectColor(hdc, rc, kWhite);
    SelectObject(hdc, font);
    RECT textRc{rc.left + 10, rc.top, rc.right - kComboArrowW - 4, rc.bottom};
    DrawTextIn(hdc, text ? text : L"", textRc, kText,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const int arrowCenterX = rc.right - kComboArrowW / 2;
    const int arrowCenterY = (rc.top + rc.bottom) / 2;
    DrawComboDownArrow(hdc, arrowCenterX, arrowCenterY, kMainGreen);
    DrawBorderRect(hdc, rc, open ? kMainGreen : kComboBorderGray);
}

inline void StDrawGreenButton(HDC hdc, HFONT font, const RECT& rc, const wchar_t* text,
                              bool hover, bool enabled = true) {
    const COLORREF fill = enabled
        ? (hover ? kButtonGreenHover : kButtonGreen)
        : kButtonDisabledGreen;
    FillRectColor(hdc, rc, kWhite);
    FillRoundRectColor(hdc, rc, fill, 6);
    SelectObject(hdc, font);
    DrawTextIn(hdc, text, rc, enabled ? kWhite : kButtonDisabledText,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

inline void StDrawTitleBar(HDC hdc, HFONT titleFont, HFONT closeFont,
                           int dialogW, int titleH, const wchar_t* title,
                           bool hoverClose, const RECT& closeRect) {
    FillRectColor(hdc, RECT{0, 0, dialogW, titleH}, kMainGreen);
    SelectObject(hdc, titleFont);
    DrawTextIn(hdc, title, RECT{16, 0, dialogW - kCloseBtnW, titleH}, kWhite,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (hoverClose) FillRectColor(hdc, closeRect, kCloseHover);
    SelectObject(hdc, closeFont);
    DrawTextIn(hdc, L"×", closeRect, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

inline bool StPtIn(const RECT& rc, int x, int y) {
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

inline bool StIsImeUiWindow(HWND hwnd) {
    if (!hwnd) return false;
    wchar_t cls[64]{};
    if (!GetClassNameW(hwnd, cls, 64)) return false;
    if (wcsncmp(cls, L"IME", 3) == 0) return true;
    if (wcscmp(cls, L"MSCTFIME UI") == 0) return true;
    if (wcsstr(cls, L"CANDIDATE") != nullptr) return true;
    return false;
}

/// 模态对话框消息泵：白名单模式，仅分发属于本对话框及其弹出层的消息
inline bool StModalMessageForDialog(const MSG& msg, HWND dialogHwnd,
    HWND popupA = nullptr, HWND popupB = nullptr, HWND disabledOwner = nullptr) {
    if (msg.message == WM_QUIT) return true;
    if (!msg.hwnd) return true;
    if (disabledOwner && (msg.hwnd == disabledOwner || IsChild(disabledOwner, msg.hwnd)))
        return false;
    if (msg.hwnd == dialogHwnd || IsChild(dialogHwnd, msg.hwnd)) return true;
    if (popupA && (msg.hwnd == popupA || IsChild(popupA, msg.hwnd))) return true;
    if (popupB && (msg.hwnd == popupB || IsChild(popupB, msg.hwnd))) return true;
    // IME 候选/组字窗不是 dialog 子窗口，须放行（参考 wxWidgets/Chromium 模态泵做法）
    if (msg.message >= WM_IME_SETCONTEXT && msg.message <= WM_IME_KEYUP) return true;
    if (StIsImeUiWindow(msg.hwnd)) return true;
    if (GetWindowThreadProcessId(msg.hwnd, nullptr) == GetCurrentThreadId()) return true;
    return false;
}

/// 子模态关闭后丢弃可能误投到 owner 的鼠标/关闭消息（须在 EnableWindow(owner) 之前调用）
inline void StDiscardSpuriousInputAfterModal(HWND ownerHwnd) {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE)) {}
    while (PeekMessageW(&msg, nullptr, WM_NCMOUSEMOVE, WM_NCMBUTTONDBLCLK, PM_REMOVE)) {}
    if (!ownerHwnd) return;
    while (PeekMessageW(&msg, ownerHwnd, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE)) {}
    while (PeekMessageW(&msg, ownerHwnd, WM_NCLBUTTONDOWN, WM_NCMBUTTONDBLCLK, PM_REMOVE)) {}
    while (PeekMessageW(&msg, ownerHwnd, WM_CLOSE, WM_CLOSE, PM_REMOVE)) {}
    while (PeekMessageW(&msg, ownerHwnd, WM_SYSCOMMAND, WM_SYSCOMMAND, PM_REMOVE)) {}
}
