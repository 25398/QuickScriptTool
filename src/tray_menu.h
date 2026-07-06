#pragma once
// ──────────────────────────────────────────────────────────────────
// tray_menu.h — 系统托盘右键菜单
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

private:
    enum class Item { ShowWindow = 0, Exit = 1 };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void Paint(HDC hdc);
    int HitItem(int x, int y) const;
    RECT ItemRect(int index) const;
    void Close(TrayMenuAction action);
    void TrackMouseLeave();

    static constexpr int kMenuW = 225;
    static constexpr int kMenuH = 95;
    static constexpr int kItemH = kMenuH / 2;
    static constexpr int kTextPadX = 16;
    static constexpr UINT kDeactivateCloseMsg = WM_USER + 701;

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HFONT font_ = nullptr;
    int hover_ = -1;
    TrayMenuAction result_ = TrayMenuAction::None;
    bool done_ = false;
};
