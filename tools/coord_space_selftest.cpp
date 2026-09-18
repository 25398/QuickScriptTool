// =============================================================================
// CoordSpaceSelfTest — 坐标归一化 / coordMeta 自检
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:CoordSpaceSelfTest
//   build\Release\CoordSpaceSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "coord_space.h"
#include "findimage_template_crop.h"
#include "image_match.h"
#include "script_types.h"
#include "utils.h"

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
    {L"normalize_mousedrag_end_nstar", L"default",
        L"NormalizeActionCoords maps mouseDrag endX/endY to nEndX/nEndY"},
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
    {L"crop_normalize_inverted", L"crop",
        L"R<L / B<T normalize"},
    {L"crop_clamp_to_image", L"crop",
        L"out-of-bounds clamp"},
    {L"crop_full_identity", L"crop",
        L"full image keeps offset"},
    {L"crop_offset_preserves_click", L"crop",
        L"100x80,(10,-4),[20,10,90,70) -> (5,-4)"},
    {L"crop_allows_click_outside_rect", L"crop",
        L"in-image click outside crop still ok"},
    {L"crop_allows_click_outside_image", L"crop",
        L"click outside image + valid crop ok"},
    {L"crop_min_side", L"crop",
        L"side <8 fails"},
    {L"crop_odd_size_center", L"crop",
        L"W=101 uses W/2"},
    {L"crop_chain_twice", L"crop",
        L"two crops preserve click vs original"},
    {L"crop_noffset_roundtrip", L"crop",
        L"Sync then round(nOffset*W)==offset"},
    {L"getcolor_imagelocate_xy_roundtrip", L"default",
        L"getColor imageLocate x/y 按模板 Sync/Denorm 往返"},
    {L"var_image_offset_norm_from_producer", L"default",
        L"变量图 offset 按前序保存图片搜索区归一化，不按屏幕宽高"},
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

void CaseNormalizeMouseDragEnd() {
    CoordMeta meta = StandardScriptCoordMeta();
    ScriptAction a{};
    a.type = ActionType::MouseDrag;
    a.x = 1280;
    a.y = 720;
    a.endX = 2560;
    a.endY = 0;
    NormalizeActionCoords(a, meta);
    const bool ok = a.coordsAreNormalized
        && Near(a.nx, 0.5) && Near(a.ny, 0.5)
        && Near(a.nEndX, 1.0) && Near(a.nEndY, 0.0);
    Emit(L"normalize_mousedrag_end_nstar", ok,
        ok ? L"" : (L"nEndX=" + std::to_wstring(a.nEndX) + L" nEndY=" + std::to_wstring(a.nEndY)).c_str());
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

void CaseResolveClickRemappedBox() {
    // 窗口模式把命中从截图 80x80 映射到客户区 40x40 后，偏移必须跟框缩放，
    // 不能仍按 origTpl=80 × scale=1 加到客户区中心。
    ImageMatchResult m{};
    m.found = true;
    m.topLeftX = 10;
    m.topLeftY = 20;
    m.bottomRightX = 50;
    m.bottomRightY = 60; // 40x40 客户区框
    m.scale = 1.0;
    TemplateScale ts{1.0, 1.0};
    int tx = 0, ty = 0;
    ResolveFindImageClickPoint(m, 80, 80, 0.25, -0.25, ts, false, tx, ty);
    // center=(30,40); nOffset×box => +10,-10 -> (40,30)
    // 若误用 origTpl×scale 会得到 +20,-20 -> (50,20)
    const bool ok = tx == 40 && ty == 30;
    Emit(L"resolve_click_point_remapped_box", ok,
        ok ? L"" : (L"tx=" + std::to_wstring(tx) + L" ty=" + std::to_wstring(ty)).c_str());
}

void CaseCropNormalize() {
    const CropRect r = NormalizeCropRect(90, 70, 20, 10);
    Emit(L"crop_normalize_inverted",
        r.L == 20 && r.T == 10 && r.R == 90 && r.B == 70, L"");
}

void CaseCropClamp() {
    CropRect r = NormalizeCropRect(-10, -5, 2000, 1500);
    r = ClampCropRectToImage(r, 100, 80);
    Emit(L"crop_clamp_to_image",
        r.L == 0 && r.T == 0 && r.R == 100 && r.B == 80, L"");
}

void CaseCropFullIdentity() {
    CropRect full{0, 0, 100, 80};
    const auto res = ComputeCroppedFindImageOffset(100, 80, 10, -4, full);
    Emit(L"crop_full_identity",
        res.ok && IsFullImageCrop(res.rect, 100, 80)
            && res.offsetX == 10 && res.offsetY == -4, L"");
}

void CaseCropOffsetPreserves() {
    CropRect raw{20, 10, 90, 70};
    const auto res = ComputeCroppedFindImageOffset(100, 80, 10, -4, raw);
    Emit(L"crop_offset_preserves_click",
        res.ok && res.offsetX == 5 && res.offsetY == -4
            && res.rect.L == 20 && res.rect.T == 10
            && res.rect.R == 90 && res.rect.B == 70, L"");
}

void CaseCropAllowsOutsideRect() {
    // click=(50,40); crop excludes it → still ok, offset' relative to crop
    CropRect raw{0, 0, 40, 30};
    const auto res = ComputeCroppedFindImageOffset(100, 80, 0, 0, raw);
    // offsetX'=(50-0)-20=30, offsetY'=(40-0)-15=25
    Emit(L"crop_allows_click_outside_rect",
        res.ok && res.offsetX == 30 && res.offsetY == 25, L"");
}

void CaseCropAllowsOutsideImage() {
    // W/2=50, offset=100 → clickX=150 outside; crop ok without contain
    CropRect raw{10, 10, 50, 50};
    const auto res = ComputeCroppedFindImageOffset(100, 80, 100, 0, raw);
    Emit(L"crop_allows_click_outside_image",
        res.ok && res.reject == CropOffsetReject::None, L"");
}

void CaseCropMinSide() {
    CropRect raw{0, 0, 7, 40};
    const auto res = ComputeCroppedFindImageOffset(100, 80, 0, 0, raw);
    Emit(L"crop_min_side",
        !res.ok && res.reject == CropOffsetReject::MinSide, L"");
}

void CaseCropOddCenter() {
    // W=101 → oldCx=W/2=50（向零）
    CropRect raw{0, 0, 80, 80};
    const auto res = ComputeCroppedFindImageOffset(101, 80, 0, 0, raw);
    // offsetX'=(50-0)-40=10；若误用 (W+1)/2=51 会得到 11
    Emit(L"crop_odd_size_center",
        res.ok && res.offsetX == 10 && res.offsetY == 0, L"");
}

void CaseCropChainTwice() {
    const int W0 = 100, H0 = 80, ox0 = 10, oy0 = -4;
    const int clickX = W0 / 2 + ox0;
    const int clickY = H0 / 2 + oy0;
    auto r1 = ComputeCroppedFindImageOffset(W0, H0, ox0, oy0, CropRect{20, 10, 90, 70});
    const int W1 = r1.rect.R - r1.rect.L;
    const int H1 = r1.rect.B - r1.rect.T;
    // second crop relative to first image
    auto r2 = ComputeCroppedFindImageOffset(W1, H1, r1.offsetX, r1.offsetY,
        CropRect{5, 5, W1 - 5, H1 - 5});
    const int W2 = r2.rect.R - r2.rect.L;
    const int H2 = r2.rect.B - r2.rect.T;
    // Map click back to original: abs = L1 + L2 + (W2/2 + ox2)
    const int click2 = (r2.rect.L) + (W2 / 2 + r2.offsetX);
    const int click2y = (r2.rect.T) + (H2 / 2 + r2.offsetY);
    const int absX = r1.rect.L + click2;
    const int absY = r1.rect.T + click2y;
    Emit(L"crop_chain_twice",
        r1.ok && r2.ok && absX == clickX && absY == clickY, L"");
}

void CaseCropNOffsetRoundtrip() {
    // Create solid 70x60 bmp then Sync
    EnsureFindImagesDir();
    const std::wstring path = FindImagesDir() + L"\\selftest_crop_noffset.bmp";
    // Build via CaptureScreenRegion of a tiny area then... easier: use SaveCropped
    // from a temporary captured piece. Or create DIB manually.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 70;
    bi.bmiHeader.biHeight = -60;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp && bits) {
        memset(bits, 0x40, static_cast<size_t>(70) * 60 * 4);
        ok = SaveBitmapToFile(bmp, path);
        DeleteBitmapHandle(bmp);
    }
    if (!ok) {
        Emit(L"crop_noffset_roundtrip", false, L"failed to write stub bmp");
        return;
    }
    ScriptAction a{};
    a.type = ActionType::FindImage;
    a.imagePath = path;
    a.offsetX = 5;
    a.offsetY = -4;
    SyncFindImageOffsetNorm(a);
    const int rx = static_cast<int>(std::round(a.nOffsetX * 70.0));
    const int ry = static_cast<int>(std::round(a.nOffsetY * 60.0));
    Emit(L"crop_noffset_roundtrip", rx == 5 && ry == -4, L"");
}

void CaseGetColorImageLocateXyRoundtrip() {
    const std::wstring path = AppDir() + L"\\selftest_getcolor_locate.bmp";
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 20;
    bi.bmiHeader.biHeight = -10;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool wrote = false;
    if (bmp && bits) {
        memset(bits, 0x40, static_cast<size_t>(20) * 10 * 4);
        wrote = SaveBitmapToFile(bmp, path);
        DeleteBitmapHandle(bmp);
    }
    if (!wrote) {
        Emit(L"getcolor_imagelocate_xy_roundtrip", false, L"failed to write stub bmp");
        return;
    }
    ScriptAction a{};
    a.type = ActionType::GetColor;
    a.imageLocate = true;
    a.imagePath = path;
    a.x = 8;
    a.y = -2;
    SyncMouseDragNorm(a);
    const CoordMeta meta = StandardScriptCoordMeta();
    DenormalizeActionCoords(a, meta, meta.refWidth, meta.refHeight);
    DeleteFileW(path.c_str());
    const bool ok = Near(a.nx, 8.0 / 20.0) && Near(a.ny, -2.0 / 10.0)
        && a.x == 8 && a.y == -2;
    Emit(L"getcolor_imagelocate_xy_roundtrip", ok, ok ? L"" : L"template-relative xy lost");
}

void CaseVarImageOffsetNormFromProducer() {
    ScriptAction save{};
    save.type = ActionType::FindImage;
    save.findImageFollowUp = 3;
    save.matchVarName = L"image";
    save.searchFullScreen = false;
    save.searchX1 = 10;
    save.searchY1 = 20;
    save.searchX2 = 90;
    save.searchY2 = 60; // 80x40 保存区

    ScriptAction click{};
    click.type = ActionType::FindImage;
    click.imageUseVar = true;
    click.imagePath = L"image";
    click.findImageFollowUp = 0;
    click.offsetX = 10;
    click.offsetY = -4;

    std::vector<ScriptAction> acts{save, click};
    const CoordMeta meta = StandardScriptCoordMeta();
    SyncNormFieldsFromPixels(acts, meta);
    const bool normOk = Near(acts[1].nOffsetX, 10.0 / 80.0)
        && Near(acts[1].nOffsetY, -4.0 / 40.0)
        && !Near(acts[1].nOffsetX, 10.0 / static_cast<double>(meta.refWidth));
    DenormalizeScriptCoords(acts, meta, meta.refWidth, meta.refHeight);
    const bool denormOk = acts[1].offsetX == 10 && acts[1].offsetY == -4;
    Emit(L"var_image_offset_norm_from_producer", normOk && denormOk,
        (normOk && denormOk)
            ? L""
            : (L"n=(" + std::to_wstring(acts[1].nOffsetX) + L","
                + std::to_wstring(acts[1].nOffsetY) + L") px=("
                + std::to_wstring(acts[1].offsetX) + L","
                + std::to_wstring(acts[1].offsetY) + L")").c_str());
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
    CaseNormalizeMouseDragEnd();
    CaseMigrateLegacy();
    CaseTemplateScale();
    CaseExecOptsSameRes();
    CaseExecOptsCrossIso();
    CaseResolveClick();
    CaseResolveClickRemappedBox();
    CaseCropNormalize();
    CaseCropClamp();
    CaseCropFullIdentity();
    CaseCropOffsetPreserves();
    CaseCropAllowsOutsideRect();
    CaseCropAllowsOutsideImage();
    CaseCropMinSide();
    CaseCropOddCenter();
    CaseCropChainTwice();
    CaseCropNOffsetRoundtrip();
    CaseGetColorImageLocateXyRoundtrip();
    CaseVarImageOffsetNormFromProducer();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
