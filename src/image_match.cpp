// image_match.cpp — OpenCV pyramid + multi-scale template matching
//
// Algorithm (参考 MatchTool / OpenCV matchTemplate):
//   1. Convert screen/template to grayscale cv::Mat
//   2. For each scale in [scaleMin, scaleMax]:
//      a. Resize template
//      b. Build image pyramids (cv::buildPyramid)
//      c. Top-level exhaustive TM_CCOEFF_NORMED scan, collect peaks
//      d. Cascade refine through finer pyramid levels (±window search)
//   3. Global NMS across all scales, sort by score, return top N matches

#include "image_match.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

namespace {

struct RawBitmap {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

bool ReadBitmapPixels(HBITMAP bitmap, RawBitmap& out) {
    if (!bitmap) return false;
    BITMAP bm{};
    if (!GetObjectW(bitmap, sizeof(bm), &bm)) return false;
    out.width = bm.bmWidth;
    out.height = bm.bmHeight;
    if (out.width <= 0 || out.height <= 0) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = out.width;
    bi.bmiHeader.biHeight = -out.height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.pixels.resize(static_cast<size_t>(out.width) * out.height * 4);

    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    const int lines = GetDIBits(dc, bitmap, 0, out.height, out.pixels.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return lines > 0;
}

cv::Mat BitmapToGrayMat(HBITMAP bitmap) {
    RawBitmap raw{};
    if (!ReadBitmapPixels(bitmap, raw)) return {};
    cv::Mat bgra(raw.height, raw.width, CV_8UC4, raw.pixels.data());
    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    return gray.clone();
}

cv::Mat CropGrayMat(const cv::Mat& full, int x, int y, int w, int h) {
    const cv::Rect roi(x, y, w, h);
    if (roi.x < 0 || roi.y < 0 || roi.x + roi.width > full.cols || roi.y + roi.height > full.rows)
        return {};
    return full(roi).clone();
}

int CalcPyramidLevels(int tplW, int tplH) {
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

void BuildPyramid(const cv::Mat& src, std::vector<cv::Mat>& out, int levels) {
    out.clear();
    out.reserve(static_cast<size_t>(levels) + 1);
    cv::buildPyramid(src, out, levels);
}

void SuppressPeak(cv::Mat& result, cv::Point pt, int tplW, int tplH, double maxOverlap) {
    const int padX = std::max(1, static_cast<int>(tplW * (1.0 - maxOverlap)));
    const int padY = std::max(1, static_cast<int>(tplH * (1.0 - maxOverlap)));
    const int x1 = std::max(0, pt.x - padX);
    const int y1 = std::max(0, pt.y - padY);
    const int x2 = std::min(result.cols - 1, pt.x + padX);
    const int y2 = std::min(result.rows - 1, pt.y + padY);
    cv::rectangle(result, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(-1.0), cv::FILLED);
}

std::vector<std::pair<cv::Point, double>> FindPeaks(
    const cv::Mat& result, double threshold, int tplW, int tplH,
    double maxOverlap, int maxCount) {
    cv::Mat work = result.clone();
    std::vector<std::pair<cv::Point, double>> peaks;
    peaks.reserve(static_cast<size_t>(maxCount));
    for (int i = 0; i < maxCount; ++i) {
        double maxVal = 0.0;
        cv::Point maxLoc;
        cv::minMaxLoc(work, nullptr, &maxVal, nullptr, &maxLoc);
        if (maxVal < threshold) break;
        peaks.emplace_back(maxLoc, maxVal);
        SuppressPeak(work, maxLoc, tplW, tplH, maxOverlap);
    }
    return peaks;
}

ImageMatchResult MakeResult(const cv::Point& tl, int tplW, int tplH, double score01, double scale) {
    ImageMatchResult r{};
    r.found = true;
    r.scale = scale;
    r.score = score01 * 100.0;
    r.topLeftX = tl.x;
    r.topLeftY = tl.y;
    r.bottomRightX = tl.x + tplW;
    r.bottomRightY = tl.y + tplH;
    r.x = tl.x + tplW / 2;
    r.y = tl.y + tplH / 2;
    return r;
}

bool RectsOverlapTooMuch(const ImageMatchResult& a, const ImageMatchResult& b, double maxOverlap) {
    const int ax1 = a.topLeftX;
    const int ay1 = a.topLeftY;
    const int ax2 = a.bottomRightX;
    const int ay2 = a.bottomRightY;
    const int bx1 = b.topLeftX;
    const int by1 = b.topLeftY;
    const int bx2 = b.bottomRightX;
    const int by2 = b.bottomRightY;

    const int ix1 = std::max(ax1, bx1);
    const int iy1 = std::max(ay1, by1);
    const int ix2 = std::min(ax2, bx2);
    const int iy2 = std::min(ay2, by2);
    if (ix2 <= ix1 || iy2 <= iy1) return false;

    const double inter = static_cast<double>(ix2 - ix1) * (iy2 - iy1);
    const double areaA = static_cast<double>(ax2 - ax1) * (ay2 - ay1);
    const double areaB = static_cast<double>(bx2 - bx1) * (by2 - by1);
    const double minArea = std::min(areaA, areaB);
    if (minArea <= 0.0) return false;
    return (inter / minArea) > maxOverlap;
}

std::vector<ImageMatchResult> GlobalNms(std::vector<ImageMatchResult> matches,
                                        double maxOverlap, int maxMatches) {
    std::sort(matches.begin(), matches.end(),
              [](const ImageMatchResult& a, const ImageMatchResult& b) { return a.score > b.score; });

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

ImageMatchResult RefineAtLevel(
    const cv::Mat& src, const cv::Mat& tmpl, cv::Point predTL,
    int searchRadius, double threshold01) {
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
    cv::matchTemplate(roiSrc, tmpl, result, cv::TM_CCOEFF_NORMED);

    double maxVal = 0.0;
    cv::Point maxLoc;
    cv::minMaxLoc(result, nullptr, &maxVal, nullptr, &maxLoc);
    if (maxVal < threshold01) return r;

    return MakeResult(cv::Point(x1 + maxLoc.x, y1 + maxLoc.y), tplW, tplH, maxVal, 1.0);
}

std::vector<ImageMatchResult> MatchSingleScale(
    const cv::Mat& srcGray, const cv::Mat& tplGray,
    double scale, const ImageMatchOptions& opt) {
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

    const int levels = CalcPyramidLevels(tplW, tplH);
    if (levels <= 0) {
        cv::Mat result;
        cv::matchTemplate(srcGray, scaledTpl, result, cv::TM_CCOEFF_NORMED);
        const double threshold01 = opt.thresholdPercent / 100.0;
        const auto peaks = FindPeaks(result, threshold01, tplW, tplH, opt.maxOverlap, opt.maxMatches);
        for (const auto& [pt, score] : peaks) {
            auto m = MakeResult(pt, tplW, tplH, score, scale);
            m.scale = scale;
            results.push_back(m);
        }
        return results;
    }

    std::vector<cv::Mat> srcPyr;
    std::vector<cv::Mat> tplPyr;
    BuildPyramid(srcGray, srcPyr, levels);
    BuildPyramid(scaledTpl, tplPyr, levels);

    const int top = levels;
    const double threshold01 = opt.thresholdPercent / 100.0;
    const double coarseThresh = std::max(0.3, threshold01 * 0.75);

    cv::Mat topResult;
    cv::matchTemplate(srcPyr[top], tplPyr[top], topResult, cv::TM_CCOEFF_NORMED);
    const int topTplW = tplPyr[top].cols;
    const int topTplH = tplPyr[top].rows;
    auto topPeaks = FindPeaks(topResult, coarseThresh, topTplW, topTplH, opt.maxOverlap, opt.maxMatches);
    if (topPeaks.empty()) {
        topPeaks = FindPeaks(topResult, 0.3, topTplW, topTplH, opt.maxOverlap, opt.maxMatches);
    }
    if (topPeaks.empty()) return results;

    for (const auto& [topPt, topScore] : topPeaks) {
        cv::Point loc = topPt;
        double bestScore = topScore;
        int bestLevel = top;

        for (int lvl = top - 1; lvl >= 0; --lvl) {
            loc.x *= 2;
            loc.y *= 2;
            const int R = (lvl == 0) ? 32 : 20;
            const double lvlThresh = (lvl == 0) ? threshold01 : 0.0;
            ImageMatchResult refined = RefineAtLevel(srcPyr[lvl], tplPyr[lvl], loc, R, lvlThresh);
            if (refined.found) {
                loc = cv::Point(refined.topLeftX, refined.topLeftY);
                bestScore = refined.score / 100.0;
                bestLevel = lvl;
            } else if (lvl == 0) {
                bestLevel = -1;
                break;
            }
        }

        if (bestLevel < 0) continue;

        if (bestLevel > 0) {
            const int R = std::min(64, 16 << bestLevel);
            ImageMatchResult rescore = RefineAtLevel(srcGray, scaledTpl, loc, R, threshold01);
            if (!rescore.found) continue;
            loc = cv::Point(rescore.topLeftX, rescore.topLeftY);
            bestScore = rescore.score / 100.0;
        }

        auto m = MakeResult(loc, tplW, tplH, bestScore, scale);
        results.push_back(m);
    }

    return results;
}

ImageMatchOutput MatchInGrayMats(const cv::Mat& srcGray, const cv::Mat& tplGray,
                                 const ImageMatchOptions& opt, int offsetX, int offsetY) {
    ImageMatchOutput out{};
    const auto t0 = std::chrono::steady_clock::now();

    if (srcGray.empty() || tplGray.empty()) {
        out.elapsedMs = 0;
        return out;
    }

    ImageMatchOptions normalized = opt;
    normalized.thresholdPercent = std::clamp(normalized.thresholdPercent, 1.0, 100.0);
    normalized.scaleMin = std::max(0.1, normalized.scaleMin);
    normalized.scaleMax = std::max(normalized.scaleMin, normalized.scaleMax);
    normalized.scaleStep = std::max(0.01, normalized.scaleStep);
    normalized.maxMatches = std::clamp(normalized.maxMatches, 1, 200);
    normalized.maxOverlap = std::clamp(normalized.maxOverlap, 0.0, 0.95);

    std::vector<ImageMatchResult> all;
    for (double scale = normalized.scaleMin; scale <= normalized.scaleMax + 1e-9; scale += normalized.scaleStep) {
        auto batch = MatchSingleScale(srcGray, tplGray, scale, normalized);
        all.insert(all.end(), batch.begin(), batch.end());
    }

    all = GlobalNms(std::move(all), normalized.maxOverlap, normalized.maxMatches);

    for (auto& m : all) {
        m.topLeftX += offsetX;
        m.topLeftY += offsetY;
        m.bottomRightX += offsetX;
        m.bottomRightY += offsetY;
        m.x += offsetX;
        m.y += offsetY;
    }

    out.matches = std::move(all);
    out.found = !out.matches.empty();
    const auto t1 = std::chrono::steady_clock::now();
    out.elapsedMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    return out;
}

ImageMatchOptions NormalizeLegacyOptions(double thresholdPercent, double scale, double scaleMax) {
    ImageMatchOptions opt;
    opt.thresholdPercent = thresholdPercent;
    opt.scaleMin = scale;
    opt.scaleMax = (scaleMax > 0.0) ? scaleMax : scale;
    return opt;
}

cv::Mat ImReadW(const std::wstring& path) {
    if (path.empty()) return {};
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) return {};
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return {};
    }
    const long size = ftell(fp);
    if (size <= 0) {
        fclose(fp);
        return {};
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return {};
    }
    std::vector<uchar> buf(static_cast<size_t>(size));
    if (fread(buf.data(), 1, buf.size(), fp) != buf.size()) {
        fclose(fp);
        return {};
    }
    fclose(fp);
    return cv::imdecode(buf, cv::IMREAD_COLOR);
}

HBITMAP BgrMatToHBitmap(const cv::Mat& bgr) {
    if (bgr.empty()) return nullptr;
    cv::Mat bgra;
    cv::cvtColor(bgr, bgra, cv::COLOR_BGR2BGRA);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bgra.cols;
    bi.bmiHeader.biHeight = -bgra.rows;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!bmp || !bits) return nullptr;

    const size_t bytes = static_cast<size_t>(bgra.cols) * bgra.rows * 4;
    memcpy(bits, bgra.data, bytes);
    return bmp;
}

}  // namespace

HBITMAP LoadBitmapFromFile(const std::wstring& path) {
    if (path.empty()) return nullptr;
    const cv::Mat img = ImReadW(path);
    if (!img.empty()) return BgrMatToHBitmap(img);
    return static_cast<HBITMAP>(LoadImageW(nullptr, path.c_str(), IMAGE_BITMAP, 0, 0,
                                           LR_LOADFROMFILE | LR_CREATEDIBSECTION));
}

bool SaveBitmapToFile(HBITMAP bitmap, const std::wstring& path) {
    if (!bitmap || path.empty()) return false;
    RawBitmap data{};
    if (!ReadBitmapPixels(bitmap, data)) return false;

    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = data.width;
    ih.biHeight = data.height;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = static_cast<DWORD>(data.pixels.size());
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + ih.biSizeImage;

    std::vector<uint8_t> bottomUp(data.pixels.size());
    const int rb = data.width * 4;
    for (int y = 0; y < data.height; ++y) {
        memcpy(bottomUp.data() + static_cast<size_t>(y) * rb,
               data.pixels.data() + static_cast<size_t>(data.height - 1 - y) * rb,
               rb);
    }

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || !fp) return false;
    fwrite(&fh, sizeof(fh), 1, fp);
    fwrite(&ih, sizeof(ih), 1, fp);
    fwrite(bottomUp.data(), bottomUp.size(), 1, fp);
    fclose(fp);
    return true;
}

void DeleteBitmapHandle(HBITMAP bitmap) {
    if (bitmap) DeleteObject(bitmap);
}

HBITMAP CaptureScreenRegion(int x1, int y1, int x2, int y2) {
    const int L = std::min(x1, x2);
    const int T = std::min(y1, y2);
    const int R = std::max(x1, x2);
    const int B = std::max(y1, y2);
    const int w = std::max(1, R - L);
    const int h = std::max(1, B - T);

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    HBITMAP bmp = CreateCompatibleBitmap(screenDc, w, h);
    HGDIOBJ old = SelectObject(memDc, bmp);
    BitBlt(memDc, 0, 0, w, h, screenDc, L, T, SRCCOPY);
    SelectObject(memDc, old);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    return bmp;
}

void GetVirtualScreenRect(int& x, int& y, int& w, int& h) {
    x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

ImageMatchOutput FindTemplateInFrozenScreenMulti(
    HBITMAP frozenScreen, int virtX, int virtY,
    int searchX1, int searchY1, int searchX2, int searchY2,
    HBITMAP templateBmp, const ImageMatchOptions& options) {
    ImageMatchOutput out{};
    if (!frozenScreen || !templateBmp) return out;

    const cv::Mat full = BitmapToGrayMat(frozenScreen);
    const cv::Mat templ = BitmapToGrayMat(templateBmp);
    if (full.empty() || templ.empty()) return out;

    const int left = std::min(searchX1, searchX2);
    const int top = std::min(searchY1, searchY2);
    const int right = std::max(searchX1, searchX2);
    const int bottom = std::max(searchY1, searchY2);
    const int rw = std::max(1, right - left);
    const int rh = std::max(1, bottom - top);

    const int cx = left - virtX;
    const int cy = top - virtY;
    cv::Mat crop = CropGrayMat(full, cx, cy, rw, rh);
    if (crop.empty()) return out;

    return MatchInGrayMats(crop, templ, options, left, top);
}

ImageMatchOutput FindTemplateOnScreenMulti(
    int searchX1, int searchY1, int searchX2, int searchY2,
    HBITMAP templateBmp, const ImageMatchOptions& options) {
    ImageMatchOutput out{};
    if (!templateBmp) return out;

    const int left = std::min(searchX1, searchX2);
    const int top = std::min(searchY1, searchY2);
    const int right = std::max(searchX1, searchX2);
    const int bottom = std::max(searchY1, searchY2);

    HBITMAP regionBmp = CaptureScreenRegion(left, top, right, bottom);
    if (!regionBmp) return out;

    const cv::Mat screen = BitmapToGrayMat(regionBmp);
    const cv::Mat templ = BitmapToGrayMat(templateBmp);
    DeleteBitmapHandle(regionBmp);

    if (screen.empty() || templ.empty()) return out;
    return MatchInGrayMats(screen, templ, options, left, top);
}

ImageMatchResult FindTemplateInFrozenScreen(
    HBITMAP frozenScreen, int virtX, int virtY,
    int searchX1, int searchY1, int searchX2, int searchY2,
    HBITMAP templateBmp, double thresholdPercent, double scale,
    int* outTemplateW, int* outTemplateH, double scaleMax) {
    ImageMatchOptions opt = NormalizeLegacyOptions(thresholdPercent, scale, scaleMax);
    if (outTemplateW || outTemplateH) {
        cv::Mat templ = BitmapToGrayMat(templateBmp);
        if (!templ.empty()) {
            const double useScale = (opt.scaleMin + opt.scaleMax) * 0.5;
            if (std::abs(useScale - 1.0) > 0.001) {
                cv::resize(templ, templ, cv::Size(), useScale, useScale, cv::INTER_AREA);
            }
            if (outTemplateW) *outTemplateW = templ.cols;
            if (outTemplateH) *outTemplateH = templ.rows;
        }
    }

    ImageMatchOutput output = FindTemplateInFrozenScreenMulti(
        frozenScreen, virtX, virtY, searchX1, searchY1, searchX2, searchY2, templateBmp, opt);
    if (output.matches.empty()) return ImageMatchResult{};
    return output.matches.front();
}

ImageMatchResult FindTemplateOnScreen(
    int searchX1, int searchY1, int searchX2, int searchY2,
    HBITMAP templateBmp, double thresholdPercent, double scale, double scaleMax) {
    ImageMatchOptions opt = NormalizeLegacyOptions(thresholdPercent, scale, scaleMax);
    ImageMatchOutput output = FindTemplateOnScreenMulti(
        searchX1, searchY1, searchX2, searchY2, templateBmp, opt);
    if (output.matches.empty()) return ImageMatchResult{};
    return output.matches.front();
}

void SendMouseWheel(int steps, bool vertical, bool horizontal, bool positive) {
    if (steps <= 0) return;
    const int delta = (positive ? 1 : -1) * WHEEL_DELTA;
    INPUT input{};
    input.type = INPUT_MOUSE;
    for (int i = 0; i < steps; ++i) {
        input.mi.dwFlags = 0;
        input.mi.mouseData = 0;
        if (vertical) {
            input.mi.dwFlags = MOUSEEVENTF_WHEEL;
            input.mi.mouseData = static_cast<DWORD>(delta);
        }
        if (horizontal) {
            input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
            input.mi.mouseData = static_cast<DWORD>(delta);
        }
        if (input.mi.dwFlags) SendInput(1, &input, sizeof(INPUT));
    }
}
