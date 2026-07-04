#pragma once
// ──────────────────────────────────────────────────────────────────
// scheduled_task_store.h — 定时任务持久化
// ──────────────────────────────────────────────────────────────────

#include <vector>

#include "scheduled_task_types.h"

std::wstring ScheduledTasksFilePath();
bool LoadScheduledTasks(std::vector<ScheduledTask>& out, bool* globalDisabled = nullptr);
bool SaveScheduledTasks(const std::vector<ScheduledTask>& tasks, bool globalDisabled = false);
