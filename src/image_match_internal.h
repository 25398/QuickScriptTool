// ──────────────────────────────────────────────────────────────────
// image_match_internal.h — 图像匹配内部辅助函数
// 提供图像金字塔构建、峰值检测与抑制、NMS 和分数归一化等底层组件。
// 所有函数定义在 image_match_internal 命名空间中，inline 实现。
// ──────────────────────────────────────────────────────────────────
#pragma once

#include "image_match.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace image_match_internal {

// 根据模板尺寸计算最大金字塔层级数 (模板缩到 < 8px 为止)
inline int CalcPyramidLevels(int tplW, int tplH) {
    int levels = 0;
    int w = tplW;
    int h = tplH;
    while (w >= 8 && h >= 8) {
        w /= 2;
        h /= 2;
        ++levels;
    }
    return levels;
}

// 使用 OpenCV 构建图像金字塔
inline void BuildPyramid(const cv::Mat& src, std::vector<cv::Mat>& out, int levels) {
    out.clear();
    out.reserve(static_cast<size_t>(levels) + 1);
    cv::buildPyramid(src, out, levels);
}

inline void SuppressPeak(cv::Mat& result, cv::Point pt, int tplW, int tplH, double maxOverlap) {
    const int padX = std::max(1, static_cast<int>(tplW * (1.0 - maxOverlap)));
    const int padY = std::max(1, static_cast<int>(tplH * (1.0 - maxOverlap)));
    const int x1 = std::max(0, pt.x - padX);
    const int y1 = std::max(0, pt.y - padY);
    const int x2 = std::min(result.cols - 1, pt.x + padX);
    const int y2 = std::min(result.rows - 1, pt.y + padY);
    cv::rectangle(result, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(-1.0), cv::FILLED);
}

inline std::vector<std::pair<cv::Point, double>> FindPeaks(
    const cv::Mat& result, double threshold, int tplW, int tplH,
    double maxOverlap, int maxCount, bool lowerIsBetter) {
    cv::Mat work = result.clone();
    std::vector<std::pair<cv::Point, double>> peaks;
    peaks.reserve(static_cast<size_t>(maxCount));
    for (int i = 0; i < maxCount; ++i) {
        double extreme = 0.0;
        cv::Point extremeLoc;
        if (lowerIsBetter) {
            cv::minMaxLoc(work, &extreme, nullptr, &extremeLoc, nullptr);
            if (extreme > threshold) break;
        } else {
            cv::minMaxLoc(work, nullptr, &extreme, nullptr, &extremeLoc);
            if (extreme < threshold) break;
        }
        peaks.emplace_back(extremeLoc, extreme);
        SuppressPeak(work, extremeLoc, tplW, tplH, maxOverlap);
    }
    return peaks;
}

inline double RawScoreToSimilarity(double rawScore, cv::TemplateMatchModes mode) {
    switch (mode) {
    case cv::TM_SQDIFF:
    case cv::TM_SQDIFF_NORMED:
        return std::clamp((1.0 - rawScore) * 100.0, 0.0, 100.0);
    default:
        return std::clamp(rawScore * 100.0, 0.0, 100.0);
    }
}

inline double SimilarityThreshold01(double thresholdPercent, cv::TemplateMatchModes mode) {
    const double t = thresholdPercent / 100.0;
    switch (mode) {
    case cv::TM_SQDIFF:
    case cv::TM_SQDIFF_NORMED:
        return 1.0 - t;
    default:
        return t;
    }
}

inline ImageMatchResult MakeResult(const cv::Point& tl, int tplW, int tplH,
                                   double scorePercent, double scale) {
    ImageMatchResult r{};
    r.found = true;
    r.scale = scale;
    r.score = scorePercent;
    r.topLeftX = tl.x;
    r.topLeftY = tl.y;
    r.bottomRightX = tl.x + tplW;
    r.bottomRightY = tl.y + tplH;
    r.x = tl.x + tplW / 2;
    r.y = tl.y + tplH / 2;
    return r;
}

inline bool RectsOverlapTooMuch(const ImageMatchResult& a, const ImageMatchResult& b,
                                double maxOverlap) {
    const int ix1 = std::max(a.topLeftX, b.topLeftX);
    const int iy1 = std::max(a.topLeftY, b.topLeftY);
    const int ix2 = std::min(a.bottomRightX, b.bottomRightX);
    const int iy2 = std::min(a.bottomRightY, b.bottomRightY);
    if (ix2 <= ix1 || iy2 <= iy1) return false;

    const double inter = static_cast<double>(ix2 - ix1) * (iy2 - iy1);
    const double areaA = static_cast<double>(a.bottomRightX - a.topLeftX) *
                         (a.bottomRightY - a.topLeftY);
    const double areaB = static_cast<double>(b.bottomRightX - b.topLeftX) *
                         (b.bottomRightY - b.topLeftY);
    const double minArea = std::min(areaA, areaB);
    if (minArea <= 0.0) return false;
    return (inter / minArea) > maxOverlap;
}

inline std::vector<ImageMatchResult> GlobalNms(std::vector<ImageMatchResult> matches,
                                               double maxOverlap, int maxMatches) {
    std::sort(matches.begin(), matches.end(),
              [](const ImageMatchResult& a, const ImageMatchResult& b) {
                  return a.score > b.score;
              });

    std::vector<ImageMatchResult> kept;
    kept.reserve(static_cast<size_t>(maxMatches));
    for (const auto& m : matches) {
        bool dup = false;
        for (const auto& k : kept) {
            if (RectsOverlapTooMuch(m, k, maxOverlap)) {
                dup = true;
                break;
            }
        }
        if (!dup) kept.push_back(m);
        if (static_cast<int>(kept.size()) >= maxMatches) break;
    }
    return kept;
}

inline ImageMatchResult RefineAtLevel(
    const cv::Mat& src, const cv::Mat& tmpl, cv::Point predTL,
    int searchRadius, double threshold01, cv::TemplateMatchModes mode) {
    ImageMatchResult r{};
    const int tplW = tmpl.cols;
    const int tplH = tmpl.rows;
    const int maxX = src.cols - tplW;
    const int maxY = src.rows - tplH;
    if (maxX < 0 || maxY < 0) return r;

    const int x1 = std::max(0, predTL.x - searchRadius);
    const int y1 = std::max(0, predTL.y - searchRadius);
    const int x2 = std::min(maxX, predTL.x + searchRadius);
    const int y2 = std::min(maxY, predTL.y + searchRadius);
    if (x1 > x2 || y1 > y2) return r;

    const cv::Rect roi(x1, y1, x2 - x1 + tplW, y2 - y1 + tplH);
    cv::Mat roiSrc = src(roi);
    cv::Mat result;
    cv::matchTemplate(roiSrc, tmpl, result, mode);

    const bool lowerIsBetter =
        mode == cv::TM_SQDIFF || mode == cv::TM_SQDIFF_NORMED;

    double extreme = 0.0;
    cv::Point extremeLoc;
    if (lowerIsBetter) {
        cv::minMaxLoc(result, &extreme, nullptr, &extremeLoc, nullptr);
        if (extreme > threshold01) return r;
    } else {
        cv::minMaxLoc(result, nullptr, &extreme, nullptr, &extremeLoc);
        if (extreme < threshold01) return r;
    }

    return MakeResult(cv::Point(x1 + extremeLoc.x, y1 + extremeLoc.y), tplW, tplH,
                      RawScoreToSimilarity(extreme, mode), 1.0);
}

inline std::vector<ImageMatchResult> MatchSingleScale(
    const cv::Mat& srcGray, const cv::Mat& tplGray, double scale,
    const ImageMatchOptions& opt, cv::TemplateMatchModes mode) {
    std::vector<ImageMatchResult> results;
    if (srcGray.empty() || tplGray.empty()) return results;

    cv::Mat scaledTpl;
    if (std::abs(scale - 1.0) > 0.001) {
        cv::resize(tplGray, scaledTpl, cv::Size(), scale, scale, cv::INTER_AREA);
    } else {
        scaledTpl = tplGray;
    }

    const int tplW = scaledTpl.cols;
    const int tplH = scaledTpl.rows;
    if (tplW < 4 || tplH < 4 || tplW > srcGray.cols || tplH > srcGray.rows) return results;

    const bool lowerIsBetter =
        mode == cv::TM_SQDIFF || mode == cv::TM_SQDIFF_NORMED;
    const double threshold01 = SimilarityThreshold01(opt.thresholdPercent, mode);

    const int levels = CalcPyramidLevels(tplW, tplH);
    if (levels <= 0) {
        cv::Mat result;
        cv::matchTemplate(srcGray, scaledTpl, result, mode);
        const double coarseThresh = lowerIsBetter
            ? std::min(0.7, threshold01 + 0.15)
            : std::max(0.3, threshold01 * 0.75);
        auto peaks = FindPeaks(result, coarseThresh, tplW, tplH, opt.maxOverlap,
                               opt.maxMatches, lowerIsBetter);
        for (const auto& [pt, rawScore] : peaks) {
            auto m = MakeResult(pt, tplW, tplH, RawScoreToSimilarity(rawScore, mode), scale);
            m.scale = scale;
            if (m.score >= opt.thresholdPercent) results.push_back(m);
        }
        return results;
    }

    std::vector<cv::Mat> srcPyr;
    std::vector<cv::Mat> tplPyr;
    BuildPyramid(srcGray, srcPyr, levels);
    BuildPyramid(scaledTpl, tplPyr, levels);

    const int top = levels;
    const double coarseThresh = lowerIsBetter
        ? std::min(0.7, threshold01 + 0.15)
        : std::max(0.3, threshold01 * 0.75);

    cv::Mat topResult;
    cv::matchTemplate(srcPyr[top], tplPyr[top], topResult, mode);
    const int topTplW = tplPyr[top].cols;
    const int topTplH = tplPyr[top].rows;
    auto topPeaks = FindPeaks(topResult, coarseThresh, topTplW, topTplH,
                              opt.maxOverlap, opt.maxMatches, lowerIsBetter);
    if (topPeaks.empty()) {
        const double fallback = lowerIsBetter ? 0.7 : 0.3;
        topPeaks = FindPeaks(topResult, fallback, topTplW, topTplH,
                             opt.maxOverlap, opt.maxMatches, lowerIsBetter);
    }
    if (topPeaks.empty()) return results;

    for (const auto& [topPt, topScore] : topPeaks) {
        cv::Point loc = topPt;
        double bestScore = RawScoreToSimilarity(topScore, mode);
        int bestLevel = top;

        for (int lvl = top - 1; lvl >= 0; --lvl) {
            loc.x *= 2;
            loc.y *= 2;
            const int R = (lvl == 0) ? 32 : 20;
            const double lvlThresh = (lvl == 0) ? threshold01 : (lowerIsBetter ? 1.0 : 0.0);
            ImageMatchResult refined =
                RefineAtLevel(srcPyr[lvl], tplPyr[lvl], loc, R, lvlThresh, mode);
            if (refined.found) {
                loc = cv::Point(refined.topLeftX, refined.topLeftY);
                bestScore = refined.score;
                bestLevel = lvl;
            } else if (lvl == 0) {
                bestLevel = -1;
                break;
            }
        }

        if (bestLevel < 0) continue;

        if (bestLevel > 0) {
            const int R = std::min(64, 16 << bestLevel);
            ImageMatchResult rescore =
                RefineAtLevel(srcGray, scaledTpl, loc, R, threshold01, mode);
            if (!rescore.found) continue;
            loc = cv::Point(rescore.topLeftX, rescore.topLeftY);
            bestScore = rescore.score;
        }

        if (bestScore < opt.thresholdPercent) continue;
        auto m = MakeResult(loc, tplW, tplH, bestScore, scale);
        results.push_back(m);
    }

    return results;
}

inline int AutoConsensusTolerancePx(int tplW, int tplH) {
    return std::max(5, std::min(tplW, tplH) / 6);
}

inline bool PositionsAgree(int ax, int ay, int bx, int by, int tolerancePx) {
    return std::abs(ax - bx) <= tolerancePx && std::abs(ay - by) <= tolerancePx;
}

}  // namespace image_match_internal
