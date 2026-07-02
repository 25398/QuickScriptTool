 // ──────────────────────────────────────────────────────────────────
// screenshot_overlay.cpp — QQ-style screenshot overlay implementation
// Full-screen transparent overlay for region selection and screenshot
// ──────────────────────────────────────────────────────────────────

#include "screenshot_overlay.h"

#include <algorithm>
#include <cstring>
#include <windowsx.h>

bool ScreenshotOverlay::classRegistered_ = false;

// ── Static window class registration ───────────────────────────
void ScreenshotOverlay::RegisterWindowClass() {
    if (classRegistered_) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = &ScreenshotOverlay::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);
    classRegistered_ = true;
}

// ── Construction / Destruction ─────────────────────────────────
ScreenshotOverlay::ScreenshotOverlay() = default;

ScreenshotOverlay::~ScreenshotOverlay() {
    if (screenBitmap_) { DeleteObject(screenBitmap_); screenBitmap_ = nullptr; }
    if (dimOverlay_) { DeleteObject(dimOverlay_); dimOverlay_ = nullptr; }
    // hwnd_ is already destroyed in Show() after the message loop exits.
    // Only destroy if Show() was never called or CreateWindow failed (hwnd_ would be nullptr then).
}

// ── Show the overlay ───────────────────────────────────────────
void ScreenshotOverlay::Show(std::function<void(RECT)> onConfirm) {
    onConfirm_ = std::move(onConfirm);
    resultCancelled_ = false;
    if (!classRegistered_) RegisterWindowClass();

    CaptureVirtualScreen();

    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kClassName, L"", WS_POPUP,
        x, y, w, h, nullptr, nullptr,
        GetModuleHandleW(nullptr), this);

    if (!hwnd_) return;

    state_ = State::Dragging;
    selection_ = RECT{};
    dragStart_ = POINT{};
    dragEnd_ = POINT{};

    ShowWindow(hwnd_, SW_SHOW);
    SetFocus(hwnd_);
    SetCapture(hwnd_);

    // ── Blocking modal message loop (same pattern as Flameshot / all screenshot tools) ──
    MSG msg;
    BOOL bRet;
    while ((bRet = GetMessage(&msg, nullptr, 0, 0)) != 0) {
        if (bRet == -1) break;  // GetMessage error
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // ── Loop exited (PostQuitMessage was called from WndProc) ──
    // Window is still alive but hidden; destroy it cleanly
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    // Fire callback — window is gone so screen capture is clean
    if (onConfirm_) {
        onConfirm_(resultCancelled_ ? RECT{} : selection_);
    }
}

// ── Window procedure ───────────────────────────────────────────
LRESULT CALLBACK ScreenshotOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ScreenshotOverlay* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<ScreenshotOverlay*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    self = reinterpret_cast<ScreenshotOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Message handler ────────────────────────────────────────────
LRESULT ScreenshotOverlay::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        Paint(hdc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_SETCURSOR: {
        if (state_ == State::Dragging) {
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        } else {
            POINT pt{}; GetCursorPos(&pt); ScreenToClient(hwnd_, &pt);
            const State hit = HitTest(pt.x, pt.y);
            const wchar_t* cursor = IDC_ARROW;
            if (hit == State::Moving) cursor = IDC_SIZEALL;
            else if (hit == State::ResizingN || hit == State::ResizingS) cursor = IDC_SIZENS;
            else if (hit == State::ResizingW || hit == State::ResizingE) cursor = IDC_SIZEWE;
            else if (hit == State::ResizingNW || hit == State::ResizingSE) cursor = IDC_SIZENWSE;
            else if (hit == State::ResizingNE || hit == State::ResizingSW) cursor = IDC_SIZENESW;
            else if (HitToolbar(pt.x, pt.y)) cursor = IDC_HAND;
            SetCursor(LoadCursorW(nullptr, cursor));
        }
        return TRUE;
    }

    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);

        if (state_ == State::Dragging) {
            dragStart_ = POINT{x, y};
            dragEnd_ = POINT{x, y};
            return 0;
        }

        if (state_ == State::Selected) {
            if (HitConfirm(x, y)) {
                NormalizeSelection();
                if (IsValidSelection()) {
                    resultCancelled_ = false;
                    ReleaseCapture();
                    ShowWindow(hwnd_, SW_HIDE);
                    PostQuitMessage(0);
                }
                return 0;
            }
            if (HitCancel(x, y)) {
                resultCancelled_ = true;
                ReleaseCapture();
                ShowWindow(hwnd_, SW_HIDE);
                PostQuitMessage(0);
                return 0;
            }

            const State hit = HitTest(x, y);
            if (hit == State::Moving) {
                state_ = State::Moving;
                moveAnchor_ = POINT{x, y};
                moveStart_ = selection_;
                SetCapture(hwnd_);
                return 0;
            }
            if (hit >= State::ResizingN && hit <= State::ResizingSE) {
                state_ = hit;
                dragStart_ = POINT{x, y};
                moveStart_ = selection_;
                SetCapture(hwnd_);
                return 0;
            }

            // Clicked outside selection -> restart dragging
            state_ = State::Dragging;
            dragStart_ = POINT{x, y};
            dragEnd_ = POINT{x, y};
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        // Double-click inside selection = confirm (Flameshot/QQ style)
        if (state_ == State::Selected) {
            const int x = GET_X_LPARAM(lp);
            const int y = GET_Y_LPARAM(lp);
            if (x > selection_.left && x < selection_.right &&
                y > selection_.top && y < selection_.bottom) {
                NormalizeSelection();
                if (IsValidSelection()) {
                    resultCancelled_ = false;
                    ReleaseCapture();
                    ShowWindow(hwnd_, SW_HIDE);
                    PostQuitMessage(0);
                }
                return 0;
            }
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);

        if (state_ == State::Dragging && (wp & MK_LBUTTON)) {
            dragEnd_ = POINT{x, y};
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        if (state_ == State::Moving && (wp & MK_LBUTTON)) {
            const int dx = x - moveAnchor_.x;
            const int dy = y - moveAnchor_.y;
            selection_.left   = moveStart_.left   + dx;
            selection_.top    = moveStart_.top    + dy;
            selection_.right  = moveStart_.right  + dx;
            selection_.bottom = moveStart_.bottom + dy;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }

        if (state_ >= State::ResizingN && state_ <= State::ResizingSE && (wp & MK_LBUTTON)) {
            const int dx = x - dragStart_.x;
            const int dy = y - dragStart_.y;
            selection_ = moveStart_;
            switch (state_) {
            case State::ResizingN:  selection_.top    += dy; break;
            case State::ResizingS:  selection_.bottom += dy; break;
            case State::ResizingW:  selection_.left   += dx; break;
            case State::ResizingE:  selection_.right  += dx; break;
            case State::ResizingNW: selection_.left   += dx; selection_.top    += dy; break;
            case State::ResizingNE: selection_.right  += dx; selection_.top    += dy; break;
            case State::ResizingSW: selection_.left   += dx; selection_.bottom += dy; break;
            case State::ResizingSE: selection_.right  += dx; selection_.bottom += dy; break;
            default: break;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (state_ == State::Dragging) {
            // Drag-and-release = immediate confirm (mainstream screenshot flow)
            NormalizeSelection();
            if (IsValidSelection()) {
                resultCancelled_ = false;
                ReleaseCapture();
                ShowWindow(hwnd_, SW_HIDE);
                PostQuitMessage(0);
            } else {
                // Too small — restart
                selection_ = RECT{};
                dragStart_ = POINT{};
                dragEnd_ = POINT{};
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        if (state_ == State::Moving || (state_ >= State::ResizingN && state_ <= State::ResizingSE)) {
            NormalizeSelection();
            state_ = State::Selected;
            ReleaseCapture();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        return 0;
    }

    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: {
        resultCancelled_ = true;
        ReleaseCapture();
        ShowWindow(hwnd_, SW_HIDE);
        PostQuitMessage(0);
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            resultCancelled_ = true;
            ReleaseCapture();
            ShowWindow(hwnd_, SW_HIDE);
            PostQuitMessage(0);
            return 0;
        }
        if (wp == VK_RETURN) {
            NormalizeSelection();
            if (IsValidSelection()) {
                resultCancelled_ = false;
                ReleaseCapture();
                ShowWindow(hwnd_, SW_HIDE);
                PostQuitMessage(0);
            }
            return 0;
        }
        return 0;

    case WM_DESTROY:
        return 0;

    default:
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

// ── Capture the entire virtual screen ──────────────────────────
void ScreenshotOverlay::CaptureVirtualScreen() {
    if (screenBitmap_) { DeleteObject(screenBitmap_); screenBitmap_ = nullptr; }
    if (dimOverlay_) { DeleteObject(dimOverlay_); dimOverlay_ = nullptr; }

    screenX_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
    screenY_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
    screenW_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    screenH_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    screenBitmap_ = CreateCompatibleBitmap(screenDc, screenW_, screenH_);
    HGDIOBJ oldBmp = SelectObject(memDc, screenBitmap_);
    BitBlt(memDc, 0, 0, screenW_, screenH_, screenDc, screenX_, screenY_, SRCCOPY);
    SelectObject(memDc, oldBmp);

    // Pre-render the dim overlay (solid black) — reused every frame
    dimOverlay_ = CreateCompatibleBitmap(screenDc, screenW_, screenH_);
    HGDIOBJ oldDim = SelectObject(memDc, dimOverlay_);
    RECT full{0, 0, screenW_, screenH_};
    HBRUSH blackBr = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(memDc, &full, blackBr);
    DeleteObject(blackBr);
    SelectObject(memDc, oldDim);

    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

// ── Normalize selection ────────────────────────────────────────
void ScreenshotOverlay::NormalizeSelection() {
    if (state_ == State::Dragging) {
        selection_.left   = std::min(dragStart_.x, dragEnd_.x);
        selection_.top    = std::min(dragStart_.y, dragEnd_.y);
        selection_.right  = std::max(dragStart_.x, dragEnd_.x);
        selection_.bottom = std::max(dragStart_.y, dragEnd_.y);
    } else {
        LONG l = selection_.left, r = selection_.right;
        LONG t = selection_.top, b = selection_.bottom;
        selection_.left   = std::min(l, r);
        selection_.top    = std::min(t, b);
        selection_.right  = std::max(l, r);
        selection_.bottom = std::max(t, b);
    }
    // Clamp
    auto clamp = [](LONG v, LONG lo, LONG hi) -> LONG { return v < lo ? lo : v > hi ? hi : v; };
    selection_.left   = clamp(selection_.left,   0, screenW_);
    selection_.top    = clamp(selection_.top,    0, screenH_);
    selection_.right  = clamp(selection_.right,  0, screenW_);
    selection_.bottom = clamp(selection_.bottom, 0, screenH_);
}

bool ScreenshotOverlay::IsValidSelection() const {
    const int w = selection_.right - selection_.left;
    const int h = selection_.bottom - selection_.top;
    return w >= 5 && h >= 5;
}

// ── Hit testing ────────────────────────────────────────────────
ScreenshotOverlay::State ScreenshotOverlay::HitTest(int x, int y) {
    if (state_ != State::Selected) return State::Dragging;

    const int l = selection_.left, t = selection_.top;
    const int r = selection_.right, b = selection_.bottom;
    const int hs = kHandleSize;

    // Corners
    if (abs(x - l) <= hs && abs(y - t) <= hs) return State::ResizingNW;
    if (abs(x - r) <= hs && abs(y - t) <= hs) return State::ResizingNE;
    if (abs(x - l) <= hs && abs(y - b) <= hs) return State::ResizingSW;
    if (abs(x - r) <= hs && abs(y - b) <= hs) return State::ResizingSE;

    // Edges
    if (x > l + hs && x < r - hs && abs(y - t) <= hs) return State::ResizingN;
    if (x > l + hs && x < r - hs && abs(y - b) <= hs) return State::ResizingS;
    if (y > t + hs && y < b - hs && abs(x - l) <= hs) return State::ResizingW;
    if (y > t + hs && y < b - hs && abs(x - r) <= hs) return State::ResizingE;

    // Inside
    if (x > l && x < r && y > t && y < b) return State::Moving;

    // Toolbar
    if (HitToolbar(x, y)) return State::Selected;

    return State::Dragging;
}

RECT ScreenshotOverlay::ToolbarRect() const {
    const int tw = kToolbarBtnW * 2 + 8;
    const int gap = 12;
    int ty = selection_.bottom + gap;
    if (ty + kToolbarH > screenH_) {
        ty = selection_.top - gap - kToolbarH;
        if (ty < 0) ty = selection_.bottom + 2;
    }
    int tx = selection_.right - tw;
    if (tx < 0) tx = selection_.left;
    return RECT{tx, ty, tx + tw, ty + kToolbarH};
}

bool ScreenshotOverlay::HitConfirm(int x, int y) const {
    const RECT tb = ToolbarRect();
    return x >= tb.left && x <= tb.left + kToolbarBtnW &&
           y >= tb.top && y <= tb.bottom;
}

bool ScreenshotOverlay::HitCancel(int x, int y) const {
    const RECT tb = ToolbarRect();
    return x >= tb.right - kToolbarBtnW && x <= tb.right &&
           y >= tb.top && y <= tb.bottom;
}

bool ScreenshotOverlay::HitToolbar(int x, int y) const {
    const RECT tb = ToolbarRect();
    return x >= tb.left && x <= tb.right && y >= tb.top && y <= tb.bottom;
}

// ──────────────────────────────────────────────────────────────────
// Paint
// ──────────────────────────────────────────────────────────────────
void ScreenshotOverlay::Paint(HDC hdc) {
    // Full double-buffer: draw everything off-screen, then present in a single blit.
    // This eliminates flicker caused by partial draws during rapid mouse drag.
    HDC memDc = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, screenW_, screenH_);
    HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

    DrawBackground(memDc);

    RECT currentSel = selection_;
    if (state_ == State::Dragging) {
        currentSel = RECT{
            std::min(dragStart_.x, dragEnd_.x),
            std::min(dragStart_.y, dragEnd_.y),
            std::max(dragStart_.x, dragEnd_.x),
            std::max(dragStart_.y, dragEnd_.y)
        };
    }

    const bool hasSel = (state_ == State::Dragging)
        ? (currentSel.right > currentSel.left + 1 && currentSel.bottom > currentSel.top + 1)
        : (currentSel.right > currentSel.left && currentSel.bottom > currentSel.top);

    if (hasSel) {
        // Draw clear area
        if (screenBitmap_) {
            HDC mem = CreateCompatibleDC(memDc);
            HGDIOBJ old = SelectObject(mem, screenBitmap_);
            BitBlt(memDc, currentSel.left, currentSel.top,
                currentSel.right - currentSel.left, currentSel.bottom - currentSel.top,
                mem, currentSel.left, currentSel.top, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
    }

    if (state_ != State::Dragging && IsValidSelection()) {
        DrawSelectionBorder(memDc);
        DrawSizeInfo(memDc);
        DrawToolbar(memDc);
    } else if (state_ == State::Dragging && hasSel) {
        // Dragging border without handles
        HPEN pen = CreatePen(PS_SOLID, kBorderWidth, kBorderColor);
        HGDIOBJ oldPen = SelectObject(memDc, pen);
        HGDIOBJ oldBrush = SelectObject(memDc, GetStockObject(NULL_BRUSH));
        Rectangle(memDc, currentSel.left, currentSel.top, currentSel.right, currentSel.bottom);
        SelectObject(memDc, oldPen);
        SelectObject(memDc, oldBrush);
        DeleteObject(pen);

        // Size info while dragging
        const int w = currentSel.right - currentSel.left;
        const int h = currentSel.bottom - currentSel.top;
        std::wstring text = std::to_wstring(w) + L" × " + std::to_wstring(h);
        SetBkMode(memDc, TRANSPARENT);
        SetTextColor(memDc, RGB(255, 255, 255));
        HFONT font = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
        HGDIOBJ oldFont = SelectObject(memDc, font);
        RECT textRc{currentSel.left + 4, currentSel.top - 20, currentSel.left + 200, currentSel.top - 2};
        if (textRc.top < 0) textRc = RECT{currentSel.left + 4, currentSel.bottom + 4, currentSel.left + 200, currentSel.bottom + 22};
        DrawTextW(memDc, text.c_str(), -1, &textRc, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
        SelectObject(memDc, oldFont);
        DeleteObject(font);
    }

    // Present in a single blit
    BitBlt(hdc, 0, 0, screenW_, screenH_, memDc, 0, 0, SRCCOPY);
    SelectObject(memDc, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDc);
}

void ScreenshotOverlay::DrawBackground(HDC hdc) {
    // Build composite in a memory DC
    HDC memDc = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, screenW_, screenH_);
    HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

    // Draw the full screen capture as base
    if (screenBitmap_) {
        HDC srcDc = CreateCompatibleDC(memDc);
        HGDIOBJ oldSrc = SelectObject(srcDc, screenBitmap_);
        BitBlt(memDc, 0, 0, screenW_, screenH_, srcDc, 0, 0, SRCCOPY);
        SelectObject(srcDc, oldSrc);
        DeleteDC(srcDc);
    } else {
        RECT fill{0, 0, screenW_, screenH_};
        HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(memDc, &fill, black);
        DeleteObject(black);
    }

    // Determine current selection rectangle
    RECT sel = selection_;
    if (state_ == State::Dragging) {
        sel = RECT{
            std::min(dragStart_.x, dragEnd_.x),
            std::min(dragStart_.y, dragEnd_.y),
            std::max(dragStart_.x, dragEnd_.x),
            std::max(dragStart_.y, dragEnd_.y)
        };
    }

    // Alpha-blend the pre-cached dark overlay, excluding the selection area
    if (sel.right > sel.left + 1 && sel.bottom > sel.top + 1) {
        HRGN fullRgn = CreateRectRgn(0, 0, screenW_, screenH_);
        HRGN holeRgn = CreateRectRgn(sel.left, sel.top, sel.right, sel.bottom);
        CombineRgn(fullRgn, fullRgn, holeRgn, RGN_DIFF);
        SelectClipRgn(memDc, fullRgn);
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = kOverlayAlpha;
        if (dimOverlay_) {
            HDC ovDc = CreateCompatibleDC(memDc);
            HGDIOBJ oldOv = SelectObject(ovDc, dimOverlay_);
            AlphaBlend(memDc, 0, 0, screenW_, screenH_, ovDc, 0, 0, screenW_, screenH_, blend);
            SelectObject(ovDc, oldOv);
            DeleteDC(ovDc);
        }
        SelectClipRgn(memDc, nullptr);
        DeleteObject(holeRgn);
        DeleteObject(fullRgn);
    } else if (dimOverlay_) {
        // No selection yet — apply full dim
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = kOverlayAlpha;
        HDC ovDc = CreateCompatibleDC(memDc);
        HGDIOBJ oldOv = SelectObject(ovDc, dimOverlay_);
        AlphaBlend(memDc, 0, 0, screenW_, screenH_, ovDc, 0, 0, screenW_, screenH_, blend);
        SelectObject(ovDc, oldOv);
        DeleteDC(ovDc);
    }

    // Copy to window DC
    BitBlt(hdc, 0, 0, screenW_, screenH_, memDc, 0, 0, SRCCOPY);

    SelectObject(memDc, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDc);
}

void ScreenshotOverlay::DrawSelectionBorder(HDC hdc) {
    const int l = selection_.left, t = selection_.top;
    const int r = selection_.right, b = selection_.bottom;

    // Blue border
    HPEN pen = CreatePen(PS_SOLID, kBorderWidth, kBorderColor);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, l, t, r, b);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);

    // 8 resize handles (white fill, blue border)
    HBRUSH handleBrush = CreateSolidBrush(RGB(255, 255, 255));
    HPEN handlePen = CreatePen(PS_SOLID, 1, kBorderColor);
    oldPen = SelectObject(hdc, handlePen);
    oldBrush = SelectObject(hdc, handleBrush);

    const int hs = kHandleSize;
    auto drawH = [&](int cx, int cy) {
        Rectangle(hdc, cx - hs, cy - hs, cx + hs + 1, cy + hs + 1);
    };

    drawH(l, t); drawH(r, t);
    drawH(l, b); drawH(r, b);
    drawH((l + r) / 2, t); drawH((l + r) / 2, b);
    drawH(l, (t + b) / 2); drawH(r, (t + b) / 2);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(handleBrush);
    DeleteObject(handlePen);
}

void ScreenshotOverlay::DrawSizeInfo(HDC hdc) {
    const int sw = selection_.right - selection_.left;
    const int sh = selection_.bottom - selection_.top;
    std::wstring text = std::to_wstring(sw) + L" × " + std::to_wstring(sh);

    HFONT font = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    HGDIOBJ oldFont = SelectObject(hdc, font);

    RECT measure{0, 0, 0, 0};
    DrawTextW(hdc, text.c_str(), -1, &measure, DT_CALCRECT | DT_SINGLELINE);
    const int tw = measure.right - measure.left + 16;
    const int th = 24;

    int ty = selection_.top - th - 4;
    if (ty < 0) ty = selection_.bottom + 4;
    int tx = selection_.left + 4;

    RECT bgRc{tx, ty, tx + tw, ty + th};
    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &bgRc, bg);
    DeleteObject(bg);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    RECT textRc{tx + 8, ty, tx + tw - 8, ty + th};
    DrawTextW(hdc, text.c_str(), -1, &textRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

void ScreenshotOverlay::DrawToolbar(HDC hdc) {
    const RECT tb = ToolbarRect();

    // Toolbar background
    HBRUSH bg = CreateSolidBrush(RGB(45, 45, 45));
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, bg));
    // Use PatBlt directly
    SelectObject(hdc, GetStockObject(NULL_PEN));
    Rectangle(hdc, tb.left, tb.top, tb.right, tb.bottom);
    SelectObject(hdc, oldBrush);
    DeleteObject(bg);

    SetBkMode(hdc, TRANSPARENT);

    // Confirm button (green)
    RECT confirmRc{tb.left + 3, tb.top + 4, tb.left + kToolbarBtnW - 3, tb.bottom - 4};
    HBRUSH green = CreateSolidBrush(RGB(0, 150, 70));
    FillRect(hdc, &confirmRc, green);
    DeleteObject(green);
    SetTextColor(hdc, RGB(255, 255, 255));
    HFONT font = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    HGDIOBJ oldFont = SelectObject(hdc, font);
    DrawTextW(hdc, L"✓ 确认", -1, &confirmRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

    // Cancel button
    RECT cancelRc{tb.right - kToolbarBtnW + 3, tb.top + 4, tb.right - 3, tb.bottom - 4};
    HBRUSH grayBtn = CreateSolidBrush(RGB(90, 90, 90));
    FillRect(hdc, &cancelRc, grayBtn);
    DeleteObject(grayBtn);
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextW(hdc, L"✗ 取消", -1, &cancelRc, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

    // Title text between buttons
    SetTextColor(hdc, RGB(170, 170, 170));
    RECT titleArea{confirmRc.right + 4, tb.top, cancelRc.left - 4, tb.bottom};
    DrawTextW(hdc, title_.c_str(), -1, &titleArea, DT_SINGLELINE | DT_CENTER | DT_VCENTER);

    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

void ScreenshotOverlay::DrawMagnifier(HDC hdc) {
    (void)hdc;
}
