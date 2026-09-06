// ──────────────────────────────────────────────────────────────────
// recording_to_findimage.cpp — 录制点击 → 找图点击纯逻辑
// ──────────────────────────────────────────────────────────────────

#include "recording_to_findimage.h"

#include "coord_space.h"
#include "recorder_timeline.h"
#include "utils.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace {

bool IsMouseDown(const ScriptAction& a) { return a.type == ActionType::MouseDown; }
bool IsMouseUp(const ScriptAction& a) { return a.type == ActionType::MouseUp; }
bool IsAbsMove(const ScriptAction& a) { return a.type == ActionType::MoveMouse; }
bool IsRelMove(const ScriptAction& a) { return a.type == ActionType::MoveMouseRelative; }
bool IsWait(const ScriptAction& a) { return a.type == ActionType::Wait; }

enum class ModFamily { Ctrl, Alt, Shift, Win };

bool VkToModFamily(UINT vk, ModFamily& out) {
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        out = ModFamily::Ctrl; return true;
    case VK_MENU: case VK_LMENU: case VK_RMENU:
        out = ModFamily::Alt; return true;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        out = ModFamily::Shift; return true;
    case VK_LWIN: case VK_RWIN:
        out = ModFamily::Win; return true;
    default:
        return false;
    }
}

bool ActionHasHoldModifier(const ScriptAction& a) {
    return a.holdLeftWin || a.holdRightWin
        || a.holdLeftCtrl || a.holdRightCtrl
        || a.holdLeftAlt || a.holdRightAlt
        || a.holdLeftShift || a.holdRightShift;
}

/// 正向扫描 actions[0 .. downIndex) 是否仍有未抬起的修饰键族。
bool ModifierHeldBefore(const std::vector<ScriptAction>& actions, int downIndex) {
    if (downIndex < 0 || downIndex > static_cast<int>(actions.size())) return false;
    if (ActionHasHoldModifier(actions[static_cast<size_t>(downIndex)])) return true;
    std::set<ModFamily> held;
    for (int i = 0; i < downIndex; ++i) {
        const auto& a = actions[static_cast<size_t>(i)];
        if (a.type == ActionType::KeyDown) {
            ModFamily f{};
            if (VkToModFamily(a.keyVk, f)) held.insert(f);
        } else if (a.type == ActionType::KeyUp) {
            ModFamily f{};
            if (VkToModFamily(a.keyVk, f)) held.erase(f);
        }
    }
    return !held.empty();
}

bool AllowedBetweenDownUp(const ScriptAction& a) {
    return IsAbsMove(a) || IsRelMove(a) || IsWait(a);
}

int ManhattanRel(const ScriptAction& a) {
    return std::abs(a.x) + std::abs(a.y);
}

int DistSq(int x0, int y0, int x1, int y1) {
    const int dx = x0 - x1;
    const int dy = y0 - y1;
    return dx * dx + dy * dy;
}

bool TryPointFromAction(const ScriptAction& a, int& x, int& y) {
    // MouseDown/Up/MoveMouse 均可带屏幕坐标；禁止用「是否为 0」判断无效。
    if (IsAbsMove(a) || IsMouseDown(a) || IsMouseUp(a) || a.type == ActionType::MouseClick) {
        x = a.x;
        y = a.y;
        return true;
    }
    return false;
}

ScriptAction MakeFindImageFromDown(const ScriptAction& down,
    const std::wstring& capturePath,
    int captureOffsetX, int captureOffsetY,
    bool preferLocalSearch,
    int clickX, int clickY,
    const std::wstring& findTimeExpr) {
    ScriptAction fi{};
    fi.type = ActionType::FindImage;
    fi.remark = down.remark.empty() ? L"录制升级-找图点击" : down.remark;
    fi.indent = down.indent;
    fi.button = down.button;
    fi.imagePath = EnsureImageInLibrary(capturePath);
    if (fi.imagePath.empty()) fi.imagePath = ResolveImagePath(capturePath);
    fi.matchThreshold = 65.0;
    fi.windowRelative = down.windowRelative;
    if (down.windowRelative) {
        fi.coordsAreNormalized = false;
        // 窗口相对：在目标客户区全图匹配；尺寸变化靠模板等比缩放（与点击坐标同一套）。
        fi.searchFullScreen = true;
    } else {
        fi.searchFullScreen = !preferLocalSearch;
        if (preferLocalSearch) {
            fi.searchX1 = clickX - 200;
            fi.searchY1 = clickY - 200;
            fi.searchX2 = clickX + 200;
            fi.searchY2 = clickY + 200;
        }
    }
    fi.imageScale = 1.0;
    fi.imageScaleMin = 1.0;
    fi.imageScaleMax = 1.0;
    fi.findImageFollowUp = 0;
    fi.findTimeExpr = findTimeExpr.empty() ? L"0" : findTimeExpr;
    fi.matchVarName = L"matchRet";
    fi.offsetX = captureOffsetX;
    fi.offsetY = captureOffsetY;
    fi.timingUs = 0;
    fi.duration = 0.0;
    fi.randomDuration = 0.0;
    fi.clickCount = 1;
    fi.recordedCapturePath.clear();
    fi.captureOffsetX = 0;
    fi.captureOffsetY = 0;
    fi.x = 0;
    fi.y = 0;
    SyncFindImageOffsetNorm(fi);
    return fi;
}

}  // namespace

ClickCaptureRectResult ComputeClickCaptureRect(
    int cx, int cy, int half,
    int vsLeft, int vsTop, int vsRight, int vsBottom) {
    ClickCaptureRectResult r{};
    half = ClampClickCaptureHalfSize(half);
    int x1 = cx - half;
    int y1 = cy - half;
    int x2 = cx + half;  // exclusive
    int y2 = cy + half;
    x1 = std::max(x1, vsLeft);
    y1 = std::max(y1, vsTop);
    x2 = std::min(x2, vsRight);
    y2 = std::min(y2, vsBottom);
    const int w = x2 - x1;
    const int h = y2 - y1;
    if (w < kClickCaptureMinSide || h < kClickCaptureMinSide) {
        r.valid = false;
        return r;
    }
    r.x1 = x1;
    r.y1 = y1;
    r.x2 = x2;
    r.y2 = y2;
    const int centerX = x1 + w / 2;
    const int centerY = y1 + h / 2;
    r.offsetX = cx - centerX;
    r.offsetY = cy - centerY;
    r.valid = true;
    return r;
}

std::wstring MakeRecordingClickCaptureFileName(uint64_t sessionId, uint64_t sequence) {
    return L"rec_" + std::to_wstring(sessionId) + L"_" + std::to_wstring(sequence) + L".bmp";
}

void RenumberScriptActions(std::vector<ScriptAction>& actions) {
    for (size_t i = 0; i < actions.size(); ++i) {
        actions[i].originalNo = static_cast<int>(i + 1);
    }
}

bool ResolveClickScreenPoint(
    const std::vector<ScriptAction>& actions,
    const ClickUnit& unit,
    int& outX, int& outY) {
    if (!unit.ok || unit.downIndex < 0
        || unit.downIndex >= static_cast<int>(actions.size())) {
        return false;
    }
    const auto& down = actions[static_cast<size_t>(unit.downIndex)];
    if (TryPointFromAction(down, outX, outY)) return true;

    if (unit.snapMoveIndex >= 0
        && unit.snapMoveIndex < static_cast<int>(actions.size())) {
        if (TryPointFromAction(actions[static_cast<size_t>(unit.snapMoveIndex)], outX, outY)) {
            return true;
        }
    }

    if (unit.downIndex >= 1) {
        const auto& prev = actions[static_cast<size_t>(unit.downIndex - 1)];
        if (IsAbsMove(prev) && TryPointFromAction(prev, outX, outY)) return true;
    }

    int lastX = 0, lastY = 0;
    bool found = false;
    const int up = unit.upIndex >= 0 ? unit.upIndex : unit.downIndex;
    for (int i = unit.downIndex; i <= up && i < static_cast<int>(actions.size()); ++i) {
        int x = 0, y = 0;
        if (TryPointFromAction(actions[static_cast<size_t>(i)], x, y)) {
            lastX = x;
            lastY = y;
            found = true;
        }
    }
    if (found) {
        outX = lastX;
        outY = lastY;
        return true;
    }
    return false;
}

ClickUnitProbe ProbeClickUnitFromDown(
    const std::vector<ScriptAction>& actions, int downIndex,
    int begin, int end) {
    ClickUnitProbe probe{};
    if (downIndex < begin || downIndex >= end
        || downIndex >= static_cast<int>(actions.size())) {
        probe.reject = ClickUnitRejectReason::NotApplicable;
        return probe;
    }
    const auto& down = actions[static_cast<size_t>(downIndex)];
    if (!IsMouseDown(down)) {
        probe.reject = ClickUnitRejectReason::NotApplicable;
        return probe;
    }

    int upIndex = -1;
    int absMoveCount = 0;
    int relManhattan = 0;
    for (int j = downIndex + 1; j < end && j < static_cast<int>(actions.size()); ++j) {
        const auto& a = actions[static_cast<size_t>(j)];
        if (IsMouseUp(a) && a.button == down.button) {
            upIndex = j;
            break;
        }
        if (!AllowedBetweenDownUp(a)) {
            probe.reject = ClickUnitRejectReason::ForbiddenBetween;
            return probe;
        }
        if (IsAbsMove(a)) ++absMoveCount;
        if (IsRelMove(a)) relManhattan += ManhattanRel(a);
    }
    if (upIndex < 0) {
        probe.reject = ClickUnitRejectReason::NoPair;
        return probe;
    }

    int downX = 0, downY = 0, upX = 0, upY = 0;
    ClickUnit temp{};
    temp.ok = true;
    temp.downIndex = downIndex;
    temp.upIndex = upIndex;
    if (!ResolveClickScreenPoint(actions, temp, downX, downY)) {
        // Down 无坐标时仍可用 Up
        TryPointFromAction(actions[static_cast<size_t>(upIndex)], downX, downY);
    }
    TryPointFromAction(actions[static_cast<size_t>(upIndex)], upX, upY);
    // 若 Up 也无显式坐标，用 Resolve 的点作为两端
    if (!TryPointFromAction(actions[static_cast<size_t>(downIndex)], downX, downY)) {
        downX = upX;
        downY = upY;
    }
    if (!TryPointFromAction(actions[static_cast<size_t>(upIndex)], upX, upY)) {
        upX = downX;
        upY = downY;
    }

    const int dragLimitSq = kDragRejectPx * kDragRejectPx;
    if (DistSq(downX, downY, upX, upY) > dragLimitSq) {
        probe.reject = ClickUnitRejectReason::DragDistance;
        return probe;
    }
    if (absMoveCount > kDragRejectMaxAbsMoves) {
        probe.reject = ClickUnitRejectReason::DragAbsMoveCount;
        return probe;
    }
    if (relManhattan > kDragRejectRelativeSum) {
        probe.reject = ClickUnitRejectReason::DragRelativeSum;
        return probe;
    }
    if (ModifierHeldBefore(actions, downIndex)) {
        probe.reject = ClickUnitRejectReason::ModifierHeld;
        return probe;
    }

    // 点击前连续绝对 Move（中间可夹 Wait）整段并入删除：回放由找图定位。
    int approachBegin = -1;
    if (downIndex >= 1) {
        int k = downIndex - 1;
        while (k >= 0 && (IsAbsMove(actions[static_cast<size_t>(k)])
            || IsWait(actions[static_cast<size_t>(k)]))) {
            if (IsAbsMove(actions[static_cast<size_t>(k)]))
                approachBegin = k;
            --k;
        }
    }

    probe.unit.ok = true;
    probe.unit.downIndex = downIndex;
    probe.unit.upIndex = upIndex;
    probe.unit.snapMoveIndex = approachBegin;
    probe.reject = ClickUnitRejectReason::None;
    return probe;
}

ClickUnitProbe ProbeClickUnitFromSelection(
    const std::vector<ScriptAction>& actions, int selectedIndex) {
    ClickUnitProbe probe{};
    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(actions.size())) {
        probe.reject = ClickUnitRejectReason::NotApplicable;
        return probe;
    }
    const int n = static_cast<int>(actions.size());
    const auto& sel = actions[static_cast<size_t>(selectedIndex)];

    if (sel.type == ActionType::FindImage) {
        probe.reject = ClickUnitRejectReason::NotApplicable;
        return probe;
    }

    if (sel.type == ActionType::MouseClick) {
        if (sel.clickCount > 1) {
            probe.reject = ClickUnitRejectReason::MultiClick;
            return probe;
        }
        // 合成单步：当作 down==up==selected
        if (ModifierHeldBefore(actions, selectedIndex) || ActionHasHoldModifier(sel)) {
            probe.reject = ClickUnitRejectReason::ModifierHeld;
            return probe;
        }
        probe.unit.ok = true;
        probe.unit.downIndex = selectedIndex;
        probe.unit.upIndex = selectedIndex;
        probe.unit.snapMoveIndex = -1;
        if (selectedIndex >= 1) {
            int k = selectedIndex - 1;
            int approachBegin = -1;
            while (k >= 0 && (IsAbsMove(actions[static_cast<size_t>(k)])
                || IsWait(actions[static_cast<size_t>(k)]))) {
                if (IsAbsMove(actions[static_cast<size_t>(k)]))
                    approachBegin = k;
                --k;
            }
            probe.unit.snapMoveIndex = approachBegin;
        }
        probe.reject = ClickUnitRejectReason::None;
        return probe;
    }

    if (IsMouseDown(sel)) {
        return ProbeClickUnitFromDown(actions, selectedIndex, 0, n);
    }
    if (IsMouseUp(sel)) {
        for (int i = selectedIndex - 1; i >= 0; --i) {
            const auto& a = actions[static_cast<size_t>(i)];
            if (IsMouseDown(a) && a.button == sel.button) {
                return ProbeClickUnitFromDown(actions, i, 0, n);
            }
            if (IsMouseDown(a) || IsMouseUp(a)) break;
            if (!AllowedBetweenDownUp(a)) break;
        }
        probe.reject = ClickUnitRejectReason::NoPair;
        return probe;
    }
    if (IsAbsMove(sel)) {
        int j = selectedIndex + 1;
        while (j < n && IsWait(actions[static_cast<size_t>(j)])) ++j;
        if (j < n && IsMouseDown(actions[static_cast<size_t>(j)])) {
            return ProbeClickUnitFromDown(actions, j, 0, n);
        }
        probe.reject = ClickUnitRejectReason::NotApplicable;
        return probe;
    }

    probe.reject = ClickUnitRejectReason::NotApplicable;
    return probe;
}

bool ConvertOneClickUnitToFindImage(
    std::vector<ScriptAction>& actions,
    const ClickUnit& unit,
    const ConvertToFindImageOptions& options,
    ClickUnitRejectReason* outReject) {
    auto fail = [&](ClickUnitRejectReason r) {
        if (outReject) *outReject = r;
        return false;
    };
    if (!unit.ok || unit.downIndex < 0
        || unit.downIndex >= static_cast<int>(actions.size())
        || unit.upIndex < 0
        || unit.upIndex >= static_cast<int>(actions.size())
        || unit.upIndex < unit.downIndex) {
        return fail(ClickUnitRejectReason::NotApplicable);
    }

    const ScriptAction down = actions[static_cast<size_t>(unit.downIndex)];
    const bool isMouseClick = down.type == ActionType::MouseClick
        && unit.downIndex == unit.upIndex;

    std::wstring capturePath = down.recordedCapturePath;
    int capOffX = down.captureOffsetX;
    int capOffY = down.captureOffsetY;
    if (capturePath.empty() && isMouseClick) {
        // MouseClick 无录制模板时走调用方补图；此处仍可要求 path
    }
    if (options.requireCapturePath && capturePath.empty()) {
        return fail(ClickUnitRejectReason::NoCapturePath);
    }
    if (capturePath.empty()) {
        return fail(ClickUnitRejectReason::NoCapturePath);
    }

    int clickX = 0, clickY = 0;
    if (!ResolveClickScreenPoint(actions, unit, clickX, clickY)
        && !TryPointFromAction(down, clickX, clickY)) {
        return fail(ClickUnitRejectReason::NoCoordinate);
    }

    const int i = unit.downIndex;
    const int j = unit.upIndex;
    const int approachBegin = unit.snapMoveIndex;

    auto canEraseAbsAt = [&](int idx) -> bool {
        if (idx < 0 || idx >= static_cast<int>(actions.size())) return false;
        if (!IsAbsMove(actions[static_cast<size_t>(idx)])) return false;
        if (!options.restrictAbsMoveErase) return true;
        return options.erasableAbsMoveNos.count(
            actions[static_cast<size_t>(idx)].originalNo) != 0;
    };

    auto shouldEraseWaitAt = [&](int idx, int zoneLo, int zoneHi) -> bool {
        if (idx < zoneLo || idx >= zoneHi) return false;
        if (!IsWait(actions[static_cast<size_t>(idx)])) return false;
        bool erasableBefore = false;
        bool erasableAfter = false;
        for (int t = zoneLo; t < idx; ++t) {
            if (canEraseAbsAt(t)) { erasableBefore = true; break; }
        }
        for (int t = idx + 1; t < zoneHi; ++t) {
            if (canEraseAbsAt(t)) { erasableAfter = true; break; }
        }
        // 夹在可删 Abs 之间，或位于可删 Abs 与 Down 之间
        return erasableBefore || erasableAfter
            || (erasableBefore && zoneHi == i);
    };

    // 被删 Abs / 夹 Wait / Down·Up → 前置显式 Wait；FindImage 自身 timing=0
    uint64_t sumUs = ActionStepUs(down);
    if (!isMouseClick) {
        sumUs += ActionStepUs(actions[static_cast<size_t>(j)]);
        for (int k = i + 1; k < j; ++k) {
            if (IsWait(actions[static_cast<size_t>(k)]) || canEraseAbsAt(k))
                sumUs += ActionStepUs(actions[static_cast<size_t>(k)]);
        }
    }
    if (approachBegin >= 0 && approachBegin < i) {
        for (int k = approachBegin; k < i; ++k) {
            if (canEraseAbsAt(k))
                sumUs += ActionStepUs(actions[static_cast<size_t>(k)]);
            else if (shouldEraseWaitAt(k, approachBegin, i))
                sumUs += ActionStepUs(actions[static_cast<size_t>(k)]);
        }
    }

    ScriptAction fi = MakeFindImageFromDown(
        down, capturePath, capOffX, capOffY,
        options.preferLocalSearch, clickX, clickY, options.findTimeExpr);

    auto eraseApproachThenInsert = [&](int downAt) {
        int insertAt = downAt;
        if (approachBegin >= 0 && approachBegin < downAt) {
            int firstErased = -1;
            for (int k = downAt - 1; k >= approachBegin; --k) {
                if (canEraseAbsAt(k)
                    || shouldEraseWaitAt(k, approachBegin, downAt)) {
                    actions.erase(actions.begin() + k);
                    firstErased = k;
                }
            }
            if (firstErased >= 0) insertAt = firstErased;
        }
        if (sumUs > 0) {
            actions.insert(actions.begin() + insertAt,
                MakeExplicitWaitUs(sumUs, fi.indent));
            ++insertAt;
        }
        actions.insert(actions.begin() + insertAt, std::move(fi));
    };

    if (isMouseClick) {
        actions.erase(actions.begin() + i);
        eraseApproachThenInsert(i);
        if (outReject) *outReject = ClickUnitRejectReason::None;
        return true;
    }

    actions.erase(actions.begin() + j);
    for (int k = j - 1; k > i; --k) {
        if (IsWait(actions[static_cast<size_t>(k)]) || canEraseAbsAt(k))
            actions.erase(actions.begin() + k);
    }
    actions.erase(actions.begin() + i);
    eraseApproachThenInsert(i);
    if (outReject) *outReject = ClickUnitRejectReason::None;
    return true;
}

namespace {

ConvertToFindImageResult ConvertWantedDowns(
    std::vector<ScriptAction>& actions,
    const std::set<int>& wantNos,
    const ConvertToFindImageOptions& options) {
    ConvertToFindImageResult result{};
    std::set<int> skippedNos;
    std::set<int> remaining = wantNos;

    bool progressed = true;
    while (progressed) {
        progressed = false;
        const int n = static_cast<int>(actions.size());
        for (int i = n - 1; i >= 0; --i) {
            if (!IsMouseDown(actions[static_cast<size_t>(i)])) continue;
            const int no = actions[static_cast<size_t>(i)].originalNo;
            if (!remaining.count(no) || skippedNos.count(no)) continue;
            auto probe = ProbeClickUnitFromDown(actions, i, 0, n);
            if (!probe.unit.ok) {
                skippedNos.insert(no);
                remaining.erase(no);
                ++result.skipped;
                continue;
            }
            ClickUnitRejectReason reject = ClickUnitRejectReason::None;
            if (ConvertOneClickUnitToFindImage(actions, probe.unit, options, &reject)) {
                ++result.converted;
                remaining.erase(no);
                progressed = true;
                break;
            }
            skippedNos.insert(no);
            remaining.erase(no);
            ++result.skipped;
        }
    }

    RenumberScriptActions(actions);
    std::wostringstream oss;
    oss << L"成功 " << result.converted << L"，跳过 " << result.skipped;
    result.detail = oss.str();
    return result;
}

bool UnitIntersectsSelection(const ClickUnit& unit, const std::vector<char>& selected) {
    for (int k = unit.downIndex; k <= unit.upIndex; ++k) {
        if (k >= 0 && k < static_cast<int>(selected.size()) && selected[static_cast<size_t>(k)]) {
            return true;
        }
    }
    if (unit.snapMoveIndex >= 0 && unit.snapMoveIndex < unit.downIndex) {
        for (int k = unit.snapMoveIndex; k < unit.downIndex; ++k) {
            if (k < static_cast<int>(selected.size()) && selected[static_cast<size_t>(k)]) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

ConvertToFindImageResult ConvertActionsToFindImage(
    std::vector<ScriptAction>& actions,
    int begin, int end,
    const ConvertToFindImageOptions& options) {
    ConvertToFindImageResult result{};
    begin = std::max(0, begin);
    end = std::min(end, static_cast<int>(actions.size()));
    if (begin >= end) {
        result.detail = L"无可转换范围";
        return result;
    }

    RenumberScriptActions(actions);
    std::set<int> wantNos;
    for (int i = begin; i < end; ++i) {
        if (IsMouseDown(actions[static_cast<size_t>(i)])) {
            wantNos.insert(actions[static_cast<size_t>(i)].originalNo);
        }
    }
    return ConvertWantedDowns(actions, wantNos, options);
}

ConvertToFindImageResult ConvertActionsToFindImageSelected(
    std::vector<ScriptAction>& actions,
    const std::vector<char>& selected,
    const ConvertToFindImageOptions& options) {
    const bool useSelection = !selected.empty()
        && selected.size() == actions.size()
        && std::any_of(selected.begin(), selected.end(), [](char c) { return c != 0; });

    if (!useSelection) {
        ConvertToFindImageResult result{};
        result.detail = L"未选择动作";
        return result;
    }

    RenumberScriptActions(actions);
    ConvertToFindImageOptions opts = options;
    opts.restrictAbsMoveErase = true;
    opts.erasableAbsMoveNos.clear();
    const int n0 = static_cast<int>(actions.size());
    for (int i = 0; i < n0; ++i) {
        if (!selected[static_cast<size_t>(i)]) continue;
        if (IsAbsMove(actions[static_cast<size_t>(i)])) {
            opts.erasableAbsMoveNos.insert(actions[static_cast<size_t>(i)].originalNo);
        }
    }

    std::set<int> wantNos;
    for (int i = 0; i < n0; ++i) {
        if (!IsMouseDown(actions[static_cast<size_t>(i)])) continue;
        auto probe = ProbeClickUnitFromDown(actions, i, 0, n0);
        if (!probe.unit.ok) continue;
        if (UnitIntersectsSelection(probe.unit, selected)) {
            wantNos.insert(actions[static_cast<size_t>(i)].originalNo);
        }
    }
    return ConvertWantedDowns(actions, wantNos, opts);
}
