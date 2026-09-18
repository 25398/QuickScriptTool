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
#include "desktop_tools/float_ball_geom.h"
#include "findimage_gpu.h"
#include "low_power_mode.h"
#include "utils.h"

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>

namespace {

using selftest::Emit;
using quickscript::AppSettings;
using quickscript::DefaultAppSettings;

const selftest::CaseInfo kCases[] = {
    {L"default_settings_baseline", L"default",
        L"DefaultAppSettings AI URL/model, playSoundOnStart/End, themeId=Arctic"},
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
    {L"save_load_low_performance_mode", L"default",
        L"lowPerformanceMode JSON 往返 + 保存/载入后立即同步 LowPerformanceMode() 进程开关"},
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
        L"autoStartOnBoot + resolveImeConflict + playSoundOnEnd roundtrip"},
    {L"save_load_float_ball", L"default",
        L"showFloatBall default true; docked/free + edge 0-3 + x/yRatio clamp"},
    {L"float_ball_geom_dock_expand", L"default",
        L"docked peek on left/right; expanded panel inward; snap mid-line; yRatio clamp"},
    {L"save_load_ui_scale_factor", L"default",
        L"other.uiScaleFactor default 1.0; roundtrip; <=0/missing clamp to 1.0"},
    {L"save_load_editor_default_view", L"default",
        L"other.editorDefaultView code/visual roundtrip; other values normalize to code"},
    {L"save_load_editor_visual_flags", L"default",
        L"other visualLoopWrap/call/if/block/watch/jump/grid/cardId roundtrip; missing keys stay default on"},
    {L"save_load_editor_action_catalog", L"default",
        L"other editorActionOrder/hidden/catalogPreset/searchAll + custom sidecar roundtrip; missing/junk → all"},
    {L"save_load_editor_var_filters", L"default",
        L"editorHideFixedVars/HideCoordVars/MultiResultPlaceholderOnly default false; roundtrip true"},
    {L"save_load_editor_general_flags", L"default",
        L"editorDisableModifyButton/AutoSaveOnExit/EnableBatchInsert default false; roundtrip true; missing keys stay false"},
    {L"save_load_prefer_direct2d", L"default",
        L"preferDirect2D default false; roundtrip true"},
    {L"load_omits_auto_hide_keeps_default", L"default",
        L"other JSON without autoHideMainWindow keeps default true (partial save must not uncheck)"},
    {L"load_garbage_partial_safe", L"default",
        L"Garbage JSON does not crash; defaults remain usable"},
    {L"try_load_missing_leaves_out", L"default",
        L"TryLoadAppSettings fails on missing file without resetting out"},
    {L"home_runtime_save_preserves_playback", L"default",
        L"PreserveUserSettings keeps playback/ai/uiMode; overlays selectedScriptPath"},
    {L"startup_wav_missing_rejected", L"default",
        L"IsPlayableWavFile false for missing path"},
    {L"startup_wav_garbage_rejected", L"default",
        L"IsPlayableWavFile false for non-RIFF / truncated file"},
    {L"startup_wav_valid_header_accepted", L"default",
        L"IsPlayableWavFile true for minimal RIFF/WAVE"},
    {L"finish_wav_path_sidecar", L"default",
        L"AppFinishSoundFilePath is AppDir\\\\finish.wav"},
    {L"path_is_under_root", L"default",
        L"PathIsUnderRoot prefix + Program Files vs Program Files (x86)"},
    {L"webview_userdata_programfiles_roams", L"default",
        L"Program Files install uses LocalAppData WebView2UserData"},
    {L"webview_userdata_portable_sidecar", L"default",
        L"custom/portable dir keeps WebView2UserData beside exe"},
    {L"webview_fetchdata_programfiles_roams", L"default",
        L"Program Files FetchData also roams to LocalAppData"},
    {L"extract_string_object_last_wins", L"default",
        L"ExtractString 只读根对象键且同名键 last-wins，不误读嵌套 apiKey"},
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
    const bool ok = d.other.themeId == quickscript::kDefaultThemeId
        && d.other.playSoundOnStart
        && d.other.playSoundOnEnd
        && d.ai.modelName == L"gpt-4o"
        && d.ai.apiUrl.find(L"openai.com") != std::wstring::npos
        && d.windowMode.previewRefreshMs == 500
        && !d.playback.enablePlaybackSpeed
        && d.playback.playbackSpeed == 1.0
        && d.playback.scheduledTaskConflictPolicy == 0
        && !d.playback.scheduledTaskAutoResume
        && d.home.uiMode == L"simple"
        && d.other.editorDefaultView == L"code"
        && std::abs(d.other.uiScaleFactor - 1.0) < 1e-9
        && d.other.visualLoopWrap
        && d.other.visualBlockCallWires
        && d.other.visualIfWrap
        && d.other.visualBlockWrap
        && d.other.visualJumpWires
        && d.other.visualShowGrid
        && d.other.visualShowCardId
        && d.other.editorActionOrder.empty()
        && d.other.editorHiddenActions.empty()
        && d.other.autoHideMainWindow
        && d.other.showFloatBall
        && d.other.floatBallDocked
        && d.other.floatBallEdge == 1
        && std::abs(d.other.floatBallXRatio - 1.0) < 1e-9
        && std::abs(d.other.floatBallYRatio - 0.55) < 1e-9;
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
    const bool ok = !loaded && out.other.themeId == quickscript::kDefaultThemeId
        && out.other.playSoundOnStart
        && out.other.playSoundOnEnd
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

void CaseLowPerformanceMode(SettingsFileGuard& /*g*/) {
    // 低性能模式 / 找图 GPU 加速：既要 JSON 往返，也要**立即**反映到进程级开关
    // （引擎/找图/时间轴都直接读 LowPerformanceMode()/FindImageGpuAccelFlag()，不等重启）
    AppSettings s = DefaultAppSettings();
    s.playback.lowPerformanceMode = true;
    s.playback.findImageGpuAccel = true;
    bool wrote = SaveAppSettings(s);
    const bool flagOnAfterSave = LowPerformanceMode();
    const bool gpuOnAfterSave = FindImageGpuAccelEnabled();
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool roundtrip = loaded.playback.lowPerformanceMode;
    const bool gpuRoundtrip = loaded.playback.findImageGpuAccel;
    const bool flagOnAfterLoad = LowPerformanceMode();
    // 低性能模式压制 GPU 加速
    const bool gpuSuppressed = !FindImageGpuAccelActive();

    s.playback.lowPerformanceMode = false;
    s.playback.findImageGpuAccel = false;
    wrote = wrote && SaveAppSettings(s);
    LoadAppSettings(loaded);
    const bool offRoundtrip = !loaded.playback.lowPerformanceMode && !loaded.playback.findImageGpuAccel;
    const bool flagOff = !LowPerformanceMode() && !FindImageGpuAccelEnabled();
    const std::wstring detail = (wrote ? L"" : L"保存失败 ")
        + std::wstring(flagOnAfterSave ? L"" : L"保存后低性能开关未置位 ")
        + std::wstring(gpuOnAfterSave ? L"" : L"保存后 GPU 开关未置位 ")
        + std::wstring(roundtrip ? L"" : L"JSON 往返丢低性能字段 ")
        + std::wstring(gpuRoundtrip ? L"" : L"JSON 往返丢 GPU 字段 ")
        + std::wstring(flagOnAfterLoad ? L"" : L"载入后低性能开关未置位 ")
        + std::wstring(gpuSuppressed ? L"" : L"低性能模式未压制 GPU 加速 ")
        + std::wstring(offRoundtrip ? L"" : L"关闭未往返 ")
        + std::wstring(flagOff ? L"" : L"关闭后开关未清");
    Emit(L"save_load_low_performance_mode",
        wrote && flagOnAfterSave && gpuOnAfterSave && roundtrip && gpuRoundtrip
            && flagOnAfterLoad && gpuSuppressed && offRoundtrip && flagOff,
        detail.c_str());
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
    s.other.playSoundOnEnd = false;
    s.other.holdThresholdSeconds = 0.35;
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool ok = loaded.other.autoStartOnBoot && loaded.other.resolveImeConflict
        && !loaded.other.playSoundOnEnd
        && std::abs(loaded.other.holdThresholdSeconds - 0.35) < 1e-9;
    Emit(L"save_load_other_os_flags", ok, ok ? L"" : L"other OS flags lost");
}

void CaseFloatBallSettings(SettingsFileGuard& /*g*/) {
    AppSettings d = DefaultAppSettings();
    const bool defaultOn = d.other.showFloatBall && d.other.floatBallDocked
        && d.other.floatBallEdge == 1
        && std::abs(d.other.floatBallXRatio - 1.0) < 1e-9
        && std::abs(d.other.floatBallYRatio - 0.55) < 1e-9;

    AppSettings s = DefaultAppSettings();
    s.other.showFloatBall = false;
    s.other.floatBallDocked = false;
    s.other.floatBallEdge = 2;
    s.other.floatBallXRatio = 0.4;
    s.other.floatBallYRatio = 0.25;
    s.other.floatBallMonitorId = L"\\\\.\\DISPLAY2";
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool roundtrip = !loaded.other.showFloatBall
        && !loaded.other.floatBallDocked
        && loaded.other.floatBallEdge == 2
        && std::abs(loaded.other.floatBallXRatio - 0.4) < 1e-9
        && std::abs(loaded.other.floatBallYRatio - 0.25) < 1e-9
        && loaded.other.floatBallMonitorId == L"\\\\.\\DISPLAY2";

    AppSettings s2 = DefaultAppSettings();
    s2.other.floatBallYRatio = -1.0;
    s2.other.floatBallXRatio = 2.0;
    s2.other.floatBallEdge = 9;
    SaveAppSettings(s2);
    AppSettings loadedNeg{};
    LoadAppSettings(loadedNeg);
    const bool negClamped = std::abs(loadedNeg.other.floatBallYRatio) < 1e-9
        && std::abs(loadedNeg.other.floatBallXRatio - 1.0) < 1e-9
        && loadedNeg.other.floatBallEdge == 1;

    WriteRawSettings(AppSettingsFilePath(),
        "{\"other\":{\"editorDefaultView\":\"code\",\"holdThresholdSeconds\":0.2}}\n");
    AppSettings loadedMissing{};
    LoadAppSettings(loadedMissing);
    const bool missingDefault = loadedMissing.other.showFloatBall
        && loadedMissing.other.floatBallDocked
        && loadedMissing.other.floatBallEdge == 1
        && std::abs(loadedMissing.other.floatBallXRatio - 1.0) < 1e-9
        && std::abs(loadedMissing.other.floatBallYRatio - 0.55) < 1e-9;

    const bool ok = defaultOn && roundtrip && negClamped && missingDefault;
    Emit(L"save_load_float_ball", ok, ok ? L"" : L"float ball settings roundtrip/default failed");
}

void CaseFloatBallGeom() {
    using namespace qst::desktop_tools;
    const RECT work{0, 0, 1920, 1080};
    FloatBallMetrics m;
    m.peekPx = 18;
    m.ballPx = 64;
    m.panelW = 148;
    m.panelH = 64;
    m.neckPx = 32;
    m.gapPx = 0;
    m.padPx = 8;
    m.buttonH = 26;
    m.snapPx = 28;

    const FloatBallFrame dockR = ComputeDockedFrame(work, FloatBallEdge::Right, 1.0, 0.5, m);
    const bool dockRight = dockR.window.right == work.right
        && (dockR.window.right - dockR.window.left) == m.peekPx
        && dockR.local.ball.left == 0
        && dockR.window.bottom > dockR.window.top;

    const FloatBallFrame dockL = ComputeDockedFrame(work, FloatBallEdge::Left, 0.0, 0.0, m);
    const bool dockLeft = dockL.window.left == work.left
        && (dockL.window.right - dockL.window.left) == m.peekPx
        && dockL.local.ball.left == m.peekPx - m.ballPx
        && dockL.window.top == work.top;

    const FloatBallFrame dockT = ComputeDockedFrame(work, FloatBallEdge::Top, 0.4, 0.0, m);
    const bool dockTop = dockT.window.top == work.top
        && (dockT.window.bottom - dockT.window.top) == m.peekPx
        && dockT.local.ball.top == m.peekPx - m.ballPx;

    const int neck = NeckClamped(m);
    const int expW = ThermometerSpan(m.ballPx, m.panelW, neck);
    const FloatBallFrame expR = ComputeExpandedFrame(work, FloatBallEdge::Right, 1.0, 0.5, m);
    const bool expRight = expR.window.right == work.right
        && (expR.window.right - expR.window.left) == expW
        && expR.local.ball.left == m.panelW - neck
        && expR.local.ball.left < expR.local.panel.right
        && expR.local.button.bottom > expR.local.button.top
        && expR.local.title.bottom > expR.local.title.top
        && expR.local.title.bottom <= expR.local.button.top
        && (expR.window.bottom - expR.window.top) == m.ballPx;

    const FloatBallFrame expL = ComputeExpandedFrame(work, FloatBallEdge::Left, 0.0, 1.0, m);
    const bool expLeft = expL.window.left == work.left
        && expL.local.panel.left < expL.local.ball.right
        && expL.window.bottom <= work.bottom;

    const FloatBallFrame expT = ComputeExpandedFrame(work, FloatBallEdge::Top, 0.5, 0.0, m);
    const int stemThick = StemThickness(m);
    const bool expTop = expT.window.top == work.top
        && (expT.window.bottom - expT.window.top) == m.ballPx
        && (expT.local.panel.bottom - expT.local.panel.top) == stemThick
        && expT.local.button.right > expT.local.button.left;

    const FloatBallFrame freeBall = ComputeFreeFrame(work, 0.5, 0.5, m, false);
    const bool freeCircle = (freeBall.window.right - freeBall.window.left) == m.ballPx
        && (freeBall.window.bottom - freeBall.window.top) == m.ballPx
        && freeBall.window.left > work.left
        && freeBall.window.right < work.right
        && freeBall.window.top > work.top;

    const FloatBallFrame freeUp = ComputeFreeFrame(work, 0.5, 0.15, m, true);
    const bool freeUpperHorizontal = (freeUp.window.bottom - freeUp.window.top) == m.ballPx
        && (freeUp.local.panel.bottom - freeUp.local.panel.top) == stemThick
        && freeUp.local.panel.left > freeUp.local.ball.left;

    const FloatBallFrame freeRight = ComputeFreeFrame(work, 0.85, 0.2, m, true);
    const bool freeRightExpandsRight = (freeRight.window.bottom - freeRight.window.top) == m.ballPx
        && freeRight.local.panel.left > freeRight.local.ball.left
        && freeRight.local.panel.right > freeRight.local.ball.right;

    const FloatBallFrame freeLeft = ComputeFreeFrame(work, 0.15, 0.2, m, true);
    const bool freeLeftExpandsLeft = (freeLeft.window.bottom - freeLeft.window.top) == m.ballPx
        && freeLeft.local.panel.left < freeLeft.local.ball.left
        && freeLeft.local.panel.right < freeLeft.local.ball.right;

    const FloatBallSnap snapL = ResolveReleaseSnap(4, 400, work, m.ballPx, m.snapPx);
    const bool snapLeft = snapL.docked && snapL.edge == FloatBallEdge::Left;
    const FloatBallSnap snapTop = ResolveReleaseSnap(900, 6, work, m.ballPx, m.snapPx);
    const bool snapToTop = snapTop.docked && snapTop.edge == FloatBallEdge::Top;
    const FloatBallSnap snapFree = ResolveReleaseSnap(900, 500, work, m.ballPx, m.snapPx);
    const bool staysFree = !snapFree.docked;

    const bool yClamp = ClampFloatBallYRatio(-2.0) == 0.0
        && ClampFloatBallYRatio(2.0) == 1.0
        && std::abs(ClampFloatBallYRatio(0.3) - 0.3) < 1e-9
        && ClampFloatBallEdgeInt(2) == 2
        && ClampFloatBallEdgeInt(9) == 1;

    const RECT ball{0, 0, 56, 56};
    const bool inCircle = PointInCircle(28, 28, ball) && !PointInCircle(0, 0, ball);

    const auto peekAnim = ComputeDockedAnimFrame(work, FloatBallEdge::Right, 1.0, 0.5, m, 0.f);
    const auto midR = ComputeDockedAnimFrame(work, FloatBallEdge::Right, 1.0, 0.5, m, 0.20f);
    const auto lateR = ComputeDockedAnimFrame(work, FloatBallEdge::Right, 1.0, 0.5, m, 0.75f);
    const auto fullAnim = ComputeDockedAnimFrame(work, FloatBallEdge::Right, 1.0, 0.5, m, 1.f);
    const int midW = midR.window.right - midR.window.left;
    const int lateW = lateR.window.right - lateR.window.left;
    const int lateH = lateR.window.bottom - lateR.window.top;
    const bool pinAnim = midR.window.right == work.right
        && midR.window.left == fullAnim.window.left
        && peekAnim.local.ball.left == 0
        && peekAnim.local.panel.right <= peekAnim.local.panel.left
        && fullAnim.local.ball.right == (fullAnim.window.right - fullAnim.window.left)
        && midW == expW
        && midR.local.panel.right <= midR.local.panel.left
        && lateR.window.right == work.right
        && lateW == expW
        && lateR.local.ball.left >= 0
        && lateR.local.ball.right <= lateW
        && lateR.local.ball.top >= 0
        && lateR.local.ball.bottom <= lateH;

    const auto midT = ComputeDockedAnimFrame(work, FloatBallEdge::Top, 0.4, 0.0, m, 0.4f);
    const auto fullT = ComputeDockedAnimFrame(work, FloatBallEdge::Top, 0.4, 0.0, m, 1.f);
    const auto peekTop = ComputeDockedAnimFrame(work, FloatBallEdge::Top, 0.4, 0.0, m, 0.f);
    const bool pinTopAnim = midT.window.top == work.top
        && midT.window.left == fullT.window.left
        && midT.window.right == fullT.window.right
        && peekTop.local.ball.top == m.peekPx - m.ballPx;

    const FloatBallSnap snapCursor = ResolveReleaseSnap(400, 400, work, m.ballPx, 56, 1910, 500);
    const bool snapByCursor = snapCursor.docked && snapCursor.edge == FloatBallEdge::Right;

    RECT revealBox{0, 0, 180, 64};
    RECT revealBall{180 - 64, 0, 180, 64};
    const RECT rev0 = DockedRevealRect(FloatBallEdge::Right, revealBox, revealBall, 0.f);
    const RECT rev1 = DockedRevealRect(FloatBallEdge::Right, revealBox, revealBall, 1.f);
    const bool revealRight = (rev0.right == revealBox.right)
        && (rev0.right - rev0.left) == 64
        && rev1.left == revealBox.left
        && rev1.right == revealBox.right;

    const bool ok = dockRight && dockLeft && dockTop && expRight && expLeft && expTop
        && freeCircle && freeUpperHorizontal && freeRightExpandsRight && freeLeftExpandsLeft
        && snapLeft && snapToTop && staysFree && yClamp && inCircle && pinAnim && pinTopAnim
        && snapByCursor && revealRight;
    Emit(L"float_ball_geom_dock_expand", ok, ok ? L"" : L"float ball geom failed");
}

void CaseUiScaleFactor(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    const bool defaultOk = std::abs(s.other.uiScaleFactor - 1.0) < 1e-9;
    s.other.uiScaleFactor = 0.75;
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool roundtrip = std::abs(loaded.other.uiScaleFactor - 0.75) < 1e-9;

    AppSettings s2 = DefaultAppSettings();
    s2.other.uiScaleFactor = 0.0;
    SaveAppSettings(s2);
    AppSettings loadedZero{};
    LoadAppSettings(loadedZero);
    const bool zeroClamped = std::abs(loadedZero.other.uiScaleFactor - 1.0) < 1e-9;

    AppSettings s3 = DefaultAppSettings();
    s3.other.uiScaleFactor = -2.0;
    SaveAppSettings(s3);
    AppSettings loadedNeg{};
    LoadAppSettings(loadedNeg);
    const bool negClamped = std::abs(loadedNeg.other.uiScaleFactor - 1.0) < 1e-9;

    AppSettings s4 = DefaultAppSettings();
    s4.other.uiScaleFactor = 8.0;
    SaveAppSettings(s4);
    AppSettings loadedHi{};
    LoadAppSettings(loadedHi);
    const bool hiClamped = std::abs(loadedHi.other.uiScaleFactor - 3.0) < 1e-9;

    WriteRawSettings(AppSettingsFilePath(),
        "{\"other\":{\"editorDefaultView\":\"code\",\"holdThresholdSeconds\":0.2}}\n");
    AppSettings loadedMissing{};
    LoadAppSettings(loadedMissing);
    const bool missingDefault = std::abs(loadedMissing.other.uiScaleFactor - 1.0) < 1e-9;

    const bool ok = defaultOk && roundtrip && zeroClamped && negClamped && hiClamped && missingDefault;
    Emit(L"save_load_ui_scale_factor", ok, ok ? L"" : L"uiScaleFactor roundtrip/normalize failed");
}

void CaseEditorDefaultView(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    s.other.editorDefaultView = L"visual";
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool roundtrip = loaded.other.editorDefaultView == L"visual";
    AppSettings s2 = DefaultAppSettings();
    s2.other.editorDefaultView = L"weird";
    SaveAppSettings(s2);
    AppSettings loaded2{};
    LoadAppSettings(loaded2);
    const bool clamp = loaded2.other.editorDefaultView == L"code";
    Emit(L"save_load_editor_default_view", roundtrip && clamp,
        roundtrip && clamp ? L"" : L"editorDefaultView roundtrip/normalize failed");
}

void CaseEditorVisualFlags(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    const bool defaultsOn = s.other.visualLoopWrap && s.other.visualBlockCallWires
        && s.other.visualIfWrap && s.other.visualBlockWrap && s.other.visualWatchWrap
        && s.other.visualJumpWires && s.other.visualShowGrid && s.other.visualShowCardId;
    s.other.visualLoopWrap = false;
    s.other.visualBlockCallWires = false;
    s.other.visualIfWrap = false;
    s.other.visualBlockWrap = false;
    s.other.visualWatchWrap = false;
    s.other.visualJumpWires = false;
    s.other.visualShowGrid = false;
    s.other.visualShowCardId = false;
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool roundtrip = !loaded.other.visualLoopWrap && !loaded.other.visualBlockCallWires
        && !loaded.other.visualIfWrap && !loaded.other.visualBlockWrap
        && !loaded.other.visualWatchWrap
        && !loaded.other.visualJumpWires && !loaded.other.visualShowGrid
        && !loaded.other.visualShowCardId;
    AppSettings missing = DefaultAppSettings();
    SaveAppSettings(missing);
    WriteRawSettings(AppSettingsFilePath(),
        "{\"other\":{\"editorDefaultView\":\"code\",\"holdThresholdSeconds\":0.2}}\n");
    AppSettings loadedMissing{};
    LoadAppSettings(loadedMissing);
    const bool missingKeepsDefault = loadedMissing.other.visualLoopWrap
        && loadedMissing.other.visualBlockCallWires
        && loadedMissing.other.visualIfWrap
        && loadedMissing.other.visualBlockWrap
        && loadedMissing.other.visualWatchWrap
        && loadedMissing.other.visualJumpWires
        && loadedMissing.other.visualShowGrid
        && loadedMissing.other.visualShowCardId;
    const bool ok = defaultsOn && roundtrip && missingKeepsDefault;
    Emit(L"save_load_editor_visual_flags", ok,
        ok ? L"" : L"editor visual flags default/roundtrip/missing failed");
}

void CaseEditorActionCatalog(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    const bool emptyDefault = s.other.editorActionOrder.empty()
        && s.other.editorHiddenActions.empty()
        && s.other.editorCatalogPreset.empty()
        && s.other.editorCustomActionOrder.empty()
        && s.other.editorCustomHiddenActions.empty()
        && !s.other.editorSearchAllActions;
    s.other.editorActionOrder = {L"wait", L"moveMouse", L"goto"};
    s.other.editorHiddenActions = {L"goto"};
    s.other.editorCatalogPreset = L"simple";
    s.other.editorCustomActionOrder = {L"wait", L"keyClick"};
    s.other.editorCustomHiddenActions = {L"goto"};
    s.other.editorSearchAllActions = true;
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool roundtrip = loaded.other.editorActionOrder.size() == 3
        && loaded.other.editorActionOrder[0] == L"wait"
        && loaded.other.editorActionOrder[1] == L"moveMouse"
        && loaded.other.editorActionOrder[2] == L"goto"
        && loaded.other.editorHiddenActions.size() == 1
        && loaded.other.editorHiddenActions[0] == L"goto"
        && quickscript::NormalizeEditorCatalogPreset(loaded.other.editorCatalogPreset) == L"simple"
        && loaded.other.editorCustomActionOrder.size() == 2
        && loaded.other.editorCustomActionOrder[0] == L"wait"
        && loaded.other.editorCustomActionOrder[1] == L"keyClick"
        && loaded.other.editorCustomHiddenActions.size() == 1
        && loaded.other.editorCustomHiddenActions[0] == L"goto"
        && loaded.other.editorSearchAllActions;

    AppSettings missing = DefaultAppSettings();
    SaveAppSettings(missing);
    WriteRawSettings(AppSettingsFilePath(),
        "{\"other\":{\"editorDefaultView\":\"code\",\"holdThresholdSeconds\":0.2}}\n");
    AppSettings loadedMissing{};
    LoadAppSettings(loadedMissing);
    const bool missingEmpty = loadedMissing.other.editorActionOrder.empty()
        && loadedMissing.other.editorHiddenActions.empty()
        && loadedMissing.other.editorCustomActionOrder.empty()
        && loadedMissing.other.editorCustomHiddenActions.empty()
        && !loadedMissing.other.editorSearchAllActions
        && quickscript::NormalizeEditorCatalogPreset(loadedMissing.other.editorCatalogPreset) == L"all";

    WriteRawSettings(AppSettingsFilePath(),
        "{\"other\":{\"editorActionOrder\":[\"wait\",\"bad-token\",\"wait\",\"moveMouse\"],"
        "\"editorHiddenActions\":[\"goto\",\"??\"],"
        "\"editorCatalogPreset\":\"nope\","
        "\"editorCustomActionOrder\":[\"wait\",\"bad-token\"],"
        "\"editorCustomHiddenActions\":[\"goto\",\"??\"]}}\n");
    AppSettings loadedJunk{};
    LoadAppSettings(loadedJunk);
    const bool junkFiltered = loadedJunk.other.editorActionOrder.size() == 2
        && loadedJunk.other.editorActionOrder[0] == L"wait"
        && loadedJunk.other.editorActionOrder[1] == L"moveMouse"
        && loadedJunk.other.editorHiddenActions.size() == 1
        && loadedJunk.other.editorHiddenActions[0] == L"goto"
        && quickscript::NormalizeEditorCatalogPreset(loadedJunk.other.editorCatalogPreset) == L"all"
        && loadedJunk.other.editorCustomActionOrder.size() == 1
        && loadedJunk.other.editorCustomActionOrder[0] == L"wait"
        && loadedJunk.other.editorCustomHiddenActions.size() == 1
        && loadedJunk.other.editorCustomHiddenActions[0] == L"goto";

    const bool ok = emptyDefault && roundtrip && missingEmpty && junkFiltered;
    Emit(L"save_load_editor_action_catalog", ok,
        ok ? L"" : L"editor action catalog default/roundtrip/missing/junk failed");
}

void CaseEditorVarFilters(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    const bool defaultsOff = !s.other.editorHideFixedVars
        && !s.other.editorHideCoordVars
        && !s.other.editorMultiResultPlaceholderOnly;
    s.other.editorHideFixedVars = true;
    s.other.editorHideCoordVars = true;
    s.other.editorMultiResultPlaceholderOnly = true;
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool roundtrip = loaded.other.editorHideFixedVars
        && loaded.other.editorHideCoordVars
        && loaded.other.editorMultiResultPlaceholderOnly;

    AppSettings missing = DefaultAppSettings();
    SaveAppSettings(missing);
    WriteRawSettings(AppSettingsFilePath(),
        "{\"other\":{\"editorDefaultView\":\"code\",\"holdThresholdSeconds\":0.2}}\n");
    AppSettings loadedMissing{};
    LoadAppSettings(loadedMissing);
    const bool missingOff = !loadedMissing.other.editorHideFixedVars
        && !loadedMissing.other.editorHideCoordVars
        && !loadedMissing.other.editorMultiResultPlaceholderOnly;

    const bool ok = defaultsOff && roundtrip && missingOff;
    Emit(L"save_load_editor_var_filters", ok,
        ok ? L"" : L"editor var filters default/roundtrip/missing failed");
}

void CaseEditorGeneralFlags(SettingsFileGuard& /*g*/) {
    AppSettings s = DefaultAppSettings();
    const bool defaultsOff = !s.other.editorDisableModifyButton
        && !s.other.editorAutoSaveOnExit
        && !s.other.editorEnableBatchInsert;
    s.other.editorDisableModifyButton = true;
    s.other.editorAutoSaveOnExit = true;
    s.other.editorEnableBatchInsert = true;
    SaveAppSettings(s);
    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool roundtrip = loaded.other.editorDisableModifyButton
        && loaded.other.editorAutoSaveOnExit
        && loaded.other.editorEnableBatchInsert;

    AppSettings missing = DefaultAppSettings();
    SaveAppSettings(missing);
    WriteRawSettings(AppSettingsFilePath(),
        "{\"other\":{\"editorDefaultView\":\"code\",\"holdThresholdSeconds\":0.2}}\n");
    AppSettings loadedMissing{};
    LoadAppSettings(loadedMissing);
    const bool missingOff = !loadedMissing.other.editorDisableModifyButton
        && !loadedMissing.other.editorAutoSaveOnExit
        && !loadedMissing.other.editorEnableBatchInsert;

    const bool ok = defaultsOff && roundtrip && missingOff;
    Emit(L"save_load_editor_general_flags", ok,
        ok ? L"" : L"editor general flags default/roundtrip/missing failed");
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

void CaseLoadOmitsAutoHideKeepsDefault(SettingsFileGuard& /*g*/) {
    WriteRawSettings(AppSettingsFilePath(),
        u8"{\"other\":{\"closeToTray\":true}}");
    AppSettings loaded{};
    const bool okLoad = LoadAppSettings(loaded);
    const bool ok = okLoad && loaded.other.autoHideMainWindow && loaded.other.closeToTray;
    Emit(L"load_omits_auto_hide_keeps_default", ok,
        ok ? L"" : L"omitting autoHideMainWindow cleared the default");
}

void CaseGarbage(SettingsFileGuard& /*g*/) {
    WriteRawSettings(AppSettingsFilePath(), "{not json!!!}");
    AppSettings loaded{};
    LoadAppSettings(loaded);
    Emit(L"load_garbage_partial_safe",
        loaded.ai.modelName == L"gpt-4o"
            && loaded.other.themeId == quickscript::kDefaultThemeId
            && loaded.other.playSoundOnStart
            && loaded.other.playSoundOnEnd, L"");
}

void CaseTryLoadMissingLeavesOut(SettingsFileGuard& /*g*/) {
    DeleteFileW(AppSettingsFilePath().c_str());
    AppSettings out = DefaultAppSettings();
    out.playback.scheduledTaskConflictPolicy = 1;
    out.playback.scheduledTaskAutoResume = true;
    out.ai.apiKey = L"keep";
    const bool loaded = TryLoadAppSettings(out);
    const bool ok = !loaded
        && out.playback.scheduledTaskConflictPolicy == 1
        && out.playback.scheduledTaskAutoResume
        && out.ai.apiKey == L"keep";
    Emit(L"try_load_missing_leaves_out", ok,
        ok ? L"" : L"TryLoad mutated out or succeeded on missing file");
}

void CaseHomeRuntimeSavePreservesPlayback(SettingsFileGuard& /*g*/) {
    AppSettings disk = DefaultAppSettings();
    disk.playback.scheduledTaskConflictPolicy = 1;
    disk.playback.scheduledTaskAutoResume = true;
    disk.home.uiMode = L"pro";
    disk.home.globalHotkeyText = L"F9";
    disk.home.globalHotkeyVk = 0x78;
    disk.ai.apiKey = L"keep-key";
    disk.home.selectedScriptPath = L"C:\\old.json";
    SaveAppSettings(disk);
    {
        std::ifstream raw(AppSettingsFilePath(), std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(raw)),
            std::istreambuf_iterator<char>());
        if (bytes.find("keep-key") != std::string::npos
            || bytes.find("dpapi:") == std::string::npos) {
            Emit(L"home_runtime_save_preserves_playback", false,
                L"apiKey not DPAPI-wrapped on disk");
            return;
        }
    }

    AppSettings engine = DefaultAppSettings();
    engine.playback.scheduledTaskConflictPolicy = 0;
    engine.playback.scheduledTaskAutoResume = false;
    engine.home.uiMode = L"simple";
    engine.home.selectedScriptPath = L"C:\\new.json";
    engine.home.activeTab = 2;
    const bool saved = SaveAppSettingsPreserveUserSettings(engine, false);

    AppSettings loaded{};
    LoadAppSettings(loaded);
    const bool ok = saved
        && loaded.playback.scheduledTaskConflictPolicy == 1
        && loaded.playback.scheduledTaskAutoResume
        && loaded.home.uiMode == L"pro"
        && loaded.home.globalHotkeyText == L"F9"
        && loaded.home.globalHotkeyVk == 0x78
        && loaded.ai.apiKey == L"keep-key"
        && loaded.home.selectedScriptPath == L"C:\\new.json"
        && loaded.home.activeTab == 2
        && engine.playback.scheduledTaskConflictPolicy == 1;
    Emit(L"home_runtime_save_preserves_playback", ok,
        ok ? L"" : L"home persist clobbered playback/ai/uiMode/hotkey");
}

void WriteRawBytes(const std::wstring& path, const void* data, size_t n) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
}

void CaseStartupWavMissing() {
    const std::wstring path = AppDir() + L"\\__startup_missing.selftest.wav";
    DeleteFileW(path.c_str());
    const bool ok = !IsPlayableWavFile(path) && !IsPlayableWavFile(L"");
    Emit(L"startup_wav_missing_rejected", ok,
        ok ? L"" : L"missing/empty path should be rejected");
}

void CaseStartupWavGarbage() {
    const std::wstring path = AppDir() + L"\\__startup_garbage.selftest.wav";
    WriteRawBytes(path, "not a wav", 9);
    const bool garbage = !IsPlayableWavFile(path);
    WriteRawBytes(path, "RIFF", 4);
    const bool truncated = !IsPlayableWavFile(path);
    const char riffNotWave[12] = {'R','I','F','F',0,0,0,0,'A','V','I',' '};
    WriteRawBytes(path, riffNotWave, sizeof(riffNotWave));
    const bool wrongType = !IsPlayableWavFile(path);
    DeleteFileW(path.c_str());
    const bool ok = garbage && truncated && wrongType;
    Emit(L"startup_wav_garbage_rejected", ok,
        ok ? L"" : L"garbage/truncated/non-WAVE should be rejected");
}

void CaseStartupWavValidHeader() {
    const std::wstring path = AppDir() + L"\\__startup_valid.selftest.wav";
    const char wav[12] = {'R','I','F','F',4,0,0,0,'W','A','V','E'};
    WriteRawBytes(path, wav, sizeof(wav));
    const bool ok = IsPlayableWavFile(path);
    DeleteFileW(path.c_str());
    Emit(L"startup_wav_valid_header_accepted", ok,
        ok ? L"" : L"RIFF/WAVE header should be accepted");
}

void CaseFinishSoundPath() {
    const std::wstring path = AppFinishSoundFilePath();
    const std::wstring dir = AppDir();
    const bool ok = path.find(dir) == 0
        && path.size() == dir.size() + 11
        && path.compare(dir.size(), 11, L"\\finish.wav") == 0;
    Emit(L"finish_wav_path_sidecar", ok,
        ok ? L"" : path.c_str());
}

void CasePathIsUnderRoot() {
    const bool underPf = PathIsUnderRoot(L"C:\\Program Files\\QuickScriptTool",
        L"C:\\Program Files");
    const bool notPf86 = !PathIsUnderRoot(L"C:\\Program Files (x86)\\QuickScriptTool",
        L"C:\\Program Files");
    const bool caseOk = PathIsUnderRoot(L"c:\\program files\\x", L"C:\\Program Files");
    const bool selfOk = PathIsUnderRoot(L"C:\\Program Files", L"C:\\Program Files");
    const bool ok = underPf && notPf86 && caseOk && selfOk;
    Emit(L"path_is_under_root", ok,
        ok ? L"" : L"Program Files prefix matching failed");
}

void CaseWebViewUserDataProgramFiles() {
    const std::wstring dir = ResolveWebView2UserDataDir(
        L"C:\\Program Files\\QuickScriptTool",
        L"C:\\Program Files",
        L"C:\\Program Files (x86)",
        L"C:\\Users\\me\\AppData\\Local");
    const bool ok = dir == L"C:\\Users\\me\\AppData\\Local\\QuickScriptTool\\WebView2UserData";
    Emit(L"webview_userdata_programfiles_roams", ok,
        ok ? L"" : L"expected LocalAppData\\QuickScriptTool\\WebView2UserData");
}

void CaseWebViewUserDataPortable() {
    const std::wstring dir = ResolveWebView2UserDataDir(
        L"D:\\other\\software\\build\\Release",
        L"C:\\Program Files",
        L"C:\\Program Files (x86)",
        L"C:\\Users\\me\\AppData\\Local");
    const bool ok = dir == L"D:\\other\\software\\build\\Release\\WebView2UserData";
    Emit(L"webview_userdata_portable_sidecar", ok,
        ok ? L"" : L"expected exe-sidecar WebView2UserData");
}

void CaseWebViewFetchDataProgramFiles() {
    const std::wstring dir = ResolveWebView2FetchDataDir(
        L"C:\\Program Files\\QuickScriptTool",
        L"C:\\Program Files",
        L"C:\\Program Files (x86)",
        L"C:\\Users\\me\\AppData\\Local");
    const bool ok = dir == L"C:\\Users\\me\\AppData\\Local\\QuickScriptTool\\WebView2FetchData";
    Emit(L"webview_fetchdata_programfiles_roams", ok,
        ok ? L"" : L"expected LocalAppData\\QuickScriptTool\\WebView2FetchData");
}

void CaseExtractStringObjectLastWins() {
    const std::wstring nested =
        L"{\"ai\":{\"apiKey\":\"nested-secret\"},\"apiKey\":\"top-level\"}";
    const std::wstring top = ExtractString(nested, L"apiKey");
    const std::wstring dup =
        L"{\"apiKey\":\"first\",\"other\":1,\"apiKey\":\"second\"}";
    const std::wstring last = ExtractString(dup, L"apiKey");
    const std::wstring onlyNested = L"{\"ai\":{\"apiKey\":\"nested-only\"}}";
    const std::wstring missed = ExtractString(onlyNested, L"apiKey");
    const bool ok = top == L"top-level" && last == L"second" && missed.empty();
    std::wstring detail;
    if (top != L"top-level") detail += L"nested wins:" + top + L" ";
    if (last != L"second") detail += L"dup first-wins:" + last + L" ";
    if (!missed.empty()) detail += L"read nested:" + missed;
    Emit(L"extract_string_object_last_wins", ok, detail.c_str());
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
    CaseFloatBallGeom();

    const std::wstring testPath = AppDir() + L"\\app_settings.selftest.json";
    DeleteFileW(testPath.c_str());
    DeleteFileW((testPath + L".tmp").c_str());
    SetAppSettingsFilePathForTest(testPath);
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
        CaseLowPerformanceMode(guard);
        CasePlaybackSpeedMath(guard);
        CaseThemeClamp(guard);
        CaseCustomTheme(guard);
        CaseWmPreviewClamp(guard);
        CaseAiModels(guard);
        CaseHomeTab(guard);
        CaseHomeUiMode(guard);
        CaseGlobalHotkey(guard);
        CaseOtherOsFlags(guard);
        CaseFloatBallSettings(guard);
        CaseUiScaleFactor(guard);
        CaseEditorDefaultView(guard);
        CaseEditorVisualFlags(guard);
        CaseEditorActionCatalog(guard);
        CaseEditorVarFilters(guard);
        CaseEditorGeneralFlags(guard);
        CasePreferDirect2D(guard);
        CaseLoadOmitsAutoHideKeepsDefault(guard);
        CaseGarbage(guard);
        CaseTryLoadMissingLeavesOut(guard);
        CaseHomeRuntimeSavePreservesPlayback(guard);
        CaseStartupWavMissing();
        CaseStartupWavGarbage();
        CaseStartupWavValidHeader();
        CaseFinishSoundPath();
        CasePathIsUnderRoot();
        CaseWebViewUserDataProgramFiles();
        CaseWebViewUserDataPortable();
        CaseWebViewFetchDataProgramFiles();
        CaseExtractStringObjectLastWins();
    }
    SetAppSettingsFilePathForTest(L"");
    DeleteFileW(testPath.c_str());
    DeleteFileW((testPath + L".tmp").c_str());

    selftest::EmitSummary();
    return selftest::ExitCode();
}
