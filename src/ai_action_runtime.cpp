#include "ai_action_runtime.h"

#include "ai_action_service.h"
#include "ai_action_router.h"
#include "macro_execute_tools.h"

#include <algorithm>

namespace {

AgentCore* EnsureSlotCore(
    AiSessionSlot& slot,
    const std::wstring& model,
    const std::wstring& systemPrompt,
    const quickscript::AppSettings& settings,
    int recvTimeoutMs,
    bool withTools,
    int maxTokens) {

    auto makeCore = [&]() -> std::unique_ptr<AgentCore> {
        if (withTools) {
            return CreateAiActionExecuteCore(
                model, settings.ai.savedModels,
                settings.ai.apiUrl, settings.ai.apiKey,
                systemPrompt, recvTimeoutMs, -1.0);
        }
        return CreateAiActionCore(
            model, settings.ai.savedModels,
            settings.ai.apiUrl, settings.ai.apiKey,
            systemPrompt, recvTimeoutMs, -1.0, maxTokens);
    };

    if (!slot.core || slot.model != model) {
        slot.core = makeCore();
        slot.model = model;
        return slot.core.get();
    }

    quickscript::AiModelProfile profile;
    profile.modelName = model;
    for (const auto& m : settings.ai.savedModels) {
        if (m.modelName == model) { profile = m; break; }
    }
    if (profile.apiUrl.empty()) profile.apiUrl = settings.ai.apiUrl;
    if (profile.apiKey.empty()) profile.apiKey = settings.ai.apiKey;

    AgentConfig cfg;
    cfg.apiUrl = profile.apiUrl;
    cfg.apiKey = profile.apiKey;
    cfg.model = profile.modelName;
    cfg.temperature = profile.temperature;
    cfg.maxTokens = profile.maxTokens;
    cfg.recvTimeoutMs = std::max(5000, recvTimeoutMs);
    slot.core->UpdateConfig(cfg, systemPrompt);
    if (withTools) {
        slot.core->UpdateTools(BuildAiActionExecuteTools());
    } else {
        slot.core->UpdateTools({});
    }
    return slot.core.get();
}

void AppendHistoryRange(AgentCore* primary, AiSessionSlot& target, size_t startIdx) {
    if (!primary || !target.core || target.core.get() == primary) return;
    target.core->ImportHistoryFrom(*primary, startIdx);
}

}  // namespace

void AiSessionStore::ClearAll() {
    macro.Reset();
    loops.clear();
    block.Reset();
}

void AiSessionStore::ClearMacro() {
    macro.Reset();
}

void AiSessionStore::EnsureLoopDepth(int depth) {
    if (depth <= 0) return;
    while (static_cast<int>(loops.size()) < depth) {
        loops.emplace_back();
    }
}

void AiSessionStore::ClearLoopAt(int depthIndex) {
    if (depthIndex < 0 || depthIndex >= static_cast<int>(loops.size())) return;
    if (loops[static_cast<size_t>(depthIndex)].core) {
        loops[static_cast<size_t>(depthIndex)].core->ClearHistory();
    }
}

void AiSessionStore::MergeLoopChildIntoParent(int childDepthIndex) {
    if (childDepthIndex <= 0 || childDepthIndex >= static_cast<int>(loops.size())) return;
    auto& child = loops[static_cast<size_t>(childDepthIndex)];
    auto& parent = loops[static_cast<size_t>(childDepthIndex - 1)];
    if (!child.core || !parent.core) return;
    parent.core->ImportHistoryFrom(*child.core, 1);
}

void AiSessionStore::ClearBlock() {
    block.Reset();
}

void AiSessionStore::PropagateHistoryAfterCall(
    int contextMode,
    int loopDepth,
    AgentCore* primary,
    size_t startIdx,
    const ScriptAction& action,
    const std::wstring& systemPrompt,
    const quickscript::AppSettings& settings,
    int recvTimeoutMs,
    bool withTools,
    int maxTokens) {

    if (!primary || contextMode == 0) return;
    if (startIdx >= primary->GetHistory().size()) return;

    auto ensureMirror = [&](AiSessionSlot& slot) {
        if (!slot.core) {
            EnsureSlotCore(slot, action.aiModelName, systemPrompt, settings, recvTimeoutMs, withTools, maxTokens);
        }
    };

    if (contextMode != 1) {
        ensureMirror(macro);
        AppendHistoryRange(primary, macro, startIdx);
    }

    if (contextMode == 2 && loopDepth > 1) {
        for (int d = 0; d < loopDepth - 1; ++d) {
            EnsureLoopDepth(d + 1);
            ensureMirror(loops[static_cast<size_t>(d)]);
            AppendHistoryRange(primary, loops[static_cast<size_t>(d)], startIdx);
        }
    }

    if (contextMode == 3 && loopDepth > 0) {
        for (int d = 0; d < loopDepth; ++d) {
            EnsureLoopDepth(d + 1);
            ensureMirror(loops[static_cast<size_t>(d)]);
            AppendHistoryRange(primary, loops[static_cast<size_t>(d)], startIdx);
        }
    }
}

ScriptAction InheritAiActionFields(const ScriptAction& child, const ScriptAction& parent) {
    ScriptAction r = child;
    if (r.aiModelName.empty()) r.aiModelName = parent.aiModelName;
    if (r.aiContextMode == 0) r.aiContextMode = parent.aiContextMode;
    if (r.aiTimeoutSec <= 0) r.aiTimeoutSec = parent.aiTimeoutSec;
    if (r.aiSearchX2 <= r.aiSearchX1 || r.aiSearchY2 <= r.aiSearchY1) {
        r.aiSearchX1 = parent.aiSearchX1;
        r.aiSearchY1 = parent.aiSearchY1;
        r.aiSearchX2 = parent.aiSearchX2;
        r.aiSearchY2 = parent.aiSearchY2;
    }
    if (!child.aiWithImage && parent.aiWithImage) r.aiWithImage = true;
    return r;
}

AgentCore* ResolveAiContextCore(
    AiSessionStore& store,
    const ScriptAction& action,
    int loopDepth,
    const std::wstring& systemPrompt,
    const quickscript::AppSettings& settings,
    int recvTimeoutMs,
    bool withTools,
    int maxTokens) {

    const int mode = std::clamp(action.aiContextMode, 0, 3);
    const std::wstring& model = action.aiModelName;

    switch (mode) {
    case 1:
        return EnsureSlotCore(store.macro, model, systemPrompt, settings, recvTimeoutMs, withTools, maxTokens);
    case 2:
        if (loopDepth <= 0) loopDepth = 1;
        store.EnsureLoopDepth(loopDepth);
        return EnsureSlotCore(
            store.loops[static_cast<size_t>(loopDepth - 1)],
            model, systemPrompt, settings, recvTimeoutMs, withTools, maxTokens);
    case 3:
        return EnsureSlotCore(store.block, model, systemPrompt, settings, recvTimeoutMs, withTools, maxTokens);
    default:
        return nullptr;
    }
}

std::unique_ptr<AgentCore> PrepareAiAnalysisCore(
    AiSessionStore* sessions,
    const ScriptAction& action,
    int loopDepth,
    const std::wstring& systemPrompt,
    const quickscript::AppSettings& settings,
    int timeoutMs,
    int maxTokens,
    AgentCore*& coreOut) {

    coreOut = nullptr;
    if (sessions && action.aiContextMode != 0) {
        coreOut = ResolveAiContextCore(
            *sessions, action, loopDepth, systemPrompt, settings, timeoutMs, false, maxTokens);
        return nullptr;
    }

    return CreateAiActionCore(
        action.aiModelName, settings.ai.savedModels,
        settings.ai.apiUrl, settings.ai.apiKey,
        systemPrompt, timeoutMs, -1.0, maxTokens);
}

std::unique_ptr<AgentCore> PrepareAiActionExecuteCore(
    AiSessionStore* sessions,
    const ScriptAction& action,
    int loopDepth,
    AiActionRouteKind route,
    int apiWidth,
    int apiHeight,
    const quickscript::AppSettings& settings,
    int timeoutMs,
    AgentCore*& coreOut) {

    coreOut = nullptr;
    const bool withImage = apiWidth > 0 && apiHeight > 0;
    const bool useTools = (route == AiActionRouteKind::ToolExecute
        || route == AiActionRouteKind::MultiTurnTools);

    std::wstring sysPrompt;
    if (route == AiActionRouteKind::VisionQuery || route == AiActionRouteKind::CompositeClick) {
        sysPrompt = BuildAiActionVisionQuerySystemPrompt(apiWidth, apiHeight);
    } else if (route == AiActionRouteKind::MultiTurnTools) {
        sysPrompt = BuildAiActionHybridSystemPrompt(apiWidth, apiHeight);
    } else if (withImage) {
        sysPrompt = BuildAiActionExecuteSystemPrompt(apiWidth, apiHeight);
    } else {
        sysPrompt = BuildAiActionExecuteTextSystemPrompt();
    }

    if (sessions && action.aiContextMode != 0) {
        coreOut = ResolveAiContextCore(
            *sessions, action, loopDepth, sysPrompt, settings, timeoutMs, useTools, 1024);
        return nullptr;
    }

    if (useTools) {
        return CreateAiActionExecuteCore(
            action.aiModelName, settings.ai.savedModels,
            settings.ai.apiUrl, settings.ai.apiKey,
            sysPrompt, timeoutMs);
    }

    return CreateAiActionCore(
        action.aiModelName, settings.ai.savedModels,
        settings.ai.apiUrl, settings.ai.apiKey,
        sysPrompt, timeoutMs, -1.0, 1024);
}
