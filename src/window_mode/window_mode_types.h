#pragma once

#include "window_mode_requirements.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace windowmode {

/// Visible virtual desktop name shown in Windows Task View.
constexpr wchar_t kMacroDesktopDisplayName[] = L"鼠标宏";
/// Legacy Win32 hidden desktop name (no longer used for window mode).
constexpr wchar_t kMacroDesktopName[] = L"QuickScriptMacroDesktop";

enum class WindowModeHealth {
    Ok,
    Unknown,
    DesktopNotReady,
    TargetNotFound,
    TargetMinimized,
    TargetNoRender,
    CaptureFailed,
    PermissionMismatch,
};

enum class WindowModeCoordinateSpace {
    ScreenAbsolute,
    WindowClient,
};

/// 窗口选择方式（与编辑界面下拉一致）
enum class WindowSelectMethod {
    SelectOnStartup = 0,       ///< 启动时选择窗口
    MousePositionOnStartup = 1,  ///< 启动时获取鼠标位置的窗口
    UseEditorWindowClass = 2,    ///< 使用宏编辑时获取到的窗口类名
    NoSelect = 3,                ///< 不选择窗口
};

enum class WindowModeExecutionKind {
    HiddenDesktop,    ///< 窗口模式：独立宏桌面
    BackgroundWindow, ///< 后台窗口模式：用户桌面上已打开的窗口
};

struct WindowModeScriptConfig {
    bool enabled = false;
    WindowModeExecutionKind executionKind = WindowModeExecutionKind::HiddenDesktop;
    std::wstring targetExePath;
    std::wstring targetWindowTitle;
    WindowModeCoordinateSpace coordSpace = WindowModeCoordinateSpace::WindowClient;
    bool autoLaunchTarget = false;
    std::wstring launchArgs;

    WindowSelectMethod selectMethod = WindowSelectMethod::SelectOnStartup;
    std::wstring windowName;
    std::wstring windowClassName;
    std::wstring childWindowClassName;
    bool useTopLevelWindow = true;
    int targetPickX = 0;
    int targetPickY = 0;
    bool allowForegroundInputFallback = false;
};

/// 「不选择窗口」「指定窗口类」：有目标路径时窗口不存在应自动启动。
inline bool ShouldAutoLaunchTarget(const WindowModeScriptConfig& config) {
    if (!config.enabled || config.targetExePath.empty()) return false;
    if (config.selectMethod == WindowSelectMethod::NoSelect
        || config.selectMethod == WindowSelectMethod::UseEditorWindowClass) {
        return true;
    }
    return config.autoLaunchTarget;
}

struct WindowModeSessionState {
    GUID macroDesktopId{};
    int macroDesktopIndex = -1;
    HWND targetHwnd = nullptr;
    DWORD targetPid = 0;

    RECT clientRectScreen = {};
    int clientW = 0;
    int clientH = 0;

    WindowModeHealth health = WindowModeHealth::Unknown;
    std::wstring lastError;
};

const wchar_t* HealthToDisplayText(WindowModeHealth health);
const wchar_t* HealthToUserHint(WindowModeHealth health);

inline bool WindowModeCancelled(const std::atomic_bool* flag) {
    return flag && flag->load(std::memory_order_relaxed);
}

inline void WindowModeSleepInterruptible(const std::atomic_bool* flag, std::chrono::milliseconds total) {
    const auto end = std::chrono::steady_clock::now() + total;
    while (std::chrono::steady_clock::now() < end) {
        if (WindowModeCancelled(flag)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}  // namespace windowmode
