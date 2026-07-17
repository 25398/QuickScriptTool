#include "coord_space.h"

#include "image_match.h"
#include "utils.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <vector>

namespace {

cv::Mat ImReadW(const std::wstring& path) {
    if (path.empty()) return {};
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) return {};
    std::vector<uint8_t> buf;
    fseek(fp, 0, SEEK_END);
    const long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return {}; }
    buf.resize(static_cast<size_t>(sz));
    fread(buf.data(), 1, buf.size(), fp);
    fclose(fp);
    return cv::imdecode(buf, cv::IMREAD_COLOR);
}

HBITMAP BgrMatToHBitmap(const cv::Mat& img) {
    if (img.empty()) return nullptr;
    cv::Mat bgra;
    cv::cvtColor(img, bgra, cv::COLOR_BGR2BGRA);
    const int w = bgra.cols;
    const int h = bgra.rows;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) return bmp;
    const size_t bytes = static_cast<size_t>(w) * h * 4;
    memcpy(bits, bgra.data, bytes);
    return bmp;
}

bool GetTemplateBitmapSize(const std::wstring& path, int& outW, int& outH) {
    outW = outH = 0;
    if (path.empty()) return false;
    HBITMAP bmp = LoadBitmapFromFile(path);
    if (!bmp) return false;
    BITMAP bm{};
    GetObjectW(bmp, sizeof(bm), &bm);
    DeleteBitmapHandle(bmp);
    if (bm.bmWidth <= 0 || bm.bmHeight <= 0) return false;
    outW = bm.bmWidth;
    outH = bm.bmHeight;
    return true;
}

void SyncFindImageOffsetNorm(ScriptAction& a) {
    int tplW = 0, tplH = 0;
    if (!GetTemplateBitmapSize(a.imagePath, tplW, tplH)) return;
    a.nOffsetX = a.offsetX / static_cast<double>(tplW);
    a.nOffsetY = a.offsetY / static_cast<double>(tplH);
}

void DenormFindImageOffsetPixels(ScriptAction& a) {
    int tplW = 0, tplH = 0;
    if (!GetTemplateBitmapSize(a.imagePath, tplW, tplH)) return;
    a.offsetX = static_cast<int>(std::round(a.nOffsetX * tplW));
    a.offsetY = static_cast<int>(std::round(a.nOffsetY * tplH));
}

void SyncNormFieldsFromPixelsAction(ScriptAction& a, const CoordMeta& meta) {
    if (a.type == ActionType::MoveMouseRelative) {
        // 相对位移不是屏幕坐标，禁止写入 n*
        a.coordsAreNormalized = false;
        return;
    }
    const double rw = static_cast<double>(meta.refWidth);
    const double rh = static_cast<double>(meta.refHeight);
    if (rw <= 0 || rh <= 0) return;

    a.coordsAreNormalized = true;
    a.nx = (a.x - meta.refOriginX) / rw;
    a.ny = (a.y - meta.refOriginY) / rh;
    a.nRandomX = a.randomX / rw;
    a.nRandomY = a.randomY / rh;

    if (!a.searchFullScreen) {
        const bool relativeSearch = (a.type == ActionType::TextRecognition && a.ocrRegionByImage)
            || ((a.type == ActionType::AiImageAnalysis || a.type == ActionType::AiActionExecute)
                && a.aiRegionByImage);
        if (relativeSearch) {
            a.nSearchX1 = a.searchX1 / rw;
            a.nSearchY1 = a.searchY1 / rh;
            a.nSearchX2 = a.searchX2 / rw;
            a.nSearchY2 = a.searchY2 / rh;
        } else {
            a.nSearchX1 = (a.searchX1 - meta.refOriginX) / rw;
            a.nSearchY1 = (a.searchY1 - meta.refOriginY) / rh;
            a.nSearchX2 = (a.searchX2 - meta.refOriginX) / rw;
            a.nSearchY2 = (a.searchY2 - meta.refOriginY) / rh;
        }
    } else {
        a.nSearchX1 = 0.0;
        a.nSearchY1 = 0.0;
        a.nSearchX2 = 0.0;
        a.nSearchY2 = 0.0;
    }

    a.nOffsetX = a.offsetX / rw;
    a.nOffsetY = a.offsetY / rh;
    if (a.type == ActionType::FindImage) {
        SyncFindImageOffsetNorm(a);
    }

    if (a.aiSearchRegion == 6) {
        a.nAiSearchX1 = (a.aiSearchX1 - meta.refOriginX) / rw;
        a.nAiSearchY1 = (a.aiSearchY1 - meta.refOriginY) / rh;
        a.nAiSearchX2 = (a.aiSearchX2 - meta.refOriginX) / rw;
        a.nAiSearchY2 = (a.aiSearchY2 - meta.refOriginY) / rh;
    } else if (a.aiRegionByImage) {
        a.nAiSearchX1 = a.aiSearchX1 / rw;
        a.nAiSearchY1 = a.aiSearchY1 / rh;
        a.nAiSearchX2 = a.aiSearchX2 / rw;
        a.nAiSearchY2 = a.aiSearchY2 / rh;
    }
}

}  // namespace

CoordMeta CaptureCurrentCoordMeta(const windowmode::WindowModeScriptConfig* wmCfg) {
    CoordMeta meta;
    meta.refDpi = GetDpiForSystem();

    const bool isWindowClient = wmCfg && wmCfg->enabled
        && wmCfg->coordSpace == windowmode::WindowModeCoordinateSpace::WindowClient;

    if (isWindowClient) {
        meta.space = CoordMeta::Space::WindowClient;
    } else {
        meta.space = CoordMeta::Space::ScreenVirtual;
    }

    GetVirtualScreenBounds(meta.refOriginX, meta.refOriginY, meta.refWidth, meta.refHeight);
    return meta;
}

namespace {

struct VirtualScreenEnumCtx {
    RECT unionRect{};
    bool any = false;
};

BOOL CALLBACK UnionMonitorRectProc(HMONITOR hMonitor, HDC, LPRECT, LPARAM lp) {
    auto* ctx = reinterpret_cast<VirtualScreenEnumCtx*>(lp);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMonitor, &mi)) return TRUE;
    if (!ctx->any) {
        ctx->unionRect = mi.rcMonitor;
        ctx->any = true;
    } else {
        UnionRect(&ctx->unionRect, &ctx->unionRect, &mi.rcMonitor);
    }
    return TRUE;
}

bool RectsNearSameSize(int w1, int h1, int w2, int h2, double tolRatio) {
    if (w1 <= 0 || h1 <= 0 || w2 <= 0 || h2 <= 0) return false;
    const double dw = std::abs(w1 - w2) / static_cast<double>(w2);
    const double dh = std::abs(h1 - h2) / static_cast<double>(h2);
    return dw <= tolRatio && dh <= tolRatio;
}

}  // namespace

void GetVirtualScreenBounds(int& x, int& y, int& w, int& h) {
    VirtualScreenEnumCtx ctx;
    EnumDisplayMonitors(nullptr, nullptr, UnionMonitorRectProc,
        reinterpret_cast<LPARAM>(&ctx));
    if (ctx.any) {
        x = ctx.unionRect.left;
        y = ctx.unionRect.top;
        w = ctx.unionRect.right - ctx.unionRect.left;
        h = ctx.unionRect.bottom - ctx.unionRect.top;
        return;
    }
    x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

CoordMeta StandardScriptCoordMeta() {
    CoordMeta meta;
    meta.version = 1;
    meta.space = CoordMeta::Space::ScreenVirtual;
    meta.refOriginX = 0;
    meta.refOriginY = 0;
    meta.refWidth = 2560;
    meta.refHeight = 1440;
    meta.refDpi = 96;
    return meta;
}

CoordMeta BuildScriptCoordMetaForSave(const CoordMeta& pixelMeta) {
    CoordMeta meta = StandardScriptCoordMeta();
    meta.refOriginX = pixelMeta.refOriginX;
    meta.refOriginY = pixelMeta.refOriginY;
    if (pixelMeta.refWidth > 0 && pixelMeta.refHeight > 0) {
        meta.captureWidth = pixelMeta.refWidth;
        meta.captureHeight = pixelMeta.refHeight;
    } else {
        meta.captureWidth = meta.refWidth;
        meta.captureHeight = meta.refHeight;
    }
    return meta;
}

CoordMeta ScriptCoordMetaForExecution(const CoordMeta& fromFile) {
    CoordMeta meta = StandardScriptCoordMeta();
    if (fromFile.captureWidth > 0 && fromFile.captureHeight > 0) {
        meta.captureWidth = fromFile.captureWidth;
        meta.captureHeight = fromFile.captureHeight;
    } else if (fromFile.refWidth > 0 && fromFile.refHeight > 0
        && !RectsNearSameSize(fromFile.refWidth, fromFile.refHeight,
            meta.refWidth, meta.refHeight, 0.02)) {
        // 旧脚本：ref 即为保存时的实际分辨率
        meta.captureWidth = fromFile.refWidth;
        meta.captureHeight = fromFile.refHeight;
    } else {
        meta.captureWidth = meta.refWidth;
        meta.captureHeight = meta.refHeight;
    }
    return meta;
}

void DenormalizeScriptToCurrentScreen(std::vector<ScriptAction>& actions) {
    int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
    GetVirtualScreenBounds(vsX, vsY, vsW, vsH);
    DenormalizeScriptCoords(actions, StandardScriptCoordMeta(), vsW, vsH);
}

void NormalizeActionCoords(ScriptAction& a, const CoordMeta& meta) {
    SyncNormFieldsFromPixelsAction(a, meta);
}

void NormalizeScriptCoords(std::vector<ScriptAction>& actions, const CoordMeta& meta) {
    for (auto& a : actions) {
        NormalizeActionCoords(a, meta);
    }
}

void DenormalizeActionCoords(ScriptAction& a, const CoordMeta& meta, int targetW, int targetH) {
    if (a.type == ActionType::MoveMouseRelative) return;
    (void)meta;
    const double tw = static_cast<double>(targetW);
    const double th = static_cast<double>(targetH);
    if (tw <= 0 || th <= 0) return;

    int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
    GetVirtualScreenBounds(vsX, vsY, vsW, vsH);

    a.x = vsX + static_cast<int>(std::round(a.nx * tw));
    a.y = vsY + static_cast<int>(std::round(a.ny * th));
    a.randomX = static_cast<int>(std::round(a.nRandomX * tw));
    a.randomY = static_cast<int>(std::round(a.nRandomY * th));

    const bool hasSearch = (a.nSearchX2 > a.nSearchX1 || a.nSearchY2 > a.nSearchY1);
    if (hasSearch) {
        // OCR/AI 锚点模式：search 区域为相对偏移，不含虚拟桌面原点
        const bool relativeSearch = (a.type == ActionType::TextRecognition && a.ocrRegionByImage)
            || ((a.type == ActionType::AiImageAnalysis || a.type == ActionType::AiActionExecute)
                && a.aiRegionByImage);
        if (relativeSearch) {
            a.searchX1 = static_cast<int>(std::round(a.nSearchX1 * tw));
            a.searchY1 = static_cast<int>(std::round(a.nSearchY1 * th));
            a.searchX2 = static_cast<int>(std::round(a.nSearchX2 * tw));
            a.searchY2 = static_cast<int>(std::round(a.nSearchY2 * th));
        } else {
            a.searchX1 = vsX + static_cast<int>(std::round(a.nSearchX1 * tw));
            a.searchY1 = vsY + static_cast<int>(std::round(a.nSearchY1 * th));
            a.searchX2 = vsX + static_cast<int>(std::round(a.nSearchX2 * tw));
            a.searchY2 = vsY + static_cast<int>(std::round(a.nSearchY2 * th));
        }
    }

    a.offsetX = static_cast<int>(std::round(a.nOffsetX * tw));
    a.offsetY = static_cast<int>(std::round(a.nOffsetY * th));
    if (a.type == ActionType::FindImage) {
        DenormFindImageOffsetPixels(a);
    }

    const bool hasAiSearch = (a.nAiSearchX2 > a.nAiSearchX1 || a.nAiSearchY2 > a.nAiSearchY1);
    if (hasAiSearch) {
        if (a.aiRegionByImage) {
            a.aiSearchX1 = static_cast<int>(std::round(a.nAiSearchX1 * tw));
            a.aiSearchY1 = static_cast<int>(std::round(a.nAiSearchY1 * th));
            a.aiSearchX2 = static_cast<int>(std::round(a.nAiSearchX2 * tw));
            a.aiSearchY2 = static_cast<int>(std::round(a.nAiSearchY2 * th));
        } else {
            a.aiSearchX1 = vsX + static_cast<int>(std::round(a.nAiSearchX1 * tw));
            a.aiSearchY1 = vsY + static_cast<int>(std::round(a.nAiSearchY1 * th));
            a.aiSearchX2 = vsX + static_cast<int>(std::round(a.nAiSearchX2 * tw));
            a.aiSearchY2 = vsY + static_cast<int>(std::round(a.nAiSearchY2 * th));
        }
    }
}

void DenormalizeScriptCoords(std::vector<ScriptAction>& actions, const CoordMeta& meta,
    int targetW, int targetH) {
    for (auto& a : actions) {
        if (a.coordsAreNormalized) {
            DenormalizeActionCoords(a, meta, targetW, targetH);
        }
    }
}

void MigrateLegacyScriptToNormalized(std::vector<ScriptAction>& actions,
    const CoordMeta& assumedRef) {
    const double rw = static_cast<double>(assumedRef.refWidth);
    const double rh = static_cast<double>(assumedRef.refHeight);
    if (rw <= 0 || rh <= 0) return;

    for (auto& a : actions) {
        if (a.type == ActionType::MoveMouseRelative) {
            a.coordsAreNormalized = false;
            continue;
        }
        a.nx = (a.x - assumedRef.refOriginX) / rw;
        a.ny = (a.y - assumedRef.refOriginY) / rh;
        a.nRandomX = a.randomX / rw;
        a.nRandomY = a.randomY / rh;

        if (!a.searchFullScreen) {
            a.nSearchX1 = (a.searchX1 - assumedRef.refOriginX) / rw;
            a.nSearchY1 = (a.searchY1 - assumedRef.refOriginY) / rh;
            a.nSearchX2 = (a.searchX2 - assumedRef.refOriginX) / rw;
            a.nSearchY2 = (a.searchY2 - assumedRef.refOriginY) / rh;
        }

        a.nOffsetX = a.offsetX / rw;
        a.nOffsetY = a.offsetY / rh;
        if (a.type == ActionType::FindImage) {
            SyncFindImageOffsetNorm(a);
        }

        if (a.aiSearchRegion == 6) {
            a.nAiSearchX1 = (a.aiSearchX1 - assumedRef.refOriginX) / rw;
            a.nAiSearchY1 = (a.aiSearchY1 - assumedRef.refOriginY) / rh;
            a.nAiSearchX2 = (a.aiSearchX2 - assumedRef.refOriginX) / rw;
            a.nAiSearchY2 = (a.aiSearchY2 - assumedRef.refOriginY) / rh;
        }

        a.coordsAreNormalized = true;
    }
}

void SyncNormFieldsFromPixels(std::vector<ScriptAction>& actions, const CoordMeta& meta) {
    CoordMeta captureMeta = meta;
    if (captureMeta.refWidth <= 0 || captureMeta.refHeight <= 0) {
        captureMeta = CaptureCurrentCoordMeta(nullptr);
    }
    for (auto& a : actions) {
        SyncNormFieldsFromPixelsAction(a, captureMeta);
    }
}

std::vector<ScriptAction> PrepareScriptActionsForExecution(
    const std::vector<ScriptAction>& actions, const CoordMeta& scriptMeta) {
    std::vector<ScriptAction> execActions = actions;
    const CoordMeta refMeta = ScriptCoordMetaForExecution(scriptMeta);
    if (refMeta.refWidth <= 0 || refMeta.refHeight <= 0) return execActions;

    int vsX = 0, vsY = 0, targetW = 0, targetH = 0;
    GetVirtualScreenBounds(vsX, vsY, targetW, targetH);

    DenormalizeScriptCoords(execActions, refMeta, targetW, targetH);
    return execActions;
}

TemplateScale ComputeTemplateScale(const CoordMeta& meta, int currentW, int currentH) {
    TemplateScale ts;
    const int capW = meta.captureWidth > 0 ? meta.captureWidth : meta.refWidth;
    const int capH = meta.captureHeight > 0 ? meta.captureHeight : meta.refHeight;
    if (capW <= 0 || capH <= 0 || currentW <= 0 || currentH <= 0) {
        return ts;
    }
    // 模板是保存时屏幕的位图像素；仅按分辨率比例缩放，不按 DPI 再乘
    ts.sx = static_cast<double>(currentW) / capW;
    ts.sy = static_cast<double>(currentH) / capH;
    return ts;
}

ImageMatchOptions BuildResolutionAwareMatchOptions(const ScriptAction& action,
    const TemplateScale& resolutionScale) {
    ImageMatchOptions opt;
    opt.thresholdPercent = action.matchThreshold;

    double userMin = action.imageScaleMin > 0.0 ? action.imageScaleMin : action.imageScale;
    double userMax = action.imageScaleMax > 0.0 ? action.imageScaleMax : userMin;
    if (userMax < userMin) userMax = userMin;

    const double sx = resolutionScale.sx > 0.0 ? resolutionScale.sx : 1.0;
    const double sy = resolutionScale.sy > 0.0 ? resolutionScale.sy : 1.0;
    const double ratio = std::sqrt(sx * sy);

    opt.scaleMin = userMin * ratio;
    opt.scaleMax = userMax * ratio;

    // 用户未配置缩放容差时，围绕目标比例给 ±15% 余量
    if (std::abs(userMax - userMin) < 0.02) {
        opt.scaleMin = ratio * 0.85;
        opt.scaleMax = ratio * 1.15;
    }

    opt.scaleStep = 0.02;
    opt.maxMatches = 20;
    opt.maxOverlap = 0.5;
    if (std::abs(ratio - 1.0) > 0.02) {
        opt.crossResolutionMatch = true;
    }
    return opt;
}

/// 执行期找图：
/// - 同宽高比：原图 + 单一等比 scale=ratio（OpenCV 内只 resize 一次，不做 HBITMAP 预缩放）
/// - 不同宽高比：原图 + 跨分辨率 NCC 粗→细
ImageMatchOptions BuildExecutionFindImageOptions(const ScriptAction& action,
    const TemplateScale& resolutionScale) {
    ImageMatchOptions opt;
    opt.thresholdPercent = action.matchThreshold;

    double userMin = action.imageScaleMin > 0.0 ? action.imageScaleMin : action.imageScale;
    double userMax = action.imageScaleMax > 0.0 ? action.imageScaleMax : userMin;
    if (userMax < userMin) userMax = userMin;

    const double sx = resolutionScale.sx > 0.0 ? resolutionScale.sx : 1.0;
    const double sy = resolutionScale.sy > 0.0 ? resolutionScale.sy : 1.0;
    const double ratio = std::sqrt(sx * sy);
    const bool anamorphic = std::abs(sx - sy) > 0.02;

    if (anamorphic) {
        // 区间要盖住「UI 未按短边等比缩小」的情况（1600 上曾命中 ~0.91）
        const double lo = std::min(sx, sy) * std::min(userMin, userMax);
        const double hi = std::max(sx, sy) * std::max(userMin, userMax);
        const double geo = std::sqrt(std::max(1e-6, sx * sy));
        opt.scaleMin = std::max(0.1, std::min(lo, hi) * 0.85);
        // 重度缩小（如 800x600）时放宽上界，避免真实尺度略高于 max(sx,sy)
        const double spanHi = std::max(hi, geo) * (hi < 0.55 ? 1.45 : 1.20);
        opt.scaleMax = std::max(opt.scaleMin, std::min(1.05, spanHi));
        opt.scaleStep = 0.05;
        opt.crossResolutionMatch = true;
        // 小尺度模板金字塔易虚高峰值，改为全分辨率匹配
        opt.disablePyramid = (opt.scaleMax < 0.60);
    } else if (std::abs(ratio - 1.0) > 0.02) {
        // 同宽高比跨分辨率：围绕理论比例窄范围搜（±6%），避免单点 scale 差几% 就掉到阈值下
        const double mid = (userMin + userMax) * 0.5;
        const double target = mid * ratio;
        opt.scaleMin = target * 0.94;
        opt.scaleMax = target * 1.06;
        opt.scaleStep = 0.02;
        opt.crossResolutionMatch = true;
        opt.disablePyramid = true;
    } else if (std::abs(userMax - userMin) < 0.02) {
        opt.scaleMin = userMin;
        opt.scaleMax = userMax;
        opt.scaleStep = 1.0;
        opt.disablePyramid = true;
    } else {
        opt.scaleMin = userMin;
        opt.scaleMax = userMax;
        opt.scaleStep = 0.05;
    }

    opt.maxMatches = 20;
    opt.maxOverlap = 0.5;
    return opt;
}

HBITMAP LoadResolutionMatchedTemplate(const std::wstring& path, const TemplateScale& ts) {
    return LoadScaledTemplateBitmap(path, ts.sx, ts.sy);
}

HBITMAP LoadScaledTemplateBitmap(const std::wstring& path, double sx, double sy) {
    if (path.empty()) return nullptr;

    if (std::abs(sx - 1.0) < 1e-6 && std::abs(sy - 1.0) < 1e-6) {
        return LoadBitmapFromFile(path);
    }

    const cv::Mat src = ImReadW(path);
    if (src.empty()) {
        HBITMAP orig = LoadBitmapFromFile(path);
        if (!orig) return nullptr;

        BITMAP bm{};
        GetObjectW(orig, sizeof(bm), &bm);
        const int newW = std::max(1, static_cast<int>(std::round(bm.bmWidth * sx)));
        const int newH = std::max(1, static_cast<int>(std::round(bm.bmHeight * sy)));

        HDC hdcScreen = GetDC(nullptr);
        HDC hdcSrc = CreateCompatibleDC(hdcScreen);
        HDC hdcDst = CreateCompatibleDC(hdcScreen);
        HBITMAP scaled = CreateCompatibleBitmap(hdcScreen, newW, newH);

        HBITMAP oldSrc = static_cast<HBITMAP>(SelectObject(hdcSrc, orig));
        HBITMAP oldDst = static_cast<HBITMAP>(SelectObject(hdcDst, scaled));

        SetStretchBltMode(hdcDst, HALFTONE);
        SetBrushOrgEx(hdcDst, 0, 0, nullptr);
        StretchBlt(hdcDst, 0, 0, newW, newH,
            hdcSrc, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);

        SelectObject(hdcSrc, oldSrc);
        SelectObject(hdcDst, oldDst);
        DeleteDC(hdcSrc);
        DeleteDC(hdcDst);
        ReleaseDC(nullptr, hdcScreen);
        DeleteObject(orig);
        return scaled;
    }

    const int newW = std::max(1, static_cast<int>(std::round(src.cols * sx)));
    const int newH = std::max(1, static_cast<int>(std::round(src.rows * sy)));

    cv::Mat dst;
    cv::resize(src, dst, cv::Size(newW, newH), 0, 0, cv::INTER_LINEAR);
    return BgrMatToHBitmap(dst);
}

PreparedFindImageMatch PrepareFindImageMatch(const ScriptAction& action, const TemplateScale& ts) {
    PreparedFindImageMatch prep;
    const double sx = ts.sx > 0.0 ? ts.sx : 1.0;
    const double sy = ts.sy > 0.0 ? ts.sy : 1.0;
    prep.effScaleX = sx;
    prep.effScaleY = sy;
    // 始终加载原图像素；缩放交给 matchTemplate 的单一/粗细尺度，
    // 避免 HBITMAP 预缩放 + 二次 scale 搜索导致位置偏移。
    prep.bitmap = LoadBitmapFromFile(action.imagePath);
    prep.templatePreScaled = false;
    prep.options = BuildExecutionFindImageOptions(action, ts);

    if (prep.bitmap) {
        BITMAP bm{};
        GetObjectW(prep.bitmap, sizeof(bm), &bm);
        prep.templateW = bm.bmWidth;
        prep.templateH = bm.bmHeight;
    }
    return prep;
}

void ResolveFindImageClickPoint(const ImageMatchResult& match,
    int origTplW, int origTplH, double nOffsetX, double nOffsetY,
    const TemplateScale& tmplScale, bool templatePreScaled, int& tx, int& ty) {
    if (!match.found) {
        tx = ty = 0;
        return;
    }
    int cx = 0;
    int cy = 0;
    FindImageMatchCenter(match, cx, cy);

    // 落点中心已是屏幕坐标；偏移按「原图尺寸 × 实际匹配尺度」
    // 若模板曾非等比预缩放，则分轴用分辨率比例，再乘 match.scale
    const double matchScale = match.scale > 0.0 ? match.scale : 1.0;
    const double offSx = templatePreScaled
        ? (tmplScale.sx > 0.0 ? tmplScale.sx : 1.0) * matchScale
        : matchScale;
    const double offSy = templatePreScaled
        ? (tmplScale.sy > 0.0 ? tmplScale.sy : 1.0) * matchScale
        : matchScale;

    const int offX = static_cast<int>(std::round(nOffsetX * origTplW * offSx));
    const int offY = static_cast<int>(std::round(nOffsetY * origTplH * offSy));
    tx = cx + offX;
    ty = cy + offY;
}

void WriteCoordMetaJson(std::wstring& out, const CoordMeta& meta, bool trailingComma) {
    out += L"  \"coordMeta\": {\n";
    out += L"    \"version\": " + std::to_wstring(meta.version) + L",\n";
    out += L"    \"space\": \""
        + std::wstring(meta.space == CoordMeta::Space::ScreenVirtual ? L"screenVirtual" : L"windowClient")
        + L"\",\n";
    out += L"    \"refOriginX\": " + std::to_wstring(meta.refOriginX) + L",\n";
    out += L"    \"refOriginY\": " + std::to_wstring(meta.refOriginY) + L",\n";
    out += L"    \"refWidth\": " + std::to_wstring(meta.refWidth) + L",\n";
    out += L"    \"refHeight\": " + std::to_wstring(meta.refHeight) + L",\n";
    out += L"    \"refDpi\": " + std::to_wstring(meta.refDpi) + L",\n";
    out += L"    \"captureWidth\": " + std::to_wstring(meta.captureWidth) + L",\n";
    out += L"    \"captureHeight\": " + std::to_wstring(meta.captureHeight) + L"\n";
    out += L"  }" + std::wstring(trailingComma ? L",\n" : L"\n");
}

CoordMeta ParseCoordMetaJson(const std::wstring& content) {
    CoordMeta meta;
    if (content.empty()) return meta;

    meta.version = static_cast<int>(ExtractNumber(content, L"version", 1));
    const auto space = ExtractString(content, L"space");
    meta.space = (space == L"windowClient")
        ? CoordMeta::Space::WindowClient : CoordMeta::Space::ScreenVirtual;
    meta.refOriginX = static_cast<int>(ExtractNumber(content, L"refOriginX", 0));
    meta.refOriginY = static_cast<int>(ExtractNumber(content, L"refOriginY", 0));
    meta.refWidth = static_cast<int>(ExtractNumber(content, L"refWidth", 0));
    meta.refHeight = static_cast<int>(ExtractNumber(content, L"refHeight", 0));
    meta.refDpi = static_cast<int>(ExtractNumber(content, L"refDpi", 96));
    meta.captureWidth = static_cast<int>(ExtractNumber(content, L"captureWidth", 0));
    meta.captureHeight = static_cast<int>(ExtractNumber(content, L"captureHeight", 0));
    return meta;
}

bool HasCoordMetaJson(const std::wstring& content) {
    return content.find(L"\"coordMeta\"") != std::wstring::npos;
}
