#pragma once
// ──────────────────────────────────────────────────────────────────
// script_action_builder.h — 从参数构建 ScriptAction（与手动编辑逻辑一致）
// ──────────────────────────────────────────────────────────────────

#include "script_types.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

struct ScriptActionBuildResult {
    bool ok = false;
    std::wstring error;
    ScriptAction action;
};

/// 由 JSON 参数构建单个动作（字段校验与 UI 表单 ActionFromForm 一致）
ScriptActionBuildResult BuildScriptActionFromJson(const nlohmann::json& params);

/// 把嵌套 children 展开为扁平列表并写入 indent（子动作 indent=父级+1，忽略子项自带 indent）。
/// 仅 loop/if/else/defineBlock 允许 children。失败时 flat 被清空。
bool FlattenNestedActionParamList(const std::vector<nlohmann::json>& nested,
    std::vector<nlohmann::json>& flat, std::wstring& error);

/// 规划动作树：展开 children、校验空容器/endLoop，返回带缩进的中文树。
/// 不校验必填参数、不追加 stopMacro、不保存。供 AI 先核对父子再补齐细节。
std::wstring PlanScriptActionsOutline(const std::vector<nlohmann::json>& nested,
    std::wstring& error);

/// 批量构建，返回 actions 数组 JSON 文本（不含外层 script 结构）。
/// validateContainerBodies：完整脚本必须为 true（空循环会失败）。
/// AI 动作执行逐条合并时传 false，合并后再校验整批树。
std::wstring BuildScriptActionsJsonArray(const std::vector<nlohmann::json>& actionParams,
    std::wstring& error, bool validateContainerBodies = true);

/// 各动作 type 及参数字段说明（供 AI 工具描述引用）
std::wstring ScriptActionBuilderSchema();

/// 简短动作目录（嵌入 system prompt，不含参数字段）
std::wstring ScriptActionCatalog();

/// 按 type（如 keyClick）或 section（mouse|keyboard|flow|findImage|ocr|system|ai|all）查参数字段
std::wstring LookupMacroActionSchema(const std::wstring& typeOrSection);

/// 规范动作序号（1..n）并清除非 customText 类型上误写的 text/customText
void NormalizeScriptActionList(std::vector<ScriptAction>& actions);

/// AI 构建路径：无 stopMacro 且非顶层无限循环脚本时，末尾自动追加 stopMacro；返回是否追加
bool EnsureStopMacroOnActions(std::vector<ScriptAction>& actions);
