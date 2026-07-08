#include "prompt_modal.h"

#include "drawing.h"

#include <windowsx.h>

namespace {

constexpr COLORREF kPromptDialogBg = RGB(18, 18, 18);
constexpr COLORREF kPromptDialogBorder = RGB(70, 70, 70);
constexpr COLORREF kPromptText = RGB(255, 226, 110);
constexpr COLORREF kPromptOkFill = RGB(255, 188, 75);
constexpr COLORREF kPromptOkText = RGB(70, 45, 10);
constexpr COLORREF kPromptCancelBorder = RGB(255, 205, 95);
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

void DrawPromptText(HDC hdc, HFONT font, const std::wstring& text, const RECT& rc, COLORREF color, UINT format) {
    HGDIOBJ oldFont = font ? SelectObject(hdc, font) : nullptr;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    DrawTextW(hdc, text.c_str(), -1, const_cast<RECT*>(&rc), format);
    if (oldFont) SelectObject(hdc, oldFont);
}

void DrawPromptMessageCentered(HDC hdc, HFONT font, const std::wstring& text, const RECT& area, COLORREF color) {
    HGDIOBJ oldFont = font ? SelectObject(hdc, font) : nullptr;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    RECT measure = area;
    DrawTextW(hdc, text.c_str(), -1, &measure,
        DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_EDITCONTROL);
    const int textH = measure.bottom - measure.top;
    const int areaH = area.bottom - area.top;
    RECT draw = area;
    if (textH > 0 && textH < areaH) {
        draw.top = area.top + (areaH - textH) / 2;
        draw.bottom = draw.top + textH;
    }
    DrawTextW(hdc, text.c_str(), -1, &draw, DT_CENTER | DT_WORDBREAK | DT_EDITCONTROL);
    if (oldFont) SelectObject(hdc, oldFont);
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

}  // namespace

PromptModalLayout ComputePromptModalLayout(const RECT& client, PromptModalMode mode) {
    PromptModalLayout layout{};
    const int w = 450;
    const int h = 252;
    const int x = client.left + ((client.right - client.left) - w) / 2;
    const int y = client.top + ((client.bottom - client.top) - h) / 2;
    layout.dialog = RECT{x, y, x + w, y + h};

    const int btnTop = layout.dialog.top + 138;
    layout.message = RECT{
        layout.dialog.left + 28,
        layout.dialog.top + 24,
        layout.dialog.right - 28,
        btnTop - 18
    };

    const int btnBottom = layout.dialog.top + 208;
    if (mode == PromptModalMode::Confirm) {
        layout.hasCancel = true;
        layout.ok = RECT{layout.dialog.left + 32, btnTop, layout.dialog.left + 216, btnBottom};
        layout.cancel = RECT{layout.dialog.left + 236, btnTop, layout.dialog.right - 32, btnBottom};
    } else {
        const int btnW = 184;
        const int cx = (layout.dialog.left + layout.dialog.right) / 2;
        layout.ok = RECT{cx - btnW / 2, btnTop, cx + btnW / 2, btnBottom};
    }
    return layout;
}

void PaintPromptModal(HDC hdc, const RECT& client, const std::wstring& message,
    PromptModalMode mode, bool hoverOk, bool hoverCancel, HFONT textFont) {
    FillAlphaRectLocal(hdc, client, RGB(0, 0, 0), kPromptOverlayAlpha);
    const PromptModalLayout layout = ComputePromptModalLayout(client, mode);
    FillRectColor(hdc, layout.dialog, kPromptDialogBg);
    DrawBorderRoundRect(hdc, layout.dialog, kPromptDialogBorder, 6);
    DrawPromptMessageCentered(hdc, textFont, message, layout.message, kPromptText);

    FillRectColor(hdc, layout.ok, hoverOk ? RGB(255, 205, 95) : kPromptOkFill);
    DrawPromptText(hdc, textFont, L"确定", layout.ok, kPromptOkText,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (layout.hasCancel) {
        if (hoverCancel) FillRectColor(hdc, layout.cancel, RGB(40, 40, 40));
        DrawBorderRect(hdc, layout.cancel, kPromptCancelBorder);
        DrawPromptText(hdc, textFont, L"取消", layout.cancel, kPromptText,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

PromptModalButton PromptModalHitTest(int x, int y, const RECT& client, PromptModalMode mode) {
    const PromptModalLayout layout = ComputePromptModalLayout(client, mode);
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
    // 使用 owned WS_POPUP，保证遮罩始终盖在 owner 及其全部子控件之上。
    shield_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
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
    InvalidateRect(shield_, nullptr, TRUE);
    UpdateWindow(shield_);
}

void PromptModal::InvalidateButtonRegion() {
    if (!shield_) return;
    RECT client{};
    GetClientRect(shield_, &client);
    const PromptModalLayout layout = ComputePromptModalLayout(client, mode_);
    InvalidateRect(shield_, &layout.ok, FALSE);
    if (layout.hasCancel) InvalidateRect(shield_, &layout.cancel, FALSE);
}

bool PromptModal::UpdateHover(int x, int y, const RECT& client) {
    const PromptModalLayout layout = ComputePromptModalLayout(client, mode_);
    const bool hoverOk = PtInRectLocal(layout.ok, x, y);
    const bool hoverCancel = layout.hasCancel && PtInRectLocal(layout.cancel, x, y);
    if (hoverOk == hoverOk_ && hoverCancel == hoverCancel_) return false;
    hoverOk_ = hoverOk;
    hoverCancel_ = hoverCancel;
    return true;
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
    onDone_ = nullptr;
    visible_ = true;
    SyncShield();
}

void PromptModal::ShowConfirm(const std::wstring& message, std::function<void(bool accepted)> onDone) {
    message_ = message;
    mode_ = PromptModalMode::Confirm;
    hoverOk_ = false;
    hoverCancel_ = false;
    onDone_ = std::move(onDone);
    visible_ = true;
    SyncShield();
}

void PromptModal::Close(PromptModalButton button) {
    if (!visible_) return;
    auto done = std::move(onDone_);
    visible_ = false;
    hoverOk_ = false;
    hoverCancel_ = false;
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
        PaintPromptModal(hdc, rc, self->message_, self->mode_, self->hoverOk_, self->hoverCancel_,
            self->messageFont_ ? self->messageFont_ : self->font_);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        if (self->UpdateHover(x, y, rc)) self->InvalidateButtonRegion();
        const auto hit = PromptModalHitTest(x, y, rc, self->mode_);
        SetCursor(LoadCursorW(nullptr,
            hit == PromptModalButton::Ok || hit == PromptModalButton::Cancel ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const PromptModalButton hit = PromptModalHitTest(x, y, rc, self->mode_);
        if (hit == PromptModalButton::Ok) self->Close(PromptModalButton::Ok);
        else if (hit == PromptModalButton::Cancel) self->Close(PromptModalButton::Cancel);
        return 0;
    }
    case WM_LBUTTONUP:
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
