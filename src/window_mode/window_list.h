#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace windowmode {

/// 一个「可切换窗口」（等价于 Alt+Tab 列表里的一项）。
/// 用途：AI 动作执行时把切窗从「截图识图猜」换成本地台账直接激活。
struct SwitchableWindow {
    HWND hwnd = nullptr;
    std::wstring title;
    /// 进程可执行文件名（不含目录），如 EXCEL.EXE
    std::wstring processName;
    bool minimized = false;
    bool foreground = false;
    /// 在 Z 序中的位置（0 = 最前）。Alt+Tab 默认落在 z=1 的那个窗口上。
    int zIndex = 0;
};

/// 按 Z 序（前→后）枚举可切换的顶层窗口。
/// 过滤规则对齐系统 Alt+Tab：可见、未被 shell 隐藏(cloaked)、非工具窗、
/// 非 NOACTIVATE、且是所有者链里的代表窗口。
/// excludePid：跳过该进程的窗口（0 = 不跳过，自检用）。
std::vector<SwitchableWindow> ListSwitchableWindows(DWORD excludePid);

/// 产品默认：排除本软件自己的窗口（切到自动化工具自身没有意义）
inline std::vector<SwitchableWindow> ListSwitchableWindows() {
    return ListSwitchableWindows(GetCurrentProcessId());
}

/// 子串匹配（不分大小写）标题或进程名；query 为空时返回全部。
std::vector<SwitchableWindow> MatchWindows(
    const std::vector<SwitchableWindow>& all, const std::wstring& query);

/// 把窗口列表排版成给模型看的短文本（每行一个窗口，带序号/进程/状态）。
std::wstring FormatWindowList(const std::vector<SwitchableWindow>& list);

/// 把窗口切到前台（必要时先还原最小化）。成功返回 true；失败时写出原因。
bool ActivateWindow(HWND hwnd, std::wstring& error);

/// 按进程可执行文件名激活（如 EXCEL.EXE / excel）；多窗口时取 Z 序最前。
/// 用于 runProgram 后宿主自动置顶，绕过「match=excel 多候选拒绝」。
bool ActivateByProcessName(const std::wstring& processName,
    SwitchableWindow* outActivated, std::wstring& error);

}  // namespace windowmode
