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
/// 仅 OutputDebugString，不进宏调试窗（扩展桥心跳等噪音）
void WindowModeLogVerbose(const std::wstring& line);
void WindowModeLogVerbosef(const wchar_t* fmt, ...);

/// 持久化生命周期事件：除 DebugString/宏调试窗外，追加写入
/// `<exe 目录>\window_mode_debug.log`（跨运行留存，用于排查「宏桌面」何时被创建）。
/// 低频调用（桌面创建/复用、窗口模式起止），勿用于逐帧日志。
void WindowModeLogEvent(const std::wstring& line);
void WindowModeLogEventf(const wchar_t* fmt, ...);

/// 输出当前用户桌面 / 目标窗口桌面 / iconic 等快照（用于找图诊断）
void WindowModeLogDesktopSnap(const wchar_t* tag, HWND hwnd);

}  // namespace windowmode
