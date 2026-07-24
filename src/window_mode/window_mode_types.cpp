#include "window_mode_types.h"

#include <algorithm>
#include <cctype>

namespace windowmode {

namespace {

std::wstring ToLowerCopy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

}  // namespace

bool LooksLikeChromiumBrowserClass(const std::wstring& className) {
    if (className.empty()) return false;
    const std::wstring lower = ToLowerCopy(className);
    // Chrome / Edge / Chromium 顶层与常见子类。
    if (lower.find(L"chrome_widgetwin") != std::wstring::npos) return true;
    if (lower.find(L"chrome_renderwidgethosthwnd") != std::wstring::npos) return true;
    if (lower.rfind(L"chrome_", 0) == 0) return true;
    return false;
}

WindowModeInputStrategy ResolveInputStrategy(const WindowModeScriptConfig& config) {
    if (config.inputStrategy == WindowModeInputStrategy::Cdp) {
        return WindowModeInputStrategy::Cdp;
    }
    if (config.inputStrategy == WindowModeInputStrategy::SoftMessage) {
        return WindowModeInputStrategy::SoftMessage;
    }
    // Auto
    if (LooksLikeChromiumBrowserClass(config.windowClassName)
        || LooksLikeChromiumBrowserClass(config.childWindowClassName)) {
        return WindowModeInputStrategy::Cdp;
    }
    return WindowModeInputStrategy::SoftMessage;
}

void AnnotateInputStrategyForSave(WindowModeScriptConfig& config) {
    if (config.inputStrategy != WindowModeInputStrategy::Auto) return;
    if (LooksLikeChromiumBrowserClass(config.windowClassName)
        || LooksLikeChromiumBrowserClass(config.childWindowClassName)) {
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
    case WindowModeHealth::PermissionMismatch: return L"请以相同权限运行本工具与目标程序";
    default: return L"";
    }
}

}  // namespace windowmode
