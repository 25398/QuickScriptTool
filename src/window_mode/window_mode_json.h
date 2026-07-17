#pragma once

#include "window_mode_types.h"

#include <string>

namespace windowmode {

WindowModeScriptConfig ParseWindowModeJson(const std::wstring& content);
WindowModeScriptConfig ParseWindowModeConfigObject(const std::wstring& block);
WindowModeScriptConfig DefaultWindowModeConfig();
std::wstring WindowModeConfigSummary(const WindowModeScriptConfig& cfg);
void WriteWindowModeJson(std::wstring& out, const WindowModeScriptConfig& cfg, bool trailingComma);

}  // namespace windowmode
