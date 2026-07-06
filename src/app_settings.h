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

struct AppSettings {
    ClickTabSettings click{};
    PlaybackTabSettings playback{};
    OtherTabSettings other{};
    AiApiSettings ai{};
};

inline AppSettings DefaultAppSettings() {
    return AppSettings{};
}

}  // namespace quickscript
