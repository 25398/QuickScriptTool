#include "ocr_overlay.h"

#include "drawing.h"
#include "ocr_engine.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <windowsx.h>

bool OcrOverlay::classRegistered_ = false;

OcrOverlay::OcrOverlay() = default;

OcrOverlay::~OcrOverlay() {
    CleanupGdi();
}

void OcrOverlay::CleanupGdi() {
    ClearLineFontCache();
    if (screenBitmap_) { DeleteObject(screenBitmap_); screenBitmap_ = nullptr; }
    if (statusFont_) { DeleteObject(statusFont_); statusFont_ = nullptr; }
    if (magnifierFont_) { DeleteObject(magnifierFont_); magnifierFont_ = nullptr; }
}

void OcrOverlay::ClearLineFontCache() {
    for (auto& entry : lineFontCache_) {
        if (entry.second) DeleteObject(entry.second);
    }
    lineFontCache_.clear();
}

HFONT OcrOverlay::GetOrCreateFont(int fontPx) {
    const int px = std::max(8, fontPx);
    const auto found = lineFontCache_.find(px);
    if (found != lineFontCache_.end()) return found->second;

    HFONT font = CreateFontW(
        -px, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    lineFontCache_[px] = font;
    return font;
}

int OcrOverlay::FitFontPx(HDC hdc, const std::wstring& text, int boxWidth, int boxHeight) {
    if (text.empty() || boxWidth <= 4 || boxHeight <= 4) return 10;

    constexpr int kPadX = 4;
    constexpr int kPadY = 2;
    const int maxW = boxWidth - kPadX;
    const int maxH = boxHeight - kPadY;
    if (maxW <= 0 || maxH <= 0) return 10;

    const HGDIOBJ oldFont = GetCurrentObject(hdc, OBJ_FONT);

    auto measure = [&](int fontPx) -> SIZE {
        HFONT font = GetOrCreateFont(fontPx);
        SelectObject(hdc, font);
        SIZE sz{};
        GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &sz);
        return sz;
    };

    int lo = 8;
    int hi = maxH;
    int best = lo;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const SIZE sz = measure(mid);
        if (sz.cx <= maxW && sz.cy <= maxH) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    if (oldFont) SelectObject(hdc, oldFont);
    return best;
}

void OcrOverlay::DimBoxInterior(HDC hdc, int left, int top, int right, int bottom) const {
    const int w = right - left;
    const int h = bottom - top;
    if (w <= 0 || h <= 0) return;

    HDC memDc = CreateCompatibleDC(hdc);
    if (!memDc) return;

    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    if (!memBmp) {
        DeleteDC(memDc);
        return;
    }

    HGDIOBJ oldBmp = SelectObject(memDc, memBmp);
    HBRUSH fillBr = CreateSolidBrush(RGB(255, 255, 255));
    RECT fillRc{0, 0, w, h};
    FillRect(memDc, &fillRc, fillBr);
    DeleteObject(fillBr);

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = kDimOverlayAlpha;

    AlphaBlend(hdc, left, top, w, h, memDc, 0, 0, w, h, blend);

    SelectObject(memDc, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDc);
}

void OcrOverlay::RegisterWindowClass() {
    if (classRegistered_) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = &OcrOverlay::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);
    classRegistered_ = true;
}

void OcrOverlay::CaptureScreen() {
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

void OcrOverlay::RunOcr() {
    if (ocrDone_) return;

    const OcrEngineOutput output = RunOcrOnScreenRegion(
        searchX1_, searchY1_, searchX2_, searchY2_,
        screenBitmap_, screenX_, screenY_, digitsOnly_);

    ocrSuccess_ = output.success;
    errorMessage_ = output.error;
    lines_ = output.lines;
    elapsedMs_ = output.elapsedMs;
    highlightFound_ = false;

    if (ocrSuccess_ && !highlightText_.empty()) {
        for (const auto& line : lines_) {
            if (line.text.find(highlightText_) != std::wstring::npos) {
                highlightFound_ = true;
                break;
            }
        }
    }

    ocrDone_ = true;
}

OcrOverlay::ActionResult OcrOverlay::Show(int searchX1, int searchY1, int searchX2, int searchY2,
                      const std::wstring& highlightText, OcrOverlayMode mode, bool digitsOnly) {
    searchX1_ = searchX1;
    searchY1_ = searchY1;
    searchX2_ = searchX2;
    searchY2_ = searchY2;
    highlightText_ = highlightText;
    mode_ = mode;
    digitsOnly_ = digitsOnly;
    actionResult_ = {};
    cancelled_ = false;
    pendingCancel_ = false;
    ocrDone_ = false;
    ocrSuccess_ = false;
    errorMessage_.clear();
    lines_.clear();
    elapsedMs_ = 0;
    highlightFound_ = false;
    cursorValid_ = false;
    loopExited_ = false;
    ClearLineFontCache();

    if (!classRegistered_) RegisterWindowClass();

    if (!statusFont_) {
        statusFont_ = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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

    if (!hwnd_) return actionResult_;

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
    if (cancelled_) actionResult_.cancelled = true;
    return actionResult_;
}

OcrTextLine OcrOverlay::NearestLineToPoint(int screenX, int screenY) const {
    OcrTextLine best{};
    long long bestDist = LLONG_MAX;
    for (const auto& line : lines_) {
        const int cx = (line.x1 + line.x2) / 2;
        const int cy = (line.y1 + line.y2) / 2;
        const long long dx = static_cast<long long>(screenX - cx);
        const long long dy = static_cast<long long>(screenY - cy);
        const long long dist = dx * dx + dy * dy;
        if (dist < bestDist) {
            bestDist = dist;
            best = line;
        }
    }
    return best;
}

bool OcrOverlay::PickAnchorCenter(int& centerX, int& centerY) const {
    if (!ocrSuccess_ || lines_.empty()) return false;
    if (!highlightText_.empty()) {
        OcrEngineOutput output;
        output.lines = lines_;
        const auto found = FindTextInOcrLines(output, highlightText_);
        if (!found) return false;
        centerX = (found->x1 + found->x2) / 2;
        centerY = (found->y1 + found->y2) / 2;
        return true;
    }
    int minX = lines_.front().x1, minY = lines_.front().y1;
    int maxX = lines_.front().x2, maxY = lines_.front().y2;
    for (const auto& line : lines_) {
        minX = std::min(minX, line.x1);
        minY = std::min(minY, line.y1);
        maxX = std::max(maxX, line.x2);
        maxY = std::max(maxY, line.y2);
    }
    centerX = (minX + maxX) / 2;
    centerY = (minY + maxY) / 2;
    return true;
}

LRESULT CALLBACK OcrOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    OcrOverlay* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<OcrOverlay*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    self = reinterpret_cast<OcrOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->Handle(msg, wp, lp);
}

LRESULT OcrOverlay::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_USER + 1:
        RunOcr();
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
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;

    case WM_SETCURSOR:
        if (mode_ == OcrOverlayMode::OffsetPick && ocrDone_ && ocrSuccess_ && !lines_.empty()) {
            int cx = 0, cy = 0;
            if (PickAnchorCenter(cx, cy)) {
                SetCursor(LoadCursorW(nullptr, IDC_CROSS));
                return TRUE;
            }
        }
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
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
        if (mode_ == OcrOverlayMode::OffsetPick && ocrDone_ && ocrSuccess_ && !lines_.empty()) {
            const int clickX = GET_X_LPARAM(lp) + screenX_;
            const int clickY = GET_Y_LPARAM(lp) + screenY_;
            cursorX_ = clickX;
            cursorY_ = clickY;
            int centerX = 0, centerY = 0;
            if (PickAnchorCenter(centerX, centerY)) {
                actionResult_.offsetX = clickX - centerX;
                actionResult_.offsetY = clickY - centerY;
                actionResult_.anchorValid = true;
                actionResult_.cancelled = false;
                cancelled_ = false;
                PostQuitMessage(0);
                return 0;
            }
        }
        break;

    case WM_DESTROY:
        if (!loopExited_) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

COLORREF OcrOverlay::SamplePixel(int screenX, int screenY) const {
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

void OcrOverlay::DrawStatusBar(HDC hdc) {
    wchar_t status[384];
    if (mode_ == OcrOverlayMode::OffsetPick) {
        if (!ocrSuccess_) {
            swprintf_s(status, L"[按ESC退出] 文字识别失败: %s",
                errorMessage_.empty() ? L"未知错误" : errorMessage_.c_str());
        } else if (!highlightText_.empty() && !highlightFound_) {
            swprintf_s(status, L"[按ESC退出] 未找到「%s」，无法选择偏移位置", highlightText_.c_str());
        } else if (lines_.empty()) {
            swprintf_s(status, L"[按ESC退出] 未识别到文字，无法选择偏移位置");
        } else {
            swprintf_s(status, L"[按ESC退出] 识别用时: %d毫秒, 请在文字区域点击选择偏移位置",
                elapsedMs_);
        }
    } else if (!ocrSuccess_) {
        swprintf_s(status, L"[按ESC退出] 文字识别失败: %s",
            errorMessage_.empty() ? L"未知错误" : errorMessage_.c_str());
    } else if (!highlightText_.empty()) {
        swprintf_s(status, L"[按ESC退出] 识别用时: %d毫秒, 共%d处文字, 查找「%s」%s",
            elapsedMs_, static_cast<int>(lines_.size()), highlightText_.c_str(),
            highlightFound_ ? L"已找到" : L"未找到");
    } else if (!lines_.empty()) {
        swprintf_s(status, L"[按ESC退出] 识别用时: %d毫秒, 共%d处文字, 请检查红框与覆盖文字",
            elapsedMs_, static_cast<int>(lines_.size()));
    } else {
        swprintf_s(status, L"[按ESC退出] 识别用时: %d毫秒, 未识别到文字", elapsedMs_);
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

void OcrOverlay::DrawOcrMarkers(HDC hdc) {
    if (!ocrDone_ || !ocrSuccess_ || lines_.empty()) return;

    HPEN redPen = CreatePen(PS_SOLID, kBorderWidth, kBorderColor);
    HBRUSH nullBr = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HGDIOBJ oldPen = SelectObject(hdc, redPen);
    HGDIOBJ oldBr = SelectObject(hdc, nullBr);
    HGDIOBJ oldFont = nullptr;
    SetBkMode(hdc, TRANSPARENT);

    for (const auto& line : lines_) {
        const int left = line.x1 - screenX_;
        const int top = line.y1 - screenY_;
        const int right = line.x2 - screenX_;
        const int bottom = line.y2 - screenY_;
        if (right <= left || bottom <= top) continue;

        DimBoxInterior(hdc, left, top, right, bottom);
        Rectangle(hdc, left, top, right, bottom);

        const bool highlighted = !highlightText_.empty()
            && line.text.find(highlightText_) != std::wstring::npos;
        SetTextColor(hdc, highlighted ? kHighlightTextColor : kTextColor);

        if (!line.text.empty()) {
            const int boxWidth = right - left;
            const int boxHeight = bottom - top;
            const int fontPx = FitFontPx(hdc, line.text, boxWidth, boxHeight);
            HFONT lineFont = GetOrCreateFont(fontPx);
            oldFont = SelectObject(hdc, lineFont);

            RECT textRc{left, top, right, bottom};
            DrawTextW(hdc, line.text.c_str(), static_cast<int>(line.text.size()),
                &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }

    if (oldFont) SelectObject(hdc, oldFont);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPen);
    DeleteObject(redPen);
}

void OcrOverlay::DrawMagnifier(HDC hdc) {
    if (!cursorValid_ || !screenBitmap_) return;

    const int lx = cursorX_ - screenX_;
    const int ly = cursorY_ - screenY_;
    if (lx < 0 || ly < 0 || lx >= screenW_ || ly >= screenH_) return;

    constexpr int kMagSize = 130;
    constexpr int kZoom = 8;
    constexpr int kSample = kMagSize / kZoom;

    int magX = lx + 20;
    int magY = ly + 20;
    if (magX + kMagSize + 10 > screenW_) magX = lx - kMagSize - 20;
    if (magY + kMagSize + 50 > screenH_) magY = ly - kMagSize - 50;

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

    const int srcX = lx - kSample / 2;
    const int srcY = ly - kSample / 2;

    HDC srcDc = CreateCompatibleDC(hdc);
    HGDIOBJ oldSrc = SelectObject(srcDc, screenBitmap_);
    SetStretchBltMode(hdc, COLORONCOLOR);
    StretchBlt(hdc, magX, magY, kMagSize, kMagSize,
        srcDc, srcX, srcY, kSample, kSample, SRCCOPY);
    SelectObject(srcDc, oldSrc);
    DeleteDC(srcDc);

    HPEN crossPen = CreatePen(PS_SOLID, 1, RGB(0, 120, 255));
    oldPen = SelectObject(hdc, crossPen);
    const int cx = magX + kMagSize / 2;
    const int cy = magY + kMagSize / 2;
    MoveToEx(hdc, cx - 10, cy, nullptr); LineTo(hdc, cx + 10, cy);
    MoveToEx(hdc, cx, cy - 10, nullptr); LineTo(hdc, cx, cy + 10);
    SelectObject(hdc, oldPen);
    DeleteObject(crossPen);

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

void OcrOverlay::Paint(HDC hdc) {
    RECT rcClient{};
    GetClientRect(hwnd_, &rcClient);
    const int w = rcClient.right - rcClient.left;
    const int h = rcClient.bottom - rcClient.top;

    HDC memDc = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

    if (screenBitmap_) {
        HDC scrDc = CreateCompatibleDC(memDc);
        HGDIOBJ oldScr = SelectObject(scrDc, screenBitmap_);
        BitBlt(memDc, 0, 0, w, h, scrDc, 0, 0, SRCCOPY);
        SelectObject(scrDc, oldScr);
        DeleteDC(scrDc);
    }

    if (ocrDone_) {
        DrawStatusBar(memDc);
    }

    DrawOcrMarkers(memDc);

    if (ocrDone_) {
        DrawMagnifier(memDc);
    }

    BitBlt(hdc, 0, 0, w, h, memDc, 0, 0, SRCCOPY);
    SelectObject(memDc, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDc);
}
