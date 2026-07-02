#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "image_match.h"
#include "script_types.h"

struct QuickInputVarItem {
    std::wstring display;
    std::wstring insertText;
    std::wstring tooltip;
    std::wstring codeHint;
};

struct MacroVariableContext {
    const std::unordered_map<std::wstring, ImageMatchResult>* matchVars = nullptr;
    const std::unordered_map<std::wstring, int>* loopVars = nullptr;
    int curLoops = 0;
};

std::vector<QuickInputVarItem> BuildQuickInputVarItems(const std::vector<ScriptAction>& actions);
std::wstring ResolveMacroVariables(const std::wstring& text, const MacroVariableContext& ctx);
std::wstring DecodeQuickInputEscapes(const std::wstring& text);
