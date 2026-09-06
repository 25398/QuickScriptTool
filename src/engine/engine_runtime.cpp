// engine_runtime.cpp — headless Engine 运行时（热键/跑停/录制/连点/Reload）
// 产品门面：qst::engine（webview/qst_engine_host.h → engine/qst_engine.h）
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <fstream>
#include <functional>
#include <iomanip>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "msimg32.lib")

#include "action_tree.h"
#include "drawing.h"
#include "render_device.h"
#include "main_features.h"
#include "script_types.h"
#include "action_utils.h"
#include "config.h"
#include "process_utils.h"
#include "recorder.h"
#include "utils.h"
#include "ui_scale.h"

struct EditorControlLayout {
    UINT label;
    RECT rc;
    std::wstring hint;
    UINT buddy;
};

struct HotkeyMenuItem {
    int index;
    std::wstring name;
    Hotkey hotkey;
};

#include "engine/engine_host_window.h"
#include "input/input_emergency_teardown.h"
#include "webview/qst_engine_host.h"

namespace {

EngineHost* g_engine = nullptr;
bool g_renderInited = false;

void EmergencyUnhookPersistentLl() {
    if (ghHotkeyKbHook) {
        UnhookWindowsHookEx(ghHotkeyKbHook);
        ghHotkeyKbHook = nullptr;
    }
    if (ghHotkeyMouseHook) {
        UnhookWindowsHookEx(ghHotkeyMouseHook);
        ghHotkeyMouseHook = nullptr;
    }
    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
}

}  // namespace

namespace qst::engine {

bool Start(HINSTANCE inst) {
    (void)inst;
    if (g_engine) return g_engine->Hwnd() != nullptr;
    input_emergency::InstallProcessHandlers();
    input_emergency::RegisterExtraTeardown(EmergencyUnhookPersistentLl);
    if (!g_renderInited) {
        UiScaleInitFromPrimaryMonitor();
        InitRenderDevice();
        g_renderInited = true;
    }
    g_engine = new EngineHost();
    g_engine->SetHeadlessUi(true);
    if (!g_engine->Create()) {
        delete g_engine;
        g_engine = nullptr;
        return false;
    }
    g_engine->Show(SW_HIDE);
    // 巩固：headless 引擎窗禁止以任何 SW_SHOW* 露脸（产品 UI 仅 Web 壳）
    if (HWND hw = g_engine->Hwnd()) {
        ShowWindow(hw, SW_HIDE);
        SetWindowLongPtrW(hw, GWL_EXSTYLE,
            GetWindowLongPtrW(hw, GWL_EXSTYLE) | WS_EX_TOOLWINDOW);
    }
    // 启动后确保磁盘热键进 RegisterHotKey（Init 已 Load，此处再刷一次防漏）
    g_engine->EngineReloadScriptsAndHotkeys();
    return true;
}

void Shutdown() {
    input_emergency::TeardownNow();
    if (g_engine) {
        // DestroyWindow(headless) 会进 WM_DESTROY→TerminateProcess，须先落盘选中缓存
        try { g_engine->EngineSaveHomeState(); } catch (...) {}
        if (g_engine->Hwnd() && IsWindow(g_engine->Hwnd())) {
            DestroyWindow(g_engine->Hwnd());
        }
        delete g_engine;
        g_engine = nullptr;
    }
    if (g_renderInited) {
        ShutdownRenderDevice();
        g_renderInited = false;
    }
}

HWND Hwnd() {
    return g_engine ? g_engine->Hwnd() : nullptr;
}

bool RunScriptPath(const std::wstring& path, std::string& err) {
    if (!g_engine) {
        err = "engine not started";
        return false;
    }
    if (g_engine->EngineIsRunning()) {
        err = "busy";
        return false;
    }
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = "script not found";
        return false;
    }
    // WebView 桥在 UI 线程：同步启动，便于回传「无动作 / 窗口模式取消」等错误
    std::wstring werr;
    if (!g_engine->EngineRunFromPath(path, werr)) {
        err = ToUtf8(werr);
        if (err.empty()) err = "run failed";
        return false;
    }
    return true;
}

void StopScript() {
    if (!g_engine) return;
    PostMessageW(g_engine->Hwnd(), WM_APP_EXT_STOP_SCRIPT, 0, 0);
}

bool IsRunning() {
    return g_engine && g_engine->EngineIsRunning();
}

bool DebugRunActions(const std::vector<ScriptAction>& actions, int startIndex,
    bool stepMode, const std::vector<int>& breakpoints,
    const Hotkey& debugHotkey, const std::wstring& displayName,
    const windowmode::WindowModeScriptConfig& wmCfg, std::string& err) {
    if (!g_engine) {
        err = "engine not started";
        return false;
    }
    if (g_engine->EngineIsRunning()) {
        err = "busy";
        return false;
    }
    std::wstring werr;
    if (!g_engine->EngineDebugRunActions(actions, startIndex, stepMode, breakpoints,
            debugHotkey, displayName, wmCfg, werr)) {
        err = ToUtf8(werr.empty() ? L"debug start failed" : werr);
        return false;
    }
    return true;
}

bool IsDebugging() {
    return g_engine && g_engine->EngineIsDebugging();
}

bool DebugPaused() {
    return g_engine && g_engine->EngineDebugPaused();
}

bool DebugStepMode() {
    return g_engine && g_engine->EngineDebugStepMode();
}

int ExecutedSteps() {
    return g_engine ? g_engine->EngineExecutedSteps() : 0;
}

std::string RunningScriptNameUtf8() {
    if (!g_engine) return {};
    return ToUtf8(g_engine->EngineRunningScriptName());
}

int RunningMode() {
    if (!g_engine || !g_engine->EngineIsRunning()) return 0;
    return g_engine->EngineRunningMode();
}

bool RunningWindowMode(windowmode::WindowModeScriptConfig& out) {
    if (!g_engine || !g_engine->EngineIsRunning()) return false;
    return g_engine->EngineRunningWindowMode(out);
}

bool IsBreakoutPaused() {
    return g_engine && g_engine->EngineIsBreakoutPaused();
}

void StartClicker() {
    if (!g_engine) return;
    g_engine->EngineStartClicking();
}

void StopClicker() {
    if (!g_engine) return;
    g_engine->EngineStopClicking();
}

void ToggleClicker() {
    if (!g_engine) return;
    g_engine->EngineToggleClicker();
}

bool IsClicking() {
    return g_engine && g_engine->EngineIsClicking();
}

void ApplyClickerSettings(const quickscript::ClickerSettings& s) {
    if (!g_engine) return;
    g_engine->EngineApplyClickerSettings(s);
}

void StartRecording() {
    if (!g_engine) return;
    g_engine->EngineStartRecording();
}

void StopRecording() {
    if (!g_engine) return;
    g_engine->EngineStopRecording();
}

bool IsRecording() {
    return g_engine && g_engine->EngineIsRecording();
}

bool StartRecordingEx(std::string& errUtf8, int recorderWindowModeOverride) {
    errUtf8.clear();
    if (!g_engine) {
        errUtf8 = "engine not started";
        return false;
    }
    if (recorderWindowModeOverride >= 0) {
        g_engine->EngineApplyRecorderWindowMode(recorderWindowModeOverride != 0);
    }
    std::wstring werr;
    if (!g_engine->EngineStartRecordingEx(werr)) {
        errUtf8 = ToUtf8(werr);
        if (errUtf8.empty()) errUtf8 = "start recording failed";
        return false;
    }
    return true;
}

bool StopRecordingEx(std::string& savedPathUtf8, int& actionCount, std::string& errUtf8) {
    savedPathUtf8.clear();
    actionCount = 0;
    errUtf8.clear();
    if (!g_engine) {
        errUtf8 = "engine not started";
        return false;
    }
    std::wstring path, werr;
    if (!g_engine->EngineStopRecordingEx(path, actionCount, werr)) {
        errUtf8 = ToUtf8(werr);
        return false;
    }
    savedPathUtf8 = ToUtf8(path);
    return true;
}

void SetActiveHomeTab(int tab) {
    if (!g_engine) return;
    g_engine->EngineSetActiveHomeTab(tab);
}

void SetUiMode(int mode) {
    if (!g_engine) return;
    g_engine->EngineSetUiMode(mode);
}

void EnsureHotkeysArmed() {
    if (!g_engine) return;
    g_engine->EngineEnsureHotkeysArmed();
}

void SetTypingHotkeysMuted(bool muted, int source) {
    if (!g_engine) return;
    g_engine->EngineSetTypingHotkeysMuted(muted, source);
}

void SelectHomeItem(int tab, const std::wstring& path) {
    if (!g_engine) return;
    g_engine->EngineSelectHomeItem(tab, path);
}

std::string GetHomeStateJson() {
    if (!g_engine) {
        return "{\"activeTab\":0,\"selectedScriptPath\":\"\",\"selectedRecordingPath\":\"\"}";
    }
    return g_engine->EngineGetHomeStateJson();
}

void BeginHotkeyCaptureRelease(const Hotkey& editing) {
    if (!g_engine) return;
    g_engine->EngineBeginHotkeyCaptureRelease(editing);
}

void BeginActionKeyCaptureRelease() {
    if (!g_engine) return;
    g_engine->EngineBeginActionKeyCaptureRelease();
}

void DebugWindowClosedByUser() {
    if (!g_engine) return;
    g_engine->EngineDebugWindowClosedByUser();
}

void EndHotkeyCaptureRelease() {
    if (!g_engine) return;
    g_engine->EngineEndHotkeyCaptureRelease();
}

void ReloadSettings() {
    if (!g_engine) return;
    g_engine->EngineReloadSettings();
}

void ReloadScriptsAndHotkeys() {
    if (!g_engine) return;
    g_engine->EngineReloadScriptsAndHotkeys();
}

void OpenScheduledTasks() {
    if (!g_engine) return;
    g_engine->EngineOpenScheduledTasks();
}

bool CanOpenNativeScheduledTasks() {
    if (!g_engine) return false;
    return !g_engine->IsHeadlessUi();
}

void ReloadScheduledTasks() {
    if (!g_engine) return;
    g_engine->EngineReloadScheduledTasks();
}

void TouchScheduledIntervalClock(const std::wstring& id) {
    if (!g_engine || id.empty()) return;
    g_engine->EngineTouchScheduledIntervalClock(id);
}

void SetUiHost(HWND hwnd) {
    if (g_engine) g_engine->SetWebViewHostHwnd(hwnd);
}

std::wstring GlobalHotkeyText() {
    if (!g_engine) return L"F8";
    const Hotkey hk = g_engine->EngineGlobalHotkey();
    if (!hk.text.empty()) return hk.text;
    return L"F8";
}

void SetGlobalHotkey(const Hotkey& hk) {
    if (!g_engine) return;
    g_engine->EngineSetGlobalHotkey(hk);
}

Hotkey GetGlobalHotkey() {
    if (!g_engine) return Hotkey{0, VK_F8, L"F8", true};
    return g_engine->EngineGlobalHotkey();
}

bool HotkeyChordConflicts(UINT vk, UINT modifiers, const std::wstring& excludePath,
    bool excludeGlobal, std::wstring& errOut) {
    if (!g_engine) return false;
    return g_engine->EngineHotkeyChordConflicts(vk, modifiers, excludePath, excludeGlobal, errOut);
}

}  // namespace qst::engine
