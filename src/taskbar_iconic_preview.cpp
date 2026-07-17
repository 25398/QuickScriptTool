#include "taskbar_window.h"

#include <dwmapi.h>

namespace {

void ApplyExStyle(HWND hwnd, LONG_PTR ex) {
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

}  // namespace

ScopedCoveredWindowThumbnail::ScopedCoveredWindowThumbnail(HWND hwnd)
    : hwnd_(hwnd) {
    if (!hwnd_ || !IsWindow(hwnd_)) {
        hwnd_ = nullptr;
        return;
    }

    // 参考常见 Win32 / PowerToys 做法：完全被同尺寸窗口盖住时，不要对下层窗口使用
    // DWMWA_FORCE_ICONIC_REPRESENTATION（Win11 Alt+Tab / 任务栏易黑屏或一直转圈），
    // 而是暂时移出 Alt+Tab 与任务栏缩略图组；上层窗口继续用系统实时预览。
    savedExStyle_ = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    LONG_PTR ex = savedExStyle_;
    ex |= WS_EX_TOOLWINDOW;
    ex &= ~WS_EX_APPWINDOW;
    ApplyExStyle(hwnd_, ex);

    // 冻结当前 DWM 画面，避免短暂仍被采样时采到上层窗口。
    BOOL freeze = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_FREEZE_REPRESENTATION, &freeze, sizeof(freeze));

    BOOL disallowPeek = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_DISALLOW_PEEK, &disallowPeek, sizeof(disallowPeek));

    active_ = true;
}

ScopedCoveredWindowThumbnail::~ScopedCoveredWindowThumbnail() {
    if (!hwnd_ || !active_ || !IsWindow(hwnd_)) return;

    BOOL disallowPeek = FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_DISALLOW_PEEK, &disallowPeek, sizeof(disallowPeek));

    BOOL freeze = FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_FREEZE_REPRESENTATION, &freeze, sizeof(freeze));

    ApplyExStyle(hwnd_, savedExStyle_);
}
