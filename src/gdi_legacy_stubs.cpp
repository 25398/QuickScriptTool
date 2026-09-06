// gdi_legacy_stubs.cpp — 旧 GDI 对话框空实现（EngineHost 头仍声明符号；产品 UI 在 Web）
// 真实 archive/gdi_legacy 与 QuickScriptTool.Gdi 已移除。

#include "agent_dialog.h"
#include "ocr_install_dialog.h"
#include "recording_optimize_dialog.h"
#include "scheduled_task_datetime_picker.h"
#include "scheduled_task_dialog.h"
#include "settings_dialog.h"
#include "theme_custom_dialog.h"
#include "window_mode/window_pick_dialog.h"

HWND RecordingOptimizeDialog::s_activeHwnd_ = nullptr;

ScheduledDateTimePicker::~ScheduledDateTimePicker() = default;
void ScheduledDateTimePicker::Attach(HWND) {}
void ScheduledDateTimePicker::Detach() {}
bool ScheduledDateTimePicker::Visible() const { return false; }
bool ScheduledDateTimePicker::Toggle(const RECT&, Mode, ScheduledTaskTime&) { return false; }
void ScheduledDateTimePicker::Hide() {}
void ScheduledDateTimePicker::SyncPosition() {}
bool ScheduledDateTimePicker::ConsumeAccepted() { return false; }
LRESULT CALLBACK ScheduledDateTimePicker::WndProc(HWND, UINT, WPARAM, LPARAM) { return 0; }

SettingsDialog::SettingsDialog() = default;
SettingsDialog::~SettingsDialog() = default;

bool SettingsDialog::Show(HWND, quickscript::AppSettings&, SavedCallback) {
    return false;
}

HWND SettingsDialog::ActiveHwnd() { return nullptr; }

void NotifyActiveSettingsDialogSync() {}
void NotifyActiveSettingsDialogRelayout() {}

AgentDialog::AgentDialog() = default;
AgentDialog::~AgentDialog() = default;

bool AgentDialog::Show(HWND, const quickscript::AiApiSettings&,
    const RestoreData*, CloseCallback) {
    return false;
}

void AgentDialog::ApplyDpiLayout() {}

void ScheduledTaskDialog::Show(HWND, ScheduledTaskScheduler&) {}

RecordingOptimizeDialog::Result RecordingOptimizeDialog::Show(HWND, const ScriptMeta&) {
    return {};
}

HWND RecordingOptimizeDialog::ActiveHwnd() { return nullptr; }

bool OcrInstallDialog::Show(HWND, bool) { return false; }

namespace quickscript {
bool ShowCustomThemePicker(HWND, COLORREF&, COLORREF&) { return false; }
}  // namespace quickscript

namespace windowmode {
bool WindowPickDialog::Show(HWND, WindowPickResult&) { return false; }
}  // namespace windowmode
