// image_match_engines.cpp — Template matching with pixel-tolerance accept
//
// Locate: TM_SQDIFF_NORMED (plus an unnormalized SQDIFF global-min seed).
// Extra CCORR/CCOEFF locate passes were expensive and unused once pixel-agree
// became the accept gate.
// Accept: pixel-tolerance agree (ImageSearchDLL / AHK ImageSearch) with
// edge-weighted pixels so similar UI chrome + different inner icon is rejected.
// Transparent PNG pixels (tplMask) are ignored in verification; locate fills
// holes with the opaque-region mean so NCC is not attracted to black padding.

#include "image_match_engines.h"

#include "image_match_internal.h"
#include "low_power_mode.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <cmath>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <emmintrin.h>
#define IMAGE_MATCH_HAS_SSE2 1
#endif

namespace {

using namespace image_match_internal;

constexpr double kEdgeBoostPercent = 8.0;
constexpr double kColorBoostPercent = 6.0;

cv::Mat FillTransparentWithOpaqueMean(const cv::Mat& gray, const cv::Mat& mask) {
    if (gray.empty() || mask.empty() || mask.size() != gray.size()) return gray;
    const cv::Scalar m = cv::mean(gray, mask);
    cv::Mat out = gray.clone();
    cv::Mat transparent;
    cv::compare(mask, 0, transparent, cv::CMP_EQ);
    const int fill = std::clamp(static_cast<int>(std::lround(m[0])), 0, 255);
    out.setTo(cv::Scalar(fill), transparent);
    return out;
}

cv::Rect OpaqueContentRect(const cv::Mat& mask) {
    if (mask.empty()) return {};
    const cv::Rect box = cv::boundingRect(mask);
    if (box.width < 4 || box.height < 4) return {};
    return box;
}

/// 透明边可以伸出搜索区；不透明内容必须整块在画面内，否则验收为 0。
struct AlignedPatch {
    cv::Rect srcRc;
    cv::Rect tplRc;
    bool valid = false;
};

AlignedPatch AlignOpaquePatch(int srcW, int srcH, int tplW, int tplH,
                              int topLeftX, int topLeftY, const cv::Mat& opaqueMask) {
    AlignedPatch out;
    if (srcW <= 0 || srcH <= 0 || tplW <= 0 || tplH <= 0) return out;
    if (topLeftX >= 0 && topLeftY >= 0 && topLeftX + tplW <= srcW && topLeftY + tplH <= srcH) {
        out.srcRc = cv::Rect(topLeftX, topLeftY, tplW, tplH);
        out.tplRc = cv::Rect(0, 0, tplW, tplH);
        out.valid = true;
        return out;
    }
    const bool hasMask = !opaqueMask.empty() && opaqueMask.cols == tplW && opaqueMask.rows == tplH
        && opaqueMask.type() == CV_8UC1;
    if (!hasMask) return out;
    const int sx0 = std::max(0, topLeftX);
    const int sy0 = std::max(0, topLeftY);
    const int sx1 = std::min(srcW, topLeftX + tplW);
    const int sy1 = std::min(srcH, topLeftY + tplH);
    if (sx1 - sx0 < 4 || sy1 - sy0 < 4) return out;
    const int tx = sx0 - topLeftX;
    const int ty = sy0 - topLeftY;
    const int tw = sx1 - sx0;
    const int th = sy1 - sy0;
    const int opaqueAll = cv::countNonZero(opaqueMask);
    if (opaqueAll < 8) return out;
    if (cv::countNonZero(opaqueMask(cv::Rect(tx, ty, tw, th))) < opaqueAll) return out;
    out.srcRc = cv::Rect(sx0, sy0, tw, th);
    out.tplRc = cv::Rect(tx, ty, tw, th);
    out.valid = true;
    return out;
}

void ExpandCroppedSeedToFullTemplate(ImageMatchResult& r, const cv::Rect& box,
                                     int tplW, int tplH) {
    if (box.width <= 0 || !r.found) return;
    const int ox = static_cast<int>(std::lround(box.x * r.scale));
    const int oy = static_cast<int>(std::lround(box.y * r.scale));
    r.topLeftX -= ox;
    r.topLeftY -= oy;
    const int sw = std::max(1, static_cast<int>(std::lround(tplW * r.scale)));
    const int sh = std::max(1, static_cast<int>(std::lround(tplH * r.scale)));
    r.bottomRightX = r.topLeftX + sw;
    r.bottomRightY = r.topLeftY + sh;
    r.x = r.topLeftX + sw / 2;
    r.y = r.topLeftY + sh / 2;
}

/// 模板灰度标准差的**硬下限**：低于它直接判「模板无判别力」，不做匹配。
/// 依据：OpenCV templmatch.cpp 在零方差模板 + TM_CCOEFF_NORMED 时直接 `result = all(1)`，
/// 平方差侧则处处 0 —— 两者都会报出「100% 匹配」的假结果（常在 (0,0)），
/// 而日志上完全看不出来。这类模板只能靠颜色类工具（findColor/getColor）判断。
constexpr double kFlatTemplateMinStdDev = 4.0;

struct PatchVerifierContext {
    cv::Mat tplEdge;
    cv::Mat tplAngleDeg;
    cv::Mat tplHsvHist;
    cv::Mat tplOpaqueMask;
    bool hasColor = false;
    // 模板边缘总能量：低纹理（纯色/近纯色）模板边缘验证无判别力，
    // 直接中性化，避免共识把真实匹配误拒（见 ComputePatchEdgeSimilarity）。
    double tplEdgeEnergy = 0.0;
    double tplGrayStddev = 0.0;
    bool lowTexture = false;

    static void ComputeEdgeAndAngle(const cv::Mat& gray, cv::Mat& edge, cv::Mat& angleDeg) {
        cv::Mat gx;
        cv::Mat gy;
        cv::Sobel(gray, gx, CV_32F, 1, 0, 3);
        cv::Sobel(gray, gy, CV_32F, 0, 1, 3);
        cv::magnitude(gx, gy, edge);
        cv::phase(gx, gy, angleDeg, true);
    }

    static cv::Mat ComputeEdgeMap(const cv::Mat& gray) {
        cv::Mat edge;
        cv::Mat angle;
        ComputeEdgeAndAngle(gray, edge, angle);
        return edge;
    }

    static cv::Mat ComputeHsvHist(const cv::Mat& bgr, const cv::Mat& mask = cv::Mat()) {
        cv::Mat hsv;
        cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
        const int hBins = 24;
        const int sBins = 16;
        const int histSize[] = {hBins, sBins};
        const float hRanges[] = {0.0f, 180.0f};
        const float sRanges[] = {0.0f, 256.0f};
        const float* ranges[] = {hRanges, sRanges};
        const int channels[] = {0, 1};

        cv::Mat hist;
        cv::calcHist(&hsv, 1, channels, mask, hist, 2, histSize, ranges, true, false);
        if (cv::sum(hist)[0] < 1e-9) return {};
        // L1：bin 是概率质量。MINMAX 会把最热的 bin 拉成 1，不同 UI 面板直方图被拉齐后虚高。
        cv::normalize(hist, hist, 1.0, 0.0, cv::NORM_L1);
        return hist;
    }

    static PatchVerifierContext Build(const cv::Mat& tplGray, const cv::Mat& tplBgr,
                                       const cv::Mat& tplMask = cv::Mat()) {
        PatchVerifierContext ctx;
        if (tplGray.empty()) return ctx;
        const bool hasMask = !tplMask.empty() && tplMask.size() == tplGray.size()
            && tplMask.type() == CV_8UC1 && cv::countNonZero(tplMask) >= 8;
        if (hasMask) ctx.tplOpaqueMask = tplMask;

        const cv::Mat edgeSrc = hasMask
            ? FillTransparentWithOpaqueMean(tplGray, ctx.tplOpaqueMask)
            : tplGray;
        ComputeEdgeAndAngle(edgeSrc, ctx.tplEdge, ctx.tplAngleDeg);
        if (hasMask) {
            cv::Mat transparent;
            cv::compare(ctx.tplOpaqueMask, 0, transparent, cv::CMP_EQ);
            ctx.tplEdge.setTo(0, transparent);
            ctx.tplAngleDeg.setTo(0, transparent);
        }
        ctx.tplEdgeEnergy = cv::norm(ctx.tplEdge, cv::NORM_L2);
        cv::Scalar mean;
        cv::Scalar stdev;
        if (hasMask) {
            cv::meanStdDev(tplGray, mean, stdev, ctx.tplOpaqueMask);
        } else {
            cv::meanStdDev(tplGray, mean, stdev);
        }
        ctx.tplGrayStddev = stdev[0];
        ctx.lowTexture = ctx.tplEdgeEnergy < 0.5 || ctx.tplGrayStddev < 8.0;
        if (!tplBgr.empty() && tplBgr.size() == tplGray.size()) {
            ctx.tplHsvHist = ComputeHsvHist(tplBgr, ctx.tplOpaqueMask);
            ctx.hasColor = !ctx.tplHsvHist.empty();
        }
        return ctx;
    }
};

double ComputeNormedCorrelation32F(const cv::Mat& a, const cv::Mat& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) return 0.0;
    cv::Mat a32;
    cv::Mat b32;
    a.convertTo(a32, CV_32F);
    b.convertTo(b32, CV_32F);

    const cv::Scalar meanA = cv::mean(a32);
    const cv::Scalar meanB = cv::mean(b32);
    a32 -= static_cast<float>(meanA[0]);
    b32 -= static_cast<float>(meanB[0]);

    const double num = a32.dot(b32);
    const double den = cv::norm(a32) * cv::norm(b32);
    if (den <= 1e-6) return 0.0;
    return std::clamp(num / den, -1.0, 1.0);
}

double ComputePatchEdgeSimilarity(const cv::Mat& srcGray, const PatchVerifierContext& ctx,
                                  int topLeftX, int topLeftY) {
    if (srcGray.empty() || ctx.tplEdge.empty()) return 0.0;
    // 模板本身几乎没有边缘（纯色按钮内部、色块选区等）：边缘相关性无判别力，
    // 返回中性 100 分（不参与拒绝），否则正确位置会因边缘能量≈0 被误判为不匹配。
    if (ctx.tplEdgeEnergy < 0.5) return 100.0;
    const int tplW = ctx.tplEdge.cols;
    const int tplH = ctx.tplEdge.rows;
    const AlignedPatch win = AlignOpaquePatch(srcGray.cols, srcGray.rows, tplW, tplH,
                                               topLeftX, topLeftY, ctx.tplOpaqueMask);
    if (!win.valid) return 0.0;

    const cv::Mat patch = srcGray(win.srcRc);
    cv::Mat patchEdge = PatchVerifierContext::ComputeEdgeMap(patch);
    cv::Mat tplEdge = ctx.tplEdge(win.tplRc).clone();
    if (!ctx.tplOpaqueMask.empty() && ctx.tplOpaqueMask.size() == cv::Size(tplW, tplH)) {
        cv::Mat transparent;
        cv::compare(ctx.tplOpaqueMask(win.tplRc), 0, transparent, cv::CMP_EQ);
        patchEdge.setTo(0, transparent);
        tplEdge.setTo(0, transparent);
    }
    const double corr = ComputeNormedCorrelation32F(tplEdge, patchEdge);
    // 相关∈[-1,1]。旧映射 (corr+1)*50 让无关边缘也有 50 分，65% 阈值几乎不起作用。
    return std::clamp(corr, 0.0, 1.0) * 100.0;
}

double AngleDeltaDeg(float a, float b) {
    float d = std::fabs(a - b);
    if (d > 180.0f) d = 360.0f - d;
    if (d > 90.0f) d = 180.0f - d;  // 梯度方向取无符号，忽略黑白翻转
    return d;
}

/// LINEMOD/Halcon 形状匹配的精简版：只在候选上比强边缘的梯度方向，不整图搜。
double ComputePatchOrientationAgreePercent(const cv::Mat& srcGray, const PatchVerifierContext& ctx,
                                           int topLeftX, int topLeftY) {
    if (ctx.lowTexture || ctx.tplEdgeEnergy < 0.5) return 100.0;
    if (srcGray.empty() || ctx.tplEdge.empty() || ctx.tplAngleDeg.empty()) return 100.0;
    const int tplW = ctx.tplEdge.cols;
    const int tplH = ctx.tplEdge.rows;
    const AlignedPatch win = AlignOpaquePatch(srcGray.cols, srcGray.rows, tplW, tplH,
                                               topLeftX, topLeftY, ctx.tplOpaqueMask);
    if (!win.valid) return 0.0;

    double maxE = 0.0;
    cv::minMaxLoc(ctx.tplEdge(win.tplRc), nullptr, &maxE);
    const double tau = std::max(1.0, maxE * 0.10);

    const cv::Mat patch = srcGray(win.srcRc);
    cv::Mat patchEdge;
    cv::Mat patchAngle;
    PatchVerifierContext::ComputeEdgeAndAngle(patch, patchEdge, patchAngle);

    int considered = 0;
    int agreed = 0;
    for (int y = 0; y < win.tplRc.height; ++y) {
        const float* e = ctx.tplEdge.ptr<float>(win.tplRc.y + y) + win.tplRc.x;
        const float* ta = ctx.tplAngleDeg.ptr<float>(win.tplRc.y + y) + win.tplRc.x;
        const float* pa = patchAngle.ptr<float>(y);
        const uint8_t* m = ctx.tplOpaqueMask.empty()
            ? nullptr
            : ctx.tplOpaqueMask.ptr<uint8_t>(win.tplRc.y + y) + win.tplRc.x;
        for (int x = 0; x < win.tplRc.width; ++x) {
            if (m && m[x] == 0) continue;
            if (e[x] < tau) continue;
            ++considered;
            if (AngleDeltaDeg(ta[x], pa[x]) <= 25.0f) ++agreed;
        }
    }
    if (considered < 8) return 100.0;
    return 100.0 * static_cast<double>(agreed) / static_cast<double>(considered);
}

double ComputePatchColorSimilarity(const cv::Mat& srcBgr, const PatchVerifierContext& ctx,
                                   int topLeftX, int topLeftY) {
    if (!ctx.hasColor || srcBgr.empty()) return 100.0;
    const int tplW = ctx.tplEdge.cols;
    const int tplH = ctx.tplEdge.rows;
    const AlignedPatch win = AlignOpaquePatch(srcBgr.cols, srcBgr.rows, tplW, tplH,
                                               topLeftX, topLeftY, ctx.tplOpaqueMask);
    if (!win.valid) return 0.0;

    const cv::Mat patch = srcBgr(win.srcRc);
    const cv::Mat histMask = ctx.tplOpaqueMask.empty()
        ? cv::Mat()
        : ctx.tplOpaqueMask(win.tplRc);
    const cv::Mat patchHist = PatchVerifierContext::ComputeHsvHist(patch, histMask);
    if (patchHist.empty()) return 0.0;

    const double corr = cv::compareHist(ctx.tplHsvHist, patchHist, cv::HISTCMP_CORREL);
    return std::clamp(corr * 100.0, 0.0, 100.0);
}

int PixelAgreeChannelTol(double thresholdPercent) {
    return std::clamp(static_cast<int>(std::lround((100.0 - thresholdPercent) * 0.55)), 8, 40);
}

double AgreeRatioFromMaxDiff(const cv::Mat& maxDiff, int channelTol,
                             const cv::Mat& edgeMask, const cv::Mat& opaqueMask) {
    if (maxDiff.empty()) return 0.0;
    cv::Mat ok;
    cv::compare(maxDiff, channelTol, ok, cv::CMP_LE);

    double allRatio = 0.0;
    if (opaqueMask.empty()) {
        const double total = static_cast<double>(maxDiff.rows) * maxDiff.cols;
        if (total <= 0.0) return 0.0;
        allRatio = static_cast<double>(cv::countNonZero(ok)) / total;
    } else {
        cv::Mat okM;
        cv::bitwise_and(ok, opaqueMask, okM);
        const int denom = cv::countNonZero(opaqueMask);
        if (denom < 8) return 0.0;
        allRatio = static_cast<double>(cv::countNonZero(okM)) / static_cast<double>(denom);
    }
    if (edgeMask.empty()) return allRatio;
    cv::Mat edgeCountMask = edgeMask;
    if (!opaqueMask.empty()) {
        cv::bitwise_and(edgeMask, opaqueMask, edgeCountMask);
    }
    const int edgeCount = cv::countNonZero(edgeCountMask);
    if (edgeCount < 8) return allRatio;
    cv::Mat edgeOk;
    cv::bitwise_and(ok, edgeCountMask, edgeOk);
    const double edgeRatio = static_cast<double>(cv::countNonZero(edgeOk)) / static_cast<double>(edgeCount);
    return std::min(allRatio, edgeRatio);
}

/// ImageSearch 风格像素容差：全图 agree 与「模板强边缘像素 agree」取更严者。
/// 同类灰底按钮换了内部图标时，全图像素仍可能 90%+ 相同，必须看边缘像素。
double ComputePatchPixelAgreePercent(const cv::Mat& srcGray, const cv::Mat& srcBgr,
                                     const cv::Mat& tplGray, const cv::Mat& tplBgr,
                                     const PatchVerifierContext& ctx,
                                     int topLeftX, int topLeftY, int channelTol) {
    if (srcGray.empty() || tplGray.empty()) return 0.0;
    const int tplW = tplGray.cols;
    const int tplH = tplGray.rows;
    const AlignedPatch win = AlignOpaquePatch(srcGray.cols, srcGray.rows, tplW, tplH,
                                               topLeftX, topLeftY, ctx.tplOpaqueMask);
    if (!win.valid) return 0.0;

    cv::Mat edgeMask;
    if (!ctx.lowTexture && !ctx.tplEdge.empty() && ctx.tplEdge.size() == tplGray.size()) {
        double maxE = 0.0;
        cv::minMaxLoc(ctx.tplEdge, nullptr, &maxE);
        const double tau = std::max(1.0, maxE * 0.10);
        cv::compare(ctx.tplEdge, tau, edgeMask, cv::CMP_GE);
        if (cv::countNonZero(edgeMask) < 8) edgeMask.release();
        else edgeMask = edgeMask(win.tplRc);
    }

    const cv::Mat opaqueRoi = ctx.tplOpaqueMask.empty()
        ? cv::Mat()
        : ctx.tplOpaqueMask(win.tplRc);

    const bool useBgr = !srcBgr.empty() && !tplBgr.empty()
        && srcBgr.size() == srcGray.size() && tplBgr.size() == tplGray.size()
        && srcBgr.type() == CV_8UC3 && tplBgr.type() == CV_8UC3;

    if (useBgr) {
        const cv::Mat patch = srcBgr(win.srcRc);
        const cv::Mat tpl = tplBgr(win.tplRc);
        cv::Mat diff;
        cv::absdiff(patch, tpl, diff);
        std::vector<cv::Mat> ch;
        cv::split(diff, ch);
        cv::Mat maxd;
        cv::max(ch[0], ch[1], maxd);
        cv::max(maxd, ch[2], maxd);
        return std::clamp(AgreeRatioFromMaxDiff(maxd, channelTol, edgeMask, opaqueRoi) * 100.0, 0.0, 100.0);
    }

    const cv::Mat patch = srcGray(win.srcRc);
    const cv::Mat tpl = tplGray(win.tplRc);
    cv::Mat diff;
    cv::absdiff(patch, tpl, diff);
    return std::clamp(AgreeRatioFromMaxDiff(diff, channelTol, edgeMask, opaqueRoi) * 100.0, 0.0, 100.0);
}

std::vector<double> BuildUniformScales(double scaleMin, double scaleMax, double scaleStep) {
    std::vector<double> scales;
    if (scaleMax < scaleMin) std::swap(scaleMin, scaleMax);
    scaleStep = std::max(0.01, scaleStep);
    for (double s = scaleMin; s <= scaleMax + 1e-9; s += scaleStep) {
        scales.push_back(s);
    }
    if (scales.empty()) {
        scales.push_back(scaleMin);
    } else if (std::abs(scales.back() - scaleMax) > 1e-4) {
        scales.push_back(scaleMax);
    }
    return scales;
}

/// 跨分辨率粗尺度：约 5 个采样点（含端点与中点）
std::vector<double> BuildCoarseScales(double scaleMin, double scaleMax) {
    if (scaleMax < scaleMin) std::swap(scaleMin, scaleMax);
    if (scaleMax - scaleMin < 0.035) {
        return {0.5 * (scaleMin + scaleMax)};
    }
    constexpr int kSamples = 5;
    std::vector<double> scales;
    scales.reserve(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        const double t = static_cast<double>(i) / (kSamples - 1);
        scales.push_back(scaleMin + (scaleMax - scaleMin) * t);
    }
    return scales;
}

std::vector<double> BuildFineScalesAround(double center, double scaleMin, double scaleMax) {
    const double lo = std::max(scaleMin, center * 0.96);
    const double hi = std::min(scaleMax, center * 1.04);
    return BuildUniformScales(lo, hi, 0.02);
}

std::vector<ImageMatchResult> RunEngineOnScales(
    const cv::Mat& srcGray, const cv::Mat& tplGray,
    const ImageMatchOptions& opt, cv::TemplateMatchModes mode,
    const std::vector<double>& scales, double* outPeakPercent) {
    std::vector<ImageMatchResult> all;
    for (double scale : scales) {
        auto batch = MatchSingleScale(srcGray, tplGray, scale, opt, mode, outPeakPercent);
        all.insert(all.end(), batch.begin(), batch.end());
    }
    return GlobalNms(std::move(all), opt.maxOverlap, opt.maxMatches);
}

std::vector<ImageMatchResult> RunEngineAllScales(
    const cv::Mat& srcGray, const cv::Mat& tplGray,
    const ImageMatchOptions& opt, cv::TemplateMatchModes mode,
    double* outPeakPercent = nullptr) {
    return RunEngineOnScales(srcGray, tplGray, opt, mode,
        BuildUniformScales(opt.scaleMin, opt.scaleMax, opt.scaleStep), outPeakPercent);
}

/// 跨分辨率：单引擎粗→细定位最佳尺度
struct CrossResScaleSearch {
    std::vector<double> fineScales;
    std::vector<ImageMatchResult> nccResults;
    double bestPeakNcc = 0.0;
    double bestScale = 1.0;
};

CrossResScaleSearch RunCrossResolutionLocateSearch(
    const cv::Mat& srcGray, const cv::Mat& tplGray, const ImageMatchOptions& opt,
    cv::TemplateMatchModes mode) {
    CrossResScaleSearch out;
    out.bestScale = 0.5 * (opt.scaleMin + opt.scaleMax);
    const bool lowerIsBetter = mode == cv::TM_SQDIFF || mode == cv::TM_SQDIFF_NORMED;

    // 小模板/小尺度禁用金字塔，避免粗层 NCC 虚高而精修丢候选
    ImageMatchOptions searchOpt = opt;
    const double midScale = out.bestScale;
    const int approxTpl = static_cast<int>(std::min(tplGray.cols, tplGray.rows) * midScale);
    if (searchOpt.disablePyramid || midScale < 0.55 || approxTpl < 160) {
        searchOpt.disablePyramid = true;
    }

    const auto coarse = BuildCoarseScales(opt.scaleMin, opt.scaleMax);
    std::vector<ImageMatchResult> coarseAll;
    for (double scale : coarse) {
        double peak = 0.0;
        auto batch = MatchSingleScale(srcGray, tplGray, scale, searchOpt, mode, &peak);
        if (peak > out.bestPeakNcc) {
            out.bestPeakNcc = peak;
            out.bestScale = scale;
        }
        for (const auto& r : batch) {
            if (r.score > out.bestPeakNcc) {
                out.bestPeakNcc = r.score;
                out.bestScale = r.scale;
            }
            coarseAll.push_back(r);
        }
    }
    coarseAll = GlobalNms(std::move(coarseAll), opt.maxOverlap, opt.maxMatches);
    if (!coarseAll.empty()) {
        const ImageMatchResult* best = &coarseAll.front();
        for (const auto& r : coarseAll) {
            if (r.score > best->score) best = &r;
        }
        out.bestScale = best->scale;
        out.bestPeakNcc = std::max(out.bestPeakNcc, best->score);
    }

    if (out.bestPeakNcc < opt.thresholdPercent * 0.70) {
        out.fineScales.clear();
        out.nccResults = std::move(coarseAll);
        return out;
    }

    out.fineScales = BuildFineScalesAround(out.bestScale, opt.scaleMin, opt.scaleMax);
    double finePeak = out.bestPeakNcc;
    out.nccResults = RunEngineOnScales(srcGray, tplGray, searchOpt, mode,
                                       out.fineScales, &finePeak);
    out.bestPeakNcc = std::max(out.bestPeakNcc, finePeak);
    if (!out.nccResults.empty()) {
        const ImageMatchResult* best = &out.nccResults.front();
        for (const auto& r : out.nccResults) {
            if (r.score > best->score) best = &r;
        }
        out.bestScale = best->scale;
        out.bestPeakNcc = std::max(out.bestPeakNcc, best->score);
    } else if (!coarseAll.empty()) {
        out.nccResults = std::move(coarseAll);
    }

    // 峰值过阈但金字塔路径未产出候选：全分辨率补一刀
    if (out.nccResults.empty() && out.bestPeakNcc >= opt.thresholdPercent * 0.85) {
        ImageMatchOptions flat = searchOpt;
        flat.disablePyramid = true;
        double peak = 0.0;
        auto recovered = MatchSingleScale(srcGray, tplGray, out.bestScale, flat, mode, &peak);
        out.bestPeakNcc = std::max(out.bestPeakNcc, peak);
        if (recovered.empty() && peak >= opt.thresholdPercent) {
            cv::Mat scaledTpl;
            cv::resize(tplGray, scaledTpl, cv::Size(), out.bestScale, out.bestScale, cv::INTER_AREA);
            if (scaledTpl.cols >= 4 && scaledTpl.rows >= 4 &&
                scaledTpl.cols <= srcGray.cols && scaledTpl.rows <= srcGray.rows) {
                cv::Mat result;
                cv::matchTemplate(srcGray, scaledTpl, result, mode);
                double extreme = 0.0;
                cv::Point loc;
                if (lowerIsBetter) {
                    cv::minMaxLoc(result, &extreme, nullptr, &loc, nullptr);
                } else {
                    cv::minMaxLoc(result, nullptr, &extreme, nullptr, &loc);
                }
                const double score = RawScoreToSimilarity(extreme, mode);
                out.bestPeakNcc = std::max(out.bestPeakNcc, score);
                if (score >= image_match_internal::CandidateThresholdPercent(
                        opt.thresholdPercent, true)) {
                    recovered.push_back(MakeResult(loc, scaledTpl.cols, scaledTpl.rows,
                                                   score, out.bestScale));
                }
            }
        }
        out.nccResults = std::move(recovered);
        if (out.fineScales.empty()) {
            out.fineScales.push_back(out.bestScale);
        }
    }
    return out;
}

const ImageMatchResult* FindAgreeingMatch(
    const std::vector<ImageMatchResult>& candidates,
    int centerX, int centerY, int tolerancePx) {
    const ImageMatchResult* best = nullptr;
    for (const auto& c : candidates) {
        if (!c.found) continue;
        if (!PositionsAgree(c.x, c.y, centerX, centerY, tolerancePx)) continue;
        if (!best || c.score > best->score) best = &c;
    }
    return best;
}

double BestNccScore(const std::vector<ImageMatchResult>& nccResults) {
    double best = 0.0;
    for (const auto& r : nccResults) best = std::max(best, r.score);
    return best;
}

/// 在尺度范围内扫全局峰（共识失败时的兜底）
ImageMatchResult RecoverGlobalPeak(
    const cv::Mat& srcGray, const cv::Mat& tplGray, const ImageMatchOptions& opt,
    cv::TemplateMatchModes mode) {
    ImageMatchResult best{};
    double bestScore = 0.0;
    ImageMatchOptions flat = opt;
    flat.disablePyramid = true;
    const bool lowerIsBetter = mode == cv::TM_SQDIFF || mode == cv::TM_SQDIFF_NORMED;
    const auto scales = BuildUniformScales(opt.scaleMin, opt.scaleMax, opt.scaleStep);
    for (double scale : scales) {
        double peak = 0.0;
        auto batch = MatchSingleScale(srcGray, tplGray, scale, flat, mode, &peak);
        for (const auto& r : batch) {
            if (r.score > bestScore) {
                bestScore = r.score;
                best = r;
            }
        }
        if (peak <= bestScore) continue;

        cv::Mat scaledTpl;
        if (std::abs(scale - 1.0) > 0.001) {
            cv::resize(tplGray, scaledTpl, cv::Size(), scale, scale, cv::INTER_AREA);
        } else {
            scaledTpl = tplGray;
        }
        if (scaledTpl.cols < 4 || scaledTpl.rows < 4 ||
            scaledTpl.cols > srcGray.cols || scaledTpl.rows > srcGray.rows) {
            continue;
        }
        cv::Mat result;
        cv::matchTemplate(srcGray, scaledTpl, result, mode);
        double extreme = 0.0;
        cv::Point loc;
        if (lowerIsBetter) {
            cv::minMaxLoc(result, &extreme, nullptr, &loc, nullptr);
        } else {
            cv::minMaxLoc(result, nullptr, &extreme, nullptr, &loc);
        }
        const double score = RawScoreToSimilarity(extreme, mode);
        if (score > bestScore) {
            bestScore = score;
            best = MakeResult(loc, scaledTpl.cols, scaledTpl.rows, score, scale);
        }
    }
    return best;
}

ImageMatchResult RecoverGlobalNccPeak(
    const cv::Mat& srcGray, const cv::Mat& tplGray, const ImageMatchOptions& opt) {
    return RecoverGlobalPeak(srcGray, tplGray, opt, cv::TM_CCOEFF_NORMED);
}

}  // namespace

using image_match_internal::AutoConsensusTolerancePx;
using image_match_internal::GlobalNms;
using image_match_internal::MakeResult;
using image_match_internal::PositionsAgree;

void SyncImageMatchThreadBudget() {
    // OpenCV 默认按逻辑核数 fan-out：i7-7700（4 核 8 线程）跑一次全屏 matchTemplate
    // 会把 4 个物理核全部顶满，是用户反馈「找图时 CPU 温度飙到 80°C」的直接原因。
    // 低性能模式限 1 线程：单帧变慢一些，但峰值占用降到 1/4，且不再触发 turbo。
    static std::atomic<int> applied{-2};
    static int baseline = 0;
    if (baseline == 0) {
        baseline = cv::getNumThreads();
        if (baseline <= 0) baseline = 1;
    }
    const int want = LowPerformanceMode() ? 1 : baseline;
    if (applied.load(std::memory_order_relaxed) == want) return;
    cv::setNumThreads(want);
    applied.store(want, std::memory_order_relaxed);
}

double ComputePatchSadSimilarity(const cv::Mat& srcGray, const cv::Mat& tplGray,
                                 int topLeftX, int topLeftY) {
    if (srcGray.empty() || tplGray.empty()) return 0.0;
    const int tplW = tplGray.cols;
    const int tplH = tplGray.rows;
    if (topLeftX < 0 || topLeftY < 0 ||
        topLeftX + tplW > srcGray.cols || topLeftY + tplH > srcGray.rows) {
        return 0.0;
    }

    const cv::Mat patch = srcGray(cv::Rect(topLeftX, topLeftY, tplW, tplH));
    if (!patch.isContinuous() || !tplGray.isContinuous()) {
        uint64_t sad = 0;
        for (int y = 0; y < tplH; ++y) {
            const uint8_t* s = patch.ptr<uint8_t>(y);
            const uint8_t* t = tplGray.ptr<uint8_t>(y);
            for (int x = 0; x < tplW; ++x) {
                sad += static_cast<uint64_t>(std::abs(static_cast<int>(s[x]) - static_cast<int>(t[x])));
            }
        }
        const double maxSad = static_cast<double>(tplW) * tplH * 255.0;
        return std::clamp((1.0 - static_cast<double>(sad) / maxSad) * 100.0, 0.0, 100.0);
    }

    uint64_t sad = 0;
    const int totalPixels = tplW * tplH;

#if defined(IMAGE_MATCH_HAS_SSE2)
    const int simdWidth = totalPixels - (totalPixels % 16);
    const uint8_t* sBase = patch.ptr<uint8_t>(0);
    const uint8_t* tBase = tplGray.ptr<uint8_t>(0);

    for (int i = 0; i < simdWidth; i += 16) {
        __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(sBase + i));
        __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(tBase + i));
        __m128i diff = _mm_sad_epu8(a, b);
        sad += static_cast<uint64_t>(_mm_cvtsi128_si32(diff));
        sad += static_cast<uint64_t>(_mm_cvtsi128_si32(_mm_shuffle_epi32(diff, 0xEE)));
    }
    for (int i = simdWidth; i < totalPixels; ++i) {
        sad += static_cast<uint64_t>(std::abs(static_cast<int>(sBase[i]) - static_cast<int>(tBase[i])));
    }
#else
    const uint8_t* sBase = patch.ptr<uint8_t>(0);
    const uint8_t* tBase = tplGray.ptr<uint8_t>(0);
    for (int i = 0; i < totalPixels; ++i) {
        sad += static_cast<uint64_t>(std::abs(static_cast<int>(sBase[i]) - static_cast<int>(tBase[i])));
    }
#endif

    const double maxSad = static_cast<double>(totalPixels) * 255.0;
    return std::clamp((1.0 - static_cast<double>(sad) / maxSad) * 100.0, 0.0, 100.0);
}

double ComputePatchSadSimilarityMasked(const cv::Mat& srcGray, const cv::Mat& tplGray,
                                        const cv::Mat& mask, int topLeftX, int topLeftY) {
    if (mask.empty()) {
        return ComputePatchSadSimilarity(srcGray, tplGray, topLeftX, topLeftY);
    }
    if (srcGray.empty() || tplGray.empty() || mask.size() != tplGray.size()) return 0.0;
    const AlignedPatch win = AlignOpaquePatch(srcGray.cols, srcGray.rows,
        tplGray.cols, tplGray.rows, topLeftX, topLeftY, mask);
    if (!win.valid) return 0.0;
    uint64_t sad = 0;
    int n = 0;
    for (int y = 0; y < win.tplRc.height; ++y) {
        const uint8_t* s = srcGray.ptr<uint8_t>(win.srcRc.y + y) + win.srcRc.x;
        const uint8_t* t = tplGray.ptr<uint8_t>(win.tplRc.y + y) + win.tplRc.x;
        const uint8_t* m = mask.ptr<uint8_t>(win.tplRc.y + y) + win.tplRc.x;
        for (int x = 0; x < win.tplRc.width; ++x) {
            if (m[x] == 0) continue;
            sad += static_cast<uint64_t>(std::abs(static_cast<int>(s[x]) - static_cast<int>(t[x])));
            ++n;
        }
    }
    if (n < 8) return 0.0;
    const double maxSad = static_cast<double>(n) * 255.0;
    return std::clamp((1.0 - static_cast<double>(sad) / maxSad) * 100.0, 0.0, 100.0);
}

/// 逐像素终审：每个通道 |Δ| ≤ tol 才算通过（tol=1 吸收截图 1LSB 抖动）
bool PatchPixelsNearEqual(const cv::Mat& srcBgr, const cv::Mat& tplBgr,
                          int topLeftX, int topLeftY, int channelTol,
                          const cv::Mat& mask) {
    if (srcBgr.empty() || tplBgr.empty() || srcBgr.type() != CV_8UC3 || tplBgr.type() != CV_8UC3) {
        return false;
    }
    const int tw = tplBgr.cols;
    const int th = tplBgr.rows;
    const AlignedPatch win = AlignOpaquePatch(srcBgr.cols, srcBgr.rows, tw, th,
                                               topLeftX, topLeftY, mask);
    if (!win.valid) return false;
    const int tol = std::max(0, channelTol);
    const bool useMask = !mask.empty() && mask.size() == tplBgr.size() && mask.type() == CV_8UC1;
    int checked = 0;
    for (int y = 0; y < win.tplRc.height; ++y) {
        const cv::Vec3b* sp = srcBgr.ptr<cv::Vec3b>(win.srcRc.y + y) + win.srcRc.x;
        const cv::Vec3b* tp = tplBgr.ptr<cv::Vec3b>(win.tplRc.y + y) + win.tplRc.x;
        const uint8_t* mp = useMask
            ? mask.ptr<uint8_t>(win.tplRc.y + y) + win.tplRc.x
            : nullptr;
        for (int x = 0; x < win.tplRc.width; ++x) {
            if (mp && mp[x] == 0) continue;
            ++checked;
            const cv::Vec3b& a = sp[x];
            const cv::Vec3b& b = tp[x];
            if (std::abs(static_cast<int>(a[0]) - static_cast<int>(b[0])) > tol
                || std::abs(static_cast<int>(a[1]) - static_cast<int>(b[1])) > tol
                || std::abs(static_cast<int>(a[2]) - static_cast<int>(b[2])) > tol) {
                return false;
            }
        }
    }
    return checked >= 8 || (!useMask && checked > 0);
}

bool PatchGrayNearEqual(const cv::Mat& srcGray, const cv::Mat& tplGray,
                        int topLeftX, int topLeftY, int channelTol,
                        const cv::Mat& mask) {
    if (srcGray.empty() || tplGray.empty()) return false;
    const int tw = tplGray.cols;
    const int th = tplGray.rows;
    const AlignedPatch win = AlignOpaquePatch(srcGray.cols, srcGray.rows, tw, th,
                                               topLeftX, topLeftY, mask);
    if (!win.valid) return false;
    const int tol = std::max(0, channelTol);
    const bool useMask = !mask.empty() && mask.size() == tplGray.size() && mask.type() == CV_8UC1;
    int checked = 0;
    for (int y = 0; y < win.tplRc.height; ++y) {
        const uint8_t* sp = srcGray.ptr<uint8_t>(win.srcRc.y + y) + win.srcRc.x;
        const uint8_t* tp = tplGray.ptr<uint8_t>(win.tplRc.y + y) + win.tplRc.x;
        const uint8_t* mp = useMask
            ? mask.ptr<uint8_t>(win.tplRc.y + y) + win.tplRc.x
            : nullptr;
        for (int x = 0; x < win.tplRc.width; ++x) {
            if (mp && mp[x] == 0) continue;
            ++checked;
            if (std::abs(static_cast<int>(sp[x]) - static_cast<int>(tp[x])) > tol) return false;
        }
    }
    return checked >= 8 || (!useMask && checked > 0);
}

/// 完美匹配：NCC 粗定位（低阈值只为捞候选）→ 邻域精修 → 像素终审
ImageMatchOutput MatchPerfectPixel(
    const cv::Mat& srcGray, const cv::Mat& tplGray,
    const cv::Mat& srcBgr, const cv::Mat& tplBgr,
    const ImageMatchOptions& optIn, int offsetX, int offsetY,
    const cv::Mat& tplMask) {
    ImageMatchOutput out{};
    const auto t0 = std::chrono::steady_clock::now();
    if (srcGray.empty() || tplGray.empty()) return out;
    if (tplGray.cols > srcGray.cols || tplGray.rows > srcGray.rows) return out;

    const cv::Mat filledGray = FillTransparentWithOpaqueMean(tplGray, tplMask);
    cv::Mat locateGray = filledGray;
    cv::Rect opaqueBox;
    if (!tplMask.empty()) {
        opaqueBox = OpaqueContentRect(tplMask);
        if (opaqueBox.width > 0 &&
            (opaqueBox.width < tplGray.cols || opaqueBox.height < tplGray.rows)) {
            locateGray = filledGray(opaqueBox).clone();
        } else {
            opaqueBox = {};
        }
    }

    ImageMatchOptions coarse = optIn;
    coarse.perfectMatch = false;
    coarse.scaleMin = coarse.scaleMax = 1.0;
    coarse.scaleStep = 1.0;
    coarse.disablePyramid = true;
    coarse.crossResolutionMatch = false;
    // 粗定位门槛刻意放低：只负责找候选，通过与否由像素终审决定
    coarse.thresholdPercent = 50.0;
    coarse.maxMatches = std::clamp(optIn.maxMatches, 1, 30);
    coarse.maxOverlap = std::clamp(optIn.maxOverlap, 0.0, 0.95);

    const int channelTol = std::max(0, optIn.perfectMatchChannelTol);
    const int refineR = AutoConsensusTolerancePx(tplGray.cols, tplGray.rows);

    std::vector<ImageMatchResult> seeds;
    seeds.reserve(16);

    // 主定位用 SQDIFF：对近纯色模板也稳定（CCOEFF 在零方差模板上不可靠）
    {
        cv::Mat result;
        cv::matchTemplate(srcGray, locateGray, result, cv::TM_SQDIFF_NORMED);
        double minVal = 1.0;
        cv::Point minLoc;
        cv::minMaxLoc(result, &minVal, nullptr, &minLoc, nullptr);
        const double sim = std::clamp((1.0 - minVal) * 100.0, 0.0, 100.0);
        out.debugBestNccPercent = sim;
        if (minLoc.x >= 0 && minLoc.y >= 0) {
            seeds.push_back(MakeResult(minLoc, locateGray.cols, locateGray.rows, sim, 1.0));
        }
    }

    double peakNcc = 0.0;
    auto nccSeeds =
        RunEngineAllScales(srcGray, locateGray, coarse, cv::TM_CCOEFF_NORMED, &peakNcc);
    out.debugBestNccPercent = std::max(out.debugBestNccPercent, peakNcc);
    for (auto& s : nccSeeds) seeds.push_back(std::move(s));

    {
        ImageMatchResult recovered = RecoverGlobalNccPeak(srcGray, locateGray, coarse);
        out.debugBestNccPercent = std::max(out.debugBestNccPercent, recovered.score);
        if (recovered.found) seeds.push_back(recovered);
    }
    out.debugRawCandidates = static_cast<int>(seeds.size());

    // 按相似度分排序，优先精修高分候选
    std::sort(seeds.begin(), seeds.end(),
              [](const ImageMatchResult& a, const ImageMatchResult& b) { return a.score > b.score; });

    // 去重：相近 topLeft 只留最高分
    {
        std::vector<ImageMatchResult> uniq;
        uniq.reserve(seeds.size());
        for (const auto& s : seeds) {
            bool dup = false;
            for (const auto& u : uniq) {
                if (std::abs(u.topLeftX - s.topLeftX) <= 1 && std::abs(u.topLeftY - s.topLeftY) <= 1) {
                    dup = true;
                    break;
                }
            }
            if (!dup) uniq.push_back(s);
        }
        seeds = std::move(uniq);
    }

    if (opaqueBox.width > 0) {
        for (auto& s : seeds) {
            ExpandCroppedSeedToFullTemplate(s, opaqueBox, tplGray.cols, tplGray.rows);
        }
    }

    const bool useBgr = !srcBgr.empty() && !tplBgr.empty()
        && srcBgr.size() == srcGray.size() && tplBgr.size() == tplGray.size();

    auto trySeed = [&](const ImageMatchResult& seed) -> bool {
        const int baseX = seed.topLeftX;
        const int baseY = seed.topLeftY;
        for (int dy = -refineR; dy <= refineR; ++dy) {
            for (int dx = -refineR; dx <= refineR; ++dx) {
                const int x = baseX + dx;
                const int y = baseY + dy;
                const bool ok = useBgr
                    ? PatchPixelsNearEqual(srcBgr, tplBgr, x, y, channelTol, tplMask)
                    : PatchGrayNearEqual(srcGray, tplGray, x, y, channelTol, tplMask);
                if (!ok) continue;

                ImageMatchResult hit = MakeResult(
                    cv::Point(x, y), tplGray.cols, tplGray.rows, 100.0, 1.0);
                hit.topLeftX += offsetX;
                hit.topLeftY += offsetY;
                hit.bottomRightX += offsetX;
                hit.bottomRightY += offsetY;
                hit.x += offsetX;
                hit.y += offsetY;
                out.matches.push_back(hit);
                out.found = true;
                return true;
            }
        }
        return false;
    };

    for (const auto& seed : seeds) {
        if (trySeed(seed)) break;
    }

    const auto t1 = std::chrono::steady_clock::now();
    out.elapsedMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    return out;
}

ImageMatchOutput MatchInGrayMatsMultiVerify(
    const cv::Mat& srcGray, const cv::Mat& tplGray,
    const cv::Mat& srcBgr, const cv::Mat& tplBgr,
    const ImageMatchOptions& opt, int offsetX, int offsetY,
    const cv::Mat& tplMask) {
    if (opt.perfectMatch) {
        return MatchPerfectPixel(srcGray, tplGray, srcBgr, tplBgr, opt, offsetX, offsetY, tplMask);
    }
    ImageMatchOutput out{};
    const auto t0 = std::chrono::steady_clock::now();

    if (srcGray.empty() || tplGray.empty()) return out;

    ImageMatchOptions normalized = opt;
    normalized.thresholdPercent = std::clamp(normalized.thresholdPercent, 1.0, 100.0);
    normalized.scaleMin = std::max(0.1, normalized.scaleMin);
    normalized.scaleMax = std::max(normalized.scaleMin, normalized.scaleMax);
    normalized.scaleStep = std::max(0.01, normalized.scaleStep);
    normalized.maxMatches = std::clamp(normalized.maxMatches, 1, 200);
    normalized.maxOverlap = std::clamp(normalized.maxOverlap, 0.0, 0.95);
    const PatchVerifierContext tplTex = PatchVerifierContext::Build(tplGray, tplBgr, tplMask);
    out.debugTemplateStdDev = tplTex.tplGrayStddev;
    // ★零方差模板必须直接判失败（OpenCV 的静默陷阱）：
    // templmatch.cpp 里 `if (templNorm < DBL_EPSILON && method == TM_CCOEFF_NORMED)
    // { result = Scalar::all(1); return; }` —— 模板几乎是纯色时，**每个位置**都是 1.0，
    // 于是报出一个 100% 的假匹配（通常落在 (0,0)）；TM_SQDIFF 侧同理「处处 0 = 完美」。
    // 找图/定位/游戏挂机都会因此点到错误位置，且日志上看起来完全正常。
    if (tplTex.tplGrayStddev >= 0.0 && tplTex.tplGrayStddev < kFlatTemplateMinStdDev) {
        out.reason = L"模板几乎纯色（灰度标准差 "
            + std::to_wstring(static_cast<int>(tplTex.tplGrayStddev + 0.5))
            + L" < " + std::to_wstring(static_cast<int>(kFlatTemplateMinStdDev))
            + L"）：零方差模板在归一化互相关/平方差里会「处处满分」，"
              L"匹配结果必然是假的。请改用 findColor/getColor 判颜色，"
              L"或换一块有纹理的区域当模板。";
        out.elapsedMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count());
        return out;
    }
    if (tplTex.lowTexture || tplTex.tplGrayStddev < 16.0) {
        // 低纹理上金字塔粗层会虚高，精修丢掉真位置。
        normalized.disablePyramid = true;
    }
    // ★大区域搜索必须走「金字塔粗搜 → 全分辨率精修」（2026-09-16 性能修复）：
    // 旧规则 `maxMatches<=1 → disablePyramid` 让**找图（只取最佳命中）永远全分辨率扫全屏**，
    // 实测这正是「全屏找图把 CPU 烧到 80°C」的主因：单尺度 + 关金字塔时，
    // 一次 findImage 就是对 2560×1440 做一遍 DFT 版 matchTemplate（几十毫秒），
    // 无限循环里每秒 20 次 → 直接吃满几个核。改成按**面积**决定：
    //   · 搜索区足够大且有纹理 → 允许金字塔（4 倍降采样后粗搜，代价约 1/16），
    //     粗层本来就会保留 ≥40 个候选（locateOpt.maxMatches），最后还是全分辨率验证 + 精修，
    //     所以**精度不降级**；
    //   · 小区域（常见的区域找图）保持关闭——那时金字塔只有额外开销没有收益。
    if (normalized.disablePyramid && !tplTex.lowTexture && normalized.scaleMin == normalized.scaleMax) {
        constexpr int kPyramidMinAreaPx = 400 * 1000;   // 约 720p 以上才值得粗搜
        constexpr int kPyramidMinTemplateSide = 12;     // 太小的模板粗层会糊掉
        const bool areaBigEnough =
            static_cast<long long>(srcGray.cols) * srcGray.rows >= kPyramidMinAreaPx;
        const bool tplBigEnough =
            tplGray.cols >= kPyramidMinTemplateSide && tplGray.rows >= kPyramidMinTemplateSide
            && tplGray.cols <= srcGray.cols / 2 && tplGray.rows <= srcGray.rows / 2;
        if (areaBigEnough && tplBigEnough) normalized.disablePyramid = false;
    }

    const int tolerancePx = AutoConsensusTolerancePx(tplGray.cols, tplGray.rows);
    const double threshold = normalized.thresholdPercent;
    const cv::Mat locateGray = FillTransparentWithOpaqueMean(tplGray, tplMask);
    cv::Mat locateSrc = locateGray;
    cv::Rect opaqueBox;
    if (!tplMask.empty()) {
        opaqueBox = OpaqueContentRect(tplMask);
        if (opaqueBox.width > 0 &&
            (opaqueBox.width < tplGray.cols || opaqueBox.height < tplGray.rows)) {
            locateSrc = locateGray(opaqueBox).clone();
        } else {
            opaqueBox = {};
        }
    }
    // SQDIFF 对桌面 UI 比 CCOEFF 更有唯一性：弱纹理上 CCOEFF 会先填满 maxMatches 假峰。
    const cv::TemplateMatchModes locateMode = cv::TM_SQDIFF_NORMED;
    // 定位峰抑制要比最终 NMS 更狠：否则 SQDIFF 会在最佳命中周围爬出一串近邻峰，
    // 把 maxMatches 名额占满，同一模板的其它真实例进不了候选（找图/多图只剩 1 框）。
    constexpr double kLocatePeakMaxOverlap = 0.15;
    ImageMatchOptions locateOpt = normalized;
    locateOpt.maxOverlap = std::min(normalized.maxOverlap, kLocatePeakMaxOverlap);
    locateOpt.maxMatches = std::min(200, std::max(normalized.maxMatches * 3, 40));
    if (normalized.maxMatches > 1) {
        locateOpt.disablePyramid = true;
    }

    std::vector<ImageMatchResult> locateResults;
    double trackedPeakNcc = 0.0;

    if (locateOpt.crossResolutionMatch &&
        (locateOpt.scaleMax - locateOpt.scaleMin) > 0.04) {
        CrossResScaleSearch locateSearch =
            RunCrossResolutionLocateSearch(srcGray, locateSrc, locateOpt, locateMode);
        trackedPeakNcc = locateSearch.bestPeakNcc;
        locateResults = std::move(locateSearch.nccResults);
    } else {
        locateResults = RunEngineAllScales(
            srcGray, locateSrc, locateOpt, locateMode, &trackedPeakNcc);
    }

    if (opaqueBox.width > 0) {
        for (auto& r : locateResults) {
            ExpandCroppedSeedToFullTemplate(r, opaqueBox, tplGray.cols, tplGray.rows);
        }
    }

    // 弱纹理上 SQDIFF_NORMED 会铺成近 0 平台，FindPeaks 在左上角填满 maxMatches，
    // 精确贴图处反而进不了候选。非归一化 SQDIFF 在完全相同处 sumSq=0，位置唯一。
    //
    // ★性能（2026-09-16 用户实测「全屏找图把 CPU 烧到 80°C」的主因）：
    // TM_SQDIFF 是**直接卷积**（不像 SQDIFF_NORMED 走 DFT），
    // 对 2560×1440 + 96×96 模板一次约 3×10^10 次运算 ≈ 85ms；
    // 无限循环每秒 20 次 findImage 就是几个核满载。
    // 现在按面积自适应：区域大时先在**降采样图**上粗搜（代价约 1/k⁴），
    // 再回到全分辨率在候选点周围的小窗口复算同一个 sumSq —— 语义与分数口径不变。
    if (normalized.scaleMin <= 1.0 + 1e-6 && normalized.scaleMax >= 1.0 - 1e-6
        && locateSrc.cols <= srcGray.cols && locateSrc.rows <= srcGray.rows
        && locateSrc.cols >= 4 && locateSrc.rows >= 4) {
        // 大区域才值得降采样（小区域 k=1，行为与旧实现完全一致）
        constexpr double kRescueFullResMaxArea = 400.0 * 1000.0;   // ≈640×640
        const int areaPx = srcGray.cols * srcGray.rows;
        const int maxDownscale = std::clamp(normalized.rescueMaxDownscale, 1, 8);
        int k = 1;
        while (k < maxDownscale && areaPx / ((k + 1) * (k + 1)) >= kRescueFullResMaxArea) ++k;

        auto rmsToSim = [](double sumSq, int tplPixels) {
            const double denom = static_cast<double>(tplPixels);
            const double rms = denom > 0.0 ? std::sqrt(std::max(0.0, sumSq) / denom) : 255.0;
            return std::clamp((1.0 - rms / 255.0) * 100.0, 0.0, 100.0);
        };

        double bestSumSq = -1.0;
        cv::Point bestLoc(-1, -1);
        if (k == 1) {
            cv::Mat sq;
            cv::matchTemplate(srcGray, locateSrc, sq, cv::TM_SQDIFF);
            double minVal = 0.0;
            cv::minMaxLoc(sq, &minVal, nullptr, &bestLoc, nullptr);
            bestSumSq = minVal;
        } else {
            cv::Mat coarseSrc;
            cv::Mat coarseTpl;
            cv::resize(srcGray, coarseSrc, cv::Size(), 1.0 / k, 1.0 / k, cv::INTER_AREA);
            cv::resize(locateSrc, coarseTpl, cv::Size(), 1.0 / k, 1.0 / k, cv::INTER_AREA);
            if (coarseTpl.cols >= 4 && coarseTpl.rows >= 4
                && coarseTpl.cols <= coarseSrc.cols && coarseTpl.rows <= coarseSrc.rows) {
                cv::Mat sq;
                cv::matchTemplate(coarseSrc, coarseTpl, sq, cv::TM_SQDIFF);
                // 粗层取前 N 个极小值（模板尺度半径内抑制重叠），再逐个回全分辨率复算
                constexpr int kRescueCandidates = 6;
                cv::Mat work = sq.clone();
                for (int c = 0; c < kRescueCandidates; ++c) {
                    double cval = 0.0;
                    cv::Point cloc;
                    cv::minMaxLoc(work, &cval, nullptr, &cloc, nullptr);
                    if (cloc.x < 0 || cloc.y < 0) break;
                    // 抑制邻域（半径 = 粗层模板的短边）
                    const int rad = std::max(2, (std::min)(coarseTpl.cols, coarseTpl.rows) / 2);
                    cv::rectangle(work,
                        cv::Rect(std::max(0, cloc.x - rad), std::max(0, cloc.y - rad),
                            rad * 2 + 1, rad * 2 + 1),
                        cv::Scalar(std::numeric_limits<double>::max()), cv::FILLED);
                    // 全分辨率小窗口复算：候选位置 ±(k+2)px
                    const int fx = cloc.x * k;
                    const int fy = cloc.y * k;
                    const int pad = k + 2;
                    const int wx1 = std::max(0, fx - pad);
                    const int wy1 = std::max(0, fy - pad);
                    const int wx2 = std::min(srcGray.cols, fx + pad + locateSrc.cols);
                    const int wy2 = std::min(srcGray.rows, fy + pad + locateSrc.rows);
                    if (wx2 - wx1 < locateSrc.cols || wy2 - wy1 < locateSrc.rows) continue;
                    const cv::Mat window = srcGray(cv::Rect(wx1, wy1,
                        wx2 - wx1, wy2 - wy1));
                    cv::Mat ws;
                    cv::matchTemplate(window, locateSrc, ws, cv::TM_SQDIFF);
                    double wmin = 0.0;
                    cv::Point wloc;
                    cv::minMaxLoc(ws, &wmin, nullptr, &wloc, nullptr);
                    if (wloc.x < 0 || wloc.y < 0) continue;
                    if (bestSumSq < 0.0 || wmin < bestSumSq) {
                        bestSumSq = wmin;
                        bestLoc = cv::Point(wx1 + wloc.x, wy1 + wloc.y);
                    }
                }
            }
        }
        if (bestSumSq >= 0.0 && bestLoc.x >= 0 && bestLoc.y >= 0) {
            const double sim = rmsToSim(bestSumSq, locateSrc.cols * locateSrc.rows);
            out.debugBestNccPercent = std::max(out.debugBestNccPercent, sim);
            ImageMatchResult exact =
                MakeResult(bestLoc, locateSrc.cols, locateSrc.rows, sim, 1.0);
            if (opaqueBox.width > 0) {
                ExpandCroppedSeedToFullTemplate(exact, opaqueBox, tplGray.cols, tplGray.rows);
            }
            locateResults.insert(locateResults.begin(), exact);
        }
    }

    locateResults = GlobalNms(std::move(locateResults), kLocatePeakMaxOverlap,
                                normalized.maxMatches);

    out.debugRawCandidates = static_cast<int>(locateResults.size());
    out.debugBestNccPercent = std::max(trackedPeakNcc, BestNccScore(locateResults));

    const double bestNccScore = out.debugBestNccPercent;

    auto scaledTemplateMats = [](const cv::Mat& gray, const cv::Mat& bgr, const cv::Mat& mask,
                                  double scale, cv::Mat& outGray, cv::Mat& outBgr, cv::Mat& outMask) {
        if (std::abs(scale - 1.0) < 0.001) {
            outGray = gray;
            outBgr = bgr;
            outMask = mask;
            return;
        }
        cv::resize(gray, outGray, cv::Size(), scale, scale, cv::INTER_AREA);
        if (!bgr.empty()) {
            cv::resize(bgr, outBgr, cv::Size(), scale, scale, cv::INTER_AREA);
        } else {
            outBgr.release();
        }
        if (mask.empty()) {
            outMask.release();
        } else {
            cv::resize(mask, outMask, cv::Size(), scale, scale, cv::INTER_NEAREST);
        }
    };

    std::vector<ImageMatchResult> consensus;
    consensus.reserve(static_cast<size_t>(normalized.maxMatches));

    const int pixelTol = PixelAgreeChannelTol(threshold);

    auto tryAcceptSeed = [&](const ImageMatchResult& seed) {
        const ImageMatchResult* locateHit =
            FindAgreeingMatch(locateResults, seed.x, seed.y, tolerancePx);
        if (seed.found && seed.score >= threshold) {
            if (!locateHit || locateHit->score < seed.score) {
                locateHit = &seed;
            }
        }
        if (!locateHit || locateHit->score < threshold) return;

        const double verifyScale = seed.scale > 0.0 ? seed.scale : 1.0;

        struct AgreeHit {
            double percent = -1.0;
            double sadSim = -1.0;
            int x = 0;
            int y = 0;
            double scale = 1.0;
            cv::Mat gray;
            cv::Mat bgr;
            PatchVerifierContext ctx;
        };
        AgreeHit hit;
        auto considerScale = [&](double scale) {
            cv::Mat g;
            cv::Mat b;
            cv::Mat m;
            scaledTemplateMats(tplGray, tplBgr, tplMask, scale, g, b, m);
            if (g.empty()) return;
            const PatchVerifierContext ctx = PatchVerifierContext::Build(g, b, m);
            int baseX = seed.topLeftX;
            int baseY = seed.topLeftY;
            if (std::abs(scale - verifyScale) > 0.001) {
                const int cx = seed.topLeftX + (seed.bottomRightX - seed.topLeftX) / 2;
                const int cy = seed.topLeftY + (seed.bottomRightY - seed.topLeftY) / 2;
                baseX = cx - g.cols / 2;
                baseY = cy - g.rows / 2;
            }
            const int refineR = (g.cols * g.rows > 80000) ? 1 : 3;
            for (int dy = -refineR; dy <= refineR; ++dy) {
                for (int dx = -refineR; dx <= refineR; ++dx) {
                    const int x = baseX + dx;
                    const int y = baseY + dy;
                    const double a = ComputePatchPixelAgreePercent(
                        srcGray, srcBgr, g, b, ctx, x, y, pixelTol);
                    if (hit.percent >= 0.0 && a < hit.percent - 0.05) continue;
                    const double sadSim = ComputePatchSadSimilarityMasked(
                        srcGray, g, ctx.tplOpaqueMask, x, y);
                    const bool take = hit.percent < 0.0
                        || a > hit.percent + 0.05
                        || (std::abs(a - hit.percent) <= 0.05 && sadSim > hit.sadSim);
                    if (!take) continue;
                    hit.percent = a;
                    hit.sadSim = sadSim;
                    hit.x = x;
                    hit.y = y;
                    hit.scale = scale;
                    hit.gray = g;
                    hit.bgr = b;
                    hit.ctx = ctx;
                }
            }
        };
        considerScale(verifyScale);
        if (std::abs(verifyScale - 1.0) > 0.001 && std::abs(verifyScale - 1.0) < 0.16) {
            considerScale(1.0);
        }
        out.debugBestPixelAgreePercent =
            std::max(out.debugBestPixelAgreePercent, std::max(0.0, hit.percent));
        if (hit.percent < threshold) return;

        const double nccScore = locateHit->score;
        const bool isTopNccPeak = (bestNccScore - nccScore) < 1.0;
        // 顶峰允许略低于用户阈值：截图噪声会压低 Sobel 相关；像素容差仍是硬门槛。
        const double edgeNeed = isTopNccPeak ? threshold * 0.75 : (threshold + kEdgeBoostPercent);
        const double colorNeed = isTopNccPeak ? threshold * 0.75 : (threshold + kColorBoostPercent);

        if (hit.percent < 96.0) {
            const double edgeScore =
                ComputePatchEdgeSimilarity(srcGray, hit.ctx, hit.x, hit.y);
            const double colorScore =
                ComputePatchColorSimilarity(srcBgr, hit.ctx, hit.x, hit.y);
            const double orientScore =
                ComputePatchOrientationAgreePercent(srcGray, hit.ctx, hit.x, hit.y);
            if (edgeScore < edgeNeed) return;
            if (orientScore < edgeNeed) return;
            if (hit.ctx.hasColor && colorScore < colorNeed) return;
        }

        const int hitCx = hit.x + hit.gray.cols / 2;
        const int hitCy = hit.y + hit.gray.rows / 2;
        for (const auto& existing : consensus) {
            if (PositionsAgree(existing.x, existing.y, hitCx, hitCy, tolerancePx)) return;
        }

        ImageMatchResult merged = seed;
        merged.topLeftX = hit.x;
        merged.topLeftY = hit.y;
        merged.bottomRightX = hit.x + hit.gray.cols;
        merged.bottomRightY = hit.y + hit.gray.rows;
        merged.x = hitCx;
        merged.y = hitCy;
        merged.scale = hit.scale;
        merged.score = std::min(nccScore, hit.percent);
        consensus.push_back(merged);
    };

    for (const auto& seed : locateResults) {
        tryAcceptSeed(seed);
        if (static_cast<int>(consensus.size()) >= normalized.maxMatches) break;
    }

    if (consensus.empty()) {
        ImageMatchResult recovered = RecoverGlobalPeak(srcGray, locateSrc, normalized, locateMode);
        if (opaqueBox.width > 0) {
            ExpandCroppedSeedToFullTemplate(recovered, opaqueBox, tplGray.cols, tplGray.rows);
        }
        out.debugBestNccPercent = std::max(out.debugBestNccPercent, recovered.score);
        if (recovered.found && recovered.score >= threshold) {
            tryAcceptSeed(recovered);
        }
    }

    consensus = GlobalNms(std::move(consensus), normalized.maxOverlap, normalized.maxMatches);

    for (auto& m : consensus) {
        m.topLeftX += offsetX;
        m.topLeftY += offsetY;
        m.bottomRightX += offsetX;
        m.bottomRightY += offsetY;
        m.x += offsetX;
        m.y += offsetY;
    }

    out.matches = std::move(consensus);
    out.found = !out.matches.empty();
    const auto t1 = std::chrono::steady_clock::now();
    out.elapsedMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    return out;
}
