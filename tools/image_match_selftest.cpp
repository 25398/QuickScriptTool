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
#include "findimage_gpu.h"
#include "low_power_mode.h"

#include <opencv2/core/ocl.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
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
    {L"var_offset_anchor_screen_center", L"default",
        L"选偏移时把变量图区域合成到搜索区/屏幕中心，nOffset 仍相对模板"},
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
    {L"findpeaks_sqdiff_keeps_two_minima", L"default",
        L"SQDIFF FindPeaks 抑制不得把后续峰填成更小值"},
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
    {L"similar_buttons_only_true_match", L"default",
        L"同类灰底按钮换内部图标不得以高分命中；只留真正那一枚"},
    {L"unrelated_scene_rejected", L"default",
        L"屏幕上没有模板时不得因 NCC 虚高而填满 maxMatches"},
    {L"repeated_template_finds_all", L"default",
        L"同一模板出现多处时，找图/多图应返回全部命中而不是只留 1 个"},
    {L"offset_pick_max_matches_one", L"default",
        L"选偏移点时 maxMatches=1 只保留最佳一处"},
    {L"nearest_match_offset_not_first", L"default",
        L"相对偏移按离点击最近的命中计算，而不是永远用第一框"},
    {L"alpha_mask_ignores_transparent", L"default",
        L"PNG 透明像素不得当黑边去吸黑色块；只匹配不透明内容"},
    {L"alpha_padded_near_edge", L"default",
        L"透明边伸出搜索区时不得把贴边的不透明内容误拒"},
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
    {L"fullscreen_locate_perf_and_accuracy", L"default",
        L"全屏找图（单命中、单尺度）必须走金字塔粗搜+全分辨率精修：位置仍准，耗时应显著低于全分辨率扫全屏"},
    {L"low_power_limits_cv_threads", L"default",
        L"低性能模式把 OpenCV 线程预算限到 1，关闭后恢复基线（找图线程 fan-out 是升温主因）"},
    {L"template_bitmap_cache_reuse_and_invalidate", L"default",
        L"模板解码缓存：同路径复用；文件改写（大小/时间戳变化）后必须重新解码，不能返回旧像素"},
    {L"opencl_matchtemplate_bench", L"default",
        L"实测 iGPU(OpenCL) vs CPU 跑 matchTemplate：只要求位置一致；无 OpenCL 设备时直接过"},
    {L"gpu_accel_same_result_and_area_gate", L"default",
        L"找图 GPU 加速：大区域命中位置/分数与 CPU 一致；小区域（<500k 像素）不受 GPU 开关影响"},
    {L"find_image_fastpath_gate", L"default",
        L"找图「上一帧命中」本地复核守卫：只在同请求+上次全屏慢+命中新鲜时规划窗口，窗口必须包住漂移带；命中需同实例且分数留 3 点余量"},
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

/// 确定性伪随机纹理：给合成屏幕/模板足够细节，避免落进「低纹理不用金字塔」的分支。
void PaintPattern(HBITMAP target, int w, int h, unsigned int seed) {
    unsigned int s = seed ? seed : 1u;
    for (int y = 0; y < h; y += 8) {
        for (int x = 0; x < w; x += 8) {
            s = s * 1664525u + 1013904223u;
            const int v = static_cast<int>((s >> 16) & 0xFF);
            PaintRect(target, x, y, 8, 8, RGB(v, (v * 3) % 256, (v * 7) % 256));
        }
    }
}

HBITMAP MakeTexturedBmp(int w, int h, unsigned int seed) {
    HBITMAP bmp = MakeSolidBmp(w, h, RGB(128, 128, 128));
    if (bmp) PaintPattern(bmp, w, h, seed);
    return bmp;
}

void BlitBmp(HBITMAP dest, int x, int y, HBITMAP src) {
    BITMAP sb{};
    if (!GetObjectW(src, sizeof(sb), &sb) || sb.bmWidth <= 0 || sb.bmHeight <= 0) return;
    HDC screen = GetDC(nullptr);
    HDC ddc = CreateCompatibleDC(screen);
    HDC sdc = CreateCompatibleDC(screen);
    HGDIOBJ oldD = SelectObject(ddc, dest);
    HGDIOBJ oldS = SelectObject(sdc, src);
    BitBlt(ddc, x, y, sb.bmWidth, sb.bmHeight, sdc, 0, 0, SRCCOPY);
    SelectObject(ddc, oldD);
    SelectObject(sdc, oldS);
    DeleteDC(ddc);
    DeleteDC(sdc);
    ReleaseDC(nullptr, screen);
}

void CaseFrozenMatch() {
    // ① 纯色模板必须被**拒绝**：OpenCV 在零方差模板 + TM_CCOEFF_NORMED 时直接
    //    result=all(1)、平方差侧处处 0 —— 「处处满分」会报出 100% 的假匹配
    //    （旧用例就是靠这个假匹配通过的，所以这里改成断言拒绝）。
    HBITMAP screen = MakeSolidBmp(120, 80, RGB(240, 240, 240));
    if (!screen) {
        Emit(L"frozen_bitmap_find_template", false, L"CreateCompatibleBitmap screen failed");
        return;
    }
    PaintRect(screen, 40, 20, 24, 24, RGB(20, 20, 200));
    HBITMAP flatTpl = MakeSolidBmp(24, 24, RGB(20, 20, 200));
    if (!flatTpl) {
        DeleteBitmapHandle(screen);
        Emit(L"frozen_bitmap_find_template", false, L"CreateCompatibleBitmap tpl failed");
        return;
    }
    ImageMatchOptions opt{};
    opt.thresholdPercent = 70.0;
    opt.scaleMin = opt.scaleMax = 1.0;
    opt.disablePyramid = true;
    const ImageMatchOutput flatOut = FindTemplateInFrozenScreenMulti(
        screen, 0, 0, 0, 0, 120, 80, flatTpl, opt);
    DeleteBitmapHandle(flatTpl);
    const bool flatRejected = !flatOut.found && flatOut.matches.empty()
        && flatOut.reason.find(L"纯色") != std::wstring::npos;
    DeleteBitmapHandle(screen);

    // ② 有纹理的模板仍必须正确定位（不能因为加护栏把正常找图打死）
    HBITMAP screen2 = MakeSolidBmp(120, 80, RGB(240, 240, 240));
    PaintRect(screen2, 40, 20, 24, 24, RGB(250, 250, 250));
    PaintRect(screen2, 40, 20, 8, 8, RGB(200, 30, 30));
    PaintRect(screen2, 56, 36, 8, 8, RGB(30, 30, 200));
    PaintRect(screen2, 40, 36, 8, 8, RGB(20, 160, 60));
    HBITMAP tpl = MakeSolidBmp(24, 24, RGB(250, 250, 250));
    PaintRect(tpl, 0, 0, 8, 8, RGB(200, 30, 30));
    PaintRect(tpl, 16, 16, 8, 8, RGB(30, 30, 200));
    PaintRect(tpl, 0, 16, 8, 8, RGB(20, 160, 60));
    const ImageMatchOutput out = FindTemplateInFrozenScreenMulti(
        screen2, 0, 0, 0, 0, 120, 80, tpl, opt);
    DeleteBitmapHandle(tpl);
    DeleteBitmapHandle(screen2);

    const bool ok = out.found && !out.matches.empty() && out.matches[0].score >= 70.0;
    const bool posOk = ok
        && std::abs(out.matches[0].topLeftX - 40) <= 1
        && std::abs(out.matches[0].topLeftY - 20) <= 1;
    std::wstring detail;
    if (posOk && flatRejected) {
        detail = L"score=" + std::to_wstring(out.matches[0].score)
            + L" at " + std::to_wstring(out.matches[0].topLeftX)
            + L"," + std::to_wstring(out.matches[0].topLeftY)
            + L"; 纯色模板已拒（stdDev="
            + std::to_wstring(static_cast<int>(flatOut.debugTemplateStdDev + 0.5)) + L"）";
    } else {
        detail = L"纯色拒绝=" + std::to_wstring(flatRejected ? 1 : 0)
            + L" reason=" + flatOut.reason.substr(0, 60)
            + L" | found=" + std::to_wstring(out.found ? 1 : 0)
            + L" n=" + std::to_wstring(out.matches.size())
            + L" bestNcc=" + std::to_wstring(out.debugBestNccPercent);
        if (!out.matches.empty()) {
            detail += L" tl=" + std::to_wstring(out.matches[0].topLeftX)
                + L"," + std::to_wstring(out.matches[0].topLeftY)
                + L" score=" + std::to_wstring(out.matches[0].score);
        }
    }
    Emit(L"frozen_bitmap_find_template", posOk && flatRejected, detail.c_str());
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
            + L" pixelAgree=" + std::to_wstring(out.debugBestPixelAgreePercent)
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
            + L" pixelAgree=" + std::to_wstring(out.debugBestPixelAgreePercent)
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
            + L" pixelAgree=" + std::to_wstring(out.debugBestPixelAgreePercent)
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
            + L" pixelAgree=" + std::to_wstring(out.debugBestPixelAgreePercent)
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
            + L" pixelAgree=" + std::to_wstring(out.debugBestPixelAgreePercent)
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

void CaseSimilarButtonsOnlyTrueMatch() {
    // 桌面/IM 误匹配典型场景：一排几乎一样的灰底控件，只换了内部图标。
    // 旧路径会用 NCC 把 maxMatches=20 填满，并报 80%+。
    const int bw = 96;
    const int bh = 48;
    HBITMAP tpl = MakeSolidBmp(bw, bh, RGB(220, 222, 226));
    PaintRect(tpl, 8, 8, 12, 32, RGB(20, 20, 20));

    HBITMAP btnB = MakeSolidBmp(bw, bh, RGB(220, 222, 226));
    PaintRect(btnB, 76, 8, 12, 32, RGB(20, 20, 20));
    HBITMAP btnC = MakeSolidBmp(bw, bh, RGB(220, 222, 226));
    PaintRect(btnC, 24, 19, 48, 10, RGB(20, 20, 20));
    HBITMAP btnD = MakeSolidBmp(bw, bh, RGB(220, 222, 226));
    PaintRect(btnD, 38, 14, 20, 20, RGB(20, 20, 20));

    HBITMAP screen = MakeSolidBmp(640, 360, RGB(240, 240, 240));
    BlitBmp(screen, 40, 40, tpl);
    BlitBmp(screen, 200, 40, btnB);
    BlitBmp(screen, 40, 160, btnC);
    BlitBmp(screen, 360, 160, btnD);
    DeleteBitmapHandle(btnB);
    DeleteBitmapHandle(btnC);
    DeleteBitmapHandle(btnD);

    ImageMatchOptions opt{};
    opt.thresholdPercent = 65.0;
    opt.scaleMin = opt.scaleMax = 1.0;
    opt.disablePyramid = true;
    opt.maxMatches = 20;
    const ImageMatchOutput out = FindTemplateInFrozenScreenMulti(
        screen, 0, 0, 0, 0, 640, 360, tpl, opt);
    DeleteBitmapHandle(tpl);
    DeleteBitmapHandle(screen);

    bool ok = out.found && out.matches.size() == 1;
    if (ok) {
        const int dx = std::abs(out.matches[0].topLeftX - 40);
        const int dy = std::abs(out.matches[0].topLeftY - 40);
        ok = dx <= 4 && dy <= 4;
    }
    std::wstring detail = L"n=" + std::to_wstring(out.matches.size())
        + L" bestNcc=" + std::to_wstring(out.debugBestNccPercent)
        + L" pixelAgree=" + std::to_wstring(out.debugBestPixelAgreePercent);
    for (size_t i = 0; i < out.matches.size(); ++i) {
        detail += L" [" + std::to_wstring(static_cast<int>(i)) + L"]="
            + std::to_wstring(out.matches[i].topLeftX) + L","
            + std::to_wstring(out.matches[i].topLeftY)
            + L" s=" + std::to_wstring(static_cast<int>(out.matches[i].score + 0.5));
    }
    Emit(L"similar_buttons_only_true_match", ok, detail.c_str());
}

void CaseUnrelatedSceneRejected() {
    HBITMAP tpl = MakeSolidBmp(48, 36, RGB(200, 40, 40));
    PaintRect(tpl, 4, 4, 16, 28, RGB(40, 180, 220));
    PaintRect(tpl, 24, 8, 20, 8, RGB(240, 220, 40));

    HBITMAP screen = MakeSolidBmp(400, 280, RGB(48, 90, 48));
    PaintRect(screen, 20, 20, 70, 40, RGB(30, 60, 160));
    PaintRect(screen, 160, 80, 50, 90, RGB(180, 90, 30));
    PaintRect(screen, 280, 40, 80, 30, RGB(90, 90, 90));
    PaintRect(screen, 100, 180, 120, 50, RGB(220, 220, 220));

    ImageMatchOptions opt{};
    opt.thresholdPercent = 65.0;
    opt.scaleMin = opt.scaleMax = 1.0;
    opt.disablePyramid = true;
    opt.maxMatches = 20;
    const ImageMatchOutput out = FindTemplateInFrozenScreenMulti(
        screen, 0, 0, 0, 0, 400, 280, tpl, opt);
    DeleteBitmapHandle(tpl);
    DeleteBitmapHandle(screen);

    const bool ok = !out.found && out.matches.empty();
    std::wstring detail = L"n=" + std::to_wstring(out.matches.size())
        + L" bestNcc=" + std::to_wstring(out.debugBestNccPercent)
        + L" raw=" + std::to_wstring(out.debugRawCandidates);
    if (!out.matches.empty()) {
        detail += L" score=" + std::to_wstring(out.matches[0].score);
    }
    Emit(L"unrelated_scene_rejected", ok, detail.c_str());
}

void CaseRepeatedTemplateFindsAll() {
    // 同一图标在画面上出现两处：定位若在最佳命中周围爬峰，会把 maxMatches 用尽，只剩 1 框。
    const int tw = 40;
    const int th = 28;
    HBITMAP tpl = MakeSolidBmp(tw, th, RGB(30, 80, 200));
    PaintRect(tpl, 4, 4, 12, 20, RGB(240, 40, 40));
    PaintRect(tpl, 22, 8, 14, 8, RGB(40, 220, 80));

    HBITMAP screen = MakeSolidBmp(640, 240, RGB(245, 245, 245));
    PaintRect(screen, 40, 30, 80, 50, RGB(200, 180, 80));
    PaintRect(screen, 400, 120, 90, 40, RGB(80, 80, 90));
    BlitBmp(screen, 50, 40, tpl);
    BlitBmp(screen, 360, 90, tpl);

    ImageMatchOptions opt{};
    opt.thresholdPercent = 65.0;
    opt.scaleMin = opt.scaleMax = 1.0;
    opt.disablePyramid = true;
    opt.maxMatches = 20;
    const ImageMatchOutput out = FindTemplateInFrozenScreenMulti(
        screen, 0, 0, 0, 0, 640, 240, tpl, opt);
    DeleteBitmapHandle(tpl);
    DeleteBitmapHandle(screen);

    auto nearPos = [](const ImageMatchResult& m, int x, int y) {
        return std::abs(m.topLeftX - x) <= 3 && std::abs(m.topLeftY - y) <= 3;
    };
    bool hitA = false;
    bool hitB = false;
    for (const auto& m : out.matches) {
        if (nearPos(m, 50, 40)) hitA = true;
        if (nearPos(m, 360, 90)) hitB = true;
    }
    const bool ok = out.found && out.matches.size() == 2 && hitA && hitB;
    std::wstring detail = L"n=" + std::to_wstring(out.matches.size())
        + L" raw=" + std::to_wstring(out.debugRawCandidates)
        + L" bestNcc=" + std::to_wstring(out.debugBestNccPercent);
    for (size_t i = 0; i < out.matches.size(); ++i) {
        detail += L" [" + std::to_wstring(static_cast<int>(i)) + L"]="
            + std::to_wstring(out.matches[i].topLeftX) + L","
            + std::to_wstring(out.matches[i].topLeftY);
    }
    Emit(L"repeated_template_finds_all", ok, detail.c_str());
}

void CaseOffsetPickMaxMatchesOne() {
    const int tw = 40;
    const int th = 28;
    HBITMAP tpl = MakeSolidBmp(tw, th, RGB(30, 80, 200));
    PaintRect(tpl, 4, 4, 12, 20, RGB(240, 40, 40));
    PaintRect(tpl, 22, 8, 14, 8, RGB(40, 220, 80));

    HBITMAP screen = MakeSolidBmp(640, 240, RGB(245, 245, 245));
    BlitBmp(screen, 50, 40, tpl);
    BlitBmp(screen, 360, 90, tpl);

    ImageMatchOptions opt{};
    opt.thresholdPercent = 65.0;
    opt.scaleMin = opt.scaleMax = 1.0;
    opt.disablePyramid = true;
    opt.maxMatches = 1;
    const ImageMatchOutput out = FindTemplateInFrozenScreenMulti(
        screen, 0, 0, 0, 0, 640, 240, tpl, opt);
    DeleteBitmapHandle(tpl);
    DeleteBitmapHandle(screen);

    const bool ok = out.found && out.matches.size() == 1;
    std::wstring detail = L"n=" + std::to_wstring(out.matches.size())
        + L" raw=" + std::to_wstring(out.debugRawCandidates);
    if (!out.matches.empty()) {
        detail += L" tl=" + std::to_wstring(out.matches[0].topLeftX) + L","
            + std::to_wstring(out.matches[0].topLeftY);
    }
    Emit(L"offset_pick_max_matches_one", ok, detail.c_str());
}

void CaseNearestMatchOffsetNotFirst() {
    ImageMatchResult a{};
    ImageMatchResult b{};
    ImageMatchResult c{};
    a.found = b.found = c.found = true;
    a.topLeftX = 0; a.topLeftY = 0; a.bottomRightX = 10; a.bottomRightY = 10;
    b.topLeftX = 100; b.topLeftY = 0; b.bottomRightX = 110; b.bottomRightY = 10;
    c.topLeftX = 200; c.topLeftY = 0; c.bottomRightX = 210; c.bottomRightY = 10;
    const std::vector<ImageMatchResult> matches{a, b, c};
    const ImageMatchResult* n = FindNearestImageMatch(matches, 208, 6);
    int ox = 0, oy = 0;
    if (n) FindImageRelativeClickOffset(*n, 208, 6, ox, oy);
    const bool ok = n && n->topLeftX == 200 && ox == 3 && oy == 1;
    std::wstring detail = n
        ? (L"tl=" + std::to_wstring(n->topLeftX) + L" ox=" + std::to_wstring(ox)
            + L" oy=" + std::to_wstring(oy))
        : L"null";
    Emit(L"nearest_match_offset_not_first", ok, detail.c_str());
}

bool WritePngW(const std::wstring& path, const cv::Mat& img) {
    std::vector<uchar> buf;
    if (!cv::imencode(".png", img, buf) || buf.empty()) return false;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || !fp) return false;
    const size_t n = fwrite(buf.data(), 1, buf.size(), fp);
    fclose(fp);
    return n == buf.size();
}

cv::Vec4b AlphaSpritePixel(int tx, int ty) {
    return cv::Vec4b(
        static_cast<uint8_t>(16 + (tx * 7) % 200),
        static_cast<uint8_t>(40 + (ty * 11) % 180),
        static_cast<uint8_t>(220 - (tx + ty) % 160),
        255);
}

void CaseAlphaMaskIgnoresTransparent() {
    // 48x48 PNG：8px 透明边 + 32x32 彩色精灵。屏幕上另放一块同尺寸黑矩形当诱饵。
    // 旧路径把透明当黑色，会吸到黑块；带 mask 后应只命中精灵。
    const int pad = 8;
    const int sprite = 32;
    const int tplW = pad * 2 + sprite;
    const int tplH = pad * 2 + sprite;
    cv::Mat bgra(tplH, tplW, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    for (int y = 0; y < sprite; ++y) {
        for (int x = 0; x < sprite; ++x) {
            bgra.at<cv::Vec4b>(pad + y, pad + x) = AlphaSpritePixel(x, y);
        }
    }

    wchar_t tmpDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmpDir);
    const std::wstring path = std::wstring(tmpDir) + L"__img_match_alpha.png";
    DeleteFileW(path.c_str());
    if (!WritePngW(path, bgra)) {
        Emit(L"alpha_mask_ignores_transparent", false, L"写 PNG 失败");
        return;
    }
    HBITMAP tpl = LoadBitmapFromFile(path);
    DeleteFileW(path.c_str());
    if (!tpl) {
        Emit(L"alpha_mask_ignores_transparent", false, L"加载透明 PNG 失败");
        return;
    }

    const int scrW = 240;
    const int scrH = 160;
    const int spriteX = 90;
    const int spriteY = 50;
    HBITMAP screen = MakeSolidBmp(scrW, scrH, RGB(180, 210, 170));
    PaintRect(screen, 12, 16, tplW, tplH, RGB(0, 0, 0));
    {
        HDC dc = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, screen);
        for (int y = 0; y < sprite; ++y) {
            for (int x = 0; x < sprite; ++x) {
                const cv::Vec4b p = AlphaSpritePixel(x, y);
                SetPixel(mem, spriteX + x, spriteY + y, RGB(p[2], p[1], p[0]));
            }
        }
        SelectObject(mem, old);
        DeleteDC(mem);
        ReleaseDC(nullptr, dc);
    }

    ImageMatchOptions opt{};
    opt.thresholdPercent = 65.0;
    opt.scaleMin = opt.scaleMax = 1.0;
    opt.disablePyramid = true;
    opt.maxMatches = 20;
    const ImageMatchOutput out = FindTemplateInFrozenScreenMulti(
        screen, 0, 0, 0, 0, scrW, scrH, tpl, opt);
    DeleteBitmapHandle(tpl);
    DeleteBitmapHandle(screen);

    const int expectX = spriteX - pad;
    const int expectY = spriteY - pad;
    bool ok = out.found && out.matches.size() == 1;
    if (ok) {
        const int dx = std::abs(out.matches[0].topLeftX - expectX);
        const int dy = std::abs(out.matches[0].topLeftY - expectY);
        ok = dx <= 2 && dy <= 2;
    }
    std::wstring detail = L"n=" + std::to_wstring(out.matches.size())
        + L" expect=" + std::to_wstring(expectX) + L"," + std::to_wstring(expectY)
        + L" bestNcc=" + std::to_wstring(out.debugBestNccPercent)
        + L" pixelAgree=" + std::to_wstring(out.debugBestPixelAgreePercent);
    if (!out.matches.empty()) {
        detail += L" tl=" + std::to_wstring(out.matches[0].topLeftX) + L","
            + std::to_wstring(out.matches[0].topLeftY)
            + L" s=" + std::to_wstring(out.matches[0].score);
    }
    Emit(L"alpha_mask_ignores_transparent", ok, detail.c_str());
}

void CaseAlphaPaddedNearEdge() {
    const int pad = 8;
    const int sprite = 32;
    const int tplW = pad * 2 + sprite;
    const int tplH = pad * 2 + sprite;
    cv::Mat bgra(tplH, tplW, CV_8UC4, cv::Scalar(0, 0, 0, 0));
    for (int y = 0; y < sprite; ++y) {
        for (int x = 0; x < sprite; ++x) {
            bgra.at<cv::Vec4b>(pad + y, pad + x) = AlphaSpritePixel(x, y);
        }
    }

    wchar_t tmpDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmpDir);
    const std::wstring path = std::wstring(tmpDir) + L"__img_match_alpha_edge.png";
    DeleteFileW(path.c_str());
    if (!WritePngW(path, bgra)) {
        Emit(L"alpha_padded_near_edge", false, L"写 PNG 失败");
        return;
    }
    HBITMAP tpl = LoadBitmapFromFile(path);
    DeleteFileW(path.c_str());
    if (!tpl) {
        Emit(L"alpha_padded_near_edge", false, L"加载透明 PNG 失败");
        return;
    }

    const int scrW = 120;
    const int scrH = 80;
    const int spriteX = 0;
    const int spriteY = 20;
    HBITMAP screen = MakeSolidBmp(scrW, scrH, RGB(180, 210, 170));
    {
        HDC dc = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, screen);
        for (int y = 0; y < sprite; ++y) {
            for (int x = 0; x < sprite; ++x) {
                const cv::Vec4b p = AlphaSpritePixel(x, y);
                SetPixel(mem, spriteX + x, spriteY + y, RGB(p[2], p[1], p[0]));
            }
        }
        SelectObject(mem, old);
        DeleteDC(mem);
        ReleaseDC(nullptr, dc);
    }

    ImageMatchOptions opt{};
    opt.thresholdPercent = 65.0;
    opt.scaleMin = opt.scaleMax = 1.0;
    opt.disablePyramid = true;
    opt.maxMatches = 1;
    const ImageMatchOutput out = FindTemplateInFrozenScreenMulti(
        screen, 0, 0, 0, 0, scrW, scrH, tpl, opt);
    DeleteBitmapHandle(tpl);
    DeleteBitmapHandle(screen);

    const int expectX = spriteX - pad;
    const int expectY = spriteY - pad;
    bool ok = out.found && out.matches.size() == 1;
    if (ok) {
        const int dx = std::abs(out.matches[0].topLeftX - expectX);
        const int dy = std::abs(out.matches[0].topLeftY - expectY);
        ok = dx <= 2 && dy <= 2;
    }
    std::wstring detail = L"n=" + std::to_wstring(out.matches.size())
        + L" expect=" + std::to_wstring(expectX) + L"," + std::to_wstring(expectY);
    if (!out.matches.empty()) {
        detail += L" tl=" + std::to_wstring(out.matches[0].topLeftX) + L","
            + std::to_wstring(out.matches[0].topLeftY);
    }
    Emit(L"alpha_padded_near_edge", ok, detail.c_str());
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

void CaseVarTemplateMissCenter() {
    const ImageMatchResult m = SynthesizeSearchRectCenterMatch(0, 0, 200, 100, 40, 20);
    int cx = 0, cy = 0;
    FindImageMatchCenter(m, cx, cy);
    TemplateScale ts{};
    ts.sx = 1.0;
    ts.sy = 1.0;
    int tx = 0, ty = 0;
    ResolveFindImageClickPoint(m, 40, 20, 0.25, 0.0, ts, false, tx, ty);
    const bool empty = !SynthesizeSearchRectCenterMatch(10, 10, 10, 20, 8, 8).found;
    const bool ok = m.found && cx == 100 && cy == 50 && tx == 110 && ty == 50 && empty;
    Emit(L"var_offset_anchor_screen_center", ok,
        (L"found=" + std::to_wstring(m.found ? 1 : 0)
            + L" cx=" + std::to_wstring(cx) + L" cy=" + std::to_wstring(cy)
            + L" tx=" + std::to_wstring(tx) + L" ty=" + std::to_wstring(ty)
            + L" empty=" + std::to_wstring(empty ? 1 : 0)).c_str());
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

void CaseFindPeaksSqdiffTwoMinima() {
    cv::Mat result(48, 160, CV_32F, cv::Scalar(0.85));
    result.at<float>(10, 12) = 0.02f;
    result.at<float>(10, 120) = 0.03f;
    const auto peaks = image_match_internal::FindPeaks(result, 0.5, 20, 16, 0.15, 10, true);
    bool hitA = false;
    bool hitB = false;
    for (const auto& p : peaks) {
        if (std::abs(p.first.x - 12) <= 1 && std::abs(p.first.y - 10) <= 1) hitA = true;
        if (std::abs(p.first.x - 120) <= 1 && std::abs(p.first.y - 10) <= 1) hitB = true;
    }
    const bool ok = peaks.size() == 2 && hitA && hitB;
    std::wstring detail = L"n=" + std::to_wstring(peaks.size());
    for (const auto& p : peaks) {
        detail += L" " + std::to_wstring(p.first.x) + L"," + std::to_wstring(p.first.y);
    }
    Emit(L"findpeaks_sqdiff_keeps_two_minima", ok, detail.c_str());
}

void CaseFindImageFastPathGate() {
    // 「上一帧命中」本地复核的判据是纯函数，这里穷举守卫。核心承诺：
    //   ① 只有「同一请求 + 上次全屏很慢 + 命中新鲜」才规划窗口
    //   ② 窗口必须完整包住整个漂移带（否则宁可回退全屏，也不给出「找不到」）
    //   ③ 命中必须同实例（漂移 ≤8px）且分数比阈值高 3 个点
    const FindImageFastPathParams p{};
    bool ok = true;
    std::wstring detail;

    auto plan = [&](int rx1, int ry1, int rx2, int ry2, int px, int py, int tw, int th,
                    double ms, long long age, int& wx1, int& wy1, int& wx2, int& wy2) {
        return PlanFindImageFastPath(p, rx1, ry1, rx2, ry2, px, py, tw, th, ms, age,
            wx1, wy1, wx2, wy2);
    };

    int wx1 = 0, wy1 = 0, wx2 = 0, wy2 = 0;
    // ① 正常全屏：命中 (1000,700)，窗口应远小于搜索区且包住漂移带
    const bool legit = plan(0, 0, 2560, 1440, 1000, 700, 96, 96, 60.0, 100, wx1, wy1, wx2, wy2);
    const int legitWx1 = wx1;
    const int legitWy1 = wy1;
    const int legitWx2 = wx2;
    const int legitWy2 = wy2;
    const bool legitBand = legit
        && wx1 <= 1000 - p.maxDriftPx && wx2 - 96 >= 1000 + p.maxDriftPx
        && wy1 <= 700 - p.maxDriftPx && wy2 - 96 >= 700 + p.maxDriftPx;
    const bool legitSmall = legit
        && static_cast<long long>(wx2 - wx1) * (wy2 - wy1) * 4
            < static_cast<long long>(2560) * 1440;
    ok = ok && legit && legitBand && legitSmall;
    if (!(legit && legitBand && legitSmall)) detail += L" legit";

    // ② 上次搜索本来很快（区域找图）→ 不规划
    ok = ok && !plan(0, 0, 2560, 1440, 1000, 700, 96, 96, 3.0, 100, wx1, wy1, wx2, wy2);
    // ③ 上一帧太旧 → 不规划
    ok = ok && !plan(0, 0, 2560, 1440, 1000, 700, 96, 96, 60.0, 5000, wx1, wy1, wx2, wy2);
    // ④ 命中贴搜索区左/上边 → 漂移带放不进窗口 → 不规划
    ok = ok && !plan(0, 0, 2560, 1440, 0, 700, 96, 96, 60.0, 100, wx1, wy1, wx2, wy2);
    ok = ok && !plan(0, 0, 2560, 1440, 1000, 0, 96, 96, 60.0, 100, wx1, wy1, wx2, wy2);
    // ⑤ 命中贴右/下边（模板右下角正好顶到搜索区边界）→ 不规划
    ok = ok && !plan(0, 0, 2560, 1440, 2560 - 96, 700, 96, 96, 60.0, 100, wx1, wy1, wx2, wy2);
    ok = ok && !plan(0, 0, 2560, 1440, 1000, 1440 - 96, 96, 96, 60.0, 100, wx1, wy1, wx2, wy2);
    // ⑥ 搜索区本身就只比模板大一点（窗口≈搜索区）→ 没收益 → 不规划
    ok = ok && !plan(1000, 700, 1000 + 96 + 20, 700 + 96 + 20, 1000, 700, 96, 96,
        60.0, 100, wx1, wy1, wx2, wy2);
    // ⑦ 模板尺寸非法 → 不规划
    ok = ok && !plan(0, 0, 2560, 1440, 1000, 700, 0, 96, 60.0, 100, wx1, wy1, wx2, wy2);

    // ⑧ 命中判据：同实例 + 分数留余量
    const bool acc0 = AcceptFindImageFastPathHit(p, 1000, 700, 1000, 700, 65.0, 99.0);
    const bool accDriftOk = AcceptFindImageFastPathHit(p, 1000, 700, 1008, 692, 65.0, 99.0);
    const bool accDriftBad = AcceptFindImageFastPathHit(p, 1000, 700, 1009, 700, 65.0, 99.0);
    const bool accScoreEdge = AcceptFindImageFastPathHit(p, 1000, 700, 1000, 700, 65.0, 66.9);
    const bool accScoreOk = AcceptFindImageFastPathHit(p, 1000, 700, 1000, 700, 65.0, 68.0);
    ok = ok && acc0 && accDriftOk && !accDriftBad && !accScoreEdge && accScoreOk;
    if (!(acc0 && accDriftOk && !accDriftBad && !accScoreEdge && accScoreOk)) detail += L" accept";

    // ⑨ 目标动了：同一窗口内找不到上一帧实例 → 规划仍为真（会去搜），
    //    但用旧坐标之外的命中做判据必须被拒（否则会点到别的实例）
    const bool movedRejected = !AcceptFindImageFastPathHit(p, 1000, 700, 1120, 700, 65.0, 99.0);
    ok = ok && movedRejected;
    detail += L" 正常窗口=" + std::to_wstring(legitWx1) + L"," + std::to_wstring(legitWy1) + L"-"
        + std::to_wstring(legitWx2) + L"," + std::to_wstring(legitWy2);
    Emit(L"find_image_fastpath_gate", ok, detail.c_str());
}

void CaseGpuAccelSameResultAndGate() {
    // 走**公开入口**验证两件事：
    //   1) 面积 < 500k 像素（区域找图）时 GPU 开关不生效 —— 小区域不该被传输开销拖慢
    //   2) 大区域开 GPU 后，命中位置/分数与 CPU 一致（精度不降）
    // 无 OpenCL 设备时直接算过（本机/客户机都可能没有）。
    const std::wstring dev = FindImageGpuDeviceName();
    if (dev.empty()) {
        Emit(L"gpu_accel_same_result_and_area_gate", true, L"无 OpenCL 设备 → GPU 路径不可用（已自动回落 CPU）");
        return;
    }
    constexpr int kBigW = 1400;
    constexpr int kBigH = 900;   // 1.26M 像素 > 门槛
    constexpr int kTpl = 64;
    constexpr int kTX = 511;
    constexpr int kTY = 333;
    HBITMAP big = MakeTexturedBmp(kBigW, kBigH, 20260917u);
    HBITMAP tpl = MakeSolidBmp(kTpl, kTpl, RGB(128, 128, 128));
    bool ok = big && tpl;
    if (ok) {
        HDC sdc = GetDC(nullptr);
        HDC src = CreateCompatibleDC(sdc);
        HDC dst = CreateCompatibleDC(sdc);
        HGDIOBJ o1 = SelectObject(src, big);
        HGDIOBJ o2 = SelectObject(dst, tpl);
        BitBlt(dst, 0, 0, kTpl, kTpl, src, kTX, kTY, SRCCOPY);
        SelectObject(src, o1);
        SelectObject(dst, o2);
        DeleteDC(src);
        DeleteDC(dst);
        ReleaseDC(nullptr, sdc);
    }

    auto run = [&](bool gpu) -> ImageMatchOutput {
        SetFindImageGpuAccel(gpu);
        ImageMatchOptions opt;
        opt.thresholdPercent = 65.0;
        opt.scaleMin = opt.scaleMax = 1.0;
        opt.scaleStep = 1.0;
        opt.maxMatches = 1;
        return FindTemplateInFrozenScreenMulti(big, 0, 0, 0, 0, kBigW, kBigH, tpl, opt);
    };
    ImageMatchOutput cpuOut{};
    ImageMatchOutput gpuOut{};
    if (ok) {
        cpuOut = run(false);
        gpuOut = run(true);
        // 小区域：GPU 开关必须“不生效”，结果当然要一致
        ImageMatchOptions smallOpt;
        smallOpt.thresholdPercent = 65.0;
        smallOpt.scaleMin = smallOpt.scaleMax = 1.0;
        smallOpt.scaleStep = 1.0;
        smallOpt.maxMatches = 1;
        SetFindImageGpuAccel(true);
        const ImageMatchOutput smallGpu = FindTemplateInFrozenScreenMulti(
            big, 0, 0, kTX - 40, kTY - 40, kTX + kTpl + 40, kTY + kTpl + 40, tpl, smallOpt);
        SetFindImageGpuAccel(false);
        const bool smallOk = smallGpu.found && !smallGpu.matches.empty()
            && std::abs(smallGpu.matches[0].topLeftX - kTX) <= 1
            && std::abs(smallGpu.matches[0].topLeftY - kTY) <= 1;
        const bool cpuOk = cpuOut.found && !cpuOut.matches.empty()
            && std::abs(cpuOut.matches[0].topLeftX - kTX) <= 1
            && std::abs(cpuOut.matches[0].topLeftY - kTY) <= 1;
        const bool gpuOk = gpuOut.found && !gpuOut.matches.empty()
            && std::abs(gpuOut.matches[0].topLeftX - kTX) <= 1
            && std::abs(gpuOut.matches[0].topLeftY - kTY) <= 1;
        const double cpuScore = cpuOut.matches.empty() ? -1.0 : cpuOut.matches[0].score;
        const double gpuScore = gpuOut.matches.empty() ? -1.0 : gpuOut.matches[0].score;
        const bool scoreOk = cpuScore > 0.0 && std::abs(cpuScore - gpuScore) <= 1.0;
        ok = cpuOk && gpuOk && scoreOk && smallOk;
        std::wstring detail = L"设备=" + dev
            + L" CPU分=" + std::to_wstring(static_cast<int>(cpuScore))
            + L" GPU分=" + std::to_wstring(static_cast<int>(gpuScore))
            + L" 小区域=" + std::to_wstring(smallOk ? 1 : 0);
        DeleteBitmapHandle(tpl);
        DeleteBitmapHandle(big);
        Emit(L"gpu_accel_same_result_and_area_gate", ok, detail.c_str());
        return;
    }
    SetFindImageGpuAccel(false);
    if (tpl) DeleteBitmapHandle(tpl);
    if (big) DeleteBitmapHandle(big);
    Emit(L"gpu_accel_same_result_and_area_gate", false, L"合成位图失败");
}

void CaseOpenClMatchTemplateBench() {
    // 只做**测量**，不绑定结论：iGPU（i7-7700 的 HD 630）跑 matchTemplate 到底比 CPU 快还是慢。
    // 通过条件与「谁更快」无关 —— 有 OpenCL 时只要求结果位置与 CPU 一致；
    // 没有 OpenCL 时直接算过（本机装了 opencv_world 也完全可能不带 OpenCL）。
    const bool haveOcl = cv::ocl::haveOpenCL();
    std::string devName;
    size_t devCount = 0;
    if (haveOcl) {
        try {
            cv::ocl::Context& ctx = cv::ocl::Context::getDefault();
            devCount = ctx.ndevices();
            if (devCount > 0) devName = cv::ocl::Device::getDefault().name();
        } catch (const cv::Exception&) {
            devCount = 0;
        }
    }
    if (!haveOcl || devCount == 0) {
        Emit(L"opencl_matchtemplate_bench", true,
            (std::wstring(L"无可用 OpenCL 设备（haveOpenCL=")
                + (haveOcl ? L"1" : L"0") + L", devices=" + std::to_wstring(devCount)
                + L"）→ 不启用 GPU 路径")
                .c_str());
        return;
    }

    // 合成确定性伪随机灰图（够纹理）
    auto makeSrc = [](int w, int h) {
        cv::Mat m(h, w, CV_8UC1);
        unsigned int r = 0x51ED2701u;
        for (int y = 0; y < h; ++y) {
            uint8_t* row = m.ptr<uint8_t>(y);
            for (int x = 0; x < w; ++x) {
                r = r * 1664525u + 1013904223u;
                row[x] = static_cast<uint8_t>((r >> 17) & 0xFF);
            }
        }
        return m;
    };

    auto bench = [](const std::function<cv::Point()>& run, int iters) -> double {
        cv::Point p;
        for (int i = 0; i < 2; ++i) p = run();          // 预热（含 OpenCL 首次 kernel 编译）
        LARGE_INTEGER f{}, t0{}, t1{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < iters; ++i) p = run();
        QueryPerformanceCounter(&t1);
        return (t1.QuadPart - t0.QuadPart) * 1000.0 / (static_cast<double>(f.QuadPart) * iters);
    };

    int prevThreads = cv::getNumThreads();
    std::wstring detail = L"设备=" + std::wstring(devName.begin(), devName.end());
    bool allLocOk = true;
    // 三个面积档：区域找图(~480x360) / 半屏(1280x720) / 全屏(2560x1440)。
    // 「小模板+传输开销反而更慢」这条经验必须用数据定门槛。
    struct Case { int w, h, t; };
    const Case cases[] = {{480, 360, 48}, {1280, 720, 96}, {2560, 1440, 96}};
    for (const Case& c : cases) {
        cv::Mat src = makeSrc(c.w, c.h);
        const int tx = c.w / 3;
        const int ty = c.h / 2;
        cv::Mat tpl = src(cv::Rect(tx, ty, c.t, c.t)).clone();

        cv::setNumThreads(prevThreads);
        const double cpuMs = bench([&] {
            cv::Mat r;
            cv::matchTemplate(src, tpl, r, cv::TM_SQDIFF_NORMED);
            double mn = 0.0;
            cv::Point loc;
            cv::minMaxLoc(r, &mn, nullptr, &loc, nullptr);
            return loc;
        }, 8);

        // 我们真实的 locate 需要**整张相似度图落在 CPU**（FindPeaks / 多峰 / 阈值筛选），
        // 所以按「上传 + GPU 算 + 回传」的最坏情况量。
        cv::ocl::setUseOpenCL(true);
        cv::Point gpuLoc(-1, -1);
        const double gpuMs = bench([&] {
            cv::UMat usrc;
            cv::UMat utpl;
            cv::UMat ur;
            src.copyTo(usrc);
            tpl.copyTo(utpl);
            cv::matchTemplate(usrc, utpl, ur, cv::TM_SQDIFF_NORMED);
            cv::Mat r;
            ur.copyTo(r);
            double mn = 0.0;
            cv::Point loc;
            cv::minMaxLoc(r, &mn, nullptr, &loc, nullptr);
            return loc;
        }, 8);
        {
            cv::UMat usrc;
            cv::UMat utpl;
            cv::UMat ur;
            src.copyTo(usrc);
            tpl.copyTo(utpl);
            cv::matchTemplate(usrc, utpl, ur, cv::TM_SQDIFF_NORMED);
            cv::Mat r;
            ur.copyTo(r);
            double mn = 0.0;
            cv::Point loc;
            cv::minMaxLoc(r, &mn, nullptr, &loc, nullptr);
            gpuLoc = loc;
        }
        cv::ocl::setUseOpenCL(false);

        const bool locOk = gpuLoc.x == tx && gpuLoc.y == ty;
        allLocOk = allLocOk && locOk;
        detail += L" | " + std::to_wstring(c.w) + L"x" + std::to_wstring(c.h)
            + L"@tpl" + std::to_wstring(c.t)
            + L" CPU=" + std::to_wstring(static_cast<int>(cpuMs)) + L"ms"
            + L" GPU=" + std::to_wstring(static_cast<int>(gpuMs)) + L"ms"
            + L" 位置一致=" + std::to_wstring(locOk ? 1 : 0);
    }
    SyncImageMatchThreadBudget();   // 恢复本进程线程预算（前面 setNumThreads 动过）
    Emit(L"opencl_matchtemplate_bench", allLocOk, detail.c_str());
}

void CaseTemplateBitmapCache() {
    // 1) 同路径连续加载：第二次必须命中缓存；2) 文件被改写 + 时间戳变化：
    //    必须重新解码（绝不能继续用旧像素，否则用户换模板后脚本还在按老图找）
    wchar_t tmpDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmpDir);
    const std::wstring path = std::wstring(tmpDir) + L"__img_match_tpl_cache.bmp";
    DeleteFileW(path.c_str());

    HBITMAP red = MakeSolidBmp(32, 32, RGB(220, 30, 30));
    HBITMAP blue = MakeSolidBmp(32, 32, RGB(20, 40, 220));
    bool saved = red && SaveBitmapToFile(red, path) && blue;
    if (!saved) {
        if (red) DeleteBitmapHandle(red);
        if (blue) DeleteBitmapHandle(blue);
        Emit(L"template_bitmap_cache_reuse_and_invalidate", false, L"准备位图/写文件失败");
        return;
    }

    ClearTemplateImageCache();
    const uint64_t lookups0 = TemplateImageCacheLookups();
    const uint64_t hits0 = TemplateImageCacheHits();

    auto centerPixel = [](HBITMAP bmp) -> COLORREF {
        if (!bmp) return CLR_INVALID;
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(mem, bmp);
        const COLORREF c = GetPixel(mem, 16, 16);
        SelectObject(mem, old);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return c;
    };

    auto loadPixel = [&](long long* usOut) -> COLORREF {
        LARGE_INTEGER f{}, t0{}, t1{};
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        HBITMAP bmp = LoadBitmapFromFile(path);
        QueryPerformanceCounter(&t1);
        if (usOut) {
            *usOut = (t1.QuadPart - t0.QuadPart) * 1000000LL / (f.QuadPart ? f.QuadPart : 1);
        }
        const COLORREF c = centerPixel(bmp);
        DeleteBitmapHandle(bmp);
        return c;
    };

    long long coldUs = 0;
    long long warmUs = 0;
    const COLORREF first = loadPixel(&coldUs);
    const COLORREF second = loadPixel(&warmUs);
    const COLORREF third = loadPixel(nullptr);
    const uint64_t hits = TemplateImageCacheHits() - hits0;
    const uint64_t lookups = TemplateImageCacheLookups() - lookups0;

    // 改写：换内容 + 换尺寸（大小必然变），并显式推进修改时间
    const bool rewritten = SaveBitmapToFile(blue, path);
    if (rewritten) {
        HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            FILETIME ft{};
            GetSystemTimeAsFileTime(&ft);
            ULARGE_INTEGER uli{};
            uli.LowPart = ft.dwLowDateTime;
            uli.HighPart = ft.dwHighDateTime;
            uli.QuadPart += 10000000ULL;  // +1s，绕开文件时间戳粒度
            ft.dwLowDateTime = uli.LowPart;
            ft.dwHighDateTime = uli.HighPart;
            SetFileTime(h, nullptr, nullptr, &ft);
            CloseHandle(h);
        }
    }
    const COLORREF after = rewritten ? loadPixel(nullptr) : CLR_INVALID;

    DeleteBitmapHandle(red);
    DeleteBitmapHandle(blue);
    const bool firstOk = first != CLR_INVALID && GetRValue(first) > 150 && GetBValue(first) < 100;
    const bool secondOk = second == first;
    const bool thirdOk = third == first;
    const bool cachedOk = hits >= 2 && lookups >= 3;
    const bool invalidated = GetBValue(after) > 150 && GetRValue(after) < 100;
    DeleteFileW(path.c_str());

    const bool ok = firstOk && secondOk && thirdOk && cachedOk && invalidated;
    Emit(L"template_bitmap_cache_reuse_and_invalidate", ok,
        (L"冷=" + std::to_wstring(coldUs) + L"us 热=" + std::to_wstring(warmUs)
            + L"us 命中=" + std::to_wstring(hits) + L"/" + std::to_wstring(lookups)
            + L" 换图后生效=" + std::to_wstring(invalidated ? 1 : 0)
            + (ok ? L"" : L" 像素/缓存断言未过")).c_str());
}

void CaseLowPowerLimitsCvThreads() {
    // 基线（低性能模式关闭）
    SetLowPerformanceMode(false);
    SyncImageMatchThreadBudget();
    const int baseline = cv::getNumThreads();
    // 打开：预算必须收到 1（matchTemplate/DFT 不再按核数 fan-out）
    SetLowPerformanceMode(true);
    SyncImageMatchThreadBudget();
    const int low = cv::getNumThreads();
    // 关闭：必须回到基线
    SetLowPerformanceMode(false);
    SyncImageMatchThreadBudget();
    const int restored = cv::getNumThreads();
    const bool ok = low == 1 && baseline >= 1 && restored == baseline;
    Emit(L"low_power_limits_cv_threads", ok,
        (L"基线=" + std::to_wstring(baseline) + L" 低性能=" + std::to_wstring(low)
            + L" 恢复=" + std::to_wstring(restored)).c_str());
}

}  // namespace

void CaseFullscreenLocatePerf() {
    constexpr int kW = 2560;
    constexpr int kH = 1440;
    constexpr int kTpl = 96;
    constexpr int kTargetX = 1731;
    constexpr int kTargetY = 902;
    HBITMAP screen = MakeTexturedBmp(kW, kH, 20260916u);
    if (!screen) {
        Emit(L"fullscreen_locate_perf_and_accuracy", false, L"合成屏幕失败");
        return;
    }
    HBITMAP tpl = MakeSolidBmp(kTpl, kTpl, RGB(128, 128, 128));
    {
        HDC sdc = GetDC(nullptr);
        HDC src = CreateCompatibleDC(sdc);
        HDC dst = CreateCompatibleDC(sdc);
        HGDIOBJ o1 = SelectObject(src, screen);
        HGDIOBJ o2 = SelectObject(dst, tpl);
        BitBlt(dst, 0, 0, kTpl, kTpl, src, kTargetX, kTargetY, SRCCOPY);
        SelectObject(src, o1);
        SelectObject(dst, o2);
        DeleteDC(src);
        DeleteDC(dst);
        ReleaseDC(nullptr, sdc);
    }
    auto measure = [&](int rescueDownscale, ImageMatchOutput* out) {
        ImageMatchOptions opt;
        opt.thresholdPercent = 65.0;
        opt.scaleMin = opt.scaleMax = 1.0;
        opt.scaleStep = 1.0;
        opt.maxMatches = 1;
        opt.rescueMaxDownscale = rescueDownscale;   // 1 = 旧行为（救援也全分辨率）
        const auto t0 = std::chrono::steady_clock::now();
        *out = FindTemplateInFrozenScreenMulti(screen, 0, 0, 0, 0, kW, kH, tpl, opt);
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
    };
    ImageMatchOutput fast;
    const long long fastMs = measure(4, &fast);
    ImageMatchOutput slow;
    const long long slowMs = measure(1, &slow);
    DeleteBitmapHandle(tpl);
    DeleteBitmapHandle(screen);

    auto atTarget = [&](const ImageMatchOutput& o) {
        if (o.matches.empty()) return false;
        return std::abs(o.matches[0].topLeftX - kTargetX) <= 1
            && std::abs(o.matches[0].topLeftY - kTargetY) <= 1;
    };
    const bool fastOk = fast.found && atTarget(fast);
    const bool slowOk = slow.found && atTarget(slow);
    const bool ok = fastOk && slowOk && fastMs <= slowMs;
    Emit(L"fullscreen_locate_perf_and_accuracy", ok,
        (L"降采样救援=" + std::to_wstring(fastMs) + L"ms 全分辨率=" + std::to_wstring(slowMs)
            + L"ms 省=" + std::to_wstring(slowMs > 0 ? (100 - fastMs * 100 / slowMs) : 0)
            + L"% 位置准=" + std::to_wstring(fastOk && slowOk ? 1 : 0)).c_str());
}

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
    CaseVarTemplateMissCenter();
    CasePyramid();
    CaseScoreSqdiff();
    CaseThreshold01();
    CaseNmsOverlap();
    CaseNmsMax();
    CaseFindPeaksSqdiffTwoMinima();
    CaseFrozenMatch();
    CaseIdenticalFullscreenTemplate();
    CaseRealScreenIdenticalTemplate();
    CaseRealScreenFileRoundtripTemplate();
    CaseRealTemplateOnSyntheticScreen();
    CaseRealTemplateWithLiveDelta();
    CaseFlatTemplateFindsOnTexturedScreen();
    CaseSimilarButtonsOnlyTrueMatch();
    CaseUnrelatedSceneRejected();
    CaseRepeatedTemplateFindsAll();
    CaseOffsetPickMaxMatchesOne();
    CaseNearestMatchOffsetNotFirst();
    CaseAlphaMaskIgnoresTransparent();
    CaseAlphaPaddedNearEdge();
    CasePerfectIdentical();
    CasePerfectRejectDelta2();
    CasePerfectAllowDelta1();
    CaseScreenDiffIdentical();
    CaseScreenDiffFindsRoi();
    CaseBusyMaskPersistentMotion();
    CaseDiffIgnoresBusyDynamicOnly();
    CaseFullscreenLocatePerf();
    CaseOpenClMatchTemplateBench();
    CaseFindImageFastPathGate();
    CaseGpuAccelSameResultAndGate();
    CaseTemplateBitmapCache();
    CaseLowPowerLimitsCvThreads();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
