#include "window_list.h"
#include "window_target.h"

#include <dwmapi.h>

#include <algorithm>

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

namespace windowmode {

namespace {

std::wstring FileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

std::wstring ProcessNameOfWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) name = FileNameOf(path);
    CloseHandle(process);
    return name;
}

bool IsCloakedByShell(HWND hwnd) {
    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return false;
    return cloaked != 0;
}

/// Raymond Chen 的 Alt+Tab 判定：沿所有者链找到根，再沿「最后活动弹窗」链走回来，
/// 回到自己才说明本窗口是这一簇窗口的代表项。
bool IsOwnerChainRepresentative(HWND hwnd) {
    HWND walk = nullptr;
    HWND probe = GetAncestor(hwnd, GA_ROOTOWNER);
    while (probe != walk) {
        walk = probe;
        probe = GetLastActivePopup(walk);
        if (IsWindowVisible(probe)) break;
    }
    return walk == hwnd;
}

bool IsSwitchableWindow(HWND hwnd, DWORD excludePid) {
    if (!IsWindowVisible(hwnd)) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    // 本软件自己的窗口不该出现在切窗候选里（切过去等于打断自动化）
    if (excludePid != 0 && pid == excludePid) return false;

    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_NOACTIVATE) return false;
    // 工具窗默认不进 Alt+Tab；显式声明 APPWINDOW 的除外
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
    if (IsCloakedByShell(hwnd)) return false;
    if (!(ex & WS_EX_APPWINDOW) && !IsOwnerChainRepresentative(hwnd)) return false;

    // 无标题的多为壳/占位窗口，报给模型也没法指认
    if (GetWindowTextLengthW(hwnd) <= 0) return false;
    return true;
}

std::wstring TitleOf(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring title(static_cast<size_t>(len) + 1, L'\0');
    const int got = GetWindowTextW(hwnd, title.data(), len + 1);
    title.resize(got > 0 ? static_cast<size_t>(got) : 0);
    return title;
}

struct EnumContext {
    std::vector<SwitchableWindow>* out = nullptr;
    DWORD excludePid = 0;
    HWND foreground = nullptr;
};

BOOL CALLBACK EnumSwitchableProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumContext*>(lp);
    if (!ctx || !ctx->out) return FALSE;
    if (!IsSwitchableWindow(hwnd, ctx->excludePid)) return TRUE;

    SwitchableWindow entry;
    entry.hwnd = hwnd;
    entry.title = TitleOf(hwnd);
    entry.processName = ProcessNameOfWindow(hwnd);
    entry.minimized = IsIconic(hwnd) != FALSE;
    entry.foreground = (hwnd == ctx->foreground);
    entry.zIndex = static_cast<int>(ctx->out->size());
    ctx->out->push_back(std::move(entry));
    return TRUE;
}

std::wstring ToLower(const std::wstring& s) {
    std::wstring out = s;
    std::transform(out.begin(), out.end(), out.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return out;
}

/// 去掉零宽/不间断空格等（Edge 标题常见「Microsoft​ Edge」夹 ZWSP，导致子串匹配失败）
std::wstring NormalizeMatchText(std::wstring s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (c == 0x200B || c == 0x200C || c == 0x200D || c == 0xFEFF
            || c == 0x00A0 || c == 0x202F) {
            continue;
        }
        out.push_back(c);
    }
    return ToLower(out);
}

bool IsEdgeBrowserAlias(const std::wstring& needleLower) {
    return needleLower == L"edge"
        || needleLower == L"msedge"
        || needleLower == L"msedge.exe"
        || needleLower == L"microsoft edge"
        || needleLower == L"microsoftedge";
}

bool WindowMatchesQuery(const SwitchableWindow& w, const std::wstring& needleNorm) {
    if (needleNorm.empty()) return true;
    const std::wstring title = NormalizeMatchText(w.title);
    const std::wstring proc = NormalizeMatchText(w.processName);
    if (title.find(needleNorm) != std::wstring::npos) return true;
    if (proc.find(needleNorm) != std::wstring::npos) return true;
    // 「Microsoft Edge」应对上 msedge.exe（标题里可能只有站点名）
    if (IsEdgeBrowserAlias(needleNorm)
        && (proc.find(L"msedge") != std::wstring::npos
            || title.find(L"microsoft edge") != std::wstring::npos)) {
        return true;
    }
    return false;
}

}  // namespace

std::vector<SwitchableWindow> ListSwitchableWindows(DWORD excludePid) {
    std::vector<SwitchableWindow> out;
    EnumContext ctx;
    ctx.out = &out;
    ctx.excludePid = excludePid;
    ctx.foreground = GetForegroundWindow();
    // EnumWindows 按 Z 序（前→后）回调，正好就是 Alt+Tab 的排列顺序
    EnumWindows(&EnumSwitchableProc, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

std::vector<SwitchableWindow> MatchWindows(
    const std::vector<SwitchableWindow>& all, const std::wstring& query) {
    if (query.empty()) return all;
    const std::wstring needle = NormalizeMatchText(query);
    std::vector<SwitchableWindow> hits;
    for (const auto& w : all) {
        if (WindowMatchesQuery(w, needle))
            hits.push_back(w);
    }
    // 多候选时：标题命中 > 仅进程名命中；同档里非最小化优先，再按 Z 序（已是前→后）
    // 避免 match="msedge"/"edge" 在多标签 Edge 里随便切到最近用过的无关页
    std::stable_sort(hits.begin(), hits.end(),
        [&](const SwitchableWindow& a, const SwitchableWindow& b) {
            const std::wstring aTitle = NormalizeMatchText(a.title);
            const std::wstring bTitle = NormalizeMatchText(b.title);
            const bool aTitleHit = aTitle.find(needle) != std::wstring::npos
                || (IsEdgeBrowserAlias(needle) && aTitle.find(L"microsoft edge") != std::wstring::npos);
            const bool bTitleHit = bTitle.find(needle) != std::wstring::npos
                || (IsEdgeBrowserAlias(needle) && bTitle.find(L"microsoft edge") != std::wstring::npos);
            if (aTitleHit != bTitleHit) return aTitleHit;
            if (a.minimized != b.minimized) return !a.minimized;
            return a.zIndex < b.zIndex;
        });
    return hits;
}

std::wstring FormatWindowList(const std::vector<SwitchableWindow>& list) {
    if (list.empty()) return L"（当前没有可切换的窗口）";
    std::wstring out;
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& w = list[i];
        out += L"#" + std::to_wstring(i + 1) + L" " + w.title;
        if (!w.processName.empty()) out += L" [" + w.processName + L"]";
        if (w.foreground) out += L"（前台）";
        if (w.minimized) out += L"（最小化）";
        if (i + 1 < list.size()) out += L"\n";
    }
    return out;
}

bool ActivateWindow(HWND hwnd, std::wstring& error) {
    error.clear();
    if (!hwnd || !IsWindow(hwnd)) {
        error = L"窗口已不存在（可能已被关闭）";
        return false;
    }
    hwnd = TopLevelTargetWindow(hwnd);
    HWND fg = GetForegroundWindow();
    if (fg == hwnd) return true;

    auto trySimpleForeground = [&]() {
        AllowSetForegroundWindow(ASFW_ANY);
        SetForegroundWindow(hwnd);
        return GetForegroundWindow() == hwnd;
    };
    auto tryAttachForeground = [&](bool allowPosAndRestore) {
        HWND curFg = GetForegroundWindow();
        if (curFg == hwnd) return true;
        const DWORD curTid = GetCurrentThreadId();
        const DWORD fgTid = curFg ? GetWindowThreadProcessId(curFg, nullptr) : 0;
        // 不附加到前台线程时 SetForegroundWindow 会被系统的前台锁挡下
        const BOOL attached = (fgTid && fgTid != curTid)
            ? AttachThreadInput(fgTid, curTid, TRUE) : FALSE;
        if (allowPosAndRestore) {
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            BringWindowToTop(hwnd);
            SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        }
        SetForegroundWindow(hwnd);
        if (attached) AttachThreadInput(fgTid, curTid, FALSE);
        for (int i = 0; i < 20; ++i) {
            if (GetForegroundWindow() == hwnd) return true;
            Sleep(25);
        }
        return GetForegroundWindow() == hwnd;
    };

    // 真铺满监视器：禁止 SetWindowPos / Restore，避免拆 DXGI 独占。
    if (LooksLikeMonitorCoveringFullscreen(hwnd)) {
        if (trySimpleForeground()) return true;
        error = L"全屏独占游戏无法用 Win32 强制切前台（请先手动点进游戏）";
        return false;
    }
    // 窗口化 UE5（国王大道等）：仍算 DXGI 敏感，但必须 AttachThreadInput
    // 才能把 SendInput 打进游戏；禁止改尺寸/Z 序。
    if (LooksLikeFullscreenGameTarget(hwnd)) {
        if (trySimpleForeground()) return true;
        if (tryAttachForeground(false)) return true;
        error = L"未能把游戏切到前台（SendInput 只打前台窗；请先点一下游戏窗口）";
        return false;
    }
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    if (GetForegroundWindow() == hwnd) return true;
    if (tryAttachForeground(true)) return true;
    error = L"系统未把该窗口切到前台（可能被全屏独占或 UAC 窗口挡住）";
    return false;
}

namespace {
std::wstring NormProcessKey(std::wstring s) {
    for (auto& c : s) {
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    }
    if (s.size() > 4 && s.compare(s.size() - 4, 4, L".EXE") == 0)
        s.resize(s.size() - 4);
    return s;
}
}  // namespace

bool ActivateByProcessName(const std::wstring& processName,
    SwitchableWindow* outActivated, std::wstring& error) {
    error.clear();
    if (outActivated) *outActivated = {};
    const std::wstring want = NormProcessKey(processName);
    if (want.empty()) {
        error = L"进程名为空";
        return false;
    }
    const auto all = ListSwitchableWindows(0); // 含本进程：runProgram 刚起的窗也可能同会话调试场景需要
    const SwitchableWindow* best = nullptr;
    for (const auto& w : all) {
        if (NormProcessKey(w.processName) != want) continue;
        if (!best || w.zIndex < best->zIndex) best = &w;
    }
    if (!best) {
        error = L"没有进程名为「" + processName + L"」的可切换窗口";
        return false;
    }
    if (!ActivateWindow(best->hwnd, error)) return false;
    if (outActivated) *outActivated = *best;
    return true;
}

}  // namespace windowmode
