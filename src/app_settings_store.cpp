#include "app_settings_store.h"

#include "utils.h"

#include <algorithm>
#include <fstream>

namespace {

bool ParseBoolField(const std::wstring& src, const std::wstring& key, bool fallback) {
    const auto pos = src.find(L"\"" + key + L"\"");
    if (pos == std::wstring::npos) return fallback;
    const auto colon = src.find(L':', pos);
    if (colon == std::wstring::npos) return fallback;
    const auto valPos = src.find(L"true", colon);
    if (valPos != std::wstring::npos && valPos < colon + 12) return true;
    const auto falsePos = src.find(L"false", colon);
    if (falsePos != std::wstring::npos && falsePos < colon + 12) return false;
    return fallback;
}

int ParseIntField(const std::wstring& src, const std::wstring& key, int fallback) {
    const auto pos = src.find(L"\"" + key + L"\"");
    if (pos == std::wstring::npos) return fallback;
    const auto colon = src.find(L':', pos);
    if (colon == std::wstring::npos) return fallback;
    size_t i = colon + 1;
    while (i < src.size() && (src[i] == L' ' || src[i] == L'\t')) ++i;
    return static_cast<int>(std::wcstol(src.c_str() + i, nullptr, 10));
}

double ParseDoubleField(const std::wstring& src, const std::wstring& key, double fallback) {
    const auto pos = src.find(L"\"" + key + L"\"");
    if (pos == std::wstring::npos) return fallback;
    const auto colon = src.find(L':', pos);
    if (colon == std::wstring::npos) return fallback;
    size_t i = colon + 1;
    while (i < src.size() && (src[i] == L' ' || src[i] == L'\t')) ++i;
    return std::wcstod(src.c_str() + i, nullptr);
}

std::wstring ExtractObject(const std::wstring& src, const std::wstring& key) {
    const auto pos = src.find(L"\"" + key + L"\"");
    if (pos == std::wstring::npos) return {};
    const auto brace = src.find(L'{', pos);
    if (brace == std::wstring::npos) return {};
    int depth = 0;
    for (size_t i = brace; i < src.size(); ++i) {
        if (src[i] == L'{') ++depth;
        else if (src[i] == L'}') {
            --depth;
            if (depth == 0) return src.substr(brace, i - brace + 1);
        }
    }
    return {};
}

void LoadClickSettings(const std::wstring& obj, quickscript::ClickTabSettings& out) {
    out.enableRandomInterval = ParseBoolField(obj, L"enableRandomInterval", out.enableRandomInterval);
    out.randomIntervalMaxSeconds = ParseDoubleField(obj, L"randomIntervalMaxSeconds", out.randomIntervalMaxSeconds);
    out.enablePressReleaseInterval = ParseBoolField(obj, L"enablePressReleaseInterval", out.enablePressReleaseInterval);
    out.pressReleaseIntervalSeconds = ParseDoubleField(obj, L"pressReleaseIntervalSeconds", out.pressReleaseIntervalSeconds);
    out.enableCoordinateJitter = ParseBoolField(obj, L"enableCoordinateJitter", out.enableCoordinateJitter);
    out.jitterX = ParseIntField(obj, L"jitterX", out.jitterX);
    out.jitterY = ParseIntField(obj, L"jitterY", out.jitterY);
    out.enableFixedCoordinates = ParseBoolField(obj, L"enableFixedCoordinates", out.enableFixedCoordinates);
    out.fixedX = ParseIntField(obj, L"fixedX", out.fixedX);
    out.fixedY = ParseIntField(obj, L"fixedY", out.fixedY);
    out.enableClickCountLimit = ParseBoolField(obj, L"enableClickCountLimit", out.enableClickCountLimit);
    out.clickCountLimit = ParseIntField(obj, L"clickCountLimit", out.clickCountLimit);
}

void LoadPlaybackSettings(const std::wstring& obj, quickscript::PlaybackTabSettings& out) {
    out.enablePlaybackCount = ParseBoolField(obj, L"enablePlaybackCount", out.enablePlaybackCount);
    out.playbackCount = ParseIntField(obj, L"playbackCount", out.playbackCount);
    out.enablePlaybackInterval = ParseBoolField(obj, L"enablePlaybackInterval", out.enablePlaybackInterval);
    out.playbackIntervalMinSeconds = ParseDoubleField(obj, L"playbackIntervalMinSeconds", out.playbackIntervalMinSeconds);
    out.playbackIntervalMaxSeconds = ParseDoubleField(obj, L"playbackIntervalMaxSeconds", out.playbackIntervalMaxSeconds);
    out.enableDebugOutputWindow = ParseBoolField(obj, L"enableDebugOutputWindow", out.enableDebugOutputWindow);
    out.autoOutputKeyFunctionDebug = ParseBoolField(obj, L"autoOutputKeyFunctionDebug", out.autoOutputKeyFunctionDebug);
}

void LoadOtherSettings(const std::wstring& obj, quickscript::OtherTabSettings& out) {
    out.autoHideMainWindow = ParseBoolField(obj, L"autoHideMainWindow", out.autoHideMainWindow);
    out.playSoundOnStart = ParseBoolField(obj, L"playSoundOnStart", out.playSoundOnStart);
    out.hideBottomRightTip = ParseBoolField(obj, L"hideBottomRightTip", out.hideBottomRightTip);
    out.closeToTray = ParseBoolField(obj, L"closeToTray", out.closeToTray);
}

void WriteClickSettings(std::wofstream& file, const quickscript::ClickTabSettings& s) {
    file << L"    \"enableRandomInterval\": " << (s.enableRandomInterval ? L"true" : L"false") << L",\n";
    file << L"    \"randomIntervalMaxSeconds\": " << s.randomIntervalMaxSeconds << L",\n";
    file << L"    \"enablePressReleaseInterval\": " << (s.enablePressReleaseInterval ? L"true" : L"false") << L",\n";
    file << L"    \"pressReleaseIntervalSeconds\": " << s.pressReleaseIntervalSeconds << L",\n";
    file << L"    \"enableCoordinateJitter\": " << (s.enableCoordinateJitter ? L"true" : L"false") << L",\n";
    file << L"    \"jitterX\": " << s.jitterX << L",\n";
    file << L"    \"jitterY\": " << s.jitterY << L",\n";
    file << L"    \"enableFixedCoordinates\": " << (s.enableFixedCoordinates ? L"true" : L"false") << L",\n";
    file << L"    \"fixedX\": " << s.fixedX << L",\n";
    file << L"    \"fixedY\": " << s.fixedY << L",\n";
    file << L"    \"enableClickCountLimit\": " << (s.enableClickCountLimit ? L"true" : L"false") << L",\n";
    file << L"    \"clickCountLimit\": " << s.clickCountLimit << L"\n";
}

void WritePlaybackSettings(std::wofstream& file, const quickscript::PlaybackTabSettings& s) {
    file << L"    \"enablePlaybackCount\": " << (s.enablePlaybackCount ? L"true" : L"false") << L",\n";
    file << L"    \"playbackCount\": " << s.playbackCount << L",\n";
    file << L"    \"enablePlaybackInterval\": " << (s.enablePlaybackInterval ? L"true" : L"false") << L",\n";
    file << L"    \"playbackIntervalMinSeconds\": " << s.playbackIntervalMinSeconds << L",\n";
    file << L"    \"playbackIntervalMaxSeconds\": " << s.playbackIntervalMaxSeconds << L",\n";
    file << L"    \"enableDebugOutputWindow\": " << (s.enableDebugOutputWindow ? L"true" : L"false") << L",\n";
    file << L"    \"autoOutputKeyFunctionDebug\": " << (s.autoOutputKeyFunctionDebug ? L"true" : L"false") << L"\n";
}

void WriteOtherSettings(std::wofstream& file, const quickscript::OtherTabSettings& s) {
    file << L"    \"autoHideMainWindow\": " << (s.autoHideMainWindow ? L"true" : L"false") << L",\n";
    file << L"    \"playSoundOnStart\": " << (s.playSoundOnStart ? L"true" : L"false") << L",\n";
    file << L"    \"hideBottomRightTip\": " << (s.hideBottomRightTip ? L"true" : L"false") << L",\n";
    file << L"    \"closeToTray\": " << (s.closeToTray ? L"true" : L"false") << L"\n";
}

}  // namespace

std::wstring AppSettingsFilePath() {
    return AppDir() + L"\\app_settings.json";
}

bool LoadAppSettings(quickscript::AppSettings& out) {
    out = quickscript::DefaultAppSettings();
    const std::wstring path = AppSettingsFilePath();
    std::wifstream file(path);
    if (!file.is_open()) return false;
    std::wstring content((std::istreambuf_iterator<wchar_t>(file)), std::istreambuf_iterator<wchar_t>());
    file.close();
    if (content.empty()) return false;

    const std::wstring clickObj = ExtractObject(content, L"click");
    const std::wstring playbackObj = ExtractObject(content, L"playback");
    const std::wstring otherObj = ExtractObject(content, L"other");
    if (!clickObj.empty()) LoadClickSettings(clickObj, out.click);
    if (!playbackObj.empty()) LoadPlaybackSettings(playbackObj, out.playback);
    if (!otherObj.empty()) LoadOtherSettings(otherObj, out.other);
    return true;
}

bool SaveAppSettings(const quickscript::AppSettings& settings) {
    const std::wstring path = AppSettingsFilePath();
    std::wofstream file(path);
    if (!file.is_open()) return false;
    file << L"{\n";
    file << L"  \"click\": {\n";
    WriteClickSettings(file, settings.click);
    file << L"  },\n";
    file << L"  \"playback\": {\n";
    WritePlaybackSettings(file, settings.playback);
    file << L"  },\n";
    file << L"  \"other\": {\n";
    WriteOtherSettings(file, settings.other);
    file << L"  }\n";
    file << L"}\n";
    return file.good();
}
