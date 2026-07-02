#pragma once

#include "image_match.h"

#include <opencv2/core.hpp>

#include <vector>

// Multi-engine consensus matching (see image_match_engines.cpp).
// Inspired by high-performance open-source matchers:
//   - OpenCV TM_CCOEFF_NORMED / TM_SQDIFF_NORMED / TM_CCORR_NORMED
//   - Fastest_Image_Pattern_Matching (SIMD NCC + pyramid)
//   - ImageSearchDLL (SIMD SAD verification)
ImageMatchOutput MatchInGrayMatsMultiVerify(
    const cv::Mat& srcGray, const cv::Mat& tplGray,
    const cv::Mat& srcBgr, const cv::Mat& tplBgr,
    const ImageMatchOptions& opt, int offsetX, int offsetY);

// SIMD-accelerated patch SAD similarity (0-100), used as an extra verifier.
double ComputePatchSadSimilarity(const cv::Mat& srcGray, const cv::Mat& tplGray,
                                 int topLeftX, int topLeftY);
