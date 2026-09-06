#pragma once
// 编辑器「并入」：把同级勾选动作包进新容器（拆解的反向）。不修改其它宏文件。
#include "action_disassemble.h"

#include <set>
#include <utility>

enum class MergePlacement {
    InPlace = 0,
    Last = 1,
    First = 2,
    Before = 3,
    After = 4,
};

struct MergeAnalyze {
    bool ok = false;
    std::wstring error;
    std::vector<int> roots;
    int parent = -1;
    int indent = 0;
    bool contiguous = false;
};

struct MergeResult {
    bool ok = false;
    std::wstring error;
    std::vector<ScriptAction> actions;
    int containerIndex = -1;
};

inline bool IsMergeContainerType(ActionType type) {
    return type == ActionType::Loop || type == ActionType::If
        || type == ActionType::Else || type == ActionType::DefineBlock;
}

inline MergeAnalyze FailMergeAnalyze(const std::wstring& error) {
    MergeAnalyze a;
    a.error = error;
    return a;
}

inline MergeResult FailMergeResult(const std::wstring& error) {
    MergeResult r;
    r.error = error;
    return r;
}

inline std::vector<int> SortedUniqueSelected(const std::vector<int>& selected, int n) {
    std::set<int> uniq;
    for (int i : selected) {
        if (i >= 0 && i < n) uniq.insert(i);
    }
    return std::vector<int>(uniq.begin(), uniq.end());
}

inline bool IndexCoveredBySelectedAncestor(const std::vector<ScriptAction>& actions,
    int index, const std::set<int>& sel) {
    int indent = actions[static_cast<size_t>(index)].indent;
    int p = FindDirectParentIndex(actions, static_cast<size_t>(index), indent);
    while (p >= 0) {
        if (sel.count(p)) return true;
        indent = actions[static_cast<size_t>(p)].indent;
        p = FindDirectParentIndex(actions, static_cast<size_t>(p), indent);
    }
    return false;
}

inline std::vector<int> MergeSelectedRoots(const std::vector<ScriptAction>& actions,
    const std::vector<int>& selected) {
    const int n = static_cast<int>(actions.size());
    const auto uniq = SortedUniqueSelected(selected, n);
    std::set<int> sel(uniq.begin(), uniq.end());
    std::vector<int> roots;
    for (int i : uniq) {
        if (!IndexCoveredBySelectedAncestor(actions, i, sel)) roots.push_back(i);
    }
    return roots;
}

inline std::vector<int> DirectSiblingIndices(const std::vector<ScriptAction>& actions,
    int parent, int childIndent) {
    std::vector<int> out;
    const int n = static_cast<int>(actions.size());
    if (parent < 0) {
        for (int i = 0; i < n; ++i) {
            if (actions[static_cast<size_t>(i)].indent == childIndent) out.push_back(i);
        }
        return out;
    }
    if (parent >= n) return out;
    const int expect = actions[static_cast<size_t>(parent)].indent + 1;
    if (expect != childIndent) return out;
    const int end = ContainerBodyEnd(actions, parent);
    for (int i = parent + 1; i < end; ++i) {
        if (actions[static_cast<size_t>(i)].indent == expect) out.push_back(i);
    }
    return out;
}

inline bool MergeRootsAreSiblings(const std::vector<int>& siblings, const std::vector<int>& roots) {
    if (roots.empty()) return false;
    std::set<int> sib(siblings.begin(), siblings.end());
    for (int r : roots) {
        if (!sib.count(r)) return false;
    }
    return true;
}

inline bool MergeRootsContiguous(const std::vector<int>& siblings, const std::vector<int>& roots) {
    if (roots.size() < 2 || siblings.empty()) return false;
    std::set<int> rootSet(roots.begin(), roots.end());
    int firstK = -1;
    int lastK = -1;
    for (int k = 0; k < static_cast<int>(siblings.size()); ++k) {
        if (!rootSet.count(siblings[static_cast<size_t>(k)])) continue;
        if (firstK < 0) firstK = k;
        lastK = k;
    }
    if (firstK < 0) return false;
    for (int k = firstK; k <= lastK; ++k) {
        if (!rootSet.count(siblings[static_cast<size_t>(k)])) return false;
    }
    return true;
}

inline MergeAnalyze AnalyzeMergeSelection(const std::vector<ScriptAction>& actions,
    const std::vector<int>& selected) {
    const auto roots = MergeSelectedRoots(actions, selected);
    if (static_cast<int>(roots.size()) < 2) {
        return FailMergeAnalyze(L"请勾选至少两个同级动作");
    }
    const int indent = actions[static_cast<size_t>(roots.front())].indent;
    const int parent = FindDirectParentIndex(
        actions, static_cast<size_t>(roots.front()), indent);
    for (int r : roots) {
        const int ind = actions[static_cast<size_t>(r)].indent;
        const int p = FindDirectParentIndex(actions, static_cast<size_t>(r), ind);
        if (ind != indent || p != parent) {
            return FailMergeAnalyze(L"请勾选同一父节点下的同级动作");
        }
    }
    const auto siblings = DirectSiblingIndices(actions, parent, indent);
    if (!MergeRootsAreSiblings(siblings, roots)) {
        return FailMergeAnalyze(L"请勾选同一父节点下的同级动作");
    }
    MergeAnalyze a;
    a.ok = true;
    a.roots = roots;
    a.parent = parent;
    a.indent = indent;
    a.contiguous = MergeRootsContiguous(siblings, roots);
    return a;
}

inline bool ElseHasPrecedingIf(const std::vector<ScriptAction>& actions, int insertPos,
    int elseIndent) {
    for (int i = insertPos - 1; i >= 0; --i) {
        const int ind = actions[static_cast<size_t>(i)].indent;
        if (ind < elseIndent) return false;
        if (ind == elseIndent) return actions[static_cast<size_t>(i)].type == ActionType::If;
    }
    return false;
}

inline int RemainingCountBefore(int origIndex, const std::vector<std::pair<int, int>>& ranges) {
    int count = 0;
    for (int i = 0; i < origIndex; ++i) {
        bool extracted = false;
        for (const auto& r : ranges) {
            if (i >= r.first && i < r.second) {
                extracted = true;
                break;
            }
        }
        if (!extracted) ++count;
    }
    return count;
}

inline MergeResult MergeSelectedIntoContainer(const std::vector<ScriptAction>& actions,
    const std::vector<int>& selected, const ScriptAction& container, MergePlacement placement) {
    if (!IsMergeContainerType(container.type)) {
        return FailMergeResult(L"该动作不能作为并入容器");
    }
    const MergeAnalyze plan = AnalyzeMergeSelection(actions, selected);
    if (!plan.ok) {
        return FailMergeResult(plan.error);
    }
    if (placement == MergePlacement::InPlace && !plan.contiguous) {
        return FailMergeResult(L"所选动作不连续，请选择插入位置");
    }

    const bool asDefine = container.type == ActionType::DefineBlock;
    const int destIndent = asDefine ? 0 : plan.indent;

    std::vector<std::pair<int, int>> ranges;
    std::vector<ScriptAction> body;
    for (int root : plan.roots) {
        const int end = SubtreeEnd(actions, root);
        ranges.push_back({root, end});
        body.insert(body.end(), actions.begin() + root, actions.begin() + end);
    }
    RemapCopiedActionIndents(body, destIndent + 1);

    const int n = static_cast<int>(actions.size());
    int extracted = 0;
    for (const auto& r : ranges) extracted += r.second - r.first;

    int insertPos = 0;
    if (asDefine) {
        insertPos = 0;
    } else if (placement == MergePlacement::Last) {
        insertPos = n - extracted;
    } else if (placement == MergePlacement::First) {
        insertPos = 0;
    } else if (placement == MergePlacement::After) {
        insertPos = RemainingCountBefore(SubtreeEnd(actions, plan.roots.back()), ranges);
    } else {
        // InPlace / Before：第一个勾选项原来的位置
        insertPos = RemainingCountBefore(plan.roots.front(), ranges);
    }
    if (insertPos < 0) insertPos = 0;
    if (insertPos > n - extracted) insertPos = n - extracted;

    std::vector<ScriptAction> result = actions;
    for (int i = static_cast<int>(ranges.size()) - 1; i >= 0; --i) {
        result.erase(result.begin() + ranges[static_cast<size_t>(i)].first,
            result.begin() + ranges[static_cast<size_t>(i)].second);
    }

    if (container.type == ActionType::Else
        && !ElseHasPrecedingIf(result, insertPos, destIndent)) {
        return FailMergeResult(L"请将「条件-否则」放在同级「条件-如果」后面");
    }

    ScriptAction head = container;
    head.indent = destIndent;
    head.originalNo = 0;
    result.insert(result.begin() + insertPos, head);
    result.insert(result.begin() + insertPos + 1, body.begin(), body.end());

    const std::wstring endLoopErr = ValidateEndLoopPlacements(result);
    if (!endLoopErr.empty()) {
        return FailMergeResult(endLoopErr);
    }

    MergeResult out;
    out.ok = true;
    out.actions = std::move(result);
    out.containerIndex = insertPos;
    return out;
}
