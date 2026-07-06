#include "tray_menu.h"

#include "config.h"
#include "drawing.h"

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

}  // namespace

TrayMenuAction TrayMenu::Show(HWND owner, POINT screenPt) {
    TrayMenu dialog{};
    dialog.owner_ = owner;
    dialog.result_ = TrayMenuAction::None;
    dialog.done_ = false;

    static bool registered = false;
    const wchar_t* cls = L"QuickScriptTrayMenu";
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &TrayMenu::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
        registered = true;
    }

    ClampMenuPosition(screenPt, kMenuW, kMenuH);
    dialog.hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        cls, L"", WS_POPUP,
        screenPt.x, screenPt.y, kMenuW, kMenuH,
        owner, nullptr, GetModuleHandleW(nullptr), &dialog);
    if (!dialog.hwnd_) return TrayMenuAction::None;

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
        if (msg.hwnd == dialog.hwnd_ || IsChild(dialog.hwnd_, msg.hwnd)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            continue;
        }
        if (IsMouseDownMessage(msg.message)) {
            dialog.Close(TrayMenuAction::None);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (owner && IsWindow(owner)) PostMessageW(owner, WM_NULL, 0, 0);
    return dialog.result_;
}

LRESULT CALLBACK TrayMenu::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TrayMenu* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<TrayMenu*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return TRUE;
    }
    self = reinterpret_cast<TrayMenu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayMenu::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        font_ = CreateFontW(26, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            PostMessageW(hwnd_, kDeactivateCloseMsg, 0, 0);
            return 0;
        }
        return 0;
    case kDeactivateCloseMsg:
        Close(TrayMenuAction::None);
        return 0;
    case WM_MOUSEMOVE: {
        TrackMouseLeave();
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        const int hit = HitItem(x, y);
        if (hit != hover_) {
            hover_ = hit;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (hover_ != -1) {
            hover_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        const int hit = HitItem(x, y);
        if (hit == static_cast<int>(Item::ShowWindow)) Close(TrayMenuAction::ShowWindow);
        else if (hit == static_cast<int>(Item::Exit)) Close(TrayMenuAction::Exit);
        else Close(TrayMenuAction::None);
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

RECT TrayMenu::ItemRect(int index) const {
    return RECT{1, 1 + index * kItemH, kMenuW - 1, 1 + (index + 1) * kItemH};
}

int TrayMenu::HitItem(int x, int y) const {
    if (x < 1 || x >= kMenuW - 1 || y < 1 || y >= kMenuH - 1) return -1;
    const int index = (y - 1) / kItemH;
    return (index >= 0 && index < 2) ? index : -1;
}

void TrayMenu::TrackMouseLeave() {
    TRACKMOUSEEVENT tme{};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = hwnd_;
    TrackMouseEvent(&tme);
}

void TrayMenu::Close(TrayMenuAction action) {
    if (result_ == TrayMenuAction::None) result_ = action;
    if (IsWindow(hwnd_)) DestroyWindow(hwnd_);
    else done_ = true;
}

void TrayMenu::Paint(HDC hdc) {
    RECT client{0, 0, kMenuW, kMenuH};
    FillRectColor(hdc, client, kWhite);
    DrawBorderRect(hdc, client, kComboPopupBorderGray);

    static const wchar_t* kLabels[] = {L"显示窗口", L"退出"};
    for (int i = 0; i < 2; ++i) {
        const RECT row = ItemRect(i);
        if (hover_ == i) FillRectColor(hdc, row, kComboMenuHoverBlue);
        DrawItemText(hdc, font_, kLabels[i], row, kText, kTextPadX);
    }
}
