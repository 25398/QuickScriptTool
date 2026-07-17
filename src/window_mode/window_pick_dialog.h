#pragma once

#include <windows.h>

#include <string>

#include "crosshair_drag.h"
#include "drawing.h"
#include "process_utils.h"

namespace windowmode {

struct WindowPickResult {
    bool accepted = false;
    std::wstring windowTitle;
    std::wstring windowClassName;
    std::wstring childWindowClassName;
    std::wstring processPath;
    std::wstring documentPath;
    int pickX = 0;
    int pickY = 0;
};

/// 指定窗口类弹窗（拖动准星拾取窗口信息，并回填程序路径）
class WindowPickDialog {
public:
    bool Show(HWND owner, WindowPickResult& inOut);

private:
    enum class Id {
        Crosshair = 6001,
        PickX,
        PickY,
        WindowTitle,
        WindowClass,
        ChildClass,
        ProcessPath,
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK CrosshairSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
        UINT_PTR, DWORD_PTR refData);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void OnCreate();
    void PositionControls();
    void OnPaint();
    void CleanupGdi();
    void Close(bool accepted);
    void ApplyPickInfo(const WindowInfoFromPoint& info);
    void SyncFieldsToResult();
    void UpdateHover(int x, int y);
    void EnsureMouseLeaveTracking();
    void OnSetCursor();
    void DrawOutlineButton(HDC hdc, const RECT& rc, const wchar_t* text, bool hover) const;
    void PaintFieldBorders(HDC hdc) const;

    bool HitClose(int x, int y) const;
    bool HitTitle(int x, int y) const;
    bool PtIn(const RECT& rc, int x, int y) const;

    RECT CloseRect() const;
    RECT CrosshairRect() const;
    RECT CancelBtnRect() const;
    RECT OkBtnRect() const;
    RECT FieldLabelRect(int row) const;
    RECT FieldEditRect(int row) const;
    RECT PickXEditRect() const;
    RECT PickYEditRect() const;

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND crosshairBtn_ = nullptr;
    HWND pickXEdit_ = nullptr;
    HWND pickYEdit_ = nullptr;
    HWND titleEdit_ = nullptr;
    HWND classEdit_ = nullptr;
    HWND childClassEdit_ = nullptr;
    HWND pathEdit_ = nullptr;

    HFONT titleFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT btnFont_ = nullptr;
    HFONT closeFont_ = nullptr;
    HCURSOR crosshairCursor_ = nullptr;

    bool done_ = false;
    bool accepted_ = false;
    bool trackingMouse_ = false;
    bool hoverClose_ = false;
    bool hoverCancel_ = false;
    bool hoverOk_ = false;
    bool hoverCrosshair_ = false;

    WindowPickResult* result_ = nullptr;
    CrosshairDragController crosshairDrag_{};
    WindowOuterShadow outerShadow_{};

    static constexpr int kDlgW = 520;
    static constexpr int kDlgH = 434;
    static constexpr int kTitleH = 38;
    static constexpr int kMargin = 24;
    static constexpr int kFooterH = 62;
    static constexpr int kFooterTop = kDlgH - kFooterH;
    static constexpr int kBtnH = 38;
    static constexpr int kBtnGap = 12;
    static constexpr int kCancelBtnW = 108;
    static constexpr int kOkBtnW = 168;
    static constexpr int kEditH = 30;
    static constexpr int kRowGap = 10;
    static constexpr int kLabelW = 96;
    static constexpr int kCrosshairH = 38;
    static constexpr int kRowH = 34;
};

}  // namespace windowmode
