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

/// 已找到目标但因权限/桌面失败：禁止当成「未找到」再自动打开 exe。
/// 冒险岛等客户端重复启动同一 exe 会直接把已开着的游戏搞崩。
inline bool ShouldAbortAutoLaunchOnBindFailure(WindowModeHealth health) {
    return health == WindowModeHealth::PermissionMismatch
        || health == WindowModeHealth::DesktopNotReady;
}

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
    HiddenDesktop,    ///< 独立桌面模式：独立宏桌面
    BackgroundWindow, ///< 后台窗口模式：用户桌面上已打开的窗口
};

/// 窗口/后台窗口模式输入策略。
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
    // 窗口相对坐标仅由「后台窗口模式录制」等显式写入 windowClient 才启用。
    WindowModeCoordinateSpace coordSpace = WindowModeCoordinateSpace::ScreenAbsolute;
    /// 窗口相对坐标标记：仅「后台窗口模式录制」写入（1 = 动作坐标已是目标窗口客户区，
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

    /// ⚠⚠ `windowName` 是否**只作参考**、不作硬性匹配门。
    ///
    /// 语义：为 true 时，窗口查找不再要求「实际标题必须包含 windowName 的 stem」，
    /// 身份判定退化为「进程路径 + 窗口类名（+ 子窗类名）」。
    ///
    /// 为什么需要它：**窗口相对录制**（`windowRelativeCoordinates`）由录制端自动写入
    /// `windowName = 录制瞬间的标题`（见 `SaveScriptFileData`），这不是用户表达的
    /// 「我要绑标题含 XX 的窗」，而只是「我当时在那个窗上录的」。
    /// 而标题是**易变量**：换文档 / 换标签页 / 游戏换场景 / 存档改名，标题就变了。
    /// 若把它当硬门（`EnumWindowsOnDesktopProc` 的 `TitleMatches`），回放会
    /// **枚举不到任何窗口** ⇒ 绑不到目标 ⇒ 表现为「后台窗口模式不操作后台」。
    ///
    /// 实测复现：`tools/verify/probe_record_playback_bind.py`
    /// （同一窗口、同一类名，仅标题变化 ⇒ 命中 0；摘掉标题过滤 ⇒ 命中 1）。
    ///
    /// 向后兼容：旧脚本无此字段 ⇒ 默认 false ⇒ 行为与历史一致（标题仍是硬门）。
    bool windowNameIsHintOnly = false;

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
/// 真浏览器进程（msedge/chrome 等）。Electron/CEF 壳（QQ/Discord/VS Code）同为 Chrome_WidgetWin，但不能走配套扩展。
/// 微信 4.x（Weixin.exe + Qt*QWindowIcon）不是 Chromium 壳。
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
/// Unity / Unreal / SDL / GLFW / Godot / AIR / 传奇 Delphi(TFrmMain) / 天龙八部 / 冒险岛 MapleStoryClass 等游戏类名。
/// 冒险岛识别仍走此类名；UsesFakeFocus 为 false（技能键 PostMessage），走路另走 mapleSafe 精简注入。
bool LooksLikeGameWindowClass(const std::wstring& className);
/// UWP 壳窗口/进程（2026-10-05）：类名 `ApplicationFrameWindow`、进程 `ApplicationFrameHost.exe`。
/// ⚠ 它是**系统壳进程** —— 一个进程托管**所有** UWP 应用（计算器 / 商店应用…）
///   ⇒ 往里注入 DLL 的影响面远超单个应用。**实测：会话结束后目标窗口崩溃**。
/// ★ UWP/WinUI 本来就不靠假焦点（`PostMessage` 对它无效，走 `background_uia_input` 的
///   UIA Invoke 兜底）⇒ 跳过假焦点注入**功能不受影响**，只去掉崩溃风险。
bool LooksLikeUwpShellWindowClass(const std::wstring& className);
bool LooksLikeUwpShellExecutable(const std::wstring& exePath);

/// ⚠⚠⚠ 2026-10-06：共享宿主进程（`explorer.exe` / `dllhost.exe` / `RuntimeBroker.exe` /
/// 系统关键进程 …）—— 注入会**连坐崩一大片**，一律不注入。判据是 exe 文件名。
/// ⚠ 取「宁可误判不注入」这一侧（误判只是该目标不能变速）。
bool LooksLikeSharedHostProcess(const std::wstring& exePath);
/// 新天龙八部：类名 `TianLongBaBuHJ WndClass`（含空格）。PostMessage 鼠标不够，须精简假焦点钩光标/键态。
bool LooksLikeTianLongBaBuWindowClass(const std::wstring& className);
bool LooksLikeTianLongBaBuExecutable(const std::wstring& exePath);
bool LooksLikeTianLongBaBuTitle(const std::wstring& title);
bool LooksLikeTianLongBaBuTarget(const WindowModeScriptConfig& config, HWND hwnd);
/// 冒险岛客户端：窗口类 MapleStoryClass；exe MapleStory.exe；标题含 MapleStory/冒险岛。
bool LooksLikeMapleStoryWindowClass(const std::wstring& className);
bool LooksLikeMapleStoryExecutable(const std::wstring& exePath);
bool LooksLikeMapleStoryTitle(const std::wstring& title);
/// 配置或直播 HWND 任一命中冒险岛。技能键走 LCA 窗口消息；走路须 mapleSafe 精简注入。
bool LooksLikeMapleStoryTarget(const WindowModeScriptConfig& config, HWND hwnd);
/// 后台走路：IAT 吞 WM_ACTIVATE + DirectInput 填键。UsesFakeFocus 仍为 false，禁止跳过 PostMessage。
bool MapleNeedsSafeFakeFocusLite(const WindowModeScriptConfig& config, HWND hwnd = nullptr);
/// 记事本/资源管理器等标准桌面程序：走 Edit 子控件 + WM_CHAR，不要当成游戏。
bool LooksLikeStandardDesktopAppClass(const std::wstring& className);
/// Unity/UE/GLFW/SDL/Godot/AIR/传奇/天龙八部：PostMessage 不够，须假焦点（不含冒险岛）。
bool LooksLikeInjectRequiredGameClass(const std::wstring& className);
/// 已知引擎需要注入假焦点（Unity/UE/GLFW/SDL/Godot/AIR/传奇/桌面模拟器/Chromium 壳/微信 4.x Qt）。
bool NeedsFakeFocusInjection(const WindowModeScriptConfig& config, HWND hwnd = nullptr);
/// 微信 PC 客户端：Weixin.exe / WeChat.exe（不含开发者工具）。
bool LooksLikeWeixinExecutable(const std::wstring& exePath);
/// 标题为「微信」或 WeChat/Weixin；「微信开发者工具」除外。
bool LooksLikeWeixinTitle(const std::wstring& title);
/// 配置或直播 HWND 命中微信 PC 客户端（4.x Qt 或 3.x）。Chromium 子窗仍由壳路径优先。
bool LooksLikeWeixinTarget(const WindowModeScriptConfig& config, HWND hwnd = nullptr);
/// 直播 HWND 像未登记的游戏窗（自定义类名、无 Edit）：对齐 LCA 后台一。
bool HwndPrefersLcaBackgroundMessages(HWND hwnd);
/// 冒险岛 + 未登记游戏：PostMessage KEY*、不发 WM_ACTIVATE。冒险岛仍可叠加 mapleSafe 精简注入。
bool PrefersLcaBackgroundMessages(const WindowModeScriptConfig& config, HWND hwnd = nullptr);
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
/// MuMu / 微信 4.x 等 Qt 顶层或渲染子窗：Qt51514QWindowIcon / Qt5QWindowIcon。
/// 单凭类名不能当安卓模拟器；须再看 exe/标题（微信 4.x 与 MuMu 同类名）。
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
/// 配置类名为空时（拖拽拾取常见）传 hwnd，会回头读已绑定窗口的真实类名 —— 见下方定义。
inline bool UsesFakeFocus(const WindowModeScriptConfig& config, HWND hwnd = nullptr);

/// 配置类名为空时，再用已绑定 HWND 的类名判断（录制拾取后类名可能只在窗口上）。
bool UsesFakeFocusForTarget(const WindowModeScriptConfig& config, HWND hwnd);
/// UsesFakeFocus 或 MuMu 等无 TheRender 的 Qt 安卓壳（须假焦点键鼠）。
bool UsesFakeFocusOrAndroidQt(const WindowModeScriptConfig& config, HWND hwnd = nullptr);
/// 窗口/后台窗口模式：游戏/UE5 等 Raw Input 目标在未注入假焦点时必须假前台 SendInput。
/// PostMessage 进不了 Unreal/Unity；安卓壳与 Chromium 壳除外（另有路径）。
bool GameTargetNeedsHardwareWithoutFakeFocus(const WindowModeScriptConfig& config, HWND hwnd);

/// 判定用的类名集合：**配置里有就用配置的，没有则必须回头问已绑定的 HWND**。
/// ⚠ 历史坏点（2026-09-20 修）：本函数原先只看 `config.windowClassName` /
/// `config.childWindowClassName`。绑定时拿到的真实类名（如 MC 的 `GLFW30`）**只用于记录，
/// 没参与判据** ⇒ `UsesFakeFocus==false` ⇒ `fakeFocusNeeded==false` ⇒ 接着落到
/// 「不需要假焦点就走 PostMessage」，**GLFW/RawInput 目标永远拿不到假焦点**。
/// 而 `PreferHardwareInput()` 里 `if (FakeFocusActive()) return false;` 排在游戏判据之前，
/// 于是软输入分支把每一步移动都变成一次跨进程 `WM_MOUSEMOVE`（对 GLFW 无效，纯延迟）。
inline bool UsesFakeFocus(const WindowModeScriptConfig& config, HWND hwnd) {
    if (!config.enabled || UsesCdpInput(config)) return false;
    if (config.executionKind != WindowModeExecutionKind::HiddenDesktop
        && config.executionKind != WindowModeExecutionKind::BackgroundWindow) {
        return false;
    }
    // 冒险岛 / 未登记游戏：LCA 后台窗口消息。UsesFakeFocus=false 以免注入失败后抢前台 SendInput。
    if (PrefersLcaBackgroundMessages(config, nullptr)) return false;
    if (config.fakeFocusEnabled) return true;
    if (ConfigLooksLikeElectronShell(config)) return true;
    if (LooksLikeWeixinTarget(config, nullptr)) return true;
    if (LooksLikeGameWindowClass(config.windowClassName)
        || LooksLikeGameWindowClass(config.childWindowClassName)
        || LooksLikeEmulatorWindowClass(config.windowClassName)
        || LooksLikeEmulatorWindowClass(config.childWindowClassName)
        || LooksLikeEmulatorExecutable(config.targetExePath)) {
        return true;
    }
    // 配置类名为空（拖拽拾取常见）：用已绑定 HWND 的真实类名再判一次。
    // 与 `UsesFakeFocusForTarget` 的「类名可能只在窗口上」是同一个坑，这里补齐。
    // ⚠ 只做**纯类名**判定：进程镜像路径要 `QueryHwndProcessImagePath`（cdp 子模块），
    //   在本底层头里不可用 —— 那一步留给 `UsesFakeFocusForTarget`（cpp，已含 cdp_input.h）。
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t hwndCls[256]{};
    GetClassNameW(hwnd, hwndCls, 256);
    return LooksLikeGameWindowClass(hwndCls) || LooksLikeEmulatorWindowClass(hwndCls);
}

/// `PrepareSoftInput` 快速路径的纯粹判据（抽出以便自检，且不依赖执行器私有状态）。
///
/// 为什么需要它：相对移动是**每拍一次**的最热路径（一段回放可上万包），而完整
/// `PrepareSoftInput` 每次都做 `RefreshInputBinding` —— 它会 `EnumChildWindows`
/// 全树找 RenderWidget/渲染子窗。对 GLFW/UE 这类**绑定恒为顶层**的目标纯属白跑，
/// 8ms 一拍根本喂不起，表现为 `late=` 成片 >1ms、`rebase` 猛涨、落点漂移。
///
/// 三个前提必须同时成立才允许走快速路径（任一不成立 ⇒ 退回完整路径）：
///   ① 目标**顶层**窗口仍然有效（`IsWindow` 廉价，能抓住闪退/重开）；
///   ② 当前绑定目标**就是顶层自身** —— 子窗绑定不适用，因为子控件往往**晚于顶层出现**，
///      正是 `RefreshInputBinding` 要反复处理的场景；
///   ③ 客户区几何未变 —— 尺寸变了说明窗口被缩放/还原/换 DPI，必须重算映射。
inline bool CanUseSoftInputFastPath(bool topAlive, bool boundIsTopLevel,
    bool haveCached, int cachedW, int cachedH, int liveW, int liveH) {
    if (!topAlive) return false;
    if (!boundIsTopLevel) return false;
    if (!haveCached) return false;
    if (cachedW <= 0 || cachedH <= 0) return false;
    return cachedW == liveW && cachedH == liveH;
}

/// 同上，外加「顶层窗类名未变」。
/// 为什么单看 HWND + 几何还不够：窗口句柄会被系统**复用** —— 关掉旧目标再开新目标
/// （或闪退重开）可能拿到同一个 HWND 值，且新旧客户区尺寸恰好相同。那时
/// ①②③ 全部成立，于是拿**旧绑定**去投递，输入落在错误的目标上。
/// 类名是目标的稳定身份字段，一次 `GetClassNameW` 即可排除这种误判。
inline bool CanUseSoftInputFastPathClass(bool topAlive, bool boundIsTopLevel,
    bool haveCached, bool classMatches, int cachedW, int cachedH, int liveW, int liveH) {
    if (!classMatches) return false;
    return CanUseSoftInputFastPath(topAlive, boundIsTopLevel, haveCached,
        cachedW, cachedH, liveW, liveH);
}

/// 不区分大小写的宽字符串相等（类名都是 ASCII；不为一处判据拉 `<wchar.h>`）。
inline bool WideEqualsNoCase(const wchar_t* a, const wchar_t* b) {
    if (!a || !b) return false;
    for (;; ++a, ++b) {
        wchar_t ca = *a;
        wchar_t cb = *b;
        if (ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca - L'A' + L'a');
        if (cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb - L'A' + L'a');
        if (ca != cb) return false;
        if (!ca) return true;
    }
}

/// WinUI（XAML）文本控件会**自己**把 `WM_KEYDOWN` 译成字符 —— 宿主再补一个 `WM_CHAR`
/// 就**一次变两次**。
///
/// 实测（本机 Windows 11 商店版记事本，`class=RichEditD2DPT`，2026-09-23）：
///   只 `KEYDOWN(A)` → `'a'`；只 `WM_CHAR('a')` → `'a'`；`KEYDOWN(A)+WM_CHAR('a')` → **`'aa'`**。
/// 它的自译走**真实键态**（脚本按住的 Shift 它看不见 ⇒ Shift+A 会退化成 `'a'`），
/// 所以可打印字符改由宿主发 `WM_CHAR`（宿主用软修饰键态译好字符）。
inline bool ClassSelfTranslatesPostedKeys(const wchar_t* cls) {
    return WideEqualsNoCase(cls, L"RichEditD2DPT");
}

/// 「自译目标」下该键是否走 `WM_CHAR`：只有**可打印字符**走。
/// `Enter`/`Tab`（`'\r'`/`'\t'` < 0x20）实测 `WM_CHAR` **不换行/不制表**；
/// 退格/删除/方向键/功能键更只能靠 `KEYDOWN`（`SoftVkToChar` 对它们返回 0）。
inline bool SelfTranslateKeyUsesWmChar(bool selfTranslate, wchar_t ch) {
    return selfTranslate && ch >= 0x20;
}

/// 只装时钟补丁（不装任何假焦点钩）的条件。
/// ⚠ 判据里**不能**出现「用户关掉了假焦点注入」这一项（2026-09-19 修，MC 实测踩到）：
/// 需要假焦点的目标若只装时钟补丁，引擎会回退「假前台 SendInput（绝对坐标）」——
/// 那是**会抢鼠标/键盘的假后台**，用户看到的就是「假后台」。
inline bool ShouldInjectTimeScaleOnly(bool timeScaleWanted, bool fakeFocusNeeded) {
    return timeScaleWanted && !fakeFocusNeeded;
}

/// 目标进程里挂着的注入模块是不是**旧内容**（同路径复用旧实例的隐患）。
///
/// 背景（2026-10-03 用户报障「冒险岛后台原地不动的平A，不能走A」）：
/// 用户升级/重建过软件，但游戏进程**没重启** ⇒ `TargetHasStaleFakeFocusModule` 只比**路径**
/// ⇒ 判「同一份文件」⇒ 复用已装实例（§14 说这条是安全路径）。可 `LoadLibrary` 同路径
/// **只加引用计数、不会重新执行 DllMain** ⇒ 进程里跑的还是**旧代码**。而共享内存结构
/// 一旦加过字段（本仓 `kSoftInputVersion` 从 7 一路抬到 10），旧 DLL 与新宿主就不兼容
/// ⇒ `SoftInputStateLooksValid` 失败 ⇒ DLL 静默关掉视图 ⇒ 软键态/DirectInput 全失效
/// （后台只剩 PostMessage 的攻击键）+ 诊断计数全 0（日志里注入行凭空消失）。
///
/// 判据：磁盘上该 DLL 的最后写入时间 **晚于** 目标进程的启动时间 ⇒ 进程内必然是旧内容。
/// ⚠ 任一为 0（拿不到进程启动时间 / 文件时间）⇒ 返回 false：**宁可漏报，不误报**
///   （误报会让用户被无谓地要求重启游戏）。
/// ⚠ 这里只报警、**不阻断**：同路径复用本身不会双重挂钩，阻断反而会牺牲可用性。
inline bool InjectedModuleLooksStale(ULONGLONG dllLastWriteFileTime,
    ULONGLONG targetProcessStartFileTime) {
    if (dllLastWriteFileTime == 0 || targetProcessStartFileTime == 0) return false;
    return dllLastWriteFileTime > targetProcessStartFileTime;
}

/// 哪些目标**禁止**用 `SetWindowsHook` 注入。
///
/// 理由：`SetWindowsHookEx` 是把 DLL **装进目标的 UI 线程**（在它自己的消息线程里
/// `LoadLibrary`）。下面这些目标对此零容忍，实测是当场崩或卡死退出：
/// GLFW/Java《我的世界》、DeSmuME/Dolphin 等桌面模拟器、微信 4.x Qt、Chromium 壳、
/// 原生 3D/游戏窗、Qt 安卓壳，以及 **Adobe AIR（造梦西游 / 4399 微端）** ——
/// AIR 的现场是「一启动就卡死退出，鼠标原地抽」。
/// 命中即必须降级成 `ClassicRemoteThread`：注入线程是我们自己起的，一个字节都不动
/// 目标的消息线程。
///
/// ⚠ 判据收在**这一处**：新增脆弱目标类型只改这里。别在 `TryInstallFakeFocus` 里
///   另散一份 —— 漏一个类型 = 那条目标被 `setwindowshook` 带走，而且**只在用户
///   把注入技术选成 setwindowshook 时才复现**（默认是 classic，日常测不出来）。
inline bool ForbidsSetWindowsHookTechnique(bool chromiumShell, bool weixinQt,
    bool androidQt, bool native3d, bool desktopEmu, bool adobeAir) {
    return chromiumShell || weixinQt || androidQt || native3d || desktopEmu || adobeAir;
}

/// 后台窗口模式 + 需要假焦点的 3D/游戏目标：**即使**用户关了「假焦点注入」也必须注入。
/// 理由与微信/冒险岛一致 —— 没有假焦点就没有真后台，只能退化成会抢鼠标的假前台 SendInput。
/// 非后台模式（前台回放）不走这条：那时用户本来就允许占键鼠，不该强行忽略设置。
inline bool BackgroundTargetRequiresFakeFocus(
    const WindowModeScriptConfig& config, HWND hwnd, bool enableFakeFocusInjection) {
    if (enableFakeFocusInjection) return false;
    if (config.executionKind != WindowModeExecutionKind::BackgroundWindow) return false;
    return GameTargetNeedsHardwareWithoutFakeFocus(config, hwnd);
}

/// CDP：绑窗后由 Park 自行 Minimize→Move 宏桌面（勿再二次最小化）；假焦点保持可见。
/// hwnd 非空时按直播窗口再判一次（配置类名为空、TForm1+TDXDraw 子窗）。
inline bool ShouldMinimizeTargetAfterBind(const WindowModeScriptConfig& config, HWND hwnd = nullptr) {
    if (PrefersLcaBackgroundMessages(config, hwnd)) return false;
    if (LooksLikeRemoteDesktopWindowClass(config.windowClassName)
        || LooksLikeRemoteDesktopWindowClass(config.childWindowClassName)
        || LooksLikeRemoteDesktopExePath(config.targetExePath)) {
        return false;
    }
    // Chromium 壳须可见以便焦点欺骗 + 进程内灌入；绑后最小化会导致键鼠无效。
    // 禁止对 Chromium 安静 ShowWindow「还原」——只会得到空白合成窗（已证伪）。
    // 微信 4.x Qt Quick 最小化同样停合成，绑后保持已还原可被遮挡。
    if (ConfigLooksLikeElectronShell(config)) return false;
    if (LooksLikeWeixinTarget(config, hwnd)) return false;
    if (UsesFakeFocus(config) || UsesCdpInput(config)) return false;
    if (hwnd && UsesFakeFocusForTarget(config, hwnd)) return false;
    return true;
}

struct WindowModeSessionState {
    GUID macroDesktopId{};
    int macroDesktopIndex = -1;
    HWND targetHwnd = nullptr;
    /// 顶层窗进程（UWP 计算器 = ApplicationFrameHost）。
    DWORD targetPid = 0;
    /// 输入绑定窗进程（UWP CoreWindow = CalculatorApp，可与 targetPid 不同）。
    DWORD bindPid = 0;

    RECT clientRectScreen = {};
    int clientW = 0;
    int clientH = 0;

    WindowModeHealth health = WindowModeHealth::Unknown;
    std::wstring lastError;
};

/// 存活判定：输入 HWND 的当前 PID 须对上绑定时的 bindPid。
/// 禁止用顶层 ApplicationFrameHost PID 去对 CoreWindow——UWP 分进程会被误判成闪退。
/// storedBindPid=0 时回退：同进程 Win32，或顶层仍是原宿主（旧会话）。
inline bool TargetBindPidStillMatches(DWORD storedBindPid, DWORD liveBindPid,
    DWORD storedTopPid, DWORD liveTopPid) {
    if (liveBindPid == 0) return true;
    if (storedBindPid != 0) return liveBindPid == storedBindPid;
    if (storedTopPid == 0) return true;
    if (liveBindPid == storedTopPid) return true;
    if (liveTopPid != 0 && liveTopPid == storedTopPid) return true;
    return false;
}

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
