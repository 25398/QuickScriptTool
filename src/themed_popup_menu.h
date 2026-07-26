#pragma once
// ──────────────────────────────────────────────────────────────────
// themed_popup_menu.h — 与下拉/托盘一致的自绘弹出菜单（非系统 HMENU）
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <vector>

struct ThemedPopupMenuItem {
    int id = 0;
    const wchar_t* text = nullptr;
};

class ThemedPopupMenu {
public:
    /// 在屏幕坐标弹出菜单；选中返回对应 id，取消返回 0。
    static int Show(HWND owner, POINT screenPt, const std::vector<ThemedPopupMenuItem>& items);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void Paint(HDC hdc);
    int HitItem(int x, int y) const;
    RECT ItemRect(int index) const;
    void Close(int id);
    void TrackMouseLeave();
    void Measure();

    static constexpr UINT kDeactivateCloseMsg = WM_USER + 702;

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HFONT font_ = nullptr;
    std::vector<ThemedPopupMenuItem> items_;
    int hover_ = -1;
    int resultId_ = 0;
    bool done_ = false;
    /// 打开时若左键仍按着，需吞掉这次松开，避免立刻选中/关闭。
    bool ignoreOpeningButtonUp_ = false;
    int menuW_ = 0;
    int menuH_ = 0;
    int itemH_ = 0;
    int textPadX_ = 0;
};
