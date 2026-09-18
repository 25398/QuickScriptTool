#include "coord_space.h"

#include "image_match.h"
#include "opencv_runtime.h"
#include "utils.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <cstring>
#include <vector>

namespace {

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
    std::vector<uint8_t> buf;
    fseek(fp, 0, SEEK_END);
    const long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return {}; }
    buf.resize(static_cast<size_t>(sz));
    fread(buf.data(), 1, buf.size(), fp);
    fclose(fp);
    return NormalizeDecodedImage(cv::imdecode(buf, cv::IMREAD_UNCHANGED));
}

HBITMAP BgrMatToHBitmap(const cv::Mat& img) {
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

bool TryTemplateBitmapSize(const std::wstring& path, int& outW, int& outH) {
    outW = outH = 0;
    if (path.empty()) return false;
    // 已解码过的模板直接拿缓存宽高：不必再造一个 HBITMAP（编辑期这里调得非常频繁）
    if (TryGetCachedTemplateImageSize(path, outW, outH)) return true;
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

bool GetTemplateBitmapSize(const std::wstring& path, int& outW, int& outH) {
    if (TryTemplateBitmapSize(path, outW, outH)) return true;
    const std::wstring resolved = ResolveImagePath(path);
    if (resolved.empty() || resolved == path) return false;
    return TryTemplateBitmapSize(resolved, outW, outH);
}

bool LooksLikeImageFilePath(const std::wstring& p) {
    if (p.empty()) return false;
    if (p.find(L'\\') != std::wstring::npos || p.find(L'/') != std::wstring::npos) return true;
    if (p.size() >= 2 && p[1] == L':') {
        const wchar_t c = p[0];
        return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
    }
    return false;
}

std::wstring OffsetTemplateToken(const ScriptAction& a) {
    if (a.type == ActionType::MultiMatch && !a.imagePaths.empty() && !a.imagePaths[0].empty()) {
        return a.imagePaths[0];
    }
    return a.imagePath;
}

bool OffsetTemplateUseVar(const ScriptAction& a) {
    if (a.type == ActionType::MultiMatch) return MultiMatchSlotUseVar(a, 0);
    return a.imageUseVar;
}

bool ProducerSearchRectSize(const ScriptAction& p, int& w, int& h) {
    if (p.searchFullScreen || p.searchX2 <= p.searchX1 || p.searchY2 <= p.searchY1) {
        int x = 0, y = 0;
        GetVirtualScreenBounds(x, y, w, h);
        return w >= 8 && h >= 8;
    }
    w = p.searchX2 - p.searchX1;
    h = p.searchY2 - p.searchY1;
    if (w < 0) w = -w;
    if (h < 0) h = -h;
    return w >= 8 && h >= 8;
}

/// 找图偏移 / 找图定位的模板像素尺寸：文件模板读位图；变量图用前序「保存图片」的搜索区或相对区。
bool ResolveFindImageOffsetTemplateSize(
    const std::vector<ScriptAction>& actions, size_t index, int& w, int& h) {
    w = 0;
    h = 0;
    if (index >= actions.size()) return false;
    const ScriptAction& a = actions[index];
    const std::wstring token = Trim(OffsetTemplateToken(a));
    const bool useVar = OffsetTemplateUseVar(a);
    if (!useVar) {
        return GetTemplateBitmapSize(a.imagePath, w, h) && w > 0 && h > 0;
    }
    if (LooksLikeImageFilePath(token)) {
        return GetTemplateBitmapSize(token, w, h) && w > 0 && h > 0;
    }
    const std::wstring name = token.empty() ? L"image" : token;
    for (size_t i = index; i > 0;) {
        --i;
        const ScriptAction& p = actions[i];
        if (p.type != ActionType::FindImage || p.findImageFollowUp != 3) continue;
        std::wstring vn = Trim(p.matchVarName);
        if (vn.empty()) vn = L"image";
        if (vn != name) continue;
        if (p.imagePath.empty()) {
            if (ProducerSearchRectSize(p, w, h)) return true;
            continue;
        }
        if (p.imageRegionX2 > p.imageRegionX1 && p.imageRegionY2 > p.imageRegionY1) {
            w = p.imageRegionX2 - p.imageRegionX1;
            h = p.imageRegionY2 - p.imageRegionY1;
            if (w >= 1 && h >= 1) return true;
        }
        if (!p.imageUseVar && GetTemplateBitmapSize(p.imagePath, w, h) && w > 0 && h > 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

void SyncFindImageOffsetNorm(ScriptAction& a) {
    int tplW = 0, tplH = 0;
    if (!GetTemplateBitmapSize(a.imagePath, tplW, tplH)) return;
    a.nOffsetX = a.offsetX / static_cast<double>(tplW);
    a.nOffsetY = a.offsetY / static_cast<double>(tplH);
}

void SyncMouseDragNorm(ScriptAction& a) {
    if (!a.imageLocate) return;
    if (a.type != ActionType::MouseDrag
        && a.type != ActionType::GetColor
        && a.type != ActionType::ColorMatch) return;
    int tplW = 0, tplH = 0;
    if (!GetTemplateBitmapSize(a.imagePath, tplW, tplH) || tplW <= 0 || tplH <= 0) return;
    a.nx = a.x / static_cast<double>(tplW);
    a.ny = a.y / static_cast<double>(tplH);
    a.nRandomX = a.randomX / static_cast<double>(tplW);
    a.nRandomY = a.randomY / static_cast<double>(tplH);
    if (a.type == ActionType::MouseDrag) {
        a.nEndX = a.endX / static_cast<double>(tplW);
        a.nEndY = a.endY / static_cast<double>(tplH);
        a.nRandomEndX = a.randomEndX / static_cast<double>(tplW);
        a.nRandomEndY = a.randomEndY / static_cast<double>(tplH);
    }
}

namespace {

void DenormFindImageOffsetPixels(ScriptAction& a) {
    int tplW = 0, tplH = 0;
    if (!GetTemplateBitmapSize(a.imagePath, tplW, tplH)) return;
    a.offsetX = static_cast<int>(std::round(a.nOffsetX * tplW));
    a.offsetY = static_cast<int>(std::round(a.nOffsetY * tplH));
}

void DenormMouseDragPixels(ScriptAction& a) {
    if (!a.imageLocate) return;
    if (a.type != ActionType::MouseDrag
        && a.type != ActionType::GetColor
        && a.type != ActionType::ColorMatch) return;
    int tplW = 0, tplH = 0;
    if (!GetTemplateBitmapSize(a.imagePath, tplW, tplH) || tplW <= 0 || tplH <= 0) return;
    a.x = static_cast<int>(std::round(a.nx * tplW));
    a.y = static_cast<int>(std::round(a.ny * tplH));
    a.randomX = static_cast<int>(std::round(a.nRandomX * tplW));
    a.randomY = static_cast<int>(std::round(a.nRandomY * tplH));
    if (a.type == ActionType::MouseDrag) {
        a.endX = static_cast<int>(std::round(a.nEndX * tplW));
        a.endY = static_cast<int>(std::round(a.nEndY * tplH));
        a.randomEndX = static_cast<int>(std::round(a.nRandomEndX * tplW));
        a.randomEndY = static_cast<int>(std::round(a.nRandomEndY * tplH));
    }
}

void ApplyFindImageOffsetNormFromScript(std::vector<ScriptAction>& actions, size_t index) {
    if (index >= actions.size()) return;
    ScriptAction& a = actions[index];
    if (a.type != ActionType::FindImage && a.type != ActionType::MultiMatch) return;
    int tplW = 0, tplH = 0;
    if (!ResolveFindImageOffsetTemplateSize(actions, index, tplW, tplH) || tplW <= 0 || tplH <= 0) {
        return;
    }
    a.nOffsetX = a.offsetX / static_cast<double>(tplW);
    a.nOffsetY = a.offsetY / static_cast<double>(tplH);
}

void ApplyFindImageOffsetDenormFromScript(std::vector<ScriptAction>& actions, size_t index) {
    if (index >= actions.size()) return;
    ScriptAction& a = actions[index];
    if (a.type != ActionType::FindImage && a.type != ActionType::MultiMatch) return;
    int tplW = 0, tplH = 0;
    if (!ResolveFindImageOffsetTemplateSize(actions, index, tplW, tplH) || tplW <= 0 || tplH <= 0) {
        return;
    }
    a.offsetX = static_cast<int>(std::round(a.nOffsetX * tplW));
    a.offsetY = static_cast<int>(std::round(a.nOffsetY * tplH));
}

void ApplyMouseDragNormFromScript(std::vector<ScriptAction>& actions, size_t index) {
    if (index >= actions.size()) return;
    ScriptAction& a = actions[index];
    if (!a.imageLocate) return;
    if (a.type != ActionType::MouseDrag
        && a.type != ActionType::GetColor
        && a.type != ActionType::ColorMatch) return;
    int tplW = 0, tplH = 0;
    if (!ResolveFindImageOffsetTemplateSize(actions, index, tplW, tplH) || tplW <= 0 || tplH <= 0) {
        return;
    }
    a.nx = a.x / static_cast<double>(tplW);
    a.ny = a.y / static_cast<double>(tplH);
    a.nRandomX = a.randomX / static_cast<double>(tplW);
    a.nRandomY = a.randomY / static_cast<double>(tplH);
    if (a.type == ActionType::MouseDrag) {
        a.nEndX = a.endX / static_cast<double>(tplW);
        a.nEndY = a.endY / static_cast<double>(tplH);
        a.nRandomEndX = a.randomEndX / static_cast<double>(tplW);
        a.nRandomEndY = a.randomEndY / static_cast<double>(tplH);
    }
}

void ApplyMouseDragDenormFromScript(std::vector<ScriptAction>& actions, size_t index) {
    if (index >= actions.size()) return;
    ScriptAction& a = actions[index];
    if (!a.imageLocate) return;
    if (a.type != ActionType::MouseDrag
        && a.type != ActionType::GetColor
        && a.type != ActionType::ColorMatch) return;
    int tplW = 0, tplH = 0;
    if (!ResolveFindImageOffsetTemplateSize(actions, index, tplW, tplH) || tplW <= 0 || tplH <= 0) {
        return;
    }
    a.x = static_cast<int>(std::round(a.nx * tplW));
    a.y = static_cast<int>(std::round(a.ny * tplH));
    a.randomX = static_cast<int>(std::round(a.nRandomX * tplW));
    a.randomY = static_cast<int>(std::round(a.nRandomY * tplH));
    if (a.type == ActionType::MouseDrag) {
        a.endX = static_cast<int>(std::round(a.nEndX * tplW));
        a.endY = static_cast<int>(std::round(a.nEndY * tplH));
        a.randomEndX = static_cast<int>(std::round(a.nRandomEndX * tplW));
        a.randomEndY = static_cast<int>(std::round(a.nRandomEndY * tplH));
    }
}

void SyncNormFieldsFromPixelsAction(ScriptAction& a, const CoordMeta& meta) {
    if (a.type == ActionType::MoveMouseRelative) {
        // 相对位移不是屏幕坐标，禁止写入 n*
        a.coordsAreNormalized = false;
        return;
    }
    // 窗口相对录制动作：x/y 即目标窗口客户区像素，跳过屏幕归一化。
    if (a.windowRelative) {
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
    a.nEndX = (a.endX - meta.refOriginX) / rw;
    a.nEndY = (a.endY - meta.refOriginY) / rh;
    a.nRandomEndX = a.randomEndX / rw;
    a.nRandomEndY = a.randomEndY / rh;

    if (!a.searchFullScreen) {
        // search 区域始终为屏幕绝对坐标（OCR「根据图片」的相对偏移在 imageRegion*）
        a.nSearchX1 = (a.searchX1 - meta.refOriginX) / rw;
        a.nSearchY1 = (a.searchY1 - meta.refOriginY) / rh;
        a.nSearchX2 = (a.searchX2 - meta.refOriginX) / rw;
        a.nSearchY2 = (a.searchY2 - meta.refOriginY) / rh;
    } else {
        a.nSearchX1 = 0.0;
        a.nSearchY1 = 0.0;
        a.nSearchX2 = 0.0;
        a.nSearchY2 = 0.0;
    }

    a.nOffsetX = a.offsetX / rw;
    a.nOffsetY = a.offsetY / rh;
    if (a.type == ActionType::FindImage || a.type == ActionType::MultiMatch) {
        SyncFindImageOffsetNorm(a);
    }
    if (a.type == ActionType::MouseDrag
        || a.type == ActionType::GetColor
        || a.type == ActionType::ColorMatch) {
        SyncMouseDragNorm(a);
    }

    // 模板内相对偏移：按参考分辨率归一化（与历史 OCR 锚点行为一致）
    a.nImageRegionX1 = a.imageRegionX1 / rw;
    a.nImageRegionY1 = a.imageRegionY1 / rh;
    a.nImageRegionX2 = a.imageRegionX2 / rw;
    a.nImageRegionY2 = a.imageRegionY2 / rh;

    // AI 识别区域始终按屏幕绝对坐标归一化
    const bool isAiRegionAction = (a.type == ActionType::AiImageAnalysis
        || a.type == ActionType::AiActionExecute);
    if (a.aiSearchRegion == 6
        || (isAiRegionAction && (a.aiSearchX2 > a.aiSearchX1 || a.searchFullScreen))) {
        a.nAiSearchX1 = (a.aiSearchX1 - meta.refOriginX) / rw;
        a.nAiSearchY1 = (a.aiSearchY1 - meta.refOriginY) / rh;
        a.nAiSearchX2 = (a.aiSearchX2 - meta.refOriginX) / rw;
        a.nAiSearchY2 = (a.aiSearchY2 - meta.refOriginY) / rh;
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
    for (size_t i = 0; i < actions.size(); ++i) {
        NormalizeActionCoords(actions[i], meta);
        ApplyFindImageOffsetNormFromScript(actions, i);
        ApplyMouseDragNormFromScript(actions, i);
    }
}

void DenormalizeActionCoords(ScriptAction& a, const CoordMeta& meta, int targetW, int targetH) {
    if (a.type == ActionType::MoveMouseRelative) return;
    if (a.windowRelative) return;  // 窗口相对坐标保持客户区像素
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
    a.endX = vsX + static_cast<int>(std::round(a.nEndX * tw));
    a.endY = vsY + static_cast<int>(std::round(a.nEndY * th));
    a.randomEndX = static_cast<int>(std::round(a.nRandomEndX * tw));
    a.randomEndY = static_cast<int>(std::round(a.nRandomEndY * th));

    const bool hasSearch = (a.nSearchX2 > a.nSearchX1 || a.nSearchY2 > a.nSearchY1);
    if (hasSearch) {
        a.searchX1 = vsX + static_cast<int>(std::round(a.nSearchX1 * tw));
        a.searchY1 = vsY + static_cast<int>(std::round(a.nSearchY1 * th));
        a.searchX2 = vsX + static_cast<int>(std::round(a.nSearchX2 * tw));
        a.searchY2 = vsY + static_cast<int>(std::round(a.nSearchY2 * th));
    }

    a.offsetX = static_cast<int>(std::round(a.nOffsetX * tw));
    a.offsetY = static_cast<int>(std::round(a.nOffsetY * th));
    if (a.type == ActionType::FindImage || a.type == ActionType::MultiMatch) {
        DenormFindImageOffsetPixels(a);
    }
    if (a.type == ActionType::MouseDrag
        || a.type == ActionType::GetColor
        || a.type == ActionType::ColorMatch) {
        DenormMouseDragPixels(a);
    }

    a.imageRegionX1 = static_cast<int>(std::round(a.nImageRegionX1 * tw));
    a.imageRegionY1 = static_cast<int>(std::round(a.nImageRegionY1 * th));
    a.imageRegionX2 = static_cast<int>(std::round(a.nImageRegionX2 * tw));
    a.imageRegionY2 = static_cast<int>(std::round(a.nImageRegionY2 * th));

    const bool hasAiSearch = (a.nAiSearchX2 > a.nAiSearchX1 || a.nAiSearchY2 > a.nAiSearchY1);
    if (hasAiSearch) {
        a.aiSearchX1 = vsX + static_cast<int>(std::round(a.nAiSearchX1 * tw));
        a.aiSearchY1 = vsY + static_cast<int>(std::round(a.nAiSearchY1 * th));
        a.aiSearchX2 = vsX + static_cast<int>(std::round(a.nAiSearchX2 * tw));
        a.aiSearchY2 = vsY + static_cast<int>(std::round(a.nAiSearchY2 * th));
    }
}

void DenormalizeScriptCoords(std::vector<ScriptAction>& actions, const CoordMeta& meta,
    int targetW, int targetH) {
    for (size_t i = 0; i < actions.size(); ++i) {
        if (!actions[i].coordsAreNormalized) continue;
        DenormalizeActionCoords(actions[i], meta, targetW, targetH);
        ApplyFindImageOffsetDenormFromScript(actions, i);
        ApplyMouseDragDenormFromScript(actions, i);
    }
}

void MigrateLegacyScriptToNormalized(std::vector<ScriptAction>& actions,
    const CoordMeta& assumedRef) {
    const double rw = static_cast<double>(assumedRef.refWidth);
    const double rh = static_cast<double>(assumedRef.refHeight);
    if (rw <= 0 || rh <= 0) return;

    for (size_t i = 0; i < actions.size(); ++i) {
        ScriptAction& a = actions[i];
        if (a.type == ActionType::MoveMouseRelative) {
            a.coordsAreNormalized = false;
            continue;
        }
        a.nx = (a.x - assumedRef.refOriginX) / rw;
        a.ny = (a.y - assumedRef.refOriginY) / rh;
        a.nRandomX = a.randomX / rw;
        a.nRandomY = a.randomY / rh;
        a.nEndX = (a.endX - assumedRef.refOriginX) / rw;
        a.nEndY = (a.endY - assumedRef.refOriginY) / rh;
        a.nRandomEndX = a.randomEndX / rw;
        a.nRandomEndY = a.randomEndY / rh;

        if (!a.searchFullScreen) {
            a.nSearchX1 = (a.searchX1 - assumedRef.refOriginX) / rw;
            a.nSearchY1 = (a.searchY1 - assumedRef.refOriginY) / rh;
            a.nSearchX2 = (a.searchX2 - assumedRef.refOriginX) / rw;
            a.nSearchY2 = (a.searchY2 - assumedRef.refOriginY) / rh;
        }

        a.nOffsetX = a.offsetX / rw;
        a.nOffsetY = a.offsetY / rh;
        if (a.type == ActionType::FindImage || a.type == ActionType::MultiMatch) {
            SyncFindImageOffsetNorm(a);
        }
        if (a.type == ActionType::MouseDrag
            || a.type == ActionType::GetColor
            || a.type == ActionType::ColorMatch) {
            SyncMouseDragNorm(a);
        }
        ApplyFindImageOffsetNormFromScript(actions, i);
        ApplyMouseDragNormFromScript(actions, i);

        a.nImageRegionX1 = a.imageRegionX1 / rw;
        a.nImageRegionY1 = a.imageRegionY1 / rh;
        a.nImageRegionX2 = a.imageRegionX2 / rw;
        a.nImageRegionY2 = a.imageRegionY2 / rh;

        if (a.aiSearchRegion == 6
            || ((a.type == ActionType::AiImageAnalysis || a.type == ActionType::AiActionExecute)
                && (a.aiSearchX2 > a.aiSearchX1 || a.searchFullScreen))) {
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
    for (size_t i = 0; i < actions.size(); ++i) {
        SyncNormFieldsFromPixelsAction(actions[i], captureMeta);
        ApplyFindImageOffsetNormFromScript(actions, i);
        ApplyMouseDragNormFromScript(actions, i);
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

    if (action.perfectMatch) {
        // 完美匹配：强制 1:1 + 关金字塔；通过与否由像素终审决定（不走阈值）
        opt.perfectMatch = true;
        opt.perfectMatchChannelTol = 1;
        opt.scaleMin = 1.0;
        opt.scaleMax = 1.0;
        opt.scaleStep = 1.0;
        opt.disablePyramid = true;
        opt.crossResolutionMatch = false;
        RestrictFindImageToSingleAnchor(opt);
        opt.maxOverlap = 0.5;
        return opt;
    }

    double userMin = action.imageScaleMin > 0.0 ? action.imageScaleMin : action.imageScale;
    double userMax = action.imageScaleMax > 0.0 ? action.imageScaleMax : userMin;
    if (userMax < userMin) userMax = userMin;
    // 防 CPU 风暴：脚本给超大 scale 区间时全帧 matchTemplate 会跑几分钟。
    constexpr double kMinScale = 0.1;
    constexpr double kMaxScale = 4.0;
    userMin = std::clamp(userMin, kMinScale, kMaxScale);
    userMax = std::clamp(userMax, kMinScale, kMaxScale);
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
        // 采样数上限：区间过大时放大步长，防止 matchTemplate 风暴（上限约 64 次）。
        constexpr double kMaxSamples = 64.0;
        const double span = userMax - userMin;
        opt.scaleStep = span > kMaxSamples * 0.05 ? span / kMaxSamples : 0.05;
    }

    RestrictFindImageToSingleAnchor(opt);
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
    if (src.channels() == 4) {
        std::vector<cv::Mat> ch;
        cv::split(src, ch);
        cv::Mat bgr;
        cv::merge(std::vector<cv::Mat>{ch[0], ch[1], ch[2]}, bgr);
        cv::Mat bgrR;
        cv::Mat aR;
        cv::resize(bgr, bgrR, cv::Size(newW, newH), 0, 0, cv::INTER_AREA);
        cv::resize(ch[3], aR, cv::Size(newW, newH), 0, 0, cv::INTER_NEAREST);
        std::vector<cv::Mat> outCh;
        cv::split(bgrR, outCh);
        outCh.push_back(aR);
        cv::merge(outCh, dst);
    } else {
        cv::resize(src, dst, cv::Size(newW, newH), 0, 0, cv::INTER_LINEAR);
    }
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
    std::wstring path = action.imagePath;
    if (!path.empty() && !action.imageUseVar) {
        const std::wstring resolved = ResolveImagePath(path);
        if (!resolved.empty()) path = resolved;
    }
    prep.bitmap = LoadBitmapFromFile(path);
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
    int cx = 0;
    int cy = 0;
    FindImageMatchCenter(match, cx, cy);

    // nOffset 相对模板宽高。命中框可能已被窗口模式从截图像素映射到客户区，
    // 必须跟框同一坐标系加偏移，不能再用 origTpl×match.scale（仍是模板/截图空间）。
    const int boxW = match.bottomRightX - match.topLeftX;
    const int boxH = match.bottomRightY - match.topLeftY;
    if (boxW > 0 && boxH > 0) {
        tx = cx + static_cast<int>(std::round(nOffsetX * boxW));
        ty = cy + static_cast<int>(std::round(nOffsetY * boxH));
        return;
    }

    const double matchScale = match.scale > 0.0 ? match.scale : 1.0;
    const double offSx = templatePreScaled
        ? (tmplScale.sx > 0.0 ? tmplScale.sx : 1.0) * matchScale
        : matchScale;
    const double offSy = templatePreScaled
        ? (tmplScale.sy > 0.0 ? tmplScale.sy : 1.0) * matchScale
        : matchScale;
    tx = cx + static_cast<int>(std::round(nOffsetX * origTplW * offSx));
    ty = cy + static_cast<int>(std::round(nOffsetY * origTplH * offSy));
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

    // ExtractString/Number 只认当前对象顶层键；脚本里这些字段在 coordMeta 内。
    const std::wstring* src = &content;
    std::wstring nested;
    const std::wstring kCoordMetaKey = L"\"coordMeta\"";
    const auto keyPos = content.find(kCoordMetaKey);
    if (keyPos != std::wstring::npos) {
        const auto brace = content.find(L'{', keyPos + kCoordMetaKey.size());
        if (brace != std::wstring::npos) {
            const auto end = FindMatchingJsonBrace(content, brace);
            if (end != std::wstring::npos) {
                nested = content.substr(brace, end - brace + 1);
                src = &nested;
            }
        }
    }

    meta.version = static_cast<int>(ExtractNumber(*src, L"version", 1));
    const auto space = ExtractString(*src, L"space");
    meta.space = (space == L"windowClient")
        ? CoordMeta::Space::WindowClient : CoordMeta::Space::ScreenVirtual;
    meta.refOriginX = static_cast<int>(ExtractNumber(*src, L"refOriginX", 0));
    meta.refOriginY = static_cast<int>(ExtractNumber(*src, L"refOriginY", 0));
    meta.refWidth = static_cast<int>(ExtractNumber(*src, L"refWidth", 0));
    meta.refHeight = static_cast<int>(ExtractNumber(*src, L"refHeight", 0));
    meta.refDpi = static_cast<int>(ExtractNumber(*src, L"refDpi", 96));
    meta.captureWidth = static_cast<int>(ExtractNumber(*src, L"captureWidth", 0));
    meta.captureHeight = static_cast<int>(ExtractNumber(*src, L"captureHeight", 0));
    return meta;
}

bool HasCoordMetaJson(const std::wstring& content) {
    return content.find(L"\"coordMeta\"") != std::wstring::npos;
}
