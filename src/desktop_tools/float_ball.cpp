// ──────────────────────────────────────────────────────────────────
// float_ball.cpp — 桌面悬浮球（分层窗，不抢前台）
// ──────────────────────────────────────────────────────────────────

#include "desktop_tools/float_ball.h"

#include "app_theme.h"
#include "themed_popup_menu.h"

#include <objidl.h>
#include <windowsx.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "msimg32.lib")

namespace qst::desktop_tools {
namespace {

constexpr UINT kCollapseTimer = 2;
constexpr UINT kAnimTimer = 3;
constexpr UINT kFsTimer = 4;
constexpr UINT kPulseTimer = 5;
constexpr UINT kCollapseDelayMs = 140;
constexpr int kDragSlopPx = 6;

enum Hit : int { HitNone = 0, HitBall = 1, HitButton = 2, HitPanel = 3 };

Gdiplus::Color Argb(BYTE a, COLORREF c) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

void AddRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rc, float r) {
    const float rr = (std::min)(r, (std::min)(rc.Width, rc.Height) * 0.5f);
    path.AddArc(rc.X, rc.Y, rr * 2, rr * 2, 180.f, 90.f);
    path.AddArc(rc.X + rc.Width - rr * 2, rc.Y, rr * 2, rr * 2, 270.f, 90.f);
    path.AddArc(rc.X + rc.Width - rr * 2, rc.Y + rc.Height - rr * 2, rr * 2, rr * 2, 0.f, 90.f);
    path.AddArc(rc.X, rc.Y + rc.Height - rr * 2, rr * 2, rr * 2, 90.f, 90.f);
    path.CloseFigure();
}

bool IsOwnProcessWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

bool IsForegroundFullscreen() {
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindow(fg) || IsOwnProcessWindow(fg)) return false;
    wchar_t cls[64]{};
    GetClassNameW(fg, cls, 64);
    if (_wcsicmp(cls, L"Progman") == 0 || _wcsicmp(cls, L"WorkerW") == 0
        || _wcsicmp(cls, L"Shell_TrayWnd") == 0) {
        return false;
    }
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    RECT wr{};
    if (!GetWindowRect(fg, &wr)) return false;
    const int mw = mi.rcMonitor.right - mi.rcMonitor.left;
    const int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
    const int ww = wr.right - wr.left;
    const int wh = wr.bottom - wr.top;
    return mw > 0 && mh > 0 && ww >= mw * 95 / 100 && wh >= mh * 95 / 100;
}

struct MonitorPick {
    RECT work{};
    std::wstring id;
    HMONITOR handle = nullptr;
};

BOOL CALLBACK FindMonitorByIdProc(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    auto* want = reinterpret_cast<MonitorPick*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    if (want->id.empty() || _wcsicmp(mi.szDevice, want->id.c_str()) != 0) return TRUE;
    want->work = mi.rcWork;
    want->handle = mon;
    want->id = mi.szDevice;
    return FALSE;
}

void ClampBallToWork(int& left, int& top, const RECT& work, int ballPx) {
    if (left < work.left) left = work.left;
    if (top < work.top) top = work.top;
    if (left + ballPx > work.right) left = work.right - ballPx;
    if (top + ballPx > work.bottom) top = work.bottom - ballPx;
    if (left < work.left) left = work.left;
    if (top < work.top) top = work.top;
}

void FillRoundBlob(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, const Gdiplus::Color& color) {
    if (rc.Width <= 0.5f || rc.Height <= 0.5f) return;
    Gdiplus::GraphicsPath path;
    AddRoundRect(path, rc, (std::min)(rc.Width, rc.Height) * 0.5f);
    Gdiplus::SolidBrush br(color);
    g.FillPath(&br, &path);
}

// 先铺描边色再填白色，避免合并 Path 自交把手指画穿。造型参考常见 IDC_HAND / 360 白手套。
void DrawGloveBlobs(Gdiplus::Graphics& g, const Gdiplus::RectF* blobs, int n,
    const Gdiplus::Color& outline, const Gdiplus::Color& fill, float inflate) {
    for (int i = 0; i < n; ++i) {
        const Gdiplus::RectF& rc = blobs[i];
        FillRoundBlob(g, Gdiplus::RectF(rc.X - inflate, rc.Y - inflate,
            rc.Width + inflate * 2.f, rc.Height + inflate * 2.f), outline);
    }
    for (int i = 0; i < n; ++i) {
        FillRoundBlob(g, blobs[i], fill);
    }
}

void DrawWhiteGlove(Gdiplus::Graphics& g, float ox, float oy, float unit, bool fist) {
    const Gdiplus::Color outline(255, 88, 96, 108);
    const Gdiplus::Color fill(255, 255, 255, 255);
    const Gdiplus::Color shade(90, 186, 196, 208);
    Gdiplus::SolidBrush shadeBr(shade);
    const float inf = (std::max)(1.4f, unit * 0.055f);

    if (!fist) {
        const Gdiplus::RectF blobs[] = {
            {ox + unit * 0.28f, oy + unit * 0.40f, unit * 0.54f, unit * 0.50f},
            {ox + unit * 0.08f, oy + unit * 0.40f, unit * 0.34f, unit * 0.20f},
            {ox + unit * 0.34f, oy + unit * 0.02f, unit * 0.18f, unit * 0.50f},
            {ox + unit * 0.50f, oy + unit * 0.12f, unit * 0.16f, unit * 0.40f},
            {ox + unit * 0.64f, oy + unit * 0.18f, unit * 0.14f, unit * 0.34f},
            {ox + unit * 0.76f, oy + unit * 0.24f, unit * 0.12f, unit * 0.26f},
        };
        DrawGloveBlobs(g, blobs, 6, outline, fill, inf);
        g.FillEllipse(&shadeBr, ox + unit * 0.42f, oy + unit * 0.56f, unit * 0.22f, unit * 0.16f);
    } else {
        const Gdiplus::RectF blobs[] = {
            {ox + unit * 0.22f, oy + unit * 0.30f, unit * 0.58f, unit * 0.50f},
            {ox + unit * 0.06f, oy + unit * 0.40f, unit * 0.32f, unit * 0.24f},
            {ox + unit * 0.30f, oy + unit * 0.20f, unit * 0.16f, unit * 0.22f},
            {ox + unit * 0.44f, oy + unit * 0.16f, unit * 0.16f, unit * 0.24f},
            {ox + unit * 0.58f, oy + unit * 0.20f, unit * 0.15f, unit * 0.22f},
        };
        DrawGloveBlobs(g, blobs, 5, outline, fill, inf);
        Gdiplus::Pen crease(Gdiplus::Color(160, 150, 158, 170), (std::max)(1.1f, unit * 0.06f));
        crease.SetStartCap(Gdiplus::LineCapRound);
        crease.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&crease, ox + unit * 0.36f, oy + unit * 0.42f, ox + unit * 0.70f, oy + unit * 0.40f);
        g.DrawLine(&crease, ox + unit * 0.36f, oy + unit * 0.54f, ox + unit * 0.68f, oy + unit * 0.52f);
        g.FillEllipse(&shadeBr, ox + unit * 0.40f, oy + unit * 0.50f, unit * 0.20f, unit * 0.14f);
    }
}

HCURSOR MakeGloveCursor(bool pressed) {
    const int w = GetSystemMetrics(SM_CXCURSOR);
    const int h = GetSystemMetrics(SM_CYCURSOR);
    if (w <= 0 || h <= 0) return nullptr;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) {
        if (color) DeleteObject(color);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }
    std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    const float unit = (std::min)(w, h) * 0.90f;
    const float ox = (w - unit) * 0.5f;
    const float oy = (h - unit) * 0.5f;
    {
        Gdiplus::Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        DrawWhiteGlove(g, ox, oy, unit, pressed);
    }

    const int maskStride = ((w + 15) / 16) * 2;
    std::vector<BYTE> maskBits(static_cast<size_t>(maskStride) * static_cast<size_t>(h), 0xFF);
    const auto* px = static_cast<const BYTE*>(bits);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (px[(y * w + x) * 4 + 3] <= 16) continue;
            maskBits[static_cast<size_t>(y) * static_cast<size_t>(maskStride) + static_cast<size_t>(x / 8)]
                &= static_cast<BYTE>(~(0x80 >> (x % 8)));
        }
    }
    HBITMAP mask = CreateBitmap(w, h, 1, 1, maskBits.data());
    ICONINFO ii{};
    ii.fIcon = FALSE;
    if (pressed) {
        ii.xHotspot = static_cast<DWORD>(w / 2);
        ii.yHotspot = static_cast<DWORD>(h * 45 / 100);
    } else {
        ii.xHotspot = static_cast<DWORD>((std::max)(2, static_cast<int>(ox + unit * 0.43f)));
        ii.yHotspot = static_cast<DWORD>((std::max)(2, static_cast<int>(oy + unit * 0.04f)));
    }
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HCURSOR cur = mask ? static_cast<HCURSOR>(CreateIconIndirect(&ii)) : nullptr;
    if (mask) DeleteObject(mask);
    DeleteObject(color);
    ReleaseDC(nullptr, screen);
    return cur;
}

void DrawActivityGlyph(Gdiplus::Graphics& g, float cx, float cy, float rad,
    FloatBallActivity activity, Gdiplus::Color accent) {
    Gdiplus::SolidBrush glyph(accent);
    const float s = rad * 0.34f;
    if (activity == FloatBallActivity::BreakoutPaused
        || activity == FloatBallActivity::MacroRunning) {
        Gdiplus::GraphicsPath pause;
        AddRoundRect(pause, Gdiplus::RectF(cx - s * 0.62f, cy - s, s * 0.36f, s * 2), 2.4f);
        AddRoundRect(pause, Gdiplus::RectF(cx + s * 0.22f, cy - s, s * 0.36f, s * 2), 2.4f);
        g.FillPath(&glyph, &pause);
    } else if (activity == FloatBallActivity::Recording) {
        g.FillEllipse(&glyph, cx - s * 0.52f, cy - s * 0.52f, s * 1.04f, s * 1.04f);
    } else if (activity == FloatBallActivity::Clicking) {
        g.FillEllipse(&glyph, cx - s * 1.08f, cy - s * 0.20f, s * 0.40f, s * 0.40f);
        g.FillEllipse(&glyph, cx - s * 0.20f, cy - s * 0.20f, s * 0.40f, s * 0.40f);
        g.FillEllipse(&glyph, cx + s * 0.68f, cy - s * 0.20f, s * 0.40f, s * 0.40f);
    } else {
        Gdiplus::GraphicsPath play;
        Gdiplus::PointF tri[3] = {
            {cx - s * 0.38f, cy - s * 0.92f},
            {cx - s * 0.38f, cy + s * 0.92f},
            {cx + s * 0.88f, cy},
        };
        play.AddPolygon(tri, 3);
        g.FillPath(&glyph, &play);
    }
}

void DrawUniformHalo(Gdiplus::Graphics& g, Gdiplus::GraphicsPath& path, float maxPx) {
    if (maxPx < 1.f) return;
    const int n = (std::max)(1, static_cast<int>(maxPx + 0.5f));
    for (int i = n; i >= 1; --i) {
        const BYTE a = static_cast<BYTE>((std::max)(8, 36 - i * 4));
        Gdiplus::Pen pen(Gdiplus::Color(a, 48, 64, 80), static_cast<Gdiplus::REAL>(i * 2));
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        g.DrawPath(&pen, &path);
    }
}
void DrawHeadRing(Gdiplus::Graphics& g, float cx, float cy, float rad,
    COLORREF ring, COLORREF glow, bool busy, float pulse, bool fillDisc, float remain01) {
    if (rad < 4.f) return;
    if (fillDisc) {
        Gdiplus::GraphicsPath orb;
        orb.AddEllipse(cx - rad, cy - rad, rad * 2, rad * 2);
        Gdiplus::PathGradientBrush pgb(&orb);
        pgb.SetCenterPoint(Gdiplus::PointF(cx - rad * 0.16f, cy - rad * 0.22f));
        pgb.SetCenterColor(Gdiplus::Color(255, 252, 253, 255));
        Gdiplus::Color surround(255, 226, 232, 238);
        INT n = 1;
        pgb.SetSurroundColors(&surround, &n);
        g.FillPath(&pgb, &orb);
    }

    const float track = (std::max)(2.4f, rad * 0.11f);
    Gdiplus::Pen trackPen(Gdiplus::Color(70, 160, 176, 188), track);
    g.DrawEllipse(&trackPen, cx - rad + track, cy - rad + track, (rad - track) * 2, (rad - track) * 2);

    float sweep = 320.f;
    if (remain01 >= 0.f) {
        sweep = 360.f * (std::max)(0.f, (std::min)(1.f, remain01));
    } else if (busy) {
        sweep = 220.f + 50.f * pulse;
    }
    const BYTE ringA = static_cast<BYTE>(busy ? (210 + 40 * pulse) : 235);
    Gdiplus::Pen ringPen(Argb(ringA, busy ? glow : ring), track);
    ringPen.SetStartCap(Gdiplus::LineCapRound);
    ringPen.SetEndCap(Gdiplus::LineCapRound);
    if (sweep >= 1.f) {
        g.DrawArc(&ringPen, cx - rad + track, cy - rad + track, (rad - track) * 2, (rad - track) * 2,
            -90.f, sweep);
    }

    Gdiplus::SolidBrush spec(Gdiplus::Color(70, 255, 255, 255));
    g.FillEllipse(&spec, cx - rad * 0.46f, cy - rad * 0.58f, rad * 0.55f, rad * 0.28f);
}

}  // namespace

FloatBall& FloatBall::Instance() {
    static FloatBall g;
    return g;
}

void FloatBall::EnsureGdiplus() {
    if (gdiplusOk_) return;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&gdiplusToken_, &in, nullptr) == Gdiplus::Ok)
        gdiplusOk_ = true;
}

void FloatBall::ShutdownGdiplus() {
    if (!gdiplusOk_) return;
    Gdiplus::GdiplusShutdown(gdiplusToken_);
    gdiplusToken_ = 0;
    gdiplusOk_ = false;
}

void FloatBall::EnsureMoveCursors() {
    const int px = GetSystemMetrics(SM_CXCURSOR);
    if (hoverCursor_ && pressCursor_ && cursorPx_ == px) return;
    DestroyMoveCursors();
    EnsureGdiplus();
    cursorPx_ = px;
    hoverCursor_ = MakeGloveCursor(false);
    pressCursor_ = MakeGloveCursor(true);
}

void FloatBall::DestroyMoveCursors() {
    if (hoverCursor_) {
        DestroyCursor(hoverCursor_);
        hoverCursor_ = nullptr;
    }
    if (pressCursor_) {
        DestroyCursor(pressCursor_);
        pressCursor_ = nullptr;
    }
    cursorPx_ = 0;
}

bool FloatBall::EnsureClass(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    if (GetClassInfoExW(inst, kFloatBallClass, &wc)) return true;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &FloatBall::WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kFloatBallClass;
    wc.hCursor = nullptr;
    wc.hbrBackground = nullptr;
    return RegisterClassExW(&wc) != 0;
}

bool FloatBall::EnsureWindow(HINSTANCE inst) {
    if (hwnd_ && IsWindow(hwnd_)) return true;
    if (!EnsureClass(inst)) return false;
    hwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kFloatBallClass, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, inst, this);
    return hwnd_ != nullptr;
}

void FloatBall::Create(HINSTANCE inst, HWND ownerHint) {
    inst_ = inst;
    ownerHint_ = ownerHint;
    EnsureGdiplus();
    EnsureMoveCursors();
    if (!EnsureWindow(inst)) return;
    SetTimer(hwnd_, kFsTimer, 400, nullptr);
    SetTimer(hwnd_, kPulseTimer, 50, nullptr);
    ApplyLayout(true);
    const bool show = userVisible_ && !fsHidden_;
    ShowWindow(hwnd_, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (show) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void FloatBall::Destroy() {
    if (hwnd_ && IsWindow(hwnd_)) {
        KillTimer(hwnd_, kCollapseTimer);
        KillTimer(hwnd_, kAnimTimer);
        KillTimer(hwnd_, kFsTimer);
        KillTimer(hwnd_, kPulseTimer);
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    DestroyMoveCursors();
    ShutdownGdiplus();
}

void FloatBall::SetCallbacks(FloatBallCallbacks cb) {
    cb_ = std::move(cb);
}

void FloatBall::SetVisible(bool show) {
    userVisible_ = show;
    if (!hwnd_ || !IsWindow(hwnd_)) return;
    if (!show) {
        ShowWindow(hwnd_, SW_HIDE);
        return;
    }
    TickFullscreen();
    if (!fsHidden_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ApplyLayout(true);
    }
}

void FloatBall::SetPlacement(bool docked, int edge, double xRatio, double yRatio,
    const std::wstring& monitorId) {
    if (dragging_) return;
    docked_ = docked;
    edge_ = ClampFloatBallEdge(edge);
    xRatio_ = ClampFloatBallXRatio(xRatio);
    yRatio_ = ClampFloatBallYRatio(yRatio);
    monitorId_ = monitorId;
    if (!docked_) {
        wantExpand_ = true;
        expandT_ = 1.f;
        hoverInside_ = true;
        KillHoverTimers();
    }
    ApplyLayout(true);
}

void FloatBall::SetModel(const FloatBallModel& model) {
    model_ = model;
    RelayoutNoScriptButton();
    if (hwnd_ && IsWindow(hwnd_)) Paint();
}

void FloatBall::RefreshTheme() {
    if (hwnd_ && IsWindow(hwnd_)) Paint();
}

UINT FloatBall::MonitorDpi(HMONITOR mon) const {
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static auto fn = reinterpret_cast<GetDpiForMonitorFn>(
        GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor"));
    UINT x = 96, y = 96;
    if (fn && mon && SUCCEEDED(fn(mon, 0, &x, &y)) && x > 0) return x;
    return 96;
}

FloatBallMetrics FloatBall::ScaledMetrics(HMONITOR mon) const {
    const UINT dpi = MonitorDpi(mon);
    auto S = [dpi](int v) { return MulDiv(v, static_cast<int>(dpi), 96); };
    FloatBallMetrics m;
    m.peekPx = (std::max)(20, S(28));
    m.ballPx = (std::max)(52, S(64));
    m.panelW = S(156);
    m.panelH = m.ballPx;
    m.neckPx = S(32);
    m.gapPx = 0;
    m.padPx = S(8);
    m.buttonH = S(28);
    m.snapPx = (std::max)(40, S(56));
    m.shadowPx = (std::max)(4, S(6));
    return m;
}

HMONITOR FloatBall::ResolveMonitor() const {
    if (!monitorId_.empty()) {
        MonitorPick pick;
        pick.id = monitorId_;
        EnumDisplayMonitors(nullptr, nullptr, FindMonitorByIdProc, reinterpret_cast<LPARAM>(&pick));
        if (pick.handle) return pick.handle;
    }
    return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

RECT FloatBall::WorkArea(HMONITOR mon) const {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) return mi.rcWork;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    return work;
}

std::wstring FloatBall::MonitorId(HMONITOR mon) const {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) return mi.szDevice;
    return {};
}

void FloatBall::ApplyLayout(bool paint) {
    if (!hwnd_ || !IsWindow(hwnd_)) return;
    const HMONITOR mon = ResolveMonitor();
    metrics_ = ScaledMetrics(mon);
    const RECT work = WorkArea(mon);
    if (dragging_) {
        frame_ = ComputeFreeFrame(work, xRatio_, yRatio_, metrics_, false);
    } else if (!docked_) {
        wantExpand_ = true;
        expandT_ = 1.f;
        frame_ = ComputeFreeFrame(work, xRatio_, yRatio_, metrics_, true);
    } else if (expandT_ <= 0.f && !wantExpand_) {
        frame_ = ComputeDockedFrame(work, edge_, xRatio_, yRatio_, metrics_);
    } else {
        const float t = (std::max)(LayoutT(), 0.0001f);
        frame_ = ComputeDockedAnimFrame(work, edge_, xRatio_, yRatio_, metrics_, t);
    }
    ApplyShadowPad();
    RelayoutNoScriptButton();
    const int w = (std::max)(1, static_cast<int>(frame_.window.right - frame_.window.left));
    const int h = (std::max)(1, static_cast<int>(frame_.window.bottom - frame_.window.top));
    RECT cur{};
    GetWindowRect(hwnd_, &cur);
    if (cur.left != frame_.window.left || cur.top != frame_.window.top
        || cur.right - cur.left != w || cur.bottom - cur.top != h) {
        SetWindowPos(hwnd_, HWND_TOPMOST, frame_.window.left, frame_.window.top, w, h,
            SWP_NOACTIVATE);
    }
    if (paint) Paint();
}

void FloatBall::ApplyShadowPad() {
    const int sh = metrics_.shadowPx;
    if (sh <= 0) return;
    int padL = sh, padT = sh, padR = sh, padB = sh;
    if (docked_ && !dragging_) {
        if (edge_ == FloatBallEdge::Right) padR = 0;
        else if (edge_ == FloatBallEdge::Left) padL = 0;
        else if (edge_ == FloatBallEdge::Top) padT = 0;
        else padB = 0;
    }
    auto off = [&](RECT& r) {
        if (r.right <= r.left && r.bottom <= r.top) return;
        r.left += padL;
        r.right += padL;
        r.top += padT;
        r.bottom += padT;
    };
    off(frame_.local.ball);
    off(frame_.local.panel);
    off(frame_.local.button);
    off(frame_.local.title);
    off(frame_.local.status);
    frame_.window.left -= padL;
    frame_.window.top -= padT;
    frame_.window.right += padR;
    frame_.window.bottom += padB;
}

void FloatBall::RelayoutNoScriptButton() {
    if (model_.canStart || model_.busy) return;
    RECT& btn = frame_.local.button;
    RECT& title = frame_.local.title;
    if (btn.right <= btn.left || btn.bottom <= btn.top) return;
    RECT slot = btn;
    if (title.bottom > title.top) {
        slot.top = (std::min)(btn.top, title.top);
        slot.bottom = (std::max)(btn.bottom, title.bottom);
    }
    const int slotH = static_cast<int>(slot.bottom - slot.top);
    const int bh = (std::min)(metrics_.buttonH, (std::max)(16, slotH));
    const int y = slot.top + ((slot.bottom - slot.top) - bh) / 2;
    btn = {slot.left, y, slot.right, y + bh};
    title = {};
}

float FloatBall::LayoutT() const {
    if (!docked_ || dragging_) return 1.f;
    return Smoothstep01(expandT_);
}

bool FloatBall::PanelVisible() const {
    return !dragging_ && (!docked_ || LayoutT() > kFloatBallDockBallPhase + 0.001f);
}

bool FloatBall::BallPressed() const {
    return dragging_ || ballPressed_;
}

void FloatBall::Paint() {
    if (!hwnd_ || !IsWindow(hwnd_) || !gdiplusOk_) return;
    RECT wr{};
    GetWindowRect(hwnd_, &wr);
    const int w = wr.right - wr.left;
    const int h = wr.bottom - wr.top;
    if (w <= 0 || h <= 0) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        ReleaseDC(nullptr, screen);
        return;
    }
    std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);

    {
        Gdiplus::Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);

        const quickscript::AppTheme& theme = quickscript::CurrentTheme();
        COLORREF ring = theme.mainColor;
        COLORREF glow = theme.accentColor;
        if (model_.activity == FloatBallActivity::Recording) {
            glow = RGB(245, 158, 11);
            ring = glow;
        } else if (model_.activity == FloatBallActivity::BreakoutPaused) {
            glow = RGB(251, 191, 36);
            ring = glow;
        } else if (model_.activity == FloatBallActivity::Clicking
            || model_.activity == FloatBallActivity::MacroRunning) {
            glow = theme.accentColor;
        }

        const float pulse = (model_.activity == FloatBallActivity::Idle)
            ? 0.f
            : (0.5f + 0.5f * std::sin(pulse_));
        float remain01 = -1.f;
        if (model_.activity == FloatBallActivity::MacroRunning && model_.actionTotal > 0) {
            remain01 = 1.f - static_cast<float>(model_.actionIndex)
                / static_cast<float>(model_.actionTotal);
            if (remain01 < 0.f) remain01 = 0.f;
            if (remain01 > 1.f) remain01 = 1.f;
        } else if (model_.activity == FloatBallActivity::Idle) {
            remain01 = cpu01_;
        }

        const RECT& brc = frame_.local.ball;
        const float cx = (brc.left + brc.right) * 0.5f;
        const float cy = (brc.top + brc.bottom) * 0.5f;
        const float rad = (brc.right - brc.left) * 0.5f - 1.f;
        const bool headLook = dragging_ || !docked_ || LayoutT() >= kFloatBallDockBallPhase;
        const bool panelOn = PanelVisible();
        const bool showChrome = panelOn && LayoutT() >= 0.62f;

        Gdiplus::FontFamily fam(L"Microsoft YaHei UI");
        const Gdiplus::FontFamily* useFam = &fam;
        Gdiplus::FontFamily fallback(L"Microsoft YaHei");
        if (fam.GetLastStatus() != Gdiplus::Ok) useFam = &fallback;
        Gdiplus::StringFormat center;
        center.SetAlignment(Gdiplus::StringAlignmentCenter);
        center.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        center.SetTrimming(Gdiplus::StringTrimmingNone);
        center.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        const int sh = metrics_.shadowPx;
        Gdiplus::RectF content(0.f, 0.f, static_cast<Gdiplus::REAL>(w), static_cast<Gdiplus::REAL>(h));
        if (sh > 0) {
            float padL = static_cast<float>(sh), padT = static_cast<float>(sh);
            float padR = static_cast<float>(sh), padB = static_cast<float>(sh);
            if (docked_ && !dragging_) {
                if (edge_ == FloatBallEdge::Right) padR = 0.f;
                else if (edge_ == FloatBallEdge::Left) padL = 0.f;
                else if (edge_ == FloatBallEdge::Top) padT = 0.f;
                else padB = 0.f;
            }
            content = Gdiplus::RectF(padL, padT,
                (std::max)(1.f, static_cast<float>(w) - padL - padR),
                (std::max)(1.f, static_cast<float>(h) - padT - padB));
        }

        RECT contentRc{
            static_cast<LONG>(content.X),
            static_cast<LONG>(content.Y),
            static_cast<LONG>(content.X + content.Width),
            static_cast<LONG>(content.Y + content.Height),
        };
        RECT visRc = contentRc;
        if (docked_ && !dragging_ && expandT_ > 0.f) {
            visRc = DockedRevealRect(edge_, contentRc, brc, DockedBodyProgress(LayoutT()));
        }
        Gdiplus::RectF vis(
            static_cast<Gdiplus::REAL>(visRc.left),
            static_cast<Gdiplus::REAL>(visRc.top),
            static_cast<Gdiplus::REAL>((std::max)(1, static_cast<int>(visRc.right - visRc.left))),
            static_cast<Gdiplus::REAL>((std::max)(1, static_cast<int>(visRc.bottom - visRc.top))));

        Gdiplus::GraphicsPath haloPath;
        if (panelOn) {
            AddRoundRect(haloPath, vis, (std::min)(vis.Width, vis.Height) * 0.5f);
        } else {
            haloPath.AddEllipse(cx - rad, cy - rad, rad * 2, rad * 2);
        }
        DrawUniformHalo(g, haloPath, static_cast<float>((std::max)(3, sh)));

        Gdiplus::GraphicsPath stadium;
        const Gdiplus::RectF shellRc = panelOn ? vis : content;
        AddRoundRect(stadium, shellRc, (std::min)(shellRc.Width, shellRc.Height) * 0.5f);
        if (!panelOn && !(docked_ && !dragging_ && expandT_ > 0.f)) {
            Gdiplus::SolidBrush hitPlate(Gdiplus::Color(8, 248, 250, 252));
            g.FillPath(&hitPlate, &stadium);
        }

        if (panelOn) {
            Gdiplus::LinearGradientBrush shellBr(vis,
                Gdiplus::Color(255, 252, 253, 255),
                Gdiplus::Color(255, 228, 234, 241),
                90.f);
            g.FillPath(&shellBr, &stadium);
            Gdiplus::Pen rim(Gdiplus::Color(40, 120, 136, 150), 1.0f);
            g.DrawPath(&rim, &stadium);

            DrawHeadRing(g, cx, cy, rad, ring, glow, model_.activity != FloatBallActivity::Idle,
                pulse, false, remain01);

            g.SetClip(&stadium, Gdiplus::CombineModeIntersect);

            const RECT& btn = frame_.local.button;
            const bool en = ButtonEnabled();
            const bool hoverBtn = hoverHit_ == HitButton;
            COLORREF btnFill = model_.busy ? glow : theme.mainColor;
            if (!en) btnFill = RGB(148, 163, 176);
            const int btnW = btn.right - btn.left;
            const int btnH = btn.bottom - btn.top;
            if (showChrome && btnW > 8 && btnH >= 16) {
                Gdiplus::GraphicsPath btnPath;
                const Gdiplus::RectF btnRf(
                    static_cast<Gdiplus::REAL>(btn.left),
                    static_cast<Gdiplus::REAL>(btn.top),
                    static_cast<Gdiplus::REAL>(btnW),
                    static_cast<Gdiplus::REAL>(btnH));
                AddRoundRect(btnPath, btnRf, (std::min)(btnRf.Width, btnRf.Height) * 0.5f);
                Gdiplus::Color btnA = Argb(en ? 255 : 150, btnFill);
                Gdiplus::Color btnB = en
                    ? Gdiplus::Color(255,
                        static_cast<BYTE>((std::min)(255, GetRValue(btnFill) + (hoverBtn ? 18 : 8))),
                        static_cast<BYTE>((std::min)(255, GetGValue(btnFill) + (hoverBtn ? 18 : 8))),
                        static_cast<BYTE>((std::min)(255, GetBValue(btnFill) + (hoverBtn ? 12 : 4))))
                    : btnA;
                Gdiplus::LinearGradientBrush btnBr(btnRf, btnA, btnB, 90.f);
                g.FillPath(&btnBr, &btnPath);
                Gdiplus::Font btnFont(useFam, (std::max)(15.f, static_cast<float>(metrics_.buttonH) * 0.58f),
                    Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                Gdiplus::SolidBrush btnText(Gdiplus::Color(255, 255, 255, 255));
                g.DrawString(ButtonLabel(), -1, &btnFont, btnRf, &center, &btnText);
            }

            const RECT& titleRc = frame_.local.title;
            if (showChrome && (model_.canStart || model_.busy)
                && titleRc.right > titleRc.left && titleRc.bottom > titleRc.top) {
                Gdiplus::Font titleFont(useFam, (std::max)(12.5f, static_cast<float>(metrics_.buttonH) * 0.48f),
                    Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                Gdiplus::SolidBrush muted(Gdiplus::Color(255, 38, 52, 70));
                const std::wstring sub = model_.title;
                g.DrawString(sub.c_str(), -1, &titleFont,
                    Gdiplus::RectF(
                        static_cast<Gdiplus::REAL>(titleRc.left),
                        static_cast<Gdiplus::REAL>(titleRc.top),
                        static_cast<Gdiplus::REAL>(titleRc.right - titleRc.left),
                        static_cast<Gdiplus::REAL>(titleRc.bottom - titleRc.top)),
                    &center, &muted);
            }
            g.ResetClip();
        } else {
            DrawHeadRing(g, cx, cy, rad, ring, glow, model_.activity != FloatBallActivity::Idle,
                pulse, true, remain01);
        }

        if (headLook && !model_.statusText.empty()) {
            Gdiplus::Font headFont(useFam, (std::max)(14.f, rad * 0.42f),
                Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush ink(Gdiplus::Color(255, 28, 52, 72));
            g.DrawString(model_.statusText.c_str(), -1, &headFont,
                Gdiplus::RectF(cx - rad * 0.78f, cy - rad * 0.36f, rad * 1.56f, rad * 0.72f),
                &center, &ink);
        } else if (!headLook) {
            DrawActivityGlyph(g, cx, cy, rad, model_.activity, Argb(255, ring));
        }
    }

    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, dib);
    POINT ptDst{wr.left, wr.top};
    SIZE size{w, h};
    POINT ptSrc{0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(hwnd_, screen, &ptDst, &size, mem, &ptSrc, 0, &blend, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(dib);
    ReleaseDC(nullptr, screen);
}

void FloatBall::TrackHover(bool over) {
    if (dragging_ || pendingDrag_ || !docked_) return;
    if (over) {
        hoverInside_ = true;
        SetWantExpand(true);
        return;
    }
    if (!hoverInside_) return;
    hoverInside_ = false;
    StartCollapseTimer();
}

bool FloatBall::CursorOnDockedStrip() const {
    if (!docked_ || dragging_) return false;
    POINT p{};
    GetCursorPos(&p);
    const RECT work = WorkArea(ResolveMonitor());
    const int ball = metrics_.ballPx;
    const int band = metrics_.peekPx + metrics_.shadowPx + 24;
    const int top = BallTopFromRatio(work, yRatio_, ball);
    const int left = BallLeftFromRatio(work, xRatio_, ball);
    if (edge_ == FloatBallEdge::Left) {
        return p.x <= work.left + band && p.y >= top - 16 && p.y < top + ball + 16;
    }
    if (edge_ == FloatBallEdge::Right) {
        return p.x >= work.right - band && p.y >= top - 16 && p.y < top + ball + 16;
    }
    if (edge_ == FloatBallEdge::Top) {
        return p.y <= work.top + band && p.x >= left - 16 && p.x < left + ball + 16;
    }
    return p.y >= work.bottom - band && p.x >= left - 16 && p.x < left + ball + 16;
}

bool FloatBall::PointerOverInteractive() const {
    if (CursorOnDockedStrip()) return true;
    POINT p{};
    GetCursorPos(&p);
    if (hwnd_ && IsWindow(hwnd_)) {
        RECT wr{};
        GetWindowRect(hwnd_, &wr);
        if (p.x >= wr.left && p.x < wr.right && p.y >= wr.top && p.y < wr.bottom) {
            POINT c = p;
            ScreenToClient(hwnd_, &c);
            if (HitTest(c.x, c.y) != HitNone) return true;
            const int ww = wr.right - wr.left;
            const int hh = wr.bottom - wr.top;
            if (PointHitsClientStadium(c.x, c.y, ww, hh)) return true;
        }
    }
    if (!docked_ || dragging_) return false;
    const HMONITOR mon = ResolveMonitor();
    const RECT work = WorkArea(mon);
    const auto exp = ComputeExpandedFrame(work, edge_, xRatio_, yRatio_, metrics_);
    const int lx = p.x - exp.window.left;
    const int ly = p.y - exp.window.top;
    if (PointHitsThermometer(lx, ly, exp.local, true)) return true;
    const auto peek = ComputeDockedFrame(work, edge_, xRatio_, yRatio_, metrics_);
    RECT band = peek.window;
    const int inflate = (std::max)(16, metrics_.peekPx);
    if (edge_ == FloatBallEdge::Right) band.left -= inflate;
    else if (edge_ == FloatBallEdge::Left) band.right += inflate;
    else if (edge_ == FloatBallEdge::Top) band.bottom += inflate;
    else band.top -= inflate;
    return p.x >= band.left && p.x < band.right && p.y >= band.top && p.y < band.bottom;
}

void FloatBall::RefreshHoverFromCursor() {
    if (!hwnd_ || dragging_ || pendingDrag_) return;
    if (!docked_) {
        hoverInside_ = true;
        return;
    }
    const bool over = PointerOverInteractive();
    if (over) {
        hoverInside_ = true;
        KillTimer(hwnd_, kCollapseTimer);
        if (!wantExpand_) SetWantExpand(true);
        return;
    }
    if (wantExpand_ && expandT_ > 0.f && expandT_ < 1.f) return;
    TrackHover(false);
}

void FloatBall::StartCollapseTimer() {
    if (!hwnd_ || !docked_) return;
    SetTimer(hwnd_, kCollapseTimer, kCollapseDelayMs, nullptr);
}

void FloatBall::KillHoverTimers() {
    if (!hwnd_) return;
    KillTimer(hwnd_, kCollapseTimer);
}

void FloatBall::SetWantExpand(bool expand) {
    wantExpand_ = expand;
    if (!hwnd_) return;
    if (expand && docked_ && expandT_ <= 0.f) ApplyLayout(true);
    SetTimer(hwnd_, kAnimTimer, 16, nullptr);
}

void FloatBall::TickAnim() {
    if (!docked_ && !dragging_) {
        expandT_ = 1.f;
        wantExpand_ = true;
        ApplyLayout(true);
        if (hwnd_) KillTimer(hwnd_, kAnimTimer);
        return;
    }
    const float target = wantExpand_ ? 1.f : 0.f;
    const float step = 1.f / 20.f;
    if (expandT_ < target) expandT_ = (std::min)(target, expandT_ + step);
    else if (expandT_ > target) expandT_ = (std::max)(target, expandT_ - step);
    ApplyLayout(true);
    if (!(wantExpand_ && expandT_ < 1.f)) {
        RefreshHoverFromCursor();
    }
    if (std::fabs(expandT_ - target) < 0.001f && hwnd_)
        KillTimer(hwnd_, kAnimTimer);
}

void FloatBall::TickFullscreen() {
    const bool fs = IsForegroundFullscreen();
    if (fs == fsHidden_) return;
    fsHidden_ = fs;
    if (!hwnd_ || !IsWindow(hwnd_) || !userVisible_) return;
    ShowWindow(hwnd_, fsHidden_ ? SW_HIDE : SW_SHOWNOACTIVATE);
    if (!fsHidden_) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ApplyLayout(true);
    }
}

void FloatBall::TickCpu() {
    const DWORD now = GetTickCount();
    if (cpuLastTick_ != 0 && now - cpuLastTick_ < 250) return;
    cpuLastTick_ = now;
    FILETIME idleFt{}, kernelFt{}, userFt{};
    if (!GetSystemTimes(&idleFt, &kernelFt, &userFt)) return;
    auto toU64 = [](const FILETIME& ft) -> unsigned long long {
        return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    const unsigned long long idle = toU64(idleFt);
    const unsigned long long kernel = toU64(kernelFt);
    const unsigned long long user = toU64(userFt);
    if (cpuPrimed_) {
        const unsigned long long idleD = idle - cpuIdle_;
        const unsigned long long totalD = (kernel - cpuKernel_) + (user - cpuUser_);
        float sample = cpu01_;
        if (totalD > 0) {
            sample = 1.f - static_cast<float>(idleD) / static_cast<float>(totalD);
            if (sample < 0.f) sample = 0.f;
            if (sample > 1.f) sample = 1.f;
        }
        cpu01_ = cpu01_ * 0.65f + sample * 0.35f;
    }
    cpuIdle_ = idle;
    cpuKernel_ = kernel;
    cpuUser_ = user;
    cpuPrimed_ = true;
}

void FloatBall::BeginDrag() {
    pendingDrag_ = true;
    dragging_ = false;
    dragMoved_ = false;
    POINT screen{};
    GetCursorPos(&screen);
    dragOriginScreen_ = screen;
    RECT wr{};
    GetWindowRect(hwnd_, &wr);
    dragGrab_.x = screen.x - (wr.left + frame_.local.ball.left);
    dragGrab_.y = screen.y - (wr.top + frame_.local.ball.top);
    SetCapture(hwnd_);
    Paint();
}

void FloatBall::UpdateDrag(POINT screen) {
    if (!pendingDrag_ && !dragging_) return;
    const int dx = screen.x - dragOriginScreen_.x;
    const int dy = screen.y - dragOriginScreen_.y;
    if (!dragMoved_ && dx * dx + dy * dy < kDragSlopPx * kDragSlopPx) {
        Paint();
        return;
    }
    if (!dragMoved_) {
        dragMoved_ = true;
        dragging_ = true;
        wantExpand_ = false;
        expandT_ = 0.f;
        KillHoverTimers();
    }
    HMONITOR mon = MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST);
    metrics_ = ScaledMetrics(mon);
    const RECT work = WorkArea(mon);
    int left = screen.x - dragGrab_.x;
    int top = screen.y - dragGrab_.y;
    ClampBallToWork(left, top, work, metrics_.ballPx);
    const FloatBallSnap pose = PoseFromBallTopLeft(left, top, work, metrics_.ballPx);
    docked_ = false;
    xRatio_ = pose.xRatio;
    yRatio_ = pose.yRatio;
    monitorId_ = MonitorId(mon);
    expandT_ = 0.f;
    wantExpand_ = false;
    ApplyLayout(true);
}

void FloatBall::EndDrag() {
    if (!pendingDrag_ && !dragging_) return;
    ReleaseCapture();
    pendingDrag_ = false;
    dragging_ = false;
    if (dragMoved_) {
        HMONITOR mon = ResolveMonitor();
        metrics_ = ScaledMetrics(mon);
        const RECT work = WorkArea(mon);
        POINT cursor{};
        GetCursorPos(&cursor);
        const int left = BallLeftFromRatio(work, xRatio_, metrics_.ballPx);
        const int top = BallTopFromRatio(work, yRatio_, metrics_.ballPx);
        FloatBallSnap snap = ResolveReleaseSnap(left, top, work, metrics_.ballPx, metrics_.snapPx,
            cursor.x, cursor.y);
        if (!snap.docked) {
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) {
                snap = ResolveReleaseSnap(left, top, mi.rcMonitor, metrics_.ballPx, metrics_.snapPx,
                    cursor.x, cursor.y);
            }
        }
        docked_ = snap.docked;
        edge_ = snap.edge;
        xRatio_ = snap.xRatio;
        yRatio_ = snap.yRatio;
        PersistPlacement();
    }
    POINT pt{};
    GetCursorPos(&pt);
    if (!docked_) {
        wantExpand_ = true;
        expandT_ = 1.f;
        hoverInside_ = true;
        KillHoverTimers();
    } else {
        RefreshHoverFromCursor();
        if (!hoverInside_) StartCollapseTimer();
    }
    ApplyLayout(true);
}

void FloatBall::PersistPlacement() {
    if (cb_.onPlacementChanged) {
        cb_.onPlacementChanged(docked_, static_cast<int>(edge_), xRatio_, yRatio_, monitorId_);
    }
}

int FloatBall::HitTest(int x, int y) const {
    const int ww = frame_.window.right - frame_.window.left;
    const int hh = frame_.window.bottom - frame_.window.top;
    const bool inStadium = PointHitsClientStadium(x, y, ww, hh);
    if (dragging_) {
        if (PointInCircle(x, y, frame_.local.ball) || inStadium) return HitBall;
        return HitNone;
    }
    const bool panelOn = PanelVisible();
    if (panelOn && PointInRect(x, y, frame_.local.button)) return HitButton;
    if (PointInCircle(x, y, frame_.local.ball)) return HitBall;
    if (panelOn && PointInRect(x, y, frame_.local.panel)) return HitPanel;
    if (inStadium) return panelOn ? HitPanel : HitBall;
    if (docked_ && x >= 0 && y >= 0 && x < ww && y < hh) return HitBall;
    return HitNone;
}

bool FloatBall::ButtonEnabled() const {
    return true;
}

const wchar_t* FloatBall::ButtonLabel() const {
    if (model_.busy) return L"停止";
    if (model_.canStart) return L"启动";
    return L"选择脚本";
}

void FloatBall::OnPrimaryClick(int hit) {
    if (hit != HitButton) return;
    if (model_.busy) {
        if (cb_.onStopRunning) cb_.onStopRunning();
    } else if (model_.canStart) {
        if (cb_.onStartSelectedMacro) cb_.onStartSelectedMacro();
    } else if (cb_.onShowMainWindow) {
        cb_.onShowMainWindow();
    }
}

void FloatBall::ShowContextMenu(POINT screen) {
    if (!hwnd_) return;
    ReleaseCapture();
    dragging_ = false;
    pendingDrag_ = false;
    ballPressed_ = false;
    const int id = ThemedPopupMenu::Show(hwnd_, screen, {
        {1, L"显示主窗口"},
        {2, L"隐藏悬浮球"},
    });
    if (id == 1) {
        if (cb_.onShowMainWindow) cb_.onShowMainWindow();
    } else if (id == 2) {
        if (cb_.onHideFromMenu) cb_.onHideFromMenu();
    }
}

LRESULT CALLBACK FloatBall::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    FloatBall* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<FloatBall*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<FloatBall*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->Handle(msg, wp, lp);
}

LRESULT FloatBall::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_DISPLAYCHANGE:
        ApplyLayout(true);
        return 0;
    case WM_TIMER:
        if (wp == kCollapseTimer) {
            KillTimer(hwnd_, kCollapseTimer);
            if (!docked_) return 0;
            if (PointerOverInteractive()) {
                hoverInside_ = true;
                if (!wantExpand_) SetWantExpand(true);
                return 0;
            }
            hoverInside_ = false;
            if (!dragging_ && !pendingDrag_) SetWantExpand(false);
            return 0;
        }
        if (wp == kAnimTimer) {
            TickAnim();
            return 0;
        }
        if (wp == kFsTimer) {
            TickFullscreen();
            return 0;
        }
        if (wp == kPulseTimer) {
            const DWORD now = GetTickCount();
            const bool cpuDue = (cpuLastTick_ == 0) || (now - cpuLastTick_ >= 250);
            TickCpu();
            bool needPaint = false;
            if (model_.activity != FloatBallActivity::Idle) {
                pulse_ += 0.12f;
                needPaint = true;
            } else if (cpuDue) {
                needPaint = true;
            }
            if (docked_ && !dragging_ && !pendingDrag_
                && !(expandT_ > 0.f && expandT_ < 1.f)) {
                if (PointerOverInteractive()) {
                    hoverInside_ = true;
                    KillTimer(hwnd_, kCollapseTimer);
                    if (!wantExpand_) SetWantExpand(true);
                }
            }
            if (needPaint) Paint();
            return 0;
        }
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd_;
        TrackMouseEvent(&tme);
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (dragging_ || pendingDrag_) {
            POINT screen{};
            GetCursorPos(&screen);
            UpdateDrag(screen);
        } else if (docked_) {
            RefreshHoverFromCursor();
            const int hit = HitTest(pt.x, pt.y);
            if (hit != hoverHit_) {
                hoverHit_ = hit;
                Paint();
            }
        } else {
            const int hit = HitTest(pt.x, pt.y);
            if (hit != hoverHit_) {
                hoverHit_ = hit;
                Paint();
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (pendingDrag_ || dragging_) return 0;
        if (PointerOverInteractive()) {
            hoverInside_ = true;
            return 0;
        }
        TrackHover(false);
        btnDown_ = false;
        ballPressed_ = false;
        hoverHit_ = HitNone;
        Paint();
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        pressHit_ = HitTest(pt.x, pt.y);
        hoverHit_ = pressHit_;
        btnDown_ = (pressHit_ == HitButton);
        ballPressed_ = (pressHit_ == HitBall || pressHit_ == HitPanel || pressHit_ == HitButton);
        if (pressHit_ != HitNone)
            BeginDrag();
        else
            Paint();
        EnsureMoveCursors();
        if (pressHit_ == HitButton && !dragMoved_) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        } else if (pressCursor_) {
            SetCursor(pressCursor_);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const bool wasDrag = dragging_;
        const bool moved = dragMoved_;
        EndDrag();
        ballPressed_ = false;
        if (!wasDrag || !moved) {
            const int hit = HitTest(pt.x, pt.y);
            if (btnDown_ && hit == HitButton) OnPrimaryClick(HitButton);
        }
        btnDown_ = false;
        pressHit_ = HitNone;
        hoverHit_ = HitTest(pt.x, pt.y);
        Paint();
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int hit = HitTest(pt.x, pt.y);
        if (hit != HitButton && hit != HitNone && cb_.onShowMainWindow) {
            cb_.onShowMainWindow();
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT screen{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd_, &screen);
        ShowContextMenu(screen);
        return 0;
    }
    case WM_SETCURSOR: {
        POINT pt{};
        GetCursorPos(&pt);
        POINT c = pt;
        ScreenToClient(hwnd_, &c);
        const int hit = (dragging_ || pendingDrag_) ? HitBall : HitTest(c.x, c.y);
        if (hit == HitButton) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        } else {
            EnsureMoveCursors();
            const bool press = dragging_ || pendingDrag_ || ballPressed_;
            HCURSOR cur = press ? pressCursor_ : hoverCursor_;
            SetCursor(cur ? cur : LoadCursorW(nullptr, IDC_ARROW));
        }
        return TRUE;
    }
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        if (HitTest(pt.x, pt.y) == HitNone) return HTTRANSPARENT;
        return HTCLIENT;
    }
    case WM_DESTROY:
        hwnd_ = nullptr;
        return 0;
    default:
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

}  // namespace qst::desktop_tools
