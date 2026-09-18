#pragma once
// ──────────────────────────────────────────────────────────────────
// app_settings.h — 应用全局设置结构体
// ──────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string>
#include <vector>

namespace quickscript {

enum class ForegroundInputBackend {
    Software = 0,
    Interception = 1,
    VirtualHid = 2,
};

inline ForegroundInputBackend ClampForegroundInputBackend(int v) {
    if (v < 0 || v > 2) return ForegroundInputBackend::Software;
    return static_cast<ForegroundInputBackend>(v);
}

inline const wchar_t* ForegroundInputBackendName(ForegroundInputBackend b) {
    switch (b) {
    case ForegroundInputBackend::Interception: return L"Interception";
    case ForegroundInputBackend::VirtualHid: return L"VirtualHid";
    case ForegroundInputBackend::Software:
    default: return L"Software";
    }
}

struct ClickTabSettings {
    bool enableRandomInterval = false;
    double randomIntervalMaxSeconds = 0.5;
    bool enablePressReleaseInterval = true;
    double pressReleaseIntervalSeconds = 0.001;
    bool enableCoordinateJitter = false;
    int jitterX = 2;
    int jitterY = 2;
    bool enableFixedCoordinates = false;
    int fixedX = 0;
    int fixedY = 0;
    bool enableClickCountLimit = false;
    int clickCountLimit = 0;
};

struct PlaybackTabSettings {
    bool enablePlaybackCount = false;
    int playbackCount = 1;
    bool enablePlaybackInterval = false;
    double playbackIntervalMinSeconds = 0.5;
    double playbackIntervalMaxSeconds = 1.0;
    bool enableDebugOutputWindow = false;
    bool autoOutputKeyFunctionDebug = true;
    /// 录制时对每次鼠标按下自动截取点击附近模板
    bool recordingClickCaptureEnabled = true;
    /// 模板半边像素（实际约 2N×2N）；读写时 clamp 到代码常量范围
    int recordingClickCaptureHalfSize = 40;
    /// 专业模式：录制页直接播放是否按 playbackSpeed 缩放。极简模式无视此勾选（视为已启用）
    bool enablePlaybackSpeed = false;
    /// 倍速 0.25~4，1=原速。仅录制页直接播放使用；鼠标宏顶层不缩放
    double playbackSpeed = 1.0;
    /// 前台注入后端：Software / Interception / VirtualHid
    ForegroundInputBackend foregroundInputBackend = ForegroundInputBackend::Software;
    /// 兼容旧设置：true 且无 foregroundInputBackend 字段时视为 Interception
    bool enableHidDriverSimulation = false;
    /// 定时任务优先级：0=执行脚本优先 1=定时脚本优先
    int scheduledTaskConflictPolicy = 0;
    /// 脚本中断后自动恢复：排队到当前脚本结束后跑，或插入定时后再从原步骤继续
    bool scheduledTaskAutoResume = false;
    /// 低性能模式（用户反馈「挂机脚本时 CPU 温度到 80°C」后的取舍开关）：
    /// 勾选后优先「少占资源」而不是「最快/最准」——
    ///   · 图像匹配限 1 个 OpenCV 线程（默认按核数 fan-out，4 核瞬间满载升温）
    ///   · 输入时间轴大幅减少自旋（改用高精度定时器等待），代价是注入节奏可有 ~1ms 抖动
    ///   · 回放期间不再提进程/线程优先级、不绑核、不抬全系统定时器分辨率
    ///   · 找图监视（WatchImage 时间模式）最小轮询间隔 50ms → 200ms
    /// 不勾选时行为与之前完全一致。开关在 Load/SaveAppSettings 里同步到进程级原子量，
    /// 保存后立即生效、无需重启。详见 docs/ai-action-exec-optimization.md §19。
    bool lowPerformanceMode = false;
    /// AI 高级加速（默认开）：布局记忆 + 相对网格 + 观察帧省上传/文字索引。
    /// 出问题（点错位置/界面看不懂）时可一键关掉，退回「每步真识图 + 每轮回传整帧」。
    /// 见 src/ai_fast_paths.h。
    bool aiFastPaths = true;
    /// 找图 GPU 加速（OpenCL）：大区域全屏找图走显卡（实测 ~2.9x），区域找图自动走 CPU。
    /// 勾选但机器没有 OpenCL 设备时自动回落 CPU（只写一次调试日志，不报错）。
    /// 与「低性能模式」冲突时低性能模式优先（见 FindImageGpuAccelActive()）。
    bool findImageGpuAccel = false;
};

/// 安装/恢复默认主题：Web 壳为「极光 Arctic」(id=7)；GDI 目录无 Arctic，回退 id=0。
#ifdef QST_WEBVIEW_SHELL
inline constexpr int kDefaultThemeId = 7;
#else
inline constexpr int kDefaultThemeId = 0;
#endif

struct OtherTabSettings {
    bool autoHideMainWindow = true;
    bool playSoundOnStart = true;
    bool playSoundOnEnd = true;
    bool hideBottomRightTip = true;
    bool closeToTray = true;
    bool autoStartOnBoot = false;
    bool resolveImeConflict = false;
    /// 桌面悬浮球；默认显示
    bool showFloatBall = true;
    /// 贴边半露；false=自由悬浮整圆
    bool floatBallDocked = true;
    /// 0=左 1=右 2=上 3=下
    int floatBallEdge = 1;
    /// 沿工作区宽度的球左缘 0~1（自由/上下贴边）
    double floatBallXRatio = 1.0;
    /// 沿工作区高度的球上缘 0~1
    double floatBallYRatio = 0.55;
    /// MONITORINFOEX.szDevice；空=主屏
    std::wstring floatBallMonitorId;
    /// 鼠标宏编辑界面默认视图：code=代码化 visual=可视化
    std::wstring editorDefaultView = L"code";
    /// 可视化：循环体大包裹框（头卡角标不受影响）
    bool visualLoopWrap = true;
    /// 可视化：定义宏 ↔ 运行宏 的虚线调用线
    bool visualBlockCallWires = true;
    /// 可视化：if/else 大包裹框
    bool visualIfWrap = true;
    /// 可视化：定义宏指令块大包裹框
    bool visualBlockWrap = true;
    /// 可视化：找图监视大包裹框
    bool visualWatchWrap = true;
    /// 可视化：goto/跳转连线
    bool visualJumpWires = true;
    /// 可视化：画布点阵网格
    bool visualShowGrid = true;
    /// 可视化：卡片标题中的 ID
    bool visualShowCardId = true;
    /// 编辑器「请选择要添加的宏」排列顺序（动作 type）；空=产品默认顺序
    std::vector<std::wstring> editorActionOrder;
    /// 从添加列表中隐藏的动作 type；空=全部显示。脚本里已有步骤仍可见。
    std::vector<std::wstring> editorHiddenActions;
    /// 编辑器动作目录预设：simple/office/game/all/custom；空=all
    std::wstring editorCatalogPreset;
    /// 「自定义」槽：切换精简/办公/游戏图色/全部时保留；仅在其它预设上改勾选/顺序时覆盖
    std::vector<std::wstring> editorCustomActionOrder;
    std::vector<std::wstring> editorCustomHiddenActions;
    /// 添加列表搜索是否包含已隐藏动作；false=只搜当前目录显示项
    bool editorSearchAllActions = false;
    /// 插入变量列表：隐藏 ctrl:CurLoops 等固定变量
    bool editorHideFixedVars = false;
    /// 插入变量列表：隐藏 .x/.y/.cx 等坐标字段
    bool editorHideCoordVars = false;
    /// 多结果只显示 matchRet[n] 代指，不列出 [0]/[1]/[2]
    bool editorMultiResultPlaceholderOnly = false;
    /// 不启用修改按钮：隐藏「修改」，选中后改参数失焦即写回列表（不立刻存盘）
    bool editorDisableModifyButton = false;
    /// 退出时自动保存：去掉「保存」，×/关软件时写盘；「取消」恢复到打开时
    bool editorAutoSaveOnExit = false;
    /// 启用批量插入：批量勾选多条时，非容器类型点「添加」逐条插入；容器类型仍并入
    bool editorEnableBatchInsert = false;
    /// 长按判定（秒）：热键捕获与运行时按住启停的最小按住时间，须 > 0
    double holdThresholdSeconds = 0.2;
    /// 界面缩放倍率：叠在分辨率自适应之后，相对当前分辨率下的默认大小；默认 1.0，须为正数
    double uiScaleFactor = 1.0;
    int themeId = kDefaultThemeId;
    /// 为 true 时使用 customMain/Accent，忽略预设 themeId 的外观（themeId 仍保留以便取消自定义后回退）
    bool useCustomTheme = false;
    int customMainColor = 0x0063A840;    // COLORREF: RGB(64,168,99)
    int customAccentColor = 0x00489AFF;  // COLORREF: RGB(255,154,72)
    /// 优先 Direct2D 绘制（不可用或创建失败时自动回退 GDI）
    bool preferDirect2D = false;
};

struct WindowModeSettings {
    bool showPreviewThumbnail = true;
    int previewRefreshMs = 500;
    bool blockRunWhenUnhealthy = true;
    /// 仅当后台输入全部失败时，才短暂抢焦点用系统键盘输入（会打扰用户）
    bool allowForegroundInputFallback = false;
    /// 是否允许假焦点 DLL 注入（关闭后窗口模式只走 PostMessage / 必要时假前台，不注入）
    bool enableFakeFocusInjection = true;
    /// 假焦点注入技术（对抗性测试用，对应 windowmode::inject::Technique）：
    /// 0=经典远线程 1=NtCreateThreadEx 2=APC 3=线程劫持
    /// 4=手动映射 5=XOR 手动映射 6=窗口消息钩子
    /// 7=手动映射+线程劫持 8=XOR+手动映射+线程劫持
    /// 9=映像节映射 10=映像节映射+线程劫持；默认 0 与原行为一致。
    int injectionTechnique = 0;
    /// 注入后从 PEB 模块链表摘除（测试模块枚举检测；卸载时自动恢复）
    bool hideInjectedModule = false;
};

struct AiModelProfile {
    std::wstring apiUrl = L"https://api.openai.com/v1/chat/completions";
    std::wstring apiKey;
    std::wstring modelName = L"gpt-4o";
    double temperature = 0.3;
    int maxTokens = 4096;
};

struct AiApiSettings {
    bool enabled = false;
    std::wstring apiUrl = L"https://api.openai.com/v1/chat/completions";
    std::wstring apiKey;
    std::wstring modelName = L"gpt-4o";
    double temperature = 0.3;
    int maxTokens = 4096;
    std::vector<AiModelProfile> savedModels;
};

// ── 主界面状态缓存（退出时保存，启动时恢复） ───────────────────
struct HomeState {
    // 当前选中的标签页: 0=连点 1=录制 2=宏 3=脚本定制
    int activeTab = 0;
    // 连点设置
    int clickerButton = 0;            // 0=左键 1=中键 2=右键
    int clickerIntervalMode = 1;      // 0=自定义 1=高效 2=极限
    double clickerCustomInterval = 0.1;
    // 录制设置（捕获范围固定全局；窗口过滤入口已移除）
    int recorderCaptureScope = 1;
    int recorderInputMode = 0;        // 0=自动 1=绝对坐标 2=相对坐标 3=图片定位
    int recorderWindowMode = 0;       // 0=全屏模式 1=窗口模式（窗口相对录制）
    // 选中项（用文件路径标识，如果文件被删除则自动忽略）
    std::wstring selectedScriptPath;
    std::wstring selectedRecordingPath;
    // 各标签页滚动位置
    int clickerScrollOffset = 0;
    int recorderScrollOffset = 0;
    int macroScrollOffset = 0;
    int scriptCustomScrollOffset = 0;
    // 全局启停热键（跨重启；与 EngineHost::globalHotkey_ 同步）
    std::wstring globalHotkeyText = L"F8";
    int globalHotkeyVk = 0x77; // VK_F8
    int globalHotkeyModifiers = 0;
    bool globalHotkeyHold = false;
    /// 界面模式：simple=极简 / pro=专业（录制页倍速是否强制启用）
    std::wstring uiMode = L"simple";
};

/// 引擎写 home 运行时字段时用：叠选中/滚动/连点/录制模式，不碰 uiMode / 全局热键。
/// 避免 SaveHomeState 把内存里过期的整份 AppSettings 盖掉设置页刚保存的 playback/other/ai。
inline void OverlayHomeRuntimeSelection(HomeState& dst, const HomeState& src) {
    dst.activeTab = src.activeTab;
    dst.clickerButton = src.clickerButton;
    dst.clickerIntervalMode = src.clickerIntervalMode;
    dst.clickerCustomInterval = src.clickerCustomInterval;
    dst.recorderCaptureScope = src.recorderCaptureScope;
    dst.recorderInputMode = src.recorderInputMode;
    dst.recorderWindowMode = src.recorderWindowMode;
    dst.selectedScriptPath = src.selectedScriptPath;
    dst.selectedRecordingPath = src.selectedRecordingPath;
    dst.clickerScrollOffset = src.clickerScrollOffset;
    dst.recorderScrollOffset = src.recorderScrollOffset;
    dst.macroScrollOffset = src.macroScrollOffset;
    dst.scriptCustomScrollOffset = src.scriptCustomScrollOffset;
}

struct AppSettings {
    ClickTabSettings click{};
    PlaybackTabSettings playback{};
    OtherTabSettings other{};
    WindowModeSettings windowMode{};
    AiApiSettings ai{};
    HomeState home{};
};

inline AppSettings DefaultAppSettings() {
    return AppSettings{};
}

inline constexpr double kPlaybackSpeedMin = 0.25;
inline constexpr double kPlaybackSpeedMax = 4.0;

inline double ClampPlaybackSpeed(double s) {
    if (!(s >= kPlaybackSpeedMin)) return kPlaybackSpeedMin;
    if (s > kPlaybackSpeedMax) return kPlaybackSpeedMax;
    return s;
}

inline constexpr double kUiScaleFactorMin = 0.25;
inline constexpr double kUiScaleFactorMax = 3.0;
inline constexpr double kUiScaleFactorDefault = 1.0;

/// 界面缩放倍率：须为正数；非正/NaN 回 1.0；过小/过大钳到 0.25~3.0
inline double NormalizeUiScaleFactor(double s) {
    if (!(s > 0.0)) return kUiScaleFactorDefault;
    if (s < kUiScaleFactorMin) return kUiScaleFactorMin;
    if (s > kUiScaleFactorMax) return kUiScaleFactorMax;
    return s;
}

inline std::wstring NormalizeHomeUiMode(const std::wstring& m) {
    return m == L"pro" ? L"pro" : L"simple";
}

inline std::wstring NormalizeEditorDefaultView(const std::wstring& v) {
    return v == L"visual" ? L"visual" : L"code";
}

inline std::wstring NormalizeEditorCatalogPreset(const std::wstring& v) {
    if (v == L"simple" || v == L"office" || v == L"game" || v == L"all" || v == L"custom")
        return v;
    return L"all";
}

inline bool HomeUiModeIsPro(const HomeState& h) {
    return NormalizeHomeUiMode(h.uiMode) == L"pro";
}

/// 始终按倍速换算时间系数（mousePlayback 动作字段；无启用勾选）
inline double PlaybackTimeScaleAlways(double speed) {
    return 1.0 / ClampPlaybackSpeed(speed);
}

/// 录制页直接播放用的时间系数：极简始终启用；专业看勾选
inline double RecordingPlaybackTimeScale(const AppSettings& s) {
    if (HomeUiModeIsPro(s.home) && !s.playback.enablePlaybackSpeed) return 1.0;
    return PlaybackTimeScaleAlways(s.playback.playbackSpeed);
}

/// 时间缩放系数：实际秒数 = 脚本秒数 * PlaybackTimeScale。未勾选时为 1。
inline double PlaybackTimeScale(const PlaybackTabSettings& p) {
    if (!p.enablePlaybackSpeed) return 1.0;
    return PlaybackTimeScaleAlways(p.playbackSpeed);
}

/// 对已采样的等待/间隔按运行时系数缩放。≤0 保持原值（0=不等待）。找图时限不走此函数。
inline double ScalePlaybackTimeSeconds(double seconds, double timeScale) {
    if (seconds <= 0.0 || timeScale == 1.0) return seconds;
    return seconds * timeScale;
}

inline uint64_t ScalePlaybackTimeUs(uint64_t us, double timeScale) {
    if (us == 0 || timeScale == 1.0) return us;
    const double scaled = static_cast<double>(us) * timeScale;
    if (!(scaled > 0.0)) return 1;
    constexpr double kMax = 18446744073709549568.0;  // UINT64_MAX as double
    if (scaled >= kMax) return ~static_cast<uint64_t>(0);
    const uint64_t rounded = static_cast<uint64_t>(scaled + 0.5);
    return rounded == 0 ? 1 : rounded;
}

inline double ScalePlaybackTimeSeconds(double seconds, const PlaybackTabSettings& p) {
    return ScalePlaybackTimeSeconds(seconds, PlaybackTimeScale(p));
}

inline uint64_t ScalePlaybackTimeUs(uint64_t us, const PlaybackTabSettings& p) {
    return ScalePlaybackTimeUs(us, PlaybackTimeScale(p));
}

}  // namespace quickscript
