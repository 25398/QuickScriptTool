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
};

struct OtherTabSettings {
    bool autoHideMainWindow = true;
    bool playSoundOnStart = false;
    bool hideBottomRightTip = true;
    bool closeToTray = true;
    bool autoStartOnBoot = false;
    bool resolveImeConflict = false;
    /// 长按判定（秒）：热键捕获与运行时按住启停的最小按住时间，须 > 0
    double holdThresholdSeconds = 0.2;
    int themeId = 0;
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

inline std::wstring NormalizeHomeUiMode(const std::wstring& m) {
    return m == L"pro" ? L"pro" : L"simple";
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
