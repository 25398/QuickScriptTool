#pragma once
// ──────────────────────────────────────────────────────────────────
// tray_menu.h — 系统托盘右键菜单（委托 ThemedPopupMenu）
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

enum class TrayMenuAction {
    None = 0,
    ShowWindow,
    Exit,
};

class TrayMenu {
public:
    static TrayMenuAction Show(HWND owner, POINT screenPt);
};
