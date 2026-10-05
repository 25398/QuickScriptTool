#include "window_mode_log.h"

#include "virtual_desktop_accessor.h"
#include "window_target.h"

#include <cstdarg>
#include <mutex>

namespace windowmode {

namespace {

/// ★★ 变参格式化到**动态缓冲**（2026-09-25）。
///
/// ⚠⚠ 为什么不能用 `wchar_t buf[1024]{} + vswprintf_s(buf, fmt, args)`：
///   `vswprintf_s` 在**截断**时返回 -1 并调用 **invalid parameter handler**，
///   而默认 handler 会**直接终止进程** —— 也就是说
///   **一条过长的日志就能让软件静默闪退**（没有日志、没有退出码，极难定位）。
///   本仓的日志里会拼入网页返回的错误消息、URL、路径，长度不受我们控制 ⇒ 必须动态。
std::wstring FormatVarArgs(const wchar_t* fmt, va_list args) {
    va_list probe;
    va_copy(probe, args);
    // 先量需要多少（_vscwprintf 不写缓冲，不会触发 invalid parameter handler）
    const int need = _vscwprintf(fmt, probe);
    va_end(probe);
    if (need < 0) return std::wstring(fmt);   // 格式串本身有问题：原样返回，绝不终止进程
    std::wstring out(static_cast<size_t>(need) + 1, L'\0');
    va_list once;
    va_copy(once, args);
    const int wrote = _vsnwprintf_s(out.data(), out.size(), _TRUNCATE, fmt, once);
    va_end(once);
    if (wrote < 0) out.resize(out.size() - 1);   // 理论到不了；保险
    else out.resize(static_cast<size_t>(wrote));
    return out;
}

}  // namespace


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
    va_list args;
    va_start(args, fmt);
    const std::wstring line = FormatVarArgs(fmt, args);
    va_end(args);
    Emit(line);
}

void WindowModeLogVerbose(const std::wstring& line) {
    OutputDebugStringW(line.c_str());
    OutputDebugStringW(L"\n");
}

void WindowModeLogVerbosef(const wchar_t* fmt, ...) {
    if (!fmt) return;
    va_list args;
    va_start(args, fmt);
    const std::wstring line = FormatVarArgs(fmt, args);
    va_end(args);
    WindowModeLogVerbose(line);
}

namespace {

void AppendPersistentEvent(const std::wstring& line) {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash - path), L"window_mode_debug.log");

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path, L"a, ccs=UTF-8") != 0 || !fp) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    fwprintf(fp, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] %s\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        line.c_str());
    fclose(fp);
}

}  // namespace

void WindowModeLogEvent(const std::wstring& line) {
    Emit(line);
    AppendPersistentEvent(line);
}

void WindowModeLogEventf(const wchar_t* fmt, ...) {
    if (!fmt) return;
    va_list args;
    va_start(args, fmt);
    const std::wstring line = FormatVarArgs(fmt, args);
    va_end(args);
    WindowModeLogEvent(line);
}

void WindowModeLogDesktopSnap(const wchar_t* tag, HWND hwnd) {
    HWND root = TopLevelTargetWindow(hwnd);
    if (!root) root = hwnd;
    if (!root) {
        WindowModeLogf(L"[窗口/后台窗口模式] %s (无效 HWND)", tag ? tag : L"?");
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
        L"[窗口/后台窗口模式] %s UserDesk=%d TargetDesk=%d(%s) OnCurrentVD=%d Iconic=%d showCmd=%u Layered=%d FgDesk=%d hwnd=0x%p",
        tag ? tag : L"snap",
        userDesk, targetDesk,
        deskName.empty() ? L"?" : deskName.c_str(),
        onCurrent, IsIconic(root) ? 1 : 0, showCmd, layered ? 1 : 0, fgDesk, root);
}

}  // namespace windowmode
