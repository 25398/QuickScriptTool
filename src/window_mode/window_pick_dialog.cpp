#include "window_pick_dialog.h"

#include "config.h"
#include "controls.h"
#include "drawing.h"
#include "modern_edit.h"
#include "render_context.h"
#include "scheduled_task_ui.h"
#include "taskbar_window.h"
#include "utils.h"

#include <windowsx.h>
#include <commctrl.h>

namespace windowmode {

namespace {

constexpr wchar_t kDlgClass[] = L"QuickScriptWindowPickDlg";

}  // namespace

bool WindowPickDialog::Show(HWND owner, WindowPickResult& inOut) {
    owner_ = owner;
    result_ = &inOut;
    done_ = false;
    accepted_ = false;

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &WindowPickDialog::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kDlgClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
        registered = true;
    }

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = work.left + ((work.right - work.left) - kDlgW) / 2;
    int y = work.top + ((work.bottom - work.top) - kDlgH) / 2;
    if (owner && IsWindow(owner) && IsWindowVisible(owner) && !IsIconic(owner)) {
        RECT ownerRc{};
        GetWindowRect(owner, &ownerRc);
        x = ownerRc.left + ((ownerRc.right - ownerRc.left) - kDlgW) / 2;
        y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - kDlgH) / 2;
    }

    // 不要用隐藏的主窗口做 owner，否则弹窗可能不显示。
    HWND createOwner = (owner && IsWindow(owner) && IsWindowVisible(owner) && !IsIconic(owner))
        ? owner : nullptr;

    hwnd_ = CreateWindowExW(WS_EX_TOPMOST, kDlgClass, L"",
        WS_POPUP | WS_CLIPCHILDREN,
        x, y, kDlgW, kDlgH, createOwner, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    ApplyTaskbarWindowStyle(hwnd_, L"鼠大侠-请选择窗口");
    outerShadow_.Attach(hwnd_);
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd_);
    if (owner && IsWindow(owner)) EnableWindow(owner, FALSE);

    MSG msg{};
    while (!done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            done_ = true;
            break;
        }
        if (!StModalMessageForDialog(msg, hwnd_, nullptr, nullptr, owner)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (owner && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        if (IsWindowVisible(owner)) SetForegroundWindow(owner);
    }
    StDiscardSpuriousInputAfterModal(owner);

    if (IsWindow(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    result_ = nullptr;

    inOut.accepted = accepted_;
    return accepted_;
}

LRESULT CALLBACK WindowPickDialog::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WindowPickDialog* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<WindowPickDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return TRUE;
    }
    self = reinterpret_cast<WindowPickDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK WindowPickDialog::CrosshairSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR, DWORD_PTR refData) {
    if (msg == WM_LBUTTONDOWN) {
        auto* self = reinterpret_cast<WindowPickDialog*>(refData);
        if (self) {
            CrosshairDragBinding binding{};
            if (self->crosshairDrag_.TryGetBinding(hwnd, binding)) {
                self->crosshairDrag_.Begin(binding);
                return 0;
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void WindowPickDialog::OnCreate() {
    titleFont_ = CreateFontW(22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
    bodyFont_ = CreateFontW(26, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
    btnFont_ = CreateFontW(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
    closeFont_ = CreateFontW(32, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
    crosshairCursor_ = CreateCrosshairDragCursor(kCrosshairBlue);

    crosshairBtn_ = MakeGreenButton(hwnd_, L"拖动准星到目标窗口",
        static_cast<int>(Id::Crosshair), 0, 0, 10, kCrosshairH);
    pickXEdit_ = MakeModernSingleLineEdit(hwnd_, L"0", static_cast<int>(Id::PickX), 0, 0, 10, kEditH);
    pickYEdit_ = MakeModernSingleLineEdit(hwnd_, L"0", static_cast<int>(Id::PickY), 0, 0, 10, kEditH);
    titleEdit_ = MakeModernSingleLineEdit(hwnd_, L"", static_cast<int>(Id::WindowTitle), 0, 0, 10, kEditH);
    classEdit_ = MakeModernSingleLineEdit(hwnd_, L"", static_cast<int>(Id::WindowClass), 0, 0, 10, kEditH);
    childClassEdit_ = MakeModernSingleLineEdit(hwnd_, L"", static_cast<int>(Id::ChildClass), 0, 0, 10, kEditH);
    pathEdit_ = MakeModernSingleLineEdit(hwnd_, L"", static_cast<int>(Id::ProcessPath), 0, 0, 10, kEditH);

    for (HWND h : {crosshairBtn_, pickXEdit_, pickYEdit_, titleEdit_, classEdit_, childClassEdit_, pathEdit_}) {
        if (h && bodyFont_) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
    }

    crosshairDrag_.SetOwner(hwnd_);
    crosshairDrag_.SetDragCursor(crosshairCursor_);
    crosshairDrag_.RegisterButton(crosshairBtn_, {CrosshairDragMode::Coordinates, nullptr});
    SetWindowSubclass(crosshairBtn_, CrosshairSubclassProc, 0, reinterpret_cast<DWORD_PTR>(this));

    if (result_) {
        SetWindowTextW(pickXEdit_, std::to_wstring(result_->pickX).c_str());
        SetWindowTextW(pickYEdit_, std::to_wstring(result_->pickY).c_str());
        SetWindowTextW(titleEdit_, result_->windowTitle.c_str());
        SetWindowTextW(classEdit_, result_->windowClassName.c_str());
        SetWindowTextW(childClassEdit_, result_->childWindowClassName.c_str());
        if (pathEdit_) SetWindowTextW(pathEdit_, result_->processPath.c_str());
    }

    PositionControls();
}

void WindowPickDialog::PositionControls() {
    const int crossTop = kTitleH + 16;
    const int contentLeft = kMargin + kLabelW + 10;
    const int contentW = kDlgW - contentLeft - kMargin;
    const int halfGap = 10;
    const int halfW = (contentW - halfGap) / 2;

    if (crosshairBtn_) {
        MoveWindow(crosshairBtn_, kMargin, crossTop, kDlgW - kMargin * 2, kCrosshairH, FALSE);
    }

    const int row0 = crossTop + kCrosshairH + 18;
    const int row1 = row0 + kRowH + kRowGap;
    const int row2 = row1 + kRowH + kRowGap;
    const int row3 = row2 + kRowH + kRowGap;
    const int row4 = row3 + kRowH + kRowGap;

    if (pickXEdit_) MoveWindow(pickXEdit_, contentLeft, row0, halfW, kEditH, FALSE);
    if (pickYEdit_) MoveWindow(pickYEdit_, contentLeft + halfW + halfGap, row0, halfW, kEditH, FALSE);
    if (titleEdit_) MoveWindow(titleEdit_, contentLeft, row1, contentW, kEditH, FALSE);
    if (classEdit_) MoveWindow(classEdit_, contentLeft, row2, contentW, kEditH, FALSE);
    if (childClassEdit_) MoveWindow(childClassEdit_, contentLeft, row3, contentW, kEditH, FALSE);
    if (pathEdit_) MoveWindow(pathEdit_, contentLeft, row4, contentW, kEditH, FALSE);
}

void WindowPickDialog::ApplyPickInfo(const WindowInfoFromPoint& info) {
    if (pickXEdit_) SetWindowTextW(pickXEdit_, std::to_wstring(info.x).c_str());
    if (pickYEdit_) SetWindowTextW(pickYEdit_, std::to_wstring(info.y).c_str());
    if (titleEdit_) SetWindowTextW(titleEdit_, info.windowTitle.c_str());
    if (classEdit_) SetWindowTextW(classEdit_, info.windowClassName.c_str());
    if (childClassEdit_) SetWindowTextW(childClassEdit_, info.childWindowClassName.c_str());
    if (pathEdit_) SetWindowTextW(pathEdit_, info.processPath.c_str());
    if (result_) {
        if (!info.processPath.empty()) result_->processPath = info.processPath;
        if (!info.documentPath.empty()) result_->documentPath = info.documentPath;
    }
}

void WindowPickDialog::SyncFieldsToResult() {
    if (!result_) return;
    result_->pickX = pickXEdit_ ? _wtoi(GetText(pickXEdit_).c_str()) : 0;
    result_->pickY = pickYEdit_ ? _wtoi(GetText(pickYEdit_).c_str()) : 0;
    result_->windowTitle = titleEdit_ ? Trim(GetText(titleEdit_)) : L"";
    result_->windowClassName = classEdit_ ? Trim(GetText(classEdit_)) : L"";
    result_->childWindowClassName = childClassEdit_ ? Trim(GetText(childClassEdit_)) : L"";
    result_->processPath = pathEdit_ ? Trim(GetText(pathEdit_)) : L"";
    if (result_->pickX != 0 || result_->pickY != 0) {
        const auto info = GetWindowInfoFromPoint(result_->pickX, result_->pickY);
        if (!info.processPath.empty()) {
            result_->processPath = info.processPath;
            if (pathEdit_) SetWindowTextW(pathEdit_, info.processPath.c_str());
        }
        if (!info.documentPath.empty()) result_->documentPath = info.documentPath;
    }
}

void WindowPickDialog::CleanupGdi() {
    outerShadow_.Detach();
    if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
    if (bodyFont_) { DeleteObject(bodyFont_); bodyFont_ = nullptr; }
    if (btnFont_) { DeleteObject(btnFont_); btnFont_ = nullptr; }
    if (closeFont_) { DeleteObject(closeFont_); closeFont_ = nullptr; }
    if (crosshairCursor_) { DestroyCursor(crosshairCursor_); crosshairCursor_ = nullptr; }
}

void WindowPickDialog::Close(bool accepted) {
    if (crosshairDrag_.IsActive()) crosshairDrag_.End();
    if (accepted) SyncFieldsToResult();
    accepted_ = accepted;
    done_ = true;
}

bool WindowPickDialog::PtIn(const RECT& rc, int x, int y) const {
    return StPtIn(rc, x, y);
}

RECT WindowPickDialog::CloseRect() const {
    return RECT{kDlgW - kCloseBtnW - 4, 0, kDlgW, kTitleH};
}

RECT WindowPickDialog::CrosshairRect() const {
    return RECT{kMargin, kTitleH + 16, kDlgW - kMargin, kTitleH + 16 + kCrosshairH};
}

RECT WindowPickDialog::OkBtnRect() const {
    const int top = kFooterTop + (kFooterH - kBtnH) / 2;
    return RECT{kDlgW - kMargin - kOkBtnW, top, kDlgW - kMargin, top + kBtnH};
}

RECT WindowPickDialog::CancelBtnRect() const {
    const RECT ok = OkBtnRect();
    return RECT{ok.left - kBtnGap - kCancelBtnW, ok.top, ok.left - kBtnGap, ok.bottom};
}

RECT WindowPickDialog::FieldLabelRect(int row) const {
    const int crossTop = kTitleH + 16;
    const int top = crossTop + kCrosshairH + 18 + row * (kRowH + kRowGap);
    return RECT{kMargin, top, kMargin + kLabelW, top + kRowH};
}

RECT WindowPickDialog::FieldEditRect(int row) const {
    const RECT label = FieldLabelRect(row);
    const int left = kMargin + kLabelW + 10;
    return RECT{left, label.top, kDlgW - kMargin, label.top + kEditH};
}

RECT WindowPickDialog::PickXEditRect() const {
    const RECT row = FieldEditRect(0);
    const int halfGap = 10;
    const int halfW = (row.right - row.left - halfGap) / 2;
    return RECT{row.left, row.top, row.left + halfW, row.bottom};
}

RECT WindowPickDialog::PickYEditRect() const {
    const RECT row = FieldEditRect(0);
    const int halfGap = 10;
    const int halfW = (row.right - row.left - halfGap) / 2;
    return RECT{row.left + halfW + halfGap, row.top, row.right, row.bottom};
}

bool WindowPickDialog::HitClose(int x, int y) const {
    return PtIn(CloseRect(), x, y);
}

bool WindowPickDialog::HitTitle(int x, int y) const {
    return y >= 0 && y < kTitleH && x >= 0 && x < kDlgW - kCloseBtnW;
}

void WindowPickDialog::DrawOutlineButton(HDC hdc, const RECT& rc, const wchar_t* text, bool hover) const {
    DrawBorderRoundRect(hdc, rc, hover ? kMainGreen : kComboBorderGray, 6);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kMainGreen);
    SelectObject(hdc, btnFont_);
    DrawTextW(hdc, text, -1, const_cast<RECT*>(&rc), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void WindowPickDialog::PaintFieldBorders(HDC hdc) const {
    auto border = [&](HWND h) {
        if (!h || !IsWindowVisible(h)) return;
        RECT rc{};
        GetWindowRect(h, &rc);
        MapWindowPoints(HWND_DESKTOP, hwnd_, reinterpret_cast<POINT*>(&rc), 2);
        InflateRect(&rc, 1, 1);
        DrawBorderRect(hdc, rc, kComboBorderGray);
    };
    border(pickXEdit_);
    border(pickYEdit_);
    border(titleEdit_);
    border(classEdit_);
    border(childClassEdit_);
    border(pathEdit_);
}

void WindowPickDialog::OnPaint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT client{};
    GetClientRect(hwnd_, &client);
    FillRectColor(hdc, client, kWhite);

    StDrawTitleBar(hdc, titleFont_, closeFont_, kDlgW, kTitleH,
        L"鼠大侠-请选择窗口", hoverClose_, CloseRect());

    FillRectColor(hdc, RECT{0, kFooterTop, kDlgW, kFooterTop + 1}, RGB(230, 230, 230));

    SelectObject(hdc, bodyFont_);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextIn(hdc, L"横坐标(x)", FieldLabelRect(0), kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    {
        const RECT yEdit = PickYEditRect();
        DrawTextIn(hdc, L"纵坐标(y)", RECT{yEdit.left - 86, yEdit.top, yEdit.left - 8, yEdit.bottom},
            kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    DrawTextIn(hdc, L"窗口名称", FieldLabelRect(1), kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"窗口类名", FieldLabelRect(2), kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"子窗口类名", FieldLabelRect(3), kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"程序路径", FieldLabelRect(4), kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    PaintFieldBorders(hdc);
    DrawOutlineButton(hdc, CancelBtnRect(), L"取消选择", hoverCancel_);
    StDrawGreenButton(hdc, btnFont_, OkBtnRect(), L"使用选择的窗口类", hoverOk_);

    EndPaint(hwnd_, &ps);
}

void WindowPickDialog::UpdateHover(int x, int y) {
    const bool hClose = HitClose(x, y);
    const bool hCancel = PtIn(CancelBtnRect(), x, y);
    const bool hOk = PtIn(OkBtnRect(), x, y);
    const bool hCross = crosshairDrag_.HitButton(x, y, [this](HWND btn) {
        RECT rc{};
        GetWindowRect(btn, &rc);
        POINT tl{rc.left, rc.top};
        ScreenToClient(hwnd_, &tl);
        return RECT{tl.x, tl.y, tl.x + (rc.right - rc.left), tl.y + (rc.bottom - rc.top)};
    }) != nullptr;
    if (hClose != hoverClose_ || hCancel != hoverCancel_ || hOk != hoverOk_ || hCross != hoverCrosshair_) {
        hoverClose_ = hClose;
        hoverCancel_ = hCancel;
        hoverOk_ = hOk;
        hoverCrosshair_ = hCross;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void WindowPickDialog::EnsureMouseLeaveTracking() {
    if (trackingMouse_) return;
    TRACKMOUSEEVENT tme{};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = hwnd_;
    trackingMouse_ = TrackMouseEvent(&tme) != FALSE;
}

void WindowPickDialog::OnSetCursor() {
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(hwnd_, &pt);
    UpdateHover(pt.x, pt.y);
    const bool hand = hoverClose_ || hoverCancel_ || hoverOk_ || hoverCrosshair_;
    SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
}

LRESULT WindowPickDialog::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    const bool dragActive = crosshairDrag_.IsActive();
    if (dragActive) {
        if (crosshairDrag_.HandleMessage(msg, wp, lp,
            [this](int x, int y) {
                ApplyPickInfo(GetWindowInfoFromPoint(x, y));
            },
            nullptr)) {
            if (msg == WM_LBUTTONUP) {
                POINT pt{};
                GetCursorPos(&pt);
                ApplyPickInfo(GetWindowInfoFromPoint(pt.x, pt.y));
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
    }

    switch (msg) {
    case WM_CREATE:
        OnCreate();
        return 0;
    case WM_PAINT:
        OnPaint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        SetBkColor(hdc, kWhite);
        SetTextColor(hdc, kText);
        static HBRUSH brush = CreateSolidBrush(kWhite);
        return reinterpret_cast<LRESULT>(brush);
    }
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        if (HitClose(pt.x, pt.y)) return HTCLIENT;
        if (HitTitle(pt.x, pt.y)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (dis && crosshairDrag_.IsCrosshairButton(dis->hwndItem)) {
            crosshairDrag_.DrawButton(dis, bodyFont_, hoverCrosshair_ ? crosshairBtn_ : nullptr);
            return TRUE;
        }
        return FALSE;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && crosshairDrag_.IsActive()) crosshairDrag_.End();
        return DefWindowProcW(hwnd_, msg, wp, lp);
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (HitClose(x, y)) { Close(false); return 0; }
        if (PtIn(CancelBtnRect(), x, y)) { Close(false); return 0; }
        if (PtIn(OkBtnRect(), x, y)) { Close(true); return 0; }
        HWND crosshairHit = crosshairDrag_.HitButton(x, y, [this](HWND btn) {
            RECT rc{};
            GetWindowRect(btn, &rc);
            POINT tl{rc.left, rc.top};
            ScreenToClient(hwnd_, &tl);
            return RECT{tl.x, tl.y, tl.x + (rc.right - rc.left), tl.y + (rc.bottom - rc.top)};
        });
        if (crosshairHit) {
            CrosshairDragBinding binding{};
            if (crosshairDrag_.TryGetBinding(crosshairHit, binding)) crosshairDrag_.Begin(binding);
            return 0;
        }
        if (HitTitle(x, y)) return 0;
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
    case WM_MOUSEMOVE:
        EnsureMouseLeaveTracking();
        UpdateHover(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        hoverClose_ = hoverCancel_ = hoverOk_ = hoverCrosshair_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            OnSetCursor();
            return TRUE;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    case WM_CLOSE:
        Close(false);
        return 0;
    case WM_DESTROY:
        CleanupGdi();
        return 0;
    default:
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

}  // namespace windowmode
