#include "app_settings_store.h"

#include "app_theme.h"
#include "findimage_gpu.h"
#include "ai_fast_paths.h"
#include "low_power_mode.h"
#include "utils.h"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

#pragma comment(lib, "crypt32.lib")

namespace {

std::wstring ProtectSecret(const std::wstring& plain) {
    if (plain.empty()) return {};
    if (plain.rfind(L"dpapi:", 0) == 0) return plain;
    const std::string utf8 = ToUtf8(plain);
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(utf8.data()));
    in.cbData = static_cast<DWORD>(utf8.size());
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"QuickScriptTool", nullptr, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return plain;
    }
    DWORD n = 0;
    CryptBinaryToStringA(out.pbData, out.cbData,
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
    std::string b64(n, '\0');
    if (n > 0) {
        CryptBinaryToStringA(out.pbData, out.cbData,
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &n);
        if (!b64.empty() && b64.back() == '\0') b64.pop_back();
        if (n < b64.size()) b64.resize(n);
    }
    LocalFree(out.pbData);
    return L"dpapi:" + FromUtf8(b64);
}

std::wstring UnprotectSecret(const std::wstring& stored) {
    if (stored.empty()) return {};
    if (stored.rfind(L"dpapi:", 0) != 0) return stored;
    const std::string b64 = ToUtf8(stored.substr(6));
    DWORD n = 0;
    if (!CryptStringToBinaryA(b64.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &n,
            nullptr, nullptr) || n == 0) {
        return {};
    }
    std::vector<BYTE> raw(n);
    if (!CryptStringToBinaryA(b64.c_str(), 0, CRYPT_STRING_BASE64, raw.data(), &n,
            nullptr, nullptr)) {
        return {};
    }
    DATA_BLOB in{};
    in.pbData = raw.data();
    in.cbData = n;
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return {};
    }
    const std::string utf8(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return FromUtf8(utf8);
}

bool WriteUtf8File(const std::wstring& path, const std::string& utf8) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
    return ok && written == utf8.size();
}

bool ParseBoolField(const std::wstring& src, const std::wstring& key, bool fallback) {
    const auto colon = FindTopLevelJsonKeyColon(src, key);
    if (colon == std::wstring::npos) return fallback;
    size_t i = colon + 1;
    while (i < src.size() && (src[i] == L' ' || src[i] == L'\t' || src[i] == L'\r' || src[i] == L'\n')) {
        ++i;
    }
    if (i + 4 <= src.size()
        && src.compare(i, 4, L"true") == 0
        && (i + 4 >= src.size()
            || src[i + 4] == L',' || src[i + 4] == L'}' || src[i + 4] == L']'
            || src[i + 4] == L' ' || src[i + 4] == L'\t'
            || src[i + 4] == L'\r' || src[i + 4] == L'\n')) {
        return true;
    }
    if (i + 5 <= src.size()
        && src.compare(i, 5, L"false") == 0
        && (i + 5 >= src.size()
            || src[i + 5] == L',' || src[i + 5] == L'}' || src[i + 5] == L']'
            || src[i + 5] == L' ' || src[i + 5] == L'\t'
            || src[i + 5] == L'\r' || src[i + 5] == L'\n')) {
        return false;
    }
    return fallback;
}

int ParseIntField(const std::wstring& src, const std::wstring& key, int fallback) {
    const auto colon = FindTopLevelJsonKeyColon(src, key);
    if (colon == std::wstring::npos) return fallback;
    size_t i = colon + 1;
    while (i < src.size() && (src[i] == L' ' || src[i] == L'\t')) ++i;
    return static_cast<int>(std::wcstol(src.c_str() + i, nullptr, 10));
}

bool IsEditorActionTypeToken(const std::wstring& s) {
    if (s.empty() || s.size() > 64) return false;
    for (const wchar_t c : s) {
        const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')
            || (c >= L'0' && c <= L'9') || c == L'_';
        if (!ok) return false;
    }
    return true;
}

size_t FindMatchingBracketW(const std::wstring& src, size_t openPos) {
    if (openPos >= src.size() || src[openPos] != L'[') return std::wstring::npos;
    int depth = 0;
    bool inStr = false;
    bool esc = false;
    for (size_t i = openPos; i < src.size(); ++i) {
        const wchar_t c = src[i];
        if (inStr) {
            if (esc) {
                esc = false;
                continue;
            }
            if (c == L'\\') {
                esc = true;
                continue;
            }
            if (c == L'"') inStr = false;
            continue;
        }
        if (c == L'"') {
            inStr = true;
            continue;
        }
        if (c == L'[') ++depth;
        else if (c == L']') {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::wstring::npos;
}

bool ParseJsonStringArrayField(const std::wstring& src, const std::wstring& key,
    std::vector<std::wstring>& out) {
    const auto pos = src.find(L"\"" + key + L"\"");
    if (pos == std::wstring::npos) return false;
    const auto colon = src.find(L':', pos);
    if (colon == std::wstring::npos) return false;
    size_t tok = colon + 1;
    while (tok < src.size() && (src[tok] == L' ' || src[tok] == L'\t'
        || src[tok] == L'\n' || src[tok] == L'\r')) {
        ++tok;
    }
    if (tok >= src.size() || src[tok] != L'[') return false;
    const auto rb = FindMatchingBracketW(src, tok);
    if (rb == std::wstring::npos) return false;
    out.clear();
    size_t i = tok + 1;
    while (i < rb) {
        while (i < rb && (src[i] == L' ' || src[i] == L'\t' || src[i] == L'\n'
            || src[i] == L'\r' || src[i] == L',')) {
            ++i;
        }
        if (i >= rb || src[i] != L'"') break;
        ++i;
        std::wstring val;
        bool esc = false;
        for (; i < rb; ++i) {
            const wchar_t c = src[i];
            if (esc) {
                val.push_back(c);
                esc = false;
                continue;
            }
            if (c == L'\\') {
                esc = true;
                continue;
            }
            if (c == L'"') {
                ++i;
                break;
            }
            val.push_back(c);
        }
        if (IsEditorActionTypeToken(val) && out.size() < 80
            && std::find(out.begin(), out.end(), val) == out.end()) {
            out.push_back(std::move(val));
        }
    }
    return true;
}

void WriteJsonStringArrayField(std::wostream& file, const wchar_t* key,
    const std::vector<std::wstring>& items) {
    file << L"    \"" << key << L"\": [";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) file << L", ";
        file << L"\"" << EscapeJson(items[i]) << L"\"";
    }
    file << L"],\n";
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
    const auto braceEnd = FindMatchingJsonBrace(src, brace);
    if (braceEnd == std::wstring::npos) return {};
    return src.substr(brace, braceEnd - brace + 1);
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
    out.recordingClickCaptureEnabled = ParseBoolField(obj, L"recordingClickCaptureEnabled",
        out.recordingClickCaptureEnabled);
    out.recordingClickCaptureHalfSize = ParseIntField(obj, L"recordingClickCaptureHalfSize",
        out.recordingClickCaptureHalfSize);
    if (out.recordingClickCaptureHalfSize < 16) out.recordingClickCaptureHalfSize = 16;
    if (out.recordingClickCaptureHalfSize > 120) out.recordingClickCaptureHalfSize = 120;
    out.enablePlaybackSpeed = ParseBoolField(obj, L"enablePlaybackSpeed", out.enablePlaybackSpeed);
    out.playbackSpeed = ParseDoubleField(obj, L"playbackSpeed", out.playbackSpeed);
    out.playbackSpeed = quickscript::ClampPlaybackSpeed(out.playbackSpeed);
    out.enableHidDriverSimulation = ParseBoolField(obj, L"enableHidDriverSimulation",
        out.enableHidDriverSimulation);
    if (obj.find(L"\"foregroundInputBackend\"") != std::wstring::npos) {
        out.foregroundInputBackend = quickscript::ClampForegroundInputBackend(
            ParseIntField(obj, L"foregroundInputBackend",
                static_cast<int>(out.foregroundInputBackend)));
    } else if (out.enableHidDriverSimulation) {
        out.foregroundInputBackend = quickscript::ForegroundInputBackend::Interception;
    } else {
        out.foregroundInputBackend = quickscript::ForegroundInputBackend::Software;
    }
    // Keep legacy bool in sync for Agent / old UI paths
    out.enableHidDriverSimulation =
        out.foregroundInputBackend != quickscript::ForegroundInputBackend::Software;
    out.scheduledTaskConflictPolicy = ParseIntField(obj, L"scheduledTaskConflictPolicy",
        out.scheduledTaskConflictPolicy);
    if (out.scheduledTaskConflictPolicy < 0 || out.scheduledTaskConflictPolicy > 1)
        out.scheduledTaskConflictPolicy = 0;
    out.scheduledTaskAutoResume = ParseBoolField(obj, L"scheduledTaskAutoResume",
        out.scheduledTaskAutoResume);
    out.lowPerformanceMode = ParseBoolField(obj, L"lowPerformanceMode", out.lowPerformanceMode);
    out.aiFastPaths = ParseBoolField(obj, L"aiFastPaths", out.aiFastPaths);
    out.findImageGpuAccel = ParseBoolField(obj, L"findImageGpuAccel", out.findImageGpuAccel);
}

void LoadOtherSettings(const std::wstring& obj, quickscript::OtherTabSettings& out) {
    out.autoHideMainWindow = ParseBoolField(obj, L"autoHideMainWindow", out.autoHideMainWindow);
    out.playSoundOnStart = ParseBoolField(obj, L"playSoundOnStart", out.playSoundOnStart);
    out.playSoundOnEnd = ParseBoolField(obj, L"playSoundOnEnd", out.playSoundOnEnd);
    out.hideBottomRightTip = ParseBoolField(obj, L"hideBottomRightTip", out.hideBottomRightTip);
    out.closeToTray = ParseBoolField(obj, L"closeToTray", out.closeToTray);
    out.autoStartOnBoot = ParseBoolField(obj, L"autoStartOnBoot", out.autoStartOnBoot);
    out.resolveImeConflict = ParseBoolField(obj, L"resolveImeConflict", out.resolveImeConflict);
    out.showFloatBall = ParseBoolField(obj, L"showFloatBall", out.showFloatBall);
    out.floatBallDocked = ParseBoolField(obj, L"floatBallDocked", out.floatBallDocked);
    {
        int edge = ParseIntField(obj, L"floatBallEdge", out.floatBallEdge);
        if (edge < 0 || edge > 3) edge = 1;
        out.floatBallEdge = edge;
    }
    out.floatBallXRatio = ParseDoubleField(obj, L"floatBallXRatio", out.floatBallXRatio);
    if (!(out.floatBallXRatio >= 0.0)) out.floatBallXRatio = 0.0;
    if (out.floatBallXRatio > 1.0) out.floatBallXRatio = 1.0;
    out.floatBallYRatio = ParseDoubleField(obj, L"floatBallYRatio", out.floatBallYRatio);
    if (!(out.floatBallYRatio >= 0.0)) out.floatBallYRatio = 0.0;
    if (out.floatBallYRatio > 1.0) out.floatBallYRatio = 1.0;
    {
        const std::wstring mon = ExtractString(obj, L"floatBallMonitorId");
        if (!mon.empty() || obj.find(L"\"floatBallMonitorId\"") != std::wstring::npos)
            out.floatBallMonitorId = mon;
    }
    {
        const std::wstring view = ExtractString(obj, L"editorDefaultView");
        if (!view.empty()) out.editorDefaultView = quickscript::NormalizeEditorDefaultView(view);
    }
    out.visualLoopWrap = ParseBoolField(obj, L"visualLoopWrap", out.visualLoopWrap);
    out.visualBlockCallWires = ParseBoolField(obj, L"visualBlockCallWires", out.visualBlockCallWires);
    out.visualIfWrap = ParseBoolField(obj, L"visualIfWrap", out.visualIfWrap);
    out.visualBlockWrap = ParseBoolField(obj, L"visualBlockWrap", out.visualBlockWrap);
    out.visualWatchWrap = ParseBoolField(obj, L"visualWatchWrap", out.visualWatchWrap);
    out.visualJumpWires = ParseBoolField(obj, L"visualJumpWires", out.visualJumpWires);
    out.visualShowGrid = ParseBoolField(obj, L"visualShowGrid", out.visualShowGrid);
    out.visualShowCardId = ParseBoolField(obj, L"visualShowCardId", out.visualShowCardId);
    ParseJsonStringArrayField(obj, L"editorActionOrder", out.editorActionOrder);
    ParseJsonStringArrayField(obj, L"editorHiddenActions", out.editorHiddenActions);
    out.editorCatalogPreset = quickscript::NormalizeEditorCatalogPreset(
        ExtractString(obj, L"editorCatalogPreset"));
    ParseJsonStringArrayField(obj, L"editorCustomActionOrder", out.editorCustomActionOrder);
    ParseJsonStringArrayField(obj, L"editorCustomHiddenActions", out.editorCustomHiddenActions);
    out.editorSearchAllActions = ParseBoolField(obj, L"editorSearchAllActions",
        out.editorSearchAllActions);
    out.editorHideFixedVars = ParseBoolField(obj, L"editorHideFixedVars",
        out.editorHideFixedVars);
    out.editorHideCoordVars = ParseBoolField(obj, L"editorHideCoordVars",
        out.editorHideCoordVars);
    out.editorMultiResultPlaceholderOnly = ParseBoolField(obj, L"editorMultiResultPlaceholderOnly",
        out.editorMultiResultPlaceholderOnly);
    out.editorDisableModifyButton = ParseBoolField(obj, L"editorDisableModifyButton",
        out.editorDisableModifyButton);
    out.editorAutoSaveOnExit = ParseBoolField(obj, L"editorAutoSaveOnExit",
        out.editorAutoSaveOnExit);
    out.editorEnableBatchInsert = ParseBoolField(obj, L"editorEnableBatchInsert",
        out.editorEnableBatchInsert);
    out.holdThresholdSeconds = ParseDoubleField(obj, L"holdThresholdSeconds", out.holdThresholdSeconds);
    out.holdThresholdSeconds = NormalizeHoldThresholdSeconds(out.holdThresholdSeconds);
    out.uiScaleFactor = ParseDoubleField(obj, L"uiScaleFactor", out.uiScaleFactor);
    out.uiScaleFactor = quickscript::NormalizeUiScaleFactor(out.uiScaleFactor);
    out.themeId = ParseIntField(obj, L"themeId", out.themeId);
    out.themeId = std::clamp(out.themeId, 0, quickscript::kThemeCount - 1);
    out.useCustomTheme = ParseBoolField(obj, L"useCustomTheme", out.useCustomTheme);
    out.customMainColor = ParseIntField(obj, L"customMainColor", out.customMainColor);
    out.customAccentColor = ParseIntField(obj, L"customAccentColor", out.customAccentColor);
    out.customMainColor = std::clamp(out.customMainColor, 0, 0xFFFFFF);
    out.customAccentColor = std::clamp(out.customAccentColor, 0, 0xFFFFFF);
    out.preferDirect2D = ParseBoolField(obj, L"preferDirect2D", out.preferDirect2D);
}

void LoadWindowModeSettings(const std::wstring& obj, quickscript::WindowModeSettings& out) {
    out.showPreviewThumbnail = ParseBoolField(obj, L"showPreviewThumbnail", out.showPreviewThumbnail);
    out.previewRefreshMs = ParseIntField(obj, L"previewRefreshMs", out.previewRefreshMs);
    out.blockRunWhenUnhealthy = ParseBoolField(obj, L"blockRunWhenUnhealthy", out.blockRunWhenUnhealthy);
    out.allowForegroundInputFallback = ParseBoolField(obj, L"allowForegroundInputFallback",
        out.allowForegroundInputFallback);
    out.enableFakeFocusInjection = ParseBoolField(obj, L"enableFakeFocusInjection",
        out.enableFakeFocusInjection);
    out.injectionTechnique = ParseIntField(obj, L"injectionTechnique", out.injectionTechnique);
    out.injectionTechnique = std::clamp(out.injectionTechnique, 0, 10);
    out.hideInjectedModule = ParseBoolField(obj, L"hideInjectedModule", out.hideInjectedModule);
    out.previewRefreshMs = std::clamp(out.previewRefreshMs, 200, 5000);
}

void LoadAiApiSettings(const std::wstring& obj, quickscript::AiApiSettings& out) {
    out.enabled = ParseBoolField(obj, L"enabled", out.enabled);
    out.apiUrl = ExtractString(obj, L"apiUrl");
    if (out.apiUrl.empty()) out.apiUrl = L"https://api.openai.com/v1/chat/completions";
    out.apiKey = UnprotectSecret(ExtractString(obj, L"apiKey"));
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
        if (obj[pos] != L'{') {
            if (obj[pos] == L'"') {
                // 跳过字符串字面量，避免把键名里的 { 当成对象起点
                bool esc = false;
                ++pos;
                for (; pos < obj.size(); ++pos) {
                    if (esc) { esc = false; continue; }
                    if (obj[pos] == L'\\') { esc = true; continue; }
                    if (obj[pos] == L'"') { ++pos; break; }
                }
            } else {
                ++pos;
            }
            continue;
        }
        const auto objEnd = FindMatchingJsonBrace(obj, pos);
        if (objEnd == std::wstring::npos) break;
        const std::wstring block = obj.substr(pos, objEnd - pos + 1);
        quickscript::AiModelProfile profile{};
        profile.apiUrl = ExtractString(block, L"apiUrl");
        if (profile.apiUrl.empty()) profile.apiUrl = L"https://api.openai.com/v1/chat/completions";
        profile.apiKey = UnprotectSecret(ExtractString(block, L"apiKey"));
        profile.modelName = ExtractString(block, L"modelName");
        if (profile.modelName.empty()) {
            pos = objEnd + 1;
            continue;
        }
        profile.temperature = ParseDoubleField(block, L"temperature", profile.temperature);
        profile.maxTokens = ParseIntField(block, L"maxTokens", profile.maxTokens);
        out.savedModels.push_back(std::move(profile));
        pos = objEnd + 1;
    }
}

void LoadHomeState(const std::wstring& obj, quickscript::HomeState& out) {
    out.activeTab = ParseIntField(obj, L"activeTab", out.activeTab);
    out.clickerButton = ParseIntField(obj, L"clickerButton", out.clickerButton);
    out.clickerIntervalMode = ParseIntField(obj, L"clickerIntervalMode", out.clickerIntervalMode);
    out.clickerCustomInterval = ParseDoubleField(obj, L"clickerCustomInterval", out.clickerCustomInterval);
    out.recorderCaptureScope = 1; // 固定全局；忽略旧配置中的窗口范围
    out.recorderInputMode = std::clamp(
        ParseIntField(obj, L"recorderInputMode", out.recorderInputMode), 0, 3);
    out.recorderWindowMode = ParseIntField(obj, L"recorderWindowMode", 0) != 0 ? 1 : 0;
    out.selectedScriptPath = ExtractString(obj, L"selectedScriptPath");
    out.selectedRecordingPath = ExtractString(obj, L"selectedRecordingPath");
    out.clickerScrollOffset = ParseIntField(obj, L"clickerScrollOffset", out.clickerScrollOffset);
    out.recorderScrollOffset = ParseIntField(obj, L"recorderScrollOffset", out.recorderScrollOffset);
    out.macroScrollOffset = ParseIntField(obj, L"macroScrollOffset", out.macroScrollOffset);
    out.scriptCustomScrollOffset = ParseIntField(obj, L"scriptCustomScrollOffset", out.scriptCustomScrollOffset);
    out.globalHotkeyText = ExtractString(obj, L"globalHotkeyText");
    if (out.globalHotkeyText.empty()) out.globalHotkeyText = L"F8";
    out.globalHotkeyVk = ParseIntField(obj, L"globalHotkeyVk", out.globalHotkeyVk);
    if (out.globalHotkeyVk == 0) out.globalHotkeyVk = 0x77;
    out.globalHotkeyModifiers = ParseIntField(obj, L"globalHotkeyModifiers", out.globalHotkeyModifiers);
    out.globalHotkeyHold = ParseBoolField(obj, L"globalHotkeyHold", out.globalHotkeyHold);
    {
        const std::wstring mode = ExtractString(obj, L"uiMode");
        if (!mode.empty()) out.uiMode = quickscript::NormalizeHomeUiMode(mode);
    }
}

void WriteClickSettings(std::wostream& file, const quickscript::ClickTabSettings& s) {
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

void WritePlaybackSettings(std::wostream& file, const quickscript::PlaybackTabSettings& s) {
    file << L"    \"enablePlaybackCount\": " << (s.enablePlaybackCount ? L"true" : L"false") << L",\n";
    file << L"    \"playbackCount\": " << s.playbackCount << L",\n";
    file << L"    \"enablePlaybackInterval\": " << (s.enablePlaybackInterval ? L"true" : L"false") << L",\n";
    file << L"    \"playbackIntervalMinSeconds\": " << s.playbackIntervalMinSeconds << L",\n";
    file << L"    \"playbackIntervalMaxSeconds\": " << s.playbackIntervalMaxSeconds << L",\n";
    file << L"    \"enableDebugOutputWindow\": " << (s.enableDebugOutputWindow ? L"true" : L"false") << L",\n";
    file << L"    \"autoOutputKeyFunctionDebug\": " << (s.autoOutputKeyFunctionDebug ? L"true" : L"false") << L",\n";
    file << L"    \"recordingClickCaptureEnabled\": " << (s.recordingClickCaptureEnabled ? L"true" : L"false") << L",\n";
    file << L"    \"recordingClickCaptureHalfSize\": " << s.recordingClickCaptureHalfSize << L",\n";
    file << L"    \"enablePlaybackSpeed\": " << (s.enablePlaybackSpeed ? L"true" : L"false") << L",\n";
    file << L"    \"playbackSpeed\": " << s.playbackSpeed << L",\n";
    file << L"    \"foregroundInputBackend\": " << static_cast<int>(s.foregroundInputBackend) << L",\n";
    file << L"    \"enableHidDriverSimulation\": "
        << ((s.foregroundInputBackend != quickscript::ForegroundInputBackend::Software) ? L"true" : L"false")
        << L",\n";
    file << L"    \"scheduledTaskConflictPolicy\": " << s.scheduledTaskConflictPolicy << L",\n";
    file << L"    \"scheduledTaskAutoResume\": " << (s.scheduledTaskAutoResume ? L"true" : L"false") << L",\n";
    file << L"    \"lowPerformanceMode\": " << (s.lowPerformanceMode ? L"true" : L"false") << L",\n";
    file << L"    \"aiFastPaths\": " << (s.aiFastPaths ? L"true" : L"false") << L",\n";
    file << L"    \"findImageGpuAccel\": " << (s.findImageGpuAccel ? L"true" : L"false") << L"\n";
}

void WriteOtherSettings(std::wostream& file, const quickscript::OtherTabSettings& s) {
    file << L"    \"autoHideMainWindow\": " << (s.autoHideMainWindow ? L"true" : L"false") << L",\n";
    file << L"    \"playSoundOnStart\": " << (s.playSoundOnStart ? L"true" : L"false") << L",\n";
    file << L"    \"playSoundOnEnd\": " << (s.playSoundOnEnd ? L"true" : L"false") << L",\n";
    file << L"    \"hideBottomRightTip\": " << (s.hideBottomRightTip ? L"true" : L"false") << L",\n";
    file << L"    \"closeToTray\": " << (s.closeToTray ? L"true" : L"false") << L",\n";
    file << L"    \"autoStartOnBoot\": " << (s.autoStartOnBoot ? L"true" : L"false") << L",\n";
    file << L"    \"resolveImeConflict\": " << (s.resolveImeConflict ? L"true" : L"false") << L",\n";
    file << L"    \"showFloatBall\": " << (s.showFloatBall ? L"true" : L"false") << L",\n";
    file << L"    \"floatBallDocked\": " << (s.floatBallDocked ? L"true" : L"false") << L",\n";
    file << L"    \"floatBallEdge\": " << s.floatBallEdge << L",\n";
    file << L"    \"floatBallXRatio\": " << s.floatBallXRatio << L",\n";
    file << L"    \"floatBallYRatio\": " << s.floatBallYRatio << L",\n";
    file << L"    \"floatBallMonitorId\": \"" << EscapeJson(s.floatBallMonitorId) << L"\",\n";
    file << L"    \"editorDefaultView\": \""
        << EscapeJson(quickscript::NormalizeEditorDefaultView(s.editorDefaultView)) << L"\",\n";
    file << L"    \"visualLoopWrap\": " << (s.visualLoopWrap ? L"true" : L"false") << L",\n";
    file << L"    \"visualBlockCallWires\": " << (s.visualBlockCallWires ? L"true" : L"false") << L",\n";
    file << L"    \"visualIfWrap\": " << (s.visualIfWrap ? L"true" : L"false") << L",\n";
    file << L"    \"visualBlockWrap\": " << (s.visualBlockWrap ? L"true" : L"false") << L",\n";
    file << L"    \"visualWatchWrap\": " << (s.visualWatchWrap ? L"true" : L"false") << L",\n";
    file << L"    \"visualJumpWires\": " << (s.visualJumpWires ? L"true" : L"false") << L",\n";
    file << L"    \"visualShowGrid\": " << (s.visualShowGrid ? L"true" : L"false") << L",\n";
    file << L"    \"visualShowCardId\": " << (s.visualShowCardId ? L"true" : L"false") << L",\n";
    WriteJsonStringArrayField(file, L"editorActionOrder", s.editorActionOrder);
    WriteJsonStringArrayField(file, L"editorHiddenActions", s.editorHiddenActions);
    file << L"    \"editorCatalogPreset\": \""
        << EscapeJson(quickscript::NormalizeEditorCatalogPreset(s.editorCatalogPreset)) << L"\",\n";
    WriteJsonStringArrayField(file, L"editorCustomActionOrder", s.editorCustomActionOrder);
    WriteJsonStringArrayField(file, L"editorCustomHiddenActions", s.editorCustomHiddenActions);
    file << L"    \"editorSearchAllActions\": "
        << (s.editorSearchAllActions ? L"true" : L"false") << L",\n";
    file << L"    \"editorHideFixedVars\": "
        << (s.editorHideFixedVars ? L"true" : L"false") << L",\n";
    file << L"    \"editorHideCoordVars\": "
        << (s.editorHideCoordVars ? L"true" : L"false") << L",\n";
    file << L"    \"editorMultiResultPlaceholderOnly\": "
        << (s.editorMultiResultPlaceholderOnly ? L"true" : L"false") << L",\n";
    file << L"    \"editorDisableModifyButton\": "
        << (s.editorDisableModifyButton ? L"true" : L"false") << L",\n";
    file << L"    \"editorAutoSaveOnExit\": "
        << (s.editorAutoSaveOnExit ? L"true" : L"false") << L",\n";
    file << L"    \"editorEnableBatchInsert\": "
        << (s.editorEnableBatchInsert ? L"true" : L"false") << L",\n";
    file << L"    \"holdThresholdSeconds\": " << s.holdThresholdSeconds << L",\n";
    file << L"    \"uiScaleFactor\": " << s.uiScaleFactor << L",\n";
    file << L"    \"themeId\": " << s.themeId << L",\n";
    file << L"    \"useCustomTheme\": " << (s.useCustomTheme ? L"true" : L"false") << L",\n";
    file << L"    \"customMainColor\": " << s.customMainColor << L",\n";
    file << L"    \"customAccentColor\": " << s.customAccentColor << L",\n";
    file << L"    \"preferDirect2D\": " << (s.preferDirect2D ? L"true" : L"false") << L"\n";
}

void WriteWindowModeSettings(std::wostream& file, const quickscript::WindowModeSettings& s) {
    file << L"    \"showPreviewThumbnail\": " << (s.showPreviewThumbnail ? L"true" : L"false") << L",\n";
    file << L"    \"previewRefreshMs\": " << s.previewRefreshMs << L",\n";
    file << L"    \"blockRunWhenUnhealthy\": " << (s.blockRunWhenUnhealthy ? L"true" : L"false") << L",\n";
    file << L"    \"allowForegroundInputFallback\": "
        << (s.allowForegroundInputFallback ? L"true" : L"false") << L",\n";
    file << L"    \"enableFakeFocusInjection\": "
        << (s.enableFakeFocusInjection ? L"true" : L"false") << L",\n";
    file << L"    \"injectionTechnique\": " << s.injectionTechnique << L",\n";
    file << L"    \"hideInjectedModule\": "
        << (s.hideInjectedModule ? L"true" : L"false") << L"\n";
}

void WriteAiApiSettings(std::wostream& file, const quickscript::AiApiSettings& s) {
    file << L"    \"enabled\": " << (s.enabled ? L"true" : L"false") << L",\n";
    file << L"    \"apiUrl\": \"" << EscapeJson(s.apiUrl) << L"\",\n";
    file << L"    \"apiKey\": \"" << EscapeJson(ProtectSecret(s.apiKey)) << L"\",\n";
    file << L"    \"modelName\": \"" << EscapeJson(s.modelName) << L"\",\n";
    file << L"    \"temperature\": " << s.temperature << L",\n";
    file << L"    \"maxTokens\": " << s.maxTokens << L",\n";
    file << L"    \"savedModels\": [\n";
    for (size_t i = 0; i < s.savedModels.size(); ++i) {
        const auto& m = s.savedModels[i];
        file << L"      {\n";
        file << L"        \"apiUrl\": \"" << EscapeJson(m.apiUrl) << L"\",\n";
        file << L"        \"apiKey\": \"" << EscapeJson(ProtectSecret(m.apiKey)) << L"\",\n";
        file << L"        \"modelName\": \"" << EscapeJson(m.modelName) << L"\",\n";
        file << L"        \"temperature\": " << m.temperature << L",\n";
        file << L"        \"maxTokens\": " << m.maxTokens << L"\n";
        file << L"      }";
        if (i + 1 < s.savedModels.size()) file << L",";
        file << L"\n";
    }
    file << L"    ]\n";
}

void WriteHomeState(std::wostream& file, const quickscript::HomeState& s) {
    file << L"    \"activeTab\": " << s.activeTab << L",\n";
    file << L"    \"clickerButton\": " << s.clickerButton << L",\n";
    file << L"    \"clickerIntervalMode\": " << s.clickerIntervalMode << L",\n";
    file << L"    \"clickerCustomInterval\": " << s.clickerCustomInterval << L",\n";
    file << L"    \"recorderCaptureScope\": " << s.recorderCaptureScope << L",\n";
    file << L"    \"recorderInputMode\": " << s.recorderInputMode << L",\n";
    file << L"    \"recorderWindowMode\": " << (s.recorderWindowMode ? 1 : 0) << L",\n";
    file << L"    \"selectedScriptPath\": \"" << EscapeJson(s.selectedScriptPath) << L"\",\n";
    file << L"    \"selectedRecordingPath\": \"" << EscapeJson(s.selectedRecordingPath) << L"\",\n";
    file << L"    \"clickerScrollOffset\": " << s.clickerScrollOffset << L",\n";
    file << L"    \"recorderScrollOffset\": " << s.recorderScrollOffset << L",\n";
    file << L"    \"macroScrollOffset\": " << s.macroScrollOffset << L",\n";
    file << L"    \"scriptCustomScrollOffset\": " << s.scriptCustomScrollOffset << L",\n";
    file << L"    \"globalHotkeyText\": \"" << EscapeJson(s.globalHotkeyText) << L"\",\n";
    file << L"    \"globalHotkeyVk\": " << s.globalHotkeyVk << L",\n";
    file << L"    \"globalHotkeyModifiers\": " << s.globalHotkeyModifiers << L",\n";
    file << L"    \"globalHotkeyHold\": " << (s.globalHotkeyHold ? L"true" : L"false") << L",\n";
    file << L"    \"uiMode\": \"" << EscapeJson(quickscript::NormalizeHomeUiMode(s.uiMode)) << L"\"\n";
}

}  // namespace

static std::wstring g_appSettingsPathOverride;

void SetAppSettingsFilePathForTest(const std::wstring& path) {
    g_appSettingsPathOverride = path;
}

std::wstring AppSettingsFilePath() {
    if (!g_appSettingsPathOverride.empty()) return g_appSettingsPathOverride;
    return AppDir() + L"\\app_settings.json";
}

bool TryLoadAppSettings(quickscript::AppSettings& out) {
    const std::wstring path = AppSettingsFilePath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    quickscript::AppSettings loaded;
    if (!LoadAppSettings(loaded)) return false;
    out = std::move(loaded);
    return true;
}

bool SaveAppSettingsPreserveUserSettings(quickscript::AppSettings& inOut, bool includeGlobalHotkey) {
    const quickscript::HomeState runtime = inOut.home;
    quickscript::AppSettings disk = inOut;
    bool loaded = false;
    const std::wstring path = AppSettingsFilePath();
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        for (int attempt = 0; attempt < 6; ++attempt) {
            if (TryLoadAppSettings(disk)) {
                loaded = true;
                break;
            }
            Sleep(15u * static_cast<DWORD>(attempt + 1));
        }
    }
    if (!loaded) disk = inOut;
    quickscript::OverlayHomeRuntimeSelection(disk.home, runtime);
    if (includeGlobalHotkey) {
        disk.home.globalHotkeyText = runtime.globalHotkeyText;
        disk.home.globalHotkeyVk = runtime.globalHotkeyVk;
        disk.home.globalHotkeyModifiers = runtime.globalHotkeyModifiers;
        disk.home.globalHotkeyHold = runtime.globalHotkeyHold;
    }
    if (!SaveAppSettings(disk)) return false;
    inOut = std::move(disk);
    return true;
}

bool LoadAppSettings(quickscript::AppSettings& out) {
    out = quickscript::DefaultAppSettings();
    const std::wstring path = AppSettingsFilePath();
    // 必须按 UTF-8 读：wofstream 旧写盘会截断中文路径，且 wifstream 默认 locale 会再读坏
    const std::wstring content = ReadAll(path);
    if (content.empty()) return false;

    const std::wstring clickObj = ExtractObject(content, L"click");
    const std::wstring playbackObj = ExtractObject(content, L"playback");
    const std::wstring otherObj = ExtractObject(content, L"other");
    if (!clickObj.empty()) LoadClickSettings(clickObj, out.click);
    if (!playbackObj.empty()) LoadPlaybackSettings(playbackObj, out.playback);
    if (!otherObj.empty()) LoadOtherSettings(otherObj, out.other);
    const std::wstring wmObj = ExtractObject(content, L"windowMode");
    if (!wmObj.empty()) LoadWindowModeSettings(wmObj, out.windowMode);
    const std::wstring aiObj = ExtractObject(content, L"ai");
    if (!aiObj.empty()) LoadAiApiSettings(aiObj, out.ai);
    const std::wstring homeObj = ExtractObject(content, L"home");
    if (!homeObj.empty()) LoadHomeState(homeObj, out.home);
    // 低性能模式是进程级开关：载入设置即同步，不依赖各 UI 是否已初始化
    SetLowPerformanceMode(out.playback.lowPerformanceMode);
    SetFindImageGpuAccel(out.playback.findImageGpuAccel);
    SetAiFastPaths(out.playback.aiFastPaths);
    return true;
}

bool SaveAppSettings(const quickscript::AppSettings& settings) {
    const std::wstring path = AppSettingsFilePath();
    const std::wstring tmpPath = path + L".tmp";
    // 勾选/取消「低性能模式」后立刻生效（含引擎已起、正在跑脚本的场景），不等重启
    SetLowPerformanceMode(settings.playback.lowPerformanceMode);
    SetFindImageGpuAccel(settings.playback.findImageGpuAccel);
    SetAiFastPaths(settings.playback.aiFastPaths);

    auto buildUtf8 = [&]() -> std::string {
        std::wostringstream file;
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
        file << L"  \"windowMode\": {\n";
        WriteWindowModeSettings(file, settings.windowMode);
        file << L"  },\n";
        file << L"  \"ai\": {\n";
        WriteAiApiSettings(file, settings.ai);
        file << L"  },\n";
        file << L"  \"home\": {\n";
        WriteHomeState(file, settings.home);
        file << L"  }\n";
        file << L"}\n";
        return ToUtf8(file.str());
    };

    // 必须写 UTF-8：wofstream 默认 locale 遇中文路径会截断，导致 app_settings.json 损坏、热键/选中全坏
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::string utf8 = buildUtf8();
        if (!WriteUtf8File(tmpPath, utf8)) {
            Sleep(20u * static_cast<DWORD>(attempt + 1));
            continue;
        }
        if (MoveFileExW(tmpPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            return true;
        }
        Sleep(20u * static_cast<DWORD>(attempt + 1));
    }
    DeleteFileW(tmpPath.c_str());
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (WriteUtf8File(path, buildUtf8())) return true;
        Sleep(30u * static_cast<DWORD>(attempt + 1));
    }
    return false;
}
