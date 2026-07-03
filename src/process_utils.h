#pragma once

#include <windows.h>

#include <string>

/// 获取鼠标位置下窗口对应进程的可执行文件绝对路径
std::wstring GetProcessPathFromPoint(int x, int y);

/// 启动程序（path 可为绝对路径或系统可搜索到的程序名）
bool LaunchProgram(const std::wstring& path, const std::wstring& args);

/// 关闭匹配 target 的进程；matchFileNameOnly 为 true 时仅匹配文件名
bool CloseProgramsByTarget(const std::wstring& target, bool matchFileNameOnly);
