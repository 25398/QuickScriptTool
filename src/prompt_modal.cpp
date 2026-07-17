#include "prompt_modal.h"

#include "config.h"
#include "drawing.h"
#include "ui_scale.h"

#include <windowsx.h>

#include <algorithm>

namespace {

constexpr BYTE kPromptOverlayAlpha = 145;

void FillAlphaRectLocal(HDC hdc, const RECT& rc, COLORREF color, BYTE alpha) {
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    RECT local{0, 0, w, h};
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(mem, &local, brush);
    DeleteObject(brush);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, 0};
    AlphaBlend(hdc, rc.left, rc.top, w, h, mem, 0, 0, w, h, blend);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void DrawPromptText(HDC hdc, HFONT font, const std::wstring& text, const RECT& rc,
                    COLORREF color, UINT format) {
    if (font) SelectObject(hdc, font);
    DrawTextIn(hdc, text, rc, color, format);
}

bool PtInRectLocal(const RECT& rc, int x, int y) {
    return x >= rc.left && x <= rc.right && y >= rc.top && y <= rc.bottom;
}

const wchar_t kPromptShieldClass[] = L"QuickScriptPromptShield";

void EnsurePromptShieldClassRegistered() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kPromptShieldClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    registered = true;
}

int MeasureMessageHeight(HDC hdc, HFONT font, const std::wstring& message, int maxTextW) {
    if (message.empty() || maxTextW <= 0) return UiLen(40);
    HGDIOBJ old = nullptr;
    if (font) old = SelectObject(hdc, font);
    RECT calc{0, 0, maxTextW, 0};
    DrawTextW(hdc, message.c_str(), -1, &calc,
        DT_CALCRECT | DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    if (old) SelectObject(hdc, old);
    return std::max(UiLen(40), static_cast<int>(calc.bottom - calc.top));
}

}  // namespace

PromptModalLayout ComputePromptModalLayout(const RECT& client, PromptModalMode mode,
                                           const std::wstring& message, HFONT font) {
    PromptModalLayout layout{};
    const int padX = UiLen(28);
    const int padTop = UiLen(24);
    const int gapMsgBtn = UiLen(18);
    const int btnH = UiLen(70);
    const int padBottom = UiLen(28);
    const int minDialogW = UiLen(450);
    const int clientW = static_cast<int>(client.right - client.left);
    const int clientH = static_cast<int>(client.bottom - client.top);
    const int maxDialogW = std::max(minDialogW, clientW - UiLen(80));
    const int dialogW = std::min(maxDialogW, std::max(minDialogW, UiLen(520)));
    const int textW = dialogW - padX * 2;

    int msgH = UiLen(48);
    HDC screenDc = GetDC(nullptr);
    if (screenDc) {
        msgH = MeasureMessageHeight(screenDc, font, message, textW);
        ReleaseDC(nullptr, screenDc);
    } else if (!message.empty()) {
        int lines = 1;
        for (wchar_t ch : message) if (ch == L'\n') ++lines;
        msgH = std::max(msgH, lines * UiLen(26));
    }

    const int dialogH = padTop + msgH + gapMsgBtn + btnH + padBottom;
    const int maxH = std::max(UiLen(200), clientH - UiLen(40));
    const int h = std::min(dialogH, maxH);
    const int w = dialogW;
    const int x = client.left + ((client.right - client.left) - w) / 2;
    const int y = client.top + ((client.bottom - client.top) - h) / 2;
    layout.dialog = RECT{x, y, x + w, y + h};

    const int btnBottom = layout.dialog.bottom - padBottom;
    const int btnTop = btnBottom - btnH;
    layout.message = RECT{
        layout.dialog.left + padX,
        layout.dialog.top + padTop,
        layout.dialog.right - padX,
        btnTop - gapMsgBtn
    };

    if (mode == PromptModalMode::Confirm) {
        layout.hasCancel = true;
        const int gap = UiLen(16);
        const int btnW = (layout.message.right - layout.message.left - gap) / 2;
        layout.ok = RECT{layout.message.left, btnTop, layout.message.left + btnW, btnBottom};
        layout.cancel = RECT{layout.ok.right + gap, btnTop, layout.message.right, btnBottom};
    } else {
        const int btnW = UiLen(184);
        const int cx = (layout.dialog.left + layout.dialog.right) / 2;
        layout.ok = RECT{cx - btnW / 2, btnTop, cx + btnW / 2, btnBottom};
    }
    return layout;
}

void PaintPromptModal(HDC hdc, const RECT& client, const std::wstring& message,
    PromptModalMode mode, bool hoverOk, bool hoverCancel, HFONT textFont) {
    // Caller should provide a clean buffer; we always fully redraw to avoid
    // partial AlphaBlend over stale pixels (hover flicker).
    FillAlphaRectLocal(hdc, client, RGB(0, 0, 0), kPromptOverlayAlpha);
    const PromptModalLayout layout =
        ComputePromptModalLayout(client, mode, message, textFont);
    FillRectColor(hdc, layout.dialog, kPromptDialogBg);
    DrawBorderRoundRect(hdc, layout.dialog, kPromptDialogBorder, 6);
    DrawPromptText(hdc, textFont, message, layout.message, kPromptText,
        DT_CENTER | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

    FillRectColor(hdc, layout.ok, hoverOk ? kPromptOkHover : kPromptOkFill);
    DrawPromptText(hdc, textFont, L"确定", layout.ok, kPromptOkText,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (layout.hasCancel) {
        // Always opaque-fill cancel so hover toggles don't punch through overlay.
        FillRectColor(hdc, layout.cancel, hoverCancel ? kComboHoverGreen : kPromptDialogBg);
        DrawBorderRect(hdc, layout.cancel, kPromptCancelBorder);
        DrawPromptText(hdc, textFont, L"取消", layout.cancel, kPromptText,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

PromptModalButton PromptModalHitTest(int x, int y, const RECT& client, PromptModalMode mode,
                                     const std::wstring& message, HFONT font) {
    const PromptModalLayout layout = ComputePromptModalLayout(client, mode, message, font);
    if (PtInRectLocal(layout.ok, x, y)) return PromptModalButton::Ok;
    if (layout.hasCancel && PtInRectLocal(layout.cancel, x, y)) return PromptModalButton::Cancel;
    return PromptModalButton::None;
}

void PromptModal::ReleaseMessageFont() {
    if (messageFont_) {
        DeleteObject(messageFont_);
        messageFont_ = nullptr;
    }
}

void PromptModal::EnsureMessageFont() {
    ReleaseMessageFont();
    if (!font_) return;
    LOGFONTW lf{};
    GetObjectW(font_, sizeof(lf), &lf);
    const int px = abs(lf.lfHeight);
    lf.lfHeight = lf.lfHeight < 0 ? -(px + 2) : (px + 2);
    messageFont_ = CreateFontIndirectW(&lf);
}

void PromptModal::Bind(HWND owner, HFONT font, std::function<void()> afterClose) {
    owner_ = owner;
    font_ = font;
    EnsureMessageFont();
    afterClose_ = std::move(afterClose);
    EnsureShield();
}

void PromptModal::OnOwnerResize() {
    if (!visible_) return;
    SyncShield();
}

RECT PromptModal::ClientRect() const {
    RECT rc{};
    if (owner_) GetClientRect(owner_, &rc);
    return rc;
}

RECT PromptModal::OwnerScreenRect() const {
    RECT rc = ClientRect();
    POINT tl{rc.left, rc.top};
    if (owner_) ClientToScreen(owner_, &tl);
    return RECT{tl.x, tl.y, tl.x + rc.right, tl.y + rc.bottom};
}

void PromptModal::EnsureShield() {
    if (shield_ || !owner_) return;
    EnsurePromptShieldClassRegistered();
    shield_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kPromptShieldClass, L"",
        WS_POPUP,
        0, 0, 0, 0,
        owner_, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowSubclass(shield_, &PromptModal::ShieldProc, 1, reinterpret_cast<DWORD_PTR>(this));
    ShowWindow(shield_, SW_HIDE);
}

void PromptModal::SyncShield() {
    EnsureShield();
    if (!shield_ || !owner_) return;
    if (!visible_) {
        ShowWindow(shield_, SW_HIDE);
        return;
    }
    const RECT sr = OwnerScreenRect();
    const int w = sr.right - sr.left;
    const int h = sr.bottom - sr.top;
    SetWindowPos(shield_, HWND_TOP, sr.left, sr.top, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    InvalidateRect(shield_, nullptr, FALSE);
    UpdateWindow(shield_);
}

void PromptModal::InvalidateButtonRegion() {
    if (!shield_) return;
    // Full client redraw into an offscreen buffer — partial invalidate + AlphaBlend
    // stacks darken over previous frame and flickers the hover color.
    InvalidateRect(shield_, nullptr, FALSE);
}

bool PromptModal::UpdateHover(int x, int y, const RECT& client) {
    HFONT font = messageFont_ ? messageFont_ : font_;
    const PromptModalLayout layout =
        ComputePromptModalLayout(client, mode_, message_, font);
    const bool hoverOk = PtInRectLocal(layout.ok, x, y);
    const bool hoverCancel = layout.hasCancel && PtInRectLocal(layout.cancel, x, y);
    if (hoverOk == hoverOk_ && hoverCancel == hoverCancel_) return false;
    hoverOk_ = hoverOk;
    hoverCancel_ = hoverCancel;
    return true;
}

void PromptModal::TrackMouseLeave() {
    if (!shield_ || trackingLeave_) return;
    TRACKMOUSEEVENT tme{};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = shield_;
    if (TrackMouseEvent(&tme)) trackingLeave_ = true;
}

void PromptModal::RefreshOwnerAfterClose() {
    if (!owner_) return;
    RedrawWindow(owner_, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

void PromptModal::ShowInfo(const std::wstring& message) {
    message_ = message;
    mode_ = PromptModalMode::Info;
    hoverOk_ = false;
    hoverCancel_ = false;
    cursorOnButton_ = false;
    trackingLeave_ = false;
    armedButton_ = PromptModalButton::None;
    // Owner usually opens us on WM_LBUTTONDOWN; the matching UP must not hit OK.
    suppressClickUntilRelease_ = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    onDone_ = nullptr;
    visible_ = true;
    SyncShield();
    if (shield_) {
        SetForegroundWindow(shield_);
        SetWindowPos(shield_, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
}

void PromptModal::ShowConfirm(const std::wstring& message, std::function<void(bool accepted)> onDone) {
    message_ = message;
    mode_ = PromptModalMode::Confirm;
    hoverOk_ = false;
    hoverCancel_ = false;
    cursorOnButton_ = false;
    trackingLeave_ = false;
    armedButton_ = PromptModalButton::None;
    suppressClickUntilRelease_ = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    onDone_ = std::move(onDone);
    visible_ = true;
    SyncShield();
    if (shield_) {
        SetForegroundWindow(shield_);
        SetWindowPos(shield_, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
}

void PromptModal::Close(PromptModalButton button) {
    if (!visible_) return;
    auto done = std::move(onDone_);
    visible_ = false;
    hoverOk_ = false;
    hoverCancel_ = false;
    cursorOnButton_ = false;
    trackingLeave_ = false;
    suppressClickUntilRelease_ = false;
    armedButton_ = PromptModalButton::None;
    onDone_ = nullptr;
    SyncShield();
    RefreshOwnerAfterClose();
    if (afterClose_) afterClose_();
    if (done) done(button == PromptModalButton::Ok);
}

LRESULT CALLBACK PromptModal::ShieldProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR, DWORD_PTR refData) {
    auto* self = reinterpret_cast<PromptModal*>(refData);
    if (!self) return DefSubclassProc(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w > 0 && h > 0) {
            // Snapshot owner → dim → dialog in an offscreen DC, blit once.
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
            HGDIOBJ oldBmp = SelectObject(mem, bmp);

            if (self->owner_) {
                HDC ownerDc = GetDC(self->owner_);
                if (ownerDc) {
                    BitBlt(mem, 0, 0, w, h, ownerDc, 0, 0, SRCCOPY);
                    ReleaseDC(self->owner_, ownerDc);
                } else {
                    FillRectColor(mem, rc, RGB(32, 32, 32));
                }
            } else {
                FillRectColor(mem, rc, RGB(32, 32, 32));
            }

            HFONT font = self->messageFont_ ? self->messageFont_ : self->font_;
            PaintPromptModal(mem, rc, self->message_, self->mode_, self->hoverOk_,
                self->hoverCancel_, font);
            BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        self->TrackMouseLeave();
        if (self->UpdateHover(x, y, rc)) self->InvalidateButtonRegion();
        HFONT font = self->messageFont_ ? self->messageFont_ : self->font_;
        const auto hit = PromptModalHitTest(x, y, rc, self->mode_, self->message_, font);
        const bool onBtn = hit == PromptModalButton::Ok || hit == PromptModalButton::Cancel;
        if (onBtn != self->cursorOnButton_) {
            self->cursorOnButton_ = onBtn;
            SetCursor(LoadCursorW(nullptr, onBtn ? IDC_HAND : IDC_ARROW));
        }
        return 0;
    }
    case WM_MOUSELEAVE: {
        self->trackingLeave_ = false;
        if (self->hoverOk_ || self->hoverCancel_) {
            self->hoverOk_ = false;
            self->hoverCancel_ = false;
            self->InvalidateButtonRegion();
        }
        if (self->cursorOnButton_) {
            self->cursorOnButton_ = false;
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        }
        return 0;
    }
    case WM_SETCURSOR:
        // We manage the cursor in WM_MOUSEMOVE — ignore default class cursor thrashing.
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, self->cursorOnButton_ ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        if (self->suppressClickUntilRelease_) return 0;
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HFONT font = self->messageFont_ ? self->messageFont_ : self->font_;
        const PromptModalButton hit =
            PromptModalHitTest(x, y, rc, self->mode_, self->message_, font);
        self->armedButton_ = PromptModalButton::None;
        if (hit == PromptModalButton::Ok || hit == PromptModalButton::Cancel) {
            self->armedButton_ = hit;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (GetCapture() == hwnd) ReleaseCapture();
        if (self->suppressClickUntilRelease_) {
            self->suppressClickUntilRelease_ = false;
            self->armedButton_ = PromptModalButton::None;
            return 0;
        }
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HFONT font = self->messageFont_ ? self->messageFont_ : self->font_;
        const PromptModalButton hit =
            PromptModalHitTest(x, y, rc, self->mode_, self->message_, font);
        const PromptModalButton armed = self->armedButton_;
        self->armedButton_ = PromptModalButton::None;
        // Require press+release on the same button (no click-through from opener).
        if (armed != PromptModalButton::None && hit == armed) {
            self->Close(hit);
        }
        return 0;
    }
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        return 0;
    case WM_MOUSEWHEEL:
        return 0;
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
