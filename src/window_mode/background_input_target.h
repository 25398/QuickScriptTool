#pragma once

#include "window_mode_types.h"

#include <string>

namespace windowmode {

/// 后台 PostMessage 输入子窗命中原因（日志/自检）。
enum class BackgroundInputTargetKind {
    TopLevel,
    ConfigChildClass,
    AndroidEmulatorRender,
    BrowserRenderWidget,
    KnownRenderSurface,
    TextInput,
    LargestSurface,
};

const wchar_t* BackgroundInputTargetKindName(BackgroundInputTargetKind kind);

/// 竞品 SmartMacroAI FindEmulatorRenderChild + 通用渲染子窗探测。
/// config 可为 nullptr（跳过 childWindowClassName，仍走启发式）。
HWND FindBackgroundInputChild(HWND top, const WindowModeScriptConfig* config,
    BackgroundInputTargetKind* outKind = nullptr);

/// 客户区坐标：fromHwnd → toHwnd（LDPlayer 工具栏偏移等）。
bool MapClientPointBetweenHwnds(HWND fromHwnd, HWND toHwnd, int& cx, int& cy);

/// 将坐标限制在目标客户区内。
void ClampToClientRect(HWND hwnd, int& cx, int& cy);

/// LDPlayer 等有 TheRender → PostMessage；MuMu 等 Qt 壳无 TheRender → 假焦点。
bool AndroidEmulatorPrefersFakeFocus(HWND top, const WindowModeScriptConfig* config);
bool AndroidEmulatorPrefersFakeFocusFromConfig(const WindowModeScriptConfig& config);
bool IsDesktopEmulatorTarget(HWND hwnd, const WindowModeScriptConfig* config = nullptr);
bool IsAndroidEmulatorTarget(HWND hwnd, const WindowModeScriptConfig* config = nullptr);
/// MuMu/DirectX 等：后台模式已禁用假前台 SendInput；保留 API 恒为 false。
bool AndroidEmulatorNeedsHardwareInput(HWND hwnd, const WindowModeScriptConfig* config = nullptr);

}  // namespace windowmode
