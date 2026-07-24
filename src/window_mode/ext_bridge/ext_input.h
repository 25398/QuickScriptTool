#pragma once

#include "script_types.h"

#include <windows.h>

#include <cstdint>
#include <string>

namespace windowmode {

/// 经配套扩展（chrome.debugger）投递键鼠，API 对齐 CdpInputSession。
class ExtInputSession {
public:
    ExtInputSession() = default;
    ~ExtInputSession();

    ExtInputSession(const ExtInputSession&) = delete;
    ExtInputSession& operator=(const ExtInputSession&) = delete;

    bool EnsureReady(const std::wstring& titleHint, std::wstring& err);
    /// 带绑定 HWND 几何/桌面偏好，避免同名标签键鼠打到同类窗。
    bool EnsureReady(const std::wstring& titleHint, HWND boundTop, std::wstring& err);
    /// 重测壳页 iframe 矩形与内容尺寸（调用方须保证窗口已展开；本函数不碰 Win32 窗口）。
    bool RefreshLayout(std::wstring& err);
    void Disconnect();
    bool IsConnected() const { return attached_; }

    /// 扩展版本号（attach 时写入），如 "1.0.21"。
    const std::string& ExtensionVersion() const { return extVersion_; }
    /// v1.0.19+：iframe canvas 截图（不断桥）。
    bool SupportsSafeExtScreenshot() const;
    /// v1.0.21+：扩展视觉协议（vision/screenshot + lifecycle wake），宿主找图禁止 Win32 展开。
    bool SupportsExtVision() const;
    /// v1.1.9+：layout/清戳记用 scripting，禁止 debugger soft-swap（否则 MV3 断桥）。
    bool SupportsStableBridgeApi() const;
    /// attach/layout 后已有可用 iframe CSS 矩形。
    bool HasValidIframeLayout() const;
    int ContentW() const { return contentW_; }
    int ContentH() const { return contentH_; }

    /// 壳页 iframe 在 CSS 像素中的位置/尺寸（attach/layout 写入）。
    void GetIframeCssRect(int& x, int& y, int& w, int& h) const {
        x = iframeCssX_;
        y = iframeCssY_;
        w = iframeCssW_;
        h = iframeCssH_;
    }
    void GetPageCssSize(int& w, int& h) const {
        w = pageCssW_;
        h = pageCssH_;
    }
    /// 壳页 devicePixelRatio（attach/layout 写入）；用于统一 surface=pageCss×dpr。
    double DevicePixelRatio() const { return dpr_ > 0.1 ? dpr_ : 1.5; }

    /// 找图/截图用的客户区尺寸，供扩展把宿主像素映射到 iframe 文档坐标。
    void SetSurfaceSize(int w, int h) { surfaceW_ = w; surfaceH_ = h; }
    int SurfaceW() const { return surfaceW_; }
    int SurfaceH() const { return surfaceH_; }

    /// 经扩展截 iframe canvas，并合成到 clientW×clientH（与 Win32 模板同坐标空间）。
    /// 不还原/展开 Win32 窗口。成功时 *outBmp 为调用方所有，须 DeleteObject。
    bool CaptureScreenshotForClientMatch(int clientW, int clientH,
        HBITMAP* outBmp, int* outW, int* outH, std::wstring& err);
    /// 扩展视觉找图：优先合成到客户区；合成不可用时回退 canvas 像素空间（*outCanvasSpace=true）。
    bool CaptureScreenshotForVisionMatch(int clientW, int clientH,
        HBITMAP* outBmp, int* outW, int* outH, bool* outCanvasSpace, std::wstring& err);

    bool KeyEvent(UINT vk, bool down, std::wstring& err);
    bool MouseMove(int cx, int cy, std::wstring& err);
    bool MouseButton(int cx, int cy, MouseButtonType button, bool down, std::wstring& err);
    bool MouseClick(int cx, int cy, MouseButtonType button, std::wstring& err);
    bool Scroll(int cx, int cy, int steps, bool vertical, bool positive, std::wstring& err);
    bool InsertText(const std::wstring& text, std::wstring& err);

private:
    bool CallCdp(const std::string& method, const std::string& paramsJson, std::wstring& err,
        bool allowReattach = true);
    /// 扩展 v1.0.13+：move/click/down/up，并回传 host→iframe 映射供诊断。
    bool CallMouse(const char* action, int cx, int cy, MouseButtonType button, std::wstring& err,
        bool allowReattach = true);
    /// debugger 中途脱落（找图最小化等）时重新 attach。
    bool RecoverAttach(std::wstring& err);
    static bool IsDetachError(const std::string& result, const std::wstring& err);
    std::string AppendSurfaceFields(std::string paramsJson) const;
    bool CaptureScreenshot(HBITMAP* outBmp, int* outW, int* outH, std::wstring& err);
    bool DispatchKeyEventOnly(UINT vk, bool down, std::wstring& err);
    void ApplyLayoutFields(int ix, int iy, int iw, int ih, int cw, int ch, int pw, int ph);
    bool FinishAttachFromResult(const std::string& result, int tabId, std::wstring& err);

    bool attached_ = false;
    std::wstring titleHint_;
    std::string extVersion_;
    HWND boundTopHwnd_ = nullptr;
    int attachedTabId_ = 0;
    int lastCx_ = 0;
    int lastCy_ = 0;
    int surfaceW_ = 0;
    int surfaceH_ = 0;
    int iframeCssX_ = 0;
    int iframeCssY_ = 0;
    int iframeCssW_ = 0;
    std::string lastShotVia_;
    uint64_t lastVisionJpegHash_ = 0;
    int iframeCssH_ = 0;
    int pageCssW_ = 0;
    int pageCssH_ = 0;
    int contentW_ = 0;
    int contentH_ = 0;
    double dpr_ = 1.5;
};

}  // namespace windowmode
