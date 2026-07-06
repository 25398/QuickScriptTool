#pragma once
// ──────────────────────────────────────────────────────────────────
// agent_ai_actions.h — AI 动作工具辅助：模型选择与动作构建
// ──────────────────────────────────────────────────────────────────

#include "app_settings.h"
#include "script_types.h"

#include <nlohmann/json.hpp>
#include <string>

/// 读取应用设置（文件不存在时仍返回默认值，不视为致命错误）
quickscript::AppSettings LoadAgentAppSettings();

/// 根据模型名称启发式判断是否支持识图（多模态）
bool ModelSupportsVision(const std::wstring& modelName);

/// 从已保存模型中解析可用模型名；requireVision 时优先返回识图模型
std::wstring ResolveAiModelName(const quickscript::AiApiSettings& ai,
    bool requireVision, const std::wstring& preferred = L"");

/// 列出已添加模型及是否支持识图
std::wstring FormatAvailableAiModelsList(const quickscript::AiApiSettings& ai);

/// 构建单个动作并返回与 buildScriptActions 相同格式的结果文本
std::wstring BuildSingleAgentActionResult(const nlohmann::json& actionParams,
    const std::wstring& note = L"");

/// 为 JSON 动作参数补全 aiModelName（createMacroScript / buildScriptActions 路径）
bool ApplyResolvedAiModelToActionParams(nlohmann::json& params);

/// 为已构建的 ScriptAction 补全 aiModelName
void EnsureAiModelOnAction(ScriptAction& action);

/// 解析动作应使用的模型名（空时从 savedModels 选取）
std::wstring ResolveActionAiModelName(const ScriptAction& action,
    const quickscript::AiApiSettings& ai);
