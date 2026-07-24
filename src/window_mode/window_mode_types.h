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

/// 窗口模式输入策略。
enum class WindowModeInputStrategy {
    Auto = 0,         ///< Chrome/Edge 类名 → CDP/扩展；其它 → softMessage
    SoftMessage = 1,  ///< Win32 PostMessage / 假焦点
    Cdp = 2,          ///< 配套扩展（优先）或 CDP
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

    /// 假焦点实验开关（进程注入）；CDP 策略下忽略。
    bool fakeFocusEnabled = false;
    WindowModeInputStrategy inputStrategy = WindowModeInputStrategy::Auto;
    int cdpPort = 9222;
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

bool LooksLikeChromiumBrowserClass(const std::wstring& className);
WindowModeInputStrategy ResolveInputStrategy(const WindowModeScriptConfig& config);
void AnnotateInputStrategyForSave(WindowModeScriptConfig& config);
std::wstring EnsureRemoteDebuggingLaunchArgs(const std::wstring& args, int port);

inline bool UsesCdpInput(const WindowModeScriptConfig& config) {
    return config.enabled
        && ResolveInputStrategy(config) == WindowModeInputStrategy::Cdp;
}

/// 假焦点仅在「窗口模式 + 开关 + 非 CDP」下启用；后台窗口模式本阶段不启用。
inline bool UsesFakeFocus(const WindowModeScriptConfig& config) {
    return config.enabled
        && config.fakeFocusEnabled
        && config.executionKind == WindowModeExecutionKind::HiddenDesktop
        && !UsesCdpInput(config);
}

/// CDP：绑窗后由 Park 自行 Minimize→Move 宏桌面（勿再二次最小化）；假焦点保持可见。
inline bool ShouldMinimizeTargetAfterBind(const WindowModeScriptConfig& config) {
    return !UsesFakeFocus(config) && !UsesCdpInput(config);
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
