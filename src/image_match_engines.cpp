// image_match_engines.cpp — Multi-engine consensus template matching
//
// Gray matchers: TM_CCOEFF_NORMED / TM_SQDIFF_NORMED / TM_CCORR_NORMED + SIMD SAD
// Structural verifiers (reduce false positives on similar-sized icons):
//   - Sobel edge NCC
//   - HSV color histogram correlation
//   - Peak margin vs best NCC candidate

#include "image_match_engines.h"

#include "image_match_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <emmintrin.h>
#define IMAGE_MATCH_HAS_SSE2 1
#endif

namespace {

using namespace image_match_internal;

struct EngineSpec {
    const wchar_t* name;
    cv::TemplateMatchModes mode;
};

constexpr EngineSpec kTemplateEngines[] = {
    {L"NCC", cv::TM_CCOEFF_NORMED},
    {L"SQDIFF", cv::TM_SQDIFF_NORMED},
    {L"CCORR", cv::TM_CCORR_NORMED},
};

constexpr double kEdgeBoostPercent = 8.0;
constexpr double kColorBoostPercent = 6.0;

struct PatchVerifierContext {
    cv::Mat tplEdge;
    cv::Mat tplHsvHist;
    bool hasColor = false;

    static cv::Mat ComputeEdgeMap(const cv::Mat& gray) {
        cv::Mat gx;
        cv::Mat gy;
        cv::Sobel(gray, gx, CV_32F, 1, 0, 3);
        cv::Sobel(gray, gy, CV_32F, 0, 1, 3);
        cv::Mat edge;
        cv::magnitude(gx, gy, edge);
        return edge;
    }

    static cv::Mat ComputeHsvHist(const cv::Mat& bgr) {
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
        cv::calcHist(&hsv, 1, channels, cv::Mat(), hist, 2, histSize, ranges, true, false);
        cv::normalize(hist, hist, 0.0, 1.0, cv::NORM_MINMAX);
        return hist;
    }

    static PatchVerifierContext Build(const cv::Mat& tplGray, const cv::Mat& tplBgr) {
        PatchVerifierContext ctx;
        if (tplGray.empty()) return ctx;
        ctx.tplEdge = ComputeEdgeMap(tplGray);
        if (!tplBgr.empty() && tplBgr.size() == tplGray.size()) {
            ctx.tplHsvHist = ComputeHsvHist(tplBgr);
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
    const int tplW = ctx.tplEdge.cols;
    const int tplH = ctx.tplEdge.rows;
    if (topLeftX < 0 || topLeftY < 0 ||
        topLeftX + tplW > srcGray.cols || topLeftY + tplH > srcGray.rows) {
        return 0.0;
    }

    const cv::Mat patch = srcGray(cv::Rect(topLeftX, topLeftY, tplW, tplH));
    const cv::Mat patchEdge = PatchVerifierContext::ComputeEdgeMap(patch);
    const double corr = ComputeNormedCorrelation32F(ctx.tplEdge, patchEdge);
    return std::clamp((corr + 1.0) * 50.0, 0.0, 100.0);
}

double ComputePatchColorSimilarity(const cv::Mat& srcBgr, const PatchVerifierContext& ctx,
                                   int topLeftX, int topLeftY) {
    if (!ctx.hasColor || srcBgr.empty()) return 100.0;
    const int tplW = ctx.tplEdge.cols;
    const int tplH = ctx.tplEdge.rows;
    if (topLeftX < 0 || topLeftY < 0 ||
        topLeftX + tplW > srcBgr.cols || topLeftY + tplH > srcBgr.rows) {
        return 0.0;
    }

    const cv::Mat patch = srcBgr(cv::Rect(topLeftX, topLeftY, tplW, tplH));
    const cv::Mat patchHist = PatchVerifierContext::ComputeHsvHist(patch);
    if (patchHist.empty()) return 0.0;

    const double corr = cv::compareHist(ctx.tplHsvHist, patchHist, cv::HISTCMP_CORREL);
    return std::clamp(corr * 100.0, 0.0, 100.0);
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

/// 跨分辨率：NCC 粗→细定位最佳尺度，其它引擎只在少量精尺度上跑
struct CrossResScaleSearch {
    std::vector<double> fineScales;
    std::vector<ImageMatchResult> nccResults;
    double bestPeakNcc = 0.0;
    double bestScale = 1.0;
};

CrossResScaleSearch RunCrossResolutionNccSearch(
    const cv::Mat& srcGray, const cv::Mat& tplGray, const ImageMatchOptions& opt) {
    CrossResScaleSearch out;
    out.bestScale = 0.5 * (opt.scaleMin + opt.scaleMax);

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
        auto batch = MatchSingleScale(srcGray, tplGray, scale, searchOpt, cv::TM_CCOEFF_NORMED, &peak);
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
    out.nccResults = RunEngineOnScales(srcGray, tplGray, searchOpt, cv::TM_CCOEFF_NORMED,
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
        auto recovered = MatchSingleScale(srcGray, tplGray, out.bestScale, flat,
                                          cv::TM_CCOEFF_NORMED, &peak);
        out.bestPeakNcc = std::max(out.bestPeakNcc, peak);
        if (recovered.empty() && peak >= opt.thresholdPercent) {
            // MatchSingleScale 仍空时，直接取 minMaxLoc 峰位
            cv::Mat scaledTpl;
            cv::resize(tplGray, scaledTpl, cv::Size(), out.bestScale, out.bestScale, cv::INTER_AREA);
            if (scaledTpl.cols >= 4 && scaledTpl.rows >= 4 &&
                scaledTpl.cols <= srcGray.cols && scaledTpl.rows <= srcGray.rows) {
                cv::Mat result;
                cv::matchTemplate(srcGray, scaledTpl, result, cv::TM_CCOEFF_NORMED);
                double maxVal = 0.0;
                cv::Point maxLoc;
                cv::minMaxLoc(result, nullptr, &maxVal, nullptr, &maxLoc);
                const double score = maxVal * 100.0;
                out.bestPeakNcc = std::max(out.bestPeakNcc, score);
                if (score >= image_match_internal::CandidateThresholdPercent(
                        opt.thresholdPercent, true)) {
                    recovered.push_back(MakeResult(maxLoc, scaledTpl.cols, scaledTpl.rows,
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

/// 在尺度范围内扫全局 NCC 峰（共识失败时的兜底）
ImageMatchResult RecoverGlobalNccPeak(
    const cv::Mat& srcGray, const cv::Mat& tplGray, const ImageMatchOptions& opt) {
    ImageMatchResult best{};
    double bestScore = 0.0;
    ImageMatchOptions flat = opt;
    flat.disablePyramid = true;
    const auto scales = BuildUniformScales(opt.scaleMin, opt.scaleMax, opt.scaleStep);
    for (double scale : scales) {
        double peak = 0.0;
        auto batch = MatchSingleScale(srcGray, tplGray, scale, flat, cv::TM_CCOEFF_NORMED, &peak);
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
        cv::matchTemplate(srcGray, scaledTpl, result, cv::TM_CCOEFF_NORMED);
        double maxVal = 0.0;
        cv::Point maxLoc;
        cv::minMaxLoc(result, nullptr, &maxVal, nullptr, &maxLoc);
        const double score = maxVal * 100.0;
        if (score > bestScore) {
            bestScore = score;
            best = MakeResult(maxLoc, scaledTpl.cols, scaledTpl.rows, score, scale);
        }
    }
    return best;
}

}  // namespace

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

ImageMatchOutput MatchInGrayMatsMultiVerify(
    const cv::Mat& srcGray, const cv::Mat& tplGray,
    const cv::Mat& srcBgr, const cv::Mat& tplBgr,
    const ImageMatchOptions& opt, int offsetX, int offsetY) {
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

    const int tolerancePx = AutoConsensusTolerancePx(tplGray.cols, tplGray.rows);
    const double threshold = normalized.thresholdPercent;

    std::vector<std::vector<ImageMatchResult>> engineResults(std::size(kTemplateEngines));
    double trackedPeakNcc = 0.0;

    if (normalized.crossResolutionMatch &&
        (normalized.scaleMax - normalized.scaleMin) > 0.04) {
        // 跨分辨率：NCC 粗→细，峰值不足阈值则早停（避免 3 引擎 × 密尺度）
        CrossResScaleSearch nccSearch =
            RunCrossResolutionNccSearch(srcGray, tplGray, normalized);
        trackedPeakNcc = nccSearch.bestPeakNcc;
        engineResults[0] = std::move(nccSearch.nccResults);

        const bool nccHopeful = trackedPeakNcc >= threshold * 0.85
            || BestNccScore(engineResults[0]) >= image_match_internal::CandidateThresholdPercent(
                   threshold, true);
        if (nccHopeful && !nccSearch.fineScales.empty()) {
            std::vector<std::future<std::vector<ImageMatchResult>>> futures;
            futures.reserve(2);
            for (size_t i = 1; i < std::size(kTemplateEngines); ++i) {
                const auto mode = kTemplateEngines[i].mode;
                const auto scales = nccSearch.fineScales;
                futures.push_back(std::async(std::launch::async,
                    [&srcGray, &tplGray, normalized, mode, scales]() {
                        return RunEngineOnScales(srcGray, tplGray, normalized, mode,
                                                 scales, nullptr);
                    }));
            }
            for (size_t i = 0; i < futures.size(); ++i) {
                engineResults[i + 1] = futures[i].get();
            }
        }
    } else {
        std::vector<std::future<std::pair<std::vector<ImageMatchResult>, double>>> futures;
        futures.reserve(std::size(kTemplateEngines));
        for (const auto& spec : kTemplateEngines) {
            futures.push_back(std::async(std::launch::async,
                [&srcGray, &tplGray, normalized, spec]() {
                    double localPeak = 0.0;
                    auto results = RunEngineAllScales(
                        srcGray, tplGray, normalized, spec.mode,
                        spec.mode == cv::TM_CCOEFF_NORMED ? &localPeak : nullptr);
                    return std::make_pair(std::move(results), localPeak);
                }));
        }
        for (size_t i = 0; i < futures.size(); ++i) {
            auto batch = futures[i].get();
            engineResults[i] = std::move(batch.first);
            if (i == 0) trackedPeakNcc = batch.second;
        }
    }

    if (engineResults.empty()) {
        const auto t1 = std::chrono::steady_clock::now();
        out.elapsedMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
        return out;
    }

    bool anyCandidates = false;
    int rawCandidateCount = 0;
    for (const auto& results : engineResults) {
        rawCandidateCount += static_cast<int>(results.size());
        if (!results.empty()) {
            anyCandidates = true;
        }
    }
    out.debugRawCandidates = rawCandidateCount;
    out.debugBestNccPercent = std::max(trackedPeakNcc, BestNccScore(engineResults[0]));

    if (!anyCandidates) {
        const auto t1 = std::chrono::steady_clock::now();
        out.elapsedMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
        return out;
    }

    const double bestNccScore = out.debugBestNccPercent;

    auto scaledTemplateMats = [](const cv::Mat& gray, const cv::Mat& bgr, double scale,
                                  cv::Mat& outGray, cv::Mat& outBgr) {
        if (std::abs(scale - 1.0) < 0.001) {
            outGray = gray;
            outBgr = bgr;
            return;
        }
        cv::resize(gray, outGray, cv::Size(), scale, scale, cv::INTER_AREA);
        if (!bgr.empty()) {
            cv::resize(bgr, outBgr, cv::Size(), scale, scale, cv::INTER_AREA);
        } else {
            outBgr.release();
        }
    };

    std::vector<ImageMatchResult> consensus;
    consensus.reserve(static_cast<size_t>(normalized.maxMatches));

    auto tryAcceptSeed = [&](const ImageMatchResult& seed, bool requireAllEngines,
                             bool relaxVerify) {
        std::vector<const ImageMatchResult*> perEngineBest(engineResults.size(), nullptr);
        const size_t engineLimit = requireAllEngines ? engineResults.size() : 1;
        for (size_t e = 0; e < engineLimit; ++e) {
            perEngineBest[e] =
                FindAgreeingMatch(engineResults[e], seed.x, seed.y, tolerancePx);
            if (!perEngineBest[e]) return;
            if (perEngineBest[e]->score < threshold) return;
        }

        const double verifyScale = seed.scale > 0.0 ? seed.scale : 1.0;
        cv::Mat verifyGray;
        cv::Mat verifyBgr;
        scaledTemplateMats(tplGray, tplBgr, verifyScale, verifyGray, verifyBgr);
        if (verifyGray.empty()) return;

        const PatchVerifierContext scaledVerifier =
            PatchVerifierContext::Build(verifyGray, verifyBgr);

        const double sadScore =
            ComputePatchSadSimilarity(srcGray, verifyGray, seed.topLeftX, seed.topLeftY);
        const double sadNeed = relaxVerify ? (threshold * 0.55) : threshold;
        if (sadScore < sadNeed) {
            if (!relaxVerify) return;
            // 跨分辨率时 SAD 与 NCC 尺度不完全一致，NCC 已过阈值则仍接受
            if (seed.score < threshold) return;
        }

        if (!relaxVerify) {
            const double edgeScore =
                ComputePatchEdgeSimilarity(srcGray, scaledVerifier, seed.topLeftX, seed.topLeftY);
            const double colorScore =
                ComputePatchColorSimilarity(srcBgr, scaledVerifier, seed.topLeftX, seed.topLeftY);

            const double nccScore = perEngineBest[0]->score;
            const bool isTopNccPeak = (bestNccScore - nccScore) < 1.0;
            const double edgeNeed = isTopNccPeak ? threshold : (threshold + kEdgeBoostPercent);
            const double colorNeed = isTopNccPeak ? threshold : (threshold + kColorBoostPercent);

            if (edgeScore < edgeNeed) return;
            if (scaledVerifier.hasColor && colorScore < colorNeed) return;
        }

        for (const auto& existing : consensus) {
            if (PositionsAgree(existing.x, existing.y, seed.x, seed.y, tolerancePx)) return;
        }

        ImageMatchResult merged = seed;
        if (relaxVerify) {
            merged.score = std::max(seed.score, sadScore);
        } else {
            const double edgeScore =
                ComputePatchEdgeSimilarity(srcGray, scaledVerifier, seed.topLeftX, seed.topLeftY);
            const double colorScore =
                ComputePatchColorSimilarity(srcBgr, scaledVerifier, seed.topLeftX, seed.topLeftY);
            double scoreSum = sadScore + edgeScore;
            if (scaledVerifier.hasColor) scoreSum += colorScore;
            scoreSum += perEngineBest[0]->score;
            if (requireAllEngines) {
                for (size_t e = 1; e < perEngineBest.size(); ++e) {
                    if (perEngineBest[e]) scoreSum += perEngineBest[e]->score;
                }
            }
            const int divisor = (requireAllEngines ? static_cast<int>(perEngineBest.size()) : 1)
                + 2 + (scaledVerifier.hasColor ? 1 : 0);
            merged.score = scoreSum / static_cast<double>(divisor);
        }
        consensus.push_back(merged);
    };

    for (const auto& results : engineResults) {
        for (const auto& seed : results) {
            tryAcceptSeed(seed, true, false);
            if (static_cast<int>(consensus.size()) >= normalized.maxMatches) break;
        }
        if (static_cast<int>(consensus.size()) >= normalized.maxMatches) break;
    }

    if (consensus.empty() && !engineResults[0].empty()) {
        ImageMatchResult bestNcc{};
        for (const auto& r : engineResults[0]) {
            if (r.score > bestNcc.score) bestNcc = r;
        }
        if (bestNcc.found && bestNcc.score >= threshold) {
            tryAcceptSeed(bestNcc, false, true);
            if (consensus.empty()) {
                consensus.push_back(bestNcc);
            }
        }
    }

    // NCC 峰值接近阈值但三引擎共识失败：窄范围重扫 + 仅 NCC 放宽验收
    if (consensus.empty()) {
        const double acceptFloor = std::max(threshold * 0.97, threshold - 2.0);
        if (trackedPeakNcc >= acceptFloor ||
            (normalized.scaleMax - normalized.scaleMin) > 0.03) {
            ImageMatchResult recovered = RecoverGlobalNccPeak(srcGray, tplGray, normalized);
            out.debugBestNccPercent = std::max(out.debugBestNccPercent, recovered.score);
            if (recovered.found && recovered.score >= acceptFloor) {
                tryAcceptSeed(recovered, false, true);
                if (consensus.empty()) {
                    consensus.push_back(recovered);
                }
            }
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
