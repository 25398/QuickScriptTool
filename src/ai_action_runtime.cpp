#include "ai_action_runtime.h"

#include "agent_ai_actions.h"
#include "ai_action_service.h"
#include "ai_action_router.h"
#include "macro_execute_tools.h"
#include "web_ai/web_ai_config.h"

#include <algorithm>

namespace {

/// ★★ 铸造一个**桥会话 key**（网页版 AI 靠它给对话分区；真 API 只当"终端用户标识"）。
///
/// 为什么需要（2026-10-02 真机"串台"）：会话 key 是 thread_local，宿主只在 UI 面板线程
/// 设过它 ⇒ 引擎这条线程没有 ⇒ 桥回落成 `provider` ⇒ **桌面任务与网页任务共用一个豆包对话**
/// ⇒ 模型看到别的任务的上下文并回答"当前会话上下文已经丢失…你最初想让我做什么？"。
///
/// 为什么不是"每次 AI 动作执行一个 key"：那会摧毁**规定的上下文语义**
/// （`script_action_builder.cpp` 的 `aiContextMode`）—— 宏级要"同一脚本共用一个对话"、
/// 循环级要"按嵌套深度分槽"、块级要"块自己的对话"、只有**无上下文**才是"每次独立请求"。
/// ⇒ 所以 key 挂在 `AiSessionSlot` 上（宏 / loops[depth] / block），随槽存活；
///    只有 mode 0 的一次性 core 才每次现铸一个。
/// 无上下文 AI 动作共用的会话 key（见下方两处用法的说明）。
/// ⚠ 必须是**稳定字符串**：桥按它给每个"软件侧对话"分配一个网页标签页。
constexpr const char* kOnceSessionKey = "qst-once";

std::string MintAiSessionKey(const char* kind) {
    static std::atomic<unsigned long long> seq{0};
    std::string k = "aiCtx:";
    k += (kind && *kind) ? kind : "Slot";
    k += ":";
    k += std::to_string(++seq);
    return k;
}

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
        // ★★ 对话换了 ⇒ **key 必须跟着换**（桥按 key 记"已发到第几条"；沿用旧 key 会错乱）。
        slot.sessionKey = MintAiSessionKey("Slot");
    }
    // 本线程的请求发到**这个槽的对话**里（网页版 AI 以此分区；见 ai_action_runtime.h 的说明）
    if (!slot.sessionKey.empty()) SetAgentSessionKeyForThisThread(slot.sessionKey);
    return slot.core.get();

    quickscript::AiModelProfile profile;
    profile.modelName = model;
    for (const auto& m : settings.ai.savedModels) {
        if (m.modelName == model) { profile = m; break; }
    }
    if (profile.apiUrl.empty()) profile.apiUrl = settings.ai.apiUrl;
    if (profile.apiKey.empty()) profile.apiKey = settings.ai.apiKey;
    // ★ 与 CreateAiActionExecuteCore 同一口径：网页版 AI 档案要把端口/token
    //   规范到**当前**桥（否则复用 slot 时 UpdateConfig 会把新端口又改回旧的）。
    quickscript::webai::ApplyProfileOverride(profile);

    AgentConfig cfg;
    cfg.apiUrl = profile.apiUrl;
    cfg.apiKey = profile.apiKey;
    cfg.model = profile.modelName;
    cfg.temperature = profile.temperature;
    if (withTools)
        // 与 CreateAiActionExecuteCore 同一口径：思考也吃 max_tokens，2048 会中途截断
        cfg.maxTokens = (std::max)(profile.maxTokens > 0 ? profile.maxTokens : 8192, 8192);
    else
        cfg.maxTokens = maxTokens > 0 ? maxTokens : profile.maxTokens;
    cfg.recvTimeoutMs = std::max(5000, recvTimeoutMs);
    slot.core->UpdateConfig(cfg, systemPrompt);
    if (withTools) {
        // Prepare 挂无 hooks 占位工具；ExecuteAiActionExecute 入口立刻 UpdateTools(带 hooks)。
        // SendMessage 前已替换，时序安全（勿在 Prepare↔Execute 之间触发 tool-call）。
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
    // 嵌套 AI / Agent 产出动作与父共用步数语义（含 -1=不限）
    r.aiMaxSteps = parent.aiMaxSteps;
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

    // ★ 无上下文（0）= 每次独立请求 ⇒ 每次一个**一次性**会话 key
    // ★★★ 无上下文（`contextMode=0`）的 AI 动作 ⇒ **共用一个稳定的 key**
        //   （2026-10-02 用户规则："在我们软件中共用一个对话的就共用一个网页 Agents，跟 API 一个道理"）
        //
        //   ⚠⚠ 原来这里每次 `MintAiSessionKey("Once")` 铸**新 key** ⇒ 桥按 key 分标签页
        //     ⇒ **每次无上下文的动作都开一个新网页标签页**（真机：一次任务开出 4 个豆包标签页）。
        //   ⚠ 复用 key **不会**让模型失忆：system 与协议**每轮都发**（见 web_ai_prompt.cpp），
        //     历史本来就不发 ⇒ "无上下文"的语义不受影响。
        // ★★★ 优先用**调用方（AI 助手）的会话 key**（2026-10-02 用户要求）：
        //   AI 助手调 `runDesktopTask` ⇒ 引擎回放 ⇒ 这里的 AI 动作**应该打在助手那个对话里**，
        //   **不要**另开新对话（用户原话："调用 AI 动作执行不要再开新对话，在原来的那个对话里面就行"）。
        //   ⚠ 引擎是**另一个线程** ⇒ thread_local 传不过来 ⇒ 用全局 override（见 agent_core.h）。
        {
            const std::string ov = EngineSessionKeyOverride();
            SetAgentSessionKeyForThisThread(ov.empty() ? std::string(kOnceSessionKey) : ov);
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

    // 带图执行：
    // · VisionQuery / CompositeClick：必须用多模态（主模型非 VL 则改用列表识图模型）
    // · ToolExecute / MultiTurn：规划轮保持用户所选文本模型；识图由 locateAndClick 子模型承担
    //   （勿整段改用 VL，否则每轮规划都烧多模态且打乱「文本+识图」分工）
    std::wstring effModel = action.aiModelName;
    const bool toolAgent = (route == AiActionRouteKind::ToolExecute
        || route == AiActionRouteKind::MultiTurnTools);
    if (withImage && !ModelSupportsVision(effModel) && !toolAgent) {
        const std::wstring visionModel = ResolveVisionSubtaskModelName(settings.ai, effModel);
        if (!visionModel.empty()) effModel = visionModel;
    }

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
        ScriptAction routedAction = action;
        routedAction.aiModelName = effModel;
        coreOut = ResolveAiContextCore(
            *sessions, routedAction, loopDepth, sysPrompt, settings, timeoutMs, useTools, 2048);
        return nullptr;
    }

    if (useTools) {
        // ★ 无上下文（0）= **每次独立请求** ⇒ 每次铸一个一次性 key。
        //   走上面那条（contextMode≠0）时 key 由槽自己持有（见 EnsureSlotCore），
        //   这里**不要**覆盖它 —— 那正是"共用脚本对话/按深度分槽"被破坏的方式。
        // ★★★ 无上下文（`contextMode=0`）的 AI 动作 ⇒ **共用一个稳定的 key**
        //   （2026-10-02 用户规则："在我们软件中共用一个对话的就共用一个网页 Agents，跟 API 一个道理"）
        //
        //   ⚠⚠ 原来这里每次 `MintAiSessionKey("Once")` 铸**新 key** ⇒ 桥按 key 分标签页
        //     ⇒ **每次无上下文的动作都开一个新网页标签页**（真机：一次任务开出 4 个豆包标签页）。
        //   ⚠ 复用 key **不会**让模型失忆：system 与协议**每轮都发**（见 web_ai_prompt.cpp），
        //     历史本来就不发 ⇒ "无上下文"的语义不受影响。
        // ★★★ 优先用**调用方（AI 助手）的会话 key**（2026-10-02 用户要求）：
        //   AI 助手调 `runDesktopTask` ⇒ 引擎回放 ⇒ 这里的 AI 动作**应该打在助手那个对话里**，
        //   **不要**另开新对话（用户原话："调用 AI 动作执行不要再开新对话，在原来的那个对话里面就行"）。
        //   ⚠ 引擎是**另一个线程** ⇒ thread_local 传不过来 ⇒ 用全局 override（见 agent_core.h）。
        {
            const std::string ov = EngineSessionKeyOverride();
            SetAgentSessionKeyForThisThread(ov.empty() ? std::string(kOnceSessionKey) : ov);
        }
        return CreateAiActionExecuteCore(
            effModel, settings.ai.savedModels,
            settings.ai.apiUrl, settings.ai.apiKey,
            sysPrompt, timeoutMs);
    }

    // ★ 无上下文（0）= 每次独立请求 ⇒ 同样是**一次性 key**
    // ★★★ 无上下文（`contextMode=0`）的 AI 动作 ⇒ **共用一个稳定的 key**
        //   （2026-10-02 用户规则："在我们软件中共用一个对话的就共用一个网页 Agents，跟 API 一个道理"）
        //
        //   ⚠⚠ 原来这里每次 `MintAiSessionKey("Once")` 铸**新 key** ⇒ 桥按 key 分标签页
        //     ⇒ **每次无上下文的动作都开一个新网页标签页**（真机：一次任务开出 4 个豆包标签页）。
        //   ⚠ 复用 key **不会**让模型失忆：system 与协议**每轮都发**（见 web_ai_prompt.cpp），
        //     历史本来就不发 ⇒ "无上下文"的语义不受影响。
        // ★★★ 优先用**调用方（AI 助手）的会话 key**（2026-10-02 用户要求）：
        //   AI 助手调 `runDesktopTask` ⇒ 引擎回放 ⇒ 这里的 AI 动作**应该打在助手那个对话里**，
        //   **不要**另开新对话（用户原话："调用 AI 动作执行不要再开新对话，在原来的那个对话里面就行"）。
        //   ⚠ 引擎是**另一个线程** ⇒ thread_local 传不过来 ⇒ 用全局 override（见 agent_core.h）。
        {
            const std::string ov = EngineSessionKeyOverride();
            SetAgentSessionKeyForThisThread(ov.empty() ? std::string(kOnceSessionKey) : ov);
        }
    return CreateAiActionCore(
        effModel, settings.ai.savedModels,
        settings.ai.apiUrl, settings.ai.apiKey,
        sysPrompt, timeoutMs, -1.0, 1024);
}
