#pragma once
// ──────────────────────────────────────────────────────────────────
// findimage_template_crop.h — 找图模板裁切 offset 变换纯逻辑
// ──────────────────────────────────────────────────────────────────

#include <string>

constexpr int kFindImageCropMinSide = 8;

enum class CropOffsetReject {
    None = 0,
    InvalidRect,
    EmptyAfterClamp,
    MinSide,
    ClickOutsideCrop,
};

struct CropRect {
    int L = 0;
    int T = 0;
    int R = 0;  // exclusive
    int B = 0;  // exclusive
};

CropRect NormalizeCropRect(int x0, int y0, int x1, int y1);
CropRect ClampCropRectToImage(CropRect r, int W, int H);
bool IsFullImageCrop(CropRect r, int W, int H);
bool IsClickInsideImage(int clickX, int clickY, int W, int H);

struct CropOffsetResult {
    bool ok = false;
    CropOffsetReject reject = CropOffsetReject::InvalidRect;
    int offsetX = 0;
    int offsetY = 0;
    CropRect rect{};
};

/// 内部先 Normalize 再 Clamp。全图返回 identity（ok=true，offset 不变）。
CropOffsetResult ComputeCroppedFindImageOffset(
    int W, int H, int offsetX, int offsetY, CropRect raw);

/// crop_{tick}_{seq}.bmp
std::wstring MakeFindImageCropFileName(unsigned long long tick, unsigned seq);
