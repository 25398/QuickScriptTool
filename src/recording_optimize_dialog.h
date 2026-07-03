#pragma once
// ──────────────────────────────────────────────────────────────────
// recording_optimize_dialog.h — 鼠标录制优化对话框
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <string>
#include <vector>

#include "config.h"
#include "script_types.h"
#include "utils.h"

/// 录制优化对话框 — 批量删除、等待调整、移动合并/压缩
class RecordingOptimizeDialog {
public:
    struct Result {
        bool saved = false;
        std::wstring savedPath;
    };

    Result Show(HWND owner, const ScriptMeta& recording);

private:
    static constexpr int kDialogW = 1200;
    static constexpr int kDialogH = 870;
    static constexpr int kOptTitleH = 52;
    static constexpr int kMargin = 16;
    static constexpr int kGap = 12;
    static constexpr int kRightPanelW = 300;
    static constexpr int kRowH = 48;
    static constexpr int kPopupItemH = kEditorPopupItemH;
    static constexpr int kCheckboxSize = 26;
    static constexpr int kRadioSize = 20;
    static constexpr int kRadioRowH = 36;
    static constexpr int kFieldGap = 8;
    static constexpr int kScrollBarW = 16;
    static constexpr int kFooterH = 72;
    static constexpr int kEditH = 38;
    static constexpr int kEditW = 96;
    static constexpr int kComboH = 42;
    static constexpr int kToolbarBtnH = 44;
    static constexpr int kPrevKeyW = 52;
    static constexpr int kKeySearchW = 228;
    static constexpr int kNextKeyW = 52;
    static constexpr int kQuickSelectW = 118;
    static constexpr int kToolbarGap = 6;

    static constexpr int kNameRowTop = 62;
    static constexpr int kNameRowH = 42;
    static constexpr int kListHeaderTop = 116;
    static constexpr int kListHeaderH = kToolbarBtnH;
    static constexpr int kListTop = kListHeaderTop + kListHeaderH + 8;
    static constexpr int kFooterTop = kDialogH - kFooterH;
    static constexpr int kListLeft = kMargin;
    static constexpr int kListW = kDialogW - kMargin - kGap - kRightPanelW - kMargin;
    static constexpr int kListH = kFooterTop - kListTop;

    static constexpr int kPanelSelectedTop = 12;
    static constexpr int kSchemeLabelTop = 44;
    static constexpr int kSchemeComboTop = 76;
    static constexpr int kPanelContentTop = kSchemeComboTop + kComboH + 14;
    static constexpr int kMergeAdjustLabelTop = kPanelContentTop + 36 + 5 * kRadioRowH + 6;
    static constexpr int kMergeAdjustEditTop = kMergeAdjustLabelTop + 30;
    static constexpr int kCompressWaitEditTop = kPanelContentTop + 30;
    static constexpr int kCompressThresholdLabelTop = kCompressWaitEditTop + kEditH + kFieldGap + 4;
    static constexpr int kCompressThresholdEditTop = kCompressThresholdLabelTop + 28;

    enum class PopupKind { None, KeySearch, QuickSelect, OptimizeScheme, WaitFilter };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK DropPopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleDropPopup(UINT msg, WPARAM wp, LPARAM lp);

    void CleanupGdi();
    void Paint();
    void PaintList(HDC hdc);
    void PaintRightPanel(HDC hdc);
    void PaintDropPopupContent(HDC hdc);
    void DrawGreenButton(HDC hdc, const RECT& rc, const wchar_t* text, bool hover, bool enabled = true);
    void DrawOutlineButton(HDC hdc, const RECT& rc, const wchar_t* text, bool hover);
    void DrawCheckbox(HDC hdc, const RECT& rc, bool checked);
    void DrawRadio(HDC hdc, const RECT& rc, bool checked);
    void DrawPanelCombo(HDC hdc, const RECT& rc, const wchar_t* text, bool open);

    bool HitCloseButton(int x, int y) const;
    bool HitTitleBar(int x, int y) const;
    bool PtInRect(const RECT& rc, int x, int y) const;
    bool DropPopupVisible() const;

    RECT CloseRect() const {
        return RECT{kDialogW - kCloseBtnW - 4, 0, kDialogW, kOptTitleH};
    }
    RECT NameLabelRect() const {
        return RECT{kMargin, kNameRowTop, kMargin + 96, kNameRowTop + kNameRowH};
    }
    RECT NameEditRect() const {
        return RECT{kMargin + 100, kNameRowTop, 720, kNameRowTop + kNameRowH};
    }
    RECT StatsRect() const {
        return RECT{820, kNameRowTop, kDialogW - kMargin, kNameRowTop + kNameRowH};
    }
    RECT ListHeaderTextRect() const {
        return RECT{kListLeft, kListHeaderTop,
            PrevKeyBtnRect().left - kToolbarGap, kListHeaderTop + kListHeaderH};
    }
    RECT PrevKeyBtnRect() const {
        const int right = kListLeft + kListW - kQuickSelectW - kToolbarGap - kNextKeyW
            - kToolbarGap - kKeySearchW - kToolbarGap;
        return RECT{right - kPrevKeyW, kListHeaderTop, right, kListHeaderTop + kToolbarBtnH};
    }
    RECT KeySearchBtnRect() const {
        const int right = kListLeft + kListW - kQuickSelectW - kToolbarGap - kNextKeyW - kToolbarGap;
        return RECT{right - kKeySearchW, kListHeaderTop, right, kListHeaderTop + kToolbarBtnH};
    }
    RECT NextKeyBtnRect() const {
        const int right = kListLeft + kListW - kQuickSelectW - kToolbarGap;
        return RECT{right - kNextKeyW, kListHeaderTop, right, kListHeaderTop + kToolbarBtnH};
    }
    RECT QuickSelectBtnRect() const {
        return RECT{kListLeft + kListW - kQuickSelectW, kListHeaderTop,
            kListLeft + kListW, kListHeaderTop + kToolbarBtnH};
    }
    RECT ListContentRect() const {
        return RECT{kListLeft, kListTop, kListLeft + kListW, kFooterTop};
    }
    RECT RightPanelRect() const {
        return RECT{kListLeft + kListW + kGap, kListHeaderTop,
            kDialogW - kMargin, kFooterTop};
    }
    RECT ApplyBtnRect() const {
        const RECT panel = RightPanelRect();
        return RECT{panel.left + kMargin, panel.bottom - 68,
            panel.right - kMargin, panel.bottom - 18};
    }
    RECT SchemeComboRect() const {
        const RECT panel = RightPanelRect();
        return RECT{panel.left + kMargin, panel.top + kSchemeComboTop,
            panel.right - kMargin, panel.top + kSchemeComboTop + kComboH};
    }
    RECT WaitFilterComboRect() const {
        const RECT panel = RightPanelRect();
        return RECT{panel.left + kMargin, panel.top + kPanelContentTop + 36,
            panel.right - kMargin, panel.top + kPanelContentTop + 36 + kComboH};
    }
    int WaitFilterComboBottom(const RECT& panel) const {
        return panel.top + kPanelContentTop + 36 + kComboH;
    }
    int WaitCompareEditTop(const RECT& panel) const {
        return WaitFilterComboBottom(panel) + 8;
    }
    int WaitAdjustRowTop(const RECT& panel) const {
        if (waitFilterOp_ == 0) return WaitFilterComboBottom(panel) + 14;
        return WaitCompareEditTop(panel) + kEditH + kFieldGap + 4;
    }
    RECT CancelBtnRect() const {
        return RECT{kDialogW - kMargin - 340, kFooterTop + 10,
            kDialogW - kMargin - 180, kFooterTop + kFooterH - 10};
    }
    RECT SaveBtnRect() const {
        return RECT{kDialogW - kMargin - 168, kFooterTop + 10,
            kDialogW - kMargin, kFooterTop + kFooterH - 10};
    }
    RECT FooterRect() const { return RECT{0, kFooterTop, kDialogW, kDialogH}; }

    int VisibleRowCount() const { return kListH / kRowH; }
    int PopupVisibleCount() const;
    int MaxScrollTop() const;
    void ClampScroll();
    void ScrollToIndex(int index);
    int HitListRow(int x, int y) const;
    int HitPopupItemLocal(int x, int y) const;
    RECT RowRect(int index) const;
    RECT CheckboxRect(int index) const;

    void SyncSelectionSize();
    void UpdateStats();
    void ApplyActionChange();
    void UpdatePanelControls();
    void CenterEditTextVertically(HWND edit);
    void CreateDropPopup();
    void SyncDropPopup();
    void CloseMenuPopup();
    void OpenMenuPopup(PopupKind kind, const RECT& anchor, const std::vector<std::wstring>& items);
    void InvalidatePopupRow(int idx);
    void SetHoverFlag(bool& flag, bool value, const RECT& rc);
    void InvalidateListArea();
    void InvalidatePanelArea();

    bool IsKeyOperation(const ScriptAction& action) const;
    bool IsMoveOrWait(const ScriptAction& action) const;
    COLORREF RowBackground(const ScriptAction& action) const;
    COLORREF RowBackgroundAt(int index) const;
    std::wstring ActionDisplayText(const ScriptAction& action) const;
    double ComputeDuration(const std::vector<ScriptAction>& actions) const;

    int SelectedCount() const;
    std::vector<int> SelectedIndices() const;
    std::vector<int> ContiguousSelectedRange() const;
    bool SelectionIsContiguousMoveWait() const;

    void FindKeyOperation(int mode);
    int FindKeyIndexFrom(int start, bool forward) const;
    void QuickSelect(int mode);

    void ApplyBatchDelete();
    void ApplyWaitAdjust();
    void ApplyMoveMerge();
    void ApplyMoveCompress();
    bool WaitMatchesFilter(double duration, double compareValue) const;

    void ShowAlert(const wchar_t* message);
    bool SaveToNewRecording();

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND dropPopup_ = nullptr;
    HWND nameEdit_ = nullptr;
    HWND valueEdit_ = nullptr;
    HWND thresholdEdit_ = nullptr;
    HWND mergeWaitEdit_ = nullptr;

    HFONT titleFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HFONT closeFont_ = nullptr;
    HFONT btnFont_ = nullptr;

    std::vector<ScriptAction> actions_;
    std::vector<bool> selected_;
    std::wstring sourcePath_;
    Hotkey hotkey_{};
    int originalActionCount_ = 0;
    double originalDuration_ = 0;
    double currentDuration_ = 0;

    int scrollTop_ = 0;
    int anchorIndex_ = 0;
    int keySearchHighlight_ = -1;
    int lastKeySearchIndex_ = -1;

    PopupKind openPopup_ = PopupKind::None;
    RECT popupAnchor_{};
    std::vector<std::wstring> popupItems_;
    int popupHover_ = -1;
    int popupScroll_ = 0;

    int optimizeScheme_ = 0;
    bool protectKeyOps_ = true;
    int waitFilterOp_ = 0;
    double waitAdjustValue_ = 0.1;
    double filterCompareValue_ = 0.1;
    int mergeWaitMode_ = 2;
    double mergeWaitValue_ = 0.1;
    double compressWait_ = 0.1;
    double compressThreshold_ = 1.0;

    bool done_ = false;
    bool saved_ = false;
    std::wstring savedPath_;
    bool hoverClose_ = false;
    bool hoverCancel_ = false;
    bool hoverSave_ = false;
    bool hoverApply_ = false;
    bool hoverPrevKey_ = false;
    bool hoverNextKey_ = false;
    bool hoverKeySearch_ = false;
    bool hoverQuickSelect_ = false;
    bool draggingScroll_ = false;
    int scrollDragOffset_ = 0;
};
