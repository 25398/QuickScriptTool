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

/// 判断是否为推理模型（o1/o3/o4 系列、DeepSeek R1/Reasoner、Kimi K2 Thinking、
/// Grok Reasoning 等）：这些模型不接受 temperature 等采样参数，需在请求层自适应。
bool ModelIsReasoningType(const std::wstring& modelName);

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

/// 视觉子任务模型解析：主模型支持识图则用它；否则从 savedModels 选第一个识图模型；
/// 都不可用时返回空（调用方应提示配置多模态模型）。
/// 用途：AI 动作执行主模型为纯文本模型时，locateAndClick/VisionQuery 等
/// 识图子任务自动路由到已保存的多模态模型，实现「文本主模型 + 视觉模型」长任务协作。
std::wstring ResolveVisionSubtaskModelName(const quickscript::AiApiSettings& ai,
    const std::wstring& primaryModel);

/// 主模型本身或 savedModels 中是否已有可用识图模型
bool HasUsableVisionModel(const quickscript::AiApiSettings& ai,
    const std::wstring& primaryModel);

/// 无可用识图模型时的统一错误（调用方应终止/跳过本步，勿仅打警告后继续）
std::wstring MissingVisionModelError(const std::wstring& primaryModel);

/// 规划轮是否应把观察截图附给当前 Agent 模型。
/// 纯文本主模型收图会导致网关挂起/超时（deepseek-flash 等）；观察结论用文字，
/// 点击交给 locateAndClick（识图子模型）。
bool ShouldAttachObserveImageToPlanner(const std::wstring& plannerModel, bool haveImage);
