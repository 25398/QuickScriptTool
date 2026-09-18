// =============================================================================
// ScriptIoSelfTest — 脚本 JSON 读写 / 录制路径自检
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:ScriptIoSelfTest
//   build\Release\ScriptIoSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "script_io.h"
#include "image_match.h"
#include "image_var_util.h"
#include "utils.h"

#include <cmath>
#include <cstring>
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
    {L"nested_use_mode_inherit_default", L"default",
        L"runMacro/mousePlayback missing useMode parses as inherit(3)"},
    {L"nested_use_mode_window_roundtrip", L"default",
        L"useMode + nestedWindowMode + breakoutTimeSeconds roundtrip"},
    {L"write_norm_xy_keeps_pixel", L"default",
        L"normalized x/y JSON uses enough precision to round-trip screen pixels"},
    {L"instant_duration_default_zero", L"default",
        L"moveMouse missing duration parses as 0"},
    {L"recorded_capture_path_roundtrip", L"default",
        L"recordedCapturePath + captureOffset roundtrip"},
    {L"collect_image_paths_recorded_capture", L"default",
        L"CollectImagePathsFromJson includes recordedCapturePath"},
    {L"parse_findimage_save_image", L"default",
        L"findImageFollowUp=3 + imageUseVar roundtrip；有模板保留 findTimeExpr；无模板清 0"},
    {L"parse_findimage_perfect_match", L"default",
        L"perfectMatch field roundtrip"},
    {L"parse_multimatch_roundtrip", L"default",
        L"multiMatch imagePaths/mode/sort/followUp=2 保留 findTimeExpr"},
    {L"parse_multimatch_template_t_path", L"default",
        L"imagePaths 含 template_ 文件名不得把 \\t 吃成 imagestemplate_"},
    {L"parse_multimatch_image_use_vars", L"default",
        L"imageUseVars 与变量图槽位 roundtrip"},
    {L"parse_multimatch_hole_use_vars", L"default",
        L"imagePaths 中间空槽不得把后面的 imageUseVars 错位"},
    {L"resolve_imagestemplate_typo", L"default",
        L"旧 bug 把 template_ 吃成 imagestemplate_ 时应找回原文件"},
    {L"parse_watchimage_resume", L"default",
        L"watchImage resumeAfterWatch + imagePath roundtrip"},
    {L"parse_watchimage_time_mode", L"default",
        L"watchImage watchMode=1/time + watchPollSeconds roundtrip"},
    {L"parse_mousedrag_abs", L"default",
        L"mouseDrag 绝对坐标 endX/endY/duration/imageLocate=0 roundtrip"},
    {L"parse_mousedrag_image_locate", L"default",
        L"mouseDrag imageLocate + imagePath + 相对偏移 roundtrip"},
    {L"parse_color_actions_image_locate", L"default",
        L"getColor/colorMatch/findColor imageLocate + imagePath roundtrip"},
    {L"parse_findcolor_keeps_find_time", L"default",
        L"findColor followUp=2 不得清 findTimeExpr；findImage followUp=2 同样保留"},
    {L"save_load_color_image_locate", L"default",
        L"Save/Load getColor 找图定位按模板归一化；findColor 保留等图时间"},
    {L"load_imagelocate_norm_xy_not_pixels", L"default",
        L"getColor imageLocate nx>1.5 不得当像素重解析"},
    {L"parse_varcompute_code", L"default",
        L"varCompute computeCode roundtrip"},
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
    {L"find_json_by_filename_nested", L"default",
        L"FindScriptJsonByFileName finds .json under a subfolder"},
    {L"resolve_library_stale_root_path", L"default",
        L"ResolveLibraryScriptPath finds a file after it was moved into a subfolder"},
    {L"resolve_library_outside_rejected", L"default",
        L"ResolveLibraryScriptPath rejects files outside scripts/recordings"},
    {L"retarget_nested_runmacro_path", L"default",
        L"RetargetNestedLibraryScriptPaths rewrites runMacro targetPath after a move"},
    {L"resolve_library_chinese_folder", L"default",
        L"ResolveLibraryScriptPath finds scripts under a Chinese subfolder (stale root + real path)"},
    {L"save_omits_visual_layout", L"default",
        L"SaveScriptFileData does not write visualLayout into the script file"},
    {L"load_legacy_visual_layout_ok", L"default",
        L"legacy script with visualLayout still loads actions and parses the field"},
    {L"load_missing_visual_layout_ok", L"default",
        L"legacy script without visualLayout still loads actions"},
    {L"visual_layout_cache_roundtrip", L"default",
        L"visual layout cache saves under AppDir\\cache\\visual and deletes with the script key"},
    {L"parse_arrow_keytext_vk", L"default",
        L"keyText ← / keyVk 8592 normalize to VK_LEFT"},
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
    Emit(L"parse_action_unknown_is_custom",
        a.type == ActionType::CustomText
            && a.remark.find(L"[未知动作]") != std::wstring::npos, L"");
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

void CaseNestedUseModeInheritDefault() {
    ScriptAction missing = ParseScriptActionBlock(
        L"{\"type\":\"runMacro\",\"targetPath\":\"a.json\"}", 0, false);
    ScriptAction pb = ParseScriptActionBlock(
        L"{\"type\":\"mousePlayback\",\"targetPath\":\"r.json\"}", 0, false);
    const bool ok = missing.type == ActionType::RunMacro
        && missing.useMode == kNestedUseModeInherit
        && pb.type == ActionType::MousePlayback
        && pb.useMode == kNestedUseModeInherit
        && std::fabs(missing.breakoutTimeSeconds) < 1e-12;
    Emit(L"nested_use_mode_inherit_default", ok, L"");
}

void CaseNestedUseModeWindowRoundtrip() {
    ScriptAction a{};
    a.type = ActionType::RunMacro;
    a.targetPath = L"child.json";
    a.useMode = kNestedUseModeWindow;
    a.breakoutTimeSeconds = 1.5;
    a.nestedWindowMode.enabled = true;
    a.nestedWindowMode.executionKind = windowmode::WindowModeExecutionKind::HiddenDesktop;
    a.nestedWindowMode.selectMethod = windowmode::WindowSelectMethod::UseEditorWindowClass;
    a.nestedWindowMode.targetExePath = L"C:\\\\game\\\\game.exe";
    a.nestedWindowMode.windowClassName = L"GameClass";
    a.nestedWindowMode.windowName = L"Game";
    a.nestedWindowMode.fakeFocusEnabled = true;
    const std::wstring json = ScriptActionToJsonString(a);
    const ScriptAction loaded = ParseScriptActionBlock(json, 0, false);
    ScriptAction inheritKeep{};
    inheritKeep.type = ActionType::MousePlayback;
    inheritKeep.targetPath = L"rec.json";
    inheritKeep.useMode = kNestedUseModeInherit;
    inheritKeep.nestedWindowMode.selectMethod =
        windowmode::WindowSelectMethod::UseEditorWindowClass;
    inheritKeep.nestedWindowMode.windowClassName = L"KeepClass";
    inheritKeep.nestedWindowMode.targetExePath = L"keep.exe";
    const std::wstring inheritJson = ScriptActionToJsonString(inheritKeep);
    const ScriptAction inheritLoaded = ParseScriptActionBlock(inheritJson, 0, false);
    ScriptAction fromStr = ParseScriptActionBlock(
        L"{\"type\":\"mousePlayback\",\"targetPath\":\"x.json\",\"useMode\":\"backgroundWindow\","
        L"\"breakoutTimeSeconds\":2,\"nestedWindowMode\":{\"enabled\":1,"
        L"\"executionKind\":\"backgroundWindow\",\"selectMethod\":\"noSelect\","
        L"\"targetExePath\":\"app.exe\"}}", 0, false);
    ScriptAction enabledZero = ParseScriptActionBlock(
        L"{\"type\":\"runMacro\",\"targetPath\":\"a.json\",\"useMode\":1,"
        L"\"nestedWindowMode\":{\"enabled\":0,\"windowClassName\":\"KeepMe\","
        L"\"windowTitle\":\"T1\"}}", 0, false);
    const bool keepDisabledIdentity = enabledZero.useMode == kNestedUseModeWindow
        && enabledZero.nestedWindowMode.windowClassName == L"KeepMe"
        && enabledZero.nestedWindowMode.windowName == L"T1";
    std::wstring why;
    if (loaded.type != ActionType::RunMacro) why = L"type";
    else if (loaded.useMode != kNestedUseModeWindow) why = L"loaded.useMode";
    else if (!loaded.nestedWindowMode.enabled) why = L"loaded.enabled";
    else if (loaded.nestedWindowMode.selectMethod
        != windowmode::WindowSelectMethod::UseEditorWindowClass) why = L"loaded.selectMethod";
    else if (loaded.nestedWindowMode.windowClassName != L"GameClass")
        why = L"loaded.class=" + loaded.nestedWindowMode.windowClassName;
    else if (loaded.nestedWindowMode.targetExePath.find(L"game.exe") == std::wstring::npos)
        why = L"loaded.exe=" + loaded.nestedWindowMode.targetExePath;
    else if (!loaded.nestedWindowMode.fakeFocusEnabled) why = L"loaded.fakeFocus";
    else if (json.find(L"\"useMode\"") == std::wstring::npos) why = L"json.useMode";
    else if (json.find(L"nestedWindowMode") == std::wstring::npos) why = L"json.nwm";
    else if (inheritLoaded.useMode != kNestedUseModeInherit) why = L"inherit.useMode";
    else if (inheritLoaded.nestedWindowMode.windowClassName != L"KeepClass")
        why = L"inherit.class=" + inheritLoaded.nestedWindowMode.windowClassName;
    else if (fromStr.useMode != kNestedUseModeBackground)
        why = L"fromStr.useMode=" + std::to_wstring(fromStr.useMode);
    else if (fromStr.nestedWindowMode.selectMethod != windowmode::WindowSelectMethod::NoSelect)
        why = L"fromStr.selectMethod";
    else if (fromStr.nestedWindowMode.targetExePath != L"app.exe")
        why = L"fromStr.exe=" + fromStr.nestedWindowMode.targetExePath;
    else if (std::fabs(fromStr.breakoutTimeSeconds - 2.0) >= 1e-9)
        why = L"fromStr.breakout";
    else if (!keepDisabledIdentity) {
        why = L"keepDisabled mode=" + std::to_wstring(enabledZero.useMode)
            + L" class=" + enabledZero.nestedWindowMode.windowClassName
            + L" name=" + enabledZero.nestedWindowMode.windowName;
    }
    const bool ok = why.empty();
    Emit(L"nested_use_mode_window_roundtrip", ok, why.c_str());
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

    const ScriptAction timed = ParseScriptActionBlock(
        L"{\"type\":\"findImage\",\"findImageFollowUp\":3,\"imagePath\":\"images\\\\a.bmp\","
        L"\"findTimeExpr\":\"4\",\"matchVarName\":\"image\"}",
        1, false);
    const bool timedOk = timed.findImageFollowUp == 3
        && timed.findTimeExpr == L"4";
    const ScriptAction noTpl = ParseScriptActionBlock(
        L"{\"type\":\"findImage\",\"findImageFollowUp\":3,\"findTimeExpr\":\"7\","
        L"\"matchVarName\":\"image\"}",
        1, false);
    const bool noTplOk = noTpl.findImageFollowUp == 3
        && noTpl.imagePath.empty()
        && noTpl.findTimeExpr == L"0";
    Emit(L"parse_findimage_save_image", ok && wrote && timedOk && noTplOk,
        (ok && wrote && timedOk && noTplOk) ? L"" : (L"json=" + written).c_str());
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

void CaseParseMultiMatchRoundtrip() {
    const ScriptAction a = ParseScriptActionBlock(
        L"{\"type\":\"multiMatch\",\"imagePaths\":[\"images\\\\a.png\",\"images\\\\b.png\"],"
        L"\"multiMatchMode\":1,\"multiMatchMax\":8,\"multiMatchSort\":1,"
        L"\"findImageFollowUp\":2,\"findTimeExpr\":\"3\",\"duration\":0.05,"
        L"\"matchVarName\":\"matchRet\"}",
        1, false);
    const bool parsed = a.type == ActionType::MultiMatch
        && a.imagePaths.size() == 2
        && a.imagePaths[0].find(L"a.png") != std::wstring::npos
        && a.imagePaths[1].find(L"b.png") != std::wstring::npos
        && a.multiMatchMode == 1
        && a.multiMatchMax == 8
        && a.multiMatchSort == 1
        && a.findImageFollowUp == 2
        && a.findTimeExpr == L"3"
        && std::abs(a.duration - 0.05) < 1e-9;
    const std::wstring written = ScriptActionToJsonString(a);
    const bool wrote = written.find(L"\"type\": \"multiMatch\"") != std::wstring::npos
        && written.find(L"\"imagePaths\"") != std::wstring::npos
        && written.find(L"a.png") != std::wstring::npos
        && written.find(L"b.png") != std::wstring::npos
        && written.find(L"\"multiMatchMode\": 1") != std::wstring::npos
        && written.find(L"\"findTimeExpr\": \"3\"") != std::wstring::npos;
    Emit(L"parse_multimatch_roundtrip", parsed && wrote,
        (parsed && wrote) ? L"" : (L"json=" + written).c_str());
}

void CaseParseMultiMatchTemplateTPath() {
    const ScriptAction a = ParseScriptActionBlock(
        L"{\"type\":\"multiMatch\",\"imagePaths\":[\"images\\\\template_1.bmp\"],"
        L"\"multiMatchMode\":1}",
        1, false);
    const bool hasName = !a.imagePaths.empty()
        && a.imagePaths[0].find(L"template_1.bmp") != std::wstring::npos;
    const bool notEaten = a.imagePaths.empty()
        || a.imagePaths[0].find(L"imagestemplate_") == std::wstring::npos;
    Emit(L"parse_multimatch_template_t_path", hasName && notEaten,
        (hasName && notEaten) ? L"" : (a.imagePaths.empty() ? L"empty" : a.imagePaths[0].c_str()));
}

void CaseParseMultiMatchImageUseVars() {
    const ScriptAction a = ParseScriptActionBlock(
        L"{\"type\":\"multiMatch\",\"imagePaths\":[\"fooVar\",\"images\\\\b.png\"],"
        L"\"imageUseVars\":[1,0],\"imageUseVar\":1}",
        1, false);
    const bool ok = a.type == ActionType::MultiMatch
        && a.imagePaths.size() == 2
        && a.imagePaths[0] == L"fooVar"
        && a.imageUseVar
        && a.imageUseVars.size() == 2
        && a.imageUseVars[0] != 0
        && a.imageUseVars[1] == 0;
    const std::wstring written = ScriptActionToJsonString(a);
    const bool wrote = written.find(L"\"imageUseVars\"") != std::wstring::npos
        && written.find(L"fooVar") != std::wstring::npos;
    Emit(L"parse_multimatch_image_use_vars", ok && wrote,
        (ok && wrote) ? L"" : (L"json=" + written).c_str());
}

void CaseParseMultiMatchHoleUseVars() {
    const ScriptAction a = ParseScriptActionBlock(
        L"{\"type\":\"multiMatch\",\"imagePaths\":[\"aaVar\",\"\",\"images\\\\c.png\"],"
        L"\"imageUseVars\":[1,0,0],\"imageUseVar\":1}",
        1, false);
    const bool ok = a.type == ActionType::MultiMatch
        && a.imagePaths.size() == 2
        && a.imagePaths[0] == L"aaVar"
        && a.imagePaths[1].find(L"c.png") != std::wstring::npos
        && a.imageUseVars.size() == 2
        && a.imageUseVars[0] != 0
        && a.imageUseVars[1] == 0;
    Emit(L"parse_multimatch_hole_use_vars", ok,
        ok ? L"" : (L"n=" + std::to_wstring(static_cast<int>(a.imagePaths.size()))).c_str());
}

void CaseResolveImagestemplateTypo() {
    EnsureFindImagesDir();
    const std::wstring real = FindImagesDir() + L"\\template_selftest_recover.bmp";
    WriteUtf8File(real, "x");
    const std::wstring resolved = ResolveImagePath(L"images/imagestemplate_selftest_recover.bmp");
    const bool ok = resolved.find(L"template_selftest_recover.bmp") != std::wstring::npos
        && resolved.find(L"imagestemplate_") == std::wstring::npos
        && GetFileAttributesW(resolved.c_str()) != INVALID_FILE_ATTRIBUTES;
    DeleteFileW(real.c_str());
    Emit(L"resolve_imagestemplate_typo", ok, resolved.c_str());
}

void CaseParseWatchImageResume() {
    const ScriptAction a = ParseScriptActionBlock(
        L"{\"type\":\"watchImage\",\"imagePath\":\"images\\\\d.bmp\","
        L"\"matchThreshold\":80,\"resumeAfterWatch\":0,\"searchFullScreen\":1}",
        1, false);
    const bool ok = a.type == ActionType::WatchImage
        && a.matchThreshold == 80.0
        && !a.resumeAfterWatch
        && a.imagePath.find(L"d.bmp") != std::wstring::npos;
    const std::wstring written = ScriptActionToJsonString(a);
    const bool wrote = written.find(L"\"type\": \"watchImage\"") != std::wstring::npos
        && written.find(L"\"resumeAfterWatch\": 0") != std::wstring::npos;
    Emit(L"parse_watchimage_resume", ok && wrote,
        (ok && wrote) ? L"" : (L"json=" + written).c_str());
}

void CaseParseWatchImageTimeMode() {
    const ScriptAction a = ParseScriptActionBlock(
        L"{\"type\":\"watchImage\",\"imagePath\":\"images\\\\e.bmp\","
        L"\"watchMode\":1,\"watchPollSeconds\":0.5,\"resumeAfterWatch\":1}",
        1, false);
    const bool ok = a.type == ActionType::WatchImage
        && a.watchMode == 1
        && std::abs(a.watchPollSeconds - 0.5) < 1e-9;
    const std::wstring written = ScriptActionToJsonString(a);
    const bool wrote = written.find(L"\"watchMode\": 1") != std::wstring::npos
        && written.find(L"\"watchPollSeconds\":") != std::wstring::npos;
    const ScriptAction alias = ParseScriptActionBlock(
        L"{\"type\":\"watchImage\",\"imagePath\":\"images\\\\e.bmp\","
        L"\"watchMode\":\"time\",\"watchPollSeconds\":2.5}",
        1, false);
    const bool aliasOk = alias.watchMode == 1
        && std::abs(alias.watchPollSeconds - 2.5) < 1e-9;
    const ScriptAction intervalOnly = ParseScriptActionBlock(
        L"{\"type\":\"watchImage\",\"imagePath\":\"images\\\\e.bmp\","
        L"\"watchMode\":\"time\",\"watchPollInterval\":3}",
        1, false);
    const bool intervalOk = intervalOnly.watchMode == 1
        && std::abs(intervalOnly.watchPollSeconds - 3.0) < 1e-9;
    Emit(L"parse_watchimage_time_mode", ok && wrote && aliasOk && intervalOk,
        (ok && wrote && aliasOk && intervalOk) ? L"" : (L"json=" + written).c_str());
}

void CaseParseMouseDragAbs() {
    const ScriptAction a = ParseScriptActionBlock(
        L"{\"type\":\"mouseDrag\",\"button\":\"left\",\"x\":10,\"y\":20,"
        L"\"endX\":30,\"endY\":40,\"duration\":0.3,\"imageLocate\":0}",
        1, false);
    const bool ok = a.type == ActionType::MouseDrag
        && a.x == 10 && a.y == 20 && a.endX == 30 && a.endY == 40
        && std::abs(a.duration - 0.3) < 1e-9
        && !a.imageLocate;
    const std::wstring written = ScriptActionToJsonString(a);
    const bool wrote = written.find(L"\"type\": \"mouseDrag\"") != std::wstring::npos
        && written.find(L"\"endX\":") != std::wstring::npos
        && written.find(L"\"imageLocate\": 0") != std::wstring::npos;
    Emit(L"parse_mousedrag_abs", ok && wrote,
        (ok && wrote) ? L"" : (L"json=" + written).c_str());
}

void CaseParseMouseDragImageLocate() {
    const ScriptAction a = ParseScriptActionBlock(
        L"{\"type\":\"mouseDrag\",\"imageLocate\":1,\"imagePath\":\"images\\\\d.bmp\","
        L"\"x\":-5,\"y\":6,\"endX\":7,\"endY\":-8,\"duration\":0.25,"
        L"\"searchFullScreen\":1,\"matchThreshold\":70}",
        1, false);
    const bool ok = a.type == ActionType::MouseDrag
        && a.imageLocate
        && a.imagePath.find(L"d.bmp") != std::wstring::npos
        && a.x == -5 && a.y == 6 && a.endX == 7 && a.endY == -8
        && std::abs(a.duration - 0.25) < 1e-9;
    const std::wstring written = ScriptActionToJsonString(a);
    const bool wrote = written.find(L"\"imageLocate\": 1") != std::wstring::npos
        && written.find(L"d.bmp") != std::wstring::npos;
    Emit(L"parse_mousedrag_image_locate", ok && wrote,
        (ok && wrote) ? L"" : (L"json=" + written).c_str());
}

bool WriteStubBmp(const std::wstring& path, int w, int h) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteBitmapHandle(bmp);
        return false;
    }
    memset(bits, 0x40, static_cast<size_t>(w) * h * 4);
    const bool ok = SaveBitmapToFile(bmp, path);
    DeleteBitmapHandle(bmp);
    return ok;
}

void CaseParseColorActionsImageLocate() {
    const ScriptAction gc = ParseScriptActionBlock(
        L"{\"type\":\"getColor\",\"imageLocate\":1,\"imagePath\":\"images\\\\c.bmp\","
        L"\"x\":-3,\"y\":4,\"matchVarName\":\"colorRet\",\"searchFullScreen\":1}",
        1, false);
    const bool gcOk = gc.type == ActionType::GetColor && gc.imageLocate
        && gc.imagePath.find(L"c.bmp") != std::wstring::npos
        && gc.x == -3 && gc.y == 4;
    const std::wstring gcWritten = ScriptActionToJsonString(gc);
    const bool gcWrote = gcWritten.find(L"\"imageLocate\": 1") != std::wstring::npos
        && gcWritten.find(L"c.bmp") != std::wstring::npos;

    const ScriptAction cm = ParseScriptActionBlock(
        L"{\"type\":\"colorMatch\",\"imageLocate\":true,\"imagePath\":\"images\\\\m.bmp\","
        L"\"x\":1,\"y\":2,\"colorR\":255,\"colorG\":0,\"colorB\":0,\"colorTolerance\":16}",
        1, false);
    const bool cmOk = cm.type == ActionType::ColorMatch && cm.imageLocate
        && cm.imagePath.find(L"m.bmp") != std::wstring::npos;

    const ScriptAction fc = ParseScriptActionBlock(
        L"{\"type\":\"findColor\",\"imageLocate\":1,\"imagePath\":\"images\\\\f.bmp\","
        L"\"colorR\":255,\"colorG\":0,\"colorB\":0,\"findImageFollowUp\":2,"
        L"\"searchFullScreen\":1}",
        1, false);
    const bool fcOk = fc.type == ActionType::FindColor && fc.imageLocate
        && fc.imagePath.find(L"f.bmp") != std::wstring::npos
        && fc.findImageFollowUp == 2;
    const bool ok = gcOk && gcWrote && cmOk && fcOk;
    Emit(L"parse_color_actions_image_locate", ok,
        ok ? L"" : (L"gc=" + gcWritten).c_str());
}

void CaseParseFindColorKeepsFindTime() {
    const ScriptAction keep = ParseScriptActionBlock(
        L"{\"type\":\"findColor\",\"imageLocate\":1,\"imagePath\":\"images\\\\f.bmp\","
        L"\"findImageFollowUp\":2,\"findTimeExpr\":\"5\"}",
        1, false);
    const ScriptAction clamp = ParseScriptActionBlock(
        L"{\"type\":\"findColor\",\"findImageFollowUp\":3,\"findTimeExpr\":\"2\"}",
        1, false);
    const ScriptAction findImageKeep = ParseScriptActionBlock(
        L"{\"type\":\"findImage\",\"findImageFollowUp\":2,\"findTimeExpr\":\"9\"}",
        1, false);
    const bool ok = keep.findTimeExpr == L"5"
        && clamp.findImageFollowUp == 2
        && clamp.findTimeExpr == L"2"
        && findImageKeep.findTimeExpr == L"9";
    Emit(L"parse_findcolor_keeps_find_time", ok,
        ok ? L"" : (L"keep=" + keep.findTimeExpr + L" clampFu="
            + std::to_wstring(clamp.findImageFollowUp)).c_str());
}

void CaseSaveLoadColorImageLocate() {
    const std::wstring bmpPath = AppDir() + L"\\selftest_color_locate.bmp";
    const std::wstring jsonPath = TempScriptPath(L"color_locate");
    if (!WriteStubBmp(bmpPath, 20, 10)) {
        Emit(L"save_load_color_image_locate", false, L"failed to write stub bmp");
        return;
    }

    ScriptFileData data{};
    data.scriptName = L"color-locate";
    ScriptAction gc{};
    gc.type = ActionType::GetColor;
    gc.imageLocate = true;
    gc.imagePath = bmpPath;
    gc.x = 8;
    gc.y = -2;
    gc.matchVarName = L"colorRet";
    gc.searchFullScreen = true;
    gc.findTimeExpr = L"3";
    gc.originalNo = 1;
    ScriptAction fc{};
    fc.type = ActionType::FindColor;
    fc.imageLocate = true;
    fc.imagePath = bmpPath;
    fc.findImageFollowUp = 2;
    fc.findTimeExpr = L"5";
    fc.colorR = 255;
    fc.searchFullScreen = true;
    fc.matchVarName = L"colorRet";
    fc.originalNo = 2;
    data.actions.push_back(gc);
    data.actions.push_back(fc);

    const bool saved = SaveScriptFileData(jsonPath, data);
    ScriptFileData loadedN = LoadScriptFileData(jsonPath, false);
    ScriptFileData loadedP = LoadScriptFileData(jsonPath, true);
    const std::wstring written = ReadAll(jsonPath);
    const auto collected = CollectImagePathsFromJson(written);

    const bool gcNorm = saved && loadedN.actions.size() >= 2
        && loadedN.actions[0].type == ActionType::GetColor
        && loadedN.actions[0].imageLocate
        && std::fabs(loadedN.actions[0].nx - 8.0 / 20.0) < 1e-6
        && std::fabs(loadedN.actions[0].ny - (-2.0 / 10.0)) < 1e-6
        && loadedN.actions[0].findTimeExpr == L"3";
    const bool gcPix = loadedP.actions.size() >= 1
        && loadedP.actions[0].x == 8
        && loadedP.actions[0].y == -2;
    const bool fcOk = loadedN.actions.size() >= 2
        && loadedN.actions[1].type == ActionType::FindColor
        && loadedN.actions[1].imageLocate
        && loadedN.actions[1].findImageFollowUp == 2
        && loadedN.actions[1].findTimeExpr == L"5";
    bool collectedOk = false;
    for (const auto& p : collected) {
        if (p.find(L"selftest_color_locate") != std::wstring::npos) {
            collectedOk = true;
            break;
        }
    }

    if (loadedN.actions.size() >= 1 && !loadedN.actions[0].imagePath.empty()) {
        DeleteFileW(loadedN.actions[0].imagePath.c_str());
    }
    DeleteFileW(bmpPath.c_str());
    DeleteFileW(jsonPath.c_str());

    const bool ok = gcNorm && gcPix && fcOk && collectedOk;
    Emit(L"save_load_color_image_locate", ok,
        ok ? L"" : (L"norm=" + std::to_wstring(gcNorm)
            + L" pix=" + std::to_wstring(gcPix)
            + L" fc=" + std::to_wstring(fcOk)
            + L" zip=" + std::to_wstring(collectedOk)).c_str());
}

void CaseLoadImageLocateNormXyNotPixels() {
    const std::wstring path = TempScriptPath(L"locate_nx");
    WriteUtf8File(path,
        u8"{\"scriptName\":\"t\",\"coordMeta\":{\"version\":1,\"space\":\"screenVirtual\","
        u8"\"refOriginX\":0,\"refOriginY\":0,\"refWidth\":2560,\"refHeight\":1440,"
        u8"\"refDpi\":96,\"captureWidth\":2560,\"captureHeight\":1440},"
        u8"\"actions\":[{\"type\":\"getColor\",\"imageLocate\":1,\"x\":2.5,\"y\":0.1}]}");
    ScriptFileData loaded = LoadScriptFileData(path, false);
    DeleteFileW(path.c_str());
    const bool ok = !loaded.actions.empty()
        && loaded.actions[0].imageLocate
        && std::fabs(loaded.actions[0].nx - 2.5) < 1e-9
        && std::fabs(loaded.actions[0].ny - 0.1) < 1e-9;
    Emit(L"load_imagelocate_norm_xy_not_pixels", ok,
        ok ? L"" : L"nx treated as leftover pixels");
}

void CaseParseVarComputeCode() {
    const ScriptAction a = ParseScriptActionBlock(
        L"{\"type\":\"varCompute\",\"computeCode\":\"combo = 1;\\nreturn combo;\"}",
        1, false);
    const bool ok = a.type == ActionType::VarCompute
        && a.computeCode.find(L"combo = 1") != std::wstring::npos
        && a.computeCode.find(L"return combo") != std::wstring::npos;
    const std::wstring written = ScriptActionToJsonString(a);
    const bool wrote = written.find(L"\"type\": \"varCompute\"") != std::wstring::npos
        && written.find(L"computeCode") != std::wstring::npos;
    Emit(L"parse_varcompute_code", ok && wrote,
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

void RemoveDirIfEmpty(const std::wstring& dir) {
    RemoveDirectoryW(dir.c_str());
}

void CaseFindJsonByFileNameNested() {
    const std::wstring root = AppDir() + L"\\__qst_iotest_findtree";
    const std::wstring sub = root + L"\\nested";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(sub.c_str(), nullptr);
    const std::wstring file = sub + L"\\__qst_find_only.json";
    WriteUtf8File(file, "{}");
    std::wstring found;
    const bool ok = FindScriptJsonByFileName(root, L"__qst_find_only", found)
        && LibraryPathsEqual(found, file);
    DeleteFileW(file.c_str());
    RemoveDirIfEmpty(sub);
    RemoveDirIfEmpty(root);
    Emit(L"find_json_by_filename_nested", ok,
        ok ? L"" : L"recursive filename search missed nested json");
}

void CaseResolveLibraryStaleRootPath() {
    EnsureScriptsDir();
    const std::wstring sub = ScriptsDir() + L"\\__qst_iotest_sub";
    CreateDirectoryW(sub.c_str(), nullptr);
    const std::wstring name = L"__qst_iotest_moved.json";
    const std::wstring realPath = sub + L"\\" + name;
    const std::wstring staleRoot = ScriptsDir() + L"\\" + name;
    ScriptFileData data;
    data.scriptName = L"iotest_moved";
    ScriptAction wait{};
    wait.type = ActionType::Wait;
    wait.duration = 0.01;
    data.actions.push_back(wait);
    const bool saved = SaveScriptFileData(realPath, data);

    std::wstring byName;
    std::wstring byStale;
    std::wstring byBare;
    const bool ok = saved
        && FindScriptJsonByFileName(ScriptsDir(), name, byName)
        && LibraryPathsEqual(byName, realPath)
        && ResolveLibraryScriptPath(staleRoot, byStale)
        && LibraryPathsEqual(byStale, realPath)
        && ResolveLibraryScriptPath(name, byBare)
        && LibraryPathsEqual(byBare, realPath);

    DeleteFileW(realPath.c_str());
    RemoveDirIfEmpty(sub);
    Emit(L"resolve_library_stale_root_path", ok,
        ok ? L"" : L"stale scripts\\\\name.json did not resolve to subfolder file");
}

void CaseResolveLibraryOutsideRejected() {
    const std::wstring outside = AppDir() + L"\\ScriptIoSelfTest.exe";
    std::wstring out;
    const bool ok = GetFileAttributesW(outside.c_str()) != INVALID_FILE_ATTRIBUTES
        && !ResolveLibraryScriptPath(outside, out);
    Emit(L"resolve_library_outside_rejected", ok,
        ok ? L"" : L"non-library path must be rejected");
}

void CaseRetargetNestedRunMacro() {
    EnsureScriptsDir();
    const std::wstring sub = ScriptsDir() + L"\\__qst_iotest_sub";
    CreateDirectoryW(sub.c_str(), nullptr);
    const std::wstring name = L"__qst_iotest_moved.json";
    const std::wstring realPath = sub + L"\\" + name;
    const std::wstring staleRoot = ScriptsDir() + L"\\" + name;
    const std::wstring parentPath = ScriptsDir() + L"\\__qst_iotest_parent.json";

    ScriptFileData child;
    child.scriptName = L"iotest_child";
    ScriptAction wait{};
    wait.type = ActionType::Wait;
    wait.duration = 0.01;
    child.actions.push_back(wait);
    SaveScriptFileData(realPath, child);

    ScriptFileData parent;
    parent.scriptName = L"iotest_parent";
    ScriptAction run{};
    run.type = ActionType::RunMacro;
    run.targetPath = staleRoot;
    run.clickCount = 1;
    parent.actions.push_back(run);
    SaveScriptFileData(parentPath, parent);

    const int n = RetargetNestedLibraryScriptPaths(staleRoot, realPath);
    ScriptFileData loaded = LoadScriptFileData(parentPath, false);
    const bool ok = n >= 1
        && !loaded.actions.empty()
        && loaded.actions[0].type == ActionType::RunMacro
        && LibraryPathsEqual(loaded.actions[0].targetPath, realPath);

    DeleteFileW(parentPath.c_str());
    DeleteFileW(realPath.c_str());
    RemoveDirIfEmpty(sub);
    Emit(L"retarget_nested_runmacro_path", ok,
        ok ? L"" : L"runMacro targetPath was not rewritten after move");
}

void CaseResolveLibraryChineseFolder() {
    EnsureScriptsDir();
    const std::wstring sub = ScriptsDir() + L"\\中文夹_qstiotest";
    CreateDirectoryW(sub.c_str(), nullptr);
    const std::wstring name = L"__qst_cn_moved.json";
    const std::wstring realPath = sub + L"\\" + name;
    const std::wstring staleRoot = ScriptsDir() + L"\\" + name;
    ScriptFileData data;
    data.scriptName = L"iotest_cn";
    ScriptAction wait{};
    wait.type = ActionType::Wait;
    wait.duration = 0.01;
    data.actions.push_back(wait);
    const bool saved = SaveScriptFileData(realPath, data);

    std::wstring byStale;
    std::wstring byReal;
    const bool ok = saved
        && GetFileAttributesW(realPath.c_str()) != INVALID_FILE_ATTRIBUTES
        && GetFileAttributesW(staleRoot.c_str()) == INVALID_FILE_ATTRIBUTES
        && ResolveLibraryScriptPath(staleRoot, byStale)
        && LibraryPathsEqual(byStale, realPath)
        && ResolveLibraryScriptPath(realPath, byReal)
        && LibraryPathsEqual(byReal, realPath);

    DeleteFileW(realPath.c_str());
    RemoveDirIfEmpty(sub);
    Emit(L"resolve_library_chinese_folder", ok,
        ok ? L"" : L"Chinese subfolder stale/real path did not resolve");
}

void CaseVisualLayoutOmittedOnSave() {
    const std::wstring path = TempScriptPath(L"io_visual_layout");
    ScriptFileData data{};
    data.scriptName = L"visual-layout";
    data.coordMeta = StandardScriptCoordMeta();
    data.coordsNormalized = true;
    ScriptAction wait{};
    wait.type = ActionType::Wait;
    wait.duration = 0.2;
    data.actions.push_back(wait);
    data.visualLayoutJson =
        L"{\"viewMode\":\"visual\",\"zoom\":1.15,\"scrollX\":10,\"scrollY\":20,"
        L"\"nodes\":[{\"x\":40,\"y\":80}]}";

    const bool saved = SaveScriptFileData(path, data);
    const std::wstring written = ReadAll(path);
    ScriptFileData loaded = LoadScriptFileData(path, false);
    DeleteFileW(path.c_str());

    const bool ok = saved
        && written.find(L"\"visualLayout\"") == std::wstring::npos
        && loaded.actions.size() == 1
        && loaded.actions[0].type == ActionType::Wait
        && loaded.visualLayoutJson.empty();
    Emit(L"save_omits_visual_layout", ok,
        ok ? L"" : L"visualLayout should stay out of script files");
}

void CaseMissingVisualLayout() {
    const std::wstring path = TempScriptPath(L"io_no_visual_layout");
    WriteUtf8File(path,
        u8"{\"scriptName\":\"legacy\",\"hotkeyText\":\"\",\"hotkeyVk\":0,"
        u8"\"hotkeyModifiers\":0,\"hotkeyHold\":0,"
        u8"\"actions\":[{\"type\":\"wait\",\"duration\":0.1}]}");
    ScriptFileData loaded = LoadScriptFileData(path, false);
    DeleteFileW(path.c_str());
    const bool ok = loaded.visualLayoutJson.empty()
        && loaded.actions.size() == 1
        && loaded.actions[0].type == ActionType::Wait;
    Emit(L"load_missing_visual_layout_ok", ok,
        ok ? L"" : L"legacy script should load without visualLayout");
}

void CaseLoadLegacyVisualLayout() {
    const std::wstring path = TempScriptPath(L"io_legacy_visual_layout");
    WriteUtf8File(path,
        u8"{\"scriptName\":\"legacy-vis\",\"hotkeyText\":\"\",\"hotkeyVk\":0,"
        u8"\"hotkeyModifiers\":0,\"hotkeyHold\":0,"
        u8"\"visualLayout\":{\"viewMode\":\"visual\",\"zoom\":1.1,\"nodes\":[{\"x\":3,\"y\":4}]},"
        u8"\"actions\":[{\"type\":\"wait\",\"duration\":0.1}]}");
    ScriptFileData loaded = LoadScriptFileData(path, false);
    DeleteFileW(path.c_str());
    const bool ok = loaded.actions.size() == 1
        && loaded.actions[0].type == ActionType::Wait
        && loaded.visualLayoutJson.find(L"\"zoom\":1.1") != std::wstring::npos
        && loaded.visualLayoutJson.find(L"\"x\":3") != std::wstring::npos;
    Emit(L"load_legacy_visual_layout_ok", ok,
        ok ? L"" : L"legacy visualLayout should still parse for cache migration");
}

void CaseVisualLayoutCacheRoundtrip() {
    const std::wstring script = TempScriptPath(L"io_visual_cache");
    const std::wstring json =
        L"{\"viewMode\":\"visual\",\"zoom\":1.25,\"nodes\":[{\"x\":11,\"y\":22}]}";
    DeleteVisualLayoutCache(script);
    const bool saved = SaveVisualLayoutCache(script, json);
    std::wstring loaded;
    const bool okLoad = LoadVisualLayoutCache(script, loaded);
    const std::wstring cachePath = VisualLayoutCachePathForScript(script);
    DeleteVisualLayoutCache(script);
    std::wstring after;
    const bool gone = !LoadVisualLayoutCache(script, after);
    const bool ok = saved && okLoad
        && loaded.find(L"\"zoom\":1.25") != std::wstring::npos
        && cachePath.find(L"\\cache\\visual\\") != std::wstring::npos
        && gone;
    Emit(L"visual_layout_cache_roundtrip", ok,
        ok ? L"" : L"cache roundtrip/delete failed");
}

void CaseParseArrowKeyTextVk() {
    const ScriptAction missing = ParseScriptActionBlock(
        L"{\"type\":\"keyDown\",\"keyText\":\"\x2190\"}", 0, false);
    const ScriptAction unicodeVk = ParseScriptActionBlock(
        L"{\"type\":\"keyDown\",\"keyText\":\"\x2190\",\"keyVk\":8592}", 0, false);
    const ScriptAction okVk = ParseScriptActionBlock(
        L"{\"type\":\"keyDown\",\"keyText\":\"\x2190\",\"keyVk\":37}", 0, false);
    const bool ok = missing.type == ActionType::KeyDown && missing.keyVk == VK_LEFT
        && unicodeVk.keyVk == VK_LEFT && okVk.keyVk == VK_LEFT;
    wchar_t detail[120]{};
    swprintf_s(detail, L"missing=%u unicodeVk=%u okVk=%u",
        missing.keyVk, unicodeVk.keyVk, okVk.keyVk);
    Emit(L"parse_arrow_keytext_vk", ok, ok ? L"" : detail);
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
    CaseNestedUseModeInheritDefault();
    CaseNestedUseModeWindowRoundtrip();
    CaseWriteNormXyKeepsPixel();
    CaseInstantDurationDefaultZero();
    CaseRecordedCaptureRoundtrip();
    CaseCollectRecordedCapture();
    CaseParseFindImageSaveImage();
    CaseParseFindImagePerfectMatch();
    CaseParseMultiMatchRoundtrip();
    CaseParseMultiMatchTemplateTPath();
    CaseParseMultiMatchImageUseVars();
    CaseParseMultiMatchHoleUseVars();
    CaseResolveImagestemplateTypo();
    CaseParseWatchImageResume();
    CaseParseWatchImageTimeMode();
    CaseParseMouseDragAbs();
    CaseParseMouseDragImageLocate();
    CaseParseColorActionsImageLocate();
    CaseParseFindColorKeepsFindTime();
    CaseSaveLoadColorImageLocate();
    CaseLoadImageLocateNormXyNotPixels();
    CaseParseVarComputeCode();
    CaseParseQuickInputEscapes();
    CaseLooksLikeFilePath();
    CaseWindowRelativePixelXy();
    CaseRecordingKeepsWindowRelativeMode();
    CaseRecordingRecoversWmFromRelActions();
    CaseScriptDefaultModeNotRevived();
    CaseRecordingWipesEditorWindowMode();
    CaseSaveAfterLoadFalseKeepsXy();
    CaseFindJsonByFileNameNested();
    CaseResolveLibraryStaleRootPath();
    CaseResolveLibraryOutsideRejected();
    CaseRetargetNestedRunMacro();
    CaseResolveLibraryChineseFolder();
    CaseVisualLayoutOmittedOnSave();
    CaseMissingVisualLayout();
    CaseLoadLegacyVisualLayout();
    CaseVisualLayoutCacheRoundtrip();
    CaseParseArrowKeyTextVk();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
