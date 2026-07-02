// image_match.cpp — OpenCV pyramid + multi-engine consensus template matching
//
// Primary path: 3 independent pyramid matchers (NCC / SQDIFF / CCORR) run in
// parallel, plus SIMD SAD patch verification. A location is accepted only when
// all engines agree within tolerance.

#include "image_match.h"

#include "image_match_engines.h"
#include "image_match_internal.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {

using namespace image_match_internal;

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

cv::Mat BitmapToBgrMat(HBITMAP bitmap) {
    RawBitmap raw{};
    if (!ReadBitmapPixels(bitmap, raw)) return {};
    cv::Mat bgra(raw.height, raw.width, CV_8UC4, raw.pixels.data());
    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    return bgr.clone();
}

cv::Mat BitmapToGrayMat(HBITMAP bitmap) {
    const cv::Mat bgr = BitmapToBgrMat(bitmap);
    if (bgr.empty()) return {};
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    return gray;
}

cv::Mat CropBgrMat(const cv::Mat& full, int x, int y, int w, int h) {
    const cv::Rect roi(x, y, w, h);
    if (roi.x < 0 || roi.y < 0 || roi.x + roi.width > full.cols || roi.y + roi.height > full.rows)
        return {};
    return full(roi).clone();
}

cv::Mat CropGrayMat(const cv::Mat& full, int x, int y, int w, int h) {
    const cv::Rect roi(x, y, w, h);
    if (roi.x < 0 || roi.y < 0 || roi.x + roi.width > full.cols || roi.y + roi.height > full.rows)
        return {};
    return full(roi).clone();
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

    const cv::Mat fullBgr = BitmapToBgrMat(frozenScreen);
    const cv::Mat templBgr = BitmapToBgrMat(templateBmp);
    if (fullBgr.empty() || templBgr.empty()) return out;

    cv::Mat fullGray;
    cv::Mat templGray;
    cv::cvtColor(fullBgr, fullGray, cv::COLOR_BGR2GRAY);
    cv::cvtColor(templBgr, templGray, cv::COLOR_BGR2GRAY);

    const int left = std::min(searchX1, searchX2);
    const int top = std::min(searchY1, searchY2);
    const int right = std::max(searchX1, searchX2);
    const int bottom = std::max(searchY1, searchY2);
    const int rw = std::max(1, right - left);
    const int rh = std::max(1, bottom - top);

    const int cx = left - virtX;
    const int cy = top - virtY;
    cv::Mat cropGray = CropGrayMat(fullGray, cx, cy, rw, rh);
    cv::Mat cropBgr = CropBgrMat(fullBgr, cx, cy, rw, rh);
    if (cropGray.empty()) return out;

    return MatchInGrayMatsMultiVerify(cropGray, templGray, cropBgr, templBgr, options, left, top);
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

    const cv::Mat screenBgr = BitmapToBgrMat(regionBmp);
    const cv::Mat templBgr = BitmapToBgrMat(templateBmp);
    DeleteBitmapHandle(regionBmp);

    if (screenBgr.empty() || templBgr.empty()) return out;

    cv::Mat screenGray;
    cv::Mat templGray;
    cv::cvtColor(screenBgr, screenGray, cv::COLOR_BGR2GRAY);
    cv::cvtColor(templBgr, templGray, cv::COLOR_BGR2GRAY);

    return MatchInGrayMatsMultiVerify(screenGray, templGray, screenBgr, templBgr, options, left, top);
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
