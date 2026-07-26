#pragma once
// ──────────────────────────────────────────────────────────────────
// findimage_crop_editor.h — 找图模板预览裁切编辑器（主题/字号对齐主工程）
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <string>

#include "drawing.h"
#include "findimage_template_crop.h"

struct FindImageCropEditorResult {
    bool confirmed = false;
    bool fullImageNoOp = false;
    std::wstring newPath;
    int offsetX = 0;
    int offsetY = 0;
    CropRect rect{};
};

/// 模态裁切窗：显示模板、框选特征区、确认后写出新 BMP + offset'。
class FindImageCropEditor {
public:
    FindImageCropEditor();
    ~FindImageCropEditor();

    FindImageCropEditorResult Show(HWND owner,
        const std::wstring& imagePath,
        int offsetX, int offsetY,
        bool followUpSaveVar);

    bool IsOpen() const { return hwnd_ != nullptr && IsWindow(hwnd_); }

private:
    static void RegisterWindowClass();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void EnsureFonts();
    void Paint();
    void LayoutImageRect();
    RECT TitleBarRect() const;
    RECT CloseRect() const;
    RECT HelpRect() const;
    RECT StatusRect() const;
    RECT FooterRect() const;
    RECT ResetBtnRect() const;
    RECT ConfirmBtnRect() const;
    RECT CancelBtnRect() const;

    bool ClientToImage(int cx, int cy, int& ix, int& iy) const;
    void ImageToClient(int ix, int iy, int& cx, int& cy) const;
    RECT ImageCropToClient(const CropRect& r) const;
    void ResetSelectionFull();
    void UpdateConfirmEnabled();
    bool TryConfirm();
    void Cancel();
    void Cleanup();

    void DrawGreenButton(HDC hdc, const RECT& rc, const wchar_t* text, bool hover, bool enabled);
    void DrawOutlineButton(HDC hdc, const RECT& rc, const wchar_t* text, bool hover);

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HBITMAP imageBmp_ = nullptr;
    int imgW_ = 0, imgH_ = 0;
    int winW_ = 0, winH_ = 0;
    RECT imageDest_{};
    double scale_ = 1.0;

    int offsetX_ = 0, offsetY_ = 0;
    bool followUpSaveVar_ = false;
    std::wstring srcPath_;

    CropRect selection_{};
    bool dragging_ = false;
    bool draggingWindow_ = false;
    POINT dragStartClient_{};
    POINT dragEndClient_{};
    POINT windowDragScreen_{};
    int dragStartImgX_ = 0, dragStartImgY_ = 0;

    bool confirmEnabled_ = false;
    bool done_ = false;
    FindImageCropEditorResult result_{};

    bool hoverClose_ = false;
    bool hoverReset_ = false;
    bool hoverConfirm_ = false;
    bool hoverCancel_ = false;
    /// 0=arrow 1=cross 2=hand；避免每帧 SetCursor 导致闪烁
    int cursorKind_ = -1;

    HFONT titleFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HFONT btnFont_ = nullptr;
    HFONT closeFont_ = nullptr;

    WindowOuterShadow outerShadow_;

    static bool classRegistered_;
    static constexpr wchar_t kClassName[] = L"QST.FindImageCropEditor";
    static constexpr int kTitleH = 40;
    static constexpr int kPad = 16;
    static constexpr int kToolbarH = 64;
    static constexpr int kStatusH = 40;
    static constexpr int kHelpH = 36;
    static constexpr int kDragDeadPx = 3;
};
