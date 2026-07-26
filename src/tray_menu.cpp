#include "tray_menu.h"

#include "themed_popup_menu.h"

TrayMenuAction TrayMenu::Show(HWND owner, POINT screenPt) {
    const int id = ThemedPopupMenu::Show(owner, screenPt, {
        {static_cast<int>(TrayMenuAction::ShowWindow), L"显示窗口"},
        {static_cast<int>(TrayMenuAction::Exit), L"退出"},
    });
    if (id == static_cast<int>(TrayMenuAction::ShowWindow)) return TrayMenuAction::ShowWindow;
    if (id == static_cast<int>(TrayMenuAction::Exit)) return TrayMenuAction::Exit;
    return TrayMenuAction::None;
}
