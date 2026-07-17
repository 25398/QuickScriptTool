#pragma once
// ──────────────────────────────────────────────────────────────────
// action_utils.h — 脚本动作相关的辅助函数（声明）
// 提供动作类型名称生成、JSON序列化辅助、键盘模拟等功能
// 依赖 script_types.h 中的 ActionType/MouseButtonType/ScriptAction
// ──────────────────────────────────────────────────────────────────

#include "script_types.h"
#include "utils.h"

#include <atomic>
#include <string>
#include <vector>

#include <windows.h>

/// 获取鼠标按键类型的中文名称
std::wstring ButtonText(MouseButtonType button);

/// 生成修饰键组合文本（如 "Ctrl+Shift"）
std::wstring HoldText(const ScriptAction& action);

/// 生成重复动作描述文本
std::wstring RepeatInfo(const ScriptAction& action);

/// 根据动作序号（originalNo）查找动作在列表中的索引；未找到返回 actions.size()
size_t FindActionIndexByNo(const std::vector<ScriptAction>& actions, int targetNo);

/// 根据动作类型和参数生成可读的动作描述名称
std::wstring ActionName(const ScriptAction& action);

/// 编辑器动作类型的简短中文名（无参数，对用户说明时用）
std::wstring ActionTypeBriefLabel(ActionType type);

/// JSON type 字段 → 编辑器中文动作名
std::wstring JsonTypeBriefLabel(const std::wstring& jsonType);

/// 脚本动作列表的可读大纲（供 AI 回复用户时引用）
std::wstring FormatScriptActionsOutline(const std::vector<ScriptAction>& actions);

/// 动作 type 英文标识 → 中文说法对照表（注入 AI 提示）
std::wstring ActionTypeReplyCatalog();

/// 将动作类型转换为 JSON 类型标识字符串
std::wstring JsonType(ActionType type);

/// 将鼠标按键类型转换为 JSON 标识字符串
std::wstring JsonButton(MouseButtonType button);

/// 验证宏指令块名称是否合法（字母开头，仅包含字母数字）
bool IsValidBlockName(const std::wstring& name);

/// 脚本是否包含文字识别动作（用于运行期按需启用 OCR 变量上下文）
bool ScriptUsesTextRecognition(const std::vector<ScriptAction>& actions);

/// 脚本是否仅包含键鼠时序动作（可用于绝对时间轴精密回放）
bool ScriptIsTimedInputSequence(const std::vector<ScriptAction>& actions);

/// duration/randomDuration 表示「相邻两次 clickCount 重复之间」的间隔（非 Wait，也非执行前延迟）
bool ActionUsesInterRepeatInterval(ActionType type);

/// 完成第 repeatIndex 次（0-based）之后是否还应按 duration 等待
bool ShouldWaitAfterRepeat(const ScriptAction& action, int repeatIndex);

// ── 键盘/鼠标模拟函数 ─────────────────────────────────────────────

/// 通过扫描码发送键盘按键事件（比虚拟键码更可靠）
void SendKeyboardKey(UINT vk, bool down);

/// 模拟鼠标按下事件（根据按键类型）
void MouseButtonEvent(MouseButtonType button, bool down);

/// 模拟鼠标完整点击（按下 + 释放）
void MouseClick(MouseButtonType button);

/// 相对移动鼠标（dx/dy 像素，SendInput MOUSEEVENTF_MOVE；FPS 视角等）
void SendMouseMoveRelative(int dx, int dy);

/// 精密回放时临时关闭鼠标加速并设中性速度（录制为 Raw，回放 SendInput 会再套加速）
class MouseBallisticsGuard {
public:
    explicit MouseBallisticsGuard(bool enable);
    ~MouseBallisticsGuard();
    MouseBallisticsGuard(const MouseBallisticsGuard&) = delete;
    MouseBallisticsGuard& operator=(const MouseBallisticsGuard&) = delete;
private:
    bool active_ = false;
    int mouseParams_[3]{};
    int mouseSpeed_ = 10;
};

/// 精密回放时提升进程优先级，降低调度抖动
class PlaybackProcessPriorityGuard {
public:
    explicit PlaybackProcessPriorityGuard(bool enable);
    ~PlaybackProcessPriorityGuard();
    PlaybackProcessPriorityGuard(const PlaybackProcessPriorityGuard&) = delete;
    PlaybackProcessPriorityGuard& operator=(const PlaybackProcessPriorityGuard&) = delete;
private:
    bool active_ = false;
    DWORD prevClass_ = NORMAL_PRIORITY_CLASS;
};

/// 精密回放时把工作线程绑到单个逻辑核，减小跨核迁移抖动
class PlaybackThreadAffinityGuard {
public:
    explicit PlaybackThreadAffinityGuard(bool enable);
    ~PlaybackThreadAffinityGuard();
    PlaybackThreadAffinityGuard(const PlaybackThreadAffinityGuard&) = delete;
    PlaybackThreadAffinityGuard& operator=(const PlaybackThreadAffinityGuard&) = delete;
private:
    bool active_ = false;
    HANDLE thread_ = nullptr;
    DWORD_PTR prevMask_ = 0;
};

/// 精密回放时 timeBeginPeriod(1) + 尽量申请更高定时器分辨率
class MultimediaTimerGuard {
public:
    explicit MultimediaTimerGuard(bool enable);
    ~MultimediaTimerGuard();
    MultimediaTimerGuard(const MultimediaTimerGuard&) = delete;
    MultimediaTimerGuard& operator=(const MultimediaTimerGuard&) = delete;
private:
    bool activePeriod_ = false;
    bool activeNtRes_ = false;
    ULONG prevNtRes_ = 0;
};

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
/// 逐字快捷输入；cancelFlag 为 true 时在字间立即返回（热键停止宏用）。
void SendQuickInputText(const std::wstring& text, double charInterval,
    const std::atomic_bool* cancelFlag = nullptr);

struct RunProgramPreset {
    const wchar_t* label;
    const wchar_t* path;
    const wchar_t* displayName;
};

int RunProgramPresetCount();
const RunProgramPreset& RunProgramPresetAt(int index);
std::wstring ResolveRunProgramPath(int presetIndex, const std::wstring& customPath);
std::wstring RunProgramDisplayName(int presetIndex, const std::wstring& customPath);
