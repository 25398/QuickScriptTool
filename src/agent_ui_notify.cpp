#include "agent_ui_notify.h"

#include "config.h"

namespace {

HWND g_mainWindow = nullptr;

}  // namespace

void SetAgentUiNotifyHwnd(HWND mainWindow) {
    g_mainWindow = mainWindow;
}

void NotifyAgentScriptLibraryChanged() {
    if (g_mainWindow && IsWindow(g_mainWindow))
        PostMessageW(g_mainWindow, WM_AGENT_SCRIPT_LIBRARY_CHANGED, 0, 0);
}
