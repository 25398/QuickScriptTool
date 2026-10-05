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
///
/// ★★**通用规则（docs §70.4）：凡是「给模型看的」或「用来做判断的」截图，都必须先构造它。**
///   这不是"顺手好看"：AI 调试窗是**最顶层**的，而且它在**滚动追加模型自己的思考文本** ——
///   漏藏一次，模型就会在自己的放大图里读到自己的独白，然后花整轮去猜"那是什么窗口、
///   要不要点它的最小化按钮"（实测就这么烧掉过一轮 7KB 思考 / 14.6s）。
///   已经藏了的：观察帧（`captureObservationNow` / `runWithOptionalAutoCapture`）、
///   找图模板裁剪、OCR 区域动作、切窗台账、**`zoom` 放大图**、**`ocrProbeText` 落点复核**。
///   ⚠ 加新的截图点（新工具、新复核、新裁剪）时，**先问这一句再写代码**：
///     「这张图会给模型看、或会被本地拿去当判据吗？」是 → 把它包进这个作用域。
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

/// 准星取点的**坐标系选项**（2026-10-05）。
///
/// 背景：默认取到的是**屏幕绝对坐标**，而「后台窗口模式」回放时按**当前**窗口位置换算
/// ⇒ 窗口一动，所有坐标动作整体偏移（用户报障：「后台窗口模式貌似使用的坐标是绝对坐标，
/// 窗口移动后就不能使用了」）。⇒ 在**取点这一步**就把坐标转成目标窗口客户区，
/// 从源头消除这个缺陷。对标 AutoHotkey 的 `CoordMode, Mouse, Client`。
///
/// ⚠ 匹配判据（**路径 + 类名是硬门，标题只作提示**）与引擎侧
///   `ResolveWindowModeSelectMethod` 的 `windowNameIsHintOnly` 语义保持一致 ——
///   标题易变（换文档 / 换标签页 / 游戏换场景），当硬匹配门会枚举不到窗口。
struct CrosshairPickOptions {
    /// true = 按目标窗口客户区取点（`x/y` 返回客户区像素，**允许负值**表示点在窗口外）。
    /// false（默认）= 与旧行为**完全一致**，返回屏幕坐标。
    bool windowClient = false;
    std::wstring windowClassName;   ///< 顶层窗口类名（硬判据）
    std::wstring exePath;           ///< 目标进程完整路径（硬判据，大小写不敏感）
    std::wstring title;             ///< 标题（**仅提示/日志**，不参与匹配）
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
/// ⚠ `opts` 传 nullptr = 旧行为（屏幕坐标），保证向后兼容。
CrosshairPickResult CrosshairPick(HWND owner, const std::string& modeUtf8,
    const CrosshairPickOptions* opts = nullptr);
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
///
/// ★★**同时落一份盘**（`AppDir()\ai_action_debug.log`，见 docs §61）：
///   调试窗是**视图**，不是记录本身。`AppendLog` 原先在「窗没建」时**直接丢弃**
///   ⇒ 用户报的现象**没有任何可读的现场**，排查只能靠猜（这正是加它的原因）。
///   落盘**不依赖** `IsCreated()`；上限 `kMacroDebugLogMaxBytes` 满了轮转一代 `.1`。
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
    /// 清空**窗口**并截断**落盘日志**（「清空日志」在用户眼里是一件事，两个出口都必须清）。
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

/// 落盘日志的**单行上限**与**文件上限**（超过则轮转一代 `.1`）。公开只是为了自检能断言。
constexpr size_t kMacroDebugLogMaxLineChars = 4000;
constexpr unsigned long long kMacroDebugLogMaxBytes = 4ull * 1024ull * 1024ull;
/// 落盘日志路径（`AppDir()\ai_action_debug.log`）。自检与打包/文档都从这里取，别另写字面量。
std::wstring MacroDebugLogFilePath();
/// 直接往落盘日志追加一行（**不经过窗口**：即使调试窗从未创建也照写）。
void AppendMacroDebugLogFile(const std::wstring& text);
/// 截断落盘日志（`ClearLog` 用；也供自检复位）。
void ClearMacroDebugLogFile();

/// Shell 安装：与 PostToJs / SetJsPoster 同一通道。
void SetMacroDebugWebPoster(std::function<void(std::string)> poster);

MacroDebugController& MacroDebug();
void DestroyMacroDebug();

/// 打开调试窗：调用方传入 ReloadSettings / ApplyDebugWindowSetting。
void RequestShowDebugWindow(const std::function<void()>& applyDebugWindowSetting);

/// 录制 / 回放精度诊断行落盘：追加到 `AppDir()\recorder_diag.log`（与 findimage_diag.log
/// 同级同格式，UTF-16LE + 时间戳）。
///
/// 为什么单独落盘：`[鼠标报告]` / `[录制结束]` / `[回放保真]` 这三行是判定「偏差在输入层
/// 还是目标侧」的唯一依据，但它们原先只在**宏调试窗口已打开**时可见，而调试浮窗没有
/// 复制/导出按钮 ⇒ 用户拿不出来，排查无法闭环。落盘后「把三行发我」= 发这个文件。
///
/// 不自己做门控（由调用方决定要不要写）；超限裁剪的行边界规则见
/// `src/recorder_diag_log.h`（纯逻辑，有自检）。
void AppendRecorderDiagLog(const std::wstring& line);

/// 上述日志的**实际**路径（可能因只读安装目录回退到 `%LOCALAPPDATA%`）。
/// 给自检读回、以及给用户排查时确认「文件到底在哪」。
std::wstring RecorderDiagLogPath();

}  // namespace qst::desktop_tools
