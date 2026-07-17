#include "recorder_timeline.h"

#include "action_utils.h"

#include <algorithm>
#include <cmath>

namespace {

bool RecordedEventMatchesHotkey(const RecordedEvent& e, const Hotkey& hk) {
    if (!hk.enabled || !hk.vk) return false;
    if (hk.vk == VK_LBUTTON) return e.msg == WM_LBUTTONDOWN || e.msg == WM_LBUTTONUP;
    if (hk.vk == VK_RBUTTON) return e.msg == WM_RBUTTONDOWN || e.msg == WM_RBUTTONUP;
    if (hk.vk == VK_MBUTTON) return e.msg == WM_MBUTTONDOWN || e.msg == WM_MBUTTONUP;
    if (hk.vk == VK_XBUTTON1 || hk.vk == VK_XBUTTON2)
        return e.msg == WM_XBUTTONDOWN || e.msg == WM_XBUTTONUP;
    return (e.msg == WM_KEYDOWN || e.msg == WM_KEYUP
        || e.msg == WM_SYSKEYDOWN || e.msg == WM_SYSKEYUP)
        && static_cast<UINT>(e.vkOrButton) == hk.vk;
}

MouseButtonType RecordedButton(WPARAM vk) {
    if (vk == VK_RBUTTON) return MouseButtonType::Right;
    if (vk == VK_MBUTTON) return MouseButtonType::Middle;
    if (vk == VK_XBUTTON1) return MouseButtonType::X1;
    if (vk == VK_XBUTTON2) return MouseButtonType::X2;
    return MouseButtonType::Left;
}

uint64_t SecondsToUs(double seconds) {
    if (!(seconds > 0.0) || !std::isfinite(seconds)) return 0;
    const long double us = static_cast<long double>(seconds) * 1000000.0L;
    return static_cast<uint64_t>(std::llround(us));
}

}  // namespace

void SortRecordedEvents(std::vector<RecordedEvent>& events) {
    std::stable_sort(events.begin(), events.end(),
        [](const RecordedEvent& a, const RecordedEvent& b) {
            if (a.timeOffsetUs != b.timeOffsetUs) return a.timeOffsetUs < b.timeOffsetUs;
            return a.sequence < b.sequence;
        });
}

RecordingConversionResult ConvertRecordedEventsToActions(
    std::vector<RecordedEvent> events, const Hotkey& stopHotkey) {
    RecordingConversionResult out{};
    SortRecordedEvents(events);
    while (!events.empty() && RecordedEventMatchesHotkey(events.back(), stopHotkey))
        events.pop_back();
    if (events.empty()) return out;

    out.durationSeconds = events.back().timeOffsetUs / 1000000.0;
    uint64_t previousUs = 0;
    auto emitTimed = [&](ScriptAction action, uint64_t eventUs) {
        const uint64_t gapUs = eventUs >= previousUs ? eventUs - previousUs : 0;
        action.timingUs = gapUs;
        action.duration = gapUs / 1000000.0;
        action.randomDuration = 0.0;
        out.actions.push_back(std::move(action));
        previousUs = std::max(previousUs, eventUs);
    };

    for (const auto& e : events) {
        if (e.msg == WM_MOUSEMOVE) {
            ScriptAction action{};
            action.type = ActionType::MoveMouse;
            action.x = e.x;
            action.y = e.y;
            action.randomX = action.randomY = 0;
            emitTimed(action, e.timeOffsetUs);
            ++out.absoluteMoveCount;
        } else if (e.msg == kWmRecordedRelativeMove) {
            if (e.x == 0 && e.y == 0) continue;
            // 仅合并同时间戳相对包，保总 dx/dy；勿合并有间隙的包以免压扁高报率轨迹。
            if (!out.actions.empty()
                && out.actions.back().type == ActionType::MoveMouseRelative
                && e.timeOffsetUs == previousUs) {
                out.actions.back().x += e.x;
                out.actions.back().y += e.y;
                ++out.relativeMoveCount;
                continue;
            }
            ScriptAction action{};
            action.type = ActionType::MoveMouseRelative;
            action.x = e.x;
            action.y = e.y;
            action.coordsAreNormalized = false;
            emitTimed(action, e.timeOffsetUs);
            ++out.relativeMoveCount;
        } else if (e.msg == WM_KEYDOWN || e.msg == WM_SYSKEYDOWN
                || e.msg == WM_KEYUP || e.msg == WM_SYSKEYUP) {
            ScriptAction action{};
            const bool down = e.msg == WM_KEYDOWN || e.msg == WM_SYSKEYDOWN;
            action.type = down ? ActionType::KeyDown : ActionType::KeyUp;
            action.keyVk = static_cast<UINT>(e.vkOrButton);
            action.keyText = VkName(action.keyVk);
            emitTimed(action, e.timeOffsetUs);
        } else if (e.msg == WM_LBUTTONDOWN || e.msg == WM_RBUTTONDOWN
                || e.msg == WM_MBUTTONDOWN || e.msg == WM_XBUTTONDOWN
                || e.msg == WM_LBUTTONUP || e.msg == WM_RBUTTONUP
                || e.msg == WM_MBUTTONUP || e.msg == WM_XBUTTONUP) {
            ScriptAction action{};
            const bool down = e.msg == WM_LBUTTONDOWN || e.msg == WM_RBUTTONDOWN
                || e.msg == WM_MBUTTONDOWN || e.msg == WM_XBUTTONDOWN;
            action.type = down ? ActionType::MouseDown : ActionType::MouseUp;
            action.button = RecordedButton(e.vkOrButton);
            emitTimed(action, e.timeOffsetUs);
        } else if (e.msg == WM_MOUSEWHEEL || e.msg == WM_MOUSEHWHEEL) {
            const uint64_t gapUs = e.timeOffsetUs >= previousUs
                ? e.timeOffsetUs - previousUs : 0;
            if (gapUs != 0) {
                ScriptAction wait{};
                wait.type = ActionType::Wait;
                wait.timingUs = gapUs;
                wait.duration = gapUs / 1000000.0;
                wait.randomDuration = 0.0;
                out.actions.push_back(wait);
                previousUs = std::max(previousUs, e.timeOffsetUs);
            }
            ScriptAction action{};
            action.type = ActionType::ScrollWheel;
            action.scrollVertical = e.msg == WM_MOUSEWHEEL;
            action.scrollHorizontal = e.msg == WM_MOUSEHWHEEL;
            action.scrollSteps = std::max(1, std::abs(e.wheelDelta) / WHEEL_DELTA);
            action.scrollDirection = e.wheelDelta > 0 ? 0 : 1;
            action.clickCount = 1;
            action.duration = 0.01;
            action.randomDuration = 0.0;
            out.actions.push_back(action);
        }
    }
    return out;
}

std::vector<TimedInputEvent> CompileInputTimeline(
    const std::vector<ScriptAction>& actions) {
    std::vector<TimedInputEvent> out;
    out.reserve(actions.size());
    uint64_t elapsedUs = 0;
    for (const auto& action : actions) {
        if (action.randomDuration > 1e-12) return {};
        const uint64_t stepUs = action.timingUs > 0
            ? action.timingUs
            : SecondsToUs(action.duration);
        if (action.type == ActionType::Wait) {
            elapsedUs += stepUs;
            continue;
        }
        if (!ActionUsesInterRepeatInterval(action.type))
            elapsedUs += stepUs;
        TimedInputEvent timed{};
        timed.deadlineUs = elapsedUs;
        timed.action = action;
        out.push_back(std::move(timed));
    }
    return out;
}
