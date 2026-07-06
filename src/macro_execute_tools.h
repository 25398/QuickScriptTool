#pragma once
// ──────────────────────────────────────────────────────────────────
// macro_execute_tools.h — AI 动作执行运行时工具（每个宏动作对应一个 API 接口）
// ──────────────────────────────────────────────────────────────────

#include "agent_core.h"

#include <string>
#include <vector>

/// 构建并校验单个宏动作，返回纯 JSON 数组（执行格式）；失败返回 [错误] 前缀文本
std::wstring BuildAndValidateMacroActionJson(const nlohmann::json& params);

/// 是否为宏动作执行工具名（macro* / submitMacroActions）
bool IsMacroExecutionToolName(const std::wstring& name);

/// API 运行时工具：submitMacroActions + lookupMacroAction（Skill 按需查参）
std::vector<AgentTool> BuildAiActionExecuteTools();

/// 与 BuildAiActionExecuteTools 相同（保留扩展入口）
std::vector<AgentTool> BuildAiActionExecuteToolsFull();
