#pragma once
// ──────────────────────────────────────────────────────────────────
// script_io.h — 脚本 JSON 读写（与鼠标宏/录制导入导出格式一致）
// ──────────────────────────────────────────────────────────────────

#include "script_types.h"
#include "utils.h"

#include <sstream>
#include <string>
#include <vector>

struct ScriptFileData {
    std::wstring scriptName;
    std::wstring recordTime;
    double durationSeconds = 0;
    Hotkey hotkey{};
    std::vector<ScriptAction> actions;
};

ScriptAction ParseScriptActionBlock(const std::wstring& block, size_t fallbackNo);
void WriteActionJson(std::wstringstream& file, const ScriptAction& a, bool last);
std::wstring ScriptActionToJsonString(const ScriptAction& a);
ScriptFileData LoadScriptFileData(const std::wstring& path);
ScriptFileData ParseScriptContent(const std::wstring& content);
bool SaveScriptFileData(const std::wstring& path, const ScriptFileData& data);
