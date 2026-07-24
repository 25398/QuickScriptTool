// ── 热键/按键捕获模态对话框 ────────────────────────────────────────
// UI 绘制沿用现有样式；消息泵、悬停与关闭流程参考定时任务对话框。
// 热键模式（脚本/录制/启停）：短按=普通热键，按住达「长按判定」=按住启停。
// 按键点击/按下动作捕获：仍为按下即定键，无按住语义。
#include "hotkey_dialog.h"
#include "drawing.h"
#include "render_context.h"
#include "scheduled_task_ui.h"
#include "taskbar_window.h"
#include <windowsx.h>

namespace {

constexpr int kDlgW = 364;
constexpr int kDlgH = 210;
constexpr wchar_t kDlgClass[] = L"QuickScriptHotkeyDlg";

bool IsModifierVk(UINT vk) {
    return vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_SHIFT
        || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_CONTROL
        || vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU
        || vk == VK_LWIN || vk == VK_RWIN;
}

UINT ReadModifierMask() {
    UINT mods = 0;
    if (GetAsyncKeyState(VK_LCONTROL) & 0x8000 || GetAsyncKeyState(VK_RCONTROL) & 0x8000)
        mods |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_LMENU) & 0x8000 || GetAsyncKeyState(VK_RMENU) & 0x8000)
        mods |= MOD_ALT;
    if (GetAsyncKeyState(VK_LSHIFT) & 0x8000 || GetAsyncKeyState(VK_RSHIFT) & 0x8000)
        mods |= MOD_SHIFT;
    if (GetAsyncKeyState(VK_LWIN) & 0x8000 || GetAsyncKeyState(VK_RWIN) & 0x8000)
        mods |= MOD_WIN;
    return mods;
}

}  // namespace

bool HotkeyCapture::Show(HWND owner, const Hotkey& oldValue,
                         bool scriptHotkey, Hotkey& out,
                         bool globalStartStop,
                         double holdThresholdSeconds) {
    owner_ = owner;
    old_ = oldValue;
    current_ = oldValue;
    scriptHotkey_ = scriptHotkey;
    globalStartStop_ = globalStartStop;
    holdThresholdSeconds_ = NormalizeHoldThresholdSeconds(holdThresholdSeconds);
    done_ = false;
    accepted_ = false;
    trackingMouse_ = false;
    hoverOk_ = hoverCancel_ = hoverReset_ = hoverDelete_ = false;
    pendingDown_ = false;
    pendingVk_ = 0;
    pendingMods_ = 0;
    pendingDownTick_ = 0;

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &HotkeyCapture::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kDlgClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
        registered = true;
    }

    RECT ownerRc{};
    GetWindowRect(owner, &ownerRc);
    const int x = ownerRc.left + ((ownerRc.right - ownerRc.left) - kDlgW) / 2;
    const int y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - kDlgH) / 2;

    hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kDlgClass, L"设置热键",
        WS_POPUP | WS_MINIMIZEBOX, x, y, kDlgW, kDlgH,
        owner, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    ApplyTaskbarWindowStyle(hwnd_, L"设置热键");
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);

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

    if (IsWindow(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;

    if (IsWindow(owner)) LockWindowUpdate(owner);
    StDiscardSpuriousInputAfterModal(owner);
    if (IsWindow(owner)) {
        SetForegroundWindow(owner);
    }
    StDiscardSpuriousInputAfterModal(owner);
    if (IsWindow(owner)) LockWindowUpdate(nullptr);

    if (accepted_) out = current_;
    return accepted_;
}

LRESULT CALLBACK HotkeyCapture::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HotkeyCapture* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<HotkeyCapture*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return TRUE;
    }
    self = reinterpret_cast<HotkeyCapture*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT HotkeyCapture::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: OnCreate(); return 0;
    case WM_PAINT: OnPaint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_IME_SETCONTEXT: return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: OnKeyDown(static_cast<UINT>(wp)); return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP: OnKeyUp(static_cast<UINT>(wp)); return 0;
    case WM_TIMER:
        if (wp == kHoldCaptureTimerId) OnHoldCaptureTimer();
        return 0;
    case WM_LBUTTONDOWN:
        OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: return 0;
    case WM_MOUSEMOVE:
        OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSELEAVE:
        OnMouseLeave();
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
        CancelPendingCapture();
        CleanupGdi();
        return 0;
    default:
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

void HotkeyCapture::OnCreate() {
    auto mkFont = [](int height, int weight) {
        return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
    };
    font_ = mkFont(22, FW_NORMAL);
    valueFont_ = mkFont(28, FW_NORMAL);
    outerShadow_.Attach(hwnd_);
}

void HotkeyCapture::CleanupGdi() {
    if (font_) { DeleteObject(font_); font_ = nullptr; }
    if (valueFont_) { DeleteObject(valueFont_); valueFont_ = nullptr; }
}

void HotkeyCapture::CancelPendingCapture() {
    if (hwnd_) KillTimer(hwnd_, kHoldCaptureTimerId);
    pendingDown_ = false;
    pendingVk_ = 0;
    pendingMods_ = 0;
    pendingDownTick_ = 0;
}

void HotkeyCapture::Close(bool accept) {
    if (done_) return;
    // 确定时若仍按住未抬起：按已按下时长裁定短按/长按，避免点确定把长按冲成点击
    if (accept && IsHotkeyCaptureMode() && pendingDown_ && pendingVk_ != 0) {
        const bool hold = (GetTickCount() - pendingDownTick_) >= CaptureHoldMs();
        ApplyCapturedKey(pendingVk_, pendingMods_, hold);
    }
    CancelPendingCapture();
    accepted_ = accept;
    done_ = true;
    if (IsWindow(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
}

void HotkeyCapture::SetHoverFlag(bool& flag, bool value, const RECT& rc) {
    if (flag == value) return;
    flag = value;
    InvalidateRect(hwnd_, &rc, FALSE);
}

void HotkeyCapture::UpdateHover(int x, int y) {
    SetHoverFlag(hoverOk_, InRect(OkRect(), x, y), OkRect());
    SetHoverFlag(hoverCancel_, InRect(CancelRect(), x, y), CancelRect());
    if (scriptHotkey_ && !globalStartStop_) {
        SetHoverFlag(hoverReset_, InRect(ResetRect(), x, y), ResetRect());
        SetHoverFlag(hoverDelete_, InRect(DeleteRect(), x, y), DeleteRect());
    }
}

void HotkeyCapture::UpdateHoverFromCursor() {
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(hwnd_, &pt);
    UpdateHover(pt.x, pt.y);
}

void HotkeyCapture::EnsureMouseLeaveTracking() {
    if (trackingMouse_) return;
    TRACKMOUSEEVENT tme{};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = hwnd_;
    trackingMouse_ = TrackMouseEvent(&tme) != FALSE;
}

void HotkeyCapture::OnMouseMove(int /*x*/, int /*y*/) {
    EnsureMouseLeaveTracking();
    UpdateHoverFromCursor();
}

void HotkeyCapture::OnMouseLeave() {
    trackingMouse_ = false;
    SetHoverFlag(hoverOk_, false, OkRect());
    SetHoverFlag(hoverCancel_, false, CancelRect());
    if (scriptHotkey_ && !globalStartStop_) {
        SetHoverFlag(hoverReset_, false, ResetRect());
        SetHoverFlag(hoverDelete_, false, DeleteRect());
    }
}

void HotkeyCapture::OnSetCursor() {
    UpdateHoverFromCursor();
    const bool hand = hoverOk_ || hoverCancel_
        || (scriptHotkey_ && !globalStartStop_ && (hoverReset_ || hoverDelete_));
    SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
}

void HotkeyCapture::OnLButtonDown(int x, int y) {
    if (InRect(OkRect(), x, y)) { Close(true); return; }
    if (InRect(CancelRect(), x, y)) { Close(false); return; }
    if (!scriptHotkey_ || globalStartStop_) return;
    if (InRect(ResetRect(), x, y)) {
        CancelPendingCapture();
        current_ = old_;
        InvalidateValueArea();
        return;
    }
    if (InRect(DeleteRect(), x, y)) {
        CancelPendingCapture();
        current_.enabled = false;
        current_.text = L"无";
        current_.vk = 0;
        current_.modifiers = 0;
        current_.holdMode = false;
        InvalidateValueArea();
    }
}

void HotkeyCapture::ApplyCapturedKey(UINT vk, UINT mods, bool holdMode) {
    current_.vk = vk;
    current_.modifiers = mods;
    current_.holdMode = holdMode && IsHotkeyCaptureMode();
    current_.text = HotkeyText(current_.modifiers, current_.vk, current_.holdMode);
    current_.enabled = true;
    InvalidateValueArea();
}

void HotkeyCapture::OnKeyDown(UINT vk) {
    if (vk == VK_PROCESSKEY) return;
    if (IsModifierVk(vk)) return;

    // 按键点击 / 按下抬起：按下即定键（无长按语义）
    if (!IsHotkeyCaptureMode()) {
        ApplyCapturedKey(vk, ReadModifierMask(), false);
        return;
    }

    // 热键捕获：等抬起再判定短按 / 长按；重复 KEYDOWN 忽略
    if (pendingDown_ && pendingVk_ == vk) return;
    CancelPendingCapture();
    pendingDown_ = true;
    pendingVk_ = vk;
    pendingMods_ = ReadModifierMask();
    pendingDownTick_ = GetTickCount();
    ApplyCapturedKey(pendingVk_, pendingMods_, false);
    SetTimer(hwnd_, kHoldCaptureTimerId, CaptureHoldMs(), nullptr);
}

void HotkeyCapture::OnKeyUp(UINT vk) {
    if (!IsHotkeyCaptureMode() || !pendingDown_) return;
    if (vk != pendingVk_) return;

    const bool hold = (GetTickCount() - pendingDownTick_) >= CaptureHoldMs();
    const UINT mods = pendingMods_;
    const UINT key = pendingVk_;
    CancelPendingCapture();
    ApplyCapturedKey(key, mods, hold);
}

void HotkeyCapture::OnHoldCaptureTimer() {
    if (!pendingDown_ || !IsHotkeyCaptureMode()) {
        KillTimer(hwnd_, kHoldCaptureTimerId);
        return;
    }
    if ((GetTickCount() - pendingDownTick_) < CaptureHoldMs()) return;
    KillTimer(hwnd_, kHoldCaptureTimerId);
    // 仍按住：实时把文案切成「长按」，抬起时再最终确认
    ApplyCapturedKey(pendingVk_, pendingMods_, true);
}

void HotkeyCapture::InvalidateValueArea() {
    const RECT rc = ValueRect();
    InvalidateRect(hwnd_, &rc, FALSE);
}

void HotkeyCapture::OnPaint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    RenderBatchScope batch(hdc);

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    FillRectColor(hdc, rc, RGB(255, 255, 255));

    RECT bottom{0, 155, rc.right, rc.bottom};
    FillRectColor(hdc, bottom, RGB(247, 247, 247));

    SelectObject(hdc, font_);
    std::wstring prompt;
    if (IsHotkeyCaptureMode()) {
        prompt = L"请直接在键盘上输入新的热键\n（按住设为启停，按住"
            + FormatHoldThresholdLabel(holdThresholdSeconds_) + L"秒算按住）";
    } else {
        prompt = L"请直接在键盘上输入要点击的键（\n截屏键";
    }
    DrawTextIn(hdc, prompt, RECT{16, 16, 348, 74}, RGB(150, 150, 150), DT_LEFT);

    SelectObject(hdc, valueFont_);
    std::wstring val = L"无";
    if (current_.enabled) {
        val = current_.text;
        if (current_.holdMode) val += L"（长按）";
    }
    DrawTextIn(hdc, val, ValueRect(), RGB(80, 80, 80), DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    DrawBtn(hdc, OkRect(), L"确定", true, hoverOk_);
    DrawBtn(hdc, CancelRect(), L"取消", true, hoverCancel_);
    if (scriptHotkey_ && !globalStartStop_) {
        DrawBtn(hdc, ResetRect(), L"重置热键", false, hoverReset_);
        DrawBtn(hdc, DeleteRect(), L"删除热键", false, hoverDelete_);
    }

    batch.End();
    EndPaint(hwnd_, &ps);
}

void HotkeyCapture::DrawBtn(HDC hdc, const RECT& rc, const wchar_t* text,
                            bool green, bool hover) {
    COLORREF fill, txt;
    if (green) {
        fill = hover ? kButtonGreenHover : kButtonGreen;
        txt = kWhite;
    } else {
        fill = hover ? kGrayButtonHover : kGrayButton;
        txt = hover ? kGrayButtonText : RGB(140, 140, 140);
    }

    FillRoundRectColor(hdc, rc, fill, 5);
    SelectObject(hdc, font_);
    DrawTextIn(hdc, text, rc, txt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
