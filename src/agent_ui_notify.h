#pragma once
// ──────────────────────────────────────────────────────────────────
// agent_ui_notify.h — AI 助手修改脚本/录制后通知主界面刷新
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

void SetAgentUiNotifyHwnd(HWND mainWindow);
void NotifyAgentScriptLibraryChanged();
