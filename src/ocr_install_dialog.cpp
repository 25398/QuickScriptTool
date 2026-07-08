#include "ocr_install_dialog.h"

#include "config.h"
#include "drawing.h"
#include "ocr_engine.h"
#include "taskbar_window.h"

#include <windowsx.h>

#include <algorithm>
#include <thread>

namespace {

constexpr UINT WM_OCR_DLG_PROGRESS = WM_APP + 16;
constexpr UINT WM_OCR_DLG_DONE = WM_APP + 17;

struct InstallDonePayload {
    bool ok = false;
    std::wstring message;
};

}  // namespace

bool OcrInstallDialog::Show(HWND owner, bool repairMode) {
    owner_ = owner;
    repairMode_ = repairMode;
    done_ = false;
    accepted_ = false;
    installing_ = false;
    progress_ = 0;
    statusText_ = repairMode
        ? L"已安装，点击可进行修复/更新..."
        : L"已就绪，点击安装开始下载安装...";
    resultMessage_.clear();
    installRunning_ = false;

    static bool registered = false;
    const wchar_t* clsName = L"QuickScriptOcrInstallDlg";
    if (!registered) {
        WNDCLASSW wc{};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &OcrInstallDialog::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = clsName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
        registered = true;
    }

    RECT ownerRc{};
    GetWindowRect(owner, &ownerRc);
    const int x = ownerRc.left + ((ownerRc.right - ownerRc.left) - kDialogW) / 2;
    const int y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - kDialogH) / 2;

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST,
        clsName, L"", WS_POPUP,
        x, y, kDialogW, kDialogH,
        owner, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    ApplyTaskbarWindowStyle(hwnd_, repairMode ? L"文字识别插件修复" : L"文字识别插件安装");
    outerShadow_.Attach(hwnd_);

    EnableWindow(owner, FALSE);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    SetForegroundWindow(hwnd_);

    MSG msg{};
    while (!done_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return accepted_;
}

LRESULT CALLBACK OcrInstallDialog::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    OcrInstallDialog* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<OcrInstallDialog*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return TRUE;
    }
    self = reinterpret_cast<OcrInstallDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT OcrInstallDialog::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        titleFont_ = CreateFontW(22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        bodyFont_ = CreateFontW(23, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        nameFont_ = CreateFontW(26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        btnFont_ = CreateFontW(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        closeFont_ = CreateFontW(32, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            kUiFontQuality, DEFAULT_PITCH, L"Microsoft YaHei UI");
        return 0;
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        if (HitCloseButton(pt.x, pt.y)) return HTCLIENT;
        if (HitTitleBar(pt.x, pt.y)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        const bool hoverInstall = HitInstallButton(x, y) && !installing_;
        const bool hoverClose = HitCloseButton(x, y) && !installing_;
        if (hoverInstall != hoverInstall_ || hoverClose != hoverClose_) {
            hoverInstall_ = hoverInstall;
            hoverClose_ = hoverClose;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (HitInstallButton(x, y) || HitCloseButton(x, y)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
        } else if (HitTitleBar(x, y)) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp);
        const int y = GET_Y_LPARAM(lp);
        if (installing_) return 0;
        if (HitCloseButton(x, y)) {
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        }
        if (HitInstallButton(x, y)) {
            if (progress_ >= 100 && accepted_) {
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            BeginInstall();
            return 0;
        }
        return 0;
    }
    case WM_OCR_DLG_PROGRESS:
        OnInstallProgress(static_cast<int>(wp), *reinterpret_cast<std::wstring*>(lp));
        delete reinterpret_cast<std::wstring*>(lp);
        return 0;
    case WM_OCR_DLG_DONE: {
        auto* payload = reinterpret_cast<InstallDonePayload*>(lp);
        OnInstallFinished(payload->ok, payload->message);
        delete payload;
        return 0;
    }
    case WM_CLOSE:
        if (!installing_) {
            done_ = true;
            DestroyWindow(hwnd_);
        }
        return 0;
    case WM_DESTROY:
        done_ = true;
        CleanupGdi();
        return 0;
    default:
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
}

void OcrInstallDialog::CleanupGdi() {
    if (titleFont_) DeleteObject(titleFont_);
    if (bodyFont_) DeleteObject(bodyFont_);
    if (nameFont_) DeleteObject(nameFont_);
    if (btnFont_) DeleteObject(btnFont_);
    if (closeFont_) DeleteObject(closeFont_);
    titleFont_ = bodyFont_ = nameFont_ = btnFont_ = closeFont_ = nullptr;
}

void OcrInstallDialog::DrawCloseButton(HDC hdc) {
    RECT close = CloseRect();
    if (hoverClose_) {
        FillRectColor(hdc, close, RGB(90, 190, 125));
    }
    SelectObject(hdc, closeFont_);
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextW(hdc, L"×", -1, &close, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void OcrInstallDialog::Paint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT client{};
    GetClientRect(hwnd_, &client);
    FillRectColor(hdc, client, kWhite);

    RECT title = TitleBarRect();
    FillRectColor(hdc, title, kMainGreen);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    HGDIOBJ oldFont = SelectObject(hdc, titleFont_);
    DrawTextW(hdc, L"  鼠大侠-插件安装", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawCloseButton(hdc);

    SelectObject(hdc, bodyFont_);
    SetTextColor(hdc, RGB(50, 50, 50));
    RECT labelRc{28, 52, kDialogW - 28, 84};
    DrawTextW(hdc, L"安装插件:", -1, &labelRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, nameFont_);
    SetTextColor(hdc, kMainGreen);
    RECT nameRc{28, 88, kDialogW - 28, 126};
    DrawTextW(hdc, L"文字识别(PaddleOCR)", -1, &nameRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, bodyFont_);
    SetTextColor(hdc, RGB(50, 50, 50));
    RECT descLabelRc{28, 132, kDialogW - 28, 156};
    DrawTextW(hdc, L"插件说明:", -1, &descLabelRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT descBox = DescBoxRect();
    FillRectColor(hdc, descBox, RGB(255, 255, 255));
    DrawBorderRect(hdc, descBox, kComboBorderGray);
    RECT descTextRc{descBox.left + 14, descBox.top + 12, descBox.right - 14, descBox.bottom - 12};
    const wchar_t* desc =
        L"此插件支持中文（简/繁）、英文等多种语言文字识别，识别速度快、准确度高。"
        L"首次安装将自动安装 Python 3.12 并下载 OCR 依赖，请保持联网并耐心等待。";
    DrawTextW(hdc, desc, -1, &descTextRc, DT_LEFT | DT_WORDBREAK);

    SelectObject(hdc, bodyFont_);
    SetTextColor(hdc, RGB(80, 80, 80));
    RECT statusRc{28, 296, kDialogW - 28, 346};
    DrawTextW(hdc, statusText_.c_str(), -1, &statusRc, DT_CENTER | DT_VCENTER | DT_WORDBREAK);

    DrawProgressBar(hdc);

    if (!installing_ || progress_ >= 100) {
        RECT installRc = InstallBtnRect();
        DrawInstallButton(hdc, installRc);
    } else {
        RECT installRc = InstallBtnRect();
        HBRUSH disabled = CreateSolidBrush(RGB(180, 220, 195));
        HGDIOBJ oldBrush = SelectObject(hdc, disabled);
        HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
        RoundRect(hdc, installRc.left, installRc.top, installRc.right, installRc.bottom, 8, 8);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(disabled);
        SetTextColor(hdc, RGB(255, 255, 255));
        SelectObject(hdc, btnFont_);
        DrawTextW(hdc, L"安装中...", -1, &installRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(hdc, oldFont);
    EndPaint(hwnd_, &ps);
}

void OcrInstallDialog::DrawProgressBar(HDC hdc) {
    const RECT track = ProgressRect();
    HBRUSH trackBrush = CreateSolidBrush(RGB(230, 230, 230));
    HPEN trackPen = CreatePen(PS_SOLID, 1, RGB(210, 210, 210));
    HGDIOBJ oldBrush = SelectObject(hdc, trackBrush);
    HGDIOBJ oldPen = SelectObject(hdc, trackPen);
    RoundRect(hdc, track.left, track.top, track.right, track.bottom, 12, 12);

    if (progress_ > 0) {
        const int trackW = track.right - track.left - 6;
        const int fillW = std::max(12, trackW * progress_ / 100);
        RECT fillRc{track.left + 3, track.top + 3, track.left + 3 + fillW, track.bottom - 3};
        HBRUSH fillBrush = CreateSolidBrush(kMainGreen);
        SelectObject(hdc, fillBrush);
        SelectObject(hdc, GetStockObject(NULL_PEN));
        RoundRect(hdc, fillRc.left, fillRc.top, fillRc.right, fillRc.bottom, 10, 10);
        DeleteObject(fillBrush);
    }

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(trackPen);
    DeleteObject(trackBrush);
}

void OcrInstallDialog::DrawInstallButton(HDC hdc, const RECT& rc) {
    const COLORREF fill = hoverInstall_ ? kDarkGreen : kMainGreen;
    HBRUSH brush = CreateSolidBrush(fill);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 8, 8);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(brush);

    SetTextColor(hdc, RGB(255, 255, 255));
    HGDIOBJ oldFont = SelectObject(hdc, btnFont_);
    const wchar_t* label = repairMode_ ? L"修复/更新" : L"安装插件";
    if (progress_ >= 100 && accepted_) label = L"完成";
    RECT textRc = rc;
    DrawTextW(hdc, label, -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
}

bool OcrInstallDialog::HitInstallButton(int x, int y) const {
    const RECT rc = InstallBtnRect();
    return x >= rc.left && x <= rc.right && y >= rc.top && y <= rc.bottom;
}

bool OcrInstallDialog::HitCloseButton(int x, int y) const {
    const RECT rc = CloseRect();
    return x >= rc.left && x <= rc.right && y >= rc.top && y <= rc.bottom;
}

bool OcrInstallDialog::HitTitleBar(int x, int y) const {
    return y >= 0 && y < kTitleH && !HitCloseButton(x, y);
}

void OcrInstallDialog::BeginInstall() {
    if (installRunning_.exchange(true)) return;
    installing_ = true;
    progress_ = 0;
    statusText_ = L"正在准备安装...";
    InvalidateRect(hwnd_, nullptr, FALSE);

    OcrInstallDialog* self = this;
    std::thread([self]() {
        std::wstring message;
        const bool ok = RunOcrInstall(message, [self](int percent, const std::wstring& status) {
            auto* statusCopy = new std::wstring(status);
            PostMessageW(self->hwnd_, WM_OCR_DLG_PROGRESS, static_cast<WPARAM>(percent), reinterpret_cast<LPARAM>(statusCopy));
        });
        auto* payload = new InstallDonePayload{ok, message};
        PostMessageW(self->hwnd_, WM_OCR_DLG_DONE, 0, reinterpret_cast<LPARAM>(payload));
    }).detach();
}

void OcrInstallDialog::OnInstallProgress(int percent, const std::wstring& status) {
    progress_ = std::clamp(percent, 0, 100);
    statusText_ = status;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void OcrInstallDialog::OnInstallFinished(bool ok, const std::wstring& message) {
    installRunning_ = false;
    installing_ = false;
    resultMessage_ = message;
    accepted_ = ok;
    if (ok) {
        progress_ = 100;
        statusText_ = message.empty() ? L"安装完成" : message;
    } else {
        progress_ = 0;
        statusText_ = message.empty() ? L"安装失败，请重试" : message;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}
