#include "settings_dialog.h"

#include "app_branding.h"
#include "controls.h"
#include "drawing.h"
#include "scheduled_task_ui.h"
#include "utils.h"

#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

void SetEditText(HWND edit, const std::wstring& text) {
    if (edit) SetWindowTextW(edit, text.c_str());
}

std::wstring FormatDouble4(double v) {
    wchar_t buf[32]{};
    swprintf_s(buf, L"%.4f", v);
    return buf;
}

void DrawTextIn(HDC hdc, const std::wstring& text, RECT rc, COLORREF color,
                UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    DrawTextW(hdc, text.c_str(), -1, &rc, format);
}

void FillAlphaRect(HDC hdc, RECT rc, COLORREF color, BYTE alpha) {
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    RECT local{0, 0, w, h};
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(mem, &local, brush);
    DeleteObject(brush);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, 0};
    GdiAlphaBlend(hdc, rc.left, rc.top, w, h, mem, 0, 0, w, h, bf);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void MoveCtrl(HWND hwnd, int x, int y, int w, int h) {
    if (hwnd) SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

HWND MakeBorderedEdit(HWND parent, const wchar_t* text, int id) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
}

}  // namespace

int SettingsDialog::CenteredEditY(int rowTop, int rowHeight) const {
    return rowTop + (rowHeight - kEditH) / 2;
}

void SettingsDialog::CenterEditTextVertically(HWND edit) {
    if (!edit) return;
    RECT rc{};
    GetClientRect(edit, &rc);
    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
    HDC hdc = GetDC(edit);
    HFONT oldFont = font ? reinterpret_cast<HFONT>(SelectObject(hdc, font)) : nullptr;
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    if (oldFont) SelectObject(hdc, oldFont);
    ReleaseDC(edit, hdc);
    const int textH = tm.tmHeight;
    const int pad = std::max(0, static_cast<int>((rc.bottom - rc.top - textH) / 2));
    rc.top = pad;
    rc.bottom = rc.top + textH + 2;
    SendMessageW(edit, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&rc));
}

int SettingsDialog::ClickRowY(int index) const {
    const int base = ClickTop();
    const int jitterSub = base + kRowH * 2 + 34;
    const int fixedCb = jitterSub + kSubRowH + 12;
    const int fixedSub = fixedCb + 34;
    const int crossY = fixedSub + kSubRowH;
    const int countCb = crossY + kSubRowH + 10;
    switch (index) {
    case 0: return base;
    case 1: return base + kRowH;
    case 2: return base + kRowH * 2;
    case 3: return fixedCb;
    case 4: return countCb;
    default: return base;
    }
}

RECT SettingsDialog::ClickCheckboxRect(int index) const {
    const int top = ClickRowY(index);
    return RECT{kMargin, top, kMargin + kCheckboxSize, top + kCheckboxSize};
}

bool SettingsDialog::HitClickCheckbox(int x, int y, int& outIndex) const {
    for (int i = 0; i < 5; ++i) {
        if (PtIn(ClickCheckboxRect(i), x, y)) { outIndex = i; return true; }
    }
    return false;
}

int SettingsDialog::PlaybackRowY(int index) const {
    const int base = PlaybackTop();
    const int sec1 = base + kSubLineOffset + kSubRowH + 14;
    const int sec2 = sec1 + kSubLineOffset + kSubRowH + 14;
    switch (index) {
    case 0: return base;
    case 1: return sec1;
    case 2: return sec2;
    case 3: return sec2;
    default: return base;
    }
}

RECT SettingsDialog::PlaybackCheckboxRect(int index) const {
    const int top = PlaybackRowY(index);
    const int left = (index == 3) ? 400 : kMargin;
    return RECT{left, top, left + kCheckboxSize, top + kCheckboxSize};
}

bool SettingsDialog::HitPlaybackCheckbox(int x, int y, int& outIndex) const {
    for (int i = 0; i < 4; ++i) {
        if (PtIn(PlaybackCheckboxRect(i), x, y)) { outIndex = i; return true; }
    }
    return false;
}

RECT SettingsDialog::OtherCheckboxRect(int index) const {
    const int top = kContentTop + kContentPad + index * kRowH;
    return RECT{kMargin, top, kMargin + kCheckboxSize, top + kCheckboxSize};
}

void SettingsDialog::ToggleCheckbox(Tab tab, int index) {
    if (tab == Tab::Click) {
        auto& c = working_.click;
        switch (index) {
        case 0: c.enableRandomInterval = !c.enableRandomInterval; break;
        case 1: c.enablePressReleaseInterval = !c.enablePressReleaseInterval; break;
        case 2: c.enableCoordinateJitter = !c.enableCoordinateJitter; break;
        case 3: c.enableFixedCoordinates = !c.enableFixedCoordinates; break;
        case 4: c.enableClickCountLimit = !c.enableClickCountLimit; break;
        default: break;
        }
    } else if (tab == Tab::Playback) {
        auto& p = working_.playback;
        switch (index) {
        case 0: p.enablePlaybackCount = !p.enablePlaybackCount; break;
        case 1: p.enablePlaybackInterval = !p.enablePlaybackInterval; break;
        case 2: p.enableDebugOutputWindow = !p.enableDebugOutputWindow; break;
        case 3: p.autoOutputKeyFunctionDebug = !p.autoOutputKeyFunctionDebug; break;
        default: break;
        }
    } else if (tab == Tab::Other) {
        auto& o = working_.other;
        switch (index) {
        case 0: o.autoHideMainWindow = !o.autoHideMainWindow; break;
        case 1: o.playSoundOnStart = !o.playSoundOnStart; break;
        case 2: o.hideBottomRightTip = !o.hideBottomRightTip; break;
        case 3: o.closeToTray = !o.closeToTray; break;
        default: break;
        }
    }
    UpdateControlVisibility();
}

void SettingsDialog::PositionChildControls() {
    static constexpr int kClickEditX = 368;

    const int jitterSubY = ClickRowY(2) + kSubLineOffset;
    const int fixedSubY = ClickRowY(3) + kSubLineOffset;
    const int crossY = fixedSubY + kSubRowH + 4;

    MoveCtrl(editRandomInterval_, kClickEditX, CenteredEditY(ClickRowY(0), kCheckboxSize), kEditW, kEditH);
    MoveCtrl(editPressRelease_, kClickEditX, CenteredEditY(ClickRowY(1), kCheckboxSize), kEditW, kEditH);
    MoveCtrl(editJitterX_, kIndent + 148, CenteredEditY(jitterSubY, kSubRowH), kSmallEditW, kEditH);
    MoveCtrl(editJitterY_, kIndent + 400, CenteredEditY(jitterSubY, kSubRowH), kSmallEditW, kEditH);
    MoveCtrl(editFixedX_, kIndent + 108, CenteredEditY(fixedSubY, kSubRowH), kSmallEditW, kEditH);
    MoveCtrl(editFixedY_, kIndent + 320, CenteredEditY(fixedSubY, kSubRowH), kSmallEditW, kEditH);
    MoveCtrl(crosshairBtn_, kIndent, crossY, 420, 38);
    MoveCtrl(editClickLimit_, kClickEditX, CenteredEditY(ClickRowY(4), kCheckboxSize), kEditW, kEditH);

    const int pbSub0 = PlaybackRowY(0) + kSubLineOffset;
    const int pbSub1 = PlaybackRowY(1) + kSubLineOffset;
    MoveCtrl(editPlaybackCount_, kIndent + 56, CenteredEditY(pbSub0, kSubRowH), kSmallEditW, kEditH);
    MoveCtrl(editPlaybackMin_, kIndent + 108, CenteredEditY(pbSub1, kSubRowH), kEditW, kEditH);
    MoveCtrl(editPlaybackMax_, kIndent + 360, CenteredEditY(pbSub1, kSubRowH), kEditW, kEditH);

    const HWND edits[] = {
        editRandomInterval_, editPressRelease_, editJitterX_, editJitterY_,
        editFixedX_, editFixedY_, editClickLimit_,
        editPlaybackCount_, editPlaybackMin_, editPlaybackMax_,
    };
    for (HWND edit : edits) CenterEditTextVertically(edit);
}

bool SettingsDialog::Show(HWND owner, quickscript::AppSettings& settings) {
    owner_ = owner;
    settings_ = &settings;
    working_ = settings;
    done_ = false;
    saved_ = false;
    activeTab_ = Tab::Click;

    static bool registered = false;
    const wchar_t* cls = L"QuickScriptSettingsDlg";
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &SettingsDialog::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
        registered = true;
    }

    RECT ownerRc{};
    GetWindowRect(owner, &ownerRc);
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, cls, L"", WS_POPUP | WS_CLIPCHILDREN,
        ownerRc.left, ownerRc.top, kDialogW, kDialogH, nullptr, nullptr,
        GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
    ShowWindow(owner, SW_HIDE);

    MSG msg{};
    while (!done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            done_ = true;
            break;
        }
        if (!StModalMessageForDialog(msg, hwnd_)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (IsWindow(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
        SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        DestroyWindow(hwnd_);
    }

    StDiscardSpuriousInputAfterModal(owner);
    if (IsWindow(owner)) ShowWindow(owner, SW_SHOW);
    StDiscardSpuriousInputAfterModal(owner);
    if (IsWindow(owner)) SetForegroundWindow(owner);

    return saved_;
}

LRESULT CALLBACK SettingsDialog::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsDialog* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<SettingsDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return TRUE;
    }
    self = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SettingsDialog::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (crosshairDrag_.IsActive()) {
        if (crosshairDrag_.HandleMessage(msg, wp, lp,
            [this](int x, int y) {
                SetEditText(editFixedX_, std::to_wstring(x));
                SetEditText(editFixedY_, std::to_wstring(y));
            },
            nullptr)) {
            return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    switch (msg) {
    case WM_CREATE: {
        titleFont_ = CreateFontW(26, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        closeFont_ = CreateFontW(36, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        bodyFont_ = CreateFontW(26, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        tabFont_ = CreateFontW(28, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        smallFont_ = CreateFontW(22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        aboutTitleFont_ = CreateFontW(42, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");

        crosshairDragCursor_ = CreateCrosshairDragCursor(kCrosshairBlue);
        crosshairDrag_.SetOwner(hwnd_);
        crosshairDrag_.SetDragCursor(crosshairDragCursor_);

        editRandomInterval_ = MakeBorderedEdit(hwnd_, L"0.5000", kEditRandomInterval);
        editPressRelease_ = MakeBorderedEdit(hwnd_, L"0.0010", kEditPressRelease);
        editJitterX_ = MakeBorderedEdit(hwnd_, L"2", kEditJitterX);
        editJitterY_ = MakeBorderedEdit(hwnd_, L"2", kEditJitterY);
        editFixedX_ = MakeBorderedEdit(hwnd_, L"0", kEditFixedX);
        editFixedY_ = MakeBorderedEdit(hwnd_, L"0", kEditFixedY);
        crosshairBtn_ = MakeGreenButton(hwnd_, L"拖动准星到需要点击的地方", kCrosshairBtn, 0, 0, 420, 38);
        editClickLimit_ = MakeBorderedEdit(hwnd_, L"0", kEditClickLimit);
        editPlaybackCount_ = MakeBorderedEdit(hwnd_, L"1", kEditPlaybackCount);
        editPlaybackMin_ = MakeBorderedEdit(hwnd_, L"0.5000", kEditPlaybackMin);
        editPlaybackMax_ = MakeBorderedEdit(hwnd_, L"1.0000", kEditPlaybackMax);

        crosshairDrag_.RegisterButton(crosshairBtn_, {CrosshairDragMode::Coordinates, nullptr});
        SetWindowSubclass(crosshairBtn_, [](HWND btn, UINT msg, WPARAM wp, LPARAM lp,
            UINT_PTR, DWORD_PTR refData) -> LRESULT {
            if (msg == WM_LBUTTONDOWN) {
                auto* self = reinterpret_cast<SettingsDialog*>(refData);
                CrosshairDragBinding binding{};
                if (self && self->crosshairDrag_.TryGetBinding(btn, binding)) {
                    self->crosshairDrag_.Begin(binding);
                    return 0;
                }
            }
            return DefSubclassProc(btn, msg, wp, lp);
        }, 0, reinterpret_cast<DWORD_PTR>(this));
        ApplyFont(hwnd_, bodyFont_);
        PositionChildControls();
        SyncControlsFromSettings();
        UpdateControlVisibility();
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        SetBkColor(hdc, kWhite);
        SetTextColor(hdc, kText);
        static HBRUSH brush = CreateSolidBrush(kWhite);
        return reinterpret_cast<LRESULT>(brush);
    }
    case WM_ERASEBKGND:
        return 1;
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
            crosshairDrag_.DrawButton(dis, bodyFont_, hoverCrosshairBtn_);
            return TRUE;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && crosshairDrag_.IsActive()) crosshairDrag_.End();
        return DefWindowProcW(hwnd_, msg, wp, lp);
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (HitClose(x, y)) { done_ = true; DestroyWindow(hwnd_); return 0; }
        if (PtIn(RestoreLinkRect(), x, y)) { RestoreDefaults(); InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
        if (PtIn(SaveBtnRect(), x, y)) { SaveAndClose(); return 0; }
        if (activeTab_ == Tab::About && PtIn(CheckUpgradeBtnRect(), x, y)) {
            MessageBoxW(hwnd_, L"当前已是最新版本。", L"检查升级", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        for (int t = 0; t < 4; ++t) {
            if (PtIn(TabRect(static_cast<Tab>(t)), x, y)) {
                activeTab_ = static_cast<Tab>(t);
                UpdateControlVisibility();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        HWND crosshairHit = crosshairDrag_.HitButton(x, y, [this](HWND btn) {
            RECT rc{}; GetWindowRect(btn, &rc);
            POINT tl{rc.left, rc.top};
            ScreenToClient(hwnd_, &tl);
            return RECT{tl.x, tl.y, tl.x + (rc.right - rc.left), tl.y + (rc.bottom - rc.top)};
        });
        if (crosshairHit) {
            CrosshairDragBinding b{};
            if (crosshairDrag_.TryGetBinding(crosshairHit, b)) crosshairDrag_.Begin(b);
            return 0;
        }
        if (activeTab_ == Tab::Click) {
            int row = -1;
            if (HitClickCheckbox(x, y, row)) {
                ToggleCheckbox(Tab::Click, row);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        } else if (activeTab_ == Tab::Playback) {
            int row = -1;
            if (HitPlaybackCheckbox(x, y, row)) {
                ToggleCheckbox(Tab::Playback, row);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        } else if (activeTab_ == Tab::Other) {
            for (int row = 0; row < 4; ++row) {
                if (PtIn(OtherCheckboxRect(row), x, y)) {
                    ToggleCheckbox(Tab::Other, row);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        bool needRedraw = false;
        if (HitClose(x, y) != hoverClose_) { hoverClose_ = HitClose(x, y); needRedraw = true; }
        if (PtIn(RestoreLinkRect(), x, y) != hoverRestore_) { hoverRestore_ = PtIn(RestoreLinkRect(), x, y); needRedraw = true; }
        if (PtIn(SaveBtnRect(), x, y) != hoverSave_) { hoverSave_ = PtIn(SaveBtnRect(), x, y); needRedraw = true; }
        const bool hu = activeTab_ == Tab::About && PtIn(CheckUpgradeBtnRect(), x, y);
        if (hu != hoverCheckUpgrade_) { hoverCheckUpgrade_ = hu; needRedraw = true; }
        HWND ch = crosshairDrag_.HitButton(x, y, [this](HWND btn) {
            RECT rc{}; GetWindowRect(btn, &rc);
            POINT tl{rc.left, rc.top};
            ScreenToClient(hwnd_, &tl);
            return RECT{tl.x, tl.y, tl.x + (rc.right - rc.left), tl.y + (rc.bottom - rc.top)};
        });
        if (ch != hoverCrosshairBtn_) {
            if (hoverCrosshairBtn_) InvalidateRect(hoverCrosshairBtn_, nullptr, FALSE);
            hoverCrosshairBtn_ = ch;
            if (hoverCrosshairBtn_) InvalidateRect(hoverCrosshairBtn_, nullptr, FALSE);
        }
        if (needRedraw) InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_PAINT:
        Paint();
        return 0;
    case WM_DESTROY:
        CleanupGdi();
        done_ = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void SettingsDialog::CleanupGdi() {
    if (crosshairDragCursor_) { DestroyCursor(crosshairDragCursor_); crosshairDragCursor_ = nullptr; }
    if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
    if (closeFont_) { DeleteObject(closeFont_); closeFont_ = nullptr; }
    if (bodyFont_) { DeleteObject(bodyFont_); bodyFont_ = nullptr; }
    if (tabFont_) { DeleteObject(tabFont_); tabFont_ = nullptr; }
    if (smallFont_) { DeleteObject(smallFont_); smallFont_ = nullptr; }
    if (aboutTitleFont_) { DeleteObject(aboutTitleFont_); aboutTitleFont_ = nullptr; }
}

bool SettingsDialog::PtIn(const RECT& rc, int x, int y) const { return StPtIn(rc, x, y); }
bool SettingsDialog::HitClose(int x, int y) const { return PtIn(CloseRect(), x, y); }
bool SettingsDialog::HitTitle(int x, int y) const { return y >= 0 && y < kTitleH && x < CloseRect().left; }

RECT SettingsDialog::CloseRect() const { return RECT{kDialogW - kCloseBtnW, 0, kDialogW, kTitleH}; }
RECT SettingsDialog::TabRect(Tab tab) const {
    const int idx = static_cast<int>(tab);
    return RECT{idx * kTabW, kTitleH, (idx + 1) * kTabW, kContentTop};
}
RECT SettingsDialog::FooterRect() const { return RECT{0, kFooterTop, kDialogW, kDialogH}; }
RECT SettingsDialog::RestoreLinkRect() const { return RECT{kMargin, kFooterTop + 10, kMargin + 180, kFooterTop + kFooterH - 10}; }
RECT SettingsDialog::SaveBtnRect() const { return RECT{kDialogW - kMargin - 96, kFooterTop + 12, kDialogW - kMargin, kFooterTop + kFooterH - 12}; }
RECT SettingsDialog::CheckUpgradeBtnRect() const {
    const RECT save = SaveBtnRect();
    return RECT{save.left - 116, save.top, save.left - 12, save.bottom};
}

void SettingsDialog::SyncControlsFromSettings() {
    const auto& c = working_.click;
    const auto& p = working_.playback;
    SetEditText(editRandomInterval_, FormatDouble4(c.randomIntervalMaxSeconds));
    SetEditText(editPressRelease_, FormatDouble4(c.pressReleaseIntervalSeconds));
    SetEditText(editJitterX_, std::to_wstring(c.jitterX));
    SetEditText(editJitterY_, std::to_wstring(c.jitterY));
    SetEditText(editFixedX_, std::to_wstring(c.fixedX));
    SetEditText(editFixedY_, std::to_wstring(c.fixedY));
    SetEditText(editClickLimit_, std::to_wstring(c.clickCountLimit));
    SetEditText(editPlaybackCount_, std::to_wstring(p.playbackCount));
    SetEditText(editPlaybackMin_, FormatDouble4(p.playbackIntervalMinSeconds));
    SetEditText(editPlaybackMax_, FormatDouble4(p.playbackIntervalMaxSeconds));
}

void SettingsDialog::SyncSettingsFromControls() {
    auto& c = working_.click;
    auto& p = working_.playback;
    c.randomIntervalMaxSeconds = std::max(0.0, ToDouble(editRandomInterval_, c.randomIntervalMaxSeconds));
    c.pressReleaseIntervalSeconds = std::max(0.0, ToDouble(editPressRelease_, c.pressReleaseIntervalSeconds));
    c.jitterX = std::max(0, static_cast<int>(ToDouble(editJitterX_, c.jitterX)));
    c.jitterY = std::max(0, static_cast<int>(ToDouble(editJitterY_, c.jitterY)));
    c.fixedX = static_cast<int>(ToDouble(editFixedX_, c.fixedX));
    c.fixedY = static_cast<int>(ToDouble(editFixedY_, c.fixedY));
    c.clickCountLimit = std::max(0, static_cast<int>(ToDouble(editClickLimit_, c.clickCountLimit)));
    p.playbackCount = std::max(0, static_cast<int>(ToDouble(editPlaybackCount_, p.playbackCount)));
    p.playbackIntervalMinSeconds = std::max(0.0, ToDouble(editPlaybackMin_, p.playbackIntervalMinSeconds));
    p.playbackIntervalMaxSeconds = std::max(p.playbackIntervalMinSeconds,
        ToDouble(editPlaybackMax_, p.playbackIntervalMaxSeconds));
}

void SettingsDialog::UpdateControlVisibility() {
    const bool clickTab = activeTab_ == Tab::Click;
    const bool playbackTab = activeTab_ == Tab::Playback;
    const HWND clickEdits[] = {editRandomInterval_, editPressRelease_, editJitterX_, editJitterY_,
        editFixedX_, editFixedY_, editClickLimit_, crosshairBtn_};
    for (HWND e : clickEdits) if (e) ShowWindow(e, clickTab ? SW_SHOW : SW_HIDE);
    const HWND playbackEdits[] = {editPlaybackCount_, editPlaybackMin_, editPlaybackMax_};
    for (HWND e : playbackEdits) if (e) ShowWindow(e, playbackTab ? SW_SHOW : SW_HIDE);
    if (clickTab || playbackTab) PositionChildControls();
}

void SettingsDialog::RestoreDefaults() {
    working_ = quickscript::DefaultAppSettings();
    SyncControlsFromSettings();
    UpdateControlVisibility();
}

void SettingsDialog::SaveAndClose() {
    SyncSettingsFromControls();
    if (settings_) *settings_ = working_;
    saved_ = true;
    done_ = true;
    DestroyWindow(hwnd_);
}

void SettingsDialog::FillGradientRect(HDC hdc, RECT rc, COLORREF start, COLORREF end, bool vertical) {
    TRIVERTEX vertex[2]{};
    vertex[0].x = rc.left; vertex[0].y = rc.top;
    vertex[0].Red = static_cast<COLOR16>(GetRValue(start) << 8);
    vertex[0].Green = static_cast<COLOR16>(GetGValue(start) << 8);
    vertex[0].Blue = static_cast<COLOR16>(GetBValue(start) << 8);
    vertex[0].Alpha = 0;
    vertex[1].x = rc.right; vertex[1].y = rc.bottom;
    vertex[1].Red = static_cast<COLOR16>(GetRValue(end) << 8);
    vertex[1].Green = static_cast<COLOR16>(GetGValue(end) << 8);
    vertex[1].Blue = static_cast<COLOR16>(GetBValue(end) << 8);
    vertex[1].Alpha = 0;
    GRADIENT_RECT gr{0, 1};
    GradientFill(hdc, vertex, 2, &gr, 1, vertical ? GRADIENT_FILL_RECT_V : GRADIENT_FILL_RECT_H);
}

void SettingsDialog::DrawSettingsTab(HDC hdc, const RECT& rc, Tab tab, const wchar_t* text) {
    if (activeTab_ == tab) FillGradientRect(hdc, rc, RGB(59, 157, 92), RGB(44, 128, 75), true);
    else FillRectColor(hdc, rc, kMainGreen);
    SelectObject(hdc, tabFont_);
    DrawTextIn(hdc, text, rc, kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void SettingsDialog::DrawFooter(HDC hdc) {
    FillRectColor(hdc, FooterRect(), kPanel);
    SelectObject(hdc, bodyFont_);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, hoverRestore_ ? kDarkGreen : kMainGreen);
    RECT restoreRc = RestoreLinkRect();
    DrawTextW(hdc, L"恢复所有默认值", -1, &restoreRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    StDrawGreenButton(hdc, bodyFont_, SaveBtnRect(), L"保存", hoverSave_);
    if (activeTab_ == Tab::About) {
        StDrawGreenButton(hdc, bodyFont_, CheckUpgradeBtnRect(), L"检查升级", hoverCheckUpgrade_);
    }
}

void SettingsDialog::PaintClickTab(HDC hdc) {
    const auto& c = working_.click;
    SelectObject(hdc, bodyFont_);
    const bool checks[] = {c.enableRandomInterval, c.enablePressReleaseInterval,
        c.enableCoordinateJitter, c.enableFixedCoordinates, c.enableClickCountLimit};
    const wchar_t* labels[] = {
        L"启用随机间隔时间，最长为", L"启用按下抬起间隔，间隔",
        L"启用坐标随机抖动", L"启用固定坐标", L"启用次数限制，点击",
    };
    for (int i = 0; i < 5; ++i) {
        const RECT cb = ClickCheckboxRect(i);
        StDrawCheckbox(hdc, cb, checks[i]);
        DrawTextIn(hdc, labels[i], RECT{cb.right + 10, cb.top, kDialogW - kMargin, cb.bottom},
            kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    const int jitterSubY = ClickRowY(2) + kSubLineOffset;
    const int fixedSubY = ClickRowY(3) + kSubLineOffset;
    static constexpr int kClickEditX = 368;
    static constexpr int kClickUnitX = kClickEditX + kEditW + 10;

    DrawTextIn(hdc, L"秒", RECT{kClickUnitX, ClickRowY(0), kClickUnitX + 40, ClickRowY(0) + kCheckboxSize},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"秒(建议0.001~0.05)", RECT{kClickUnitX, ClickRowY(1), kDialogW - kMargin, ClickRowY(1) + kCheckboxSize},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(hdc, L"横坐标抖动(X)", RECT{kIndent, jitterSubY, kIndent + 140, jitterSubY + kSubRowH},
        c.enableCoordinateJitter ? kText : kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"像素", RECT{kIndent + 206, jitterSubY, kIndent + 250, jitterSubY + kSubRowH},
        c.enableCoordinateJitter ? kText : kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"竖坐标抖动(Y)", RECT{kIndent + 300, jitterSubY, kIndent + 440, jitterSubY + kSubRowH},
        c.enableCoordinateJitter ? kText : kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"像素", RECT{kIndent + 458, jitterSubY, kIndent + 500, jitterSubY + kSubRowH},
        c.enableCoordinateJitter ? kText : kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(hdc, L"横坐标(X)", RECT{kIndent, fixedSubY, kIndent + 100, fixedSubY + kSubRowH},
        c.enableFixedCoordinates ? kText : kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"像素", RECT{kIndent + 166, fixedSubY, kIndent + 210, fixedSubY + kSubRowH},
        c.enableFixedCoordinates ? kText : kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"纵坐标(Y)", RECT{kIndent + 240, fixedSubY, kIndent + 340, fixedSubY + kSubRowH},
        c.enableFixedCoordinates ? kText : kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"像素", RECT{kIndent + 378, fixedSubY, kIndent + 420, fixedSubY + kSubRowH},
        c.enableFixedCoordinates ? kText : kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(hdc, L"次后，自动停止。", RECT{kClickUnitX, ClickRowY(4), kDialogW - kMargin, ClickRowY(4) + kCheckboxSize},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void SettingsDialog::PaintPlaybackTab(HDC hdc) {
    const auto& p = working_.playback;
    SelectObject(hdc, bodyFont_);

    for (int i = 0; i < 4; ++i) {
        const RECT cb = PlaybackCheckboxRect(i);
        bool checked = false;
        switch (i) {
        case 0: checked = p.enablePlaybackCount; break;
        case 1: checked = p.enablePlaybackInterval; break;
        case 2: checked = p.enableDebugOutputWindow; break;
        case 3: checked = p.autoOutputKeyFunctionDebug; break;
        }
        StDrawCheckbox(hdc, cb, checked);
    }

    const int sub0 = PlaybackRowY(0) + kSubLineOffset;
    const int sub1 = PlaybackRowY(1) + kSubLineOffset;
    const int hintY = PlaybackRowY(2) + kRowH + 8;

    DrawTextIn(hdc, L"启用回放次数", RECT{kLabelAfterCheck, PlaybackRowY(0), kDialogW - kMargin, PlaybackRowY(0) + kCheckboxSize},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"回放", RECT{kIndent, sub0, kIndent + 52, sub0 + kSubRowH}, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"次后，自动停止(默认为0无限循环)", RECT{kIndent + 124, sub0, kDialogW - kMargin, sub0 + kSubRowH},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(hdc, L"启用回放间隔(多次回放间的间隔)", RECT{kLabelAfterCheck, PlaybackRowY(1), kDialogW - kMargin, PlaybackRowY(1) + kCheckboxSize},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"最小间隔", RECT{kIndent, sub1, kIndent + 100, sub1 + kSubRowH}, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"秒", RECT{kIndent + 208, sub1, kIndent + 232, sub1 + kSubRowH}, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"最大间隔", RECT{kIndent + 262, sub1, kIndent + 362, sub1 + kSubRowH}, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"秒", RECT{kIndent + 460, sub1, kIndent + 484, sub1 + kSubRowH}, kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    DrawTextIn(hdc, L"启用/关闭宏调试信息输出窗口", RECT{kLabelAfterCheck, PlaybackRowY(2), 390, PlaybackRowY(2) + kCheckboxSize},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"自动输出宏关键函数调试信息", RECT{438, PlaybackRowY(2), kDialogW - kMargin, PlaybackRowY(2) + kCheckboxSize},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, smallFont_);
    DrawTextIn(hdc,
        L"*点击上方勾选框可关闭调试信息输出窗口，可也点击标题栏激活窗口后，按Ctrl+C(有光标时需要按两次)或Ctrl+Break关闭；其他方式关闭将导致鼠大侠退出。",
        RECT{kMargin, hintY, kDialogW - kMargin, hintY + 64}, kHint, DT_LEFT | DT_WORDBREAK);
}

void SettingsDialog::PaintOtherTab(HDC hdc) {
    const auto& o = working_.other;
    const wchar_t* labels[] = {
        L"启用连点/录制/回放后自动隐藏主窗口", L"启动连点/录制/回放时播放声音",
        L"关闭右下角连点/录制/回放提示", L"点击主界面关闭按钮隐藏到托盘",
    };
    const bool checks[] = {o.autoHideMainWindow, o.playSoundOnStart, o.hideBottomRightTip, o.closeToTray};
    SelectObject(hdc, bodyFont_);
    for (int i = 0; i < 4; ++i) {
        const RECT rc = OtherCheckboxRect(i);
        StDrawCheckbox(hdc, rc, checks[i]);
        DrawTextIn(hdc, labels[i], RECT{rc.right + 10, rc.top, kDialogW - kMargin, rc.bottom},
            kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

void SettingsDialog::PaintAboutTab(HDC hdc) {
    HPEN pen = CreatePen(PS_SOLID, 2, kMainGreen);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    Ellipse(hdc, kMargin + 8, kContentTop + 24, kMargin + 72, kContentTop + 88);
    MoveToEx(hdc, kMargin + 40, kContentTop + 18, nullptr);
    LineTo(hdc, kMargin + 40, kContentTop + 50);
    MoveToEx(hdc, kMargin + 24, kContentTop + 18, nullptr);
    LineTo(hdc, kMargin + 56, kContentTop + 18);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    const auto& name = quickscript::AppBranding::AppDisplayName();
    const auto& version = quickscript::AppBranding::Version();
    const auto& tagline = quickscript::AppBranding::Tagline();
    const auto& website = quickscript::AppBranding::WebsiteUrl();
    const auto& contact = quickscript::AppBranding::ContactInfo();
    const auto& qq = quickscript::AppBranding::QqGroup();
    const auto& copyright = quickscript::AppBranding::CopyrightText();

    SelectObject(hdc, aboutTitleFont_);
    DrawTextIn(hdc, name, RECT{kMargin + 88, kContentTop + 20, 400, kContentTop + 72},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, bodyFont_);
    DrawTextIn(hdc, version, RECT{kMargin + 220, kContentTop + 36, 400, kContentTop + 68},
        kMainGreen, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, smallFont_);
    DrawTextIn(hdc, tagline, RECT{kMargin + 88, kContentTop + 76, 500, kContentTop + 100},
        kHint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const RECT infoBox{kMargin, kContentTop + 120, kDialogW - kMargin, kContentTop + 260};
    DrawBorderRect(hdc, infoBox, kComboBorderGray);
    SelectObject(hdc, bodyFont_);

    const int rowH = 40;
    const int valueLeft = infoBox.left + 120;
    const COLORREF linkColor = RGB(0, 102, 204);

    DrawTextIn(hdc, L"官网地址：", RECT{infoBox.left + 16, infoBox.top + 16, valueLeft, infoBox.top + 16 + rowH},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (!website.empty()) {
        DrawTextIn(hdc, website, RECT{valueLeft, infoBox.top + 16, infoBox.right - 16, infoBox.top + 16 + rowH},
            linkColor, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    DrawTextIn(hdc, L"联系方式：", RECT{infoBox.left + 16, infoBox.top + 16 + rowH, valueLeft, infoBox.top + 16 + rowH * 2},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (!contact.empty()) {
        DrawTextIn(hdc, contact, RECT{valueLeft, infoBox.top + 16 + rowH, infoBox.right - 16, infoBox.top + 16 + rowH * 2},
            linkColor, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    DrawTextIn(hdc, L"QQ交流群：", RECT{infoBox.left + 16, infoBox.top + 16 + rowH * 2, valueLeft, infoBox.top + 16 + rowH * 3},
        kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (!qq.empty()) {
        DrawTextIn(hdc, qq, RECT{valueLeft, infoBox.top + 16 + rowH * 2, infoBox.right - 16, infoBox.top + 16 + rowH * 3},
            linkColor, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    if (!copyright.empty()) {
        SelectObject(hdc, smallFont_);
        DrawTextIn(hdc, copyright, RECT{kMargin, kContentTop + 300, kDialogW - kMargin, kContentTop + 330},
            kText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

void SettingsDialog::Paint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT rc{}; GetClientRect(hwnd_, &rc);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    FillRectColor(mem, rc, kWhite);
    FillRectColor(mem, RECT{0, 0, rc.right, kTitleH}, kMainGreen);
    FillRectColor(mem, RECT{0, kTitleH, rc.right, kContentTop}, kMainGreen);
    HPEN pen = CreatePen(PS_SOLID, 2, kWhite);
    HGDIOBJ oldPen = SelectObject(mem, pen);
    Ellipse(mem, 11, 10, 28, 28);
    MoveToEx(mem, 19, 7, nullptr); LineTo(mem, 19, 15);
    MoveToEx(mem, 12, 7, nullptr); LineTo(mem, 26, 7);
    SelectObject(mem, oldPen); DeleteObject(pen);
    SelectObject(mem, titleFont_);
    DrawTextIn(mem, L"鼠大侠-设置", RECT{33, 0, 220, kTitleH}, kWhite, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (hoverClose_) FillAlphaRect(mem, CloseRect(), RGB(0, 0, 0), kCloseHoverAlpha);
    SelectObject(mem, closeFont_);
    DrawTextIn(mem, L"×", CloseRect(), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawSettingsTab(mem, TabRect(Tab::Click), Tab::Click, L"点击设置");
    DrawSettingsTab(mem, TabRect(Tab::Playback), Tab::Playback, L"录制/宏回放设置");
    DrawSettingsTab(mem, TabRect(Tab::Other), Tab::Other, L"其他设置");
    DrawSettingsTab(mem, TabRect(Tab::About), Tab::About, L"关于");
    switch (activeTab_) {
    case Tab::Click: PaintClickTab(mem); break;
    case Tab::Playback: PaintPlaybackTab(mem); break;
    case Tab::Other: PaintOtherTab(mem); break;
    case Tab::About: PaintAboutTab(mem); break;
    }
    DrawFooter(mem);
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
}
