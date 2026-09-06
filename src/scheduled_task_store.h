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
