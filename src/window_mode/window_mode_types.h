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
    SelectOnStartup = 0,       ///< 启动时使用当前所在窗口（前台聚焦窗）
    MousePositionOnStartup = 1,  ///< 已合并入 SelectOnStartup；读旧脚本时规范化
    UseEditorWindowClass = 2,    ///< 使用宏编辑时获取到的窗口类名
    NoSelect = 3,                ///< 不选择窗口
};

inline WindowSelectMethod NormalizeSelectMethod(WindowSelectMethod method) {
    if (method == WindowSelectMethod::MousePositionOnStartup) {
        return WindowSelectMethod::SelectOnStartup;
    }
    return method;
}

/// GDI 下拉：0 当前所在窗口 / 1 指定窗口类 / 2 不选择。与 enum 数值不再 1:1。
inline int SelectMethodToComboIndex(WindowSelectMethod method) {
    switch (NormalizeSelectMethod(method)) {
    case WindowSelectMethod::UseEditorWindowClass: return 1;
    case WindowSelectMethod::NoSelect: return 2;
    default: return 0;
    }
}

inline WindowSelectMethod ComboIndexToSelectMethod(int sel) {
    switch (sel) {
    case 1: return WindowSelectMethod::UseEditorWindowClass;
    case 2: return WindowSelectMethod::NoSelect;
    default: return WindowSelectMethod::SelectOnStartup;
    }
}

/// JSON 令牌 → 枚举。selectOnStartup / 旧值 mousePositionOnStartup / 缺省 / 未知 → 当前所在窗口。
inline WindowSelectMethod SelectMethodFromJson(const std::wstring& text) {
    if (text == L"useEditorWindowClass") return WindowSelectMethod::UseEditorWindowClass;
    if (text == L"noSelect") return WindowSelectMethod::NoSelect;
    return WindowSelectMethod::SelectOnStartup;
}

inline WindowSelectMethod SelectMethodFromJsonUtf8(const std::string& text) {
    if (text == "useEditorWindowClass") return WindowSelectMethod::UseEditorWindowClass;
    if (text == "noSelect") return WindowSelectMethod::NoSelect;
    return WindowSelectMethod::SelectOnStartup;
}

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
    // 脚本坐标默认是屏幕绝对坐标（编辑器/录制均存屏幕坐标）。
    // 窗口相对坐标仅由「窗口模式录制」等显式写入 windowClient 才启用。
    WindowModeCoordinateSpace coordSpace = WindowModeCoordinateSpace::ScreenAbsolute;
    /// 窗口相对坐标标记：仅「窗口模式录制」写入（1 = 动作坐标已是目标窗口客户区，
    /// 执行时不再做屏幕→客户区映射）。历史脚本被旧默认误标 windowClient 但存的是
    /// 屏幕坐标，因此不能用 coordSpace 单独判断。
    bool windowRelativeCoordinates = false;
    /// 窗口相对录制时的目标客户区宽高；回放按当前客户区等比缩放。0 = 旧脚本未记录。
    int recordClientWidth = 0;
    int recordClientHeight = 0;
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

/// 「启动时使用当前所在窗口」身份在运行时取前台窗，不得把上次拾取的类名/路径写进脚本。
/// 窗口相对录制除外：enabled=0 时身份要留给 FinalizeWindowModeForPlayback 复活。
inline void StripRuntimeOnlySelectTarget(WindowModeScriptConfig& cfg) {
    if (NormalizeSelectMethod(cfg.selectMethod) != WindowSelectMethod::SelectOnStartup) {
        return;
    }
    if (cfg.windowRelativeCoordinates) return;
    cfg.selectMethod = WindowSelectMethod::SelectOnStartup;
    cfg.windowName.clear();
    cfg.windowClassName.clear();
    cfg.childWindowClassName.clear();
    cfg.targetWindowTitle.clear();
    cfg.targetPickX = 0;
    cfg.targetPickY = 0;
    cfg.targetExePath.clear();
    cfg.launchArgs.clear();
    cfg.autoLaunchTarget = false;
}

bool LooksLikeChromiumBrowserClass(const std::wstring& className);
/// 真浏览器进程（msedge/chrome 等）。Electron/CEF 壳（QQ/微信/Discord/VS Code）同为 Chrome_WidgetWin，但不能走配套扩展。
bool LooksLikeChromiumBrowserExecutable(const std::wstring& exePath);
/// 类名为 Chromium 且（无 exe 或 exe 为真浏览器）→ 可走扩展桥 / CDP。
bool ConfigLooksLikeExtBridgeBrowser(const WindowModeScriptConfig& config);
/// Chromium 壳（Electron/CEF/QQNT 等：Chrome_WidgetWin + 非浏览器 exe）。
/// 与 ConfigLooksLikeElectronShell 同义（历史名保留）。
bool ConfigLooksLikeElectronShell(const WindowModeScriptConfig& config);
/// 已绑定 HWND：按直播类名+进程图判定 Chromium 壳（config.exe 为空时也能识别）。
bool HwndLooksLikeChromiumShell(HWND hwnd);
/// config 或 hwnd 任一判定为 Chromium 壳。
bool LooksLikeChromiumShellTarget(const WindowModeScriptConfig& config, HWND hwnd);
/// Adobe AIR（造梦西游等）：窗口类 ApolloRuntimeContentWindow，PostMessage 不够，需假焦点。
bool LooksLikeAdobeAirWindowClass(const std::wstring& className);
/// Unity / Unreal / SDL / GLFW / Godot / AIR / 传奇 Delphi(TFrmMain) / 冒险岛 MapleStoryClass 等：PostMessage 不够，需假焦点注入。
bool LooksLikeGameWindowClass(const std::wstring& className);
/// 冒险岛客户端：窗口类 MapleStoryClass；exe MapleStory.exe；标题含 MapleStory/冒险岛。
bool LooksLikeMapleStoryWindowClass(const std::wstring& className);
bool LooksLikeMapleStoryExecutable(const std::wstring& exePath);
bool LooksLikeMapleStoryTitle(const std::wstring& title);
/// 英雄联盟 / Valorant 等 Riot Vanguard 内核反作弊：后台窗口无法注入、PostMessage 无效。
bool LooksLikeKernelAntiCheatToken(const std::wstring& text);
bool LooksLikeKernelAntiCheatProtectedTarget(const WindowModeScriptConfig& config, HWND hwnd = nullptr);
const wchar_t* KernelAntiCheatBackgroundUnsupportedHint();
/// GLFW / SDL 原生窗：禁止在游戏线程 SetWindowsHook（Java《我的世界》会崩），假焦点用精简 RawInput。
bool LooksLikeGlfwOrSdlWindowClass(const std::wstring& className);
/// 传奇私服等 Delphi VCL 主窗/场景（TFrmMain / TPlayScene / TDXDraw）。不含 TForm1、不含 VB6 默认类。
bool LooksLikeDelphiVclGameWindowClass(const std::wstring& className);
/// 顶层类名或子孙控件（TDXDraw/TPlayScene）像传奇引擎时为 true。用于 TForm1 壳套 DirectX 子窗。
bool HwndLooksLikeDelphiVclGame(HWND hwnd);
/// DeSmuME / Dolphin 等桌面模拟器：轮询 GetAsyncKeyState/DirectInput，外层 WM_KEY* 无效 → 假焦点。
bool LooksLikeEmulatorWindowClass(const std::wstring& className);
bool LooksLikeEmulatorExecutable(const std::wstring& exePath);
/// 雷电/LDPlayer/蓝叠等安卓模拟器：PostMessage 到渲染子窗即可，勿假焦点注入。
bool LooksLikeAndroidEmulatorWindowClass(const std::wstring& className);
bool LooksLikeAndroidEmulatorExecutable(const std::wstring& exePath);
/// 窗口标题含 MuMu/雷电 等安卓壳特征。
bool LooksLikeAndroidEmulatorWindowTitle(const std::wstring& title);
/// MuMu 等 Qt 壳渲染子窗：Qt5156QWindowIcon / Qt5QWindowIcon。
bool LooksLikeQtRenderWindowClass(const std::wstring& className);
/// 配置或已绑定 HWND 是否为桌面模拟器（非安卓壳）。
bool LooksLikeEmulatorTarget(const WindowModeScriptConfig& config, HWND hwnd = nullptr);
/// 仅凭脚本配置判断是否为模拟器（MuMu/DeSmuME 等）；用于禁止宏桌面搬窗。
bool ConfigLooksLikeEmulatorTarget(const WindowModeScriptConfig& config);
/// UE5 `UnrealWindow`：完整假焦点钩 PeekMessage 会冻 DXGI；窗口化改精简注入（不钩消息泵）。
bool LooksLikeUnrealEngineWindowClass(const std::wstring& className);
/// Windows 远程桌面客户端窗口类（TscShellContainerClass / IHWindowClass 等）。
bool LooksLikeRemoteDesktopWindowClass(const std::wstring& className);
/// 目标 exe 是否为 mstsc.exe。
bool LooksLikeRemoteDesktopExePath(const std::wstring& exePath);
WindowModeInputStrategy ResolveInputStrategy(const WindowModeScriptConfig& config);
void AnnotateInputStrategyForSave(WindowModeScriptConfig& config);
std::wstring EnsureRemoteDebuggingLaunchArgs(const std::wstring& args, int port);

inline bool UsesCdpInput(const WindowModeScriptConfig& config) {
    return config.enabled
        && ResolveInputStrategy(config) == WindowModeInputStrategy::Cdp;
}

/// 假焦点：开关打开，或游戏类名自动启用（宏桌面与后台窗口均可；CDP 除外）。
/// Electron 壳自动启用：配合本机 SendInput 欺骗 GetForegroundWindow 等查询。
inline bool UsesFakeFocus(const WindowModeScriptConfig& config) {
    if (!config.enabled || UsesCdpInput(config)) return false;
    if (config.executionKind != WindowModeExecutionKind::HiddenDesktop
        && config.executionKind != WindowModeExecutionKind::BackgroundWindow) {
        return false;
    }
    if (config.fakeFocusEnabled) return true;
    if (ConfigLooksLikeElectronShell(config)) return true;
    return LooksLikeGameWindowClass(config.windowClassName)
        || LooksLikeGameWindowClass(config.childWindowClassName)
        || LooksLikeMapleStoryExecutable(config.targetExePath)
        || LooksLikeMapleStoryTitle(config.windowName)
        || LooksLikeMapleStoryTitle(config.targetWindowTitle)
        || LooksLikeEmulatorWindowClass(config.windowClassName)
        || LooksLikeEmulatorWindowClass(config.childWindowClassName)
        || LooksLikeEmulatorExecutable(config.targetExePath);
}

/// 配置类名为空时，再用已绑定 HWND 的类名判断（录制拾取后类名可能只在窗口上）。
bool UsesFakeFocusForTarget(const WindowModeScriptConfig& config, HWND hwnd);
/// UsesFakeFocus 或 MuMu 等无 TheRender 的 Qt 安卓壳（须假焦点键鼠）。
bool UsesFakeFocusOrAndroidQt(const WindowModeScriptConfig& config, HWND hwnd = nullptr);
/// 窗口模式：游戏/UE5 等 Raw Input 目标在未注入假焦点时必须假前台 SendInput。
/// PostMessage 进不了 Unreal/Unity；安卓壳与 Chromium 壳除外（另有路径）。
bool GameTargetNeedsHardwareWithoutFakeFocus(const WindowModeScriptConfig& config, HWND hwnd);

/// CDP：绑窗后由 Park 自行 Minimize→Move 宏桌面（勿再二次最小化）；假焦点保持可见。
/// hwnd 非空时按直播窗口再判一次（配置类名为空、TForm1+TDXDraw 子窗）。
inline bool ShouldMinimizeTargetAfterBind(const WindowModeScriptConfig& config, HWND hwnd = nullptr) {
    if (LooksLikeRemoteDesktopWindowClass(config.windowClassName)
        || LooksLikeRemoteDesktopWindowClass(config.childWindowClassName)
        || LooksLikeRemoteDesktopExePath(config.targetExePath)) {
        return false;
    }
    // Chromium 壳须可见以便焦点欺骗 + 进程内灌入；绑后最小化会导致键鼠无效。
    // 禁止对 Chromium 安静 ShowWindow「还原」——只会得到空白合成窗（已证伪）。
    if (ConfigLooksLikeElectronShell(config)) return false;
    if (UsesFakeFocus(config) || UsesCdpInput(config)) return false;
    if (hwnd && UsesFakeFocusForTarget(config, hwnd)) return false;
    return true;
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
