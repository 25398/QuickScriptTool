#pragma once
// ──────────────────────────────────────────────────────────────────
// scheduled_task_datetime_picker.h — 运行时间滚轮选择器（非模态弹出层）
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <vector>

#include "scheduled_task_types.h"

class ScheduledDateTimePicker {
public:
    enum class Mode {
        Hourly,
        DailyWeekly,
        Custom,
    };

    ~ScheduledDateTimePicker();

    void Attach(HWND owner);
    void Detach();
    bool Visible() const;
    bool Toggle(const RECT& anchorClient, Mode mode, ScheduledTaskTime& time);
    void Hide();
    void SyncPosition();
    bool ConsumeAccepted();

    HWND Window() const { return hwnd_; }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    struct Column {
        std::wstring suffix;
        int minValue = 0;
        int maxValue = 0;
        int value = 0;
        int step = 1;
        bool circular = false;
    };

    static constexpr int kPickerW = 360;
    static constexpr int kPickerH = 236;
    static constexpr int kColW = 68;
    static constexpr int kRowH = 34;
    static constexpr int kVisibleRows = 5;

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void EnsureWindow();
    void BuildColumns(Mode mode);
    void ApplyToTime();
    void Paint();
    int HitColumn(int x) const;
    int HitRowInColumn(int y) const;
    void ScrollColumn(int col, int delta);
    int DisplayValue(const Column& col, int offset) const;
    void AcceptAndHide();
    int ColWidth() const;

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    RECT anchor_{};
    Mode mode_ = Mode::Custom;
    ScheduledTaskTime* time_ = nullptr;
    std::vector<Column> columns_;
    int hoverCol_ = -1;
    int hoverRow_ = -1;
    bool accepted_ = false;
    HFONT bodyFont_ = nullptr;
};
