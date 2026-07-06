#include "macro_execute_tools.h"

#include "agent_ai_actions.h"
#include "script_action_builder.h"
#include "utils.h"

#include <string>
#include <vector>

namespace {

using json = nlohmann::json;

json ParseToolParams(const std::wstring& paramsJson, std::wstring& error) {
    try {
        return json::parse(ToUtf8(paramsJson));
    } catch (const json::parse_error&) {
        error = L"[错误] 参数 JSON 解析失败。";
        return json::object();
    }
}

bool PrepareAiModelFields(json& params) {
    if (!params.contains("type") || !params["type"].is_string()) return true;
    const std::string type = params["type"].get<std::string>();
    bool requireVision = false;
    if (type == "aiImageAnalysis") requireVision = true;
    else if (type == "aiActionExecute") {
        if (params.contains("aiWithImage") && params["aiWithImage"].is_boolean())
            requireVision = params["aiWithImage"].get<bool>();
        else if (params.contains("aiWithImage") && params["aiWithImage"].is_number_integer())
            requireVision = params["aiWithImage"].get<int>() != 0;
        else
            requireVision = true;
    } else if (type != "aiTextAnalysis") {
        return true;
    }

    if (ApplyResolvedAiModelToActionParams(params)) return true;

    const quickscript::AppSettings settings = LoadAgentAppSettings();
    const std::wstring preferred = FromUtf8(params.value("aiModelName", ""));
    const std::wstring resolved = ResolveAiModelName(settings.ai, requireVision, preferred);
    if (Trim(resolved).empty()) return false;
    params["aiModelName"] = ToUtf8(resolved);
    return true;
}

AgentTool MakeLookupMacroActionTool() {
    AgentTool tool;
    tool.name = L"lookupMacroAction";
    tool.description =
        L"按需查阅宏动作参数字段。type 填动作名（如 keyClick、findImage）或 section"
        L"（mouse|keyboard|flow|findImage|ocr|system|ai|all）。目录已在 system prompt 中。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "type": {
                "type": "string",
                "description": "动作 type 或 section：mouse|keyboard|flow|findImage|ocr|system|ai|all|catalog"
            }
        },
        "required": ["type"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        std::wstring query;
        if (params.contains("type") && params["type"].is_string())
            query = FromUtf8(params["type"].get<std::string>());
        return LookupMacroActionSchema(query);
    };
    return tool;
}

AgentTool MakeSubmitMacroActionsToolLocal() {
    AgentTool tool;
    tool.name = L"submitMacroActions";
    tool.description =
        L"提交本批次要立刻执行的宏动作。actions 数组每项含 type 及参数。"
        L"不确定字段时先 lookupMacroAction，禁止在文字回复中手写 JSON。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "actions": {
                "type": "array",
                "description": "动作对象数组，每项至少含 type",
                "items": {
                    "type": "object",
                    "properties": { "type": { "type": "string" } },
                    "required": ["type"],
                    "additionalProperties": true
                }
            }
        },
        "required": ["actions"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;

        if (!params.contains("actions") || !params["actions"].is_array())
            return L"[错误] 缺少 actions 数组。目录见 system prompt；细节用 lookupMacroAction。\n\n"
                + ScriptActionCatalog();

        std::vector<json> items;
        for (const auto& item : params["actions"]) {
            if (item.is_object()) items.push_back(item);
        }
        if (items.empty())
            return L"[错误] actions 数组为空。";

        nlohmann::json merged = nlohmann::json::array();
        for (size_t i = 0; i < items.size(); ++i) {
            json one = items[i];
            if (!one.contains("type") || !one["type"].is_string())
                return L"[错误] 第 " + std::to_wstring(i + 1) + L" 个动作缺少 type。";
            if (!PrepareAiModelFields(one))
                return L"[错误] 第 " + std::to_wstring(i + 1)
                    + L" 个 AI 动作未配置可用模型。";
            std::wstring err;
            const std::wstring single = BuildScriptActionsJsonArray({one}, err);
            if (!err.empty())
                return L"[错误] 第 " + std::to_wstring(i + 1) + L" 个动作：" + err;
            try {
                const json parsed = json::parse(ToUtf8(single));
                if (parsed.is_array()) {
                    for (const auto& step : parsed) merged.push_back(step);
                }
            } catch (...) {
                return L"[错误] 第 " + std::to_wstring(i + 1) + L" 个动作 JSON 解析失败。";
            }
        }
        return FromUtf8(merged.dump());
    };
    return tool;
}

}  // namespace

std::wstring BuildAndValidateMacroActionJson(const json& params) {
    if (!params.is_object() || !params.contains("type") || !params["type"].is_string())
        return L"[错误] 缺少 type 字段。";

    json copy = params;
    if (!PrepareAiModelFields(copy))
        return L"[错误] 未配置可用 AI 模型。请先在「设置→AI助手」中添加模型。";

    std::wstring error;
    const std::wstring jsonArray = BuildScriptActionsJsonArray({copy}, error);
    if (!error.empty()) return L"[错误] " + error;
    return jsonArray;
}

bool IsMacroExecutionToolName(const std::wstring& name) {
    return name == L"submitMacroActions" || name == L"lookupMacroAction";
}

std::vector<AgentTool> BuildAiActionExecuteTools() {
    return {
        MakeSubmitMacroActionsToolLocal(),
        MakeLookupMacroActionTool(),
    };
}

std::vector<AgentTool> BuildAiActionExecuteToolsFull() {
    return BuildAiActionExecuteTools();
}
