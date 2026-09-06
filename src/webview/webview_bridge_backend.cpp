// webview_bridge_backend.cpp — real scripts/settings/clicker/agent for WebView shell
#include "webview/webview_bridge_backend.h"

#include "agent_attachment.h"
#include "agent_conversation_store.h"
#include "agent_core.h"
#include "agent_script_ops.h"
#include "agent_system_prompt.h"
#include "agent_tools.h"
#include "agent_undo.h"
#include "ai_logic_convert.h"
#include "ai_action_service.h"
#include "app_settings_store.h"
#include "app_theme.h"
#include "desktop_tools/desktop_tools.h"
#include "image_match.h"
#include "main_features.h"
#include "recording_optimize_ops.h"
#include "recording_to_findimage.h"
#include "recorder_timeline.h"
#include "scheduled_task_store.h"
#include "scheduled_task_types.h"
#include "script_io.h"
#include "app_branding.h"
#include "utils.h"
#include "action_tree.h"
#include "action_utils.h"
#include "window_mode/window_mode_json.h"
#include "window_mode/window_mode_types.h"
#include "window_mode/window_capture.h"
#include "window_mode/window_target.h"

#include <shellapi.h>
#include <wincrypt.h>

#include <commdlg.h>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <algorithm>
#include <cwctype>
#include <system_error>
#include <thread>


namespace qst::webview {
namespace {

std::mutex g_mu;
BridgeContext g_ctx;
bool g_settingsLoaded = false;
std::function<void(std::string)> g_jsPoster;

struct AgentSessionState {
    std::wstring id;
    std::wstring name;
    std::wstring createdTime;
    std::shared_ptr<AgentCore> core;
    std::atomic<bool> busy{false};
    std::shared_ptr<std::atomic<bool>> cancelFlag;
    AiHttpAbortSlot httpAbort;
    bool titleAiDone = false; // 首轮后由独立 API 总结标题
};
AgentSessionState g_agent;

// 录制优化对话框工作副本：合并/压缩只改这里；点「保存到新录制」才落盘。
struct OptimizeWork {
    std::wstring sourcePath;
    ScriptFileData data;
    bool active = false;
};
OptimizeWork g_optWork;

bool SameOptPath(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

void ApplyLoadedSettingsToClicker() {
    g_ctx.clicker.button = static_cast<quickscript::MouseButtonChoice>(
        std::clamp(g_ctx.settings.home.clickerButton, 0, 2));
    g_ctx.clicker.intervalMode = static_cast<quickscript::ClickIntervalMode>(
        std::clamp(g_ctx.settings.home.clickerIntervalMode, 0, 2));
    g_ctx.clicker.customIntervalSeconds = g_ctx.settings.home.clickerCustomInterval;
    if (g_ctx.clicker.customIntervalSeconds <= 0) g_ctx.clicker.customIntervalSeconds = 0.1;
}

void EnsureSettingsLoaded() {
    if (g_settingsLoaded) return;
    LoadAppSettings(g_ctx.settings);
    ApplyLoadedSettingsToClicker();
    g_settingsLoaded = true;
}

std::string EscapeJson(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                o += buf;
            } else {
                o.push_back(static_cast<char>(c));
            }
        }
    }
    return o;
}

std::wstring EscapeJsonW(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
        case L'"': o += L"\\\""; break;
        case L'\\': o += L"\\\\"; break;
        case L'\n': o += L"\\n"; break;
        case L'\r': o += L"\\r"; break;
        case L'\t': o += L"\\t"; break;
        default:
            if (c < 0x20) {
                wchar_t buf[8];
                swprintf_s(buf, L"\\u%04x", static_cast<unsigned>(c));
                o += buf;
            } else {
                o.push_back(c);
            }
        }
    }
    return o;
}

std::string JsonString(const std::wstring& w) {
    return "\"" + EscapeJson(ToUtf8(w)) + "\"";
}

double ClickIntervalSeconds(const quickscript::ClickerSettings& s) {
    using quickscript::ClickIntervalMode;
    switch (s.intervalMode) {
    case ClickIntervalMode::Extreme: return 0.01;
    case ClickIntervalMode::Efficient: return 0.1;
    case ClickIntervalMode::Custom:
    default: return (std::max)(0.001, s.customIntervalSeconds);
    }
}

UINT ClickVk(const quickscript::ClickerSettings& s) {
    using quickscript::MouseButtonChoice;
    switch (s.button) {
    case MouseButtonChoice::Middle: return VK_MBUTTON;
    case MouseButtonChoice::Right: return VK_RBUTTON;
    case MouseButtonChoice::Left:
    default: return VK_LBUTTON;
    }
}

DWORD DownFlag(UINT vk) {
    if (vk == VK_RBUTTON) return MOUSEEVENTF_RIGHTDOWN;
    if (vk == VK_MBUTTON) return MOUSEEVENTF_MIDDLEDOWN;
    return MOUSEEVENTF_LEFTDOWN;
}
DWORD UpFlag(UINT vk) {
    if (vk == VK_RBUTTON) return MOUSEEVENTF_RIGHTUP;
    if (vk == VK_MBUTTON) return MOUSEEVENTF_MIDDLEUP;
    return MOUSEEVENTF_LEFTUP;
}

std::string ListJsonFiles(const std::wstring& dir, bool recordings) {
    EnsureScriptsDir();
    if (recordings) CreateDirectoryW(RecordingsDir().c_str(), nullptr);
    std::vector<ScriptFileEntry> files;
    EnumerateScriptJsonFiles(dir, files);
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    for (const auto& fe : files) {
        const auto content = ReadAll(fe.path);
        std::wstring name = ExtractString(content, L"scriptName");
        if (name.empty()) name = fe.fileName;
        name = StripJsonExtension(std::move(name));
        if (name.empty()) name = L"未命名";
        std::wstring recordTime = ExtractString(content, L"recordTime");
        if (recordTime.empty()) recordTime = L"未知";
        const int actions = CountActionsInJson(content);
        const std::wstring hotText = ExtractString(content, L"hotkeyText");
        const bool hotHold = ExtractBool(content, L"hotkeyHold", false);
        const UINT hotVk = static_cast<UINT>(ExtractNumber(content, L"hotkeyVk", 0));
        const UINT hotMods = static_cast<UINT>(ExtractNumber(content, L"hotkeyModifiers", 0));
        // vk=0 时文本为僵尸标签（曾误保存）：列表不展示，避免「看着有热键却不触发」
        std::wstring hotDisplay;
        if (hotVk) {
            hotDisplay = hotText;
            if (hotHold && !hotText.empty()) hotDisplay += L"（长按）";
        }

        if (!first) oss << ",";
        first = false;
        oss << "{"
            << "\"id\":" << JsonString(fe.path) << ","
            << "\"path\":" << JsonString(fe.path) << ","
            << "\"file\":" << JsonString(fe.fileName) << ","
            << "\"folder\":" << JsonString(fe.folder) << ","
            << "\"name\":" << JsonString(name) << ","
            << "\"recordTime\":" << JsonString(recordTime) << ","
            << "\"actionCount\":" << actions << ","
            << "\"hotkey\":" << JsonString(hotDisplay) << ","
            << "\"hotkeyVk\":" << hotVk << ","
            << "\"hotkeyModifiers\":" << hotMods << ","
            << "\"hotkeyHold\":" << (hotHold ? "true" : "false") << ","
            << "\"hotkeyEnabled\":" << (hotVk ? "true" : "false");
        if (recordings) {
            const double dur = ExtractNumber(content, L"durationSeconds", 0);
            oss << ",\"durationSeconds\":" << dur;
        }
        oss << "}";
    }
    oss << "]";
    return oss.str();
}

// 列表缓存：廉价元数据扫描一致时复用上次的 JSON。
// fingerprint = 各文件 LastWriteTime 之和，避免「只改非最新文件」时 count/newest 不变导致脏缓存。
// schemaRev：JSON 字段集变更时递增，强制丢掉旧缓存（如补 hotkeyVk）。
constexpr ULONGLONG kListJsonSchemaRev = 2;
struct ListCacheEntry {
    std::string json;
    FILETIME newestW{};
    ULONGLONG stampSum = 0;
    int count = -1;
};
std::mutex g_listCacheMu;
ListCacheEntry g_scriptsCache;
ListCacheEntry g_recordingsCache;
void InvalidateScriptListCache_Internal() {
    std::lock_guard<std::mutex> lock(g_listCacheMu);
    g_scriptsCache = {};
    g_recordingsCache = {};
}

std::string ListJsonFilesCached(const std::wstring& dir, bool recordings) {
    EnsureScriptsDir();
    if (recordings) CreateDirectoryW(RecordingsDir().c_str(), nullptr);

    int count = 0;
    FILETIME newest{};
    ULONGLONG stampSum = kListJsonSchemaRev * 0x100000001b3ull;
    {
        std::vector<ScriptFileEntry> files;
        EnumerateScriptJsonFiles(dir, files);
        count = static_cast<int>(files.size());
        for (const auto& fe : files) {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!GetFileAttributesExW(fe.path.c_str(), GetFileExInfoStandard, &fad)) continue;
            ULARGE_INTEGER u{};
            u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
            u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
            stampSum += u.QuadPart;
            if (CompareFileTime(&fad.ftLastWriteTime, &newest) > 0)
                newest = fad.ftLastWriteTime;
        }
        // 目录树变更（空文件夹增删）也要失效缓存
        std::vector<std::wstring> folders;
        EnumerateRelativeFolders(dir, folders);
        stampSum += static_cast<ULONGLONG>(folders.size()) * 1000003ull;
        count += static_cast<int>(folders.size()) * 1000;
    }

    std::lock_guard<std::mutex> lock(g_listCacheMu);
    ListCacheEntry& cache = recordings ? g_recordingsCache : g_scriptsCache;
    if (cache.count == count && cache.stampSum == stampSum
        && !cache.json.empty() && CompareFileTime(&newest, &cache.newestW) == 0) {
        return cache.json;
    }
    cache.json = ListJsonFiles(dir, recordings);
    cache.count = count;
    cache.newestW = newest;
    cache.stampSum = stampSum;
    return cache.json;
}

bool JsonGetNumber(const std::string& json, const char* key, double& out) {
    const std::string pat = std::string("\"") + key + "\"";
    const auto k = json.find(pat);
    if (k == std::string::npos) return false;
    const auto colon = json.find(':', k + pat.size());
    if (colon == std::string::npos) return false;
    size_t i = colon + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    try {
        size_t n = 0;
        out = std::stod(json.substr(i), &n);
        return n > 0;
    } catch (...) {
        return false;
    }
}

bool JsonGetBool(const std::string& json, const char* key, bool& out) {
    const std::string pat = std::string("\"") + key + "\"";
    const auto k = json.find(pat);
    if (k == std::string::npos) return false;
    const auto colon = json.find(':', k + pat.size());
    if (colon == std::string::npos) return false;
    size_t i = colon + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) ++i;
    if (json.compare(i, 4, "true") == 0) {
        out = true;
        return true;
    }
    if (json.compare(i, 5, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

bool JsonGetInt(const std::string& json, const char* key, int& out) {
    double d = 0;
    if (!JsonGetNumber(json, key, d)) return false;
    out = static_cast<int>(d);
    return true;
}

}  // namespace

void InvalidateScriptListCache() {
    InvalidateScriptListCache_Internal();
}

void SetJsPoster(std::function<void(std::string)> poster) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_jsPoster = std::move(poster);
}

void PostToWebUi(std::string jsonUtf8) {
    std::function<void(std::string)> poster;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        poster = g_jsPoster;
    }
    if (poster) poster(std::move(jsonUtf8));
}

void ReloadSettingsFromDisk() {
    std::lock_guard<std::mutex> lock(g_mu);
    LoadAppSettings(g_ctx.settings);
    ApplyLoadedSettingsToClicker();
    g_settingsLoaded = true;
}

void SyncHomeSelectionCache(const std::wstring& selectedScriptPath,
    const std::wstring& selectedRecordingPath, int activeTab) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_settingsLoaded) {
        LoadAppSettings(g_ctx.settings);
        ApplyLoadedSettingsToClicker();
        g_settingsLoaded = true;
    }
    g_ctx.settings.home.selectedScriptPath = selectedScriptPath;
    g_ctx.settings.home.selectedRecordingPath = selectedRecordingPath;
    if (activeTab >= 0 && activeTab <= 3)
        g_ctx.settings.home.activeTab = activeTab;
}

void NotifyWebDebugWindowSetting(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_settingsLoaded) {
            LoadAppSettings(g_ctx.settings);
            ApplyLoadedSettingsToClicker();
            g_settingsLoaded = true;
        }
        g_ctx.settings.playback.enableDebugOutputWindow = enabled;
    }
    PostToWebUi(std::string("{\"type\":\"settings.changed\",\"playback\":{\"enableDebugOutputWindow\":")
        + (enabled ? "true" : "false") + "}}");
}

BridgeContext& Ctx() {
    EnsureSettingsLoaded();
    return g_ctx;
}

std::string JsonListScripts() {
    return ListJsonFilesCached(ScriptsDir(), false);
}

std::string JsonListRecordings() {
    return ListJsonFilesCached(RecordingsDir(), true);
}

std::string JsonListAgentConversations() {
    std::vector<AgentConversationMeta> list;
    LoadAgentConversationList(list);
    std::vector<std::wstring> dropEmpty;
    for (const auto& m : list) {
        int rounds = m.roundCount;
        if (rounds < 1) {
            AgentConversationRecord rec;
            if (LoadAgentConversationRecord(m.id, rec)) {
                const int fileRounds = rec.meta.roundCount > 0
                    ? rec.meta.roundCount
                    : CountConversationRounds(rec.messages);
                if (fileRounds > rounds) rounds = fileRounds;
            }
        }
        if (rounds < 1) dropEmpty.push_back(m.id);
    }
    for (const auto& id : dropEmpty) DeleteAgentConversation(id);
    if (!dropEmpty.empty()) LoadAgentConversationList(list);
    // 历史遗留：首轮后仍叫「新对话」时，用首条用户话回填标题
    for (auto& m : list) {
        if (m.roundCount < 1) continue;
        if (!m.name.empty() && m.name != L"新对话") continue;
        AgentConversationRecord rec;
        if (!LoadAgentConversationRecord(m.id, rec)) continue;
        const std::wstring title = SummarizeConversationName(rec.messages);
        if (title.empty() || title == L"新对话") continue;
        m.name = title;
        SetAgentConversationName(m.id, title);
    }
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& m = list[i];
        if (i) oss << ",";
        oss << "{"
            << "\"id\":" << JsonString(m.id) << ","
            << "\"path\":" << JsonString(m.filePath) << ","
            << "\"name\":" << JsonString(m.name) << ","
            << "\"roundCount\":" << m.roundCount << ","
            << "\"createdTime\":" << JsonString(m.createdTime) << ","
            << "\"folder\":" << JsonString(m.folder) << ","
            << "\"meta\":" << JsonString(m.createdTime.empty()
                ? (std::to_wstring(m.roundCount) + L" 轮")
                : (m.createdTime + L" · " + std::to_wstring(m.roundCount) + L" 轮"))
            << "}";
    }
    oss << "]";
    return oss.str();
}

std::string JsonClickerStatus() {
    EnsureSettingsLoaded();
    std::lock_guard<std::mutex> lock(g_mu);
    const auto& h = g_ctx.settings.home;
    const char* btn = "左键";
    if (h.clickerButton == 1) btn = "中键";
    else if (h.clickerButton == 2) btn = "右键";

    std::string intervalLabel;
    if (h.clickerIntervalMode == 2) {
        intervalLabel = "极限模式 · 10 ms";
    } else if (h.clickerIntervalMode == 1) {
        intervalLabel = "高效模式 · 100 ms";
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "自定义 · %.3gs", (std::max)(0.001, h.clickerCustomInterval));
        intervalLabel = buf;
    }

    std::ostringstream oss;
    oss << "{"
        << "\"running\":false,"
        << "\"button\":" << h.clickerButton << ","
        << "\"buttonLabel\":\"" << btn << "\","
        << "\"intervalMode\":" << h.clickerIntervalMode << ","
        << "\"customInterval\":" << h.clickerCustomInterval << ","
        << "\"intervalLabel\":\"" << EscapeJson(intervalLabel) << "\""
        << "}";
    return oss.str();
}

std::string JsonOpenSettings() {
    // 每次打开设置都从磁盘刷新，避免引擎侧（如关调试窗）写盘后 g_ctx 仍是旧值。
    ReloadSettingsFromDisk();
    std::lock_guard<std::mutex> lock(g_mu);
    const auto& s = g_ctx.settings;
    std::ostringstream oss;
    oss << "{"
        << "\"click\":{"
        << "\"enableRandomInterval\":" << (s.click.enableRandomInterval ? "true" : "false") << ","
        << "\"randomIntervalMaxSeconds\":" << s.click.randomIntervalMaxSeconds << ","
        << "\"enablePressReleaseInterval\":" << (s.click.enablePressReleaseInterval ? "true" : "false") << ","
        << "\"pressReleaseIntervalSeconds\":" << s.click.pressReleaseIntervalSeconds << ","
        << "\"enableCoordinateJitter\":" << (s.click.enableCoordinateJitter ? "true" : "false") << ","
        << "\"jitterX\":" << s.click.jitterX << ","
        << "\"jitterY\":" << s.click.jitterY << ","
        << "\"enableFixedCoordinates\":" << (s.click.enableFixedCoordinates ? "true" : "false") << ","
        << "\"fixedX\":" << s.click.fixedX << ","
        << "\"fixedY\":" << s.click.fixedY << ","
        << "\"enableClickCountLimit\":" << (s.click.enableClickCountLimit ? "true" : "false") << ","
        << "\"clickCountLimit\":" << s.click.clickCountLimit
        << "},"
        << "\"other\":{"
        << "\"themeId\":" << s.other.themeId << ","
        << "\"useCustomTheme\":" << (s.other.useCustomTheme ? "true" : "false") << ","
        << "\"customMainColor\":" << static_cast<unsigned>(s.other.customMainColor) << ","
        << "\"customAccentColor\":" << static_cast<unsigned>(s.other.customAccentColor) << ","
        << "\"preferDirect2D\":" << (s.other.preferDirect2D ? "true" : "false") << ","
        << "\"holdThresholdSeconds\":" << s.other.holdThresholdSeconds << ","
        << "\"autoHideMainWindow\":" << (s.other.autoHideMainWindow ? "true" : "false") << ","
        << "\"playSoundOnStart\":" << (s.other.playSoundOnStart ? "true" : "false") << ","
        << "\"hideBottomRightTip\":" << (s.other.hideBottomRightTip ? "true" : "false") << ","
        << "\"closeToTray\":" << (s.other.closeToTray ? "true" : "false") << ","
        << "\"autoStartOnBoot\":" << (s.other.autoStartOnBoot ? "true" : "false") << ","
        << "\"resolveImeConflict\":" << (s.other.resolveImeConflict ? "true" : "false")
        << "},"
        << "\"windowMode\":{"
        << "\"showPreviewThumbnail\":" << (s.windowMode.showPreviewThumbnail ? "true" : "false") << ","
        << "\"previewRefreshMs\":" << s.windowMode.previewRefreshMs << ","
        << "\"blockRunWhenUnhealthy\":" << (s.windowMode.blockRunWhenUnhealthy ? "true" : "false") << ","
        << "\"allowForegroundInputFallback\":"
        << (s.windowMode.allowForegroundInputFallback ? "true" : "false") << ","
        << "\"enableFakeFocusInjection\":"
        << (s.windowMode.enableFakeFocusInjection ? "true" : "false") << ","
        << "\"injectionTechnique\":" << s.windowMode.injectionTechnique << ","
        << "\"hideInjectedModule\":"
        << (s.windowMode.hideInjectedModule ? "true" : "false")
        << "},"
        << "\"playback\":{"
        << "\"enablePlaybackCount\":" << (s.playback.enablePlaybackCount ? "true" : "false") << ","
        << "\"playbackCount\":" << s.playback.playbackCount << ","
        << "\"enablePlaybackInterval\":" << (s.playback.enablePlaybackInterval ? "true" : "false") << ","
        << "\"playbackIntervalMinSeconds\":" << s.playback.playbackIntervalMinSeconds << ","
        << "\"playbackIntervalMaxSeconds\":" << s.playback.playbackIntervalMaxSeconds << ","
        << "\"enableDebugOutputWindow\":" << (s.playback.enableDebugOutputWindow ? "true" : "false") << ","
        << "\"autoOutputKeyFunctionDebug\":" << (s.playback.autoOutputKeyFunctionDebug ? "true" : "false") << ","
        << "\"recordingClickCaptureEnabled\":" << (s.playback.recordingClickCaptureEnabled ? "true" : "false") << ","
        << "\"recordingClickCaptureHalfSize\":" << s.playback.recordingClickCaptureHalfSize << ","
        << "\"enablePlaybackSpeed\":" << (s.playback.enablePlaybackSpeed ? "true" : "false") << ","
        << "\"playbackSpeed\":" << s.playback.playbackSpeed << ","
        << "\"foregroundInputBackend\":" << static_cast<int>(s.playback.foregroundInputBackend) << ","
        << "\"enableHidDriverSimulation\":"
        << (s.playback.foregroundInputBackend != quickscript::ForegroundInputBackend::Software
                ? "true" : "false") << ","
        << "\"scheduledTaskConflictPolicy\":" << s.playback.scheduledTaskConflictPolicy << ","
        << "\"scheduledTaskAutoResume\":"
        << (s.playback.scheduledTaskAutoResume ? "true" : "false")
        << "},"
        << "\"ai\":{"
        << "\"enabled\":" << (s.ai.enabled ? "true" : "false") << ","
        << "\"apiUrl\":" << JsonString(s.ai.apiUrl) << ","
        << "\"apiKey\":" << JsonString(s.ai.apiKey) << ","
        << "\"modelName\":" << JsonString(s.ai.modelName) << ","
        << "\"temperature\":" << s.ai.temperature << ","
        << "\"maxTokens\":" << s.ai.maxTokens << ","
        << "\"savedModels\":[";
    for (size_t mi = 0; mi < s.ai.savedModels.size(); ++mi) {
        const auto& m = s.ai.savedModels[mi];
        if (mi) oss << ",";
        oss << "{"
            << "\"apiUrl\":" << JsonString(m.apiUrl) << ","
            << "\"apiKey\":" << JsonString(m.apiKey) << ","
            << "\"modelName\":" << JsonString(m.modelName) << ","
            << "\"temperature\":" << m.temperature << ","
            << "\"maxTokens\":" << m.maxTokens
            << "}";
    }
    oss << "]},"
        << "\"home\":{"
        << "\"clickerButton\":" << s.home.clickerButton << ","
        << "\"clickerIntervalMode\":" << s.home.clickerIntervalMode << ","
        << "\"clickerCustomInterval\":" << s.home.clickerCustomInterval << ","
        << "\"recorderInputMode\":" << s.home.recorderInputMode << ","
        << "\"recorderWindowMode\":" << (s.home.recorderWindowMode ? 1 : 0) << ","
        << "\"recorderCaptureScope\":" << s.home.recorderCaptureScope << ","
        << "\"activeTab\":" << s.home.activeTab << ","
        << "\"selectedScriptPath\":" << JsonString(s.home.selectedScriptPath) << ","
        << "\"selectedRecordingPath\":" << JsonString(s.home.selectedRecordingPath) << ","
        << "\"clickerScrollOffset\":" << s.home.clickerScrollOffset << ","
        << "\"macroScrollOffset\":" << s.home.macroScrollOffset << ","
        << "\"recorderScrollOffset\":" << s.home.recorderScrollOffset << ","
        << "\"scriptCustomScrollOffset\":" << s.home.scriptCustomScrollOffset << ","
        << "\"globalHotkeyText\":" << JsonString(s.home.globalHotkeyText) << ","
        << "\"globalHotkeyVk\":" << s.home.globalHotkeyVk << ","
        << "\"globalHotkeyModifiers\":" << s.home.globalHotkeyModifiers << ","
        << "\"globalHotkeyHold\":" << (s.home.globalHotkeyHold ? "true" : "false") << ","
        << "\"uiMode\":" << JsonString(quickscript::NormalizeHomeUiMode(s.home.uiMode))
        << "}"
        << "}";
    return oss.str();
}

std::string JsonAppBranding() {
    using quickscript::AppBranding;
    std::ostringstream oss;
    oss << "{"
        << "\"name\":\"" << EscapeJson(ToUtf8(AppBranding::AppDisplayName())) << "\","
        << "\"version\":\"" << EscapeJson(ToUtf8(AppBranding::Version())) << "\","
        << "\"tagline\":\"" << EscapeJson(ToUtf8(AppBranding::Tagline())) << "\","
        << "\"website\":\"" << EscapeJson(ToUtf8(AppBranding::WebsiteUrl())) << "\","
        << "\"contact\":\"" << EscapeJson(ToUtf8(AppBranding::ContactInfo())) << "\","
        << "\"qq\":\"" << EscapeJson(ToUtf8(AppBranding::QqGroup())) << "\","
        << "\"copyright\":\"" << EscapeJson(ToUtf8(AppBranding::CopyrightText())) << "\""
        << "}";
    return oss.str();
}

bool ApplySaveSettingsJson(const std::string& settingsObjJson, std::string& err) {
    EnsureSettingsLoaded();
    std::lock_guard<std::mutex> lock(g_mu);
    auto& s = g_ctx.settings;
    bool b = false;
    int i = 0;
    double d = 0;
    if (JsonGetBool(settingsObjJson, "enableRandomInterval", b)) s.click.enableRandomInterval = b;
    if (JsonGetNumber(settingsObjJson, "randomIntervalMaxSeconds", d)) s.click.randomIntervalMaxSeconds = d;
    if (JsonGetBool(settingsObjJson, "enablePressReleaseInterval", b)) s.click.enablePressReleaseInterval = b;
    if (JsonGetNumber(settingsObjJson, "pressReleaseIntervalSeconds", d)) s.click.pressReleaseIntervalSeconds = d;
    if (JsonGetBool(settingsObjJson, "enableCoordinateJitter", b)) s.click.enableCoordinateJitter = b;
    if (JsonGetInt(settingsObjJson, "jitterX", i)) s.click.jitterX = i;
    if (JsonGetInt(settingsObjJson, "jitterY", i)) s.click.jitterY = i;
    if (JsonGetBool(settingsObjJson, "enableFixedCoordinates", b)) s.click.enableFixedCoordinates = b;
    if (JsonGetInt(settingsObjJson, "fixedX", i)) s.click.fixedX = i;
    if (JsonGetInt(settingsObjJson, "fixedY", i)) s.click.fixedY = i;
    if (JsonGetBool(settingsObjJson, "enableClickCountLimit", b)) s.click.enableClickCountLimit = b;
    if (JsonGetInt(settingsObjJson, "clickCountLimit", i)) s.click.clickCountLimit = i;
    if (JsonGetInt(settingsObjJson, "themeId", i)) s.other.themeId = i;
    if (JsonGetBool(settingsObjJson, "useCustomTheme", b)) s.other.useCustomTheme = b;
    if (JsonGetInt(settingsObjJson, "customMainColor", i)) s.other.customMainColor = i;
    if (JsonGetInt(settingsObjJson, "customAccentColor", i)) s.other.customAccentColor = i;
    if (JsonGetBool(settingsObjJson, "preferDirect2D", b)) s.other.preferDirect2D = b;
    if (JsonGetNumber(settingsObjJson, "holdThresholdSeconds", d)) s.other.holdThresholdSeconds = d;
    if (JsonGetBool(settingsObjJson, "autoHideMainWindow", b)) s.other.autoHideMainWindow = b;
    if (JsonGetBool(settingsObjJson, "playSoundOnStart", b)) s.other.playSoundOnStart = b;
    if (JsonGetBool(settingsObjJson, "hideBottomRightTip", b)) s.other.hideBottomRightTip = b;
    if (JsonGetBool(settingsObjJson, "closeToTray", b)) s.other.closeToTray = b;
    if (JsonGetBool(settingsObjJson, "autoStartOnBoot", b)) s.other.autoStartOnBoot = b;
    if (JsonGetBool(settingsObjJson, "resolveImeConflict", b)) s.other.resolveImeConflict = b;
    if (JsonGetBool(settingsObjJson, "showPreviewThumbnail", b)) s.windowMode.showPreviewThumbnail = b;
    if (JsonGetInt(settingsObjJson, "previewRefreshMs", i)) {
        s.windowMode.previewRefreshMs = std::clamp(i, 200, 5000);
    }
    if (JsonGetBool(settingsObjJson, "blockRunWhenUnhealthy", b)) s.windowMode.blockRunWhenUnhealthy = b;
    if (JsonGetBool(settingsObjJson, "allowForegroundInputFallback", b))
        s.windowMode.allowForegroundInputFallback = b;
    if (JsonGetBool(settingsObjJson, "enableFakeFocusInjection", b))
        s.windowMode.enableFakeFocusInjection = b;
    if (JsonGetInt(settingsObjJson, "injectionTechnique", i)) {
        s.windowMode.injectionTechnique = std::clamp(i, 0, 10);
    }
    if (JsonGetBool(settingsObjJson, "hideInjectedModule", b))
        s.windowMode.hideInjectedModule = b;
    if (JsonGetBool(settingsObjJson, "aiEnabled", b)) s.ai.enabled = b;
    if (JsonGetBool(settingsObjJson, "enabled", b) && settingsObjJson.find("\"ai\"") != std::string::npos) {
        // prefer nested via flat keys from collectSettings
    }
    // playback (flat keys from collectSettings)
    if (JsonGetBool(settingsObjJson, "enablePlaybackCount", b)) s.playback.enablePlaybackCount = b;
    if (JsonGetInt(settingsObjJson, "playbackCount", i)) s.playback.playbackCount = (std::max)(0, i);
    if (JsonGetBool(settingsObjJson, "enablePlaybackInterval", b)) s.playback.enablePlaybackInterval = b;
    if (JsonGetNumber(settingsObjJson, "playbackIntervalMinSeconds", d))
        s.playback.playbackIntervalMinSeconds = d;
    if (JsonGetNumber(settingsObjJson, "playbackIntervalMaxSeconds", d))
        s.playback.playbackIntervalMaxSeconds = d;
    if (JsonGetBool(settingsObjJson, "enableDebugOutputWindow", b)) s.playback.enableDebugOutputWindow = b;
    if (JsonGetBool(settingsObjJson, "autoOutputKeyFunctionDebug", b))
        s.playback.autoOutputKeyFunctionDebug = b;
    if (JsonGetBool(settingsObjJson, "recordingClickCaptureEnabled", b))
        s.playback.recordingClickCaptureEnabled = b;
    if (JsonGetInt(settingsObjJson, "recordingClickCaptureHalfSize", i)) {
        if (i < 16) i = 16;
        if (i > 120) i = 120;
        s.playback.recordingClickCaptureHalfSize = i;
    }
    if (JsonGetBool(settingsObjJson, "enablePlaybackSpeed", b))
        s.playback.enablePlaybackSpeed = b;
    if (JsonGetNumber(settingsObjJson, "playbackSpeed", d))
        s.playback.playbackSpeed = quickscript::ClampPlaybackSpeed(d);
    if (JsonGetInt(settingsObjJson, "foregroundInputBackend", i)) {
        s.playback.foregroundInputBackend = quickscript::ClampForegroundInputBackend(i);
        s.playback.enableHidDriverSimulation =
            s.playback.foregroundInputBackend != quickscript::ForegroundInputBackend::Software;
    } else if (JsonGetBool(settingsObjJson, "enableHidDriverSimulation", b)) {
        s.playback.enableHidDriverSimulation = b;
        s.playback.foregroundInputBackend = b
            ? quickscript::ForegroundInputBackend::Interception
            : quickscript::ForegroundInputBackend::Software;
    }
    if (JsonGetInt(settingsObjJson, "scheduledTaskConflictPolicy", i)) {
        s.playback.scheduledTaskConflictPolicy = static_cast<int>(
            ClampScheduledTaskConflictPolicy(i));
    }
    if (JsonGetBool(settingsObjJson, "scheduledTaskAutoResume", b))
        s.playback.scheduledTaskAutoResume = b;
    {
        auto getStr = [](const std::string& json, const char* key, std::string& out) -> bool {
            const std::string pat = std::string("\"") + key + "\"";
            const auto k = json.find(pat);
            if (k == std::string::npos) return false;
            const auto colon = json.find(':', k + pat.size());
            if (colon == std::string::npos) return false;
            const auto q1 = json.find('"', colon + 1);
            if (q1 == std::string::npos) return false;
            std::string val;
            for (size_t i = q1 + 1; i < json.size(); ++i) {
                if (json[i] == '\\' && i + 1 < json.size()) { val.push_back(json[i + 1]); ++i; continue; }
                if (json[i] == '"') { out = val; return true; }
                val.push_back(json[i]);
            }
            return false;
        };
        std::string str;
        // 顶层字段须在 savedModels 数组之前匹配，避免命中子对象里的同名 key
        auto getTopStr = [&settingsObjJson](const char* key, std::string& out) -> bool {
            const std::string pat = std::string("\"") + key + "\"";
            const auto smPos = settingsObjJson.find("\"savedModels\"");
            const auto k = settingsObjJson.find(pat);
            if (k == std::string::npos) return false;
            if (smPos != std::string::npos && k > smPos) return false;
            const auto colon = settingsObjJson.find(':', k + pat.size());
            if (colon == std::string::npos) return false;
            const auto q1 = settingsObjJson.find('"', colon + 1);
            if (q1 == std::string::npos) return false;
            std::string val;
            for (size_t i = q1 + 1; i < settingsObjJson.size(); ++i) {
                if (settingsObjJson[i] == '\\' && i + 1 < settingsObjJson.size()) {
                    val.push_back(settingsObjJson[i + 1]);
                    ++i;
                    continue;
                }
                if (settingsObjJson[i] == '"') {
                    out = val;
                    return true;
                }
                val.push_back(settingsObjJson[i]);
            }
            return false;
        };
        if (getTopStr("apiUrl", str)) s.ai.apiUrl = FromUtf8(str);
        // 空字符串不覆盖已有密钥：助手窗 collectSettings 时 DOM 可能未填，避免把主设置里的 key 冲掉
        if (getTopStr("apiKey", str) && !str.empty()) s.ai.apiKey = FromUtf8(str);
        if (getTopStr("modelName", str) && !str.empty()) s.ai.modelName = FromUtf8(str);
        if (JsonGetBool(settingsObjJson, "aiEnabled", b)) s.ai.enabled = b;
        if (JsonGetNumber(settingsObjJson, "temperature", d)) s.ai.temperature = d;
        if (JsonGetInt(settingsObjJson, "maxTokens", i)) s.ai.maxTokens = i;
        if (getTopStr("selectedScriptPath", str))
            s.home.selectedScriptPath = FromUtf8(str);
        if (getTopStr("selectedRecordingPath", str))
            s.home.selectedRecordingPath = FromUtf8(str);
        if (getTopStr("uiMode", str))
            s.home.uiMode = quickscript::NormalizeHomeUiMode(FromUtf8(str));

        // savedModels: optional array of profiles（字符串感知，避免 apiKey 等字段中的 }/] 截断）。
        // 空数组且未显式声明 savedModelsClear=1 时视为「页面状态未加载/未修改」，保留磁盘已有
        // 模型，避免助手窗等页面以空列表保存时把用户已保存的 DeepSeek/豆包模型整组冲掉。
        const auto smPos = settingsObjJson.find("\"savedModels\"");
        if (smPos != std::string::npos) {
            std::vector<quickscript::AiModelProfile> parsedModels;
            const auto lb = settingsObjJson.find('[', smPos);
            if (lb != std::string::npos) {
                const auto rb = FindMatchingJsonBracket(settingsObjJson, lb);
                if (rb != std::string::npos) {
                    size_t pos = lb + 1;
                    while (pos < rb) {
                        while (pos < rb && (settingsObjJson[pos] == ' ' || settingsObjJson[pos] == '\n'
                            || settingsObjJson[pos] == '\r' || settingsObjJson[pos] == '\t'
                            || settingsObjJson[pos] == ',')) {
                            ++pos;
                        }
                        if (pos >= rb || settingsObjJson[pos] != '{') break;
                        const auto oe = FindMatchingJsonBrace(settingsObjJson, pos);
                        if (oe == std::string::npos || oe > rb) break;
                        const std::string obj = settingsObjJson.substr(pos, oe - pos + 1);
                        quickscript::AiModelProfile m{};
                        std::string v;
                        if (getStr(obj, "apiUrl", v)) m.apiUrl = FromUtf8(v);
                        if (getStr(obj, "apiKey", v)) m.apiKey = FromUtf8(v);
                        if (getStr(obj, "modelName", v)) m.modelName = FromUtf8(v);
                        double td = 0;
                        int ti = 0;
                        if (JsonGetNumber(obj, "temperature", td)) m.temperature = td;
                        if (JsonGetInt(obj, "maxTokens", ti)) m.maxTokens = ti;
                        if (!m.modelName.empty() || !m.apiUrl.empty()) parsedModels.push_back(m);
                        pos = oe + 1;
                    }
                }
            }
            const bool explicitClear =
                settingsObjJson.find("\"savedModelsClear\"") != std::string::npos
                && JsonGetBool(settingsObjJson, "savedModelsClear", b) && b;
            if (!parsedModels.empty() || explicitClear) {
                s.ai.savedModels = std::move(parsedModels);
            }
        }
        // 当前模型不在列表 / 顶层缺密钥时，从已保存配置回填
        if (!s.ai.savedModels.empty()) {
            const quickscript::AiModelProfile* hit = nullptr;
            for (const auto& m : s.ai.savedModels) {
                if (!s.ai.modelName.empty() && m.modelName == s.ai.modelName) {
                    hit = &m;
                    break;
                }
            }
            if (!hit) {
                hit = &s.ai.savedModels.front();
                s.ai.modelName = hit->modelName;
            }
            if (s.ai.apiKey.empty() && !hit->apiKey.empty()) s.ai.apiKey = hit->apiKey;
            if (s.ai.apiUrl.empty() && !hit->apiUrl.empty()) s.ai.apiUrl = hit->apiUrl;
            if (hit->temperature > 0) s.ai.temperature = hit->temperature;
            if (hit->maxTokens > 0) s.ai.maxTokens = hit->maxTokens;
        }
    }
    if (JsonGetInt(settingsObjJson, "clickerButton", i)) {
        s.home.clickerButton = std::clamp(i, 0, 2);
        g_ctx.clicker.button = static_cast<quickscript::MouseButtonChoice>(s.home.clickerButton);
    }
    if (JsonGetInt(settingsObjJson, "clickerIntervalMode", i)) {
        s.home.clickerIntervalMode = std::clamp(i, 0, 2);
        g_ctx.clicker.intervalMode = static_cast<quickscript::ClickIntervalMode>(s.home.clickerIntervalMode);
    }
    if (JsonGetNumber(settingsObjJson, "clickerCustomInterval", d)) {
        s.home.clickerCustomInterval = d;
        g_ctx.clicker.customIntervalSeconds = d;
    }
    if (JsonGetInt(settingsObjJson, "recorderCaptureScope", i)) {
        // 产品已去掉「当前窗口」入口，一律全局捕获
        s.home.recorderCaptureScope = 1;
    }
    if (JsonGetInt(settingsObjJson, "recorderInputMode", i)) {
        s.home.recorderInputMode = std::clamp(i, 0, 3);
    }
    if (JsonGetInt(settingsObjJson, "recorderWindowMode", i)) {
        s.home.recorderWindowMode = i != 0 ? 1 : 0;
    }
    if (JsonGetInt(settingsObjJson, "activeTab", i)) {
        s.home.activeTab = std::clamp(i, 0, 3);
    }
    if (JsonGetInt(settingsObjJson, "clickerScrollOffset", i))
        s.home.clickerScrollOffset = (std::max)(0, i);
    if (JsonGetInt(settingsObjJson, "macroScrollOffset", i))
        s.home.macroScrollOffset = (std::max)(0, i);
    if (JsonGetInt(settingsObjJson, "recorderScrollOffset", i))
        s.home.recorderScrollOffset = (std::max)(0, i);
    if (JsonGetInt(settingsObjJson, "scriptCustomScrollOffset", i))
        s.home.scriptCustomScrollOffset = (std::max)(0, i);
    if (!SaveAppSettings(s)) {
        err = "SaveAppSettings failed";
        return false;
    }
    // 与原生 ApplyOtherOsSettings 对齐：开机自启写 HKCU\...\Run「键鼠工坊」
    SetAutoStartOnBoot(s.other.autoStartOnBoot);
    quickscript::ApplyThemeFromSettings(s);
    return true;
}

bool RestoreSettingsDefaults(std::string& err) {
    EnsureSettingsLoaded();
    std::lock_guard<std::mutex> lock(g_mu);
    g_ctx.settings = quickscript::DefaultAppSettings();
    g_ctx.clicker.button = static_cast<quickscript::MouseButtonChoice>(
        std::clamp(g_ctx.settings.home.clickerButton, 0, 2));
    g_ctx.clicker.intervalMode = static_cast<quickscript::ClickIntervalMode>(
        std::clamp(g_ctx.settings.home.clickerIntervalMode, 0, 2));
    g_ctx.clicker.customIntervalSeconds = g_ctx.settings.home.clickerCustomInterval;
    if (g_ctx.clicker.customIntervalSeconds <= 0) g_ctx.clicker.customIntervalSeconds = 0.1;
    if (!SaveAppSettings(g_ctx.settings)) {
        err = "SaveAppSettings failed";
        return false;
    }
    SetAutoStartOnBoot(g_ctx.settings.other.autoStartOnBoot);
    quickscript::ApplyThemeFromSettings(g_ctx.settings);
    return true;
}

/// Persist clicker opts only — execution must go through engine::StartClicker (no SendInput here).
bool StartClickerFromOpts(const std::string& msgJson, std::string& err) {
    EnsureSettingsLoaded();
    StopClicker();
    {
        std::lock_guard<std::mutex> lock(g_mu);
        int i = 0;
        double d = 0;
        if (JsonGetInt(msgJson, "button", i)) {
            g_ctx.settings.home.clickerButton = std::clamp(i, 0, 2);
            g_ctx.clicker.button = static_cast<quickscript::MouseButtonChoice>(g_ctx.settings.home.clickerButton);
        }
        if (JsonGetInt(msgJson, "intervalMode", i)) {
            g_ctx.settings.home.clickerIntervalMode = std::clamp(i, 0, 2);
            g_ctx.clicker.intervalMode = static_cast<quickscript::ClickIntervalMode>(g_ctx.settings.home.clickerIntervalMode);
        }
        if (JsonGetNumber(msgJson, "customInterval", d)) {
            g_ctx.settings.home.clickerCustomInterval = d;
            g_ctx.clicker.customIntervalSeconds = d;
        }
        if (!SaveAppSettings(g_ctx.settings)) {
            err = "SaveAppSettings failed";
            return false;
        }
    }
    return true;
}

void StopClicker() {
    // WebView shell: execution is engine::StartClicker only; nothing to stop here.
}

namespace {

bool IsPathInside(const std::wstring& full, const std::wstring& dir) {
    // 用 std::filesystem 规范化（支持长路径、解析 ..），避免 MAX_PATH 截断
    // 导致包含关系判断被绕过（截断后的路径可能恰好落在目录前缀内）。
    std::error_code ec;
    const auto fullCanon = std::filesystem::weakly_canonical(full, ec);
    if (ec || fullCanon.empty()) return false;
    const auto dirCanon = std::filesystem::weakly_canonical(dir, ec);
    if (ec || dirCanon.empty()) return false;
    std::wstring f = fullCanon.wstring();
    std::wstring d = dirCanon.wstring();
    for (auto& ch : f) {
        if (ch == L'/') ch = L'\\';
    }
    for (auto& ch : d) {
        if (ch == L'/') ch = L'\\';
    }
    if (f.size() <= d.size() || _wcsnicmp(f.c_str(), d.c_str(), d.size()) != 0) return false;
    return f[d.size()] == L'\\';
}

}  // namespace

bool ResolveScriptPath(const std::string& idOrPathUtf8, std::wstring& outPath, std::string& err) {
    if (idOrPathUtf8.empty()) {
        err = "empty path";
        return false;
    }
    std::wstring cand = FromUtf8(idOrPathUtf8);
    if (cand.find(L'\\') == std::wstring::npos && cand.find(L'/') == std::wstring::npos) {
        std::wstring name = cand;
        if (name.size() < 5 || _wcsicmp(name.c_str() + name.size() - 5, L".json") != 0)
            name += L".json";
        auto findByName = [&](const std::wstring& root) -> bool {
            std::vector<ScriptFileEntry> files;
            EnumerateScriptJsonFiles(root, files);
            for (const auto& f : files) {
                if (_wcsicmp(f.fileName.c_str(), name.c_str()) == 0) {
                    outPath = f.path;
                    return true;
                }
            }
            return false;
        };
        if (findByName(ScriptsDir()) || findByName(RecordingsDir())) return true;
        err = "script not found";
        return false;
    }
    // 含路径分隔符：只允许应用脚本/录制目录内的既有文件，防路径穿越读写任意文件。
    if (GetFileAttributesW(cand.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = "script not found";
        return false;
    }
    if (!IsPathInside(cand, ScriptsDir()) && !IsPathInside(cand, RecordingsDir())) {
        err = "script path outside app directories";
        return false;
    }
    outPath = cand;
    return true;
}

std::wstring ActionTypeLabel(const std::wstring& type) {
    if (type == L"moveMouse") return L"移动鼠标到";
    if (type == L"moveMouseRelative") return L"相对移动鼠标";
    if (type == L"mouseDown") return L"鼠标按下";
    if (type == L"mouseUp") return L"鼠标松开";
    if (type == L"mouseClick") return L"鼠标点击";
    if (type == L"mousePlayback") return L"运行录制回放";
    if (type == L"keyDown") return L"键盘按下";
    if (type == L"keyUp") return L"键盘松开";
    if (type == L"keyClick") return L"按键点击";
    if (type == L"hotkeyShortcut") return L"快捷按键";
    if (type == L"quickInput") return L"快捷输入";
    if (type == L"scrollWheel") return L"滚动滚轮";
    if (type == L"findImage") return L"找图";
    if (type == L"textRecognition") return L"文字识别";
    if (type == L"wait") return L"等待";
    if (type == L"loop") return L"循环";
    if (type == L"endLoop") return L"跳出循环";
    if (type == L"defineBlock") return L"定义宏指令块";
    if (type == L"runBlock") return L"运行宏指令块";
    if (type == L"runMacro") return L"运行鼠标宏";
    if (type == L"if") return L"条件-如果";
    if (type == L"else") return L"条件-否则";
    if (type == L"lockScreenshot") return L"锁定截屏";
    if (type == L"unlockScreenshot") return L"解锁截屏";
    if (type == L"stopMacro") return L"结束宏运行";
    if (type == L"goto") return L"跳转";
    if (type == L"runProgram") return L"运行程序";
    if (type == L"closeProgram") return L"关闭程序";
    if (type == L"openWebpage") return L"打开网页";
    if (type == L"openFile") return L"打开文件";
    if (type == L"timerRecordTime") return L"计时器记录时间";
    if (type == L"getCursorPos") return L"获取当前光标位置";
    if (type == L"aiTextAnalysis") return L"AI文字分析";
    if (type == L"aiImageAnalysis") return L"AI图片分析";
    if (type == L"aiActionExecute") return L"AI动作执行";
    if (type.empty()) return L"（未知）";
    return type;
}

std::string SerializeEditorActionsJsonArray(const std::vector<ScriptAction>& actions) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < actions.size(); ++i) {
        if (i) oss << ",";
        ScriptAction a = actions[i];
        a.coordsAreNormalized = false;
        std::wstring aj = ScriptActionToJsonString(a);
        size_t start = 0;
        while (start < aj.size() && (aj[start] == L' ' || aj[start] == L'\t' || aj[start] == L'\r' || aj[start] == L'\n')) ++start;
        size_t end = aj.size();
        while (end > start && (aj[end - 1] == L' ' || aj[end - 1] == L'\t' || aj[end - 1] == L'\r' || aj[end - 1] == L'\n' || aj[end - 1] == L',')) --end;
        std::string utf = ToUtf8(aj.substr(start, end - start));
        if (!utf.empty() && utf.front() == '{') {
            std::string typeStr;
            const auto tpos = utf.find("\"type\"");
            if (tpos != std::string::npos) {
                const auto colon = utf.find(':', tpos);
                const auto q1 = utf.find('"', colon + 1);
                const auto q2 = utf.find('"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos)
                    typeStr = utf.substr(q1 + 1, q2 - q1 - 1);
            }
            const std::string display = EscapeJson(ToUtf8(ActionTypeLabel(FromUtf8(typeStr))));
            utf.insert(1, "\"name\":\"" + display + "\",");
        }
        oss << utf;
    }
    oss << "]";
    return oss.str();
}

std::string JsonPeekScriptActions(const std::wstring& path) {
    ScriptFileData data = LoadScriptFileData(path, true);
    return SerializeEditorActionsJsonArray(data.actions);
}

std::string JsonPreviewScriptActions(const std::wstring& path, int maxNames) {
    if (maxNames <= 0) maxNames = 16;
    if (maxNames > 40) maxNames = 40;
    ScriptFileData data = LoadScriptFileData(path, true);
    const int total = static_cast<int>(data.actions.size());
    const int n = (std::min)(maxNames, total);
    std::ostringstream oss;
    oss << "{\"actionCount\":" << total << ",\"names\":[";
    for (int i = 0; i < n; ++i) {
        if (i) oss << ",";
        oss << JsonString(ActionName(data.actions[static_cast<size_t>(i)]));
    }
    oss << "]}";
    return oss.str();
}

std::string JsonLoadScriptEditor(const std::wstring& path, bool isNew) {
    std::ostringstream oss;
    oss << "{";
    if (isNew || path.empty()) {
        // 与 GDI 编辑器 / 鼠标录制一致：鼠标宏-<unix秒>
        const std::wstring newName = L"鼠标宏-" + TimestampName();
        // selectedActionIndex=-1：打开编辑器不选中列表项，右栏固定「移动鼠标」添加预览（更快）
        oss << "\"name\":" << JsonString(newName) << ",\"path\":\"\",\"actions\":[],\"selectedActionIndex\":-1,"
            << "\"breakoutTimeSeconds\":0,"
            << "\"mode\":0,\"windowMode\":{\"enabled\":0,\"executionKind\":\"hiddenDesktop\","
            << "\"selectMethod\":\"selectOnStartup\",\"targetExePath\":\"\",\"fakeFocusEnabled\":0}";
        oss << "}";
        return oss.str();
    }
    // Editor: denormalize to screen pixels for editable x/y fields
    ScriptFileData data = LoadScriptFileData(path, true);
    std::wstring name = data.scriptName;
    if (name.empty()) {
        const auto slash = path.find_last_of(L"\\/");
        name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    }
    name = StripJsonExtension(std::move(name));
    int modeSel = 0;
    if (data.windowMode.enabled) {
        modeSel = (data.windowMode.executionKind == windowmode::WindowModeExecutionKind::BackgroundWindow) ? 2 : 1;
    }
    std::wstring wmJson;
    windowmode::WriteWindowModeJson(wmJson, data.windowMode, false);
    // WriteWindowModeJson emits `  "windowMode": { ... }` — strip leading spaces for embedding
    while (!wmJson.empty() && (wmJson.front() == L' ' || wmJson.front() == L'\n')) wmJson.erase(wmJson.begin());
    // 打开编辑器：不选中任何列表项（selectedActionIndex=-1），右栏固定「移动鼠标」添加预览
    const int selectedActionIndex = -1;
    oss << "\"name\":" << JsonString(name) << ","
        << "\"path\":" << JsonString(path) << ","
        << "\"hotkey\":" << JsonString(data.hotkey.text) << ","
        << "\"hotkeyVk\":" << data.hotkey.vk << ","
        << "\"hotkeyModifiers\":" << data.hotkey.modifiers << ","
        << "\"hotkeyHold\":" << (data.hotkey.holdMode ? "true" : "false") << ","
        << "\"breakoutTimeSeconds\":" << data.breakoutTimeSeconds << ","
        << "\"mode\":" << modeSel << ","
        << "\"selectedActionIndex\":" << selectedActionIndex << ","
        << ToUtf8(wmJson) << ","
        << "\"actions\":" << SerializeEditorActionsJsonArray(data.actions);
    oss << "}";
    return oss.str();
}

namespace {

std::string ExtractJsonArray(const std::string& json, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    const auto k = json.find(pat);
    if (k == std::string::npos) return "[]";
    const auto bracket = json.find('[', k + pat.size());
    if (bracket == std::string::npos) return "[]";
    int depth = 0;
    bool inStr = false;
    bool esc = false;
    for (size_t i = bracket; i < json.size(); ++i) {
        const char c = json[i];
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') {
            inStr = true;
            continue;
        }
        if (c == '[') ++depth;
        else if (c == ']') {
            --depth;
            if (depth == 0) return json.substr(bracket, i - bracket + 1);
        }
    }
    return "[]";
}

std::vector<std::string> SplitJsonObjects(const std::string& arr) {
    std::vector<std::string> out;
    int depth = 0;
    size_t start = std::string::npos;
    bool inStr = false;
    bool esc = false;
    for (size_t i = 0; i < arr.size(); ++i) {
        const char c = arr[i];
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') {
            inStr = true;
            continue;
        }
        if (c == '{') {
            if (depth == 0) start = i;
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && start != std::string::npos) {
                out.push_back(arr.substr(start, i - start + 1));
                start = std::string::npos;
            }
        }
    }
    return out;
}

bool ParseEditorActionsJson(const std::string& msgJson, std::vector<ScriptAction>& out,
    std::string& err) {
    out.clear();
    const std::string arr = ExtractJsonArray(msgJson, "actions");
    const auto objs = SplitJsonObjects(arr);
    size_t no = 0;
    for (const auto& obj : objs) {
        ++no;
        const std::wstring block = FromUtf8(obj);
        ScriptAction a = ParseScriptActionBlock(block, no - 1, false);
        if (ExtractString(block, L"type").empty()
            && a.type == ActionType::MoveMouse && a.remark.empty()) {
            continue;  // 空动作
        }
        a.originalNo = static_cast<int>(no);
        out.push_back(std::move(a));
    }
    if (const std::wstring endLoopErr = ValidateEndLoopPlacements(out); !endLoopErr.empty()) {
        err = ToUtf8(endLoopErr);
        return false;
    }
    return true;
}

}  // namespace

bool ParseDebugScriptJson(const std::string& msgJson, std::vector<ScriptAction>& actions,
    int& startIndex, bool& stepMode, std::vector<int>& breakpoints,
    Hotkey& debugHotkey, std::wstring& displayName,
    windowmode::WindowModeScriptConfig& wmCfg, std::string& err) {
    actions.clear();
    breakpoints.clear();
    displayName.clear();
    startIndex = 0;
    stepMode = false;
    debugHotkey = Hotkey{};
    wmCfg = windowmode::WindowModeScriptConfig{};

    if (!ParseEditorActionsJson(msgJson, actions, err)) return false;
    if (actions.empty()) {
        err = "没有可调试的动作";
        return false;
    }

    // 顶层 name（先于 actions 数组出现，避免误取动作内的 name 字段）
    std::string nameUtf8;
    const size_t actionsPos = msgJson.find("\"actions\"");
    const size_t namePos = msgJson.find("\"name\"");
    if (namePos != std::string::npos
        && (actionsPos == std::string::npos || namePos < actionsPos)) {
        const auto colon = msgJson.find(':', namePos + 6);
        if (colon != std::string::npos) {
            const auto q1 = msgJson.find('"', colon + 1);
            if (q1 != std::string::npos) {
                for (size_t i = q1 + 1; i < msgJson.size() && msgJson[i] != '"'; ++i) {
                    if (msgJson[i] == '\\' && i + 1 < msgJson.size()) {
                        nameUtf8.push_back(msgJson[i + 1]);
                        ++i;
                        continue;
                    }
                    nameUtf8.push_back(msgJson[i]);
                }
            }
        }
    }
    displayName = FromUtf8(nameUtf8);

    std::string mode;
    const std::string modePat = "\"mode\"";
    const auto modePos = msgJson.find(modePat);
    if (modePos != std::string::npos) {
        const auto colon = msgJson.find(':', modePos + modePat.size());
        if (colon != std::string::npos) {
            const auto q1 = msgJson.find('"', colon + 1);
            if (q1 != std::string::npos) {
                for (size_t i = q1 + 1; i < msgJson.size() && msgJson[i] != '"'; ++i) {
                    mode.push_back(msgJson[i]);
                }
            }
        }
    }
    stepMode = (mode == "step");
    JsonGetInt(msgJson, "startIndex", startIndex);
    if (startIndex < 0 || startIndex >= static_cast<int>(actions.size())) {
        err = "调试起点超出动作列表";
        return false;
    }

    const auto bpPos = msgJson.find("\"breakpoints\"");
    if (bpPos != std::string::npos) {
        const auto lb = msgJson.find('[', bpPos);
        const auto rb = msgJson.find(']', lb);
        if (lb != std::string::npos && rb != std::string::npos) {
            std::string arr = msgJson.substr(lb + 1, rb - lb - 1);
            size_t i = 0;
            while (i < arr.size()) {
                while (i < arr.size() && (arr[i] == ' ' || arr[i] == ',')) ++i;
                if (i >= arr.size()) break;
                const int idx = atoi(arr.c_str() + i);
                if (idx >= 0 && idx < static_cast<int>(actions.size())) {
                    breakpoints.push_back(idx);
                }
                while (i < arr.size() && arr[i] != ',') ++i;
            }
        }
    }

    int vk = 0, mods = 0;
    JsonGetInt(msgJson, "hotkeyVk", vk);
    JsonGetInt(msgJson, "hotkeyModifiers", mods);
    debugHotkey.vk = static_cast<UINT>(vk);
    debugHotkey.modifiers = static_cast<UINT>(mods);

    // 与保存同一套 windowMode；顶层 mode 是 step/run，编辑器模式走 editorMode。
    {
        const std::string pat = "\"windowMode\"";
        const auto k = msgJson.find(pat);
        if (k != std::string::npos) {
            const auto brace = msgJson.find('{', k + pat.size());
            if (brace != std::string::npos) {
                const auto braceEnd = FindMatchingJsonBrace(msgJson, brace);
                if (braceEnd != std::string::npos) {
                    const std::wstring block = FromUtf8(msgJson.substr(brace, braceEnd - brace + 1));
                    wmCfg = windowmode::ParseWindowModeConfigObject(block);
                }
            }
        }
        int editorMode = -1;
        JsonGetInt(msgJson, "editorMode", editorMode);
        if (editorMode == 0) {
            wmCfg.enabled = false;
            wmCfg.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
        } else if (editorMode == 1) {
            wmCfg.enabled = true;
            wmCfg.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
        } else if (editorMode == 2) {
            wmCfg.enabled = true;
            wmCfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
        }
        if (wmCfg.selectMethod == windowmode::WindowSelectMethod::NoSelect) {
            wmCfg.windowName.clear();
            wmCfg.windowClassName.clear();
            wmCfg.childWindowClassName.clear();
            wmCfg.targetWindowTitle.clear();
            wmCfg.targetPickX = 0;
            wmCfg.targetPickY = 0;
        }
        windowmode::StripRuntimeOnlySelectTarget(wmCfg);
    }
    return true;
}

bool SaveEditorFromJson(const std::string& msgJson, std::string& outPathUtf8, std::string& err) {
    std::string pathUtf8, nameUtf8;
    auto getStr = [](const std::string& json, const char* key, std::string& out) -> bool {
        const std::string pat = std::string("\"") + key + "\"";
        const auto k = json.find(pat);
        if (k == std::string::npos) return false;
        const auto colon = json.find(':', k + pat.size());
        if (colon == std::string::npos) return false;
        const auto q1 = json.find('"', colon + 1);
        if (q1 == std::string::npos) return false;
        std::string val;
        for (size_t i = q1 + 1; i < json.size(); ++i) {
            if (json[i] == '\\' && i + 1 < json.size()) {
                val.push_back(json[i + 1]);
                ++i;
                continue;
            }
            if (json[i] == '"') {
                out = val;
                return true;
            }
            val.push_back(json[i]);
        }
        return false;
    };
    getStr(msgJson, "path", pathUtf8);
    getStr(msgJson, "name", nameUtf8);

    EnsureScriptsDir();
    std::wstring path;
    if (pathUtf8.empty()) {
        path = ScriptsDir() + L"\\鼠标宏-" + TimestampName() + L".json";
    } else {
        if (!ResolveScriptPath(pathUtf8, path, err)) {
            // Allow saving new path under scripts only（防穿越：拒绝含分隔符的任意路径）
            path = FromUtf8(pathUtf8);
            if (path.find(L'\\') == std::wstring::npos && path.find(L'/') == std::wstring::npos) {
                path = ScriptsDir() + L"\\" + path;
                if (path.size() < 5 || path.substr(path.size() - 5) != L".json") path += L".json";
                err.clear();
            } else {
                err = "script path must be inside app scripts directory";
                return false;
            }
        }
    }

    ScriptFileData data{};
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        data = LoadScriptFileData(path, false);
    } else {
        // 新建脚本：勿用 Hotkey 默认 F8（与全局启停热键冲突 →「热键注册失败」）
        data.recordTime = NowText();
        data.inputTimingVersion = 2;
        data.hotkey.vk = 0;
        data.hotkey.modifiers = 0;
        data.hotkey.text.clear();
        data.hotkey.enabled = false;
        data.hotkey.holdMode = false;
    }
    if (!nameUtf8.empty()) data.scriptName = FromUtf8(nameUtf8);
    data.scriptName = StripJsonExtension(std::move(data.scriptName));
    if (data.scriptName.empty()) data.scriptName = L"鼠标宏-" + TimestampName();

    {
        double br = 0;
        const std::string pat = "\"breakoutTimeSeconds\"";
        const auto k = msgJson.find(pat);
        if (k != std::string::npos) {
            const auto colon = msgJson.find(':', k + pat.size());
            if (colon != std::string::npos) {
                br = atof(msgJson.c_str() + colon + 1);
                data.breakoutTimeSeconds = br;
            }
        }
    }

    // windowMode object (optional)
    int modeSel = -1;
    {
        const std::string pat = "\"windowMode\"";
        const auto k = msgJson.find(pat);
        if (k != std::string::npos) {
            const auto brace = msgJson.find('{', k + pat.size());
            if (brace != std::string::npos) {
                const auto braceEnd = FindMatchingJsonBrace(msgJson, brace);
                if (braceEnd != std::string::npos) {
                    const std::wstring block = FromUtf8(msgJson.substr(brace, braceEnd - brace + 1));
                    data.windowMode = windowmode::ParseWindowModeConfigObject(block);
                }
            }
        }
        const std::string mpat = "\"mode\"";
        const auto mk = msgJson.find(mpat);
        if (mk != std::string::npos) {
            const auto colon = msgJson.find(':', mk + mpat.size());
            if (colon != std::string::npos) modeSel = atoi(msgJson.c_str() + colon + 1);
        }
        if (modeSel == 0) {
            data.windowMode.enabled = false;
            data.windowMode.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
        } else if (modeSel == 1) {
            data.windowMode.enabled = true;
            data.windowMode.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
            data.breakoutTimeSeconds = 0;
        } else if (modeSel == 2) {
            data.windowMode.enabled = true;
            data.windowMode.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
            data.breakoutTimeSeconds = 0;
        }
        if (data.windowMode.selectMethod == windowmode::WindowSelectMethod::NoSelect) {
            data.windowMode.windowName.clear();
            data.windowMode.windowClassName.clear();
            data.windowMode.childWindowClassName.clear();
            data.windowMode.targetWindowTitle.clear();
            data.windowMode.targetPickX = 0;
            data.windowMode.targetPickY = 0;
        }
        windowmode::StripRuntimeOnlySelectTarget(data.windowMode);
        windowmode::AnnotateInputStrategyForSave(data.windowMode);
    }

    if (!ParseEditorActionsJson(msgJson, data.actions, err)) {
        return false;
    }
    {
        bool anyRel = data.windowMode.windowRelativeCoordinates;
        for (const auto& a : data.actions) {
            if (a.windowRelative) { anyRel = true; break; }
        }
        if (modeSel == 0) {
            if (anyRel) {
                data.windowMode.windowRelativeCoordinates = true;
                data.windowMode.coordSpace =
                    windowmode::WindowModeCoordinateSpace::WindowClient;
            }
        } else {
            windowmode::FinalizeWindowModeForPlayback(data.windowMode, anyRel, false);
        }
        windowmode::AnnotateInputStrategyForSave(data.windowMode);
    }
    {
        std::set<std::wstring> blockNames;
        for (size_t i = 0; i < data.actions.size(); ++i) {
            const auto& a = data.actions[i];
            if (a.type != ActionType::DefineBlock) continue;
            if (!IsValidBlockName(a.blockName)) {
                err = "块名称只能以字母开始，后面只能包含字母和数字。";
                return false;
            }
            if (!blockNames.insert(a.blockName).second) {
                err = "块名称不能重复。";
                return false;
            }
            if (a.indent != 0) {
                // 纠正：DefineBlock 必须顶层
                data.actions[i].indent = 0;
            }
        }
    }

    if (!SaveScriptFileData(path, data)) {
        err = "SaveScriptFileData failed";
        return false;
    }

    // 显示名变更时同步改文件名（与列表「重命名」一致）
    {
        std::wstring safe = data.scriptName;
        for (wchar_t& ch : safe) {
            if (wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
        }
        if (!safe.empty()) {
            const auto slash = path.find_last_of(L"\\/");
            const std::wstring dir = (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
            const std::wstring newPath = dir + L"\\" + safe + L".json";
            if (_wcsicmp(path.c_str(), newPath.c_str()) != 0) {
                if (GetFileAttributesW(newPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    // 目标已存在：保留内容已写入旧路径，不覆盖；仍返回旧路径
                } else if (MoveFileW(path.c_str(), newPath.c_str())) {
                    path = newPath;
                }
            }
        }
    }

    CleanOrphanImages();
    outPathUtf8 = ToUtf8(path);
    return true;
}

bool DeleteScriptFile(const std::string& pathUtf8, std::string& err) {
    std::wstring path;
    if (!ResolveScriptPath(pathUtf8, path, err)) {
        // try recordings
        err.clear();
        path = FromUtf8(pathUtf8);
        if (path.find(L'\\') == std::wstring::npos && path.find(L'/') == std::wstring::npos) {
            path = RecordingsDir() + L"\\" + path;
            if (path.size() < 5 || path.substr(path.size() - 5) != L".json") path += L".json";
        }
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            // 文件已不存在：删除目标已达成，按成功处理（幂等），
            // 避免“幽灵条目”反复报错且列表不刷新。
            err.clear();
            return true;
        }
        // 回退分支必须仍限定在脚本/录制目录内，禁止删除目录外文件
        if (!IsPathInside(path, ScriptsDir()) && !IsPathInside(path, RecordingsDir())) {
            err = "script path outside app directories";
            return false;
        }
    }
    if (!DeleteFileW(path.c_str())) {
        err = "DeleteFile failed";
        return false;
    }
    return true;
}

bool RenameScriptFile(const std::string& pathUtf8, const std::string& newNameUtf8,
    std::string& outNewPathUtf8, std::string& err) {
    outNewPathUtf8.clear();
    std::wstring path;
    if (!ResolveScriptPath(pathUtf8, path, err)) {
        err.clear();
        path = FromUtf8(pathUtf8);
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            err = "file not found";
            return false;
        }
        if (!IsPathInside(path, ScriptsDir()) && !IsPathInside(path, RecordingsDir())) {
            err = "script path outside app directories";
            return false;
        }
    }
    std::wstring newName = StripJsonExtension(Trim(FromUtf8(newNameUtf8)));
    if (newName.empty()) {
        err = "名称不能为空";
        return false;
    }
    for (wchar_t& ch : newName) {
        if (wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
    }
    const auto slash = path.find_last_of(L"\\/");
    const std::wstring dir = (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
    const std::wstring newPath = dir + L"\\" + newName + L".json";
    if (_wcsicmp(path.c_str(), newPath.c_str()) != 0
        && GetFileAttributesW(newPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        err = "目标文件已存在，无法重命名";
        return false;
    }

    auto content = ReadAll(path);
    if (content.empty()) {
        err = "empty file";
        return false;
    }
    content = UpdateJsonStringField(content, L"scriptName", newName);

    // 先写到目标路径再删源文件，避免「已改 scriptName 但 MoveFile 失败」只改显示名
    if (_wcsicmp(path.c_str(), newPath.c_str()) != 0) {
        if (GetFileAttributesW(newPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            err = "目标文件已存在，无法重命名";
            return false;
        }
        std::ofstream out(newPath, std::ios::binary);
        if (!out) {
            err = "write failed";
            return false;
        }
        out.write("\xEF\xBB\xBF", 3);
        const auto bytes = ToUtf8(content);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out.good()) {
            out.close();
            DeleteFileW(newPath.c_str());
            err = "write failed";
            return false;
        }
        out.close();
        if (!DeleteFileW(path.c_str())) {
            // 新文件已写好；旧文件删不掉时仍以新路径为准
        }
        outNewPathUtf8 = ToUtf8(newPath);
    } else {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            err = "write failed";
            return false;
        }
        out.write("\xEF\xBB\xBF", 3);
        const auto bytes = ToUtf8(content);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out.good()) {
            err = "write failed";
            return false;
        }
        out.close();
        outNewPathUtf8 = ToUtf8(path);
    }
    return true;
}

bool ImportScriptFile(bool toRecordings, std::wstring& outPath, std::string& err) {
    wchar_t fileBuf[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetForegroundWindow();
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter =
        L"脚本文件 (*.zip;*.json)\0*.zip;*.json\0ZIP 脚本包 (*.zip)\0*.zip\0JSON 脚本 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) {
        err = "cancelled";
        return false;
    }
    EnsureScriptsDir();
    const std::wstring src = fileBuf;
    std::wstring lower = src;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    const bool isZip = lower.size() >= 4 && lower.substr(lower.size() - 4) == L".zip";

    auto safeName = [](std::wstring name) {
        if (Trim(name).empty()) name = TimestampName();
        for (wchar_t& ch : name) {
            if (wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
        }
        return name;
    };
    auto uniqueTarget = [&](const std::wstring& baseDir, const std::wstring& scriptName) {
        std::wstring target = baseDir + L"\\" + safeName(scriptName) + L".json";
        int suffix = 1;
        while (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            target = baseDir + L"\\" + safeName(scriptName) + L"-" + std::to_wstring(suffix++) + L".json";
        }
        return target;
    };

    const std::wstring destDir = toRecordings ? RecordingsDir() : ScriptsDir();
    CreateDirectoryW(destDir.c_str(), nullptr);

    if (!isZip) {
        const auto content = ReadAll(src);
        ScriptFileData data = ParseScriptContent(content);
        if (data.scriptName.empty()) {
            // 纯 CopyFile 回退：无 scriptName 时仍复制
            const auto slash = src.find_last_of(L"\\/");
            std::wstring name = slash == std::wstring::npos ? src : src.substr(slash + 1);
            outPath = destDir + L"\\" + name;
            if (GetFileAttributesW(outPath.c_str()) != INVALID_FILE_ATTRIBUTES)
                outPath = destDir + L"\\" + TimestampName() + L"-" + name;
            if (!CopyFileW(src.c_str(), outPath.c_str(), FALSE)) {
                err = "CopyFile failed";
                return false;
            }
            return true;
        }
        if (toRecordings) {
            data.windowMode = windowmode::DefaultWindowModeConfig();
            data.breakoutTimeSeconds = 0;
        }
        data.recordTime = NowText();
        outPath = uniqueTarget(destDir, data.scriptName);
        if (!SaveScriptFileData(outPath, data)) {
            err = "SaveScriptFileData failed";
            return false;
        }
        return true;
    }

    // ZIP：script.json + 图片（对齐原生 ImportScriptFromZipFile）
    std::string jsonUtf8 = ReadTextFromZip(src, "script.json");
    if (jsonUtf8.empty()) {
        err = "ZIP missing script.json";
        return false;
    }
    std::wstring content = FromUtf8(jsonUtf8);
    std::wstring name = ExtractString(content, L"scriptName");
    if (name.empty()) {
        err = "invalid scriptName";
        return false;
    }

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    const std::wstring extractDir = std::wstring(tempDir) + L"qs_import_" + std::to_wstring(GetTickCount());
    if (ExtractZipFile(src, extractDir) < 0) {
        err = "ExtractZipFile failed";
        return false;
    }

    EnsureFindImagesDir();
    std::wstring modifiedContent = content;
    const auto imgDir = FindImagesDir();
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW((extractDir + L"\\*").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring fileName(fd.cFileName);
            if (_wcsicmp(fileName.c_str(), L"script.json") == 0) continue;
            auto dotPos = fileName.find_last_of(L'.');
            if (dotPos == std::wstring::npos) continue;
            std::wstring ext = fileName.substr(dotPos);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
            if (ext != L".bmp" && ext != L".png" && ext != L".jpg" && ext != L".jpeg") continue;

            std::wstring extractedFile = extractDir + L"\\" + fileName;
            std::wstring destPath = imgDir + L"\\" + fileName;
            if (GetFileAttributesW(destPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                destPath = imgDir + L"\\" + fileName.substr(0, dotPos) + L"_"
                    + std::to_wstring(GetTickCount()) + ext;
            }
            if (!CopyFileW(extractedFile.c_str(), destPath.c_str(), FALSE)) continue;

            const std::wstring relPath = ImagePathForJson(destPath);
            RemapImportedImagePathInScriptJson(modifiedContent, fileName, relPath);
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    // cleanup temp
    WIN32_FIND_DATAW fd2{};
    HANDLE hFind2 = FindFirstFileW((extractDir + L"\\*").c_str(), &fd2);
    if (hFind2 != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                DeleteFileW((extractDir + L"\\" + fd2.cFileName).c_str());
        } while (FindNextFileW(hFind2, &fd2));
        FindClose(hFind2);
    }
    RemoveDirectoryW(extractDir.c_str());

    ScriptFileData importData = ParseScriptContent(modifiedContent);
    if (importData.scriptName.empty()) importData.scriptName = name;
    if (toRecordings) {
        importData.windowMode = windowmode::DefaultWindowModeConfig();
        importData.breakoutTimeSeconds = 0;
    }
    importData.recordTime = NowText();
    outPath = uniqueTarget(destDir, importData.scriptName);
    if (!SaveScriptFileData(outPath, importData)) {
        err = "SaveScriptFileData failed";
        return false;
    }
    return true;
}

bool ExportScriptFile(const std::string& pathUtf8, std::string& err,
    int* outSkipped, std::string* outSkippedFilesJson) {
    if (outSkipped) *outSkipped = 0;
    if (outSkippedFilesJson) outSkippedFilesJson->clear();
    std::wstring path;
    if (!ResolveScriptPath(pathUtf8, path, err)) {
        err.clear();
        path = FromUtf8(pathUtf8);
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            err = "file not found";
            return false;
        }
    }
    const auto content = ReadAll(path);
    const auto imgPaths = CollectImagePathsFromJson(content);
    ScriptFileData data = ParseScriptContent(content);
    std::wstring baseName = data.scriptName.empty()
        ? ([&]() {
            const auto slash = path.find_last_of(L"\\/");
            std::wstring n = slash == std::wstring::npos ? path : path.substr(slash + 1);
            const auto dot = n.find_last_of(L'.');
            if (dot != std::wstring::npos) n = n.substr(0, dot);
            return n;
        })()
        : data.scriptName;
    for (wchar_t& ch : baseName) {
        if (wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
    }
    if (baseName.empty()) baseName = TimestampName();

    if (!imgPaths.empty()) {
        wchar_t fileBuf[MAX_PATH]{};
        wcsncpy_s(fileBuf, (baseName + L".zip").c_str(), _TRUNCATE);
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = GetForegroundWindow();
        ofn.lpstrFile = fileBuf;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrFilter = L"ZIP 脚本包 (*.zip)\0*.zip\0所有文件 (*.*)\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        ofn.lpstrDefExt = L"zip";
        if (!GetSaveFileNameW(&ofn)) {
            err = "cancelled";
            return false;
        }
        std::wstring zipPath(fileBuf);
        if (zipPath.size() < 4 || _wcsicmp(zipPath.substr(zipPath.size() - 4).c_str(), L".zip") != 0)
            zipPath += L".zip";
        std::vector<std::pair<std::wstring, std::wstring>> files;
        files.push_back({L"script.json", path});
        for (const auto& imgPath : imgPaths) {
            const auto slashPos = imgPath.find_last_of(L"\\/");
            std::wstring imgName = (slashPos == std::wstring::npos) ? imgPath : imgPath.substr(slashPos + 1);
            files.push_back({imgName, imgPath});
        }
        const auto zipResult = CreateZipFile(zipPath, files, path);
        if (!zipResult.success) {
            err = "CreateZipFile failed";
            return false;
        }
        if (outSkipped) *outSkipped = static_cast<int>(zipResult.skippedFiles.size());
        if (outSkippedFilesJson) {
            std::ostringstream oss;
            oss << "[";
            for (size_t i = 0; i < zipResult.skippedFiles.size(); ++i) {
                if (i) oss << ",";
                oss << "\"" << EscapeJson(ToUtf8(zipResult.skippedFiles[i])) << "\"";
            }
            oss << "]";
            *outSkippedFilesJson = oss.str();
        }
        return true;
    }

    wchar_t fileBuf[MAX_PATH]{};
    wcsncpy_s(fileBuf, (baseName + L".json").c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetForegroundWindow();
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"JSON Scripts\0*.json\0All\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_EXPLORER;
    ofn.lpstrDefExt = L"json";
    if (!GetSaveFileNameW(&ofn)) {
        err = "cancelled";
        return false;
    }
    if (!CopyFileW(path.c_str(), fileBuf, FALSE)) {
        err = "CopyFile failed";
        return false;
    }
    return true;
}

bool SetScriptHotkeyJson(const std::string& pathUtf8, const std::string& hotkeyTextUtf8,
    UINT vk, UINT modifiers, bool hold, std::string& err) {
    std::wstring path;
    if (!ResolveScriptPath(pathUtf8, path, err)) {
        err.clear();
        path = FromUtf8(pathUtf8);
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            err = "file not found";
            return false;
        }
    }
    auto content = ReadAll(path);
    if (content.empty()) {
        err = "empty";
        return false;
    }
    content = UpdateJsonStringField(content, L"hotkeyText", FromUtf8(hotkeyTextUtf8));
    // numeric fields: rewrite via Load/Save for reliability
    ScriptFileData data = LoadScriptFileData(path, false);
    data.hotkey.text = FromUtf8(hotkeyTextUtf8);
    data.hotkey.vk = vk;
    data.hotkey.modifiers = modifiers;
    data.hotkey.holdMode = hold;
    data.hotkey.enabled = vk != 0;
    if (!SaveScriptFileData(path, data)) {
        err = "save failed";
        return false;
    }
    return true;
}

bool SetRecorderInputMode(int mode, std::string& err) {
    EnsureSettingsLoaded();
    std::lock_guard<std::mutex> lock(g_mu);
    g_ctx.settings.home.recorderInputMode = std::clamp(mode, 0, 3);
    if (!SaveAppSettings(g_ctx.settings)) {
        err = "SaveAppSettings failed";
        return false;
    }
    return true;
}

namespace {

std::string MessagesToJson(const std::vector<ChatMessage>& msgs) {
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    for (const auto& m : msgs) {
        if (m.role == L"system") continue;
        if (m.role == L"tool") continue;
        // 内部引导消息（重复提示/图片参考等）不显示给用户
        if (m.internal_nudge) continue;
        if (!first) oss << ",";
        first = false;
        const bool isUser = (m.role == L"user");
        std::wstring content = m.content;
        if (content.empty() && !m.parts.empty()) {
            for (const auto& p : m.parts) {
                if (p.type == L"text") content += p.text;
            }
        }
        oss << "{"
            << "\"role\":" << JsonString(m.role) << ","
            << "\"content\":" << JsonString(content) << ","
            << "\"who\":" << JsonString(isUser ? L"你" : L"AI助手");
        if (!isUser && !m.reasoning_content.empty())
            oss << ",\"reasoning\":" << JsonString(m.reasoning_content);
        oss << "}";
    }
    oss << "]";
    return oss.str();
}

AgentConfig BuildCfgFromSettings(const quickscript::AiApiSettings& ai) {
    AgentConfig cfg;
    cfg.apiUrl = ai.apiUrl;
    cfg.apiKey = ai.apiKey;
    cfg.model = ai.modelName;
    cfg.temperature = ai.temperature;
    cfg.maxTokens = ai.maxTokens;
    if (cfg.maxTokens > 393216) cfg.maxTokens = 393216;
    if (cfg.maxTokens < 1) cfg.maxTokens = 4096;
    // 聊天助手输出上限 8192：脚本 JSON 加总结足够；更重要的是思考型模型
    // 会把 max_tokens 当思考预算，32768 实测可让单轮思考拖到数分钟（表现为「卡死」）。
    if (cfg.maxTokens > 8192) cfg.maxTokens = 8192;
    // 单轮最坏等待必须有界：无上限的 maxTokens×25ms 会把超时抬到
    // 17 分钟（40969 tokens）甚至数小时（393216 tokens），
    // 配合思考模式/慢响应就表现为「卡死」。上限 5 分钟，够长输出也够失败兜底。
    const int scaledTimeout = cfg.maxTokens > 0
        ? std::min(cfg.maxTokens * 25, 300000) : 0;
    cfg.recvTimeoutMs = std::min(300000, (std::max)({120000, 180000, scaledTimeout}));
    return cfg;
}

void RebuildAgentCoreLocked() {
    EnsureSettingsLoaded();
    const auto cfg = BuildCfgFromSettings(g_ctx.settings.ai);
    const std::wstring systemPrompt = BuildAgentSystemPrompt(cfg.model, cfg.apiUrl);
    const auto tools = BuildDefaultAgentTools();
    if (g_agent.core) {
        g_agent.core->UpdateConfig(cfg, systemPrompt);
        g_agent.core->UpdateTools(tools);
    } else {
        g_agent.core = std::make_shared<AgentCore>(cfg, systemPrompt, tools);
    }
}

void PersistAgentSession() {
    if (!g_agent.core || g_agent.id.empty()) return;
    AgentConversationSavePayload payload;
    payload.messages = g_agent.core->GetHistory();
    payload.roundCount = CountConversationRounds(payload.messages);
    payload.shouldSave = payload.roundCount > 0;
    if (!payload.shouldSave) return;
    payload.id = g_agent.id;
    payload.createdTime = g_agent.createdTime.empty() ? NowText() : g_agent.createdTime;
    // 占位名；正式名由首轮后的 AI 总结写入
    if (g_agent.name.empty()) g_agent.name = L"新对话";
    payload.name = g_agent.name;
    SaveAgentConversation(payload);
}

bool NeedsAiConversationTitle(const std::wstring& name) {
    return name.empty() || name == L"新对话";
}

std::wstring ClipForTitle(const std::wstring& s, size_t maxChars) {
    if (s.size() <= maxChars) return s;
    return s.substr(0, maxChars) + L"…";
}

std::wstring FirstUserTextFromHistory(const std::vector<ChatMessage>& msgs) {
    for (const auto& m : msgs) {
        if (m.role != L"user") continue;
        if (!m.content.empty()) return m.content;
        std::wstring t;
        for (const auto& p : m.parts) {
            if (p.type == L"text") t += p.text;
        }
        if (!t.empty()) return t;
    }
    return {};
}

std::wstring SanitizeAiConversationTitle(std::wstring raw) {
    // 去掉常见包装
    while (!raw.empty() && (raw.front() == L'"' || raw.front() == L'\'' || raw.front() == L'「' || raw.front() == L'《'))
        raw.erase(raw.begin());
    while (!raw.empty() && (raw.back() == L'"' || raw.back() == L'\'' || raw.back() == L'」' || raw.back() == L'》'
        || raw.back() == L'。' || raw.back() == L'.' || raw.back() == L'!' || raw.back() == L'！'))
        raw.pop_back();
    const auto nl = raw.find_first_of(L"\r\n");
    if (nl != std::wstring::npos) raw = raw.substr(0, nl);
    // 去掉 markdown 加粗
    raw.erase(std::remove(raw.begin(), raw.end(), L'*'), raw.end());
    raw.erase(std::remove(raw.begin(), raw.end(), L'#'), raw.end());
    while (!raw.empty() && iswspace(raw.front())) raw.erase(raw.begin());
    while (!raw.empty() && iswspace(raw.back())) raw.pop_back();
    if (raw.size() > 16) raw = raw.substr(0, 16);
    return raw;
}

void RequestAiConversationTitleAsync(AgentConfig cfg, std::wstring id,
    std::wstring userText, std::wstring replyText) {
    std::thread([cfg = std::move(cfg), id = std::move(id),
                 userText = std::move(userText), replyText = std::move(replyText)]() {
        std::wstring title;
        try {
            AgentCore titleCore(
                cfg,
                L"根据对话内容起一个简短中文标题。只输出标题本身，不超过16个字，不要引号、不要解释、不要标点收尾。",
                {});
            AgentSendCallbacks cb;
            cb.preferNonStream = true;
            ChatMessage u;
            u.role = L"user";
            u.content = L"用户：" + ClipForTitle(userText, 240) + L"\n助手：" + ClipForTitle(replyText, 360);
            title = SanitizeAiConversationTitle(titleCore.SendMessage(u, cb));
        } catch (...) {
            title.clear();
        }
        if (title.empty() || NeedsAiConversationTitle(title)) {
            // AI 失败：回退本地摘要，避免一直停在「新对话」
            title = DeriveConversationTitleFromPrompt(userText);
        }
        if (title.empty() || NeedsAiConversationTitle(title)) return;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (g_agent.id == id) g_agent.name = title;
        }
        SetAgentConversationName(id, title);
        PostToWebUi(std::string("{\"type\":\"agentConversation.title\",\"ok\":true,\"id\":\"")
            + EscapeJson(ToUtf8(id)) + "\",\"name\":\"" + EscapeJson(ToUtf8(title))
            + "\",\"conversations\":" + JsonListAgentConversations() + "}");
    }).detach();
}

// 会话草稿字段（未发送文本/附件/编辑态）的 JSON 片段，前缀逗号。
std::string JsonAgentDraftFields(const AgentConversationRecord& rec) {
    std::string atts = "[";
    for (size_t i = 0; i < rec.attachmentPaths.size(); ++i) {
        if (i) atts += ",";
        atts += JsonString(rec.attachmentPaths[i]);
    }
    atts += "]";
    return ",\"draft\":" + JsonString(rec.draft)
        + ",\"attachments\":" + atts
        + ",\"editIndex\":" + std::to_string(rec.editIndex);
}

}  // namespace

std::string JsonOpenAgentConversation(const std::string& idUtf8, bool createNew, bool peekOnly) {
    std::lock_guard<std::mutex> lock(g_mu);
    EnsureSettingsLoaded();

    // peek：只读磁盘，不切换活跃会话（多 Tab 打开其它对话、或 busy 时安全查看）
    if (peekOnly && !createNew && !idUtf8.empty()) {
        std::wstring id = FromUtf8(idUtf8);
        if (id.size() > 5 && id.substr(id.size() - 5) == L".json") id = id.substr(0, id.size() - 5);
        AgentConversationRecord rec;
        if (!LoadAgentConversationRecord(id, rec)) {
            return std::string("{\"ok\":false,\"detail\":\"conversation not found\"}");
        }
        std::ostringstream oss;
        oss << "{"
            << "\"id\":" << JsonString(rec.meta.id) << ","
            << "\"name\":" << JsonString(rec.meta.name) << ","
            << "\"modelName\":" << JsonString(g_ctx.settings.ai.modelName) << ","
            << "\"messages\":" << MessagesToJson(rec.messages)
            << JsonAgentDraftFields(rec) << ","
            << "\"peek\":true"
            << "}";
        return oss.str();
    }

    // busy 且请求其它会话：自动降级为 peek，避免打断流式中的 core
    if (!createNew && !idUtf8.empty() && g_agent.busy.load()) {
        std::wstring want = FromUtf8(idUtf8);
        if (want.size() > 5 && want.substr(want.size() - 5) == L".json")
            want = want.substr(0, want.size() - 5);
        if (g_agent.id != want) {
            AgentConversationRecord rec;
            if (!LoadAgentConversationRecord(want, rec)) {
                return std::string("{\"ok\":false,\"detail\":\"conversation not found\"}");
            }
            std::ostringstream oss;
            oss << "{"
                << "\"id\":" << JsonString(rec.meta.id) << ","
                << "\"name\":" << JsonString(rec.meta.name) << ","
                << "\"modelName\":" << JsonString(g_ctx.settings.ai.modelName) << ","
                << "\"messages\":" << MessagesToJson(rec.messages)
                << JsonAgentDraftFields(rec) << ","
                << "\"peek\":true"
                << "}";
            return oss.str();
        }
    }

    RebuildAgentCoreLocked();
    if (createNew || idUtf8.empty()) {
        if (g_agent.busy.load()) {
            return std::string("{\"ok\":false,\"detail\":\"助手忙碌中\"}");
        }
        g_agent.id = TimestampName();
        g_agent.name.clear();
        g_agent.createdTime = NowText();
        g_agent.titleAiDone = false;
        g_agent.core->ClearHistory();
        std::ostringstream oss;
        oss << "{"
            << "\"id\":" << JsonString(g_agent.id) << ","
            << "\"name\":" << JsonString(L"新对话") << ","
            << "\"modelName\":" << JsonString(g_ctx.settings.ai.modelName) << ","
            << "\"messages\":[]"
            << ",\"draft\":\"\",\"attachments\":[],\"editIndex\":-1"
            << "}";
        return oss.str();
    }
    std::wstring id = FromUtf8(idUtf8);
    if (id.size() > 5 && id.substr(id.size() - 5) == L".json") id = id.substr(0, id.size() - 5);
    // 路径穿越防护：会话 id 直接拼进文件名，拒绝一切路径分隔符/驱动符
    bool idUnsafe = false;
    for (wchar_t c : id) {
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"'
            || c == L'<' || c == L'>' || c == L'|') { idUnsafe = true; break; }
    }
    if (idUnsafe) {
        return std::string("{\"ok\":false,\"detail\":\"invalid conversation id\"}");
    }
    AgentConversationRecord rec;
    if (!LoadAgentConversationRecord(id, rec)) {
        return std::string("{\"ok\":false,\"detail\":\"conversation not found\"}");
    }
    g_agent.id = rec.meta.id;
    g_agent.name = rec.meta.name.empty() ? L"新对话" : rec.meta.name;
    g_agent.createdTime = rec.meta.createdTime;
    g_agent.titleAiDone = !NeedsAiConversationTitle(g_agent.name);
    g_agent.core->SetFullHistory(std::move(rec.messages));
    // SetFullHistory 可能带回磁盘上的旧 system；立刻用当前提示覆盖
    {
        const auto cfg = BuildCfgFromSettings(g_ctx.settings.ai);
        g_agent.core->UpdateConfig(cfg, BuildAgentSystemPrompt(cfg.model, cfg.apiUrl));
    }
    std::ostringstream oss;
    oss << "{"
        << "\"id\":" << JsonString(g_agent.id) << ","
        << "\"name\":" << JsonString(g_agent.name) << ","
        << "\"modelName\":" << JsonString(g_ctx.settings.ai.modelName) << ","
        << "\"messages\":" << MessagesToJson(g_agent.core->GetHistory())
        << JsonAgentDraftFields(rec)
        << "}";
    return oss.str();
}

bool DeleteAgentConversationById(const std::string& idUtf8, std::string& err) {
    std::wstring id = FromUtf8(idUtf8);
    if (id.size() > 5 && id.substr(id.size() - 5) == L".json") id = id.substr(0, id.size() - 5);
    // 路径穿越防护：会话 id 直接拼进文件名，拒绝一切路径分隔符/驱动符
    for (wchar_t c : id) {
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"'
            || c == L'<' || c == L'>' || c == L'|') {
            err = "invalid conversation id";
            return false;
        }
    }
    if (id.empty()) {
        err = "empty id";
        return false;
    }
    if (!DeleteAgentConversation(id)) {
        err = "delete failed";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_agent.id == id) {
        g_agent.id.clear();
        g_agent.name.clear();
        if (g_agent.core) g_agent.core->ClearHistory();
    }
    return true;
}

bool SaveAgentDraft(const std::string& msgJson, std::string& err) {
    err.clear();
    try {
        const json j = json::parse(msgJson);
        const std::wstring id = FromUtf8(j.value("id", ""));
        if (id.empty()) {
            err = "empty id";
            return false;
        }
        std::vector<std::wstring> paths;
        if (j.contains("attachments") && j["attachments"].is_array()) {
            for (const auto& p : j["attachments"]) {
                if (p.is_string() && !p.get<std::string>().empty())
                    paths.push_back(FromUtf8(p.get<std::string>()));
            }
        }
        const int editIndex = j.value("editIndex", -1);
        const std::wstring text = FromUtf8(j.value("text", ""));
        if (!SaveAgentConversationDraft(id, text, paths, editIndex)) {
            err = "save draft failed";
            return false;
        }
        return true;
    } catch (const json::parse_error&) {
        err = "invalid json";
        return false;
    }
}

std::string JsonListAgentChanges() {
    return ToUtf8(AgentChangesToJson(50));
}

bool RevertAgentChangeById(const std::string& idUtf8, std::string& err) {
    std::wstring werr;
    const bool ok = RevertAgentChange(FromUtf8(idUtf8), werr);
    err = ToUtf8(werr);
    return ok;
}

namespace {

bool JsonGetStringField(const std::string& json, const char* key, std::string& out) {
    const std::string pat = std::string("\"") + key + "\"";
    const auto k = json.find(pat);
    if (k == std::string::npos) return false;
    const auto colon = json.find(':', k + pat.size());
    if (colon == std::string::npos) return false;
    const auto q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return false;
    std::string val;
    for (size_t i = q1 + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) { val.push_back(json[i + 1]); ++i; continue; }
        if (json[i] == '"') { out = val; return true; }
        val.push_back(json[i]);
    }
    return false;
}

std::vector<std::string> JsonGetStringArray(const std::string& json, const char* key) {
    std::vector<std::string> out;
    const std::string pat = std::string("\"") + key + "\"";
    const auto k = json.find(pat);
    if (k == std::string::npos) return out;
    const auto lb = json.find('[', k + pat.size());
    if (lb == std::string::npos) return out;
    size_t i = lb + 1;
    while (i < json.size()) {
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r' || json[i] == ',')) ++i;
        if (i >= json.size() || json[i] == ']') break;
        if (json[i] != '"') break;
        ++i;
        std::string val;
        for (; i < json.size(); ++i) {
            if (json[i] == '\\' && i + 1 < json.size()) { val.push_back(json[i + 1]); ++i; continue; }
            if (json[i] == '"') { out.push_back(val); ++i; break; }
            val.push_back(json[i]);
        }
    }
    return out;
}

}  // namespace

bool BeginSendAgentMessage(const std::string& msgJson, std::string& err) {
    std::string textUtf8, idUtf8, modelUtf8, panelKeyUtf8;
    int rewindUserIndex = -1;
    JsonGetStringField(msgJson, "text", textUtf8);
    JsonGetStringField(msgJson, "id", idUtf8);
    JsonGetStringField(msgJson, "model", modelUtf8);
    JsonGetStringField(msgJson, "panelKey", panelKeyUtf8);
    {
        const std::string pat = "\"rewindUserIndex\"";
        const auto k = msgJson.find(pat);
        if (k != std::string::npos) {
            const auto colon = msgJson.find(':', k + pat.size());
            if (colon != std::string::npos) {
                size_t i = colon + 1;
                while (i < msgJson.size() && (msgJson[i] == ' ' || msgJson[i] == '\t'
                    || msgJson[i] == '\r' || msgJson[i] == '\n')) ++i;
                int sign = 1;
                if (i < msgJson.size() && msgJson[i] == '-') { sign = -1; ++i; }
                int val = 0;
                while (i < msgJson.size() && msgJson[i] >= '0' && msgJson[i] <= '9') {
                    val = val * 10 + (msgJson[i] - '0');
                    if (val > 100000) break;
                    ++i;
                }
                rewindUserIndex = sign * val;
            }
        }
    }
    if (textUtf8.empty()) {
        const auto paths = JsonGetStringArray(msgJson, "attachments");
        if (paths.empty()) {
            err = "empty text";
            return false;
        }
    }

    if (idUtf8.empty()) {
        JsonOpenAgentConversation("", true);
    } else {
        bool needOpen = false;
        {
            std::wstring want = FromUtf8(idUtf8);
            if (want.size() > 5 && want.substr(want.size() - 5) == L".json")
                want = want.substr(0, want.size() - 5);
            std::lock_guard<std::mutex> lock(g_mu);
            needOpen = (!g_agent.core || g_agent.id != want);
        }
        if (needOpen) {
            const std::string opened = JsonOpenAgentConversation(idUtf8, false);
            if (opened.find("conversation not found") != std::string::npos) {
                err = "conversation not found";
                return false;
            }
        }
    }

    std::vector<AgentPendingAttachment> attachments;
    for (const auto& pathUtf8 : JsonGetStringArray(msgJson, "attachments")) {
        if (pathUtf8.empty()) continue;
        AgentPendingAttachment item;
        std::wstring loadErr;
        if (AgentLoadAttachmentFromPath(FromUtf8(pathUtf8), item, loadErr)) {
            attachments.push_back(std::move(item));
        }
    }

    std::shared_ptr<AgentCore> core;
    std::shared_ptr<std::atomic<bool>> cancel;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        EnsureSettingsLoaded();
        if (!modelUtf8.empty()) g_ctx.settings.ai.modelName = FromUtf8(modelUtf8);
        // 按当前模型名从 savedModels 回填密钥/地址（顶层可能为空）
        if (!g_ctx.settings.ai.savedModels.empty()) {
            const quickscript::AiModelProfile* hit = nullptr;
            for (const auto& m : g_ctx.settings.ai.savedModels) {
                if (!g_ctx.settings.ai.modelName.empty() && m.modelName == g_ctx.settings.ai.modelName) {
                    hit = &m;
                    break;
                }
            }
            if (!hit) hit = &g_ctx.settings.ai.savedModels.front();
            if (hit) {
                if (g_ctx.settings.ai.modelName.empty()) g_ctx.settings.ai.modelName = hit->modelName;
                if (g_ctx.settings.ai.apiKey.empty() && !hit->apiKey.empty())
                    g_ctx.settings.ai.apiKey = hit->apiKey;
                if (g_ctx.settings.ai.apiUrl.empty() && !hit->apiUrl.empty())
                    g_ctx.settings.ai.apiUrl = hit->apiUrl;
                if (hit->temperature > 0) g_ctx.settings.ai.temperature = hit->temperature;
                if (hit->maxTokens > 0) g_ctx.settings.ai.maxTokens = hit->maxTokens;
            }
        }
        if (!g_ctx.settings.ai.enabled) {
            err = "请先在「设置 → AI助手」中启用 AI 脚本助手";
            return false;
        }
        if (g_ctx.settings.ai.apiUrl.empty()) {
            err = "请先在「设置 → AI助手」中配置 API 地址";
            return false;
        }
        if (g_ctx.settings.ai.apiKey.empty()) {
            err = "请先在「设置 → AI助手」中配置 API 密钥";
            return false;
        }
        if (g_ctx.settings.ai.modelName.empty()) {
            err = "请先在「设置 → AI助手」中配置模型名称";
            return false;
        }
        if (g_agent.busy.load()) {
            err = "助手忙碌中";
            return false;
        }
        RebuildAgentCoreLocked();
        if (g_agent.id.empty()) {
            g_agent.id = TimestampName();
            g_agent.createdTime = NowText();
        }
        if (rewindUserIndex >= 0) {
            if (!g_agent.core
                || !g_agent.core->TruncateHistoryToUserRound(
                    static_cast<size_t>(rewindUserIndex))) {
                err = "编辑的消息序号无效，请刷新对话后重试";
                return false;
            }
            if (rewindUserIndex == 0) {
                g_agent.name.clear();
                g_agent.titleAiDone = false;
            }
        }
        g_agent.busy = true;
        g_agent.cancelFlag = std::make_shared<std::atomic<bool>>(false);
        cancel = g_agent.cancelFlag;
        core = g_agent.core;
    }
    if (!core) {
        err = "agent core missing";
        return false;
    }

    const std::wstring userText = FromUtf8(textUtf8);
    SetAgentToolUserContext(userText);
    ChatMessage userMsg = AgentBuildUserMessage(userText, attachments);
    AgentReleaseAttachmentBitmaps(attachments);
    std::string streamIdUtf8;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        streamIdUtf8 = ToUtf8(g_agent.id);
    }
    std::thread([core, cancel, userMsg = std::move(userMsg), streamIdUtf8, panelKeyUtf8]() {
        const auto idJson = [&]() {
            return std::string("\"id\":\"") + EscapeJson(streamIdUtf8) + "\"";
        };
        const auto panelJson = [&]() {
            return panelKeyUtf8.empty()
                ? std::string()
                : (std::string(",\"panelKey\":\"") + EscapeJson(panelKeyUtf8) + "\"");
        };
        AgentSendCallbacks callbacks;
        callbacks.cancelFlag = cancel.get();
        callbacks.httpAbort = &g_agent.httpAbort;
        callbacks.onContentDelta = [idJson, panelJson](const std::wstring& delta) {
            PostToWebUi(std::string("{\"type\":\"sendAgentMessage.delta\",\"ok\":true,")
                + idJson() + panelJson() + ",\"delta\":\"" + EscapeJson(ToUtf8(delta)) + "\"}");
        };
        callbacks.onStatus = [idJson, panelJson](const std::wstring& status) {
            PostToWebUi(std::string("{\"type\":\"sendAgentMessage.status\",\"ok\":true,")
                + idJson() + panelJson() + ",\"status\":\"" + EscapeJson(ToUtf8(status)) + "\"}");
        };
        callbacks.onToolCall = [idJson, panelJson](const std::wstring& name, const std::wstring& args) {
            PostToWebUi(std::string("{\"type\":\"sendAgentMessage.tool\",\"ok\":true,")
                + idJson() + panelJson() + ",\"name\":\"" + EscapeJson(ToUtf8(name))
                + "\",\"args\":\"" + EscapeJson(ToUtf8(args)) + "\"}");
        };
        callbacks.onReasoningDelta = [idJson, panelJson](const std::wstring& delta) {
            PostToWebUi(std::string("{\"type\":\"sendAgentMessage.reasoningDelta\",\"ok\":true,")
                + idJson() + panelJson() + ",\"delta\":\"" + EscapeJson(ToUtf8(delta)) + "\"}");
        };
        callbacks.onReasoning = [idJson, panelJson](const std::wstring& reasoning) {
            PostToWebUi(std::string("{\"type\":\"sendAgentMessage.reasoning\",\"ok\":true,")
                + idJson() + panelJson() + ",\"reasoning\":\"" + EscapeJson(ToUtf8(reasoning)) + "\"}");
        };

        std::wstring response;
        try {
            response = core->SendMessage(userMsg, callbacks);
        } catch (...) {
            response = L"请求异常";
        }

        std::string outId, outName;
        bool requestTitle = false;
        AgentConfig titleCfg;
        std::wstring titleId, titleUser, titleReply;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            outId = ToUtf8(g_agent.id);
            const bool failLocal = response.empty()
                || response.rfind(L"[错误]", 0) == 0;
            // 首轮成功：立刻用本地摘要标题（勿干等 AI 总结失败一直「新对话」）
            if (!failLocal && NeedsAiConversationTitle(g_agent.name) && g_agent.core
                && CountConversationRounds(g_agent.core->GetHistory()) >= 1) {
                const std::wstring localTitle =
                    SummarizeConversationName(g_agent.core->GetHistory());
                if (!NeedsAiConversationTitle(localTitle)) {
                    g_agent.name = localTitle;
                }
                if (!g_agent.titleAiDone) {
                    g_agent.titleAiDone = true;
                    requestTitle = true;
                    titleCfg = BuildCfgFromSettings(g_ctx.settings.ai);
                    titleId = g_agent.id;
                    titleUser = FirstUserTextFromHistory(g_agent.core->GetHistory());
                    titleReply = response;
                }
            }
            // 先改名再落盘，保证列表/关窗刷新不是「新对话」
            PersistAgentSession();
            outName = ToUtf8(g_agent.name);
            g_agent.busy = false;
        }

        const bool fail = response.empty();
        std::ostringstream oss;
        oss << "{\"type\":\"sendAgentMessage.result\",\"ok\":" << (fail ? "false" : "true") << ","
            << "\"id\":\"" << EscapeJson(outId) << "\","
            << "\"name\":\"" << EscapeJson(outName) << "\""
            << panelJson() << ","
            << "\"reply\":\"" << EscapeJson(ToUtf8(response)) << "\","
            << "\"conversations\":" << JsonListAgentConversations() << ","
            << "\"scripts\":" << JsonListScripts() << ","
            << "\"recordings\":" << JsonListRecordings() << "}";
        PostToWebUi(oss.str());
        if (requestTitle)
            RequestAiConversationTitleAsync(std::move(titleCfg), std::move(titleId),
                std::move(titleUser), std::move(titleReply));
    }).detach();

    return true;
}

bool CancelAgentMessage(std::string& err) {
    err.clear();
    std::shared_ptr<AgentCore> core;
    std::shared_ptr<std::atomic<bool>> cancel;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_agent.busy.load()) {
            err = "not busy";
            return false;
        }
        cancel = g_agent.cancelFlag;
        core = g_agent.core;
    }
    if (cancel) cancel->store(true);
    g_agent.httpAbort.Abort();
    if (core) core->AbortActiveHttp();
    return true;
}

namespace {

bool DecodeBase64ToBytes(const std::string& b64, std::vector<uint8_t>& out) {
    out.clear();
    if (b64.empty()) return false;
    // 约 16MiB 原始数据上限，防恶意 data URL OOM
    constexpr size_t kMaxB64Chars = 22 * 1024 * 1024;
    constexpr DWORD kMaxDecoded = 16 * 1024 * 1024;
    if (b64.size() > kMaxB64Chars) return false;
    DWORD need = 0;
    if (!CryptStringToBinaryA(b64.c_str(), static_cast<DWORD>(b64.size()), CRYPT_STRING_BASE64,
            nullptr, &need, nullptr, nullptr) || need == 0) {
        return false;
    }
    if (need > kMaxDecoded) return false;
    out.resize(need);
    if (!CryptStringToBinaryA(b64.c_str(), static_cast<DWORD>(b64.size()), CRYPT_STRING_BASE64,
            out.data(), &need, nullptr, nullptr)) {
        out.clear();
        return false;
    }
    out.resize(need);
    return true;
}

bool WriteBytesToFile(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    const size_t n = fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    return n == bytes.size();
}

}  // namespace

bool PasteAgentClipboardAttachments(HWND owner, std::string& outPathsJson, std::string& err) {
    outPathsJson = "[]";
    err.clear();
    std::vector<std::wstring> filePaths;
    AgentPendingAttachment imageAttachment;
    bool hasImage = false;
    std::wstring werr;
    if (!AgentTryReadClipboardAttachments(owner ? owner : GetDesktopWindow(), filePaths,
            imageAttachment, hasImage, werr)) {
        err = werr.empty() ? "剪贴板无附件" : ToUtf8(werr);
        return false;
    }
    if (!werr.empty()) {
        AgentReleaseAttachmentBitmap(imageAttachment);
        err = ToUtf8(werr);
        return false;
    }

    std::ostringstream oss;
    oss << "[";
    bool first = true;
    auto addPath = [&](const std::wstring& p) {
        if (p.empty()) return;
        if (!first) oss << ",";
        first = false;
        oss << "\"" << EscapeJson(ToUtf8(p)) << "\"";
    };
    for (const auto& p : filePaths) addPath(p);

    if (hasImage) {
        EnsureFindImagesDir();
        const std::wstring dest =
            FindImagesDir() + L"\\agent_clip_" + std::to_wstring(GetTickCount64()) + L".jpg";
        std::vector<uint8_t> bytes;
        if (!DecodeBase64ToBytes(imageAttachment.base64, bytes) || !WriteBytesToFile(dest, bytes)) {
            AgentReleaseAttachmentBitmap(imageAttachment);
            err = "保存剪贴板图片失败";
            return false;
        }
        addPath(dest);
    }
    AgentReleaseAttachmentBitmap(imageAttachment);
    oss << "]";
    outPathsJson = oss.str();
    if (first) {
        err = "剪贴板无附件";
        return false;
    }
    return true;
}

bool SaveClipboardImageDataUrl(const std::string& dataUrlUtf8, const std::string& extHint,
    std::string& outPathUtf8, std::string& err) {
    outPathUtf8.clear();
    err.clear();
    const auto comma = dataUrlUtf8.find(',');
    if (comma == std::string::npos) {
        err = "invalid data url";
        return false;
    }
    const std::string header = dataUrlUtf8.substr(0, comma);
    const std::string b64 = dataUrlUtf8.substr(comma + 1);
    if (header.find("base64") == std::string::npos) {
        err = "data url not base64";
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!DecodeBase64ToBytes(b64, bytes) || bytes.empty()) {
        err = "base64 decode failed";
        return false;
    }
    std::string ext = extHint;
    if (ext != "png" && ext != "jpg" && ext != "jpeg") {
        ext = header.find("png") != std::string::npos ? "png" : "jpg";
    }
    if (ext == "jpeg") ext = "jpg";
    EnsureFindImagesDir();
    const std::wstring dest = FindImagesDir() + L"\\agent_clip_"
        + std::to_wstring(GetTickCount64()) + L"." + FromUtf8(ext);
    if (!WriteBytesToFile(dest, bytes)) {
        err = "write failed";
        return false;
    }
    outPathUtf8 = ToUtf8(dest);
    return true;
}

bool SaveAgentImageAsDialog(HWND owner, const std::string& pathUtf8, const std::string& dataUrlUtf8,
    std::string& err) {
    err.clear();
    std::wstring srcPath = FromUtf8(pathUtf8);
    std::vector<uint8_t> bytes;
    std::wstring defExt = L"png";
    std::wstring defName = L"image.png";

    if (!srcPath.empty() && GetFileAttributesW(srcPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const auto slash = srcPath.find_last_of(L"\\/");
        defName = (slash == std::wstring::npos) ? srcPath : srcPath.substr(slash + 1);
        const auto dot = defName.find_last_of(L'.');
        if (dot != std::wstring::npos && dot + 1 < defName.size())
            defExt = defName.substr(dot + 1);
    } else if (!dataUrlUtf8.empty()) {
        const auto comma = dataUrlUtf8.find(',');
        if (comma == std::string::npos) {
            err = "invalid data url";
            return false;
        }
        const std::string header = dataUrlUtf8.substr(0, comma);
        const std::string b64 = dataUrlUtf8.substr(comma + 1);
        if (header.find("base64") == std::string::npos) {
            err = "data url not base64";
            return false;
        }
        if (!DecodeBase64ToBytes(b64, bytes) || bytes.empty()) {
            err = "base64 decode failed";
            return false;
        }
        if (header.find("jpeg") != std::string::npos || header.find("jpg") != std::string::npos) {
            defExt = L"jpg";
            defName = L"image.jpg";
        } else if (header.find("webp") != std::string::npos) {
            defExt = L"webp";
            defName = L"image.webp";
        } else {
            defExt = L"png";
            defName = L"image.png";
        }
        if (!srcPath.empty()) {
            const auto slash = srcPath.find_last_of(L"\\/");
            std::wstring n = (slash == std::wstring::npos) ? srcPath : srcPath.substr(slash + 1);
            if (!n.empty() && n.find(L'.') != std::wstring::npos) defName = n;
        }
    } else {
        err = "no image source";
        return false;
    }

    for (wchar_t& ch : defName) {
        if (wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
    }
    if (defName.empty()) defName = L"image." + defExt;

    wchar_t fileBuf[MAX_PATH]{};
    wcsncpy_s(fileBuf, defName.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner ? owner : GetForegroundWindow();
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter =
        L"PNG 图片 (*.png)\0*.png\0JPEG 图片 (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0所有文件 (*.*)\0*.*\0";
    ofn.nFilterIndex = (_wcsicmp(defExt.c_str(), L"jpg") == 0 || _wcsicmp(defExt.c_str(), L"jpeg") == 0) ? 2 : 1;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrDefExt = defExt.c_str();
    ofn.lpstrTitle = L"Save Image As...";
    if (!GetSaveFileNameW(&ofn)) {
        err = "cancelled";
        return false;
    }
    std::wstring dest(fileBuf);
    if (!srcPath.empty() && GetFileAttributesW(srcPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (!CopyFileW(srcPath.c_str(), dest.c_str(), FALSE)) {
            err = "CopyFile failed";
            return false;
        }
        return true;
    }
    if (!WriteBytesToFile(dest, bytes)) {
        err = "write failed";
        return false;
    }
    return true;
}

namespace {

std::string ColorToCssHex(COLORREF c) {
    char buf[16];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X",
        GetRValue(c), GetGValue(c), GetBValue(c));
    return buf;
}

}  // namespace

std::string JsonEngineStatus() {
    // Lightweight placeholder; shell prefers qst::engine::* for real state.
    std::ostringstream oss;
    oss << "{\"clicking\":false"
        << ",\"running\":false,\"recording\":false}";
    return oss.str();
}

std::string JsonThemeCatalog() {
    std::ostringstream oss;
    oss << "[";
    const quickscript::AppTheme* cat = quickscript::ThemeCatalog();
    for (int i = 0; i < quickscript::kThemeCount; ++i) {
        if (i) oss << ",";
        const auto& t = cat[i];
        oss << "{\"id\":" << i
            << ",\"name\":\"" << EscapeJson(ToUtf8(t.name)) << "\""
            << ",\"main\":\"" << ColorToCssHex(t.mainColor) << "\""
            << ",\"accent\":\"" << ColorToCssHex(t.accentColor) << "\""
            << ",\"light\":\"" << ColorToCssHex(t.lightBg) << "\""
            << ",\"workspace\":\"" << ColorToCssHex(t.workspaceBg) << "\""
            << ",\"nav\":\"" << ColorToCssHex(t.navStripColor) << "\""
            << "}";
    }
    oss << "]";
    return oss.str();
}

bool ApplyThemeSettings(int themeId, bool useCustom, COLORREF main, COLORREF accent,
    std::string& err) {
    EnsureSettingsLoaded();
    auto& s = Ctx().settings;
    s.other.themeId = std::clamp(themeId, 0, quickscript::kThemeCount - 1);
    s.other.useCustomTheme = useCustom;
    if (useCustom) {
        s.other.customMainColor = static_cast<int>(main);
        s.other.customAccentColor = static_cast<int>(accent);
    }
    if (!SaveAppSettings(s)) {
        err = "SaveAppSettings failed";
        return false;
    }
    quickscript::ApplyThemeFromSettings(s);
    return true;
}

std::string JsonListScheduledTasks() {
    std::vector<ScheduledTask> tasks;
    bool globalDisabled = false;
    LoadScheduledTasks(tasks, &globalDisabled);
    std::ostringstream oss;
    oss << "{\"globalDisabled\":" << (globalDisabled ? "true" : "false") << ",\"tasks\":[";
    for (size_t i = 0; i < tasks.size(); ++i) {
        const auto& t = tasks[i];
        if (i) oss << ",";
        oss << "{"
            << "\"id\":\"" << EscapeJson(ToUtf8(t.id)) << "\","
            << "\"name\":\"" << EscapeJson(ToUtf8(t.name)) << "\","
            << "\"kind\":" << static_cast<int>(t.kind) << ","
            << "\"filePath\":\"" << EscapeJson(ToUtf8(t.filePath)) << "\","
            << "\"fileDisplayName\":\"" << EscapeJson(ToUtf8(t.fileDisplayName)) << "\","
            << "\"frequency\":" << static_cast<int>(t.frequency) << ","
            << "\"status\":" << static_cast<int>(t.status) << ","
            << "\"customFired\":" << (t.customFired ? "true" : "false") << ","
            << "\"year\":" << t.time.year << ","
            << "\"month\":" << t.time.month << ","
            << "\"day\":" << t.time.day << ","
            << "\"hour\":" << t.time.hour << ","
            << "\"minute\":" << t.time.minute << ","
            << "\"second\":" << t.time.second << ","
            << "\"millisecond\":" << t.time.millisecond << ","
            << "\"weekDays\":" << static_cast<int>(t.time.weekDays) << ","
            << "\"folder\":\"" << EscapeJson(ToUtf8(t.folder)) << "\","
            << "\"timeLabel\":\"" << EscapeJson(ToUtf8(FormatScheduledRunTime(t))) << "\""
            << "}";
    }
    oss << "]}";
    return oss.str();
}

bool SaveScheduledTaskFromJson(const std::string& msgJson, bool isUpdate, std::string& err) {
    std::vector<ScheduledTask> tasks;
    bool globalDisabled = false;
    LoadScheduledTasks(tasks, &globalDisabled);

    auto getStr = [](const std::string& json, const char* key, std::string& out) -> bool {
        const std::string pat = std::string("\"") + key + "\"";
        const auto k = json.find(pat);
        if (k == std::string::npos) return false;
        const auto colon = json.find(':', k + pat.size());
        if (colon == std::string::npos) return false;
        const auto q1 = json.find('"', colon + 1);
        if (q1 == std::string::npos) return false;
        std::string val;
        for (size_t i = q1 + 1; i < json.size(); ++i) {
            if (json[i] == '\\' && i + 1 < json.size()) { val.push_back(json[i + 1]); ++i; continue; }
            if (json[i] == '"') { out = val; return true; }
            val.push_back(json[i]);
        }
        return false;
    };

    std::string idUtf8, nameUtf8, pathUtf8;
    getStr(msgJson, "id", idUtf8);
    getStr(msgJson, "name", nameUtf8);
    getStr(msgJson, "filePath", pathUtf8);

    // 列表点状态：只改启用/禁用，不重写时间/路径，也不清 customFired。
    int statusOnly = 0;
    JsonGetInt(msgJson, "statusOnly", statusOnly);
    if (isUpdate && statusOnly != 0) {
        if (idUtf8.empty()) {
            err = "缺少任务 ID";
            return false;
        }
        const std::wstring id = FromUtf8(idUtf8);
        ScheduledTask* target = nullptr;
        for (auto& t : tasks) {
            if (t.id == id) { target = &t; break; }
        }
        if (!target) {
            err = "任务不存在";
            return false;
        }
        int status = static_cast<int>(target->status);
        JsonGetInt(msgJson, "status", status);
        target->status = (status == 0)
            ? ScheduledTaskStatus::Enabled
            : ScheduledTaskStatus::Disabled;
        if (!SaveScheduledTasks(tasks, globalDisabled)) {
            err = "保存定时任务失败";
            return false;
        }
        return true;
    }
    if (pathUtf8.empty()) getStr(msgJson, "targetFile", pathUtf8);
    int kind = 1, freq = 1, status = 0, weekDays = 0;
    int year = 0, month = 0, day = 0, hour = 9, minute = 0, second = 0, millisecond = 0;
    JsonGetInt(msgJson, "kind", kind);
    JsonGetInt(msgJson, "frequency", freq);
    JsonGetInt(msgJson, "status", status);
    JsonGetInt(msgJson, "weekDays", weekDays);
    JsonGetInt(msgJson, "year", year);
    JsonGetInt(msgJson, "month", month);
    JsonGetInt(msgJson, "day", day);
    JsonGetInt(msgJson, "hour", hour);
    JsonGetInt(msgJson, "minute", minute);
    const bool hasSecond = JsonGetInt(msgJson, "second", second);
    const bool hasMillisecond = JsonGetInt(msgJson, "millisecond", millisecond);

    if (pathUtf8.empty()) {
        err = "缺少脚本文件路径";
        return false;
    }
    std::wstring path = FromUtf8(pathUtf8);
    if (path.find(L'\\') == std::wstring::npos && path.find(L'/') == std::wstring::npos) {
        // try scripts then recordings
        std::wstring tryPath = ScriptsDir() + L"\\" + path;
        if (GetFileAttributesW(tryPath.c_str()) == INVALID_FILE_ATTRIBUTES)
            tryPath = RecordingsDir() + L"\\" + path;
        path = tryPath;
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = "目标文件不存在";
        return false;
    }

    ScheduledTask* target = nullptr;
    if (isUpdate) {
        const std::wstring id = FromUtf8(idUtf8);
        for (auto& t : tasks) {
            if (t.id == id) { target = &t; break; }
        }
        if (!target) {
            err = "任务不存在";
            return false;
        }
    } else {
        tasks.push_back({});
        target = &tasks.back();
        target->id = GenerateScheduledTaskId();
    }

    if (!nameUtf8.empty()) target->name = FromUtf8(nameUtf8);
    else if (target->name.empty()) target->name = DefaultScheduledTaskName();
    target->kind = (kind == 0) ? ScheduledTaskKind::Recording : ScheduledTaskKind::Macro;
    target->filePath = path;
    const auto slash = path.find_last_of(L"\\/");
    target->fileDisplayName = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    target->frequency = ClampScheduledFrequency(freq);
    target->status = (status == 0) ? ScheduledTaskStatus::Enabled : ScheduledTaskStatus::Disabled;
    target->time.year = year;
    target->time.month = month;
    target->time.day = day;
    target->time.hour = hour;
    target->time.minute = minute;
    // 缺 second/millisecond 时更新保留原值，避免 Web 旧表单抹掉非零秒。
    if (hasSecond) target->time.second = std::clamp(second, 0, 59);
    else if (!isUpdate) target->time.second = 0;
    if (hasMillisecond) target->time.millisecond = std::clamp(millisecond, 0, 999);
    else if (!isUpdate) target->time.millisecond = 0;
    target->time.weekDays = static_cast<uint8_t>(weekDays & 0x7F);
    if (target->frequency == ScheduledFrequency::Custom) target->customFired = false;
    if (target->frequency == ScheduledFrequency::Interval
        && ScheduledIntervalDurationMs(target->time) <= 0) {
        err = "间隔时间必须大于 0";
        return false;
    }
    {
        std::string folderUtf8;
        if (getStr(msgJson, "folder", folderUtf8)) {
            const std::wstring folder = NormalizeRelativeFolder(FromUtf8(folderUtf8));
            if (!IsSafeRelativeFolder(folder)) {
                err = "非法文件夹路径";
                return false;
            }
            target->folder = folder;
            if (!folder.empty()) {
                EnsureLibraryKindDir(L"sched");
                EnsureRelativeFolder(LibraryKindDir(L"sched"), folder);
            }
        }
    }

    if (!SaveScheduledTasks(tasks, globalDisabled)) {
        err = "保存定时任务失败";
        return false;
    }
    return true;
}

bool DeleteScheduledTaskById(const std::string& idUtf8, std::string& err) {
    std::vector<ScheduledTask> tasks;
    bool globalDisabled = false;
    LoadScheduledTasks(tasks, &globalDisabled);
    const std::wstring id = FromUtf8(idUtf8);
    const auto before = tasks.size();
    tasks.erase(std::remove_if(tasks.begin(), tasks.end(),
        [&](const ScheduledTask& t) { return t.id == id; }), tasks.end());
    if (tasks.size() == before) {
        err = "任务不存在";
        return false;
    }
    if (!SaveScheduledTasks(tasks, globalDisabled)) {
        err = "保存失败";
        return false;
    }
    return true;
}

bool SetScheduledTasksGlobalDisabled(bool disabled, std::string& err) {
    std::vector<ScheduledTask> tasks;
    bool globalDisabled = false;
    LoadScheduledTasks(tasks, &globalDisabled);
    if (!SaveScheduledTasks(tasks, disabled)) {
        err = "保存失败";
        return false;
    }
    return true;
}

std::string FormatOptimizeRecordingJson(const std::wstring& path, const ScriptFileData& data) {
    std::ostringstream oss;
    oss << "{\"path\":\"" << EscapeJson(ToUtf8(path)) << "\","
        << "\"name\":\"" << EscapeJson(ToUtf8(data.scriptName.empty()
            ? std::filesystem::path(path).stem().wstring() : data.scriptName)) << "\","
        << "\"durationSeconds\":" << data.durationSeconds << ","
        << "\"actions\":[";
    for (size_t i = 0; i < data.actions.size(); ++i) {
        if (i) oss << ",";
        std::wstring aj = ScriptActionToJsonString(data.actions[i]);
        size_t start = aj.find(L'{');
        size_t end = aj.rfind(L'}');
        if (start == std::wstring::npos || end == std::wstring::npos) continue;
        std::wstring body = aj.substr(start, end - start);
        const std::wstring typeKey = ExtractString(aj, L"type");
        oss << ToUtf8(body)
            << ",\"name\":\"" << EscapeJson(ToUtf8(ActionTypeLabel(typeKey))) << "\"}";
    }
    oss << "]}";
    return oss.str();
}

std::string JsonLoadOptimizeRecording(const std::string& pathUtf8, std::string& err) {
    std::wstring path;
    if (!ResolveScriptPath(pathUtf8, path, err)) return "{}";
    ScriptFileData data = LoadScriptFileData(path, true);
    g_optWork.sourcePath = path;
    g_optWork.data = data;
    g_optWork.active = true;
    return FormatOptimizeRecordingJson(path, data);
}

namespace {

std::vector<char> ParseSelectedMask(const std::string& msgJson, size_t actionCount, bool defaultAll) {
    std::vector<char> selected(actionCount, defaultAll ? 1 : 0);
    const auto pos = msgJson.find("\"selected\"");
    if (pos == std::string::npos) return selected;
    selected.assign(actionCount, 0);
    const auto lb = msgJson.find('[', pos);
    const auto rb = msgJson.find(']', lb);
    if (lb == std::string::npos || rb == std::string::npos) return selected;
    std::string arr = msgJson.substr(lb + 1, rb - lb - 1);
    size_t i = 0;
    while (i < arr.size()) {
        while (i < arr.size() && (arr[i] == ' ' || arr[i] == ',')) ++i;
        if (i >= arr.size()) break;
        const int idx = atoi(arr.c_str() + i);
        if (idx >= 0 && idx < static_cast<int>(selected.size())) selected[static_cast<size_t>(idx)] = 1;
        while (i < arr.size() && arr[i] != ',') ++i;
    }
    return selected;
}

bool WaitMatchesFilterOp(int filterOp, double duration, double compareValue) {
    switch (filterOp) {
    case 0: return true;
    case 1: return duration < compareValue;
    case 2: return duration <= compareValue;
    case 3: return duration > compareValue;
    case 4: return duration >= compareValue;
    case 5: return std::abs(duration - compareValue) < 0.0005;
    case 6: return std::abs(duration - compareValue) >= 0.0005;
    default: return true;
    }
}

int ParseWaitFilterOp(const std::string& msgJson) {
    int op = 0;
    if (JsonGetInt(msgJson, "waitFilter", op)) return std::clamp(op, 0, 6);
    std::string label;
    if (!JsonGetStringField(msgJson, "waitFilter", label)) return 0;
    static const char* kLabels[] = {
        "全部", "小于", "小于等于", "大于", "大于等于", "等于", "不等于"
    };
    for (int i = 0; i < 7; ++i) {
        if (label == kLabels[i]) return i;
    }
    return 0;
}

void RecalcDurationSeconds(ScriptFileData& data) {
    double total = 0;
    for (const auto& a : data.actions)
        if (a.type == ActionType::Wait) total += ActionStepUs(a) / 1000000.0;
    data.durationSeconds = total;
    data.inputTimingVersion = kInputTimingVersionExplicitWaits;
}

const char* RelativeMoveErr(bool forMerge) {
    return forMerge
        ? "相对移动轨迹受保护，不能对 FPS 相对位移使用绝对路径合并。"
        : "相对移动轨迹受保护，不能对 FPS 相对位移使用绝对路径压缩。";
}

}  // namespace

bool ApplyOptimizeAndSave(const std::string& msgJson, std::string& err, std::string& outExtraJson) {
    outExtraJson.clear();
    std::string pathUtf8;
    JsonGetStringField(msgJson, "path", pathUtf8);
    std::wstring path;
    if (!ResolveScriptPath(pathUtf8, path, err)) return false;

    int scheme = 0;
    JsonGetInt(msgJson, "scheme", scheme);
    // 必须 denorm：合并/压缩读的是像素 x/y；false 时只有 n*、x/y=0，会把轨迹冲掉。
    ScriptFileData data;
    if (g_optWork.active && SameOptPath(g_optWork.sourcePath, path)) {
        data = g_optWork.data;
    } else {
        data = LoadScriptFileData(path, true);
    }

    if (scheme == 0) {
        int protect = 1;
        JsonGetInt(msgJson, "protectKeyOps", protect);
        const auto selected = ParseSelectedMask(msgJson, data.actions.size(), false);
        bool anySelected = false;
        for (char c : selected) {
            if (c) { anySelected = true; break; }
        }
        if (!anySelected) {
            err = "请先选择要删除的动作。";
            return false;
        }
        std::vector<ScriptAction> kept;
        for (size_t i = 0; i < data.actions.size(); ++i) {
            if (!selected[i]) { kept.push_back(data.actions[i]); continue; }
            // 对齐原生 IsKeyOperation：除绝对/相对移动与等待外均视为关键操作
            if (protect) {
                const auto t = data.actions[i].type;
                if (t != ActionType::MoveMouse
                    && t != ActionType::MoveMouseRelative
                    && t != ActionType::Wait) {
                    kept.push_back(data.actions[i]);
                }
            }
        }
        data.actions = std::move(kept);
    } else if (scheme == 1) {
        // 对齐 RecordingOptimizeDialog::ApplyWaitAdjust + WaitMatchesFilter
        double newWait = 0.01;
        JsonGetNumber(msgJson, "waitValue", newWait);
        const int filterOp = ParseWaitFilterOp(msgJson);
        double compareVal = 0.0;
        if (!JsonGetNumber(msgJson, "compareValue", compareVal))
            JsonGetNumber(msgJson, "waitCompare", compareVal);
        const auto selected = ParseSelectedMask(msgJson, data.actions.size(), false);
        int changed = 0;
        for (size_t i = 0; i < data.actions.size(); ++i) {
            if (!selected[i] || data.actions[i].type != ActionType::Wait) continue;
            const double dur = ActionStepUs(data.actions[i]) / 1000000.0;
            if (!WaitMatchesFilterOp(filterOp, dur, filterOp == 0 ? 0.0 : compareVal)) continue;
            data.actions[i].duration = newWait;
            if (data.actions[i].duration > 0.0) {
                const long double us = static_cast<long double>(data.actions[i].duration) * 1000000.0L;
                data.actions[i].timingUs = static_cast<uint64_t>(std::llround(us));
            } else {
                data.actions[i].timingUs = 0;
            }
            data.actions[i].randomDuration = 0.0;
            ++changed;
        }
        if (changed == 0) {
            err = "没有符合条件的等待动作被调整。";
            return false;
        }
    } else if (scheme == 2 || scheme == 3) {
        // 与 recopt 共用：选中含关键动作时按关键动作分段；否则仍要求连续 move/wait。
        const bool forMerge = scheme == 2;
        const auto selected = ParseSelectedMask(msgJson, data.actions.size(), false);
        recopt::OptimizeApplyResult applied;
        if (forMerge) {
            std::string waitCalc = "sum";
            JsonGetStringField(msgJson, "waitCalculation", waitCalc);
            if (waitCalc.empty()) waitCalc = "sum";
            double mergeWait = 0.1;
            JsonGetNumber(msgJson, "mergeWaitValue", mergeWait);
            JsonGetNumber(msgJson, "waitValue", mergeWait);
            applied = recopt::MergeSelected(data.actions, selected, waitCalc, mergeWait);
        } else {
            double thr = 5.0, cw = 0.05;
            JsonGetNumber(msgJson, "distanceThreshold", thr);
            JsonGetNumber(msgJson, "compressWait", cw);
            applied = recopt::CompressSelected(data.actions, selected, thr, cw);
        }
        if (!applied.collectOk) {
            err = applied.collectErr;
            return false;
        }
        if (applied.applied == 0) {
            if (applied.skippedRelative > 0 && applied.skippedNoMove == 0 && applied.skippedTooFew == 0)
                err = RelativeMoveErr(forMerge);
            else if (forMerge)
                err = "选中范围内没有鼠标移动动作。";
            else
                err = "选中范围内至少需要两个鼠标移动点。";
            return false;
        }
    } else if (scheme == 4) {
        ConvertToFindImageOptions copt{};
        copt.requireCapturePath = true;
        std::string findTime = "0";
        JsonGetStringField(msgJson, "findTimeExpr", findTime);
        if (findTime.empty()) findTime = "0";
        copt.findTimeExpr = FromUtf8(findTime);
        const auto selected = ParseSelectedMask(msgJson, data.actions.size(), false);
        bool any = false;
        for (char c : selected) if (c) { any = true; break; }
        if (!any) {
            err = "请先选择要转为找图点击的动作";
            return false;
        }
        const ConvertToFindImageResult conv =
            ConvertActionsToFindImageSelected(data.actions, selected, copt);
        outExtraJson = "\"converted\":" + std::to_string(conv.converted)
            + ",\"skipped\":" + std::to_string(conv.skipped)
            + ",\"detail\":\"" + EscapeJson(ToUtf8(conv.detail)) + "\"";
    } else if (scheme == -1) {
        // no-op transform: used by「保存到新录制」to snapshot current file
    } else {
        err = "未知优化方案";
        return false;
    }

    RecalcDurationSeconds(data);
    g_optWork.sourcePath = path;
    g_optWork.data = data;
    g_optWork.active = true;

    int saveAsNew = 0;
    JsonGetInt(msgJson, "saveAsNew", saveAsNew);
    if (saveAsNew) {
        EnsureScriptsDir();
        CreateDirectoryW(RecordingsDir().c_str(), nullptr);
        // 优先用前端 #optName 传入的 name；勿硬盖「原名-优化」
        std::string nameUtf8;
        JsonGetStringField(msgJson, "name", nameUtf8);
        std::wstring baseName = nameUtf8.empty()
            ? (data.scriptName.empty()
                   ? std::filesystem::path(path).stem().wstring()
                   : data.scriptName)
            : FromUtf8(nameUtf8);
        for (wchar_t& ch : baseName) {
            if (wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
        }
        if (baseName.empty()) baseName = L"录制优化";
        std::wstring outPath = RecordingsDir() + L"\\" + baseName + L".json";
        for (int suffix = 1; GetFileAttributesW(outPath.c_str()) != INVALID_FILE_ATTRIBUTES; ++suffix) {
            outPath = RecordingsDir() + L"\\" + baseName + L"-" + std::to_wstring(suffix) + L".json";
        }
        data.scriptName = std::filesystem::path(outPath).stem().wstring();
        if (!SaveScriptFileData(outPath, data)) {
            err = "保存到新录制失败";
            return false;
        }
        return true;
    }

    // 对话框内应用：只改工作副本，不写原文件。前端用 recording 刷新列表。
    if (!outExtraJson.empty()) outExtraJson += ",";
    outExtraJson += "\"recording\":" + FormatOptimizeRecordingJson(path, data);
    return true;
}

bool ParseWindowModePreviewRequest(const std::string& json,
    windowmode::WindowModeScriptConfig& outCfg, std::string& err) {
    outCfg = windowmode::DefaultWindowModeConfig();
    const std::string pat = "\"windowMode\"";
    const auto k = json.find(pat);
    if (k != std::string::npos) {
        const auto brace = json.find('{', k + pat.size());
        if (brace != std::string::npos) {
            const auto braceEnd = FindMatchingJsonBrace(json, brace);
            if (braceEnd != std::string::npos) {
                outCfg = windowmode::ParseWindowModeConfigObject(
                    FromUtf8(json.substr(brace, braceEnd - brace + 1)));
            }
        }
    }
    int editorMode = 0;
    JsonGetInt(json, "editorMode", editorMode);
    if (editorMode == 0) {
        outCfg.enabled = false;
        outCfg.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
    } else if (editorMode == 1) {
        outCfg.enabled = true;
        outCfg.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
    } else if (editorMode == 2) {
        outCfg.enabled = true;
        outCfg.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    }
    if (!outCfg.enabled) {
        err = "window mode not enabled";
        return false;
    }
    return true;
}

bool ResolveWindowModeTargetHwnd(const windowmode::WindowModeScriptConfig& cfg,
    HWND& outHwnd, std::wstring& outLabel, std::string& err) {
    outHwnd = nullptr;
    outLabel.clear();
    if (!cfg.enabled) {
        err = "window mode not enabled";
        return false;
    }
    const windowmode::WindowTargetQuery query = windowmode::BuildTargetQuery(cfg);
    HWND top = windowmode::FindMainWindowDefault(query, true);
    if (!top || !IsWindow(top)) {
        err = "target window not found";
        return false;
    }
    HWND bind = top;
    if (!cfg.childWindowClassName.empty()) {
        if (HWND child = windowmode::FindChildWindowByClass(top, cfg.childWindowClassName))
            bind = child;
    } else if (windowmode::LooksLikeChromiumBrowserClass(cfg.windowClassName)) {
        if (HWND rw = windowmode::FindBrowserRenderWidget(top))
            bind = rw;
    }
    outHwnd = bind;
    outLabel = windowmode::WindowModeConfigSummary(cfg);
    if (outLabel.empty()) {
        wchar_t title[256]{};
        GetWindowTextW(top, title, 256);
        outLabel = title[0] ? title : L"目标窗口";
    }
    return true;
}

std::string CaptureHwndPreviewDataUrl(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return {};
    windowmode::WindowCaptureResult capture = windowmode::CaptureWindowClient(hwnd);
    if (!capture.bitmap) return {};
    // 预览小图：长边压到 ~480px、质量 60，显著降低每次刷新截图+JPEG 编码耗时
    const int longEdge = std::max(capture.w, capture.h);
    double scale = 1.0;
    if (longEdge > 480) scale = 480.0 / static_cast<double>(longEdge);
    const std::string b64 = BitmapToBase64Jpeg(capture.bitmap, 60, scale);
    DeleteObject(capture.bitmap);
    if (b64.empty()) return {};
    return "data:image/jpeg;base64," + b64;
}

namespace {

std::wstring KindFromUtf8(const std::string& kindUtf8) {
    const std::wstring k = FromUtf8(kindUtf8);
    if (k == L"macro" || k == L"scripts" || k == L"script") return L"macro";
    if (k == L"rec" || k == L"recordings" || k == L"recording") return L"rec";
    if (k == L"sched" || k == L"schedule") return L"sched";
    if (k == L"ai" || k == L"agent") return L"ai";
    return k;
}

std::wstring FolderWinPath(const std::wstring& root, const std::wstring& folder) {
    const std::wstring norm = NormalizeRelativeFolder(folder);
    if (norm.empty()) return root;
    std::wstring rel = norm;
    for (auto& ch : rel) if (ch == L'/') ch = L'\\';
    return root + L"\\" + rel;
}

bool RemoveDirRecursive(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0
                || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
            const std::wstring child = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirRecursive(child);
            else
                DeleteFileW(child.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(dir.c_str()) != 0;
}

bool MoveDirRecursive(const std::wstring& src, const std::wstring& dst) {
    if (MoveFileW(src.c_str(), dst.c_str())) return true;
    // 跨卷或非空失败时：创建目标后逐项移动
    CreateDirectoryW(dst.c_str(), nullptr);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do {
        if (fd.cFileName[0] == L'.' && (fd.cFileName[1] == 0
            || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
        const std::wstring from = src + L"\\" + fd.cFileName;
        const std::wstring to = dst + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!MoveDirRecursive(from, to)) ok = false;
        } else if (!MoveFileW(from.c_str(), to.c_str())) {
            ok = false;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(src.c_str());
    return ok;
}

void CollectFoldersFromItems(const std::wstring& kind, std::vector<std::wstring>& out) {
    if (kind == L"macro" || kind == L"rec") {
        EnumerateRelativeFolders(LibraryKindDir(kind), out);
        return;
    }
    if (kind == L"sched") {
        EnsureLibraryKindDir(L"sched");
        EnumerateRelativeFolders(LibraryKindDir(L"sched"), out);
        std::vector<ScheduledTask> tasks;
        LoadScheduledTasks(tasks, nullptr);
        for (const auto& t : tasks) {
            if (!t.folder.empty()) out.push_back(NormalizeRelativeFolder(t.folder));
        }
    } else if (kind == L"ai") {
        EnsureLibraryKindDir(L"ai");
        EnumerateRelativeFolders(LibraryKindDir(L"ai"), out);
        std::vector<AgentConversationMeta> list;
        LoadAgentConversationList(list);
        for (const auto& m : list) {
            if (!m.folder.empty()) out.push_back(NormalizeRelativeFolder(m.folder));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

}  // namespace

bool MoveScriptToFolder(const std::string& pathUtf8, const std::string& destFolderUtf8,
    std::string& outNewPathUtf8, std::string& err);

std::string JsonListLibraryFolders(const std::string& kindUtf8) {
    const std::wstring kind = KindFromUtf8(kindUtf8);
    std::vector<std::wstring> folders;
    CollectFoldersFromItems(kind, folders);
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < folders.size(); ++i) {
        if (i) oss << ",";
        oss << JsonString(folders[i]);
    }
    oss << "]";
    return oss.str();
}

bool CreateLibraryFolder(const std::string& kindUtf8, const std::string& folderUtf8, std::string& err) {
    const std::wstring kind = KindFromUtf8(kindUtf8);
    const std::wstring folder = NormalizeRelativeFolder(FromUtf8(folderUtf8));
    if (folder.empty()) { err = "文件夹名称不能为空"; return false; }
    if (!IsSafeRelativeFolder(folder)) { err = "非法文件夹路径"; return false; }
    EnsureLibraryKindDir(kind);
    if (!EnsureRelativeFolder(LibraryKindDir(kind), folder)) {
        err = "创建文件夹失败";
        return false;
    }
    InvalidateScriptListCache();
    return true;
}

bool RenameLibraryFolder(const std::string& kindUtf8, const std::string& folderUtf8,
    const std::string& newNameUtf8, std::string& err) {
    const std::wstring kind = KindFromUtf8(kindUtf8);
    const std::wstring folder = NormalizeRelativeFolder(FromUtf8(folderUtf8));
    std::wstring newName = Trim(FromUtf8(newNameUtf8));
    for (wchar_t& ch : newName) {
        if (wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
    }
    if (folder.empty() || newName.empty()) { err = "参数无效"; return false; }
    if (!IsSafeRelativeFolder(folder)) { err = "非法文件夹路径"; return false; }

    const auto slash = folder.find_last_of(L'/');
    const std::wstring parent = (slash == std::wstring::npos) ? L"" : folder.substr(0, slash);
    const std::wstring newFolder = parent.empty() ? newName : (parent + L"/" + newName);
    if (!IsSafeRelativeFolder(newFolder)) { err = "非法新名称"; return false; }

    const std::wstring root = LibraryKindDir(kind);
    const std::wstring src = FolderWinPath(root, folder);
    const std::wstring dst = FolderWinPath(root, newFolder);
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // 逻辑文件夹（sched/ai 仅有条目归属）：批量改字段
        if (kind != L"sched" && kind != L"ai") {
            err = "文件夹不存在";
            return false;
        }
    } else if (!MoveDirRecursive(src, dst)) {
        err = "重命名文件夹失败";
        return false;
    }

    const std::wstring prefix = folder + L"/";
    if (kind == L"macro" || kind == L"rec") {
        // 物理移动已完成；列表缓存失效即可
    } else if (kind == L"sched") {
        std::vector<ScheduledTask> tasks;
        bool gd = false;
        LoadScheduledTasks(tasks, &gd);
        for (auto& t : tasks) {
            if (t.folder == folder) t.folder = newFolder;
            else if (t.folder.size() > prefix.size()
                && t.folder.compare(0, prefix.size(), prefix) == 0)
                t.folder = newFolder + t.folder.substr(folder.size());
        }
        SaveScheduledTasks(tasks, gd);
    } else if (kind == L"ai") {
        std::vector<AgentConversationMeta> list;
        LoadAgentConversationList(list);
        for (auto& m : list) {
            if (m.folder == folder) m.folder = newFolder;
            else if (m.folder.size() > prefix.size()
                && m.folder.compare(0, prefix.size(), prefix) == 0)
                m.folder = newFolder + m.folder.substr(folder.size());
        }
        SaveAgentConversationIndex(list);
    }
    InvalidateScriptListCache();
    return true;
}

bool DeleteLibraryFolder(const std::string& kindUtf8, const std::string& folderUtf8, std::string& err) {
    const std::wstring kind = KindFromUtf8(kindUtf8);
    const std::wstring folder = NormalizeRelativeFolder(FromUtf8(folderUtf8));
    if (folder.empty() || !IsSafeRelativeFolder(folder)) { err = "非法文件夹路径"; return false; }

    const std::wstring root = LibraryKindDir(kind);
    const std::wstring abs = FolderWinPath(root, folder);
    const std::wstring prefix = folder + L"/";

    // 脚本/录制：文件夹内文件上移到父级，再删空目录
    if (kind == L"macro" || kind == L"rec") {
        const auto slash = folder.find_last_of(L'/');
        const std::wstring parent = (slash == std::wstring::npos) ? L"" : folder.substr(0, slash);
        std::vector<ScriptFileEntry> files;
        EnumerateScriptJsonFiles(root, files);
        for (const auto& f : files) {
            if (f.folder != folder && !(f.folder.size() > prefix.size()
                && f.folder.compare(0, prefix.size(), prefix) == 0)) continue;
            // 子树内：保留相对尾部挂到 parent
            std::wstring rest;
            if (f.folder == folder) rest = L"";
            else rest = f.folder.substr(prefix.size());
            const std::wstring destFolder = rest.empty() ? parent
                : (parent.empty() ? rest : (parent + L"/" + rest));
            std::string outPath, moveErr;
            if (!MoveScriptToFolder(ToUtf8(f.path), ToUtf8(destFolder), outPath, moveErr)) {
                err = moveErr.empty() ? "移动文件失败" : moveErr;
                return false;
            }
        }
        if (GetFileAttributesW(abs.c_str()) != INVALID_FILE_ATTRIBUTES)
            RemoveDirRecursive(abs);
    } else if (kind == L"sched") {
        std::vector<ScheduledTask> tasks;
        bool gd = false;
        LoadScheduledTasks(tasks, &gd);
        for (auto& t : tasks) {
            if (t.folder == folder || (t.folder.size() > prefix.size()
                && t.folder.compare(0, prefix.size(), prefix) == 0))
                t.folder.clear();
        }
        SaveScheduledTasks(tasks, gd);
        if (GetFileAttributesW(abs.c_str()) != INVALID_FILE_ATTRIBUTES)
            RemoveDirRecursive(abs);
    } else if (kind == L"ai") {
        std::vector<AgentConversationMeta> list;
        LoadAgentConversationList(list);
        for (auto& m : list) {
            if (m.folder == folder || (m.folder.size() > prefix.size()
                && m.folder.compare(0, prefix.size(), prefix) == 0))
                m.folder.clear();
        }
        SaveAgentConversationIndex(list);
        if (GetFileAttributesW(abs.c_str()) != INVALID_FILE_ATTRIBUTES)
            RemoveDirRecursive(abs);
    }
    InvalidateScriptListCache();
    return true;
}

bool MoveScriptToFolder(const std::string& pathUtf8, const std::string& destFolderUtf8,
    std::string& outNewPathUtf8, std::string& err) {
    outNewPathUtf8.clear();
    std::wstring path;
    if (!ResolveScriptPath(pathUtf8, path, err)) {
        err.clear();
        path = FromUtf8(pathUtf8);
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            err = "file not found";
            return false;
        }
        if (!IsPathInside(path, ScriptsDir()) && !IsPathInside(path, RecordingsDir())) {
            err = "script path outside app directories";
            return false;
        }
    }
    const std::wstring destFolder = NormalizeRelativeFolder(FromUtf8(destFolderUtf8));
    if (!IsSafeRelativeFolder(destFolder)) { err = "非法文件夹路径"; return false; }

    const bool inRec = IsPathInside(path, RecordingsDir());
    const std::wstring root = inRec ? RecordingsDir() : ScriptsDir();
    if (!EnsureRelativeFolder(root, destFolder)) { err = "目标文件夹不可用"; return false; }

    const auto slash = path.find_last_of(L"\\/");
    const std::wstring fileName = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    const std::wstring destDir = FolderWinPath(root, destFolder);
    const std::wstring newPath = destDir + L"\\" + fileName;
    if (_wcsicmp(path.c_str(), newPath.c_str()) == 0) {
        outNewPathUtf8 = ToUtf8(path);
        return true;
    }
    if (GetFileAttributesW(newPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        err = "目标已存在同名文件";
        return false;
    }
    if (!MoveFileW(path.c_str(), newPath.c_str())) {
        err = "移动失败";
        return false;
    }
    InvalidateScriptListCache();
    outNewPathUtf8 = ToUtf8(newPath);
    return true;
}

bool SetItemLibraryFolder(const std::string& kindUtf8, const std::string& idUtf8,
    const std::string& folderUtf8, std::string& err) {
    const std::wstring kind = KindFromUtf8(kindUtf8);
    const std::wstring folder = NormalizeRelativeFolder(FromUtf8(folderUtf8));
    if (!IsSafeRelativeFolder(folder)) { err = "非法文件夹路径"; return false; }
    if (kind == L"macro" || kind == L"rec") {
        std::string outPath;
        return MoveScriptToFolder(idUtf8, ToUtf8(folder), outPath, err);
    }
    if (kind == L"sched") {
        std::vector<ScheduledTask> tasks;
        bool gd = false;
        LoadScheduledTasks(tasks, &gd);
        const std::wstring id = FromUtf8(idUtf8);
        ScheduledTask* hit = nullptr;
        for (auto& t : tasks) if (t.id == id) { hit = &t; break; }
        if (!hit) { err = "任务不存在"; return false; }
        hit->folder = folder;
        if (!folder.empty()) {
            EnsureLibraryKindDir(L"sched");
            EnsureRelativeFolder(LibraryKindDir(L"sched"), folder);
        }
        if (!SaveScheduledTasks(tasks, gd)) { err = "保存失败"; return false; }
        return true;
    }
    if (kind == L"ai") {
        if (!SetAgentConversationFolder(FromUtf8(idUtf8), folder)) {
            err = "设置对话文件夹失败";
            return false;
        }
        return true;
    }
    err = "未知类型";
    return false;
}

}  // namespace qst::webview
