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
    /// ── 可辨识信息（2026-09-29 加）──────────────────────────────────────────
    /// 起因：双开同一款游戏时两行条目**标题完全一样**，界面上只有句柄
    /// （`#1 (句柄: 134368874)` / `#2 (句柄: 66944)`）—— 句柄对人没有任何辨识意义，
    /// 用户无法判断该选哪一份，于是出现"绑错窗口/一会走A一会原地A"。
    DWORD pid = 0;
    int clientWidth = 0;
    int clientHeight = 0;
    int left = 0;
    int top = 0;
    /// 列表里还有几个窗口与它**同类名且同标题**（>0 即「双开/多开」特征）。
    /// UI 据此标 ⚠；绑定校验（`CountSameNamePeers`）据此报警而不是静默挑一个。
    int sameNamePeers = 0;
};

/// 同名兄弟窗口数（同一个 hwnd 之外，**同类名 + 同标题**的顶层窗口个数）。
/// 用途：绑定校验「报警不静默」—— 双开时两份客户端标题完全相同，
/// 自动化只能按用户选定的 hwnd 投递；一旦存在同名兄弟，就必须在日志里说清楚，
/// 让用户能判断"动作是不是跑到另一份上了"，而不是让程序猜。
int CountSameNamePeers(HWND hwnd);

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
/// ★pid != 0 时**先按进程号精确过滤**，再按 query 子串过滤。
///   起因（2026-10-02）：`FormatWindowList` 的台账**已经打印 `pid=…`**，双开同名窗口时
///   还明确写着「⚠同类名同标题共N个（**按 pid/客户区区分，别只按标题选**）」
///   ⇒ 模型照台账的指示用 pid 消歧，而 `activateWindow` 只认 match ⇒ 被回一句
///   「缺少 match」⇒ 模型在「match 命中 2 个 / pid 不被接受」之间反复绕圈。
///   ⇒ **产品叫模型用 pid，就必须真的收 pid**（否则是产品自相矛盾）。
///   query 为空 + pid != 0 = 取该进程的全部窗口；两者同时给 = 取交集。
std::vector<SwitchableWindow> MatchWindows(
    const std::vector<SwitchableWindow>& all, const std::wstring& query,
    DWORD pid = 0);

/// 把窗口列表排版成给模型看的短文本（每行一个窗口，带序号/进程/状态）。
std::wstring FormatWindowList(const std::vector<SwitchableWindow>& list);

/// 把窗口切到前台（必要时先还原最小化）。成功返回 true；失败时写出原因。
bool ActivateWindow(HWND hwnd, std::wstring& error);

/// 按进程可执行文件名激活（如 EXCEL.EXE / excel）；多窗口时取 Z 序最前。
/// 用于 runProgram 后宿主自动置顶，绕过「match=excel 多候选拒绝」。
bool ActivateByProcessName(const std::wstring& processName,
    SwitchableWindow* outActivated, std::wstring& error);

}  // namespace windowmode
