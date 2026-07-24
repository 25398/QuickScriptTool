#pragma once
// ──────────────────────────────────────────────────────────────────
// hotkey_dialog.h — 热键/按键捕获模态对话框
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include "config.h"
#include "drawing.h"
#include "utils.h"

class HotkeyCapture {
public:
    HotkeyCapture() = default;
    ~HotkeyCapture() = default;

    bool Show(HWND owner, const Hotkey& oldValue, bool scriptHotkey,
              Hotkey& out, bool globalStartStop = false,
              double holdThresholdSeconds = 0.2);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void OnCreate();
    void OnPaint();
    void OnMouseMove(int x, int y);
    void OnMouseLeave();
    void OnSetCursor();
    void OnLButtonDown(int x, int y);
    void OnKeyDown(UINT vk);
    void OnKeyUp(UINT vk);
    void OnHoldCaptureTimer();
    void Close(bool accept);
    void CleanupGdi();
    void CancelPendingCapture();
    void ApplyCapturedKey(UINT vk, UINT mods, bool holdMode);
    bool IsHotkeyCaptureMode() const { return scriptHotkey_ || globalStartStop_; }
    DWORD CaptureHoldMs() const {
        return HoldThresholdMsFromSeconds(holdThresholdSeconds_);
    }

    void SetHoverFlag(bool& flag, bool value, const RECT& rc);
    void UpdateHover(int x, int y);
    void UpdateHoverFromCursor();
    void EnsureMouseLeaveTracking();
    void InvalidateValueArea();

    RECT OkRect() const { return {284, 164, 348, 196}; }
    RECT CancelRect() const { return {206, 164, 270, 196}; }
    RECT ResetRect() const { return {16, 160, 102, 196}; }
    RECT DeleteRect() const { return {108, 160, 194, 196}; }
    RECT ValueRect() const { return {16, 82, 348, 126}; }

    static bool InRect(const RECT& rc, int x, int y) {
        return x >= rc.left && x <= rc.right && y >= rc.top && y <= rc.bottom;
    }

    void DrawBtn(HDC hdc, const RECT& rc, const wchar_t* text, bool green, bool hover);

    static constexpr UINT_PTR kHoldCaptureTimerId = 1;

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HFONT font_ = nullptr;
    HFONT valueFont_ = nullptr;
    Hotkey old_{};
    Hotkey current_{};
    bool scriptHotkey_ = false;
    bool globalStartStop_ = false;
    double holdThresholdSeconds_ = 0.2;
    bool done_ = false;
    bool accepted_ = false;
    bool trackingMouse_ = false;
    bool hoverOk_ = false;
    bool hoverCancel_ = false;
    bool hoverReset_ = false;
    bool hoverDelete_ = false;
    bool pendingDown_ = false;
    UINT pendingVk_ = 0;
    UINT pendingMods_ = 0;
    DWORD pendingDownTick_ = 0;
    WindowOuterShadow outerShadow_;
};
