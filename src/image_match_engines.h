#pragma once

#include "image_match.h"

#include <opencv2/core.hpp>

#include <vector>

// Locate with TM_SQDIFF_NORMED. Accept only after pixel-tolerance verification
// (AHK ImageSearch style). Displayed score is min(locate-sim, pixel-agree).
// Optional tplMask (CV_8UC1, 255=opaque) ignores PNG transparent pixels.
// Gradient orientation on the candidate is a cheap uniqueness check.
ImageMatchOutput MatchInGrayMatsMultiVerify(
    const cv::Mat& srcGray, const cv::Mat& tplGray,
    const cv::Mat& srcBgr, const cv::Mat& tplBgr,
    const ImageMatchOptions& opt, int offsetX, int offsetY,
    const cv::Mat& tplMask = cv::Mat());

// SIMD SAD helper kept for diagnostics; acceptance uses pixel-agree, not SAD/255.
double ComputePatchSadSimilarity(const cv::Mat& srcGray, const cv::Mat& tplGray,
                                 int topLeftX, int topLeftY);
