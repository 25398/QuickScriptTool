#pragma once
// ──────────────────────────────────────────────────────────────────
// scheduled_task_dialog.h — 定时任务管理对话框（列表/创建双视图）
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <string>
#include <vector>

#include "config.h"
#include "scheduled_task_datetime_picker.h"
#include "scheduled_task_scheduler.h"
#include "scheduled_task_types.h"

class ScheduledTaskDialog {
public:
    void Show(HWND owner, ScheduledTaskScheduler& scheduler);

private:
    enum class View { List, Create };
    enum class PopupKind { None, FileSelect };

    // ── Window / message dispatch ──────────────────────────────────
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK DropPopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleDropPopup(UINT msg, WPARAM wp, LPARAM lp);

    // ── View switching ─────────────────────────────────────────────
    void ShowListView();
    void ShowCreateView(const ScheduledTask* editTask = nullptr);
    int DialogW() const { return view_ == View::List ? kListW : kCreateW; }

    // ── Painting ───────────────────────────────────────────────────
    void Paint();
    void PaintList(HDC hdc);
    void PaintCreate(HDC hdc);
    void PaintTable(HDC hdc);
    void PaintDropPopup(HDC hdc);
    void CleanupGdi();

    // ── List view ──────────────────────────────────────────────────
    RECT CreateBtnRect() const;
    RECT PauseHintRect() const;
    RECT DisableAllCheckboxRect() const;
    RECT TableRect() const;
    RECT RowRect(int index) const;
    RECT EditBtnRect(int row) const;
    RECT DeleteBtnRect(int row) const;
    int HitRow(int y) const;
    bool HitEdit(int row, int x, int y) const;
    bool HitDelete(int row, int x, int y) const;
    void DeleteTaskAt(int index);

    // ── Create view ────────────────────────────────────────────────
    RECT CloseRect() const;
    RECT RowLabelRect(int row) const;
    RECT RowFieldRect(int row) const;
    RECT KindRadioRect(int index) const;
    RECT FreqRadioRect(int index) const;
    int StatusRowIndex() const;
    RECT StatusRadioRect(int index) const;
    RECT WeekDayRect(int index) const;
    RECT CancelBtnRect() const;
    RECT OkBtnRect() const;
    void RefreshFileList();
    void UpdateDefaultTime();
    void SyncTimePreview();
    void SyncPopups();
    void ToggleFilePopup();
    void ToggleTimePicker();
    void CloseFilePopup();
    void CloseAllPopups();
    bool DropPopupVisible() const;
    bool ValidateAndCollect();
    void ShowAlert(const wchar_t* msg);
    bool ClickOnPopup(int x, int y) const;

    // ── Hit-test helpers ───────────────────────────────────────────
    bool HitClose(int x, int y) const;
    bool HitTitle(int x, int y) const;
    bool PtIn(const RECT& rc, int x, int y) const;

    // ── Sizes ──────────────────────────────────────────────────────
    static constexpr int kListW = 845;
    static constexpr int kCreateW = 570;
    static constexpr int kDialogH = 480;
    static constexpr int kTitleH = 38;

    // List view sizes
    static constexpr int kToolbarH = 48;
    static constexpr int kHeaderH = 36;
    static constexpr int kRowH = 40;
    static constexpr int kMargin = 12;
    static constexpr int kCheckboxSize = 18;

    // Create view sizes
    static constexpr int kLabelW = 96;
    static constexpr int kFieldLeft = 118;
    static constexpr int kFieldW = 360;
    static constexpr int kCreateRowH = 44;
    static constexpr int kComboH = 36;
    static constexpr int kRadioSize = 20;
    static constexpr int kFirstRowY = 56;
    static constexpr int kPopupItemH = 34;

    // ── Window handles ─────────────────────────────────────────────
    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND nameEdit_ = nullptr;
    HWND dropPopup_ = nullptr;
    ScheduledTaskScheduler* scheduler_ = nullptr;

    // ── Fonts ──────────────────────────────────────────────────────
    HFONT titleFont_ = nullptr;
    HFONT closeFont_ = nullptr;
    HFONT listFont_ = nullptr;     // 23pt, used for list body/buttons
    HFONT listSmallFont_ = nullptr; // 21pt, used for table cells
    HFONT createFont_ = nullptr;    // 24pt, used for create form/buttons

    // ── Modal state ────────────────────────────────────────────────
    View view_ = View::List;
    bool done_ = false;

    // ── List view state ────────────────────────────────────────────
    bool hoverClose_ = false;
    bool hoverCreate_ = false;
    bool hoverDisableAll_ = false;
    int hoverEditRow_ = -1;
    int hoverDeleteRow_ = -1;
    int scrollTop_ = 0;

    // ── Create view state ──────────────────────────────────────────
    ScheduledDateTimePicker timePicker_;
    ScheduledTask task_{};
    bool editing_ = false;
    std::vector<std::wstring> fileItems_;
    std::vector<std::wstring> filePaths_;
    int fileSel_ = -1;
    PopupKind openPopup_ = PopupKind::None;
    RECT filePopupAnchor_{};
    int popupHover_ = -1;
    int popupScroll_ = 0;
    bool hoverCancel_ = false;
    bool hoverOk_ = false;
    bool hoverFileCombo_ = false;
    bool hoverTimeCombo_ = false;
    std::wstring timePreview_;
};
