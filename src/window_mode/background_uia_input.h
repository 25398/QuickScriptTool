#pragma once

#include <windows.h>

#include <string>

namespace windowmode {

bool SendQuickInputViaUiAutomation(HWND hwnd, const std::wstring& text);

/// UWP/WinUI 宿主（ApplicationFrameWindow、CoreWindow 等）不响应 PostMessage，
/// 后台点击走 UIA Invoke。Win32/Unity 禁止探测 UIA（ElementFromHandle 也可能抢前台）。
bool WindowUsesUiaClickFallback(HWND hwnd);

/// 在目标窗口的 UIA 树中查找包含屏幕点 (sx,sy) 的最深可调用元素并执行 Invoke。
/// 返回 true 表示已找到元素并成功调用（含 Toggle 型控件）；false 表示无可用元素。
/// 适用后台/被遮挡窗口（UWP 计算器、WinUI 等不响应 PostMessage 的应用）。
bool TryUiaInvokeAtScreenPoint(HWND topLevel, int sx, int sy);

}  // namespace windowmode
