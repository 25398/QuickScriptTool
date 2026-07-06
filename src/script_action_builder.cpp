#include "script_action_builder.h"

#include "action_utils.h"
#include "agent_ai_actions.h"
#include "ai_action_router.h"
#include "script_io.h"
#include "utils.h"

#include <algorithm>
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

bool ParseActionType(const std::wstring& type, ActionType& out) {
    static const struct { const wchar_t* name; ActionType t; } kMap[] = {
        {L"moveMouse", ActionType::MoveMouse},
        {L"mouseMove", ActionType::MoveMouse},
        {L"wait", ActionType::Wait},
        {L"mouseClick", ActionType::MouseClick},
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
        {L"textRecognition", ActionType::TextRecognition},
        {L"if", ActionType::If},
        {L"else", ActionType::Else},
        {L"lockScreenshot", ActionType::LockScreenshot},
        {L"unlockScreenshot", ActionType::UnlockScreenshot},
        {L"stopMacro", ActionType::StopMacro},
        {L"runProgram", ActionType::RunProgram},
        {L"closeProgram", ActionType::CloseProgram},
        {L"openWebpage", ActionType::OpenWebpage},
        {L"openFile", ActionType::OpenFile},
        {L"timerRecordTime", ActionType::TimerRecordTime},
        {L"getCursorPos", ActionType::GetCursorPos},
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
            if (s == L"click") return 0;
            if (s == L"move") return 1;
            if (s == L"saveVar" || s == L"saveVariable" || s == L"save") return 2;
        } else if (p["followUp"].is_number_integer()) {
            return std::clamp(p["followUp"].get<int>(), 0, 2);
        }
    }
    if (intKey && p.contains(intKey))
        return std::clamp(JsonInt(p, intKey, def), 0, 2);
    return def;
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

UINT ResolveKeyVk(const json& p, const std::wstring& keyText) {
    if (p.contains("keyVk") && p["keyVk"].is_number_integer())
        return static_cast<UINT>(p["keyVk"].get<int64_t>());
    if (keyText.empty()) return '7';
    if (keyText.size() == 1) return towupper(keyText[0]);
    const std::wstring& upper = keyText;
    if (upper == L"Enter" || upper == L"Return") return VK_RETURN;
    if (upper == L"Tab") return VK_TAB;
    if (upper == L"Space") return VK_SPACE;
    if (upper == L"Escape" || upper == L"Esc") return VK_ESCAPE;
    if (upper == L"Backspace") return VK_BACK;
    if (upper == L"Delete") return VK_DELETE;
    if (upper.size() >= 2 && (upper[0] == L'F' || upper[0] == L'f')) {
        wchar_t* end = nullptr;
        const long n = wcstol(upper.c_str() + 1, &end, 10);
        if (end && *end == L'\0' && n >= 1 && n <= 24)
            return static_cast<UINT>(VK_F1 + n - 1);
    }
    return towupper(keyText[0]);
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
    action.aiContextMode = std::clamp(JsonInt(p, "aiContextMode", 0), 0, 3);
    action.aiTimeoutSec = std::max(5, JsonInt(p, "aiTimeoutSec", 30));
    action.aiFallbackValue = Trim(JsonWString(p, "aiFallbackValue"));
    action.aiImageScale = std::clamp(JsonDouble(p, "aiImageScale", 0.5), 0.1, 1.0);
    action.aiRegionByImage = JsonBool(p, "aiRegionByImage");
    action.aiTargetImagePath = Trim(JsonWString(p, "aiTargetImagePath"));
    action.aiSearchX1 = JsonInt(p, "aiSearchX1");
    action.aiSearchY1 = JsonInt(p, "aiSearchY1");
    action.aiSearchX2 = JsonInt(p, "aiSearchX2");
    action.aiSearchY2 = JsonInt(p, "aiSearchY2");
    action.aiMaxSteps = JsonInt(p, "aiMaxSteps", 10);
    action.aiWithImage = JsonBool(p, "aiWithImage", true);
    action.aiConfirmExecute = JsonBool(p, "aiConfirmExecute");
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
    action.customText = JsonWString(p, "text");
    if (action.customText.empty()) action.customText = JsonWString(p, "customText");
    action.remark = JsonWString(p, "remark");
    action.originalNo = std::max(1, JsonInt(p, "no", 1));
    action.indent = std::max(0, JsonInt(p, "indent", 0));
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
        break;

    case ActionType::Wait:
        action.duration = std::max(0.0, JsonDouble(p, "duration", JsonDouble(p, "seconds", 0.5)));
        action.randomDuration = std::max(0.0, JsonDouble(p, "randomDuration", 0.0));
        break;

    case ActionType::MouseClick:
        action.button = ParseButton(p);
        action.x = JsonInt(p, "x");
        action.y = JsonInt(p, "y");
        ApplyModifierFields(action, p);
        ApplyRepeatTiming(action, p, 1, 0.01);
        break;

    case ActionType::MouseDown:
    case ActionType::MouseUp:
        action.button = ParseButton(p);
        ApplyModifierFields(action, p);
        break;

    case ActionType::KeyClick:
        action.keyText = JsonWString(p, "keyText", L"7");
        if (action.keyText.empty()) action.keyText = L"7";
        action.keyVk = ResolveKeyVk(p, action.keyText);
        ApplyModifierFields(action, p);
        ApplyRepeatTiming(action, p, 1, 0.01);
        break;

    case ActionType::KeyDown:
    case ActionType::KeyUp:
        action.keyText = JsonWString(p, "keyText", L"7");
        if (action.keyText.empty()) action.keyText = L"7";
        action.keyVk = ResolveKeyVk(p, action.keyText);
        ApplyModifierFields(action, p);
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
        break;

    case ActionType::RunMacro:
    case ActionType::MousePlayback:
        action.blockName = Trim(JsonWString(p, "blockName"));
        action.targetPath = Trim(JsonWString(p, "targetPath"));
        ApplyRepeatTiming(action, p, 1, 0.01);
        break;

    case ActionType::FindImage: {
        action.searchX1 = JsonInt(p, "searchX1");
        action.searchY1 = JsonInt(p, "searchY1");
        action.searchX2 = JsonInt(p, "searchX2");
        action.searchY2 = JsonInt(p, "searchY2");
        action.searchFullScreen = JsonBool(p, "searchFullScreen", true);
        action.imagePath = Trim(JsonWString(p, "imagePath"));
        action.matchThreshold = std::clamp(JsonDouble(p, "matchThreshold", 65.0), 1.0, 100.0);
        action.imageScaleMin = std::max(0.1, JsonDouble(p, "imageScaleMin", 1.0));
        action.imageScaleMax = std::max(action.imageScaleMin,
            JsonDouble(p, "imageScaleMax", action.imageScaleMin));
        action.imageScale = JsonDouble(p, "imageScale",
            (action.imageScaleMin + action.imageScaleMax) * 0.5);
        action.findImageFollowUp = ParseFollowUpValue(p, "findImageFollowUp", 0);
        action.offsetX = JsonInt(p, "offsetX");
        action.offsetY = JsonInt(p, "offsetY");
        action.matchVarName = Trim(JsonWString(p, "matchVarName", L"matchRet"));
        if (action.matchVarName.empty()) action.matchVarName = L"matchRet";
        if (action.findImageFollowUp == 2) {
            action.findUntilFound = false;
        } else {
            action.findUntilFound = JsonBool(p, "findUntilFound");
        }
        break;
    }

    case ActionType::TextRecognition: {
        action.ocrRegionByImage = JsonBool(p, "ocrRegionByImage");
        action.ocrDigitsOnly = JsonBool(p, "ocrDigitsOnly");
        action.searchX1 = JsonInt(p, "searchX1");
        action.searchY1 = JsonInt(p, "searchY1");
        action.searchX2 = JsonInt(p, "searchX2");
        action.searchY2 = JsonInt(p, "searchY2");
        action.searchFullScreen = action.ocrRegionByImage
            ? false : JsonBool(p, "searchFullScreen", true);
        action.imagePath = Trim(JsonWString(p, "imagePath"));
        if (action.ocrRegionByImage) {
            action.matchThreshold = std::clamp(JsonDouble(p, "matchThreshold", 65.0), 1.0, 100.0);
            action.imageScaleMin = std::max(0.1, JsonDouble(p, "imageScaleMin", 1.0));
            action.imageScaleMax = std::max(action.imageScaleMin,
                JsonDouble(p, "imageScaleMax", action.imageScaleMin));
            action.imageScale = JsonDouble(p, "imageScale",
                (action.imageScaleMin + action.imageScaleMax) * 0.5);
        }
        action.ocrResultMode = std::clamp(JsonInt(p, "ocrResultMode", 0), 0, 1);
        action.ocrSearchText = JsonWString(p, "ocrSearchText");
        action.ocrFollowUp = ParseFollowUpValue(p, "ocrFollowUp", 0);
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

    case ActionType::TimerRecordTime:
        action.loopVarName = Trim(JsonWString(p, "loopVarName"));
        if (action.loopVarName.empty()) action.loopVarName = Trim(JsonWString(p, "timerVarName"));
        break;

    case ActionType::GetCursorPos:
        action.matchVarName = Trim(JsonWString(p, "matchVarName"));
        if (action.matchVarName.empty()) action.matchVarName = Trim(JsonWString(p, "varName"));
        if (action.matchVarName.empty()) action.matchVarName = L"cursor";
        break;

    case ActionType::CustomText:
        if (action.customText.empty())
            action.customText = JsonWString(p, "customText");
        break;

    case ActionType::LockScreenshot:
    case ActionType::UnlockScreenshot:
    case ActionType::StopMacro:
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

}  // namespace

ScriptActionBuildResult BuildScriptActionFromJson(const json& params) {
    if (!params.is_object()) return Fail(L"参数必须是 JSON 对象。");

    const std::wstring typeStr = Trim(JsonWString(params, "type"));
    if (typeStr.empty()) return Fail(L"缺少 type 字段。");

    ActionType type{};
    if (!ParseActionType(typeStr, type))
        return Fail(L"未知动作类型：" + typeStr);

    auto result = BuildTypedAction(type, params);
    if (!result.ok) return result;

    if (type == ActionType::DefineBlock || type == ActionType::RunBlock) {
        if (!ValidateVarName(result.action.blockName, L"块名", result.error)) {
            result.ok = false;
            return result;
        }
    }
    if (type == ActionType::FindImage || type == ActionType::TextRecognition) {
        if (!ValidateVarName(result.action.matchVarName, L"变量名", result.error)) {
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
    if (type == ActionType::FindImage && result.action.imagePath.empty()) {
        if (result.action.remark.empty())
            result.action.remark = L"待确认: imagePath";
    }
    if (type == ActionType::If && result.action.conditionExpr.empty())
        return Fail(L"if 动作缺少 conditionExpr。");

    return result;
}

std::wstring BuildScriptActionsJsonArray(const std::vector<json>& actionParams,
    std::wstring& error) {
    error.clear();
    if (actionParams.empty()) {
        error = L"actions 数组为空。";
        return L"";
    }

    std::wstring out = L"[\n";
    for (size_t i = 0; i < actionParams.size(); ++i) {
        auto built = BuildScriptActionFromJson(actionParams[i]);
        if (!built.ok) {
            error = L"第 " + std::to_wstring(i + 1) + L" 个动作构建失败：" + built.error;
            return L"";
        }
        if (built.action.originalNo <= 0)
            built.action.originalNo = static_cast<int>(i + 1);
        out += ScriptActionToJsonString(built.action);
        if (i + 1 < actionParams.size()) out += L",";
        out += L"\n";
    }
    out += L"]";
    return out;
}

std::wstring ScriptActionBuilderSchema() {
    return LR"(【buildScriptActions 参数说明 — 与手动编辑完全一致】

通用字段（每个动作可选）：
  type      动作类型（必填）
  no        序号，默认按数组顺序 1,2,3…
  indent    缩进层级，默认 0；if/loop 子动作 = 父级+1
  remark    备注
  text      自定义显示名

followUp 语义别名（findImage / textRecognition）：
  "click"=点击(0)  "move"=移动(1)  "saveVar"=保存变量(2)
  也可写 findImageFollowUp / ocrFollowUp 整数 0/1/2

── 基础动作 ──
wait:           duration, randomDuration
moveMouse:      x, y, randomX, randomY, moveFromVar, moveVarExprX, moveVarExprY
mouseClick:     button(left/right/middle/x1/x2), clickCount, duration, randomDuration, modifiers/hold*
mouseDown/Up:   button, modifiers/hold*
keyClick:       keyText, keyVk, clickCount, duration, randomDuration, modifiers
keyDown/Up:     keyText, keyVk, modifiers
hotkeyShortcut: shortcutPreset(0~8), clickCount, duration, randomDuration
quickInput:     inputText, charInterval, clickCount, duration, randomDuration
scrollWheel:    scrollVertical, scrollHorizontal, scrollDirection(0|1|"up"|"down"), scrollSteps, clickCount, duration

── 流程 ──
loop:           loopCount(-1=无限), loopFromVar, loopVarExpr, loopVarName
endLoop:        （无额外字段，自动生成「跳出循环」）
defineBlock:    blockName
runBlock:       blockName
if:             conditionExpr
else:           （无额外字段）

── 识别 ──
findImage:      imagePath, matchThreshold, searchFullScreen, searchX1~Y2,
                followUp/saveVar→findImageFollowUp=2, matchVarName,
                offsetX/Y, findUntilFound, imageScaleMin/Max
                变量: {name}.x/.y 左上角, {name}.cx/.cy 中心点, {name}.x1/.y1 右下角
textRecognition: ocrResultMode(0文字/1查找), ocrFollowUp/followUp,
                matchVarName, ocrSearchText, ocrRegionByImage, ocrDigitsOnly,
                searchFullScreen, searchX1~Y2, imagePath, offsetX/Y, findUntilFound

── 系统 ──
runProgram:     shortcutPreset, targetPath, inputText
closeProgram:   targetPath, matchFileNameOnly
openWebpage:    targetPath(URL)
openFile:       targetPath
timerRecordTime: loopVarName 或 timerVarName
runMacro:       blockName, targetPath
mousePlayback:  blockName, targetPath, clickCount, duration
lockScreenshot / unlockScreenshot / stopMacro / customText

── AI ──
getCursorPos:   matchVarName（默认 cursor；引用 {name}.x / {name}.y 为屏幕坐标）
aiTextAnalysis:  aiPrompt(必填), aiOutputVarName(默认 aiResult), aiOutputType(0文本/1数字),
                 aiModelName, aiContextMode(0无/1宏/2循环/3块), aiTimeoutSec, aiFallbackValue
aiImageAnalysis: 同上 + aiImageScale(0.1~1), aiRegionByImage, aiTargetImagePath,
                 aiSearchX1~Y2（区域截屏；不填则全屏）
aiActionExecute: aiPrompt(必填任务描述), aiModelName, aiWithImage(1=带截图),
                 aiRegionByImage, aiTargetImagePath, aiSearchX1~Y2,
                 aiMaxSteps(默认10,-1=不限), aiTimeoutSec, aiConfirmExecute(1=执行前确认),
                 aiContextMode, aiFallbackValue

aiContextMode：0=每次独立请求；1=宏级共享上下文；2=循环级；3=块级。
aiModelName 留空时使用软件「设置→AI助手」中的默认模型。
后续 quickInput / if 等可引用 {aiResult}、{aiImgResult} 等输出变量。

modifiers 示例: ["ctrl","shift"] 或 holdLeftCtrl 等 0/1
)";
}

std::wstring ScriptActionCatalog() {
    return LR"(【宏动作目录 — 参数细节用 lookupMacroAction 查询】
通用可选字段：type(必填), remark, no, indent, text
鼠标: moveMouse, mouseClick, mouseDown, mouseUp, scrollWheel
键盘: keyClick, keyDown, keyUp, hotkeyShortcut, quickInput
流程: loop, endLoop, if, else, defineBlock, runBlock, stopMacro
识别: findImage(找图点击), textRecognition(OCR)
系统: runProgram, closeProgram, openWebpage, openFile, lockScreenshot, unlockScreenshot
其它: runMacro, mousePlayback, timerRecordTime, getCursorPos, customText
AI: aiTextAnalysis, aiImageAnalysis, aiActionExecute
lookupMacroAction(type="keyClick") 查单个；section=mouse|keyboard|flow|findImage|ocr|system|ai|all 查分类)";
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
    size_t pos = 0;
    while (pos < schema.size()) {
        const size_t lineEnd = schema.find(L'\n', pos);
        const std::wstring line = schema.substr(pos,
            lineEnd == std::wstring::npos ? std::wstring::npos : lineEnd - pos);
        const size_t trimStart = line.find_first_not_of(L" \t");
        if (trimStart != std::wstring::npos) {
            const std::wstring trimmed = line.substr(trimStart);
            if (trimmed.rfind(prefix, 0) == 0
                || (typeName == L"textRecognition" && trimmed.rfind(L"textRecognition:", 0) == 0)) {
                out << trimmed << L"\n";
                found = true;
            }
        }
        if (lineEnd == std::wstring::npos) break;
        pos = lineEnd + 1;
    }
    if (!found) return L"";
    out << L"\n通用可选: remark, no, indent, text\n";
    out << L"followUp: click|move|saveVar（findImage/textRecognition 适用）\n";
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
        return MacroActionCompositeSkill() + L"\n\n" + ScriptActionCatalog();
    }

    static const wchar_t* kTypes[] = {
        L"wait", L"moveMouse", L"mouseClick", L"mouseDown", L"mouseUp",
        L"keyClick", L"keyDown", L"keyUp", L"hotkeyShortcut", L"quickInput", L"scrollWheel",
        L"loop", L"endLoop", L"defineBlock", L"runBlock", L"if", L"else",
        L"findImage", L"textRecognition",
        L"lockScreenshot", L"unlockScreenshot", L"stopMacro", L"customText",
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
        L"type 示例: keyClick, findImage, wait\n"
        L"section 示例: mouse, keyboard, flow, findImage, ocr, system, ai, all\n\n"
        + ScriptActionCatalog();
}
