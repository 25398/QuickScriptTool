#pragma once
// ──────────────────────────────────────────────────────────────────
// app_settings.h — 应用全局设置结构体
// ──────────────────────────────────────────────────────────────────

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
};

struct AppSettings {
    ClickTabSettings click{};
    PlaybackTabSettings playback{};
    OtherTabSettings other{};
};

inline AppSettings DefaultAppSettings() {
    return AppSettings{};
}

}  // namespace quickscript
