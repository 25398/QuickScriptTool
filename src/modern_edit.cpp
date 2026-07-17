#include "modern_edit.h"

#include "config.h"
#include "drawing.h"

#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <string>

namespace {

constexpr UINT_PTR kModernEditSubclassId = 8801;
// refData bit0 = 真多行；bit1 = 伪单行（ES_MULTILINE 仅用于 EM_SETRECT 垂直居中）
constexpr DWORD_PTR kRefTrueMultiline = 1u;
constexpr DWORD_PTR kRefPseudoSingle = 2u;

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

bool IsPseudoSingleLine(DWORD_PTR refData) {
    return (refData & kRefPseudoSingle) != 0;
}

bool IsTrueMultiline(DWORD_PTR refData) {
    return (refData & kRefTrueMultiline) != 0;
}

void StripNewlinesInPlace(HWND hwnd) {
    if (!hwnd) return;
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return;
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    std::wstring cleaned;
    cleaned.reserve(text.size());
    bool changed = false;
    for (wchar_t ch : text) {
        if (ch == L'\r' || ch == L'\n') {
            changed = true;
            continue;
        }
        cleaned.push_back(ch);
    }
    if (!changed) return;
    DWORD start = 0;
    DWORD end = 0;
    SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    SetWindowTextW(hwnd, cleaned.c_str());
    const int caret = std::min(static_cast<int>(start), static_cast<int>(cleaned.size()));
    SendMessageW(hwnd, EM_SETSEL, caret, caret);
}

void CenterPseudoSingleLineEdit(HWND hwnd) {
    CenterModernSingleLineEditText(hwnd);
}

LRESULT CALLBACK ModernEditSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR, DWORD_PTR refData) {
    const bool trueMultiline = IsTrueMultiline(refData);
    const bool pseudoSingle = IsPseudoSingleLine(refData);

    if (ModernEditHandleShortcutMessage(hwnd, msg, wp, lp)) {
        if (trueMultiline) RefreshMultilineEditDisplay(hwnd);
        return 0;
    }
    // 伪单行 / 真单行：吞掉回车，避免换行
    if (msg == WM_CHAR && !trueMultiline && (wp == L'\r' || wp == L'\n'))
        return 0;

    switch (msg) {
    case WM_SETFONT: {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        if (pseudoSingle) CenterPseudoSingleLineEdit(hwnd);
        return r;
    }
    case WM_SIZE: {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        if (pseudoSingle) CenterPseudoSingleLineEdit(hwnd);
        return r;
    }
    case WM_WINDOWPOSCHANGED: {
        LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        const auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
        if (pseudoSingle && pos && !(pos->flags & SWP_NOSIZE))
            CenterPseudoSingleLineEdit(hwnd);
        return r;
    }
    case WM_KEYDOWN:
        if (trueMultiline && (wp == VK_BACK || wp == VK_DELETE)) {
            LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            RefreshMultilineEditDisplay(hwnd);
            return r;
        }
        break;
    case WM_CUT:
    case WM_CLEAR:
        if (trueMultiline) {
            LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            RefreshMultilineEditDisplay(hwnd);
            return r;
        }
        break;
    case WM_PASTE:
        if (trueMultiline) {
            LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            RefreshMultilineEditDisplay(hwnd);
            return r;
        }
        if (pseudoSingle) {
            LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            StripNewlinesInPlace(hwnd);
            CenterPseudoSingleLineEdit(hwnd);
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

void CenterModernSingleLineEditText(HWND edit) {
    if (!edit || !IsWindow(edit)) return;
    const LONG style = GetWindowLongW(edit, GWL_STYLE);
    // EM_SETRECT 仅对多行 EDIT 生效；密码框不能开 ES_MULTILINE
    if (!(style & ES_MULTILINE) || (style & ES_PASSWORD)) return;

    RECT rc{};
    GetClientRect(edit, &rc);
    const int clientH = rc.bottom - rc.top;
    const int clientW = rc.right - rc.left;
    if (clientH <= 0 || clientW <= 0) return;

    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
    HDC hdc = GetDC(edit);
    HFONT oldFont = font ? reinterpret_cast<HFONT>(SelectObject(hdc, font)) : nullptr;
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    // 用字形 ascent+descent，避免 tmHeight 含内部 leading 导致相对标签偏上
    SIZE sample{};
    GetTextExtentPoint32W(hdc, L"汉字Ag", 4, &sample);
    if (oldFont) SelectObject(hdc, oldFont);
    ReleaseDC(edit, hdc);

    const int textH = std::max(1, std::min(static_cast<int>(tm.tmAscent + tm.tmDescent),
        static_cast<int>(sample.cy)));
    int pad = std::max(0, (clientH - textH) / 2);
    // 相对 SS_CENTERIMAGE / DT_VCENTER 标签略下移 1px，消除「框内字偏高」
    if (clientH - textH >= 2)
        pad = std::min(pad + 1, clientH - textH);

    RECT fmt{0, pad, clientW, pad + textH};
    SendMessageW(edit, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&fmt));
}

void ApplyModernEditBehavior(HWND edit, bool multiline, int maxChars, bool /*drawBorder*/) {
    if (!edit) return;
    const LONG style = GetWindowLongW(edit, GWL_STYLE);
    const bool password = (style & ES_PASSWORD) != 0;
    const bool pseudoSingle = !multiline && !password && (style & ES_MULTILINE) != 0;

    DWORD_PTR ref = 0;
    if (multiline) ref |= kRefTrueMultiline;
    if (pseudoSingle) ref |= kRefPseudoSingle;

    SetWindowSubclass(edit, ModernEditSubclassProc, kModernEditSubclassId, ref);

    const int limit = maxChars > 0 ? maxChars
        : (multiline ? kModernEditLimitMulti : kModernEditLimitSingle);
    SendMessageW(edit, EM_SETLIMITTEXT, static_cast<WPARAM>(limit), 0);
    SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(4, 4));
    if (pseudoSingle) CenterModernSingleLineEditText(edit);
}

HWND MakeModernSingleLineEdit(HWND parent, const wchar_t* text, int id,
    int x, int y, int w, int h, DWORD extraStyle) {
    // ES_PASSWORD 不能与 ES_MULTILINE 共用；其余单行用伪多行以便 EM_SETRECT 垂直居中
    const bool password = (extraStyle & ES_PASSWORD) != 0;
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extraStyle;
    if (!password) style |= ES_MULTILINE;

    HWND edit = CreateWindowExW(0, L"EDIT", text ? text : L"",
        style, x, y, w, h, parent,
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
    CenterModernSingleLineEditText(edit);
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
