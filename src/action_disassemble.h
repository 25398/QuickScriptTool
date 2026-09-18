#pragma once
// 编辑器「拆解为动作」：一层内联。不修改被调宏/录制/指令块源文件。
#include "action_tree.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

inline bool IsDisassemblableAction(ActionType type) {
    switch (type) {
    case ActionType::Loop:
    case ActionType::If:
    case ActionType::Else:
    case ActionType::DefineBlock:
    case ActionType::WatchImage:
    case ActionType::RunBlock:
    case ActionType::RunMacro:
    case ActionType::MousePlayback:
        return true;
    default:
        return false;
    }
}

inline void RemapCopiedActionIndents(std::vector<ScriptAction>& copied, int destIndent) {
    if (copied.empty()) return;
    int srcMin = copied.front().indent;
    for (const auto& a : copied) srcMin = (std::min)(srcMin, a.indent);
    const int delta = destIndent - srcMin;
    for (auto& a : copied) {
        a.indent = (std::max)(0, a.indent + delta);
        a.originalNo = 0;
    }
}

struct DisassemblePatch {
    bool ok = false;
    std::wstring error;
    int replaceStart = 0;
    int replaceCount = 0;
    std::vector<ScriptAction> inserted;
};

using NestedScriptLoader = std::function<bool(
    const std::wstring& path, std::vector<ScriptAction>& out, std::wstring& err)>;

inline void ApplyDisassemblePatch(std::vector<ScriptAction>& actions, const DisassemblePatch& patch) {
    if (!patch.ok || patch.replaceStart < 0) return;
    const int start = patch.replaceStart;
    const int count = patch.replaceCount;
    if (start > static_cast<int>(actions.size())) return;
    const int end = (std::min)(start + count, static_cast<int>(actions.size()));
    actions.erase(actions.begin() + start, actions.begin() + end);
    actions.insert(actions.begin() + start, patch.inserted.begin(), patch.inserted.end());
}

inline DisassemblePatch FailDisassemble(const std::wstring& error) {
    DisassemblePatch p;
    p.error = error;
    return p;
}

inline int FindLastDefineBlockIndex(const std::vector<ScriptAction>& actions,
    const std::wstring& name) {
    int found = -1;
    if (name.empty()) return found;
    for (int i = 0; i < static_cast<int>(actions.size()); ++i) {
        if (actions[static_cast<size_t>(i)].type == ActionType::DefineBlock
            && actions[static_cast<size_t>(i)].blockName == name) {
            found = i;
        }
    }
    return found;
}

inline std::vector<ScriptAction> CopyContainerBody(
    const std::vector<ScriptAction>& actions, int containerIndex) {
    const int bodyEnd = ContainerBodyEnd(actions, containerIndex);
    std::vector<ScriptAction> body;
    if (bodyEnd <= containerIndex + 1) return body;
    body.assign(actions.begin() + containerIndex + 1, actions.begin() + bodyEnd);
    return body;
}

inline DisassemblePatch DisassembleActionAt(const std::vector<ScriptAction>& actions, int index,
    const NestedScriptLoader& loadNested) {
    if (index < 0 || index >= static_cast<int>(actions.size())) {
        return FailDisassemble(L"没有可拆解的动作");
    }
    const ScriptAction& target = actions[static_cast<size_t>(index)];
    if (!IsDisassemblableAction(target.type)) {
        return FailDisassemble(L"该动作不能拆解");
    }

    DisassemblePatch patch;
    patch.replaceStart = index;
    const int destIndent = (std::max)(0, target.indent);

    if (IsSubtreeContainer(target.type)) {
        auto body = CopyContainerBody(actions, index);
        if (body.empty()) {
            return FailDisassemble(L"没有可拆解的子动作");
        }
        RemapCopiedActionIndents(body, destIndent);
        patch.replaceCount = ContainerBodyEnd(actions, index) - index;
        patch.inserted = std::move(body);
        patch.ok = true;
        return patch;
    }

    if (target.type == ActionType::RunBlock) {
        const std::wstring name = target.blockName;
        if (name.empty()) {
            return FailDisassemble(L"请先选择要运行的宏指令块");
        }
        const int def = FindLastDefineBlockIndex(actions, name);
        if (def < 0) {
            return FailDisassemble(L"未找到宏指令块「" + name + L"」的定义");
        }
        auto body = CopyContainerBody(actions, def);
        if (body.empty()) {
            return FailDisassemble(L"宏指令块「" + name + L"」没有可拆解的动作");
        }
        RemapCopiedActionIndents(body, destIndent);
        patch.replaceCount = 1;
        patch.inserted = std::move(body);
        patch.ok = true;
        return patch;
    }

    // runMacro / mousePlayback：从目标文件拷一层动作；次数信息随运行动作丢弃
    const bool playback = target.type == ActionType::MousePlayback;
    if (target.targetPath.empty()) {
        return FailDisassemble(playback
            ? L"请先选择用于回放的键鼠录制"
            : L"请先选择要运行的鼠标宏");
    }
    if (!loadNested) {
        return FailDisassemble(playback
            ? L"找不到要拆解的录制"
            : L"找不到要拆解的鼠标宏");
    }
    std::vector<ScriptAction> nested;
    std::wstring loadErr;
    if (!loadNested(target.targetPath, nested, loadErr)) {
        if (!loadErr.empty()) return FailDisassemble(loadErr);
        return FailDisassemble(playback
            ? L"找不到要拆解的录制"
            : L"找不到要拆解的鼠标宏");
    }
    if (nested.empty()) {
        return FailDisassemble(playback
            ? L"录制没有可拆解的动作"
            : L"鼠标宏没有可拆解的动作");
    }
    RemapCopiedActionIndents(nested, destIndent);
    patch.replaceCount = 1;
    patch.inserted = std::move(nested);
    patch.ok = true;
    return patch;
}
