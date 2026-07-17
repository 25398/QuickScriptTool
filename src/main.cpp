// ──────────────────────────────────────────────────────────────────
// main.cpp — QuickScriptTool 应用程序入口
// ──────────────────────────────────────────────────────────────────

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
#include "popup_combo.h"
#include "script_types.h"

// g_instance 定义在全局作用域，供所有模块使用
HINSTANCE g_instance = nullptr;

// 模块化头文件 — 提供全局函数/类型声明
#include "action_utils.h"
#include "config.h"
#include "controls.h"
#include "process_utils.h"
#include "recorder.h"
#include "utils.h"
#include "ui_scale.h"

// ── 编辑器控件布局数据结构 ──────────────────────────────────────
struct EditorControlLayout {
    UINT label;
    RECT rc;
    std::wstring hint;
    UINT buddy;
};

// ── 热键菜单项数据结构 ──────────────────────────────────────────
struct HotkeyMenuItem {
    int index;
    std::wstring name;
    Hotkey hotkey;
};

// 主窗口类 — 在 main_window.h 中完整声明
#include "main_window.h"

// ──────────────────────────────────────────────────────────────────
// 应用程序入口
// ──────────────────────────────────────────────────────────────────
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (auto setCtx = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(GetModuleHandleW(L"user32"), "SetProcessDpiAwarenessContext"))) {
        setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    } else {
        SetProcessDPIAware();
    }
    HANDLE hMutex = nullptr;
    for (int attempt = 0; attempt < 40; ++attempt) {
        hMutex = CreateMutexW(nullptr, FALSE, L"QuickScriptTool_SingleInstance");
        if (hMutex && GetLastError() != ERROR_ALREADY_EXISTS) break;
        HWND existing = FindWindowW(L"QuickScriptToolWindow", nullptr);
        if (existing && IsWindow(existing)) {
            PostMessageW(existing, WM_APP_RESTORE_INSTANCE, 0, 0);
            if (hMutex) CloseHandle(hMutex);
            return 0;
        }
        if (hMutex) CloseHandle(hMutex);
        hMutex = nullptr;
        if (attempt == 8 || attempt == 20) {
            TerminateOtherInstancesOfCurrentExe();
        }
        Sleep(100);
    }
    if (!hMutex) {
        TerminateOtherInstancesOfCurrentExe();
        Sleep(300);
        hMutex = CreateMutexW(nullptr, FALSE, L"QuickScriptTool_SingleInstance");
        if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (hMutex) CloseHandle(hMutex);
            return 1;
        }
    }
    g_instance = hInstance;
    SetCurrentProcessExplicitAppUserModelID(L"ShuDaXia.MouseMacro");
    UiScaleInitFromPrimaryMonitor();
    InitRenderDevice();
    MainWindow window;
    if (!window.Create()) {
        ShutdownRenderDevice();
        CloseHandle(hMutex);
        return 1;
    }
    window.Show(nCmdShow);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_DISPLAYCHANGE && ghHotkeyHwnd) {
            PostMessageW(ghHotkeyHwnd, WM_APP_UI_SCALE_SYNC, 0, 1);
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ShutdownRenderDevice();
    CloseHandle(hMutex);
    // ExitProcess 可能卡在第三方 DLL 卸载；强制结束，避免幽灵进程占住 exe。
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(msg.wParam));
    return 0;
}
