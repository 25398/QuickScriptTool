// =============================================================================
// ScriptIoSelfTest — 脚本 JSON 读写 / 录制路径自检
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:ScriptIoSelfTest
//   build\Release\ScriptIoSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "script_io.h"
#include "image_var_util.h"
#include "utils.h"

#include <cmath>
#include <fstream>
#include <string>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"recording_path_under_recordings", L"default",
        L"IsRecordingScriptPath true under AppDir\\recordings"},
    {L"recording_path_forward_slash", L"default",
        L"IsRecordingScriptPath true after canonicalizing mixed slashes"},
    {L"recording_path_scripts_false", L"default",
        L"scripts\\foo.json is not a recording path"},
    {L"breakout_normalize_nonpositive", L"default",
        L"NormalizeBreakoutTimeSeconds: <=0 -> 0"},
    {L"breakout_disabled_when_window_mode", L"default",
        L"EffectiveBreakoutTimeSeconds is 0 when WM enabled"},
    {L"parse_action_wait_block", L"default",
        L"ParseScriptActionBlock wait with duration"},
    {L"parse_action_unknown_is_custom", L"default",
        L"Unknown action type becomes CustomText"},
    {L"parse_normalized_xy_fields", L"default",
        L"coordsNormalized=true fills nx/ny"},
    {L"parse_legacy_pixel_xy", L"default",
        L"coordsNormalized=false fills x/y"},
    {L"parse_move_mouse_relative_pixels", L"default",
        L"moveMouseRelative keeps integer dx/dy even if coordsNormalized"},
    {L"write_move_mouse_relative_ints", L"default",
        L"ScriptActionToJsonString writes integer dx/dy for relative move"},
    {L"save_load_roundtrip_actions", L"default",
        L"Save/Load denorm=false keeps wait action"},
    {L"hotkey_hold_roundtrip", L"default",
        L"hotkeyHold 1/0 and legacy true/false load as holdMode"},
    {L"load_invalid_coordmeta_fallback", L"default",
        L"Broken coordMeta falls back to standard meta"},
    {L"parse_truncated_no_crash", L"default",
        L"Truncated JSON does not abort"},
    {L"write_action_json_contains_type", L"default",
        L"ScriptActionToJsonString emits type wait"},
    {L"timing_us_roundtrip", L"default",
        L"Wait timingUs survives parse/write roundtrip"},
    {L"mouse_playback_speed_roundtrip", L"default",
        L"mousePlayback playbackSpeed parse/write; missing=1; clamp 0.25~4"},
    {L"write_norm_xy_keeps_pixel", L"default",
        L"normalized x/y JSON uses enough precision to round-trip screen pixels"},
    {L"instant_duration_default_zero", L"default",
        L"moveMouse missing duration parses as 0"},
    {L"recorded_capture_path_roundtrip", L"default",
        L"recordedCapturePath + captureOffset roundtrip"},
    {L"collect_image_paths_recorded_capture", L"default",
        L"CollectImagePathsFromJson includes recordedCapturePath"},
    {L"parse_findimage_save_image", L"default",
        L"findImageFollowUp=3 + imageUseVar roundtrip fields"},
    {L"parse_findimage_perfect_match", L"default",
        L"perfectMatch field roundtrip"},
    {L"parse_quickinput_escapes", L"default",
        L"parseEscapes default 0; 1/true roundtrip"},
    {L"looks_like_file_path", L"default",
        L"LooksLikeFilePath distinguishes paths from var names"},
    {L"window_relative_pixel_xy", L"default",
        L"windowRelative 动作在 coordsNormalized 文件里仍按像素读取 x/y"},
    {L"recording_keeps_window_relative_mode", L"default",
        L"recordings 路径保存时保留窗口相对 windowMode（不得清成全屏）"},
    {L"recording_recovers_wm_from_rel_actions", L"default",
        L"录制保存时仅凭 windowRelative 动作 + 窗口身份即可复活 enabled"},
    {L"script_default_mode_not_revived", L"default",
        L"鼠标宏 scripts 路径 enabled=0 保存后再读不得复活为窗口模式"},
    {L"recording_wipes_editor_window_mode", L"default",
        L"普通录制仍强制关闭编辑器残留的非窗口相对 windowMode"},
    {L"save_after_load_false_keeps_xy", L"default",
        L"Load denorm=false 后改 wait 再 Save 不得把 move/click 坐标冲成原点"},
};

std::wstring TempScriptPath(const wchar_t* name) {
    return AppDir() + L"\\selftest_" + name + L".json";
}

void WriteUtf8File(const std::wstring& path, const std::string& utf8) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

void CaseRecordingPathTrue() {
    const std::wstring path = RecordingsDir() + L"\\unit-rec.json";
    Emit(L"recording_path_under_recordings", IsRecordingScriptPath(path), path.c_str());
}

void CaseRecordingPathForwardSlash() {
    std::wstring slashy = RecordingsDir();
    for (auto& ch : slashy) {
        if (ch == L'\\') ch = L'/';
    }
    slashy += L"/unit-rec.json";
    Emit(L"recording_path_forward_slash", IsRecordingScriptPath(slashy), slashy.c_str());
}

void CaseRecordingPathFalse() {
    const std::wstring path = AppDir() + L"\\scripts\\foo.json";
    Emit(L"recording_path_scripts_false", !IsRecordingScriptPath(path), path.c_str());
}

void CaseBreakoutNormalize() {
    const bool ok = NormalizeBreakoutTimeSeconds(0.0) == 0.0
        && NormalizeBreakoutTimeSeconds(-1.0) == 0.0
        && NormalizeBreakoutTimeSeconds(2.5) == 2.5;
    Emit(L"breakout_normalize_nonpositive", ok, ok ? L"" : L"normalize rules broken");
}

void CaseBreakoutWm() {
    ScriptFileData data{};
    data.breakoutTimeSeconds = 5.0;
    data.windowMode.enabled = true;
    Emit(L"breakout_disabled_when_window_mode",
        EffectiveBreakoutTimeSeconds(data) == 0.0, L"");
}

void CaseParseWait() {
    const std::wstring block =
        L"{\"type\":\"wait\",\"duration\":1.5,\"no\":3}";
    const ScriptAction a = ParseScriptActionBlock(block, 99, false);
    const bool ok = a.type == ActionType::Wait && a.duration == 1.5 && a.originalNo == 3;
    Emit(L"parse_action_wait_block", ok, ok ? L"" : L"wait parse failed");
}

void CaseParseUnknown() {
    const ScriptAction a = ParseScriptActionBlock(
        L"{\"type\":\"notARealType\",\"text\":\"x\"}", 1, false);
    Emit(L"parse_action_unknown_is_custom", a.type == ActionType::CustomText, L"");
}

void CaseParseNormXy() {
    const ScriptAction a = ParseScriptActionBlock(
        L"{\"type\":\"moveMouse\",\"x\":0.25,\"y\":0.75}", 1, true);
    const bool ok = a.type == ActionType::MoveMouse
        && std::fabs(a.nx - 0.25) < 1e-6 && std::fabs(a.ny - 0.75) < 1e-6;
    Emit(L"parse_normalized_xy_fields", ok, ok ? L"" : L"nx/ny not filled");
}

void CaseParseLegacyXy() {
    const ScriptAction a = ParseScriptActionBlock(
        L"{\"type\":\"moveMouse\",\"x\":100,\"y\":200}", 1, false);
    Emit(L"parse_legacy_pixel_xy", a.x == 100 && a.y == 200, L"");
}

void CaseParseMoveMouseRelative() {
    const ScriptAction a = ParseScriptActionBlock(
        L"{\"type\":\"moveMouseRelative\",\"x\":-15,\"y\":8,\"randomX\":1}", 1, true);
    const bool ok = a.type == ActionType::MoveMouseRelative
        && a.x == -15 && a.y == 8 && a.randomX == 1
        && !a.coordsAreNormalized;
    Emit(L"parse_move_mouse_relative_pixels", ok, ok ? L"" : L"relative parse wrong");
}

void CaseWriteMoveMouseRelative() {
    ScriptAction a{};
    a.type = ActionType::MoveMouseRelative;
    a.x = -7;
    a.y = 9;
    a.coordsAreNormalized = true; // 即便误标，写出仍应是整数像素
    a.nx = 0.25;
    a.ny = 0.75;
    const std::wstring s = ScriptActionToJsonString(a);
    const bool ok = s.find(L"moveMouseRelative") != std::wstring::npos
        && s.find(L"\"x\": -7") != std::wstring::npos
        && s.find(L"\"y\": 9") != std::wstring::npos
        && s.find(L"\"x\": 0.25") == std::wstring::npos
        && s.find(L"\"y\": 0.75") == std::wstring::npos;
    Emit(L"write_move_mouse_relative_ints", ok, s.c_str());
}

void CaseSaveLoadRoundtrip() {
    const std::wstring path = TempScriptPath(L"io_roundtrip");
    ScriptFileData data{};
    data.scriptName = L"selftest-io";
    data.coordMeta = StandardScriptCoordMeta();
    data.coordsNormalized = true;
    data.recordingCaptureMode = 2;
    data.inputTimingVersion = 2;
    ScriptAction wait{};
    wait.type = ActionType::Wait;
    wait.duration = 0.2;
    wait.timingUs = 200000;
    wait.originalNo = 1;
    data.actions.push_back(wait);
    ScriptAction stop{};
    stop.type = ActionType::StopMacro;
    stop.originalNo = 2;
    data.actions.push_back(stop);

    const bool saved = SaveScriptFileData(path, data);
    ScriptFileData loaded = LoadScriptFileData(path, false);
    DeleteFileW(path.c_str());

    const bool ok = saved && loaded.actions.size() >= 1
        && loaded.actions[0].type == ActionType::Wait
        && std::fabs(loaded.actions[0].duration - 0.2) < 1e-6
        && loaded.actions[0].timingUs == 200000
        && loaded.recordingCaptureMode == 2
        && loaded.inputTimingVersion == 2;
    Emit(L"save_load_roundtrip_actions", ok, ok ? L"" : L"roundtrip failed");
}

void CaseHotkeyHoldRoundtrip() {
    const std::wstring path = TempScriptPath(L"io_hold");
    ScriptFileData data{};
    data.scriptName = L"hold-test";
    data.hotkey = Hotkey{0, VK_F9, L"F9", true, true};
    data.coordMeta = StandardScriptCoordMeta();
    data.coordsNormalized = true;
    const bool saved = SaveScriptFileData(path, data);
    ScriptFileData loaded = LoadScriptFileData(path, false);
    const std::wstring written = ReadAll(path);
    DeleteFileW(path.c_str());

    const bool writeOk = saved && written.find(L"\"hotkeyHold\": 1") != std::wstring::npos
        && loaded.hotkey.holdMode && loaded.hotkey.vk == VK_F9;

    // 兼容旧文件里的 true/false（曾用 ExtractNumber 读读成点击）
    const bool legacyTrue = ExtractBool(
        L"{\"hotkeyHold\": true, \"hotkeyVk\": 120}", L"hotkeyHold", false);
    const bool legacyFalse = !ExtractBool(
        L"{\"hotkeyHold\": false, \"hotkeyVk\": 120}", L"hotkeyHold", true);
    const bool numZero = !ExtractBool(L"{\"hotkeyHold\": 0}", L"hotkeyHold", true);

    const bool ok = writeOk && legacyTrue && legacyFalse && numZero;
    Emit(L"hotkey_hold_roundtrip", ok,
        ok ? L"" : L"holdMode lost on save/load or legacy true parse");
}

void CaseInvalidCoordMeta() {
    const std::wstring path = TempScriptPath(L"io_badmeta");
    // refWidth 0 should fall back to standard when loading
    WriteUtf8File(path,
        u8"{\"name\":\"bad\",\"coordMeta\":{\"refWidth\":0,\"refHeight\":0},"
        u8"\"actions\":[{\"type\":\"wait\",\"duration\":0.1}]}");
    ScriptFileData loaded = LoadScriptFileData(path, false);
    DeleteFileW(path.c_str());
    const bool ok = loaded.coordMeta.refWidth == 2560 && loaded.coordMeta.refHeight == 1440;
    Emit(L"load_invalid_coordmeta_fallback", ok,
        ok ? L"" : L"expected StandardScriptCoordMeta fallback");
}

void CaseTruncated() {
    ScriptFileData parsed = ParseScriptContent(L"{\"name\":\"x\",\"actions\":[{\"type\":\"wa");
    Emit(L"parse_truncated_no_crash", true,
        (L"actions=" + std::to_wstring(parsed.actions.size())).c_str());
}

void CaseWriteActionJson() {
    ScriptAction a{};
    a.type = ActionType::Wait;
    a.duration = 0.1;
    const std::wstring s = ScriptActionToJsonString(a);
    Emit(L"write_action_json_contains_type",
        s.find(L"\"type\"") != std::wstring::npos
            && s.find(L"wait") != std::wstring::npos, s.c_str());
}

void CaseTimingUsRoundtrip() {
    ScriptAction a{};
    a.type = ActionType::Wait;
    a.duration = 0.012345;
    a.timingUs = 12345;
    const std::wstring json = ScriptActionToJsonString(a);
    ScriptAction loaded = ParseScriptActionBlock(json, 1, false);
    const bool ok = loaded.type == ActionType::Wait
        && loaded.timingUs == 12345
        && json.find(L"\"timingUs\"") != std::wstring::npos;
    Emit(L"timing_us_roundtrip", ok,
        ok ? L"" : (L"json=" + json).c_str());
}

void CaseMousePlaybackSpeed() {
    ScriptAction a{};
    a.type = ActionType::MousePlayback;
    a.targetPath = L"rec.json";
    a.playbackSpeed = 2.0;
    const std::wstring json = ScriptActionToJsonString(a);
    ScriptAction loaded = ParseScriptActionBlock(json, 0, false);
    const bool written = json.find(L"\"playbackSpeed\"") != std::wstring::npos;
    const bool roundtrip = loaded.type == ActionType::MousePlayback
        && std::fabs(loaded.playbackSpeed - 2.0) < 1e-9;
    ScriptAction missing = ParseScriptActionBlock(
        L"{\"type\":\"mousePlayback\",\"targetPath\":\"a.json\"}", 0, false);
    const bool defOne = std::fabs(missing.playbackSpeed - 1.0) < 1e-9;
    ScriptAction hi = ParseScriptActionBlock(
        L"{\"type\":\"mousePlayback\",\"playbackSpeed\":9}", 0, false);
    ScriptAction lo = ParseScriptActionBlock(
        L"{\"type\":\"mousePlayback\",\"playbackSpeed\":0.01}", 0, false);
    const bool clamped = std::fabs(hi.playbackSpeed - 4.0) < 1e-9
        && std::fabs(lo.playbackSpeed - 0.25) < 1e-9;
    const bool ok = written && roundtrip && defOne && clamped;
    Emit(L"mouse_playback_speed_roundtrip", ok,
        ok ? L"" : (L"json=" + json).c_str());
}

void CaseWriteNormXyKeepsPixel() {
    ScriptAction a{};
    a.type = ActionType::MoveMouse;
    a.coordsAreNormalized = true;
    a.nx = 632.0 / 1920.0;
    a.ny = 356.0 / 1080.0;
    const std::wstring json = ScriptActionToJsonString(a);
    ScriptAction loaded = ParseScriptActionBlock(json, 0, true);
    const int x = static_cast<int>(std::llround(loaded.nx * 1920.0));
    const int y = static_cast<int>(std::llround(loaded.ny * 1080.0));
    const bool ok = x == 632 && y == 356;
    Emit(L"write_norm_xy_keeps_pixel", ok,
        ok ? L"" : (L"json=" + json).c_str());
}

void CaseInstantDurationDefaultZero() {
    ScriptAction loaded = ParseScriptActionBlock(
        L"{\"type\":\"moveMouse\",\"x\":1,\"y\":2}", 0, false);
    Emit(L"instant_duration_default_zero",
        loaded.type == ActionType::MoveMouse
            && loaded.duration == 0.0
            && loaded.timingUs == 0, L"");
}

void CaseRecordedCaptureRoundtrip() {
    ScriptAction a{};
    a.type = ActionType::MouseDown;
    a.button = MouseButtonType::Left;
    a.x = 12;
    a.y = 34;
    a.recordedCapturePath = L"images\\rec_1_2.bmp";
    a.captureOffsetX = 3;
    a.captureOffsetY = -4;
    const std::wstring json = ScriptActionToJsonString(a);
    const ScriptAction loaded = ParseScriptActionBlock(json, 0, false);
    const bool ok = loaded.type == ActionType::MouseDown
        && loaded.x == 12 && loaded.y == 34
        && loaded.captureOffsetX == 3 && loaded.captureOffsetY == -4
        && !loaded.recordedCapturePath.empty()
        && json.find(L"recordedCapturePath") != std::wstring::npos;
    Emit(L"recorded_capture_path_roundtrip", ok,
        ok ? L"" : (L"json=" + json).c_str());
}

void CaseCollectRecordedCapture() {
    const std::wstring json =
        L"{\"actions\":[{\"type\":\"mouseDown\",\"recordedCapturePath\":\"images\\\\rec_x.bmp\"}]}";
    auto paths = CollectImagePathsFromJson(json);
    bool found = false;
    for (const auto& p : paths) {
        if (p.find(L"rec_x.bmp") != std::wstring::npos) found = true;
    }
    Emit(L"collect_image_paths_recorded_capture", found, L"");
}

void CaseParseFindImageSaveImage() {
    const ScriptAction a = ParseScriptActionBlock(
        L"{\"type\":\"findImage\",\"findImageFollowUp\":3,\"imageUseVar\":1,"
        L"\"imagePath\":\"image\",\"matchVarName\":\"image\",\"searchFullScreen\":1}",
        1, false);
    const bool ok = a.type == ActionType::FindImage
        && a.findImageFollowUp == 3
        && a.imageUseVar
        && a.imagePath == L"image"
        && a.matchVarName == L"image"
        && a.findTimeExpr == L"0";
    const std::wstring written = ScriptActionToJsonString(a);
    const bool wrote = written.find(L"\"imageUseVar\": 1") != std::wstring::npos
        && written.find(L"\"findImageFollowUp\": 3") != std::wstring::npos;
    Emit(L"parse_findimage_save_image", ok && wrote,
        (ok && wrote) ? L"" : (L"json=" + written).c_str());
}

void CaseParseFindImagePerfectMatch() {
    const ScriptAction a = ParseScriptActionBlock(
        L"{\"type\":\"findImage\",\"perfectMatch\":1,\"matchThreshold\":65,"
        L"\"imagePath\":\"images\\\\a.bmp\",\"searchFullScreen\":1}",
        1, false);
    const bool ok = a.type == ActionType::FindImage && a.perfectMatch;
    const std::wstring written = ScriptActionToJsonString(a);
    const bool wrote = written.find(L"\"perfectMatch\": 1") != std::wstring::npos;
    Emit(L"parse_findimage_perfect_match", ok && wrote,
        (ok && wrote) ? L"" : (L"json=" + written).c_str());
}

void CaseParseQuickInputEscapes() {
    const ScriptAction def = ParseScriptActionBlock(
        L"{\"type\":\"quickInput\",\"inputText\":\"hello\"}", 0, false);
    const ScriptAction off = ParseScriptActionBlock(
        L"{\"type\":\"quickInput\",\"inputText\":\"x\",\"parseEscapes\":0}",
        0, false);
    const ScriptAction fromFalse = ParseScriptActionBlock(
        L"{\"type\":\"quickInput\",\"inputText\":\"x\",\"parseEscapes\":false}",
        0, false);
    const ScriptAction on = ParseScriptActionBlock(
        L"{\"type\":\"quickInput\",\"inputText\":\"x\",\"parseEscapes\":1}",
        0, false);
    const ScriptAction fromTrue = ParseScriptActionBlock(
        L"{\"type\":\"quickInput\",\"inputText\":\"x\",\"parseEscapes\":true}",
        0, false);
    ScriptAction writeOff{};
    writeOff.type = ActionType::QuickInput;
    writeOff.inputText = L"x";
    writeOff.parseEscapes = false;
    const std::wstring writtenOff = ScriptActionToJsonString(writeOff);
    ScriptAction writeOn{};
    writeOn.type = ActionType::QuickInput;
    writeOn.inputText = L"x";
    writeOn.parseEscapes = true;
    const std::wstring writtenOn = ScriptActionToJsonString(writeOn);
    const ScriptAction roundOff = ParseScriptActionBlock(writtenOff, 0, false);
    const ScriptAction roundOn = ParseScriptActionBlock(writtenOn, 0, false);
    const bool ok = !def.parseEscapes
        && !off.parseEscapes && !fromFalse.parseEscapes
        && on.parseEscapes && fromTrue.parseEscapes
        && writtenOff.find(L"\"parseEscapes\": 0") != std::wstring::npos
        && writtenOn.find(L"\"parseEscapes\": 1") != std::wstring::npos
        && !roundOff.parseEscapes && roundOn.parseEscapes;
    Emit(L"parse_quickinput_escapes", ok,
        ok ? L"" : (L"json0=" + writtenOff + L" json1=" + writtenOn).c_str());
}

void CaseLooksLikeFilePath() {
    const bool ok = LooksLikeFilePath(L"C:\\Users\\a\\b.png")
        && LooksLikeFilePath(L"D:/tmp/x")
        && !LooksLikeFilePath(L"image")
        && !LooksLikeFilePath(L"matchRet");
    Emit(L"looks_like_file_path", ok, ok ? L"" : L"LooksLikeFilePath heuristic broken");
}

void CaseWindowRelativePixelXy() {
    // 窗口相对录制文件带 coordMeta（coordsNormalized=true 路径），
    // 但 windowRelative 动作的 x/y 是客户区像素：必须按像素读，不能当归一化。
    const ScriptAction a = ParseScriptActionBlock(
        L"{\"type\":\"mouseDown\",\"x\":171,\"y\":730,\"windowRelative\":1,"
        L"\"searchX1\":5,\"offsetX\":7,\"randomX\":3}", 1, true);
    const bool ok = a.windowRelative
        && !a.coordsAreNormalized
        && a.x == 171 && a.y == 730
        && a.randomX == 3 && a.searchX1 == 5 && a.offsetX == 7;
    Emit(L"window_relative_pixel_xy", ok, ok ? L"" : L"windowRelative pixel parse wrong");
}

void CaseRecordingKeepsWindowRelativeMode() {
    EnsureScriptsDir();
    const std::wstring path = RecordingsDir() + L"\\selftest_wm_rec.json";
    ScriptFileData data{};
    data.scriptName = L"selftest_wm_rec";
    data.windowMode.enabled = true;
    data.windowMode.executionKind = windowmode::WindowModeExecutionKind::BackgroundWindow;
    data.windowMode.coordSpace = windowmode::WindowModeCoordinateSpace::WindowClient;
    data.windowMode.windowRelativeCoordinates = true;
    data.windowMode.windowClassName = L"Notepad";
    data.windowMode.recordClientWidth = 800;
    data.windowMode.recordClientHeight = 600;
    ScriptAction a{};
    a.type = ActionType::MouseDown;
    a.windowRelative = true;
    a.coordsAreNormalized = false;
    a.x = 120;
    a.y = 80;
    a.originalNo = 1;
    data.actions.push_back(a);

    const bool saved = SaveScriptFileData(path, data);
    ScriptFileData loaded = LoadScriptFileData(path, false);
    const std::wstring written = ReadAll(path);
    DeleteFileW(path.c_str());

    const bool ok = saved
        && loaded.windowMode.enabled
        && loaded.windowMode.windowRelativeCoordinates
        && loaded.windowMode.recordClientWidth == 800
        && loaded.windowMode.recordClientHeight == 600
        && loaded.windowMode.windowClassName == L"Notepad"
        && loaded.coordMeta.captureWidth == 800
        && loaded.coordMeta.captureHeight == 600
        && !loaded.actions.empty()
        && loaded.actions[0].windowRelative
        && loaded.actions[0].x == 120
        && loaded.actions[0].y == 80
        && written.find(L"\"windowRelativeCoordinates\": 1") != std::wstring::npos;
    Emit(L"recording_keeps_window_relative_mode", ok,
        ok ? L"" : (L"saved=" + std::to_wstring(saved ? 1 : 0)
            + L" enabled=" + std::to_wstring(loaded.windowMode.enabled ? 1 : 0)
            + L" rel=" + std::to_wstring(loaded.windowMode.windowRelativeCoordinates ? 1 : 0)
            + L" class=" + loaded.windowMode.windowClassName).c_str());
}

void CaseRecordingRecoversWmFromRelActions() {
    EnsureScriptsDir();
    const std::wstring path = RecordingsDir() + L"\\selftest_wm_recover.json";
    ScriptFileData data{};
    data.scriptName = L"selftest_wm_recover";
    data.windowMode.enabled = false;
    data.windowMode.windowRelativeCoordinates = false;
    data.windowMode.windowClassName = L"UnityWndClass";
    data.windowMode.targetExePath = L"C:\\Games\\game.exe";
    ScriptAction a{};
    a.type = ActionType::FindImage;
    a.windowRelative = true;
    a.searchFullScreen = true;
    a.originalNo = 1;
    data.actions.push_back(a);

    const bool saved = SaveScriptFileData(path, data);
    ScriptFileData loaded = LoadScriptFileData(path, false);
    DeleteFileW(path.c_str());

    const bool ok = saved
        && loaded.windowMode.enabled
        && loaded.windowMode.windowRelativeCoordinates
        && loaded.windowMode.windowClassName == L"UnityWndClass"
        && loaded.windowMode.executionKind == windowmode::WindowModeExecutionKind::BackgroundWindow
        && !loaded.actions.empty()
        && loaded.actions[0].windowRelative;
    Emit(L"recording_recovers_wm_from_rel_actions", ok,
        ok ? L"" : (L"saved=" + std::to_wstring(saved ? 1 : 0)
            + L" enabled=" + std::to_wstring(loaded.windowMode.enabled ? 1 : 0)
            + L" rel=" + std::to_wstring(loaded.windowMode.windowRelativeCoordinates ? 1 : 0)
            + L" class=" + loaded.windowMode.windowClassName).c_str());
}

void CaseScriptDefaultModeNotRevived() {
    EnsureScriptsDir();
    const std::wstring path = ScriptsDir() + L"\\selftest_wm_default.json";
    ScriptFileData data{};
    data.scriptName = L"selftest_wm_default";
    data.windowMode.enabled = false;
    data.windowMode.windowRelativeCoordinates = true;
    data.windowMode.windowClassName = L"UnityWndClass";
    data.windowMode.targetExePath = L"C:\\Games\\game.exe";
    data.windowMode.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
    ScriptAction a{};
    a.type = ActionType::FindImage;
    a.windowRelative = true;
    a.searchFullScreen = true;
    a.originalNo = 1;
    data.actions.push_back(a);

    const bool saved = SaveScriptFileData(path, data);
    ScriptFileData loaded = LoadScriptFileData(path, false);
    ScriptFileData parsed = ParseScriptContent(ReadAll(path));
    DeleteFileW(path.c_str());

    const bool ok = saved
        && !loaded.windowMode.enabled
        && loaded.windowMode.windowRelativeCoordinates
        && loaded.windowMode.windowClassName == L"UnityWndClass"
        && loaded.windowMode.executionKind == windowmode::WindowModeExecutionKind::HiddenDesktop
        && !parsed.windowMode.enabled
        && !loaded.actions.empty()
        && loaded.actions[0].windowRelative;
    Emit(L"script_default_mode_not_revived", ok,
        ok ? L"" : (L"saved=" + std::to_wstring(saved ? 1 : 0)
            + L" enabled=" + std::to_wstring(loaded.windowMode.enabled ? 1 : 0)
            + L" kind=" + std::to_wstring(static_cast<int>(loaded.windowMode.executionKind))
            + L" parseEnabled=" + std::to_wstring(parsed.windowMode.enabled ? 1 : 0)).c_str());
}

void CaseRecordingWipesEditorWindowMode() {
    EnsureScriptsDir();
    const std::wstring path = RecordingsDir() + L"\\selftest_screen_rec.json";
    ScriptFileData data{};
    data.scriptName = L"selftest_screen_rec";
    data.windowMode.enabled = true;
    data.windowMode.windowRelativeCoordinates = false;
    data.windowMode.windowClassName = L"ShouldBeCleared";
    ScriptAction a{};
    a.type = ActionType::MoveMouse;
    a.coordsAreNormalized = false;
    a.x = 400;
    a.y = 300;
    data.actions.push_back(a);

    const bool saved = SaveScriptFileData(path, data);
    ScriptFileData loaded = LoadScriptFileData(path, false);
    DeleteFileW(path.c_str());

    const bool ok = saved
        && !loaded.windowMode.enabled
        && !loaded.windowMode.windowRelativeCoordinates
        && loaded.windowMode.windowClassName.empty();
    Emit(L"recording_wipes_editor_window_mode", ok,
        ok ? L"" : L"ordinary recording must still strip leftover editor windowMode");
}

void CaseSaveAfterLoadFalseKeepsXy() {
    // 回归：优化改延时 / 改热键等路径 Load(..., false) 只带 n*，
    // Save 若用 x/y=0 再 SyncNorm 会把坐标冲成原点。
    const std::wstring path = TempScriptPath(L"io_save_load_false_xy");
    ScriptFileData data{};
    data.scriptName = L"selftest_save_load_false_xy";
    data.coordMeta = StandardScriptCoordMeta();
    data.coordsNormalized = true;
    data.inputTimingVersion = 2;

    ScriptAction wait{};
    wait.type = ActionType::Wait;
    wait.duration = 0.05;
    wait.timingUs = 50000;
    wait.originalNo = 1;
    data.actions.push_back(wait);

    ScriptAction move{};
    move.type = ActionType::MoveMouse;
    move.coordsAreNormalized = false;
    move.x = 800;
    move.y = 450;
    move.originalNo = 2;
    data.actions.push_back(move);

    ScriptAction down{};
    down.type = ActionType::MouseDown;
    down.coordsAreNormalized = false;
    down.x = 800;
    down.y = 450;
    down.button = MouseButtonType::Left;
    down.originalNo = 3;
    data.actions.push_back(down);

    if (!SaveScriptFileData(path, data)) {
        Emit(L"save_after_load_false_keeps_xy", false, L"initial save failed");
        return;
    }

    ScriptFileData loaded = LoadScriptFileData(path, false);
    if (loaded.actions.size() < 3
        || loaded.actions[1].type != ActionType::MoveMouse
        || loaded.actions[2].type != ActionType::MouseDown) {
        DeleteFileW(path.c_str());
        Emit(L"save_after_load_false_keeps_xy", false, L"load false shape");
        return;
    }
    const double nxMove = loaded.actions[1].nx;
    const double nyMove = loaded.actions[1].ny;
    const double nxDown = loaded.actions[2].nx;
    const double nyDown = loaded.actions[2].ny;
    if (!(std::fabs(nxMove) > 1e-6 || std::fabs(nyMove) > 1e-6)) {
        DeleteFileW(path.c_str());
        Emit(L"save_after_load_false_keeps_xy", false, L"expected non-zero nx/ny after first save");
        return;
    }

    loaded.actions[0].duration = 0.2;
    loaded.actions[0].timingUs = 200000;
    const bool saved2 = SaveScriptFileData(path, loaded);
    ScriptFileData again = LoadScriptFileData(path, false);
    DeleteFileW(path.c_str());

    const bool ok = saved2
        && again.actions.size() >= 3
        && again.actions[1].type == ActionType::MoveMouse
        && again.actions[2].type == ActionType::MouseDown
        && again.actions[1].coordsAreNormalized
        && again.actions[2].coordsAreNormalized
        && std::fabs(again.actions[1].nx - nxMove) < 1e-9
        && std::fabs(again.actions[1].ny - nyMove) < 1e-9
        && std::fabs(again.actions[2].nx - nxDown) < 1e-9
        && std::fabs(again.actions[2].ny - nyDown) < 1e-9
        && std::fabs(again.actions[0].duration - 0.2) < 1e-6;
    Emit(L"save_after_load_false_keeps_xy", ok,
        ok ? L"" : L"wait edit via load(false)+save wiped move/click norms");
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
                L"  ScriptIoSelfTest.exe [--json] [--list] [--help]\n");
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"ScriptIoSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }
    if (!selftest::gJson) {
        std::fwprintf(stderr, L"=== ScriptIoSelfTest ===\n");
    }

    CaseRecordingPathTrue();
    CaseRecordingPathForwardSlash();
    CaseRecordingPathFalse();
    CaseBreakoutNormalize();
    CaseBreakoutWm();
    CaseParseWait();
    CaseParseUnknown();
    CaseParseNormXy();
    CaseParseLegacyXy();
    CaseParseMoveMouseRelative();
    CaseWriteMoveMouseRelative();
    CaseSaveLoadRoundtrip();
    CaseHotkeyHoldRoundtrip();
    CaseInvalidCoordMeta();
    CaseTruncated();
    CaseWriteActionJson();
    CaseTimingUsRoundtrip();
    CaseMousePlaybackSpeed();
    CaseWriteNormXyKeepsPixel();
    CaseInstantDurationDefaultZero();
    CaseRecordedCaptureRoundtrip();
    CaseCollectRecordedCapture();
    CaseParseFindImageSaveImage();
    CaseParseFindImagePerfectMatch();
    CaseParseQuickInputEscapes();
    CaseLooksLikeFilePath();
    CaseWindowRelativePixelXy();
    CaseRecordingKeepsWindowRelativeMode();
    CaseRecordingRecoversWmFromRelActions();
    CaseScriptDefaultModeNotRevived();
    CaseRecordingWipesEditorWindowMode();
    CaseSaveAfterLoadFalseKeepsXy();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
