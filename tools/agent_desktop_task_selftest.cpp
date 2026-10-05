// =============================================================================
// AgentDesktopTaskSelfTest — AI 助手「动手」层纯逻辑自检
// =============================================================================
// 被测：src/agent_desktop_task.cpp（runDesktopTask 的参数解析 / 安全闸 / 动作构造 /
//       结果清洗 / 中断标志）。
//
// 为什么单独一套：这一层是「助手第一次能自己动手」的全部**判据**所在。
// 真机跑一次桌面任务要几十秒且依赖桌面会话，不能进逻辑档；但「什么样的目标必须先
// 问用户」「结果怎么清洗」「临时脚本必须落在哪」这些都是纯函数，必须逐格钉死。
//
//   MSBuild ... /t:AgentDesktopTaskSelfTest
//   build\Release\AgentDesktopTaskSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "agent_desktop_task.h"
#include "utils.h"

#include <string>
#include <vector>
#include <windows.h>

namespace {

using selftest::Emit;
using qst::agent::DesktopTaskNeedsConfirm;
using qst::agent::DesktopTaskOptions;

const selftest::CaseInfo kCases[] = {
    {L"parse_requires_goal", L"default",
        L"缺 goal 直接拒绝（空目标跑起来只会烧 API）"},
    {L"parse_rejects_bad_json", L"default",
        L"非法 JSON 返回面向模型的错误串，不抛异常"},
    {L"parse_defaults", L"default",
        L"默认值：withImage=true / maxSteps=12 / timeoutSec=120 / confirmed=false"},
    {L"parse_clamps_ranges", L"default",
        L"超时与步数被夹到引擎能接受的范围（1→10、9999→600、0→12、99→60、contextMode 越界→0）"},
    {L"danger_verbs_flagged", L"default",
        L"不可逆/对外动作必须要求确认（删/发送/下单/提交/卸载/delete/send email）"},
    {L"benign_goals_not_flagged", L"default",
        L"日常桌面操作不该被拦（打开、输入、点击、保存、求和）"},
    {L"negation_suppresses_false_positive", L"default",
        L"整句否定能压掉误报（「不要发送」），否则闸会被问废"},
    {L"partial_limiter_does_not_suppress", L"default",
        L"★局部限定词不得压掉危险判据：「只填表，然后点发送」仍须要求确认"},
    {L"confirm_prompt_demands_explicit_consent", L"default",
        L"确认回执要求模型先问用户、且必须 confirmed=true 才执行"},
    {L"action_carries_result_var", L"default",
        L"动作是 AiActionExecute，且带结果变量（助手靠它把结论读回来）"},
    {L"script_is_single_ai_action", L"default",
        L"临时脚本恰好一条动作、schemaVersion=2（多一条都会改变回放语义）"},
    {L"temp_script_in_scripts_dir", L"default",
        L"临时脚本必须落在 scripts\\ 下 —— 引擎只认 scripts/recordings 内的文件"},
    {L"temp_script_paths_unique", L"default",
        L"并发/连续调用不互相覆盖临时脚本"},
    {L"temp_script_name_predicate", L"default",
        L"★临时脚本名判据：生成器产出的名字必须被认出来；用户自己的 `_agent_task_备注.json`"
        L" 之类**不许**被误判（判严只漏过滤一个，判松会吞掉用户脚本）"},
    {L"sweep_stale_temp_scripts", L"default",
        L"★启动清扫只删「创建它的进程已不在」的残留：自己进程的（可能正跑任务）与"
        L"名字不匹配的一律不动"},
    {L"summarize_strips_control_markers", L"default",
        L"回放控制标记（[EXECUTED]/[OBSERVE]/[结果]）不出现在用户可见摘要里"},
    {L"summarize_flattens_and_caps", L"default",
        L"换行压平成单行、超长截断（助手气泡是单行）"},
    {L"summarize_empty_is_explicit", L"default",
        L"空结果显式说「未给出结论」，不许静默成空串冒充成功"},
    {L"cancel_flag_roundtrip", L"default",
        L"中断标志置位/清零可往返（面板取消 → 工具轮询看到即停）"},
};

void CaseParseRequiresGoal() {
    DesktopTaskOptions o;
    std::wstring err;
    const bool noGoal = !qst::agent::ParseDesktopTaskOptions(LR"({})", o, err);
    const bool hasMsg = err.find(L"goal") != std::wstring::npos;
    Emit(L"parse_requires_goal", noGoal && hasMsg,
        (L"noGoal=" + std::to_wstring(noGoal ? 1 : 0) + L" err=" + err).c_str());
}

void CaseParseRejectsBadJson() {
    DesktopTaskOptions o;
    std::wstring err;
    const bool rejected = !qst::agent::ParseDesktopTaskOptions(LR"({not json)", o, err);
    const bool hasMsg = !err.empty();
    Emit(L"parse_rejects_bad_json", rejected && hasMsg, err.c_str());
}

void CaseParseDefaults() {
    DesktopTaskOptions o;
    std::wstring err;
    const bool ok = qst::agent::ParseDesktopTaskOptions(LR"({"goal":"打开记事本"})", o, err);
    const bool good = ok && o.withImage && o.maxSteps == 12 && o.timeoutSec == 120
        && o.contextMode == 0 && !o.confirmed && o.goal == L"打开记事本";
    Emit(L"parse_defaults", good,
        (L"ok=" + std::to_wstring(ok ? 1 : 0) + L" withImage=" + std::to_wstring(o.withImage ? 1 : 0)
            + L" maxSteps=" + std::to_wstring(o.maxSteps)
            + L" timeoutSec=" + std::to_wstring(o.timeoutSec)
            + L" contextMode=" + std::to_wstring(o.contextMode)).c_str());
}

void CaseParseClampsRanges() {
    DesktopTaskOptions o;
    std::wstring err;
    bool ok = qst::agent::ParseDesktopTaskOptions(
        LR"({"goal":"x","timeoutSec":1,"maxSteps":0,"contextMode":9})", o, err);
    const bool lo = ok && o.timeoutSec == 10 && o.maxSteps == 12 && o.contextMode == 0;

    DesktopTaskOptions o2;
    ok = qst::agent::ParseDesktopTaskOptions(
        LR"({"goal":"x","timeoutSec":9999,"maxSteps":99,"contextMode":-5})", o2, err);
    const bool hi = ok && o2.timeoutSec == 600 && o2.maxSteps == 60 && o2.contextMode == 0;

    // maxSteps=-1 是合法语义（不封顶），必须原样保留 —— 夹紧逻辑不能把它吃掉
    DesktopTaskOptions o3;
    ok = qst::agent::ParseDesktopTaskOptions(LR"({"goal":"x","maxSteps":-1})", o3, err);
    const bool neg = ok && o3.maxSteps == -1;

    std::wstring detail = L"lo=" + std::to_wstring(lo ? 1 : 0) + L" hi="
        + std::to_wstring(hi ? 1 : 0) + L" neg=" + std::to_wstring(neg ? 1 : 0);
    Emit(L"parse_clamps_ranges", lo && hi && neg, detail.c_str());
}

void CaseDangerVerbsFlagged() {
    struct Item { const wchar_t* goal; const wchar_t* why; };
    const Item items[] = {
        { L"帮我把桌面上那些旧截图删除", L"删除" },
        { L"回复客户那条消息并发送", L"发送" },
        { L"在网站上下单购买这个商品", L"下单/购买" },
        { L"把这个表单提交到系统", L"提交" },
        { L"卸载掉那个不用的软件", L"卸载" },
        { L"清空回收站", L"清空" },
        { L"delete the temp folder", L"delete" },
        { L"send email to the team", L"send email" },
    };
    bool all = true;
    std::wstring bad;
    for (const auto& it : items) {
        if (!DesktopTaskNeedsConfirm(it.goal)) {
            all = false;
            bad += std::wstring(L"[漏]") + it.goal + L"(" + it.why + L") ";
        }
    }
    Emit(L"danger_verbs_flagged", all, bad.c_str());
}

void CaseBenignGoalsNotFlagged() {
    const wchar_t* goals[] = {
        L"打开记事本写一份今日待办并保存到桌面",
        L"在 Excel 里把 A 列求和",
        L"点击搜索框输入关键词然后回车",
        L"把这段文字输入到输入框里",
        L"把当前窗口截个图存到桌面",
        L"在浏览器里打开官网首页",
        // 英文必须按词边界匹配：`send` 不能命中 `sender`（否则英文界面里到处误报）
        L"找到邮件里的 sender 字段并复制出来",
        L"read the information panel and copy the number",
    };
    bool all = true;
    std::wstring bad;
    for (const auto* g : goals) {
        if (DesktopTaskNeedsConfirm(g)) {
            all = false;
            bad += std::wstring(L"[误报]") + g + L" ";
        }
    }
    Emit(L"benign_goals_not_flagged", all, bad.c_str());
}

void CaseNegationSuppressesFalsePositive() {
    const wchar_t* goals[] = {
        L"把内容填进输入框，不要发送",
        L"整理这些文件，先别删除任何东西",
        L"fill the form but do not submit",
    };
    bool all = true;
    std::wstring bad;
    for (const auto* g : goals) {
        if (DesktopTaskNeedsConfirm(g)) {
            all = false;
            bad += std::wstring(L"[未压掉]") + g + L" ";
        }
    }
    Emit(L"negation_suppresses_false_positive", all, bad.c_str());
}

void CasePartialLimiterDoesNotSuppress() {
    // ★ 这一条是防「闸被问废」的：局部限定词（只填/only fill）**不得**压掉危险判据。
    //   如果哪天有人为了「少问几句」把「只填」加进否定表，这条用例会立刻红。
    const wchar_t* goals[] = {
        L"只填表，然后点发送",
        L"只输入收件人，最后点提交",
        L"only fill the fields then press send",
    };
    bool all = true;
    std::wstring bad;
    for (const auto* g : goals) {
        if (!DesktopTaskNeedsConfirm(g)) {
            all = false;
            bad += std::wstring(L"[被压掉了]") + g + L" ";
        }
    }
    Emit(L"partial_limiter_does_not_suppress", all, bad.c_str());
}

void CaseConfirmPromptDemandsExplicitConsent() {
    const std::wstring p = qst::agent::DesktopTaskConfirmPrompt(L"删除桌面上的旧文件");
    const bool hasGoal = p.find(L"删除桌面上的旧文件") != std::wstring::npos;
    const bool asksUser = p.find(L"用户") != std::wstring::npos;
    const bool needsFlag = p.find(L"confirmed") != std::wstring::npos
        && p.find(L"true") != std::wstring::npos;
    const bool noTool = p.find(L"不要调用任何工具") != std::wstring::npos;
    std::wstring detail;
    if (!hasGoal) detail += L"未回显目标 ";
    if (!asksUser) detail += L"未要求问用户 ";
    if (!needsFlag) detail += L"未要求 confirmed=true ";
    if (!noTool) detail += L"未禁止先调工具 ";
    Emit(L"confirm_prompt_demands_explicit_consent",
        hasGoal && asksUser && needsFlag && noTool, detail.c_str());
}

void CaseActionCarriesResultVar() {
    DesktopTaskOptions o;
    o.goal = L"打开记事本";
    o.withImage = false;
    o.maxSteps = 7;
    o.timeoutSec = 90;
    o.contextMode = 2;
    o.modelName = L"some-model";
    const ScriptAction a = qst::agent::BuildDesktopTaskAction(o);
    const bool good = a.type == ActionType::AiActionExecute
        && a.aiPrompt == L"打开记事本"
        && !a.aiWithImage
        && a.aiMaxSteps == 7
        && a.aiTimeoutSec == 90
        && a.aiContextMode == 2
        && a.aiModelName == L"some-model"
        && a.aiOutputVarName == qst::agent::DesktopTaskResultVar()
        && !a.aiOutputVarName.empty();
    Emit(L"action_carries_result_var", good,
        (L"type=" + std::to_wstring(static_cast<int>(a.type))
            + L" var=" + a.aiOutputVarName).c_str());
}

void CaseScriptIsSingleAiAction() {
    DesktopTaskOptions o;
    o.goal = L"打开记事本";
    const ScriptFileData d = qst::agent::BuildDesktopTaskScript(o);
    const bool good = d.actions.size() == 1
        && d.actions[0].type == ActionType::AiActionExecute
        && d.schemaVersion == 2;
    Emit(L"script_is_single_ai_action", good,
        (L"actions=" + std::to_wstring(d.actions.size())
            + L" schemaVersion=" + std::to_wstring(d.schemaVersion)).c_str());
}

void CaseTempScriptInScriptsDir() {
    const std::wstring p = qst::agent::DesktopTaskTempScriptPath();
    const std::wstring scripts = ScriptsDir();
    const bool under = !scripts.empty() && p.rfind(scripts, 0) == 0;
    const bool json = p.size() > 5 && p.rfind(L".json") == p.size() - 5;
    Emit(L"temp_script_in_scripts_dir", under && json,
        (L"path=" + p).c_str());
}

void CaseTempScriptPathsUnique() {
    const std::wstring a = qst::agent::DesktopTaskTempScriptPath();
    const std::wstring b = qst::agent::DesktopTaskTempScriptPath();
    const bool diff = !a.empty() && !b.empty() && a != b;
    Emit(L"temp_script_paths_unique", diff, (L"a=" + a + L" b=" + b).c_str());
}

// ── 临时脚本名判据 ────────────────────────────────────────────────────────────
// 期望值**逐条手写**（不复用被测表达式）：把「必须认出来」和「必须不误判」两侧都钉死。
// 判严的代价只是漏过滤一个残留；判松的代价是**吞掉用户自己的脚本**（更糟）。
void CaseTempScriptNamePredicate() {
    const wchar_t* mustMatch[] = {
        L"_agent_task_1234_567890_0.json",
        L"_agent_task_1_2_3.json",
        L"_agent_task_4294967295_18446744073709551615_99.json",
    };
    const wchar_t* mustNotMatch[] = {
        L"_agent_task_备注.json",          // 中段不是三段数字
        L"_agent_task_1_2.json",           // 只有两段
        L"_agent_task_1_2_3_4.json",       // 四段
        L"_agent_task__1_2.json",          // 首段为空
        L"_agent_task_1_2_.json",          // 末段为空
        L"_agent_task_1_2_3.txt",          // 不是 .json
        L"_agent_task_1_2_3.json.bak",     // 后缀被追加过
        L"_agent_task_1_2_3",              // 缺扩展名
        L"agent_task_1_2_3.json",          // 少了开头下划线
        L"_agent_1_2_3.json",
        L"我的脚本.json",
        L"",
    };

    int bad = 0;
    std::wstring detail;
    for (const auto* n : mustMatch) {
        if (!IsAgentTaskTempFileName(n)) {
            ++bad;
            detail += L" 漏认:";
            detail += n;
        }
    }
    for (const auto* n : mustNotMatch) {
        if (IsAgentTaskTempFileName(n)) {
            ++bad;
            detail += L" 误判:";
            detail += n;
        }
    }

    // ★ 契约链：**生成器产出的真实路径**必须能被判据认出来。
    //   这两处分别定义在前缀常量与生成函数里，判据一变就必须一起动 —— 这里就是那道锁。
    const std::wstring realPath = qst::agent::DesktopTaskTempScriptPath();
    const size_t slash = realPath.find_last_of(L"\\/");
    const std::wstring realName = (slash == std::wstring::npos) ? realPath : realPath.substr(slash + 1);
    if (!IsAgentTaskTempFileName(realName)) {
        ++bad;
        detail += L" 真实生成的名字没被认出来:" + realName;
    }
    // 前缀也必须是同一个来源（`kAgentTaskTempScriptPrefix`），别处再写一份字面量就会漂移。
    if (realName.rfind(kAgentTaskTempScriptPrefix(), 0) != 0) {
        ++bad;
        detail += L" 前缀不一致:" + realName;
    }

    Emit(L"temp_script_name_predicate", bad == 0,
        (L"bad=" + std::to_wstring(bad) + detail).c_str());
}

// ── 启动清扫 ──────────────────────────────────────────────────────────────────
// 真建文件、真调清扫、真看目录 —— 纯逻辑断言测不出「删错文件」这类事故。
void CaseSweepStaleTempScripts() {
    wchar_t tmp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tmp) == 0) {
        Emit(L"sweep_stale_temp_scripts", false, L"GetTempPathW 失败");
        return;
    }
    const std::wstring dir = std::wstring(tmp) + L"qst_task_sweep_"
        + std::to_wstring(GetCurrentProcessId());
    EnsureDirectoryTree(dir);

    // ① 死进程的残留 ⇒ 必须删
    //    PID 取一个大到不存在的值：Windows 的 PID 是 4 的倍数且实际远小于此，
    //    OpenProcess 对它必然失败 ⇒ 判据走「进程已不在」分支。（理论上可能撞上复用，概率可忽略。）
    const unsigned long deadPid = 0x7FFFFFF0ul;
    // ② 自己进程的 ⇒ 必须留（可能正有另一个实例在跑任务；自己这份也是活的）
    const unsigned long livePid = static_cast<unsigned long>(GetCurrentProcessId());

    const std::wstring fStale = dir + L"\\" + std::wstring(kAgentTaskTempScriptPrefix())
        + std::to_wstring(deadPid) + L"_111_0.json";
    const std::wstring fLive = dir + L"\\" + std::wstring(kAgentTaskTempScriptPrefix())
        + std::to_wstring(livePid) + L"_222_0.json";
    const std::wstring fUser = dir + L"\\我的脚本.json";

    auto writeFile = [](const std::wstring& p) {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        const char payload[] = "{}";
        DWORD written = 0;
        const bool ok = WriteFile(h, payload, 2, &written, nullptr) != 0;
        CloseHandle(h);
        return ok;
    };
    auto exists = [](const std::wstring& p) {
        return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    };

    const bool wrote = writeFile(fStale) && writeFile(fLive) && writeFile(fUser);
    if (!wrote) {
        Emit(L"sweep_stale_temp_scripts", false, (L"造 fixture 失败：" + dir).c_str());
        return;
    }

    std::vector<std::wstring> removed;
    const int n = SweepStaleAgentTaskTempScripts(dir, &removed);

    const bool staleGone = !exists(fStale);
    const bool liveKept = exists(fLive);
    const bool userKept = exists(fUser);
    const bool countOk = (n == 1) && (removed.size() == 1);

    // 再跑一次必须是 0（幂等：第一次已删干净，不许越删越多）
    const int again = SweepStaleAgentTaskTempScripts(dir, nullptr);
    const bool idempotent = (again == 0);

    const bool ok = staleGone && liveKept && userKept && countOk && idempotent;
    Emit(L"sweep_stale_temp_scripts", ok,
        (L"n=" + std::to_wstring(n) + L" again=" + std::to_wstring(again)
            + L" staleGone=" + (staleGone ? L"1" : L"0")
            + L" liveKept=" + (liveKept ? L"1" : L"0")
            + L" userKept=" + (userKept ? L"1" : L"0")).c_str());

    // 用例自建自删（本仓约定：自检不许留垃圾）
    DeleteFileW(fStale.c_str());
    DeleteFileW(fLive.c_str());
    DeleteFileW(fUser.c_str());
    RemoveDirectoryW(dir.c_str());
}

void CaseSummarizeStripsControlMarkers() {
    const std::wstring r = qst::agent::SummarizeDesktopTaskResult(
        L"[EXECUTED][OBSERVE] 已完成，记事本已保存到桌面");
    const bool ok = r == L"已完成，记事本已保存到桌面";
    Emit(L"summarize_strips_control_markers", ok, (L"got=" + r).c_str());
}

void CaseSummarizeFlattensAndCaps() {
    std::wstring multi = L"[结果] 第一行\r\n第二行\n第三行";
    const std::wstring flat = qst::agent::SummarizeDesktopTaskResult(multi);
    const bool flattened = flat.find(L'\n') == std::wstring::npos
        && flat.find(L'\r') == std::wstring::npos
        && flat.find(L"第一行 第二行 第三行") != std::wstring::npos;

    std::wstring longText(1200, L'长');
    const std::wstring capped = qst::agent::SummarizeDesktopTaskResult(longText);
    const bool cappedOk = capped.size() <= 401
        && capped.size() > 300
        && capped.rfind(L"…") == capped.size() - 1;

    std::wstring detail = L"flattened=" + std::to_wstring(flattened ? 1 : 0)
        + L" flat=" + flat
        + L" cappedLen=" + std::to_wstring(capped.size());
    Emit(L"summarize_flattens_and_caps", flattened && cappedOk, detail.c_str());
}

void CaseSummarizeEmptyIsExplicit() {
    const std::wstring a = qst::agent::SummarizeDesktopTaskResult(L"");
    const std::wstring b = qst::agent::SummarizeDesktopTaskResult(L"   \r\n  ");
    const bool ok = !a.empty() && !b.empty()
        && a == b
        && a.find(L"未给出结论") != std::wstring::npos;
    Emit(L"summarize_empty_is_explicit", ok, (L"got=" + a).c_str());
}

void CaseCancelFlagRoundtrip() {
    qst::agent::ClearDesktopTaskCancel();
    const bool cleared = !qst::agent::DesktopTaskCancelRequested();
    qst::agent::RequestDesktopTaskCancel();
    const bool set = qst::agent::DesktopTaskCancelRequested();
    qst::agent::ClearDesktopTaskCancel();
    const bool clearedAgain = !qst::agent::DesktopTaskCancelRequested();
    Emit(L"cancel_flag_roundtrip", cleared && set && clearedAgain,
        (L"cleared=" + std::to_wstring(cleared ? 1 : 0)
            + L" set=" + std::to_wstring(set ? 1 : 0)
            + L" again=" + std::to_wstring(clearedAgain ? 1 : 0)).c_str());
}

void PrintHelp() {
    std::fwprintf(stdout,
        L"AgentDesktopTaskSelfTest — AI 助手动手层纯逻辑自检\n"
        L"  --json   每行一个用例结果 + 末行汇总\n"
        L"  --list   列出用例名/分类/含义\n");
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
        selftest::PrintCaseList(L"AgentDesktopTaskSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    CaseParseRequiresGoal();
    CaseParseRejectsBadJson();
    CaseParseDefaults();
    CaseParseClampsRanges();
    CaseDangerVerbsFlagged();
    CaseBenignGoalsNotFlagged();
    CaseNegationSuppressesFalsePositive();
    CasePartialLimiterDoesNotSuppress();
    CaseConfirmPromptDemandsExplicitConsent();
    CaseActionCarriesResultVar();
    CaseScriptIsSingleAiAction();
    CaseTempScriptInScriptsDir();
    CaseTempScriptPathsUnique();
    CaseTempScriptNamePredicate();
    CaseSweepStaleTempScripts();
    CaseSummarizeStripsControlMarkers();
    CaseSummarizeFlattensAndCaps();
    CaseSummarizeEmptyIsExplicit();
    CaseCancelFlagRoundtrip();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
