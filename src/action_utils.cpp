// ── 脚本动作辅助函数实现 ────────────────────────────────────
#include "action_utils.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace {
const RunProgramPreset kRunProgramPresets[] = {
    {L"选择文件", L"", L""},
    {L"快捷运行-记事本", L"notepad.exe", L"记事本"},
    {L"快捷运行-计算器", L"calc.exe", L"计算器"},
    {L"快捷运行-画图", L"mspaint.exe", L"画图"},
    {L"快捷运行-文件管理器", L"explorer.exe", L"文件管理器"},
    {L"快捷运行-命令行", L"cmd.exe", L"命令行"},
    {L"快捷运行-PowerShell", L"powershell.exe", L"PowerShell"},
    {L"快捷运行-进程管理器", L"taskmgr.exe", L"进程管理器"},
    {L"快捷运行-注册表编辑器", L"regedit.exe", L"注册表编辑器"},
    {L"快捷运行-服务", L"services.msc", L"服务"},
    {L"快捷运行-计算机管理", L"compmgmt.msc", L"计算机管理"},
    {L"快捷运行-控制面板", L"control.exe", L"控制面板"},
    {L"快捷运行-设置", L"ms-settings:", L"设置"},
};

std::wstring FileNameFromPath(const std::wstring& path) {
    const auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring StripExtension(std::wstring name) {
    const auto dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.erase(dot);
    return name;
}
} // namespace

int RunProgramPresetCount() {
    return static_cast<int>(sizeof(kRunProgramPresets) / sizeof(kRunProgramPresets[0]));
}

const RunProgramPreset& RunProgramPresetAt(int index) {
    return kRunProgramPresets[std::clamp(index, 0, RunProgramPresetCount() - 1)];
}

std::wstring ResolveRunProgramPath(int presetIndex, const std::wstring& customPath) {
    if (presetIndex <= 0) return Trim(customPath);
    return RunProgramPresetAt(presetIndex).path;
}

std::wstring RunProgramDisplayName(int presetIndex, const std::wstring& customPath) {
    if (presetIndex > 0) {
        const auto& preset = RunProgramPresetAt(presetIndex);
        if (preset.displayName && preset.displayName[0]) return preset.displayName;
    }
    const std::wstring path = Trim(customPath);
    if (path.empty()) return L"未选择";
    const std::wstring file = FileNameFromPath(path);
    if (file.empty()) return path;
    return StripExtension(file);
}

std::wstring ButtonText(MouseButtonType button) {
    if (button == MouseButtonType::Right) return L"右键";
    if (button == MouseButtonType::Middle) return L"中键";
    return L"左键";
}

std::wstring HoldText(const ScriptAction& action) {
    std::wstring text;
    auto add = [&text](bool enabled, const wchar_t* value) {
        if (!enabled) return;
        if (!text.empty()) text += L"+";
        text += value;
    };
    add(action.holdLeftWin || action.holdRightWin, L"Win");
    add(action.holdLeftCtrl || action.holdRightCtrl, L"Ctrl");
    add(action.holdLeftAlt || action.holdRightAlt, L"Alt");
    add(action.holdLeftShift || action.holdRightShift, L"Shift");
    return text;
}

std::wstring RepeatInfo(const ScriptAction& action) {
    return L"[重复" + std::to_wstring(action.clickCount)
        + L"次等待" + F3(action.duration)
        + L"+随机" + F3(action.randomDuration) + L"秒]";
}

std::wstring ActionName(const ScriptAction& action) {
    if (!action.customText.empty()) return action.customText;
    switch (action.type) {
    case ActionType::MoveMouse:
        if (action.moveFromVar) {
            return L"移动鼠标到(" + action.moveVarExprX + L"," + action.moveVarExprY + L")";
        }
        return L"移动鼠标到(" + std::to_wstring(action.x)
            + L"," + std::to_wstring(action.y) + L")";
    case ActionType::MouseDown: {
        const auto holds = HoldText(action);
        return L"鼠标按下"
            + (holds.empty() ? L"" : holds + L"+")
            + ButtonText(action.button);
    }
    case ActionType::MouseUp: {
        const auto holds = HoldText(action);
        return L"鼠标松开"
            + (holds.empty() ? L"" : holds + L"+")
            + ButtonText(action.button);
    }
    case ActionType::MouseClick: {
        const auto holds = HoldText(action);
        return L"鼠标点击"
            + (holds.empty() ? L"" : holds + L"+")
            + ButtonText(action.button) + RepeatInfo(action);
    }
    case ActionType::KeyDown: {
        const auto holds = HoldText(action);
        return L"键盘按下"
            + (holds.empty() ? L"" : holds + L"+")
            + action.keyText;
    }
    case ActionType::KeyUp: {
        const auto holds = HoldText(action);
        return L"键盘松开"
            + (holds.empty() ? L"" : holds + L"+")
            + action.keyText;
    }
    case ActionType::KeyClick: {
        const auto holds = HoldText(action);
        return L"按键点击"
            + (holds.empty() ? L"" : holds + L"+")
            + action.keyText + RepeatInfo(action);
    }
    case ActionType::Wait:
        return L"等待 " + F3(action.duration)
            + L"秒+随机0~" + F3(action.randomDuration) + L"秒";
    case ActionType::Loop:
        if (action.loopFromVar && !action.loopVarExpr.empty()) {
            return L"循环[" + action.loopVarExpr + L"]";
        }
        return action.loopCount < 0
            ? L"循环[无限循环]"
            : L"循环[" + std::to_wstring(action.loopCount) + L"次]";
    case ActionType::EndLoop:
        return L"跳出循环";
    case ActionType::DefineBlock:
        return L"定义宏指令块["
            + (action.blockName.empty() ? L"未命名" : action.blockName) + L"]";
    case ActionType::RunBlock:
        return L"运行宏指令块["
            + (action.blockName.empty() ? L"未命名" : action.blockName) + L"]";
    case ActionType::RunMacro:
        return L"运行鼠标宏["
            + (action.blockName.empty() ? L"未选择" : action.blockName) + L"]";
    case ActionType::MousePlayback:
        return L"鼠标回放["
            + (action.blockName.empty() ? L"未选择" : action.blockName) + L"]" + RepeatInfo(action);
    case ActionType::HotkeyShortcut: {
        const int idx = std::clamp(action.shortcutPreset, 0, ShortcutPresetCount() - 1);
        return L"快捷按键[" + std::wstring(ShortcutPresetAt(idx).label) + L"]" + RepeatInfo(action);
    }
    case ActionType::QuickInput: {
        std::wstring preview = action.inputText;
        if (preview.size() > 24) preview = preview.substr(0, 24) + L"...";
        return L"快捷输入[" + preview + L"]" + RepeatInfo(action);
    }
    case ActionType::ScrollWheel: {
        std::wstring type;
        if (action.scrollVertical) type += L"垂直";
        if (action.scrollHorizontal) {
            if (!type.empty()) type += L"+";
            type += L"水平";
        }
        if (type.empty()) type = L"垂直";
        const wchar_t* dir = action.scrollDirection == 0 ? L"向上/左" : L"向下/右";
        return L"滚动滚轮[" + type + L"," + std::wstring(dir) + L"," + std::to_wstring(action.scrollSteps)
            + L"步]" + RepeatInfo(action);
    }
    case ActionType::FindImage: {
        const wchar_t* follow = action.findImageFollowUp == 1 ? L"移动到"
            : action.findImageFollowUp == 2 ? L"保存变量" : L"点击";
        std::wstring scaleText = F3(action.imageScaleMin) + L"-" + F3(action.imageScaleMax);
        return std::wstring(L"找图(返回最匹配的)[") + follow + L",匹配>"
            + std::to_wstring(static_cast<int>(action.matchThreshold)) + L"%,缩放"
            + scaleText + L"]";
    }
    case ActionType::TextRecognition: {
        const wchar_t* mode = action.ocrResultMode == 1 ? L"文字查找" : L"获取文字";
        const wchar_t* follow = action.ocrFollowUp == 1 ? L"移动到"
            : action.ocrFollowUp == 2 ? L"保存变量" : L"点击";
        return std::wstring(L"文字识别[") + mode + L"," + follow + L"]";
    }
    case ActionType::If: {
        std::wstring cond = action.conditionExpr;
        for (wchar_t& ch : cond) {
            if (ch == L'\r' || ch == L'\n') ch = L' ';
        }
        if (cond.size() > 36) cond = cond.substr(0, 36) + L"...";
        return L"如果[条件:" + cond + L"]";
    }
    case ActionType::Else:
        return L"否则";
    case ActionType::LockScreenshot:
        return L"锁定截屏";
    case ActionType::UnlockScreenshot:
        return L"解锁截屏";
    case ActionType::StopMacro:
        return L"结束宏运行";
    case ActionType::RunProgram:
        return L"打开程序[" + RunProgramDisplayName(action.shortcutPreset, action.targetPath) + L"]";
    case ActionType::CloseProgram: {
        std::wstring target = action.targetPath;
        if (target.size() > 36) target = target.substr(0, 36) + L"...";
        return L"关闭程序[" + (target.empty() ? L"未选择" : target) + L"]";
    }
    case ActionType::OpenWebpage: {
        std::wstring url = action.targetPath;
        if (url.size() > 36) url = url.substr(0, 36) + L"...";
        return L"打开网页[" + (url.empty() ? L"未填写" : url) + L"]";
    }
    case ActionType::OpenFile: {
        std::wstring path = action.targetPath;
        if (path.size() > 36) path = path.substr(0, 36) + L"...";
        return L"打开文件[" + (path.empty() ? L"未选择" : path) + L"]";
    }
    case ActionType::TimerRecordTime:
        return L"计时器记录时间["
            + (action.loopVarName.empty() ? L"未命名" : action.loopVarName) + L"]";
    case ActionType::CustomText:
        return action.customText;
    }
    return L"未知动作";
}

bool ScriptUsesTextRecognition(const std::vector<ScriptAction>& actions) {
    for (const auto& a : actions) {
        if (a.type == ActionType::TextRecognition) return true;
    }
    return false;
}

std::wstring JsonType(ActionType type) {
    switch (type) {
    case ActionType::MoveMouse:   return L"moveMouse";
    case ActionType::MouseDown:   return L"mouseDown";
    case ActionType::MouseUp:     return L"mouseUp";
    case ActionType::MouseClick:  return L"mouseClick";
    case ActionType::KeyDown:     return L"keyDown";
    case ActionType::KeyUp:       return L"keyUp";
    case ActionType::KeyClick:    return L"keyClick";
    case ActionType::Wait:        return L"wait";
    case ActionType::Loop:        return L"loop";
    case ActionType::EndLoop:     return L"endLoop";
    case ActionType::DefineBlock: return L"defineBlock";
    case ActionType::RunBlock:       return L"runBlock";
    case ActionType::RunMacro:       return L"runMacro";
    case ActionType::MousePlayback:  return L"mousePlayback";
    case ActionType::HotkeyShortcut: return L"hotkeyShortcut";
    case ActionType::QuickInput:     return L"quickInput";
    case ActionType::ScrollWheel:    return L"scrollWheel";
    case ActionType::FindImage:      return L"findImage";
    case ActionType::TextRecognition: return L"textRecognition";
    case ActionType::If:             return L"if";
    case ActionType::Else:           return L"else";
    case ActionType::LockScreenshot: return L"lockScreenshot";
    case ActionType::UnlockScreenshot: return L"unlockScreenshot";
    case ActionType::StopMacro:      return L"stopMacro";
    case ActionType::RunProgram:     return L"runProgram";
    case ActionType::CloseProgram:   return L"closeProgram";
    case ActionType::OpenWebpage:    return L"openWebpage";
    case ActionType::OpenFile:       return L"openFile";
    case ActionType::TimerRecordTime: return L"timerRecordTime";
    case ActionType::CustomText:     return L"customText";
    }
    return L"customText";
}

std::wstring JsonButton(MouseButtonType button) {
    if (button == MouseButtonType::Right) return L"right";
    if (button == MouseButtonType::Middle) return L"middle";
    return L"left";
}

bool IsValidBlockName(const std::wstring& name) {
    if (name.empty()) return false;
    if (!iswalpha(name[0])) return false;
    for (wchar_t ch : name) {
        if (!iswalnum(ch)) return false;
    }
    return true;
}

// ── 键盘/鼠标模拟函数 ─────────────────────────────────────────────

void SendKeyboardKey(UINT vk, bool down) {
    if (vk == 0) return;
    UINT scanEx = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    if (scanEx == 0) scanEx = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (scanEx == 0) return;
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = static_cast<WORD>(scanEx & 0xFF);
    input.ki.dwFlags = KEYEVENTF_SCANCODE;
    const UINT high = scanEx & 0xFF00;
    if (high == 0xE000 || high == 0xE100)
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!down) input.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseButtonEvent(MouseButtonType button, bool down) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    if (button == MouseButtonType::Right)
        input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    else if (button == MouseButtonType::Middle)
        input.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    else
        input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

void MouseClick(MouseButtonType button) {
    MouseButtonEvent(button, true);
    MouseButtonEvent(button, false);
}

namespace {
const ShortcutPreset kShortcutPresets[] = {
    {L"Ctrl+C(拷贝)", 'C', true, false, false, false},
    {L"Ctrl+V(粘贴)", 'V', true, false, false, false},
    {L"Ctrl+X(剪切)", 'X', true, false, false, false},
    {L"Ctrl+S(保存)", 'S', true, false, false, false},
    {L"Ctrl+F(查找)", 'F', true, false, false, false},
    {L"Alt+F4(关闭窗口)", VK_F4, false, true, false, false},
    {L"Win+D(所有最小化)", 'D', false, false, false, true},
    {L"Win+R(打开运行)", 'R', false, false, false, true},
    {L"Ctrl+Alt+Delete", VK_DELETE, true, true, false, false},
};
} // namespace

int ShortcutPresetCount() {
    return static_cast<int>(sizeof(kShortcutPresets) / sizeof(kShortcutPresets[0]));
}

const ShortcutPreset& ShortcutPresetAt(int index) {
    return kShortcutPresets[std::clamp(index, 0, ShortcutPresetCount() - 1)];
}

void ApplyShortcutPreset(ScriptAction& action, int presetIndex) {
    const auto& preset = ShortcutPresetAt(presetIndex);
    action.shortcutPreset = std::clamp(presetIndex, 0, ShortcutPresetCount() - 1);
    action.keyVk = preset.vk;
    action.holdLeftCtrl = preset.ctrl;
    action.holdRightCtrl = false;
    action.holdLeftAlt = preset.alt;
    action.holdRightAlt = false;
    action.holdLeftShift = preset.shift;
    action.holdRightShift = false;
    action.holdLeftWin = preset.win;
    action.holdRightWin = false;
}

void SendShortcutCombo(const ScriptAction& action) {
    ScriptAction tmp = action;
    ApplyShortcutPreset(tmp, action.shortcutPreset);
    if (tmp.holdLeftWin) SendKeyboardKey(VK_LWIN, true);
    if (tmp.holdLeftCtrl) SendKeyboardKey(VK_LCONTROL, true);
    if (tmp.holdLeftAlt) SendKeyboardKey(VK_LMENU, true);
    if (tmp.holdLeftShift) SendKeyboardKey(VK_LSHIFT, true);
    SendKeyboardKey(tmp.keyVk, true);
    SendKeyboardKey(tmp.keyVk, false);
    if (tmp.holdLeftShift) SendKeyboardKey(VK_LSHIFT, false);
    if (tmp.holdLeftAlt) SendKeyboardKey(VK_LMENU, false);
    if (tmp.holdLeftCtrl) SendKeyboardKey(VK_LCONTROL, false);
    if (tmp.holdLeftWin) SendKeyboardKey(VK_LWIN, false);
}

void SendUnicodeChar(wchar_t ch) {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wScan = ch;
    inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;
    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

void SendQuickInputText(const std::wstring& text, double charInterval) {
    auto delay = [charInterval]() {
        if (charInterval <= 0) return;
        const auto end = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(static_cast<int>(charInterval * 1000.0));
        while (std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };

    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            SendKeyboardKey(VK_RETURN, true);
            SendKeyboardKey(VK_RETURN, false);
        } else if (ch == L'\n') {
            SendKeyboardKey(VK_RETURN, true);
            SendKeyboardKey(VK_RETURN, false);
        } else {
            SendUnicodeChar(ch);
        }
        delay();
    }
}
