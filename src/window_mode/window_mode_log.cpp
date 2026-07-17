#include "window_mode_log.h"

#include "virtual_desktop_accessor.h"
#include "window_target.h"

#include <cstdarg>
#include <mutex>

namespace windowmode {

namespace {

std::mutex gLogMutex;
WindowModeLogSink gSink;

void Emit(const std::wstring& line) {
    OutputDebugStringW(line.c_str());
    OutputDebugStringW(L"\n");
    std::lock_guard<std::mutex> lock(gLogMutex);
    if (gSink) gSink(line);
}

}  // namespace

void SetWindowModeLogSink(WindowModeLogSink sink) {
    std::lock_guard<std::mutex> lock(gLogMutex);
    gSink = std::move(sink);
}

void WindowModeLog(const std::wstring& line) {
    Emit(line);
}

void WindowModeLog(const wchar_t* line) {
    if (!line) return;
    Emit(line);
}

void WindowModeLogf(const wchar_t* fmt, ...) {
    if (!fmt) return;
    wchar_t buf[1024]{};
    va_list args;
    va_start(args, fmt);
    vswprintf_s(buf, fmt, args);
    va_end(args);
    Emit(buf);
}

void WindowModeLogDesktopSnap(const wchar_t* tag, HWND hwnd) {
    HWND root = TopLevelTargetWindow(hwnd);
    if (!root) root = hwnd;
    if (!root) {
        WindowModeLogf(L"[窗口模式] %s (无效 HWND)", tag ? tag : L"?");
        return;
    }

    auto& vda = VirtualDesktopAccessor::Instance();
    std::wstring err;
    vda.EnsureLoaded(err);

    const int userDesk = vda.GetCurrentDesktopNumber();
    const int targetDesk = vda.GetWindowDesktopNumber(root);
    const int onCurrent = vda.IsWindowOnCurrentVirtualDesktop(root);
    std::wstring deskName;
    if (targetDesk >= 0) deskName = vda.GetDesktopName(targetDesk);

    UINT showCmd = 0;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(root, &wp)) showCmd = wp.showCmd;

    const LONG exStyle = static_cast<LONG>(GetWindowLongPtr(root, GWL_EXSTYLE));
    const bool layered = (exStyle & WS_EX_LAYERED) != 0;

    HWND fg = GetForegroundWindow();
    int fgDesk = -1;
    if (fg && IsWindow(fg)) {
        fgDesk = vda.GetWindowDesktopNumber(fg);
    }

    WindowModeLogf(
        L"[窗口模式] %s UserDesk=%d TargetDesk=%d(%s) OnCurrentVD=%d Iconic=%d showCmd=%u Layered=%d FgDesk=%d hwnd=0x%p",
        tag ? tag : L"snap",
        userDesk, targetDesk,
        deskName.empty() ? L"?" : deskName.c_str(),
        onCurrent, IsIconic(root) ? 1 : 0, showCmd, layered ? 1 : 0, fgDesk, root);
}

}  // namespace windowmode
