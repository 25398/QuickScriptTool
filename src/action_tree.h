#pragma once

#include "script_types.h"

#include <set>
#include <vector>

inline int ContainerBodyEnd(const std::vector<ScriptAction>& actions, int containerIndex) {
    if (containerIndex < 0 || containerIndex >= static_cast<int>(actions.size())) return containerIndex + 1;
    const int level = actions[static_cast<size_t>(containerIndex)].indent;
    int i = containerIndex + 1;
    while (i < static_cast<int>(actions.size()) && actions[static_cast<size_t>(i)].indent > level) ++i;
    return i;
}

inline int SubtreeEnd(const std::vector<ScriptAction>& actions, int index) {
    if (index < 0 || index >= static_cast<int>(actions.size())) return index + 1;
    if (IsSubtreeContainer(actions[static_cast<size_t>(index)].type)) return ContainerBodyEnd(actions, index);
    return index + 1;
}

inline bool IsContainerExpanded(const std::set<int>& collapsed, int index) {
    return collapsed.count(index) == 0;
}

inline bool IsRowVisibleInTree(const std::vector<ScriptAction>& actions, const std::set<int>& collapsed, int index) {
    if (index < 0 || index >= static_cast<int>(actions.size())) return false;
    int targetLevel = actions[static_cast<size_t>(index)].indent;
    for (int i = index - 1; i >= 0; --i) {
        if (actions[static_cast<size_t>(i)].indent < targetLevel) {
            if (IsExpandableContainer(actions[static_cast<size_t>(i)].type) && !IsContainerExpanded(collapsed, i)) return false;
            targetLevel = actions[static_cast<size_t>(i)].indent;
            if (targetLevel == 0) break;
        }
    }
    return true;
}

inline std::set<int> RemapCollapsedAfterDelete(const std::set<int>& collapsed, int start, int end) {
    const int count = end - start;
    std::set<int> updated;
    for (int idx : collapsed) {
        if (idx >= start && idx < end) continue;
        updated.insert(idx >= end ? idx - count : idx);
    }
    return updated;
}

inline std::set<int> RemapCollapsedAfterMove(
    const std::set<int>& collapsed,
    int dragStart,
    int dragEnd,
    int insertIndex,
    int blockSize) {
    std::set<int> updated;
    for (int idx : collapsed) {
        if (idx >= dragStart && idx < dragEnd) {
            updated.insert(idx - dragStart + insertIndex);
            continue;
        }
        int mapped = idx;
        if (mapped >= dragEnd && mapped < insertIndex) mapped -= blockSize;
        else if (mapped >= insertIndex && mapped < dragStart) mapped += blockSize;
        updated.insert(mapped);
    }
    return updated;
}
