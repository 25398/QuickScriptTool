// ──────────────────────────────────────────────────────────────────
// main.cpp — QuickScriptTool 应用程序入口
// ──────────────────────────────────────────────────────────────────

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

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
#include "main_features.h"
#include "popup_combo.h"
#include "script_types.h"

// g_instance 定义在全局作用域，供所有模块使用
HINSTANCE g_instance = nullptr;

// 模块化头文件 — 提供全局函数/类型声明
#include "action_utils.h"
#include "config.h"
#include "controls.h"
#include "recorder.h"
#include "utils.h"

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
    SetProcessDPIAware();
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, L"QuickScriptTool_SingleInstance");
    if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }
    g_instance = hInstance;
    MainWindow window;
    if (!window.Create()) { CloseHandle(hMutex); return 1; }
    window.Show(nCmdShow);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(hMutex);
    return static_cast<int>(msg.wParam);
}
