#pragma once
// ──────────────────────────────────────────────────────────────────
// ai_action_runtime.h — AI 动作执行运行时（步骤预算 + 上下文会话）
// ──────────────────────────────────────────────────────────────────

#include <memory>
#include <string>
#include <vector>

#include "agent_core.h"
#include "ai_action_router.h"
#include "app_settings.h"
#include "script_types.h"

struct AiStepBudgetState {
    int maxSteps = -1;
    int usedSteps = 0;
    bool exhausted = false;
};

struct AiStepFrame {
    int localMax = -1;
    int localUsed = 0;
    AiStepBudgetState* shared = nullptr;
};

inline bool ConsumeAiStep(AiStepFrame& frame) {
    if (!frame.shared) return true;
    if (frame.shared->exhausted) return false;
    if (frame.shared->maxSteps >= 0 && frame.shared->usedSteps >= frame.shared->maxSteps) {
        frame.shared->exhausted = true;
        return false;
    }
    if (frame.localMax >= 0 && frame.localUsed >= frame.localMax) return false;
    ++frame.shared->usedSteps;
    ++frame.localUsed;
    return true;
}

struct AiSessionSlot {
    std::unique_ptr<AgentCore> core;
    std::wstring model;
    /// ★★ 本槽对应的**桥会话 key**（网页版 AI 靠它分对话）。
    ///
    /// 规定见 `script_action_builder.cpp` 的 `aiContextMode`：
    ///   0=每次独立请求；1=宏级（同一脚本共用一个对话）；2=循环级（按嵌套深度分槽）；
    ///   3=块级。⇒ key 必须**跟着槽走**，而不是跟着"一次执行"走：
    ///   槽是跨轮次存活的，那正是"上下文"这三个字的含义。
    ///
    /// ⚠ 槽的 core 重建（换模型 / 被清空）时必须**换新 key** —— 桥按 key 记
    ///   "已发到第几条"（见 `agent_core.h` 的说明）；对话换了而 key 没换，
    ///   第二轮起内容就错乱。
    std::string sessionKey;

    void Reset() {
        core.reset();
        model.clear();
        sessionKey.clear();   // 清上下文 ⇒ 下一次是新对话
    }
};

struct AiSessionStore {
    AiSessionSlot macro;
    std::vector<AiSessionSlot> loops;
    AiSessionSlot block;

    void ClearAll();
    void ClearMacro();
    void ClearLoopAt(int depthIndex);
    void MergeLoopChildIntoParent(int childDepthIndex);
    void ClearBlock();
    void EnsureLoopDepth(int depth);

    /// 将 primary 在 startIdx 之后的新消息同步到宏/上级循环（上级可见下级对话）
    void PropagateHistoryAfterCall(
        int contextMode,
        int loopDepth,
        AgentCore* primary,
        size_t startIdx,
        const ScriptAction& action,
        const std::wstring& systemPrompt,
        const quickscript::AppSettings& settings,
        int recvTimeoutMs,
        bool withTools,
        int maxTokens);
};

ScriptAction InheritAiActionFields(const ScriptAction& child, const ScriptAction& parent);

AgentCore* ResolveAiContextCore(
    AiSessionStore& store,
    const ScriptAction& action,
    int loopDepth,
    const std::wstring& systemPrompt,
    const quickscript::AppSettings& settings,
    int recvTimeoutMs,
    bool withTools,
    int maxTokens);

std::unique_ptr<AgentCore> PrepareAiAnalysisCore(
    AiSessionStore* sessions,
    const ScriptAction& action,
    int loopDepth,
    const std::wstring& systemPrompt,
    const quickscript::AppSettings& settings,
    int timeoutMs,
    int maxTokens,
    AgentCore*& coreOut);

/// 按路由准备 AgentCore：共享会话（contextMode≠0）或一次性 core
std::unique_ptr<AgentCore> PrepareAiActionExecuteCore(
    AiSessionStore* sessions,
    const ScriptAction& action,
    int loopDepth,
    AiActionRouteKind route,
    int apiWidth,
    int apiHeight,
    const quickscript::AppSettings& settings,
    int timeoutMs,
    AgentCore*& coreOut);
