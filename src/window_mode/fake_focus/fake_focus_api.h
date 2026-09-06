#pragma once

// FakeFocus DLL exports — loaded into the target process.
// Build: FakeFocus32.dll / FakeFocus64.dll

#include <windows.h>

#ifdef FAKEFOCUS_EXPORTS
#define FAKEFOCUS_API extern "C" __declspec(dllexport)
#else
#define FAKEFOCUS_API extern "C" __declspec(dllimport)
#endif

/// Install hooks + subclass so the target process treats `targetTop` as focused.
/// Also opens soft-input shared memory (Phase 2) named by current PID.
FAKEFOCUS_API BOOL WINAPI FakeFocus_Install(HWND targetTop);

/// UE5 / GLFW 精简注入：不钩 PeekMessage/GetMessage、不发 Prime WM_ACTIVATE（避免冻 DXGI / 崩 Java）。
/// 默认仍钩 GetForegroundWindow / GetAsyncKeyState / GetCursorPos / GetRawInputData。
/// AIR（ApolloRuntime）只钩前景查询。
/// 冒险岛（MapleStoryClass）：IAT/指针扫描 + DirectInput 虚表（禁止 user32/win32u 方法体 JMP / 子类化 / RawInput）。
FAKEFOCUS_API BOOL WINAPI FakeFocus_InstallLite(HWND targetTop);

/// Update the HWND returned by GetForegroundWindow / GetActiveWindow / GetFocus.
FAKEFOCUS_API BOOL WINAPI FakeFocus_UpdateTarget(HWND targetTop);

/// Remove hooks and restore original WndProc. Safe to call when not installed.
FAKEFOCUS_API BOOL WINAPI FakeFocus_Uninstall(void);

/// Non-zero when hooks are active (for diagnostics / self-test).
FAKEFOCUS_API BOOL WINAPI FakeFocus_IsInstalled(void);

/// Phase 2: non-zero when soft-input shared memory is mapped.
FAKEFOCUS_API BOOL WINAPI FakeFocus_HasSoftInput(void);

/// 冒险岛诊断：低 16 位=轮询 API 槽数（GetCursorPos/GetAsyncKeyState/GetKeyState/GetKeyboardState/DirectInput8Create）；
/// 高 16 位=diag 位图（1=Cursor 2=AsyncKey 4=KeyState 8=KbState 10=DiCreate 20=DiState 40=Flash
/// 80=dinput已加载 100=Acquire 200=FgWnd 400=SetFg 800=GetDeviceState方法体JMP
/// 1000=user32/win32u方法体JMP 必须为0 2000=GetDeviceData方法体JMP 必须为0
/// 4000=GetDeviceData虚表槽 8000=软输入映射可读）。
/// 远程线程退出码即该值。低 16 位为 0 表示没挂上导入。
FAKEFOCUS_API DWORD WINAPI FakeFocus_MapleIatCount(HWND unused);

/// 冒险岛运行期钩命中：byte0=GetAsyncKeyState 次数 byte1=GetDeviceState 次数
/// byte2=GetDeviceData 次数 byte3=最近一次 GetDeviceState 的 cb（0=从未调用）。
FAKEFOCUS_API DWORD WINAPI FakeFocus_MapleHookHits(HWND unused);

/// WH_GETMESSAGE 钩子过程（供 SetWindowsHookEx 注入技术使用；本身不做事）。
FAKEFOCUS_API LRESULT CALLBACK FakeFocus_HookProc(
    int nCode, WPARAM wParam, LPARAM lParam);
