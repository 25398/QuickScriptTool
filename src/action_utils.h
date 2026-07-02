#pragma once
// ──────────────────────────────────────────────────────────────────
// action_utils.h — 脚本动作相关的辅助函数（声明）
// 提供动作类型名称生成、JSON序列化辅助、键盘模拟等功能
// 依赖 script_types.h 中的 ActionType/MouseButtonType/ScriptAction
// ──────────────────────────────────────────────────────────────────

#include "script_types.h"
#include "utils.h"

#include <windows.h>

#include <string>

/// 获取鼠标按键类型的中文名称
std::wstring ButtonText(MouseButtonType button);

/// 生成修饰键组合文本（如 "Ctrl+Shift"）
std::wstring HoldText(const ScriptAction& action);

/// 生成重复动作描述文本
std::wstring RepeatInfo(const ScriptAction& action);

/// 根据动作类型和参数生成可读的动作描述名称
std::wstring ActionName(const ScriptAction& action);

/// 将动作类型转换为 JSON 类型标识字符串
std::wstring JsonType(ActionType type);

/// 将鼠标按键类型转换为 JSON 标识字符串
std::wstring JsonButton(MouseButtonType button);

/// 验证宏指令块名称是否合法（字母开头，仅包含字母数字）
bool IsValidBlockName(const std::wstring& name);

// ── 键盘/鼠标模拟函数 ─────────────────────────────────────────────

/// 通过扫描码发送键盘按键事件（比虚拟键码更可靠）
void SendKeyboardKey(UINT vk, bool down);

/// 模拟鼠标按下事件（根据按键类型）
void MouseButtonEvent(MouseButtonType button, bool down);

/// 模拟鼠标完整点击（按下 + 释放）
void MouseClick(MouseButtonType button);

struct ShortcutPreset {
    const wchar_t* label;
    UINT vk;
    bool ctrl;
    bool alt;
    bool shift;
    bool win;
};

int ShortcutPresetCount();
const ShortcutPreset& ShortcutPresetAt(int index);
void ApplyShortcutPreset(ScriptAction& action, int presetIndex);
void SendShortcutCombo(const ScriptAction& action);
void SendUnicodeChar(wchar_t ch);
void SendQuickInputText(const std::wstring& text, double charInterval);
