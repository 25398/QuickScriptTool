#include "macro_debug_window.h"

#include "action_utils.h"
#include "drawing.h"
#include "modern_edit.h"
#include "render_context.h"
#include "taskbar_window.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace {

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

std::wstring FormatMoveMouseRelativeDebug(const ScriptAction& action, int dx, int dy) {
    return BracketIndex(action) + L"相对移动鼠标(" + std::to_wstring(dx) + L"," + std::to_wstring(dy) + L")";
}

std::wstring FormatFindImageDebug(const ScriptAction& action, const ImageMatchResult& rawMatch,
                                  bool hasTarget, int targetX, int targetY) {
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
    std::wstring line = prefix + pctText;
    if (follow[0]) line += L"，" + std::wstring(follow);
    if (hasTarget && (action.findImageFollowUp == 0 || action.findImageFollowUp == 1)) {
        line += L"(" + std::to_wstring(targetX) + L"," + std::to_wstring(targetY) + L")";
    }
    return line;
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

    // 不用 WS_EX_CLIENTEDGE：经典凹陷边在父窗口 BitBlt 时会闪；改为父窗口画 1px 灰边
    const RECT frame = ContentFrameRect();
    edit_ = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        frame.left + 1, frame.top + 1,
        (frame.right - frame.left) - 2, (frame.bottom - frame.top) - 2,
        hwnd_, reinterpret_cast<HMENU>(1), GetModuleHandleW(nullptr), nullptr);
    if (edit_ && bodyFont_) {
        SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        ApplyModernEditBehavior(edit_, true);
    }
    ApplyTopmost();
    outerShadow_.Attach(hwnd_);
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
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        pendingLogs_.clear();
        ++clearEpoch_;
    }
    PostMessageW(hwnd_, WM_DEBUG_CLEAR, 0, 0);
}

void MacroDebugWindow::ClearLogDirect() {
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        pendingLogs_.clear();
        ++clearEpoch_;
    }
    if (edit_) SetWindowTextW(edit_, L"");
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
    unsigned epoch = 0;
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        batch.swap(pendingLogs_);
        epoch = clearEpoch_;
    }
    for (const auto& line : batch) {
        {
            std::lock_guard<std::mutex> lock(logMutex_);
            if (clearEpoch_ != epoch) return;
        }
        AppendLogDirect(line);
    }
}

void MacroDebugWindow::ApplyTopmost() {
    if (!hwnd_) return;
    SetWindowPos(hwnd_, pinned_ ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void MacroDebugWindow::PositionEdit() {
    if (!edit_) return;
    const RECT frame = ContentFrameRect();
    const int w = std::max(1, static_cast<int>(frame.right - frame.left) - 2);
    const int h = std::max(1, static_cast<int>(frame.bottom - frame.top) - 2);
    MoveWindow(edit_, frame.left + 1, frame.top + 1, w, h, TRUE);
}

int MacroDebugWindow::ClientWidth() const {
    RECT rc{};
    if (hwnd_) GetClientRect(hwnd_, &rc);
    return rc.right > 0 ? rc.right : kWindowW;
}

RECT MacroDebugWindow::TitleBarRect() const {
    return RECT{0, 0, ClientWidth(), kTitleH};
}

RECT MacroDebugWindow::ContentFrameRect() const {
    RECT rc{};
    if (hwnd_) GetClientRect(hwnd_, &rc);
    else rc = RECT{0, 0, kWindowW, kWindowH};
    return RECT{
        kContentPad,
        kTitleH + kContentPad,
        rc.right - kContentPad,
        rc.bottom - kContentPad};
}

void MacroDebugWindow::InvalidateTitleBar() {
    if (!hwnd_) return;
    RECT title = TitleBarRect();
    InvalidateRect(hwnd_, &title, FALSE);
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
    // 微信风格置顶图钉：平头顶 + 梯形针身 + 竖直针尖
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    const COLORREF color = pinned ? kWhite : RGB(190, 220, 200);
    const float stroke = pinned ? 2.0f : 1.6f;
    IRenderContext& ctx = ResolveRenderContext(hdc);

    const int topY = cy - 8;
    const int bodyTop = cy - 5;
    const int bodyBot = cy + 2;
    const int tipY = cy + 10;
    const int headHalf = 6;
    const int bodyTopHalf = 4;
    const int bodyBotHalf = 3;

    ctx.DrawLine(cx - headHalf, topY, cx + headHalf, topY, color, stroke);
    POINT body[4] = {
        {cx - bodyTopHalf, bodyTop},
        {cx + bodyTopHalf, bodyTop},
        {cx + bodyBotHalf, bodyBot},
        {cx - bodyBotHalf, bodyBot},
    };
    if (pinned) {
        ctx.DrawPolygon(body, 4, color, true);
    } else {
        ctx.DrawLine(body[0].x, body[0].y, body[1].x, body[1].y, color, stroke);
        ctx.DrawLine(body[1].x, body[1].y, body[2].x, body[2].y, color, stroke);
        ctx.DrawLine(body[2].x, body[2].y, body[3].x, body[3].y, color, stroke);
        ctx.DrawLine(body[3].x, body[3].y, body[0].x, body[0].y, color, stroke);
    }
    ctx.DrawLine(cx, bodyBot, cx, tipY, color, stroke);
}

void MacroDebugWindow::DrawTitleButtons(HDC hdc) {
    if (pinned_) {
        ::FillAlphaRect(hdc, PinRect(), RGB(255, 210, 80), 90);
    }
    if (hoverPin_) ::FillAlphaRect(hdc, PinRect(), RGB(0, 0, 0), kCloseHoverAlpha);
    if (hoverMin_) ::FillAlphaRect(hdc, MinimizeRect(), RGB(0, 0, 0), kCloseHoverAlpha);
    if (hoverClose_) ::FillAlphaRect(hdc, CloseRect(), RGB(0, 0, 0), kCloseHoverAlpha);
    DrawPinIcon(hdc, PinRect(), pinned_);

    // 用线段绘制最小化“-”，避免字体字形被渲染成竖线
    const RECT minRc = MinimizeRect();
    const int minCx = (minRc.left + minRc.right) / 2;
    const int minCy = (minRc.top + minRc.bottom) / 2;
    ResolveRenderContext(hdc).DrawLine(minCx - 7, minCy, minCx + 7, minCy, kWhite, 2.0f);

    SelectObject(hdc, closeFont_);
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
    RenderBatchScope batch(mem);

    FillRectColor(mem, rc, kBatchSelectedRow);
    FillRectColor(mem, TitleBarRect(), kMainGreen);
    // 内容区底板 + 灰边（EDIT 内缩 1px）；边框画在父窗口上，避免 CLIENTEDGE 被盖住后闪
    const RECT frame = ContentFrameRect();
    if (frame.right > frame.left && frame.bottom > frame.top) {
        FillRectColor(mem, frame, kWhite);
        DrawBorderRect(mem, frame, kComboBorderGray);
    }
    SelectObject(mem, titleFont_);
    DrawTextIn(mem, L"宏调试信息输出窗口",
        RECT{16, 0, rc.right - kTitleBtnW * 3, kTitleH}, kWhite,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTitleButtons(mem);

    batch.End();
    // 禁止 BitBlt 盖住 EDIT：否则悬停标题按钮全窗重绘时，日志区会被薄荷绿盖住再重画，
    // ClearType/边框会“突然变一下”，像字体跳变。
    if (edit_ && IsWindowVisible(edit_)) {
        RECT editRc{};
        GetWindowRect(edit_, &editRc);
        MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&editRc), 2);
        ExcludeClipRect(hdc, editRc.left, editRc.top, editRc.right, editRc.bottom);
    }
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
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetBkColor(dc, kWhite);
        SetTextColor(dc, kText);
        return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
    }
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
            InvalidateTitleBar();
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
            InvalidateTitleBar();
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
