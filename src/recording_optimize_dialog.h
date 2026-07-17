#pragma once
// ──────────────────────────────────────────────────────────────────
// recording_optimize_dialog.h — 鼠标录制优化对话框
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "config.h"
#include "drawing.h"
#include "script_io.h"
#include "script_types.h"
#include "utils.h"
#include "prompt_modal.h"
#include "ui_scale.h"

/// 录制优化对话框 — 批量删除、等待调整、移动合并/压缩
class RecordingOptimizeDialog {
public:
    struct Result {
        bool saved = false;
        std::wstring savedPath;
    };

    Result Show(HWND owner, const ScriptMeta& recording);

    /// 当前打开的优化窗（托盘「显示窗口」应激活它，而不是揭开主窗）
    static HWND ActiveHwnd();

private:
    static HWND s_activeHwnd_;
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
    void DrawRadio(HDC hdc, const RECT& rc, bool checked);
    void DrawPanelCombo(HDC hdc, const RECT& rc, const wchar_t* text, bool open);

    bool HitCloseButton(int x, int y) const;
    bool HitTitleBar(int x, int y) const;
    bool PtInRect(const RECT& rc, int x, int y) const;
    bool DropPopupVisible() const;
    bool HitClickableControl(int x, int y) const;

    static int S(int designPx) { return UiLen(designPx); }

    RECT CloseRect() const {
        return RECT{S(kDialogW) - S(kCloseBtnW) - S(4), 0, S(kDialogW), S(kOptTitleH)};
    }
    RECT NameLabelRect() const {
        return RECT{S(kMargin), S(kNameRowTop), S(kMargin) + S(96), S(kNameRowTop) + S(kNameRowH)};
    }
    /// 与「录制名称:」同排垂直居中的输入框外框（高度用 kEditH，避免文字相对标签上偏）
    RECT NameEditRect() const {
        const int rowTop = S(kNameRowTop);
        const int rowH = S(kNameRowH);
        const int editH = S(kEditH);
        const int top = rowTop + std::max(0, (rowH - editH) / 2);
        return RECT{S(kMargin) + S(100), top, S(720), top + editH};
    }
    RECT StatsRect() const {
        return RECT{S(820), S(kNameRowTop), S(kDialogW) - S(kMargin), S(kNameRowTop) + S(kNameRowH)};
    }
    RECT ListHeaderTextRect() const {
        return RECT{S(kListLeft), S(kListHeaderTop),
            PrevKeyBtnRect().left - S(kToolbarGap), S(kListHeaderTop) + S(kListHeaderH)};
    }
    RECT PrevKeyBtnRect() const {
        const int right = S(kListLeft) + S(kListW) - S(kQuickSelectW) - S(kToolbarGap) - S(kNextKeyW)
            - S(kToolbarGap) - S(kKeySearchW) - S(kToolbarGap);
        return RECT{right - S(kPrevKeyW), S(kListHeaderTop), right, S(kListHeaderTop) + S(kToolbarBtnH)};
    }
    RECT KeySearchBtnRect() const {
        const int right = S(kListLeft) + S(kListW) - S(kQuickSelectW) - S(kToolbarGap) - S(kNextKeyW) - S(kToolbarGap);
        return RECT{right - S(kKeySearchW), S(kListHeaderTop), right, S(kListHeaderTop) + S(kToolbarBtnH)};
    }
    RECT NextKeyBtnRect() const {
        const int right = S(kListLeft) + S(kListW) - S(kQuickSelectW) - S(kToolbarGap);
        return RECT{right - S(kNextKeyW), S(kListHeaderTop), right, S(kListHeaderTop) + S(kToolbarBtnH)};
    }
    RECT QuickSelectBtnRect() const {
        return RECT{S(kListLeft) + S(kListW) - S(kQuickSelectW), S(kListHeaderTop),
            S(kListLeft) + S(kListW), S(kListHeaderTop) + S(kToolbarBtnH)};
    }
    RECT ListContentRect() const {
        return RECT{S(kListLeft), S(kListTop), S(kListLeft) + S(kListW), S(kFooterTop)};
    }
    RECT RightPanelRect() const {
        return RECT{S(kListLeft) + S(kListW) + S(kGap), S(kListHeaderTop),
            S(kDialogW) - S(kMargin), S(kFooterTop)};
    }
    RECT ApplyBtnRect() const {
        const RECT panel = RightPanelRect();
        return RECT{panel.left + S(kMargin), panel.bottom - S(68),
            panel.right - S(kMargin), panel.bottom - S(18)};
    }
    RECT SchemeComboRect() const {
        const RECT panel = RightPanelRect();
        return RECT{panel.left + S(kMargin), panel.top + S(kSchemeComboTop),
            panel.right - S(kMargin), panel.top + S(kSchemeComboTop) + S(kComboH)};
    }
    RECT WaitFilterComboRect() const {
        const RECT panel = RightPanelRect();
        return RECT{panel.left + S(kMargin), panel.top + S(kPanelContentTop) + S(36),
            panel.right - S(kMargin), panel.top + S(kPanelContentTop) + S(36) + S(kComboH)};
    }
    int WaitFilterComboBottom(const RECT& panel) const {
        return panel.top + S(kPanelContentTop) + S(36) + S(kComboH);
    }
    int WaitCompareEditTop(const RECT& panel) const {
        return WaitFilterComboBottom(panel) + S(8);
    }
    int WaitAdjustRowTop(const RECT& panel) const {
        if (waitFilterOp_ == 0) return WaitFilterComboBottom(panel) + S(14);
        return WaitCompareEditTop(panel) + S(kEditH) + S(kFieldGap) + S(4);
    }
    RECT CancelBtnRect() const {
        return RECT{S(kDialogW) - S(kMargin) - S(340), S(kFooterTop) + S(10),
            S(kDialogW) - S(kMargin) - S(180), S(kFooterTop) + S(kFooterH) - S(10)};
    }
    RECT SaveBtnRect() const {
        return RECT{S(kDialogW) - S(kMargin) - S(168), S(kFooterTop) + S(10),
            S(kDialogW) - S(kMargin), S(kFooterTop) + S(kFooterH) - S(10)};
    }
    RECT FooterRect() const { return RECT{0, S(kFooterTop), S(kDialogW), S(kDialogH)}; }

    int VisibleRowCount() const {
        const RECT list = ListContentRect();
        const int rowH = std::max(1, S(kRowH));
        const int listH = std::max(0, static_cast<int>(list.bottom - list.top));
        return std::max(1, listH / rowH);
    }
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
    void LoadActionsFromDisk();
    void BeginProgressiveLoad();
    void EnsureActionsParsed(int begin, int end);
    void EnsureFullyParsed();
    void ApplyLoadedActions(ScriptFileData&& fileData);
    void EnsureRowLabels(int begin, int end);
    void SchedulePrerender();
    void PrerenderMoreRowLabels();
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
    const std::wstring& RowLabelAt(int index);
    double ComputeDuration(const std::vector<ScriptAction>& actions) const;

    int SelectedCount() const;
    std::vector<int> SelectedIndices() const;
    std::vector<int> ContiguousSelectedRange() const;
    bool SelectionContainsRelativeMove() const;
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
    std::vector<std::wstring> rowLabels_;
    std::vector<std::wstring> actionBlocks_; // 未解析完的 JSON 块（与 actions_ 下标对齐）
    std::vector<uint8_t> actionParsed_;
    bool loadCoordsNormalized_ = false;
    CoordMeta loadCoordMeta_{};
    std::wstring sourcePath_;
    Hotkey hotkey_{};
    int originalActionCount_ = 0;
    double originalDuration_ = 0;
    double currentDuration_ = 0;
    int loadGeneration_ = 0;
    int prerenderCursor_ = 0;

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
    bool actionsLoaded_ = false;
    bool actionsLoading_ = false;
    std::wstring pendingLoadName_;
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
    PromptModal promptModal_;
    WindowOuterShadow outerShadow_;
};
