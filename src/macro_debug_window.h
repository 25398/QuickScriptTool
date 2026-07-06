#pragma once
// ──────────────────────────────────────────────────────────────────
// macro_debug_window.h — 宏调试信息输出窗口
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "image_match.h"
#include "macro_variables.h"
#include "script_types.h"

/// 非模态宏调试信息窗口（绿色标题栏，支持顶置/最小化/关闭）
class MacroDebugWindow {
public:
    void Create(HFONT bodyFont, HFONT titleFont, HFONT closeFont,
                std::function<void()> onClosed = {});
    void Show();
    void Hide();
    void Destroy();
    bool IsCreated() const { return hwnd_ != nullptr; }
    HWND Hwnd() const { return hwnd_; }

    /// 线程安全：可从工作线程调用
    void AppendLog(const std::wstring& text);
    void ClearLog();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void CleanupGdi();
    void Paint();
    void PositionEdit();
    void ApplyTopmost();
    void FlushPendingLogs();
    void AppendLogDirect(const std::wstring& text);
    void ClearLogDirect();
    void CloseByUser();

    void DrawTitleButtons(HDC hdc);
    void DrawPinIcon(HDC hdc, const RECT& rc, bool pinned);

    RECT CloseRect() const;
    RECT MinimizeRect() const;
    RECT PinRect() const;
    int ClientWidth() const;
    bool HitClose(int x, int y) const;
    bool HitMinimize(int x, int y) const;
    bool HitPin(int x, int y) const;
    bool HitTitleBar(int x, int y) const;

    static constexpr int kWindowW = 480;
    static constexpr int kWindowH = 300;
    static constexpr int kContentPad = 8;
    static constexpr UINT WM_DEBUG_APPEND = WM_APP + 18;
    static constexpr UINT WM_DEBUG_CLEAR = WM_APP + 19;

    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT closeFont_ = nullptr;
    bool pinned_ = true;
    bool hoverPin_ = false;
    bool hoverMin_ = false;
    bool hoverClose_ = false;
    std::function<void()> onClosed_;
    std::mutex logMutex_;
    std::vector<std::wstring> pendingLogs_;
};

// ── 调试信息格式化 ────────────────────────────────────────────────

int ActionDebugIndex(const ScriptAction& action);

std::wstring FormatMacroLoopDebug(int loopIndex);

std::wstring FormatGenericActionDebug(const ScriptAction& action);

std::wstring FormatMoveMouseDebug(const ScriptAction& action, int x, int y);

std::wstring FormatFindImageDebug(const ScriptAction& action, const ImageMatchResult& rawMatch);

std::wstring FormatOcrDebug(const ScriptAction& action, const std::wstring& textContent,
                            bool searchFound, const MacroVariableContext& ctx);
