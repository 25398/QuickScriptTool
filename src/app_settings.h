#pragma once
// ──────────────────────────────────────────────────────────────────
// app_settings.h — 应用全局设置结构体
// ──────────────────────────────────────────────────────────────────

#include <string>
#include <vector>

namespace quickscript {

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
};

struct WindowModeSettings {
    bool showPreviewThumbnail = true;
    int previewRefreshMs = 500;
    bool blockRunWhenUnhealthy = true;
    /// 仅当后台输入全部失败时，才短暂抢焦点用系统键盘输入（会打扰用户）
    bool allowForegroundInputFallback = false;
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
    // 录制设置
    int recorderCaptureScope = 0;     // 0=当前窗口 1=全局
    int recorderInputMode = 0;        // 0=自动 1=桌面绝对 2=FPS相对
    // 选中项（用文件路径标识，如果文件被删除则自动忽略）
    std::wstring selectedScriptPath;
    std::wstring selectedRecordingPath;
    // 各标签页滚动位置
    int clickerScrollOffset = 0;
    int recorderScrollOffset = 0;
    int macroScrollOffset = 0;
    int scriptCustomScrollOffset = 0;
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

}  // namespace quickscript
