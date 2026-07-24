#include "ocr_install_dialog.h"

#include "config.h"
#include "drawing.h"
#include "ocr_engine.h"
#include "render_context.h"
#include "scheduled_task_ui.h"
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
    hoverInstall_ = false;
    hoverClose_ = false;
    trackingMouse_ = false;

    static bool registered = false;
    const wchar_t* clsName = L"QuickScriptOcrInstallDlg";
    if (!registered) {
        WNDCLASSW wc{};
        // 不用 CS_HREDRAW|CS_VREDRAW，避免尺寸/移动时整窗重绘闪烁
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
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            done_ = true;
            break;
        }
        if (!StModalMessageForDialog(msg, hwnd_, nullptr, nullptr, owner)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StDiscardSpuriousInputAfterModal(owner);
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
        if (!trackingMouse_) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd_;
            trackingMouse_ = TrackMouseEvent(&tme) != FALSE;
        }
        UpdateHover(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        if (hoverInstall_ || hoverClose_) {
            hoverInstall_ = false;
            hoverClose_ = false;
            InvalidateHoverTargets();
        }
        return 0;
    case WM_SETCURSOR: {
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            if ((!installing_ && HitInstallButton(pt.x, pt.y)) || HitCloseButton(pt.x, pt.y)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;
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
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void OcrInstallDialog::CleanupGdi() {
    if (titleFont_) DeleteObject(titleFont_);
    if (bodyFont_) DeleteObject(bodyFont_);
    if (nameFont_) DeleteObject(nameFont_);
    if (btnFont_) DeleteObject(btnFont_);
    if (closeFont_) DeleteObject(closeFont_);
    titleFont_ = bodyFont_ = nameFont_ = btnFont_ = closeFont_ = nullptr;
}

void OcrInstallDialog::InvalidateHoverTargets() {
    RECT close = CloseRect();
    RECT install = InstallBtnRect();
    InvalidateRect(hwnd_, &close, FALSE);
    InvalidateRect(hwnd_, &install, FALSE);
}

void OcrInstallDialog::UpdateHover(int x, int y) {
    const bool hoverInstall = HitInstallButton(x, y) && !installing_;
    const bool hoverClose = HitCloseButton(x, y) && !installing_;
    if (hoverInstall == hoverInstall_ && hoverClose == hoverClose_) return;
    hoverInstall_ = hoverInstall;
    hoverClose_ = hoverClose;
    InvalidateHoverTargets();
}

void OcrInstallDialog::Paint() {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd_, &ps);
    RenderBatchScope batch(hdc);
    RECT client{};
    GetClientRect(hwnd_, &client);
    FillRectColor(hdc, client, kWhite);

    const wchar_t* title = repairMode_ ? L"鼠大侠-插件修复" : L"鼠大侠-插件安装";
    StDrawTitleBar(hdc, titleFont_, closeFont_, kDialogW, kTitleH, title,
        hoverClose_, CloseRect());

    SelectObject(hdc, bodyFont_);
    DrawTextIn(hdc, L"安装插件:", RECT{28, 52, kDialogW - 28, 84}, kText,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, nameFont_);
    DrawTextIn(hdc, L"文字识别(PaddleOCR)", RECT{28, 88, kDialogW - 28, 126}, kMainGreen,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, bodyFont_);
    DrawTextIn(hdc, L"插件说明:", RECT{28, 132, kDialogW - 28, 156}, kText,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT descBox = DescBoxRect();
    FillRectColor(hdc, descBox, kWhite);
    DrawBorderRect(hdc, descBox, kComboBorderGray);
    RECT descTextRc{descBox.left + 14, descBox.top + 12, descBox.right - 14, descBox.bottom - 12};
    const wchar_t* desc =
        L"此插件支持中文（简/繁）、英文等多种语言文字识别，识别速度快、准确度高。"
        L"首次安装将自动安装 Python 3.12 并下载 OCR 依赖，请保持联网并耐心等待。";
    DrawTextIn(hdc, desc, descTextRc, kText, DT_LEFT | DT_WORDBREAK);

    SelectObject(hdc, bodyFont_);
    DrawTextIn(hdc, statusText_, RECT{28, 296, kDialogW - 28, 346}, kSecondaryText,
        DT_CENTER | DT_VCENTER | DT_WORDBREAK);

    DrawProgressBar(hdc);

    RECT installRc = InstallBtnRect();
    if (installing_ && progress_ < 100) {
        StDrawGreenButton(hdc, btnFont_, installRc, L"安装中...", false, false);
    } else {
        const wchar_t* label = repairMode_ ? L"修复/更新" : L"安装插件";
        if (progress_ >= 100 && accepted_) label = L"完成";
        StDrawGreenButton(hdc, btnFont_, installRc, label, hoverInstall_, true);
    }

    batch.End();
    EndPaint(hwnd_, &ps);
}

void OcrInstallDialog::DrawProgressBar(HDC hdc) {
    const RECT track = ProgressRect();
    FillRoundRectColor(hdc, track, RGB(230, 230, 230), 12);
    DrawBorderRoundRect(hdc, track, RGB(210, 210, 210), 12);

    if (progress_ > 0) {
        const int trackW = track.right - track.left - 6;
        const int fillW = std::max(12, trackW * progress_ / 100);
        RECT fillRc{track.left + 3, track.top + 3, track.left + 3 + fillW, track.bottom - 3};
        FillRoundRectColor(hdc, fillRc, kMainGreen, 10);
    }
}

bool OcrInstallDialog::HitInstallButton(int x, int y) const {
    return StPtIn(InstallBtnRect(), x, y);
}

bool OcrInstallDialog::HitCloseButton(int x, int y) const {
    return StPtIn(CloseRect(), x, y);
}

bool OcrInstallDialog::HitTitleBar(int x, int y) const {
    return y >= 0 && y < kTitleH && !HitCloseButton(x, y);
}

void OcrInstallDialog::BeginInstall() {
    if (installRunning_.exchange(true)) return;
    installing_ = true;
    progress_ = 0;
    statusText_ = L"正在准备安装...";
    hoverInstall_ = false;
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
