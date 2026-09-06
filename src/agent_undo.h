#pragma once
// =============================================================================
// agent_undo.h - AI 助手变更日志 / 撤销恢复
// 每次助手工具修改文件前记录"修改前内容"，用户可随时恢复为修改前状态。
// =============================================================================

#include <string>
#include <vector>

struct AgentChangeEntry {
    std::wstring id;              // 变更唯一 id
    std::wstring time;            // 修改时间（NowText）
    std::wstring tool;            // 触发工具名：writeScript / updateSettings ...
    std::wstring title;           // 人类可读标题
    std::wstring path;            // 受影响文件（绝对路径）
    bool existed = false;         // 修改前文件是否存在
    std::wstring before;          // 修改前内容（UTF-8 文本；existed=false 时为空）
    std::wstring after;           // 修改后内容（成功且可读时）
    bool ok = false;              // 修改是否成功
    bool reverted = false;        // 是否已恢复
    std::wstring conversationId;  // 会话 id（可为空）
};

/// 变更日志目录：AppDir()\agent_changes
std::wstring AgentChangesDir();
std::wstring AgentChangeJournalPath();

/// 开始一次可撤销修改；返回变更 id（失败返回空串）。
/// beforeContent 为修改前文件内容；existed=false 表示修改前文件不存在。
std::wstring AgentUndoBegin(const std::wstring& tool,
                            const std::wstring& title,
                            const std::wstring& path,
                            const std::wstring& beforeContent,
                            bool existed,
                            const std::wstring& conversationId = L"");

/// 修改结束后回填：afterContent 为修改后内容（可空），ok 表示是否成功。
void AgentUndoFinish(const std::wstring& id,
                     const std::wstring& afterContent,
                     bool ok);

/// 读取变更日志（最新在前），maxEntries 截断。
bool LoadAgentChanges(std::vector<AgentChangeEntry>& out, size_t maxEntries = 100);

/// 恢复指定变更；成功返回 true 并写入 err 为空。
bool RevertAgentChange(const std::wstring& id, std::wstring& err);

/// 最近变更的纯文本摘要（供 AI 工具返回）。
std::wstring ListAgentChangesText(size_t maxEntries = 50);

/// 最近变更的 JSON 数组字符串（供前端展示）。
std::wstring AgentChangesToJson(size_t maxEntries = 50);
