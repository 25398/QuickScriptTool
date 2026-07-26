// =============================================================================
// ScriptIoSelfTest — 脚本 JSON 读写 / 录制路径自检
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:ScriptIoSelfTest
//   build\Release\ScriptIoSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "script_io.h"
#include "utils.h"

#include <cmath>
#include <fstream>
#include <string>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"recording_path_under_recordings", L"default",
        L"IsRecordingScriptPath true under AppDir\\recordings"},
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
    {L"instant_duration_default_zero", L"default",
        L"moveMouse missing duration parses as 0"},
    {L"recorded_capture_path_roundtrip", L"default",
        L"recordedCapturePath + captureOffset roundtrip"},
    {L"collect_image_paths_recorded_capture", L"default",
        L"CollectImagePathsFromJson includes recordedCapturePath"},
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
    CaseInstantDurationDefaultZero();
    CaseRecordedCaptureRoundtrip();
    CaseCollectRecordedCapture();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
