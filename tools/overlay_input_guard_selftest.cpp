// =============================================================================
// OverlayInputGuardSelfTest — 「我们扣住了用户鼠标」的兜底判据（纯逻辑 + 消息队列）
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   build\Release\OverlayInputGuardSelfTest.exe --json
//
// 回归焦点（用户报障原文）：
//   「用 QQ 远程协助他人的电脑使用我们这个软件时，如果我们这个软件在对方电脑的最上层，
//     那我会失去对方电脑的控制权（只能看到屏幕变化，但无法操作对方的屏幕了）。」
//
// 事故链：选区 / 找图 / OCR / 拖拽取点 / 拖动准星 这些叠层都是同一个套路 ——
//   `SetCapture(自己)` + 把窗口挪到屏外 + **阻塞的模态消息循环**，只在收到
//   「按钮抬起 / Esc」时才退出。终止事件一旦丢了（远控走 SendInput，注入的抬起本来就可能丢），
//   叠层就永远停在「等 UP」：鼠标捕获被我们扣着（而窗口在屏外）⇒ 用户**看得见屏幕、点不动**。
//   本地还能按 Esc 自救，**远控下几乎没有别的入口** ⇒ 用户体感「失去控制权」。
//
// 本用例钉死的三条：
//   ① 判据不许误伤正常拖拽（还按着键 ⇒ 一律不动手）
//   ② 判据不许漏判「抬起丢了」（没键按下 + 已停手 + 捕获在我们手上 ⇒ 必须收尾）
//   ③ 兜底通道必须真能**超时醒来** —— 用裸 GetMessage 的循环根本醒不过来，
//      那样判据写得再对也是死的（wait_* 三条就是在钉这个前提）
// =============================================================================
#include "selftest_harness.h"

#include "overlay_input_guard.h"

#include <string>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"no_capture_never_aborts", L"default",
        L"没扣着鼠标捕获时一律不动手(哪怕空闲 10s 且无键按下)"},
    {L"holding_button_never_aborts", L"default",
        L"还按着鼠标键 ⇒ 正常拖拽，绝不许收尾(哪怕空闲 10s)"},
    {L"lost_button_up_aborts", L"default",
        L"捕获在我们手上 + 全抬起 + 已停手 ⇒ 判定抬起丢了，必须收尾"},
    {L"not_idle_yet_keeps_waiting", L"default",
        L"同样条件但空闲未到阈值 ⇒ 继续等，不许抢跑"},
    {L"idle_threshold_is_inclusive", L"default",
        L"空闲恰好等于阈值即触发(>= 语义，写成 > 会永远差一帧)"},
    {L"zero_threshold_aborts_when_idle", L"default",
        L"阈值 0 + 捕获在手 + 无键按下 ⇒ 立即判定(宿主自检节拍用得上)"},
    {L"wait_times_out_when_queue_quiet", L"default",
        L"队列无消息时 WaitMessageWithTimeout 必须按时返回 false —— 兜底能生效的前提"},
    {L"wait_returns_posted_message", L"default",
        L"有消息时必须原样取到(不吞消息、不改 msg.message)"},
    {L"wait_flushes_existing_backlog", L"default",
        L"队列里已有消息时立即返回，不许先睡一个超时周期"},
    {L"live_capture_predicate_matches_button_state", L"default",
        L"真窗口 SetCapture 后：阈值 0 的判据 == 「无鼠标键按下」；释放后恒为 false"},
};

std::wstring Detail(bool hold, bool btn, unsigned long long idle, unsigned long long th) {
    return std::wstring(L"hold=") + (hold ? L"1" : L"0")
        + L" btn=" + (btn ? L"1" : L"0")
        + L" idle=" + std::to_wstring(idle)
        + L" th=" + std::to_wstring(th);
}

void CaseNoCaptureNeverAborts() {
    const bool r = overlay_guard::ShouldAbortStuckCapture(false, false, 10000, 1200);
    Emit(L"no_capture_never_aborts", !r, Detail(false, false, 10000, 1200).c_str());
}

void CaseHoldingButtonNeverAborts() {
    const bool r = overlay_guard::ShouldAbortStuckCapture(true, true, 10000, 1200);
    Emit(L"holding_button_never_aborts", !r, Detail(true, true, 10000, 1200).c_str());
}

void CaseLostButtonUpAborts() {
    const bool r = overlay_guard::ShouldAbortStuckCapture(true, false, 5000, 1200);
    Emit(L"lost_button_up_aborts", r, Detail(true, false, 5000, 1200).c_str());
}

void CaseNotIdleYetKeepsWaiting() {
    const bool r = overlay_guard::ShouldAbortStuckCapture(true, false, 300, 1200);
    Emit(L"not_idle_yet_keeps_waiting", !r, Detail(true, false, 300, 1200).c_str());
}

void CaseIdleThresholdIsInclusive() {
    const bool r = overlay_guard::ShouldAbortStuckCapture(
        true, false, overlay_guard::kStuckCaptureIdleMs, overlay_guard::kStuckCaptureIdleMs);
    Emit(L"idle_threshold_is_inclusive", r,
        Detail(true, false, overlay_guard::kStuckCaptureIdleMs,
            overlay_guard::kStuckCaptureIdleMs).c_str());
}

void CaseZeroThresholdAbortsWhenIdle() {
    const bool r = overlay_guard::ShouldAbortStuckCapture(true, false, 0, 0);
    Emit(L"zero_threshold_aborts_when_idle", r, Detail(true, false, 0, 0).c_str());
}

/// 清空本线程消息队列（避免上一条用例的残留干扰计时）。
void FlushThreadQueue() {
    MSG m{};
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
    }
}

void CaseWaitTimesOutWhenQueueQuiet() {
    FlushThreadQueue();
    MSG msg{};
    const ULONGLONG t0 = GetTickCount64();
    const bool got = overlay_guard::WaitMessageWithTimeout(msg, 80);
    const ULONGLONG elapsed = GetTickCount64() - t0;
    // 必须「等够」并「等到就返回」：太快 ⇒ 根本没等（判据拿不到机会跑）；太慢 ⇒ 卡住。
    const bool ok = !got && elapsed >= 40 && elapsed <= 2000;
    Emit(L"wait_times_out_when_queue_quiet", ok,
        (L"got=" + std::to_wstring(got ? 1 : 0) + L" elapsedMs=" + std::to_wstring(elapsed)).c_str());
}

void CaseWaitReturnsPostedMessage(HWND hwnd) {
    FlushThreadQueue();
    constexpr UINT kMsg = WM_APP + 77;
    PostMessageW(hwnd, kMsg, 0x1234, 0x5678);
    MSG msg{};
    const bool got = overlay_guard::WaitMessageWithTimeout(msg, 1000);
    const bool ok = got && msg.message == kMsg
        && msg.wParam == 0x1234 && msg.lParam == 0x5678;
    Emit(L"wait_returns_posted_message", ok,
        (L"got=" + std::to_wstring(got ? 1 : 0) + L" msg=" + std::to_wstring(msg.message)).c_str());
}

void CaseWaitFlushesExistingBacklog(HWND hwnd) {
    FlushThreadQueue();
    PostMessageW(hwnd, WM_APP + 78, 0, 0);
    MSG msg{};
    const ULONGLONG t0 = GetTickCount64();
    const bool got = overlay_guard::WaitMessageWithTimeout(msg, 1000);
    const ULONGLONG elapsed = GetTickCount64() - t0;
    const bool ok = got && elapsed < 300;   // 有积压 ⇒ 不许先睡一个周期
    Emit(L"wait_flushes_existing_backlog", ok,
        (L"got=" + std::to_wstring(got ? 1 : 0) + L" elapsedMs=" + std::to_wstring(elapsed)).c_str());
    // 清掉另一条，免得污染后续用例
    FlushThreadQueue();
}

void CaseLiveCapturePredicate(HWND hwnd) {
    if (!hwnd) {
        Emit(L"live_capture_predicate_matches_button_state", false, L"测试窗口创建失败");
        return;
    }
    const bool btn = overlay_guard::AnyMouseButtonDown();
    SetCapture(hwnd);
    const bool holdSeen = (GetCapture() == hwnd);
    const bool whileHeld = overlay_guard::ShouldAbortStuckCaptureNow(hwnd, 0);
    ReleaseCapture();
    const bool afterRelease = overlay_guard::ShouldAbortStuckCaptureNow(hwnd, 0);
    // 阈值 0 ⇒ 判据只剩「持有捕获 && 无键按下」；用真实键态算期望值，测试对按键状态免疫。
    const bool ok = holdSeen && whileHeld == !btn && !afterRelease;
    Emit(L"live_capture_predicate_matches_button_state", ok,
        (L"holdSeen=" + std::to_wstring(holdSeen ? 1 : 0)
            + L" btn=" + std::to_wstring(btn ? 1 : 0)
            + L" whileHeld=" + std::to_wstring(whileHeld ? 1 : 0)
            + L" afterRelease=" + std::to_wstring(afterRelease ? 1 : 0)).c_str());
}

void PrintHelp() {
    std::fwprintf(stdout,
        L"OverlayInputGuardSelfTest — 叠层扣住鼠标捕获的兜底判据\n"
        L"\n"
        L"用法:\n"
        L"  OverlayInputGuardSelfTest.exe [--json] [--list] [--help]\n"
        L"\n"
        L"Agent: 见 .cursor/skills/module-selftest/SKILL.md\n"
        L"  源码: src/overlay_input_guard.h；应用点: src/screenshot_overlay.cpp,\n"
        L"        src/match_overlay.cpp, src/ocr_overlay.cpp, src/drag_pick_overlay.cpp,\n"
        L"        src/desktop_tools/desktop_tools.cpp, src/engine/engine_host_window.h\n");
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
        selftest::PrintCaseList(L"OverlayInputGuardSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    // 纯判据
    CaseNoCaptureNeverAborts();
    CaseHoldingButtonNeverAborts();
    CaseLostButtonUpAborts();
    CaseNotIdleYetKeepsWaiting();
    CaseIdleThresholdIsInclusive();
    CaseZeroThresholdAbortsWhenIdle();

    // 兜底通道（真消息队列）
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
        0, 0, 8, 8, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CaseWaitTimesOutWhenQueueQuiet();
    CaseWaitReturnsPostedMessage(hwnd);
    CaseWaitFlushesExistingBacklog(hwnd);
    CaseLiveCapturePredicate(hwnd);
    if (hwnd) DestroyWindow(hwnd);

    selftest::EmitSummary();
    return selftest::ExitCode();
}
