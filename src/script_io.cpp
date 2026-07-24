#include "script_io.h"

#include "action_utils.h"
#include "coord_space.h"
#include "window_mode/window_mode_json.h"

#include <fstream>
#include <sstream>

bool IsRecordingScriptPath(const std::wstring& path) {
    const std::wstring dir = RecordingsDir();
    if (path.size() < dir.size()) return false;
    if (_wcsnicmp(path.c_str(), dir.c_str(), dir.size()) != 0) return false;
    return path.size() == dir.size() || path[dir.size()] == L'\\';
}

ScriptAction ParseScriptActionBlock(const std::wstring& block, size_t fallbackNo,
    bool coordsNormalized) {
    ScriptAction a{};
    const auto type = ExtractString(block, L"type");
    if (type.empty()) return a;
    if (type == L"moveMouse") a.type = ActionType::MoveMouse;
    else if (type == L"moveMouseRelative") a.type = ActionType::MoveMouseRelative;
    else if (type == L"mouseDown") a.type = ActionType::MouseDown;
    else if (type == L"mouseUp") a.type = ActionType::MouseUp;
    else if (type == L"mouseClick") a.type = ActionType::MouseClick;
    else if (type == L"mousePlayback") a.type = ActionType::MousePlayback;
    else if (type == L"runMacro") a.type = ActionType::RunMacro;
    else if (type == L"keyDown") a.type = ActionType::KeyDown;
    else if (type == L"keyUp") a.type = ActionType::KeyUp;
    else if (type == L"keyClick") a.type = ActionType::KeyClick;
    else if (type == L"hotkeyShortcut") a.type = ActionType::HotkeyShortcut;
    else if (type == L"quickInput") a.type = ActionType::QuickInput;
    else if (type == L"scrollWheel") a.type = ActionType::ScrollWheel;
    else if (type == L"findImage") a.type = ActionType::FindImage;
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
    else if (type == L"timerRecordTime") a.type = ActionType::TimerRecordTime;
    else if (type == L"getCursorPos") a.type = ActionType::GetCursorPos;
    else if (type == L"aiTextAnalysis") a.type = ActionType::AiTextAnalysis;
    else if (type == L"aiImageAnalysis") a.type = ActionType::AiImageAnalysis;
    else if (type == L"aiActionExecute") a.type = ActionType::AiActionExecute;
    else a.type = ActionType::CustomText;
    a.customText = ExtractString(block, L"text");
    a.remark = ExtractString(block, L"remark");
    a.originalNo = static_cast<int>(ExtractNumber(block, L"no", static_cast<double>(fallbackNo + 1)));
    a.indent = static_cast<int>(ExtractNumber(block, L"indent", 0));

    if (a.type == ActionType::MoveMouseRelative) {
        // 相对位移始终为整型像素 dx/dy，不参与屏幕归一化
        a.coordsAreNormalized = false;
        a.x = static_cast<int>(ExtractNumber(block, L"x", 0));
        a.y = static_cast<int>(ExtractNumber(block, L"y", 0));
        a.randomX = static_cast<int>(ExtractNumber(block, L"randomX", 0));
        a.randomY = static_cast<int>(ExtractNumber(block, L"randomY", 0));
        a.searchX1 = static_cast<int>(ExtractNumber(block, L"searchX1", 0));
        a.searchY1 = static_cast<int>(ExtractNumber(block, L"searchY1", 0));
        a.searchX2 = static_cast<int>(ExtractNumber(block, L"searchX2", 0));
        a.searchY2 = static_cast<int>(ExtractNumber(block, L"searchY2", 0));
        a.offsetX = static_cast<int>(ExtractNumber(block, L"offsetX", 0));
        a.offsetY = static_cast<int>(ExtractNumber(block, L"offsetY", 0));
        a.aiSearchX1 = static_cast<int>(ExtractNumber(block, L"aiSearchX1", 0));
        a.aiSearchY1 = static_cast<int>(ExtractNumber(block, L"aiSearchY1", 0));
        a.aiSearchX2 = static_cast<int>(ExtractNumber(block, L"aiSearchX2", 0));
        a.aiSearchY2 = static_cast<int>(ExtractNumber(block, L"aiSearchY2", 0));
    } else if (coordsNormalized) {
        // 从 JSON 读取归一化坐标（0.0–1.0 浮点数）
        a.coordsAreNormalized = true;
        a.nx = ExtractNumber(block, L"x", 0.0);
        a.ny = ExtractNumber(block, L"y", 0.0);
        a.nRandomX = ExtractNumber(block, L"randomX", 0.0);
        a.nRandomY = ExtractNumber(block, L"randomY", 0.0);
        a.nSearchX1 = ExtractNumber(block, L"searchX1", 0.0);
        a.nSearchY1 = ExtractNumber(block, L"searchY1", 0.0);
        a.nSearchX2 = ExtractNumber(block, L"searchX2", 0.0);
        a.nSearchY2 = ExtractNumber(block, L"searchY2", 0.0);
        a.nOffsetX = ExtractNumber(block, L"offsetX", 0.0);
        a.nOffsetY = ExtractNumber(block, L"offsetY", 0.0);
        a.nAiSearchX1 = ExtractNumber(block, L"aiSearchX1", 0.0);
        a.nAiSearchY1 = ExtractNumber(block, L"aiSearchY1", 0.0);
        a.nAiSearchX2 = ExtractNumber(block, L"aiSearchX2", 0.0);
        a.nAiSearchY2 = ExtractNumber(block, L"aiSearchY2", 0.0);
    } else {
        a.x = static_cast<int>(ExtractNumber(block, L"x", 0));
        a.y = static_cast<int>(ExtractNumber(block, L"y", 0));
        a.randomX = static_cast<int>(ExtractNumber(block, L"randomX", 0));
        a.randomY = static_cast<int>(ExtractNumber(block, L"randomY", 0));
        a.searchX1 = static_cast<int>(ExtractNumber(block, L"searchX1", 0));
        a.searchY1 = static_cast<int>(ExtractNumber(block, L"searchY1", 0));
        a.searchX2 = static_cast<int>(ExtractNumber(block, L"searchX2", 0));
        a.searchY2 = static_cast<int>(ExtractNumber(block, L"searchY2", 0));
        a.offsetX = static_cast<int>(ExtractNumber(block, L"offsetX", 0));
        a.offsetY = static_cast<int>(ExtractNumber(block, L"offsetY", 0));
        a.aiSearchX1 = static_cast<int>(ExtractNumber(block, L"aiSearchX1", 0));
        a.aiSearchY1 = static_cast<int>(ExtractNumber(block, L"aiSearchY1", 0));
        a.aiSearchX2 = static_cast<int>(ExtractNumber(block, L"aiSearchX2", 0));
        a.aiSearchY2 = static_cast<int>(ExtractNumber(block, L"aiSearchY2", 0));
    }
    a.moveFromVar = ExtractNumber(block, L"moveFromVar", 0) != 0;
    a.moveVarExprX = ExtractString(block, L"moveVarExprX");
    a.moveVarExprY = ExtractString(block, L"moveVarExprY");
    const auto button = ExtractString(block, L"button");
    a.button = button == L"right" ? MouseButtonType::Right
        : button == L"middle" ? MouseButtonType::Middle
        : button == L"x1" ? MouseButtonType::X1
        : button == L"x2" ? MouseButtonType::X2
        : MouseButtonType::Left;
    a.keyText = ExtractString(block, L"keyText");
    if (a.keyText.empty()) a.keyText = L"7";
    a.keyVk = static_cast<UINT>(ExtractNumber(block, L"keyVk",
        a.keyText.size() == 1 ? towupper(a.keyText[0]) : '7'));
    a.holdLeftWin = ExtractNumber(block, L"holdLeftWin", 0) != 0;
    a.holdRightWin = ExtractNumber(block, L"holdRightWin", 0) != 0;
    a.holdLeftCtrl = ExtractNumber(block, L"holdLeftCtrl", 0) != 0;
    a.holdRightCtrl = ExtractNumber(block, L"holdRightCtrl", 0) != 0;
    a.holdLeftAlt = ExtractNumber(block, L"holdLeftAlt", 0) != 0;
    a.holdRightAlt = ExtractNumber(block, L"holdRightAlt", 0) != 0;
    a.holdLeftShift = ExtractNumber(block, L"holdLeftShift", 0) != 0;
    a.holdRightShift = ExtractNumber(block, L"holdRightShift", 0) != 0;
    a.clickCount = static_cast<int>(ExtractNumber(block, L"clickCount", 1));
    a.duration = ExtractNumber(block, L"duration", 0.1);
    a.randomDuration = ExtractNumber(block, L"randomDuration", 0.0);
    a.timingUs = static_cast<uint64_t>(std::max(0.0,
        ExtractNumber(block, L"timingUs", 0.0)));
    a.loopCount = static_cast<int>(ExtractNumber(block, L"loopCount", -1));
    a.loopVarName = ExtractString(block, L"loopVarName");
    a.loopFromVar = ExtractNumber(block, L"loopFromVar", 0) != 0;
    a.loopVarExpr = ExtractString(block, L"loopVarExpr");
    a.blockName = ExtractString(block, L"blockName");
    a.targetPath = ExtractString(block, L"targetPath");
    a.shortcutPreset = static_cast<int>(ExtractNumber(block, L"shortcutPreset", 0));
    a.inputText = ExtractString(block, L"inputText");
    a.charInterval = ExtractNumber(block, L"charInterval", 0.01);
    a.scrollVertical = ExtractNumber(block, L"scrollVertical", 1) != 0;
    a.scrollHorizontal = ExtractNumber(block, L"scrollHorizontal", 0) != 0;
    a.scrollSteps = static_cast<int>(ExtractNumber(block, L"scrollSteps", 1));
    a.scrollDirection = static_cast<int>(ExtractNumber(block, L"scrollDirection", 0));
    if (!coordsNormalized) {
        a.searchX1 = static_cast<int>(ExtractNumber(block, L"searchX1", 0));
        a.searchY1 = static_cast<int>(ExtractNumber(block, L"searchY1", 0));
        a.searchX2 = static_cast<int>(ExtractNumber(block, L"searchX2", 0));
        a.searchY2 = static_cast<int>(ExtractNumber(block, L"searchY2", 0));
    }
    a.searchFullScreen = ExtractNumber(block, L"searchFullScreen", 1) != 0;
    a.imagePath = ExtractString(block, L"imagePath");
    if (!a.imagePath.empty()) a.imagePath = ResolveImagePath(a.imagePath);
    a.matchThreshold = ExtractNumber(block, L"matchThreshold", 65.0);
    a.imageScale = ExtractNumber(block, L"imageScale", 1.0);
    a.imageScaleMin = ExtractNumber(block, L"imageScaleMin", a.imageScale);
    a.imageScaleMax = ExtractNumber(block, L"imageScaleMax", a.imageScale);
    a.findImageFollowUp = static_cast<int>(ExtractNumber(block, L"findImageFollowUp", 0));
    if (!coordsNormalized) {
        a.offsetX = static_cast<int>(ExtractNumber(block, L"offsetX", 0));
        a.offsetY = static_cast<int>(ExtractNumber(block, L"offsetY", 0));
    }
    a.findUntilFound = ExtractNumber(block, L"findUntilFound", 0) != 0;
    a.findTimeExpr = ExtractString(block, L"findTimeExpr");
    if (a.findTimeExpr.empty()) a.findTimeExpr = L"0";
    a.matchVarName = ExtractString(block, L"matchVarName");
    if (a.matchVarName.empty()) {
        a.matchVarName = a.type == ActionType::TextRecognition ? L"a" : L"matchRet";
    }
    a.ocrResultMode = static_cast<int>(ExtractNumber(block, L"ocrResultMode", 0));
    a.ocrRegionByImage = ExtractNumber(block, L"ocrRegionByImage", 0) != 0;
    a.ocrDigitsOnly = ExtractNumber(block, L"ocrDigitsOnly", 0) != 0;
    a.ocrSearchText = ExtractString(block, L"ocrSearchText");
    a.ocrFollowUp = static_cast<int>(ExtractNumber(block, L"ocrFollowUp", 0));
    a.conditionExpr = ExtractString(block, L"conditionExpr");
    a.gotoStepExpr = ExtractString(block, L"gotoStepExpr");
    a.matchFileNameOnly = ExtractNumber(block, L"matchFileNameOnly", 0) != 0;
    // ── AI 动作字段 ──
    a.aiPrompt = ExtractString(block, L"aiPrompt");
    a.aiOutputVarName = ExtractString(block, L"aiOutputVarName");
    a.aiOutputType = static_cast<int>(ExtractNumber(block, L"aiOutputType", 0));
    a.aiModelName = ExtractString(block, L"aiModelName");
    a.aiContextMode = static_cast<int>(ExtractNumber(block, L"aiContextMode", 1));
    a.aiTimeoutSec = static_cast<int>(ExtractNumber(block, L"aiTimeoutSec", 30));
    a.aiImageScale = ExtractNumber(block, L"aiImageScale", 0.5);
    a.aiRegionByImage = ExtractNumber(block, L"aiRegionByImage", 0) != 0;
    a.aiTargetImagePath = ExtractString(block, L"aiTargetImagePath");
    if (!a.aiTargetImagePath.empty()) a.aiTargetImagePath = ResolveImagePath(a.aiTargetImagePath);
    a.aiSearchRegion = static_cast<int>(ExtractNumber(block, L"aiSearchRegion", 0));
    if (!coordsNormalized) {
        a.aiSearchX1 = static_cast<int>(ExtractNumber(block, L"aiSearchX1", 0));
        a.aiSearchY1 = static_cast<int>(ExtractNumber(block, L"aiSearchY1", 0));
        a.aiSearchX2 = static_cast<int>(ExtractNumber(block, L"aiSearchX2", 0));
        a.aiSearchY2 = static_cast<int>(ExtractNumber(block, L"aiSearchY2", 0));
    }
    a.aiMaxSteps = static_cast<int>(ExtractNumber(block, L"aiMaxSteps", 10));
    a.aiWithImage = ExtractNumber(block, L"aiWithImage", 1) != 0;
    a.aiFallbackValue = ExtractString(block, L"aiFallbackValue");
    a.aiConfirmExecute = ExtractNumber(block, L"aiConfirmExecute", 0) != 0;
    return a;
}

void WriteActionJson(std::wstringstream& file, const ScriptAction& a, bool last) {
    file << L"    {\n";
    file << L"      \"type\": \"" << JsonType(a.type) << L"\",\n";
    file << L"      \"text\": \"" << EscapeJson(a.customText) << L"\",\n";
    file << L"      \"remark\": \"" << EscapeJson(a.remark) << L"\",\n";
    file << L"      \"no\": " << a.originalNo << L",\n";
    file << L"      \"indent\": " << a.indent << L",\n";
    if (a.type == ActionType::MoveMouseRelative || !a.coordsAreNormalized) {
        file << L"      \"x\": " << a.x << L",\n";
        file << L"      \"y\": " << a.y << L",\n";
        file << L"      \"randomX\": " << a.randomX << L",\n";
        file << L"      \"randomY\": " << a.randomY << L",\n";
    } else {
        // 写入归一化坐标（0.0–1.0 浮点数）
        file << L"      \"x\": " << a.nx << L",\n";
        file << L"      \"y\": " << a.ny << L",\n";
        file << L"      \"randomX\": " << a.nRandomX << L",\n";
        file << L"      \"randomY\": " << a.nRandomY << L",\n";
    }
    file << L"      \"moveFromVar\": " << (a.moveFromVar ? 1 : 0) << L",\n";
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
    file << L"      \"shortcutPreset\": " << a.shortcutPreset << L",\n";
    file << L"      \"inputText\": \"" << EscapeJson(a.inputText) << L"\",\n";
    file << L"      \"charInterval\": " << a.charInterval << L",\n";
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
    const std::wstring savedImagePath = [&]() -> std::wstring {
        if (a.type == ActionType::FindImage && !a.imagePath.empty()) {
            return ImagePathForJson(EnsureImageInLibrary(a.imagePath));
        }
        if (a.type == ActionType::TextRecognition && a.ocrRegionByImage && !a.imagePath.empty()) {
            return ImagePathForJson(EnsureImageInLibrary(a.imagePath));
        }
        return a.imagePath;
    }();
    file << L"      \"imagePath\": \"" << EscapeJson(savedImagePath) << L"\",\n";
    file << L"      \"matchThreshold\": " << a.matchThreshold << L",\n";
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
    file << L"      \"ocrResultMode\": " << a.ocrResultMode << L",\n";
    file << L"      \"ocrRegionByImage\": " << (a.ocrRegionByImage ? 1 : 0) << L",\n";
    file << L"      \"ocrDigitsOnly\": " << (a.ocrDigitsOnly ? 1 : 0) << L",\n";
    file << L"      \"ocrSearchText\": \"" << EscapeJson(a.ocrSearchText) << L"\",\n";
    file << L"      \"ocrFollowUp\": " << a.ocrFollowUp << L",\n";
    file << L"      \"conditionExpr\": \"" << EscapeJson(a.conditionExpr) << L"\",\n";
    file << L"      \"gotoStepExpr\": \"" << EscapeJson(a.gotoStepExpr) << L"\",\n";
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
    const std::wstring savedAiImagePath = [&]() -> std::wstring {
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
    file << L"      \"aiFallbackValue\": \"" << EscapeJson(a.aiFallbackValue) << L"\",\n";
    file << L"      \"aiConfirmExecute\": " << (a.aiConfirmExecute ? 1 : 0) << L"\n";
    file << L"    }" << (last ? L"\n" : L",\n");
}

std::wstring ScriptActionToJsonString(const ScriptAction& a) {
    std::wstringstream file;
    WriteActionJson(file, a, true);
    return file.str();
}

namespace {

bool ScriptNormValuesLookLikePixels(const std::vector<ScriptAction>& actions) {
    for (const auto& a : actions) {
        if (!a.coordsAreNormalized) continue;
        if (a.nx > 1.5 || a.ny > 1.5 || a.nSearchX2 > 1.5 || a.nSearchY2 > 1.5
            || a.nAiSearchX2 > 1.5 || a.nAiSearchY2 > 1.5) {
            return true;
        }
    }
    return false;
}

}  // namespace

ScriptFileData LoadScriptFileData(const std::wstring& path, bool denormForDisplay) {
    ScriptFileData data{};
    if (path.empty()) return data;
    const auto content = ReadAll(path);
    data.scriptName = ExtractString(content, L"scriptName");
    data.recordTime = ExtractString(content, L"recordTime");
    data.durationSeconds = ExtractNumber(content, L"durationSeconds", 0);
    data.recordingCaptureMode = static_cast<int>(
        ExtractNumber(content, L"recordingCaptureMode", -1));
    data.inputTimingVersion = std::max(0, static_cast<int>(
        ExtractNumber(content, L"inputTimingVersion", 0)));
    data.hotkey.text = ExtractString(content, L"hotkeyText");
    data.hotkey.vk = static_cast<UINT>(ExtractNumber(content, L"hotkeyVk", 0));
    data.hotkey.modifiers = static_cast<UINT>(ExtractNumber(content, L"hotkeyModifiers", 0));
    data.hotkey.holdMode = ExtractBool(content, L"hotkeyHold", false);
    data.hotkey.enabled = data.hotkey.vk != 0;
    data.breakoutTimeSeconds = NormalizeBreakoutTimeSeconds(
        ExtractNumber(content, L"breakoutTimeSeconds", 0));
    data.windowMode = windowmode::ParseWindowModeJson(content);

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
        const auto type = ExtractString(blocks[i], L"type");
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
            const auto type = ExtractString(blocks[i], L"type");
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

    return data;
}

ScriptFileData ParseScriptContent(const std::wstring& content) {
    ScriptFileData data{};
    if (content.empty()) return data;
    data.scriptName = ExtractString(content, L"scriptName");
    data.recordTime = ExtractString(content, L"recordTime");
    data.durationSeconds = ExtractNumber(content, L"durationSeconds", 0);
    data.recordingCaptureMode = static_cast<int>(
        ExtractNumber(content, L"recordingCaptureMode", -1));
    data.inputTimingVersion = std::max(0, static_cast<int>(
        ExtractNumber(content, L"inputTimingVersion", 0)));
    data.hotkey.text = ExtractString(content, L"hotkeyText");
    data.hotkey.vk = static_cast<UINT>(ExtractNumber(content, L"hotkeyVk", 0));
    data.hotkey.modifiers = static_cast<UINT>(ExtractNumber(content, L"hotkeyModifiers", 0));
    data.hotkey.holdMode = ExtractBool(content, L"hotkeyHold", false);
    data.hotkey.enabled = data.hotkey.vk != 0;
    data.breakoutTimeSeconds = NormalizeBreakoutTimeSeconds(
        ExtractNumber(content, L"breakoutTimeSeconds", 0));
    data.windowMode = windowmode::ParseWindowModeJson(content);

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
        const auto type = ExtractString(blocks[i], L"type");
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

    return data;
}

bool SaveScriptFileData(const std::wstring& path, const ScriptFileData& data) {
    ScriptFileData normalized = data;
    normalized.breakoutTimeSeconds = NormalizeBreakoutTimeSeconds(normalized.breakoutTimeSeconds);
    if (IsRecordingScriptPath(path)) {
        normalized.windowMode = windowmode::DefaultWindowModeConfig();
        normalized.breakoutTimeSeconds = 0;
    } else if (normalized.windowMode.enabled) {
        normalized.breakoutTimeSeconds = 0;
    }

    // 像素→n* 用当前屏幕；JSON coordMeta 固定为标准 2560×1440
    CoordMeta pixelMeta = CaptureCurrentCoordMeta(
        normalized.windowMode.enabled ? &normalized.windowMode : nullptr);
    const CoordMeta storeMeta = BuildScriptCoordMetaForSave(pixelMeta);

    std::vector<ScriptAction> saveActions = normalized.actions;
    SyncNormFieldsFromPixels(saveActions, pixelMeta);

    std::wstringstream file;
    std::ofstream out(path, std::ios::binary);
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
             << std::clamp(normalized.recordingCaptureMode, 0, 2) << L",\n";
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
    return ok;
}
