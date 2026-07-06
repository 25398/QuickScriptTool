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

void LoadAiApiSettings(const std::wstring& obj, quickscript::AiApiSettings& out) {
    out.enabled = ParseBoolField(obj, L"enabled", out.enabled);
    out.apiUrl = ExtractString(obj, L"apiUrl");
    if (out.apiUrl.empty()) out.apiUrl = L"https://api.openai.com/v1/chat/completions";
    out.apiKey = ExtractString(obj, L"apiKey");
    out.modelName = ExtractString(obj, L"modelName");
    if (out.modelName.empty()) out.modelName = L"gpt-4o";
    out.temperature = ParseDoubleField(obj, L"temperature", out.temperature);
    out.maxTokens = ParseIntField(obj, L"maxTokens", out.maxTokens);
    out.savedModels.clear();
    const auto arrPos = obj.find(L"\"savedModels\"");
    if (arrPos == std::wstring::npos) return;
    const auto bracket = obj.find(L'[', arrPos);
    if (bracket == std::wstring::npos) return;
    size_t pos = bracket + 1;
    while (pos < obj.size()) {
        while (pos < obj.size() && (obj[pos] == L' ' || obj[pos] == L'\n'
            || obj[pos] == L'\r' || obj[pos] == L'\t' || obj[pos] == L',')) {
            ++pos;
        }
        if (pos >= obj.size() || obj[pos] == L']') break;
        if (obj[pos] != L'{') { ++pos; continue; }
        int depth = 0;
        const size_t start = pos;
        for (; pos < obj.size(); ++pos) {
            if (obj[pos] == L'{') ++depth;
            else if (obj[pos] == L'}') {
                --depth;
                if (depth == 0) {
                    const std::wstring block = obj.substr(start, pos - start + 1);
                    quickscript::AiModelProfile profile{};
                    profile.apiUrl = ExtractString(block, L"apiUrl");
                    if (profile.apiUrl.empty()) profile.apiUrl = L"https://api.openai.com/v1/chat/completions";
                    profile.apiKey = ExtractString(block, L"apiKey");
                    profile.modelName = ExtractString(block, L"modelName");
                    if (profile.modelName.empty()) continue;
                    profile.temperature = ParseDoubleField(block, L"temperature", profile.temperature);
                    profile.maxTokens = ParseIntField(block, L"maxTokens", profile.maxTokens);
                    out.savedModels.push_back(std::move(profile));
                    ++pos;
                    break;
                }
            }
        }
    }
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

void WriteAiApiSettings(std::wofstream& file, const quickscript::AiApiSettings& s) {
    file << L"    \"enabled\": " << (s.enabled ? L"true" : L"false") << L",\n";
    file << L"    \"apiUrl\": \"" << EscapeJson(s.apiUrl) << L"\",\n";
    file << L"    \"apiKey\": \"" << EscapeJson(s.apiKey) << L"\",\n";
    file << L"    \"modelName\": \"" << EscapeJson(s.modelName) << L"\",\n";
    file << L"    \"temperature\": " << s.temperature << L",\n";
    file << L"    \"maxTokens\": " << s.maxTokens << L",\n";
    file << L"    \"savedModels\": [\n";
    for (size_t i = 0; i < s.savedModels.size(); ++i) {
        const auto& m = s.savedModels[i];
        file << L"      {\n";
        file << L"        \"apiUrl\": \"" << EscapeJson(m.apiUrl) << L"\",\n";
        file << L"        \"apiKey\": \"" << EscapeJson(m.apiKey) << L"\",\n";
        file << L"        \"modelName\": \"" << EscapeJson(m.modelName) << L"\",\n";
        file << L"        \"temperature\": " << m.temperature << L",\n";
        file << L"        \"maxTokens\": " << m.maxTokens << L"\n";
        file << L"      }";
        if (i + 1 < s.savedModels.size()) file << L",";
        file << L"\n";
    }
    file << L"    ]\n";
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
    const std::wstring aiObj = ExtractObject(content, L"ai");
    if (!aiObj.empty()) LoadAiApiSettings(aiObj, out.ai);
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
    file << L"  },\n";
    file << L"  \"ai\": {\n";
    WriteAiApiSettings(file, settings.ai);
    file << L"  }\n";
    file << L"}\n";
    file.flush();
    if (!file.good()) return false;
    file.close();
    return true;
}
