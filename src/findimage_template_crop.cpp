// ──────────────────────────────────────────────────────────────────
// findimage_template_crop.cpp — 找图模板裁切 offset 变换
// ──────────────────────────────────────────────────────────────────

#include "findimage_template_crop.h"

#include <algorithm>

CropRect NormalizeCropRect(int x0, int y0, int x1, int y1) {
    CropRect r{};
    r.L = (std::min)(x0, x1);
    r.T = (std::min)(y0, y1);
    r.R = (std::max)(x0, x1);
    r.B = (std::max)(y0, y1);
    return r;
}

CropRect ClampCropRectToImage(CropRect r, int W, int H) {
    if (W <= 0 || H <= 0) {
        return CropRect{0, 0, 0, 0};
    }
    r.L = (std::max)(0, (std::min)(r.L, W));
    r.T = (std::max)(0, (std::min)(r.T, H));
    r.R = (std::max)(0, (std::min)(r.R, W));
    r.B = (std::max)(0, (std::min)(r.B, H));
    if (r.R < r.L) r.R = r.L;
    if (r.B < r.T) r.B = r.T;
    return r;
}

bool IsFullImageCrop(CropRect r, int W, int H) {
    return W > 0 && H > 0 && r.L == 0 && r.T == 0 && r.R == W && r.B == H;
}

bool IsClickInsideImage(int clickX, int clickY, int W, int H) {
    return W > 0 && H > 0 && clickX >= 0 && clickY >= 0 && clickX < W && clickY < H;
}

CropOffsetResult ComputeCroppedFindImageOffset(
    int W, int H, int offsetX, int offsetY, CropRect raw) {
    CropOffsetResult out{};
    if (W < kFindImageCropMinSide || H < kFindImageCropMinSide) {
        out.reject = CropOffsetReject::InvalidRect;
        return out;
    }

    CropRect rect = ClampCropRectToImage(NormalizeCropRect(raw.L, raw.T, raw.R, raw.B), W, H);
    out.rect = rect;
    const int cw = rect.R - rect.L;
    const int ch = rect.B - rect.T;
    if (cw <= 0 || ch <= 0) {
        out.reject = CropOffsetReject::EmptyAfterClamp;
        return out;
    }
    if (cw < kFindImageCropMinSide || ch < kFindImageCropMinSide) {
        out.reject = CropOffsetReject::MinSide;
        return out;
    }

    const int oldCx = W / 2;
    const int oldCy = H / 2;
    const int clickX = oldCx + offsetX;
    const int clickY = oldCy + offsetY;

    if (IsFullImageCrop(rect, W, H)) {
        out.ok = true;
        out.reject = CropOffsetReject::None;
        out.offsetX = offsetX;
        out.offsetY = offsetY;
        return out;
    }

    // 落点可在裁切框外（相对特征图的点击偏移）；仅按公式换算
    out.ok = true;
    out.reject = CropOffsetReject::None;
    out.offsetX = (clickX - rect.L) - cw / 2;
    out.offsetY = (clickY - rect.T) - ch / 2;
    return out;
}

std::wstring MakeFindImageCropFileName(unsigned long long tick, unsigned seq) {
    return L"crop_" + std::to_wstring(tick) + L"_" + std::to_wstring(seq) + L".bmp";
}
