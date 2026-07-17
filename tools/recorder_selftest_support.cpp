// RecorderSelfTest 专用：仅提供时间轴转换所需的纯逻辑符号。
// 刻意不链接 action_utils.cpp / mouse_input_backend（含 SendInput），
// 避免 360 HEUR/QVM 把自检 exe 误判为恶意软件并删除。
#include "action_utils.h"

bool ActionUsesInterRepeatInterval(ActionType type) {
    switch (type) {
    case ActionType::MouseClick:
    case ActionType::KeyClick:
    case ActionType::HotkeyShortcut:
    case ActionType::QuickInput:
    case ActionType::ScrollWheel:
    case ActionType::MousePlayback:
        return true;
    default:
        return false;
    }
}

bool ShouldWaitAfterRepeat(const ScriptAction& action, int repeatIndex) {
    return ActionUsesInterRepeatInterval(action.type)
        && repeatIndex >= 0
        && repeatIndex + 1 < action.clickCount;
}
