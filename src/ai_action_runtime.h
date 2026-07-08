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

    void Reset() {
        core.reset();
        model.clear();
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
