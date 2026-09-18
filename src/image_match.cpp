// image_match.cpp — Template matching with pixel-tolerance accept
//
// Locate with one OpenCV matcher (NCC / SQDIFF). A location is accepted only
// after pixel-agree verification. PNG alpha is preserved and used as a mask.

#include "image_match.h"

#include "image_match_engines.h"
#include "image_match_internal.h"
#include "findimage_gpu.h"
#include "input/mouse_input_backend.h"
#include "low_power_mode.h"
#include "opencv_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <opencv2/core/ocl.hpp>
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

void BitmapToBgrAndMask(HBITMAP bitmap, cv::Mat& bgr, cv::Mat& mask) {
    bgr.release();
    mask.release();
    if (!OpenCvAvailable()) return;
    RawBitmap raw{};
    if (!ReadBitmapPixels(bitmap, raw)) return;
    cv::Mat bgra(raw.height, raw.width, CV_8UC4, raw.pixels.data());
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    bgr = bgr.clone();

    std::vector<cv::Mat> ch;
    cv::split(bgra, ch);
    if (ch.size() < 4) return;
    cv::Mat opaque;
    cv::compare(ch[3], 128, opaque, cv::CMP_GE);
    const int opaqueCount = cv::countNonZero(opaque);
    const int total = raw.width * raw.height;
    const int transparentCount = total - opaqueCount;
    // GDI compatible bitmaps typically have A=0 everywhere — treat as opaque.
    if (opaqueCount >= 8 && transparentCount >= 8) {
        mask = opaque.clone();
    }
}

cv::Mat BitmapToBgrMat(HBITMAP bitmap) {
    cv::Mat bgr;
    cv::Mat mask;
    BitmapToBgrAndMask(bitmap, bgr, mask);
    return bgr;
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

cv::Mat NormalizeDecodedImage(cv::Mat img) {
    if (img.empty()) return {};
    if (img.channels() == 1) {
        cv::Mat bgr;
        cv::cvtColor(img, bgr, cv::COLOR_GRAY2BGR);
        return bgr;
    }
    if (img.channels() == 2) {
        std::vector<cv::Mat> ch;
        cv::split(img, ch);
        cv::Mat bgra;
        cv::cvtColor(ch[0], bgra, cv::COLOR_GRAY2BGRA);
        std::vector<cv::Mat> bgraCh;
        cv::split(bgra, bgraCh);
        bgraCh[3] = ch[1];
        cv::merge(bgraCh, bgra);
        return bgra;
    }
    return img;
}

cv::Mat ImReadW(const std::wstring& path) {
    if (path.empty() || !OpenCvAvailable()) return {};
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
    return NormalizeDecodedImage(cv::imdecode(buf, cv::IMREAD_UNCHANGED));
}

/// 模板解码缓存：按（路径 + 文件大小 + 修改时间）失效。
///
/// 为什么要缓存：`findImage` 每一步都走 `PrepareFindImageMatch` → `LoadBitmapFromFile`，
/// 而在 `loop` 里同一步每秒要跑十几次 —— 每次都重新开文件 + 解码（PNG 更贵）纯属白给。
/// 缓存**只记忆 `ImReadW` 的解码结果**（`cv::Mat`），返回给调用方的仍是新建的 HBITMAP，
/// 因此句柄所有权语义与之前完全一致（调用方照旧 `DeleteBitmapHandle`）。
///
/// 失效判据是「大小 + 修改时间」：模板被重新裁剪/替换（写盘）后 `ftLastWriteTime` 必然变化。
/// 注意时间戳粒度：Windows 文件时间随系统时钟更新（约 15.6ms 一跳），
/// **同一 tick 内用同尺寸内容覆盖同名文件**理论上可能漏判 —— 实际使用（人改图/编辑器另存）
/// 不可能落在同一 tick，自检里用 `SetFileTime` 显式改时间戳来覆盖这条路径。
struct TemplateCacheEntry {
    ULONGLONG size = 0;
    ULONGLONG writeTime = 0;
    cv::Mat image;
    uint64_t lastUseStamp = 0;
};

constexpr size_t kTemplateCacheMaxEntries = 24;
constexpr size_t kTemplateCacheMaxBytes = 24u * 1024u * 1024u;

std::mutex g_templateCacheMu;
std::unordered_map<std::wstring, TemplateCacheEntry> g_templateCache;
size_t g_templateCacheBytes = 0;
uint64_t g_templateCacheStamp = 0;
std::atomic<uint64_t> g_templateCacheLookups{0};
std::atomic<uint64_t> g_templateCacheHits{0};
std::atomic<uint64_t> g_templateCacheEvictions{0};

/// 找图 GPU（OpenCL）加速开关见 findimage_gpu.h（进程级原子量，设置保存后立即生效）
constexpr long long kFindImageGpuMinAreaPx = 500 * 1000;

bool StatTemplateFile(const std::wstring& path, ULONGLONG& size, ULONGLONG& writeTime) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    size = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    writeTime = (static_cast<ULONGLONG>(fad.ftLastWriteTime.dwHighDateTime) << 32)
        | fad.ftLastWriteTime.dwLowDateTime;
    return true;
}

size_t TemplateMatBytes(const cv::Mat& m) {
    return m.empty() ? 0u : m.total() * m.elemSize();
}

void DropTemplateCacheEntryLocked(
    std::unordered_map<std::wstring, TemplateCacheEntry>::iterator it) {
    g_templateCacheBytes -= TemplateMatBytes(it->second.image);
    g_templateCache.erase(it);
    ++g_templateCacheEvictions;
}

/// 命中则返回深拷贝（避免调用方拿到共享像素后被误改），未命中返回空。
cv::Mat TryGetCachedTemplateImage(const std::wstring& path) {
    if (path.empty()) return {};
    ULONGLONG size = 0;
    ULONGLONG writeTime = 0;
    if (!StatTemplateFile(path, size, writeTime)) return {};
    g_templateCacheLookups.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_templateCacheMu);
    const auto it = g_templateCache.find(path);
    if (it == g_templateCache.end()) return {};
    if (it->second.size != size || it->second.writeTime != writeTime) {
        DropTemplateCacheEntryLocked(it);
        return {};
    }
    it->second.lastUseStamp = ++g_templateCacheStamp;
    g_templateCacheHits.fetch_add(1, std::memory_order_relaxed);
    return it->second.image.clone();
}

void StoreCachedTemplateImage(const std::wstring& path, const cv::Mat& image) {
    if (path.empty() || image.empty()) return;
    ULONGLONG size = 0;
    ULONGLONG writeTime = 0;
    if (!StatTemplateFile(path, size, writeTime)) return;
    const size_t bytes = TemplateMatBytes(image);
    if (bytes == 0 || bytes > kTemplateCacheMaxBytes) return;
    std::lock_guard<std::mutex> lock(g_templateCacheMu);
    auto it = g_templateCache.find(path);
    if (it != g_templateCache.end()) DropTemplateCacheEntryLocked(it);
    while ((g_templateCache.size() >= kTemplateCacheMaxEntries
            || g_templateCacheBytes + bytes > kTemplateCacheMaxBytes)
        && !g_templateCache.empty()) {
        auto oldest = g_templateCache.begin();
        for (auto cur = g_templateCache.begin(); cur != g_templateCache.end(); ++cur) {
            if (cur->second.lastUseStamp < oldest->second.lastUseStamp) oldest = cur;
        }
        DropTemplateCacheEntryLocked(oldest);
    }
    TemplateCacheEntry entry;
    entry.size = size;
    entry.writeTime = writeTime;
    entry.image = image;
    entry.lastUseStamp = ++g_templateCacheStamp;
    g_templateCacheBytes += bytes;
    g_templateCache.emplace(path, std::move(entry));
}

HBITMAP MatToHBitmap(const cv::Mat& img) {
    if (img.empty()) return nullptr;
    cv::Mat bgra;
    if (img.channels() == 4) {
        if (img.type() != CV_8UC4) return nullptr;
        bgra = img;
    } else if (img.channels() == 1) {
        cv::cvtColor(img, bgra, cv::COLOR_GRAY2BGRA);
    } else if (img.channels() == 3) {
        cv::cvtColor(img, bgra, cv::COLOR_BGR2BGRA);
    } else {
        return nullptr;
    }

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

    const int w = bgra.cols;
    const int h = bgra.rows;
    auto* dst = static_cast<uint8_t*>(bits);
    if (bgra.isContinuous() && static_cast<int>(bgra.step) == w * 4) {
        memcpy(dst, bgra.data, static_cast<size_t>(w) * h * 4);
    } else {
        for (int y = 0; y < h; ++y) {
            memcpy(dst + static_cast<size_t>(y) * w * 4, bgra.ptr(y), static_cast<size_t>(w) * 4);
        }
    }
    return bmp;
}

}  // namespace

HBITMAP LoadBitmapFromFile(const std::wstring& path) {
    if (path.empty()) return nullptr;
    const cv::Mat cached = TryGetCachedTemplateImage(path);
    if (!cached.empty()) return MatToHBitmap(cached);
    const cv::Mat img = ImReadW(path);
    if (!img.empty()) {
        StoreCachedTemplateImage(path, img);
        return MatToHBitmap(img);
    }
    return static_cast<HBITMAP>(LoadImageW(nullptr, path.c_str(), IMAGE_BITMAP, 0, 0,
                                           LR_LOADFROMFILE | LR_CREATEDIBSECTION));
}

bool TryGetCachedTemplateImageSize(const std::wstring& path, int& outW, int& outH) {
    outW = 0;
    outH = 0;
    if (path.empty()) return false;
    ULONGLONG size = 0;
    ULONGLONG writeTime = 0;
    if (!StatTemplateFile(path, size, writeTime)) return false;
    std::lock_guard<std::mutex> lock(g_templateCacheMu);
    const auto it = g_templateCache.find(path);
    if (it == g_templateCache.end()) return false;
    if (it->second.size != size || it->second.writeTime != writeTime) return false;
    if (it->second.image.empty()) return false;
    outW = it->second.image.cols;
    outH = it->second.image.rows;
    return outW > 0 && outH > 0;
}

namespace image_match_internal {

bool TryMatchTemplateOnGpu(const cv::Mat& src, const cv::Mat& tpl,
    int method, cv::Mat& outResult) {
    outResult.release();
    static std::atomic<int> state{0};   // 0=未探测 1=可用 2=不可用
    if (FindImageGpuAccelFlag().load(std::memory_order_relaxed) != true) return false;
    // 低性能模式优先：宁可慢一点也不额外拉 GPU（笔记本上 dGPU 功耗/发热比 CPU 更凶）
    if (LowPerformanceMode()) return false;
    if (src.empty() || tpl.empty() || src.type() != CV_8UC1 || tpl.type() != CV_8UC1) return false;
    // 面积门槛：480x360 附近 CPU/GPU 打平，低于此纯属白付传输开销
    if (static_cast<long long>(src.cols) * src.rows < kFindImageGpuMinAreaPx) return false;
    if (tpl.cols > src.cols || tpl.rows > src.rows) return false;

    if (state.load(std::memory_order_relaxed) == 0) {
        int probed = 2;
        try {
            if (cv::ocl::haveOpenCL() && cv::ocl::Context::getDefault().ndevices() > 0) {
                cv::ocl::setUseOpenCL(true);
                probed = cv::ocl::useOpenCL() ? 1 : 2;
            }
        } catch (const cv::Exception&) {
            probed = 2;
        }
        state.store(probed, std::memory_order_relaxed);
        if (probed != 1) {
            OutputDebugStringW(L"[找图] OpenCL 不可用，GPU 加速自动回落 CPU\n");
            return false;
        }
    }
    if (state.load(std::memory_order_relaxed) != 1) return false;

    try {
        cv::UMat usrc;
        cv::UMat utpl;
        cv::UMat ures;
        src.copyTo(usrc);
        tpl.copyTo(utpl);
        cv::matchTemplate(usrc, utpl, ures, method);
        ures.copyTo(outResult);
    } catch (const cv::Exception&) {
        // GPU 路径出问题就地永久回落：绝不因为加速把找图搞坏
        state.store(2, std::memory_order_relaxed);
        cv::ocl::setUseOpenCL(false);
        outResult.release();
        OutputDebugStringW(L"[找图] OpenCL 计算失败，本进程改为 CPU 找图\n");
        return false;
    }
    return !outResult.empty();
}

}  // namespace image_match_internal

bool PlanFindImageFastPath(const FindImageFastPathParams& params,
    int roiX1, int roiY1, int roiX2, int roiY2,
    int prevTLX, int prevTLY, int tplW, int tplH,
    double lastFullSearchMs, long long ageMs,
    int& winX1, int& winY1, int& winX2, int& winY2) {
    winX1 = winY1 = winX2 = winY2 = 0;
    if (tplW <= 0 || tplH <= 0) return false;
    if (roiX2 <= roiX1 || roiY2 <= roiY1) return false;
    // 上帧本来很快（区域找图）→ 不值得再来一次小窗口调度
    if (!(lastFullSearchMs >= params.minFullSearchMs)) return false;
    if (ageMs < 0 || ageMs > params.maxAgeMs) return false;
    if (params.maxDriftPx < 0) return false;
    // 上一帧命中必须在搜索区内，且整块模板放得下
    if (prevTLX < roiX1 || prevTLY < roiY1) return false;
    if (prevTLX + tplW > roiX2 || prevTLY + tplH > roiY2) return false;

    // 余量至少覆盖漂移带，否则「接受区间」会有一部分落在窗口外 →
    // 那部分漂移找不到就会白回退（宁可现在就回退）
    const int drift = params.maxDriftPx;
    const int margin = (std::max)(drift, (std::min)(64, (std::max)(tplW, tplH) / 4));
    const int cx1 = (std::max)(roiX1, prevTLX - margin);
    const int cy1 = (std::max)(roiY1, prevTLY - margin);
    const int cx2 = (std::min)(roiX2, prevTLX + tplW + margin);
    const int cy2 = (std::min)(roiY2, prevTLY + tplH + margin);
    if (cx2 - cx1 < tplW || cy2 - cy1 < tplH) return false;
    // 覆盖检查：窗口内可放置的左上角范围必须完整包含 [prev-drift, prev+drift]
    if (cx1 > prevTLX - drift) return false;
    if (cy1 > prevTLY - drift) return false;
    if (cx2 - tplW < prevTLX + drift) return false;
    if (cy2 - tplH < prevTLY + drift) return false;
    // 窗口几乎等于整个搜索区 → 没有收益，还不如直接全屏搜（避免「假快速路径」）
    const long long winArea = static_cast<long long>(cx2 - cx1) * (cy2 - cy1);
    const long long roiArea = static_cast<long long>(roiX2 - roiX1) * (roiY2 - roiY1);
    if (winArea * 10 >= roiArea * 7) return false;

    winX1 = cx1;
    winY1 = cy1;
    winX2 = cx2;
    winY2 = cy2;
    return true;
}

bool AcceptFindImageFastPathHit(const FindImageFastPathParams& params,
    int prevTLX, int prevTLY, int newTLX, int newTLY,
    double thresholdPercent, double newScore) {
    if (params.maxDriftPx < 0) return false;
    if (std::abs(newTLX - prevTLX) > params.maxDriftPx) return false;
    if (std::abs(newTLY - prevTLY) > params.maxDriftPx) return false;
    return newScore >= thresholdPercent + params.scoreMarginPct;
}

std::wstring FindImageGpuDeviceName() {
    try {
        if (!cv::ocl::haveOpenCL()) return {};
        if (cv::ocl::Context::getDefault().ndevices() == 0) return {};
        const std::string name = cv::ocl::Device::getDefault().name();
        return std::wstring(name.begin(), name.end());
    } catch (const cv::Exception&) {
        return {};
    }
}

uint64_t TemplateImageCacheLookups() {
    return g_templateCacheLookups.load(std::memory_order_relaxed);
}

uint64_t TemplateImageCacheHits() {
    return g_templateCacheHits.load(std::memory_order_relaxed);
}

uint64_t TemplateImageCacheEvictions() {
    return g_templateCacheEvictions.load(std::memory_order_relaxed);
}

size_t TemplateImageCacheEntries() {
    std::lock_guard<std::mutex> lock(g_templateCacheMu);
    return g_templateCache.size();
}

void ClearTemplateImageCache() {
    std::lock_guard<std::mutex> lock(g_templateCacheMu);
    g_templateCache.clear();
    g_templateCacheBytes = 0;
    g_templateCacheStamp = 0;
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
    HBITMAP bmp = MatToHBitmap(crop);
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
    if (!frozenScreen || !templateBmp || !OpenCvAvailable()) return out;
    SyncImageMatchThreadBudget();

    const cv::Mat fullBgr = BitmapToBgrMat(frozenScreen);
    cv::Mat templBgr;
    cv::Mat templMask;
    BitmapToBgrAndMask(templateBmp, templBgr, templMask);
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

    return MatchInGrayMatsMultiVerify(
        cropGray, templGray, cropBgr, templBgr, options, left, top, templMask);
}

ImageMatchOutput FindTemplateOnScreenMulti(
    int searchX1, int searchY1, int searchX2, int searchY2,
    HBITMAP templateBmp, const ImageMatchOptions& options) {
    ImageMatchOutput out{};
    if (!templateBmp || !OpenCvAvailable()) return out;
    SyncImageMatchThreadBudget();

    const int left = std::min(searchX1, searchX2);
    const int top = std::min(searchY1, searchY2);
    const int right = std::max(searchX1, searchX2);
    const int bottom = std::max(searchY1, searchY2);

    HBITMAP regionBmp = CaptureScreenRegion(left, top, right, bottom);
    if (!regionBmp) return out;

    const cv::Mat screenBgr = BitmapToBgrMat(regionBmp);
    cv::Mat templBgr;
    cv::Mat templMask;
    BitmapToBgrAndMask(templateBmp, templBgr, templMask);
    DeleteBitmapHandle(regionBmp);

    if (screenBgr.empty() || templBgr.empty()) return out;

    cv::Mat screenGray;
    cv::Mat templGray;
    cv::cvtColor(screenBgr, screenGray, cv::COLOR_BGR2GRAY);
    cv::cvtColor(templBgr, templGray, cv::COLOR_BGR2GRAY);

    return MatchInGrayMatsMultiVerify(
        screenGray, templGray, screenBgr, templBgr, options, left, top, templMask);
}

ImageMatchResult FindTemplateInFrozenScreen(
    HBITMAP frozenScreen, int virtX, int virtY,
    int searchX1, int searchY1, int searchX2, int searchY2,
    HBITMAP templateBmp, double thresholdPercent, double scale,
    int* outTemplateW, int* outTemplateH, double scaleMax) {
    ImageMatchOptions opt = NormalizeLegacyOptions(thresholdPercent, scale, scaleMax);
    if (!OpenCvAvailable()) return {};
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
    if (!f0 || !f1 || !f2 || !OpenCvAvailable()) return out;
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
    if (!a || !b || !OpenCvAvailable()) return out;
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
