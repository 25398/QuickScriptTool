#pragma once
// ──────────────────────────────────────────────────────────────────
// taskbar_window.h — 子窗口任务栏独立显示辅助
// ──────────────────────────────────────────────────────────────────

#include <algorithm>
#include <stdio.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <dwmapi.h>
#include "resource.h"

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif

#ifndef ICON_SMALL2
#define ICON_SMALL2 2
#endif

extern HINSTANCE g_instance;

struct BreakoutTaskbarPlacement {
    RECT rect{};
    LONG_PTR exStyle = 0;
    bool saved = false;
};

inline bool GetWindowRestoredRect(HWND hwnd, RECT* rect) {
    if (!hwnd || !rect) return false;
    if (IsIconic(hwnd)) {
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (GetWindowPlacement(hwnd, &placement)) {
            *rect = placement.rcNormalPosition;
            return true;
        }
    }
    return GetWindowRect(hwnd, rect) == TRUE;
}

/// 在同尺寸窗口完全盖住 hwnd 之前构造：
/// 暂时把 hwnd 移出 Alt+Tab / 任务栏缩略图（避免 Win11 下 FORCE_ICONIC 黑屏/转圈），
/// 析构后恢复。上层窗口继续使用系统实时预览。
class ScopedCoveredWindowThumbnail {
public:
    explicit ScopedCoveredWindowThumbnail(HWND hwnd);
    ~ScopedCoveredWindowThumbnail();
    ScopedCoveredWindowThumbnail(const ScopedCoveredWindowThumbnail&) = delete;
    ScopedCoveredWindowThumbnail& operator=(const ScopedCoveredWindowThumbnail&) = delete;

private:
    HWND hwnd_ = nullptr;
    bool active_ = false;
    LONG_PTR savedExStyle_ = 0;
};

inline HICON LoadIconResource(UINT id, int cx, int cy) {
    return static_cast<HICON>(LoadImageW(
        g_instance, MAKEINTRESOURCEW(id), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
}

inline HICON LoadAppIcon(int cx = 0, int cy = 0) {
    if (cx <= 0) cx = GetSystemMetrics(SM_CXICON);
    if (cy <= 0) cy = GetSystemMetrics(SM_CYICON);
    return LoadIconResource(IDI_APPICON, cx, cy);
}

inline HICON LoadAppIconSmall() {
    return LoadAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
}

/// Win11 任务栏按钮用 ICON_SMALL2 辅助刷新；不要写入 GCLP_HICON/GCLP_HICONSM。
inline HICON LoadAppIconForTaskbar() {
    return LoadIconResource(IDI_APPICON, 256, 256);
}

inline HICON LoadTrayRunningIcon(int cx = 0, int cy = 0) {
    if (cx <= 0) cx = GetSystemMetrics(SM_CXICON);
    if (cy <= 0) cy = GetSystemMetrics(SM_CYICON);
    return LoadIconResource(IDI_TRAY_RUNNING, cx, cy);
}

inline HICON LoadTrayRunningIconSmall() {
    return LoadTrayRunningIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
}

inline HICON LoadTrayRunningIconForTaskbar() {
    return LoadIconResource(IDI_TRAY_RUNNING, 256, 256);
}

inline HICON LoadBreakoutPauseIcon(int cx = 0, int cy = 0) {
    if (cx <= 0) cx = GetSystemMetrics(SM_CXICON);
    if (cy <= 0) cy = GetSystemMetrics(SM_CYICON);
    return LoadIconResource(IDI_BREAKOUT_PAUSE, cx, cy);
}

inline HICON LoadBreakoutPauseIconSmall() {
    return LoadBreakoutPauseIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
}

inline HICON LoadBreakoutPauseIconForTaskbar() {
    return LoadIconResource(IDI_BREAKOUT_PAUSE, 256, 256);
}

inline constexpr wchar_t kMainTaskbarAppId[] = L"ShuDaXia.MouseMacro";
inline constexpr wchar_t kBreakoutTaskbarAppId[] = L"ShuDaXia.MouseMacro.BreakoutPause";

inline bool SetWindowPropertyString(IPropertyStore* store, const PROPERTYKEY& key, PCWSTR value) {
    if (!store || !value || !value[0]) return false;
    PROPVARIANT pv{};
    if (FAILED(InitPropVariantFromString(value, &pv))) return false;
    const HRESULT hr = store->SetValue(key, pv);
    PropVariantClear(&pv);
    return SUCCEEDED(hr);
}

inline bool BuildExeSiblingIconPath(PCWSTR fileName, UINT resourceId, wchar_t* out, size_t outChars) {
    if (!out || outChars == 0) return false;
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(g_instance, exePath, MAX_PATH);
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) return false;
    wchar_t iconFile[MAX_PATH]{};
    wcscpy_s(iconFile, exePath);
    wchar_t* iconSlash = wcsrchr(iconFile, L'\\');
    if (!iconSlash) return false;
    wcscpy_s(iconSlash + 1, MAX_PATH - (iconSlash + 1 - iconFile), fileName);
    if (GetFileAttributesW(iconFile) != INVALID_FILE_ATTRIBUTES) {
        swprintf_s(out, outChars, L"%s,0", iconFile);
        return true;
    }
    swprintf_s(out, outChars, L"%s,%u", exePath, resourceId);
    return true;
}

inline void TaskbarYieldMessages() {
    MSG msg{};
    for (int i = 0; i < 8 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++i) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

inline void SetWindowTaskbarRelaunchProps(HWND hwnd, PCWSTR appId, PCWSTR iconPath,
        PCWSTR displayName, PCWSTR relaunchCmd) {
    if (!hwnd) return;
    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store)))) return;
    if (appId && appId[0]) {
        SetWindowPropertyString(store, PKEY_AppUserModel_ID, appId);
    }
    if (iconPath && iconPath[0]) {
        SetWindowPropertyString(store, PKEY_AppUserModel_RelaunchIconResource, iconPath);
    }
    if (relaunchCmd && relaunchCmd[0]) {
        SetWindowPropertyString(store, PKEY_AppUserModel_RelaunchCommand, relaunchCmd);
    }
    if (displayName && displayName[0]) {
        SetWindowPropertyString(store, PKEY_AppUserModel_RelaunchDisplayNameResource, displayName);
    }
    store->Commit();
    store->Release();
}

inline void SetWindowMainTaskbarIdentity(HWND hwnd) {
    if (!hwnd) return;
    wchar_t iconPath[MAX_PATH]{};
    BuildExeSiblingIconPath(L"app_icon.ico", IDI_APPICON, iconPath, MAX_PATH);
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(g_instance, exePath, MAX_PATH);
    wchar_t relaunchCmd[MAX_PATH + 4]{};
    swprintf_s(relaunchCmd, MAX_PATH + 4, L"\"%s\"", exePath);
    SetWindowTaskbarRelaunchProps(hwnd, kMainTaskbarAppId, iconPath,
        L"鼠大侠-鼠标宏", relaunchCmd);
}

inline void RestoreWindowMainTaskbarIdentity(HWND hwnd) {
    SetWindowMainTaskbarIdentity(hwnd);
}

inline void ClearWindowBreakoutTaskbarIdentity(HWND hwnd) {
    if (!hwnd) return;
    RestoreWindowMainTaskbarIdentity(hwnd);
}

/// 脱离期间主窗口使用独立 AppUserModel ID，彻底绕开主分组的绿色固定图标缓存。
inline void SetWindowBreakoutTaskbarIdentity(HWND hwnd) {
    if (!hwnd) return;
    wchar_t iconPath[MAX_PATH]{};
    BuildExeSiblingIconPath(L"breakout_pause.ico", IDI_BREAKOUT_PAUSE, iconPath, MAX_PATH);
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(g_instance, exePath, MAX_PATH);
    wchar_t relaunchCmd[MAX_PATH + 4]{};
    swprintf_s(relaunchCmd, MAX_PATH + 4, L"\"%s\"", exePath);
    SetWindowTaskbarRelaunchProps(hwnd, kBreakoutTaskbarAppId, iconPath,
        L"鼠大侠-鼠标宏脱离中", relaunchCmd);
}

#ifndef DWMWA_FORCE_ICONIC_REPRESENTATION
#define DWMWA_FORCE_ICONIC_REPRESENTATION 7
#endif

#ifndef DWMWA_DISALLOW_PEEK
#define DWMWA_DISALLOW_PEEK 11
#endif

#ifndef DWMWA_FREEZE_REPRESENTATION
#define DWMWA_FREEZE_REPRESENTATION 14
#endif

inline void PrepareHwndTaskbarLivePreview(HWND hwnd) {
    if (!hwnd) return;
    BOOL off = FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_FORCE_ICONIC_REPRESENTATION, &off, sizeof(off));
    DwmSetWindowAttribute(hwnd, DWMWA_DISALLOW_PEEK, &off, sizeof(off));
    DwmSetWindowAttribute(hwnd, DWMWA_FREEZE_REPRESENTATION, &off, sizeof(off));
    InvalidateRect(hwnd, nullptr, TRUE);
    UpdateWindow(hwnd);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_FRAME);
    TaskbarYieldMessages();
}

inline void RefreshWindowTaskbarGrouping(HWND hwnd) {
    if (!hwnd) return;
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    TaskbarYieldMessages();
}

inline void SetWindowCloaked(HWND hwnd, bool cloaked) {
    if (!hwnd) return;
    const BOOL value = cloaked ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &value, sizeof(value));
    // cloak 不发 SHOWWINDOW；主动触发外阴影 Sync，避免主窗已隐仍留阴影框
    WINDOWPOS wp{};
    wp.hwnd = hwnd;
    wp.flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE;
    SendMessageW(hwnd, WM_WINDOWPOSCHANGED, 0, reinterpret_cast<LPARAM>(&wp));
}

#ifndef DWMWA_TRANSITIONS_FORCEDISABLED
#define DWMWA_TRANSITIONS_FORCEDISABLED 3
#endif

/// 页面切换：cloak + 禁 DWM 尺寸过渡，避免主页画面残留到编辑器尺寸上。
/// 任务栏按钮保持（不 SW_HIDE）。用法：Begin… 改布局/加载首屏 …End（再延后参数面板）。
inline void BeginSmoothPageTransition(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransitions, sizeof(disableTransitions));
    SetWindowCloaked(hwnd, true);
}

inline void EndSmoothPageTransition(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    // 仍 cloak：先父窗整面、再子控件，避免 Exclude 空洞或子控件未画时露出旧像素
    RedrawWindow(hwnd, nullptr, nullptr,
        RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
    RedrawWindow(hwnd, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    SetWindowCloaked(hwnd, false);
    BOOL disableTransitions = FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disableTransitions, sizeof(disableTransitions));
}

// 注意：模态子弹窗不要 DWM cloak 主窗。cloak 会挖透明洞，挪开/四周未盖住时
// 会透出桌面或 IDE。应叠在主窗上（同尺寸盖住，或半透明 overlay），关闭后 RedrawWindow。

inline bool IsWindowCloaked(HWND hwnd) {
    if (!hwnd) return false;
    BOOL cloaked = FALSE;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAK, &cloaked, sizeof(cloaked)))
        && cloaked == TRUE;
}

inline void RestoreWindowTaskbarLivePreview(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    SetWindowCloaked(hwnd, false);
    PrepareHwndTaskbarLivePreview(hwnd);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

inline void ApplyWindowIcons(HWND hwnd) {
    if (!hwnd) return;
    const HICON big = LoadAppIcon();
    const HICON sm = LoadAppIconSmall();
    if (big) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    }
    if (sm) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(sm));
    }
    if (big) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL2, reinterpret_cast<LPARAM>(big));
    }
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

inline void ApplyWindowMainTaskbarPresentation(HWND hwnd, PCWSTR displayName = nullptr) {
    if (!hwnd) return;
    SetWindowMainTaskbarIdentity(hwnd);
    ApplyWindowIcons(hwnd);
    if (displayName && displayName[0]) {
        SetWindowTextW(hwnd, displayName);
    }
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

inline void ApplyMainWindowNormalTaskbarPresentation(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    ApplyWindowMainTaskbarPresentation(hwnd);
}

inline void ApplyRunningTaskbarWindowIcons(HWND hwnd) {
    if (!hwnd) return;
    const HICON big = LoadTrayRunningIcon();
    const HICON sm = LoadTrayRunningIconSmall();
    if (big) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    }
    if (sm) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(sm));
    }
    if (big) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL2, reinterpret_cast<LPARAM>(big));
    }
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

inline void SetWindowRunningTaskbarIdentity(HWND hwnd) {
    if (!hwnd) return;
    wchar_t iconPath[MAX_PATH]{};
    BuildExeSiblingIconPath(L"tray_running.ico", IDI_TRAY_RUNNING, iconPath, MAX_PATH);
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(g_instance, exePath, MAX_PATH);
    wchar_t relaunchCmd[MAX_PATH + 4]{};
    swprintf_s(relaunchCmd, MAX_PATH + 4, L"\"%s\"", exePath);
    SetWindowTaskbarRelaunchProps(hwnd, kMainTaskbarAppId, iconPath,
        L"鼠大侠-鼠标宏运行中", relaunchCmd);
}

inline void ApplyRunningTaskbarPresentation(HWND hwnd) {
    if (!hwnd) return;
    SetWindowRunningTaskbarIdentity(hwnd);
    ApplyRunningTaskbarWindowIcons(hwnd);
    SetWindowTextW(hwnd, L"鼠大侠-鼠标宏运行中");
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

inline void ApplyBreakoutPauseWindowIcons(HWND hwnd) {
    if (!hwnd) return;
    const HICON big = LoadBreakoutPauseIcon();
    const HICON sm = LoadBreakoutPauseIconSmall();
    if (big) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    }
    if (sm) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(sm));
    }
    if (big) {
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL2, reinterpret_cast<LPARAM>(big));
    }
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

/// 脱离期间刷新主窗口任务栏按钮：主 AppUserModel ID + 红色图标。
inline void ApplyMainBreakoutTaskbarPresentation(HWND hwnd) {
    if (!hwnd) return;
    SetWindowBreakoutTaskbarIdentity(hwnd);
    ApplyBreakoutPauseWindowIcons(hwnd);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    TaskbarYieldMessages();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

inline void ApplyTrayNotifyIcon(HWND hwnd, UINT trayId, HICON icon, const wchar_t* tip) {
    if (!hwnd || !icon) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = trayId;
    nid.uFlags = NIF_TIP | NIF_ICON;
    nid.hIcon = icon;
    if (tip && tip[0]) wcscpy_s(nid.szTip, tip);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

inline void HideHwndFromTaskbar(HWND hwnd) {
    if (!hwnd) return;
    ShowWindow(hwnd, SW_HIDE);
}

/// 主窗口移出任务栏后，在原位 cloak 以供图缩略图采样（不加入任务栏）。
inline void ShowHwndCloakedOffTaskbar(HWND hwnd, BreakoutTaskbarPlacement* placement) {
    if (!hwnd) return;
    if (placement && !placement->saved) {
        GetWindowRect(hwnd, &placement->rect);
        placement->exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        placement->saved = true;
    }
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex |= WS_EX_TOOLWINDOW;
    ex &= ~WS_EX_APPWINDOW;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    int w = 400;
    int h = 300;
    int x = 0;
    int y = 0;
    if (placement && placement->saved) {
        w = placement->rect.right - placement->rect.left;
        h = placement->rect.bottom - placement->rect.top;
        x = placement->rect.left;
        y = placement->rect.top;
        if (w <= 0) w = 400;
        if (h <= 0) h = 300;
    }
    SetWindowCloaked(hwnd, false);
    SetWindowPos(hwnd, HWND_BOTTOM, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowCloaked(hwnd, true);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

inline void ForceHwndOffTaskbar(HWND hwnd) {
    if (!hwnd) return;
    SetWindowCloaked(hwnd, false);
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex |= WS_EX_TOOLWINDOW;
    ex &= ~WS_EX_APPWINDOW;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    ShowWindow(hwnd, SW_HIDE);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

/// 在任务栏显示窗口：保持原位并用 DWM  cloak 隐藏（缩略图可正常显示主界面）。
inline void ShowHwndCloakedOnTaskbar(HWND hwnd, BreakoutTaskbarPlacement* placement) {
    if (!hwnd) return;
    if (placement && !placement->saved) {
        GetWindowRect(hwnd, &placement->rect);
        placement->exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        placement->saved = true;
    }
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    if (!IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex &= ~WS_EX_TOOLWINDOW;
    ex |= WS_EX_APPWINDOW;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    int w = 400;
    int h = 300;
    int x = -32000;
    int y = -32000;
    if (placement && placement->saved) {
        w = placement->rect.right - placement->rect.left;
        h = placement->rect.bottom - placement->rect.top;
        x = placement->rect.left;
        y = placement->rect.top;
        if (w <= 0) w = 400;
        if (h <= 0) h = 300;
    }
    SetWindowCloaked(hwnd, false);
    SetWindowPos(hwnd, HWND_BOTTOM, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    PrepareHwndTaskbarLivePreview(hwnd);
    SetWindowCloaked(hwnd, true);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

inline void ShowMainWindowBreakoutOnTaskbar(HWND hwnd, BreakoutTaskbarPlacement* placement) {
    if (!hwnd) return;
    if (placement && !placement->saved) {
        GetWindowRect(hwnd, &placement->rect);
        placement->exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        placement->saved = true;
    }
    // 隐藏窗口重新进入任务栏前先写红色身份，避免按钮先按绿色缓存创建。
    ApplyMainBreakoutTaskbarPresentation(hwnd);
    ShowHwndCloakedOnTaskbar(hwnd, placement);
    ApplyMainBreakoutTaskbarPresentation(hwnd);
}

inline void MinimizeBreakoutWindowOnTaskbar(HWND hwnd, BreakoutTaskbarPlacement* placement) {
    if (!hwnd) return;
    if (placement && !placement->saved) {
        GetWindowRect(hwnd, &placement->rect);
        placement->exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        placement->saved = true;
    }
    SetWindowCloaked(hwnd, false);
    const bool wasIconic = IsIconic(hwnd) == TRUE;
    const bool wasVisible = IsWindowVisible(hwnd) == TRUE;
    if (!wasIconic && wasVisible) {
        PrepareHwndTaskbarLivePreview(hwnd);
    }
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex &= ~WS_EX_TOOLWINDOW;
    ex |= WS_EX_APPWINDOW;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    ApplyMainBreakoutTaskbarPresentation(hwnd);
    if (wasIconic) {
        // 隐藏的最小化窗口必须先恢复并绘制，再通过标准最小化创建可点击的任务栏按钮。
        if (!wasVisible) {
            ShowWindow(hwnd, SW_RESTORE);
            if (placement && placement->saved) {
                const int w = std::max<int>(placement->rect.right - placement->rect.left, 1);
                const int h = std::max<int>(placement->rect.bottom - placement->rect.top, 1);
                SetWindowPos(hwnd, HWND_BOTTOM, placement->rect.left, placement->rect.top, w, h,
                    SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
            PrepareHwndTaskbarLivePreview(hwnd);
            ShowWindow(hwnd, SW_MINIMIZE);
        }
    } else if (!wasVisible) {
        // 隐藏窗口先无激活显示并绘制一次，让 DWM 获得真实界面，再走标准最小化路径。
        if (placement && placement->saved) {
            const int w = std::max<int>(placement->rect.right - placement->rect.left, 1);
            const int h = std::max<int>(placement->rect.bottom - placement->rect.top, 1);
            SetWindowPos(hwnd, HWND_BOTTOM, placement->rect.left, placement->rect.top, w, h,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        PrepareHwndTaskbarLivePreview(hwnd);
        ShowWindow(hwnd, SW_MINIMIZE);
    } else {
        ShowWindow(hwnd, SW_MINIMIZE);
    }
    ApplyMainBreakoutTaskbarPresentation(hwnd);
    RefreshWindowTaskbarGrouping(hwnd);
    if (IsIconic(hwnd)) {
        PrepareHwndTaskbarLivePreview(hwnd);
        DwmInvalidateIconicBitmaps(hwnd);
    }
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

inline void HideMainWindowFromTaskbarAfterBreakout(HWND hwnd, BreakoutTaskbarPlacement* placement, bool hideWindow) {
    if (!hwnd) return;
    SetWindowCloaked(hwnd, false);
    if (placement && placement->saved) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, placement->exStyle);
    }
    ApplyWindowMainTaskbarPresentation(hwnd);
    if (!hideWindow) {
        RestoreWindowTaskbarLivePreview(hwnd);
        ApplyWindowMainTaskbarPresentation(hwnd);
    }
    if (hideWindow) {
        ShowWindow(hwnd, SW_HIDE);
    }
}

inline void HideHwndFromTaskbarOffscreen(HWND hwnd, BreakoutTaskbarPlacement* placement) {
    if (!hwnd) return;
    if (placement) placement->saved = false;
    ShowWindow(hwnd, SW_HIDE);
}

/// 将已隐藏的窗口以最小化状态显示到任务栏（不激活、不弹出界面）。
/// 注意：最小化会导致任务栏悬停预览显示静态图标，脱离时间请用 ShowHwndOnTaskbarOffscreen。
inline void ShowHwndMinimizedOnTaskbar(HWND hwnd) {
    if (!hwnd) return;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex &= ~WS_EX_TOOLWINDOW;
    ex |= WS_EX_APPWINDOW;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
    if (!IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_MINIMIZE);
    }
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

/// 使窗口在任务栏中独立显示。平时与主界面共用主 AppUserModel ID（绿色叠在一起）。
/// @param detachOwner 为 true 时解除 owner 关联，窗口与主界面完全独立。
inline void ApplyTaskbarWindowStyle(HWND hwnd, const wchar_t* taskbarTitle,
                                    bool detachOwner = false) {
    if (!hwnd) return;
    if (detachOwner) {
        SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, 0);
    }
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex &= ~WS_EX_TOOLWINDOW;
    ex |= WS_EX_APPWINDOW;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    ApplyWindowMainTaskbarPresentation(hwnd, taskbarTitle);
}
