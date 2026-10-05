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

/// 进程映像名（含扩展名，如 "msedge.exe"）；取不到返回空。
/// 用途：判断前台窗口属于哪个浏览器（打开 edge:// 内置页时要用对浏览器）。
std::wstring ProcessImageNameByPid(unsigned long pid);

/// 是否为「优先复用已开实例」的浏览器类目标（edge/chrome/firefox…）
bool IsBrowserLaunchTarget(const std::wstring& nameOrPath);

/// 启动程序（path 可为绝对路径或系统可搜索到的程序名）
bool LaunchProgram(const std::wstring& path, const std::wstring& args);

/// 最近一次 LaunchProgram 失败原因（成功则清空）；供 AI 工具层回报，避免假成功
const std::wstring& LastLaunchProgramError();

/// 解析后的路径是否像可 ShellExecute 的本地文件（排除 URL）
bool ProgramLaunchPathExists(const std::wstring& path);

// ── 「按显示名启动应用」的确定性解析（不碰键盘、不碰开始菜单搜索）────────
//
// 为什么要这条：`openAppViaSearch` 原先靠 Win+S 输入显示名 + Enter。两处不可靠 ——
//   ① Win10/11 的「搜索」默认会走网页结果，输入「植物大战僵尸融合版」回车**打开的是
//      浏览器里的 Bing 搜索**，而不是游戏（真实事故：游戏被误关后想重启，结果打开了
//      一个搜索网页，然后在「怎么读命令输出」上白烧十几轮）；
//   ② Win+S 抢焦点/被输入法拦截，全都不可控。
//
// 所以先做**纯本地解析**：桌面快捷方式 → 开始菜单快捷方式 → App Paths → 启动夹 exe。
// 命中就走 openFile（ShellExecute 交给 shell 解释 .lnk），失败就明确报「没找到」，
// 绝不悄悄退化成「搜索一下」。
struct AppLaunchTarget {
    /// 可直接 ShellExecute 的路径（.lnk 或 .exe）
    std::wstring path;
    /// 解析来源，供日志/工具回执解释「为什么找到它」：
    /// desktop-lnk / startmenu-lnk / app-paths / startmenu-exe / where
    std::wstring source;
    /// 命中的名字（快捷方式名或 exe 名），解释用
    std::wstring matchedName;
    bool ok() const { return !path.empty(); }
};

/// 用应用**显示名**（如「植物大战僵尸融合版」「Microsoft Edge」「Excel」）找启动目标。
/// 找不到返回 ok()==false —— 调用方据此明确报错，不要退化成搜索。
AppLaunchTarget ResolveAppLaunchTarget(const std::wstring& displayName);

/// 解析结果是不是「非 .exe」的可执行入口（.lnk 快捷方式等）。
/// 用途：模型给的名字本来就该由 shell 解释（游戏快捷方式几乎都是 .lnk），
/// 这类目标不该被「必须是 exe」的校验挡掉。
bool LooksLikeShellLaunchTarget(const std::wstring& path);

/// 关闭匹配 target 的进程；matchFileNameOnly 为 true 时仅匹配文件名
bool CloseProgramsByTarget(const std::wstring& target, bool matchFileNameOnly);

/// 终止与当前进程同路径 exe 的其他实例（不含当前进程）。
int TerminateOtherInstancesOfCurrentExe();
