// =============================================================================
// FloatBallProbe — 悬浮球全屏判据「实机探针」（非自检套件，需桌面会话）
// =============================================================================
// 为什么单独做一个 exe：判据分两层 —— 纯几何在 FloatBallGeomSelfTest 里已用
// 13 个用例钉死（CI 可跑），但**真实桌面上的窗口样式/多屏/任务栏组合**只有实机
// 才能看到。本探针与产品共用 `FloatBallIsForegroundFullscreen()`（float_ball_geom.h），
// 所以它打印的就是悬浮球**此刻**的行为，不是另写一套逻辑。
//
// 用法:
//   FloatBallProbe.exe            # 打印当前前台窗口的判定
//   FloatBallProbe.exe --watch    # 每 500ms 打印一次（切窗口时观察变化）
//
// 注意：本工具**不在** `$LogicSuites` / `$InteractiveSuites` / CI 清单里
// （它需要真实桌面会话，输出是给人看的诊断，不是断言）。
// =============================================================================
#include "desktop_tools/float_ball_geom.h"

#include <cstdio>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using qst::desktop_tools::FloatBallIsForegroundFullscreen;
using qst::desktop_tools::FloatBallQueryNotificationState;

namespace {

std::wstring WindowTitle(HWND h) {
    wchar_t buf[256]{};
    if (h) GetWindowTextW(h, buf, 256);
    return buf;
}

std::wstring ClassName(HWND h) {
    wchar_t buf[128]{};
    if (h) GetClassNameW(h, buf, 128);
    return buf;
}

// 2026-09-23 之前的旧口径，仅为对照展示「旧口径为什么会误隐藏」。
bool LegacyFullscreen() {
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindow(fg)) return false;
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    RECT wr{};
    if (!GetWindowRect(fg, &wr)) return false;
    const int mw = mi.rcMonitor.right - mi.rcMonitor.left;
    const int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    const int ww = wr.right - wr.left;
    const int wh = wr.bottom - wr.top;
    return mw > 0 && mh > 0 && ww >= mw * 95 / 100 && wh >= mh * 95 / 100;
}

const wchar_t* QunsName(int st) {
    switch (st) {
    case 1: return L"QUNS_NOT_PRESENT(用户不在)";
    case 2: return L"QUNS_BUSY(全屏应用运行中)";
    case 3: return L"QUNS_RUNNING_D3D_FULL_SCREEN(独占全屏)";
    case 4: return L"QUNS_PRESENTATION_MODE(演示模式)";
    case 5: return L"QUNS_ACCEPTS_NOTIFICATIONS(正常)";
    case 6: return L"QUNS_QUIET_TIME(免打扰)";
    case 7: return L"QUNS_APP(应用模式)";
    default: return L"<取不到>";
    }
}

void Dump() {
    HWND fg = GetForegroundWindow();
    std::fwprintf(stdout, L"前台  HWND=0x%llX  类=%-30s 标题=%s\n",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(fg)),
        ClassName(fg).c_str(), WindowTitle(fg).c_str());

    if (fg && IsWindow(fg)) {
        RECT wr{};
        GetWindowRect(fg, &wr);
        HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        const bool gotMon = GetMonitorInfoW(mon, &mi) != 0;
        const LONG style = GetWindowLongW(fg, GWL_STYLE);
        std::fwprintf(stdout,
            L"      窗口=(%ld,%ld)-(%ld,%ld) %ldx%ld   屏幕=(%ld,%ld)-(%ld,%ld) %ldx%ld\n",
            wr.left, wr.top, wr.right, wr.bottom, wr.right - wr.left, wr.bottom - wr.top,
            mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
            mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
        std::fwprintf(stdout,
            L"      样式 CAPTION=%d THICKFRAME=%d | 最小化=%d | 屏幕信息=%d\n",
            (style & WS_CAPTION) == WS_CAPTION, (style & WS_THICKFRAME) != 0,
            IsIconic(fg) != 0, gotMon);
    }

    const int quns = FloatBallQueryNotificationState();
    std::fwprintf(stdout, L"      %s\n", QunsName(quns));

    const bool now = FloatBallIsForegroundFullscreen();
    const bool old = LegacyFullscreen();
    std::fwprintf(stdout, L"  => 新判据=%s   旧判据(>=95%%)=%s   ⇒ 悬浮球应%s\n",
        now ? L"全屏" : L"非全屏", old ? L"全屏" : L"非全屏", now ? L"隐藏" : L"显示");
    if (now != old) {
        std::fwprintf(stdout,
            L"     ** 新旧口径不一致：旧口径会把这里误判成全屏并隐藏悬浮球 **\n");
    }
    std::fflush(stdout);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
#ifdef _WIN32
    // 与 selftest harness 同款：宽字符输出走 UTF-8，否则管道下中文按 ACP 丢字。
    _setmode(_fileno(stdout), _O_U8TEXT);
#endif
    bool watch = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i] ? argv[i] : L"";
        if (a == L"--watch") {
            watch = true;
        } else if (a == L"--help" || a == L"-h") {
            std::fwprintf(stdout,
                L"FloatBallProbe — 悬浮球全屏判据实机探针\n"
                L"  FloatBallProbe.exe           打印当前前台窗口的判定\n"
                L"  FloatBallProbe.exe --watch   每 500ms 打印一次\n");
            return 0;
        }
    }
    if (!watch) {
        Dump();
        return 0;
    }
    for (;;) {
        Dump();
        std::fwprintf(stdout, L"----\n");
        std::fflush(stdout);
        Sleep(500);
    }
}
