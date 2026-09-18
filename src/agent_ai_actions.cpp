#include "agent_ai_actions.h"

#include "app_settings_store.h"
#include "script_action_builder.h"
#include "script_types.h"
#include "utils.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <sstream>
#include <vector>

namespace {

std::wstring ToLowerCopy(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool ModelExistsInSettings(const quickscript::AiApiSettings& ai, const std::wstring& name) {
    if (Trim(name).empty()) return false;
    for (const auto& m : ai.savedModels) {
        if (m.modelName == name) return true;
    }
    return ai.modelName == name;
}

}  // namespace

namespace {

bool IsAiMacroActionType(const std::wstring& type) {
    return type == L"aiTextAnalysis" || type == L"aiImageAnalysis" || type == L"aiActionExecute";
}

bool ActionRequiresVisionModel(const std::wstring& type, const nlohmann::json& params) {
    if (type == L"aiImageAnalysis") return true;
    if (type != L"aiActionExecute") return false;
    if (!params.contains("aiWithImage")) return true;
    if (params["aiWithImage"].is_boolean()) return params["aiWithImage"].get<bool>();
    if (params["aiWithImage"].is_number_integer()) return params["aiWithImage"].get<int>() != 0;
    return true;
}

bool ActionRequiresVisionModel(const ScriptAction& action) {
    if (action.type == ActionType::AiImageAnalysis) return true;
    if (action.type == ActionType::AiActionExecute) return action.aiWithImage;
    return false;
}

}  // namespace

quickscript::AppSettings LoadAgentAppSettings() {
    quickscript::AppSettings settings = quickscript::DefaultAppSettings();
    LoadAppSettings(settings);
    return settings;
}

bool ModelSupportsVision(const std::wstring& modelName) {
    const std::wstring lower = ToLowerCopy(Trim(modelName));
    if (lower.empty()) return false;

    // DeepSeek：**v4 起是原生多模态**（v4-flash / v4.1-flash / *-vision 都能收图），
    // 而 V3.x / R1 / chat / reasoner 仍是纯文本。
    // ★这里曾经是「名字里带 deepseek 且没有 vl/vision 就一律纯文本」——于是用户选的
    //   「deepseek v4.1 flash」被判成不能识图，动作模型被静默换成豆包（用户实测报障）。
    if (lower.find(L"deepseek") != std::wstring::npos) {
        if (lower.find(L"vl") != std::wstring::npos
            || lower.find(L"vision") != std::wstring::npos
            || lower.find(L"v4") != std::wstring::npos
            || lower.find(L"v5") != std::wstring::npos) {
            return true;
        }
        return false;
    }

    static const wchar_t* kVisionHints[] = {
        L"gpt-4o", L"gpt-4.1", L"gpt-4-vision", L"gpt-4v", L"gpt-5",
        L"o1", L"o3", L"o4", L"claude-3", L"claude-sonnet-4", L"claude-opus-4",
        L"gemini", L"qwen-vl", L"qwen2-vl", L"qwen3-vl", L"glm-4v", L"glm-4.5v",
        L"deepseek-vl", L"internvl", L"yi-vision", L"llava", L"pixtral",
        L"doubao", L"seed", L"volces", L"ark.cn",
        L"vision", L"multimodal", L"-vl", L"vl-", L"识图",
    };
    for (const wchar_t* hint : kVisionHints) {
        if (lower.find(hint) != std::wstring::npos) return true;
    }
    return false;
}

bool ModelIsReasoningType(const std::wstring& modelName) {
    const std::wstring lower = ToLowerCopy(Trim(modelName));
    if (lower.empty()) return false;
    // OpenAI o 系列推理模型
    if (lower.find(L"o1") != std::wstring::npos
        || lower.find(L"o3") != std::wstring::npos
        || lower.find(L"o4") != std::wstring::npos) {
        return true;
    }
    // DeepSeek R1 / Reasoner
    if (lower.find(L"deepseek-r1") != std::wstring::npos
        || lower.find(L"deepseek-reasoner") != std::wstring::npos
        || lower.find(L"deepseek_v3.1-turbo") != std::wstring::npos) {
        return true;
    }
    // Kimi / Grok 推理版
    if (lower.find(L"kimi-k2-thinking") != std::wstring::npos
        || lower.find(L"grok-3-reasoning") != std::wstring::npos
        || lower.find(L"grok-4-reasoning") != std::wstring::npos) {
        return true;
    }
    return false;
}

std::wstring ResolveAiModelName(const quickscript::AiApiSettings& ai,
    bool requireVision, const std::wstring& preferred) {
    const std::wstring pref = Trim(preferred);
    if (!pref.empty() && ModelExistsInSettings(ai, pref)) {
        if (!requireVision || ModelSupportsVision(pref)) return pref;
    }

    // ★用户当前配置的模型（设置→AI助手）优先于「列表里的第一个」：
    // 动作没写模型名时，用户的期望显然是「就用我设置里选的那个」，
    // 而不是「我添加过的第一个模型」（实测：动作没写模型 → 跑成列表第 1 个的豆包）。
    if (!ai.modelName.empty() && (!requireVision || ModelSupportsVision(ai.modelName))) {
        return ai.modelName;
    }

    if (requireVision) {
        for (const auto& m : ai.savedModels) {
            if (ModelSupportsVision(m.modelName)) return m.modelName;
        }
        return L"";
    }

    for (const auto& m : ai.savedModels) {
        if (!m.modelName.empty()) return m.modelName;
    }
    return ai.modelName;
}

std::wstring FormatAvailableAiModelsList(const quickscript::AiApiSettings& ai) {
    std::wstringstream ss;
    ss << L"【已添加的宏 AI 模型】（与设置→AI助手、编辑器 AI 动作下拉框同源）\n";
    if (ai.savedModels.empty()) {
        if (ai.modelName.empty()) {
            ss << L"  （暂无）请在设置→AI助手中添加模型。\n";
            return ss.str();
        }
        ss << L"  1. " << ai.modelName
            << (ModelSupportsVision(ai.modelName) ? L" [识图]" : L" [文本]")
            << L"（默认模型，savedModels 为空）\n";
        return ss.str();
    }
    ss << L"共 " << ai.savedModels.size() << L" 个：\n";
    int index = 1;
    for (const auto& m : ai.savedModels) {
        ss << L"  " << index++ << L". " << m.modelName
            << (ModelSupportsVision(m.modelName) ? L" [识图]" : L" [文本]");
        if (!m.apiUrl.empty()) ss << L"  @" << m.apiUrl;
        ss << L"\n";
    }
    if (!ai.modelName.empty()) {
        ss << L"当前默认模型名：" << ai.modelName << L"\n";
    }
    return ss.str();
}

bool ApplyResolvedAiModelToActionParams(nlohmann::json& params) {
    if (!params.is_object() || !params.contains("type") || !params["type"].is_string()) return false;
    const std::wstring type = Trim(FromUtf8(params["type"].get<std::string>()));
    if (!IsAiMacroActionType(type)) return false;

    std::wstring preferred;
    if (params.contains("aiModelName") && params["aiModelName"].is_string()) {
        preferred = Trim(FromUtf8(params["aiModelName"].get<std::string>()));
    }
    // 已显式指定且满足需求（不需要识图，或该模型支持识图）→ 不改
    if (!preferred.empty() && ModelExistsInSettings(LoadAgentAppSettings().ai, preferred)
        && (!ActionRequiresVisionModel(type, params) || ModelSupportsVision(preferred))) {
        return false;
    }
    // AI 动作执行同样不在这里改写模型（识图由运行时按能力决定，见 EnsureAiModelOnAction）
    if (type == L"aiActionExecute") return false;

    const quickscript::AppSettings settings = LoadAgentAppSettings();
    const std::wstring resolved = ResolveAiModelName(
        settings.ai, ActionRequiresVisionModel(type, params), preferred);
    if (Trim(resolved).empty()) return false;
    params["aiModelName"] = ToUtf8(resolved);
    return true;
}

void EnsureAiModelOnAction(ScriptAction& action) {
    if (action.type != ActionType::AiTextAnalysis
        && action.type != ActionType::AiImageAnalysis
        && action.type != ActionType::AiActionExecute) {
        return;
    }
    const quickscript::AppSettings settings = LoadAgentAppSettings();
    // 已显式指定且满足需求（不需要识图，或该模型支持识图）→ 不改
    if (!action.aiModelName.empty() && ModelExistsInSettings(settings.ai, action.aiModelName)
        && (!ActionRequiresVisionModel(action) || ModelSupportsVision(action.aiModelName))) {
        return;
    }
    // ★AI 动作执行不改写模型：识图需要由**运行时**按模型能力决定（`RunAiActionExecuteForAction`
    // 会在带图执行时改用识图模型并打诊断），写库时静默替换会把用户选的模型永久换成别的
    //（用户实测：选了 deepseek-v4.1-flash，脚本里被存成豆包，之后再也不会用回自己选的模型）。
    if (action.type == ActionType::AiActionExecute) return;
    action.aiModelName = ResolveAiModelName(
        settings.ai, ActionRequiresVisionModel(action), action.aiModelName);
}

std::wstring ResolveActionAiModelName(const ScriptAction& action,
    const quickscript::AiApiSettings& ai) {
    // 即使动作已指定模型，也把该名字作为 preferred 交给 ResolveAiModelName：
    // 需要识图（aiImageAnalysis / aiActionExecute 带图）而指定模型不支持识图时，
    // 自动跳过并从 savedModels 挑选多模态模型，避免文本模型去识图必败。
    const std::wstring preferred = action.aiModelName;
    if (!preferred.empty() && ModelExistsInSettings(ai, preferred)) {
        const std::wstring resolved =
            ResolveAiModelName(ai, ActionRequiresVisionModel(action), preferred);
        return resolved;
    }
    return ResolveAiModelName(ai, ActionRequiresVisionModel(action), L"");
}

std::wstring ResolveVisionSubtaskModelName(const quickscript::AiApiSettings& ai,
    const std::wstring& primaryModel) {
    const std::wstring primary = Trim(primaryModel);
    if (!primary.empty() && ModelExistsInSettings(ai, primary)
        && ModelSupportsVision(primary)) {
        return primary;
    }
    for (const auto& m : ai.savedModels) {
        if (ModelSupportsVision(m.modelName)) return m.modelName;
    }
    if (ModelSupportsVision(ai.modelName)) return ai.modelName;
    return L"";
}

bool HasUsableVisionModel(const quickscript::AiApiSettings& ai,
    const std::wstring& primaryModel) {
    return !ResolveVisionSubtaskModelName(ai, primaryModel).empty();
}

std::wstring MissingVisionModelError(const std::wstring& primaryModel) {
    const std::wstring primary = Trim(primaryModel);
    std::wstring msg = L"看屏/识图需要多模态模型，但";
    if (!primary.empty())
        msg += L"当前模型「" + primary + L"」非多模态，且";
    msg += L"列表中没有可用的识图模型。"
        L"请在设置→AI助手添加支持识图的模型（如 gpt-4o / qwen-vl / doubao），"
        L"或将本动作改选为多模态模型。本步已跳过。";
    return msg;
}

bool ShouldAttachObserveImageToPlanner(const std::wstring& plannerModel, bool haveImage) {
    if (!haveImage) return false;
    return ModelSupportsVision(plannerModel);
}

std::wstring BuildSingleAgentActionResult(const nlohmann::json& actionParams,
    const std::wstring& note) {
    std::wstring error;
    const std::vector<nlohmann::json> items = { actionParams };
    const std::wstring jsonArray = BuildScriptActionsJsonArray(items, error);
    if (!error.empty()) return L"[错误] " + error;

    std::wstring summary = L"✓ 已构建 1 个动作。";
    if (!note.empty()) summary += L"\n" + note;
    summary += L"\n将下方 JSON 数组嵌入脚本的 actions 字段即可：\n\n";
    summary += jsonArray;
    return summary;
}
