#include "themed_popup_menu.h"

#include "config.h"
#include "drawing.h"
#include "ui_scale.h"

#include <algorithm>
#include <windowsx.h>

namespace {

void DrawItemText(HDC hdc, HFONT font, const wchar_t* text, const RECT& rc, COLORREF color, int padX) {
    SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    RECT textRc{rc.left + padX, rc.top, rc.right - 4, rc.bottom};
    DrawTextW(hdc, text, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void ClampMenuPosition(POINT& pt, int menuW, int menuH) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (pt.x + menuW > work.right) pt.x = work.right - menuW;
    if (pt.y + menuH > work.bottom) pt.y = work.bottom - menuH;
    if (pt.x < work.left) pt.x = work.left;
    if (pt.y < work.top) pt.y = work.top;
}

bool IsMouseDownMessage(UINT msg) {
    return msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN
        || msg == WM_NCLBUTTONDOWN || msg == WM_NCRBUTTONDOWN || msg == WM_NCMBUTTONDOWN;
}

bool IsLeftButtonDown() {
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
}

}  // namespace

int ThemedPopupMenu::Show(HWND owner, POINT screenPt, const std::vector<ThemedPopupMenuItem>& items) {
    if (items.empty()) return 0;

    ThemedPopupMenu dialog{};
    dialog.owner_ = owner;
    dialog.items_ = items;
    dialog.resultId_ = 0;
    dialog.done_ = false;
    dialog.Measure();

    static bool registered = false;
    const wchar_t* cls = L"QuickScriptThemedPopupMenu";
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &ThemedPopupMenu::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
        registered = true;
    }

    ClampMenuPosition(screenPt, dialog.menuW_, dialog.menuH_);
    // 不以 owner 为父窗：模态期间 owner 若被 Disable，子菜单会一起禁用
    dialog.hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        cls, L"", WS_POPUP,
        screenPt.x, screenPt.y, dialog.menuW_, dialog.menuH_,
        nullptr, nullptr, GetModuleHandleW(nullptr), &dialog);
    if (!dialog.hwnd_) {
        if (dialog.font_) DeleteObject(dialog.font_);
        return 0;
    }

    // 「复制/添加」在 LBUTTONDOWN 打开菜单，此时左键仍按着；松开不能当成点选/取消。
    dialog.ignoreOpeningButtonUp_ = IsLeftButtonDown();

    ShowWindow(dialog.hwnd_, SW_SHOW);
    UpdateWindow(dialog.hwnd_);
    SetForegroundWindow(dialog.hwnd_);

    MSG msg{};
    while (!dialog.done_ && IsWindow(dialog.hwnd_) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            dialog.done_ = true;
            break;
        }

        // 打开瞬间的松开：无论落在菜单还是其它窗口，都只清标志，不关菜单。
        if (dialog.ignoreOpeningButtonUp_ && msg.message == WM_LBUTTONUP) {
            dialog.ignoreOpeningButtonUp_ = false;
            if (msg.hwnd == dialog.hwnd_ || IsChild(dialog.hwnd_, msg.hwnd)) {
                // 吞掉菜单上的这次 UP，避免 HitItem 失败直接 Close(0)
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }

        if (msg.hwnd == dialog.hwnd_ || IsChild(dialog.hwnd_, msg.hwnd)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        if (IsMouseDownMessage(msg.message)) {
            dialog.Close(0);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (owner && IsWindow(owner)) PostMessageW(owner, WM_NULL, 0, 0);
    return dialog.resultId_;
}

LRESULT CALLBACK ThemedPopupMenu::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ThemedPopupMenu* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<ThemedPopupMenu*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return TRUE;
    }
    self = reinterpret_cast<ThemedPopupMenu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

void ThemedPopupMenu::Measure() {
    itemH_ = UiLen(kEditorPopupItemH);
    textPadX_ = UiLen(16);
    font_ = CreateFontW(UiFontHeight(26), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");

    HDC screen = GetDC(nullptr);
    HGDIOBJ old = SelectObject(screen, font_);
    int maxTextW = 0;
    for (const auto& item : items_) {
        if (!item.text) continue;
        SIZE sz{};
        GetTextExtentPoint32W(screen, item.text, static_cast<int>(wcslen(item.text)), &sz);
        maxTextW = std::max(maxTextW, static_cast<int>(sz.cx));
    }
    SelectObject(screen, old);
    ReleaseDC(nullptr, screen);

    menuW_ = std::max(UiLen(160), maxTextW + textPadX_ * 2 + 8);
    menuH_ = static_cast<int>(items_.size()) * itemH_ + 2;
}

LRESULT ThemedPopupMenu::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            PostMessageW(hwnd_, kDeactivateCloseMsg, 0, 0);
            return 0;
        }
        return 0;
    case kDeactivateCloseMsg:
        // 打开瞬间 SetForegroundWindow 可能伴随短暂失活；左键仍按下时勿关。
        if (resultId_ == 0 && !ignoreOpeningButtonUp_ && !IsLeftButtonDown()) Close(0);
        return 0;
    case WM_MOUSEMOVE: {
        TrackMouseLeave();
        const int hit = HitItem(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (hit != hover_) {
            const int prev = hover_;
            hover_ = hit;
            if (prev >= 0) {
                RECT rc = ItemRect(prev);
                InvalidateRect(hwnd_, &rc, FALSE);
            }
            if (hover_ >= 0) {
                RECT rc = ItemRect(hover_);
                InvalidateRect(hwnd_, &rc, FALSE);
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hover_ != -1) {
            RECT rc = ItemRect(hover_);
            hover_ = -1;
            InvalidateRect(hwnd_, &rc, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        if (ignoreOpeningButtonUp_) {
            ignoreOpeningButtonUp_ = false;
            return 0;
        }
        const int hit = HitItem(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (hit >= 0 && hit < static_cast<int>(items_.size())) {
            Close(items_[static_cast<size_t>(hit)].id);
        } else {
            Close(0);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        Paint(hdc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_DESTROY:
        if (font_) {
            DeleteObject(font_);
            font_ = nullptr;
        }
        done_ = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

RECT ThemedPopupMenu::ItemRect(int index) const {
    return RECT{1, 1 + index * itemH_, menuW_ - 1, 1 + (index + 1) * itemH_};
}

int ThemedPopupMenu::HitItem(int x, int y) const {
    if (x < 1 || x >= menuW_ - 1 || y < 1 || y >= menuH_ - 1) return -1;
    const int index = (y - 1) / itemH_;
    return (index >= 0 && index < static_cast<int>(items_.size())) ? index : -1;
}

void ThemedPopupMenu::TrackMouseLeave() {
    TRACKMOUSEEVENT tme{};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = hwnd_;
    TrackMouseEvent(&tme);
}

void ThemedPopupMenu::Close(int id) {
    if (resultId_ == 0 || id != 0) resultId_ = id;
    if (IsWindow(hwnd_)) DestroyWindow(hwnd_);
    else done_ = true;
}

void ThemedPopupMenu::Paint(HDC hdc) {
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, menuW_, menuH_);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    RECT client{0, 0, menuW_, menuH_};
    FillRectColor(mem, client, kWhite);
    DrawBorderRect(mem, client, kComboPopupBorderGray);

    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const RECT row = ItemRect(i);
        if (hover_ == i) FillRectColor(mem, row, kComboMenuHoverBlue);
        DrawItemText(mem, font_, items_[static_cast<size_t>(i)].text, row, kText, textPadX_);
    }

    BitBlt(hdc, 0, 0, menuW_, menuH_, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}
