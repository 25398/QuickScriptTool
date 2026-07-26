// ──────────────────────────────────────────────────────────────────
// findimage_crop_editor.cpp — 找图模板预览裁切编辑器
// 字号 / 主题色 / 自绘标题栏对齐录制优化对话框等主工程弹窗
// ──────────────────────────────────────────────────────────────────

#include "findimage_crop_editor.h"

#include "config.h"
#include "drawing.h"
#include "image_match.h"
#include "ui_scale.h"
#include "utils.h"

#include <windowsx.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

bool FindImageCropEditor::classRegistered_ = false;

namespace {
std::atomic_uint g_cropFileSeq{0};

int S(int v) { return UiLen(v); }

bool PtIn(const RECT& rc, int x, int y) {
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}
}  // namespace

void FindImageCropEditor::RegisterWindowClass() {
    if (classRegistered_) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &FindImageCropEditor::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    // 空类光标：完全由 WM_SETCURSOR 决定，避免与 IDC_CROSS 来回抢
    wc.hCursor = nullptr;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    classRegistered_ = true;
}

FindImageCropEditor::FindImageCropEditor() = default;

FindImageCropEditor::~FindImageCropEditor() {
    Cleanup();
}

void FindImageCropEditor::EnsureFonts() {
    static HFONT sTitle = nullptr, sBody = nullptr, sSmall = nullptr, sBtn = nullptr, sClose = nullptr;
    if (!sTitle) {
        sTitle = CreateFontW(UiFontHeight(28), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        sBody = CreateFontW(UiFontHeight(24), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        sSmall = CreateFontW(UiFontHeight(22), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        sBtn = CreateFontW(UiFontHeight(24), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        sClose = CreateFontW(UiFontHeight(38), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
    }
    titleFont_ = sTitle;
    bodyFont_ = sBody;
    smallFont_ = sSmall;
    btnFont_ = sBtn;
    closeFont_ = sClose;
}

void FindImageCropEditor::Cleanup() {
    outerShadow_.Detach();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (imageBmp_) {
        DeleteBitmapHandle(imageBmp_);
        imageBmp_ = nullptr;
    }
    titleFont_ = bodyFont_ = smallFont_ = btnFont_ = closeFont_ = nullptr;
}

RECT FindImageCropEditor::TitleBarRect() const {
    return RECT{0, 0, winW_, S(kTitleH)};
}

RECT FindImageCropEditor::CloseRect() const {
    return RECT{winW_ - S(kCloseBtnW), 0, winW_, S(kTitleH)};
}

RECT FindImageCropEditor::HelpRect() const {
    return RECT{S(kPad), S(kTitleH) + S(8), winW_ - S(kPad), S(kTitleH) + S(kHelpH)};
}

RECT FindImageCropEditor::StatusRect() const {
    return RECT{S(kPad), winH_ - S(kToolbarH) - S(kStatusH),
        winW_ - S(kPad), winH_ - S(kToolbarH)};
}

RECT FindImageCropEditor::FooterRect() const {
    return RECT{0, winH_ - S(kToolbarH), winW_, winH_};
}

RECT FindImageCropEditor::ResetBtnRect() const {
    const int y = winH_ - S(kToolbarH) + S(12);
    return RECT{S(kPad), y, S(kPad) + S(120), y + S(40)};
}

RECT FindImageCropEditor::ConfirmBtnRect() const {
    const int y = winH_ - S(kToolbarH) + S(12);
    return RECT{winW_ - S(kPad) - S(210), y, winW_ - S(kPad) - S(110), y + S(40)};
}

RECT FindImageCropEditor::CancelBtnRect() const {
    const int y = winH_ - S(kToolbarH) + S(12);
    return RECT{winW_ - S(kPad) - S(100), y, winW_ - S(kPad), y + S(40)};
}

FindImageCropEditorResult FindImageCropEditor::Show(HWND owner,
    const std::wstring& imagePath,
    int offsetX, int offsetY,
    bool followUpSaveVar) {
    result_ = {};
    done_ = false;
    owner_ = owner;
    offsetX_ = offsetX;
    offsetY_ = offsetY;
    followUpSaveVar_ = followUpSaveVar;
    srcPath_ = ResolveImagePath(imagePath);
    hoverClose_ = hoverReset_ = hoverConfirm_ = hoverCancel_ = false;
    cursorKind_ = -1;
    Cleanup();

    imageBmp_ = LoadBitmapFromFile(srcPath_);
    if (!imageBmp_) {
        result_.confirmed = false;
        return result_;
    }
    BITMAP bm{};
    GetObjectW(imageBmp_, sizeof(bm), &bm);
    imgW_ = bm.bmWidth;
    imgH_ = bm.bmHeight;
    if (imgW_ < kFindImageCropMinSide || imgH_ < kFindImageCropMinSide) {
        Cleanup();
        result_.confirmed = false;
        return result_;
    }

    if (owner) UiScaleInitFromHwnd(owner);
    EnsureFonts();
    RegisterWindowClass();

    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int chrome = S(kTitleH) + S(kHelpH) + S(kPad) * 2 + S(kStatusH) + S(kToolbarH) + S(24);
    winW_ = (std::min)(screenW - S(40), (std::max)(S(720), imgW_ + S(kPad) * 2 + S(80)));
    winH_ = (std::min)(screenH - S(40), (std::max)(S(520), imgH_ + chrome));
    const int x = (screenW - winW_) / 2;
    const int y = (screenH - winH_) / 2;

    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kClassName, L"裁切找图模板",
        WS_POPUP | WS_CLIPCHILDREN | WS_VISIBLE,
        x, y, winW_, winH_,
        owner, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        Cleanup();
        return result_;
    }
    SetClassLongPtrW(hwnd_, GCLP_HCURSOR, 0);
    outerShadow_.Attach(hwnd_);

    ResetSelectionFull();
    LayoutImageRect();
    UpdateConfirmEnabled();
    SetForegroundWindow(hwnd_);

    MSG msg{};
    while (!done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_ESCAPE) {
                Cancel();
                continue;
            }
            if (msg.wParam == VK_RETURN && confirmEnabled_) {
                TryConfirm();
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Cleanup();
    return result_;
}

LRESULT CALLBACK FindImageCropEditor::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    FindImageCropEditor* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<FindImageCropEditor*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return TRUE;
    }
    self = reinterpret_cast<FindImageCropEditor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->Handle(msg, wp, lp);
}

void FindImageCropEditor::LayoutImageRect() {
    const int top = S(kTitleH) + S(kHelpH) + S(8);
    const int bottom = winH_ - S(kToolbarH) - S(kStatusH) - S(4);
    const int availW = winW_ - S(kPad) * 2;
    const int availH = bottom - top;
    if (availW <= 0 || availH <= 0 || imgW_ <= 0 || imgH_ <= 0) {
        imageDest_ = RECT{S(kPad), top, S(kPad), top};
        scale_ = 1.0;
        return;
    }
    const double sx = static_cast<double>(availW) / imgW_;
    const double sy = static_cast<double>(availH) / imgH_;
    scale_ = (std::min)(1.0, (std::min)(sx, sy));
    const int dw = static_cast<int>(std::lround(imgW_ * scale_));
    const int dh = static_cast<int>(std::lround(imgH_ * scale_));
    const int left = S(kPad) + (availW - dw) / 2;
    const int topImg = top + (availH - dh) / 2;
    imageDest_ = RECT{left, topImg, left + dw, topImg + dh};
}

bool FindImageCropEditor::ClientToImage(int cx, int cy, int& ix, int& iy) const {
    const int dw = imageDest_.right - imageDest_.left;
    const int dh = imageDest_.bottom - imageDest_.top;
    if (dw <= 0 || dh <= 0) return false;
    const double nx = (cx - imageDest_.left) / static_cast<double>(dw);
    const double ny = (cy - imageDest_.top) / static_cast<double>(dh);
    ix = static_cast<int>(std::floor(nx * imgW_));
    iy = static_cast<int>(std::floor(ny * imgH_));
    ix = (std::max)(0, (std::min)(ix, imgW_));
    iy = (std::max)(0, (std::min)(iy, imgH_));
    return true;
}

void FindImageCropEditor::ImageToClient(int ix, int iy, int& cx, int& cy) const {
    const int dw = imageDest_.right - imageDest_.left;
    const int dh = imageDest_.bottom - imageDest_.top;
    cx = imageDest_.left + static_cast<int>(std::lround(ix * (dw / static_cast<double>(imgW_))));
    cy = imageDest_.top + static_cast<int>(std::lround(iy * (dh / static_cast<double>(imgH_))));
}

RECT FindImageCropEditor::ImageCropToClient(const CropRect& r) const {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    ImageToClient(r.L, r.T, x0, y0);
    ImageToClient(r.R, r.B, x1, y1);
    return RECT{x0, y0, x1, y1};
}

void FindImageCropEditor::ResetSelectionFull() {
    selection_ = CropRect{0, 0, imgW_, imgH_};
    dragging_ = false;
    UpdateConfirmEnabled();
}

void FindImageCropEditor::UpdateConfirmEnabled() {
    const auto computed = ComputeCroppedFindImageOffset(
        imgW_, imgH_, offsetX_, offsetY_, selection_);
    confirmEnabled_ = computed.ok;
}

void FindImageCropEditor::Cancel() {
    result_.confirmed = false;
    done_ = true;
    if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

bool FindImageCropEditor::TryConfirm() {
    const auto computed = ComputeCroppedFindImageOffset(
        imgW_, imgH_, offsetX_, offsetY_, selection_);
    if (!computed.ok) return false;

    if (IsFullImageCrop(computed.rect, imgW_, imgH_)) {
        result_.confirmed = true;
        result_.fullImageNoOp = true;
        result_.offsetX = offsetX_;
        result_.offsetY = offsetY_;
        result_.rect = computed.rect;
        done_ = true;
        if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return true;
    }

    EnsureFindImagesDir();
    const unsigned seq = g_cropFileSeq.fetch_add(1) + 1;
    const std::wstring name = MakeFindImageCropFileName(GetTickCount64(), seq);
    const std::wstring path = FindImagesDir() + L"\\" + name;
    if (!SaveCroppedTemplateRegion(srcPath_,
            computed.rect.L, computed.rect.T, computed.rect.R, computed.rect.B, path)) {
        MessageBoxW(hwnd_, L"保存裁切图片失败，请检查磁盘空间与权限。",
            L"裁切模板", MB_OK | MB_ICONWARNING);
        return false;
    }

    result_.confirmed = true;
    result_.fullImageNoOp = false;
    result_.newPath = path;
    result_.offsetX = computed.offsetX;
    result_.offsetY = computed.offsetY;
    result_.rect = computed.rect;
    done_ = true;
    if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    return true;
}

void FindImageCropEditor::DrawGreenButton(HDC hdc, const RECT& rc, const wchar_t* text,
    bool hover, bool enabled) {
    const COLORREF fill = enabled
        ? (hover ? kButtonGreenHover : kButtonGreen)
        : kButtonDisabledGreen;
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, S(6), S(6));
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, enabled ? kWhite : kButtonDisabledText);
    SelectObject(hdc, btnFont_);
    DrawTextW(hdc, text, -1, const_cast<RECT*>(&rc), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void FindImageCropEditor::DrawOutlineButton(HDC hdc, const RECT& rc, const wchar_t* text, bool hover) {
    DrawBorderRoundRect(hdc, rc, hover ? kMainGreen : kComboBorderGray, S(6));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, kMainGreen);
    SelectObject(hdc, btnFont_);
    DrawTextW(hdc, text, -1, const_cast<RECT*>(&rc), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT FindImageCropEditor::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE: {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        winW_ = rc.right;
        winH_ = rc.bottom;
        LayoutImageRect();
        outerShadow_.Sync();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_PAINT:
        Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR: {
        if (LOWORD(lp) != HTCLIENT) break;
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        int kind = 0;  // arrow
        if (dragging_) {
            kind = 1;  // 框选拖拽中固定十字，避免与箭头来回闪
        } else if (PtIn(CloseRect(), pt.x, pt.y)
            || PtIn(ResetBtnRect(), pt.x, pt.y)
            || PtIn(ConfirmBtnRect(), pt.x, pt.y)
            || PtIn(CancelBtnRect(), pt.x, pt.y)) {
            kind = 2;
        } else if (PtIn(imageDest_, pt.x, pt.y)
            && !PtIn(FooterRect(), pt.x, pt.y)
            && !PtIn(TitleBarRect(), pt.x, pt.y)) {
            kind = 1;
        }
        static HCURSOR kArrow = nullptr, kCross = nullptr, kHand = nullptr;
        if (!kArrow) {
            kArrow = LoadCursorW(nullptr, IDC_ARROW);
            kCross = LoadCursorW(nullptr, IDC_CROSS);
            kHand = LoadCursorW(nullptr, IDC_HAND);
        }
        HCURSOR cur = (kind == 1) ? kCross : ((kind == 2) ? kHand : kArrow);
        // 每次 SETCURSOR 都设置（系统会在消息间重置）；kind 缓存仅用于调试/扩展
        cursorKind_ = kind;
        SetCursor(cur);
        return TRUE;
    }
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (draggingWindow_) {
            POINT cur{};
            GetCursorPos(&cur);
            SetWindowPos(hwnd_, nullptr,
                cur.x - windowDragScreen_.x, cur.y - windowDragScreen_.y,
                0, 0, SWP_NOSIZE | SWP_NOZORDER);
            return 0;
        }
        if (dragging_) {
            dragEndClient_ = POINT{x, y};
            int ix = 0, iy = 0;
            ClientToImage(x, y, ix, iy);
            const int dx = x - dragStartClient_.x;
            const int dy = y - dragStartClient_.y;
            if (std::abs(dx) >= kDragDeadPx || std::abs(dy) >= kDragDeadPx) {
                const CropRect next = ClampCropRectToImage(
                    NormalizeCropRect(dragStartImgX_, dragStartImgY_, ix, iy), imgW_, imgH_);
                if (next.L != selection_.L || next.T != selection_.T
                    || next.R != selection_.R || next.B != selection_.B) {
                    selection_ = next;
                    UpdateConfirmEnabled();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
            return 0;
        }
        const bool hc = PtIn(CloseRect(), x, y);
        const bool hr = PtIn(ResetBtnRect(), x, y);
        const bool hok = PtIn(ConfirmBtnRect(), x, y);
        const bool hcancel = PtIn(CancelBtnRect(), x, y);
        if (hc != hoverClose_ || hr != hoverReset_ || hok != hoverConfirm_ || hcancel != hoverCancel_) {
            hoverClose_ = hc;
            hoverReset_ = hr;
            hoverConfirm_ = hok;
            hoverCancel_ = hcancel;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (PtIn(CloseRect(), x, y)) { Cancel(); return 0; }
        if (PtIn(ConfirmBtnRect(), x, y)) {
            if (confirmEnabled_) TryConfirm();
            return 0;
        }
        if (PtIn(CancelBtnRect(), x, y)) { Cancel(); return 0; }
        if (PtIn(ResetBtnRect(), x, y)) {
            ResetSelectionFull();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        if (PtIn(TitleBarRect(), x, y) && !PtIn(CloseRect(), x, y)) {
            draggingWindow_ = true;
            POINT cur{};
            GetCursorPos(&cur);
            RECT wr{};
            GetWindowRect(hwnd_, &wr);
            windowDragScreen_ = POINT{cur.x - wr.left, cur.y - wr.top};
            SetCapture(hwnd_);
            return 0;
        }
        if (PtIn(imageDest_, x, y)) {
            dragging_ = true;
            dragStartClient_ = POINT{x, y};
            dragEndClient_ = dragStartClient_;
            ClientToImage(x, y, dragStartImgX_, dragStartImgY_);
            SetCapture(hwnd_);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (draggingWindow_) {
            draggingWindow_ = false;
            ReleaseCapture();
            return 0;
        }
        if (dragging_) {
            dragging_ = false;
            ReleaseCapture();
            int ix = 0, iy = 0;
            ClientToImage(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), ix, iy);
            const int dx = GET_X_LPARAM(lp) - dragStartClient_.x;
            const int dy = GET_Y_LPARAM(lp) - dragStartClient_.y;
            if (std::abs(dx) >= kDragDeadPx || std::abs(dy) >= kDragDeadPx) {
                selection_ = ClampCropRectToImage(
                    NormalizeCropRect(dragStartImgX_, dragStartImgY_, ix, iy), imgW_, imgH_);
            }
            UpdateConfirmEnabled();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_CLOSE:
        if (!done_) {
            result_.confirmed = false;
            done_ = true;
        }
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return 0;
    case WM_DESTROY:
        outerShadow_.Detach();
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void FindImageCropEditor::Paint() {
    PAINTSTRUCT ps{};
    HDC windowDc = BeginPaint(hwnd_, &ps);
    RECT client{};
    GetClientRect(hwnd_, &client);
    HDC mem = CreateCompatibleDC(windowDc);
    HBITMAP canvas = CreateCompatibleBitmap(windowDc, client.right, client.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, canvas);

    FillRectColor(mem, client, kWhite);

    // Title bar
    const RECT titleBar = TitleBarRect();
    FillRectColor(mem, titleBar, kMainGreen);
    SelectObject(mem, titleFont_);
    DrawTextIn(mem, L"裁切找图模板",
        RECT{S(kPad), 0, winW_ - S(kCloseBtnW), S(kTitleH)},
        kWhite, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (hoverClose_) FillRectColor(mem, CloseRect(), kCloseHover);
    SelectObject(mem, closeFont_);
    DrawTextIn(mem, L"×", CloseRect(), kWhite, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // Help
    SelectObject(mem, bodyFont_);
    const wchar_t* help = followUpSaveVar_
        ? L"框选特征区；偏移参考点（存变量模式）须落在框内（图内时）。确认后更新模板并保持参考点相对内容不变。"
        : L"框选特征区；裁切后更新模板，并保持点击相对画面内容不变。";
    DrawTextIn(mem, help, HelpRect(), kMainGreen,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Image canvas background
    RECT imgBg{S(kPad) - S(4), imageDest_.top - S(4),
        winW_ - S(kPad) + S(4),
        (std::max)(imageDest_.bottom, StatusRect().top - S(8)) + S(4)};
    if (imgBg.bottom > imgBg.top) FillRectColor(mem, imgBg, kPanel);

    if (imageBmp_) {
        HDC src = CreateCompatibleDC(mem);
        HGDIOBJ old = SelectObject(src, imageBmp_);
        SetStretchBltMode(mem, HALFTONE);
        StretchBlt(mem, imageDest_.left, imageDest_.top,
            imageDest_.right - imageDest_.left, imageDest_.bottom - imageDest_.top,
            src, 0, 0, imgW_, imgH_, SRCCOPY);
        SelectObject(src, old);
        DeleteDC(src);
    }

    // Selection border
    const RECT selClient = ImageCropToClient(selection_);
    HPEN pen = CreatePen(PS_SOLID, 2, kMainGreen);
    HGDIOBJ oldPen = SelectObject(mem, pen);
    HGDIOBJ oldBr = SelectObject(mem, GetStockObject(NULL_BRUSH));
    Rectangle(mem, selClient.left, selClient.top, selClient.right, selClient.bottom);
    SelectObject(mem, oldPen);
    SelectObject(mem, oldBr);
    DeleteObject(pen);

    // Size chip
    const int cw = selection_.R - selection_.L;
    const int ch = selection_.B - selection_.T;
    SelectObject(mem, smallFont_);
    std::wstring sizeText = std::to_wstring(cw) + L" × " + std::to_wstring(ch);
    RECT sizeRc{selClient.left, selClient.top - S(28), selClient.left + S(140), selClient.top - S(4)};
    if (sizeRc.top < S(kTitleH) + S(kHelpH)) {
        sizeRc = RECT{selClient.left, selClient.bottom + S(4), selClient.left + S(140), selClient.bottom + S(28)};
    }
    DrawTextIn(mem, sizeText, sizeRc, kMainGreen, DT_LEFT | DT_SINGLELINE);

    // Click marker
    const int clickX = imgW_ / 2 + offsetX_;
    const int clickY = imgH_ / 2 + offsetY_;
    const bool clickIn = IsClickInsideImage(clickX, clickY, imgW_, imgH_);
    if (clickIn) {
        int cx = 0, cy = 0;
        ImageToClient(clickX, clickY, cx, cy);
        HPEN cross = CreatePen(PS_SOLID, 2, kOrange);
        HGDIOBJ op = SelectObject(mem, cross);
        MoveToEx(mem, cx - S(12), cy, nullptr); LineTo(mem, cx + S(13), cy);
        MoveToEx(mem, cx, cy - S(12), nullptr); LineTo(mem, cx, cy + S(13));
        SelectObject(mem, op);
        DeleteObject(cross);
    } else {
        SelectObject(mem, bodyFont_);
        DrawTextIn(mem, L"点击偏移在模板外，裁切不强制包含该点。",
            RECT{imageDest_.left, imageDest_.bottom + S(4), imageDest_.right, imageDest_.bottom + S(32)},
            kOrange, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        int ex = (std::max)(0, (std::min)(clickX, imgW_ - 1));
        int ey = (std::max)(0, (std::min)(clickY, imgH_ - 1));
        int cx = 0, cy = 0;
        ImageToClient(ex, ey, cx, cy);
        HPEN dash = CreatePen(PS_DOT, 1, kOrange);
        HGDIOBJ op = SelectObject(mem, dash);
        MoveToEx(mem, (imageDest_.left + imageDest_.right) / 2,
            (imageDest_.top + imageDest_.bottom) / 2, nullptr);
        LineTo(mem, cx, cy);
        SelectObject(mem, op);
        DeleteObject(dash);
    }

    // Status
    const auto preview = ComputeCroppedFindImageOffset(imgW_, imgH_, offsetX_, offsetY_, selection_);
    std::wstring status = L"原图 " + std::to_wstring(imgW_) + L"×" + std::to_wstring(imgH_)
        + L"   选区 " + std::to_wstring(cw) + L"×" + std::to_wstring(ch)
        + L"   offset (" + std::to_wstring(offsetX_) + L"," + std::to_wstring(offsetY_) + L")";
    if (preview.ok && !IsFullImageCrop(preview.rect, imgW_, imgH_)) {
        status += L" → (" + std::to_wstring(preview.offsetX) + L","
            + std::to_wstring(preview.offsetY) + L")";
    }
    SelectObject(mem, bodyFont_);
    DrawTextIn(mem, status, StatusRect(), kText, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (!confirmEnabled_ && preview.reject == CropOffsetReject::MinSide) {
        RECT whyRc = StatusRect();
        whyRc.top = whyRc.bottom - S(22);
        DrawTextIn(mem, L"裁切区域过小（边长至少 8 像素）。", whyRc,
            RGB(180, 40, 40), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    // Footer
    FillRectColor(mem, FooterRect(), kPanel);
    DrawOutlineButton(mem, ResetBtnRect(), L"重置选区", hoverReset_);
    DrawGreenButton(mem, ConfirmBtnRect(), L"确认", hoverConfirm_, confirmEnabled_);
    DrawOutlineButton(mem, CancelBtnRect(), L"取消", hoverCancel_);

    BitBlt(windowDc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(canvas);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
}
