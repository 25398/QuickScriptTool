#pragma once
// ──────────────────────────────────────────────────────────────────
// agent_script_ops.h — AI 助手统一的脚本/录制读写、创建、优化、删除
// ──────────────────────────────────────────────────────────────────

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "script_types.h"

struct AgentScriptOpResult {
    bool ok = false;
    std::wstring message;
};

struct AgentOptimizeOptions {
    std::wstring fileName;
    std::wstring dir;
    std::wstring outputFileName;
    std::wstring outputDir;
    std::wstring mergeMode = L"merge";
    std::wstring waitCalculation = L"sum";
    double distanceThreshold = 5.0;
    double compressWait = 0.05;
    /// waitCalculation 为 fixed/custom/specified 时使用
    double mergeWaitValue = 0.1;
};

/// 粗略版动作说明（首轮阅读）：每个动作一行 = 动作名 + 类型级语义
/// （按钮/跟随动作/循环方向等）+ 备注，不含坐标/时长/阈值等数值参数，
/// 让 AI 先做模糊理解。需要具体参数时用 DescribeScriptActionsDetail。
std::wstring DescribeScriptActionsBrief(const std::vector<ScriptAction>& actions,
    size_t startIndex, size_t maxActions, size_t& outShown);

/// 详细版动作说明（按需查询）：每个动作一行，包含关键参数
/// （图片/坐标/条件/循环/间隔/修饰键/容差/AI 超时等），省略默认值。
std::wstring DescribeScriptActionsDetail(const std::vector<ScriptAction>& actions,
    size_t startIndex, size_t maxActions, size_t& outShown);

/// 保存完整 JSON 内容（覆盖）
AgentScriptOpResult AgentSaveScriptContent(const std::wstring& fileName,
    const std::wstring& content, const std::wstring& dirHint = L"scripts");

/// 用 buildScriptActions 逻辑创建鼠标宏（scripts 目录）
AgentScriptOpResult AgentCreateMacroScript(const std::wstring& fileName,
    const std::wstring& scriptName, const std::vector<nlohmann::json>& actions,
    const nlohmann::json& extraParams = {});

/// 优化脚本或键鼠录制
AgentScriptOpResult AgentOptimizeScriptFile(const AgentOptimizeOptions& options);

/// 删除脚本宏或键鼠录制
AgentScriptOpResult AgentDeleteScriptFile(const std::wstring& fileName,
    const std::wstring& dirHint);
