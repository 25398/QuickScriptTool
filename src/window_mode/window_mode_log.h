#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace windowmode {

using WindowModeLogSink = std::function<void(const std::wstring& line)>;

void SetWindowModeLogSink(WindowModeLogSink sink);
void WindowModeLog(const std::wstring& line);
void WindowModeLog(const wchar_t* line);
void WindowModeLogf(const wchar_t* fmt, ...);

/// 输出当前用户桌面 / 目标窗口桌面 / iconic 等快照（用于找图诊断）
void WindowModeLogDesktopSnap(const wchar_t* tag, HWND hwnd);

}  // namespace windowmode
