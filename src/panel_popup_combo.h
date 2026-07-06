#pragma once
// ──────────────────────────────────────────────────────────────────
// panel_popup_combo.h — 与编辑器/宏下拉一致的自绘弹层组合框
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

class PanelPopupCombo {
public:
    static LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void Init(HWND owner, HFONT font);
    void Destroy();

    void SetItems(std::vector<std::wstring> items);
    void SetSelectedIndex(int index);
    int SelectedIndex() const { return sel_; }
    const std::vector<std::wstring>& Items() const { return items_; }

    std::wstring DisplayText() const;
    void SetPlaceholder(const wchar_t* text) { placeholder_ = text ? text : L""; }

    bool IsOpen() const { return open_; }
    void Toggle(const RECT& anchorClientRect);
    void Close();

    void DrawField(HDC hdc, const RECT& rc, bool hover) const;
    bool HitField(const RECT& rc, int x, int y) const;

    HWND PopupHwnd() const { return popup_; }
    bool IsPopupVisible() const;
    bool HitPopupScreen(int screenX, int screenY) const;
    void SyncPopupPosition(const RECT& anchorClientRect);

    LRESULT HandlePopupMessage(UINT msg, WPARAM wp, LPARAM lp);

    using SelectionCallback = std::function<void(int index)>;
    void SetSelectionCallback(SelectionCallback cb) { onSelect_ = std::move(cb); }

private:
    void PaintPopup(HDC hdc);
    int VisibleCount(int popupHeight) const;
    int HitItemIndex(int localY, int popupHeight) const;
    void SelectIndex(int index);

    HWND owner_ = nullptr;
    HWND popup_ = nullptr;
    HFONT font_ = nullptr;
    std::vector<std::wstring> items_;
    int sel_ = -1;
    int hover_ = -1;
    int scroll_ = 0;
    bool open_ = false;
    std::wstring placeholder_ = L"请选择";
    SelectionCallback onSelect_;
    RECT anchor_{};
};
