#include "ai_action_runtime.h"



#include "ai_action_service.h"
#include "ai_action_router.h"



#include <algorithm>



void AiSessionStore::ClearAll() {

    macroSession.reset();

    loopSession.reset();

    blockSession.reset();

    ephemeralSession.reset();

    macroModel.clear();

    loopModel.clear();

    blockModel.clear();

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

    if (r.aiMaxSteps == 10 && parent.aiMaxSteps != 10) {

        // JSON 缂虹渷 aiMaxSteps 鏃朵负 10锛涘祵濂楄嚜妫€閫氬父姝ユ暟杈冨皯锛屼繚鐣欐樉寮忓€?
    }

    return r;

}



AgentCore* ResolveAiSessionCore(

    AiSessionStore& store,

    const ScriptAction& action,

    const std::wstring& systemPrompt,

    const quickscript::AppSettings& settings,

    int recvTimeoutMs) {

    const int mode = std::clamp(action.aiContextMode, 0, 3);

    const std::wstring& model = action.aiModelName;



    auto ensureCore = [&](std::unique_ptr<AgentCore>& slot, std::wstring& modelSlot) -> AgentCore* {

        if (!slot || modelSlot != model) {

            slot = CreateAiActionExecuteCore(

                model, settings.ai.savedModels,

                settings.ai.apiUrl, settings.ai.apiKey,

                systemPrompt, recvTimeoutMs, -1.0);

            modelSlot = model;

            return slot.get();

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

        slot->UpdateConfig(cfg, systemPrompt);

        return slot.get();

    };



    switch (mode) {

    case 1: return ensureCore(store.macroSession, store.macroModel);

    case 2: return ensureCore(store.loopSession, store.loopModel);

    case 3: return ensureCore(store.blockSession, store.blockModel);

    default: {

        if (!store.ephemeralSession) {

            store.ephemeralSession = CreateAiActionExecuteCore(

                model, settings.ai.savedModels,

                settings.ai.apiUrl, settings.ai.apiKey,

                systemPrompt, recvTimeoutMs, -1.0);

        } else {

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

            store.ephemeralSession->UpdateConfig(cfg, systemPrompt);

        }

        return store.ephemeralSession.get();

    }

    }

}



std::unique_ptr<AgentCore> PrepareAiActionExecuteCore(
    AiSessionStore* sessions,
    const ScriptAction& action,
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

    if (sessions && action.aiContextMode != 0 && useTools) {
        coreOut = ResolveAiSessionCore(*sessions, action, sysPrompt, settings, timeoutMs);
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


