#pragma once
// 悬浮球几何（无 HWND）：贴边半露 / 自由悬浮 / 展开面板。供产品绘制与自检共用。

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <climits>

namespace qst::desktop_tools {

inline constexpr wchar_t kFloatBallClass[] = L"QstDesktopFloatBall";
inline constexpr int kFloatBallSnapPx = 56;
inline constexpr float kFloatBallDockBallPhase = 0.42f;

enum class FloatBallEdge { Left = 0, Right = 1, Top = 2, Bottom = 3 };

inline int ClampFloatBallEdgeInt(int v) {
    if (v < 0 || v > 3) return 1;
    return v;
}

inline FloatBallEdge ClampFloatBallEdge(int v) {
    return static_cast<FloatBallEdge>(ClampFloatBallEdgeInt(v));
}

inline double Clamp01(double y) {
    if (!(y >= 0.0)) return 0.0;
    if (y > 1.0) return 1.0;
    return y;
}

inline double ClampFloatBallYRatio(double y) { return Clamp01(y); }
inline double ClampFloatBallXRatio(double x) { return Clamp01(x); }

struct FloatBallMetrics {
    int peekPx = 22;
    int ballPx = 64;
    int panelW = 156;  // 身子长度（与圆同高时外轮廓是一体胶囊）
    int panelH = 64;   // 与圆头同高，才能融成一个 stadium
    int neckPx = 32;
    int gapPx = 0;
    int padPx = 8;
    int buttonH = 28;
    int snapPx = kFloatBallSnapPx;
    int shadowPx = 6;
};

// 茎相对圆头：常态左右。右半屏茎在右，左半屏茎在左；贴边不够位则翻到朝里。
enum class FloatBallStem { Left, Right, Up, Down };

inline int StemLength(const FloatBallMetrics& m) { return (std::max)(24, m.panelW); }

inline int StemThickness(const FloatBallMetrics& m) {
    return (std::max)(16, (std::min)(m.panelH, m.ballPx));
}

struct FloatBallLocal {
    RECT ball{};
    RECT panel{};
    RECT button{};
    RECT title{};
    RECT status{};
};

struct FloatBallFrame {
    RECT window{};
    FloatBallLocal local;
};

struct FloatBallSnap {
    bool docked = false;
    FloatBallEdge edge = FloatBallEdge::Right;
    double xRatio = 1.0;
    double yRatio = 0.55;
};

inline RECT LerpRect(const RECT& a, const RECT& b, float t) {
    if (t <= 0.f) return a;
    if (t >= 1.f) return b;
    auto L = [t](LONG x, LONG y) -> LONG {
        return x + static_cast<LONG>((static_cast<float>(y - x) * t) + (t >= 0.f ? 0.5f : -0.5f));
    };
    return RECT{L(a.left, b.left), L(a.top, b.top), L(a.right, b.right), L(a.bottom, b.bottom)};
}

inline int AxisFromRatio(int origin, int span, double ratio, int size) {
    const int travel = (std::max)(0, span - size);
    return origin + static_cast<int>(Clamp01(ratio) * static_cast<double>(travel) + 0.5);
}

inline double RatioFromAxis(int origin, int span, int pos, int size) {
    const int travel = (std::max)(1, span - size);
    return Clamp01(static_cast<double>(pos - origin) / static_cast<double>(travel));
}

inline int BallTopFromRatio(const RECT& work, double yRatio, int ballPx) {
    return AxisFromRatio(work.top, work.bottom - work.top, yRatio, ballPx);
}

inline int BallLeftFromRatio(const RECT& work, double xRatio, int ballPx) {
    return AxisFromRatio(work.left, work.right - work.left, xRatio, ballPx);
}

inline int ThermometerSpan(int ballPx, int panel, int neck) {
    const int n = (std::max)(0, (std::min)(neck, panel - 8));
    return ballPx + panel - n;
}

inline int NeckClamped(const FloatBallMetrics& m) {
    return (std::max)(0, (std::min)(m.neckPx, StemLength(m) - 8));
}

// 右半屏往右展开、左半屏往左展开；贴边或越界则翻到朝里。
inline FloatBallStem StemForHorizontal(const RECT& work, int ballLeft, const FloatBallMetrics& m) {
    const int cx = ballLeft + m.ballPx / 2;
    const int mid = work.left + (work.right - work.left) / 2;
    const int stick = (std::max)(0, StemLength(m) - NeckClamped(m));
    FloatBallStem stem = (cx >= mid) ? FloatBallStem::Right : FloatBallStem::Left;
    if (stem == FloatBallStem::Right && ballLeft + m.ballPx + stick > work.right)
        stem = FloatBallStem::Left;
    if (stem == FloatBallStem::Left && ballLeft - stick < work.left)
        stem = FloatBallStem::Right;
    return stem;
}

inline void FillPanelTextRects(FloatBallLocal& loc, const FloatBallMetrics& m);
inline void PinWindowToDockedEdge(RECT& win, const RECT& work, FloatBallEdge edge);

inline FloatBallFrame PlaceThermometer(int ballLeft, int ballTop, FloatBallStem stem,
    const FloatBallMetrics& m) {
    FloatBallFrame f{};
    const int neck = NeckClamped(m);
    const int len = StemLength(m);
    const int thick = StemThickness(m);
    const int ball = m.ballPx;
    const int stick = (std::max)(0, len - neck);

    if (stem == FloatBallStem::Left || stem == FloatBallStem::Right) {
        const int winW = ThermometerSpan(ball, len, neck);
        const int winH = ball;
        const int panelY = (winH - thick) / 2;
        int winLeft = 0, ballX = 0, panelX = 0;
        if (stem == FloatBallStem::Left) {
            winLeft = ballLeft - stick;
            ballX = stick;
            panelX = 0;
        } else {
            winLeft = ballLeft;
            ballX = 0;
            panelX = ball - neck;
        }
        f.window = {winLeft, ballTop, winLeft + winW, ballTop + winH};
        f.local.ball = {ballX, 0, ballX + ball, ball};
        f.local.panel = {panelX, panelY, panelX + len, panelY + thick};
    } else {
        const int winW = ball;
        const int winH = ThermometerSpan(ball, len, neck);
        const int panelX = (winW - thick) / 2;
        int winTop = 0, ballY = 0, panelY = 0;
        if (stem == FloatBallStem::Down) {
            winTop = ballTop;
            ballY = 0;
            panelY = ball - neck;
        } else {
            winTop = ballTop - stick;
            panelY = 0;
            ballY = stick;
        }
        f.window = {ballLeft, winTop, ballLeft + winW, winTop + winH};
        f.local.ball = {0, ballY, ball, ballY + ball};
        f.local.panel = {panelX, panelY, panelX + thick, panelY + len};
    }
    FillPanelTextRects(f.local, m);
    return f;
}

inline void FillPanelTextRects(FloatBallLocal& loc, const FloatBallMetrics& m) {
    const RECT& p = loc.panel;
    if (p.right <= p.left || p.bottom <= p.top) {
        loc.button = {};
        loc.title = {};
        loc.status = {};
        return;
    }
    RECT body = {p.left + m.padPx, p.top + m.padPx, p.right - m.padPx, p.bottom - m.padPx};
    const RECT& ball = loc.ball;
    const int tuck = 2;
    if (p.right > ball.left && p.left < ball.left)
        body.right = (std::min)(body.right, ball.left + tuck);
    else if (p.left < ball.right && p.right > ball.right)
        body.left = (std::max)(body.left, ball.right - tuck);
    else if (p.bottom > ball.top && p.top < ball.top)
        body.bottom = (std::min)(body.bottom, ball.top + tuck);
    else if (p.top < ball.bottom && p.bottom > ball.bottom)
        body.top = (std::max)(body.top, ball.bottom - tuck);

    loc.status = {};
    const int bodyH = body.bottom - body.top;
    const int bodyW = body.right - body.left;
    if (bodyH < 20 || bodyW < 20) {
        loc.button = body;
        loc.title = {};
        return;
    }
    const int titleH = (std::max)(16, (std::min)(22, bodyH * 34 / 100));
    const int gap = 3;
    int btnH = (std::max)(m.buttonH, (bodyH - titleH - gap) * 55 / 100);
    if (btnH + titleH + gap > bodyH) btnH = bodyH - titleH - gap;
    if (btnH < 16) {
        loc.button = body;
        loc.title = {};
        return;
    }
    loc.title = {body.left, body.top, body.right, body.top + titleH};
    int btnTop = body.bottom - btnH;
    if (btnTop < loc.title.bottom + gap) btnTop = loc.title.bottom + gap;
    loc.button = {body.left, btnTop, body.right, body.bottom};
}

inline FloatBallFrame ComputeDockedFrame(const RECT& work, FloatBallEdge edge,
    double xRatio, double yRatio, const FloatBallMetrics& m) {
    FloatBallFrame f{};
    const int left = BallLeftFromRatio(work, xRatio, m.ballPx);
    const int top = BallTopFromRatio(work, yRatio, m.ballPx);
    if (edge == FloatBallEdge::Right) {
        f.window = {work.right - m.peekPx, top, work.right, top + m.ballPx};
        f.local.ball = {0, 0, m.ballPx, m.ballPx};
    } else if (edge == FloatBallEdge::Left) {
        f.window = {work.left, top, work.left + m.peekPx, top + m.ballPx};
        f.local.ball = {m.peekPx - m.ballPx, 0, m.peekPx, m.ballPx};
    } else if (edge == FloatBallEdge::Top) {
        f.window = {left, work.top, left + m.ballPx, work.top + m.peekPx};
        f.local.ball = {0, m.peekPx - m.ballPx, m.ballPx, m.peekPx};
    } else {
        f.window = {left, work.bottom - m.peekPx, left + m.ballPx, work.bottom};
        f.local.ball = {0, 0, m.ballPx, m.ballPx};
    }
    return f;
}

inline FloatBallFrame ComputeExpandedFrame(const RECT& work, FloatBallEdge edge,
    double xRatio, double yRatio, const FloatBallMetrics& m) {
    const int left = BallLeftFromRatio(work, xRatio, m.ballPx);
    const int top = BallTopFromRatio(work, yRatio, m.ballPx);
    int ballLeft = left;
    int ballTop = top;
    if (edge == FloatBallEdge::Right) ballLeft = work.right - m.ballPx;
    else if (edge == FloatBallEdge::Left) ballLeft = work.left;
    else if (edge == FloatBallEdge::Top) ballTop = work.top;
    else ballTop = work.bottom - m.ballPx;
    FloatBallFrame f = PlaceThermometer(ballLeft, ballTop, StemForHorizontal(work, ballLeft, m), m);
    PinWindowToDockedEdge(f.window, work, edge);
    return f;
}

inline FloatBallFrame ComputeFreeFrame(const RECT& work, double xRatio, double yRatio,
    const FloatBallMetrics& m, bool withPanel) {
    const int left = BallLeftFromRatio(work, xRatio, m.ballPx);
    const int top = BallTopFromRatio(work, yRatio, m.ballPx);
    if (!withPanel) {
        FloatBallFrame f{};
        f.window = {left, top, left + m.ballPx, top + m.ballPx};
        f.local.ball = {0, 0, m.ballPx, m.ballPx};
        return f;
    }

    const FloatBallStem stem = StemForHorizontal(work, left, m);
    FloatBallFrame f = PlaceThermometer(left, top, stem, m);
    const int winW = f.window.right - f.window.left;
    const int winH = f.window.bottom - f.window.top;
    if (f.window.left < work.left) {
        f.window.left = work.left;
        f.window.right = work.left + winW;
    }
    if (f.window.right > work.right) {
        f.window.right = work.right;
        f.window.left = work.right - winW;
    }
    if (f.window.top < work.top) {
        f.window.top = work.top;
        f.window.bottom = work.top + winH;
    }
    if (f.window.bottom > work.bottom) {
        f.window.bottom = work.bottom;
        f.window.top = work.bottom - winH;
    }
    if (f.window.left < work.left) {
        f.window.left = work.left;
        f.window.right = work.left + winW;
    }
    if (f.window.top < work.top) {
        f.window.top = work.top;
        f.window.bottom = work.top + winH;
    }
    return f;
}

inline FloatBallSnap PoseFromBallTopLeft(int left, int top, const RECT& work, int ballPx) {
    FloatBallSnap s;
    s.docked = false;
    s.edge = FloatBallEdge::Right;
    s.xRatio = RatioFromAxis(work.left, work.right - work.left, left, ballPx);
    s.yRatio = RatioFromAxis(work.top, work.bottom - work.top, top, ballPx);
    return s;
}

inline FloatBallSnap ResolveReleaseSnap(int ballLeft, int ballTop, const RECT& work,
    int ballPx, int snapPx, int cursorX = INT_MIN, int cursorY = INT_MIN) {
    FloatBallSnap s = PoseFromBallTopLeft(ballLeft, ballTop, work, ballPx);
    const int right = ballLeft + ballPx;
    const int bottom = ballTop + ballPx;
    int best = snapPx + 1;
    auto consider = [&](int dist, FloatBallEdge edge) {
        if (dist < 0) dist = 0;
        if (dist <= snapPx && dist < best) {
            best = dist;
            s.docked = true;
            s.edge = edge;
        }
    };
    consider(ballLeft - work.left, FloatBallEdge::Left);
    consider(work.right - right, FloatBallEdge::Right);
    consider(ballTop - work.top, FloatBallEdge::Top);
    consider(work.bottom - bottom, FloatBallEdge::Bottom);
    if (cursorX != INT_MIN && cursorY != INT_MIN) {
        consider(cursorX - work.left, FloatBallEdge::Left);
        consider(work.right - cursorX, FloatBallEdge::Right);
        consider(cursorY - work.top, FloatBallEdge::Top);
        consider(work.bottom - cursorY, FloatBallEdge::Bottom);
    }
    return s;
}

inline bool PointInCircle(int x, int y, const RECT& ball) {
    const double cx = (ball.left + ball.right) * 0.5;
    const double cy = (ball.top + ball.bottom) * 0.5;
    const double r = (ball.right - ball.left) * 0.5;
    const double dx = x - cx;
    const double dy = y - cy;
    return dx * dx + dy * dy <= r * r + 0.5;
}

inline bool PointInRect(int x, int y, const RECT& rc) {
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

inline RECT ScreenBallRectPeek(FloatBallEdge edge, const RECT& work, double xRatio, double yRatio,
    const FloatBallMetrics& m) {
    const int left = BallLeftFromRatio(work, xRatio, m.ballPx);
    const int top = BallTopFromRatio(work, yRatio, m.ballPx);
    if (edge == FloatBallEdge::Right)
        return {work.right - m.peekPx, top, work.right - m.peekPx + m.ballPx, top + m.ballPx};
    if (edge == FloatBallEdge::Left)
        return {work.left + m.peekPx - m.ballPx, top, work.left + m.peekPx, top + m.ballPx};
    if (edge == FloatBallEdge::Top)
        return {left, work.top + m.peekPx - m.ballPx, left + m.ballPx, work.top + m.peekPx};
    return {left, work.bottom - m.peekPx, left + m.ballPx, work.bottom - m.peekPx + m.ballPx};
}

inline RECT ScreenBallRectExpanded(FloatBallEdge edge, const RECT& work, double xRatio,
    double yRatio, int ballPx) {
    const int left = BallLeftFromRatio(work, xRatio, ballPx);
    const int top = BallTopFromRatio(work, yRatio, ballPx);
    if (edge == FloatBallEdge::Right)
        return {work.right - ballPx, top, work.right, top + ballPx};
    if (edge == FloatBallEdge::Left)
        return {work.left, top, work.left + ballPx, top + ballPx};
    if (edge == FloatBallEdge::Top)
        return {left, work.top, left + ballPx, work.top + ballPx};
    return {left, work.bottom - ballPx, left + ballPx, work.bottom};
}

inline RECT ScreenBallRect(FloatBallEdge edge, const RECT& work, double xRatio, double yRatio,
    int ballPx) {
    return ScreenBallRectExpanded(edge, work, xRatio, yRatio, ballPx);
}

inline void PinWindowToDockedEdge(RECT& win, const RECT& work, FloatBallEdge edge) {
    const int w = win.right - win.left;
    const int h = win.bottom - win.top;
    if (edge == FloatBallEdge::Right) {
        win.right = work.right;
        win.left = work.right - w;
    } else if (edge == FloatBallEdge::Left) {
        win.left = work.left;
        win.right = work.left + w;
    } else if (edge == FloatBallEdge::Top) {
        win.top = work.top;
        win.bottom = work.top + h;
    } else {
        win.bottom = work.bottom;
        win.top = work.bottom - h;
    }
}

inline RECT MapFromFrame(const FloatBallFrame& src, const RECT& local, const RECT& dstWindow) {
    return RECT{
        src.window.left + local.left - dstWindow.left,
        src.window.top + local.top - dstWindow.top,
        src.window.left + local.right - dstWindow.left,
        src.window.top + local.bottom - dstWindow.top,
    };
}

inline float Smoothstep01(float t) {
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    return t * t * (3.f - 2.f * t);
}

inline bool PointInRoundRect(int x, int y, const RECT& rc, int radius) {
    if (!PointInRect(x, y, rc)) return false;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    int r = radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) return true;
    const int lx = x - rc.left;
    const int ly = y - rc.top;
    if (lx >= r && lx < w - r) return true;
    if (ly >= r && ly < h - r) return true;
    auto inCorner = [r](int dx, int dy) {
        return dx * dx + dy * dy <= r * r;
    };
    if (lx < r && ly < r) return inCorner(lx - r, ly - r);
    if (lx >= w - r && ly < r) return inCorner(lx - (w - r), ly - r);
    if (lx < r && ly >= h - r) return inCorner(lx - r, ly - (h - r));
    return inCorner(lx - (w - r), ly - (h - r));
}

inline FloatBallFrame ComputeDockedBallFrame(const RECT& work, FloatBallEdge edge,
    double xRatio, double yRatio, const FloatBallMetrics& m) {
    const int left = BallLeftFromRatio(work, xRatio, m.ballPx);
    const int top = BallTopFromRatio(work, yRatio, m.ballPx);
    FloatBallFrame f{};
    if (edge == FloatBallEdge::Right) {
        f.window = {work.right - m.ballPx, top, work.right, top + m.ballPx};
    } else if (edge == FloatBallEdge::Left) {
        f.window = {work.left, top, work.left + m.ballPx, top + m.ballPx};
    } else if (edge == FloatBallEdge::Top) {
        f.window = {left, work.top, left + m.ballPx, work.top + m.ballPx};
    } else {
        f.window = {left, work.bottom - m.ballPx, left + m.ballPx, work.bottom};
    }
    f.local.ball = {0, 0, m.ballPx, m.ballPx};
    return f;
}

inline RECT SizeLerpPinned(const RECT& from, const RECT& to, float t,
    FloatBallEdge edge, const RECT& work) {
    if (t <= 0.f) {
        RECT r = from;
        PinWindowToDockedEdge(r, work, edge);
        return r;
    }
    if (t >= 1.f) {
        RECT r = to;
        PinWindowToDockedEdge(r, work, edge);
        return r;
    }
    const int wa = from.right - from.left;
    const int wb = to.right - to.left;
    const int ha = from.bottom - from.top;
    const int hb = to.bottom - to.top;
    const int w = wa + static_cast<int>(static_cast<float>(wb - wa) * t + 0.5f);
    const int h = ha + static_cast<int>(static_cast<float>(hb - ha) * t + 0.5f);
    RECT r{};
    if (edge == FloatBallEdge::Right) {
        r.right = work.right;
        r.left = work.right - w;
        r.top = from.top;
        r.bottom = r.top + h;
    } else if (edge == FloatBallEdge::Left) {
        r.left = work.left;
        r.right = work.left + w;
        r.top = from.top;
        r.bottom = r.top + h;
    } else if (edge == FloatBallEdge::Top) {
        r.top = work.top;
        r.bottom = work.top + h;
        if (to.left < from.left) {
            r.right = from.right;
            r.left = r.right - w;
        } else {
            r.left = from.left;
            r.right = r.left + w;
        }
    } else {
        r.bottom = work.bottom;
        r.top = r.bottom - h;
        if (to.left < from.left) {
            r.right = from.right;
            r.left = r.right - w;
        } else {
            r.left = from.left;
            r.right = r.left + w;
        }
    }
    return r;
}

// 贴边展开：HWND 用最终展开尺寸（不再每帧改 left），只插值圆/身子的绘制。
inline FloatBallFrame ComputeDockedAnimFrame(const RECT& work, FloatBallEdge edge,
    double xRatio, double yRatio, const FloatBallMetrics& m, float t) {
    t = (std::max)(0.f, (std::min)(1.f, t));
    if (t <= 0.f) return ComputeDockedFrame(work, edge, xRatio, yRatio, m);

    const FloatBallFrame exp = ComputeExpandedFrame(work, edge, xRatio, yRatio, m);
    FloatBallFrame f = exp;
    const RECT peekB = ScreenBallRectPeek(edge, work, xRatio, yRatio, m);
    const RECT expB = ScreenBallRectExpanded(edge, work, xRatio, yRatio, m.ballPx);
    const float tBall = kFloatBallDockBallPhase;
    if (t < tBall) {
        const float u = t / tBall;
        const RECT sb = LerpRect(peekB, expB, u);
        f.local.ball = {sb.left - f.window.left, sb.top - f.window.top,
            sb.right - f.window.left, sb.bottom - f.window.top};
        f.local.panel = {};
        f.local.button = {};
        f.local.title = {};
        f.local.status = {};
        return f;
    }
    return exp;
}

inline float DockedBodyProgress(float t) {
    t = (std::max)(0.f, (std::min)(1.f, t));
    constexpr float tBall = kFloatBallDockBallPhase;
    if (t <= tBall) return 0.f;
    return (t - tBall) / (1.f - tBall);
}

// 贴边揭开身子：胶囊从圆头一侧往里长，HWND 尺寸不变。
inline RECT DockedRevealRect(FloatBallEdge edge, const RECT& content, const RECT& ball, float bodyU) {
    if (bodyU >= 1.f) return content;
    if (bodyU < 0.f) bodyU = 0.f;
    const int cw = content.right - content.left;
    const int ch = content.bottom - content.top;
    if (cw <= 0 || ch <= 0) return content;
    const int ballW = (std::max)(1, static_cast<int>(ball.right - ball.left));
    const int minW = (std::min)(cw, (std::max)(ch, ballW));
    int visW = minW + static_cast<int>(static_cast<float>(cw - minW) * bodyU + 0.5f);
    if (visW < minW) visW = minW;
    if (visW > cw) visW = cw;
    bool fromRight = (edge == FloatBallEdge::Right);
    if (edge != FloatBallEdge::Left && edge != FloatBallEdge::Right) {
        const int ballCx = (ball.left + ball.right) / 2;
        const int boxCx = (content.left + content.right) / 2;
        fromRight = ballCx > boxCx;
    }
    RECT r = content;
    if (fromRight) r.left = content.right - visW;
    else r.right = content.left + visW;
    return r;
}

inline bool PointHitsThermometer(int x, int y, const FloatBallLocal& local, bool panelOn) {
    if (PointInCircle(x, y, local.ball)) return true;
    if (!panelOn) return false;
    if (PointInRect(x, y, local.button) || PointInRect(x, y, local.panel)) return true;
    return false;
}

inline bool PointHitsClientStadium(int x, int y, int winW, int winH) {
    if (winW <= 0 || winH <= 0) return false;
    const RECT client{0, 0, winW, winH};
    return PointInRoundRect(x, y, client, (std::min)(winW, winH) / 2);
}

}  // namespace qst::desktop_tools
