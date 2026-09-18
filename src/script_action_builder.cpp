#include "script_action_builder.h"

#include "action_tree.h"
#include "action_utils.h"
#include "agent_ai_actions.h"
#include "app_settings.h"
#include "ai_action_router.h"
#include "color_match.h"
#include "image_var_util.h"
#include "script_io.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <sstream>

namespace {

using json = nlohmann::json;

std::wstring JsonWString(const json& p, const char* key, const std::wstring& def = L"") {
    if (!p.contains(key)) return def;
    if (p[key].is_string()) return FromUtf8(p[key].get<std::string>());
    if (p[key].is_number_integer()) return std::to_wstring(p[key].get<int64_t>());
    if (p[key].is_number_float()) return std::to_wstring(p[key].get<double>());
    if (p[key].is_boolean()) return p[key].get<bool>() ? L"1" : L"0";
    return def;
}

bool JsonBool(const json& p, const char* key, bool def = false) {
    if (!p.contains(key)) return def;
    if (p[key].is_boolean()) return p[key].get<bool>();
    if (p[key].is_number()) return p[key].get<int>() != 0;
    if (p[key].is_string()) {
        const std::wstring s = FromUtf8(p[key].get<std::string>());
        return s == L"1" || s == L"true" || s == L"True";
    }
    return def;
}

int JsonInt(const json& p, const char* key, int def = 0) {
    if (!p.contains(key)) return def;
    if (p[key].is_number_integer()) return static_cast<int>(p[key].get<int64_t>());
    if (p[key].is_number_float()) return static_cast<int>(p[key].get<double>());
    if (p[key].is_string()) {
        wchar_t* end = nullptr;
        const std::wstring s = FromUtf8(p[key].get<std::string>());
        const long v = wcstol(s.c_str(), &end, 10);
        return end != s.c_str() ? static_cast<int>(v) : def;
    }
    return def;
}

double JsonDouble(const json& p, const char* key, double def = 0.0) {
    if (!p.contains(key)) return def;
    if (p[key].is_number()) return p[key].get<double>();
    if (p[key].is_string()) {
        wchar_t* end = nullptr;
        const std::wstring s = FromUtf8(p[key].get<std::string>());
        const double v = wcstod(s.c_str(), &end);
        return end != s.c_str() ? v : def;
    }
    return def;
}

int ParseWatchModeJson(const json& p) {
    const std::wstring modeStr = JsonWString(p, "watchMode");
    if (modeStr == L"time" || modeStr == L"timed" || modeStr == L"interval"
        || modeStr == L"1" || modeStr == L"true") {
        return 1;
    }
    if (modeStr == L"action" || modeStr == L"0" || modeStr == L"false") {
        return 0;
    }
    return JsonInt(p, "watchMode", 0) != 0 ? 1 : 0;
}

bool ParseActionType(const std::wstring& type, ActionType& out) {
    static const struct { const wchar_t* name; ActionType t; } kMap[] = {
        {L"moveMouse", ActionType::MoveMouse},
        {L"mouseMove", ActionType::MoveMouse},
        {L"moveMouseRelative", ActionType::MoveMouseRelative},
        {L"mouseMoveRelative", ActionType::MoveMouseRelative},
        {L"relativeMouseMove", ActionType::MoveMouseRelative},
        {L"wait", ActionType::Wait},
        {L"mouseClick", ActionType::MouseClick},
        {L"mouseDrag", ActionType::MouseDrag},
        {L"mouseDown", ActionType::MouseDown},
        {L"mouseUp", ActionType::MouseUp},
        {L"keyClick", ActionType::KeyClick},
        {L"keyDown", ActionType::KeyDown},
        {L"keyUp", ActionType::KeyUp},
        {L"hotkeyShortcut", ActionType::HotkeyShortcut},
        {L"quickInput", ActionType::QuickInput},
        {L"scrollWheel", ActionType::ScrollWheel},
        {L"loop", ActionType::Loop},
        {L"endLoop", ActionType::EndLoop},
        {L"defineBlock", ActionType::DefineBlock},
        {L"runBlock", ActionType::RunBlock},
        {L"runMacro", ActionType::RunMacro},
        {L"mousePlayback", ActionType::MousePlayback},
        {L"findImage", ActionType::FindImage},
        {L"multiMatch", ActionType::MultiMatch},
        {L"watchImage", ActionType::WatchImage},
        {L"varCompute", ActionType::VarCompute},
        {L"textRecognition", ActionType::TextRecognition},
        {L"if", ActionType::If},
        {L"else", ActionType::Else},
        {L"lockScreenshot", ActionType::LockScreenshot},
        {L"unlockScreenshot", ActionType::UnlockScreenshot},
        {L"stopMacro", ActionType::StopMacro},
        {L"goto", ActionType::Goto},
        {L"runProgram", ActionType::RunProgram},
        {L"closeProgram", ActionType::CloseProgram},
        {L"openWebpage", ActionType::OpenWebpage},
        {L"openFile", ActionType::OpenFile},
        {L"activateWindow", ActionType::ActivateWindow},
        {L"timerRecordTime", ActionType::TimerRecordTime},
        {L"getCursorPos", ActionType::GetCursorPos},
        {L"getColor", ActionType::GetColor},
        {L"findColor", ActionType::FindColor},
        {L"colorMatch", ActionType::ColorMatch},
        {L"customText", ActionType::CustomText},
        {L"aiTextAnalysis", ActionType::AiTextAnalysis},
        {L"aiImageAnalysis", ActionType::AiImageAnalysis},
        {L"aiActionExecute", ActionType::AiActionExecute},
    };
    for (const auto& item : kMap) {
        if (type == item.name) {
            out = item.t;
            return true;
        }
    }
    return false;
}

MouseButtonType ParseButton(const json& p) {
    const std::wstring btn = JsonWString(p, "button", L"left");
    if (btn == L"right") return MouseButtonType::Right;
    if (btn == L"middle") return MouseButtonType::Middle;
    if (btn == L"x1") return MouseButtonType::X1;
    if (btn == L"x2") return MouseButtonType::X2;
    return MouseButtonType::Left;
}

int ParseFollowUpValue(const json& p, const char* intKey, int def = 0) {
    if (p.contains("followUp")) {
        if (p["followUp"].is_string()) {
            const std::wstring s = FromUtf8(p["followUp"].get<std::string>());
            if (s == L"click" || s == L"clickEach" || s == L"clickAll") return 0;
            if (s == L"move" || s == L"moveFirst") return 1;
            if (s == L"saveVar" || s == L"saveVariable" || s == L"save"
                || s == L"saveMatch" || s == L"saveMatchScore"
                || s == L"saveAll" || s == L"saveMatches") return 2;
            if (s == L"saveImage" || s == L"saveImg") return 3;
        } else if (p["followUp"].is_number_integer()) {
            return std::clamp(p["followUp"].get<int>(), 0, 3);
        }
    }
    if (intKey && p.contains(intKey))
        return std::clamp(JsonInt(p, intKey, def), 0, 3);
    return def;
}

void ApplyImageLocateFields(ScriptAction& action, const json& p, bool includeSearchRegion) {
    action.imageLocate = JsonBool(p, "imageLocate");
    if (!action.imageLocate) return;
    if (includeSearchRegion) {
        action.searchX1 = JsonInt(p, "searchX1");
        action.searchY1 = JsonInt(p, "searchY1");
        action.searchX2 = JsonInt(p, "searchX2");
        action.searchY2 = JsonInt(p, "searchY2");
        action.searchFullScreen = JsonBool(p, "searchFullScreen", true);
    }
    action.imageUseVar = JsonBool(p, "imageUseVar");
    action.imagePath = Trim(JsonWString(p, "imagePath"));
    action.matchThreshold = std::clamp(JsonDouble(p, "matchThreshold", 65.0), 1.0, 100.0);
    action.perfectMatch = JsonBool(p, "perfectMatch");
    action.imageScaleMin = std::max(0.1, JsonDouble(p, "imageScaleMin", 1.0));
    action.imageScaleMax = std::max(action.imageScaleMin,
        JsonDouble(p, "imageScaleMax", action.imageScaleMin));
    action.imageScale = JsonDouble(p, "imageScale",
        (action.imageScaleMin + action.imageScaleMax) * 0.5);
    action.findTimeExpr = JsonWString(p, "findTimeExpr", L"0");
    if (action.findTimeExpr.empty()) action.findTimeExpr = L"0";
}

void ApplyModifierFields(ScriptAction& action, const json& p) {
    if (p.contains("modifiers") && p["modifiers"].is_array()) {
        for (const auto& item : p["modifiers"]) {
            if (!item.is_string()) continue;
            const std::wstring m = FromUtf8(item.get<std::string>());
            if (m == L"ctrl") action.holdLeftCtrl = true;
            else if (m == L"alt") action.holdLeftAlt = true;
            else if (m == L"shift") action.holdLeftShift = true;
            else if (m == L"win") action.holdLeftWin = true;
        }
    }
    action.holdLeftWin = JsonBool(p, "holdLeftWin", action.holdLeftWin);
    action.holdRightWin = JsonBool(p, "holdRightWin", action.holdRightWin);
    action.holdLeftCtrl = JsonBool(p, "holdLeftCtrl", action.holdLeftCtrl);
    action.holdRightCtrl = JsonBool(p, "holdRightCtrl", action.holdRightCtrl);
    action.holdLeftAlt = JsonBool(p, "holdLeftAlt", action.holdLeftAlt);
    action.holdRightAlt = JsonBool(p, "holdRightAlt", action.holdRightAlt);
    action.holdLeftShift = JsonBool(p, "holdLeftShift", action.holdLeftShift);
    action.holdRightShift = JsonBool(p, "holdRightShift", action.holdRightShift);
}

/// 解析 keyText → 虚拟键码。
/// 多字符名大小写不敏感；无法识别时返回 0（禁止回落首字母——
/// 旧逻辑会把 Home→H、Up→U、Right→R，在 Excel 填表时污染单元格）。
UINT ResolveKeyVk(const json& p, const std::wstring& keyText) {
    if (p.contains("keyVk") && p["keyVk"].is_number_integer()) {
        const int v = static_cast<int>(p["keyVk"].get<int64_t>());
        const UINT raw = v > 0 ? static_cast<UINT>(v) : 0;
        const UINT normalized = NormalizeScriptKeyVk(raw, keyText);
        if (normalized) return normalized;
        if (raw && raw < 256) return raw;
        // keyVk=0 或误存的超范围码：继续按 keyText 解析
    }
    if (keyText.empty()) return 0;
    if (keyText.size() == 1) {
        if (const UINT named = VirtualKeyFromKeyText(keyText)) return named;
        const UINT ch = static_cast<UINT>(towupper(keyText[0]));
        return ch < 256 ? ch : 0;
    }

    std::wstring upper = keyText;
    for (auto& c : upper) {
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    }
    // 去空白：模型偶发传 "Home " / "Page Down"
    while (!upper.empty() && (upper.back() == L' ' || upper.back() == L'\t')) upper.pop_back();
    size_t lead = 0;
    while (lead < upper.size() && (upper[lead] == L' ' || upper[lead] == L'\t')) ++lead;
    if (lead) upper.erase(0, lead);
    for (size_t i = 0; i < upper.size();) {
        if (upper[i] == L' ' || upper[i] == L'\t' || upper[i] == L'_' || upper[i] == L'-')
            upper.erase(i, 1);
        else
            ++i;
    }

    if (upper == L"ENTER" || upper == L"RETURN") return VK_RETURN;
    if (upper == L"TAB") return VK_TAB;
    if (upper == L"SPACE" || upper == L"SPACEBAR") return VK_SPACE;
    if (upper == L"ESCAPE" || upper == L"ESC") return VK_ESCAPE;
    if (upper == L"BACKSPACE" || upper == L"BS" || upper == L"BACK") return VK_BACK;
    if (upper == L"DELETE" || upper == L"DEL") return VK_DELETE;
    if (upper == L"HOME") return VK_HOME;
    if (upper == L"END") return VK_END;
    if (upper == L"LEFT" || upper == L"ARROWLEFT") return VK_LEFT;
    if (upper == L"RIGHT" || upper == L"ARROWRIGHT") return VK_RIGHT;
    if (upper == L"UP" || upper == L"ARROWUP") return VK_UP;
    if (upper == L"DOWN" || upper == L"ARROWDOWN") return VK_DOWN;
    if (upper == L"PRIOR" || upper == L"PAGEUP" || upper == L"PGUP") return VK_PRIOR;
    if (upper == L"NEXT" || upper == L"PAGEDOWN" || upper == L"PGDN" || upper == L"PGDOWN")
        return VK_NEXT;
    if (upper == L"INSERT" || upper == L"INS") return VK_INSERT;
    if (upper == L"APPS" || upper == L"MENU" || upper == L"CONTEXTMENU") return VK_APPS;
    if (upper == L"PRINTSCREEN" || upper == L"PRTSC" || upper == L"SNAPSHOT") return VK_SNAPSHOT;
    if (upper == L"SCROLLLOCK" || upper == L"SCROLL") return VK_SCROLL;
    if (upper == L"PAUSE" || upper == L"BREAK") return VK_PAUSE;
    if (upper == L"CAPITAL" || upper == L"CAPSLOCK" || upper == L"CAPS") return VK_CAPITAL;
    if (upper == L"NUMLOCK") return VK_NUMLOCK;
    if (upper.size() == 7 && upper.rfind(L"NUMPAD", 0) == 0
        && upper[6] >= L'0' && upper[6] <= L'9') {
        return static_cast<UINT>(VK_NUMPAD0 + (upper[6] - L'0'));
    }
    if (upper.size() >= 2 && upper[0] == L'F') {
        wchar_t* end = nullptr;
        const long n = wcstol(upper.c_str() + 1, &end, 10);
        if (end && *end == L'\0' && n >= 1 && n <= 24)
            return static_cast<UINT>(VK_F1 + n - 1);
    }
    return 0;
}

void ApplyAiCommonFields(ScriptAction& action, const json& p) {
    action.aiPrompt = JsonWString(p, "aiPrompt");
    action.aiOutputVarName = Trim(JsonWString(p, "aiOutputVarName"));
    if (action.aiOutputVarName.empty()) {
        if (action.type == ActionType::AiImageAnalysis) action.aiOutputVarName = L"aiImgResult";
        else if (action.type == ActionType::AiTextAnalysis) action.aiOutputVarName = L"aiResult";
    }
    action.aiOutputType = std::clamp(JsonInt(p, "aiOutputType", 0), 0, 1);
    action.aiModelName = JsonWString(p, "aiModelName");
    action.aiContextMode = std::clamp(JsonInt(p, "aiContextMode", 1), 0, 3);
    action.aiTimeoutSec = std::max(5, JsonInt(p, "aiTimeoutSec", 30));
    action.aiFallbackValue = Trim(JsonWString(p, "aiFallbackValue"));
    action.aiImageScale = std::clamp(JsonDouble(p, "aiImageScale", 0.5), 0.1, 1.0);
    action.aiRegionByImage = JsonBool(p, "aiRegionByImage");
    action.aiImageUseVar = JsonBool(p, "aiImageUseVar");
    action.aiTargetImagePath = Trim(JsonWString(p, "aiTargetImagePath"));
    action.aiSearchX1 = JsonInt(p, "aiSearchX1");
    action.aiSearchY1 = JsonInt(p, "aiSearchY1");
    action.aiSearchX2 = JsonInt(p, "aiSearchX2");
    action.aiSearchY2 = JsonInt(p, "aiSearchY2");
    action.imageRegionX1 = JsonInt(p, "imageRegionX1");
    action.imageRegionY1 = JsonInt(p, "imageRegionY1");
    action.imageRegionX2 = JsonInt(p, "imageRegionX2");
    action.imageRegionY2 = JsonInt(p, "imageRegionY2");
    if (action.aiRegionByImage) {
        action.matchThreshold = std::clamp(JsonDouble(p, "matchThreshold", 65.0), 1.0, 100.0);
        action.imageScaleMin = std::max(0.1, JsonDouble(p, "imageScaleMin", 0.9));
        action.imageScaleMax = std::max(action.imageScaleMin,
            JsonDouble(p, "imageScaleMax", 1.1));
        action.imageScale = (action.imageScaleMin + action.imageScaleMax) * 0.5;
    }
    action.aiMaxSteps = JsonInt(p, "aiMaxSteps", 10);
    action.aiWithImage = JsonBool(p, "aiWithImage", true);
    action.aiLogicConvert = JsonBool(p, "aiLogicConvert", false);
    action.aiLogicBlockName = Trim(JsonWString(p, "aiLogicBlockName"));
}

void ApplyRepeatTiming(ScriptAction& action, const json& p,
    int defaultClickCount = 1, double defaultDuration = 0.01) {
    action.clickCount = std::max(1, JsonInt(p, "clickCount", defaultClickCount));
    action.duration = std::max(0.0, JsonDouble(p, "duration", defaultDuration));
    action.randomDuration = std::max(0.0, JsonDouble(p, "randomDuration", 0.0));
}

bool ValidateVarName(const std::wstring& name, const wchar_t* label, std::wstring& error) {
    if (name.empty()) return true;
    if (IsValidBlockName(name)) return true;
    error = std::wstring(label) + L"「" + name + L"」不合法：须英文字母开头，仅含字母数字。";
    return false;
}

void ApplyCommonFields(ScriptAction& action, const json& p) {
    action.remark = JsonWString(p, "remark");
    action.indent = std::max(0, JsonInt(p, "indent", 0));
    if (action.type == ActionType::CustomText) {
        action.customText = JsonWString(p, "text");
        if (action.customText.empty()) action.customText = JsonWString(p, "customText");
    }
}

void SanitizeScriptActionDisplay(ScriptAction& action) {
    if (action.type == ActionType::CustomText) return;
    if (action.type == ActionType::EndLoop) {
        action.customText = L"跳出循环";
        return;
    }
    action.customText.clear();
}

ScriptActionBuildResult Fail(const std::wstring& msg) {
    ScriptActionBuildResult r;
    r.error = msg;
    return r;
}

ScriptActionBuildResult BuildTypedAction(ActionType type, const json& p) {
    ScriptAction action{};
    action.type = type;
    ApplyCommonFields(action, p);

    switch (type) {
    case ActionType::MoveMouse:
        action.moveFromVar = JsonBool(p, "moveFromVar");
        action.moveVarExprX = Trim(JsonWString(p, "moveVarExprX"));
        action.moveVarExprY = Trim(JsonWString(p, "moveVarExprY"));
        action.x = JsonInt(p, "x");
        action.y = JsonInt(p, "y");
        action.randomX = std::max(0, JsonInt(p, "randomX"));
        action.randomY = std::max(0, JsonInt(p, "randomY"));
        action.duration = 0.0;
        action.timingUs = 0;
        action.randomDuration = 0.0;
        break;

    case ActionType::MoveMouseRelative:
        // x/y = dx/dy 像素（可负），回放走 SendInput 相对移动
        action.x = JsonInt(p, "x", JsonInt(p, "dx"));
        action.y = JsonInt(p, "y", JsonInt(p, "dy"));
        action.randomX = std::max(0, JsonInt(p, "randomX"));
        action.randomY = std::max(0, JsonInt(p, "randomY"));
        action.coordsAreNormalized = false;
        action.duration = 0.0;
        action.timingUs = 0;
        action.randomDuration = 0.0;
        break;

    case ActionType::Wait:
        action.duration = std::max(0.0, JsonDouble(p, "duration", JsonDouble(p, "seconds", 0.0)));
        action.randomDuration = std::max(0.0, JsonDouble(p, "randomDuration", 0.0));
        action.timingUs = static_cast<uint64_t>(std::max(0.0, JsonDouble(p, "timingUs", 0.0)));
        if (action.timingUs == 0 && action.duration > 0.0) {
            const long double us = static_cast<long double>(action.duration) * 1000000.0L;
            action.timingUs = static_cast<uint64_t>(std::llround(us));
        } else if (action.timingUs > 0) {
            action.duration = action.timingUs / 1000000.0;
        }
        break;

    case ActionType::MouseClick:
        action.button = ParseButton(p);
        action.x = JsonInt(p, "x");
        action.y = JsonInt(p, "y");
        ApplyModifierFields(action, p);
        ApplyRepeatTiming(action, p, 1, 0.01);
        break;

    case ActionType::MouseDrag:
        action.button = ParseButton(p);
        action.x = JsonInt(p, "x");
        action.y = JsonInt(p, "y");
        action.endX = JsonInt(p, "endX");
        action.endY = JsonInt(p, "endY");
        action.randomX = std::max(0, JsonInt(p, "randomX"));
        action.randomY = std::max(0, JsonInt(p, "randomY"));
        action.randomEndX = std::max(0, JsonInt(p, "randomEndX"));
        action.randomEndY = std::max(0, JsonInt(p, "randomEndY"));
        ApplyModifierFields(action, p);
        action.duration = std::max(0.0, JsonDouble(p, "duration", 0.3));
        action.randomDuration = std::max(0.0, JsonDouble(p, "randomDuration", 0.0));
        action.timingUs = 0;
        action.clickCount = 1;
        ApplyImageLocateFields(action, p, true);
        break;

    case ActionType::MouseDown:
    case ActionType::MouseUp:
        action.button = ParseButton(p);
        action.x = JsonInt(p, "x");
        action.y = JsonInt(p, "y");
        action.recordedCapturePath = Trim(JsonWString(p, "recordedCapturePath"));
        action.captureOffsetX = JsonInt(p, "captureOffsetX");
        action.captureOffsetY = JsonInt(p, "captureOffsetY");
        ApplyModifierFields(action, p);
        action.duration = 0.0;
        action.timingUs = 0;
        action.randomDuration = 0.0;
        break;

    case ActionType::KeyClick:
        action.keyText = Trim(JsonWString(p, "keyText"));
        if (action.keyText.empty())
            return Fail(L"缺少 keyText（按键名，如 Enter/F5/单字母数字）。禁止省略或依赖默认键。");
        action.keyVk = ResolveKeyVk(p, action.keyText);
        if (action.keyVk == 0) {
            return Fail(L"无法识别按键 keyText=\"" + action.keyText
                + L"\"。请用 Enter/Tab/Home/End/Left/Right/Up/Down/Delete/Backspace/"
                  L"PageUp/PageDown/Insert/F1-F24 或单字母数字；禁止省略或自造键名。");
        }
        ApplyModifierFields(action, p);
        ApplyRepeatTiming(action, p, 1, 0.01);
        break;

    case ActionType::KeyDown:
    case ActionType::KeyUp:
        action.keyText = Trim(JsonWString(p, "keyText"));
        if (action.keyText.empty())
            return Fail(L"缺少 keyText（按键名，如 Enter/F5/单字母数字）。禁止省略或依赖默认键。");
        action.keyVk = ResolveKeyVk(p, action.keyText);
        if (action.keyVk == 0) {
            return Fail(L"无法识别按键 keyText=\"" + action.keyText
                + L"\"。请用 Enter/Tab/Home/End/Left/Right/Up/Down/Delete/Backspace/"
                  L"PageUp/PageDown/Insert/F1-F24 或单字母数字。");
        }
        ApplyModifierFields(action, p);
        action.duration = 0.0;
        action.timingUs = 0;
        action.randomDuration = 0.0;
        break;

    case ActionType::HotkeyShortcut: {
        action.shortcutPreset = std::clamp(JsonInt(p, "shortcutPreset", 0), 0, ShortcutPresetCount() - 1);
        ApplyShortcutPreset(action, action.shortcutPreset);
        ApplyRepeatTiming(action, p, 1, 0.01);
        break;
    }

    case ActionType::QuickInput:
        action.inputText = JsonWString(p, "inputText");
        action.charInterval = std::max(0.0, JsonDouble(p, "charInterval", 0.01));
        action.parseEscapes = JsonBool(p, "parseEscapes", false);
        ApplyRepeatTiming(action, p, 1, 0.01);
        break;

    case ActionType::ScrollWheel:
        action.scrollVertical = JsonBool(p, "scrollVertical", true);
        action.scrollHorizontal = JsonBool(p, "scrollHorizontal", false);
        if (!action.scrollVertical && !action.scrollHorizontal) action.scrollVertical = true;
        action.scrollSteps = std::max(1, JsonInt(p, "scrollSteps", 1));
        if (p.contains("scrollDirection") && p["scrollDirection"].is_string()) {
            const std::wstring dir = FromUtf8(p["scrollDirection"].get<std::string>());
            action.scrollDirection = (dir == L"down" || dir == L"right") ? 1 : 0;
        } else {
            action.scrollDirection = std::clamp(JsonInt(p, "scrollDirection", 0), 0, 1);
        }
        ApplyRepeatTiming(action, p, 1, 0.01);
        break;

    case ActionType::Loop:
        action.loopCount = JsonInt(p, "loopCount", -1);
        action.loopFromVar = JsonBool(p, "loopFromVar");
        action.loopVarExpr = JsonWString(p, "loopVarExpr");
        action.loopVarName = Trim(JsonWString(p, "loopVarName"));
        break;

    case ActionType::EndLoop:
        action.customText = L"跳出循环";
        break;

    case ActionType::DefineBlock:
        action.blockName = Trim(JsonWString(p, "blockName"));
        break;

    case ActionType::RunBlock:
        action.blockName = Trim(JsonWString(p, "blockName"));
        ApplyRepeatTiming(action, p, 1, 0.01);
        break;

    case ActionType::RunMacro:
    case ActionType::MousePlayback:
        action.blockName = Trim(JsonWString(p, "blockName"));
        action.targetPath = Trim(JsonWString(p, "targetPath"));
        ApplyRepeatTiming(action, p, 1, 0.01);
        if (type == ActionType::MousePlayback) {
            action.playbackSpeed = quickscript::ClampPlaybackSpeed(
                JsonDouble(p, "playbackSpeed", 1.0));
        }
        if (p.contains("useMode") && p["useMode"].is_string()) {
            action.useMode = NestedUseModeFromText(FromUtf8(p["useMode"].get<std::string>()));
        } else if (p.contains("useMode")) {
            action.useMode = NormalizeNestedUseMode(JsonInt(p, "useMode", kNestedUseModeInherit));
        } else {
            action.useMode = kNestedUseModeInherit;
        }
        action.breakoutTimeSeconds = NormalizeBreakoutTimeSeconds(
            JsonDouble(p, "breakoutTimeSeconds", 0.0));
        if (p.contains("nestedWindowMode") && p["nestedWindowMode"].is_object()) {
            action.nestedWindowMode = windowmode::ParseWindowModeConfigObject(
                FromUtf8(p["nestedWindowMode"].dump()), false);
        }
        if (action.useMode == kNestedUseModeWindow || action.useMode == kNestedUseModeBackground) {
            action.nestedWindowMode.enabled = true;
            action.nestedWindowMode.executionKind = (action.useMode == kNestedUseModeBackground)
                ? windowmode::WindowModeExecutionKind::BackgroundWindow
                : windowmode::WindowModeExecutionKind::HiddenDesktop;
        }
        break;

    case ActionType::FindImage: {
        action.searchX1 = JsonInt(p, "searchX1");
        action.searchY1 = JsonInt(p, "searchY1");
        action.searchX2 = JsonInt(p, "searchX2");
        action.searchY2 = JsonInt(p, "searchY2");
        action.searchFullScreen = JsonBool(p, "searchFullScreen", true);
        action.imageUseVar = JsonBool(p, "imageUseVar");
        action.imagePath = Trim(JsonWString(p, "imagePath"));
        action.matchThreshold = std::clamp(JsonDouble(p, "matchThreshold", 65.0), 1.0, 100.0);
        action.perfectMatch = JsonBool(p, "perfectMatch");
        action.imageScaleMin = std::max(0.1, JsonDouble(p, "imageScaleMin", 1.0));
        action.imageScaleMax = std::max(action.imageScaleMin,
            JsonDouble(p, "imageScaleMax", action.imageScaleMin));
        action.imageScale = JsonDouble(p, "imageScale",
            (action.imageScaleMin + action.imageScaleMax) * 0.5);
        action.findImageFollowUp = ParseFollowUpValue(p, "findImageFollowUp", 0);
        action.offsetX = JsonInt(p, "offsetX");
        action.offsetY = JsonInt(p, "offsetY");
        action.recordedCapturePath = Trim(JsonWString(p, "recordedCapturePath"));
        action.captureOffsetX = JsonInt(p, "captureOffsetX");
        action.captureOffsetY = JsonInt(p, "captureOffsetY");
        action.imageRegionX1 = JsonInt(p, "imageRegionX1");
        action.imageRegionY1 = JsonInt(p, "imageRegionY1");
        action.imageRegionX2 = JsonInt(p, "imageRegionX2");
        action.imageRegionY2 = JsonInt(p, "imageRegionY2");
        const std::wstring defaultMatchVar =
            action.findImageFollowUp == 3 ? L"image" : L"matchRet";
        action.matchVarName = Trim(JsonWString(p, "matchVarName", defaultMatchVar));
        if (action.matchVarName.empty()) action.matchVarName = defaultMatchVar;
        action.findTimeExpr = JsonWString(p, "findTimeExpr", L"0");
        if (action.findTimeExpr.empty()) action.findTimeExpr = L"0";
        if (action.findImageFollowUp == 3 && action.imagePath.empty()) {
            action.findTimeExpr = L"0";
        }
        action.duration = 0.0;
        action.timingUs = 0;
        action.randomDuration = 0.0;
        break;
    }

    case ActionType::MultiMatch: {
        action.searchX1 = JsonInt(p, "searchX1");
        action.searchY1 = JsonInt(p, "searchY1");
        action.searchX2 = JsonInt(p, "searchX2");
        action.searchY2 = JsonInt(p, "searchY2");
        action.searchFullScreen = JsonBool(p, "searchFullScreen", true);
        action.imageUseVar = JsonBool(p, "imageUseVar");
        action.imagePaths.clear();
        action.imageUseVars.clear();
        if (p.contains("imagePaths") && p["imagePaths"].is_array()) {
            for (const auto& item : p["imagePaths"]) {
                if (!item.is_string()) continue;
                action.imagePaths.push_back(Trim(FromUtf8(item.get<std::string>())));
            }
        }
        if (action.imagePaths.empty()) {
            const std::wstring one = Trim(JsonWString(p, "imagePath"));
            if (!one.empty()) action.imagePaths.push_back(one);
        }
        if (p.contains("imageUseVars") && p["imageUseVars"].is_array()) {
            for (const auto& item : p["imageUseVars"]) {
                int flag = 0;
                if (item.is_boolean()) flag = item.get<bool>() ? 1 : 0;
                else if (item.is_number()) flag = item.get<int>() != 0 ? 1 : 0;
                action.imageUseVars.push_back(static_cast<char>(flag));
            }
        }
        action.multiMatchMode = std::clamp(JsonInt(p, "multiMatchMode", 0), 0, 1);
        action.multiMatchMax = std::clamp(JsonInt(p, "multiMatchMax", kMultiMatchMaxHits),
            1, kMultiMatchMaxHits);
        action.multiMatchSort = std::clamp(JsonInt(p, "multiMatchSort", 0), 0, 1);
        action.matchThreshold = std::clamp(JsonDouble(p, "matchThreshold", 65.0), 1.0, 100.0);
        action.perfectMatch = JsonBool(p, "perfectMatch");
        action.imageScaleMin = std::max(0.1, JsonDouble(p, "imageScaleMin", 1.0));
        action.imageScaleMax = std::max(action.imageScaleMin,
            JsonDouble(p, "imageScaleMax", action.imageScaleMin));
        action.imageScale = JsonDouble(p, "imageScale",
            (action.imageScaleMin + action.imageScaleMax) * 0.5);
        action.findImageFollowUp = ParseFollowUpValue(p, "findImageFollowUp", 0);
        if (action.findImageFollowUp > 2) action.findImageFollowUp = 2;
        action.offsetX = JsonInt(p, "offsetX");
        action.offsetY = JsonInt(p, "offsetY");
        action.matchVarName = Trim(JsonWString(p, "matchVarName", L"matchRet"));
        if (action.matchVarName.empty()) action.matchVarName = L"matchRet";
        action.findTimeExpr = JsonWString(p, "findTimeExpr", L"0");
        if (action.findTimeExpr.empty()) action.findTimeExpr = L"0";
        action.button = ParseButton(p);
        action.duration = JsonDouble(p, "duration", 0.05);
        if (action.duration < 0.0) action.duration = 0.0;
        action.randomDuration = JsonDouble(p, "randomDuration", 0.0);
        if (action.randomDuration < 0.0) action.randomDuration = 0.0;
        action.timingUs = 0;
        NormalizeMultiMatchFields(action);
        break;
    }

    case ActionType::WatchImage: {
        action.searchX1 = JsonInt(p, "searchX1");
        action.searchY1 = JsonInt(p, "searchY1");
        action.searchX2 = JsonInt(p, "searchX2");
        action.searchY2 = JsonInt(p, "searchY2");
        action.searchFullScreen = JsonBool(p, "searchFullScreen", true);
        action.imageUseVar = JsonBool(p, "imageUseVar");
        action.imagePath = Trim(JsonWString(p, "imagePath"));
        action.matchThreshold = std::clamp(JsonDouble(p, "matchThreshold", 65.0), 1.0, 100.0);
        action.perfectMatch = JsonBool(p, "perfectMatch");
        action.imageScaleMin = std::max(0.1, JsonDouble(p, "imageScaleMin", 1.0));
        action.imageScaleMax = std::max(action.imageScaleMin,
            JsonDouble(p, "imageScaleMax", action.imageScaleMin));
        action.imageScale = JsonDouble(p, "imageScale",
            (action.imageScaleMin + action.imageScaleMax) * 0.5);
        action.resumeAfterWatch = JsonBool(p, "resumeAfterWatch", true);
        {
            action.watchMode = ParseWatchModeJson(p);
            double poll = JsonDouble(p, "watchPollSeconds", 0.0);
            if (!(poll > 0.0)) poll = JsonDouble(p, "watchPollInterval", 1.0);
            if (!(poll > 0.0)) poll = 1.0;
            if (poll < 0.05) poll = 0.05;
            if (poll > 3600.0) poll = 3600.0;
            action.watchPollSeconds = poll;
        }
        action.indent = 0;
        action.duration = 0.0;
        action.timingUs = 0;
        action.randomDuration = 0.0;
        break;
    }

    case ActionType::VarCompute:
        action.computeCode = JsonWString(p, "computeCode");
        if (action.computeCode.empty())
            action.computeCode = JsonWString(p, "inputText");
        action.duration = 0.0;
        action.timingUs = 0;
        action.randomDuration = 0.0;
        break;

    case ActionType::TextRecognition: {
        action.ocrRegionByImage = JsonBool(p, "ocrRegionByImage");
        action.ocrDigitsOnly = JsonBool(p, "ocrDigitsOnly");
        action.searchX1 = JsonInt(p, "searchX1");
        action.searchY1 = JsonInt(p, "searchY1");
        action.searchX2 = JsonInt(p, "searchX2");
        action.searchY2 = JsonInt(p, "searchY2");
        action.searchFullScreen = JsonBool(p, "searchFullScreen", true);
        action.imageRegionX1 = JsonInt(p, "imageRegionX1");
        action.imageRegionY1 = JsonInt(p, "imageRegionY1");
        action.imageRegionX2 = JsonInt(p, "imageRegionX2");
        action.imageRegionY2 = JsonInt(p, "imageRegionY2");
        action.imageUseVar = JsonBool(p, "imageUseVar");
        action.imagePath = Trim(JsonWString(p, "imagePath"));
        if (action.ocrRegionByImage) {
            action.matchThreshold = std::clamp(JsonDouble(p, "matchThreshold", 65.0), 1.0, 100.0);
            action.perfectMatch = JsonBool(p, "perfectMatch");
            action.imageScaleMin = std::max(0.1, JsonDouble(p, "imageScaleMin", 1.0));
            action.imageScaleMax = std::max(action.imageScaleMin,
                JsonDouble(p, "imageScaleMax", action.imageScaleMin));
            action.imageScale = JsonDouble(p, "imageScale",
                (action.imageScaleMin + action.imageScaleMax) * 0.5);
        }
        action.ocrResultMode = std::clamp(JsonInt(p, "ocrResultMode", 0), 0, 1);
        action.ocrSearchText = JsonWString(p, "ocrSearchText");
        action.ocrFollowUp = ParseFollowUpValue(p, "ocrFollowUp", 0);
        if (action.ocrFollowUp > 2) action.ocrFollowUp = 2;
        action.offsetX = JsonInt(p, "offsetX");
        action.offsetY = JsonInt(p, "offsetY");
        action.findUntilFound = JsonBool(p, "findUntilFound");
        action.matchVarName = Trim(JsonWString(p, "matchVarName", L"a"));
        if (action.matchVarName.empty()) action.matchVarName = L"a";
        break;
    }

    case ActionType::If:
        action.conditionExpr = JsonWString(p, "conditionExpr");
        break;

    case ActionType::Else:
        break;

    case ActionType::RunProgram: {
        action.shortcutPreset = std::clamp(JsonInt(p, "shortcutPreset", 0), 0, RunProgramPresetCount() - 1);
        action.targetPath = Trim(JsonWString(p, "targetPath"));
        action.inputText = JsonWString(p, "inputText");
        action.blockName = RunProgramDisplayName(action.shortcutPreset, action.targetPath);
        break;
    }

    case ActionType::CloseProgram:
        action.targetPath = Trim(JsonWString(p, "targetPath"));
        action.matchFileNameOnly = JsonBool(p, "matchFileNameOnly");
        break;

    case ActionType::OpenWebpage:
    case ActionType::OpenFile:
        action.targetPath = Trim(JsonWString(p, "targetPath"));
        break;

    case ActionType::ActivateWindow: {
        // match 与 targetPath 同义（Agent 工具用 match；宏 JSON 用 targetPath）
        action.targetPath = Trim(JsonWString(p, "targetPath"));
        if (action.targetPath.empty())
            action.targetPath = Trim(JsonWString(p, "match"));
        break;
    }

    case ActionType::TimerRecordTime:
        action.loopVarName = Trim(JsonWString(p, "loopVarName"));
        if (action.loopVarName.empty()) action.loopVarName = Trim(JsonWString(p, "timerVarName"));
        break;

    case ActionType::GetCursorPos:
        action.matchVarName = Trim(JsonWString(p, "matchVarName"));
        if (action.matchVarName.empty()) action.matchVarName = Trim(JsonWString(p, "varName"));
        if (action.matchVarName.empty()) action.matchVarName = L"a";
        break;

    case ActionType::GetColor: {
        action.x = JsonInt(p, "x");
        action.y = JsonInt(p, "y");
        action.moveFromVar = JsonBool(p, "moveFromVar");
        action.moveVarExprX = JsonWString(p, "moveVarExprX");
        action.moveVarExprY = JsonWString(p, "moveVarExprY");
        action.matchVarName = Trim(JsonWString(p, "matchVarName", L"colorRet"));
        if (action.matchVarName.empty()) action.matchVarName = L"colorRet";
        ApplyImageLocateFields(action, p, true);
        break;
    }

    case ActionType::FindColor:
    case ActionType::ColorMatch: {
        std::wstring colorSpec = JsonWString(p, "color");
        if (colorSpec.empty()) colorSpec = JsonWString(p, "inputText");
        int cr = JsonInt(p, "colorR", -1);
        int cg = JsonInt(p, "colorG", -1);
        int cb = JsonInt(p, "colorB", -1);
        if (cr >= 0 && cg >= 0 && cb >= 0) {
            action.colorR = std::clamp(cr, 0, 255);
            action.colorG = std::clamp(cg, 0, 255);
            action.colorB = std::clamp(cb, 0, 255);
        } else if (!TryParseColorSpec(colorSpec, action.colorR, action.colorG, action.colorB)) {
            // 留给校验报错
            action.colorR = action.colorG = action.colorB = -1;
        }
        action.colorTolerance = std::clamp(JsonInt(p, "colorTolerance", 16), 0, 255);
        action.inputText = colorSpec.empty()
            ? FormatColorHex(action.colorR, action.colorG, action.colorB) : colorSpec;
        if (type == ActionType::ColorMatch) {
            action.x = JsonInt(p, "x");
            action.y = JsonInt(p, "y");
            action.moveFromVar = JsonBool(p, "moveFromVar");
            action.moveVarExprX = JsonWString(p, "moveVarExprX");
            action.moveVarExprY = JsonWString(p, "moveVarExprY");
            ApplyImageLocateFields(action, p, true);
        } else {
            action.searchFullScreen = JsonBool(p, "searchFullScreen", true);
            action.searchX1 = JsonInt(p, "searchX1");
            action.searchY1 = JsonInt(p, "searchY1");
            action.searchX2 = JsonInt(p, "searchX2");
            action.searchY2 = JsonInt(p, "searchY2");
            action.findImageFollowUp = ParseFollowUpValue(p, "findImageFollowUp", 2);
            if (action.findImageFollowUp > 2) action.findImageFollowUp = 2;
            action.offsetX = JsonInt(p, "offsetX");
            action.offsetY = JsonInt(p, "offsetY");
            ApplyImageLocateFields(action, p, false);
        }
        action.matchVarName = Trim(JsonWString(p, "matchVarName", L"colorRet"));
        if (action.matchVarName.empty()) action.matchVarName = L"colorRet";
        break;
    }

    case ActionType::CustomText:
        if (action.customText.empty())
            action.customText = JsonWString(p, "customText");
        break;

    case ActionType::LockScreenshot:
    case ActionType::UnlockScreenshot:
    case ActionType::StopMacro:
        break;

    case ActionType::Goto:
        action.gotoStepExpr = Trim(JsonWString(p, "gotoStepExpr"));
        if (action.gotoStepExpr.empty())
            action.gotoStepExpr = Trim(JsonWString(p, "targetStep"));
        break;

    case ActionType::AiTextAnalysis:
    case ActionType::AiImageAnalysis:
        ApplyAiCommonFields(action, p);
        break;

    case ActionType::AiActionExecute:
        ApplyAiCommonFields(action, p);
        break;
    }

    ScriptActionBuildResult result;
    result.action = action;
    result.ok = true;
    return result;
}

// 必填参数校验：禁止生成空参数/依赖误导默认值的动作（如空文件路径、空按键、空输入）。
bool ValidateActionRequiredFields(const ScriptAction& a, std::wstring& err) {
    switch (a.type) {
    case ActionType::KeyClick:
    case ActionType::KeyDown:
    case ActionType::KeyUp:
        if (Trim(a.keyText).empty()) {
            err = L"缺少 keyText（按键名，如 Enter/F5/单字母数字）。禁止省略或依赖默认键。";
            return false;
        }
        break;
    case ActionType::QuickInput:
        if (Trim(a.inputText).empty()) {
            err = L"quickInput 缺少 inputText（要输入的文字，禁止空值；变量请写表达式）。";
            return false;
        }
        break;
    case ActionType::FindImage:
        if (a.findImageFollowUp != 3 && Trim(a.imagePath).empty()) {
            err = L"findImage 缺少 imagePath（要找的图；imageUseVar 时填变量名/路径，不能为空）。";
            return false;
        }
        break;
    case ActionType::MultiMatch:
        if (a.imagePaths.empty() && Trim(a.imagePath).empty()) {
            err = L"multiMatch 缺少 imagePaths（至少一张模板图）。";
            return false;
        }
        break;
    case ActionType::WatchImage:
        if (Trim(a.imagePath).empty()) {
            err = L"watchImage 缺少 imagePath（监视用图；imageUseVar 时填变量名/路径，不能为空）。";
            return false;
        }
        break;
    case ActionType::MouseDrag:
    case ActionType::GetColor:
    case ActionType::ColorMatch:
    case ActionType::FindColor:
        if (a.imageLocate && Trim(a.imagePath).empty()) {
            err = JsonType(a.type) + L" 找图定位缺少 imagePath（要查找的图；imageUseVar 时填变量名/路径）。";
            return false;
        }
        break;
    case ActionType::VarCompute:
        if (Trim(a.computeCode).empty()) {
            err = L"varCompute 缺少 computeCode（变量运算源码）。";
            return false;
        }
        break;
    case ActionType::TextRecognition:
        if (Trim(a.imagePath).empty() && Trim(a.ocrSearchText).empty()) {
            err = L"textRecognition 需要 imagePath（识别区域图）或 ocrSearchText（查找文字）至少一项。";
            return false;
        }
        break;
    case ActionType::Goto:
        if (Trim(a.gotoStepExpr).empty()) {
            err = L"goto 缺少 gotoStepExpr（跳转目标序号或变量表达式）。";
            return false;
        }
        break;
    case ActionType::If:
        if (Trim(a.conditionExpr).empty()) {
            err = L"if 缺少 conditionExpr（条件表达式）。";
            return false;
        }
        break;
    case ActionType::DefineBlock:
    case ActionType::RunBlock:
        if (Trim(a.blockName).empty()) {
            err = L"块动作缺少 blockName。";
            return false;
        }
        break;
    case ActionType::RunMacro:
    case ActionType::MousePlayback:
        if (Trim(a.targetPath).empty() && Trim(a.blockName).empty()) {
            err = L"动作缺少 targetPath（目标脚本路径）。";
            return false;
        }
        break;
    case ActionType::RunProgram:
        if (a.shortcutPreset <= 0 && Trim(a.targetPath).empty()) {
            err = L"runProgram 需要 targetPath（程序路径）或指定预设。";
            return false;
        }
        break;
    case ActionType::CloseProgram:
    case ActionType::OpenWebpage:
    case ActionType::OpenFile:
    case ActionType::ActivateWindow:
        if (Trim(a.targetPath).empty()) {
            err = L"动作缺少 targetPath（目标路径/网址/窗口标题）。";
            return false;
        }
        break;
    case ActionType::Wait:
        if (a.duration <= 0 && a.timingUs == 0 && a.randomDuration <= 0) {
            err = L"wait 需要 duration（等待秒数，>0）。";
            return false;
        }
        break;
    default:
        break;
    }
    return true;
}

constexpr int kMaxActionTreeDepth = 16;

bool FlattenOneActionParam(const json& node, int indent, int depth,
    std::vector<json>& out, std::wstring& error, const std::wstring& path) {
    if (depth > kMaxActionTreeDepth) {
        error = path + L"：嵌套超过 16 层。";
        return false;
    }
    if (!node.is_object()) {
        error = path + L" 必须是 JSON 对象。";
        return false;
    }
    const std::wstring typeStr = Trim(JsonWString(node, "type"));
    if (typeStr.empty()) {
        error = path + L" 缺少 type。";
        return false;
    }
    ActionType type{};
    if (!ParseActionType(typeStr, type)) {
        error = path + L" 未知动作类型：" + typeStr;
        return false;
    }
    if (type == ActionType::CustomText) {
        error = path + L" 禁止 customText。说明写 remark。";
        return false;
    }
    if (SkipsInMainFlow(type) && indent != 0) {
        error = path + L"（" + typeStr
            + L"）必须放在顶层，不能写在循环/条件的 children 里。";
        return false;
    }

    const bool hasChildrenKey = node.contains("children");
    json children = json::array();
    if (hasChildrenKey) {
        if (!node["children"].is_array()) {
            error = path + L" 的 children 必须是数组。";
            return false;
        }
        children = node["children"];
        if (!IsSubtreeContainer(type)) {
            error = path + L"（" + typeStr
                + L"）不是容器，不能有 children。只有 loop/if/else/defineBlock/watchImage 可嵌套子动作。";
            return false;
        }
        if (children.empty()) {
            error = path + L"（" + typeStr
                + L"）的 children 为空。循环/条件/块体内必须至少有一个子动作。";
            return false;
        }
    }

    json copy = node;
    copy.erase("children");
    copy["indent"] = indent;
    out.push_back(std::move(copy));

    if (hasChildrenKey) {
        for (size_t i = 0; i < children.size(); ++i) {
            const std::wstring childPath = path + L"/" + typeStr + L"["
                + std::to_wstring(i) + L"]";
            if (!FlattenOneActionParam(children[i], indent + 1, depth + 1, out, error, childPath))
                return false;
        }
    }
    return true;
}

bool FlattenNestedActionParamListImpl(const std::vector<json>& nested,
    std::vector<json>& flat, std::wstring& error) {
    flat.clear();
    error.clear();
    for (size_t i = 0; i < nested.size(); ++i) {
        const int indent = std::max(0, JsonInt(nested[i], "indent", 0));
        const std::wstring path = L"第 " + std::to_wstring(i + 1) + L" 项";
        if (!FlattenOneActionParam(nested[i], indent, 0, flat, error, path)) {
            flat.clear();
            return false;
        }
    }
    return true;
}

}  // namespace

ScriptActionBuildResult BuildScriptActionFromJson(const json& params) {
    if (!params.is_object()) return Fail(L"参数必须是 JSON 对象。");

    const std::wstring typeStr = Trim(JsonWString(params, "type"));
    if (typeStr.empty()) return Fail(L"缺少 type 字段。");

    ActionType type{};
    if (!ParseActionType(typeStr, type))
        return Fail(L"未知动作类型：" + typeStr);
    if (type == ActionType::CustomText)
        return Fail(L"禁止使用 customText。说明写 remark；须用 wait、goto、mouseClick、stopMacro 等可执行动作。");

    auto result = BuildTypedAction(type, params);
    if (!result.ok) return result;

    {
        std::wstring requiredErr;
        if (!ValidateActionRequiredFields(result.action, requiredErr)) {
            return Fail(L"type=" + typeStr + L" " + requiredErr);
        }
    }

    if (type == ActionType::DefineBlock || type == ActionType::RunBlock) {
        if (!ValidateVarName(result.action.blockName, L"块名", result.error)) {
            result.ok = false;
            return result;
        }
    }
    if (type == ActionType::FindImage || type == ActionType::TextRecognition
        || type == ActionType::MultiMatch) {
        if (type == ActionType::FindImage && result.action.findImageFollowUp == 3
            && LooksLikeFilePath(result.action.matchVarName)) {
            // 持久化路径：跳过变量名校验
        } else if (!ValidateVarName(result.action.matchVarName, L"变量名", result.error)) {
            result.ok = false;
            return result;
        }
    }
    if (type == ActionType::Loop || type == ActionType::TimerRecordTime) {
        if (!ValidateVarName(result.action.loopVarName, L"变量名", result.error)) {
            result.ok = false;
            return result;
        }
    }
    if (type == ActionType::GetCursorPos) {
        if (!ValidateVarName(result.action.matchVarName, L"变量名", result.error)) {
            result.ok = false;
            return result;
        }
    }
    if (type == ActionType::GetColor || type == ActionType::FindColor
        || type == ActionType::ColorMatch) {
        if (!ValidateVarName(result.action.matchVarName, L"变量名", result.error)) {
            result.ok = false;
            return result;
        }
        if (type != ActionType::GetColor
            && (result.action.colorR < 0 || result.action.colorG < 0 || result.action.colorB < 0)) {
            return Fail(L"type=" + typeStr + L" 需要有效 color（#RRGGBB 或 r,g,b）。");
        }
    }
    if (type == ActionType::AiTextAnalysis || type == ActionType::AiImageAnalysis
        || type == ActionType::AiActionExecute) {
        if (result.action.aiPrompt.empty())
            return Fail(L"type=" + typeStr + L" 缺少 aiPrompt（提示词/任务描述）。");
        if (!result.action.aiOutputVarName.empty()
            && !ValidateVarName(result.action.aiOutputVarName, L"AI 输出变量名", result.error)) {
            result.ok = false;
            return result;
        }
        EnsureAiModelOnAction(result.action);
        if (result.action.aiModelName.empty())
            return Fail(L"type=" + typeStr + L" 未配置 AI 模型。请先在「设置→AI助手」中添加模型。");
    }
    return result;
}

bool FlattenNestedActionParamList(const std::vector<json>& nested,
    std::vector<json>& flat, std::wstring& error) {
    return FlattenNestedActionParamListImpl(nested, flat, error);
}

std::wstring PlanScriptActionsOutline(const std::vector<json>& nested, std::wstring& error) {
    error.clear();
    if (nested.empty()) {
        error = L"actions 数组为空。请传入动作树（loop/if 用 children 嵌套子动作）。";
        return L"";
    }
    std::vector<json> flat;
    if (!FlattenNestedActionParamListImpl(nested, flat, error)) return L"";

    std::vector<ScriptAction> skeleton;
    skeleton.reserve(flat.size());
    for (size_t i = 0; i < flat.size(); ++i) {
        const std::wstring typeStr = Trim(JsonWString(flat[i], "type"));
        ActionType type{};
        if (!ParseActionType(typeStr, type)) {
            error = L"第 " + std::to_wstring(i + 1) + L" 个动作未知类型：" + typeStr;
            return L"";
        }
        ScriptAction a{};
        a.type = type;
        a.indent = std::max(0, JsonInt(flat[i], "indent", 0));
        a.remark = JsonWString(flat[i], "remark");
        a.loopCount = JsonInt(flat[i], "loopCount", -1);
        a.loopFromVar = JsonBool(flat[i], "loopFromVar");
        a.conditionExpr = JsonWString(flat[i], "conditionExpr");
        if (type == ActionType::WatchImage) a.watchMode = ParseWatchModeJson(flat[i]);
        skeleton.push_back(a);
    }
    if (const std::wstring bodyErr = ValidateContainerBodies(skeleton); !bodyErr.empty()) {
        error = bodyErr;
        return L"";
    }
    if (const std::wstring endLoopErr = ValidateEndLoopPlacements(skeleton); !endLoopErr.empty()) {
        error = endLoopErr;
        return L"";
    }

    std::wstring out = L"✓ 动作树（共 " + std::to_wstring(skeleton.size())
        + L" 步）。缩进表示父子：循环/条件/块的子动作必须比父级多一级。\n";
    for (size_t i = 0; i < skeleton.size(); ++i) {
        if (skeleton[i].indent > 0)
            out.append(static_cast<size_t>(skeleton[i].indent) * 2, L' ');
        std::wstring label = ActionTypeBriefLabel(skeleton[i].type);
        if (skeleton[i].type == ActionType::Loop && skeleton[i].loopCount < 0 && !skeleton[i].loopFromVar)
            label = L"无限循环";
        out += L"第" + std::to_wstring(i + 1) + L"步 " + label;
        if (skeleton[i].type == ActionType::If && !Trim(skeleton[i].conditionExpr).empty())
            out += L"  条件:" + Trim(skeleton[i].conditionExpr);
        if (skeleton[i].type == ActionType::WatchImage)
            out += skeleton[i].watchMode != 0 ? L"  时间监视" : L"  动作监视";
        if (!Trim(skeleton[i].remark).empty())
            out += L"  备注:" + Trim(skeleton[i].remark);
        out += L"\n";
    }
    out += L"\n下一步：按此树补齐必填参数，用同一棵 children 调用 createMacroScript "
        L"/ buildScriptActions。不要把循环体改成循环后面的同级动作。";
    return out;
}

std::wstring BuildScriptActionsJsonArray(const std::vector<json>& actionParams,
    std::wstring& error, bool validateContainerBodies) {
    error.clear();
    if (actionParams.empty()) {
        error = L"actions 数组为空。";
        return L"";
    }

    std::vector<json> flat;
    if (!FlattenNestedActionParamListImpl(actionParams, flat, error)) return L"";

    std::vector<ScriptAction> built;
    built.reserve(flat.size());
    for (size_t i = 0; i < flat.size(); ++i) {
        auto result = BuildScriptActionFromJson(flat[i]);
        if (!result.ok) {
            error = L"第 " + std::to_wstring(i + 1) + L" 个动作构建失败：" + result.error;
            return L"";
        }
        SanitizeScriptActionDisplay(result.action);
        built.push_back(std::move(result.action));
    }
    const bool addedStopMacro = EnsureStopMacroOnActions(built);
    NormalizeScriptActionList(built);
    if (const std::wstring endLoopErr = ValidateEndLoopPlacements(built); !endLoopErr.empty()) {
        error = endLoopErr;
        return L"";
    }
    if (validateContainerBodies) {
        if (const std::wstring bodyErr = ValidateContainerBodies(built); !bodyErr.empty()) {
            error = bodyErr;
            return L"";
        }
    }

    std::wstring out = L"[\n";
    for (size_t i = 0; i < built.size(); ++i) {
        out += ScriptActionToJsonString(built[i]);
        if (i + 1 < built.size()) out += L",";
        out += L"\n";
    }
    out += L"]";
    if (addedStopMacro)
        out += L"\n[提示] 已自动在末尾追加 stopMacro（结束宏运行），避免脚本无限重复执行。";
    return out;
}

std::wstring ScriptActionBuilderSchema() {
    return LR"(【buildScriptActions 参数说明 — 与手动编辑完全一致】

通用字段（每个动作可选）：
  type      动作类型（必填）
  indent    缩进层级，默认 0；用 children 时由工具自动写成父级+1，不必手写
  children  仅 loop/if/else/defineBlock/watchImage：子动作数组（推荐，像写代码的花括号）。
            循环体必须放这里，不要写成循环后面的同级动作。
  remark    备注（步骤说明、待确认提示写这里，禁止用 text 改动作名）
  no / text 由工具自动分配，不要手写

followUp 语义别名（findImage / multiMatch / textRecognition）：
  "click"=点击(0)  "move"=移动(1)  "saveVar"=保存匹配度(2)  "saveImage"=保存图片(3)
  也可写 findImageFollowUp / ocrFollowUp 整数；findImage 为 0~3，ocrFollowUp 为 0~2
  multiMatch 仅 0点击 / 1移动 / 2保存匹配度（无保存图片；>2 钳到 2）
  findColor 仅 0点击 / 1移动 / 2保存到变量（无保存图片；>2 钳到 2）

── 基础动作 ──
wait:           duration, randomDuration（整段等待，与下面「重复间隔」不同）
moveMouse:      x, y, randomX, randomY, moveFromVar, moveVarExprX, moveVarExprY
moveMouseRelative: x/dx, y/dy（像素相对位移，可负；FPS 视角等）, randomX, randomY
  说明: 键鼠录制仅在光标隐藏/ClipCursor 时由 Raw Input 自动写入；桌面勿用差值伪相对
★ 重复间隔语义（mouseClick/keyClick/hotkeyShortcut/quickInput/scrollWheel/mousePlayback/runMacro/runBlock）：
  clickCount=执行次数；duration/randomDuration=相邻两次之间的间隔；
  count=1 时完全不等待；不在第一次之前、最后一次之后插入等待。
mouseClick:     button(left/right/middle/x1/x2), clickCount, duration, randomDuration, modifiers/hold*
mouseDrag:      button, x/y 起点, endX/endY 终点, duration=拖拽时长（不是重复间隔）, randomDuration,
                randomX/Y, randomEndX/Y, modifiers/hold*, imageLocate(1=找图定位：起点/终点相对图中心；
                此时还需 imagePath、searchFullScreen/searchX1~Y2、matchThreshold、findTimeExpr、缩放)
mouseDown/Up:   button, modifiers/hold*
keyClick:       keyText, keyVk, clickCount, duration, randomDuration, modifiers
keyDown/Up:     keyText, keyVk, modifiers
hotkeyShortcut: shortcutPreset(0~11), clickCount, duration, randomDuration
quickInput:     inputText, charInterval(字间), parseEscapes(1=解析文本和变量中的\\n\\r\\t\\\\，缺省0=变量内换行/Tab丢掉), clickCount, duration(整段重复间隔), randomDuration
scrollWheel:    scrollVertical, scrollHorizontal, scrollDirection(0|1|"up"|"down"), scrollSteps, clickCount, duration

── 流程 ──
loop:           loopCount(-1=无限), loopFromVar, loopVarExpr, loopVarName,
                children[]=循环体（必填；空循环会构建失败）
endLoop:        （无额外字段，自动生成「跳出循环」；必须放在某 loop 的 children 里）
defineBlock:    blockName, children[]=块体
watchImage:     imagePath, matchThreshold, searchFullScreen, searchX1~Y2, imageScaleMin/Max,
                resumeAfterWatch(1=中断后从原处重跑该次找图；0=跳到监视容器后的主流程),
                watchMode(0或action=动作监视：找图等待时顺带搜；1或time=时间监视：按 watchPollSeconds 轮询),
                watchPollSeconds(时间监视间隔秒，默认1，最小0.05),
                children[]=命中监视图后执行（主流程跳过本容器；子树里若执行了跳转则以跳转为准）
varCompute:     computeCode（类 C：赋值/if/for/while；行末分号可省略；局部变量默认销毁；return a,b 导出脚本变量）
                字符串用 "+" 或 '+'（裸写 + 是加法）；split(s, "/")、parts[0]、parts.count；toInt/toString/trim
                ctrl:Clipboard() 为剪贴板文本或文件路径字符串（不是条件里的 0/1）
runBlock:       blockName, clickCount, duration(重复间隔，仅 count>1), randomDuration
if:             conditionExpr, children[]=成立时执行
else:           children[]=否则执行；与 if 同级（同一层 children 里紧跟 if）
goto:           gotoStepExpr（目标序号；跳入循环体=第1次迭代从目标执行，循环内互跳保持同次迭代）

── 识别 ──
findImage:      imagePath, imageUseVar(1=变量/路径模式), matchThreshold, perfectMatch(1=像素级终审),
                searchFullScreen, searchX1~Y2,
                followUp/saveVar→2 保存匹配度, saveImage→3 保存图片, matchVarName,
                offsetX/Y, findTimeExpr(有模板时等图；0=只找一次；-1=直到找到；保存图片无模板时忽略),
                imageScaleMin/Max, imageRegionX1~Y2(保存图片有模板时)
                变量: {name}.matchData/.x/.y/.cx/.cy/.x1/.y1；保存图片时 matchVarName 为变量名或持久路径
multiMatch:     imagePaths[]（必填，最多8张；imagePath 可作首张镜像）,
                imageUseVar / imageUseVars[]（每张可勾选变量图，与找图相同）,
                multiMatchMode(0=多图择一：按列表顺序找，第一张过阈值即停；1=一图多处：只用第一张做 NMS 多匹配),
                multiMatchMax(一图多处最多处数 1~20), multiMatchSort(0=先左后上 1=匹配度高到低),
                matchThreshold, perfectMatch, searchFullScreen, searchX1~Y2, imageScaleMin/Max,
                followUp(0点击 1移动 2保存匹配度), matchVarName(默认 matchRet),
                offsetX/Y, findTimeExpr(等到至少一处；0=只找一次；-1=直到找到),
                duration(一图多处依次点击的间隔，默认0.05；择一/移动/保存不用)
                变量: {name.count} 命中个数；{name[0]} 第一处是否命中；{name[0].x/.y/.x1/.y1/.cx/.cy/.matchData}
                {name[0].hit} 模板序号从1计；{name[n]} 是下拉代指，运行时字面 n 不解析
getColor:       x, y, matchVarName(默认colorRet), imageLocate(1=找图定位：x/y 相对图中心；未找到则跳过)
                找图定位时还需 imagePath、searchFullScreen/searchX1~Y2、matchThreshold、findTimeExpr、缩放
findColor:      color/#RRGGBB, colorTolerance, searchFullScreen/searchX1~Y2,
                findImageFollowUp(0点击 1移动 2保存到变量；无保存图片),
                offsetX/Y(点击/移动时相对色点), matchVarName,
                imageLocate(1=先找图，再在命中图范围内找色；需 imagePath、matchThreshold、缩放、findTimeExpr)
colorMatch:     x, y, color/#RRGGBB, colorTolerance, matchVarName, imageLocate(同 getColor)
textRecognition: ocrResultMode(0文字/1查找), ocrFollowUp/followUp,
                matchVarName, ocrSearchText, ocrRegionByImage, ocrDigitsOnly, imageUseVar,
                searchFullScreen, searchX1~Y2(绝对识别/找图区),
                imageRegionX1~Y2(根据图片时模板内相对偏移), imagePath, offsetX/Y, findUntilFound

── 系统 ──
runProgram:     shortcutPreset, targetPath, inputText
closeProgram:   targetPath, matchFileNameOnly
openWebpage:    targetPath(URL)
openFile:       targetPath
timerRecordTime: loopVarName 或 timerVarName
runMacro:       blockName, targetPath, clickCount, duration(重复间隔，仅 count>1), randomDuration,
                useMode(0默认/1窗口/2后台窗口/3继承，缺省3；目标脚本若是窗口类可填 1/2 并带 nestedWindowMode),
                breakoutTimeSeconds(仅默认模式脱离时间，秒；0=禁用),
                nestedWindowMode(窗口/后台窗口绑窗对象，字段同脚本级 windowMode)
mousePlayback:  blockName, targetPath, clickCount, duration(重复间隔，仅 count>1), randomDuration,
                playbackSpeed(0.25~4，缺省 1；嵌套录制只用此字段，不叠加设置全局倍速),
                useMode/breakoutTimeSeconds/nestedWindowMode 同 runMacro
                （界面显示名=运行录制回放；type 仍为 mousePlayback）
lockScreenshot / unlockScreenshot / stopMacro
  ★ 禁止 customText（说明写 remark，勿伪造动作名）

── AI ──
getCursorPos:   matchVarName（默认 a；引用 {name}.x / {name}.y 为屏幕坐标）
aiTextAnalysis:  aiPrompt(必填), aiOutputVarName(默认 aiResult), aiOutputType(0文本/1数字),
                 aiModelName, aiContextMode(0无/1宏/2循环/3块), aiTimeoutSec, aiFallbackValue
                 ★优先级低：优先 OCR；效率模式尽量少用
aiImageAnalysis: 同上 + aiImageScale(0.1~1), aiRegionByImage, aiTargetImagePath,
                 aiSearchX1~Y2（屏幕绝对识别区；勾选根据图片时在区内找图），
                 imageRegionX1~Y2（匹配框内相对截屏区；不填则整框）
                 ★优先级低：优先 findImage；准确度兜底诊断时可用
aiActionExecute: aiPrompt(必填任务描述), aiModelName, aiWithImage(1=带截图),
                 aiLogicConvert(1=逻辑转化：段末固化指令块+条件回退),
                 aiLogicBlockName(关联块名，可空自动生成),
                 aiRegionByImage, aiTargetImagePath, aiSearchX1~Y2, imageRegionX1~Y2,
                 aiMaxSteps(默认10,-1=不限), aiTimeoutSec,
                 aiContextMode, aiFallbackValue
                 ★极低优先级：仅用户明确要求「AI动作执行」时使用；
                 ★aiLogicConvert 仅用户明确要求「逻辑转化/自愈脚本」时置 1

aiContextMode：0=每次独立请求；1=宏级（可见全部下级对话）；2=循环级（可见嵌套子循环对话）；3=块级。
层级规则：循环上下文按嵌套深度分槽；子循环每轮结束后合并到父循环；宏上下文自动同步所有非「无上下文」对话。
aiModelName 留空时使用软件「设置→AI助手」中的默认模型。
后续 quickInput / if 等可引用 {aiResult}、{aiImgResult} 等输出变量。

modifiers 示例: ["ctrl","shift"] 或 holdLeftCtrl 等 0/1
)";
}

std::wstring ScriptActionCatalog() {
    return LR"(【宏动作目录 — 参数用 lookupMacroAction(type)；场景用法用 section=usage】
通用可选字段：type(必填), remark, indent 或 children（loop/if/else/defineBlock/watchImage 推荐 children）
鼠标: moveMouse, moveMouseRelative, mouseClick, mouseDrag, mouseDown, mouseUp, scrollWheel
键盘: keyClick, keyDown, keyUp, hotkeyShortcut, quickInput
流程: loop, endLoop, if, else, defineBlock, runBlock, stopMacro, goto, varCompute
识别: findImage(找图点击), multiMatch(多图匹配), watchImage(找图监视), textRecognition(OCR), getColor, findColor, colorMatch
系统: runProgram, closeProgram, openWebpage, openFile, lockScreenshot, unlockScreenshot
其它: runMacro, mousePlayback, timerRecordTime, getCursorPos
AI: aiTextAnalysis, aiImageAnalysis, aiActionExecute
lookupMacroAction: type=keyClick|quickInput…；section=agent|usage|composite|mouse|keyboard|flow|findImage|ocr|system|ai|all)";
}

namespace {

std::wstring ToLowerW(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring SchemaSection(const wchar_t* startMarker, const wchar_t* endMarker) {
    const std::wstring schema = ScriptActionBuilderSchema();
    const size_t start = schema.find(startMarker);
    if (start == std::wstring::npos) return L"";
    const size_t contentStart = start + wcslen(startMarker);
    size_t end = schema.size();
    if (endMarker) {
        const size_t found = schema.find(endMarker, contentStart);
        if (found != std::wstring::npos) end = found;
    }
    return schema.substr(contentStart, end - contentStart);
}

std::wstring SchemaTypeDetail(const std::wstring& typeName) {
    const std::wstring schema = ScriptActionBuilderSchema();
    const std::wstring prefix = typeName + L":";
    std::wstringstream out;
    out << L"【" << typeName << L" 参数】\n";
    bool found = false;
    bool capturing = false;
    size_t pos = 0;
    while (pos < schema.size()) {
        const size_t lineEnd = schema.find(L'\n', pos);
        const std::wstring line = schema.substr(pos,
            lineEnd == std::wstring::npos ? std::wstring::npos : lineEnd - pos);
        const bool indented = !line.empty() && (line[0] == L' ' || line[0] == L'\t');
        const size_t trimStart = line.find_first_not_of(L" \t");
        if (trimStart == std::wstring::npos) {
            capturing = false;
        } else {
            const std::wstring trimmed = line.substr(trimStart);
            const bool isHead = trimmed.rfind(prefix, 0) == 0
                || (typeName == L"textRecognition" && trimmed.rfind(L"textRecognition:", 0) == 0);
            if (isHead) {
                out << trimmed << L"\n";
                found = true;
                capturing = true;
            } else if (capturing && indented) {
                out << trimmed << L"\n";
            } else {
                capturing = false;
            }
        }
        if (lineEnd == std::wstring::npos) break;
        pos = lineEnd + 1;
    }
    if (!found) return L"";
    if (typeName == L"mouseDrag") {
        out << L"\n★ duration/randomDuration = 拖拽时长（按下到松开的插值时间），不是重复间隔。\n";
    }
    if (typeName == L"mouseClick" || typeName == L"keyClick"
        || typeName == L"hotkeyShortcut" || typeName == L"quickInput"
        || typeName == L"scrollWheel" || typeName == L"mousePlayback"
        || typeName == L"runMacro" || typeName == L"runBlock") {
        out << L"\n★ duration/randomDuration = 相邻两次 clickCount 重复之间的间隔；"
            L"count=1 完全不等待；不在首前/末后插入等待（与 wait 不同）。\n";
    }
    out << L"\n通用可选: remark, indent；容器可用 children 嵌套子动作（序号与显示名由工具自动生成）\n";
    out << L"followUp: click|move|saveVar（findImage/multiMatch/textRecognition 适用）\n";
    return out.str();
}

}  // namespace

std::wstring LookupMacroActionSchema(const std::wstring& typeOrSection) {
    const std::wstring q = ToLowerW(Trim(typeOrSection));
    if (q.empty() || q == L"catalog") return ScriptActionCatalog();
    if (q == L"all") return ScriptActionBuilderSchema();

    if (q == L"mouse") {
        return L"【section=mouse】\n" + SchemaSection(L"── 基础动作 ──", L"── 流程 ──");
    }
    if (q == L"keyboard") {
        return L"【section=keyboard】\n" + SchemaSection(L"── 基础动作 ──", L"── 流程 ──");
    }
    if (q == L"flow") {
        return L"【section=flow】\n" + SchemaSection(L"── 流程 ──", L"── 识别 ──");
    }
    if (q == L"findimage" || q == L"find_image") {
        return L"【section=findImage】\n"
            + SchemaSection(L"followUp 语义别名", L"── 流程 ──")
            + SchemaSection(L"── 识别 ──", L"── 系统 ──");
    }
    if (q == L"ocr" || q == L"textrecognition") {
        return L"【section=ocr / textRecognition】\n"
            + SchemaSection(L"followUp 语义别名", L"── 流程 ──")
            + SchemaSection(L"── 识别 ──", L"── 系统 ──");
    }
    if (q == L"system") {
        return L"【section=system】\n" + SchemaSection(L"── 系统 ──", L"── AI ──");
    }
    if (q == L"ai") {
        return L"【section=ai】\n" + SchemaSection(L"── AI ──", L"aiContextMode");
    }
    if (q == L"composite" || q == L"combo" || q == L"组合") {
        return MacroActionCompositeSkill();
    }
    if (q == L"usage" || q == L"skill" || q == L"scenarios" || q == L"场景" || q == L"用法") {
        return MacroActionUsageSkill();
    }
    if (q == L"agent" || q == L"plan" || q == L"loop" || q == L"分步" || q == L"闭环") {
        return MacroActionAgentSkill();
    }
    if (q == L"command" || q == L"cli" || q == L"shell" || q == L"powershell"
        || q == L"命令行" || q == L"命令") {
        return MacroActionCommandSkill();
    }
    if (q == L"office" || q == L"excel" || q == L"word" || q == L"ppt" || q == L"powerpoint"
        || q == L"pdf" || q == L"docx" || q == L"xlsx" || q == L"办公" || q == L"文档"
        || q == L"表格" || q == L"readdocument") {
        return MacroActionOfficeSkill();
    }
    if (q == L"game" || q == L"gaming" || q == L"游戏" || q == L"实时" || q == L"动态画面"
        || q == L"fps" || q == L"挂机") {
        return MacroActionGameSkill();
    }

    static const wchar_t* kTypes[] = {
        L"wait", L"moveMouse", L"moveMouseRelative", L"mouseClick", L"mouseDrag", L"mouseDown", L"mouseUp",
        L"keyClick", L"keyDown", L"keyUp", L"hotkeyShortcut", L"quickInput", L"scrollWheel",
        L"loop", L"endLoop", L"defineBlock", L"runBlock", L"if", L"else", L"goto",
        L"findImage", L"multiMatch", L"watchImage", L"varCompute", L"textRecognition",
        L"getColor", L"findColor", L"colorMatch",
        L"lockScreenshot", L"unlockScreenshot", L"stopMacro",
        L"runProgram", L"closeProgram", L"openWebpage", L"openFile",
        L"timerRecordTime", L"runMacro", L"mousePlayback", L"getCursorPos",
        L"aiTextAnalysis", L"aiImageAnalysis", L"aiActionExecute",
    };
    for (const wchar_t* t : kTypes) {
        if (q == ToLowerW(t)) {
            const std::wstring detail = SchemaTypeDetail(t);
            if (!detail.empty()) return detail;
            break;
        }
    }

    return L"[提示] 未找到「" + typeOrSection + L"」。\n"
        L"type 示例: keyClick, quickInput, findImage, wait\n"
        L"section 示例: agent, usage, composite, mouse, keyboard, flow, findImage, ocr, system, ai, all\n\n"
        + ScriptActionCatalog();
}

void NormalizeScriptActionList(std::vector<ScriptAction>& actions) {
    for (size_t i = 0; i < actions.size(); ++i) {
        actions[i].originalNo = static_cast<int>(i + 1);
        if (actions[i].type == ActionType::CustomText) continue;
        if (actions[i].type == ActionType::EndLoop) {
            actions[i].customText = L"跳出循环";
            continue;
        }
        actions[i].customText.clear();
    }
}

namespace {

bool HasStopMacro(const std::vector<ScriptAction>& actions) {
    for (const auto& a : actions) {
        if (a.type == ActionType::StopMacro) return true;
    }
    return false;
}

bool HasTopLevelInfiniteLoop(const std::vector<ScriptAction>& actions) {
    for (const auto& a : actions) {
        if (a.indent == 0 && a.type == ActionType::Loop && a.loopCount < 0 && !a.loopFromVar)
            return true;
    }
    return false;
}

}  // namespace

bool EnsureStopMacroOnActions(std::vector<ScriptAction>& actions) {
    if (actions.empty() || HasStopMacro(actions) || HasTopLevelInfiniteLoop(actions))
        return false;
    ScriptAction stop{};
    stop.type = ActionType::StopMacro;
    actions.push_back(stop);
    return true;
}
