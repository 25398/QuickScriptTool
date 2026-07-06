#include "macro_debug_window.h"

#include "action_utils.h"
#include "drawing.h"
#include "modern_edit.h"
#include "taskbar_window.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace {

void DrawTextIn(HDC hdc, const std::wstring& text, RECT rc, COLORREF color,
                UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &rc, format);
}

void FillAlphaRect(HDC hdc, RECT rc, COLORREF color, BYTE alpha) {
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = alpha;
    bf.AlphaFormat = 0;
    HDC mem = CreateCompatibleDC(hdc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        DeleteDC(mem);
        return;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    HBRUSH brush = CreateSolidBrush(color);
    RECT fill{0, 0, w, h};
    FillRect(mem, &fill, brush);
    DeleteObject(brush);
    AlphaBlend(hdc, rc.left, rc.top, w, h, mem, 0, 0, w, h, bf);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

int FormatMatchPercent(double score) {
    return static_cast<int>(std::lround(std::clamp(score, 0.0, 100.0)));
}

std::wstring BracketIndex(const ScriptAction& action) {
    return L"[" + std::to_wstring(ActionDebugIndex(action)) + L"]";
}

const wchar_t* FindImageFollowText(int followUp) {
    switch (followUp) {
    case 1: return L"鼠标移动到";
    case 0: return L"点击";
    default: return L"";
    }
}

const wchar_t* OcrFollowText(int followUp) {
    switch (followUp) {
    case 1: return L"然后鼠标移动到";
    case 0: return L"然后点击";
    case 2: return L"";
    default: return L"";
    }
}

}  // namespace

int ActionDebugIndex(const ScriptAction& action) {
    return action.originalNo > 0 ? action.originalNo : 1;
}

std::wstring FormatMacroLoopDebug(int loopIndex) {
    return L"第" + std::to_wstring(loopIndex) + L"次执行鼠标宏";
}

std::wstring FormatGenericActionDebug(const ScriptAction& action) {
    return BracketIndex(action) + ActionName(action);
}

std::wstring FormatMoveMouseDebug(const ScriptAction& action, int x, int y) {
    return BracketIndex(action) + L"移动鼠标到(" + std::to_wstring(x) + L"," + std::to_wstring(y) + L")";
}

std::wstring FormatFindImageDebug(const ScriptAction& action, const ImageMatchResult& rawMatch) {
    const std::wstring prefix = BracketIndex(action) + L"找图，";
    const int pct = FormatMatchPercent(rawMatch.score);
    const std::wstring varName = action.matchVarName.empty() ? L"matchRet" : action.matchVarName;

    if (action.findImageFollowUp == 2) {
        return prefix + L"匹配度" + std::to_wstring(pct) + L"%，保存到变量" + varName;
    }

    const std::wstring pctText = L"匹配度为" + std::to_wstring(pct) + L"%";
    if (!rawMatch.found) {
        return prefix + pctText + L"，未找到匹配";
    }
    const wchar_t* follow = FindImageFollowText(action.findImageFollowUp);
    if (follow[0]) {
        return prefix + pctText + L"，" + follow;
    }
    return prefix + pctText;
}

std::wstring FormatOcrDebug(const ScriptAction& action, const std::wstring& textContent,
                            bool searchFound, const MacroVariableContext& ctx) {
    const std::wstring prefix = BracketIndex(action) + L"文字识别，";
    const std::wstring varName = action.matchVarName.empty() ? L"a" : action.matchVarName;

    if (action.ocrResultMode == 0) {
        std::wstring line = prefix + L"获取文字内容为：" + textContent;
        if (action.ocrFollowUp == 2) {
            line += L"，保存到变量" + varName;
        } else {
            const wchar_t* follow = OcrFollowText(action.ocrFollowUp);
            if (follow[0]) line += L"，" + std::wstring(follow);
        }
        return line;
    }

    const std::wstring target = ResolveMacroVariables(action.ocrSearchText, ctx);
    std::wstring line = prefix + L"文字查找：" + target;
    if (action.ocrFollowUp == 2) {
        line += L"，保存到变量" + varName;
    } else if (searchFound) {
        const wchar_t* follow = OcrFollowText(action.ocrFollowUp);
        if (follow[0]) line += L"，" + std::wstring(follow);
    } else {
        line += L"，未找到";
    }
    return line;
}

void MacroDebugWindow::Create(HFONT bodyFont, HFONT titleFont, HFONT closeFont,
                              std::function<void()> onClosed) {
    if (hwnd_) return;
    bodyFont_ = bodyFont;
    titleFont_ = titleFont;
    closeFont_ = closeFont;
    onClosed_ = std::move(onClosed);

    static bool registered = false;
    const wchar_t* clsName = L"QuickScriptMacroDebugWnd";
    if (!registered) {
        WNDCLASSW wc{};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &MacroDebugWindow::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = clsName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
        registered = true;
    }

    const int x = 100;
    const int y = 100;
    hwnd_ = CreateWindowExW(
        0,
        clsName, L"", WS_POPUP | WS_MINIMIZEBOX,
        x, y, kWindowW, kWindowH,
        nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return;

    ApplyTaskbarWindowStyle(hwnd_, L"宏调试信息输出窗口", true);

    const RECT content = RECT{
        kContentPad, kTitleH + kContentPad,
        kWindowW - kContentPad, kWindowH - kContentPad};
    edit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        content.left, content.top,
        content.right - content.left, content.bottom - content.top,
        hwnd_, reinterpret_cast<HMENU>(1), GetModuleHandleW(nullptr), nullptr);
    if (edit_ && bodyFont_) {
        SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        ApplyModernEditBehavior(edit_, true);
    }
    ApplyTopmost();
}

void MacroDebugWindow::Show() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
}

void MacroDebugWindow::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void MacroDebugWindow::Destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        edit_ = nullptr;
    }
    bodyFont_ = titleFont_ = closeFont_ = nullptr;
    onClosed_ = nullptr;
}

void MacroDebugWindow::CloseByUser() {
    Hide();
    if (onClosed_) onClosed_();
}

void MacroDebugWindow::AppendLog(const std::wstring& text) {
    if (!hwnd_) return;
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        pendingLogs_.push_back(text);
    }
    PostMessageW(hwnd_, WM_DEBUG_APPEND, 0, 0);
}

void MacroDebugWindow::ClearLog() {
    if (!hwnd_) return;
    PostMessageW(hwnd_, WM_DEBUG_CLEAR, 0, 0);
}

void MacroDebugWindow::ClearLogDirect() {
    if (!edit_) return;
    SetWindowTextW(edit_, L"");
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        pendingLogs_.clear();
    }
}

void MacroDebugWindow::AppendLogDirect(const std::wstring& text) {
    if (!edit_) return;
    std::wstring line = text + L"\r\n";
    const int len = GetWindowTextLengthW(edit_);
    SendMessageW(edit_, EM_SETSEL, len, len);
    SendMessageW(edit_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
}

void MacroDebugWindow::FlushPendingLogs() {
    std::vector<std::wstring> batch;
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        batch.swap(pendingLogs_);
    }
    for (const auto& line : batch) AppendLogDirect(line);
}

void MacroDebugWindow::ApplyTopmost() {
    if (!hwnd_) return;
    SetWindowPos(hwnd_, pinned_ ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void MacroDebugWindow::PositionEdit() {
    if (!edit_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const RECT content{
        kContentPad, kTitleH + kContentPad,
        rc.right - kContentPad, rc.bottom - kContentPad};
    MoveWindow(edit_, content.left, content.top,
        content.right - content.left, content.bottom - content.top, TRUE);
}

int MacroDebugWindow::ClientWidth() const {
    RECT rc{};
    if (hwnd_) GetClientRect(hwnd_, &rc);
    return rc.right > 0 ? rc.right : kWindowW;
}

RECT MacroDebugWindow::CloseRect() const {
    const int w = ClientWidth();
    return RECT{w - kCloseBtnW, 0, w, kTitleH};
}

RECT MacroDebugWindow::MinimizeRect() const {
    const RECT close = CloseRect();
    return RECT{close.left - kTitleBtnW, 0, close.left, kTitleH};
}

RECT MacroDebugWindow::PinRect() const {
    const RECT min = MinimizeRect();
    return RECT{min.left - kTitleBtnW, 0, min.left, kTitleH};
}

bool MacroDebugWindow::HitClose(int x, int y) const {
    const RECT rc = CloseRect();
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

bool MacroDebugWindow::HitMinimize(int x, int y) const {
    const RECT rc = MinimizeRect();
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

bool MacroDebugWindow::HitPin(int x, int y) const {
    const RECT rc = PinRect();
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

bool MacroDebugWindow::HitTitleBar(int x, int y) const {
    return y >= 0 && y < kTitleH && x < PinRect().left;
}

void MacroDebugWindow::DrawPinIcon(HDC hdc, const RECT& rc, bool pinned) {
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2 + 1;
    if (pinned) {
        HBRUSH headBrush = CreateSolidBrush(RGB(255, 255, 220));
        HGDIOBJ oldBrush = SelectObject(hdc, headBrush);
        HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
        const int headR = 6;
        Ellipse(hdc, cx - headR, cy - headR - 4, cx + headR, cy + headR - 4);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(headBrush);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        oldPen = SelectObject(hdc, pen);
        oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Ellipse(hdc, cx - headR, cy - headR - 4, cx + headR, cy + headR - 4);
        MoveToEx(hdc, cx, cy + 1, nullptr);
        LineTo(hdc, cx, cy + 10);
        MoveToEx(hdc, cx - 5, cy + 10, nullptr);
        LineTo(hdc, cx + 5, cy + 10);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    } else {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(190, 220, 200));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        const int headR = 5;
        Ellipse(hdc, cx - headR, cy - headR - 4, cx + headR, cy + headR - 4);
        MoveToEx(hdc, cx, cy + 1, nullptr);
        LineTo(hdc, cx, cy + 10);
        MoveToEx(hdc, cx - 4, cy + 10, nullptr);
        LineTo(hdc, cx + 4, cy + 10);
        MoveToEx(hdc, rc.left + 8, rc.bottom - 8, nullptr);
        LineTo(hdc, rc.right - 8, rc.top + 8);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }
}

void MacroDebugWindow::DrawTitleButtons(HDC hdc) {
    if (pinned_) {
        FillAlphaRect(hdc, PinRect(), RGB(255, 210, 80), 90);
    }
    if (hoverPin_) FillAlphaRect(hdc, PinRect(), RGB(0, 0, 0), kCloseHoverAlpha);
    if (hoverMin_) FillAlphaRect(hdc, MinimizeRect(), RGB(0, 0, 0), kCloseHoverAlpha);
    if (hoverClose_) FillAlphaRect(hdc, CloseRect(), RGB(0, 0, 0), kCloseHoverAlpha);
    DrawPinIcon(hdc, PinRect(), pinned_);
    SelectObject(hdc, closeFont_);
    DrawTextIn(hdc, L"−", MinimizeRect(), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawTextIn(hdc, L"×", CloseRect(), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void MacroDebugWindow::Paint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    FillRectColor(mem, rc, kBatchSelectedRow);
    FillRectColor(mem, RECT{0, 0, rc.right, kTitleH}, kMainGreen);
    SelectObject(mem, titleFont_);
    DrawTextIn(mem, L"宏调试信息输出窗口",
        RECT{16, 0, rc.right - kTitleBtnW * 3, kTitleH}, kWhite,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTitleButtons(mem);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
}

void MacroDebugWindow::CleanupGdi() {
    // 字体由 MainWindow 拥有，不在此释放
    bodyFont_ = titleFont_ = closeFont_ = nullptr;
}

LRESULT CALLBACK MacroDebugWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MacroDebugWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<MacroDebugWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return TRUE;
    }
    self = reinterpret_cast<MacroDebugWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT MacroDebugWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        if (HitClose(pt.x, pt.y) || HitMinimize(pt.x, pt.y) || HitPin(pt.x, pt.y)) return HTCLIENT;
        if (HitTitleBar(pt.x, pt.y)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        PositionEdit();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        const bool hoverPin = HitPin(x, y);
        const bool hoverMin = HitMinimize(x, y);
        const bool hoverClose = HitClose(x, y);
        if (hoverPin != hoverPin_ || hoverMin != hoverMin_ || hoverClose != hoverClose_) {
            hoverPin_ = hoverPin;
            hoverMin_ = hoverMin;
            hoverClose_ = hoverClose;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (hoverPin || hoverMin || hoverClose) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
        } else if (HitTitleBar(x, y)) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (HitClose(x, y)) {
            CloseByUser();
            return 0;
        }
        if (HitMinimize(x, y)) {
            ShowWindow(hwnd_, SW_MINIMIZE);
            return 0;
        }
        if (HitPin(x, y)) {
            pinned_ = !pinned_;
            ApplyTopmost();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (HitTitleBar(x, y)) {
            ReleaseCapture();
            SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, lp);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (wp == VK_CANCEL || (ctrl && wp == 'C' && GetFocus() != edit_)) {
            CloseByUser();
            return 0;
        }
        break;
    }
    case WM_DEBUG_APPEND:
        FlushPendingLogs();
        return 0;
    case WM_DEBUG_CLEAR:
        ClearLogDirect();
        return 0;
    case WM_CLOSE:
        CloseByUser();
        return 0;
    case WM_DESTROY:
        hwnd_ = nullptr;
        edit_ = nullptr;
        CleanupGdi();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
