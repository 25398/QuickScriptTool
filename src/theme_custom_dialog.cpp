#include "theme_custom_dialog.h"

#include "app_theme.h"
#include "config.h"
#include "drawing.h"
#include "render_context.h"
#include "scheduled_task_ui.h"
#include "taskbar_window.h"
#include "theme_ui_layout.h"
#include "ui_scale.h"

#include <algorithm>
#include <cmath>
#include <string>

#include <windowsx.h>

namespace quickscript {
namespace {

constexpr wchar_t kThemeDlgClass[] = L"QuickScriptThemeCustomDlg";
constexpr wchar_t kColorDlgClass[] = L"QuickScriptThemeColorDlg";

int S(int designPx) { return UiLen(designPx); }

RECT ScaleRect(const RECT& d) {
    return RECT{S(d.left), S(d.top), S(d.right), S(d.bottom)};
}

float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

void RgbToHsv(COLORREF c, float& h, float& s, float& v) {
    const float r = GetRValue(c) / 255.0f;
    const float g = GetGValue(c) / 255.0f;
    const float b = GetBValue(c) / 255.0f;
    const float maxv = (std::max)({r, g, b});
    const float minv = (std::min)({r, g, b});
    const float d = maxv - minv;
    v = maxv;
    s = (maxv <= 0.0001f) ? 0.0f : d / maxv;
    if (d <= 0.0001f) {
        h = 0.0f;
        return;
    }
    if (maxv == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (maxv == g) h = (b - r) / d + 2.0f;
    else h = (r - g) / d + 4.0f;
    h /= 6.0f;
}

COLORREF HsvToRgb(float h, float s, float v) {
    h = h - std::floor(h);
    s = Clamp01(s);
    v = Clamp01(v);
    const float i = std::floor(h * 6.0f);
    const float f = h * 6.0f - i;
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - f * s);
    const float t = v * (1.0f - (1.0f - f) * s);
    float r = 0, g = 0, b = 0;
    switch (static_cast<int>(i) % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return RGB(
        static_cast<int>(std::lround(r * 255.0f)),
        static_cast<int>(std::lround(g * 255.0f)),
        static_cast<int>(std::lround(b * 255.0f)));
}

COLORREF HueColor(float h) { return HsvToRgb(h, 1.0f, 1.0f); }

void DrawSwatch(HDC hdc, const RECT& rc, COLORREF color, bool selected) {
    FillRoundRectColor(hdc, rc, color, S(6));
    DrawBorderRoundRect(hdc, rc, selected ? kMainGreen : kComboBorderGray, S(6));
}

void DrawOutlineBtn(HDC hdc, HFONT font, const RECT& rc, const wchar_t* text, bool hover) {
    FillRoundRectColor(hdc, rc, hover ? kComboHoverGreen : kWhite, S(6));
    DrawBorderRoundRect(hdc, rc, hover ? kMainGreen : kComboBorderGray, S(6));
    SelectObject(hdc, font);
    DrawTextIn(hdc, text, rc, kMainGreen, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

HFONT MakeUiFont(int designHeight, int weight) {
    return CreateFontW(UiFontHeight(designHeight), 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
}

/// 子模态消息泵：不禁用 owner（避免激活跳到主窗闪烁），吞掉背景窗绘制/输入/定时器。
bool ThemeModalPump(const MSG& msg, HWND dialog, HWND blockA, HWND blockB = nullptr) {
    if (msg.message == WM_QUIT) return true;
    if (!msg.hwnd) return true;
    if (msg.hwnd == dialog || IsChild(dialog, msg.hwnd)) return true;
    if (StIsImeUiWindow(msg.hwnd)) return true;
    if (msg.message >= WM_IME_SETCONTEXT && msg.message <= WM_IME_KEYUP) return true;

    auto isBlocked = [&](HWND hwnd) {
        if (!hwnd) return false;
        if (blockA && (hwnd == blockA || IsChild(blockA, hwnd))) return true;
        if (blockB && (hwnd == blockB || IsChild(blockB, hwnd))) return true;
        return false;
    };

    if (msg.message == WM_PAINT || msg.message == WM_NCPAINT || msg.message == WM_ERASEBKGND) {
        ValidateRect(msg.hwnd, nullptr);
        return false;
    }
    if (msg.message == WM_TIMER) return false;
    if (msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST) return false;
    if (msg.message >= WM_NCMOUSEMOVE && msg.message <= WM_NCMBUTTONDBLCLK) return false;
    if (msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) return false;
    if (msg.message == WM_CLOSE || msg.message == WM_SYSCOMMAND) return false;
    if (isBlocked(msg.hwnd)) return false;
    // 其余同线程消息一律不派发，避免主窗被激活重绘闪一下
    return false;
}

void RunThemeModalLoop(HWND dialog, HWND blockA, HWND blockB, bool& doneFlag) {
    MSG msg{};
    while (!doneFlag && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            doneFlag = true;
            break;
        }
        if (!ThemeModalPump(msg, dialog, blockA, blockB)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ── 主题风取色弹窗 ───────────────────────────────────────────────

class ThemedColorPicker {
public:
    bool Show(HWND owner, COLORREF& colorInOut) {
        owner_ = owner;
        color_ = colorInOut;
        RgbToHsv(color_, hue_, sat_, val_);
        done_ = false;
        accepted_ = false;
        draggingSv_ = draggingHue_ = false;
        hoverOk_ = hoverCancel_ = hoverClose_ = false;

        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = &ThemedColorPicker::WndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = kColorDlgClass;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr;
            RegisterClassW(&wc);
            registered = true;
        }

        UiScaleInitFromHwnd(owner);
        const int dlgW = S(kDlgW);
        const int dlgH = S(kDlgH);
        RECT ownerRc{};
        if (!owner || !GetWindowRect(owner, &ownerRc)) {
            ownerRc = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        }
        const int x = ownerRc.left + ((ownerRc.right - ownerRc.left) - dlgW) / 2;
        const int y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - dlgH) / 2;

        // 无 owner 句柄作父窗、不 EnableWindow，避免激活落到主窗造成闪烁
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kColorDlgClass, L"选择颜色",
            WS_POPUP, x, y, dlgW, dlgH, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd_) return false;

        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        UpdateWindow(hwnd_);
        SetForegroundWindow(hwnd_);

        RunThemeModalLoop(hwnd_, owner_, nullptr, done_);

        if (IsWindow(hwnd_)) {
            ShowWindow(hwnd_, SW_HIDE);
            DestroyWindow(hwnd_);
        }
        hwnd_ = nullptr;
        StDiscardSpuriousInputAfterModal(owner_);
        if (owner_ && IsWindow(owner_)) SetForegroundWindow(owner_);
        StDiscardSpuriousInputAfterModal(owner_);

        if (accepted_) colorInOut = color_;
        return accepted_;
    }

private:
    static constexpr int kDlgW = theme_ui::kColorDlgW;
    static constexpr int kDlgH = theme_ui::kColorDlgH;
    static constexpr int kTitleH = theme_ui::kColorTitleH;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        ThemedColorPicker* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<ThemedColorPicker*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
            return TRUE;
        }
        self = reinterpret_cast<ThemedColorPicker*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE: OnCreate(); return 0;
        case WM_PAINT: OnPaint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_LBUTTONDOWN: OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_LBUTTONUP: OnLButtonUp(); return 0;
        case WM_MOUSELEAVE: OnMouseLeave(); return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                OnSetCursor();
                return TRUE;
            }
            break;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { Close(false); return 0; }
            if (wp == VK_RETURN) { Close(true); return 0; }
            break;
        case WM_CLOSE: Close(false); return 0;
        case WM_DESTROY: Cleanup(); return 0;
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            if (pt.y >= 0 && pt.y < S(kTitleH) && !StPtIn(CloseRect(), pt.x, pt.y))
                return HTCAPTION;
            break;
        }
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    RECT CloseRect() const { return ScaleRect(theme_ui::MakeColorPickerLayout().close); }
    RECT SvRect() const { return ScaleRect(theme_ui::MakeColorPickerLayout().sv); }
    RECT HueRect() const { return ScaleRect(theme_ui::MakeColorPickerLayout().hue); }
    RECT PreviewRect() const { return ScaleRect(theme_ui::MakeColorPickerLayout().preview); }
    RECT PreviewLabelRect() const {
        return ScaleRect(theme_ui::MakeColorPickerLayout().previewLabel);
    }
    RECT OkRect() const { return ScaleRect(theme_ui::MakeColorPickerLayout().ok); }
    RECT CancelRect() const { return ScaleRect(theme_ui::MakeColorPickerLayout().cancel); }

    void OnCreate() {
        titleFont_ = MakeUiFont(theme_ui::kTitleFontDesign, FW_NORMAL);
        bodyFont_ = MakeUiFont(theme_ui::kBodyFontDesign, FW_NORMAL);
        closeFont_ = MakeUiFont(theme_ui::kCloseFontDesign, FW_NORMAL);
        outerShadow_.Attach(hwnd_);
        EnsureHueCache();
        EnsureSvCache();
    }

    void Cleanup() {
        ReleaseCaches();
        if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
        if (bodyFont_) { DeleteObject(bodyFont_); bodyFont_ = nullptr; }
        if (closeFont_) { DeleteObject(closeFont_); closeFont_ = nullptr; }
        outerShadow_.Detach();
    }

    void ReleaseCaches() {
        if (svBmp_) { DeleteObject(svBmp_); svBmp_ = nullptr; }
        if (hueBmp_) { DeleteObject(hueBmp_); hueBmp_ = nullptr; }
        svCacheHue_ = -1.0f;
        svCacheW_ = svCacheH_ = 0;
        hueCacheW_ = hueCacheH_ = 0;
    }

    void Close(bool accept) {
        if (done_) return;
        accepted_ = accept;
        done_ = true;
        if (IsWindow(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
    }

    void SyncColorFromHsv() {
        color_ = HsvToRgb(hue_, sat_, val_);
    }

    void EnsureHueCache() {
        const RECT rc = HueRect();
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 1 || h <= 1) return;
        if (hueBmp_ && hueCacheW_ == w && hueCacheH_ == h) return;

        if (hueBmp_) { DeleteObject(hueBmp_); hueBmp_ = nullptr; }
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC screen = GetDC(nullptr);
        hueBmp_ = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!hueBmp_ || !bits) return;

        auto* px = static_cast<DWORD*>(bits);
        for (int y = 0; y < h; ++y) {
            const COLORREF c = HueColor(static_cast<float>(y) / (h - 1));
            const DWORD pixel = 0xFF000000u
                | (static_cast<DWORD>(GetRValue(c)))
                | (static_cast<DWORD>(GetGValue(c)) << 8)
                | (static_cast<DWORD>(GetBValue(c)) << 16);
            for (int x = 0; x < w; ++x)
                px[y * w + x] = pixel;
        }
        hueCacheW_ = w;
        hueCacheH_ = h;
    }

    // 返回 true 表示重建了 SV 位图
    bool EnsureSvCache() {
        const RECT rc = SvRect();
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w <= 1 || h <= 1) return false;
        const float hueKey = std::round(hue_ * 128.0f) / 128.0f;
        if (svBmp_ && svCacheW_ == w && svCacheH_ == h
            && std::fabs(svCacheHue_ - hueKey) < 0.0001f) {
            return false;
        }

        if (svBmp_) { DeleteObject(svBmp_); svBmp_ = nullptr; }
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC screen = GetDC(nullptr);
        svBmp_ = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!svBmp_ || !bits) return false;

        auto* px = static_cast<DWORD*>(bits);
        for (int y = 0; y < h; ++y) {
            const float vv = 1.0f - static_cast<float>(y) / (h - 1);
            for (int x = 0; x < w; ++x) {
                const float ss = static_cast<float>(x) / (w - 1);
                const COLORREF c = HsvToRgb(hueKey, ss, vv);
                px[y * w + x] = 0xFF000000u
                    | (static_cast<DWORD>(GetRValue(c)))
                    | (static_cast<DWORD>(GetGValue(c)) << 8)
                    | (static_cast<DWORD>(GetBValue(c)) << 16);
            }
        }
        svCacheW_ = w;
        svCacheH_ = h;
        svCacheHue_ = hueKey;
        return true;
    }

    static constexpr int kCursorPad = 8;

    void DrawSvCursor(HDC hdc, const RECT& rc) {
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        const int cx = rc.left + static_cast<int>(sat_ * (w - 1));
        const int cy = rc.top + static_cast<int>((1.0f - val_) * (h - 1));
        // 限制在色板内，避免圆圈画出边界后留下残留
        const int saved = SaveDC(hdc);
        IntersectClipRect(hdc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        Ellipse(hdc, cx - 6, cy - 6, cx + 6, cy + 6);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
        pen = CreatePen(PS_SOLID, 1, RGB(40, 40, 40));
        oldPen = SelectObject(hdc, pen);
        Ellipse(hdc, cx - 6, cy - 6, cx + 6, cy + 6);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
        RestoreDC(hdc, saved);
    }

    void PaintSv(HDC hdc, const RECT& rc) {
        EnsureSvCache();
        if (!svBmp_) return;
        HDC mem = CreateCompatibleDC(hdc);
        HGDIOBJ old = SelectObject(mem, svBmp_);
        BitBlt(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
        DrawBorderRect(hdc, rc, kComboBorderGray);
        DrawSvCursor(hdc, rc);
    }

    void PaintHue(HDC hdc, const RECT& rc) {
        EnsureHueCache();
        if (hueBmp_) {
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(mem, hueBmp_);
            BitBlt(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        DrawBorderRect(hdc, rc, kComboBorderGray);
        const int cy = rc.top + static_cast<int>(hue_ * (rc.bottom - rc.top - 1));
        const int saved = SaveDC(hdc);
        IntersectClipRect(hdc, rc.left - 4, rc.top, rc.right + 4, rc.bottom);
        RECT mark{rc.left - 3, cy - 3, rc.right + 3, cy + 3};
        DrawBorderRect(hdc, mark, RGB(40, 40, 40));
        RestoreDC(hdc, saved);
    }

    /// 色板区域离屏合成后再一次 BitBlt，避免先抹白再画造成闪烁
    void PaintSvLiveBuffered(HDC hdc) {
        const RECT sv = SvRect();
        RECT area = sv;
        InflateRect(&area, kCursorPad, kCursorPad);
        const int w = area.right - area.left;
        const int h = area.bottom - area.top;
        if (w <= 0 || h <= 0) return;

        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        RECT local{0, 0, w, h};
        FillRectColor(mem, local, kWhite);
        POINT oldOrg{};
        SetViewportOrgEx(mem, -area.left, -area.top, &oldOrg);
        PaintSv(mem, sv);
        SetViewportOrgEx(mem, oldOrg.x, oldOrg.y, nullptr);
        BitBlt(hdc, area.left, area.top, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    void PaintHueLiveBuffered(HDC hdc) {
        const RECT hue = HueRect();
        RECT area = hue;
        InflateRect(&area, 4, 4);
        const int w = area.right - area.left;
        const int h = area.bottom - area.top;
        if (w <= 0 || h <= 0) return;

        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        RECT local{0, 0, w, h};
        FillRectColor(mem, local, kWhite);
        POINT oldOrg{};
        SetViewportOrgEx(mem, -area.left, -area.top, &oldOrg);
        PaintHue(mem, hue);
        SetViewportOrgEx(mem, oldOrg.x, oldOrg.y, nullptr);
        BitBlt(hdc, area.left, area.top, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    void PaintPreview(HDC hdc) {
        DrawSwatch(hdc, PreviewRect(), color_, true);
    }

    void ApplySvAt(int x, int y) {
        const RECT rc = SvRect();
        const float u = Clamp01(static_cast<float>(x - rc.left) / (rc.right - rc.left - 1));
        const float vv = Clamp01(1.0f - static_cast<float>(y - rc.top) / (rc.bottom - rc.top - 1));
        sat_ = u;
        val_ = vv;
        SyncColorFromHsv();

        HDC hdc = GetDC(hwnd_);
        if (!hdc) return;
        PaintSvLiveBuffered(hdc);
        PaintPreview(hdc);
        ReleaseDC(hwnd_, hdc);
    }

    void ApplyHueAt(int y) {
        const RECT rc = HueRect();
        hue_ = Clamp01(static_cast<float>(y - rc.top) / (rc.bottom - rc.top - 1));
        SyncColorFromHsv();
        const bool svRebuilt = EnsureSvCache();

        HDC hdc = GetDC(hwnd_);
        if (!hdc) return;
        // 色相条每帧更新；SV 仅在色相档位变化时更新，避免整块闪
        PaintHueLiveBuffered(hdc);
        if (svRebuilt)
            PaintSvLiveBuffered(hdc);
        PaintPreview(hdc);
        ReleaseDC(hwnd_, hdc);
    }

    void EnsureTrack() {
        if (tracking_) return;
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        tracking_ = TrackMouseEvent(&tme) != FALSE;
    }

    void UpdateHover(int x, int y) {
        auto set = [&](bool& f, const RECT& rc) {
            const bool v = StPtIn(rc, x, y);
            if (f != v) { f = v; InvalidateRect(hwnd_, &rc, FALSE); }
        };
        set(hoverOk_, OkRect());
        set(hoverCancel_, CancelRect());
        set(hoverClose_, CloseRect());
    }

    void OnMouseMove(int x, int y) {
        EnsureTrack();
        if (draggingSv_) { ApplySvAt(x, y); return; }
        if (draggingHue_) { ApplyHueAt(y); return; }
        UpdateHover(x, y);
    }

    void OnMouseLeave() {
        tracking_ = false;
        if (draggingSv_ || draggingHue_) return;
        hoverOk_ = hoverCancel_ = hoverClose_ = false;
        RECT ok = OkRect(), cancel = CancelRect(), close = CloseRect();
        InvalidateRect(hwnd_, &ok, FALSE);
        InvalidateRect(hwnd_, &cancel, FALSE);
        InvalidateRect(hwnd_, &close, FALSE);
    }

    void OnSetCursor() {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        const bool hand = StPtIn(OkRect(), pt.x, pt.y) || StPtIn(CancelRect(), pt.x, pt.y)
            || StPtIn(CloseRect(), pt.x, pt.y) || StPtIn(SvRect(), pt.x, pt.y)
            || StPtIn(HueRect(), pt.x, pt.y);
        SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
    }

    void OnLButtonDown(int x, int y) {
        if (StPtIn(CloseRect(), x, y) || StPtIn(CancelRect(), x, y)) { Close(false); return; }
        if (StPtIn(OkRect(), x, y)) { Close(true); return; }
        if (StPtIn(SvRect(), x, y)) {
            draggingSv_ = true;
            SetCapture(hwnd_);
            ApplySvAt(x, y);
            return;
        }
        if (StPtIn(HueRect(), x, y)) {
            draggingHue_ = true;
            SetCapture(hwnd_);
            ApplyHueAt(y);
        }
    }

    void OnLButtonUp() {
        if (draggingSv_ || draggingHue_) {
            draggingSv_ = draggingHue_ = false;
            ReleaseCapture();
        }
    }

    void OnPaint() {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        RECT client{};
        GetClientRect(hwnd_, &client);
        const int cw = client.right - client.left;
        const int ch = client.bottom - client.top;

        // 双缓冲：整窗一次 BitBlt，避免拖拽外区域闪白
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, cw, ch);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        RenderBatchScope batch(mem);

        FillRectColor(mem, client, kWhite);
        StDrawTitleBar(mem, titleFont_, closeFont_, S(kDlgW), S(kTitleH), L"选择颜色",
            hoverClose_, CloseRect());
        PaintSv(mem, SvRect());
        PaintHue(mem, HueRect());
        DrawSwatch(mem, PreviewRect(), color_, true);
        SelectObject(mem, bodyFont_);
        DrawTextIn(mem, L"预览", PreviewLabelRect(), RGB(100, 100, 100),
            DT_CENTER | DT_SINGLELINE);
        StDrawGreenButton(mem, bodyFont_, OkRect(), L"确定", hoverOk_);
        DrawOutlineBtn(mem, bodyFont_, CancelRect(), L"取消", hoverCancel_);

        batch.End();
        BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd_, &ps);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT closeFont_ = nullptr;
    WindowOuterShadow outerShadow_;
    HBITMAP svBmp_ = nullptr;
    HBITMAP hueBmp_ = nullptr;
    float svCacheHue_ = -1.0f;
    int svCacheW_ = 0;
    int svCacheH_ = 0;
    int hueCacheW_ = 0;
    int hueCacheH_ = 0;
    COLORREF color_ = RGB(64, 168, 99);
    float hue_ = 0.35f, sat_ = 0.55f, val_ = 0.7f;
    bool done_ = false;
    bool accepted_ = false;
    bool tracking_ = false;
    bool draggingSv_ = false;
    bool draggingHue_ = false;
    bool hoverOk_ = false;
    bool hoverCancel_ = false;
    bool hoverClose_ = false;
};

bool ShowThemedColorPicker(HWND owner, COLORREF& color) {
    ThemedColorPicker picker;
    return picker.Show(owner, color);
}

// ── 自定义主题弹窗 ───────────────────────────────────────────────

class CustomThemeDialog {
public:
    bool Show(HWND owner, COLORREF& mainInOut, COLORREF& accentInOut) {
        owner_ = owner;
        main_ = mainInOut;
        accent_ = accentInOut;
        done_ = false;
        accepted_ = false;
        hoverOk_ = hoverCancel_ = hoverClose_ = false;
        hoverPickMain_ = hoverPickAccent_ = hoverRandom_ = false;
        hoverMainSwatch_ = hoverAccentSwatch_ = false;

        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = &CustomThemeDialog::WndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = kThemeDlgClass;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = nullptr;
            RegisterClassW(&wc);
            registered = true;
        }

        UiScaleInitFromHwnd(owner);
        const int dlgW = S(kDlgW);
        const int dlgH = S(kDlgH);
        RECT ownerRc{};
        if (!owner || !GetWindowRect(owner, &ownerRc)) {
            ownerRc = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        }
        const int x = ownerRc.left + ((ownerRc.right - ownerRc.left) - dlgW) / 2;
        const int y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - dlgH) / 2;

        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kThemeDlgClass, L"自定义主题",
            WS_POPUP, x, y, dlgW, dlgH, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd_) return false;

        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        UpdateWindow(hwnd_);
        SetForegroundWindow(hwnd_);

        RunThemeModalLoop(hwnd_, owner_, nullptr, done_);

        if (IsWindow(hwnd_)) {
            ShowWindow(hwnd_, SW_HIDE);
            DestroyWindow(hwnd_);
        }
        hwnd_ = nullptr;
        StDiscardSpuriousInputAfterModal(owner_);
        if (owner_ && IsWindow(owner_)) SetForegroundWindow(owner_);
        StDiscardSpuriousInputAfterModal(owner_);

        if (accepted_) {
            mainInOut = main_;
            accentInOut = accent_;
        }
        return accepted_;
    }

private:
    static constexpr int kDlgW = theme_ui::kCustomDlgW;
    static constexpr int kDlgH = theme_ui::kCustomDlgH;
    static constexpr int kTitleH = theme_ui::kCustomTitleH;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        CustomThemeDialog* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<CustomThemeDialog*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
            return TRUE;
        }
        self = reinterpret_cast<CustomThemeDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE: OnCreate(); return 0;
        case WM_PAINT: OnPaint(); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_LBUTTONDOWN: OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_MOUSELEAVE: OnMouseLeave(); return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) { OnSetCursor(); return TRUE; }
            break;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { Close(false); return 0; }
            if (wp == VK_RETURN) { Close(true); return 0; }
            break;
        case WM_CLOSE: Close(false); return 0;
        case WM_DESTROY: Cleanup(); return 0;
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            if (pt.y >= 0 && pt.y < S(kTitleH) && !StPtIn(CloseRect(), pt.x, pt.y))
                return HTCAPTION;
            break;
        }
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    RECT CloseRect() const { return ScaleRect(theme_ui::MakeCustomThemeLayout().close); }
    RECT MainLabelRect() const { return ScaleRect(theme_ui::MakeCustomThemeLayout().mainLabel); }
    RECT AccentLabelRect() const {
        return ScaleRect(theme_ui::MakeCustomThemeLayout().accentLabel);
    }
    RECT MainSwatchRect() const { return ScaleRect(theme_ui::MakeCustomThemeLayout().mainSwatch); }
    RECT AccentSwatchRect() const {
        return ScaleRect(theme_ui::MakeCustomThemeLayout().accentSwatch);
    }
    RECT PickMainRect() const { return ScaleRect(theme_ui::MakeCustomThemeLayout().pickMain); }
    RECT PickAccentRect() const { return ScaleRect(theme_ui::MakeCustomThemeLayout().pickAccent); }
    RECT RandomRect() const { return ScaleRect(theme_ui::MakeCustomThemeLayout().randomBtn); }
    RECT FooterRect() const { return ScaleRect(theme_ui::MakeCustomThemeLayout().footer); }
    RECT OkRect() const { return ScaleRect(theme_ui::MakeCustomThemeLayout().ok); }
    RECT CancelRect() const { return ScaleRect(theme_ui::MakeCustomThemeLayout().cancel); }

    void OnCreate() {
        titleFont_ = MakeUiFont(theme_ui::kTitleFontDesign, FW_NORMAL);
        bodyFont_ = MakeUiFont(theme_ui::kBodyFontDesign, FW_NORMAL);
        closeFont_ = MakeUiFont(theme_ui::kCloseFontDesign, FW_NORMAL);
        outerShadow_.Attach(hwnd_);
    }

    void Cleanup() {
        if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
        if (bodyFont_) { DeleteObject(bodyFont_); bodyFont_ = nullptr; }
        if (closeFont_) { DeleteObject(closeFont_); closeFont_ = nullptr; }
        outerShadow_.Detach();
    }

    void Close(bool accept) {
        if (done_) return;
        accepted_ = accept;
        done_ = true;
        if (IsWindow(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
    }

    void EnsureTrack() {
        if (tracking_) return;
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        tracking_ = TrackMouseEvent(&tme) != FALSE;
    }

    void SetHover(bool& flag, bool value, const RECT& rc) {
        if (flag == value) return;
        flag = value;
        InvalidateRect(hwnd_, &rc, FALSE);
    }

    void UpdateHover(int x, int y) {
        SetHover(hoverClose_, StPtIn(CloseRect(), x, y), CloseRect());
        SetHover(hoverOk_, StPtIn(OkRect(), x, y), OkRect());
        SetHover(hoverCancel_, StPtIn(CancelRect(), x, y), CancelRect());
        SetHover(hoverPickMain_, StPtIn(PickMainRect(), x, y), PickMainRect());
        SetHover(hoverPickAccent_, StPtIn(PickAccentRect(), x, y), PickAccentRect());
        SetHover(hoverRandom_, StPtIn(RandomRect(), x, y), RandomRect());
        SetHover(hoverMainSwatch_, StPtIn(MainSwatchRect(), x, y), MainSwatchRect());
        SetHover(hoverAccentSwatch_, StPtIn(AccentSwatchRect(), x, y), AccentSwatchRect());
    }

    void OnMouseMove(int x, int y) {
        EnsureTrack();
        UpdateHover(x, y);
    }

    void OnMouseLeave() {
        tracking_ = false;
        hoverOk_ = hoverCancel_ = hoverClose_ = false;
        hoverPickMain_ = hoverPickAccent_ = hoverRandom_ = false;
        hoverMainSwatch_ = hoverAccentSwatch_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void OnSetCursor() {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        const bool hand = hoverOk_ || hoverCancel_ || hoverClose_ || hoverPickMain_
            || hoverPickAccent_ || hoverRandom_ || hoverMainSwatch_ || hoverAccentSwatch_;
        SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
    }

    void InvalidateSwatches() {
        RECT a = MainSwatchRect();
        RECT b = AccentSwatchRect();
        InvalidateRect(hwnd_, &a, FALSE);
        InvalidateRect(hwnd_, &b, FALSE);
    }

    void OnLButtonDown(int x, int y) {
        if (StPtIn(CloseRect(), x, y) || StPtIn(CancelRect(), x, y)) { Close(false); return; }
        if (StPtIn(OkRect(), x, y)) { Close(true); return; }
        if (StPtIn(PickMainRect(), x, y) || StPtIn(MainSwatchRect(), x, y)) {
            if (ShowThemedColorPicker(hwnd_, main_))
                InvalidateSwatches();
            return;
        }
        if (StPtIn(PickAccentRect(), x, y) || StPtIn(AccentSwatchRect(), x, y)) {
            if (ShowThemedColorPicker(hwnd_, accent_))
                InvalidateSwatches();
            return;
        }
        if (StPtIn(RandomRect(), x, y)) {
            RandomAttractiveThemeColors(main_, accent_);
            InvalidateSwatches();
        }
    }

    void OnPaint() {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        RenderBatchScope batch(hdc);
        RECT client{};
        GetClientRect(hwnd_, &client);
        FillRectColor(hdc, client, kWhite);

        StDrawTitleBar(hdc, titleFont_, closeFont_, S(kDlgW), S(kTitleH), L"自定义主题",
            hoverClose_, CloseRect());

        SelectObject(hdc, bodyFont_);
        DrawTextIn(hdc, L"主色（整窗背景）", MainLabelRect(), RGB(60, 60, 60),
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextIn(hdc, L"点缀色（标签/强调）", AccentLabelRect(), RGB(60, 60, 60),
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        DrawSwatch(hdc, MainSwatchRect(), main_, hoverMainSwatch_);
        DrawSwatch(hdc, AccentSwatchRect(), accent_, hoverAccentSwatch_);

        DrawOutlineBtn(hdc, bodyFont_, PickMainRect(), L"选择主色", hoverPickMain_);
        DrawOutlineBtn(hdc, bodyFont_, PickAccentRect(), L"选择点缀色", hoverPickAccent_);
        StDrawGreenButton(hdc, bodyFont_, RandomRect(), L"随机", hoverRandom_);

        FillRectColor(hdc, FooterRect(), RGB(247, 247, 247));
        StDrawGreenButton(hdc, bodyFont_, OkRect(), L"确定", hoverOk_);
        DrawOutlineBtn(hdc, bodyFont_, CancelRect(), L"取消", hoverCancel_);

        batch.End();
        EndPaint(hwnd_, &ps);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT closeFont_ = nullptr;
    WindowOuterShadow outerShadow_;
    COLORREF main_ = RGB(64, 168, 99);
    COLORREF accent_ = RGB(255, 154, 72);
    bool done_ = false;
    bool accepted_ = false;
    bool tracking_ = false;
    bool hoverOk_ = false;
    bool hoverCancel_ = false;
    bool hoverClose_ = false;
    bool hoverPickMain_ = false;
    bool hoverPickAccent_ = false;
    bool hoverRandom_ = false;
    bool hoverMainSwatch_ = false;
    bool hoverAccentSwatch_ = false;
};

}  // namespace

bool ShowCustomThemePicker(HWND owner, COLORREF& mainInOut, COLORREF& accentInOut) {
    CustomThemeDialog dlg;
    return dlg.Show(owner, mainInOut, accentInOut);
}

}  // namespace quickscript
