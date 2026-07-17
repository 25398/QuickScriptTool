#pragma once

#include <windows.h>

#include <string>

/// 获取鼠标位置下窗口对应进程的可执行文件绝对路径
std::wstring GetProcessPathFromPoint(int x, int y);

struct WindowInfoFromPoint {
    std::wstring windowTitle;
    std::wstring windowClassName;
    std::wstring childWindowClassName;
    std::wstring processPath;
    std::wstring documentPath;  ///< 若能从命令行解析到打开的文档路径
    int x = 0;
    int y = 0;
};

/// 获取鼠标位置下的顶层窗口与命中子窗口信息
WindowInfoFromPoint GetWindowInfoFromPoint(int x, int y);

/// 启动程序（path 可为绝对路径或系统可搜索到的程序名）
bool LaunchProgram(const std::wstring& path, const std::wstring& args);

/// 关闭匹配 target 的进程；matchFileNameOnly 为 true 时仅匹配文件名
bool CloseProgramsByTarget(const std::wstring& target, bool matchFileNameOnly);

/// 终止与当前进程同路径 exe 的其他实例（不含当前进程）。
int TerminateOtherInstancesOfCurrentExe();
