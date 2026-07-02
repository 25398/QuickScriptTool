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

std::vector<ImageMatchResult> RunEngineAllScales(
    const cv::Mat& srcGray, const cv::Mat& tplGray,
    const ImageMatchOptions& opt, cv::TemplateMatchModes mode) {
    std::vector<ImageMatchResult> all;
    for (double scale = opt.scaleMin; scale <= opt.scaleMax + 1e-9; scale += opt.scaleStep) {
        auto batch = MatchSingleScale(srcGray, tplGray, scale, opt, mode);
        all.insert(all.end(), batch.begin(), batch.end());
    }
    return GlobalNms(std::move(all), opt.maxOverlap, opt.maxMatches);
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
    const PatchVerifierContext verifier = PatchVerifierContext::Build(tplGray, tplBgr);
    const double threshold = normalized.thresholdPercent;

    std::vector<std::future<std::vector<ImageMatchResult>>> futures;
    futures.reserve(std::size(kTemplateEngines));
    for (const auto& spec : kTemplateEngines) {
        futures.push_back(std::async(std::launch::async, [&srcGray, &tplGray, normalized, spec]() {
            return RunEngineAllScales(srcGray, tplGray, normalized, spec.mode);
        }));
    }

    std::vector<std::vector<ImageMatchResult>> engineResults;
    engineResults.reserve(futures.size());
    for (auto& fut : futures) {
        engineResults.push_back(fut.get());
    }

    if (engineResults.empty()) {
        const auto t1 = std::chrono::steady_clock::now();
        out.elapsedMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
        return out;
    }

    bool anyCandidates = false;
    for (const auto& results : engineResults) {
        if (!results.empty()) {
            anyCandidates = true;
            break;
        }
    }
    if (!anyCandidates) {
        const auto t1 = std::chrono::steady_clock::now();
        out.elapsedMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
        return out;
    }

    const double bestNccScore = BestNccScore(engineResults[0]);

    std::vector<ImageMatchResult> consensus;
    consensus.reserve(static_cast<size_t>(normalized.maxMatches));

    auto tryAcceptSeed = [&](const ImageMatchResult& seed) {
        std::vector<const ImageMatchResult*> perEngineBest(engineResults.size(), nullptr);
        for (size_t e = 0; e < engineResults.size(); ++e) {
            perEngineBest[e] =
                FindAgreeingMatch(engineResults[e], seed.x, seed.y, tolerancePx);
            if (!perEngineBest[e]) return;
            if (perEngineBest[e]->score < threshold) return;
        }

        const double sadScore =
            ComputePatchSadSimilarity(srcGray, tplGray, seed.topLeftX, seed.topLeftY);
        if (sadScore < threshold) return;

        const double edgeScore =
            ComputePatchEdgeSimilarity(srcGray, verifier, seed.topLeftX, seed.topLeftY);
        const double colorScore =
            ComputePatchColorSimilarity(srcBgr, verifier, seed.topLeftX, seed.topLeftY);

        const double nccScore = perEngineBest[0]->score;
        const bool isTopNccPeak = (bestNccScore - nccScore) < 1.0;
        const double edgeNeed = isTopNccPeak ? threshold : (threshold + kEdgeBoostPercent);
        const double colorNeed = isTopNccPeak ? threshold : (threshold + kColorBoostPercent);

        if (edgeScore < edgeNeed) return;
        if (verifier.hasColor && colorScore < colorNeed) return;

        for (const auto& existing : consensus) {
            if (PositionsAgree(existing.x, existing.y, seed.x, seed.y, tolerancePx)) return;
        }

        double scoreSum = sadScore + edgeScore;
        if (verifier.hasColor) scoreSum += colorScore;
        for (const auto* m : perEngineBest) scoreSum += m->score;
        const int divisor = static_cast<int>(perEngineBest.size()) + 2 + (verifier.hasColor ? 1 : 0);
        const double avgScore = scoreSum / static_cast<double>(divisor);

        ImageMatchResult merged = seed;
        merged.score = avgScore;
        consensus.push_back(merged);
    };

    for (const auto& results : engineResults) {
        for (const auto& seed : results) {
            tryAcceptSeed(seed);
            if (static_cast<int>(consensus.size()) >= normalized.maxMatches) break;
        }
        if (static_cast<int>(consensus.size()) >= normalized.maxMatches) break;
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
