#pragma once
// ──────────────────────────────────────────────────────────────────
// match_overlay.h — Frozen-screen overlay for find-image test / offset
// ──────────────────────────────────────────────────────────────────

#include <windows.h>
#include <string>
#include <vector>

#include "image_match.h"

enum class MatchOverlayMode { Test, OffsetPick };

class MatchOverlay {
public:
    MatchOverlay();
    ~MatchOverlay();

    struct ActionResult {
        bool cancelled = false;
        int offsetX = 0;
        int offsetY = 0;
    };

    ActionResult Show(const std::wstring& imagePath,
                      int searchX1, int searchY1, int searchX2, int searchY2,
                      double thresholdPercent, double scaleMin, double scaleMax,
                      MatchOverlayMode mode);

    bool matchDone_ = false;
    ImageMatchResult matchResult_;
    std::vector<ImageMatchResult> matchResults_;

private:
    static void RegisterWindowClass();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void Paint(HDC hdc);
    void DrawStatusBar(HDC hdc);
    void DrawMatchMarkers(HDC hdc);
    void DrawMagnifier(HDC hdc);
    COLORREF SamplePixel(int screenX, int screenY) const;

    void CleanupGdi();
    void CaptureScreen();
    void RunMatch();

    HWND hwnd_ = nullptr;
    bool loopExited_ = false;

    HBITMAP screenBitmap_ = nullptr;
    int screenW_ = 0, screenH_ = 0;
    int screenX_ = 0, screenY_ = 0;

    std::wstring imagePath_;
    int searchX1_ = 0, searchY1_ = 0, searchX2_ = 0, searchY2_ = 0;
    double thresholdPercent_ = 65.0;
    double scaleMin_ = 1.0;
    double scaleMax_ = 1.0;
    MatchOverlayMode mode_ = MatchOverlayMode::Test;

    bool cancelled_ = false;
    bool pendingCancel_ = false;
    int clickX_ = 0, clickY_ = 0;

    int matchMs_ = 0;
    int matchCount_ = 0;
    bool loadFailed_ = false;

    int cursorX_ = 0, cursorY_ = 0;
    bool cursorValid_ = false;

    HFONT statusFont_ = nullptr;
    HFONT labelFont_ = nullptr;
    HFONT magnifierFont_ = nullptr;

    static bool classRegistered_;
    static constexpr wchar_t kClassName[] = L"QMatchOverlay";
    static constexpr int kBorderWidth = 3;
    static constexpr COLORREF kBorderColor = RGB(255, 0, 0);
    static constexpr COLORREF kLabelColor = RGB(255, 40, 40);
    static constexpr COLORREF kStatusTextColor = RGB(255, 220, 60);
};
