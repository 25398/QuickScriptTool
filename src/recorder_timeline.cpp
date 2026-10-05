#include "recorder_timeline.h"

#include "action_utils.h"
#include "recorder_report_interval.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <vector>

namespace {

constexpr double kExpandDurationEps = 1e-9;

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

/// 供诊断：最近一次 `PreparePlaybackTimeline` 的「细分前 / 细分后」相对移动包数。
/// 回放线程写、回放线程读（`[时间轴统计]` 就在同一线程输出），不需要原子量。
uint64_t g_spreadBefore = 0;
uint64_t g_spreadAfter = 0;

uint64_t CountRelativeMoves(const std::vector<ScriptAction>& actions) {
    uint64_t n = 0;
    for (const auto& a : actions) {
        if (a.type == ActionType::MoveMouseRelative) ++n;
    }
    return n;
}

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

    // 只修「明显短于本机报告周期」的间隔（队列积压 / 被系统合并）。
    // ⚠ 旧实现把 [2000,16000]µs 认作唯一「健康」区间、其余一律回退 8000µs，
    //   并把 <500µs 的间隔全部改写 ⇒ 2kHz 以上鼠标的录制在转换阶段被整体拉伸
    //   （4kHz 的 250µs 间隔被拉到 8000µs，累计 32 倍），回放时「转视角 + 走路」
    //   的位移与录制完全不同。改用中位数模型，见 recorder_report_interval.h。
    qst_recorder::ReportIntervalModel model;
    auto isRelWaitRel = [&](size_t i) {
        if (i == 0 || i + 1 >= actions.size()) return false;
        if (actions[i].type != ActionType::Wait) return false;
        if (actions[i - 1].type != ActionType::MoveMouseRelative) return false;
        if (actions[i + 1].type != ActionType::MoveMouseRelative) return false;
        if (actions[i].randomDuration > kExpandDurationEps) return false;
        return true;
    };

    for (size_t i = 1; i + 1 < actions.size(); ++i) {
        if (!isRelWaitRel(i)) continue;
        model.ObserveGap(ActionStepUs(actions[i]));
    }

    const uint64_t targetUs = model.MedianUs();

    for (size_t i = 1; i + 1 < actions.size(); ++i) {
        if (!isRelWaitRel(i)) continue;
        if (!model.IsCompressedGap(ActionStepUs(actions[i]))) continue;
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

void SpreadRelativeMovePackets(std::vector<ScriptAction>& actions, int maxParts) {
    if (maxParts < 2 || actions.size() < 2) return;

    // 【为什么需要它】
    // 游戏（GLFW/Minecraft 这类）是**每帧**把该帧内到达的鼠标位移求和后一次性应用
    // （`yaw += Σdx * sens`）。录制时鼠标按固定报告周期发包，游戏每帧把落在该帧内的
    // **整包**加起来；回放时若游戏帧边界与录制时不同相位，同一个包会落到不同的帧里，
    // 于是「每帧累积量」与录制不同 ⇒ 视角曲线不同 ⇒ 走位轨迹不同。
    // 这正是 `[回放保真]` 里「位移一致但偏差在目标侧」的那一半。
    //
    // 把每个包按它前面的等待窗口**再细分成若干份**铺满该窗口后，每帧拿到的是这段
    // 位移的**积分**，而不是「整包进/整包出」——帧边界相位的影响被摊平，
    // 回放结果变得与帧率/相位无关（可复现）。
    //
    // ⚠ 代价与边界：
    //   1) 它改变的是**每帧累积量的分配方式**（更接近连续积分），所以与录制时
    //      「整包求和」的量化结果**不完全相同**。默认**关闭**，供 A/B 对比。
    //   2) 细分数按窗口长度自适应（≤2ms 一份），不按位移大小——位移为 0 的份会被
    //      跳过，但等待照发，保证 Σ窗口 与 Σ位移 都与原脚本严格相等。
    //   3) 只在「显式 Wait 紧跟一个相对移动」这种成对结构上动手，其余动作原样透传。
    std::vector<ScriptAction> out;
    out.reserve(actions.size() + actions.size() / 2 + 4);
    for (size_t i = 0; i < actions.size(); ++i) {
        const bool pairable =
            actions[i].type == ActionType::Wait
            && i + 1 < actions.size()
            && actions[i + 1].type == ActionType::MoveMouseRelative
            && actions[i].randomDuration <= kExpandDurationEps
            && actions[i + 1].randomDuration <= kExpandDurationEps;
        if (!pairable) {
            out.push_back(std::move(actions[i]));
            continue;
        }
        const uint64_t windowUs = ActionStepUs(actions[i]);
        const int dx = actions[i + 1].x;
        const int dy = actions[i + 1].y;
        if (dx == 0 && dy == 0) {
            out.push_back(std::move(actions[i]));
            continue;
        }
        // 自适应细分数 = ceil(窗口 / 2ms)，上限 maxParts。
        // 取 ceil 是为了**幂等**：每份都 ≤2ms，下次再跑不会继续细分
        // （若按固定档位（<2ms→1、<4ms→2、否则 4），8000 会拆成 4×2000，
        //   而 2000 又落进「<4ms→2」被再拆一次，不收敛）。
        constexpr uint64_t kPartUs = 2000;
        uint64_t want = (windowUs + kPartUs - 1) / kPartUs;
        if (want < 1) want = 1;
        if (want > static_cast<uint64_t>(maxParts)) want = static_cast<uint64_t>(maxParts);
        const int parts = static_cast<int>(want);
        if (parts < 2) {
            out.push_back(std::move(actions[i]));
            continue;
        }
        const ScriptAction& mv = actions[i + 1];
        uint64_t remainUs = windowUs;
        int rx = dx;
        int ry = dy;
        for (int k = 0; k < parts; ++k) {
            const int left = parts - k;
            const uint64_t partUs = remainUs / static_cast<uint64_t>(left);
            remainUs -= partUs;
            const int px = rx / left;   // 逐步消化余数，保证 Σ 严格等于原位移
            const int py = ry / left;
            rx -= px;
            ry -= py;
            out.push_back(MakeExplicitWaitUs(partUs, mv.indent));
            if (px != 0 || py != 0) {
                ScriptAction sub = mv;
                sub.x = px;
                sub.y = py;
                sub.timingUs = 0;
                sub.duration = 0.0;
                sub.randomDuration = 0.0;
                out.push_back(std::move(sub));
            }
        }
        ++i;  // 原相对移动已被上面的子步替代
    }
    actions = std::move(out);
}

void PreparePlaybackTimeline(std::vector<ScriptAction>& actions, bool spreadRelativeMoves) {
    RepairCompressedRelativeGaps(actions);
    g_spreadBefore = CountRelativeMoves(actions);
    if (spreadRelativeMoves) SpreadRelativeMovePackets(actions, 4);
    g_spreadAfter = CountRelativeMoves(actions);
}

void LastSpreadPacketCounts(uint64_t& before, uint64_t& after) {
    before = g_spreadBefore;
    after = g_spreadAfter;
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
    std::vector<RecordedEvent> events, const Hotkey& stopHotkey,
    bool windowRelative) {
    RecordingConversionResult out{};
    auto applyWindowRelative = [&](ScriptAction& action) {
        if (!windowRelative) return;
        // 窗口相对录制：x/y 已是目标窗口客户区像素；禁止屏幕归一化。
        action.windowRelative = true;
        action.coordsAreNormalized = false;
    };
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
            applyWindowRelative(action);
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
            applyWindowRelative(action);
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

RelativeMoveTotals SumRelativeMoves(const std::vector<ScriptAction>& actions) {
    RelativeMoveTotals t{};
    for (const auto& a : actions) {
        if (a.type != ActionType::MoveMouseRelative) continue;
        t.dx += a.x;
        t.dy += a.y;
        ++t.packets;
    }
    return t;
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
        if (action.type == ActionType::MouseDrag) {
            TimedInputEvent timed{};
            timed.deadlineUs = elapsedUs;
            timed.action = action;
            out.push_back(std::move(timed));
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
