#include "modern_edit.h"

#include "config.h"
#include "drawing.h"

#include <windowsx.h>
#include <commctrl.h>

namespace {

constexpr UINT_PTR kModernEditSubclassId = 8801;

void SelectAllEditText(HWND hwnd) {
    if (!hwnd) return;
    const int len = GetWindowTextLengthW(hwnd);
    SendMessageW(hwnd, EM_SETSEL, 0, len);
}

bool HandleEditExtraShortcut(HWND hwnd, WPARAM vk) {
    if (!(GetKeyState(VK_CONTROL) & 0x8000)) return false;
    switch (vk) {
    case 'A': case 'a':
        SelectAllEditText(hwnd);
        return true;
    case 'Z': case 'z':
        SendMessageW(hwnd, EM_UNDO, 0, 0);
        return true;
    default:
        return false;
    }
}

void RefreshMultilineEditDisplay(HWND hwnd) {
    if (!hwnd) return;
    InvalidateRect(hwnd, nullptr, TRUE);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
}

LRESULT CALLBACK ModernEditSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR, DWORD_PTR refData) {
    const bool multiline = (refData & 1) != 0;
    if (ModernEditHandleShortcutMessage(hwnd, msg, wp, lp)) {
        if (multiline) RefreshMultilineEditDisplay(hwnd);
        return 0;
    }
    if (msg == WM_CHAR && !multiline && wp == L'\r')
        return 0;
    switch (msg) {
    case WM_KEYDOWN:
        if (multiline && (wp == VK_BACK || wp == VK_DELETE)) {
            LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            RefreshMultilineEditDisplay(hwnd);
            return r;
        }
        break;
    case WM_CUT:
    case WM_CLEAR:
    case WM_PASTE:
        if (multiline) {
            LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            RefreshMultilineEditDisplay(hwnd);
            return r;
        }
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

bool ModernEditHandleShortcutMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!hwnd) return false;
    switch (msg) {
    case WM_KEYDOWN:
        return HandleEditExtraShortcut(hwnd, wp);
    case WM_CHAR:
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            if (wp == 1) {  // Ctrl+A
                SelectAllEditText(hwnd);
                return true;
            }
            if (wp == 26) {  // Ctrl+Z
                SendMessageW(hwnd, EM_UNDO, 0, 0);
                return true;
            }
        }
        break;
    default:
        break;
    }
    (void)lp;
    return false;
}

void ModernEditSelectAll(HWND edit) {
    SelectAllEditText(edit);
}

void ApplyModernEditBehavior(HWND edit, bool multiline, int maxChars, bool /*drawBorder*/) {
    if (!edit) return;
    const DWORD_PTR ref = multiline ? 1u : 0u;
    SetWindowSubclass(edit, ModernEditSubclassProc, kModernEditSubclassId, ref);

    const int limit = maxChars > 0 ? maxChars
        : (multiline ? kModernEditLimitMulti : kModernEditLimitSingle);
    SendMessageW(edit, EM_SETLIMITTEXT, static_cast<WPARAM>(limit), 0);
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(4, 4));
}

HWND MakeModernSingleLineEdit(HWND parent, const wchar_t* text, int id,
    int x, int y, int w, int h, DWORD extraStyle) {
    HWND edit = CreateWindowExW(0, L"EDIT", text ? text : L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extraStyle,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    if (edit) ApplyModernEditBehavior(edit, false);
    return edit;
}

HWND MakeModernMultiLineEdit(HWND parent, const wchar_t* text, int id,
    int x, int y, int w, int h, bool wantReturn, DWORD extraStyle) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | extraStyle;
    if (wantReturn) style |= ES_WANTRETURN;
    HWND edit = CreateWindowExW(0, L"EDIT", text ? text : L"",
        style, x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    if (edit) ApplyModernEditBehavior(edit, true);
    return edit;
}

void DrawModernEditBorder(HDC hdc, const RECT& rc) {
    DrawBorderRect(hdc, rc, kComboBorderGray);
}

void DrawEditControlBorder(HDC hdc, HWND dialog, HWND edit, COLORREF color) {
    if (!hdc || !dialog || !edit || !IsWindow(edit) || !IsWindowVisible(edit)) return;
    RECT rc{};
    GetWindowRect(edit, &rc);
    MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&rc), 2);
    DrawBorderRect(hdc, rc, color);
}

void PositionEditInBorderFrame(HWND edit, int outerX, int outerY, int outerW, int outerH) {
    if (!edit) return;
    SetWindowPos(edit, nullptr, outerX + 1, outerY + 1, outerW - 2, outerH - 2,
        SWP_NOZORDER | SWP_NOACTIVATE);
}

void DrawEditOuterBorder(HDC hdc, HWND dialog, HWND edit, COLORREF color) {
    if (!hdc || !dialog || !edit || !IsWindow(edit) || !IsWindowVisible(edit)) return;
    RECT rc{};
    GetWindowRect(edit, &rc);
    MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&rc), 2);
    rc.left -= 1;
    rc.top -= 1;
    rc.right += 1;
    rc.bottom += 1;
    DrawBorderRect(hdc, rc, color);
}
