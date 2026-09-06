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

#include <cmath>
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
    {L"save_load_hid_driver_flag", L"default",
        L"enableHidDriverSimulation legacy true maps to Interception"},
    {L"save_load_foreground_input_backend", L"default",
        L"foregroundInputBackend VirtualHid roundtrip"},
    {L"save_load_recording_click_capture", L"default",
        L"recordingClickCapture enabled/halfSize roundtrip + clamp"},
    {L"save_load_playback_speed", L"default",
        L"enablePlaybackSpeed + playbackSpeed roundtrip + clamp"},
    {L"save_load_scheduled_conflict_policy", L"default",
        L"scheduledTaskConflictPolicy roundtrip + clamp 0..1; scheduledTaskAutoResume roundtrip"},
    {L"playback_speed_scale_math", L"default",
        L"ScalePlaybackTimeSeconds: disabled identity; 2x halves; 0.25x *4; wait 0 stays"},
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
    {L"save_load_home_ui_mode", L"default",
        L"home.uiMode simple/pro roundtrip; other values normalize to simple"},
    {L"save_load_global_hotkey", L"default",
        L"home.globalHotkeyText/Vk/Modifiers/Hold roundtrip"},
    {L"save_load_other_os_flags", L"default",
        L"autoStartOnBoot + resolveImeConflict roundtrip"},
    {L"save_load_prefer_direct2d", L"default",
        L"preferDirect2D default false; roundtrip true"},
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
        && d.windowMode.previewRefreshMs == 500
        && !d.playback.enablePlaybackSpeed
        && d.playback.playbackSpeed == 1.0
        && d.playback.scheduledTaskConflictPolicy == 0
        && !d.playback.scheduledTaskAutoResume
        && d.home.uiMode == L"simple";
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

void CaseHidDriverFlag(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    s.playback.enableHidDriverSimulation = true;
    // Legacy path without foregroundInputBackend field: store writes both.
    s.playback.foregroundInputBackend = quickscript::ForegroundInputBackend::Interception;
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool ok = loaded.playback.enableHidDriverSimulation
        && loaded.playback.foregroundInputBackend
            == quickscript::ForegroundInputBackend::Interception;
    Emit(L"save_load_hid_driver_flag", ok, ok ? L"" : L"HID flag roundtrip failed");
}

void CaseForegroundInputBackend(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    s.playback.foregroundInputBackend = quickscript::ForegroundInputBackend::VirtualHid;
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool ok = loaded.playback.foregroundInputBackend
            == quickscript::ForegroundInputBackend::VirtualHid
        && loaded.playback.enableHidDriverSimulation;
    Emit(L"save_load_foreground_input_backend", ok,
        ok ? L"" : L"foregroundInputBackend roundtrip failed");
}

void CaseRecordingClickCapture(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    s.playback.recordingClickCaptureEnabled = false;
    s.playback.recordingClickCaptureHalfSize = 99;
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    bool ok = !loaded.playback.recordingClickCaptureEnabled
        && loaded.playback.recordingClickCaptureHalfSize == 99;
    s.playback.recordingClickCaptureHalfSize = 200;
    SaveAppSettings(s);
    LoadAppSettings(loaded);
    ok = ok && loaded.playback.recordingClickCaptureHalfSize == 120;
    s.playback.recordingClickCaptureHalfSize = 1;
    SaveAppSettings(s);
    LoadAppSettings(loaded);
    ok = ok && loaded.playback.recordingClickCaptureHalfSize == 16;
    Emit(L"save_load_recording_click_capture", ok, ok ? L"" : L"capture settings/clamp failed");
}

void CasePlaybackSpeed(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    s.playback.enablePlaybackSpeed = true;
    s.playback.playbackSpeed = 2.0;
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    bool ok = loaded.playback.enablePlaybackSpeed
        && std::abs(loaded.playback.playbackSpeed - 2.0) < 1e-9;
    s.playback.playbackSpeed = 9.0;
    SaveAppSettings(s);
    LoadAppSettings(loaded);
    ok = ok && std::abs(loaded.playback.playbackSpeed - 4.0) < 1e-9;
    s.playback.playbackSpeed = 0.05;
    SaveAppSettings(s);
    LoadAppSettings(loaded);
    ok = ok && std::abs(loaded.playback.playbackSpeed - 0.25) < 1e-9;
    Emit(L"save_load_playback_speed", ok, ok ? L"" : L"playbackSpeed roundtrip/clamp failed");
}

void CaseScheduledConflictPolicy(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    s.playback.scheduledTaskConflictPolicy = 1;
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    bool ok = loaded.playback.scheduledTaskConflictPolicy == 1;
    s.playback.scheduledTaskConflictPolicy = 2;
    SaveAppSettings(s);
    LoadAppSettings(loaded);
    ok = ok && loaded.playback.scheduledTaskConflictPolicy == 0;
    s.playback.scheduledTaskConflictPolicy = 9;
    SaveAppSettings(s);
    LoadAppSettings(loaded);
    ok = ok && loaded.playback.scheduledTaskConflictPolicy == 0;
    s.playback.scheduledTaskConflictPolicy = -1;
    SaveAppSettings(s);
    LoadAppSettings(loaded);
    ok = ok && loaded.playback.scheduledTaskConflictPolicy == 0;
    s.playback.scheduledTaskAutoResume = true;
    SaveAppSettings(s);
    LoadAppSettings(loaded);
    ok = ok && loaded.playback.scheduledTaskAutoResume;
    s.playback.scheduledTaskAutoResume = false;
    SaveAppSettings(s);
    LoadAppSettings(loaded);
    ok = ok && !loaded.playback.scheduledTaskAutoResume;
    Emit(L"save_load_scheduled_conflict_policy", ok,
        ok ? L"" : L"scheduledTaskConflictPolicy/autoResume roundtrip/clamp failed");
}

void CasePlaybackSpeedMath(SettingsFileGuard& /*g*/) {
    using quickscript::PlaybackTabSettings;
    using quickscript::ScalePlaybackTimeSeconds;
    using quickscript::ScalePlaybackTimeUs;
    using quickscript::RecordingPlaybackTimeScale;
    using quickscript::PlaybackTimeScaleAlways;
    PlaybackTabSettings off{};
    off.enablePlaybackSpeed = false;
    off.playbackSpeed = 4.0;
    PlaybackTabSettings x2{};
    x2.enablePlaybackSpeed = true;
    x2.playbackSpeed = 2.0;
    PlaybackTabSettings slow{};
    slow.enablePlaybackSpeed = true;
    slow.playbackSpeed = 0.25;
    const bool ok = std::abs(ScalePlaybackTimeSeconds(1.0, off) - 1.0) < 1e-12
        && std::abs(ScalePlaybackTimeSeconds(1.0, x2) - 0.5) < 1e-12
        && std::abs(ScalePlaybackTimeSeconds(0.8, x2) - 0.4) < 1e-12
        && std::abs(ScalePlaybackTimeSeconds(1.0, slow) - 4.0) < 1e-12
        && ScalePlaybackTimeSeconds(0.0, x2) == 0.0
        && ScalePlaybackTimeSeconds(-1.0, x2) == -1.0
        && ScalePlaybackTimeUs(1000, off) == 1000
        && ScalePlaybackTimeUs(1000, x2) == 500;
    AppSettings simpleRec = DefaultAppSettings();
    simpleRec.home.uiMode = L"simple";
    simpleRec.playback.enablePlaybackSpeed = false;
    simpleRec.playback.playbackSpeed = 2.0;
    AppSettings proOff = simpleRec;
    proOff.home.uiMode = L"pro";
    AppSettings proOn = proOff;
    proOn.playback.enablePlaybackSpeed = true;
    const bool recOk = std::abs(RecordingPlaybackTimeScale(simpleRec) - 0.5) < 1e-12
        && std::abs(RecordingPlaybackTimeScale(proOff) - 1.0) < 1e-12
        && std::abs(RecordingPlaybackTimeScale(proOn) - 0.5) < 1e-12
        && std::abs(PlaybackTimeScaleAlways(2.0) - 0.5) < 1e-12
        && std::abs(ScalePlaybackTimeSeconds(1.0, PlaybackTimeScaleAlways(2.0)) - 0.5) < 1e-12;
    Emit(L"playback_speed_scale_math", ok && recOk, ok && recOk ? L"" : L"scale helper mismatch");
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
    // 中文路径：旧 wofstream 会截断 JSON，必须 UTF-8 落盘
    s.home.selectedScriptPath = L"D:\\other\\software\\build\\Release\\scripts\\定时功能测试.json";
    s.home.recorderInputMode = 2;
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool ok = loaded.home.activeTab == 2
        && loaded.home.selectedScriptPath == s.home.selectedScriptPath
        && loaded.home.recorderInputMode == 2;
    Emit(L"save_load_home_tab", ok, ok ? L"" : L"home state lost (check UTF-8 Chinese path)");
}

void CaseHomeUiMode(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    s.home.uiMode = L"pro";
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool roundtrip = loaded.home.uiMode == L"pro";
    AppSettings s2 = DefaultAppSettings();
    s2.home.uiMode = L"weird";
    SaveAppSettings(s2);
    AppSettings loaded2{};
    LoadAppSettings(loaded2);
    const bool clamp = loaded2.home.uiMode == L"simple";
    Emit(L"save_load_home_ui_mode", roundtrip && clamp,
        roundtrip && clamp ? L"" : L"home.uiMode roundtrip/normalize failed");
}

void CaseGlobalHotkey(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    s.home.globalHotkeyText = L"F9";
    s.home.globalHotkeyVk = 0x78;
    s.home.globalHotkeyModifiers = 0;
    s.home.globalHotkeyHold = true;
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool ok = loaded.home.globalHotkeyText == L"F9"
        && loaded.home.globalHotkeyVk == 0x78
        && loaded.home.globalHotkeyModifiers == 0
        && loaded.home.globalHotkeyHold;
    Emit(L"save_load_global_hotkey", ok, ok ? L"" : L"global hotkey home fields lost");
}

void CaseOtherOsFlags(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    s.other.autoStartOnBoot = true;
    s.other.resolveImeConflict = true;
    s.other.holdThresholdSeconds = 0.35;
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool ok = loaded.other.autoStartOnBoot && loaded.other.resolveImeConflict
        && std::abs(loaded.other.holdThresholdSeconds - 0.35) < 1e-9;
    Emit(L"save_load_other_os_flags", ok, ok ? L"" : L"other OS flags lost");
}

void CasePreferDirect2D(SettingsFileGuard& /*g*/) {
    AppSettings baseline = DefaultAppSettings();
    const bool defaultOff = !baseline.other.preferDirect2D;
    baseline.other.preferDirect2D = true;
    SaveAppSettings(baseline);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool ok = defaultOff && loaded.other.preferDirect2D;
    Emit(L"save_load_prefer_direct2d", ok,
        ok ? L"" : L"preferDirect2D default/roundtrip mismatch");
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
        CaseHidDriverFlag(guard);
        CaseForegroundInputBackend(guard);
        CaseRecordingClickCapture(guard);
        CasePlaybackSpeed(guard);
        CaseScheduledConflictPolicy(guard);
        CasePlaybackSpeedMath(guard);
        CaseThemeClamp(guard);
        CaseCustomTheme(guard);
        CaseWmPreviewClamp(guard);
        CaseAiModels(guard);
        CaseHomeTab(guard);
        CaseHomeUiMode(guard);
        CaseGlobalHotkey(guard);
        CaseOtherOsFlags(guard);
        CasePreferDirect2D(guard);
        CaseGarbage(guard);
    }

    selftest::EmitSummary();
    return selftest::ExitCode();
}
