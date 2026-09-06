// =============================================================================
// ImageMatchSelfTest — 找图引擎纯逻辑自检（无实时截屏/叠加层）
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:ImageMatchSelfTest
//   build\Release\ImageMatchSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "coord_space.h"
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
        L"FindImageClickPoint = center + offset; RelativeClickOffset inverse"},
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
    {L"identical_fullscreen_template", L"default",
        L"整屏模板+完全一致画面应返回匹配（不报无共识）"},
    {L"real_screen_identical_template", L"default",
        L"真实截图与自身模板完全一致时应匹配"},
    {L"real_screen_file_roundtrip_template", L"default",
        L"真实截图存文件再读回（运行时模板路径）应能匹配"},
    {L"real_template_on_synthetic_screen", L"default",
        L"真实模板贴到合成屏幕，测试选项应在原位找到"},
    {L"real_template_with_live_delta", L"default",
        L"真实模板贴到屏幕后加少量亮度/噪声差（模拟实拍），测试选项应仍能找到"},
    {L"flat_template_finds_on_textured_screen", L"default",
        L"低纹理（近纯色）模板应能在有纹理的屏幕上被找到"},
    {L"perfect_match_identical", L"default",
        L"perfectMatch accepts identical patch with score 100"},
    {L"perfect_match_rejects_delta2", L"default",
        L"perfectMatch rejects per-channel delta>1"},
    {L"perfect_match_allows_delta1", L"default",
        L"perfectMatch accepts per-channel delta<=1"},
    {L"screen_diff_identical", L"default",
        L"DiffBitmapsChangedRegions nearlyIdentical on same frame"},
    {L"screen_diff_finds_roi", L"default",
        L"DiffBitmapsChangedRegions reports changed bounding box"},
    {L"busy_mask_marks_persistent_motion", L"default",
        L"BuildBusyMaskFromTripleFrames marks continuously changing cells"},
    {L"diff_ignores_busy_as_dynamic_only", L"default",
        L"Diff with busy mask treats video-like motion as onlyDynamicChanged"},
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

void CaseIdenticalFullscreenTemplate() {
    // 复现「整屏模板 + 完全一致画面」：bestNcc≈100% 却报「无共识匹配」。
    // 整屏模板应返回 (0,0) 高匹配，不应被共识拒绝。
    HBITMAP screen = MakeSolidBmp(200, 150, RGB(230, 230, 230));
    PaintRect(screen, 20, 15, 60, 40, RGB(60, 60, 200));
    PaintRect(screen, 120, 90, 50, 30, RGB(200, 60, 60));
    HBITMAP tpl = MakeSolidBmp(200, 150, RGB(230, 230, 230));
    PaintRect(tpl, 20, 15, 60, 40, RGB(60, 60, 200));
    PaintRect(tpl, 120, 90, 50, 30, RGB(200, 60, 60));

    ImageMatchOptions opt{};
    opt.thresholdPercent = 65.0;
    opt.scaleMin = opt.scaleMax = 1.0;
    opt.disablePyramid = true;
    const ImageMatchOutput out = FindTemplateInFrozenScreenMulti(
        screen, 0, 0, 0, 0, 200, 150, tpl, opt);
    DeleteBitmapHandle(tpl);
    DeleteBitmapHandle(screen);

    const bool ok = out.found && !out.matches.empty()
        && out.matches[0].score >= 65.0;
    std::wstring detail;
    if (!ok) {
        detail = L"found=" + std::to_wstring(out.found ? 1 : 0)
            + L" n=" + std::to_wstring(out.matches.size())
            + L" bestNcc=" + std::to_wstring(out.debugBestNccPercent)
            + L" raw=" + std::to_wstring(out.debugRawCandidates);
        if (!out.matches.empty()) {
            detail += L" tl=" + std::to_wstring(out.matches[0].topLeftX)
                + L"," + std::to_wstring(out.matches[0].topLeftY)
                + L" score=" + std::to_wstring(out.matches[0].score);
        }
    }
    Emit(L"identical_fullscreen_template", ok, detail.c_str());
}

void CaseRealScreenIdenticalTemplate() {
    // 真实屏幕截图 + 同一份作为模板（完全一致）应匹配。
    // 排除「真实截图/格式导致整屏匹配被共识误拒」的可能。
    int vx = 0, vy = 0;
    HBITMAP screen = CaptureVirtualScreen(vx, vy);
    if (!screen) {
        Emit(L"real_screen_identical_template", false, L"截屏失败");
        return;
    }
    HBITMAP tpl = static_cast<HBITMAP>(CopyImage(
        screen, IMAGE_BITMAP, 0, 0, LR_COPYRETURNORG));
    if (!tpl) {
        DeleteBitmapHandle(screen);
        Emit(L"real_screen_identical_template", false, L"复制模板失败");
        return;
    }
    BITMAP bm{};
    GetObjectW(screen, sizeof(bm), &bm);
    ImageMatchOptions opt{};
    opt.thresholdPercent = 65.0;
    opt.scaleMin = opt.scaleMax = 1.0;
    opt.disablePyramid = true;
    const ImageMatchOutput out = FindTemplateInFrozenScreenMulti(
        screen, vx, vy, vx, vy, vx + bm.bmWidth, vy + bm.bmHeight, tpl, opt);
    DeleteBitmapHandle(tpl);
    DeleteBitmapHandle(screen);
    const bool ok = out.found && !out.matches.empty()
        && out.matches[0].score >= 65.0;
    std::wstring detail;
    if (!ok) {
        detail = L"n=" + std::to_wstring(out.matches.size())
            + L" bestNcc=" + std::to_wstring(out.debugBestNccPercent)
            + L" raw=" + std::to_wstring(out.debugRawCandidates);
    }
    Emit(L"real_screen_identical_template", ok, detail.c_str());
}

void CaseRealScreenFileRoundtripTemplate() {
    // 模拟运行时模板路径：真实截图 → 存 PNG → 读回 → 与实时画面匹配。
    // 若文件往返改变像素（位深/alpha/DPI），整屏匹配会被共识误拒。
    int vx = 0, vy = 0;
    HBITMAP screen = CaptureVirtualScreen(vx, vy);
    if (!screen) {
        Emit(L"real_screen_file_roundtrip_template", false, L"截屏失败");
        return;
    }
    wchar_t tmpDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmpDir);
    const std::wstring path = std::wstring(tmpDir) + L"__img_match_roundtrip.png";
    DeleteFileW(path.c_str());
    if (!SaveBitmapToFile(screen, path)) {
        DeleteBitmapHandle(screen);
        Emit(L"real_screen_file_roundtrip_template", false, L"保存模板失败");
        return;
    }
    HBITMAP tpl = LoadBitmapFromFile(path);
    DeleteFileW(path.c_str());
    if (!tpl) {
        DeleteBitmapHandle(screen);
        Emit(L"real_screen_file_roundtrip_template", false, L"加载模板失败");
        return;
    }
    BITMAP bm{};
    GetObjectW(screen, sizeof(bm), &bm);
    ImageMatchOptions opt{};
    opt.thresholdPercent = 65.0;
    opt.scaleMin = opt.scaleMax = 1.0;
    opt.disablePyramid = true;
    const ImageMatchOutput out = FindTemplateInFrozenScreenMulti(
        screen, vx, vy, vx, vy, vx + bm.bmWidth, vy + bm.bmHeight, tpl, opt);
    DeleteBitmapHandle(tpl);
    DeleteBitmapHandle(screen);
    const bool ok = out.found && !out.matches.empty()
        && out.matches[0].score >= 65.0;
    std::wstring detail;
    if (!ok) {
        detail = L"n=" + std::to_wstring(out.matches.size())
            + L" bestNcc=" + std::to_wstring(out.debugBestNccPercent)
            + L" raw=" + std::to_wstring(out.debugRawCandidates);
    }
    Emit(L"real_screen_file_roundtrip_template", ok, detail.c_str());
}

void CaseRealTemplateOnSyntheticScreen() {
    // 取一个真实用户模板（scripts\images\template_*.bmp），贴到合成屏幕上，
    // 用与「测试」完全相同的选项搜索：完全一致时应命中原位。
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring appDir(exePath);
    const auto slash = appDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) appDir.erase(slash);
    const std::wstring imgDir = appDir + L"\\scripts\\images";
    const std::wstring pattern = imgDir + L"\\template_*.bmp";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        Emit(L"real_template_on_synthetic_screen", false, L"无模板文件");
        return;
    }
    std::wstring fileName = fd.cFileName;
    FindClose(hFind);
    const std::wstring tplPath = imgDir + L"\\" + fileName;

    HBITMAP tpl = LoadBitmapFromFile(tplPath);
    if (!tpl) {
        Emit(L"real_template_on_synthetic_screen", false, L"加载模板失败");
        return;
    }
    BITMAP tb{};
    GetObjectW(tpl, sizeof(tb), &tb);
    const int tplW = static_cast<int>(tb.bmWidth);
    const int tplH = static_cast<int>(tb.bmHeight);

    const int scrW = 1600;
    const int scrH = 900;
    HBITMAP screen = CreateCompatibleBitmap(GetDC(nullptr), scrW, scrH);
    {
        HDC screenDc = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screenDc);
        HGDIOBJ old = SelectObject(mem, screen);
        // 噪声背景，避免整屏低纹理误判
        HBRUSH bg = CreateSolidBrush(RGB(210, 205, 198));
        RECT rc{0, 0, scrW, scrH};
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        srand(12345);
        for (int i = 0; i < 1200; ++i) {
            const int x = rand() % scrW;
            const int y = rand() % scrH;
            const int v = 40 + rand() % 180;
            SetPixel(mem, x, y, RGB(v, v / 2, 255 - v));
        }
        // 若干干扰方块
        for (int i = 0; i < 8; ++i) {
            const int x = rand() % (scrW - 90);
            const int y = rand() % (scrH - 75);
            HBRUSH br = CreateSolidBrush(RGB(rand() % 256, rand() % 256, rand() % 256));
            RECT r2{x, y, x + 60 + rand() % 50, y + 40 + rand() % 40};
            FillRect(mem, &r2, br);
            DeleteObject(br);
        }
        // 原位粘贴模板（完全一致）
        HDC tplDc = CreateCompatibleDC(screenDc);
        HGDIOBJ oldT = SelectObject(tplDc, tpl);
        BitBlt(mem, 300, 200, tplW, tplH, tplDc, 0, 0, SRCCOPY);
        SelectObject(tplDc, oldT);
        DeleteDC(tplDc);
        SelectObject(mem, old);
        DeleteDC(mem);
        ReleaseDC(nullptr, screenDc);
    }

    ScriptAction probe{};
    probe.matchThreshold = 65.0;
    probe.imageScaleMin = 0.9;
    probe.imageScaleMax = 1.1;
    TemplateScale ts{};
    const ImageMatchOptions opt = BuildExecutionFindImageOptions(probe, ts);
    const ImageMatchOutput out = FindTemplateInFrozenScreenMulti(
        screen, 0, 0, 0, 0, scrW, scrH, tpl, opt);
    DeleteBitmapHandle(tpl);
    DeleteBitmapHandle(screen);

    bool ok = out.found && !out.matches.empty();
    std::wstring detail;
    if (ok) {
        const auto& m = out.matches[0];
        const int dx = std::abs(m.topLeftX - 300);
        const int dy = std::abs(m.topLeftY - 200);
        ok = dx <= std::max(4, tplW / 4) && dy <= std::max(4, tplH / 4);
        detail = L"tl=" + std::to_wstring(m.topLeftX) + L"," + std::to_wstring(m.topLeftY)
            + L" score=" + std::to_wstring(m.score)
            + L" n=" + std::to_wstring(out.matches.size());
    }
    if (!ok) {
        detail = L"n=" + std::to_wstring(out.matches.size())
            + L" bestNcc=" + std::to_wstring(out.debugBestNccPercent)
            + L" raw=" + std::to_wstring(out.debugRawCandidates);
        if (!out.matches.empty()) {
            const auto& m = out.matches[0];
            detail += L" tl=" + std::to_wstring(m.topLeftX)
                + L"," + std::to_wstring(m.topLeftY)
                + L" score=" + std::to_wstring(m.score);
        }
    }
    Emit(L"real_template_on_synthetic_screen", ok, detail.c_str());
}

void CaseRealTemplateWithLiveDelta() {
    // 真实画面与模板几乎一致但存在少量像素差（亮度 +4、±3 噪声）——
    // 若三引擎共识对 CCORR 敏感，会被误拒为「找不到」。
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring appDir(exePath);
    const auto slash = appDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) appDir.erase(slash);
    const std::wstring imgDir = appDir + L"\\scripts\\images";
    const std::wstring pattern = imgDir + L"\\template_*.bmp";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        Emit(L"real_template_with_live_delta", false, L"无模板文件");
        return;
    }
    const std::wstring fileName = fd.cFileName;
    FindClose(hFind);

    HBITMAP tpl = LoadBitmapFromFile(imgDir + L"\\" + fileName);
    if (!tpl) {
        Emit(L"real_template_with_live_delta", false, L"加载模板失败");
        return;
    }
    BITMAP tb{};
    GetObjectW(tpl, sizeof(tb), &tb);
    const int tplW = static_cast<int>(tb.bmWidth);
    const int tplH = static_cast<int>(tb.bmHeight);
    const int scrW = 1600;
    const int scrH = 900;
    const int px = 300;
    const int py = 200;

    // 先画一张与模板完全一致的屏幕，再整体加少量亮度/噪声
    HBITMAP screen = CreateCompatibleBitmap(GetDC(nullptr), scrW, scrH);
    {
        HDC screenDc = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screenDc);
        HGDIOBJ old = SelectObject(mem, screen);
        HBRUSH bg = CreateSolidBrush(RGB(205, 198, 190));
        RECT rc{0, 0, scrW, scrH};
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        srand(777);
        for (int i = 0; i < 1500; ++i) {
            const int x = rand() % scrW;
            const int y = rand() % scrH;
            const int v = 40 + rand() % 180;
            SetPixel(mem, x, y, RGB(v, v / 2, 255 - v));
        }
        HDC tplDc = CreateCompatibleDC(screenDc);
        HGDIOBJ oldT = SelectObject(tplDc, tpl);
        BitBlt(mem, px, py, tplW, tplH, tplDc, 0, 0, SRCCOPY);
        SelectObject(tplDc, oldT);
        DeleteDC(tplDc);
        SelectObject(mem, old);
        DeleteDC(mem);
        ReleaseDC(nullptr, screenDc);
    }
    // 模板本体也做同量的亮度/噪声处理 → 模拟「画面显示状态与截图时略有差异」
    {
        HDC screenDc = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screenDc);
        HGDIOBJ old = SelectObject(mem, screen);
        srand(999);
        for (int y = py; y < py + tplH; ++y) {
            for (int x = px; x < px + tplW; ++x) {
                COLORREF c = GetPixel(mem, x, y);
                const int dr = std::max(0, std::min(255, (int)GetRValue(c) + 4 + (rand() % 7) - 3));
                const int dg = std::max(0, std::min(255, (int)GetGValue(c) + 4 + (rand() % 7) - 3));
                const int db = std::max(0, std::min(255, (int)GetBValue(c) + 4 + (rand() % 7) - 3));
                SetPixel(mem, x, y, RGB(dr, dg, db));
            }
        }
        SelectObject(mem, old);
        DeleteDC(mem);
        ReleaseDC(nullptr, screenDc);
    }

    ScriptAction probe{};
    probe.matchThreshold = 65.0;
    probe.imageScaleMin = 0.9;
    probe.imageScaleMax = 1.1;
    TemplateScale ts{};
    const ImageMatchOptions opt = BuildExecutionFindImageOptions(probe, ts);
    const ImageMatchOutput out = FindTemplateInFrozenScreenMulti(
        screen, 0, 0, 0, 0, scrW, scrH, tpl, opt);
    DeleteBitmapHandle(tpl);
    DeleteBitmapHandle(screen);

    bool ok = out.found && !out.matches.empty();
    std::wstring detail;
    if (ok) {
        const auto& m = out.matches[0];
        const int dx = std::abs(m.topLeftX - px);
        const int dy = std::abs(m.topLeftY - py);
        ok = dx <= std::max(4, tplW / 4) && dy <= std::max(4, tplH / 4);
        detail = L"tl=" + std::to_wstring(m.topLeftX) + L"," + std::to_wstring(m.topLeftY)
            + L" score=" + std::to_wstring(m.score)
            + L" n=" + std::to_wstring(out.matches.size());
    }
    if (!ok) {
        detail = L"n=" + std::to_wstring(out.matches.size())
            + L" bestNcc=" + std::to_wstring(out.debugBestNccPercent)
            + L" raw=" + std::to_wstring(out.debugRawCandidates);
        if (!out.matches.empty()) {
            const auto& m = out.matches[0];
            detail += L" tl=" + std::to_wstring(m.topLeftX)
                + L"," + std::to_wstring(m.topLeftY)
                + L" score=" + std::to_wstring(m.score);
        }
    }
    Emit(L"real_template_with_live_delta", ok, detail.c_str());
}

void CaseFlatTemplateFindsOnTexturedScreen() {
    // 复现「找图测试找不到」的高概率场景：模板是近纯色（游戏按钮/色块），
    // 边缘验证器对低纹理模板返回 0 → 共识把真实位置拒掉。
    const int tplW = 64;
    const int tplH = 48;
    const int scrW = 1200;
    const int scrH = 800;
    const int px = 400;
    const int py = 300;

    // 模板：主色块 + 少量圆角渐变（低纹理，非完全纯色）
    HBITMAP tpl = MakeSolidBmp(tplW, tplH, RGB(70, 110, 170));
    {
        HDC dc = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, tpl);
        for (int y = 0; y < tplH; ++y) {
            for (int x = 0; x < tplW; ++x) {
                const int v = 8 + (x * 3 + y * 2) % 24;
                SetPixel(mem, x, y, RGB(70 + v / 2, 110 + v, 170 + v / 3));
            }
        }
        SelectObject(mem, old);
        DeleteDC(mem);
        ReleaseDC(nullptr, dc);
    }

    HBITMAP screen = CreateCompatibleBitmap(GetDC(nullptr), scrW, scrH);
    {
        HDC screenDc = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screenDc);
        HGDIOBJ old = SelectObject(mem, screen);
        HBRUSH bg = CreateSolidBrush(RGB(210, 205, 198));
        RECT rc{0, 0, scrW, scrH};
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        srand(4242);
        for (int i = 0; i < 2500; ++i) {
            const int x = rand() % scrW;
            const int y = rand() % scrH;
            const int v = 40 + rand() % 180;
            SetPixel(mem, x, y, RGB(v, v / 2, 255 - v));
        }
        for (int i = 0; i < 10; ++i) {
            const int x = rand() % (scrW - 80);
            const int y = rand() % (scrH - 60);
            HBRUSH br = CreateSolidBrush(RGB(rand() % 256, rand() % 256, rand() % 256));
            RECT r2{x, y, x + 50 + rand() % 60, y + 30 + rand() % 40};
            FillRect(mem, &r2, br);
            DeleteObject(br);
        }
        HDC tplDc = CreateCompatibleDC(screenDc);
        HGDIOBJ oldT = SelectObject(tplDc, tpl);
        BitBlt(mem, px, py, tplW, tplH, tplDc, 0, 0, SRCCOPY);
        SelectObject(tplDc, oldT);
        DeleteDC(tplDc);
        SelectObject(mem, old);
        DeleteDC(mem);
        ReleaseDC(nullptr, screenDc);
    }

    ScriptAction probe{};
    probe.matchThreshold = 65.0;
    probe.imageScaleMin = 0.9;
    probe.imageScaleMax = 1.1;
    TemplateScale ts{};
    const ImageMatchOptions opt = BuildExecutionFindImageOptions(probe, ts);
    const ImageMatchOutput out = FindTemplateInFrozenScreenMulti(
        screen, 0, 0, 0, 0, scrW, scrH, tpl, opt);
    DeleteBitmapHandle(tpl);
    DeleteBitmapHandle(screen);

    bool ok = out.found && !out.matches.empty();
    std::wstring detail;
    if (ok) {
        const auto& m = out.matches[0];
        const int dx = std::abs(m.topLeftX - px);
        const int dy = std::abs(m.topLeftY - py);
        ok = dx <= std::max(4, tplW / 3) && dy <= std::max(4, tplH / 3);
        detail = L"tl=" + std::to_wstring(m.topLeftX) + L"," + std::to_wstring(m.topLeftY)
            + L" score=" + std::to_wstring(m.score)
            + L" n=" + std::to_wstring(out.matches.size());
    }
    if (!ok) {
        detail = L"n=" + std::to_wstring(out.matches.size())
            + L" bestNcc=" + std::to_wstring(out.debugBestNccPercent)
            + L" raw=" + std::to_wstring(out.debugRawCandidates);
        if (!out.matches.empty()) {
            const auto& m = out.matches[0];
            detail += L" tl=" + std::to_wstring(m.topLeftX)
                + L"," + std::to_wstring(m.topLeftY)
                + L" score=" + std::to_wstring(m.score);
        }
    }
    Emit(L"flat_template_finds_on_textured_screen", ok, detail.c_str());
}

void CasePerfectIdentical() {
    HBITMAP screen = MakeSolidBmp(120, 80, RGB(240, 240, 240));
    PaintRect(screen, 40, 20, 24, 24, RGB(20, 20, 200));
    HBITMAP tpl = MakeSolidBmp(24, 24, RGB(20, 20, 200));
    ImageMatchOptions opt{};
    opt.perfectMatch = true;
    opt.perfectMatchChannelTol = 1;
    opt.scaleMin = opt.scaleMax = 1.0;
    opt.disablePyramid = true;
    const ImageMatchOutput out = FindTemplateInFrozenScreenMulti(
        screen, 0, 0, 0, 0, 120, 80, tpl, opt);
    DeleteBitmapHandle(tpl);
    DeleteBitmapHandle(screen);
    const bool ok = out.found && !out.matches.empty()
        && out.matches[0].score >= 99.9
        && out.matches[0].topLeftX == 40
        && out.matches[0].topLeftY == 20;
    std::wstring detail = ok ? L"score=100"
        : (L"found=" + std::to_wstring(out.found ? 1 : 0)
            + L" n=" + std::to_wstring(out.matches.size())
            + L" bestNcc=" + std::to_wstring(out.debugBestNccPercent));
    Emit(L"perfect_match_identical", ok, detail.c_str());
}

void CasePerfectRejectDelta2() {
    HBITMAP screen = MakeSolidBmp(80, 60, RGB(10, 10, 10));
    PaintRect(screen, 10, 10, 16, 16, RGB(20, 20, 200));
    HBITMAP tpl = MakeSolidBmp(16, 16, RGB(20, 20, 202));
    ImageMatchOptions opt{};
    opt.perfectMatch = true;
    opt.perfectMatchChannelTol = 1;
    opt.scaleMin = opt.scaleMax = 1.0;
    opt.disablePyramid = true;
    const ImageMatchOutput out = FindTemplateInFrozenScreenMulti(
        screen, 0, 0, 0, 0, 80, 60, tpl, opt);
    DeleteBitmapHandle(tpl);
    DeleteBitmapHandle(screen);
    Emit(L"perfect_match_rejects_delta2", !out.found,
        out.found ? L"should reject" : L"rejected");
}

void CasePerfectAllowDelta1() {
    HBITMAP screen = MakeSolidBmp(80, 60, RGB(10, 10, 10));
    PaintRect(screen, 10, 10, 16, 16, RGB(20, 20, 200));
    HBITMAP tpl = MakeSolidBmp(16, 16, RGB(20, 20, 201));
    ImageMatchOptions opt{};
    opt.perfectMatch = true;
    opt.perfectMatchChannelTol = 1;
    opt.scaleMin = opt.scaleMax = 1.0;
    opt.disablePyramid = true;
    const ImageMatchOutput out = FindTemplateInFrozenScreenMulti(
        screen, 0, 0, 0, 0, 80, 60, tpl, opt);
    DeleteBitmapHandle(tpl);
    DeleteBitmapHandle(screen);
    const bool ok = out.found && !out.matches.empty() && out.matches[0].score >= 99.9;
    Emit(L"perfect_match_allows_delta1", ok,
        ok ? L"ok" : L"should accept delta1");
}

void CaseScreenDiffIdentical() {
    HBITMAP a = MakeSolidBmp(64, 48, RGB(30, 40, 50));
    HBITMAP b = MakeSolidBmp(64, 48, RGB(30, 40, 50));
    const ScreenChangeDiffResult d = DiffBitmapsChangedRegions(a, b, 12, 16, 4);
    DeleteBitmapHandle(a);
    DeleteBitmapHandle(b);
    Emit(L"screen_diff_identical", d.ok && d.sameSize && d.nearlyIdentical && d.changedRatio == 0.0,
        d.ok ? L"identical" : L"diff failed");
}

void CaseScreenDiffFindsRoi() {
    HBITMAP a = MakeSolidBmp(100, 80, RGB(200, 200, 200));
    HBITMAP b = MakeSolidBmp(100, 80, RGB(200, 200, 200));
    PaintRect(b, 40, 30, 20, 18, RGB(10, 10, 220));
    const ScreenChangeDiffResult d = DiffBitmapsChangedRegions(a, b, 8, 16, 4);
    DeleteBitmapHandle(a);
    DeleteBitmapHandle(b);
    bool hit = false;
    for (const auto& r : d.rois) {
        if (r.x1 <= 40 && r.y1 <= 30 && r.x2 >= 60 && r.y2 >= 48) {
            hit = true;
            break;
        }
    }
    const bool ok = d.ok && d.sameSize && !d.nearlyIdentical && d.changedRatio > 0.0 && hit;
    std::wstring detail = ok ? L"roi ok" : L"rois=" + std::to_wstring(d.rois.size())
        + L" ratio=" + std::to_wstring(d.changedRatio);
    Emit(L"screen_diff_finds_roi", ok, detail.c_str());
}

void CaseBusyMaskPersistentMotion() {
    // 三帧：左侧静态，右侧「视频」每帧换色 → 右侧格应标忙碌
    HBITMAP f0 = MakeSolidBmp(128, 64, RGB(220, 220, 220));
    HBITMAP f1 = MakeSolidBmp(128, 64, RGB(220, 220, 220));
    HBITMAP f2 = MakeSolidBmp(128, 64, RGB(220, 220, 220));
    PaintRect(f0, 80, 8, 40, 48, RGB(10, 10, 10));
    PaintRect(f1, 80, 8, 40, 48, RGB(200, 20, 20));
    PaintRect(f2, 80, 8, 40, 48, RGB(20, 200, 20));
    const ScreenBusyMask busy = BuildBusyMaskFromTripleFrames(f0, f1, f2, 8, 16, 0.05);
    DeleteBitmapHandle(f0);
    DeleteBitmapHandle(f1);
    DeleteBitmapHandle(f2);
    const bool rightBusy = busy.valid() && busy.isBusyPixel(100, 32);
    const bool leftQuiet = busy.valid() && !busy.isBusyPixel(20, 32);
    Emit(L"busy_mask_marks_persistent_motion", rightBusy && leftQuiet && busy.busyCoverageRatio > 0.0,
        rightBusy ? L"right busy" : L"mask failed");
}

void CaseDiffIgnoresBusyDynamicOnly() {
    HBITMAP base = MakeSolidBmp(128, 64, RGB(220, 220, 220));
    PaintRect(base, 80, 8, 40, 48, RGB(10, 10, 10));
    HBITMAP f0 = MakeSolidBmp(128, 64, RGB(220, 220, 220));
    HBITMAP f1 = MakeSolidBmp(128, 64, RGB(220, 220, 220));
    HBITMAP f2 = MakeSolidBmp(128, 64, RGB(220, 220, 220));
    PaintRect(f0, 80, 8, 40, 48, RGB(30, 30, 30));
    PaintRect(f1, 80, 8, 40, 48, RGB(180, 40, 40));
    PaintRect(f2, 80, 8, 40, 48, RGB(40, 180, 40));
    const ScreenBusyMask busy = BuildBusyMaskFromTripleFrames(f0, f1, f2, 8, 16, 0.05);
    // 基线 vs 当前：仅右侧视频区不同
    const ScreenChangeDiffResult d = DiffBitmapsChangedRegions(base, f2, 8, 16, 4, &busy);
    DeleteBitmapHandle(base);
    DeleteBitmapHandle(f0);
    DeleteBitmapHandle(f1);
    DeleteBitmapHandle(f2);
    const bool ok = d.ok && d.onlyDynamicChanged && d.changedRatio < 0.01;
    Emit(L"diff_ignores_busy_as_dynamic_only", ok,
        ok ? L"dynamic only" : (L"struct=" + std::to_wstring(d.changedRatio)
            + L" raw=" + std::to_wstring(d.rawChangedRatio)).c_str());
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
    int ox = 0, oy = 0;
    FindImageRelativeClickOffset(m, tx, ty, ox, oy);
    Emit(L"click_point_with_offset", tx == 7 && ty == 2 && ox == 2 && oy == -3,
        (L"tx=" + std::to_wstring(tx) + L" ty=" + std::to_wstring(ty)
            + L" ox=" + std::to_wstring(ox) + L" oy=" + std::to_wstring(oy)).c_str());
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
    CaseIdenticalFullscreenTemplate();
    CaseRealScreenIdenticalTemplate();
    CaseRealScreenFileRoundtripTemplate();
    CaseRealTemplateOnSyntheticScreen();
    CaseRealTemplateWithLiveDelta();
    CaseFlatTemplateFindsOnTexturedScreen();
    CasePerfectIdentical();
    CasePerfectRejectDelta2();
    CasePerfectAllowDelta1();
    CaseScreenDiffIdentical();
    CaseScreenDiffFindsRoi();
    CaseBusyMaskPersistentMotion();
    CaseDiffIgnoresBusyDynamicOnly();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
