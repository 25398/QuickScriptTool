#pragma once
// ──────────────────────────────────────────────────────────────────
// ocr_install_dialog.h — 文字识别插件安装对话框
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <atomic>
#include <string>

#include "config.h"

/// 文字识别插件安装对话框（450×500，模态，可拖动）
class OcrInstallDialog {
public:
    /// @return 用户是否成功完成安装/修复
    bool Show(HWND owner, bool repairMode = false);

private:
    static constexpr int kDialogW = 450;
    static constexpr int kDialogH = 500;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void CleanupGdi();
    void Paint();
    void DrawInstallButton(HDC hdc, const RECT& rc);
    void DrawProgressBar(HDC hdc);
    void DrawCloseButton(HDC hdc);
    bool HitInstallButton(int x, int y) const;
    bool HitCloseButton(int x, int y) const;
    bool HitTitleBar(int x, int y) const;
    void BeginInstall();
    void OnInstallProgress(int percent, const std::wstring& status);
    void OnInstallFinished(bool ok, const std::wstring& message);

    RECT TitleBarRect() const { return RECT{0, 0, kDialogW, kTitleH}; }
    RECT CloseRect() const { return RECT{kDialogW - kCloseBtnW, 0, kDialogW, kTitleH}; }
    RECT DescBoxRect() const { return RECT{28, 156, kDialogW - 28, 278}; }
    RECT ProgressRect() const { return RECT{40, 358, kDialogW - 40, 390}; }
    RECT InstallBtnRect() const { return RECT{90, 418, kDialogW - 90, 478}; }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT nameFont_ = nullptr;
    HFONT btnFont_ = nullptr;
    HFONT closeFont_ = nullptr;
    bool repairMode_ = false;
    bool done_ = false;
    bool accepted_ = false;
    bool installing_ = false;
    bool hoverInstall_ = false;
    bool hoverClose_ = false;
    int progress_ = 0;
    std::wstring statusText_;
    std::wstring resultMessage_;
    std::atomic<bool> installRunning_{false};
};
