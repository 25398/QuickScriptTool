// =============================================================================
// ScriptRunnerSelfTest — 引擎执行侧：UI 钩子契约 + headless 跑纯逻辑动作
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:ScriptRunnerSelfTest
//   build\Release\ScriptRunnerSelfTest.exe --json            ← CI 口径（纯逻辑）
//   build\Release\ScriptRunnerSelfTest.exe --json --engine   ← 追加 headless 引擎跑
//
// 为什么有这个 suite（docs/refactor-acceptance.md §7.2 B3）：
//   此前 qst_engine 无法脱离壳链接（缺 PostToWebUi / HotkeyLogLine /
//   NotifyWebDebugWindowSetting / SyncHomeSelectionCache / g_instance），
//   所以**没有任何 SelfTest 能链引擎**，引擎执行侧零覆盖。
//   B 段把这些符号倒置成回调注入后（src/engine/engine_ui_hooks.*），
//   引擎可独立链接，本 suite 才有意义。
//
// 分两档：
//   [默认] UI 钩子契约 —— 锁住 B1 的倒置本身：谁再把引擎改成直接调壳，
//          或把 SetUiBridgeHooks 改成整体覆盖（丢掉另一半注册），这里会红。
//   [--engine] headless 引擎真实跑 Wait / Loop / VarCompute —— 引擎侧首次
//          获得执行级覆盖。**不进 CI 逻辑档**：需要能创建窗口的会话，
//          CI runner（session 0）可能起不来；用 --engine 显式打开。
//
// 仍未覆盖：Goto / If 的分支语义、窗口模式分支、找图分支。它们要等 #1
// 抽出 ActionContext 之后才能直接驱动（当前只能整条跑，观察粒度只有步数）。
// =============================================================================
#include "selftest_harness.h"

#include "engine/engine_ui_hooks.h"
#include "script_action_builder.h"
#include "webview/qst_engine_host.h"

#include <chrono>
#include <string>
#include <vector>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"hooks_default_noop", L"default",
        L"未装钩子时 4 个转发函数不得崩溃（引擎要能在无 UI 进程里跑）"},
    {L"hooks_installed_flag", L"default",
        L"装钩子后 UiBridgeHooksInstalled() 为真"},
    {L"hooks_forward_post_to_web_ui", L"default",
        L"qst::webview::PostToWebUi 转发到已装钩子（引擎→壳唯一出口）"},
    {L"hooks_forward_hotkey_log", L"default",
        L"HotkeyLogLine 转发到已装钩子"},
    {L"hooks_forward_notify_debug_window", L"default",
        L"NotifyWebDebugWindowSetting 转发到已装钩子"},
    {L"hooks_forward_sync_home_selection", L"default",
        L"SyncHomeSelectionCache 转发到已装钩子（含两个 wstring 参数）"},
    {L"hooks_merge_semantics", L"default",
        L"SetUiBridgeHooks 只覆盖非空成员：两次分别注册不得互相清空"},
    {L"engine_headless_start", L"engine",
        L"headless 引擎能在无 UI 进程里启动（--engine；不调 Shutdown，见注释）"},
    {L"engine_run_varcompute_wait", L"engine",
        L"跑 [varCompute, wait] 两步：步数与耗时符合预期"},
    {L"engine_run_loop_body", L"engine",
        L"跑 [loop 3 { wait 0.12 } endLoop]：循环体确实重复执行（耗时成倍）"},
    {L"engine_run_goto_skips", L"engine",
        L"跑 [goto 3, wait 1.0, wait 0.05]：Goto 真的跳转（耗时骤减）"},
};

// ── 钩子探针 ─────────────────────────────────────────────────────
struct HookProbe {
    int postCount = 0;
    std::string lastPosted;
    int hotkeyCount = 0;
    std::string lastHotkey;
    int notifyCount = 0;
    bool lastNotify = false;
    int syncCount = 0;
    std::wstring lastSyncScript;
    std::wstring lastSyncRecording;
    int lastSyncTab = -1;
};

HookProbe g_probe;

qst::engine::UiBridgeHooks MakeProbeHooks(bool withPost, bool withHotkey,
    bool withNotify, bool withSync) {
    qst::engine::UiBridgeHooks h;
    if (withPost) {
        h.postToWebUi = [](std::string s) {
            ++g_probe.postCount;
            g_probe.lastPosted = std::move(s);
        };
    }
    if (withHotkey) {
        h.hotkeyLogLine = [](const std::string& s) {
            ++g_probe.hotkeyCount;
            g_probe.lastHotkey = s;
        };
    }
    if (withNotify) {
        h.notifyWebDebugWindowSetting = [](bool b) {
            ++g_probe.notifyCount;
            g_probe.lastNotify = b;
        };
    }
    if (withSync) {
        h.syncHomeSelectionCache = [](const std::wstring& a, const std::wstring& b, int c) {
            ++g_probe.syncCount;
            g_probe.lastSyncScript = a;
            g_probe.lastSyncRecording = b;
            g_probe.lastSyncTab = c;
        };
    }
    return h;
}

// 1) 未装钩子：转发函数必须安全（no-op），不能崩。
void CaseHooksDefaultNoop() {
    const bool installedBefore = qst::engine::UiBridgeHooksInstalled();
    qst::webview::PostToWebUi("{\"type\":\"noop\"}");
    qst::webview::HotkeyLogLine("noop");
    qst::webview::NotifyWebDebugWindowSetting(true);
    qst::webview::SyncHomeSelectionCache(L"a", L"b", 1);
    const bool ok = !installedBefore;
    Emit(L"hooks_default_noop", ok,
        ok ? L"未装钩子时 4 个转发均安全 no-op"
           : L"钩子在测试开始前就已被装（用例顺序被破坏）");
}

// 2) 装钩子后标志为真
void CaseHooksInstalledFlag() {
    g_probe = HookProbe{};
    qst::engine::SetUiBridgeHooks(MakeProbeHooks(true, true, true, true));
    const bool ok = qst::engine::UiBridgeHooksInstalled();
    Emit(L"hooks_installed_flag", ok,
        ok ? L"installed=true" : L"装了钩子但 UiBridgeHooksInstalled() 仍为假");
}

// 3) PostToWebUi 转发
void CaseForwardPostToWebUi() {
    g_probe = HookProbe{};
    qst::webview::PostToWebUi("{\"type\":\"settings.changed\"}");
    const bool ok = g_probe.postCount == 1
        && g_probe.lastPosted == "{\"type\":\"settings.changed\"}";
    Emit(L"hooks_forward_post_to_web_ui", ok,
        ok ? L"转发 1 次且内容一致" : L"未转发或内容不符");
}

// 4) HotkeyLogLine 转发
void CaseForwardHotkeyLog() {
    g_probe = HookProbe{};
    qst::webview::HotkeyLogLine("start hotkey=ctrl+1");
    const bool ok = g_probe.hotkeyCount == 1 && g_probe.lastHotkey == "start hotkey=ctrl+1";
    Emit(L"hooks_forward_hotkey_log", ok,
        ok ? L"转发 1 次" : L"未转发");
}

// 5) NotifyWebDebugWindowSetting 转发（含 bool 参数）
void CaseForwardNotifyDebugWindow() {
    g_probe = HookProbe{};
    qst::webview::NotifyWebDebugWindowSetting(true);
    qst::webview::NotifyWebDebugWindowSetting(false);
    const bool ok = g_probe.notifyCount == 2 && g_probe.lastNotify == false;
    Emit(L"hooks_forward_notify_debug_window", ok,
        ok ? L"转发 2 次，末次=false" : L"转发次数或参数不符");
}

// 6) SyncHomeSelectionCache 转发（两个 wstring + int）
void CaseForwardSyncHomeSelection() {
    g_probe = HookProbe{};
    qst::webview::SyncHomeSelectionCache(L"宏/主脚本.json", L"录制/一次.json", 2);
    const bool ok = g_probe.syncCount == 1
        && g_probe.lastSyncScript == L"宏/主脚本.json"
        && g_probe.lastSyncRecording == L"录制/一次.json"
        && g_probe.lastSyncTab == 2;
    Emit(L"hooks_forward_sync_home_selection", ok,
        ok ? L"转发 1 次，参数完整（含中文路径）" : L"参数丢失或未转发");
}

// 7) 合并语义：两次分别注册不得互相清空
//    壳的真实现分布在两个 TU（bridge backend 三个 + shell 热键日志一个），
//    若 SetUiBridgeHooks 改成整体覆盖，后注册的会把先注册的抹掉 —— 这里锁住。
void CaseHooksMergeSemantics() {
    g_probe = HookProbe{};
    qst::engine::SetUiBridgeHooks(MakeProbeHooks(true, false, false, false));
    qst::engine::SetUiBridgeHooks(MakeProbeHooks(false, true, false, false));

    qst::webview::PostToWebUi("merge-check");
    qst::webview::HotkeyLogLine("merge-check");

    const bool ok = g_probe.postCount == 1 && g_probe.hotkeyCount == 1;
    Emit(L"hooks_merge_semantics", ok,
        ok ? L"两次注册共存（post=1 hotkey=1）"
           : L"后一次注册清掉了前一次（壳的两个 TU 会互相覆盖）");
}

// ── [--engine] headless 引擎跑纯逻辑动作 ───────────────────────────
// 只用不碰真实输入的动作（VarCompute / Wait / Loop），避免测试时动到用户鼠标键盘。
//
// ⚠ 两个实测坑（都踩过）：
//  1) **不要调 qst::engine::Shutdown()**。headless 引擎窗的 WM_DESTROY 会走
//     TerminateProcess（engine_runtime.cpp 有注释说明），会把测试进程直接杀掉，
//     且 stdout 还是块缓冲 → 输出全丢、exit code 还是 0（看起来像「什么都没发生」）。
//     进程退出时由 OS 回收即可。
//  2) 引擎只 Start 一次（多个用例共用），重复 Start 会被单例挡住。
bool EnsureEngineStarted(std::wstring& why) {
    static bool tried = false;
    static bool started = false;
    if (!tried) {
        tried = true;
        started = qst::engine::Start(GetModuleHandleW(nullptr));
    }
    if (!started) why = L"qst::engine::Start 返回 false";
    return started;
}

ScriptAction MakeVarCompute(const wchar_t* code) {
    ScriptAction a;
    a.type = ActionType::VarCompute;
    a.computeCode = code;
    return a;
}

ScriptAction MakeWait(double sec) {
    ScriptAction a;
    a.type = ActionType::Wait;
    a.duration = sec;
    return a;
}

// 用**生产构建器**从嵌套 JSON 生成动作列表（children → 扁平 + indent）。
// 这样测试走的是产品同一条编码路径，避免「手搓 indent 猜错约定」。
//
// ⚠ 实测坑：手搓 [loop(indent=0), wait(indent=1), endLoop(indent=1)] 时
//   循环体只跑**一遍**（耗时 173ms，等于单次 wait）。改用
//   FlattenNestedActionParamList 后发现生产编码是：
//     [{"indent":0,"loopCount":3,"type":"loop"},{"duration":0.12,"indent":1,"type":"wait"}]
//   —— **构建器不生成 endLoop**，循环体由「indent 回到 ≤ 父级」界定；
//   显式塞一个 indent=1 的 endLoop 会把循环体提前切断。
std::vector<ScriptAction> BuildActionsFromNestedJson(const char* jsonText,
    std::wstring& err, std::wstring* flatDump) {
    std::vector<ScriptAction> out;
    nlohmann::json arr = nlohmann::json::parse(jsonText, nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) {
        err = L"测试 JSON 非法";
        return out;
    }
    std::vector<nlohmann::json> nested = arr.get<std::vector<nlohmann::json>>();
    std::vector<nlohmann::json> flat;
    if (!FlattenNestedActionParamList(nested, flat, err)) return out;
    if (flatDump) {
        nlohmann::json j(flat);
        const std::string s = j.dump();
        *flatDump = std::wstring(s.begin(), s.end());
    }
    for (const auto& p : flat) {
        ScriptActionBuildResult r = BuildScriptActionFromJson(p);
        if (!r.ok) {
            err = r.error;
            out.clear();
            return out;
        }
        ScriptAction a = r.action;
        if (p.contains("indent") && p["indent"].is_number_integer()) {
            a.indent = p["indent"].get<int>();
        }
        out.push_back(a);
    }
    return out;
}

// 跑一组动作直到结束；返回是否正常收尾，并给出最大观测步数与耗时。
// ⚠ 两个实测坑（都踩过）：
//  1) 必须自己抽消息：headless 引擎窗的「跑完」收尾是经窗口消息派发的，
//     产品里由壳的 RunMessageLoop 负责；控制台测试没有消息循环 →
//     IsRunning() 会一直为真（实测：步数已经跑完 2 步，但 10s 后仍未收尾）。
//  2) ExecutedSteps() 在收尾后被清零（executedSteps_ 是运行期计数）→ 收尾后读
//     永远是 0，只能在跑的过程中采样取最大值。
bool RunActionsAndWait(const std::vector<ScriptAction>& actions, std::string& err,
    long long& elapsedMs, int& maxSteps) {
    const auto t0 = std::chrono::steady_clock::now();
    if (!qst::engine::DebugRunActions(actions, 0, /*stepMode=*/false, {}, Hotkey{},
            L"ScriptRunnerSelfTest", windowmode::WindowModeScriptConfig{}, err)) {
        return false;
    }
    maxSteps = 0;
    MSG msg{};
    for (int i = 0; i < 400 && qst::engine::IsRunning(); ++i) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const int s = qst::engine::ExecutedSteps();
        if (s > maxSteps) maxSteps = s;
        Sleep(25);
    }
    elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    return !qst::engine::IsRunning();
}

std::wstring EngineDetail(bool finished, int maxSteps, long long elapsed, const std::string& err) {
    std::wstring detail = L"steps=" + std::to_wstring(maxSteps)
        + L" elapsed=" + std::to_wstring(elapsed) + L"ms";
    if (!finished) detail += L" 未正常收尾 err=" + std::wstring(err.begin(), err.end());
    return detail;
}

void CaseEngineHeadlessStart() {
    std::wstring why;
    const bool started = EnsureEngineStarted(why);
    Emit(L"engine_headless_start", started,
        started ? L"headless 引擎可启动（进程退出时由 OS 回收，不调 Shutdown）"
                : (L"引擎无法在此会话启动：" + why).c_str());
}

// [varCompute, wait 0.15] —— 断言 Wait 真的阻塞（时序语义，不依赖步数计数）
void CaseEngineRunVarComputeWait() {
    std::wstring why;
    if (!EnsureEngineStarted(why)) {
        Emit(L"engine_run_varcompute_wait", false, (L"引擎未启动：" + why).c_str());
        return;
    }
    std::vector<ScriptAction> actions;
    actions.push_back(MakeVarCompute(L"x = 1 + 2;\nreturn x;"));
    actions.push_back(MakeWait(0.15));

    std::string err;
    long long elapsed = 0;
    int maxSteps = 0;
    const bool finished = RunActionsAndWait(actions, err, elapsed, maxSteps);

    const bool ok = finished && elapsed >= 130 && maxSteps >= 1;
    Emit(L"engine_run_varcompute_wait", ok, EngineDetail(finished, maxSteps, elapsed, err).c_str());
}

// [loop 3 { wait 0.12 } endLoop] —— 断言循环体真的重复执行
// 用「耗时成倍」而不是步数：步数计数在收尾后被清零，且采样会漏。
// 3 次 × 0.12s ≈ 0.36s；若循环没生效（只跑一遍）耗时约 0.12s。
void CaseEngineRunLoopBody() {
    std::wstring why;
    if (!EnsureEngineStarted(why)) {
        Emit(L"engine_run_loop_body", false, (L"引擎未启动：" + why).c_str());
        return;
    }
    std::wstring err;
    std::wstring flatDump;
    const std::vector<ScriptAction> actions = BuildActionsFromNestedJson(
        R"([{"type":"loop","loopCount":3,"children":[{"type":"wait","duration":0.12}]}])",
        err, &flatDump);
    if (actions.empty()) {
        Emit(L"engine_run_loop_body", false, (L"构建动作失败：" + err).c_str());
        return;
    }

    std::string serr;
    long long elapsed = 0;
    int maxSteps = 0;
    const bool finished = RunActionsAndWait(actions, serr, elapsed, maxSteps);

    const bool ok = finished && elapsed >= 340;
    std::wstring detail = EngineDetail(finished, maxSteps, elapsed, serr)
        + L" | 扁平=" + flatDump;
    Emit(L"engine_run_loop_body", ok, detail.c_str());
}

// [goto 3, wait 1.0, wait 0.05] —— 断言 Goto 真的跳转
// goto 3 应跳到第 3 个动作（即 0.05s 那个），跳过两个长等待；
// 若 goto 无效则会顺序执行 → 耗时 >= 1s。用「耗时骤减」判定，不依赖内部计数。
void CaseEngineRunGotoSkips() {
    std::wstring why;
    if (!EnsureEngineStarted(why)) {
        Emit(L"engine_run_goto_skips", false, (L"引擎未启动：" + why).c_str());
        return;
    }
    std::vector<ScriptAction> actions;
    ScriptAction jump;
    jump.type = ActionType::Goto;
    jump.gotoStepExpr = L"3";
    actions.push_back(jump);
    actions.push_back(MakeWait(1.0));
    actions.push_back(MakeWait(0.05));

    std::string err;
    long long elapsed = 0;
    int maxSteps = 0;
    const bool finished = RunActionsAndWait(actions, err, elapsed, maxSteps);

    const bool ok = finished && elapsed < 700;
    Emit(L"engine_run_goto_skips", ok, EngineDetail(finished, maxSteps, elapsed, err).c_str());
}

void PrintHelp() {
    std::fwprintf(stdout,
        L"ScriptRunnerSelfTest — 引擎执行侧：UI 钩子契约 + headless 跑动作\n"
        L"\n"
        L"用法:\n"
        L"  ScriptRunnerSelfTest.exe [--json] [--list] [--help] [--engine]\n"
        L"\n"
        L"  --engine  追加 headless 引擎真实跑 Wait/Loop/VarCompute（默认不跑：\n"
        L"            需要能创建窗口的会话，CI runner 可能起不来）\n"
        L"\n"
        L"Agent: 见 .cursor/skills/module-selftest/SKILL.md\n"
        L"  源码: src/engine/engine_ui_hooks.*, src/engine/engine_script_run.cpp\n"
        L"  背景: docs/refactor-acceptance.md §7.2 B1/B3\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool listOnly = false;
    bool withEngine = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i] ? argv[i] : L"";
        if (a == L"--json") {
            selftest::gJson = true;
            selftest::InitUtf8Stdout();
        } else if (a == L"--list") {
            listOnly = true;
            selftest::InitUtf8Stdout();
        } else if (a == L"--engine") {
            withEngine = true;
        } else if (a == L"--help" || a == L"-h") {
            PrintHelp();
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"ScriptRunnerSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    // 顺序有意义：hooks_default_noop 必须在任何 SetUiBridgeHooks 之前。
    CaseHooksDefaultNoop();
    CaseHooksInstalledFlag();
    CaseForwardPostToWebUi();
    CaseForwardHotkeyLog();
    CaseForwardNotifyDebugWindow();
    CaseForwardSyncHomeSelection();
    CaseHooksMergeSemantics();

    if (withEngine) {
        CaseEngineHeadlessStart();
        CaseEngineRunVarComputeWait();
        CaseEngineRunLoopBody();
        CaseEngineRunGotoSkips();
    }

    selftest::EmitSummary();
    return selftest::ExitCode();
}
