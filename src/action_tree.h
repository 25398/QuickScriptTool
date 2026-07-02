// ──────────────────────────────────────────────────────────────────
// action_tree.h — 动作树结构导航工具
// 提供展开/折叠管理、行可见性判断、子树范围查询等函数。
// 所有函数都在 action_ 向量上操作，用于编辑器的树形显示。
// ──────────────────────────────────────────────────────────────────
#pragma once

#include "script_types.h"

#include <set>
#include <vector>

// 获取容器动作 (Loop/If/DefineBlock) 的主体范围结束位置
// containerIndex: 容器动作的索引
// 返回: 第一个不属于该容器主体的动作索引
inline int ContainerBodyEnd(const std::vector<ScriptAction>& actions, int containerIndex) {
    if (containerIndex < 0 || containerIndex >= static_cast<int>(actions.size())) return containerIndex + 1;
    const int level = actions[static_cast<size_t>(containerIndex)].indent;
    int i = containerIndex + 1;
    while (i < static_cast<int>(actions.size()) && actions[static_cast<size_t>(i)].indent > level) ++i;
    return i;
}

// 获取动作子树的结束位置 (包含容器中所有子节点)
inline int SubtreeEnd(const std::vector<ScriptAction>& actions, int index) {
    if (index < 0 || index >= static_cast<int>(actions.size())) return index + 1;
    if (IsSubtreeContainer(actions[static_cast<size_t>(index)].type)) return ContainerBodyEnd(actions, index);
    return index + 1;
}

// 判断容器是否展开 (不在 collapsed 集合中即为展开)
inline bool IsContainerExpanded(const std::set<int>& collapsed, int index) {
    return collapsed.count(index) == 0;
}

// 判断某行在树中是否可见 (任何祖先容器被折叠则不可见)
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

// 删除动作 range [start, end) 后重新映射折叠容器索引
inline std::set<int> RemapCollapsedAfterDelete(const std::set<int>& collapsed, int start, int end) {
    const int count = end - start;
    std::set<int> updated;
    for (int idx : collapsed) {
        if (idx >= start && idx < end) continue;  // 被删除的折叠节点直接丢弃
        updated.insert(idx >= end ? idx - count : idx);
    }
    return updated;
}

// 移动动作块后重新映射折叠容器索引
// dragStart/dragEnd: 被移动块的原始范围 [start, end)
// insertIndex: 插入目标位置
// blockSize: 被移动块的大小
inline std::set<int> RemapCollapsedAfterMove(
    const std::set<int>& collapsed,
    int dragStart,
    int dragEnd,
    int insertIndex,
    int blockSize) {
    std::set<int> updated;
    for (int idx : collapsed) {
        if (idx >= dragStart && idx < dragEnd) {
            updated.insert(idx - dragStart + insertIndex);  // 被移动的节点映射到新位置
            continue;
        }
        int mapped = idx;
        if (mapped >= dragEnd && mapped < insertIndex) mapped -= blockSize;
        else if (mapped >= insertIndex && mapped < dragStart) mapped += blockSize;
        updated.insert(mapped);
    }
    return updated;
}
