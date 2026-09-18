#include "window_mode_types.h"
#include "background_input_target.h"
#include "window_target.h"
#include "cdp/cdp_input.h"

#include <algorithm>
#include <cctype>

namespace windowmode {

namespace {

std::wstring ToLowerCopy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

std::wstring ExeFileNameLower(const std::wstring& exePath) {
    const std::wstring lower = ToLowerCopy(exePath);
    const auto slash = lower.find_last_of(L"\\/");
    return slash == std::wstring::npos ? lower : lower.substr(slash + 1);
}

}  // namespace

bool LooksLikeChromiumBrowserClass(const std::wstring& className) {
    if (className.empty()) return false;
    const std::wstring lower = ToLowerCopy(className);
    // Chrome / Edge / Chromium / Electron / 部分 CEF。
    if (lower.find(L"chrome_widgetwin") != std::wstring::npos) return true;
    if (lower.find(L"chrome_renderwidgethosthwnd") != std::wstring::npos) return true;
    if (lower.rfind(L"chrome_", 0) == 0) return true;
    // CEF 常见顶层（网易云 / 部分桌面客户端）。
    if (lower.find(L"cefbrowserwindow") != std::wstring::npos) return true;
    if (lower == L"cefclientwindow") return true;
    return false;
}

bool LooksLikeChromiumBrowserExecutable(const std::wstring& exePath) {
    if (exePath.empty()) return false;
    const std::wstring name = ExeFileNameLower(exePath);
    // 仅真浏览器；QQ/Discord/VS Code/Spotify 等 Electron·CEF 勿误判。
    // 微信 4.x 是 Weixin.exe（Qt），也不是浏览器。
    return name == L"msedge.exe"
        || name == L"msedge_proxy.exe"
        || name == L"chrome.exe"
        || name == L"chromium.exe"
        || name == L"brave.exe"
        || name == L"opera.exe"
        || name == L"vivaldi.exe"
        || name == L"qqbrowser.exe"
        || name == L"360chrome.exe"
        || name == L"360chromex.exe";
}

bool ConfigLooksLikeExtBridgeBrowser(const WindowModeScriptConfig& config) {
    if (!LooksLikeChromiumBrowserClass(config.windowClassName)
        && !LooksLikeChromiumBrowserClass(config.childWindowClassName)) {
        return false;
    }
    // 有目标 exe 且不是真浏览器 → Chromium 壳，配套 Edge 扩展无法 attach。
    if (!config.targetExePath.empty()
        && !LooksLikeChromiumBrowserExecutable(config.targetExePath)) {
        return false;
    }
    return true;
}

bool ConfigLooksLikeElectronShell(const WindowModeScriptConfig& config) {
    if (!LooksLikeChromiumBrowserClass(config.windowClassName)
        && !LooksLikeChromiumBrowserClass(config.childWindowClassName)) {
        return false;
    }
    if (config.targetExePath.empty()) return false;
    return !LooksLikeChromiumBrowserExecutable(config.targetExePath);
}

bool HwndLooksLikeChromiumShell(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    HWND top = GetAncestor(hwnd, GA_ROOT);
    if (!top) top = hwnd;
    wchar_t cls[256]{};
    GetClassNameW(top, cls, 256);
    bool classOk = LooksLikeChromiumBrowserClass(cls);
    if (!classOk) {
        GetClassNameW(hwnd, cls, 256);
        classOk = LooksLikeChromiumBrowserClass(cls);
    }
    if (!classOk) {
        // 子树里有 RenderWidget 也视为 Chromium 嵌入（部分宿主顶层类名自定义）。
        struct Ctx { bool found = false; } ctx;
        EnumChildWindows(top, [](HWND w, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            wchar_t childCls[256]{};
            GetClassNameW(w, childCls, 256);
            if (LooksLikeChromiumBrowserClass(childCls)) {
                c->found = true;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        classOk = ctx.found;
    }
    if (!classOk) return false;
    const std::wstring path = QueryHwndProcessImagePath(top);
    if (path.empty()) return false;
    return !LooksLikeChromiumBrowserExecutable(path);
}

bool LooksLikeChromiumShellTarget(const WindowModeScriptConfig& config, HWND hwnd) {
    if (ConfigLooksLikeElectronShell(config)) return true;
    return HwndLooksLikeChromiumShell(hwnd);
}

bool LooksLikeUnrealEngineWindowClass(const std::wstring& className) {
    if (className.empty()) return false;
    const std::wstring lower = ToLowerCopy(className);
    if (lower == L"unrealwindow" || lower == L"unrealwindowwin") return true;
    // 常见变体：UnrealWindow / UnrealWindowWin / …UnrealWindow…
    if (lower.find(L"unrealwindow") != std::wstring::npos) return true;
    // UE3 / 枪神纪等：LaunchUnrealUWindowsClient（不含 UnrealWindow 子串）。
    return lower.find(L"launchunreal") != std::wstring::npos;
}

bool LooksLikeRemoteDesktopWindowClass(const std::wstring& className) {
    if (className.empty()) return false;
    const std::wstring lower = ToLowerCopy(className);
    // mstsc 顶层与常见子类（远程会话画面在这些窗上，不吃后台 PostMessage）。
    if (lower == L"tscshellcontainerclass") return true;
    if (lower == L"uimainclass") return true;
    if (lower == L"ihwindowclass") return true;
    if (lower.find(L"tscshell") != std::wstring::npos) return true;
    return false;
}

bool LooksLikeRemoteDesktopExePath(const std::wstring& exePath) {
    if (exePath.empty()) return false;
    const std::wstring lower = ToLowerCopy(exePath);
    const auto slash = lower.find_last_of(L"\\/");
    const std::wstring file = slash == std::wstring::npos ? lower : lower.substr(slash + 1);
    return file == L"mstsc.exe";
}

bool LooksLikeDelphiVclGameWindowClass(const std::wstring& className) {
    if (className.empty()) return false;
    const std::wstring lower = ToLowerCopy(className);
    // 传奇 / 热血类私服几乎都是 Delphi：TFrmMain、TFrmDlg、TFrmLogin、TPlayScene…
    // 不要用 TForm*（会误伤普通 Delphi 工具）；TFrame 是 tframe，不会命中 tfrm。
    // 不要用 ThunderRT6FormDC（所有 VB6 窗体的默认类，会误注入进销存等）。
    if (lower.compare(0, 4, L"tfrm") == 0) return true;
    if (lower == L"tformmain" || lower == L"tmainform") return true;
    if (lower.find(L"tplayscene") != std::wstring::npos) return true;
    if (lower.find(L"tdxdraw") != std::wstring::npos) return true;
    if (lower.find(L"tdxpaint") != std::wstring::npos) return true;
    if (lower.find(L"tdximage") != std::wstring::npos) return true;
    if (lower.find(L"tdxcontrol") != std::wstring::npos) return true;
    return false;
}

namespace {

BOOL CALLBACK EnumDelphiGameChildProc(HWND hwnd, LPARAM lp) {
    auto* found = reinterpret_cast<bool*>(lp);
    if (!hwnd || !IsWindow(hwnd)) return TRUE;
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    if (LooksLikeDelphiVclGameWindowClass(cls)) {
        *found = true;
        return FALSE;
    }
    return TRUE;
}

}  // namespace

bool HwndLooksLikeDelphiVclGame(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    HWND top = GetAncestor(hwnd, GA_ROOT);
    if (!top) top = hwnd;
    wchar_t cls[256]{};
    GetClassNameW(top, cls, 256);
    if (LooksLikeDelphiVclGameWindowClass(cls)) return true;
    bool found = false;
    EnumChildWindows(top, EnumDelphiGameChildProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

bool LooksLikeAdobeAirWindowClass(const std::wstring& className) {
    if (className.empty()) return false;
    const std::wstring lower = ToLowerCopy(className);
    if (lower.find(L"apolloruntime") != std::wstring::npos) return true;
    if (lower.find(L"adobeair") != std::wstring::npos) return true;
    return false;
}

bool LooksLikeMapleStoryWindowClass(const std::wstring& className) {
    if (className.empty()) return false;
    const std::wstring lower = ToLowerCopy(className);
    if (lower == L"maplestoryclass") return true;
    if (lower == L"maplestory") return true;
    return lower.find(L"maplestoryclass") != std::wstring::npos;
}

bool LooksLikeMapleStoryExecutable(const std::wstring& exePath) {
    if (exePath.empty()) return false;
    const std::wstring lower = ToLowerCopy(exePath);
    const auto slash = lower.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? lower : lower.substr(slash + 1);
    return name == L"maplestory.exe"
        || name == L"maplestoryt.exe"
        || (name.size() > 14 && name.rfind(L"maplestory", 0) == 0
            && name.size() >= 4 && name.compare(name.size() - 4, 4, L".exe") == 0);
}

bool LooksLikeMapleStoryTitle(const std::wstring& title) {
    if (title.empty()) return false;
    const std::wstring lower = ToLowerCopy(title);
    if (lower.find(L"maplestory") != std::wstring::npos) return true;
    return title.find(L"冒险岛") != std::wstring::npos;
}

bool LooksLikeMapleStoryTarget(const WindowModeScriptConfig& config, HWND hwnd) {
    if (LooksLikeMapleStoryWindowClass(config.windowClassName)
        || LooksLikeMapleStoryWindowClass(config.childWindowClassName)
        || LooksLikeMapleStoryExecutable(config.targetExePath)
        || LooksLikeMapleStoryTitle(config.windowName)
        || LooksLikeMapleStoryTitle(config.targetWindowTitle)) {
        return true;
    }
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    if (LooksLikeMapleStoryWindowClass(cls)) return true;
    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, 512);
    if (LooksLikeMapleStoryTitle(title)) return true;
    return LooksLikeMapleStoryExecutable(QueryHwndProcessImagePath(hwnd));
}

bool MapleNeedsSafeFakeFocusLite(const WindowModeScriptConfig& config, HWND hwnd) {
    return LooksLikeMapleStoryTarget(config, hwnd);
}

bool LooksLikeStandardDesktopAppClass(const std::wstring& className) {
    if (className.empty()) return false;
    const std::wstring lower = ToLowerCopy(className);
    if (lower == L"notepad" || lower == L"wordpadclass") return true;
    if (lower == L"cabinetwclass" || lower == L"explorewclass") return true;
    if (lower == L"#32770" || lower == L"consolewindowclass") return true;
    if (lower == L"applicationframewindow") return true;
    if (lower == L"edit" || lower == L"button" || lower == L"static") return true;
    if (lower == L"listbox" || lower == L"combobox" || lower == L"scrollbar") return true;
    return false;
}

bool LooksLikeInjectRequiredGameClass(const std::wstring& className) {
    if (className.empty()) return false;
    if (LooksLikeMapleStoryWindowClass(className)) return false;
    return LooksLikeGameWindowClass(className);
}

bool ClassLooksLikeLcaUnknownGame(const std::wstring& className) {
    if (className.empty()) return false;
    if (LooksLikeStandardDesktopAppClass(className)) return false;
    if (LooksLikeInjectRequiredGameClass(className)) return false;
    if (LooksLikeMapleStoryWindowClass(className)) return false;
    if (LooksLikeEmulatorWindowClass(className)) return false;
    if (LooksLikeAndroidEmulatorWindowClass(className)) return false;
    if (LooksLikeChromiumBrowserClass(className)) return false;
    if (LooksLikeQtRenderWindowClass(className)) return false;
    if (LooksLikeRemoteDesktopWindowClass(className)) return false;
    if (LooksLikeKernelAntiCheatToken(className)) return false;
    return true;
}

bool LooksLikeWeixinExecutable(const std::wstring& exePath) {
    if (exePath.empty()) return false;
    const std::wstring name = ExeFileNameLower(exePath);
    return name == L"weixin.exe" || name == L"wechat.exe";
}

bool LooksLikeWeixinTitle(const std::wstring& title) {
    if (title.empty()) return false;
    if (title.find(L"开发者工具") != std::wstring::npos) return false;
    const std::wstring lower = ToLowerCopy(title);
    if (lower.find(L"devtools") != std::wstring::npos) return false;
    if (title == L"微信") return true;
    if (title.size() >= 2 && title.compare(0, 2, L"微信") == 0) return true;
    return lower == L"wechat" || lower == L"weixin";
}

bool LooksLikeWeixinTarget(const WindowModeScriptConfig& config, HWND hwnd) {
    if (LooksLikeWeixinExecutable(config.targetExePath)) return true;
    if (LooksLikeWeixinTitle(config.windowName)
        || LooksLikeWeixinTitle(config.targetWindowTitle)) {
        return true;
    }
    if (!hwnd || !IsWindow(hwnd)) return false;
    HWND top = GetAncestor(hwnd, GA_ROOT);
    if (!top) top = hwnd;
    wchar_t title[512]{};
    GetWindowTextW(top, title, 512);
    if (LooksLikeWeixinTitle(title)) return true;
    return LooksLikeWeixinExecutable(QueryHwndProcessImagePath(top));
}

bool NeedsFakeFocusInjection(const WindowModeScriptConfig& config, HWND hwnd) {
    if (LooksLikeMapleStoryTarget(config, hwnd)) return false;
    if (LooksLikeTianLongBaBuTarget(config, hwnd)) return true;
    if (config.enabled && UsesCdpInput(config)) return false;
    if (LooksLikeRemoteDesktopWindowClass(config.windowClassName)
        || LooksLikeRemoteDesktopWindowClass(config.childWindowClassName)
        || LooksLikeRemoteDesktopExePath(config.targetExePath)) {
        return false;
    }
    if (ConfigLooksLikeElectronShell(config) || HwndLooksLikeChromiumShell(hwnd)) return true;
    if (LooksLikeWeixinTarget(config, hwnd)) return true;
    if (LooksLikeInjectRequiredGameClass(config.windowClassName)
        || LooksLikeInjectRequiredGameClass(config.childWindowClassName)) {
        return true;
    }
    if (LooksLikeEmulatorWindowClass(config.windowClassName)
        || LooksLikeEmulatorWindowClass(config.childWindowClassName)
        || LooksLikeEmulatorExecutable(config.targetExePath)
        || LooksLikeEmulatorTarget(config, hwnd)) {
        return true;
    }
    if (HwndLooksLikeDelphiVclGame(hwnd)) return true;
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    if (LooksLikeInjectRequiredGameClass(cls) || LooksLikeEmulatorWindowClass(cls)) return true;
    if (LooksLikeRemoteDesktopWindowClass(cls)) return false;
    HWND top = GetAncestor(hwnd, GA_ROOT);
    if (top && top != hwnd) {
        GetClassNameW(top, cls, 256);
        if (LooksLikeInjectRequiredGameClass(cls) || LooksLikeEmulatorWindowClass(cls)) return true;
    }
    return false;
}

bool HwndPrefersLcaBackgroundMessages(HWND hwnd) {
    // 直播 HWND 只认冒险岛。未登记游戏必须靠脚本类名，避免把记事本自检壳当成游戏。
    return LooksLikeMapleStoryTarget(WindowModeScriptConfig{}, hwnd);
}

bool PrefersLcaBackgroundMessages(const WindowModeScriptConfig& config, HWND hwnd) {
    if (LooksLikeMapleStoryTarget(config, hwnd)) return true;
    if (config.enabled && UsesCdpInput(config)) return false;
    if (NeedsFakeFocusInjection(config, hwnd)) return false;
    if (LooksLikeKernelAntiCheatProtectedTarget(config, hwnd)) return false;
    if (LooksLikeAndroidEmulatorWindowClass(config.windowClassName)
        || LooksLikeAndroidEmulatorWindowClass(config.childWindowClassName)
        || LooksLikeAndroidEmulatorExecutable(config.targetExePath)
        || LooksLikeAndroidEmulatorWindowTitle(config.windowName)
        || LooksLikeAndroidEmulatorWindowTitle(config.targetWindowTitle)) {
        return false;
    }
    if (LooksLikeStandardDesktopAppClass(config.windowClassName)
        || LooksLikeStandardDesktopAppClass(config.childWindowClassName)) {
        return false;
    }
    if (config.enabled
        && (config.executionKind == WindowModeExecutionKind::BackgroundWindow
            || config.executionKind == WindowModeExecutionKind::HiddenDesktop)) {
        if (ClassLooksLikeLcaUnknownGame(config.windowClassName)
            || ClassLooksLikeLcaUnknownGame(config.childWindowClassName)) {
            return true;
        }
        if (hwnd && IsWindow(hwnd)) {
            HWND top = GetAncestor(hwnd, GA_ROOT);
            if (!top) top = hwnd;
            wchar_t cls[256]{};
            GetClassNameW(top, cls, 256);
            if (ClassLooksLikeLcaUnknownGame(cls)) return true;
        }
    }
    return hwnd && HwndPrefersLcaBackgroundMessages(hwnd);
}

bool LooksLikeTianLongBaBuWindowClass(const std::wstring& className) {
    if (className.empty()) return false;
    const std::wstring lower = ToLowerCopy(className);
    // 官方客户端：`TianLongBaBuHJ WndClass`（类名里有空格）。
    if (lower.find(L"tianlong") != std::wstring::npos) return true;
    if (lower.find(L"babuhj") != std::wstring::npos) return true;
    return false;
}

bool LooksLikeTianLongBaBuExecutable(const std::wstring& exePath) {
    if (exePath.empty()) return false;
    const std::wstring lower = ToLowerCopy(exePath);
    if (lower.find(L"tianlongbabu") != std::wstring::npos) return true;
    if (lower.find(L"tianlong") != std::wstring::npos) return true;
    if (exePath.find(L"天龙八部") != std::wstring::npos) return true;
    if (exePath.find(L"开心天龙") != std::wstring::npos) return true;
    return false;
}

bool LooksLikeTianLongBaBuTitle(const std::wstring& title) {
    if (title.empty()) return false;
    if (title.find(L"天龙八部") != std::wstring::npos) return true;
    const std::wstring lower = ToLowerCopy(title);
    return lower.find(L"tianlong") != std::wstring::npos;
}

bool LooksLikeTianLongBaBuTarget(const WindowModeScriptConfig& config, HWND hwnd) {
    if (LooksLikeTianLongBaBuWindowClass(config.windowClassName)
        || LooksLikeTianLongBaBuWindowClass(config.childWindowClassName)
        || LooksLikeTianLongBaBuExecutable(config.targetExePath)
        || LooksLikeTianLongBaBuTitle(config.windowName)
        || LooksLikeTianLongBaBuTitle(config.targetWindowTitle)) {
        return true;
    }
    if (!hwnd || !IsWindow(hwnd)) return false;
    HWND top = GetAncestor(hwnd, GA_ROOT);
    if (!top) top = hwnd;
    wchar_t cls[256]{};
    GetClassNameW(top, cls, 256);
    if (LooksLikeTianLongBaBuWindowClass(cls)) return true;
    wchar_t title[512]{};
    GetWindowTextW(top, title, 512);
    if (LooksLikeTianLongBaBuTitle(title)) return true;
    return LooksLikeTianLongBaBuExecutable(QueryHwndProcessImagePath(top));
}

bool LooksLikeGameWindowClass(const std::wstring& className) {
    if (className.empty()) return false;
    if (LooksLikeAdobeAirWindowClass(className)) return true;
    if (LooksLikeUnrealEngineWindowClass(className)) return true;
    if (LooksLikeDelphiVclGameWindowClass(className)) return true;
    if (LooksLikeTianLongBaBuWindowClass(className)) return true;
    if (LooksLikeMapleStoryWindowClass(className)) return true;
    const std::wstring lower = ToLowerCopy(className);
    if (lower == L"unitywndclass" || lower == L"unitywndclasshidden") return true;
    if (lower == L"sdl_app" || lower == L"glfw30") return true;
    if (lower.find(L"unity") != std::wstring::npos) return true;
    if (lower.find(L"unreal") != std::wstring::npos) return true;
    if (lower.find(L"godot") != std::wstring::npos) return true;
    return false;
}

bool LooksLikeKernelAntiCheatToken(const std::wstring& text) {
    if (text.empty()) return false;
    const std::wstring lower = ToLowerCopy(text);
    if (lower.find(L"riotwindowclass") != std::wstring::npos) return true;
    if (lower.find(L"league of legends") != std::wstring::npos) return true;
    if (lower.find(L"leagueclient") != std::wstring::npos) return true;
    if (lower.find(L"league of legends.exe") != std::wstring::npos) return true;
    if (lower.find(L"riotclient") != std::wstring::npos) return true;
    if (lower.find(L"valorant") != std::wstring::npos) return true;
    if (lower.find(L"vgtray") != std::wstring::npos) return true;
    if (text.find(L"英雄联盟") != std::wstring::npos) return true;
    return false;
}

bool LooksLikeKernelAntiCheatProtectedTarget(const WindowModeScriptConfig& config, HWND hwnd) {
    if (LooksLikeKernelAntiCheatToken(config.windowClassName)
        || LooksLikeKernelAntiCheatToken(config.childWindowClassName)
        || LooksLikeKernelAntiCheatToken(config.windowName)
        || LooksLikeKernelAntiCheatToken(config.targetWindowTitle)
        || LooksLikeKernelAntiCheatToken(config.targetExePath)) {
        return true;
    }
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    if (LooksLikeKernelAntiCheatToken(cls)) return true;
    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, 512);
    if (LooksLikeKernelAntiCheatToken(title)) return true;
    return LooksLikeKernelAntiCheatToken(QueryHwndProcessImagePath(hwnd));
}

const wchar_t* KernelAntiCheatBackgroundUnsupportedHint() {
    return L"英雄联盟 / Valorant 等带内核反作弊的游戏无法使用后台窗口模式："
        L"游戏只读前台硬件输入，且反作弊会拒绝进程注入（VirtualAllocEx 拒绝访问）。"
        L"请改用默认模式，并把游戏放在前台。";
}

bool LooksLikeGlfwOrSdlWindowClass(const std::wstring& className) {
    if (className.empty()) return false;
    const std::wstring lower = ToLowerCopy(className);
    return lower == L"glfw30" || lower == L"sdl_app";
}

bool LooksLikeEmulatorWindowClass(const std::wstring& className) {
    if (className.empty()) return false;
    const std::wstring lower = ToLowerCopy(className);
    // wxWidgets 模拟器常把 exe 名注册为窗口类（DeSmuME 等）。
    if (lower == L"desmume" || lower.find(L"desmume") != std::wstring::npos) return true;
    if (lower.find(L"dolphin") != std::wstring::npos) return true;
    if (lower.find(L"melonds") != std::wstring::npos) return true;
    if (lower.find(L"visualboyadvance") != std::wstring::npos) return true;
    if (lower == L"vba" || lower.find(L"vba-m") != std::wstring::npos) return true;
    if (lower == L"ppsspp" || lower.find(L"ppsspp") != std::wstring::npos) return true;
    if (lower.find(L"bizhawk") != std::wstring::npos) return true;
    if (lower.find(L"xenia") != std::wstring::npos) return true;
    if (lower.find(L"xemu") != std::wstring::npos) return true;
    return false;
}

bool LooksLikeEmulatorExecutable(const std::wstring& exePath) {
    if (exePath.empty()) return false;
    const std::wstring lower = ToLowerCopy(exePath);
    const auto slash = lower.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? lower : lower.substr(slash + 1);
    static const wchar_t* kExact[] = {
        L"desmume.exe",
        L"desmume_avx2.exe",
        L"dolphin-emu.exe",
        L"dolphin.exe",
        L"pcsx2.exe",
        L"pcsx2-qt.exe",
        L"citra-qt.exe",
        L"citra.exe",
        L"retroarch.exe",
        L"melonds.exe",
        L"yuzu.exe",
        L"ryujinx.exe",
        L"cemu.exe",
        L"rpcs3.exe",
        L"duckstation-qt.exe",
        L"duckstation.exe",
        L"ppsspp.exe",
        L"project64.exe",
        L"snes9x.exe",
        L"fceux.exe",
        L"visualboyadvance-m.exe",
        L"mgba-qt.exe",
        L"mgba.exe",
        L"bizhawk.exe",
        L"mednafen.exe",
        L"xenia.exe",
        L"xemu.exe",
        L"epsxe.exe",
        L"winuae.exe",
        L"fs-uae.exe",
        L"higan.exe",
        L"ares.exe",
    };
    for (const wchar_t* exact : kExact) {
        if (name == exact) return true;
    }
    static const wchar_t* kPartial[] = {
        L"desmume",
        L"dolphin",
        L"pcsx2",
        L"citra",
        L"melonds",
        L"yuzu",
        L"ryujinx",
        L"duckstation",
        L"ppsspp",
        L"retroarch",
        L"bizhawk",
        L"xenia",
        L"xemu",
        L"epsxe",
        L"winuae",
        L"higan",
        L"ares",
    };
    for (const wchar_t* part : kPartial) {
        if (name.find(part) != std::wstring::npos) return true;
    }
    return false;
}

bool LooksLikeAndroidEmulatorWindowClass(const std::wstring& className) {
    if (className.empty()) return false;
    const std::wstring lower = ToLowerCopy(className);
    if (lower.find(L"ldplayer") != std::wstring::npos) return true;
    if (lower.find(L"dnplayer") != std::wstring::npos) return true;
    if (lower == L"therender") return true;
    if (lower.find(L"bluestacks") != std::wstring::npos) return true;
    if (lower.find(L"nox") != std::wstring::npos) return true;
    if (lower.find(L"mumu") != std::wstring::npos) return true;
    if (lower.find(L"nemu") != std::wstring::npos) return true;
    if (lower.find(L"memu") != std::wstring::npos) return true;
    return false;
}

bool LooksLikeAndroidEmulatorWindowTitle(const std::wstring& title) {
    if (title.empty()) return false;
    const std::wstring lower = ToLowerCopy(title);
    static const wchar_t* kPartial[] = {
        L"mumu",
        L"nemu",
        L"ldplayer",
        L"雷电",
        L"bluestacks",
        L"蓝叠",
        L"nox",
        L"夜神",
        L"memu",
        L"逍遥",
        L"安卓设备",
        L"安卓模拟器",
    };
    for (const wchar_t* part : kPartial) {
        if (lower.find(part) != std::wstring::npos) return true;
    }
    return false;
}

bool LooksLikeQtRenderWindowClass(const std::wstring& className) {
    if (className.empty()) return false;
    const std::wstring lower = ToLowerCopy(className);
    return lower.find(L"qt") != std::wstring::npos
        && lower.find(L"qwindowicon") != std::wstring::npos;
}

bool LooksLikeAndroidEmulatorExecutable(const std::wstring& exePath) {
    if (exePath.empty()) return false;
    const std::wstring lower = ToLowerCopy(exePath);
    const auto slash = lower.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? lower : lower.substr(slash + 1);
    static const wchar_t* kExact[] = {
        L"dnplayer.exe",
        L"ldplayer.exe",
        L"ldboxheadless.exe",
        L"hd-player.exe",
        L"bluestacks.exe",
        L"nox.exe",
        L"noxvmhandle.exe",
        L"mumuplayer.exe",
        L"nemuheadless.exe",
        L"nemuplayer.exe",
        L"mumunxdevice.exe",
        L"mumunxmain.exe",
        L"mumunxplayer.exe",
        L"memu.exe",
        L"memuhead.exe",
    };
    for (const wchar_t* exact : kExact) {
        if (name == exact) return true;
    }
    static const wchar_t* kPartial[] = {
        L"ldplayer",
        L"dnplayer",
        L"bluestacks",
        L"hd-player",
        L"nox",
        L"mumu",
        L"nemu",
        L"memu",
    };
    for (const wchar_t* part : kPartial) {
        if (name.find(part) != std::wstring::npos) return true;
    }
    return false;
}

bool LooksLikeEmulatorTarget(const WindowModeScriptConfig& config, HWND hwnd) {
    if (LooksLikeEmulatorWindowClass(config.windowClassName)
        || LooksLikeEmulatorWindowClass(config.childWindowClassName)
        || LooksLikeEmulatorExecutable(config.targetExePath)) {
        return true;
    }
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    if (LooksLikeEmulatorWindowClass(cls)) return true;
    return LooksLikeEmulatorExecutable(QueryHwndProcessImagePath(hwnd));
}

bool ConfigLooksLikeEmulatorTarget(const WindowModeScriptConfig& config) {
    if (LooksLikeEmulatorWindowClass(config.windowClassName)
        || LooksLikeEmulatorWindowClass(config.childWindowClassName)
        || LooksLikeEmulatorExecutable(config.targetExePath)) {
        return true;
    }
    // 微信 4.x 与 MuMu 同为 Qt*QWindowIcon，不能单凭类名当模拟器。
    if (LooksLikeWeixinTarget(config, nullptr)) return false;
    if (LooksLikeAndroidEmulatorWindowClass(config.windowClassName)
        || LooksLikeAndroidEmulatorWindowClass(config.childWindowClassName)
        || LooksLikeAndroidEmulatorExecutable(config.targetExePath)
        || LooksLikeAndroidEmulatorWindowTitle(config.windowName)
        || LooksLikeAndroidEmulatorWindowTitle(config.targetWindowTitle)
        || LooksLikeQtRenderWindowClass(config.windowClassName)
        || LooksLikeQtRenderWindowClass(config.childWindowClassName)) {
        return true;
    }
    return false;
}

bool UsesFakeFocusForTarget(const WindowModeScriptConfig& config, HWND hwnd) {
    // 冒险岛 / 未登记游戏：窗口消息即可。冒险岛走路的 mapleSafe 注入由 TryInstallFakeFocus 单独放行。
    if (PrefersLcaBackgroundMessages(config, hwnd)) return false;
    // 仅真铺满独占禁止注入；窗口化 UE5 走精简假焦点（不钩 PeekMessage）。
    // 传奇 Delphi 即使铺满也要钩 GetCursorPos：后台模式不会改走 SendInput。
    if (LooksLikeMonitorCoveringFullscreen(hwnd)) {
        bool allowCovering = HwndLooksLikeDelphiVclGame(hwnd)
            || LooksLikeDelphiVclGameWindowClass(config.windowClassName)
            || LooksLikeDelphiVclGameWindowClass(config.childWindowClassName)
            || LooksLikeTianLongBaBuTarget(config, hwnd);
        if (!allowCovering) return false;
    }
    if (LooksLikeRemoteDesktopWindowClass(config.windowClassName)
        || LooksLikeRemoteDesktopWindowClass(config.childWindowClassName)
        || LooksLikeRemoteDesktopExePath(config.targetExePath)) {
        return false;
    }
    if (hwnd && IsWindow(hwnd)) {
        wchar_t cls[256]{};
        GetClassNameW(hwnd, cls, 256);
        if (LooksLikeRemoteDesktopWindowClass(cls)) return false;
    }
    if (UsesFakeFocus(config)) return true;
    if (AndroidEmulatorPrefersFakeFocus(hwnd, &config)) return true;
    // 绑定时 config.exe 可能为空：按直播 HWND 识别 QQ/Discord 等 Chromium 壳，或微信 4.x。
    if (HwndLooksLikeChromiumShell(hwnd)) return true;
    if (LooksLikeWeixinTarget(config, hwnd)) return true;
    if (!config.enabled || UsesCdpInput(config)) return false;
    if (config.executionKind != WindowModeExecutionKind::HiddenDesktop
        && config.executionKind != WindowModeExecutionKind::BackgroundWindow) {
        return false;
    }
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    if (LooksLikeGameWindowClass(cls) || LooksLikeEmulatorWindowClass(cls)) return true;
    if (HwndLooksLikeDelphiVclGame(hwnd)) return true;
    const std::wstring path = QueryHwndProcessImagePath(hwnd);
    return LooksLikeEmulatorExecutable(path);
}

bool GameTargetNeedsHardwareWithoutFakeFocus(const WindowModeScriptConfig& config, HWND hwnd) {
    if (!config.enabled || UsesCdpInput(config)) return false;
    if (config.executionKind != WindowModeExecutionKind::HiddenDesktop
        && config.executionKind != WindowModeExecutionKind::BackgroundWindow) {
        return false;
    }
    if (LooksLikeChromiumShellTarget(config, hwnd)) return false;
    if (LooksLikeWeixinTarget(config, hwnd)) return false;
    if (AndroidEmulatorPrefersFakeFocus(hwnd, &config)
        || AndroidEmulatorPrefersFakeFocusFromConfig(config)) {
        return false;
    }
    return UsesFakeFocusForTarget(config, hwnd);
}

bool UsesFakeFocusOrAndroidQt(const WindowModeScriptConfig& config, HWND hwnd) {
    if (UsesFakeFocus(config)) return true;
    if (hwnd && AndroidEmulatorPrefersFakeFocus(hwnd, &config)) return true;
    return AndroidEmulatorPrefersFakeFocusFromConfig(config);
}

WindowModeInputStrategy ResolveInputStrategy(const WindowModeScriptConfig& config) {
    if (config.inputStrategy == WindowModeInputStrategy::SoftMessage) {
        return WindowModeInputStrategy::SoftMessage;
    }
    // Auto 或显式 cdp：Electron 壳（Chrome_WidgetWin + QQ.exe 等）不能走扩展桥。
    // 录制时曾按类名误标 cdp，回放须按 exe 降级，否则空等扩展后 EndRun、零操作。
    if (ConfigLooksLikeExtBridgeBrowser(config)) {
        return WindowModeInputStrategy::Cdp;
    }
    if (config.inputStrategy == WindowModeInputStrategy::Cdp) {
        // 显式 cdp 但目标不是真浏览器：降级，避免空等扩展。
        return WindowModeInputStrategy::SoftMessage;
    }
    return WindowModeInputStrategy::SoftMessage;
}

void AnnotateInputStrategyForSave(WindowModeScriptConfig& config) {
    if (config.inputStrategy != WindowModeInputStrategy::Auto) return;
    if (ConfigLooksLikeExtBridgeBrowser(config)) {
        config.inputStrategy = WindowModeInputStrategy::Cdp;
        if (config.cdpPort <= 0) config.cdpPort = 9222;
    }
}

std::wstring EnsureRemoteDebuggingLaunchArgs(const std::wstring& args, int port) {
    if (port <= 0) port = 9222;
    const std::wstring needle = L"--remote-debugging-port=";
    std::wstring lower = ToLowerCopy(args);
    if (lower.find(ToLowerCopy(needle)) != std::wstring::npos) {
        return args;
    }
    std::wstring out = args;
    if (!out.empty() && out.back() != L' ') out.push_back(L' ');
    out += needle;
    out += std::to_wstring(port);
    return out;
}

const wchar_t* HealthToDisplayText(WindowModeHealth health) {
    switch (health) {
    case WindowModeHealth::Ok: return L"就绪";
    case WindowModeHealth::Unknown: return L"未知";
    case WindowModeHealth::DesktopNotReady: return L"宏虚拟桌面未就绪";
    case WindowModeHealth::TargetNotFound: return L"未找到目标窗口";
    case WindowModeHealth::TargetMinimized: return L"目标已最小化";
    case WindowModeHealth::TargetNoRender: return L"目标无法渲染";
    case WindowModeHealth::CaptureFailed: return L"截图失败";
    case WindowModeHealth::PermissionMismatch: return L"权限不一致";
    default: return L"未知";
    }
}

const wchar_t* HealthToUserHint(WindowModeHealth health) {
    switch (health) {
    case WindowModeHealth::Ok: return L"窗口模式就绪";
    case WindowModeHealth::Unknown: return L"状态未知";
    case WindowModeHealth::DesktopNotReady: return L"无法创建或打开「鼠标宏」虚拟桌面";
    case WindowModeHealth::TargetNotFound: return L"请在「鼠标宏」虚拟桌面启动目标程序，或绑定当前窗口";
    case WindowModeHealth::TargetMinimized: return L"目标窗口已最小化，已尝试后台还原但仍失败，请手动还原";
    case WindowModeHealth::TargetNoRender: return L"该程序可能不支持后台截图（游戏/GPU 界面）";
    case WindowModeHealth::CaptureFailed: return L"截图失败，请稍后重试";
    case WindowModeHealth::PermissionMismatch:
        return L"目标程序权限更高。请右键本工具选择「以管理员身份运行」后再试，不要重复打开游戏客户端";
    default: return L"";
    }
}

}  // namespace windowmode
