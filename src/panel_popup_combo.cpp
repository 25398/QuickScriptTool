#include "panel_popup_combo.h"

#include "config.h"
#include "drawing.h"
#include "scheduled_task_ui.h"

#include <windowsx.h>

#include <algorithm>

namespace {

constexpr wchar_t kPopupClass[] = L"QSPanelPopupCombo";
constexpr int kPopupItemH = kComboItemH;
constexpr int kPopupMaxVisible = 6;

bool RegisterPopupClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSW wc{};
    wc.lpfnWndProc = PanelPopupCombo::PopupWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kPopupClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    registered = RegisterClassW(&wc) != 0;
    return registered;
}

}  // namespace

void PanelPopupCombo::Init(HWND owner, HFONT font) {
    owner_ = owner;
    font_ = font;
    if (!popup_ && owner_ && RegisterPopupClass()) {
        popup_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            kPopupClass, L"", WS_POPUP,
            0, 0, 100, 100, owner_, nullptr, GetModuleHandleW(nullptr), this);
        ShowWindow(popup_, SW_HIDE);
    }
}

void PanelPopupCombo::Destroy() {
    if (popup_) {
        DestroyWindow(popup_);
        popup_ = nullptr;
    }
    owner_ = nullptr;
    font_ = nullptr;
    open_ = false;
}

void PanelPopupCombo::SetItems(std::vector<std::wstring> items) {
    items_ = std::move(items);
    if (sel_ >= static_cast<int>(items_.size())) sel_ = items_.empty() ? -1 : 0;
    scroll_ = 0;
    hover_ = -1;
}

void PanelPopupCombo::SetSelectedIndex(int index) {
    if (items_.empty() || index < 0) {
        sel_ = -1;
        return;
    }
    sel_ = std::clamp(index, 0, static_cast<int>(items_.size()) - 1);
}

std::wstring PanelPopupCombo::DisplayText() const {
    if (sel_ >= 0 && sel_ < static_cast<int>(items_.size())) return items_[static_cast<size_t>(sel_)];
    return placeholder_;
}

void PanelPopupCombo::Toggle(const RECT& anchorClientRect) {
    anchor_ = anchorClientRect;
    if (IsPopupVisible()) {
        Close();
        return;
    }
    if (items_.empty()) return;
    open_ = true;
    hover_ = sel_;
    scroll_ = 0;
    SyncPopupPosition(anchorClientRect);
    if (owner_) InvalidateRect(owner_, &anchorClientRect, FALSE);
}

void PanelPopupCombo::Close() {
    if (!open_) return;
    open_ = false;
    hover_ = -1;
    if (popup_) ShowWindow(popup_, SW_HIDE);
    if (owner_) InvalidateRect(owner_, &anchor_, FALSE);
}

void PanelPopupCombo::DrawField(HDC hdc, const RECT& rc, bool hover) const {
    const bool dropped = open_;
    const COLORREF borderColor = dropped ? kMainGreen : (hover ? kMainGreen : kComboBorderGray);
    FillRectColor(hdc, rc, kWhite);
    const int arrowW = 26;
    const std::wstring text = DisplayText();
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, (sel_ >= 0 && !text.empty() && text != placeholder_) ? kText : kHint);
    RECT textRc{rc.left + 10, rc.top, rc.right - arrowW, rc.bottom};
    DrawTextW(hdc, text.c_str(), -1, &textRc,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const int arrowCenterX = rc.right - arrowW / 2;
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
    DrawBorderRect(hdc, rc, borderColor);
}

bool PanelPopupCombo::HitField(const RECT& rc, int x, int y) const {
    return StPtIn(rc, x, y);
}

bool PanelPopupCombo::IsPopupVisible() const {
    return popup_ && IsWindowVisible(popup_) == TRUE;
}

bool PanelPopupCombo::HitPopupScreen(int screenX, int screenY) const {
    if (!IsPopupVisible()) return false;
    RECT rc{};
    GetWindowRect(popup_, &rc);
    POINT pt{screenX, screenY};
    return PtInRect(&rc, pt) != FALSE;
}

void PanelPopupCombo::SyncPopupPosition(const RECT& anchorClientRect) {
    if (!popup_ || !owner_ || !open_) return;
    anchor_ = anchorClientRect;
    const int visible = std::min(kPopupMaxVisible, static_cast<int>(items_.size()));
    const int h = visible * kPopupItemH + 2;
    const int w = anchorClientRect.right - anchorClientRect.left;
    POINT screenTop{anchorClientRect.left, anchorClientRect.bottom};
    ClientToScreen(owner_, &screenTop);
    SetWindowPos(popup_, HWND_TOPMOST, screenTop.x, screenTop.y, w, h,
        SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
}

LRESULT CALLBACK PanelPopupCombo::PopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<PanelPopupCombo*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<PanelPopupCombo*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    return self ? self->HandlePopupMessage(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT PanelPopupCombo::HandlePopupMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(popup_, &ps);
        PaintPopup(hdc);
        EndPaint(popup_, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, popup_, 0};
        TrackMouseEvent(&tme);
        RECT client{};
        GetClientRect(popup_, &client);
        const int idx = HitItemIndex(GET_Y_LPARAM(lp), client.bottom - client.top);
        if (idx != hover_) {
            hover_ = idx;
            InvalidateRect(popup_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hover_ != -1) {
            hover_ = -1;
            InvalidateRect(popup_, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        RECT client{};
        GetClientRect(popup_, &client);
        const int idx = HitItemIndex(GET_Y_LPARAM(lp), client.bottom - client.top);
        Close();
        if (idx >= 0) SelectIndex(idx);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        RECT client{};
        GetClientRect(popup_, &client);
        const int scrollMax = std::max(0, static_cast<int>(items_.size()) - VisibleCount(client.bottom - client.top));
        scroll_ = std::clamp(scroll_ - delta, 0, scrollMax);
        InvalidateRect(popup_, nullptr, FALSE);
        return 0;
    }
    default:
        return DefWindowProcW(popup_, msg, wp, lp);
    }
}

void PanelPopupCombo::PaintPopup(HDC hdc) {
    RECT client{};
    GetClientRect(popup_, &client);
    FillRectColor(hdc, client, kWhite);
    DrawBorderRect(hdc, client, kComboPopupBorderGray);
    if (font_) SelectObject(hdc, font_);
    SetBkMode(hdc, TRANSPARENT);
    const int visible = VisibleCount(client.bottom - client.top);
    const int scrollMax = std::max(0, static_cast<int>(items_.size()) - visible);
    scroll_ = std::clamp(scroll_, 0, scrollMax);
    for (int vis = 0; vis < visible; ++vis) {
        const int i = vis + scroll_;
        if (i >= static_cast<int>(items_.size())) break;
        RECT row{client.left + 1, client.top + 1 + vis * kPopupItemH,
            client.right - 1, client.top + 1 + (vis + 1) * kPopupItemH};
        const bool selected = i == sel_;
        const bool hovered = hover_ == i;
        FillRectColor(hdc, row, selected ? kComboMenuSelectBlue : (hovered ? kComboMenuHoverBlue : kWhite));
        SetTextColor(hdc, selected ? kComboMenuSelectText : kText);
        RECT textRc{row.left + 10, row.top, row.right - 6, row.bottom};
        DrawTextW(hdc, items_[static_cast<size_t>(i)].c_str(), -1, &textRc,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

int PanelPopupCombo::VisibleCount(int popupHeight) const {
    return std::max(1, (popupHeight - 2) / kPopupItemH);
}

int PanelPopupCombo::HitItemIndex(int localY, int popupHeight) const {
    const int rel = localY - 1;
    if (rel < 0) return -1;
    const int vis = rel / kPopupItemH;
    const int visible = VisibleCount(popupHeight);
    if (vis >= visible) return -1;
    const int idx = vis + scroll_;
    return idx >= 0 && idx < static_cast<int>(items_.size()) ? idx : -1;
}

void PanelPopupCombo::SelectIndex(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return;
    sel_ = index;
    if (onSelect_) onSelect_(sel_);
    if (owner_) InvalidateRect(owner_, &anchor_, FALSE);
}
