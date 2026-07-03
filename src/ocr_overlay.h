#pragma once
// ocr_overlay.h — Frozen-screen overlay for OCR test

#include <windows.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "ocr_result.h"

enum class OcrOverlayMode { Test, OffsetPick };

class OcrOverlay {
public:
    OcrOverlay();
    ~OcrOverlay();

    struct ActionResult {
        bool cancelled = true;
        bool anchorValid = false;
        int offsetX = 0;
        int offsetY = 0;
    };

    ActionResult Show(int searchX1, int searchY1, int searchX2, int searchY2,
                      const std::wstring& highlightText = L"",
                      OcrOverlayMode mode = OcrOverlayMode::Test,
                      bool digitsOnly = false);

private:
    static void RegisterWindowClass();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void Paint(HDC hdc);
    void DrawStatusBar(HDC hdc);
    void DrawOcrMarkers(HDC hdc);
    void DrawMagnifier(HDC hdc);
    COLORREF SamplePixel(int screenX, int screenY) const;

    void CleanupGdi();
    void ClearLineFontCache();
    HFONT GetOrCreateFont(int fontPx);
    int FitFontPx(HDC hdc, const std::wstring& text, int boxWidth, int boxHeight);
    void DimBoxInterior(HDC hdc, int left, int top, int right, int bottom) const;
    void CaptureScreen();
    void RunOcr();
    OcrTextLine NearestLineToPoint(int screenX, int screenY) const;
    bool PickAnchorCenter(int& centerX, int& centerY) const;

    HWND hwnd_ = nullptr;
    bool loopExited_ = false;

    HBITMAP screenBitmap_ = nullptr;
    int screenW_ = 0, screenH_ = 0;
    int screenX_ = 0, screenY_ = 0;

    int searchX1_ = 0, searchY1_ = 0, searchX2_ = 0, searchY2_ = 0;
    std::wstring highlightText_;
    OcrOverlayMode mode_ = OcrOverlayMode::Test;
    bool digitsOnly_ = false;

    ActionResult actionResult_{};
    bool cancelled_ = false;
    bool pendingCancel_ = false;

    bool ocrDone_ = false;
    bool ocrSuccess_ = false;
    std::wstring errorMessage_;
    std::vector<OcrTextLine> lines_;
    int elapsedMs_ = 0;
    bool highlightFound_ = false;

    int cursorX_ = 0, cursorY_ = 0;
    bool cursorValid_ = false;

    HFONT statusFont_ = nullptr;
    HFONT magnifierFont_ = nullptr;
    std::unordered_map<int, HFONT> lineFontCache_;

    static bool classRegistered_;
    static constexpr wchar_t kClassName[] = L"QOcrOverlay";
    static constexpr int kBorderWidth = 2;
    static constexpr COLORREF kBorderColor = RGB(255, 0, 0);
    static constexpr COLORREF kTextColor = RGB(0, 80, 220);
    static constexpr COLORREF kHighlightTextColor = RGB(220, 0, 0);
    static constexpr COLORREF kStatusTextColor = RGB(255, 220, 60);
    static constexpr BYTE kDimOverlayAlpha = 200;
};
