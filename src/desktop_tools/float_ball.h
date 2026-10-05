#pragma once
// 桌面悬浮球：贴边半露 / 自由悬浮、悬停展开小面板。DesktopTools 永久原生窗。
//
// 交互（2026-09-24 定稿，用户确认）：
//   · 圆头图标 = 脚本启停开关（▶ 待启动 / ■ 运行中 / ‖ 已暂停 / ● 录制 / ··· 连点），
//     点一下就在「启动 ↔ 停止」之间切换；没有独立按钮。
//   · 面板（脚本名 + 当前状态那一块）= 点一下显示主界面。

#include "desktop_tools/float_ball_geom.h"

#include <functional>
#include <string>

namespace qst::desktop_tools {

enum class FloatBallActivity {
    Idle = 0,
    MacroRunning,
    Clicking,
    Recording,
    BreakoutPaused,
};

struct FloatBallModel {
    FloatBallActivity activity = FloatBallActivity::Idle;
    std::wstring title;       // 面板第一行：脚本名（无内容则不占位，第二行居中）
    std::wstring statusText;  // 面板第二行（原「启动/停止」按钮位）：脚本当前状态
    bool canStart = false;
    bool busy = false;
    int actionIndex = 0;  // 1-based，当前动作；0=尚未进入
    int actionTotal = 0;
};

struct FloatBallCallbacks {
    std::function<void()> onStartSelectedMacro;
    std::function<void()> onStopRunning;
    std::function<void()> onShowMainWindow;
    std::function<void()> onHideFromMenu;
    std::function<void(bool docked, int edge, double xRatio, double yRatio, std::wstring monitorId)>
        onPlacementChanged;
};

class FloatBall {
public:
    static FloatBall& Instance();

    void Create(HINSTANCE inst, HWND ownerHint);
    void Destroy();
    HWND Hwnd() const { return hwnd_; }

    void SetCallbacks(FloatBallCallbacks cb);
    void SetVisible(bool show);
    void SetPlacement(bool docked, int edge, double xRatio, double yRatio,
        const std::wstring& monitorId);
    void SetModel(const FloatBallModel& model);
    void RefreshTheme();

private:
    FloatBall() = default;
    FloatBall(const FloatBall&) = delete;
    FloatBall& operator=(const FloatBall&) = delete;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    bool EnsureClass(HINSTANCE inst);
    bool EnsureWindow(HINSTANCE inst);
    void EnsureGdiplus();
    void ShutdownGdiplus();
    void EnsureMoveCursors();
    void DestroyMoveCursors();

    UINT MonitorDpi(HMONITOR mon) const;
    FloatBallMetrics ScaledMetrics(HMONITOR mon) const;
    HMONITOR ResolveMonitor() const;
    RECT WorkArea(HMONITOR mon) const;
    std::wstring MonitorId(HMONITOR mon) const;

    void ApplyLayout(bool paint);
    void ApplyShadowPad();
    void RelayoutPanelLines();
    bool CursorOnDockedStrip() const;
    void Paint();
    void TrackHover(bool over);
    void RefreshHoverFromCursor();
    bool PointerOverInteractive() const;
    bool PanelVisible() const;
    float LayoutT() const;
    void StartCollapseTimer();
    void KillHoverTimers();
    void SetWantExpand(bool expand);
    void TickAnim();
    void TickFullscreen();
    void TickCpu();
    void BeginDrag();
    void UpdateDrag(POINT screen);
    void EndDrag();
    void PersistPlacement();
    int HitTest(int x, int y) const;
    void ShowContextMenu(POINT screen);
    void OnPrimaryClick(int hit);
    bool BallPressed() const;

    HWND hwnd_ = nullptr;
    HWND ownerHint_ = nullptr;
    HINSTANCE inst_ = nullptr;
    ULONG_PTR gdiplusToken_ = 0;
    bool gdiplusOk_ = false;
    HCURSOR hoverCursor_ = nullptr;
    HCURSOR pressCursor_ = nullptr;
    int cursorPx_ = 0;

    bool userVisible_ = true;
    bool fsHidden_ = false;
    DWORD fsExitTick_ = 0;  // 退出全屏的时刻（0=未在延迟窗口内），用于滞回恢复
    bool dragging_ = false;
    bool wantExpand_ = false;
    bool hoverInside_ = false;
    bool ballPressed_ = false;
    float expandT_ = 0.f;
    float pulse_ = 0.f;
    float cpu01_ = 0.f;
    unsigned long long cpuIdle_ = 0;
    unsigned long long cpuKernel_ = 0;
    unsigned long long cpuUser_ = 0;
    bool cpuPrimed_ = false;
    DWORD cpuLastTick_ = 0;

    bool docked_ = true;
    FloatBallEdge edge_ = FloatBallEdge::Right;
    double xRatio_ = 1.0;
    double yRatio_ = 0.55;
    std::wstring monitorId_;

    FloatBallModel model_{};
    FloatBallCallbacks cb_{};
    FloatBallFrame frame_{};
    FloatBallMetrics metrics_{};

    POINT dragGrab_{};
    POINT dragOriginScreen_{};
    bool dragMoved_ = false;
    bool pendingDrag_ = false;
    int pressHit_ = 0;
    int hoverHit_ = 0;
};

}  // namespace qst::desktop_tools
