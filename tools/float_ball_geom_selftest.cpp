// =============================================================================
// FloatBallGeomSelfTest — 桌面悬浮球几何 / 隐藏判据（纯逻辑，无 HWND）
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   build\Release\FloatBallGeomSelfTest.exe --json
//
// 回归焦点：**非全屏页面不得隐藏悬浮球**。
//
// 历史 bug（本用例要钉死的行为）：旧判据只看「窗口矩形 ≥ 显示器 95%」，
// 于是 1920x1080 屏上最大化窗口（1920x1040，任务栏 40px ⇒ 高度 96.3%）
// 也被当成全屏 ⇒ 用户切到任何最大化的普通窗口，悬浮球就直接消失。
//
// 现口径（对齐 360 悬浮球一类桌面伴侣）：
//   ① 必须**完全覆盖显示器（含任务栏区域）**
//   ② 带标题栏 + 可调整边框的普通窗口一律不算全屏（排除最大化窗口、
//      UWP 宿主 ApplicationFrameWindow —— 后者 GetWindowRect 就是整屏）
// =============================================================================
#include "selftest_harness.h"

#include "desktop_tools/float_ball_geom.h"

#include <string>

namespace {

using selftest::Emit;
using qst::desktop_tools::FloatBallForegroundGeometry;
using qst::desktop_tools::FloatBallForegroundIsFullscreen;
using qst::desktop_tools::FloatBallWindowCoversMonitor;

constexpr int kScreenW = 1920;
constexpr int kScreenH = 1080;
constexpr int kTaskbar = 40;

FloatBallForegroundGeometry Geo(int wl, int wt, int wr, int wb,
    bool caption = false, bool thick = false, bool iconic = false) {
    FloatBallForegroundGeometry g{};
    g.monitor = RECT{0, 0, kScreenW, kScreenH};
    g.window = RECT{wl, wt, wr, wb};
    g.hasCaption = caption;
    g.thickFrame = thick;
    g.iconic = iconic;
    return g;
}

std::wstring Detail(bool cover, bool fs) {
    return std::wstring(L"covers=") + (cover ? L"1" : L"0")
        + L" fullscreen=" + (fs ? L"1" : L"0");
}

const selftest::CaseInfo kCases[] = {
    {L"maximized_window_not_fullscreen", L"default",
        L"最大化窗口(铺到工作区、露出任务栏)不得判为全屏 —— 历史 bug 回归"},
    {L"framed_full_rect_not_fullscreen", L"default",
        L"矩形铺满整屏但带标题栏+边框(最大化/任务栏自动隐藏/UWP 宿主)不算全屏"},
    {L"f11_fullscreen_hides", L"default",
        L"无标题栏无边框铺满整屏(F11/全屏视频)必须判全屏并隐藏"},
    {L"borderless_windowed_fullscreen_hides", L"default",
        L"无边框窗口化全屏(无 CAPTION)必须判全屏"},
    {L"caption_only_still_fullscreen", L"default",
        L"有 CAPTION 但无 THICKFRAME 的全屏播放器仍判全屏"},
    {L"minimized_not_fullscreen", L"default",
        L"最小化窗口不得判全屏"},
    {L"small_window_not_fullscreen", L"default",
        L"普通小窗口不得判全屏"},
    {L"one_pixel_tolerance_covers", L"default",
        L"比整屏小 1px 仍在容差内(吸收 DWM 取整)"},
    {L"two_pixel_gap_does_not_cover", L"default",
        L"比整屏小 2px 视为未覆盖"},
    {L"side_taskbar_not_covered", L"default",
        L"竖排任务栏(右侧留边)不得判全屏"},
    {L"degenerate_monitor_not_fullscreen", L"default",
        L"显示器矩形非法(0 尺寸)不得判全屏"},
    {L"secondary_monitor_fullscreen", L"default",
        L"副屏上的全屏窗口按该屏 rcMonitor 判定"},
    {L"hidden_window_still_uses_rect", L"default",
        L"判据只依赖矩形/样式，与窗口可见性解耦(调用方负责过滤)"},
};

void CaseMaximizedNotFullscreen() {
    // 1920x1080 屏 + 40px 任务栏 ⇒ 最大化窗口 1920x1040（旧口径 96.3% ≥ 95% 误判）
    const auto g = Geo(0, 0, kScreenW, kScreenH - kTaskbar, true, true);
    const bool cover = FloatBallWindowCoversMonitor(g);
    const bool fs = FloatBallForegroundIsFullscreen(g);
    Emit(L"maximized_window_not_fullscreen", !cover && !fs, Detail(cover, fs).c_str());
}

void CaseFramedFullRectNotFullscreen() {
    // 任务栏自动隐藏时最大化窗口就是 1920x1080；UWP 宿主也是整屏 rect。
    // 两者都带 WS_CAPTION|WS_THICKFRAME ⇒ 必须靠样式排除。
    const auto g = Geo(0, 0, kScreenW, kScreenH, true, true);
    const bool cover = FloatBallWindowCoversMonitor(g);
    const bool fs = FloatBallForegroundIsFullscreen(g);
    Emit(L"framed_full_rect_not_fullscreen", cover && !fs, Detail(cover, fs).c_str());
}

void CaseF11FullscreenHides() {
    const auto g = Geo(0, 0, kScreenW, kScreenH, false, false);
    const bool cover = FloatBallWindowCoversMonitor(g);
    const bool fs = FloatBallForegroundIsFullscreen(g);
    Emit(L"f11_fullscreen_hides", cover && fs, Detail(cover, fs).c_str());
}

void CaseBorderlessWindowedFullscreenHides() {
    const auto g = Geo(0, 0, kScreenW, kScreenH, false, true);
    const bool fs = FloatBallForegroundIsFullscreen(g);
    Emit(L"borderless_windowed_fullscreen_hides", fs,
        Detail(FloatBallWindowCoversMonitor(g), fs).c_str());
}

void CaseCaptionOnlyStillFullscreen() {
    // 部分播放器全屏后仍保留 WS_CAPTION（无 THICKFRAME）——仍应隐藏
    const auto g = Geo(0, 0, kScreenW, kScreenH, true, false);
    const bool fs = FloatBallForegroundIsFullscreen(g);
    Emit(L"caption_only_still_fullscreen", fs,
        Detail(FloatBallWindowCoversMonitor(g), fs).c_str());
}

void CaseMinimizedNotFullscreen() {
    const auto g = Geo(0, 0, kScreenW, kScreenH, false, false, true);
    const bool cover = FloatBallWindowCoversMonitor(g);
    const bool fs = FloatBallForegroundIsFullscreen(g);
    Emit(L"minimized_not_fullscreen", !cover && !fs, Detail(cover, fs).c_str());
}

void CaseSmallWindowNotFullscreen() {
    const auto g = Geo(100, 120, 900, 700, true, true);
    const bool cover = FloatBallWindowCoversMonitor(g);
    const bool fs = FloatBallForegroundIsFullscreen(g);
    Emit(L"small_window_not_fullscreen", !cover && !fs, Detail(cover, fs).c_str());
}

void CaseOnePixelToleranceCovers() {
    const auto g = Geo(1, 1, kScreenW - 1, kScreenH - 1, false, false);
    const bool cover = FloatBallWindowCoversMonitor(g);
    const bool fs = FloatBallForegroundIsFullscreen(g);
    Emit(L"one_pixel_tolerance_covers", cover && fs, Detail(cover, fs).c_str());
}

void CaseTwoPixelGapDoesNotCover() {
    const auto g = Geo(2, 2, kScreenW - 2, kScreenH - 2, false, false);
    const bool cover = FloatBallWindowCoversMonitor(g);
    const bool fs = FloatBallForegroundIsFullscreen(g);
    Emit(L"two_pixel_gap_does_not_cover", !cover && !fs, Detail(cover, fs).c_str());
}

void CaseSideTaskbarNotCovered() {
    const auto g = Geo(0, 0, kScreenW - 80, kScreenH, false, false);
    const bool cover = FloatBallWindowCoversMonitor(g);
    const bool fs = FloatBallForegroundIsFullscreen(g);
    Emit(L"side_taskbar_not_covered", !cover && !fs, Detail(cover, fs).c_str());
}

void CaseDegenerateMonitorNotFullscreen() {
    auto g = Geo(0, 0, kScreenW, kScreenH, false, false);
    g.monitor = RECT{0, 0, 0, 0};
    const bool cover = FloatBallWindowCoversMonitor(g);
    const bool fs = FloatBallForegroundIsFullscreen(g);
    Emit(L"degenerate_monitor_not_fullscreen", !cover && !fs, Detail(cover, fs).c_str());
}

void CaseSecondaryMonitorFullscreen() {
    auto g = Geo(0, 0, kScreenW, kScreenH, false, false);
    g.monitor = RECT{kScreenW, 0, kScreenW * 2, kScreenH};
    g.window = RECT{kScreenW, 0, kScreenW * 2, kScreenH};
    const bool cover = FloatBallWindowCoversMonitor(g);
    const bool fs = FloatBallForegroundIsFullscreen(g);
    Emit(L"secondary_monitor_fullscreen", cover && fs, Detail(cover, fs).c_str());
}

void CaseHiddenWindowStillUsesRect() {
    // 判据不读窗口可见性：调用方已排除自身进程/桌面/最小化。
    // 这里只确认几何函数对同一输入是纯函数（两次调用同结果）。
    const auto g = Geo(0, 0, kScreenW, kScreenH - kTaskbar, true, true);
    const bool a = FloatBallForegroundIsFullscreen(g);
    const bool b = FloatBallForegroundIsFullscreen(g);
    Emit(L"hidden_window_still_uses_rect", a == b && !a,
        Detail(FloatBallWindowCoversMonitor(g), a).c_str());
}

void PrintHelp() {
    std::fwprintf(stdout,
        L"FloatBallGeomSelfTest — 悬浮球全屏隐藏判据（非全屏页面不得隐藏）\n"
        L"\n"
        L"用法:\n"
        L"  FloatBallGeomSelfTest.exe [--json] [--list] [--help]\n"
        L"\n"
        L"Agent: 见 .cursor/skills/module-selftest/SKILL.md\n"
        L"  源码: src/desktop_tools/float_ball_geom.h, src/desktop_tools/float_ball.cpp\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i] ? argv[i] : L"";
        if (a == L"--json") {
            selftest::gJson = true;
            selftest::InitUtf8Stdout();
        } else if (a == L"--list") {
            listOnly = true;
            selftest::InitUtf8Stdout();
        } else if (a == L"--help" || a == L"-h") {
            PrintHelp();
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"FloatBallGeomSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    CaseMaximizedNotFullscreen();
    CaseFramedFullRectNotFullscreen();
    CaseF11FullscreenHides();
    CaseBorderlessWindowedFullscreenHides();
    CaseCaptionOnlyStillFullscreen();
    CaseMinimizedNotFullscreen();
    CaseSmallWindowNotFullscreen();
    CaseOnePixelToleranceCovers();
    CaseTwoPixelGapDoesNotCover();
    CaseSideTaskbarNotCovered();
    CaseDegenerateMonitorNotFullscreen();
    CaseSecondaryMonitorFullscreen();
    CaseHiddenWindowStillUsesRect();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
