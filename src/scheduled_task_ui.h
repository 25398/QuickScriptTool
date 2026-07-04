#pragma once
// ──────────────────────────────────────────────────────────────────
// scheduled_task_ui.h — 定时任务对话框共用 UI 绘制
// ──────────────────────────────────────────────────────────────────

#include <algorithm>

#include "config.h"
#include "drawing.h"

inline void StDrawRadio(HDC hdc, const RECT& rc, bool checked) {
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const int cx = rc.left + w / 2;
    const int cy = rc.top + h / 2;
    const int inset = 1;
    HPEN pen = CreatePen(PS_SOLID, 2, checked ? kMainGreen : kComboBorderGray);
    HBRUSH brush = CreateSolidBrush(kWhite);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    Ellipse(hdc, rc.left + inset, rc.top + inset, rc.right - inset, rc.bottom - inset);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
    if (checked) {
        const int dotR = std::max(4, std::min(w, h) / 2 - 6);
        HBRUSH dot = CreateSolidBrush(kMainGreen);
        oldBrush = SelectObject(hdc, dot);
        oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, cx - dotR, cy - dotR, cx + dotR, cy + dotR);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(dot);
    }
}

inline void StDrawCheckbox(HDC hdc, const RECT& rc, bool checked) {
    DrawBorderRect(hdc, rc, kComboBorderGray);
    if (checked) {
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        HPEN pen = CreatePen(PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, 2, kMainGreen);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        MoveToEx(hdc, rc.left + w / 5, rc.top + h / 2, nullptr);
        LineTo(hdc, rc.left + 2 * w / 5, rc.bottom - h / 5);
        LineTo(hdc, rc.right - w / 6, rc.top + h / 5);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }
}

inline void StDrawPanelCombo(HDC hdc, HFONT font, const RECT& rc, const wchar_t* text, bool open) {
    FillRectColor(hdc, rc, kWhite);
    SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kText);
    RECT textRc{rc.left + 10, rc.top, rc.right - kComboArrowW - 4, rc.bottom};
    DrawTextW(hdc, text ? text : L"", -1, &textRc,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const int arrowCenterX = rc.right - kComboArrowW / 2;
    const int arrowCenterY = (rc.top + rc.bottom) / 2;
    POINT arrow[3] = {
        {arrowCenterX - 5, arrowCenterY - 3},
        {arrowCenterX + 5, arrowCenterY - 3},
        {arrowCenterX, arrowCenterY + 4}};
    HBRUSH arrowBrush = CreateSolidBrush(kMainGreen);
    HGDIOBJ oldBrush = SelectObject(hdc, arrowBrush);
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    Polygon(hdc, arrow, 3);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(arrowBrush);
    DrawBorderRect(hdc, rc, open ? kMainGreen : kComboBorderGray);
}

inline void StDrawGreenButton(HDC hdc, HFONT font, const RECT& rc, const wchar_t* text,
                              bool hover, bool enabled = true) {
    const COLORREF fill = enabled
        ? (hover ? kButtonGreenHover : kButtonGreen)
        : kButtonDisabledGreen;
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, enabled ? kWhite : kButtonDisabledText);
    HGDIOBJ oldFont = SelectObject(hdc, font);
    DrawTextW(hdc, text, -1, const_cast<RECT*>(&rc), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
}

inline void StDrawTitleBar(HDC hdc, HFONT titleFont, HFONT closeFont,
                           int dialogW, int titleH, const wchar_t* title,
                           bool hoverClose, const RECT& closeRect) {
    FillRectColor(hdc, RECT{0, 0, dialogW, titleH}, kMainGreen);
    SelectObject(hdc, titleFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kWhite);
    RECT titleRc{16, 0, dialogW - kCloseBtnW, titleH};
    DrawTextW(hdc, title, -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (hoverClose) FillRectColor(hdc, closeRect, RGB(90, 190, 125));
    SelectObject(hdc, closeFont);
    DrawTextW(hdc, L"×", -1, const_cast<RECT*>(&closeRect), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

inline bool StPtIn(const RECT& rc, int x, int y) {
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

/// 模态对话框消息泵：白名单模式，仅分发属于本对话框及其弹出层的消息
inline bool StModalMessageForDialog(const MSG& msg, HWND dialogHwnd,
    HWND popupA = nullptr, HWND popupB = nullptr) {
    if (msg.message == WM_QUIT) return true;
    if (!msg.hwnd) return true;
    if (msg.hwnd == dialogHwnd || IsChild(dialogHwnd, msg.hwnd)) return true;
    if (popupA && (msg.hwnd == popupA || IsChild(popupA, msg.hwnd))) return true;
    if (popupB && (msg.hwnd == popupB || IsChild(popupB, msg.hwnd))) return true;
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
