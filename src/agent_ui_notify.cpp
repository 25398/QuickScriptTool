#include "agent_ui_notify.h"

#include "config.h"

#include <mutex>

namespace {

HWND g_mainWindow = nullptr;
std::mutex g_logicMu;
std::mutex g_touchMu;
std::wstring g_intervalTouchId;
std::function<void(const std::wstring&, const std::wstring&)> g_logicConvertNotify;

}  // namespace

void SetAgentUiNotifyHwnd(HWND mainWindow) {
    g_mainWindow = mainWindow;
}

void NotifyAgentScriptLibraryChanged() {
    if (g_mainWindow && IsWindow(g_mainWindow))
        PostMessageW(g_mainWindow, WM_AGENT_SCRIPT_LIBRARY_CHANGED, 0, 0);
}

void NotifyAgentScheduledTasksChanged(const std::wstring& touchIntervalId) {
    {
        std::lock_guard<std::mutex> lock(g_touchMu);
        g_intervalTouchId = touchIntervalId;
    }
    if (g_mainWindow && IsWindow(g_mainWindow))
        PostMessageW(g_mainWindow, WM_AGENT_SCRIPT_LIBRARY_CHANGED, 1, 0);
}

std::wstring ConsumeAgentIntervalTouchId() {
    std::lock_guard<std::mutex> lock(g_touchMu);
    std::wstring id;
    id.swap(g_intervalTouchId);
    return id;
}

void SetLogicConvertUiNotify(
    std::function<void(const std::wstring& path, const std::wstring& summary)> fn) {
    std::lock_guard<std::mutex> lock(g_logicMu);
    g_logicConvertNotify = std::move(fn);
}

void NotifyLogicConvertUi(const std::wstring& path, const std::wstring& summary) {
    std::function<void(const std::wstring&, const std::wstring&)> fn;
    {
        std::lock_guard<std::mutex> lock(g_logicMu);
        fn = g_logicConvertNotify;
    }
    if (fn) {
        fn(path, summary);
        return;
    }
    if (g_mainWindow && IsWindow(g_mainWindow)) {
        auto* payload = new std::pair<std::wstring, std::wstring>(path, summary);
        if (!PostMessageW(g_mainWindow, WM_APP_LOGIC_CONVERT_DONE, 0,
                reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    }
}
