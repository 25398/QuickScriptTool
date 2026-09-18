#pragma once

#include "window_mode_types.h"

#include <string>

namespace windowmode {

WindowModeScriptConfig ParseWindowModeJson(const std::wstring& content);
WindowModeScriptConfig ParseWindowModeConfigObject(const std::wstring& block,
    bool sanitizeDisabled = true);
WindowModeScriptConfig DefaultWindowModeConfig();
std::wstring WindowModeConfigSummary(const WindowModeScriptConfig& cfg);
void WriteWindowModeJson(std::wstring& out, const WindowModeScriptConfig& cfg, bool trailingComma);
/// 动作级 nestedWindowMode：不因 enabled=0 清掉窗口身份（useMode 才是是否启用的来源）
void WriteNestedWindowModeJson(std::wstring& out, const WindowModeScriptConfig& cfg,
    bool trailingComma);
/// 窗口相对脚本：补 coordSpace。
/// reviveEnabled：仅录制保存/录制回放把 enabled=0 + 窗口身份复活为后台窗口模式。
/// 鼠标宏明确保存为默认模式（enabled=0）时不得复活。
void FinalizeWindowModeForPlayback(WindowModeScriptConfig& cfg,
    bool anyWindowRelativeAction = false, bool reviveEnabled = false);

}  // namespace windowmode
