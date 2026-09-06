// image_match.cpp — OpenCV pyramid + multi-engine consensus template matching
//
// Primary path: 3 independent pyramid matchers (NCC / SQDIFF / CCORR) run in
// parallel, plus SIMD SAD patch verification. A location is accepted only when
// all engines agree within tolerance.

#include "image_match.h"

#include "image_match_engines.h"
#include "image_match_internal.h"
#include "input/mouse_input_backend.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
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

bool SaveCroppedTemplateRegion(const std::wstring& srcPath,
    int L, int T, int R, int B, const std::wstring& destPath) {
    if (srcPath.empty() || destPath.empty()) return false;
    const int cw = R - L;
    const int ch = B - T;
    if (cw <= 0 || ch <= 0) return false;
    const cv::Mat full = ImReadW(srcPath);
    if (full.empty()) return false;
    const cv::Mat crop = CropBgrMat(full, L, T, cw, ch);
    if (crop.empty()) return false;
    HBITMAP bmp = BgrMatToHBitmap(crop);
    if (!bmp) return false;
    const bool ok = SaveBitmapToFile(bmp, destPath);
    DeleteBitmapHandle(bmp);
    return ok;
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
    if (!screenDc) return nullptr;
    HDC memDc = CreateCompatibleDC(screenDc);
    if (!memDc) {
        ReleaseDC(nullptr, screenDc);
        return nullptr;
    }
    HBITMAP bmp = CreateCompatibleBitmap(screenDc, w, h);
    if (!bmp) {
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return nullptr;
    }
    HGDIOBJ old = SelectObject(memDc, bmp);
    // CAPTUREBLT：把分层窗口（输入法候选框/组字框、微信截图等可见的小窗）也截进来；
    // 普通 SRCCOPY 会漏掉这些 WS_EX_LAYERED 窗口，导致 Agent 截图看不到输入法状态。
    const BOOL ok = BitBlt(memDc, 0, 0, w, h, screenDc, L, T, SRCCOPY | CAPTUREBLT);
    SelectObject(memDc, old);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    if (!ok) {
        DeleteObject(bmp);
        return nullptr;
    }
    return bmp;
}

void GetVirtualScreenRect(int& x, int& y, int& w, int& h) {
    struct Ctx { RECT r{}; bool any = false; };
    Ctx ctx;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hm, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            MONITORINFO mi{ sizeof(mi) };
            if (!GetMonitorInfoW(hm, &mi)) return TRUE;
            if (!c->any) { c->r = mi.rcMonitor; c->any = true; }
            else UnionRect(&c->r, &c->r, &mi.rcMonitor);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    if (ctx.any) {
        x = ctx.r.left;
        y = ctx.r.top;
        w = ctx.r.right - ctx.r.left;
        h = ctx.r.bottom - ctx.r.top;
        return;
    }
    x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

HBITMAP CaptureVirtualScreen(int& outX, int& outY) {
    int w = 0, h = 0;
    GetVirtualScreenRect(outX, outY, w, h);
    return CaptureScreenRegion(outX, outY, outX + w, outY + h);
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
    for (int i = 0; i < steps; ++i) {
        if (vertical) MouseInputRouter::Instance().Wheel(delta, false);
        if (horizontal) MouseInputRouter::Instance().Wheel(delta, true);
    }
}

namespace {

bool RectsOverlapOrNear(const ScreenChangeRoi& a, const ScreenChangeRoi& b, int pad) {
    return !(a.x2 + pad < b.x1 || b.x2 + pad < a.x1 || a.y2 + pad < b.y1 || b.y2 + pad < a.y1);
}

ScreenChangeRoi MergeRoi(const ScreenChangeRoi& a, const ScreenChangeRoi& b) {
    ScreenChangeRoi m;
    m.x1 = (std::min)(a.x1, b.x1);
    m.y1 = (std::min)(a.y1, b.y1);
    m.x2 = (std::max)(a.x2, b.x2);
    m.y2 = (std::max)(a.y2, b.y2);
    m.areaPx = (std::max)(0, m.x2 - m.x1) * (std::max)(0, m.y2 - m.y1);
    return m;
}

}  // namespace

namespace {

cv::Mat BuildChangeMask(const cv::Mat& ma, const cv::Mat& mb, int channelTol) {
    cv::Mat diff;
    cv::absdiff(ma, mb, diff);
    std::vector<cv::Mat> ch;
    cv::split(diff, ch);
    cv::Mat maxCh;
    cv::max(ch[0], ch[1], maxCh);
    cv::max(maxCh, ch[2], maxCh);
    cv::Mat mask;
    cv::threshold(maxCh, mask, channelTol, 255, cv::THRESH_BINARY);
    return mask;
}

double CellChangeFraction(const cv::Mat& mask, int x0, int y0, int x1, int y1) {
    x0 = std::clamp(x0, 0, mask.cols);
    y0 = std::clamp(y0, 0, mask.rows);
    x1 = std::clamp(x1, 0, mask.cols);
    y1 = std::clamp(y1, 0, mask.rows);
    if (x1 <= x0 || y1 <= y0) return 0.0;
    const cv::Mat cell = mask(cv::Rect(x0, y0, x1 - x0, y1 - y0));
    const int nz = cv::countNonZero(cell);
    const int tot = cell.rows * cell.cols;
    return tot > 0 ? static_cast<double>(nz) / static_cast<double>(tot) : 0.0;
}

void ApplyBusyMaskToChangeMask(cv::Mat& changeMask, const ScreenBusyMask* busy) {
    if (!busy || !busy->valid() || changeMask.empty()) return;
    if (busy->width != changeMask.cols || busy->height != changeMask.rows) return;
    for (int y = 0; y < changeMask.rows; ++y) {
        uint8_t* row = changeMask.ptr<uint8_t>(y);
        for (int x = 0; x < changeMask.cols; ++x) {
            if (row[x] && busy->isBusyPixel(x, y)) row[x] = 0;
        }
    }
}

bool RoiMostlyBusy(const ScreenChangeRoi& r, const ScreenBusyMask* busy, double thr = 0.55) {
    if (!busy || !busy->valid()) return false;
    int cells = 0, busyCells = 0;
    for (int y = r.y1; y < r.y2; y += busy->cellH) {
        for (int x = r.x1; x < r.x2; x += busy->cellW) {
            ++cells;
            if (busy->isBusyPixel(x, y)) ++busyCells;
        }
    }
    return cells > 0 && static_cast<double>(busyCells) / static_cast<double>(cells) >= thr;
}

}  // namespace

ScreenBusyMask BuildBusyMaskFromTripleFrames(
    HBITMAP f0, HBITMAP f1, HBITMAP f2,
    int channelTol, int cellSize, double cellChangeFrac) {
    ScreenBusyMask out;
    if (!f0 || !f1 || !f2) return out;
    channelTol = std::clamp(channelTol, 0, 64);
    cellSize = std::clamp(cellSize, 16, 96);
    cellChangeFrac = std::clamp(cellChangeFrac, 0.02, 0.5);

    const cv::Mat m0 = BitmapToBgrMat(f0);
    const cv::Mat m1 = BitmapToBgrMat(f1);
    const cv::Mat m2 = BitmapToBgrMat(f2);
    if (m0.empty() || m1.empty() || m2.empty()) return out;
    if (m0.size() != m1.size() || m1.size() != m2.size()) return out;

    out.width = m0.cols;
    out.height = m0.rows;
    out.cellW = cellSize;
    out.cellH = cellSize;
    out.cols = (out.width + out.cellW - 1) / out.cellW;
    out.rows = (out.height + out.cellH - 1) / out.cellH;
    out.busy.assign(static_cast<size_t>(out.cols * out.rows), 0);

    const cv::Mat d01 = BuildChangeMask(m0, m1, channelTol);
    const cv::Mat d12 = BuildChangeMask(m1, m2, channelTol);

    int busyCount = 0;
    for (int r = 0; r < out.rows; ++r) {
        for (int c = 0; c < out.cols; ++c) {
            const int x0 = c * out.cellW;
            const int y0 = r * out.cellH;
            const int x1 = (std::min)(out.width, x0 + out.cellW);
            const int y1 = (std::min)(out.height, y0 + out.cellH);
            const double f01 = CellChangeFraction(d01, x0, y0, x1, y1);
            const double f12 = CellChangeFraction(d12, x0, y0, x1, y1);
            // 两段连续差分都高 → 持续运动（视频），而非一次性 UI 跳变
            if (f01 >= cellChangeFrac && f12 >= cellChangeFrac) {
                out.busy[static_cast<size_t>(r * out.cols + c)] = 1;
                ++busyCount;
            }
        }
    }
    const int totalCells = out.cols * out.rows;
    out.busyCoverageRatio = totalCells > 0
        ? static_cast<double>(busyCount) / static_cast<double>(totalCells) : 0.0;
    return out;
}

ScreenChangeDiffResult DiffBitmapsChangedRegions(
    HBITMAP a, HBITMAP b, int channelTol, int minBlobArea, int maxRois,
    const ScreenBusyMask* busyMask) {
    ScreenChangeDiffResult out;
    if (!a || !b) return out;
    channelTol = std::clamp(channelTol, 0, 64);
    minBlobArea = std::max(1, minBlobArea);
    maxRois = std::clamp(maxRois, 1, 32);

    const cv::Mat ma = BitmapToBgrMat(a);
    const cv::Mat mb = BitmapToBgrMat(b);
    if (ma.empty() || mb.empty()) return out;
    out.ok = true;
    if (ma.cols != mb.cols || ma.rows != mb.rows) {
        out.sameSize = false;
        out.nearlyIdentical = false;
        out.changedRatio = 1.0;
        out.rawChangedRatio = 1.0;
        return out;
    }
    out.sameSize = true;
    if (busyMask && busyMask->valid())
        out.busyCoverageRatio = busyMask->busyCoverageRatio;

    cv::Mat mask = BuildChangeMask(ma, mb, channelTol);
    const int total = mask.rows * mask.cols;
    const int rawChanged = cv::countNonZero(mask);
    out.rawChangedRatio = total > 0
        ? static_cast<double>(rawChanged) / static_cast<double>(total) : 0.0;

    ApplyBusyMaskToChangeMask(mask, busyMask);
    const int structChanged = cv::countNonZero(mask);
    out.changedRatio = total > 0
        ? static_cast<double>(structChanged) / static_cast<double>(total) : 0.0;
    out.nearlyIdentical = (structChanged == 0);
    out.onlyDynamicChanged = (out.rawChangedRatio >= 0.01)
        && (out.changedRatio < 0.003)
        && (out.busyCoverageRatio >= 0.02 || out.rawChangedRatio - out.changedRatio >= 0.008);

    if (out.nearlyIdentical) return out;

    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5));
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    std::vector<ScreenChangeRoi> raw;
    raw.reserve(contours.size());
    for (const auto& c : contours) {
        const cv::Rect br = cv::boundingRect(c);
        if (br.area() < minBlobArea) continue;
        ScreenChangeRoi r;
        r.x1 = br.x;
        r.y1 = br.y;
        r.x2 = br.x + br.width;
        r.y2 = br.y + br.height;
        r.areaPx = br.area();
        if (RoiMostlyBusy(r, busyMask)) continue;
        raw.push_back(r);
    }
    std::sort(raw.begin(), raw.end(),
        [](const ScreenChangeRoi& x, const ScreenChangeRoi& y) { return x.areaPx > y.areaPx; });

    std::vector<ScreenChangeRoi> merged;
    for (const auto& r : raw) {
        bool absorbed = false;
        for (auto& m : merged) {
            if (RectsOverlapOrNear(m, r, 12)) {
                m = MergeRoi(m, r);
                absorbed = true;
                break;
            }
        }
        if (!absorbed) merged.push_back(r);
        if (static_cast<int>(merged.size()) >= maxRois * 2) break;
    }
    std::sort(merged.begin(), merged.end(),
        [](const ScreenChangeRoi& x, const ScreenChangeRoi& y) { return x.areaPx > y.areaPx; });
    if (static_cast<int>(merged.size()) > maxRois)
        merged.resize(static_cast<size_t>(maxRois));
    out.rois = std::move(merged);
    return out;
}

UiVisualSettleResult WaitUiReactThenSettle(
    HBITMAP baseline,
    const std::function<HBITMAP()>& capture,
    const std::atomic_bool& stopFlag,
    const UiVisualSettleOptions& opts) {
    UiVisualSettleResult out;
    if (!baseline || !capture) {
        out.outcome = UiVisualSettleResult::Outcome::NoReaction;
        out.logLine = L"UI settle：无基线或截屏回调";
        out.agentHint = L"本地 settle 无法运行；请 wait 后再观察。";
        return out;
    }

    const auto t0 = std::chrono::steady_clock::now();
    auto elapsedMs = [&]() -> int {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
    };

    HBITMAP prev = nullptr;
    bool reacted = false;
    int stableSinceMs = -1;

    auto take = [&]() -> HBITMAP {
        if (stopFlag.load()) return nullptr;
        return capture();
    };

    auto finishWith = [&](UiVisualSettleResult::Outcome oc, HBITMAP last) {
        out.outcome = oc;
        out.elapsedMs = elapsedMs();
        out.reacted = reacted;
        out.settled = (oc == UiVisualSettleResult::Outcome::Settled);
        out.suggestRefresh = out.elapsedMs >= opts.refreshSuggestMs
            && oc != UiVisualSettleResult::Outcome::Settled;
        out.lastFrame = last;
        if (prev && prev != last) {
            DeleteBitmapHandle(prev);
            prev = nullptr;
        }
        std::wstring roisTxt;
        for (size_t i = 0; i < out.lastChangeRois.size() && i < 3; ++i) {
            const auto& r = out.lastChangeRois[i];
            if (!roisTxt.empty()) roisTxt += L";";
            roisTxt += std::to_wstring(r.x1) + L"," + std::to_wstring(r.y1) + L","
                + std::to_wstring(r.x2) + L"," + std::to_wstring(r.y2);
        }
        out.logLine = L"UI settle："
            + std::wstring(oc == UiVisualSettleResult::Outcome::Settled ? L"已稳定"
                : (oc == UiVisualSettleResult::Outcome::StillChanging ? L"仍在变化"
                    : (oc == UiVisualSettleResult::Outcome::Cancelled ? L"已取消" : L"无反应")))
            + L" 耗时" + std::to_wstring(out.elapsedMs) + L"ms"
            + L" 差分" + std::to_wstring(static_cast<int>(out.lastChangedRatio * 10000.0 + 0.5))
            + L"bp";
        if (!roisTxt.empty()) out.logLine += L" 变化区[" + roisTxt + L"]";
        if (out.suggestRefresh) out.logLine += L" →建议刷新/重开";

        // 只报事实；策略见 lookupMacroAction(section=agent|usage)
        if (oc == UiVisualSettleResult::Outcome::Settled) {
            out.agentHint = L"本地settle：已变化后稳定 耗时"
                + std::to_wstring(out.elapsedMs) + L"ms。细则 section=agent。";
        } else if (oc == UiVisualSettleResult::Outcome::StillChanging) {
            out.agentHint = L"本地settle：仍在变化 耗时"
                + std::to_wstring(out.elapsedMs) + L"ms"
                + (out.suggestRefresh ? L" →可考虑刷新/重开。" : L"。")
                + L" 细则 section=agent。";
        } else if (oc == UiVisualSettleResult::Outcome::NoReaction) {
            out.agentHint = L"本地settle：相对操作前几乎无变化。"
                L" 细则 section=agent。";
        } else {
            out.agentHint = L"本地settle：已取消。";
        }
        return out;
    };

    while (elapsedMs() < opts.maxTotalMs) {
        if (stopFlag.load())
            return finishWith(UiVisualSettleResult::Outcome::Cancelled, nullptr);

        HBITMAP cur = take();
        if (!cur) {
            Sleep(static_cast<DWORD>(std::max(50, opts.pollIntervalMs)));
            continue;
        }

        if (!reacted) {
            const ScreenChangeDiffResult d = DiffBitmapsChangedRegions(
                baseline, cur, opts.channelTol);
            out.lastChangedRatio = d.changedRatio;
            out.lastChangeRois = d.rois;
            const bool changed = d.ok && d.sameSize
                && (!d.nearlyIdentical && d.changedRatio >= opts.reactedMinChangedRatio);
            if (changed) {
                reacted = true;
                stableSinceMs = -1;
                if (prev) DeleteBitmapHandle(prev);
                prev = cur;
                cur = nullptr;
            } else if (elapsedMs() >= opts.reactDeadlineMs) {
                // 第一次校验超时：没有「反应」
                return finishWith(UiVisualSettleResult::Outcome::NoReaction, cur);
            } else {
                DeleteBitmapHandle(cur);
            }
        } else {
            // 第二次：忽略持续动态区（视频）后的结构稳定性
            Sleep(70);
            HBITMAP mid = take();
            ScreenBusyMask busy{};
            if (mid && prev) {
                busy = BuildBusyMaskFromTripleFrames(prev, mid, cur, opts.channelTol);
            }
            if (mid) DeleteBitmapHandle(mid);

            const ScreenChangeDiffResult d = DiffBitmapsChangedRegions(
                prev ? prev : baseline, cur, opts.channelTol, 48, 8,
                busy.valid() ? &busy : nullptr);
            out.lastChangedRatio = d.changedRatio;
            out.lastChangeRois = d.rois;
            const bool stable = d.ok && d.sameSize
                && (d.nearlyIdentical
                    || d.changedRatio <= opts.stableMaxChangedRatio
                    || d.onlyDynamicChanged);
            if (stable) {
                if (stableSinceMs < 0) stableSinceMs = elapsedMs();
                if (elapsedMs() - stableSinceMs >= opts.stableHoldMs) {
                    if (prev) DeleteBitmapHandle(prev);
                    prev = nullptr;
                    return finishWith(UiVisualSettleResult::Outcome::Settled, cur);
                }
                DeleteBitmapHandle(cur);
            } else {
                stableSinceMs = -1;
                if (prev) DeleteBitmapHandle(prev);
                prev = cur;
                cur = nullptr;
            }
        }

        // 可中断短睡
        const int slice = std::max(40, opts.pollIntervalMs);
        const int endAt = elapsedMs() + slice;
        while (elapsedMs() < endAt) {
            if (stopFlag.load())
                return finishWith(UiVisualSettleResult::Outcome::Cancelled, nullptr);
            Sleep(40);
        }
    }

    HBITMAP last = prev;
    prev = nullptr;
    if (!last) last = take();
    if (reacted)
        return finishWith(UiVisualSettleResult::Outcome::StillChanging, last);
    return finishWith(UiVisualSettleResult::Outcome::NoReaction, last);
}
