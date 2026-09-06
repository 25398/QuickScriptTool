#pragma once
// ──────────────────────────────────────────────────────────────────
// tray_menu.h — 系统托盘右键菜单（DesktopTools）
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

enum class TrayMenuAction {
    None = 0,
    ShowWindow,
    StopRunning,  // 停止当前运行（宏 / 连点 / 录制）
    Exit,
};

struct TrayMenuOptions {
    /// 有宏/连点/录制进行中时为 true；为 false 时菜单项灰显仍可见
    bool enableStop = false;
};

class TrayMenu {
public:
    static TrayMenuAction Show(HWND owner, POINT screenPt, const TrayMenuOptions& opts = {});
};
