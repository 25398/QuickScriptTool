// =============================================================================
// AppSettingsStoreSelfTest — 设置持久化自检
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:AppSettingsStoreSelfTest
//   build\Release\AppSettingsStoreSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "app_settings_store.h"
#include "app_theme.h"
#include "utils.h"

#include <fstream>
#include <string>

namespace {

using selftest::Emit;
using quickscript::AppSettings;
using quickscript::DefaultAppSettings;

const selftest::CaseInfo kCases[] = {
    {L"default_settings_baseline", L"default",
        L"DefaultAppSettings AI URL/model and themeId=0"},
    {L"settings_path_under_appdir", L"default",
        L"AppSettingsFilePath ends with app_settings.json under AppDir"},
    {L"load_missing_file_defaults", L"default",
        L"Missing file -> Load false + defaults"},
    {L"save_load_click_jitter", L"default",
        L"jitterX/Y + enableCoordinateJitter roundtrip"},
    {L"save_load_playback_flags", L"default",
        L"playbackCount + debug window flags roundtrip"},
    {L"theme_id_clamped", L"default",
        L"themeId=99 clamps to kThemeCount-1"},
    {L"custom_theme_roundtrip", L"default",
        L"useCustomTheme + customMain/Accent roundtrip"},
    {L"wm_preview_ms_clamped", L"default",
        L"previewRefreshMs below 200 clamps to 200"},
    {L"save_load_ai_saved_models", L"default",
        L"ai.savedModels modelName roundtrip"},
    {L"save_load_home_tab", L"default",
        L"home.activeTab + selectedScriptPath roundtrip"},
    {L"load_garbage_partial_safe", L"default",
        L"Garbage JSON does not crash; defaults remain usable"},
};

struct SettingsFileGuard {
    std::wstring path;
    std::wstring backupPath;
    bool hadBackup = false;

    SettingsFileGuard() {
        path = AppSettingsFilePath();
        backupPath = path + L".selftest_bak";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(backupPath.c_str());
            if (MoveFileW(path.c_str(), backupPath.c_str())) hadBackup = true;
            else CopyFileW(path.c_str(), backupPath.c_str(), FALSE);
            hadBackup = GetFileAttributesW(backupPath.c_str()) != INVALID_FILE_ATTRIBUTES;
            DeleteFileW(path.c_str());
        }
    }

    ~SettingsFileGuard() {
        DeleteFileW(path.c_str());
        if (hadBackup) {
            MoveFileW(backupPath.c_str(), path.c_str());
        }
        DeleteFileW(backupPath.c_str());
    }
};

void WriteRawSettings(const std::wstring& path, const std::string& utf8) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

void CaseDefaults() {
    const AppSettings d = DefaultAppSettings();
    const bool ok = d.other.themeId == 0
        && d.ai.modelName == L"gpt-4o"
        && d.ai.apiUrl.find(L"openai.com") != std::wstring::npos
        && d.windowMode.previewRefreshMs == 500;
    Emit(L"default_settings_baseline", ok, ok ? L"" : L"defaults mismatch");
}

void CasePath() {
    const std::wstring path = AppSettingsFilePath();
    const std::wstring dir = AppDir();
    const bool ok = path.find(dir) == 0
        && path.size() > dir.size()
        && path.find(L"app_settings.json") != std::wstring::npos;
    Emit(L"settings_path_under_appdir", ok, path.c_str());
}

void CaseLoadMissing(SettingsFileGuard& /*g*/) {
    DeleteFileW(AppSettingsFilePath().c_str());
    AppSettings out{};
    const bool loaded = LoadAppSettings(out);
    const bool ok = !loaded && out.other.themeId == 0
        && out.ai.modelName == L"gpt-4o";
    Emit(L"load_missing_file_defaults", ok,
        loaded ? L"Load should fail for missing file" : L"");
}

void CaseClickJitter(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    s.click.enableCoordinateJitter = true;
    s.click.jitterX = 7;
    s.click.jitterY = 9;
    const bool saved = SaveAppSettings(s);
    AppSettings loaded{};
    const bool okLoad = LoadAppSettings(loaded);
    const bool ok = saved && okLoad && loaded.click.enableCoordinateJitter
        && loaded.click.jitterX == 7 && loaded.click.jitterY == 9;
    Emit(L"save_load_click_jitter", ok, ok ? L"" : L"jitter roundtrip failed");
}

void CasePlayback(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    s.playback.enablePlaybackCount = true;
    s.playback.playbackCount = 3;
    s.playback.enableDebugOutputWindow = true;
    s.playback.autoOutputKeyFunctionDebug = false;
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool ok = loaded.playback.enablePlaybackCount
        && loaded.playback.playbackCount == 3
        && loaded.playback.enableDebugOutputWindow
        && !loaded.playback.autoOutputKeyFunctionDebug;
    Emit(L"save_load_playback_flags", ok, ok ? L"" : L"playback roundtrip failed");
}

void CaseThemeClamp(SettingsFileGuard& /*g*/) {
    WriteRawSettings(AppSettingsFilePath(),
        u8"{\"other\":{\"themeId\":99}}");
    AppSettings loaded{};
    LoadAppSettings(loaded);
    Emit(L"theme_id_clamped",
        loaded.other.themeId == quickscript::kThemeCount - 1,
        (L"themeId=" + std::to_wstring(loaded.other.themeId)).c_str());
}

void CaseCustomTheme(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    s.other.useCustomTheme = true;
    s.other.customMainColor = 0x00D2944E;   // soft blue-ish COLORREF
    s.other.customAccentColor = 0x002894EB;
    s.other.themeId = 2;
    const bool saved = SaveAppSettings(s);
    AppSettings loaded{};
    const bool okLoad = LoadAppSettings(loaded);
    const bool ok = saved && okLoad
        && loaded.other.useCustomTheme
        && loaded.other.customMainColor == 0x00D2944E
        && loaded.other.customAccentColor == 0x002894EB
        && loaded.other.themeId == 2;
    Emit(L"custom_theme_roundtrip", ok, ok ? L"" : L"custom theme fields lost");
}

void CaseWmPreviewClamp(SettingsFileGuard& /*g*/) {
    WriteRawSettings(AppSettingsFilePath(),
        u8"{\"windowMode\":{\"previewRefreshMs\":50}}");
    AppSettings loaded{};
    LoadAppSettings(loaded);
    Emit(L"wm_preview_ms_clamped", loaded.windowMode.previewRefreshMs == 200,
        (L"ms=" + std::to_wstring(loaded.windowMode.previewRefreshMs)).c_str());
}

void CaseAiModels(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    quickscript::AiModelProfile m{};
    m.modelName = L"selftest-model";
    m.apiUrl = L"https://example.test/v1";
    s.ai.savedModels.push_back(m);
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool ok = !loaded.ai.savedModels.empty()
        && loaded.ai.savedModels[0].modelName == L"selftest-model";
    Emit(L"save_load_ai_saved_models", ok, ok ? L"" : L"savedModels lost");
}

void CaseHomeTab(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    s.home.activeTab = 2;
    s.home.selectedScriptPath = L"scripts\\demo.json";
    s.home.recorderInputMode = 2;
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool ok = loaded.home.activeTab == 2
        && loaded.home.selectedScriptPath == L"scripts\\demo.json"
        && loaded.home.recorderInputMode == 2;
    Emit(L"save_load_home_tab", ok, ok ? L"" : L"home state lost");
}

void CaseGarbage(SettingsFileGuard& /*g*/) {
    WriteRawSettings(AppSettingsFilePath(), "{not json!!!}");
    AppSettings loaded{};
    LoadAppSettings(loaded);
    Emit(L"load_garbage_partial_safe",
        loaded.ai.modelName == L"gpt-4o" && loaded.other.themeId == 0, L"");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--json") == 0) {
            selftest::gJson = true;
            selftest::InitUtf8Stdout();
        } else if (_wcsicmp(argv[i], L"--list") == 0) {
            listOnly = true;
            selftest::InitUtf8Stdout();
        } else if (_wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"-h") == 0) {
            std::fwprintf(stderr,
                L"  AppSettingsStoreSelfTest.exe [--json] [--list] [--help]\n");
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"AppSettingsStoreSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }
    if (!selftest::gJson) {
        std::fwprintf(stderr, L"=== AppSettingsStoreSelfTest ===\n");
    }

    CaseDefaults();
    CasePath();

    {
        SettingsFileGuard guard;
        CaseLoadMissing(guard);
        CaseClickJitter(guard);
        CasePlayback(guard);
        CaseThemeClamp(guard);
        CaseCustomTheme(guard);
        CaseWmPreviewClamp(guard);
        CaseAiModels(guard);
        CaseHomeTab(guard);
        CaseGarbage(guard);
    }

    selftest::EmitSummary();
    return selftest::ExitCode();
}
