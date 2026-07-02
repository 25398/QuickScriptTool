// ── 热键捕获对话框实现 ──────────────────────────────────────
#include "hotkey_dialog.h"
#include <windowsx.h>

HotkeyCapture::HotkeyCapture() = default;
HotkeyCapture::~HotkeyCapture() = default;

bool HotkeyCapture::Show(HWND owner, const Hotkey& oldValue,
                          bool scriptHotkey, Hotkey& out,
                          bool globalStartStop) {
    old_ = oldValue;
    current_ = oldValue;
    scriptHotkey_ = scriptHotkey;
    globalStartStop_ = globalStartStop;
    done_ = false;
    accepted_ = false;

    WNDCLASSW wc{};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &HotkeyCapture::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"HotkeyCaptureWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(
        GetStockObject(WHITE_BRUSH));
    RegisterClassW(&wc);

    RECT ownerRc{};
    GetWindowRect(owner, &ownerRc);
    const int dialogW = 364, dialogH = 210;
    const int x = ownerRc.left
        + ((ownerRc.right - ownerRc.left) - dialogW) / 2;
    const int y = ownerRc.top
        + ((ownerRc.bottom - ownerRc.top) - dialogH) / 2;
    hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME,
        wc.lpszClassName, L"设置热键", WS_POPUP,
        x, y, dialogW, dialogH, owner, nullptr,
        GetModuleHandleW(nullptr), this);
    EnableWindow(owner, FALSE);
    ShowWindow(hwnd_, SW_SHOW);
    SetFocus(hwnd_);
    InvalidateRect(hwnd_, nullptr, TRUE);
    UpdateWindow(hwnd_);

    MSG msg{};
    while (!done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (accepted_) out = current_;
    return accepted_;
}

// ── 窗口过程 ──────────────────────────────────────────────────
LRESULT CALLBACK HotkeyCapture::WndProc(HWND hwnd, UINT msg,
                                         WPARAM wp, LPARAM lp) {
    HotkeyCapture* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<HotkeyCapture*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<HotkeyCapture*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->Handle(msg, wp, lp)
                : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT HotkeyCapture::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        font_ = CreateFontW(22, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        valueFont_ = CreateFontW(28, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        whiteBrush_ = CreateSolidBrush(RGB(255, 255, 255));
        bottomBrush_ = CreateSolidBrush(RGB(247, 247, 247));
        grayButtonBrush_ = CreateSolidBrush(RGB(245, 245, 245));
        greenButtonBrush_ = CreateSolidBrush(RGB(70, 185, 111));
        CreateCaptureControls();
        UpdateCaptureControls();
        return 0;
    case WM_PAINT: Paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_COMMAND:
        if (HIWORD(wp) == STN_CLICKED && HandleCommand(LOWORD(wp)))
            return 0;
        return 0;
    case WM_CTLCOLORSTATIC:
        return OnCtlColor(reinterpret_cast<HDC>(wp),
            reinterpret_cast<HWND>(lp));
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt{}; GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            SetCursor(LoadCursorW(nullptr,
                HitButton(pt.x, pt.y) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        CaptureKey(static_cast<UINT>(wp)); return 0;
    case WM_LBUTTONDOWN:
        if (HandleClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)))
            return 0;
        CaptureKey(VK_LBUTTON); return 0;
    case WM_RBUTTONDOWN: CaptureKey(VK_RBUTTON); return 0;
    case WM_MBUTTONDOWN: CaptureKey(VK_MBUTTON); return 0;
    case WM_CLOSE: done_ = true; DestroyWindow(hwnd_); return 0;
    case WM_DESTROY:
        CleanupGdi(); return 0;
    default:
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

// ── GDI 清理 ─────────────────────────────────────────────────
void HotkeyCapture::CleanupGdi() {
    if (font_) DeleteObject(font_);
    if (valueFont_) DeleteObject(valueFont_);
    if (whiteBrush_) DeleteObject(whiteBrush_);
    if (bottomBrush_) DeleteObject(bottomBrush_);
    if (grayButtonBrush_) DeleteObject(grayButtonBrush_);
    if (greenButtonBrush_) DeleteObject(greenButtonBrush_);
}

// ── 布局 ─────────────────────────────────────────────────────
RECT HotkeyCapture::OkRect() const { return RECT{284, 164, 348, 196}; }
RECT HotkeyCapture::CancelRect() const { return RECT{206, 164, 270, 196}; }
RECT HotkeyCapture::ResetRect() const { return RECT{16, 160, 102, 196}; }
RECT HotkeyCapture::DeleteRect() const { return RECT{108, 160, 194, 196}; }

bool HotkeyCapture::HitButton(int x, int y) const {
    return PtIn(OkRect(), x, y) || PtIn(CancelRect(), x, y)
        || (scriptHotkey_ && !globalStartStop_
            && (PtIn(ResetRect(), x, y) || PtIn(DeleteRect(), x, y)));
}

bool HotkeyCapture::PtIn(RECT r, int x, int y) const {
    return x >= r.left && x <= r.right
        && y >= r.top && y <= r.bottom;
}

// ── 控件工厂 ─────────────────────────────────────────────────
HWND HotkeyCapture::MakeStatic(const wchar_t* text, int id, RECT rc,
                                DWORD style, HFONT font) {
    HWND child = CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | style,
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
        hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    if (font) SendMessageW(child, WM_SETFONT,
        reinterpret_cast<WPARAM>(font), TRUE);
    return child;
}

void HotkeyCapture::CreateCaptureControls() {
    promptText_ = MakeStatic(L"", 0,
        RECT{16, 16, 348, 74}, SS_LEFT, font_);
    valueText_ = MakeStatic(L"", 0,
        RECT{16, 82, 348, 126}, SS_CENTER | SS_CENTERIMAGE,
        valueFont_);
    resetText_ = MakeStatic(L"重置热键", kReset,
        ResetRect(), SS_NOTIFY | SS_CENTER | SS_CENTERIMAGE, font_);
    deleteText_ = MakeStatic(L"删除热键", kDelete,
        DeleteRect(), SS_NOTIFY | SS_CENTER | SS_CENTERIMAGE, font_);
    cancelText_ = MakeStatic(L"取消", kCancel,
        CancelRect(), SS_NOTIFY | SS_CENTER | SS_CENTERIMAGE, font_);
    okText_ = MakeStatic(L"确定", kOk,
        OkRect(), SS_NOTIFY | SS_CENTER | SS_CENTERIMAGE, font_);
}

void HotkeyCapture::UpdateCaptureControls() {
    const bool newPrompt = scriptHotkey_ || globalStartStop_;
    SetText(promptText_, newPrompt
        ? L"请直接在键盘上输入新的热键"
        : L"请直接在键盘上输入要点击的键（\n截屏键");
    SetText(valueText_,
        current_.enabled ? current_.text : L"无");
    const bool showExtras = scriptHotkey_ && !globalStartStop_;
    ShowWindow(resetText_, showExtras ? SW_SHOW : SW_HIDE);
    ShowWindow(deleteText_, showExtras ? SW_SHOW : SW_HIDE);
}

// ── 命令处理 ─────────────────────────────────────────────────
bool HotkeyCapture::HandleCommand(int id) {
    if (id == kOk) {
        accepted_ = true; done_ = true; DestroyWindow(hwnd_);
        return true;
    }
    if (id == kCancel) {
        done_ = true; DestroyWindow(hwnd_); return true;
    }
    if (scriptHotkey_ && !globalStartStop_ && id == kReset) {
        current_ = old_;
        UpdateCaptureControls();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (scriptHotkey_ && !globalStartStop_ && id == kDelete) {
        current_.enabled = false;
        current_.text = L"无";
        current_.vk = 0;
        current_.modifiers = 0;
        UpdateCaptureControls();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    return false;
}

bool HotkeyCapture::HandleClick(int x, int y) {
    if (PtIn(OkRect(), x, y)) {
        accepted_ = true; done_ = true; DestroyWindow(hwnd_);
        return true;
    }
    if (PtIn(CancelRect(), x, y)) {
        done_ = true; DestroyWindow(hwnd_); return true;
    }
    if (scriptHotkey_ && !globalStartStop_
        && PtIn(ResetRect(), x, y)) {
        current_ = old_;
        UpdateCaptureControls();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (scriptHotkey_ && !globalStartStop_
        && PtIn(DeleteRect(), x, y)) {
        current_.enabled = false;
        current_.text = L"无";
        current_.vk = 0;
        current_.modifiers = 0;
        UpdateCaptureControls();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    return false;
}

// ── 按键捕获 ─────────────────────────────────────────────────
void HotkeyCapture::CaptureKey(UINT vk) {
    if (vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_SHIFT
        || vk == VK_LCONTROL || vk == VK_RCONTROL
        || vk == VK_CONTROL
        || vk == VK_LMENU || vk == VK_RMENU || vk == VK_MENU
        || vk == VK_LWIN || vk == VK_RWIN) return;

    current_.vk = vk;
    current_.modifiers = 0;
    if (GetAsyncKeyState(VK_LCONTROL) & 0x8000
        || GetAsyncKeyState(VK_RCONTROL) & 0x8000)
        current_.modifiers |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_LMENU) & 0x8000
        || GetAsyncKeyState(VK_RMENU) & 0x8000)
        current_.modifiers |= MOD_ALT;
    if (GetAsyncKeyState(VK_LSHIFT) & 0x8000
        || GetAsyncKeyState(VK_RSHIFT) & 0x8000)
        current_.modifiers |= MOD_SHIFT;
    if (GetAsyncKeyState(VK_LWIN) & 0x8000
        || GetAsyncKeyState(VK_RWIN) & 0x8000)
        current_.modifiers |= MOD_WIN;
    current_.text = HotkeyText(current_.modifiers, current_.vk);
    current_.enabled = true;
    UpdateCaptureControls();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ── 控件颜色 ─────────────────────────────────────────────────
LRESULT HotkeyCapture::OnCtlColor(HDC hdc, HWND child) {
    SetBkMode(hdc, OPAQUE);
    if (child == okText_) {
        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkColor(hdc, RGB(70, 185, 111));
        return reinterpret_cast<LRESULT>(greenButtonBrush_);
    }
    if (child == cancelText_ || child == resetText_
        || child == deleteText_) {
        SetTextColor(hdc, RGB(140, 140, 140));
        SetBkColor(hdc, RGB(245, 245, 245));
        return reinterpret_cast<LRESULT>(grayButtonBrush_);
    }
    SetTextColor(hdc,
        child == valueText_ ? RGB(80, 80, 80) : RGB(150, 150, 150));
    SetBkColor(hdc, RGB(255, 255, 255));
    return reinterpret_cast<LRESULT>(whiteBrush_);
}

// ── 绘制 ─────────────────────────────────────────────────────
void HotkeyCapture::Paint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);

    RECT bottom{0, 155, rc.right, rc.bottom};
    HBRUSH bottomBrushLocal = CreateSolidBrush(RGB(247, 247, 247));
    FillRect(hdc, &bottom, bottomBrushLocal);
    DeleteObject(bottomBrushLocal);

    DrawButton(hdc, OkRect(), L"确定", true);
    DrawButton(hdc, CancelRect(), L"取消", false);
    if (scriptHotkey_ && !globalStartStop_) {
        DrawButton(hdc, ResetRect(), L"重置热键", false);
        DrawButton(hdc, DeleteRect(), L"删除热键", false);
    }
    EndPaint(hwnd_, &ps);
}

void HotkeyCapture::DrawButton(HDC hdc, RECT rc,
                                const wchar_t* text, bool green) {
    HBRUSH brush = CreateSolidBrush(
        green ? RGB(70, 185, 111) : RGB(245, 245, 245));
    HPEN pen = CreatePen(PS_SOLID, 1,
        green ? RGB(70, 185, 111) : RGB(188, 188, 188));
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 5, 5);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc,
        green ? RGB(255, 255, 255) : RGB(140, 140, 140));
    HGDIOBJ oldFont = SelectObject(hdc, font_);
    DrawTextW(hdc, text, -1, &rc,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
}
