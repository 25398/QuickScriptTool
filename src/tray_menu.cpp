#include "tray_menu.h"

#include <shellapi.h>

// 托盘右键：使用系统 TrackPopupMenu。
// 自绘 ThemedPopupMenu 在点任务栏/托盘空白时经常收不到 deactivate/点击，菜单会粘住。
TrayMenuAction TrayMenu::Show(HWND owner, POINT screenPt, const TrayMenuOptions& opts) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return TrayMenuAction::None;
    AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(TrayMenuAction::ShowWindow), L"显示窗口");
    UINT stopFlags = MF_STRING;
    if (!opts.enableStop) stopFlags |= MF_GRAYED;
    AppendMenuW(menu, stopFlags, static_cast<UINT_PTR>(TrayMenuAction::StopRunning), L"停止当前运行");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, static_cast<UINT_PTR>(TrayMenuAction::Exit), L"退出");

    if (owner && IsWindow(owner)) SetForegroundWindow(owner);
    // 不用 TPM_NONOTIFY：部分环境会导致菜单点选/消失异常
    const UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN;
    const int id = TrackPopupMenu(menu, flags, screenPt.x, screenPt.y, 0, owner, nullptr);
    // 经典托盘模式：让菜单正确消失
    if (owner && IsWindow(owner)) PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (id == static_cast<int>(TrayMenuAction::ShowWindow)) return TrayMenuAction::ShowWindow;
    if (id == static_cast<int>(TrayMenuAction::StopRunning)) return TrayMenuAction::StopRunning;
    if (id == static_cast<int>(TrayMenuAction::Exit)) return TrayMenuAction::Exit;
    return TrayMenuAction::None;
}
