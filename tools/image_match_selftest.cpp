// =============================================================================
// ImageMatchSelfTest — 找图引擎纯逻辑自检（无实时截屏/叠加层）
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:ImageMatchSelfTest
//   build\Release\ImageMatchSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "image_match.h"
#include "image_match_internal.h"

#include <cmath>
#include <string>
#include <vector>

namespace {

using selftest::Emit;
using image_match_internal::CalcPyramidLevels;
using image_match_internal::GlobalNms;
using image_match_internal::RawScoreToSimilarity;
using image_match_internal::SimilarityThreshold01;

const selftest::CaseInfo kCases[] = {
    {L"normalize_match_below_threshold", L"default",
        L"NormalizeMatchVarResult zeros when score<=threshold"},
    {L"normalize_match_above_threshold", L"default",
        L"NormalizeMatchVarResult keeps result when score>threshold"},
    {L"match_center_from_box", L"default",
        L"FindImageMatchCenter is midpoint of TL/BR"},
    {L"click_point_with_offset", L"default",
        L"FindImageClickPoint = center + offset"},
    {L"pyramid_levels_small_tpl", L"default",
        L"CalcPyramidLevels for 16x16"},
    {L"score_sqdiff_normed_invert", L"default",
        L"RawScoreToSimilarity SQDIFF_NORMED inverts"},
    {L"threshold01_modes", L"default",
        L"SimilarityThreshold01 CCOEFF vs SQDIFF"},
    {L"nms_keeps_best_nonoverlap", L"default",
        L"GlobalNms drops lower-score overlapping boxes"},
    {L"nms_respects_max_matches", L"default",
        L"GlobalNms caps at maxMatches"},
    {L"frozen_bitmap_find_template", L"default",
        L"FindTemplateInFrozenScreenMulti finds synthetic patch"},
};

bool Near(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

HBITMAP MakeSolidBmp(int w, int h, COLORREF color) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    HBRUSH br = CreateSolidBrush(color);
    RECT rc{0, 0, w, h};
    FillRect(mem, &rc, br);
    DeleteObject(br);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return bmp;
}

void PaintRect(HBITMAP target, int x, int y, int w, int h, COLORREF color) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, target);
    HBRUSH br = CreateSolidBrush(color);
    RECT rc{x, y, x + w, y + h};
    FillRect(mem, &rc, br);
    DeleteObject(br);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void CaseFrozenMatch() {
    HBITMAP screen = MakeSolidBmp(120, 80, RGB(240, 240, 240));
    if (!screen) {
        Emit(L"frozen_bitmap_find_template", false, L"CreateCompatibleBitmap screen failed");
        return;
    }
    PaintRect(screen, 40, 20, 24, 24, RGB(20, 20, 200));
    HBITMAP tpl = MakeSolidBmp(24, 24, RGB(20, 20, 200));
    if (!tpl) {
        DeleteBitmapHandle(screen);
        Emit(L"frozen_bitmap_find_template", false, L"CreateCompatibleBitmap tpl failed");
        return;
    }

    ImageMatchOptions opt{};
    opt.thresholdPercent = 70.0;
    opt.scaleMin = opt.scaleMax = 1.0;
    opt.disablePyramid = true;
    const ImageMatchOutput out = FindTemplateInFrozenScreenMulti(
        screen, 0, 0, 0, 0, 120, 80, tpl, opt);
    DeleteBitmapHandle(tpl);
    DeleteBitmapHandle(screen);

    const bool ok = out.found && !out.matches.empty() && out.matches[0].score >= 70.0;
    std::wstring detail;
    if (ok) {
        detail = L"score=" + std::to_wstring(out.matches[0].score)
            + L" at " + std::to_wstring(out.matches[0].topLeftX)
            + L"," + std::to_wstring(out.matches[0].topLeftY);
    } else {
        detail = L"found=" + std::to_wstring(out.found ? 1 : 0)
            + L" n=" + std::to_wstring(out.matches.size())
            + L" bestNcc=" + std::to_wstring(out.debugBestNccPercent);
        if (!out.matches.empty()) {
            detail += L" tl=" + std::to_wstring(out.matches[0].topLeftX)
                + L"," + std::to_wstring(out.matches[0].topLeftY)
                + L" score=" + std::to_wstring(out.matches[0].score);
        }
    }
    Emit(L"frozen_bitmap_find_template", ok, detail.c_str());
}

void CaseNormalizeBelow() {
    ImageMatchResult m{};
    m.found = true;
    m.score = 50.0;
    m.x = 1;
    const auto out = NormalizeMatchVarResult(m, 65.0);
    Emit(L"normalize_match_below_threshold", !out.found && out.score == 0.0,
        out.found ? L"should zero" : L"");
}

void CaseNormalizeAbove() {
    ImageMatchResult m{};
    m.found = true;
    m.score = 80.0;
    m.topLeftX = 3;
    const auto out = NormalizeMatchVarResult(m, 65.0);
    Emit(L"normalize_match_above_threshold", out.found && out.score == 80.0 && out.topLeftX == 3,
        out.found ? L"" : L"should keep");
}

void CaseMatchCenter() {
    ImageMatchResult m{};
    m.found = true;
    m.topLeftX = 10;
    m.topLeftY = 20;
    m.bottomRightX = 50;
    m.bottomRightY = 60;
    int cx = 0, cy = 0;
    FindImageMatchCenter(m, cx, cy);
    Emit(L"match_center_from_box", cx == 30 && cy == 40,
        (L"cx=" + std::to_wstring(cx) + L" cy=" + std::to_wstring(cy)).c_str());
}

void CaseClickOffset() {
    ImageMatchResult m{};
    m.found = true;
    m.topLeftX = 0;
    m.topLeftY = 0;
    m.bottomRightX = 10;
    m.bottomRightY = 10;
    int tx = 0, ty = 0;
    FindImageClickPoint(m, 2, -3, tx, ty);
    Emit(L"click_point_with_offset", tx == 7 && ty == 2,
        (L"tx=" + std::to_wstring(tx) + L" ty=" + std::to_wstring(ty)).c_str());
}

void CasePyramid() {
    const int levels = CalcPyramidLevels(16, 16);
    // 16->8 (1), 8->4 (2) then stop (<8)
    Emit(L"pyramid_levels_small_tpl", levels == 2,
        (L"levels=" + std::to_wstring(levels)).c_str());
}

void CaseScoreSqdiff() {
    const double sim = RawScoreToSimilarity(0.2, cv::TM_SQDIFF_NORMED);
    Emit(L"score_sqdiff_normed_invert", Near(sim, 80.0),
        (L"sim=" + std::to_wstring(sim)).c_str());
}

void CaseThreshold01() {
    const double ccoeff = SimilarityThreshold01(70.0, cv::TM_CCOEFF_NORMED);
    const double sqdiff = SimilarityThreshold01(70.0, cv::TM_SQDIFF_NORMED);
    const bool ok = Near(ccoeff, 0.7) && Near(sqdiff, 0.3);
    const std::wstring detail = L"ccoeff=" + std::to_wstring(ccoeff)
        + L" sqdiff=" + std::to_wstring(sqdiff);
    Emit(L"threshold01_modes", ok, detail.c_str());
}

void CaseNmsOverlap() {
    ImageMatchResult a = image_match_internal::MakeResult({0, 0}, 20, 20, 90.0, 1.0);
    ImageMatchResult b = image_match_internal::MakeResult({2, 2}, 20, 20, 80.0, 1.0);
    ImageMatchResult c = image_match_internal::MakeResult({100, 100}, 20, 20, 85.0, 1.0);
    auto kept = GlobalNms({a, b, c}, 0.5, 20);
    const bool ok = kept.size() == 2 && kept[0].score == 90.0 && kept[1].score == 85.0;
    Emit(L"nms_keeps_best_nonoverlap", ok,
        (L"n=" + std::to_wstring(kept.size())).c_str());
}

void CaseNmsMax() {
    std::vector<ImageMatchResult> ms;
    for (int i = 0; i < 5; ++i) {
        ms.push_back(image_match_internal::MakeResult({i * 40, 0}, 10, 10, 90.0 - i, 1.0));
    }
    auto kept = GlobalNms(ms, 0.1, 2);
    Emit(L"nms_respects_max_matches", kept.size() == 2,
        (L"n=" + std::to_wstring(kept.size())).c_str());
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
                L"  ImageMatchSelfTest.exe [--json] [--list] [--help]\n");
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"ImageMatchSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }
    if (!selftest::gJson) {
        std::fwprintf(stderr, L"=== ImageMatchSelfTest ===\n");
    }

    CaseNormalizeBelow();
    CaseNormalizeAbove();
    CaseMatchCenter();
    CaseClickOffset();
    CasePyramid();
    CaseScoreSqdiff();
    CaseThreshold01();
    CaseNmsOverlap();
    CaseNmsMax();
    CaseFrozenMatch();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
