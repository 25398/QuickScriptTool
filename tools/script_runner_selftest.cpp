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
//   [--engine] headless 引擎真实跑 Wait / Loop / VarCompute / 变量运算失败日志 ——
//          引擎侧首次获得执行级覆盖。**不进 CI 逻辑档**：需要能创建窗口的会话，
//          CI runner（session 0）可能起不来；用 --engine 显式打开。
//
// 仍未覆盖：Goto / If 的分支语义、窗口/后台窗口模式分支、找图分支。它们要等 #1
// 抽出 ActionContext 之后才能直接驱动（当前只能整条跑，观察粒度只有步数）。
// =============================================================================
#include "selftest_harness.h"

#include "desktop_tools/desktop_tools.h"
#include "engine/engine_ui_hooks.h"
#include "script_action_builder.h"
#include "script_io.h"
#include "utils.h"
#include "webview/qst_engine_host.h"

#include <chrono>
#include <cstdio>
#include <mutex>
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
    {L"engine_run_varcompute_logs_reason", L"engine",
        L"变量运算失败时运行日志必须给出可判读原因（变量名 + 转义后的源文本）"},
    {L"engine_emits_recorder_diag", L"engine",
        L"端到端：跑 8 个相对移动后，recorder_diag.log 必须出现「请求=(0,0)/8包 … 位移一致」"
        L"（不需要开宏调试窗口）"},
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

/// 等引擎自己收尾（正常回放入口用；不触发 DebugRunActions）。
bool WaitEngineIdle(long long& elapsedMs, int& maxSteps) {
    const auto t0 = std::chrono::steady_clock::now();
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

// 引擎侧端到端：变量运算失败时，**运行日志里必须出现可判读的原因**。
//
// 现场事故：OCR 血量脚本报「变量运算失败：下标越界」，日志完全没提是哪个变量、当时的
// 源文本是什么 —— 排查只能猜表达式。现在要求两条都在日志里：
//   ① 「变量 X 未定义/为空（按 0 处理）」警告；
//   ② 报错里带下标 + 长度 + **转义后**的源文本（全角括号会显示成 \uFF08，
//      而 FormatOcrDebug 打的是原文，肉眼分不出全角/半角）。
//
// 断言方式：把 MacroDebug 切到 Web 通道并挂 poster，直接读推给壳的 JSON
// （产品里这些行就是这么到调试面板的，所以这是真实链路，不是模拟）。
void CaseEngineVarComputeLogsReason() {
    std::wstring why;
    if (!EnsureEngineStarted(why)) {
        Emit(L"engine_run_varcompute_logs_reason", false, (L"引擎未启动：" + why).c_str());
        return;
    }

    auto& dbg = qst::desktop_tools::MacroDebug();
    dbg.SetWebUiEnabled(true);
    dbg.Create(nullptr, nullptr, nullptr);  // Web 通道：只置 webCreated_，不建 GDI 窗

    std::mutex mu;
    std::vector<std::string> posted;
    qst::desktop_tools::SetMacroDebugWebPoster([&](std::string s) {
        std::lock_guard<std::mutex> lock(mu);
        posted.push_back(std::move(s));
    });

    // 用户原始代码，但 hp 不在任何上下文里（等价于「OCR 那轮没写进变量」）
    std::vector<ScriptAction> actions;
    actions.push_back(MakeVarCompute(
        L"string hp_have = split(hp,\"(\")\n"
        L"a = split(hp_have[1],\"%\")\n"
        L"result = a[0]\n"
        L"return result\n"));

    std::string err;
    long long elapsed = 0;
    int maxSteps = 0;
    const bool finished = RunActionsAndWait(actions, err, elapsed, maxSteps);
    dbg.FlushWebPendingLogs();

    std::string joined;
    {
        std::lock_guard<std::mutex> lock(mu);
        for (const auto& s : posted) joined += s;
    }

    // JsonEscapeUtf8 不转义非 ASCII（原样 UTF-8），所以中文可直接 find。
    const bool hasWarn = joined.find("未定义（按 0 处理）") != std::string::npos;
    const bool hasDetail = joined.find("超出长度") != std::string::npos
        && joined.find("被拆分的文本=") != std::string::npos;

    qst::desktop_tools::SetMacroDebugWebPoster({});
    dbg.SetWebUiEnabled(false);

    const bool ok = finished && hasWarn && hasDetail;
    std::wstring detail = EngineDetail(finished, maxSteps, elapsed, err)
        + L" | 未定义警告=" + (hasWarn ? L"有" : L"无")
        + L" 报错详情=" + (hasDetail ? L"有" : L"无")
        + L" 日志字节=" + std::to_wstring(joined.size());
    Emit(L"engine_run_varcompute_logs_reason", ok, detail.c_str());
}

// ── 端到端：录制/回放诊断行必须真的产出（不依赖「宏调试窗口」） ──────────────
//
// 为什么值得一条 end-to-end 用例：`[回放保真]` / `[鼠标报告]` / `[录制结束]` 是判定
// 「偏差在输入层还是目标侧」的唯一依据。它们曾经被 `enableDebugOutputWindow`（默认**关**）
// 和「宏调试窗口已创建」挡在后面 —— 纯逻辑自检**永远发现不了「门控把它挡掉了」**，
// 只有真跑一遍引擎、再从日志文件里读回来才行。这正是本轮之前真实踩过的坑。
//
// ⚠ 会真实注入相对移动（dx 交替 ±2 ⇒ 净位移 0，光标原地返回）并临时改 SPI 鼠标加速后
//    还原（`MouseBallisticsGuard`，与真实回放同一条路径）。只在 `--engine` 档跑。
std::wstring ReadWideFileRaw(const std::wstring& path) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) return {};
    fseek(fp, 0, SEEK_END);
    const long bytes = ftell(fp);
    if (bytes <= 0) {
        fclose(fp);
        return {};
    }
    std::vector<wchar_t> buf(static_cast<size_t>(bytes) / sizeof(wchar_t));
    fseek(fp, 0, SEEK_SET);
    const size_t got = fread(buf.data(), sizeof(wchar_t), buf.size(), fp);
    fclose(fp);
    return std::wstring(buf.data(), got);
}

/// 取最后一次出现的、以 prefix 开头的那一整行（日志是 UTF-16LE + 时间戳前缀）。
std::wstring LastLineWithPrefix(const std::wstring& text, const std::wstring& prefix) {
    const size_t at = text.rfind(prefix);
    if (at == std::wstring::npos) return {};
    size_t end = text.find(L'\n', at);
    if (end == std::wstring::npos) end = text.size();
    return text.substr(at, end - at);
}

void CaseEngineEmitsRecorderDiag() {
    std::wstring why;
    if (!EnsureEngineStarted(why)) {
        Emit(L"engine_emits_recorder_diag", false, (L"引擎未启动：" + why).c_str());
        return;
    }
    const std::wstring logPath = qst::desktop_tools::RecorderDiagLogPath();
    if (logPath.empty()) {
        Emit(L"engine_emits_recorder_diag", false, L"取不到诊断日志路径");
        return;
    }
    // ⚠ 日志是**追加**的，而下面用 `rfind` 取最后一次出现 —— 若上一轮遗留的行还在，
    //   会读到**别人的**读数（实测踩过：拿到「请求=(0,0)/0包 已收尾=1」的上一轮旧行 ⇒ 假失败）。
    //   用例必须自己保证前置状态干净，不能依赖调用方先删文件。
    DeleteFileW(logPath.c_str());

    // ⚠ 必须走**正常回放入口** `RunScriptPath`：编辑器调试入口 `DebugRunActions` 在 worker 里
    //   走 `if (debugMode_)` 分支，**不经过每轮的时间轴/保真统计** ⇒ 拿它测等于什么都没测
    //   （第一版就是这么写的：日志文件建出来了、一行没写）。故把动作写成脚本文件再按路径运行。
    // 8 个相对移动 + 显式 timingUs 间隔 ⇒ ScriptIsTimedInputSequence 为真 ⇒ 精密轴启用，
    // [回放保真] 才会打。净位移 0 是为了不把正在用机器的人的光标带走。
    std::vector<ScriptAction> actions;
    for (int i = 0; i < 8; ++i) {
        ScriptAction wait{};
        wait.type = ActionType::Wait;
        wait.timingUs = 2000;
        wait.duration = 0.002;
        actions.push_back(wait);
        ScriptAction mv{};
        mv.type = ActionType::MoveMouseRelative;
        mv.x = (i % 2 == 0) ? 2 : -2;
        mv.y = 0;
        mv.duration = 0.0;
        actions.push_back(mv);
    }

    ScriptFileData data{};
    data.actions = actions;
    const std::wstring scriptPath = ScriptsDir() + L"\\__selftest_recorder_diag.json";
    if (!SaveScriptFileData(scriptPath, data)) {
        Emit(L"engine_emits_recorder_diag", false,
            (L"写测试脚本失败：" + scriptPath).c_str());
        return;
    }
    std::string err;
    if (!qst::engine::RunScriptPath(scriptPath, err)) {
        DeleteFileW(scriptPath.c_str());
        Emit(L"engine_emits_recorder_diag", false,
            (L"正常回放入口未启动：" + std::wstring(err.begin(), err.end())).c_str());
        return;
    }
    // ⚠ 引擎按「宏执行次数」跑，默认**不限次数** ⇒ 不会自己收尾。轮询到诊断行出现就主动停，
    //   再等收尾（拿「收尾」当断言会永远失败，那不是诊断链的问题）。
    std::wstring line;
    MSG msg{};
    for (int i = 0; i < 200 && line.empty(); ++i) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        line = LastLineWithPrefix(ReadWideFileRaw(logPath), L"[回放保真]");
        if (line.empty()) Sleep(25);
    }
    const bool hasLine = !line.empty();
    qst::engine::StopScript();
    long long elapsed = 0;
    int maxSteps = 0;
    const bool finished = WaitEngineIdle(elapsed, maxSteps);
    DeleteFileW(scriptPath.c_str());
    const bool requestOk = line.find(L"请求=(0,0)/8包") != std::wstring::npos;
    const bool matched = line.find(L"位移一致") != std::wstring::npos;
    // 非窗口/后台窗口模式下必须走 SendInput 计数，不能落到「未统计」分支
    const bool counted = line.find(L"未统计") == std::wstring::npos;

    const bool ok = hasLine && requestOk && matched && counted && finished;
    if (ok) {
        Emit(L"engine_emits_recorder_diag", true, L"");
        return;
    }
    std::wstring detail = L"hasLine=" + std::to_wstring(hasLine ? 1 : 0)
        + L" 请求侧=" + std::to_wstring(requestOk ? 1 : 0)
        + L" 位移一致=" + std::to_wstring(matched ? 1 : 0)
        + L" 已计数=" + std::to_wstring(counted ? 1 : 0)
        + L" 已收尾=" + std::to_wstring(finished ? 1 : 0)
        + L" path=" + logPath + L" line=" + line;
    if (!hasLine) {
        detail += L"  ← 日志里没有这行：① 设置 autoOutputKeyFunctionDebug 必须为真；"
            L"② 这行只在「时间轴脚本」下打（录制脚本，或带 timingUs 的手写脚本）——"
            L"普通无 timingUs 的宏本来就不会出现";
    }
    Emit(L"engine_emits_recorder_diag", false, detail.c_str());
}

void PrintHelp() {
    std::fwprintf(stdout,
        L"ScriptRunnerSelfTest — 引擎执行侧：UI 钩子契约 + headless 跑动作\n"
        L"\n"
        L"用法:\n"
        L"  ScriptRunnerSelfTest.exe [--json] [--list] [--help] [--engine]\n"
        L"\n"
        L"  --engine  追加 headless 引擎真实跑 Wait/Loop/VarCompute/变量运算失败日志\n"
        L"            （默认不跑：需要能创建窗口的会话，CI runner 可能起不来）\n"
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
        CaseEngineVarComputeLogsReason();
        CaseEngineEmitsRecorderDiag();
    }

    selftest::EmitSummary();
    return selftest::ExitCode();
}
