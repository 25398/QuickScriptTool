#pragma once
// ──────────────────────────────────────────────────────────────────
// settings_dialog.h — 应用设置对话框
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include "app_settings.h"
#include "config.h"
#include "crosshair_drag.h"
#include "drawing.h"
#include "panel_popup_combo.h"
#include "prompt_modal.h"

class SettingsDialog {
public:
    bool Show(HWND owner, quickscript::AppSettings& settings);

private:
    enum class Tab { Click, Playback, Other, AiApi, About };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void CleanupGdi();
    void Paint();
    void PaintClickTab(HDC hdc);
    void PaintPlaybackTab(HDC hdc);
    void PaintOtherTab(HDC hdc);
    void PaintAboutTab(HDC hdc);
    void PaintAiApiTab(HDC hdc);
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

    // ── AI 助手页布局 ──────────────────────────────────────────────
    RECT AiCheckboxRect() const;
    RECT AiModelComboRect() const;
    RECT AiDeleteModelBtnRect() const;
    RECT AiAddModelBtnRect() const;

    void RefreshAiModelCombo();
    void LoadAiProfileIntoControls(int index);
    quickscript::AiModelProfile ReadAiProfileFromControls() const;
    void AddCurrentAiProfile();
    void DeleteSelectedAiProfile();
    void PersistWorkingSettings();
    bool HitAiAddModelBtn(int x, int y) const;
    bool HitAiDeleteModelBtn(int x, int y) const;

    void ToggleCheckbox(Tab tab, int index);

    int BodyTextWidth(const wchar_t* text) const;
    void UpdateInlineLayout();

    struct InlineLayout {
        int randomIntervalEditX = 0;
        int pressReleaseEditX = 0;
        int clickLimitEditX = 0;
        int randomUnitX = 0;
        int pressReleaseUnitX = 0;
        int clickLimitSuffixX = 0;
        int playbackCountEditX = 0;
        int playbackCountSuffixX = 0;
        int playbackMinEditX = 0;
        int playbackMinUnitX = 0;
        int playbackMaxLabelX = 0;
        int playbackMaxEditX = 0;
        int playbackMaxUnitX = 0;
        int aiEditX = 0;
        int aiTempHintX = 0;
        int aiLabelW = 0;
        int jitterXEditX = 0;
        int jitterYEditX = 0;
        int fixedXEditX = 0;
        int fixedYEditX = 0;
    };

    static constexpr int kDialogW = kHomeWidth;
    static constexpr int kDialogH = kHomeHeight;
    static constexpr int kNavH = 44;
    static constexpr int kAiLabelW = 108;
    static constexpr int kContentTop = kTitleH + kNavH;
    static constexpr int kFooterH = 52;
    static constexpr int kFooterTop = kDialogH - kFooterH;
    static constexpr int kTabW = kDialogW / 5;
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
    // 点击设置 — 坐标子行（标签宽 + 8px 间距 + 输入框，与横坐标列对齐）
    static constexpr int kCoordEditGap = 8;
    static constexpr int kJitterLabelW = 140;
    static constexpr int kFixedCoordLabelW = 100;
    static constexpr int kJitterYGroupLeft = kIndent + 300;
    static constexpr int kFixedYGroupLeft = kIndent + 240;
    static constexpr int kJitterXEditLeft = kIndent + kJitterLabelW + kCoordEditGap;
    static constexpr int kJitterYEditLeft = kJitterYGroupLeft + kJitterLabelW + kCoordEditGap;
    static constexpr int kFixedXEditLeft = kIndent + kFixedCoordLabelW + kCoordEditGap;
    static constexpr int kAiTopRowBtnH = 32;
    static constexpr int kAiAddModelBtnW = 96;
    static constexpr int kAiDeleteBtnW = 64;
    static constexpr int kAiModelComboW = 200;
    static constexpr int kAiTopRowGap = 8;

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
        kEditApiUrl = 2130,
        kEditApiKey,
        kEditModelName,
        kEditTemperature,
        kEditMaxTokens,
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
    bool hoverAiAddModel_ = false;
    bool hoverAiDeleteModel_ = false;
    bool hoverAiModelCombo_ = false;
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

    HWND editApiUrl_ = nullptr;
    HWND editApiKey_ = nullptr;
    HWND editModelName_ = nullptr;
    HWND editTemperature_ = nullptr;
    HWND editMaxTokens_ = nullptr;

    PanelPopupCombo aiModelCombo_;
    PromptModal promptModal_;

    void ShowPromptAlert(const std::wstring& message);

    CrosshairDragController crosshairDrag_{};
    HCURSOR crosshairDragCursor_ = nullptr;
    InlineLayout inlineLayout_{};
    WindowOuterShadow outerShadow_;
};

/// 若设置对话框正在显示，从 settings 引用同步勾选状态并刷新界面
void NotifyActiveSettingsDialogSync();
