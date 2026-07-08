// ── 热键/按键捕获模态对话框 ────────────────────────────────────────
// UI 绘制沿用现有样式；消息泵、悬停与关闭流程参考定时任务对话框。
#include "hotkey_dialog.h"
#include "drawing.h"
#include "scheduled_task_ui.h"
#include "taskbar_window.h"
#include <windowsx.h>

namespace {

constexpr int kDlgW = 364;
constexpr int kDlgH = 210;
constexpr wchar_t kDlgClass[] = L"QuickScriptHotkeyDlg";

}  // namespace

bool HotkeyCapture::Show(HWND owner, const Hotkey& oldValue,
                         bool scriptHotkey, Hotkey& out,
                         bool globalStartStop) {
    owner_ = owner;
    old_ = oldValue;
    current_ = oldValue;
    scriptHotkey_ = scriptHotkey;
    globalStartStop_ = globalStartStop;
    done_ = false;
    accepted_ = false;
    trackingMouse_ = false;
    hoverOk_ = hoverCancel_ = hoverReset_ = hoverDelete_ = false;

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
    case WM_SYSKEYDOWN: OnKey(static_cast<UINT>(wp)); return 0;
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

void HotkeyCapture::Close(bool accept) {
    if (done_) return;
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
        current_ = old_;
        InvalidateValueArea();
        return;
    }
    if (InRect(DeleteRect(), x, y)) {
        current_.enabled = false;
        current_.text = L"无";
        current_.vk = 0;
        current_.modifiers = 0;
        InvalidateValueArea();
    }
}

void HotkeyCapture::OnKey(UINT vk) {
    if (vk == VK_PROCESSKEY) return;
    if (vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_SHIFT
        || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_CONTROL
        || vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU) return;

    current_.vk = vk;
    current_.modifiers = 0;
    if (GetAsyncKeyState(VK_LCONTROL) & 0x8000 || GetAsyncKeyState(VK_RCONTROL) & 0x8000)
        current_.modifiers |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_LMENU) & 0x8000 || GetAsyncKeyState(VK_RMENU) & 0x8000)
        current_.modifiers |= MOD_ALT;
    if (GetAsyncKeyState(VK_LSHIFT) & 0x8000 || GetAsyncKeyState(VK_RSHIFT) & 0x8000)
        current_.modifiers |= MOD_SHIFT;
    if (GetAsyncKeyState(VK_LWIN) & 0x8000 || GetAsyncKeyState(VK_RWIN) & 0x8000)
        current_.modifiers |= MOD_WIN;
    current_.text = HotkeyText(current_.modifiers, current_.vk);
    current_.enabled = true;
    InvalidateValueArea();
}

void HotkeyCapture::InvalidateValueArea() {
    const RECT rc = ValueRect();
    InvalidateRect(hwnd_, &rc, FALSE);
}

void HotkeyCapture::OnPaint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    FillRectColor(hdc, rc, RGB(255, 255, 255));

    RECT bottom{0, 155, rc.right, rc.bottom};
    FillRectColor(hdc, bottom, RGB(247, 247, 247));

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(150, 150, 150));
    SelectObject(hdc, font_);
    const wchar_t* prompt = (scriptHotkey_ || globalStartStop_)
        ? L"请直接在键盘上输入新的热键"
        : L"请直接在键盘上输入要点击的键（\n截屏键";
    RECT promptRc{16, 16, 348, 74};
    DrawTextW(hdc, prompt, -1, &promptRc, DT_LEFT);

    SelectObject(hdc, valueFont_);
    SetTextColor(hdc, RGB(80, 80, 80));
    const std::wstring val = current_.enabled ? current_.text : L"无";
    RECT valRc = ValueRect();
    DrawTextW(hdc, val.c_str(), -1, &valRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    DrawBtn(hdc, OkRect(), L"确定", true, hoverOk_);
    DrawBtn(hdc, CancelRect(), L"取消", true, hoverCancel_);
    if (scriptHotkey_ && !globalStartStop_) {
        DrawBtn(hdc, ResetRect(), L"重置热键", false, hoverReset_);
        DrawBtn(hdc, DeleteRect(), L"删除热键", false, hoverDelete_);
    }

    EndPaint(hwnd_, &ps);
}

void HotkeyCapture::DrawBtn(HDC hdc, const RECT& rc, const wchar_t* text,
                            bool green, bool hover) {
    COLORREF fill, border, txt;
    if (green) {
        fill = hover ? kButtonGreenHover : kButtonGreen;
        border = fill;
        txt = kWhite;
    } else {
        fill = hover ? kGrayButtonHover : kGrayButton;
        border = hover ? kGrayButtonHover : kGrayButtonBorder;
        txt = hover ? kGrayButtonText : RGB(140, 140, 140);
    }

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 5, 5);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, txt);
    HGDIOBJ oldFont = SelectObject(hdc, font_);
    DrawTextW(hdc, text, -1, const_cast<RECT*>(&rc),
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
}
