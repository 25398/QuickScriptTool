#include "script_io.h"

#include "json_util.h"

#include "action_utils.h"
#include "app_settings.h"
#include "coord_space.h"
#include "recorder_timeline.h"
#include "window_mode/window_mode_json.h"

#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

std::wstring ExtractNamedJsonObject(const std::wstring& content, const wchar_t* key) {
    // 统一走 json_util（严格解析取配对子对象）；原手写扫描已删。
    // 键是 ASCII 字面量，逐字符窄化（不要用 std::string(key, key+n)，那会触发
    // C4244「wchar_t → char 可能丢数据」）。
    std::string narrowKey;
    for (const wchar_t* p = key; p && *p; ++p) narrowKey.push_back(static_cast<char>(*p));
    std::string out;
    if (!qst::jsonutil::GetSubObjectText(ToUtf8(content), narrowKey.c_str(), out)) return L"";
    return FromUtf8(out);
}

namespace {

bool VisualLayoutLooksValid(const std::wstring& json) {
    if (json.size() < 2 || json.front() != L'{') return false;
    const auto end = FindMatchingJsonBrace(json, 0);
    return end != std::wstring::npos && end + 1 == json.size();
}

// 勿用 ExtractString 读 watchMode：值为数字时会误取到下一个键名。
int ParseWatchModeField(const std::wstring& block) {
    const std::wstring key = L"\"watchMode\"";
    const auto pos = block.find(key);
    if (pos == std::wstring::npos) return 0;
    const auto colon = block.find(L':', pos + key.size());
    if (colon == std::wstring::npos) return 0;
    size_t i = colon + 1;
    while (i < block.size() && (block[i] == L' ' || block[i] == L'\t'
        || block[i] == L'\n' || block[i] == L'\r')) {
        ++i;
    }
    if (i >= block.size()) return 0;
    if (block[i] == L'"') {
        ++i;
        std::wstring s;
        bool esc = false;
        for (; i < block.size(); ++i) {
            const wchar_t c = block[i];
            if (esc) {
                s.push_back(c);
                esc = false;
                continue;
            }
            if (c == L'\\') {
                esc = true;
                continue;
            }
            if (c == L'"') break;
            s.push_back(c);
        }
        if (s == L"time" || s == L"timed" || s == L"interval"
            || s == L"1" || s == L"true") return 1;
        return 0;
    }
    const auto end = block.find_first_of(L",}\n", i);
    try {
        return std::stod(Trim(block.substr(i, end == std::wstring::npos
            ? std::wstring::npos : end - i))) != 0.0 ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

} // namespace

bool IsRecordingScriptPath(const std::wstring& path) {
    if (path.empty()) return false;
    wchar_t recDir[MAX_PATH]{};
    wchar_t fullPath[MAX_PATH]{};
    const DWORD nDir = GetFullPathNameW(RecordingsDir().c_str(), MAX_PATH, recDir, nullptr);
    if (nDir == 0 || nDir >= MAX_PATH) return false;
    const DWORD nPath = GetFullPathNameW(path.c_str(), MAX_PATH, fullPath, nullptr);
    const wchar_t* probe = (nPath > 0 && nPath < MAX_PATH) ? fullPath : path.c_str();
    const size_t rl = wcslen(recDir);
    if (rl == 0) return false;
    if (_wcsnicmp(probe, recDir, rl) != 0) return false;
    const wchar_t next = probe[rl];
    return next == L'\0' || next == L'\\' || next == L'/';
}

ScriptAction ParseScriptActionBlock(const std::wstring& block, size_t fallbackNo,
    bool coordsNormalized) {
    ScriptAction a{};
    const qst::jsonutil::WideObjectView J(block);
    const auto type = J.GetString(L"type");
    if (type.empty()) return a;
    if (type == L"moveMouse") a.type = ActionType::MoveMouse;
    else if (type == L"moveMouseRelative") a.type = ActionType::MoveMouseRelative;
    else if (type == L"mouseDown") a.type = ActionType::MouseDown;
    else if (type == L"mouseUp") a.type = ActionType::MouseUp;
    else if (type == L"mouseClick") a.type = ActionType::MouseClick;
    else if (type == L"mouseDrag") a.type = ActionType::MouseDrag;
    else if (type == L"mousePlayback") a.type = ActionType::MousePlayback;
    else if (type == L"runMacro") a.type = ActionType::RunMacro;
    else if (type == L"keyDown") a.type = ActionType::KeyDown;
    else if (type == L"keyUp") a.type = ActionType::KeyUp;
    else if (type == L"keyClick") a.type = ActionType::KeyClick;
    else if (type == L"hotkeyShortcut") a.type = ActionType::HotkeyShortcut;
    else if (type == L"quickInput") a.type = ActionType::QuickInput;
    else if (type == L"scrollWheel") a.type = ActionType::ScrollWheel;
    else if (type == L"findImage") a.type = ActionType::FindImage;
    else if (type == L"multiMatch") a.type = ActionType::MultiMatch;
    else if (type == L"watchImage") a.type = ActionType::WatchImage;
    else if (type == L"varCompute") a.type = ActionType::VarCompute;
    else if (type == L"textRecognition") a.type = ActionType::TextRecognition;
    else if (type == L"wait") a.type = ActionType::Wait;
    else if (type == L"loop") a.type = ActionType::Loop;
    else if (type == L"endLoop") a.type = ActionType::EndLoop;
    else if (type == L"defineBlock") a.type = ActionType::DefineBlock;
    else if (type == L"runBlock") a.type = ActionType::RunBlock;
    else if (type == L"if") a.type = ActionType::If;
    else if (type == L"else") a.type = ActionType::Else;
    else if (type == L"lockScreenshot") a.type = ActionType::LockScreenshot;
    else if (type == L"unlockScreenshot") a.type = ActionType::UnlockScreenshot;
    else if (type == L"stopMacro") a.type = ActionType::StopMacro;
    else if (type == L"goto") a.type = ActionType::Goto;
    else if (type == L"runProgram") a.type = ActionType::RunProgram;
    else if (type == L"closeProgram") a.type = ActionType::CloseProgram;
    else if (type == L"openWebpage") a.type = ActionType::OpenWebpage;
    else if (type == L"openFile") a.type = ActionType::OpenFile;
    else if (type == L"activateWindow") a.type = ActionType::ActivateWindow;
    else if (type == L"timerRecordTime") a.type = ActionType::TimerRecordTime;
    else if (type == L"getCursorPos") a.type = ActionType::GetCursorPos;
    else if (type == L"getColor") a.type = ActionType::GetColor;
    else if (type == L"findColor") a.type = ActionType::FindColor;
    else if (type == L"colorMatch") a.type = ActionType::ColorMatch;
    else if (type == L"aiTextAnalysis") a.type = ActionType::AiTextAnalysis;
    else if (type == L"aiImageAnalysis") a.type = ActionType::AiImageAnalysis;
    else if (type == L"aiActionExecute") a.type = ActionType::AiActionExecute;
    else a.type = ActionType::CustomText;
    a.customText = J.GetString(L"text");
    a.remark = J.GetString(L"remark");
    if (a.type == ActionType::CustomText && type != L"customText") {
        const std::wstring tag = L"[未知动作] " + type;
        if (a.remark.empty()) a.remark = tag;
        else if (a.remark.find(L"[未知动作]") == std::wstring::npos)
            a.remark = tag + L" " + a.remark;
    }
    a.originalNo = static_cast<int>(J.GetNumber(L"no", static_cast<double>(fallbackNo + 1)));
    a.indent = static_cast<int>(J.GetNumber(L"indent", 0));

    if (a.type == ActionType::MoveMouseRelative) {
        // 相对位移始终为整型像素 dx/dy，不参与屏幕归一化
        a.coordsAreNormalized = false;
        a.x = static_cast<int>(J.GetNumber(L"x", 0));
        a.y = static_cast<int>(J.GetNumber(L"y", 0));
        a.randomX = static_cast<int>(J.GetNumber(L"randomX", 0));
        a.randomY = static_cast<int>(J.GetNumber(L"randomY", 0));
        a.searchX1 = static_cast<int>(J.GetNumber(L"searchX1", 0));
        a.searchY1 = static_cast<int>(J.GetNumber(L"searchY1", 0));
        a.searchX2 = static_cast<int>(J.GetNumber(L"searchX2", 0));
        a.searchY2 = static_cast<int>(J.GetNumber(L"searchY2", 0));
        a.offsetX = static_cast<int>(J.GetNumber(L"offsetX", 0));
        a.offsetY = static_cast<int>(J.GetNumber(L"offsetY", 0));
        a.aiSearchX1 = static_cast<int>(J.GetNumber(L"aiSearchX1", 0));
        a.aiSearchY1 = static_cast<int>(J.GetNumber(L"aiSearchY1", 0));
        a.aiSearchX2 = static_cast<int>(J.GetNumber(L"aiSearchX2", 0));
        a.aiSearchY2 = static_cast<int>(J.GetNumber(L"aiSearchY2", 0));
    } else if (J.GetNumber(L"windowRelative", 0.0) != 0.0) {
        // 窗口相对录制：x/y 是目标窗口客户区像素，无论文件是否带 coordMeta
        a.windowRelative = true;
        a.coordsAreNormalized = false;
        a.x = static_cast<int>(J.GetNumber(L"x", 0));
        a.y = static_cast<int>(J.GetNumber(L"y", 0));
        a.randomX = static_cast<int>(J.GetNumber(L"randomX", 0));
        a.randomY = static_cast<int>(J.GetNumber(L"randomY", 0));
        a.searchX1 = static_cast<int>(J.GetNumber(L"searchX1", 0));
        a.searchY1 = static_cast<int>(J.GetNumber(L"searchY1", 0));
        a.searchX2 = static_cast<int>(J.GetNumber(L"searchX2", 0));
        a.searchY2 = static_cast<int>(J.GetNumber(L"searchY2", 0));
        a.offsetX = static_cast<int>(J.GetNumber(L"offsetX", 0));
        a.offsetY = static_cast<int>(J.GetNumber(L"offsetY", 0));
        a.aiSearchX1 = static_cast<int>(J.GetNumber(L"aiSearchX1", 0));
        a.aiSearchY1 = static_cast<int>(J.GetNumber(L"aiSearchY1", 0));
        a.aiSearchX2 = static_cast<int>(J.GetNumber(L"aiSearchX2", 0));
        a.aiSearchY2 = static_cast<int>(J.GetNumber(L"aiSearchY2", 0));
    } else if (coordsNormalized) {
        // 从 JSON 读取归一化坐标（0.0–1.0 浮点数）
        a.coordsAreNormalized = true;
        a.nx = J.GetNumber(L"x", 0.0);
        a.ny = J.GetNumber(L"y", 0.0);
        a.nRandomX = J.GetNumber(L"randomX", 0.0);
        a.nRandomY = J.GetNumber(L"randomY", 0.0);
        a.nSearchX1 = J.GetNumber(L"searchX1", 0.0);
        a.nSearchY1 = J.GetNumber(L"searchY1", 0.0);
        a.nSearchX2 = J.GetNumber(L"searchX2", 0.0);
        a.nSearchY2 = J.GetNumber(L"searchY2", 0.0);
        a.nOffsetX = J.GetNumber(L"offsetX", 0.0);
        a.nOffsetY = J.GetNumber(L"offsetY", 0.0);
        a.nAiSearchX1 = J.GetNumber(L"aiSearchX1", 0.0);
        a.nAiSearchY1 = J.GetNumber(L"aiSearchY1", 0.0);
        a.nAiSearchX2 = J.GetNumber(L"aiSearchX2", 0.0);
        a.nAiSearchY2 = J.GetNumber(L"aiSearchY2", 0.0);
    } else {
        a.x = static_cast<int>(J.GetNumber(L"x", 0));
        a.y = static_cast<int>(J.GetNumber(L"y", 0));
        a.randomX = static_cast<int>(J.GetNumber(L"randomX", 0));
        a.randomY = static_cast<int>(J.GetNumber(L"randomY", 0));
        a.searchX1 = static_cast<int>(J.GetNumber(L"searchX1", 0));
        a.searchY1 = static_cast<int>(J.GetNumber(L"searchY1", 0));
        a.searchX2 = static_cast<int>(J.GetNumber(L"searchX2", 0));
        a.searchY2 = static_cast<int>(J.GetNumber(L"searchY2", 0));
        a.offsetX = static_cast<int>(J.GetNumber(L"offsetX", 0));
        a.offsetY = static_cast<int>(J.GetNumber(L"offsetY", 0));
        a.aiSearchX1 = static_cast<int>(J.GetNumber(L"aiSearchX1", 0));
        a.aiSearchY1 = static_cast<int>(J.GetNumber(L"aiSearchY1", 0));
        a.aiSearchX2 = static_cast<int>(J.GetNumber(L"aiSearchX2", 0));
        a.aiSearchY2 = static_cast<int>(J.GetNumber(L"aiSearchY2", 0));
    }
    if (a.coordsAreNormalized) {
        a.nEndX = J.GetNumber(L"endX", 0.0);
        a.nEndY = J.GetNumber(L"endY", 0.0);
        a.nRandomEndX = J.GetNumber(L"randomEndX", 0.0);
        a.nRandomEndY = J.GetNumber(L"randomEndY", 0.0);
    } else {
        a.endX = static_cast<int>(J.GetNumber(L"endX", 0));
        a.endY = static_cast<int>(J.GetNumber(L"endY", 0));
        a.randomEndX = static_cast<int>(J.GetNumber(L"randomEndX", 0));
        a.randomEndY = static_cast<int>(J.GetNumber(L"randomEndY", 0));
    }
    a.imageLocate = J.GetBool(L"imageLocate", false);
    a.moveFromVar = J.GetNumber(L"moveFromVar", 0) != 0;
    a.moveVarExprX = J.GetString(L"moveVarExprX");
    a.moveVarExprY = J.GetString(L"moveVarExprY");
    const auto button = J.GetString(L"button");
    a.button = button == L"right" ? MouseButtonType::Right
        : button == L"middle" ? MouseButtonType::Middle
        : button == L"x1" ? MouseButtonType::X1
        : button == L"x2" ? MouseButtonType::X2
        : MouseButtonType::Left;
    a.keyText = J.GetString(L"keyText");
    // 仅对按键类动作补旧默认键，避免污染 runProgram/鼠标等非按键动作的字段
    if (a.keyText.empty()
        && (a.type == ActionType::KeyClick
            || a.type == ActionType::KeyDown
            || a.type == ActionType::KeyUp)) {
        a.keyText = L"7";
    }
    a.keyVk = NormalizeScriptKeyVk(
        static_cast<UINT>(J.GetNumber(L"keyVk",
            a.keyText.size() == 1 ? towupper(a.keyText[0]) : 0)),
        a.keyText);
    a.holdLeftWin = J.GetNumber(L"holdLeftWin", 0) != 0;
    a.holdRightWin = J.GetNumber(L"holdRightWin", 0) != 0;
    a.holdLeftCtrl = J.GetNumber(L"holdLeftCtrl", 0) != 0;
    a.holdRightCtrl = J.GetNumber(L"holdRightCtrl", 0) != 0;
    a.holdLeftAlt = J.GetNumber(L"holdLeftAlt", 0) != 0;
    a.holdRightAlt = J.GetNumber(L"holdRightAlt", 0) != 0;
    a.holdLeftShift = J.GetNumber(L"holdLeftShift", 0) != 0;
    a.holdRightShift = J.GetNumber(L"holdRightShift", 0) != 0;
    a.clickCount = static_cast<int>(J.GetNumber(L"clickCount", 1));
    if (a.clickCount < 1) a.clickCount = 1;
    if (a.clickCount > 100000) a.clickCount = 100000;
    {
        double durationDefault = 0.1;
        if (ActionCarriesRecordingPreDelay(a.type)) durationDefault = 0.0;
        a.duration = J.GetNumber(L"duration", durationDefault);
    }
    a.randomDuration = J.GetNumber(L"randomDuration", 0.0);
    a.timingUs = static_cast<uint64_t>(std::max(0.0,
        J.GetNumber(L"timingUs", 0.0)));
    a.loopCount = static_cast<int>(J.GetNumber(L"loopCount", -1));
    a.loopVarName = J.GetString(L"loopVarName");
    a.loopFromVar = J.GetNumber(L"loopFromVar", 0) != 0;
    a.loopVarExpr = J.GetString(L"loopVarExpr");
    a.blockName = J.GetString(L"blockName");
    a.targetPath = J.GetString(L"targetPath");
    a.playbackSpeed = quickscript::ClampPlaybackSpeed(
        J.GetNumber(L"playbackSpeed", 1.0));
    if (a.type == ActionType::RunMacro || a.type == ActionType::MousePlayback) {
        const auto useModePos = block.find(L"\"useMode\"");
        if (useModePos == std::wstring::npos) {
            a.useMode = kNestedUseModeInherit;
        } else {
            const auto colon = block.find(L':', useModePos + 9);
            size_t i = (colon == std::wstring::npos) ? block.size() : colon + 1;
            while (i < block.size() && (block[i] == L' ' || block[i] == L'\t'
                || block[i] == L'\n' || block[i] == L'\r')) {
                ++i;
            }
            if (i < block.size() && block[i] == L'"') {
                a.useMode = NestedUseModeFromText(J.GetString(L"useMode"));
            } else {
                a.useMode = NormalizeNestedUseMode(
                    static_cast<int>(J.GetNumber(L"useMode", kNestedUseModeInherit)));
            }
        }
        a.breakoutTimeSeconds = NormalizeBreakoutTimeSeconds(
            J.GetNumber(L"breakoutTimeSeconds", 0.0));
        const std::wstring nestedWm = ExtractNamedJsonObject(block, L"nestedWindowMode");
        if (!nestedWm.empty()) {
            a.nestedWindowMode = windowmode::ParseWindowModeConfigObject(nestedWm, false);
        }
    }
    a.shortcutPreset = static_cast<int>(J.GetNumber(L"shortcutPreset", 0));
    a.inputText = J.GetString(L"inputText");
    a.charInterval = J.GetNumber(L"charInterval", 0.01);
    a.parseEscapes = J.GetBool(L"parseEscapes", false);
    a.scrollVertical = J.GetNumber(L"scrollVertical", 1) != 0;
    a.scrollHorizontal = J.GetNumber(L"scrollHorizontal", 0) != 0;
    a.scrollSteps = static_cast<int>(J.GetNumber(L"scrollSteps", 1));
    a.scrollDirection = static_cast<int>(J.GetNumber(L"scrollDirection", 0));
    if (!coordsNormalized) {
        a.searchX1 = static_cast<int>(J.GetNumber(L"searchX1", 0));
        a.searchY1 = static_cast<int>(J.GetNumber(L"searchY1", 0));
        a.searchX2 = static_cast<int>(J.GetNumber(L"searchX2", 0));
        a.searchY2 = static_cast<int>(J.GetNumber(L"searchY2", 0));
    }
    a.searchFullScreen = J.GetNumber(L"searchFullScreen", 1) != 0;
    a.imageUseVar = J.GetNumber(L"imageUseVar", 0) != 0;
    a.imagePath = J.GetString(L"imagePath");
    a.imagePaths = ExtractJsonStringArray(block, L"imagePaths");
    a.imageUseVars.clear();
    for (int flag : ExtractJsonIntArray(block, L"imageUseVars")) {
        a.imageUseVars.push_back(flag != 0 ? 1 : 0);
    }
    if (a.imageUseVars.empty() && a.imageUseVar) {
        a.imageUseVars.push_back(1);
    }
    while (a.imageUseVars.size() < a.imagePaths.size()) a.imageUseVars.push_back(0);
    if (a.imageUseVars.size() > a.imagePaths.size()) {
        a.imageUseVars.resize(a.imagePaths.size());
    }
    const bool pathIsVar = a.imageUseVar || (!a.imageUseVars.empty() && a.imageUseVars[0] != 0);
    if (!a.imagePath.empty() && !pathIsVar) {
        a.imagePath = ResolveImagePath(a.imagePath);
    }
    for (size_t i = 0; i < a.imagePaths.size(); ++i) {
        const bool useVar = i < a.imageUseVars.size() && a.imageUseVars[i] != 0;
        if (!useVar && !a.imagePaths[i].empty()) {
            a.imagePaths[i] = ResolveImagePath(a.imagePaths[i]);
        }
    }
    a.multiMatchMode = static_cast<int>(J.GetNumber(L"multiMatchMode", 0));
    a.multiMatchMax = static_cast<int>(J.GetNumber(L"multiMatchMax", kMultiMatchMaxHits));
    a.multiMatchSort = static_cast<int>(J.GetNumber(L"multiMatchSort", 0));
    a.matchThreshold = J.GetNumber(L"matchThreshold", 65.0);
    a.perfectMatch = J.GetNumber(L"perfectMatch", 0) != 0;
    a.imageScale = J.GetNumber(L"imageScale", 1.0);
    a.imageScaleMin = J.GetNumber(L"imageScaleMin", a.imageScale);
    a.imageScaleMax = J.GetNumber(L"imageScaleMax", a.imageScale);
    a.findImageFollowUp = static_cast<int>(J.GetNumber(L"findImageFollowUp", 0));
    if (a.findImageFollowUp < 0) a.findImageFollowUp = 0;
    if (a.type == ActionType::FindColor) {
        if (a.findImageFollowUp > 2) a.findImageFollowUp = 2;
    } else if (a.type == ActionType::MultiMatch) {
        if (a.findImageFollowUp > 2) a.findImageFollowUp = 2;
    } else if (a.findImageFollowUp > 3) {
        a.findImageFollowUp = 3;
    }
    if (!coordsNormalized) {
        a.offsetX = static_cast<int>(J.GetNumber(L"offsetX", 0));
        a.offsetY = static_cast<int>(J.GetNumber(L"offsetY", 0));
    }
    a.findUntilFound = J.GetNumber(L"findUntilFound", 0) != 0;
    a.findTimeExpr = J.GetString(L"findTimeExpr");
    if (a.findTimeExpr.empty()) a.findTimeExpr = L"0";
    // 保存图片无模板是纯截屏，不需要等图；保存匹配度/有模板的保存图片与点击/移动一样走 findTimeExpr。
    if (a.type == ActionType::FindImage
        && a.findImageFollowUp == 3
        && a.imagePath.empty()) {
        a.findTimeExpr = L"0";
    }
    a.matchVarName = J.GetString(L"matchVarName");
    if (a.matchVarName.empty()) {
        if (a.type == ActionType::TextRecognition) a.matchVarName = L"a";
        else if (a.type == ActionType::GetColor || a.type == ActionType::FindColor
            || a.type == ActionType::ColorMatch) a.matchVarName = L"colorRet";
        else if (a.findImageFollowUp == 3) a.matchVarName = L"image";
        else a.matchVarName = L"matchRet";
    }
    a.colorR = static_cast<int>(J.GetNumber(L"colorR", 0));
    a.colorG = static_cast<int>(J.GetNumber(L"colorG", 0));
    a.colorB = static_cast<int>(J.GetNumber(L"colorB", 0));
    a.colorTolerance = static_cast<int>(J.GetNumber(L"colorTolerance", 16));
    if (a.colorTolerance < 0) a.colorTolerance = 0;
    if (a.colorTolerance > 255) a.colorTolerance = 255;
    a.ocrResultMode = static_cast<int>(J.GetNumber(L"ocrResultMode", 0));
    a.ocrRegionByImage = J.GetNumber(L"ocrRegionByImage", 0) != 0;
    a.ocrDigitsOnly = J.GetNumber(L"ocrDigitsOnly", 0) != 0;
    a.ocrSearchText = J.GetString(L"ocrSearchText");
    a.ocrFollowUp = static_cast<int>(J.GetNumber(L"ocrFollowUp", 0));
    a.conditionExpr = J.GetString(L"conditionExpr");
    a.gotoStepExpr = J.GetString(L"gotoStepExpr");
    a.resumeAfterWatch = J.GetNumber(L"resumeAfterWatch", 1) != 0;
    a.watchMode = ParseWatchModeField(block);
    {
        // 缺 watchPollSeconds 时 ExtractNumber 默认 1 会挡住 watchPollInterval 别名
        double poll = J.GetNumber(L"watchPollSeconds", 0.0);
        if (!(poll > 0.0)) poll = J.GetNumber(L"watchPollInterval", 0.0);
        if (!(poll > 0.0)) poll = 1.0;
        if (poll < 0.05) poll = 0.05;
        if (poll > 3600.0) poll = 3600.0;
        a.watchPollSeconds = poll;
    }
    a.computeCode = J.GetString(L"computeCode");
    if (a.type == ActionType::VarCompute && a.computeCode.empty())
        a.computeCode = a.inputText;
    a.matchFileNameOnly = J.GetNumber(L"matchFileNameOnly", 0) != 0;
    // imageRegion*：模板内相对偏移。旧 OCR 锚点脚本把相对值写在 search* 上，需迁移。
    const bool hasImageRegionKey = block.find(L"\"imageRegionX1\"") != std::wstring::npos;
    if (coordsNormalized) {
        a.nImageRegionX1 = J.GetNumber(L"imageRegionX1", 0.0);
        a.nImageRegionY1 = J.GetNumber(L"imageRegionY1", 0.0);
        a.nImageRegionX2 = J.GetNumber(L"imageRegionX2", 0.0);
        a.nImageRegionY2 = J.GetNumber(L"imageRegionY2", 0.0);
        a.imageRegionX1 = static_cast<int>(J.GetNumber(L"imageRegionX1", 0));
        a.imageRegionY1 = static_cast<int>(J.GetNumber(L"imageRegionY1", 0));
        a.imageRegionX2 = static_cast<int>(J.GetNumber(L"imageRegionX2", 0));
        a.imageRegionY2 = static_cast<int>(J.GetNumber(L"imageRegionY2", 0));
    } else {
        a.imageRegionX1 = static_cast<int>(J.GetNumber(L"imageRegionX1", 0));
        a.imageRegionY1 = static_cast<int>(J.GetNumber(L"imageRegionY1", 0));
        a.imageRegionX2 = static_cast<int>(J.GetNumber(L"imageRegionX2", 0));
        a.imageRegionY2 = static_cast<int>(J.GetNumber(L"imageRegionY2", 0));
    }
    if (a.type == ActionType::TextRecognition && a.ocrRegionByImage && !hasImageRegionKey) {
        // 旧格式：search* 存相对偏移；识别区改为全屏绝对搜索
        if (coordsNormalized) {
            a.nImageRegionX1 = a.nSearchX1;
            a.nImageRegionY1 = a.nSearchY1;
            a.nImageRegionX2 = a.nSearchX2;
            a.nImageRegionY2 = a.nSearchY2;
            a.nSearchX1 = a.nSearchY1 = a.nSearchX2 = a.nSearchY2 = 0.0;
        } else {
            a.imageRegionX1 = a.searchX1;
            a.imageRegionY1 = a.searchY1;
            a.imageRegionX2 = a.searchX2;
            a.imageRegionY2 = a.searchY2;
            a.searchX1 = a.searchY1 = a.searchX2 = a.searchY2 = 0;
        }
        a.searchFullScreen = true;
    }
    // ── AI 动作字段 ──
    a.aiPrompt = J.GetString(L"aiPrompt");
    a.aiOutputVarName = J.GetString(L"aiOutputVarName");
    a.aiOutputType = static_cast<int>(J.GetNumber(L"aiOutputType", 0));
    a.aiModelName = J.GetString(L"aiModelName");
    a.aiContextMode = static_cast<int>(J.GetNumber(L"aiContextMode", 1));
    a.aiTimeoutSec = static_cast<int>(J.GetNumber(L"aiTimeoutSec", 30));
    a.aiImageScale = J.GetNumber(L"aiImageScale", 0.5);
    a.aiRegionByImage = J.GetNumber(L"aiRegionByImage", 0) != 0;
    a.aiImageUseVar = J.GetNumber(L"aiImageUseVar", 0) != 0;
    a.aiTargetImagePath = J.GetString(L"aiTargetImagePath");
    if (!a.aiTargetImagePath.empty() && !a.aiImageUseVar) {
        a.aiTargetImagePath = ResolveImagePath(a.aiTargetImagePath);
    }
    a.aiSearchRegion = static_cast<int>(J.GetNumber(L"aiSearchRegion", 0));
    if (!coordsNormalized) {
        a.aiSearchX1 = static_cast<int>(J.GetNumber(L"aiSearchX1", 0));
        a.aiSearchY1 = static_cast<int>(J.GetNumber(L"aiSearchY1", 0));
        a.aiSearchX2 = static_cast<int>(J.GetNumber(L"aiSearchX2", 0));
        a.aiSearchY2 = static_cast<int>(J.GetNumber(L"aiSearchY2", 0));
    }
    a.aiMaxSteps = static_cast<int>(J.GetNumber(L"aiMaxSteps", 10));
    a.aiWithImage = J.GetNumber(L"aiWithImage", 1) != 0;
    a.aiLogicConvert = J.GetNumber(L"aiLogicConvert", 0) != 0;
    a.aiLogicBlockName = J.GetString(L"aiLogicBlockName");
    a.aiFallbackValue = J.GetString(L"aiFallbackValue");
    a.recordedCapturePath = J.GetString(L"recordedCapturePath");
    if (!a.recordedCapturePath.empty()) {
        a.recordedCapturePath = ResolveImagePath(a.recordedCapturePath);
    }
    a.captureOffsetX = static_cast<int>(J.GetNumber(L"captureOffsetX", 0));
    a.captureOffsetY = static_cast<int>(J.GetNumber(L"captureOffsetY", 0));
    NormalizeMultiMatchFields(a);
    return a;
}

void WriteActionJson(std::wstringstream& file, const ScriptAction& a, bool last) {
    // 默认 precision=6 会把密集轨迹的 n*/duration 写毛，热键保存/导入再读后坐标漂移。
    file << std::setprecision(std::numeric_limits<double>::max_digits10);
    file << L"    {\n";
    file << L"      \"type\": \"" << JsonType(a.type) << L"\",\n";
    file << L"      \"text\": \"" << EscapeJson(a.customText) << L"\",\n";
    file << L"      \"remark\": \"" << EscapeJson(a.remark) << L"\",\n";
    file << L"      \"no\": " << a.originalNo << L",\n";
    file << L"      \"indent\": " << a.indent << L",\n";
    if (a.windowRelative) {
        file << L"      \"windowRelative\": 1,\n";
    }
    if (a.type == ActionType::MoveMouseRelative || !a.coordsAreNormalized) {
        file << L"      \"x\": " << a.x << L",\n";
        file << L"      \"y\": " << a.y << L",\n";
        file << L"      \"randomX\": " << a.randomX << L",\n";
        file << L"      \"randomY\": " << a.randomY << L",\n";
        file << L"      \"endX\": " << a.endX << L",\n";
        file << L"      \"endY\": " << a.endY << L",\n";
        file << L"      \"randomEndX\": " << a.randomEndX << L",\n";
        file << L"      \"randomEndY\": " << a.randomEndY << L",\n";
    } else {
        // 写入归一化坐标（0.0–1.0 浮点数）
        file << L"      \"x\": " << a.nx << L",\n";
        file << L"      \"y\": " << a.ny << L",\n";
        file << L"      \"randomX\": " << a.nRandomX << L",\n";
        file << L"      \"randomY\": " << a.nRandomY << L",\n";
        file << L"      \"endX\": " << a.nEndX << L",\n";
        file << L"      \"endY\": " << a.nEndY << L",\n";
        file << L"      \"randomEndX\": " << a.nRandomEndX << L",\n";
        file << L"      \"randomEndY\": " << a.nRandomEndY << L",\n";
    }
    file << L"      \"moveFromVar\": " << (a.moveFromVar ? 1 : 0) << L",\n";
    file << L"      \"imageLocate\": " << (a.imageLocate ? 1 : 0) << L",\n";
    file << L"      \"moveVarExprX\": \"" << EscapeJson(a.moveVarExprX) << L"\",\n";
    file << L"      \"moveVarExprY\": \"" << EscapeJson(a.moveVarExprY) << L"\",\n";
    file << L"      \"button\": \"" << JsonButton(a.button) << L"\",\n";
    file << L"      \"keyText\": \"" << EscapeJson(a.keyText) << L"\",\n";
    file << L"      \"keyVk\": " << a.keyVk << L",\n";
    file << L"      \"holdLeftWin\": " << (a.holdLeftWin ? 1 : 0) << L",\n";
    file << L"      \"holdRightWin\": " << (a.holdRightWin ? 1 : 0) << L",\n";
    file << L"      \"holdLeftCtrl\": " << (a.holdLeftCtrl ? 1 : 0) << L",\n";
    file << L"      \"holdRightCtrl\": " << (a.holdRightCtrl ? 1 : 0) << L",\n";
    file << L"      \"holdLeftAlt\": " << (a.holdLeftAlt ? 1 : 0) << L",\n";
    file << L"      \"holdRightAlt\": " << (a.holdRightAlt ? 1 : 0) << L",\n";
    file << L"      \"holdLeftShift\": " << (a.holdLeftShift ? 1 : 0) << L",\n";
    file << L"      \"holdRightShift\": " << (a.holdRightShift ? 1 : 0) << L",\n";
    file << L"      \"clickCount\": " << a.clickCount << L",\n";
    file << L"      \"duration\": " << a.duration << L",\n";
    file << L"      \"randomDuration\": " << a.randomDuration << L",\n";
    if (a.timingUs > 0) {
        file << L"      \"timingUs\": " << a.timingUs << L",\n";
    }
    file << L"      \"loopCount\": " << a.loopCount << L",\n";
    file << L"      \"loopVarName\": \"" << EscapeJson(a.loopVarName) << L"\",\n";
    file << L"      \"loopFromVar\": " << (a.loopFromVar ? 1 : 0) << L",\n";
    file << L"      \"loopVarExpr\": \"" << EscapeJson(a.loopVarExpr) << L"\",\n";
    file << L"      \"blockName\": \"" << EscapeJson(a.blockName) << L"\",\n";
    file << L"      \"targetPath\": \"" << EscapeJson(a.targetPath) << L"\",\n";
    if (a.type == ActionType::MousePlayback) {
        file << L"      \"playbackSpeed\": " << a.playbackSpeed << L",\n";
    }
    if (a.type == ActionType::RunMacro || a.type == ActionType::MousePlayback) {
        file << L"      \"useMode\": " << NormalizeNestedUseMode(a.useMode) << L",\n";
        file << L"      \"breakoutTimeSeconds\": " << a.breakoutTimeSeconds << L",\n";
        windowmode::WindowModeScriptConfig nestedWm = a.nestedWindowMode;
        if (a.useMode == kNestedUseModeWindow || a.useMode == kNestedUseModeBackground) {
            nestedWm.enabled = true;
            nestedWm.executionKind = (a.useMode == kNestedUseModeBackground)
                ? windowmode::WindowModeExecutionKind::BackgroundWindow
                : windowmode::WindowModeExecutionKind::HiddenDesktop;
        } else if (NestedWindowModeHasIdentity(nestedWm) || nestedWm.windowRelativeCoordinates) {
            nestedWm.enabled = true;
        } else {
            nestedWm.enabled = false;
        }
        std::wstring nestedWmJson;
        windowmode::WriteNestedWindowModeJson(nestedWmJson, nestedWm, true);
        file << nestedWmJson;
    }
    file << L"      \"shortcutPreset\": " << a.shortcutPreset << L",\n";
    file << L"      \"inputText\": \"" << EscapeJson(a.inputText) << L"\",\n";
    file << L"      \"charInterval\": " << a.charInterval << L",\n";
    file << L"      \"parseEscapes\": " << (a.parseEscapes ? 1 : 0) << L",\n";
    file << L"      \"scrollVertical\": " << (a.scrollVertical ? 1 : 0) << L",\n";
    file << L"      \"scrollHorizontal\": " << (a.scrollHorizontal ? 1 : 0) << L",\n";
    file << L"      \"scrollSteps\": " << a.scrollSteps << L",\n";
    file << L"      \"scrollDirection\": " << a.scrollDirection << L",\n";
    if (a.coordsAreNormalized) {
        file << L"      \"searchX1\": " << a.nSearchX1 << L",\n";
        file << L"      \"searchY1\": " << a.nSearchY1 << L",\n";
        file << L"      \"searchX2\": " << a.nSearchX2 << L",\n";
        file << L"      \"searchY2\": " << a.nSearchY2 << L",\n";
    } else {
        file << L"      \"searchX1\": " << a.searchX1 << L",\n";
        file << L"      \"searchY1\": " << a.searchY1 << L",\n";
        file << L"      \"searchX2\": " << a.searchX2 << L",\n";
        file << L"      \"searchY2\": " << a.searchY2 << L",\n";
    }
    file << L"      \"searchFullScreen\": " << (a.searchFullScreen ? 1 : 0) << L",\n";
    file << L"      \"imageUseVar\": " << (a.imageUseVar ? 1 : 0) << L",\n";
    const std::wstring savedImagePath = [&]() -> std::wstring {
        if (a.imageUseVar) return a.imagePath; // 变量名或用户路径，原样保存
        if (a.type == ActionType::FindImage && !a.imagePath.empty()) {
            return ImagePathForJson(EnsureImageInLibrary(a.imagePath));
        }
        if (a.type == ActionType::MultiMatch && !a.imagePath.empty() && !a.imageUseVar) {
            return ImagePathForJson(EnsureImageInLibrary(a.imagePath));
        }
        if (a.type == ActionType::WatchImage && !a.imagePath.empty()) {
            return ImagePathForJson(EnsureImageInLibrary(a.imagePath));
        }
        if (a.imageLocate && !a.imagePath.empty()
            && (a.type == ActionType::MouseDrag
                || a.type == ActionType::GetColor
                || a.type == ActionType::ColorMatch
                || a.type == ActionType::FindColor)) {
            return ImagePathForJson(EnsureImageInLibrary(a.imagePath));
        }
        if (a.type == ActionType::TextRecognition && a.ocrRegionByImage && !a.imagePath.empty()) {
            return ImagePathForJson(EnsureImageInLibrary(a.imagePath));
        }
        return a.imagePath;
    }();
    file << L"      \"imagePath\": \"" << EscapeJson(savedImagePath) << L"\",\n";
    if (a.type == ActionType::MultiMatch) {
        file << L"      \"imagePaths\": [";
        for (size_t i = 0; i < a.imagePaths.size(); ++i) {
            if (i) file << L", ";
            std::wstring p = a.imagePaths[i];
            const bool useVar = MultiMatchSlotUseVar(a, static_cast<int>(i));
            if (!useVar && !p.empty()) p = ImagePathForJson(EnsureImageInLibrary(p));
            file << L"\"" << EscapeJson(p) << L"\"";
        }
        file << L"],\n";
        file << L"      \"imageUseVars\": [";
        for (size_t i = 0; i < a.imagePaths.size(); ++i) {
            if (i) file << L", ";
            file << (MultiMatchSlotUseVar(a, static_cast<int>(i)) ? 1 : 0);
        }
        file << L"],\n";
        file << L"      \"multiMatchMode\": " << a.multiMatchMode << L",\n";
        file << L"      \"multiMatchMax\": " << a.multiMatchMax << L",\n";
        file << L"      \"multiMatchSort\": " << a.multiMatchSort << L",\n";
    }
    file << L"      \"matchThreshold\": " << a.matchThreshold << L",\n";
    file << L"      \"perfectMatch\": " << (a.perfectMatch ? 1 : 0) << L",\n";
    file << L"      \"imageScale\": " << a.imageScale << L",\n";
    file << L"      \"imageScaleMin\": " << a.imageScaleMin << L",\n";
    file << L"      \"imageScaleMax\": " << a.imageScaleMax << L",\n";
    file << L"      \"findImageFollowUp\": " << a.findImageFollowUp << L",\n";
    if (a.coordsAreNormalized) {
        file << L"      \"offsetX\": " << a.nOffsetX << L",\n";
        file << L"      \"offsetY\": " << a.nOffsetY << L",\n";
    } else {
        file << L"      \"offsetX\": " << a.offsetX << L",\n";
        file << L"      \"offsetY\": " << a.offsetY << L",\n";
    }
    file << L"      \"findUntilFound\": " << (a.findUntilFound ? 1 : 0) << L",\n";
    file << L"      \"findTimeExpr\": \"" << EscapeJson(a.findTimeExpr) << L"\",\n";
    file << L"      \"matchVarName\": \"" << EscapeJson(a.matchVarName) << L"\",\n";
    file << L"      \"colorR\": " << a.colorR << L",\n";
    file << L"      \"colorG\": " << a.colorG << L",\n";
    file << L"      \"colorB\": " << a.colorB << L",\n";
    file << L"      \"colorTolerance\": " << a.colorTolerance << L",\n";
    file << L"      \"ocrResultMode\": " << a.ocrResultMode << L",\n";
    file << L"      \"ocrRegionByImage\": " << (a.ocrRegionByImage ? 1 : 0) << L",\n";
    file << L"      \"ocrDigitsOnly\": " << (a.ocrDigitsOnly ? 1 : 0) << L",\n";
    file << L"      \"ocrSearchText\": \"" << EscapeJson(a.ocrSearchText) << L"\",\n";
    file << L"      \"ocrFollowUp\": " << a.ocrFollowUp << L",\n";
    if (a.coordsAreNormalized) {
        file << L"      \"imageRegionX1\": " << a.nImageRegionX1 << L",\n";
        file << L"      \"imageRegionY1\": " << a.nImageRegionY1 << L",\n";
        file << L"      \"imageRegionX2\": " << a.nImageRegionX2 << L",\n";
        file << L"      \"imageRegionY2\": " << a.nImageRegionY2 << L",\n";
    } else {
        file << L"      \"imageRegionX1\": " << a.imageRegionX1 << L",\n";
        file << L"      \"imageRegionY1\": " << a.imageRegionY1 << L",\n";
        file << L"      \"imageRegionX2\": " << a.imageRegionX2 << L",\n";
        file << L"      \"imageRegionY2\": " << a.imageRegionY2 << L",\n";
    }
    file << L"      \"conditionExpr\": \"" << EscapeJson(a.conditionExpr) << L"\",\n";
    file << L"      \"gotoStepExpr\": \"" << EscapeJson(a.gotoStepExpr) << L"\",\n";
    file << L"      \"resumeAfterWatch\": " << (a.resumeAfterWatch ? 1 : 0) << L",\n";
    file << L"      \"watchMode\": " << a.watchMode << L",\n";
    file << L"      \"watchPollSeconds\": " << a.watchPollSeconds << L",\n";
    file << L"      \"computeCode\": \"" << EscapeJson(a.computeCode) << L"\",\n";
    file << L"      \"matchFileNameOnly\": " << (a.matchFileNameOnly ? 1 : 0) << L",\n";
    // ── AI 动作字段 ──
    file << L"      \"aiPrompt\": \"" << EscapeJson(a.aiPrompt) << L"\",\n";
    file << L"      \"aiOutputVarName\": \"" << EscapeJson(a.aiOutputVarName) << L"\",\n";
    file << L"      \"aiOutputType\": " << a.aiOutputType << L",\n";
    file << L"      \"aiModelName\": \"" << EscapeJson(a.aiModelName) << L"\",\n";
    file << L"      \"aiContextMode\": " << a.aiContextMode << L",\n";
    file << L"      \"aiTimeoutSec\": " << a.aiTimeoutSec << L",\n";
    file << L"      \"aiImageScale\": " << a.aiImageScale << L",\n";
    file << L"      \"aiRegionByImage\": " << (a.aiRegionByImage ? 1 : 0) << L",\n";
    file << L"      \"aiImageUseVar\": " << (a.aiImageUseVar ? 1 : 0) << L",\n";
    const std::wstring savedAiImagePath = [&]() -> std::wstring {
        if (a.aiImageUseVar) return a.aiTargetImagePath;
        if (!a.aiTargetImagePath.empty()) {
            return ImagePathForJson(EnsureImageInLibrary(a.aiTargetImagePath));
        }
        return a.aiTargetImagePath;
    }();
    file << L"      \"aiTargetImagePath\": \"" << EscapeJson(savedAiImagePath) << L"\",\n";
    file << L"      \"aiSearchRegion\": " << a.aiSearchRegion << L",\n";
    if (a.coordsAreNormalized) {
        file << L"      \"aiSearchX1\": " << a.nAiSearchX1 << L",\n";
        file << L"      \"aiSearchY1\": " << a.nAiSearchY1 << L",\n";
        file << L"      \"aiSearchX2\": " << a.nAiSearchX2 << L",\n";
        file << L"      \"aiSearchY2\": " << a.nAiSearchY2 << L",\n";
    } else {
        file << L"      \"aiSearchX1\": " << a.aiSearchX1 << L",\n";
        file << L"      \"aiSearchY1\": " << a.aiSearchY1 << L",\n";
        file << L"      \"aiSearchX2\": " << a.aiSearchX2 << L",\n";
        file << L"      \"aiSearchY2\": " << a.aiSearchY2 << L",\n";
    }
    file << L"      \"aiMaxSteps\": " << a.aiMaxSteps << L",\n";
    file << L"      \"aiWithImage\": " << (a.aiWithImage ? 1 : 0) << L",\n";
    file << L"      \"aiLogicConvert\": " << (a.aiLogicConvert ? 1 : 0) << L",\n";
    file << L"      \"aiLogicBlockName\": \"" << EscapeJson(a.aiLogicBlockName) << L"\",\n";
    file << L"      \"aiFallbackValue\": \"" << EscapeJson(a.aiFallbackValue) << L"\"";
    if (!a.recordedCapturePath.empty()) {
        const std::wstring savedCap = ImagePathForJson(EnsureImageInLibrary(a.recordedCapturePath));
        file << L",\n      \"recordedCapturePath\": \"" << EscapeJson(savedCap) << L"\"";
    }
    if (!a.recordedCapturePath.empty() || a.captureOffsetX != 0 || a.captureOffsetY != 0) {
        file << L",\n      \"captureOffsetX\": " << a.captureOffsetX;
        file << L",\n      \"captureOffsetY\": " << a.captureOffsetY;
    }
    file << L"\n";
    file << L"    }" << (last ? L"\n" : L",\n");
}

std::wstring ScriptActionToJsonString(const ScriptAction& a) {
    std::wstringstream file;
    WriteActionJson(file, a, true);
    return file.str();
}

namespace {

bool AnyWindowRelativeAction(const std::vector<ScriptAction>& actions) {
    for (const auto& a : actions) {
        if (a.windowRelative) return true;
    }
    return false;
}

void ApplyWindowRelativePlaybackConfig(ScriptFileData& data, const std::wstring& path) {
    const bool anyRel = data.windowMode.windowRelativeCoordinates
        || AnyWindowRelativeAction(data.actions);
    windowmode::FinalizeWindowModeForPlayback(data.windowMode, anyRel,
        IsRecordingScriptPath(path));
}

bool ScriptNormValuesLookLikePixels(const std::vector<ScriptAction>& actions) {
    for (const auto& a : actions) {
        if (!a.coordsAreNormalized) continue;
        // 找图定位的 x/y 相对模板，偏移可超出图外（nx>1.5 仍合法）
        const bool templateRelXy = a.imageLocate
            && (a.type == ActionType::MouseDrag
                || a.type == ActionType::GetColor
                || a.type == ActionType::ColorMatch);
        if (!templateRelXy && (a.nx > 1.5 || a.ny > 1.5)) return true;
        if (a.nSearchX2 > 1.5 || a.nSearchY2 > 1.5
            || a.nAiSearchX2 > 1.5 || a.nAiSearchY2 > 1.5) {
            return true;
        }
    }
    return false;
}

}  // namespace

void NormalizeInputTiming(ScriptFileData& data, const std::wstring& path,
    bool forceRecordingExpand) {
    if (data.inputTimingVersion >= kInputTimingVersionExplicitWaits) return;
    ExpandRecordingPreDelayPolicy policy{};
    policy.treatAsRecordingTimeline = forceRecordingExpand
        || IsRecordingScriptPath(path)
        || data.inputTimingVersion == 1;
    data.actions = ExpandRecordingPreDelaysToExplicitWaits(data.actions, policy);
    data.inputTimingVersion = kInputTimingVersionExplicitWaits;
}

ScriptFileData LoadScriptFileData(const std::wstring& path, bool denormForDisplay) {
    ScriptFileData data{};
    if (path.empty()) return data;
    const auto content = ReadAll(path);
    const qst::jsonutil::WideObjectView JC(content);
    data.scriptName = JC.GetString(L"scriptName");
    data.recordTime = JC.GetString(L"recordTime");
    data.durationSeconds = JC.GetNumber(L"durationSeconds", 0);
    data.recordingCaptureMode = static_cast<int>(
        JC.GetNumber(L"recordingCaptureMode", -1));
    data.inputTimingVersion = std::max(0, static_cast<int>(
        JC.GetNumber(L"inputTimingVersion", 0)));
    data.hotkey.text = JC.GetString(L"hotkeyText");
    data.hotkey.vk = static_cast<UINT>(JC.GetNumber(L"hotkeyVk", 0));
    data.hotkey.modifiers = static_cast<UINT>(JC.GetNumber(L"hotkeyModifiers", 0));
    data.hotkey.holdMode = JC.GetBool(L"hotkeyHold", false);
    data.hotkey.enabled = data.hotkey.vk != 0;
    data.breakoutTimeSeconds = NormalizeBreakoutTimeSeconds(
        JC.GetNumber(L"breakoutTimeSeconds", 0));
    data.windowMode = windowmode::ParseWindowModeJson(content);
    data.visualLayoutJson = ExtractNamedJsonObject(content, L"visualLayout");
    if (!VisualLayoutLooksValid(data.visualLayoutJson)) data.visualLayoutJson.clear();

    // 解析 coordMeta
    if (HasCoordMetaJson(content)) {
        data.coordMeta = ParseCoordMetaJson(content);
        data.coordsNormalized = true;
    } else {
        // 旧脚本：检查是否已是归一化格式（x/y 为 0.0–1.0 小数）
        // 启发式：读取第一个 action 的 x 值，若为小数且 <= 1.0 则视为归一化
        data.coordsNormalized = false;
    }

    // coordMeta 无效时回退为标准参考系
    if (data.coordsNormalized
        && (data.coordMeta.refWidth <= 0 || data.coordMeta.refHeight <= 0)) {
        data.coordMeta = StandardScriptCoordMeta();
    }

    const auto blocks = ExtractJsonActionBlocks(content);
    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto type = qst::jsonutil::WideObjectView(blocks[i]).GetString(L"type");
        if (!type.empty()) {
            data.actions.push_back(
                ParseScriptActionBlock(blocks[i], i, data.coordsNormalized));
        }
    }

    // coordMeta 存在但 actions 仍是像素值（早期/混合格式）→ 按 legacy 重解析
    if (data.coordsNormalized && ScriptNormValuesLookLikePixels(data.actions)) {
        data.actions.clear();
        data.coordsNormalized = false;
        for (size_t i = 0; i < blocks.size(); ++i) {
            const auto type = qst::jsonutil::WideObjectView(blocks[i]).GetString(L"type");
            if (!type.empty()) {
                data.actions.push_back(ParseScriptActionBlock(blocks[i], i, false));
            }
        }
    }

    // 旧脚本迁移：无 coordMeta 时，假设像素在标准 2560×1440 下，转为 n*
    if (!data.coordsNormalized && !data.actions.empty()) {
        data.coordMeta = StandardScriptCoordMeta();
        MigrateLegacyScriptToNormalized(data.actions, data.coordMeta);
        data.coordsNormalized = true;
    }

    // 编辑器显示：n* → 当前屏幕像素（运行副本由 PrepareScriptActionsForExecution 生成）
    if (denormForDisplay && data.coordsNormalized && !data.actions.empty()) {
        DenormalizeScriptToCurrentScreen(data.actions);
    }

    NormalizeInputTiming(data, path);
    ApplyWindowRelativePlaybackConfig(data, path);
    return data;
}

ScriptFileData ParseScriptContent(const std::wstring& content) {
    ScriptFileData data{};
    const qst::jsonutil::WideObjectView JC(content);
    if (content.empty()) return data;
    data.scriptName = JC.GetString(L"scriptName");
    data.recordTime = JC.GetString(L"recordTime");
    data.durationSeconds = JC.GetNumber(L"durationSeconds", 0);
    data.recordingCaptureMode = static_cast<int>(
        JC.GetNumber(L"recordingCaptureMode", -1));
    data.inputTimingVersion = std::max(0, static_cast<int>(
        JC.GetNumber(L"inputTimingVersion", 0)));
    data.hotkey.text = JC.GetString(L"hotkeyText");
    data.hotkey.vk = static_cast<UINT>(JC.GetNumber(L"hotkeyVk", 0));
    data.hotkey.modifiers = static_cast<UINT>(JC.GetNumber(L"hotkeyModifiers", 0));
    data.hotkey.holdMode = JC.GetBool(L"hotkeyHold", false);
    data.hotkey.enabled = data.hotkey.vk != 0;
    data.breakoutTimeSeconds = NormalizeBreakoutTimeSeconds(
        JC.GetNumber(L"breakoutTimeSeconds", 0));
    data.windowMode = windowmode::ParseWindowModeJson(content);
    data.visualLayoutJson = ExtractNamedJsonObject(content, L"visualLayout");
    if (!VisualLayoutLooksValid(data.visualLayoutJson)) data.visualLayoutJson.clear();

    if (HasCoordMetaJson(content)) {
        data.coordMeta = ParseCoordMetaJson(content);
        data.coordsNormalized = true;
    }

    if (data.coordsNormalized
        && (data.coordMeta.refWidth <= 0 || data.coordMeta.refHeight <= 0)) {
        data.coordMeta = StandardScriptCoordMeta();
    }

    const auto blocks = ExtractJsonActionBlocks(content);
    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto type = qst::jsonutil::WideObjectView(blocks[i]).GetString(L"type");
        if (!type.empty()) {
            data.actions.push_back(
                ParseScriptActionBlock(blocks[i], i, data.coordsNormalized));
        }
    }

    if (!data.coordsNormalized && !data.actions.empty()) {
        data.coordMeta = StandardScriptCoordMeta();
        MigrateLegacyScriptToNormalized(data.actions, data.coordMeta);
        data.coordsNormalized = true;
    }

    if (data.coordsNormalized && !data.actions.empty()) {
        DenormalizeScriptToCurrentScreen(data.actions);
    }

    // 无路径：仅 version==1 视为录制时间线；scripts 默认 0.1 不展开。
    // 无路径不得按录制复活窗口模式（鼠标宏 JSON 解析须尊重 enabled=0）。
    NormalizeInputTiming(data, L"");
    ApplyWindowRelativePlaybackConfig(data, L"");
    return data;
}

bool SaveScriptFileData(const std::wstring& path, const ScriptFileData& data) {
    ScriptFileData normalized = data;
    normalized.breakoutTimeSeconds = NormalizeBreakoutTimeSeconds(normalized.breakoutTimeSeconds);
    if (IsRecordingScriptPath(path)) {
        // 普通录制强制屏幕模式；窗口相对录制（标记或动作）保留目标窗口身份。
        const bool anyRel = data.windowMode.windowRelativeCoordinates
            || AnyWindowRelativeAction(data.actions);
        if (!anyRel) {
            normalized.windowMode = windowmode::DefaultWindowModeConfig();
        } else {
            windowmode::FinalizeWindowModeForPlayback(normalized.windowMode, true, true);
        }
        normalized.breakoutTimeSeconds = 0;
        normalized.inputTimingVersion = kInputTimingVersionExplicitWaits;
    } else if (normalized.windowMode.enabled) {
        normalized.breakoutTimeSeconds = 0;
    }
    if (normalized.inputTimingVersion > 0
        && normalized.inputTimingVersion < kInputTimingVersionExplicitWaits) {
        NormalizeInputTiming(normalized, path);
    }

    // 像素→n* 用当前屏幕；JSON coordMeta 固定为标准 2560×1440
    CoordMeta pixelMeta = CaptureCurrentCoordMeta(
        normalized.windowMode.enabled ? &normalized.windowMode : nullptr);
    CoordMeta storeMeta = BuildScriptCoordMetaForSave(pixelMeta);
    if (normalized.windowMode.windowRelativeCoordinates
        && normalized.windowMode.recordClientWidth > 0
        && normalized.windowMode.recordClientHeight > 0) {
        // 窗口相对脚本：capture 记客户区，供回放按窗口大小缩放（勿用虚拟屏）。
        storeMeta.captureWidth = normalized.windowMode.recordClientWidth;
        storeMeta.captureHeight = normalized.windowMode.recordClientHeight;
    }

    // 像素→n*：仅对「尚未归一化」的动作 Sync。
    // LoadScriptFileData(..., false) 只填 n*、x/y 仍为 0；若一律 Sync 会把坐标冲成原点，
    // 表现为改延时/改热键后移动丢失、点击跑到 (0,0)。
    std::vector<ScriptAction> saveActions = normalized.actions;
    {
        std::vector<ScriptAction> needSync;
        std::vector<size_t> needIdx;
        needSync.reserve(saveActions.size());
        needIdx.reserve(saveActions.size());
        for (size_t i = 0; i < saveActions.size(); ++i) {
            if (saveActions[i].coordsAreNormalized) continue;
            needIdx.push_back(i);
            needSync.push_back(saveActions[i]);
        }
        if (!needSync.empty()) {
            SyncNormFieldsFromPixels(needSync, pixelMeta);
            for (size_t k = 0; k < needIdx.size(); ++k)
                saveActions[needIdx[k]] = std::move(needSync[k]);
        }
    }

    std::wstringstream file;
    // 先写临时文件再 MoveFileEx 原子替换，避免并发保存（编辑器/Agent 同时写）撕裂文件。
    const std::wstring tmpPath = path + L".tmp";
    std::ofstream out(tmpPath, std::ios::binary);
    if (!out) return false;
    out.write("\xEF\xBB\xBF", 3);
    file << L"{\n";
    file << L"  \"scriptName\": \"" << EscapeJson(data.scriptName) << L"\",\n";
    file << L"  \"recordTime\": \"" << EscapeJson(data.recordTime) << L"\",\n";
    if (data.durationSeconds > 0) {
        file << L"  \"durationSeconds\": " << data.durationSeconds << L",\n";
    }
    if (normalized.recordingCaptureMode >= 0) {
        file << L"  \"recordingCaptureMode\": "
        << std::clamp(normalized.recordingCaptureMode, 0, 3) << L",\n";
    }
    if (normalized.inputTimingVersion > 0) {
        file << L"  \"inputTimingVersion\": " << normalized.inputTimingVersion << L",\n";
    }
    file << L"  \"hotkeyText\": \"" << EscapeJson(data.hotkey.text) << L"\",\n";
    file << L"  \"hotkeyVk\": " << data.hotkey.vk << L",\n";
    file << L"  \"hotkeyModifiers\": " << data.hotkey.modifiers << L",\n";
    file << L"  \"hotkeyHold\": " << (data.hotkey.holdMode ? 1 : 0) << L",\n";
    if (!IsRecordingScriptPath(path)) {
        file << L"  \"breakoutTimeSeconds\": " << normalized.breakoutTimeSeconds << L",\n";
    }

    // 写入标准 coordMeta（2560×1440）
    {
        std::wstring coordMetaJson;
        WriteCoordMetaJson(coordMetaJson, storeMeta, true);
        file << coordMetaJson;
    }

    std::wstring wmJson;
    windowmode::WriteWindowModeJson(wmJson, normalized.windowMode, true);
    file << wmJson;
    file << L"  \"actions\": [\n";
    for (size_t i = 0; i < saveActions.size(); ++i) {
        WriteActionJson(file, saveActions[i], i + 1 == saveActions.size());
    }
    file << L"  ]\n}\n";
    const auto bytes = ToUtf8(file.str());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const bool ok = out.good();
    out.close();
    if (!ok) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    // 原子替换：已有目标文件用 MOVEFILE_REPLACE_EXISTING，失败再回退直接写。
    if (MoveFileExW(tmpPath.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        // 目标被占用（如被另一个进程只读打开）时回退：尽力而为。
        if (!MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            std::ofstream direct(path, std::ios::binary);
            direct.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            const bool directOk = direct.good();
            direct.close();
            DeleteFileW(tmpPath.c_str());
            return directOk;
        }
    }
    return true;
}

namespace {

int RetargetNestedLibraryRefs(bool prefix, const std::wstring& oldRef, const std::wstring& newRef) {
    if (oldRef.empty() || newRef.empty() || LibraryPathsEqual(oldRef, newRef)) return 0;
    int filesChanged = 0;
    auto visitRoot = [&](const std::wstring& root) {
        std::vector<ScriptFileEntry> files;
        EnumerateScriptJsonFiles(root, files);
        for (const auto& fe : files) {
            if (LibraryPathsEqual(fe.path, newRef)) continue;
            ScriptFileData data = LoadScriptFileData(fe.path, false);
            bool changed = false;
            for (auto& a : data.actions) {
                if (a.type != ActionType::RunMacro && a.type != ActionType::MousePlayback)
                    continue;
                if (a.targetPath.empty()) continue;
                if (prefix) {
                    if (RelocatePathUnderDir(a.targetPath, oldRef, newRef)) changed = true;
                } else if (LibraryPathsEqual(a.targetPath, oldRef)) {
                    a.targetPath = newRef;
                    changed = true;
                }
            }
            if (changed && SaveScriptFileData(fe.path, data)) ++filesChanged;
        }
    };
    visitRoot(ScriptsDir());
    visitRoot(RecordingsDir());
    return filesChanged;
}

}  // namespace

int RetargetNestedLibraryScriptPaths(const std::wstring& oldPath, const std::wstring& newPath) {
    return RetargetNestedLibraryRefs(false, oldPath, newPath);
}

int RetargetNestedLibraryScriptPathPrefix(const std::wstring& oldDir, const std::wstring& newDir) {
    return RetargetNestedLibraryRefs(true, oldDir, newDir);
}

namespace {

std::wstring NormalizeScriptPathKey(const std::wstring& scriptPath) {
    wchar_t full[MAX_PATH]{};
    const DWORD n = GetFullPathNameW(scriptPath.c_str(), MAX_PATH, full, nullptr);
    std::wstring key = (n > 0 && n < MAX_PATH) ? full : scriptPath;
    for (wchar_t& ch : key) {
        if (ch == L'/') ch = L'\\';
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return key;
}

uint64_t Fnv1a64W(const std::wstring& s) {
    uint64_t h = 14695981039346656037ull;
    for (wchar_t c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

std::wstring HexU64(uint64_t v) {
    wchar_t buf[17];
    swprintf_s(buf, L"%016llX", static_cast<unsigned long long>(v));
    return buf;
}

}  // namespace

std::wstring VisualLayoutCachePathForScript(const std::wstring& scriptPath) {
    const std::wstring dir = AppDir() + L"\\cache\\visual";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir + L"\\" + HexU64(Fnv1a64W(NormalizeScriptPathKey(scriptPath))) + L".json";
}

bool SaveVisualLayoutCache(const std::wstring& scriptPath, const std::wstring& json) {
    if (scriptPath.empty() || json.size() < 2 || json.front() != L'{') return false;
    const std::wstring path = VisualLayoutCachePathForScript(scriptPath);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const auto bytes = ToUtf8(json);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

bool LoadVisualLayoutCache(const std::wstring& scriptPath, std::wstring& jsonOut) {
    jsonOut.clear();
    if (scriptPath.empty()) return false;
    const std::wstring path = VisualLayoutCachePathForScript(scriptPath);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    jsonOut = ReadAll(path);
    while (!jsonOut.empty() && (jsonOut.front() == 0xFEFF || jsonOut.front() == L' ' || jsonOut.front() == L'\n')) {
        jsonOut.erase(jsonOut.begin());
    }
    return jsonOut.size() >= 2 && jsonOut.front() == L'{';
}

void DeleteVisualLayoutCache(const std::wstring& scriptPath) {
    if (scriptPath.empty()) return;
    const std::wstring path = VisualLayoutCachePathForScript(scriptPath);
    DeleteFileW(path.c_str());
}

void MoveVisualLayoutCache(const std::wstring& oldScriptPath, const std::wstring& newScriptPath) {
    if (oldScriptPath.empty() || newScriptPath.empty()) return;
    if (_wcsicmp(oldScriptPath.c_str(), newScriptPath.c_str()) == 0) return;
    std::wstring json;
    if (!LoadVisualLayoutCache(oldScriptPath, json)) {
        DeleteVisualLayoutCache(newScriptPath);
        return;
    }
    SaveVisualLayoutCache(newScriptPath, json);
    DeleteVisualLayoutCache(oldScriptPath);
}
