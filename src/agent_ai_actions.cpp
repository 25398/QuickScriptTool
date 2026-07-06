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

std::wstring ResolveAiModelName(const quickscript::AiApiSettings& ai,
    bool requireVision, const std::wstring& preferred) {
    const std::wstring pref = Trim(preferred);
    if (!pref.empty() && ModelExistsInSettings(ai, pref)) {
        if (!requireVision || ModelSupportsVision(pref)) return pref;
    }

    if (requireVision) {
        for (const auto& m : ai.savedModels) {
            if (ModelSupportsVision(m.modelName)) return m.modelName;
        }
        if (ModelSupportsVision(ai.modelName)) return ai.modelName;
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

    if (params.contains("aiModelName") && params["aiModelName"].is_string()) {
        if (!Trim(FromUtf8(params["aiModelName"].get<std::string>())).empty()) return false;
    }

    const quickscript::AppSettings settings = LoadAgentAppSettings();
    const std::wstring resolved = ResolveAiModelName(
        settings.ai, ActionRequiresVisionModel(type, params), L"");
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
    if (!action.aiModelName.empty()) return;
    const quickscript::AppSettings settings = LoadAgentAppSettings();
    action.aiModelName = ResolveAiModelName(
        settings.ai, ActionRequiresVisionModel(action), L"");
}

std::wstring ResolveActionAiModelName(const ScriptAction& action,
    const quickscript::AiApiSettings& ai) {
    if (!action.aiModelName.empty()) return action.aiModelName;
    return ResolveAiModelName(ai, ActionRequiresVisionModel(action), L"");
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
