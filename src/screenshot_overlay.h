#pragma once
// ──────────────────────────────────────────────────────────────────
// screenshot_overlay.h — QQ-style screenshot overlay (declaration)
// Full-screen transparent overlay for region selection and screenshot
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <functional>
#include <string>

/// Screenshot overlay — QQ-style full-screen capture with selection UI
/// Usage:
///   ScreenshotOverlay overlay;
///   overlay.Show(callback);  // callback receives final RECT (or empty on cancel)
class ScreenshotOverlay {
public:
    ScreenshotOverlay();
    ~ScreenshotOverlay();

    /// Show the overlay and begin region selection
    /// @param onConfirm  Called when user confirms; receives the selected
    ///                   screen rect.  Receives empty rect on cancel.
    void Show(std::function<void(RECT)> onConfirm);

    /// Set the title text shown in the toolbar (e.g. "屏幕截图" or "选取区域")
    void SetTitle(const std::wstring& title) { title_ = title; }

private:
    enum class State { Dragging, Selected, Moving, ResizingN, ResizingS, ResizingW, ResizingE, ResizingNW, ResizingNE, ResizingSW, ResizingSE };

    // ── Window registration ──────────────────────────────────────
    static void RegisterWindowClass();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    // ── Drawing ──────────────────────────────────────────────────
    void Paint(HDC hdc);
    void DrawBackground(HDC hdc);
    void DrawSelectionBorder(HDC hdc);
    void DrawSizeInfo(HDC hdc);
    void DrawToolbar(HDC hdc);
    void DrawMagnifier(HDC hdc);

    // ── Hit testing ──────────────────────────────────────────────
    State HitTest(int x, int y);
    RECT ToolbarRect() const;
    bool HitConfirm(int x, int y) const;
    bool HitCancel(int x, int y) const;
    bool HitToolbar(int x, int y) const;

    // ── Helpers ──────────────────────────────────────────────────
    void NormalizeSelection();
    void CaptureVirtualScreen();
    bool IsValidSelection() const;

    // ── State ────────────────────────────────────────────────────
    HWND hwnd_ = nullptr;
    State state_ = State::Dragging;
    POINT dragStart_{}, dragEnd_{};
    POINT moveAnchor_{};
    RECT selection_{};
    RECT moveStart_{};
    std::function<void(RECT)> onConfirm_;
    std::wstring title_ = L"屏幕截图";
    bool resultCancelled_ = false;   // true when user cancelled (Esc/right-click), false when confirmed
    bool pendingCancel_ = false;     // right-click DOWN consumed; waiting for UP before hiding

    // ── Screen resources ─────────────────────────────────────────
    HBITMAP screenBitmap_ = nullptr;
    HBITMAP dimOverlay_ = nullptr;   // cached dark overlay (full-screen, size screenW_×screenH_)
    int screenW_ = 0, screenH_ = 0;
    int screenX_ = 0, screenY_ = 0;

    // ── Appearance ───────────────────────────────────────────────
    static constexpr int kHandleSize = 8;
    static constexpr int kBorderWidth = 2;
    static constexpr int kToolbarH = 40;
    static constexpr int kToolbarBtnW = 70;
    static constexpr COLORREF kOverlayColor = RGB(0, 0, 0);
    static constexpr BYTE kOverlayAlpha = 120;
    static constexpr COLORREF kBorderColor = RGB(0, 120, 255);

    // ── Static (shared) ──────────────────────────────────────────
    static bool classRegistered_;
    static constexpr wchar_t kClassName[] = L"QScreenshotOverlay";
};
