#pragma once
// ──────────────────────────────────────────────────────────────────
// agent_script_ops.h — AI 助手统一的脚本/录制读写、创建、优化、删除
// ──────────────────────────────────────────────────────────────────

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

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
};

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
