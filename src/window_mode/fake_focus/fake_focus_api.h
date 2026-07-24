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

/// Update the HWND returned by GetForegroundWindow / GetActiveWindow / GetFocus.
FAKEFOCUS_API BOOL WINAPI FakeFocus_UpdateTarget(HWND targetTop);

/// Remove hooks and restore original WndProc. Safe to call when not installed.
FAKEFOCUS_API BOOL WINAPI FakeFocus_Uninstall(void);

/// Non-zero when hooks are active (for diagnostics / self-test).
FAKEFOCUS_API BOOL WINAPI FakeFocus_IsInstalled(void);

/// Phase 2: non-zero when soft-input shared memory is mapped.
FAKEFOCUS_API BOOL WINAPI FakeFocus_HasSoftInput(void);
