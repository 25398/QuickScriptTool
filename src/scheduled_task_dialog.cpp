#include "scheduled_task_dialog.h"

#include "drawing.h"
#include "modern_edit.h"
#include "render_context.h"
#include "scheduled_task_datetime_picker.h"
#include "scheduled_task_ui.h"
#include "taskbar_window.h"
#include "utils.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdio>

namespace {

constexpr const wchar_t* kFreqLabels[] = {L"每小时", L"每日", L"每周", L"自定义"};
constexpr const wchar_t* kWeekLabels[] = {L"一", L"二", L"三", L"四", L"五", L"六", L"天"};

constexpr int kColWidths[] = {118, 88, 130, 72, 150, 88, 120};

constexpr UINT kMsgOpenCreate = WM_APP + 47;
constexpr UINT kMsgAcceptCreate = WM_APP + 48;
constexpr UINT kMsgRejectCreate = WM_APP + 49;

int ColumnLeft(int col) {
    int x = 12;
    for (int i = 0; i < col; ++i) x += kColWidths[i];
    return x;
}

ScheduledDateTimePicker::Mode PickerMode(ScheduledFrequency freq) {
    if (freq == ScheduledFrequency::Hourly) return ScheduledDateTimePicker::Mode::Hourly;
    if (freq == ScheduledFrequency::Custom) return ScheduledDateTimePicker::Mode::Custom;
    return ScheduledDateTimePicker::Mode::DailyWeekly;
}

}  // namespace

// ──────────────────────────────────────────────────────────────────
// Show (modal entry point)
// ──────────────────────────────────────────────────────────────────
void ScheduledTaskDialog::Show(HWND owner, ScheduledTaskScheduler& scheduler) {
    owner_ = owner;
    scheduler_ = &scheduler;
    done_ = false;
    scrollTop_ = 0;
    view_ = View::List;
    scheduler_->SetPaused(true);

    static bool registered = false;
    const wchar_t* cls = L"QuickScriptScheduleDlg";
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &ScheduledTaskDialog::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
        registered = true;
    }

    RECT ownerRc{};
    GetWindowRect(owner, &ownerRc);
    const int x = ownerRc.left + ((ownerRc.right - ownerRc.left) - kListW) / 2;
    const int y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - kDialogH) / 2;

    hwnd_ = CreateWindowExW(0, cls, L"", WS_POPUP | WS_CLIPCHILDREN | WS_MINIMIZEBOX,
        x, y, kListW, kDialogH, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return;

    ApplyTaskbarWindowStyle(hwnd_, L"鼠大侠-定时任务");
    outerShadow_.Attach(hwnd_);

    // 不 cloak 主窗：对话框小于主窗，cloak 会挖透明洞露出桌面/IDE。
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);

    MSG msg{};
    while (!done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            done_ = true;
            break;
        }
        if (!StModalMessageForDialog(msg, hwnd_, dropPopup_, timePicker_.Window())) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    scheduler_->SetPaused(false);

    if (IsWindow(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
        SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        DestroyWindow(hwnd_);
    }
    StDiscardSpuriousInputAfterModal(owner);
    if (IsWindow(owner)) {
        RedrawWindow(owner, nullptr, nullptr,
            RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        SetForegroundWindow(owner);
    }
}

// ──────────────────────────────────────────────────────────────────
// WndProc / Handle dispatch
// ──────────────────────────────────────────────────────────────────
LRESULT CALLBACK ScheduledTaskDialog::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ScheduledTaskDialog* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<ScheduledTaskDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return TRUE;
    }
    self = reinterpret_cast<ScheduledTaskDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT ScheduledTaskDialog::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    // ── Creation ──────────────────────────────────────────────
    case WM_CREATE: {
        titleFont_ = CreateFontW(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei");
        closeFont_ = CreateFontW(36, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        listFont_ = CreateFontW(23, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei");
        listSmallFont_ = CreateFontW(21, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei");
        createFont_ = CreateFontW(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei");
        nameEdit_ = MakeModernSingleLineEdit(hwnd_, L"", 100, 0, 0, 100, 24);
        SendMessageW(nameEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(createFont_), TRUE);
        ShowWindow(nameEdit_, SW_HIDE);

        static bool dropRegistered = false;
        if (!dropRegistered) {
            WNDCLASSW dropWc{};
            dropWc.lpfnWndProc = &ScheduledTaskDialog::DropPopupWndProc;
            dropWc.hInstance = GetModuleHandleW(nullptr);
            dropWc.lpszClassName = L"QuickScriptScheduleDrop";
            dropWc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            dropWc.hbrBackground = nullptr;
            RegisterClassW(&dropWc);
            dropRegistered = true;
        }
        dropPopup_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            L"QuickScriptScheduleDrop", L"", WS_POPUP,
            0, 0, kFieldW, 160, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetWindowLongPtrW(dropPopup_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        ShowWindow(dropPopup_, SW_HIDE);

        timePicker_.Attach(hwnd_);
        promptModal_.Bind(hwnd_, listFont_);
        return 0;
    }
    // ── Edit control background ───────────────────────────────
    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        SetBkColor(hdc, kWhite);
        SetTextColor(hdc, kText);
        static HBRUSH brush = CreateSolidBrush(kWhite);
        return reinterpret_cast<LRESULT>(brush);
    }
    // ── Non-client hit test ───────────────────────────────────
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        if (HitClose(pt.x, pt.y)) return HTCLIENT;
        if (HitTitle(pt.x, pt.y)) return HTCAPTION;
        return HTCLIENT;
    }
    // ── Background erase ──────────────────────────────────────
    case WM_ERASEBKGND:
        return 1;
    // ── Show window (create view popup management) ────────────
    case WM_SHOWWINDOW:
        if (!wp && view_ == View::Create) CloseAllPopups();
        return DefWindowProcW(hwnd_, msg, wp, lp);
    // ── Enter size move：保持弹层打开并在后续 MOVE 中跟锚点同步 ──
    case WM_ENTERSIZEMOVE:
        if (view_ == View::Create) SyncPopups();
        return 0;
    // ── Size (create view popup management) ───────────────────
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED && view_ == View::Create) CloseAllPopups();
        promptModal_.OnOwnerResize();
        return DefWindowProcW(hwnd_, msg, wp, lp);
    // ── Move / position changed (create view popups) ──────────
    case WM_MOVE:
    case WM_WINDOWPOSCHANGED:
        if (view_ == View::Create) {
            SyncPopups();
            if (timePicker_.ConsumeAccepted()) SyncTimePreview();
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    // ── Activate (create view popup management) ───────────────
    case WM_ACTIVATE:
        if (view_ == View::Create && LOWORD(wp) == WA_INACTIVE) {
            HWND fg = GetForegroundWindow();
            if (fg != hwnd_ && fg != dropPopup_ && fg != timePicker_.Window()) {
                CloseAllPopups();
            }
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    // ── Paint ─────────────────────────────────────────────────
    case WM_PAINT:
        Paint();
        return 0;
    // ── Mouse wheel (list view scroll) ────────────────────────
    case WM_MOUSEWHEEL:
        if (promptModal_.visible()) return 0;
        if (view_ == View::List) {
            const auto& tasks = scheduler_->Tasks();
            const RECT table = TableRect();
            const int visible = (table.bottom - table.top - kHeaderH) / kRowH;
            const int maxScroll = std::max(0, static_cast<int>(tasks.size()) - visible);
            scrollTop_ = std::clamp(scrollTop_ - (GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA), 0, maxScroll);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    // ── Mouse move ────────────────────────────────────────────
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (promptModal_.visible()) return 0;
        if (view_ == View::List) {
            const bool hc = StPtIn(CloseRect(), x, y);
            const bool hCreate = StPtIn(CreateBtnRect(), x, y);
            const bool hDis = StPtIn(DisableAllCheckboxRect(), x, y);
            const int row = HitRow(y);
            const int hEdit = row >= 0 && HitEdit(row, x, y) ? row : -1;
            const int hDel = row >= 0 && HitDelete(row, x, y) ? row : -1;
            if (hc != hoverClose_ || hCreate != hoverCreate_ || hDis != hoverDisableAll_
                || hEdit != hoverEditRow_ || hDel != hoverDeleteRow_) {
                hoverClose_ = hc;
                hoverCreate_ = hCreate;
                hoverDisableAll_ = hDis;
                hoverEditRow_ = hEdit;
                hoverDeleteRow_ = hDel;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        } else {
            const bool hc = HitClose(x, y);
            const bool hCan = PtIn(CancelBtnRect(), x, y);
            const bool hOk = PtIn(OkBtnRect(), x, y);
            const bool hFile = PtIn(RowFieldRect(2), x, y);
            const bool hTime = PtIn(RowFieldRect(4), x, y);
            if (hc != hoverClose_ || hCan != hoverCancel_ || hOk != hoverOk_
                || hFile != hoverFileCombo_ || hTime != hoverTimeCombo_) {
                hoverClose_ = hc;
                hoverCancel_ = hCan;
                hoverOk_ = hOk;
                hoverFileCombo_ = hFile;
                hoverTimeCombo_ = hTime;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return 0;
    }
    // ── Left button down ──────────────────────────────────────
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (promptModal_.visible()) return 0;
        if (view_ == View::List) {
            if (StPtIn(CloseRect(), x, y)) {
                PostMessageW(hwnd_, WM_CLOSE, 0, 0);
                return 0;
            }
            if (StPtIn(DisableAllCheckboxRect(), x, y)) {
                scheduler_->SetGlobalDisabled(!scheduler_->GlobalDisabled());
                scheduler_->Save();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (StPtIn(CreateBtnRect(), x, y)) {
                PostMessageW(hwnd_, kMsgOpenCreate, 0, -1);
                return 0;
            }
            const int row = HitRow(y);
            if (row >= 0) {
                if (HitEdit(row, x, y)) {
                    PostMessageW(hwnd_, kMsgOpenCreate, 0, row);
                    return 0;
                }
                if (HitDelete(row, x, y)) {
                    DeleteTaskAt(row);
                    return 0;
                }
            }
            return 0;
        } else {
            POINT pt{x, y};
            HWND child = ChildWindowFromPointEx(hwnd_, pt, CWP_SKIPINVISIBLE);
            if (child && child != hwnd_) return DefWindowProcW(hwnd_, msg, wp, lp);

            if (ClickOnPopup(x, y)) return 0;
            PopupKind toggledFile = PopupKind::None;
            if (DropPopupVisible()) {
                toggledFile = openPopup_;
                CloseFilePopup();
            }
            const bool timeWasOpen = timePicker_.Visible();
            if (timeWasOpen && !PtIn(RowFieldRect(4), x, y)) {
                timePicker_.Hide();
            }

            if (HitClose(x, y)) {
                PostMessageW(hwnd_, kMsgRejectCreate, 0, 0);
                return 0;
            }
            if (PtIn(CancelBtnRect(), x, y)) {
                PostMessageW(hwnd_, kMsgRejectCreate, 0, 0);
                return 0;
            }
            if (PtIn(OkBtnRect(), x, y)) {
                CloseAllPopups();
                PostMessageW(hwnd_, kMsgAcceptCreate, 0, 0);
                return 0;
            }
            for (int i = 0; i < 2; ++i) {
                if (PtIn(KindRadioRect(i), x, y)) {
                    CloseAllPopups();
                    task_.kind = i == 0 ? ScheduledTaskKind::Recording : ScheduledTaskKind::Macro;
                    RefreshFileList();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
            }
            for (int i = 0; i < 4; ++i) {
                if (PtIn(FreqRadioRect(i), x, y)) {
                    CloseAllPopups();
                    task_.frequency = static_cast<ScheduledFrequency>(i);
                    if (task_.frequency != ScheduledFrequency::Weekly) task_.time.weekDays = 0;
                    UpdateDefaultTime();
                    SyncTimePreview();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
            }
            if (task_.frequency == ScheduledFrequency::Weekly) {
                for (int i = 0; i < 7; ++i) {
                    if (PtIn(WeekDayRect(i), x, y)) {
                        CloseAllPopups();
                        SetWeekDay(task_.time.weekDays, i, !WeekDaySelected(task_.time.weekDays, i));
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        return 0;
                    }
                }
            }
            for (int i = 0; i < 2; ++i) {
                if (PtIn(StatusRadioRect(i), x, y)) {
                    CloseAllPopups();
                    task_.status = i == 0 ? ScheduledTaskStatus::Enabled : ScheduledTaskStatus::Disabled;
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
            }
            if (PtIn(RowFieldRect(2), x, y)) {
                if (toggledFile == PopupKind::FileSelect) {
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                timePicker_.Hide();
                ToggleFilePopup();
                return 0;
            }
            if (PtIn(RowFieldRect(4), x, y)) {
                CloseFilePopup();
                ToggleTimePicker();
                return 0;
            }
            CloseAllPopups();
            return 0;
        }
    }
    // ── Create view accept ────────────────────────────────────
    case kMsgAcceptCreate:
        if (ValidateAndCollect()) {
            auto& tasks = scheduler_->Tasks();
            if (editing_) {
                for (auto& t : tasks) {
                    if (t.id == task_.id) {
                        t = task_;
                        break;
                    }
                }
            } else {
                tasks.push_back(task_);
            }
            scheduler_->Save();
            ShowListView();
        }
        return 0;
    // ── Create view reject ────────────────────────────────────
    case kMsgRejectCreate:
        ShowListView();
        return 0;
    // ── Open create (from list view) ──────────────────────────
    case kMsgOpenCreate: {
        const int row = static_cast<int>(static_cast<LPARAM>(lp));
        if (row < 0) ShowCreateView();
        else ShowCreateView(&scheduler_->Tasks()[static_cast<size_t>(row)]);
        return 0;
    }
    // ── Close / Destroy ───────────────────────────────────────
    case WM_CLOSE:
        done_ = true;
        return 0;
    case WM_DESTROY:
        CloseAllPopups();
        timePicker_.Detach();
        CleanupGdi();
        return 0;
    default:
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

// ──────────────────────────────────────────────────────────────────
// Drop popup WndProc / Handle
// ──────────────────────────────────────────────────────────────────
LRESULT CALLBACK ScheduledTaskDialog::DropPopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ScheduledTaskDialog* self = reinterpret_cast<ScheduledTaskDialog*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->HandleDropPopup(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT ScheduledTaskDialog::HandleDropPopup(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(dropPopup_, &ps);
        PaintDropPopup(hdc);
        EndPaint(dropPopup_, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int y = GET_Y_LPARAM(lp);
        const int idx = std::clamp((y - 1) / kPopupItemH + popupScroll_, 0,
            static_cast<int>(fileItems_.size()) - 1);
        if (idx != popupHover_) {
            popupHover_ = idx;
            InvalidateRect(dropPopup_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int y = GET_Y_LPARAM(lp);
        const int idx = (y - 1) / kPopupItemH + popupScroll_;
        CloseFilePopup();
        if (idx >= 0 && idx < static_cast<int>(fileItems_.size())) {
            fileSel_ = idx;
            task_.filePath = filePaths_[static_cast<size_t>(idx)];
            task_.fileDisplayName = fileItems_[static_cast<size_t>(idx)];
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_SHOWWINDOW:
        if (!wp) ShowWindow(dropPopup_, SW_HIDE);
        return DefWindowProcW(dropPopup_, msg, wp, lp);
    default:
        return DefWindowProcW(dropPopup_, msg, wp, lp);
    }
}

// ──────────────────────────────────────────────────────────────────
// View switching
// ──────────────────────────────────────────────────────────────────
void ScheduledTaskDialog::ShowListView() {
    view_ = View::List;
    hoverClose_ = false;
    hoverCreate_ = false;
    hoverDisableAll_ = false;
    hoverEditRow_ = -1;
    hoverDeleteRow_ = -1;

    ShowWindow(nameEdit_, SW_HIDE);
    CloseAllPopups();

    RECT ownerRc{};
    GetWindowRect(owner_, &ownerRc);
    const int x = ownerRc.left + ((ownerRc.right - ownerRc.left) - kListW) / 2;
    const int y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - kDialogH) / 2;
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, kListW, kDialogH, SWP_NOCOPYBITS);

    // Clamp scroll if task count decreased
    const RECT table = TableRect();
    const int visible = (table.bottom - table.top - kHeaderH) / kRowH;
    const int maxScroll = std::max(0, static_cast<int>(scheduler_->Tasks().size()) - visible);
    scrollTop_ = std::min(scrollTop_, maxScroll);

    InvalidateRect(hwnd_, nullptr, TRUE);
}

void ScheduledTaskDialog::ShowCreateView(const ScheduledTask* editTask) {
    view_ = View::Create;
    editing_ = editTask != nullptr;
    hoverClose_ = false;
    hoverCancel_ = false;
    hoverOk_ = false;
    hoverFileCombo_ = false;
    hoverTimeCombo_ = false;

    if (editTask) {
        task_ = *editTask;
    } else {
        task_ = ScheduledTask{};
        task_.id = GenerateScheduledTaskId();
        task_.name = DefaultScheduledTaskName();
        task_.kind = ScheduledTaskKind::Macro;
        task_.frequency = ScheduledFrequency::Custom;
        task_.status = ScheduledTaskStatus::Enabled;
        UpdateDefaultTime();
    }
    SyncTimePreview();
    RefreshFileList();

    SetWindowTextW(nameEdit_, task_.name.c_str());

    RECT ownerRc{};
    GetWindowRect(owner_, &ownerRc);
    const int x = ownerRc.left + ((ownerRc.right - ownerRc.left) - kCreateW) / 2;
    const int y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - kDialogH) / 2;

    // Reposition name edit to match the create view field rect
    const RECT nameRc = RowFieldRect(0);
    SetWindowPos(nameEdit_, nullptr,
        nameRc.left, nameRc.top, nameRc.right - nameRc.left, nameRc.bottom - nameRc.top,
        SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(nameEdit_, SW_SHOW);
    EnableWindow(nameEdit_, TRUE);

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, kCreateW, kDialogH, SWP_NOCOPYBITS);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

// ──────────────────────────────────────────────────────────────────
// Painting
// ──────────────────────────────────────────────────────────────────
void ScheduledTaskDialog::Paint() {
    PAINTSTRUCT ps{};
    HDC windowDc = BeginPaint(hwnd_, &ps);
    if (!windowDc) return;

    const int dw = DialogW();
    HDC screenDc = GetDC(nullptr);
    HDC hdc = CreateCompatibleDC(screenDc);
    HBITMAP bmp = CreateCompatibleBitmap(screenDc, dw, kDialogH);
    HGDIOBJ oldBmp = SelectObject(hdc, bmp);
    RenderBatchScope batch(hdc);
    FillRectColor(hdc, RECT{0, 0, dw, kDialogH}, kWhite);

    if (view_ == View::List) {
        PaintList(hdc);
    } else {
        PaintCreate(hdc);
    }

    batch.End();
    BitBlt(windowDc, 0, 0, dw, kDialogH, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(hdc);
    ReleaseDC(nullptr, screenDc);
    EndPaint(hwnd_, &ps);
}

void ScheduledTaskDialog::PaintList(HDC hdc) {
    const int dw = kListW;

    StDrawTitleBar(hdc, titleFont_, closeFont_, dw, kTitleH,
        L"鼠大侠-定时任务", hoverClose_, RECT{dw - kCloseBtnW, 0, dw, kTitleH});

    SelectObject(hdc, listFont_);
    RECT listLabel{kMargin, kTitleH + 10, 120, kTitleH + kToolbarH};
    DrawTextIn(hdc, L"任务列表", listLabel, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    StDrawCheckbox(hdc, DisableAllCheckboxRect(), scheduler_->GlobalDisabled());
    RECT disableLabel{156, kTitleH + 8, 300, kTitleH + kToolbarH};
    DrawTextIn(hdc, L"禁用所有任务", disableLabel, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT pauseHint = PauseHintRect();
    DrawTextIn(hdc, L"任务编辑中，任务调度已暂停", pauseHint, kOrange,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    StDrawGreenButton(hdc, listFont_, CreateBtnRect(), L"任务创建", hoverCreate_);
    PaintTable(hdc);
}

void ScheduledTaskDialog::PaintTable(HDC hdc) {
    const RECT table = TableRect();
    FillRectColor(hdc, table, kWhite);
    DrawBorderRect(hdc, table, kLineGreen);

    RECT header{table.left, table.top, table.right, table.top + kHeaderH};
    FillRectColor(hdc, header, RGB(245, 245, 245));
    SelectObject(hdc, listFont_);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kText);
    const wchar_t* headers[] = {
        L"任务名称", L"任务类型", L"定时运行文件", L"运行频率", L"运行时间", L"状态", L"操作"};
    for (int c = 0; c < 7; ++c) {
        RECT col{ColumnLeft(c), header.top, ColumnLeft(c) + kColWidths[c], header.bottom};
        DrawTextW(hdc, headers[c], -1, &col, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (c > 0) {
            HPEN pen = CreatePen(PS_SOLID, 1, kLineGreen);
            HGDIOBJ old = SelectObject(hdc, pen);
            MoveToEx(hdc, col.left, header.top, nullptr);
            LineTo(hdc, col.left, table.bottom);
            SelectObject(hdc, old);
            DeleteObject(pen);
        }
    }
    HPEN linePen = CreatePen(PS_SOLID, 1, kLineGreen);
    HGDIOBJ oldPen = SelectObject(hdc, linePen);
    MoveToEx(hdc, table.left, header.bottom, nullptr);
    LineTo(hdc, table.right, header.bottom);

    const auto& tasks = scheduler_->Tasks();
    SelectObject(hdc, listSmallFont_);
    for (int i = scrollTop_; i < static_cast<int>(tasks.size()); ++i) {
        RECT row = RowRect(i);
        if (row.top >= table.bottom) break;
        MoveToEx(hdc, table.left, row.bottom, nullptr);
        LineTo(hdc, table.right, row.bottom);

        const auto& task = tasks[static_cast<size_t>(i)];
        const std::wstring typeText = ScheduledTaskKindLabel(task.kind);
        const std::wstring freqText = ScheduledFrequencyLabel(task.frequency);
        const std::wstring timeText = FormatScheduledRunTime(task);
        const bool disabled = task.status == ScheduledTaskStatus::Disabled;
        const std::wstring statusText = disabled ? L"已禁用" : L"等待";

        struct Cell { int col; std::wstring text; COLORREF color; };
        const Cell cells[] = {
            {0, task.name, kText},
            {1, typeText, kText},
            {2, task.fileDisplayName.empty() ? task.filePath : task.fileDisplayName, kText},
            {3, freqText, kText},
            {4, timeText, kText},
            {5, statusText, kHint},
        };
        for (const auto& cell : cells) {
            RECT rc{ColumnLeft(cell.col) + 4, row.top,
                ColumnLeft(cell.col) + kColWidths[cell.col] - 4, row.bottom};
            SetTextColor(hdc, cell.color);
            DrawTextW(hdc, cell.text.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        SetTextColor(hdc, kText);
        RECT editRc = EditBtnRect(i);
        DrawTextW(hdc, L"编辑", -1, &editRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(hdc, RGB(220, 60, 60));
        RECT delRc = DeleteBtnRect(i);
        DrawTextW(hdc, L"删除", -1, &delRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(linePen);
}

void ScheduledTaskDialog::PaintCreate(HDC hdc) {
    const int dw = kCreateW;

    StDrawTitleBar(hdc, titleFont_, closeFont_, dw, kTitleH,
        L"鼠大侠-定时任务创建", hoverClose_, CloseRect());

    SelectObject(hdc, createFont_);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kText);

    const wchar_t* labels[] = {L"任务名称：", L"任务类型：", L"文件选择：", L"任务频率：",
        L"运行时间：", L"任务状态："};
    for (int i = 0; i < 5; ++i) {
        RECT lr = RowLabelRect(i);
        DrawTextW(hdc, labels[i], -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    RECT statusLabel = RowLabelRect(StatusRowIndex());
    DrawTextW(hdc, labels[5], -1, &statusLabel, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    DrawBorderRect(hdc, RowFieldRect(0), kComboBorderGray);

    const wchar_t* kindLabels[] = {L"鼠标录制", L"鼠标宏"};
    for (int i = 0; i < 2; ++i) {
        StDrawRadio(hdc, KindRadioRect(i), task_.kind == (i == 0 ? ScheduledTaskKind::Recording : ScheduledTaskKind::Macro));
        RECT tr{KindRadioRect(i).right + 6, RowLabelRect(1).top, KindRadioRect(i).right + 100, RowLabelRect(1).bottom};
        DrawTextW(hdc, kindLabels[i], -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    const wchar_t* fileText = fileSel_ >= 0 && fileSel_ < static_cast<int>(fileItems_.size())
        ? fileItems_[static_cast<size_t>(fileSel_)].c_str() : L"";
    StDrawPanelCombo(hdc, createFont_, RowFieldRect(2), fileText, openPopup_ == PopupKind::FileSelect);

    for (int i = 0; i < 4; ++i) {
        StDrawRadio(hdc, FreqRadioRect(i), static_cast<int>(task_.frequency) == i);
        RECT tr{FreqRadioRect(i).right + 4, RowLabelRect(3).top, FreqRadioRect(i).right + 80, RowLabelRect(3).bottom};
        DrawTextW(hdc, kFreqLabels[i], -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    StDrawPanelCombo(hdc, createFont_, RowFieldRect(4), timePreview_.c_str(), timePicker_.Visible());

    if (task_.frequency == ScheduledFrequency::Weekly) {
        for (int i = 0; i < 7; ++i) {
            RECT dayRc = WeekDayRect(i);
            const bool sel = WeekDaySelected(task_.time.weekDays, i);
            if (sel) FillRectColor(hdc, dayRc, kMainGreen);
            else DrawBorderRect(hdc, dayRc, kComboBorderGray);
            SetTextColor(hdc, sel ? kWhite : kText);
            DrawTextW(hdc, kWeekLabels[i], -1, &dayRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    for (int i = 0; i < 2; ++i) {
        const RECT rc = StatusRadioRect(i);
        const bool checked = (i == 0 && task_.status == ScheduledTaskStatus::Enabled)
            || (i == 1 && task_.status == ScheduledTaskStatus::Disabled);
        StDrawRadio(hdc, rc, checked);
        RECT tr{rc.right + 6, rc.top - 8, rc.right + 60, rc.bottom + 8};
        DrawTextW(hdc, i == 0 ? L"启用" : L"禁用", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    StDrawGreenButton(hdc, createFont_, CancelBtnRect(), L"取消", hoverCancel_);
    StDrawGreenButton(hdc, createFont_, OkBtnRect(), L"确定", hoverOk_);
}

void ScheduledTaskDialog::PaintDropPopup(HDC hdc) {
    RECT client{};
    GetClientRect(dropPopup_, &client);
    FillRectColor(hdc, client, kWhite);
    DrawBorderRect(hdc, client, kComboPopupBorderGray);
    SelectObject(hdc, createFont_);
    SetBkMode(hdc, TRANSPARENT);
    const int visible = (client.bottom - client.top - 2) / kPopupItemH;
    for (int vis = 0; vis < visible; ++vis) {
        const int i = vis + popupScroll_;
        if (i >= static_cast<int>(fileItems_.size())) break;
        RECT row{client.left + 1, client.top + 1 + vis * kPopupItemH,
            client.right - 1, client.top + 1 + (vis + 1) * kPopupItemH};
        const bool selected = i == fileSel_;
        const bool hovered = popupHover_ == i;
        FillRectColor(hdc, row, selected ? kComboMenuSelectBlue : (hovered ? kComboMenuHoverBlue : kWhite));
        SetTextColor(hdc, selected ? kComboMenuSelectText : kText);
        RECT textRc{row.left + 10, row.top, row.right - 6, row.bottom};
        DrawTextW(hdc, fileItems_[static_cast<size_t>(i)].c_str(), -1, &textRc,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

// ──────────────────────────────────────────────────────────────────
// List view rect helpers
// ──────────────────────────────────────────────────────────────────
RECT ScheduledTaskDialog::CreateBtnRect() const {
    return RECT{kListW - kMargin - 108, kTitleH + 8, kListW - kMargin, kTitleH + kToolbarH - 8};
}

RECT ScheduledTaskDialog::PauseHintRect() const {
    return RECT{310, kTitleH + 8, kListW - kMargin - 116, kTitleH + kToolbarH - 8};
}

RECT ScheduledTaskDialog::DisableAllCheckboxRect() const {
    return RECT{130, kTitleH + 14, 130 + kCheckboxSize, kTitleH + 14 + kCheckboxSize};
}

RECT ScheduledTaskDialog::TableRect() const {
    return RECT{kMargin, kTitleH + kToolbarH, kListW - kMargin, kDialogH - kMargin};
}

RECT ScheduledTaskDialog::RowRect(int index) const {
    const RECT table = TableRect();
    return RECT{table.left, table.top + kHeaderH + (index - scrollTop_) * kRowH,
        table.right, table.top + kHeaderH + (index - scrollTop_ + 1) * kRowH};
}

RECT ScheduledTaskDialog::EditBtnRect(int row) const {
    RECT r = RowRect(row);
    return RECT{r.right - 110, r.top + 8, r.right - 62, r.bottom - 8};
}

RECT ScheduledTaskDialog::DeleteBtnRect(int row) const {
    RECT r = RowRect(row);
    return RECT{r.right - 56, r.top + 8, r.right - 8, r.bottom - 8};
}

int ScheduledTaskDialog::HitRow(int y) const {
    const RECT table = TableRect();
    if (y < table.top + kHeaderH || y >= table.bottom) return -1;
    const int idx = scrollTop_ + (y - table.top - kHeaderH) / kRowH;
    if (idx < 0 || idx >= static_cast<int>(scheduler_->Tasks().size())) return -1;
    return idx;
}

bool ScheduledTaskDialog::HitEdit(int row, int x, int y) const {
    return StPtIn(EditBtnRect(row), x, y);
}

bool ScheduledTaskDialog::HitDelete(int row, int x, int y) const {
    return StPtIn(DeleteBtnRect(row), x, y);
}

void ScheduledTaskDialog::DeleteTaskAt(int index) {
    auto& tasks = scheduler_->Tasks();
    if (index < 0 || index >= static_cast<int>(tasks.size())) return;
    tasks.erase(tasks.begin() + index);
    scheduler_->Save();
    const RECT table = TableRect();
    const int visible = (table.bottom - table.top - kHeaderH) / kRowH;
    scrollTop_ = std::min(scrollTop_, std::max(0, static_cast<int>(tasks.size()) - visible));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ──────────────────────────────────────────────────────────────────
// Create view rect helpers
// ──────────────────────────────────────────────────────────────────
RECT ScheduledTaskDialog::CloseRect() const {
    const int dw = DialogW();
    return RECT{dw - kCloseBtnW, 0, dw, kTitleH};
}

RECT ScheduledTaskDialog::RowLabelRect(int row) const {
    return RECT{16, kFirstRowY + row * kCreateRowH, 16 + kLabelW, kFirstRowY + row * kCreateRowH + kCreateRowH};
}

RECT ScheduledTaskDialog::RowFieldRect(int row) const {
    return RECT{kFieldLeft, kFirstRowY + row * kCreateRowH + 4, kFieldLeft + kFieldW,
        kFirstRowY + row * kCreateRowH + 4 + kComboH};
}

RECT ScheduledTaskDialog::KindRadioRect(int index) const {
    const int y = kFirstRowY + kCreateRowH + (kCreateRowH - kRadioSize) / 2;
    return RECT{kFieldLeft + index * 130, y, kFieldLeft + index * 130 + kRadioSize, y + kRadioSize};
}

RECT ScheduledTaskDialog::FreqRadioRect(int index) const {
    const int y = kFirstRowY + 3 * kCreateRowH + (kCreateRowH - kRadioSize) / 2;
    return RECT{kFieldLeft + index * 88, y, kFieldLeft + index * 88 + kRadioSize, y + kRadioSize};
}

int ScheduledTaskDialog::StatusRowIndex() const {
    return task_.frequency == ScheduledFrequency::Weekly ? 6 : 5;
}

RECT ScheduledTaskDialog::StatusRadioRect(int index) const {
    const int row = StatusRowIndex();
    const int y = kFirstRowY + row * kCreateRowH + (kCreateRowH - kRadioSize) / 2;
    return RECT{kFieldLeft + index * 100, y, kFieldLeft + index * 100 + kRadioSize, y + kRadioSize};
}

RECT ScheduledTaskDialog::WeekDayRect(int index) const {
    const int y = kFirstRowY + 5 * kCreateRowH + 6;
    return RECT{kFieldLeft + index * 44, y, kFieldLeft + index * 44 + 36, y + 28};
}

RECT ScheduledTaskDialog::CancelBtnRect() const {
    return RECT{kCreateW - 196, kDialogH - 52, kCreateW - 108, kDialogH - 16};
}

RECT ScheduledTaskDialog::OkBtnRect() const {
    return RECT{kCreateW - 96, kDialogH - 52, kCreateW - 16, kDialogH - 16};
}

// ──────────────────────────────────────────────────────────────────
// Create view: file picker popup
// ──────────────────────────────────────────────────────────────────
void ScheduledTaskDialog::RefreshFileList() {
    fileItems_.clear();
    filePaths_.clear();
    const std::wstring dir = task_.kind == ScheduledTaskKind::Macro ? ScriptsDir() : RecordingsDir();
    const std::wstring pattern = dir + L"\\*.json";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring path = dir + L"\\" + fd.cFileName;
            const std::wstring content = ReadAll(path);
            std::wstring name = ExtractString(content, L"scriptName");
            if (name.empty()) {
                name = fd.cFileName;
                const auto dot = name.rfind(L'.');
                if (dot != std::wstring::npos) name.erase(dot);
            }
            fileItems_.push_back(name);
            filePaths_.push_back(path);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    fileSel_ = -1;
    if (!task_.filePath.empty()) {
        for (size_t i = 0; i < filePaths_.size(); ++i) {
            if (filePaths_[i] == task_.filePath) {
                fileSel_ = static_cast<int>(i);
                break;
            }
        }
    }
    if (fileSel_ < 0 && !fileItems_.empty()) fileSel_ = 0;
}

void ScheduledTaskDialog::UpdateDefaultTime() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    if (!editing_ || task_.time.year <= 0) {
        task_.time.year = static_cast<int>(now.wYear);
        task_.time.month = static_cast<int>(now.wMonth);
        task_.time.day = static_cast<int>(now.wDay);
    }
    if (task_.frequency == ScheduledFrequency::Hourly) {
        task_.time.hour = 0;
    }
}

void ScheduledTaskDialog::SyncTimePreview() {
    ScheduledTask preview = task_;
    timePreview_ = FormatScheduledRunTime(preview);
}

void ScheduledTaskDialog::SyncPopups() {
    if (!IsWindowVisible(hwnd_)) {
        CloseAllPopups();
        return;
    }
    if (openPopup_ == PopupKind::FileSelect && dropPopup_) {
        POINT screenTop{filePopupAnchor_.left, filePopupAnchor_.bottom + 2};
        ClientToScreen(hwnd_, &screenTop);
        const int itemCount = std::max(1, static_cast<int>(fileItems_.size()));
        const int h = std::min(240, itemCount * kPopupItemH + 2);
        int y = screenTop.y;
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        if (y + h > work.bottom) {
            POINT screenAnchor{filePopupAnchor_.left, filePopupAnchor_.top};
            ClientToScreen(hwnd_, &screenAnchor);
            y = screenAnchor.y - h - 2;
        }
        y = std::max(static_cast<int>(work.top),
            std::min(y, static_cast<int>(work.bottom) - h));
        SetWindowPos(dropPopup_, HWND_TOPMOST, screenTop.x, y, kFieldW, h,
            SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
    } else if (dropPopup_) {
        ShowWindow(dropPopup_, SW_HIDE);
    }
    if (timePicker_.Visible()) timePicker_.SyncPosition();
}

bool ScheduledTaskDialog::DropPopupVisible() const {
    return dropPopup_ && IsWindowVisible(dropPopup_) == TRUE;
}

void ScheduledTaskDialog::ToggleFilePopup() {
    if (fileItems_.empty()) {
        ShowAlert(L"当前类型下没有可用文件。");
        return;
    }
    if (DropPopupVisible()) {
        CloseFilePopup();
        return;
    }
    openPopup_ = PopupKind::FileSelect;
    filePopupAnchor_ = RowFieldRect(2);
    popupHover_ = fileSel_;
    popupScroll_ = 0;
    SyncPopups();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ScheduledTaskDialog::ToggleTimePicker() {
    if (timePicker_.Toggle(RowFieldRect(4), PickerMode(task_.frequency), task_.time)) {
        CloseFilePopup();
    }
    if (timePicker_.ConsumeAccepted()) SyncTimePreview();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ScheduledTaskDialog::CloseFilePopup() {
    if (dropPopup_) ShowWindow(dropPopup_, SW_HIDE);
    openPopup_ = PopupKind::None;
    popupHover_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ScheduledTaskDialog::CloseAllPopups() {
    CloseFilePopup();
    timePicker_.Hide();
}

bool ScheduledTaskDialog::ClickOnPopup(int x, int y) const {
    POINT screenPt{x, y};
    ClientToScreen(hwnd_, &screenPt);
    if (DropPopupVisible()) {
        RECT rc{};
        GetWindowRect(dropPopup_, &rc);
        if (PtInRect(&rc, screenPt)) return true;
    }
    if (timePicker_.Visible() && timePicker_.Window()) {
        RECT rc{};
        GetWindowRect(timePicker_.Window(), &rc);
        if (PtInRect(&rc, screenPt)) return true;
    }
    return false;
}

bool ScheduledTaskDialog::ValidateAndCollect() {
    wchar_t buf[256]{};
    GetWindowTextW(nameEdit_, buf, 255);
    task_.name = Trim(buf);
    if (task_.name.empty()) {
        ShowAlert(L"请输入任务名称。");
        return false;
    }
    if (fileSel_ < 0 || fileSel_ >= static_cast<int>(filePaths_.size())) {
        ShowAlert(L"请选择要运行的文件。");
        return false;
    }
    task_.filePath = filePaths_[static_cast<size_t>(fileSel_)];
    task_.fileDisplayName = fileItems_[static_cast<size_t>(fileSel_)];
    if (task_.frequency == ScheduledFrequency::Weekly && task_.time.weekDays == 0) {
        ShowAlert(L"请至少选择一个星期。");
        return false;
    }
    if (task_.frequency == ScheduledFrequency::Custom) {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        SYSTEMTIME target{};
        target.wYear = static_cast<WORD>(task_.time.year);
        target.wMonth = static_cast<WORD>(task_.time.month);
        target.wDay = static_cast<WORD>(task_.time.day);
        target.wHour = static_cast<WORD>(task_.time.hour);
        target.wMinute = static_cast<WORD>(task_.time.minute);
        target.wSecond = static_cast<WORD>(task_.time.second);
        target.wMilliseconds = static_cast<WORD>(task_.time.millisecond);
        FILETIME ftNow{}, ftTarget{};
        SystemTimeToFileTime(&now, &ftNow);
        SystemTimeToFileTime(&target, &ftTarget);
        if (CompareFileTime(&ftTarget, &ftNow) < 0 && !editing_) {
            ShowAlert(L"自定义运行时间不能早于当前时间。");
            return false;
        }
        if (CompareFileTime(&ftTarget, &ftNow) >= 0) task_.customFired = false;
    } else if (!editing_) {
        task_.customFired = false;
    }
    return true;
}

void ScheduledTaskDialog::ShowAlert(const wchar_t* msg) {
    CloseAllPopups();
    promptModal_.ShowInfo(msg ? msg : L"");
}

bool ScheduledTaskDialog::HitClose(int x, int y) const {
    return PtIn(CloseRect(), x, y);
}

bool ScheduledTaskDialog::HitTitle(int /*x*/, int y) const {
    return y >= 0 && y < kTitleH;
}

bool ScheduledTaskDialog::PtIn(const RECT& rc, int x, int y) const {
    return StPtIn(rc, x, y);
}

// ──────────────────────────────────────────────────────────────────
// Cleanup
// ──────────────────────────────────────────────────────────────────
void ScheduledTaskDialog::CleanupGdi() {
    if (titleFont_) DeleteObject(titleFont_);
    if (closeFont_) DeleteObject(closeFont_);
    if (listFont_) DeleteObject(listFont_);
    if (listSmallFont_) DeleteObject(listSmallFont_);
    if (createFont_) DeleteObject(createFont_);
    titleFont_ = closeFont_ = listFont_ = listSmallFont_ = createFont_ = nullptr;
}
