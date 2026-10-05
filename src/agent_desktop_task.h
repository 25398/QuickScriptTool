#pragma once
// ──────────────────────────────────────────────────────────────────
// agent_desktop_task.h — AI 脚本助手的「动手」层（Phase 1）
//
// 背景（差距审计见 docs/agent-capability-expansion.md）：
//   助手的 34 个工具全是「产出物」型 —— 改文件、改配置、生成脚本 JSON。
//   用户说「帮我把这件事做了」，助手只能**生成一个脚本**，不能自己动手。
//   而桌面执行闭环（observe → plan → act → verify）在宏侧**早就成熟**：
//     ExecuteAiActionExecute + 引擎在 engine_script_run.cpp 接好的 agentHooks。
//   本模块做的不是重写引擎，而是**把那条闭环接到助手上**：
//
//     一句目标  →  一条 aiActionExecute 动作  →  临时脚本  →  引擎正常回放链路
//                                                              ↓
//                       助手工作线程 ← 结果变量（aiVars_）← 引擎窗口线程
//
// 为什么走「临时脚本 + 正常回放」而不是直接调 ExecuteAiActionExecute：
//   agentHooks（截图 / 执行动作 / 观察比对 / 定位点击 / 前视）是在
//   engine_script_run.cpp 里按回放上下文组装出来的，直接从外面调等于把它**重抄一遍**
//   —— 两份必然漂移（AGENTS.md 的老毛病）。走回放链路顺带白拿：中断/脱离暂停、
//   窗口/后台窗口模式、超时、AI 调试日志、嵌套熔断。
//
// 本文件只放**纯逻辑**（可逐格自检）：参数解析、危险判据、动作构造、结果清洗。
// 真正投递与轮询在 agent_tools.cpp（runDesktopTask），跨线程原语在 qst::engine。
// ──────────────────────────────────────────────────────────────────

#include <string>

#include "script_io.h"
#include "script_types.h"

namespace qst {
namespace agent {

/// runDesktopTask 的入参
struct DesktopTaskOptions {
    std::wstring goal;            ///< 目标（自然语言，喂给 AI 动作执行当 prompt）
    bool withImage = true;        ///< 是否带截图（要看界面才做得了事）
    int maxSteps = 12;            ///< 最大执行步数；-1 = 不封顶（仅留轮次安全上限）
    int timeoutSec = 120;         ///< 单次 API 超时（秒）
    int contextMode = 0;          ///< 0=无上下文 1=宏上下文 2=循环 3=指令块
    std::wstring modelName;       ///< 空 = 由设置里的当前模型决定
    bool confirmed = false;       ///< 用户在**上一轮**已明确同意做这个危险动作
};

/// 解析 runDesktopTask 的工具参数。失败时返回 false 并填 err（err 面向模型）。
bool ParseDesktopTaskOptions(const std::wstring& paramsJson,
    DesktopTaskOptions& out, std::wstring& err);

/// 目标里是否含**不可逆 / 对外**的动作。
///
/// 为什么要有这道闸：助手的其他工具最坏情况是「改错一个文件」，有快照能回滚
/// （agent_changes）；而桌面任务最坏情况是**已经把消息发出去、把钱付了**。
/// 回滚不了的东西必须在动手之前停下来问人 —— 这是 SOUL.md「对外动作谨慎」的落点，
/// 也是主流 Agent 的 approval 语义。
///
/// 判据是**纯文本匹配**，所以：
///  · 宁可多问一句，不可少问一句（误报的代价是用户多点一次「确认」）；
///  · 但「不要发送 / 先别发」这类**明确否定**必须能压掉误报（否则助手会一直问，
///    用户会开始无脑点确认，闸就废了）。
bool DesktopTaskNeedsConfirm(const std::wstring& goal);

/// 需要确认但尚未确认时的回执。**不执行任何动作**，只让模型去问用户。
std::wstring DesktopTaskConfirmPrompt(const std::wstring& goal);

/// 构造执行用的动作（单条 aiActionExecute，带结果变量）
ScriptAction BuildDesktopTaskAction(const DesktopTaskOptions& opts);

/// 把动作包成脚本数据（供 SaveScriptFileData 落盘成临时脚本）
ScriptFileData BuildDesktopTaskScript(const DesktopTaskOptions& opts);

/// 结果变量名：引擎回放把 AI 的最终文本写进这个宏变量，助手再读回来。
/// 前缀刻意取得不可能与用户脚本重名。
const wchar_t* DesktopTaskResultVar();

/// 临时脚本落在 scripts\ 下（引擎的 ResolveLibraryScriptPath **只认 scripts/ 与
/// recordings/ 目录内的文件**，放别处根本跑不起来）。
/// 命名带 pid 与 tick，避免并发/上次残留互相覆盖；跑完即删。
std::wstring DesktopTaskTempScriptPath();

/// 从回放结果里提取给用户看的一行摘要：去控制标记、压平换行、截断。
std::wstring SummarizeDesktopTaskResult(const std::wstring& raw);

// ── 中断（S2：执行期间可打断）────────────────────────────────────
// 助手工具跑在工作线程上，拿不到 AgentCore 的 cancelFlag，所以用一个进程级标志：
// 面板上的「取消」→ CancelAgentMessage → RequestDesktopTaskCancel → 工具轮询看到即停。
void RequestDesktopTaskCancel();
void ClearDesktopTaskCancel();
bool DesktopTaskCancelRequested();

}  // namespace agent
}  // namespace qst
