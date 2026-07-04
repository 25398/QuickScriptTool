#pragma once
// ──────────────────────────────────────────────────────────────────
// settings_dialog.h — 应用设置对话框
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include "app_settings.h"
#include "config.h"
#include "crosshair_drag.h"

class SettingsDialog {
public:
    bool Show(HWND owner, quickscript::AppSettings& settings);

private:
    enum class Tab { Click, Playback, Other, About };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void CleanupGdi();
    void Paint();
    void PaintClickTab(HDC hdc);
    void PaintPlaybackTab(HDC hdc);
    void PaintOtherTab(HDC hdc);
    void PaintAboutTab(HDC hdc);
    void DrawSettingsTab(HDC hdc, const RECT& rc, Tab tab, const wchar_t* text);
    void DrawFooter(HDC hdc);
    void FillGradientRect(HDC hdc, RECT rc, COLORREF start, COLORREF end, bool vertical);
    void PositionChildControls();
    void CenterEditTextVertically(HWND edit);
    int CenteredEditY(int rowTop, int rowHeight) const;

    void SyncControlsFromSettings();
    void SyncSettingsFromControls();
    void UpdateControlVisibility();
    void RestoreDefaults();
    void SaveAndClose();

    bool HitClose(int x, int y) const;
    bool HitTitle(int x, int y) const;
    bool PtIn(const RECT& rc, int x, int y) const;

    RECT CloseRect() const;
    RECT TabRect(Tab tab) const;
    RECT FooterRect() const;
    RECT RestoreLinkRect() const;
    RECT SaveBtnRect() const;
    RECT CheckUpgradeBtnRect() const;

    // ── 点击设置页布局 ─────────────────────────────────────────────
    int ClickTop() const { return kContentTop + kContentPad; }
    int ClickRowY(int index) const;
    RECT ClickCheckboxRect(int index) const;
    bool HitClickCheckbox(int x, int y, int& outIndex) const;

    // ── 录制/宏回放设置页布局 ──────────────────────────────────────
    int PlaybackTop() const { return kContentTop + kContentPad; }
    int PlaybackRowY(int index) const;
    RECT PlaybackCheckboxRect(int index) const;
    bool HitPlaybackCheckbox(int x, int y, int& outIndex) const;

    // ── 其他设置页布局 ─────────────────────────────────────────────
    RECT OtherCheckboxRect(int index) const;

    void ToggleCheckbox(Tab tab, int index);

    static constexpr int kDialogW = kHomeWidth;
    static constexpr int kDialogH = kHomeHeight;
    static constexpr int kNavH = 44;
    static constexpr int kContentTop = kTitleH + kNavH;
    static constexpr int kFooterTop = kHomeFooterTop;
    static constexpr int kFooterH = kHomeHeight - kHomeFooterTop;
    static constexpr int kTabW = kDialogW / 4;
    static constexpr int kContentPad = 16;
    static constexpr int kCheckboxSize = 32;
    static constexpr int kMargin = 24;
    static constexpr int kRowH = 52;
    static constexpr int kSubRowH = 38;
    static constexpr int kEditW = 96;
    static constexpr int kEditH = 38;
    static constexpr int kSmallEditW = 60;
    static constexpr int kSubLineOffset = 36;
    static constexpr int kIndent = 52;
    static constexpr int kLabelAfterCheck = kMargin + kCheckboxSize + 10;

    enum CtrlId {
        kEditRandomInterval = 2101,
        kEditPressRelease,
        kEditJitterX,
        kEditJitterY,
        kEditFixedX,
        kEditFixedY,
        kEditClickLimit,
        kEditPlaybackCount,
        kEditPlaybackMin,
        kEditPlaybackMax,
        kCrosshairBtn = 2120,
    };

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    bool done_ = false;
    bool saved_ = false;

    Tab activeTab_ = Tab::Click;
    quickscript::AppSettings working_{};
    quickscript::AppSettings* settings_ = nullptr;

    bool hoverClose_ = false;
    bool hoverRestore_ = false;
    bool hoverSave_ = false;
    bool hoverCheckUpgrade_ = false;
    HWND hoverCrosshairBtn_ = nullptr;

    HFONT titleFont_ = nullptr;
    HFONT closeFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT tabFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HFONT aboutTitleFont_ = nullptr;

    HWND editRandomInterval_ = nullptr;
    HWND editPressRelease_ = nullptr;
    HWND editJitterX_ = nullptr;
    HWND editJitterY_ = nullptr;
    HWND editFixedX_ = nullptr;
    HWND editFixedY_ = nullptr;
    HWND editClickLimit_ = nullptr;
    HWND editPlaybackCount_ = nullptr;
    HWND editPlaybackMin_ = nullptr;
    HWND editPlaybackMax_ = nullptr;
    HWND crosshairBtn_ = nullptr;

    CrosshairDragController crosshairDrag_{};
    HCURSOR crosshairDragCursor_ = nullptr;
};
