// =============================================================================
// CoordSpaceSelfTest — 坐标归一化 / coordMeta 自检
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:CoordSpaceSelfTest
//   build\Release\CoordSpaceSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "coord_space.h"
#include "script_types.h"

#include <cmath>
#include <string>
#include <vector>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"standard_meta_2560x1440", L"default",
        L"StandardScriptCoordMeta is 2560x1440 origin0 dpi96"},
    {L"save_meta_captures_screen", L"default",
        L"BuildScriptCoordMetaForSave keeps ref 2560x1440 and sets capture"},
    {L"exec_meta_uses_capture", L"default",
        L"ScriptCoordMetaForExecution inherits captureW/H"},
    {L"exec_meta_legacy_ref_as_capture", L"default",
        L"Legacy ref 1920x1080 becomes capture when capture unset"},
    {L"coordmeta_json_roundtrip", L"default",
        L"WriteCoordMetaJson / ParseCoordMetaJson preserves fields"},
    {L"has_coordmeta_detect", L"default",
        L"HasCoordMetaJson true when coordMeta block present"},
    {L"normalize_move_to_nstar", L"default",
        L"NormalizeActionCoords maps pixel move to nx/ny"},
    {L"normalize_relative_skip", L"default",
        L"MoveMouseRelative is not screen-normalized"},
    {L"migrate_legacy_half", L"default",
        L"MigrateLegacyScriptToNormalized 1280,720 @2560x1440 -> 0.5,0.5"},
    {L"template_scale_half", L"default",
        L"ComputeTemplateScale capture2560x1440 current1280x720 -> 0.5"},
    {L"exec_find_opts_same_res", L"default",
        L"BuildExecutionFindImageOptions same-res disables pyramid"},
    {L"exec_find_opts_cross_iso", L"default",
        L"Isotropic cross-res uses narrow scale band"},
    {L"resolve_click_point_offset", L"default",
        L"ResolveFindImageClickPoint applies nOffset on match box"},
};

bool Near(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

void CaseStandardMeta() {
    const CoordMeta m = StandardScriptCoordMeta();
    const bool ok = m.refWidth == 2560 && m.refHeight == 1440
        && m.refOriginX == 0 && m.refOriginY == 0 && m.refDpi == 96;
    Emit(L"standard_meta_2560x1440", ok, ok ? L"" : L"standard meta mismatch");
}

void CaseSaveMetaCaptures() {
    CoordMeta pixel{};
    pixel.refWidth = 1920;
    pixel.refHeight = 1080;
    pixel.captureWidth = 1920;
    pixel.captureHeight = 1080;
    const CoordMeta save = BuildScriptCoordMetaForSave(pixel);
    const bool ok = save.refWidth == 2560 && save.refHeight == 1440
        && save.captureWidth == 1920 && save.captureHeight == 1080;
    Emit(L"save_meta_captures_screen", ok, ok ? L"" : L"save meta capture/ref wrong");
}

void CaseExecMetaCapture() {
    CoordMeta fromFile = StandardScriptCoordMeta();
    fromFile.captureWidth = 1920;
    fromFile.captureHeight = 1080;
    const CoordMeta exec = ScriptCoordMetaForExecution(fromFile);
    const bool ok = exec.refWidth == 2560 && exec.refHeight == 1440
        && exec.captureWidth == 1920 && exec.captureHeight == 1080;
    Emit(L"exec_meta_uses_capture", ok, ok ? L"" : L"execution meta capture lost");
}

void CaseExecMetaLegacy() {
    CoordMeta legacy{};
    legacy.refWidth = 1920;
    legacy.refHeight = 1080;
    legacy.captureWidth = 0;
    legacy.captureHeight = 0;
    const CoordMeta exec = ScriptCoordMetaForExecution(legacy);
    const bool ok = exec.captureWidth == 1920 && exec.captureHeight == 1080;
    Emit(L"exec_meta_legacy_ref_as_capture", ok,
        ok ? L"" : L"legacy ref should seed capture");
}

void CaseCoordMetaRoundtrip() {
    CoordMeta m = StandardScriptCoordMeta();
    m.captureWidth = 1366;
    m.captureHeight = 768;
    m.space = CoordMeta::Space::WindowClient;
    std::wstring json;
    WriteCoordMetaJson(json, m, false);
    const CoordMeta parsed = ParseCoordMetaJson(L"{" + json + L"}");
    const bool ok = parsed.refWidth == 2560 && parsed.refHeight == 1440
        && parsed.captureWidth == 1366 && parsed.captureHeight == 768
        && parsed.space == CoordMeta::Space::WindowClient;
    Emit(L"coordmeta_json_roundtrip", ok, ok ? L"" : json.c_str());
}

void CaseHasCoordMeta() {
    const bool ok = HasCoordMetaJson(L"{\"name\":\"a\",\"coordMeta\":{\"refWidth\":2560}}")
        && !HasCoordMetaJson(L"{\"name\":\"a\",\"actions\":[]}");
    Emit(L"has_coordmeta_detect", ok, ok ? L"" : L"HasCoordMetaJson detect broken");
}

void CaseNormalizeMove() {
    CoordMeta meta = StandardScriptCoordMeta();
    ScriptAction a{};
    a.type = ActionType::MoveMouse;
    a.x = 1280;
    a.y = 720;
    NormalizeActionCoords(a, meta);
    const bool ok = Near(a.nx, 0.5) && Near(a.ny, 0.5);
    Emit(L"normalize_move_to_nstar", ok,
        ok ? L"" : (L"nx=" + std::to_wstring(a.nx) + L" ny=" + std::to_wstring(a.ny)).c_str());
}

void CaseNormalizeRelativeSkip() {
    CoordMeta meta = StandardScriptCoordMeta();
    ScriptAction a{};
    a.type = ActionType::MoveMouseRelative;
    a.x = -40;
    a.y = 25;
    NormalizeActionCoords(a, meta);
    const bool ok = !a.coordsAreNormalized && a.x == -40 && a.y == 25;
    Emit(L"normalize_relative_skip", ok, ok ? L"" : L"relative should keep dx/dy");
}

void CaseMigrateLegacy() {
    CoordMeta meta = StandardScriptCoordMeta();
    std::vector<ScriptAction> actions(1);
    actions[0].type = ActionType::MoveMouse;
    actions[0].x = 1280;
    actions[0].y = 720;
    MigrateLegacyScriptToNormalized(actions, meta);
    const bool ok = Near(actions[0].nx, 0.5) && Near(actions[0].ny, 0.5);
    Emit(L"migrate_legacy_half", ok, ok ? L"" : L"legacy migrate nx/ny wrong");
}

void CaseTemplateScale() {
    CoordMeta meta = StandardScriptCoordMeta();
    meta.captureWidth = 2560;
    meta.captureHeight = 1440;
    const TemplateScale ts = ComputeTemplateScale(meta, 1280, 720);
    const bool ok = Near(ts.sx, 0.5) && Near(ts.sy, 0.5);
    Emit(L"template_scale_half", ok,
        ok ? L"" : (L"sx=" + std::to_wstring(ts.sx)).c_str());
}

void CaseExecOptsSameRes() {
    ScriptAction a{};
    a.type = ActionType::FindImage;
    a.matchThreshold = 70;
    a.imageScaleMin = 1.0;
    a.imageScaleMax = 1.0;
    TemplateScale ts{1.0, 1.0};
    const ImageMatchOptions opt = BuildExecutionFindImageOptions(a, ts);
    const bool ok = opt.disablePyramid && Near(opt.scaleMin, 1.0) && Near(opt.scaleMax, 1.0);
    Emit(L"exec_find_opts_same_res", ok, ok ? L"" : L"same-res should disablePyramid");
}

void CaseExecOptsCrossIso() {
    ScriptAction a{};
    a.type = ActionType::FindImage;
    a.imageScaleMin = 1.0;
    a.imageScaleMax = 1.0;
    TemplateScale ts{0.5, 0.5};
    const ImageMatchOptions opt = BuildExecutionFindImageOptions(a, ts);
    const bool ok = opt.crossResolutionMatch && opt.scaleMin < opt.scaleMax
        && Near(opt.scaleMin, 0.5 * 0.94, 0.02);
    Emit(L"exec_find_opts_cross_iso", ok, ok ? L"" : L"iso cross-res band broken");
}

void CaseResolveClick() {
    ImageMatchResult m{};
    m.found = true;
    m.topLeftX = 10;
    m.topLeftY = 20;
    m.bottomRightX = 50;
    m.bottomRightY = 60;
    TemplateScale ts{1.0, 1.0};
    int tx = 0, ty = 0;
    ResolveFindImageClickPoint(m, 40, 40, 0.25, -0.25, ts, false, tx, ty);
    // center=(30,40); nOffset*(origTpl) => +10,-10 -> (40,30)
    const bool ok = tx == 40 && ty == 30;
    Emit(L"resolve_click_point_offset", ok,
        ok ? L"" : (L"tx=" + std::to_wstring(tx) + L" ty=" + std::to_wstring(ty)).c_str());
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
                L"  CoordSpaceSelfTest.exe [--json] [--list] [--help]\n");
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"CoordSpaceSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }
    if (!selftest::gJson) {
        std::fwprintf(stderr, L"=== CoordSpaceSelfTest ===\n");
    }

    CaseStandardMeta();
    CaseSaveMetaCaptures();
    CaseExecMetaCapture();
    CaseExecMetaLegacy();
    CaseCoordMetaRoundtrip();
    CaseHasCoordMeta();
    CaseNormalizeMove();
    CaseNormalizeRelativeSkip();
    CaseMigrateLegacy();
    CaseTemplateScale();
    CaseExecOptsSameRes();
    CaseExecOptsCrossIso();
    CaseResolveClick();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
