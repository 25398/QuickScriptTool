// ──────────────────────────────────────────────────────────────────
// match_overlay.cpp — QQ-style frozen screen + match markers
// ──────────────────────────────────────────────────────────────────

#include "match_overlay.h"
#include "drawing.h"

#include <cstdio>
#include <windowsx.h>

bool MatchOverlay::classRegistered_ = false;

MatchOverlay::MatchOverlay() = default;

MatchOverlay::~MatchOverlay() {
    CleanupGdi();
}

void MatchOverlay::CleanupGdi() {
    if (screenBitmap_) { DeleteObject(screenBitmap_); screenBitmap_ = nullptr; }
    if (statusFont_) { DeleteObject(statusFont_); statusFont_ = nullptr; }
    if (labelFont_) { DeleteObject(labelFont_); labelFont_ = nullptr; }
    if (magnifierFont_) { DeleteObject(magnifierFont_); magnifierFont_ = nullptr; }
}

void MatchOverlay::RegisterWindowClass() {
    if (classRegistered_) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = &MatchOverlay::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);
    classRegistered_ = true;
}

void MatchOverlay::CaptureScreen() {
    screenX_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
    screenY_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
    screenW_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    screenH_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    if (screenBitmap_) DeleteObject(screenBitmap_);
    screenBitmap_ = CreateCompatibleBitmap(screenDc, screenW_, screenH_);
    HGDIOBJ oldBmp = SelectObject(memDc, screenBitmap_);
    BitBlt(memDc, 0, 0, screenW_, screenH_, screenDc, screenX_, screenY_, SRCCOPY);
    SelectObject(memDc, oldBmp);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
}

void MatchOverlay::RunMatch() {
    if (matchDone_) return;

    HBITMAP tmpl = LoadBitmapFromFile(imagePath_);
    if (!tmpl) {
        loadFailed_ = true;
        matchDone_ = true;
        return;
    }

    ImageMatchOptions opt;
    opt.thresholdPercent = thresholdPercent_;
    opt.scaleMin = scaleMin_;
    opt.scaleMax = scaleMax_;
    opt.scaleStep = 0.05;
    opt.maxMatches = 20;
    opt.maxOverlap = 0.5;

    ImageMatchOutput output = FindTemplateInFrozenScreenMulti(
        screenBitmap_, screenX_, screenY_,
        searchX1_, searchY1_, searchX2_, searchY2_,
        tmpl, opt);

    DeleteBitmapHandle(tmpl);

    matchResults_ = output.matches;
    matchResult_ = matchResults_.empty() ? ImageMatchResult{} : matchResults_.front();
    matchMs_ = output.elapsedMs;
    matchCount_ = static_cast<int>(matchResults_.size());
    matchDone_ = true;
}

MatchOverlay::ActionResult MatchOverlay::Show(
    const std::wstring& imagePath,
    int searchX1, int searchY1, int searchX2, int searchY2,
    double thresholdPercent, double scaleMin, double scaleMax,
    MatchOverlayMode mode) {

    imagePath_ = imagePath;
    searchX1_ = searchX1; searchY1_ = searchY1;
    searchX2_ = searchX2; searchY2_ = searchY2;
    thresholdPercent_ = thresholdPercent;
    scaleMin_ = scaleMin;
    scaleMax_ = scaleMax;
    mode_ = mode;
    matchDone_ = false;
    cancelled_ = true;
    pendingCancel_ = false;
    clickX_ = clickY_ = 0;
    matchResult_ = ImageMatchResult{};
    matchResults_.clear();
    matchMs_ = 0;
    matchCount_ = 0;
    loadFailed_ = false;
    cursorValid_ = false;
    loopExited_ = false;
    regionDragging_ = false;
    regionDragStart_ = {};
    regionDragEnd_ = {};
    regionSelection_ = {};

    if (!classRegistered_) RegisterWindowClass();

    if (!statusFont_) {
        statusFont_ = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    }
    if (!labelFont_) {
        labelFont_ = CreateFontW(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    }
    if (!magnifierFont_) {
        magnifierFont_ = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    }

    CaptureScreen();

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kClassName, L"", WS_POPUP,
        screenX_, screenY_, screenW_, screenH_,
        nullptr, nullptr, GetModuleHandleW(nullptr), this);

    if (!hwnd_) {
        ActionResult ar; ar.cancelled = true; return ar;
    }

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    SetFocus(hwnd_);
    PostMessageW(hwnd_, WM_USER + 1, 0, 0);

    MSG msg;
    BOOL bRet;
    while ((bRet = GetMessage(&msg, nullptr, 0, 0)) != 0) {
        if (bRet == -1) break;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    loopExited_ = true;
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    ActionResult ar;
    ar.cancelled = cancelled_;
    ar.offsetX = clickX_;
    ar.offsetY = clickY_;
    if (mode_ == MatchOverlayMode::RelativeRegionPick && !cancelled_ && IsValidRegionSelection()) {
        ar.regionValid = true;
        ar.regionX1 = regionSelection_.left;
        ar.regionY1 = regionSelection_.top;
        ar.regionX2 = regionSelection_.right;
        ar.regionY2 = regionSelection_.bottom;
    }
    return ar;
}

LRESULT CALLBACK MatchOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MatchOverlay* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<MatchOverlay*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    self = reinterpret_cast<MatchOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->Handle(msg, wp, lp);
}

LRESULT MatchOverlay::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_USER + 1:
        RunMatch();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        Paint(hdc);
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE:
        cursorX_ = GET_X_LPARAM(lp) + screenX_;
        cursorY_ = GET_Y_LPARAM(lp) + screenY_;
        cursorValid_ = true;
        if (mode_ == MatchOverlayMode::RelativeRegionPick && regionDragging_ && (wp & MK_LBUTTON)) {
            regionDragEnd_.x = GET_X_LPARAM(lp);
            regionDragEnd_.y = GET_Y_LPARAM(lp);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;

    case WM_SETCURSOR:
        if ((mode_ == MatchOverlayMode::OffsetPick || mode_ == MatchOverlayMode::RelativeRegionPick)
            && matchDone_ && matchResult_.found) {
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        } else {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        }
        return TRUE;

    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        cancelled_ = true;
        pendingCancel_ = true;
        SetCapture(hwnd_);
        SetWindowPos(hwnd_, HWND_TOPMOST, -10000, -10000, 1, 1, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return 0;

    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        if (pendingCancel_) {
            pendingCancel_ = false;
            ReleaseCapture();
            PostQuitMessage(0);
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            cancelled_ = true;
            PostQuitMessage(0);
            return 0;
        }
        break;

    case WM_LBUTTONDOWN:
        if (mode_ == MatchOverlayMode::RelativeRegionPick && matchDone_ && matchResult_.found) {
            regionDragging_ = true;
            regionDragStart_.x = GET_X_LPARAM(lp);
            regionDragStart_.y = GET_Y_LPARAM(lp);
            regionDragEnd_ = regionDragStart_;
            regionSelection_ = RECT{};
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (mode_ == MatchOverlayMode::OffsetPick && matchDone_ && matchResult_.found) {
            clickX_ = GET_X_LPARAM(lp) + screenX_;
            clickY_ = GET_Y_LPARAM(lp) + screenY_;
            cancelled_ = false;
            PostQuitMessage(0);
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (mode_ == MatchOverlayMode::RelativeRegionPick && regionDragging_) {
            regionDragging_ = false;
            regionDragEnd_.x = GET_X_LPARAM(lp);
            regionDragEnd_.y = GET_Y_LPARAM(lp);
            NormalizeRegionSelection();
            ReleaseCapture();
            if (IsValidRegionSelection()) {
                cancelled_ = false;
                PostQuitMessage(0);
            } else {
                regionSelection_ = RECT{};
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        break;

    case WM_DESTROY:
        if (!loopExited_) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

COLORREF MatchOverlay::SamplePixel(int screenX, int screenY) const {
    if (!screenBitmap_) return RGB(0, 0, 0);
    const int lx = screenX - screenX_;
    const int ly = screenY - screenY_;
    if (lx < 0 || ly < 0 || lx >= screenW_ || ly >= screenH_) return RGB(0, 0, 0);

    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, screenBitmap_);
    const COLORREF c = GetPixel(dc, lx, ly);
    SelectObject(dc, old);
    DeleteDC(dc);
    return c;
}

void MatchOverlay::NormalizeRegionSelection() {
    regionSelection_.left = std::min(regionDragStart_.x, regionDragEnd_.x);
    regionSelection_.top = std::min(regionDragStart_.y, regionDragEnd_.y);
    regionSelection_.right = std::max(regionDragStart_.x, regionDragEnd_.x);
    regionSelection_.bottom = std::max(regionDragStart_.y, regionDragEnd_.y);
    regionSelection_.left += screenX_;
    regionSelection_.top += screenY_;
    regionSelection_.right += screenX_;
    regionSelection_.bottom += screenY_;
}

bool MatchOverlay::IsValidRegionSelection() const {
    const int w = regionSelection_.right - regionSelection_.left;
    const int h = regionSelection_.bottom - regionSelection_.top;
    return w >= 5 && h >= 5;
}

void MatchOverlay::DrawRegionSelection(HDC hdc) {
    if (mode_ != MatchOverlayMode::RelativeRegionPick) return;
    RECT drawRc = regionSelection_;
    if (regionDragging_) {
        drawRc.left = std::min(regionDragStart_.x, regionDragEnd_.x) + screenX_;
        drawRc.top = std::min(regionDragStart_.y, regionDragEnd_.y) + screenY_;
        drawRc.right = std::max(regionDragStart_.x, regionDragEnd_.x) + screenX_;
        drawRc.bottom = std::max(regionDragStart_.y, regionDragEnd_.y) + screenY_;
    } else if (!IsValidRegionSelection()) {
        return;
    }
    const int left = drawRc.left - screenX_;
    const int top = drawRc.top - screenY_;
    const int right = drawRc.right - screenX_;
    const int bottom = drawRc.bottom - screenY_;
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 120, 215));
    HBRUSH nullBr = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBr = SelectObject(hdc, nullBr);
    Rectangle(hdc, left, top, right, bottom);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void MatchOverlay::DrawStatusBar(HDC hdc) {
    wchar_t status[256];
    if (loadFailed_) {
        swprintf_s(status, L"[按ESC退出] 错误：找不到图片路径(文件不存在或已删除)");
    } else if (mode_ == MatchOverlayMode::RelativeRegionPick && matchCount_ > 0) {
        swprintf_s(status, L"[按ESC退出] 找图用时: %d毫秒, 找到%d个, 请框选识别区域",
                   matchMs_, matchCount_);
    } else if (mode_ == MatchOverlayMode::RelativeRegionPick) {
        swprintf_s(status, L"[按ESC退出] 找图用时: %d毫秒, 找到0个, 无法框选区域", matchMs_);
    } else if (matchCount_ > 0) {
        swprintf_s(status, L"[按ESC退出] 找图用时: %d毫秒, 找到%d个, 请在图中检查红框标志",
                   matchMs_, matchCount_);
    } else {
        swprintf_s(status, L"[按ESC退出] 找图用时: %d毫秒, 找到0个", matchMs_);
    }

    SetBkMode(hdc, TRANSPARENT);
    HGDIOBJ oldFont = statusFont_ ? SelectObject(hdc, statusFont_) : nullptr;

    SIZE textSz{};
    const int len = static_cast<int>(wcslen(status));
    GetTextExtentPoint32W(hdc, status, len, &textSz);

    const int padX = 12, padY = 6;
    const int barH = textSz.cy + padY * 2;
    const int barW = textSz.cx + padX * 2;

    RECT barRc{8, 8, 8 + barW, 8 + barH};
    FillRectColor(hdc, barRc, RGB(30, 30, 30));
    DrawBorderRect(hdc, barRc, RGB(120, 100, 40));

    SetTextColor(hdc, kStatusTextColor);
    TextOutW(hdc, barRc.left + padX, barRc.top + padY, status, len);

    if (oldFont) SelectObject(hdc, oldFont);
}

void MatchOverlay::DrawMatchMarkers(HDC hdc) {
    if (!matchDone_ || matchResults_.empty()) return;

    HPEN redPen = CreatePen(PS_SOLID, kBorderWidth, kBorderColor);
    HBRUSH nullBr = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HGDIOBJ oldPen = SelectObject(hdc, redPen);
    HGDIOBJ oldBr = SelectObject(hdc, nullBr);
    HGDIOBJ oldFont = labelFont_ ? SelectObject(hdc, labelFont_) : nullptr;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kLabelColor);

    for (size_t i = 0; i < matchResults_.size(); ++i) {
        const auto& m = matchResults_[i];
        const int left = m.topLeftX - screenX_;
        const int top = m.topLeftY - screenY_;
        const int mw = m.bottomRightX - m.topLeftX;
        const int mh = m.bottomRightY - m.topLeftY;
        if (mw <= 0 || mh <= 0) continue;

        Rectangle(hdc, left, top, left + mw, top + mh);

        wchar_t label[96];
        swprintf_s(label, L"%zu. 匹配度 %d  缩放 %.2f",
                   i + 1, static_cast<int>(m.score + 0.5), m.scale);

        int labelX = left;
        int labelY = top - 22;
        if (labelY < 4) labelY = top + mh + 4;
        TextOutW(hdc, labelX, labelY, label, static_cast<int>(wcslen(label)));
    }

    if (oldFont) SelectObject(hdc, oldFont);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(redPen);
}

void MatchOverlay::DrawMagnifier(HDC hdc) {
    if (!cursorValid_ || !screenBitmap_) return;

    const int lx = cursorX_ - screenX_;
    const int ly = cursorY_ - screenY_;
    if (lx < 0 || ly < 0 || lx >= screenW_ || ly >= screenH_) return;

    constexpr int kMagSize = 130;
    constexpr int kZoom = 8;
    constexpr int kSample = kMagSize / kZoom;  // ~16px sample region

    // Position magnifier near cursor (offset to bottom-right)
    int magX = lx + 20;
    int magY = ly + 20;
    if (magX + kMagSize + 10 > screenW_) magX = lx - kMagSize - 20;
    if (magY + kMagSize + 50 > screenH_) magY = ly - kMagSize - 50;

    // White background + border
    RECT magRc{magX - 1, magY - 1, magX + kMagSize + 1, magY + kMagSize + 1};
    HBRUSH whiteBr = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    FillRect(hdc, &magRc, whiteBr);

    HPEN framePen = CreatePen(PS_SOLID, 2, RGB(80, 80, 80));
    HBRUSH nullBr = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HGDIOBJ oldPen = SelectObject(hdc, framePen);
    HGDIOBJ oldBr = SelectObject(hdc, nullBr);
    Rectangle(hdc, magRc.left, magRc.top, magRc.right, magRc.bottom);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(framePen);

    // Zoomed region via StretchBlt
    const int srcX = lx - kSample / 2;
    const int srcY = ly - kSample / 2;

    HDC srcDc = CreateCompatibleDC(hdc);
    HGDIOBJ oldSrc = SelectObject(srcDc, screenBitmap_);
    SetStretchBltMode(hdc, COLORONCOLOR);
    StretchBlt(hdc, magX, magY, kMagSize, kMagSize,
               srcDc, srcX, srcY, kSample, kSample, SRCCOPY);
    SelectObject(srcDc, oldSrc);
    DeleteDC(srcDc);

    // Crosshair at center
    HPEN crossPen = CreatePen(PS_SOLID, 1, RGB(0, 120, 255));
    oldPen = SelectObject(hdc, crossPen);
    const int cx = magX + kMagSize / 2;
    const int cy = magY + kMagSize / 2;
    MoveToEx(hdc, cx - 10, cy, nullptr); LineTo(hdc, cx + 10, cy);
    MoveToEx(hdc, cx, cy - 10, nullptr); LineTo(hdc, cx, cy + 10);
    SelectObject(hdc, oldPen);
    DeleteObject(crossPen);

    // Color + coordinates below magnifier
    const COLORREF px = SamplePixel(cursorX_, cursorY_);
    wchar_t info[128];
    swprintf_s(info, L"#%02X%02X%02X  (%d, %d)",
               GetRValue(px), GetGValue(px), GetBValue(px),
               cursorX_, cursorY_);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(40, 40, 40));
    HGDIOBJ oldFont = magnifierFont_ ? SelectObject(hdc, magnifierFont_) : nullptr;
    TextOutW(hdc, magX, magY + kMagSize + 4, info, static_cast<int>(wcslen(info)));
    if (oldFont) SelectObject(hdc, oldFont);
}

void MatchOverlay::Paint(HDC hdc) {
    RECT rcClient{};
    GetClientRect(hwnd_, &rcClient);
    const int w = rcClient.right - rcClient.left;
    const int h = rcClient.bottom - rcClient.top;

    HDC memDc = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

    // 1. Frozen screen — no dark overlay
    if (screenBitmap_) {
        HDC scrDc = CreateCompatibleDC(memDc);
        HGDIOBJ oldScr = SelectObject(scrDc, screenBitmap_);
        BitBlt(memDc, 0, 0, w, h, scrDc, 0, 0, SRCCOPY);
        SelectObject(scrDc, oldScr);
        DeleteDC(scrDc);
    }

    // 2. Status bar (top-left)
    if (matchDone_) {
        DrawStatusBar(memDc);
    }

    // 3. Red match box + score label
    DrawMatchMarkers(memDc);

    // 3b. OCR relative region selection
    DrawRegionSelection(memDc);

    // 4. Magnifier following cursor
    if (matchDone_) {
        DrawMagnifier(memDc);
    }

    BitBlt(hdc, 0, 0, w, h, memDc, 0, 0, SRCCOPY);
    SelectObject(memDc, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDc);
}
