#include "scheduled_task_datetime_picker.h"

#include "config.h"
#include "drawing.h"
#include "scheduled_task_ui.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdio>

namespace {

int DaysInMonth(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 31;
    int d = days[month - 1];
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    if (month == 2 && leap) ++d;
    return d;
}

void ClampDay(ScheduledTaskTime& t) {
    const int maxDay = DaysInMonth(t.year, t.month);
    if (t.day < 1) t.day = 1;
    if (t.day > maxDay) t.day = maxDay;
}

}  // namespace

ScheduledDateTimePicker::~ScheduledDateTimePicker() {
    Detach();
}

void ScheduledDateTimePicker::Attach(HWND owner) {
    owner_ = owner;
    EnsureWindow();
}

void ScheduledDateTimePicker::Detach() {
    Hide();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (bodyFont_) {
        DeleteObject(bodyFont_);
        bodyFont_ = nullptr;
    }
    owner_ = nullptr;
}

bool ScheduledDateTimePicker::Visible() const {
    return hwnd_ && IsWindowVisible(hwnd_) == TRUE;
}

void ScheduledDateTimePicker::EnsureWindow() {
    if (hwnd_) return;
    static bool registered = false;
    const wchar_t* cls = L"QuickScriptScheduleDatePicker";
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &ScheduledDateTimePicker::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
        registered = true;
    }
    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        cls, L"", WS_POPUP,
        0, 0, kPickerW, kPickerH,
        nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!bodyFont_) {
        bodyFont_ = CreateFontW(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei");
    }
    if (hwnd_) outerShadow_.Attach(hwnd_);
    ShowWindow(hwnd_, SW_HIDE);
}

bool ScheduledDateTimePicker::Toggle(const RECT& anchorClient, Mode mode, ScheduledTaskTime& time) {
    EnsureWindow();
    if (Visible()) {
        const bool sameAnchor = anchor_.left == anchorClient.left && anchor_.top == anchorClient.top
            && anchor_.right == anchorClient.right && anchor_.bottom == anchorClient.bottom
            && mode_ == mode;
        Hide();
        if (sameAnchor) return false;
    }
    anchor_ = anchorClient;
    mode_ = mode;
    time_ = &time;
    accepted_ = false;
    BuildColumns(mode);
    ShowWindow(hwnd_, SW_SHOW);
    SyncPosition();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void ScheduledDateTimePicker::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
    hoverCol_ = hoverRow_ = -1;
}

void ScheduledDateTimePicker::SyncPosition() {
    if (!hwnd_ || !owner_) return;
    if (!IsWindowVisible(owner_)) {
        Hide();
        return;
    }
    if (!Visible()) return;
    POINT screenTop{anchor_.left, anchor_.bottom + 2};
    ClientToScreen(owner_, &screenTop);
    int x = screenTop.x;
    int y = screenTop.y;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (y + kPickerH > work.bottom) {
        POINT screenAnchor{anchor_.left, anchor_.top};
        ClientToScreen(owner_, &screenAnchor);
        y = screenAnchor.y - kPickerH - 2;
    }
    y = std::max(static_cast<int>(work.top),
        std::min(y, static_cast<int>(work.bottom) - kPickerH));
    x = std::max(static_cast<int>(work.left),
        std::min(x, static_cast<int>(work.right) - kPickerW));
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, kPickerW, kPickerH,
        SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
}

bool ScheduledDateTimePicker::ConsumeAccepted() {
    const bool ok = accepted_;
    accepted_ = false;
    return ok;
}

LRESULT CALLBACK ScheduledDateTimePicker::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ScheduledDateTimePicker* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<ScheduledDateTimePicker*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return TRUE;
    }
    self = reinterpret_cast<ScheduledDateTimePicker*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

void ScheduledDateTimePicker::BuildColumns(Mode mode) {
    columns_.clear();
    if (!time_) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    if (time_->year <= 0) time_->year = static_cast<int>(now.wYear);
    if (time_->month <= 0) time_->month = static_cast<int>(now.wMonth);
    if (time_->day <= 0) time_->day = static_cast<int>(now.wDay);
    ClampDay(*time_);

    auto addCol = [&](const wchar_t* suffix, int minV, int maxV, int val, bool circular, int step = 1) {
        Column c{};
        c.suffix = suffix;
        c.minValue = minV;
        c.maxValue = maxV;
        c.value = std::clamp(val, minV, maxV);
        c.step = step;
        c.circular = circular;
        columns_.push_back(c);
    };

    if (mode == Mode::Hourly) {
        addCol(L"分", 0, 59, time_->minute, true);
        addCol(L"秒", 0, 59, time_->second, true);
        addCol(L"毫秒", 0, 999, time_->millisecond, true, 1);
    } else if (mode == Mode::DailyWeekly) {
        addCol(L"时", 0, 23, time_->hour, true);
        addCol(L"分", 0, 59, time_->minute, true);
        addCol(L"秒", 0, 59, time_->second, true);
        addCol(L"毫秒", 0, 999, time_->millisecond, true, 1);
    } else {
        addCol(L"月", 1, 12, time_->month, true);
        addCol(L"日", 1, DaysInMonth(time_->year, time_->month), time_->day, true);
        addCol(L"时", 0, 23, time_->hour, true);
        addCol(L"分", 0, 59, time_->minute, true);
        addCol(L"秒", 0, 59, time_->second, true);
        addCol(L"毫秒", 0, 999, time_->millisecond, true, 1);
    }
}

void ScheduledDateTimePicker::ApplyToTime() {
    if (!time_ || columns_.empty()) return;
    if (mode_ == Mode::Hourly) {
        time_->minute = columns_[0].value;
        time_->second = columns_[1].value;
        time_->millisecond = columns_[2].value;
    } else if (mode_ == Mode::DailyWeekly) {
        time_->hour = columns_[0].value;
        time_->minute = columns_[1].value;
        time_->second = columns_[2].value;
        time_->millisecond = columns_[3].value;
    } else {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        if (time_->year <= 0) time_->year = static_cast<int>(now.wYear);
        time_->month = columns_[0].value;
        time_->day = columns_[1].value;
        time_->hour = columns_[2].value;
        time_->minute = columns_[3].value;
        time_->second = columns_[4].value;
        time_->millisecond = columns_[5].value;
        ClampDay(*time_);
    }
}

int ScheduledDateTimePicker::DisplayValue(const Column& col, int offset) const {
    int val = col.value + offset * col.step;
    if (col.circular) {
        const int range = col.maxValue - col.minValue + 1;
        val = col.minValue + ((val - col.minValue) % range + range) % range;
        return val;
    }
    return std::clamp(val, col.minValue, col.maxValue);
}

void ScheduledDateTimePicker::ScrollColumn(int col, int delta) {
    if (col < 0 || col >= static_cast<int>(columns_.size())) return;
    auto& c = columns_[static_cast<size_t>(col)];
    if (c.circular) {
        const int range = c.maxValue - c.minValue + 1;
        int v = c.value - c.minValue + delta * c.step;
        v = ((v % range) + range) % range;
        c.value = v + c.minValue;
    } else {
        c.value = std::clamp(c.value + delta * c.step, c.minValue, c.maxValue);
    }
    if (mode_ == Mode::Custom && col <= 1) {
        if (col == 0) {
            columns_[1].maxValue = DaysInMonth(time_->year, columns_[0].value);
            if (columns_[1].value > columns_[1].maxValue) columns_[1].value = columns_[1].maxValue;
            if (columns_[1].value < columns_[1].minValue) columns_[1].value = columns_[1].minValue;
        }
    }
    ApplyToTime();
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (owner_) InvalidateRect(owner_, nullptr, FALSE);
}

int ScheduledDateTimePicker::ColWidth() const {
    const int colCount = static_cast<int>(columns_.size());
    return colCount > 0 ? kPickerW / colCount : kColW;
}

int ScheduledDateTimePicker::HitColumn(int x) const {
    const int colCount = static_cast<int>(columns_.size());
    if (colCount <= 0) return -1;
    const int colW = ColWidth();
    const int totalW = colCount * colW;
    const int startX = (kPickerW - totalW) / 2;
    if (x < startX) return -1;
    const int idx = (x - startX) / colW;
    return idx >= 0 && idx < colCount ? idx : -1;
}

int ScheduledDateTimePicker::HitRowInColumn(int y) const {
    const int top = 48;
    if (y < top || y >= top + kVisibleRows * kRowH) return -1;
    return (y - top) / kRowH;
}

void ScheduledDateTimePicker::AcceptAndHide() {
    ApplyToTime();
    accepted_ = true;
    Hide();
    if (owner_) InvalidateRect(owner_, nullptr, FALSE);
}

LRESULT ScheduledDateTimePicker::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        const int col = HitColumn(x);
        const int row = col >= 0 ? HitRowInColumn(y) : -1;
        if (col != hoverCol_ || row != hoverRow_) {
            hoverCol_ = col;
            hoverRow_ = row;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        const int col = HitColumn(pt.x);
        if (col >= 0) ScrollColumn(col, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (y >= kPickerH - 42) {
            AcceptAndHide();
            return 0;
        }
        const int col = HitColumn(x);
        const int row = col >= 0 ? HitRowInColumn(y) : -1;
        if (col >= 0 && row >= 0) ScrollColumn(col, row - 2);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { Hide(); return 0; }
        if (wp == VK_RETURN) { AcceptAndHide(); return 0; }
        return 0;
    default:
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

void ScheduledDateTimePicker::Paint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT client{};
    GetClientRect(hwnd_, &client);
    FillRectColor(hdc, client, kWhite);
    DrawBorderRect(hdc, client, kComboPopupBorderGray);

    SelectObject(hdc, bodyFont_);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kHint);
    RECT hintRc{8, 8, kPickerW - 8, 36};
    DrawTextW(hdc, L"滚轮或点击调整", -1, &hintRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const int colCount = static_cast<int>(columns_.size());
    const int colW = ColWidth();
    const int totalW = colCount * colW;
    const int startX = (kPickerW - totalW) / 2;
    const int top = 48;
    RECT highlight{startX, top + 2 * kRowH, startX + totalW, top + 3 * kRowH};
    FillRectColor(hdc, highlight, kComboHoverGreen);

    for (int c = 0; c < colCount; ++c) {
        const int cx = startX + c * colW;
        const Column& col = columns_[static_cast<size_t>(c)];
        for (int r = 0; r < kVisibleRows; ++r) {
            const int offset = r - 2;
            const int val = DisplayValue(col, offset);
            wchar_t buf[32]{};
            swprintf_s(buf, L"%d%s", val, col.suffix.c_str());
            RECT rowRc{cx, top + r * kRowH, cx + colW, top + (r + 1) * kRowH};
            SetTextColor(hdc, r == 2 ? kMainGreen : kText);
            DrawTextW(hdc, buf, -1, &rowRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (c > 0) {
            HPEN pen = CreatePen(PS_SOLID, 1, kLineGreen);
            HGDIOBJ old = SelectObject(hdc, pen);
            MoveToEx(hdc, cx, top, nullptr);
            LineTo(hdc, cx, top + kVisibleRows * kRowH);
            SelectObject(hdc, old);
            DeleteObject(pen);
        }
    }

    RECT okRc{8, kPickerH - 38, kPickerW - 8, kPickerH - 8};
    StDrawGreenButton(hdc, bodyFont_, okRc, L"确定", false, true);
    EndPaint(hwnd_, &ps);
}
