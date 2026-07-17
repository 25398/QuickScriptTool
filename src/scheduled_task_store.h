#pragma once
// ──────────────────────────────────────────────────────────────────
// scheduled_task_store.h — 定时任务持久化
// ──────────────────────────────────────────────────────────────────

#include <vector>

#include "scheduled_task_types.h"

std::wstring ScheduledTasksFilePath();
/// 从 JSON 文本解析任务列表（自检可注入内容，不碰磁盘）。
bool ParseScheduledTasksJson(const std::wstring& content,
                             std::vector<ScheduledTask>& out,
                             bool* globalDisabled = nullptr);
bool LoadScheduledTasks(std::vector<ScheduledTask>& out, bool* globalDisabled = nullptr);
bool SaveScheduledTasks(const std::vector<ScheduledTask>& tasks, bool globalDisabled = false);
