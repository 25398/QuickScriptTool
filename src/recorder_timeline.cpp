#include "recorder_timeline.h"

#include "action_utils.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

namespace {

// Raw 队列积压时 QPC 戳会挤在亚毫秒内；真实 HID 报告多在 1–8ms。
constexpr uint64_t kCompressedRelGapMaxUs = 500;
constexpr uint64_t kDefaultRelReportUs = 8000;
constexpr uint64_t kHealthyRelGapMinUs = 2000;
constexpr uint64_t kHealthyRelGapMaxUs = 16000;

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

constexpr double kExpandDurationEps = 1e-9;

}  // namespace

uint64_t ActionStepUs(const ScriptAction& a) {
    if (a.timingUs > 0) return a.timingUs;
    return SecondsToUs(a.duration);
}

bool ActionCarriesRecordingPreDelay(ActionType t) {
    switch (t) {
    case ActionType::MoveMouse:
    case ActionType::MoveMouseRelative:
    case ActionType::MouseDown:
    case ActionType::MouseUp:
    case ActionType::KeyDown:
    case ActionType::KeyUp:
    case ActionType::FindImage:
        return true;
    default:
        return false;
    }
}

ScriptAction MakeExplicitWaitUs(uint64_t gapUs, int indent) {
    ScriptAction wait{};
    wait.type = ActionType::Wait;
    wait.timingUs = gapUs;
    wait.duration = gapUs / 1000000.0;
    wait.randomDuration = 0.0;
    wait.indent = indent;
    return wait;
}

void MergeAdjacentExplicitWaits(std::vector<ScriptAction>& actions) {
    if (actions.size() < 2) return;
    std::vector<ScriptAction> out;
    out.reserve(actions.size());
    for (auto& a : actions) {
        if (!out.empty()
            && out.back().type == ActionType::Wait
            && a.type == ActionType::Wait
            && out.back().indent == a.indent
            && out.back().randomDuration <= kExpandDurationEps
            && a.randomDuration <= kExpandDurationEps) {
            const uint64_t sum = ActionStepUs(out.back()) + ActionStepUs(a);
            out.back().timingUs = sum;
            out.back().duration = sum / 1000000.0;
            out.back().randomDuration = 0.0;
            continue;
        }
        out.push_back(std::move(a));
    }
    actions = std::move(out);
}

void RepairCompressedRelativeGaps(std::vector<ScriptAction>& actions) {
    if (actions.size() < 2) return;

    std::vector<uint64_t> healthy;
    healthy.reserve(64);
    for (size_t i = 1; i + 1 < actions.size(); ++i) {
        if (actions[i].type != ActionType::Wait) continue;
        if (actions[i - 1].type != ActionType::MoveMouseRelative) continue;
        if (actions[i + 1].type != ActionType::MoveMouseRelative) continue;
        if (actions[i].randomDuration > kExpandDurationEps) continue;
        const uint64_t us = ActionStepUs(actions[i]);
        if (us >= kHealthyRelGapMinUs && us <= kHealthyRelGapMaxUs)
            healthy.push_back(us);
    }

    uint64_t targetUs = kDefaultRelReportUs;
    if (!healthy.empty()) {
        const size_t mid = healthy.size() / 2;
        std::nth_element(healthy.begin(), healthy.begin() + static_cast<std::ptrdiff_t>(mid),
            healthy.end());
        targetUs = healthy[mid];
        if (targetUs < 1000) targetUs = 1000;
        if (targetUs > kHealthyRelGapMaxUs) targetUs = kHealthyRelGapMaxUs;
    }

    for (size_t i = 1; i + 1 < actions.size(); ++i) {
        if (actions[i].type != ActionType::Wait) continue;
        if (actions[i - 1].type != ActionType::MoveMouseRelative) continue;
        if (actions[i + 1].type != ActionType::MoveMouseRelative) continue;
        if (actions[i].randomDuration > kExpandDurationEps) continue;
        if (ActionStepUs(actions[i]) >= kCompressedRelGapMaxUs) continue;
        actions[i].timingUs = targetUs;
        actions[i].duration = targetUs / 1000000.0;
        actions[i].randomDuration = 0.0;
    }

    std::vector<ScriptAction> out;
    out.reserve(actions.size() + actions.size() / 8 + 4);
    for (auto& a : actions) {
        if (a.type == ActionType::MoveMouseRelative
            && !out.empty()
            && out.back().type == ActionType::MoveMouseRelative) {
            out.push_back(MakeExplicitWaitUs(targetUs, a.indent));
        }
        out.push_back(std::move(a));
    }
    actions = std::move(out);
}

std::vector<ScriptAction> ExpandRecordingPreDelaysToExplicitWaits(
    const std::vector<ScriptAction>& actions,
    ExpandRecordingPreDelayPolicy policy) {
    std::vector<ScriptAction> out;
    out.reserve(actions.size() * 2);
    for (const auto& src : actions) {
        ScriptAction a = src;
        if (!ActionCarriesRecordingPreDelay(a.type)) {
            out.push_back(std::move(a));
            continue;
        }
        const uint64_t step = ActionStepUs(a);
        const bool shouldExpand = (a.timingUs > 0)
            || (policy.treatAsRecordingTimeline && a.duration > kExpandDurationEps);
        a.timingUs = 0;
        a.duration = 0.0;
        a.randomDuration = 0.0;
        if (shouldExpand && step > 0)
            out.push_back(MakeExplicitWaitUs(step, a.indent));
        out.push_back(std::move(a));
    }
    MergeAdjacentExplicitWaits(out);
    return out;
}

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
    std::unordered_set<UINT> heldKeys;
    auto emitInstant = [&](ScriptAction action, uint64_t eventUs) {
        const uint64_t gapUs = eventUs >= previousUs ? eventUs - previousUs : 0;
        if (gapUs > 0)
            out.actions.push_back(MakeExplicitWaitUs(gapUs, action.indent));
        action.timingUs = 0;
        action.duration = 0.0;
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
            emitInstant(action, e.timeOffsetUs);
            ++out.absoluteMoveCount;
        } else if (e.msg == kWmRecordedRelativeMove) {
            if (e.x == 0 && e.y == 0) continue;
            // 不合并同戳相对包：FPS 常按包积分视角，合并会改变包结构。
            ScriptAction action{};
            action.type = ActionType::MoveMouseRelative;
            action.x = e.x;
            action.y = e.y;
            action.coordsAreNormalized = false;
            emitInstant(action, e.timeOffsetUs);
            ++out.relativeMoveCount;
        } else if (e.msg == WM_KEYDOWN || e.msg == WM_SYSKEYDOWN
                || e.msg == WM_KEYUP || e.msg == WM_SYSKEYUP) {
            const bool down = e.msg == WM_KEYDOWN || e.msg == WM_SYSKEYDOWN;
            const UINT vk = static_cast<UINT>(e.vkOrButton);
            // 过滤自动重复 KEYDOWN：已按住则只推进时间轴（保留 Wait），不再插重复按下。
            if (down) {
                if (heldKeys.count(vk)) {
                    const uint64_t gapUs = e.timeOffsetUs >= previousUs
                        ? e.timeOffsetUs - previousUs : 0;
                    if (gapUs > 0)
                        out.actions.push_back(MakeExplicitWaitUs(gapUs));
                    previousUs = std::max(previousUs, e.timeOffsetUs);
                    continue;
                }
                heldKeys.insert(vk);
            } else {
                if (!heldKeys.count(vk)) {
                    const uint64_t gapUs = e.timeOffsetUs >= previousUs
                        ? e.timeOffsetUs - previousUs : 0;
                    if (gapUs > 0)
                        out.actions.push_back(MakeExplicitWaitUs(gapUs));
                    previousUs = std::max(previousUs, e.timeOffsetUs);
                    continue;
                }
                heldKeys.erase(vk);
            }
            ScriptAction action{};
            action.type = down ? ActionType::KeyDown : ActionType::KeyUp;
            action.keyVk = vk;
            action.keyText = VkName(action.keyVk);
            emitInstant(action, e.timeOffsetUs);
        } else if (e.msg == WM_LBUTTONDOWN || e.msg == WM_RBUTTONDOWN
                || e.msg == WM_MBUTTONDOWN || e.msg == WM_XBUTTONDOWN
                || e.msg == WM_LBUTTONUP || e.msg == WM_RBUTTONUP
                || e.msg == WM_MBUTTONUP || e.msg == WM_XBUTTONUP) {
            ScriptAction action{};
            const bool down = e.msg == WM_LBUTTONDOWN || e.msg == WM_RBUTTONDOWN
                || e.msg == WM_MBUTTONDOWN || e.msg == WM_XBUTTONDOWN;
            action.type = down ? ActionType::MouseDown : ActionType::MouseUp;
            action.button = RecordedButton(e.vkOrButton);
            action.x = e.x;
            action.y = e.y;
            if (down) {
                action.recordedCapturePath = e.capturePath;
                action.captureOffsetX = e.captureOffsetX;
                action.captureOffsetY = e.captureOffsetY;
            }
            emitInstant(action, e.timeOffsetUs);
        } else if (e.msg == WM_MOUSEWHEEL || e.msg == WM_MOUSEHWHEEL) {
            const uint64_t gapUs = e.timeOffsetUs >= previousUs
                ? e.timeOffsetUs - previousUs : 0;
            if (gapUs != 0) {
                out.actions.push_back(MakeExplicitWaitUs(gapUs));
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
    RepairCompressedRelativeGaps(out.actions);
    if (!out.actions.empty()) {
        const auto timeline = CompileInputTimeline(out.actions);
        if (!timeline.empty())
            out.durationSeconds = timeline.back().deadlineUs / 1000000.0;
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
        const uint64_t stepUs = ActionStepUs(action);
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
