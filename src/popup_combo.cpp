// ── 弹出式下拉组合框系统实现 ──────────────────────────────────
#include "popup_combo.h"
#include "config.h"
#include "drawing.h"
#include <commctrl.h>
#include <uxtheme.h>

// 全局状态
std::unordered_map<HWND, int> g_comboListHover;
std::unordered_set<HWND> g_comboHover;

void PaintComboChrome(HDC hdc, const RECT& rc, bool hovered, bool dropped) {
    RECT button{rc.right - kComboArrowW, rc.top, rc.right, rc.bottom};
    HBRUSH buttonBrush = CreateSolidBrush((hovered || dropped) ? kComboHoverGreen : kWhite);
    FillRect(hdc, &button, buttonBrush);
    DeleteObject(buttonBrush);

    const int cx = button.left + kComboArrowW / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    POINT tri[3] = {{cx - 4, cy - 3}, {cx + 4, cy - 3}, {cx, cy + 4}};
    HBRUSH greenBrush = CreateSolidBrush(kMainGreen);
    HGDIOBJ oldBrush = SelectObject(hdc, greenBrush);
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    Polygon(hdc, tri, 3);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(greenBrush);
}

LRESULT CALLBACK ComboListSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void StyleComboDropdownList(HWND combo) {
    COMBOBOXINFO cbi{sizeof(cbi)};
    if (!GetComboBoxInfo(combo, &cbi) || !cbi.hwndList) return;
    HWND list = cbi.hwndList;
    SetWindowSubclass(list, ComboListSubclassProc, 4, 0);
    FlatSB_EnableScrollBar(list, SB_VERT, ESB_ENABLE_BOTH);
    FlatSB_SetScrollProp(list, WSB_PROP_CXVSCROLL, kComboScrollW, FALSE);
    FlatSB_SetScrollProp(list, WSB_PROP_VSTYLE, FSB_ENCARTA_MODE, FALSE);
}

LRESULT CALLBACK ComboSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEMOVE) {
        if (g_comboHover.insert(hwnd).second) InvalidateRect(hwnd, nullptr, FALSE);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
    }
    if (msg == WM_MOUSELEAVE) {
        if (g_comboHover.erase(hwnd) > 0) InvalidateRect(hwnd, nullptr, FALSE);
    }
    if (msg == WM_DESTROY) {
        g_comboHover.erase(hwnd);
    }
    if (msg == WM_NCPAINT) {
        HDC hdc = GetWindowDC(hwnd);
        RECT wr;
        GetWindowRect(hwnd, &wr);
        OffsetRect(&wr, -wr.left, -wr.top);
        FillRectColor(hdc, wr, kWhite);
        DrawBorderRect(hdc, RECT{0, 0, wr.right - 1, wr.bottom - 1}, kComboBorderGray);
        ReleaseDC(hwnd, hdc);
        return 0;
    }
    if (msg == WM_PAINT) {
        LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        HDC hdc = GetDC(hwnd);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const bool hovered = g_comboHover.find(hwnd) != g_comboHover.end();
        const bool dropped = SendMessageW(hwnd, CB_GETDROPPEDSTATE, 0, 0) != 0;
        PaintComboChrome(hdc, rc, hovered, dropped);
        ReleaseDC(hwnd, hdc);
        return result;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

HWND MakeCombo(HWND parent, int id, int x, int y, int w, int h) {
    HWND combo = CreateWindowExW(0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
        CBS_HASSTRINGS | CBS_NOINTEGRALHEIGHT | WS_VSCROLL,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    if (combo) {
        SendMessageW(combo, CB_SETITEMHEIGHT, 0, kComboItemH);
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), kComboItemH);
        SendMessageW(combo, CB_SETMINVISIBLE, 4, 0);
        SendMessageW(combo, CB_SETDROPPEDWIDTH, std::max<int>(160, w), 0);
        SetWindowSubclass(combo, ComboSubclassProc, 2, 0);
    }
    return combo;
}

void ConfigureComboDropdown(HWND combo, int visibleRows) {
    if (!combo) return;
    SendMessageW(combo, CB_SETITEMHEIGHT, 0, kComboItemH);
    SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), kComboItemH);
    SendMessageW(combo, 0x1701 /* CB_SETMINVISIBLE */, static_cast<WPARAM>(visibleRows), 0);
    RECT rc{};
    GetWindowRect(combo, &rc);
    SendMessageW(combo, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(std::max(0L, rc.right - rc.left)), 0);
    StyleComboDropdownList(combo);
}

void MeasureComboOwnerItem(MEASUREITEMSTRUCT* mis) {
    if (!mis || mis->CtlType != ODT_COMBOBOX) return;
    mis->itemHeight = kComboItemH;
}

void DrawComboOwnerItem(DRAWITEMSTRUCT* dis, HFONT font) {
    if (!dis || dis->CtlType != ODT_COMBOBOX || dis->itemID == static_cast<UINT>(-1)) return;
    const bool inList = (dis->itemState & ODS_COMBOBOXEDIT) == 0;
    const int curSel = static_cast<int>(SendMessageW(dis->hwndItem, CB_GETCURSEL, 0, 0));
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
    SelectObject(dis->hDC, font);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, fg);
    RECT textRc{rc.left + 8, rc.top, rc.right - 8, rc.bottom};
    DrawTextW(dis->hDC, text, -1, &textRc,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
