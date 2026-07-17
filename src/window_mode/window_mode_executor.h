#pragma once

#include "window_mode_session.h"
#include "window_mode_types.h"

#include "coord_space.h"
#include "image_match.h"
#include "ocr_engine.h"
#include "script_types.h"

#include <atomic>
#include <functional>
#include <string>

namespace windowmode {

struct BeginRunOptions {
    bool launchTarget = true;
    const std::atomic_bool* cancelFlag = nullptr;
    std::wstring launchSearchDir;
};

class WindowModeExecutor {
public:
    WindowModeExecutor() = default;

    static bool CheckRunHealth(const WindowModeScriptConfig& config, std::wstring& err);

    bool BeginRun(const WindowModeScriptConfig& config, std::wstring& err,
        BeginRunOptions options = {});
    void EndRun();
    bool IsActive() const { return active_; }
    bool UsesBackgroundWindow() const;

    void SetCoordMeta(const CoordMeta& meta) { coordMeta_ = meta; }
    const CoordMeta& GetCoordMeta() const { return coordMeta_; }

    HWND TargetHwnd() const;
    WindowModeHealth Health() const;

    bool RefreshTarget(std::wstring& err);

    void MoveMouseClient(int cx, int cy, int randomX, int randomY,
        const std::function<int(int)>& randomInt);
    void PostMouseButtonAtClient(int cx, int cy, MouseButtonType button, bool down);
    void PostMouseClickAtClient(int cx, int cy, MouseButtonType button);
    void PostKeyToTarget(UINT vk, bool down);
    void PostScrollWheelAtClient(int cx, int cy, int steps, bool vertical, bool positive);
    void SendQuickInputToTarget(const std::wstring& text, double charInterval);

    bool ResolveClientSearchRect(const ScriptAction& a, int& x1, int& y1, int& x2, int& y2) const;
    bool MapClientRect(int cx1, int cy1, int cx2, int cy2,
        int& sx1, int& sy1, int& sx2, int& sy2) const;

    ImageMatchOutput FindImageClient(const ScriptAction& a,
        HBITMAP lockedBmp, int lockX, int lockY);

    bool LockWindowCapture(HBITMAP& outBmp, int& outX, int& outY);
    HBITMAP CaptureScreenRegionFromWindow(int sx1, int sy1, int sx2, int sy2,
        HBITMAP lockedBmp, int lockX, int lockY);
    bool ResolveAiScreenRect(const ScriptAction& a, int& sx1, int& sy1, int& sx2, int& sy2,
        HBITMAP lockedBmp, int lockX, int lockY);
    OcrEngineOutput RunOcrOnClientRegion(const ScriptAction& a,
        HBITMAP lockedBmp, int lockX, int lockY);
    bool GetCursorClientPos(int& cx, int& cy) const;

    WindowModeSession& Session() { return session_; }
    const WindowModeSession& Session() const { return session_; }

    /// 诊断用：复现 FindImageClient 的 EnsureTargetReady + VisionPrep + Capture，不写文件。
    struct VisionPipelineDiag {
        bool ensureReadyOk = false;
        bool prepReady = false;
        bool captureOk = false;
        bool captureBlank = false;
        int captureW = 0;
        int captureH = 0;
        std::wstring ensureErr;
    };
    VisionPipelineDiag DiagnoseVisionPipeline();

private:
    bool MoveToScreen(int sx, int sy);
    bool EnsureTargetReady(std::wstring& err);
    bool EnsureTargetBound(std::wstring& err);
    /// Bind / quietly restore for message injection — never expands macro-desktop windows.
    bool PrepareSoftInput(std::wstring& err);
    bool PrepareVisionCapture();
    bool ResolveOcrScreenRect(const ScriptAction& a, int& sx1, int& sy1, int& sx2, int& sy2,
        HBITMAP lockedBmp, int lockX, int lockY);
    bool UsesClientCoords() const;
    HWND CaptureTargetHwnd() const;
    HWND VisionCaptureHwnd() const;

    WindowModeSession session_;
    bool active_ = false;
    const std::atomic_bool* cancelFlag_ = nullptr;
    CoordMeta coordMeta_{};
};

}  // namespace windowmode
