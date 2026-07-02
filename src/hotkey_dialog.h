#pragma once
// ──────────────────────────────────────────────────────────────────
// hotkey_dialog.h — 热键捕获对话框（声明）
// 模态对话框，支持键盘/鼠标按键捕获，用于设置脚本热键和按键动作
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include "config.h"
#include "utils.h"

/// 热键捕获对话框 — 支持键盘和鼠标按键的捕获与配置
/// 用法: HotkeyCapture cap; Hotkey out; cap.Show(owner, old, isScript, out);
class HotkeyCapture {
public:
    HotkeyCapture();
    ~HotkeyCapture();

    /// 显示模态热键捕获对话框
    /// @param owner 父窗口
    /// @param oldValue 旧的热键值（用于重置）
    /// @param scriptHotkey 是否为脚本热键模式（影响 UI 文字）
    /// @param out 输出：用户确认后的新热键值
    /// @param globalStartStop 是否为全局启停热键模式
    /// @return 用户是否确认了选择
    bool Show(HWND owner, const Hotkey& oldValue, bool scriptHotkey,
              Hotkey& out, bool globalStartStop = false);

private:
    enum { kOk = 1, kCancel = 2, kReset = 3, kDelete = 4 };

    // ── 窗口过程 ───────────────────────────────────────────────
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                     WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    // ── GDI 清理 ──────────────────────────────────────────────
    void CleanupGdi();

    // ── 布局矩形 ──────────────────────────────────────────────
    RECT OkRect() const;
    RECT CancelRect() const;
    RECT ResetRect() const;
    RECT DeleteRect() const;
    bool HitButton(int x, int y) const;
    bool PtIn(RECT r, int x, int y) const;

    // ── 控件工厂 ──────────────────────────────────────────────
    HWND MakeStatic(const wchar_t* text, int id, RECT rc,
                    DWORD style, HFONT font);
    void CreateCaptureControls();
    void UpdateCaptureControls();

    // ── 命令处理 ──────────────────────────────────────────────
    bool HandleCommand(int id);
    bool HandleClick(int x, int y);

    // ── 按键捕获 ──────────────────────────────────────────────
    void CaptureKey(UINT vk);

    // ── 控件颜色 ──────────────────────────────────────────────
    LRESULT OnCtlColor(HDC hdc, HWND child);

    // ── 绘制 ──────────────────────────────────────────────────
    void Paint();
    void DrawButton(HDC hdc, RECT rc, const wchar_t* text, bool green);

    // ── 成员变量 ──────────────────────────────────────────────
    HWND hwnd_ = nullptr;
    HFONT font_ = nullptr, valueFont_ = nullptr;
    HBRUSH whiteBrush_ = nullptr, bottomBrush_ = nullptr;
    HBRUSH grayButtonBrush_ = nullptr, greenButtonBrush_ = nullptr;
    HWND promptText_ = nullptr, valueText_ = nullptr;
    HWND resetText_ = nullptr, deleteText_ = nullptr;
    HWND cancelText_ = nullptr, okText_ = nullptr;
    Hotkey old_{}, current_{};
    bool scriptHotkey_ = false, globalStartStop_ = false;
    bool done_ = false, accepted_ = false;
};
