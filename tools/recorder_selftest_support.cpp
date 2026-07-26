// RecorderSelfTest 专用：仅提供时间轴转换所需的纯逻辑符号。
// 刻意不链接 action_utils.cpp / mouse_input_backend（含 SendInput），
// 避免 360 HEUR/QVM 把自检 exe 误判为恶意软件并删除。
#include "action_utils.h"
#include "coord_space.h"
#include "script_types.h"

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

bool ScriptIsTimedInputSequence(const std::vector<ScriptAction>& actions) {
    if (actions.empty()) return false;
    for (const auto& a : actions) {
        switch (a.type) {
        case ActionType::MoveMouse:
        case ActionType::MoveMouseRelative:
        case ActionType::Wait:
        case ActionType::MouseDown:
        case ActionType::MouseUp:
        case ActionType::MouseClick:
        case ActionType::KeyDown:
        case ActionType::KeyUp:
        case ActionType::KeyClick:
        case ActionType::HotkeyShortcut:
        case ActionType::QuickInput:
        case ActionType::ScrollWheel:
        case ActionType::Loop:
        case ActionType::EndLoop:
        case ActionType::StopMacro:
        case ActionType::Goto:
        case ActionType::FindImage:
            break;
        default:
            return false;
        }
    }
    return true;
}

void SyncFindImageOffsetNorm(ScriptAction& a) {
    // 自检 stub：无真实位图时按 80×80 模板估算，便于断言 offset 一致性
    constexpr double kStubTpl = 80.0;
    a.nOffsetX = a.offsetX / kStubTpl;
    a.nOffsetY = a.offsetY / kStubTpl;
}
