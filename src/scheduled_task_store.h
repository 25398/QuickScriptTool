#pragma once
// ──────────────────────────────────────────────────────────────────
// scheduled_task_store.h — 定时任务持久化
// ──────────────────────────────────────────────────────────────────

#include <vector>

#include "scheduled_task_types.h"

std::wstring ScheduledTasksFilePath();
/// 自检专用：覆盖落盘路径。传空串恢复默认 AppDir\\scheduled_tasks.json。
/// 禁止自检写产品 scheduled_tasks.json（否则 UI 会出现 selftest / C:\\dummy\\script）。
void SetScheduledTasksFilePathForTest(const std::wstring& path);
/// 从 JSON 文本解析任务列表（自检可注入内容，不碰磁盘）。
bool ParseScheduledTasksJson(const std::wstring& content,
                             std::vector<ScheduledTask>& out,
                             bool* globalDisabled = nullptr);
bool LoadScheduledTasks(std::vector<ScheduledTask>& out, bool* globalDisabled = nullptr);
bool SaveScheduledTasks(const std::vector<ScheduledTask>& tasks, bool globalDisabled = false);
/// 脚本被拖进专业模式文件夹后，把仍指向旧路径的任务 filePath 改到 newPath。返回改写条数。
int RetargetScheduledTaskFilePaths(const std::wstring& oldPath, const std::wstring& newPath);
/// 文件夹重命名：把 filePath 落在 oldDir 子树内的任务改到 newDir。
int RetargetScheduledTaskFilePathPrefix(const std::wstring& oldDir, const std::wstring& newDir);
