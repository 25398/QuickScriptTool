#include "drag_pick_overlay.h"

#include "image_match.h"
#include "ui_scale.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <windowsx.h>

namespace {

constexpr COLORREF kLineColor = RGB(80, 180, 255);
constexpr BYTE kOverlayAlpha = 90;

class DragPickOverlay {
public:
    enum class Mode { Screen, Template };

    DragPickOverlay() = default;
    ~DragPickOverlay() { CleanupBitmaps(); }

    DragPickOutcome RunScreen() {
        mode_ = Mode::Screen;
        CaptureVirtualScreen();
        return RunLoop();
    }

    DragPickOutcome RunTemplate(HBITMAP src, int imgW, int imgH) {
        mode_ = Mode::Template;
        imgW_ = imgW;
        imgH_ = imgH;
        CaptureVirtualScreenSizeOnly();
        if (src && imgW > 0 && imgH > 0) {
            HDC screenDc = GetDC(nullptr);
            HDC srcDc = CreateCompatibleDC(screenDc);
            HDC dstDc = CreateCompatibleDC(screenDc);
            templateBmp_ = CreateCompatibleBitmap(screenDc, imgW, imgH);
            HGDIOBJ oldSrc = SelectObject(srcDc, src);
            HGDIOBJ oldDst = SelectObject(dstDc, templateBmp_);
            BitBlt(dstDc, 0, 0, imgW, imgH, srcDc, 0, 0, SRCCOPY);
            SelectObject(srcDc, oldSrc);
            SelectObject(dstDc, oldDst);
            DeleteDC(srcDc);
            DeleteDC(dstDc);
            ReleaseDC(nullptr, screenDc);
        }
        LayoutTemplateDest();
        return RunLoop();
    }

private:
    static bool classRegistered_;
    static constexpr const wchar_t* kClassName = L"QstDragPickOverlay";

    static void RegisterWindowClass() {
        if (classRegistered_) return;
        WNDCLASSW wc{};
        wc.lpfnWndProc = &DragPickOverlay::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
        classRegistered_ = true;
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        DragPickOverlay* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<DragPickOverlay*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        self = reinterpret_cast<DragPickOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    void CleanupBitmaps() {
        if (screenBitmap_) {
            DeleteObject(screenBitmap_);
            screenBitmap_ = nullptr;
        }
        if (dimOverlay_) {
            DeleteObject(dimOverlay_);
            dimOverlay_ = nullptr;
        }
        if (templateBmp_) {
            DeleteObject(templateBmp_);
            templateBmp_ = nullptr;
        }
    }

    void CaptureVirtualScreenSizeOnly() {
        GetVirtualScreenRect(screenX_, screenY_, screenW_, screenH_);
        HDC screenDc = GetDC(nullptr);
        HDC memDc = CreateCompatibleDC(screenDc);
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

    void CaptureVirtualScreen() {
        CleanupBitmaps();
        GetVirtualScreenRect(screenX_, screenY_, screenW_, screenH_);
        HDC screenDc = GetDC(nullptr);
        HDC memDc = CreateCompatibleDC(screenDc);
        screenBitmap_ = CreateCompatibleBitmap(screenDc, screenW_, screenH_);
        HGDIOBJ oldBmp = SelectObject(memDc, screenBitmap_);
        BitBlt(memDc, 0, 0, screenW_, screenH_, screenDc, screenX_, screenY_, SRCCOPY | CAPTUREBLT);
        SelectObject(memDc, oldBmp);
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

    void LayoutTemplateDest() {
        dest_ = RECT{0, 0, 0, 0};
        if (imgW_ <= 0 || imgH_ <= 0 || screenW_ <= 0 || screenH_ <= 0) return;
        const int maxW = static_cast<int>(screenW_ * 0.82);
        const int maxH = static_cast<int>(screenH_ * 0.82);
        double scale = 1.0;
        if (imgW_ > maxW) scale = (std::min)(scale, maxW / static_cast<double>(imgW_));
        if (imgH_ > maxH) scale = (std::min)(scale, maxH / static_cast<double>(imgH_));
        const int dw = (std::max)(1, static_cast<int>(std::lround(imgW_ * scale)));
        const int dh = (std::max)(1, static_cast<int>(std::lround(imgH_ * scale)));
        dest_.left = (screenW_ - dw) / 2;
        dest_.top = (screenH_ - dh) / 2;
        dest_.right = dest_.left + dw;
        dest_.bottom = dest_.top + dh;
    }

    DragPickOutcome RunLoop() {
        outcome_ = {};
        dragging_ = false;
        RegisterWindowClass();
        hwnd_ = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            kClassName, L"", WS_POPUP,
            screenX_, screenY_, screenW_, screenH_,
            nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd_) return outcome_;
        ShowWindow(hwnd_, SW_SHOW);
        SetFocus(hwnd_);
        SetCapture(hwnd_);
        MSG msg{};
        BOOL bRet;
        while ((bRet = GetMessage(&msg, nullptr, 0, 0)) != 0) {
            if (bRet == -1) break;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        return outcome_;
    }

    void Finish(bool ok) {
        if (!ok) {
            outcome_.ok = false;
            outcome_.cancelled = true;
        }
        ReleaseCapture();
        if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
        PostQuitMessage(0);
    }

    bool ClientToImageOffset(int cx, int cy, int& ox, int& oy) const {
        if (dest_.right <= dest_.left || dest_.bottom <= dest_.top) return false;
        const int dw = (std::max)(1, static_cast<int>(dest_.right - dest_.left));
        const int dh = (std::max)(1, static_cast<int>(dest_.bottom - dest_.top));
        int x = cx;
        int y = cy;
        if (x < dest_.left) x = dest_.left;
        if (x > dest_.right - 1) x = dest_.right - 1;
        if (y < dest_.top) y = dest_.top;
        if (y > dest_.bottom - 1) y = dest_.bottom - 1;
        const double nx = (x - dest_.left) / static_cast<double>(dw);
        const double ny = (y - dest_.top) / static_cast<double>(dh);
        const int imgX = static_cast<int>(std::lround(nx * (imgW_ - 1)));
        const int imgY = static_cast<int>(std::lround(ny * (imgH_ - 1)));
        ox = imgX - imgW_ / 2;
        oy = imgY - imgH_ / 2;
        return true;
    }

    void CommitDrag() {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        double sec = 0.0;
        if (qpcFreq_.QuadPart > 0) {
            sec = static_cast<double>(now.QuadPart - qpcStart_.QuadPart)
                / static_cast<double>(qpcFreq_.QuadPart);
        }
        if (mode_ == Mode::Screen) {
            outcome_.x1 = dragStart_.x + screenX_;
            outcome_.y1 = dragStart_.y + screenY_;
            outcome_.x2 = dragEnd_.x + screenX_;
            outcome_.y2 = dragEnd_.y + screenY_;
        } else {
            int ox1 = 0, oy1 = 0, ox2 = 0, oy2 = 0;
            if (!ClientToImageOffset(dragStart_.x, dragStart_.y, ox1, oy1)
                || !ClientToImageOffset(dragEnd_.x, dragEnd_.y, ox2, oy2)) {
                Finish(false);
                return;
            }
            outcome_.x1 = ox1;
            outcome_.y1 = oy1;
            outcome_.x2 = ox2;
            outcome_.y2 = oy2;
        }
        outcome_.durationSec = sec;
        outcome_.ok = true;
        outcome_.cancelled = false;
        Finish(true);
    }

    void Paint(HDC hdc) {
        HDC memDc = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, screenW_, screenH_);
        HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

        if (mode_ == Mode::Screen && screenBitmap_) {
            HDC srcDc = CreateCompatibleDC(memDc);
            HGDIOBJ oldSrc = SelectObject(srcDc, screenBitmap_);
            BitBlt(memDc, 0, 0, screenW_, screenH_, srcDc, 0, 0, SRCCOPY);
            SelectObject(srcDc, oldSrc);
            DeleteDC(srcDc);
        } else {
            RECT full{0, 0, screenW_, screenH_};
            HBRUSH bg = CreateSolidBrush(RGB(24, 24, 24));
            FillRect(memDc, &full, bg);
            DeleteObject(bg);
        }

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

        if (mode_ == Mode::Template && templateBmp_ && dest_.right > dest_.left) {
            HDC srcDc = CreateCompatibleDC(memDc);
            HGDIOBJ oldSrc = SelectObject(srcDc, templateBmp_);
            SetStretchBltMode(memDc, HALFTONE);
            StretchBlt(memDc, dest_.left, dest_.top, dest_.right - dest_.left, dest_.bottom - dest_.top,
                srcDc, 0, 0, imgW_, imgH_, SRCCOPY);
            SelectObject(srcDc, oldSrc);
            DeleteDC(srcDc);
            HPEN frame = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
            HGDIOBJ oldPen = SelectObject(memDc, frame);
            HGDIOBJ oldBr = SelectObject(memDc, GetStockObject(NULL_BRUSH));
            Rectangle(memDc, dest_.left, dest_.top, dest_.right, dest_.bottom);
            SelectObject(memDc, oldPen);
            SelectObject(memDc, oldBr);
            DeleteObject(frame);
        }

        if (dragging_) {
            HPEN pen = CreatePen(PS_SOLID, 3, kLineColor);
            HGDIOBJ oldPen = SelectObject(memDc, pen);
            MoveToEx(memDc, dragStart_.x, dragStart_.y, nullptr);
            LineTo(memDc, dragEnd_.x, dragEnd_.y);
            SelectObject(memDc, oldPen);
            DeleteObject(pen);
            const int r = 4;
            HBRUSH dot = CreateSolidBrush(kLineColor);
            RECT a{dragStart_.x - r, dragStart_.y - r, dragStart_.x + r, dragStart_.y + r};
            RECT b{dragEnd_.x - r, dragEnd_.y - r, dragEnd_.x + r, dragEnd_.y + r};
            FillRect(memDc, &a, dot);
            FillRect(memDc, &b, dot);
            DeleteObject(dot);
        }

        const wchar_t* hint = mode_ == Mode::Template
            ? L"在图片上拖拽设定起点和终点（Esc 取消）"
            : L"在屏幕上拖拽设定起点、终点和时长（Esc 取消）";
        SetBkMode(memDc, TRANSPARENT);
        SetTextColor(memDc, RGB(255, 255, 255));
        HFONT font = CreateFontW(UiFontHeight(18), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
        HGDIOBJ oldFont = SelectObject(memDc, font);
        RECT hintRc{16, 16, screenW_ - 16, 48};
        DrawTextW(memDc, hint, -1, &hintRc, DT_LEFT | DT_SINGLELINE);
        SelectObject(memDc, oldFont);
        DeleteObject(font);

        BitBlt(hdc, 0, 0, screenW_, screenH_, memDc, 0, 0, SRCCOPY);
        SelectObject(memDc, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDc);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
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
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        case WM_LBUTTONDOWN: {
            dragging_ = true;
            dragStart_ = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            dragEnd_ = dragStart_;
            QueryPerformanceFrequency(&qpcFreq_);
            QueryPerformanceCounter(&qpcStart_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (dragging_ && (wp & MK_LBUTTON)) {
                dragEnd_ = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (dragging_) {
                dragEnd_ = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                dragging_ = false;
                CommitDrag();
            }
            return 0;
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_KEYDOWN:
            if (msg == WM_KEYDOWN && wp != VK_ESCAPE) return 0;
            Finish(false);
            return 0;
        default:
            return DefWindowProcW(hwnd_, msg, wp, lp);
        }
    }

    HWND hwnd_ = nullptr;
    Mode mode_ = Mode::Screen;
    bool dragging_ = false;
    POINT dragStart_{}, dragEnd_{};
    HBITMAP screenBitmap_ = nullptr;
    HBITMAP dimOverlay_ = nullptr;
    HBITMAP templateBmp_ = nullptr;
    int screenW_ = 0, screenH_ = 0, screenX_ = 0, screenY_ = 0;
    int imgW_ = 0, imgH_ = 0;
    RECT dest_{};
    LARGE_INTEGER qpcFreq_{}, qpcStart_{};
    DragPickOutcome outcome_{};
};

bool DragPickOverlay::classRegistered_ = false;

}  // namespace

DragPickOutcome ShowScreenDragPickOverlay() {
    DragPickOverlay overlay;
    return overlay.RunScreen();
}

DragPickOutcome ShowTemplateDragPickOverlay(HBITMAP templateBmp, int imgW, int imgH) {
    DragPickOverlay overlay;
    return overlay.RunTemplate(templateBmp, imgW, imgH);
}
