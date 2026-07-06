#pragma once
// ──────────────────────────────────────────────────────────────────
// ai_action_runtime.h — AI 动作执行运行时（步骤预算 + 上下文会话）
// ──────────────────────────────────────────────────────────────────

#include <memory>
#include <string>

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

struct AiSessionStore {
    std::unique_ptr<AgentCore> macroSession;
    std::unique_ptr<AgentCore> loopSession;
    std::unique_ptr<AgentCore> blockSession;
    std::unique_ptr<AgentCore> ephemeralSession;
    std::wstring macroModel;
    std::wstring loopModel;
    std::wstring blockModel;

    void ClearAll();
    void ClearMacro() { macroSession.reset(); macroModel.clear(); }
    void ClearLoop() { loopSession.reset(); loopModel.clear(); }
    void ClearBlock() { blockSession.reset(); blockModel.clear(); }
};

ScriptAction InheritAiActionFields(const ScriptAction& child, const ScriptAction& parent);

AgentCore* ResolveAiSessionCore(
    AiSessionStore& store,
    const ScriptAction& action,
    const std::wstring& systemPrompt,
    const quickscript::AppSettings& settings,
    int recvTimeoutMs);

/// 按路由准备 AgentCore：共享会话（contextMode≠0 且 tools 路径）或一次性 core
std::unique_ptr<AgentCore> PrepareAiActionExecuteCore(
    AiSessionStore* sessions,
    const ScriptAction& action,
    AiActionRouteKind route,
    int apiWidth,
    int apiHeight,
    const quickscript::AppSettings& settings,
    int timeoutMs,
    AgentCore*& coreOut);
