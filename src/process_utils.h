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

/// 把常见别名/裸名解析成可 ShellExecute 的路径（edge→msedge.exe 真实路径等）。
/// 已是存在的绝对/相对路径则原样返回；解析失败返回 Trim 后的原串。
std::wstring ResolveProgramLaunchPath(const std::wstring& nameOrPath);

/// 是否为「优先复用已开实例」的浏览器类目标（edge/chrome/firefox…）
bool IsBrowserLaunchTarget(const std::wstring& nameOrPath);

/// 启动程序（path 可为绝对路径或系统可搜索到的程序名）
bool LaunchProgram(const std::wstring& path, const std::wstring& args);

/// 最近一次 LaunchProgram 失败原因（成功则清空）；供 AI 工具层回报，避免假成功
const std::wstring& LastLaunchProgramError();

/// 解析后的路径是否像可 ShellExecute 的本地文件（排除 URL）
bool ProgramLaunchPathExists(const std::wstring& path);

/// 关闭匹配 target 的进程；matchFileNameOnly 为 true 时仅匹配文件名
bool CloseProgramsByTarget(const std::wstring& target, bool matchFileNameOnly);

/// 终止与当前进程同路径 exe 的其他实例（不含当前进程）。
int TerminateOtherInstancesOfCurrentExe();
