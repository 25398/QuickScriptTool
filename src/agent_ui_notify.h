#pragma once
// ──────────────────────────────────────────────────────────────────
// agent_ui_notify.h — AI 助手修改脚本/录制后通知主界面刷新
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <functional>
#include <string>

void SetAgentUiNotifyHwnd(HWND mainWindow);
void NotifyAgentScriptLibraryChanged();
/// 定时任务 CRUD 后通知主壳 Reload；intervalTouchId 非空时再 Touch 间隔时钟。
void NotifyAgentScheduledTasksChanged(const std::wstring& touchIntervalId = {});
std::wstring ConsumeAgentIntervalTouchId();

/// 逻辑转化写回后：Shell 可注册以 PostToJs / 刷新编辑器
void SetLogicConvertUiNotify(
    std::function<void(const std::wstring& path, const std::wstring& summary)> fn);
void NotifyLogicConvertUi(const std::wstring& path, const std::wstring& summary);
