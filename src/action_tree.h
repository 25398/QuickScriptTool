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

// 判断 index 是否位于 loop 动作的主体范围内（不含 loop 头自身）
inline bool IsLoopBodyIndex(const std::vector<ScriptAction>& actions, size_t loopIdx, size_t index) {
    if (loopIdx >= actions.size()) return false;
    const int bodyEnd = ContainerBodyEnd(actions, static_cast<int>(loopIdx));
    return index > loopIdx && index < static_cast<size_t>(bodyEnd);
}

// 返回包含 index 的最外层 loop 头索引；不在任何循环体内则返回 -1
inline int OutermostEnclosingLoop(const std::vector<ScriptAction>& actions, size_t index) {
    int found = -1;
    for (int i = 0; i < static_cast<int>(actions.size()); ++i) {
        if (actions[static_cast<size_t>(i)].type != ActionType::Loop) continue;
        if (IsLoopBodyIndex(actions, static_cast<size_t>(i), index)) found = i;
    }
    return found;
}

// 查找 index 处动作（indent 为 actionIndent）的直接父节点索引；无父节点返回 -1
inline int FindDirectParentIndex(const std::vector<ScriptAction>& actions, size_t index, int actionIndent) {
    if (actionIndent <= 0) return -1;
    for (int i = static_cast<int>(index) - 1; i >= 0; --i) {
        if (actions[static_cast<size_t>(i)].indent < actionIndent)
            return i;
    }
    return -1;
}

// 结束循环必须挂在循环容器下（indent = loop.indent + 1）
inline bool HasLoopParentAt(const std::vector<ScriptAction>& actions, size_t index, int actionIndent) {
    const int parentIdx = FindDirectParentIndex(actions, index, actionIndent);
    if (parentIdx < 0) return false;
    return actions[static_cast<size_t>(parentIdx)].type == ActionType::Loop;
}

inline constexpr const wchar_t* kEndLoopNeedsLoopParentMsg = L"请将循环作为父节点";

// 校验动作列表中所有 endLoop 是否均有循环父节点；通过返回空字符串
inline std::wstring ValidateEndLoopPlacements(const std::vector<ScriptAction>& actions) {
    for (size_t i = 0; i < actions.size(); ++i) {
        if (actions[i].type != ActionType::EndLoop) continue;
        if (!HasLoopParentAt(actions, i, actions[i].indent)) {
            return std::wstring(L"第 ") + std::to_wstring(i + 1) + L" 个动作（结束循环）："
                + kEndLoopNeedsLoopParentMsg;
        }
    }
    return L"";
}

// 返回包含 index 的最内层 loop 头索引
inline int InnermostEnclosingLoop(const std::vector<ScriptAction>& actions, size_t index) {
    int found = -1;
    size_t smallestSpan = static_cast<size_t>(-1);
    for (int i = 0; i < static_cast<int>(actions.size()); ++i) {
        if (actions[static_cast<size_t>(i)].type != ActionType::Loop) continue;
        if (!IsLoopBodyIndex(actions, static_cast<size_t>(i), index)) continue;
        const size_t span = static_cast<size_t>(ContainerBodyEnd(actions, i) - i);
        if (span < smallestSpan) {
            smallestSpan = span;
            found = i;
        }
    }
    return found;
}

// 在 parentLoop 体内查找直接包含 index 的子 loop
inline int EnclosingChildLoopInBody(const std::vector<ScriptAction>& actions, size_t parentLoopIdx, size_t index) {
    if (!IsLoopBodyIndex(actions, parentLoopIdx, index)) return -1;
    const size_t bodyEnd = static_cast<size_t>(ContainerBodyEnd(actions, static_cast<int>(parentLoopIdx)));
    for (size_t j = parentLoopIdx + 1; j < bodyEnd; ++j) {
        if (actions[j].type != ActionType::Loop) continue;
        if (IsLoopBodyIndex(actions, j, index)) return static_cast<int>(j);
    }
    return -1;
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
