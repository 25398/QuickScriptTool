#include "window_mode_preview.h"

#include "window_capture.h"

namespace windowmode {

namespace {

constexpr wchar_t kPreviewClass[] = L"QuickScriptWindowModePreview";

void RegisterPreviewClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowModePreview::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kPreviewClass;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    registered = true;
}

}  // namespace

LRESULT CALLBACK WindowModePreview::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        auto* self = reinterpret_cast<WindowModePreview*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && self->previewBmp_) {
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(mem, self->previewBmp_);
            BITMAP bm{};
            GetObject(self->previewBmp_, sizeof(bm), &bm);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const int dstW = rc.right - rc.left;
            const int dstH = rc.bottom - rc.top;
            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc, 0, 0, dstW, dstH, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        } else {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
            DrawTextW(hdc, L"无预览", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_DESTROY) return 0;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void WindowModePreview::Bind(HWND owner, HFONT font) {
    owner_ = owner;
    font_ = font;
}

void WindowModePreview::Show(int x, int y, int w, int h) {
    RegisterPreviewClass();
    if (!hwnd_) {
        hwnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW, kPreviewClass, L"窗口模式预览",
            WS_POPUP | WS_BORDER | WS_VISIBLE,
            x, y, w, h, owner_, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        if (font_) SendMessageW(hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    } else {
        SetWindowPos(hwnd_, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    }
}

void WindowModePreview::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void WindowModePreview::Destroy() {
    if (previewBmp_) {
        DeleteObject(previewBmp_);
        previewBmp_ = nullptr;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void WindowModePreview::Refresh(HWND targetHwnd) {
    if (!hwnd_ || !IsWindowVisible(hwnd_)) return;
    if (previewBmp_) {
        DeleteObject(previewBmp_);
        previewBmp_ = nullptr;
    }
    if (!targetHwnd || !IsWindow(targetHwnd)) {
        InvalidateRect(hwnd_, nullptr, TRUE);
        return;
    }
    WindowCaptureResult capture = CaptureWindowClient(targetHwnd);
    previewBmp_ = capture.bitmap;
    InvalidateRect(hwnd_, nullptr, TRUE);
}

bool WindowModePreview::IsVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

}  // namespace windowmode
