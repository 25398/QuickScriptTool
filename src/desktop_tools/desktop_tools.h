#pragma once
// ──────────────────────────────────────────────────────────────────
// desktop_tools.h — Win32 桌面能力门面（参数进、结果出）
// Web/bridge 只依赖本头；实现调现有 overlay / HotkeyCapture / UAC。
// 归属真值：docs/webview-native-layering.md
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "image_match.h"
#include "match_overlay.h"
#include "utils.h"  // Hotkey

class MacroDebugWindow;

namespace qst::desktop_tools {

/// 藏 Web 壳再开桌面叠层；禁止在各 bridge 分支散落 ShowWindow。
/// 隐藏后会刷消息并短暂 settle，保证随后截屏/找图不含编辑窗。
struct ScopedHideShell {
    HWND hwnd = nullptr;
    bool wasVisible = false;
    explicit ScopedHideShell(HWND shellHwnd);
    ~ScopedHideShell();
    ScopedHideShell(const ScopedHideShell&) = delete;
    ScopedHideShell& operator=(const ScopedHideShell&) = delete;
};

/// AI/找图/截屏/点击前隐藏本进程用户可见窗（主壳、宏调试、AI 助手），避免进画面或挡点击。
/// 析构时按原可见性恢复（SHOWNOACTIVATE，不抢前台）。
struct ScopedHideOwnUiForCapture {
    std::vector<HWND> hwnds;
    std::vector<char> wasVisible; // 0/1，避免 vector<bool>
    std::vector<char> usedCloak;  // 1=DWM cloak（WebView 宿主），0=SW_HIDE
    /// mainUi：Web 壳或 GDI 主窗（可为 null）；另自动查找调试窗/助手窗
    explicit ScopedHideOwnUiForCapture(HWND mainUi);
    ~ScopedHideOwnUiForCapture();
    ScopedHideOwnUiForCapture(const ScopedHideOwnUiForCapture&) = delete;
    ScopedHideOwnUiForCapture& operator=(const ScopedHideOwnUiForCapture&) = delete;
};

struct OpError {
    bool ok = false;
    std::string detail; // utf-8；cancelled / 中文提示
};

struct ScreenRegionResult : OpError {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0; // 屏幕坐标
};

struct DragPickResult : OpError {
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    double durationSec = 0.0;
};

struct TemplateCaptureResult : OpError {
    std::wstring imagePath;     // 相对/存储路径（ImagePathForJson）
    std::wstring resolvedPath;  // 绝对路径
};

struct FindImageMatchParams {
    std::wstring imagePath; // 已 Resolve 的绝对路径，或待 Resolve 的存储路径
    std::string modeUtf8;   // test | offset | region | regionBySize | offsetBySize
    int searchX1 = 0, searchY1 = 0, searchX2 = 0, searchY2 = 0;
    int searchFullScreen = 0;
    double matchThreshold = 65.0; // 1–100（≤1 时按比例×100）
    int perfectMatch = 0;
    double imageScaleMin = 1.0;
    double imageScaleMax = 1.0;
    int syntheticW = 0; // regionBySize / offsetBySize：合成锚框宽
    int syntheticH = 0; // regionBySize / offsetBySize：合成锚框高
    int syntheticUseScreen = 0; // 1=用虚拟屏尺寸当锚框（保存图片全图）
    int maxMatches = 20; // 测试叠层最多画几处（一图多处用）；offset/region 叠层会强制 1
    int constrainToWindow = 0; // 1=窗口/后台窗口模式：在目标窗口客户区内搜，忽略绝对选取区
    std::wstring windowClassName;
    std::wstring windowTitle;
    std::wstring targetExePath;
};

struct FindImageMatchResult : OpError {
    std::string modeUtf8;
    std::wstring resolvedPath;
    int offsetX = 0, offsetY = 0;
    bool regionValid = false;
    int regionX1 = 0, regionY1 = 0, regionX2 = 0, regionY2 = 0;
    int matchTopLeftX = 0, matchTopLeftY = 0;
    bool found = false;
    int matchCount = 0;
    double bestScore = 0.0;
};

struct FindImageCropResult : OpError {
    bool unchanged = false;
    std::wstring imagePath;
    int offsetX = 0, offsetY = 0;
};

struct CrosshairPickResult : OpError {
    std::string modeUtf8;
    std::string pickJson; // 已是 JSON 对象字面量（不含外层 type）
};

/// 准星点选窗口目标（编辑器「准星找程序/绑定」等）；SelectOnStartup 改为取前台聚焦窗，不走此 API。
struct WindowTargetResult : OpError {
    int pickX = 0, pickY = 0;
    std::wstring windowTitle;
    std::wstring windowClassName;
    std::wstring childWindowClassName;
    std::wstring processPath;
    std::wstring documentPath;
};

struct ActionKeyResult : OpError {
    UINT keyVk = 0;
    std::wstring keyText;
    bool holdLeftCtrl = false;
    bool holdLeftAlt = false;
    bool holdLeftShift = false;
    bool holdLeftWin = false;
};

struct HotkeyCaptureParams {
    Hotkey editing{};
    bool scriptHotkey = false;    // 脚本热键 UI
    bool globalStartStop = false; // 全局启停
    double holdThresholdSeconds = 0.2;
    /// 捕获前/后：接 Engine Begin/EndHotkeyCaptureRelease（可选）
    std::function<void()> beginRelease;
    std::function<void()> endRelease;
};

struct HotkeyCaptureResult : OpError {
    Hotkey hotkey{};
};

struct TestOcrParams {
    std::string modeUtf8; // test | offset
    int ocrRegionByImage = 0;
    int ocrDigitsOnly = 0;
    int searchFullScreen = 0;
    int ocrResultMode = 0;
    int searchX1 = 0, searchY1 = 0, searchX2 = 0, searchY2 = 0;
    int imageRegionX1 = 0, imageRegionY1 = 0, imageRegionX2 = 0, imageRegionY2 = 0;
    int constrainToWindow = 0;
    std::wstring windowClassName;
    std::wstring windowTitle;
    std::wstring targetExePath;
    double matchThreshold = 65.0;
    int perfectMatch = 0;
    double imageScaleMin = 1.0;
    double imageScaleMax = 1.0;
    std::wstring imagePath;
    std::wstring ocrSearchText;
};

struct TestOcrResult : OpError {
    std::string modeUtf8;
    bool needInstall = false;
    std::wstring text;
    int offsetX = 0, offsetY = 0;
};

using DriverProgressFn = std::function<void(int percent, int step, const char* statusUtf8)>;

struct InstallDriverResult : OpError {
    std::string kindUtf8;
    /// 旧安装脚本曾返回 10/11；新产品安装器不再改启动配置，此标志应始终为 false
    bool rebootRequired = false;
    bool fwReboot = false;
    bool uninstalled = false;
};

/// 虚拟 HID 安装状态（只读查询，无需提权；设置弹窗打开时即时刷新）
struct VhidInstallStatus {
    bool hvciEnabled = false;           // 内存完整性（HVCI）开启
    bool rebootPending = false;         // 已预约开机续装（计划任务 QstVHidFinishInstall 存在）
    bool pendingSb = false;             // 已预约续装且等待用户在 BIOS 关闭 Secure Boot
    bool driverReady = false;           // 设备接口可探测（服务 + 设备就绪）
    bool installScriptPresent = false;  // 发版包内含安装脚本
    bool packagePresent = false;         // package\*.sys 已在本地（可直接装；否则点安装会先下载）
    bool hidDllPresent = false;          // interception.dll 在 exe 旁或 LocalAppData
    bool driverNeedsUpdate = false;     // 已装但 INF/ACL 版本过旧，需重新安装
    int lastExitCode = -1;              // install_log.txt 最近 EXIT_CODE；-1 = 无记录
};

struct BrowsePathResult : OpError {
    std::wstring path;
};

// ── API（唯一入口）──────────────────────────────────────────────

ScreenRegionResult PickScreenRegion(HWND owner, const wchar_t* title = L"选取区域");
DragPickResult PickScreenDrag(HWND owner);
DragPickResult PickTemplateDrag(HWND owner, const std::wstring& imagePath);
TemplateCaptureResult CaptureTemplateScreenshot(HWND owner, const wchar_t* title = L"屏幕截图");
FindImageMatchResult FindImageMatch(HWND owner, const FindImageMatchParams& params);
/// Web 选区裁切：图像像素矩形 (x,y,w,h)。产品路径必须带 rect。
FindImageCropResult FindImageCropRect(const std::wstring& imagePathOrStored,
    int offsetX, int offsetY, int cropX, int cropY, int cropW, int cropH);
/// 勿 ScopedHideShell：Crosshair 靠 SetCapture(owner)。
CrosshairPickResult CrosshairPick(HWND owner, const std::string& modeUtf8);
WindowTargetResult PickWindowTarget(HWND owner);
ActionKeyResult CaptureActionKey(HWND owner, const Hotkey& oldValue);
HotkeyCaptureResult CaptureHotkey(HWND owner, const HotkeyCaptureParams& params);
TestOcrResult TestOcr(HWND owner, const TestOcrParams& params);
InstallDriverResult InstallDriver(HWND owner, const std::string& kindUtf8,
    const DriverProgressFn& onProgress = {}, bool uninstall = false);
VhidInstallStatus QueryVhidInstallStatus();
BrowsePathResult BrowsePath(HWND owner, bool executableOnly);
BrowsePathResult PickImageFile(HWND owner);

/// 宏调试输出：GDI 用独立 MacroDebugWindow；Web 壳（SetWebUiEnabled）推 debugWindow.* JSON。
class MacroDebugController {
public:
    void SetWebUiEnabled(bool enabled);
    bool WebUiEnabled() const { return webUi_; }

    void Create(HFONT bodyFont, HFONT titleFont, HFONT closeFont,
                std::function<void()> onClosed = {});
    void Show();
    void Hide();
    /// 用户关独立调试窗：仅清可见标记，不二次 Post hide（壳已 HideWindow）。
    void MarkWebHidden();
    /// poster 晚于引擎首启 Apply 时：若设置要求显示则补推 debugWindow.show
    void FlushPendingWebShow();
    void Destroy();
    bool IsCreated() const;
    HWND Hwnd() const;

    void AppendLog(const std::wstring& text);
    void AppendLogBatch(const std::vector<std::wstring>& lines);
    void ClearLog();
    /// Web 调试日志合并刷出（Timer Queue 回调）
    void FlushWebPendingLogs();

private:
    void PostJson(std::string jsonUtf8) const;
    MacroDebugWindow& Native();

    std::unique_ptr<MacroDebugWindow> native_;
    bool webUi_ = false;
    bool webCreated_ = false;
    bool webVisible_ = false;  // Web 路径：当前是否应对用户可见（Hide 可跳过从未 Show）
    std::function<void()> onClosed_;
    mutable std::mutex webLogMu_;
    std::vector<std::wstring> webPendingLogs_;
    std::atomic<bool> webLogFlushScheduled_{false};
};

/// Shell 安装：与 PostToJs / SetJsPoster 同一通道。
void SetMacroDebugWebPoster(std::function<void(std::string)> poster);

MacroDebugController& MacroDebug();
void DestroyMacroDebug();

/// 打开调试窗：调用方传入 ReloadSettings / ApplyDebugWindowSetting。
void RequestShowDebugWindow(const std::function<void()>& applyDebugWindowSetting);

}  // namespace qst::desktop_tools
