#include "recording_optimize_ops.h"

#include "recorder_timeline.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace recopt {
namespace {

std::vector<int> ContiguousSelectedRange(const std::vector<char>& selected) {
    std::vector<int> indices;
    for (int i = 0; i < static_cast<int>(selected.size()); ++i)
        if (selected[static_cast<size_t>(i)]) indices.push_back(i);
    if (indices.empty()) return {};
    for (size_t i = 1; i < indices.size(); ++i)
        if (indices[i] != indices[i - 1] + 1) return {};
    return indices;
}

const char* ContiguousMoveWaitErr(bool forMerge) {
    return forMerge
        ? "鼠标移动合并时只能选择连续的移动和等待操作，其中不允许夹杂其他操作。"
        : "鼠标移动压缩时只能选择连续的移动和等待操作，其中不允许夹杂其他操作。";
}

bool RangeHasRelativeMove(const std::vector<ScriptAction>& actions, int first, int last) {
    for (int i = first; i <= last; ++i) {
        if (actions[static_cast<size_t>(i)].type == ActionType::MoveMouseRelative) return true;
    }
    return false;
}

double ComputeMergeWaitSeconds(const std::vector<double>& waits, const std::string& mode, double fixed) {
    if (mode == "fixed" || mode == "custom" || mode == "specified") return (std::max)(0.0, fixed);
    if (waits.empty()) return 0.0;
    if (mode == "average" || mode == "avg") {
        double sum = 0;
        for (double w : waits) sum += w;
        return sum / static_cast<double>(waits.size());
    }
    if (mode == "first") return waits.front();
    if (mode == "last") return waits.back();
    double sum = 0;
    for (double w : waits) sum += w;
    return sum;
}

enum class RangeApply {
    Applied,
    SkipRelative,
    SkipNoMove,
    SkipTooFewPoints,
};

RangeApply BuildMergeReplacement(const std::vector<ScriptAction>& actions, int first, int last,
    const std::string& waitCalc, double mergeWait, std::vector<ScriptAction>& out) {
    out.clear();
    if (RangeHasRelativeMove(actions, first, last)) return RangeApply::SkipRelative;
    std::vector<double> waits;
    ScriptAction lastMove{};
    bool hasMove = false;
    for (int idx = first; idx <= last; ++idx) {
        const auto& a = actions[static_cast<size_t>(idx)];
        if (a.type == ActionType::Wait) waits.push_back(ActionStepUs(a) / 1000000.0);
        else if (a.type == ActionType::MoveMouse) {
            lastMove = a;
            hasMove = true;
        }
    }
    if (!hasMove) return RangeApply::SkipNoMove;
    const double mergedWait = ComputeMergeWaitSeconds(waits, waitCalc, mergeWait);
    lastMove.duration = 0.0;
    lastMove.timingUs = 0;
    lastMove.randomDuration = 0.0;
    if (mergedWait > 0.0005) {
        ScriptAction wa{};
        wa.type = ActionType::Wait;
        wa.duration = mergedWait;
        const long double us = static_cast<long double>(wa.duration) * 1000000.0L;
        wa.timingUs = static_cast<uint64_t>(std::llround(us));
        out.push_back(wa);
    }
    out.push_back(lastMove);
    return RangeApply::Applied;
}

RangeApply BuildCompressReplacement(const std::vector<ScriptAction>& actions, int first, int last,
    double thr, double cw, std::vector<ScriptAction>& out) {
    out.clear();
    if (RangeHasRelativeMove(actions, first, last)) return RangeApply::SkipRelative;
    struct Point { int x; int y; };
    std::vector<Point> points;
    std::vector<uint64_t> gapBefore;
    uint64_t pending = 0;
    bool seenMove = false;
    for (int idx = first; idx <= last; ++idx) {
        const auto& a = actions[static_cast<size_t>(idx)];
        if (a.type == ActionType::Wait) {
            if (seenMove) pending += ActionStepUs(a);
            continue;
        }
        if (a.type == ActionType::MoveMouse) {
            if (!seenMove) {
                seenMove = true;
                points.push_back({a.x, a.y});
                pending = ActionStepUs(a);
            } else {
                gapBefore.push_back(pending + ActionStepUs(a));
                pending = 0;
                points.push_back({a.x, a.y});
            }
        }
    }
    if (points.size() < 2) return RangeApply::SkipTooFewPoints;
    auto dist = [](const Point& a, const Point& b) {
        const double dx = static_cast<double>(a.x - b.x), dy = static_cast<double>(a.y - b.y);
        return std::sqrt(dx * dx + dy * dy);
    };
    std::vector<Point> compressed;
    std::vector<uint64_t> compressedGaps;
    compressed.push_back(points.front());
    size_t lastKept = 0;
    for (size_t i = 1; i + 1 < points.size(); ++i) {
        if (dist(compressed.back(), points[i]) < thr) continue;
        uint64_t gapSum = 0;
        for (size_t g = lastKept; g < i; ++g) gapSum += gapBefore[g];
        compressedGaps.push_back(gapSum);
        compressed.push_back(points[i]);
        lastKept = i;
    }
    {
        uint64_t gapSum = 0;
        for (size_t g = lastKept; g < gapBefore.size(); ++g) gapSum += gapBefore[g];
        if (compressed.back().x != points.back().x || compressed.back().y != points.back().y) {
            compressedGaps.push_back(gapSum);
            compressed.push_back(points.back());
        }
    }
    bool keepWindowRelative = false;
    for (int idx = first; idx <= last; ++idx) {
        const auto& a = actions[static_cast<size_t>(idx)];
        if (a.type == ActionType::MoveMouse && a.windowRelative) {
            keepWindowRelative = true;
            break;
        }
    }
    for (size_t i = 0; i < compressed.size(); ++i) {
        if (i > 0) {
            const uint64_t gap = (i - 1 < compressedGaps.size()) ? compressedGaps[i - 1] : 0;
            if (gap > 0) {
                out.push_back(MakeExplicitWaitUs(gap));
            } else if (cw > 0.0005) {
                ScriptAction wa{};
                wa.type = ActionType::Wait;
                wa.duration = cw;
                const long double us = static_cast<long double>(wa.duration) * 1000000.0L;
                wa.timingUs = static_cast<uint64_t>(std::llround(us));
                out.push_back(wa);
            }
        }
        ScriptAction mv{};
        mv.type = ActionType::MoveMouse;
        mv.x = compressed[i].x;
        mv.y = compressed[i].y;
        mv.duration = 0.0;
        mv.timingUs = 0;
        if (keepWindowRelative) {
            mv.windowRelative = true;
            mv.coordsAreNormalized = false;
        }
        out.push_back(mv);
    }
    return RangeApply::Applied;
}

struct RangeReplacement {
    int first = 0;
    int last = 0;
    std::vector<ScriptAction> actions;
};

void ApplyRangeReplacements(std::vector<ScriptAction>& actions,
    const std::vector<RangeReplacement>& reps) {
    if (reps.empty()) return;
    std::vector<ScriptAction> result;
    size_t r = 0;
    for (int i = 0; i < static_cast<int>(actions.size()); ) {
        if (r < reps.size() && i == reps[r].first) {
            for (const auto& a : reps[r].actions) result.push_back(a);
            i = reps[r].last + 1;
            ++r;
        } else {
            result.push_back(actions[static_cast<size_t>(i)]);
            ++i;
        }
    }
    actions = std::move(result);
}

OptimizeApplyResult ApplyOnRanges(std::vector<ScriptAction>& actions,
    const std::vector<IndexRange>& ranges, bool forMerge,
    const std::string& waitCalc, double mergeWait, double thr, double cw) {
    OptimizeApplyResult result;
    std::vector<RangeReplacement> reps;
    for (const auto& rg : ranges) {
        std::vector<ScriptAction> built;
        const RangeApply st = forMerge
            ? BuildMergeReplacement(actions, rg.first, rg.last, waitCalc, mergeWait, built)
            : BuildCompressReplacement(actions, rg.first, rg.last, thr, cw, built);
        if (st == RangeApply::Applied) {
            reps.push_back({rg.first, rg.last, std::move(built)});
            ++result.applied;
        } else if (st == RangeApply::SkipRelative) {
            ++result.skippedRelative;
        } else if (st == RangeApply::SkipNoMove) {
            ++result.skippedNoMove;
        } else {
            ++result.skippedTooFew;
        }
    }
    ApplyRangeReplacements(actions, reps);
    return result;
}

}  // namespace

bool IsMoveOrWait(const ScriptAction& a) {
    return a.type == ActionType::MoveMouse
        || a.type == ActionType::MoveMouseRelative
        || a.type == ActionType::Wait;
}

bool CollectMoveWaitRanges(const std::vector<ScriptAction>& actions,
    const std::vector<char>& selected, bool forMerge,
    std::vector<IndexRange>& ranges, std::string& err) {
    ranges.clear();
    std::vector<int> indices;
    bool hasKey = false;
    const int n = static_cast<int>((std::min)(selected.size(), actions.size()));
    for (int i = 0; i < n; ++i) {
        if (!selected[static_cast<size_t>(i)]) continue;
        indices.push_back(i);
        if (!IsMoveOrWait(actions[static_cast<size_t>(i)])) hasKey = true;
    }
    if (indices.empty()) {
        err = forMerge ? "请先选择要合并的移动/等待。" : "请先选择要压缩的移动/等待。";
        return false;
    }
    if (!hasKey) {
        const auto range = ContiguousSelectedRange(selected);
        if (range.empty()) {
            err = ContiguousMoveWaitErr(forMerge);
            return false;
        }
        for (int idx : range) {
            if (idx < 0 || idx >= static_cast<int>(actions.size())
                || !IsMoveOrWait(actions[static_cast<size_t>(idx)])) {
                err = ContiguousMoveWaitErr(forMerge);
                return false;
            }
        }
        ranges.push_back({range.front(), range.back()});
        return true;
    }
    int runFirst = -1;
    int runLast = -1;
    auto flush = [&]() {
        if (runFirst >= 0) ranges.push_back({runFirst, runLast});
        runFirst = -1;
        runLast = -1;
    };
    for (int idx : indices) {
        if (!IsMoveOrWait(actions[static_cast<size_t>(idx)])) {
            flush();
            continue;
        }
        if (runFirst < 0) {
            runFirst = runLast = idx;
        } else if (idx == runLast + 1) {
            runLast = idx;
        } else {
            flush();
            runFirst = runLast = idx;
        }
    }
    flush();
    return true;
}

OptimizeApplyResult MergeSelected(std::vector<ScriptAction>& actions,
    const std::vector<char>& selected, const std::string& waitCalc, double fixedWait) {
    std::vector<IndexRange> ranges;
    OptimizeApplyResult result;
    if (!CollectMoveWaitRanges(actions, selected, true, ranges, result.collectErr)) {
        result.collectOk = false;
        return result;
    }
    return ApplyOnRanges(actions, ranges, true, waitCalc, fixedWait, 0.0, 0.0);
}

OptimizeApplyResult CompressSelected(std::vector<ScriptAction>& actions,
    const std::vector<char>& selected, double distanceThreshold, double compressWait) {
    std::vector<IndexRange> ranges;
    OptimizeApplyResult result;
    if (!CollectMoveWaitRanges(actions, selected, false, ranges, result.collectErr)) {
        result.collectOk = false;
        return result;
    }
    return ApplyOnRanges(actions, ranges, false, "", 0.0, distanceThreshold, compressWait);
}

OptimizeApplyResult MergeAllKeySplit(std::vector<ScriptAction>& actions,
    const std::string& waitCalc, double fixedWait) {
    std::vector<char> selected(actions.size(), 1);
    return MergeSelected(actions, selected, waitCalc, fixedWait);
}

OptimizeApplyResult CompressAllKeySplit(std::vector<ScriptAction>& actions,
    double distanceThreshold, double compressWait) {
    std::vector<char> selected(actions.size(), 1);
    return CompressSelected(actions, selected, distanceThreshold, compressWait);
}

}  // namespace recopt
