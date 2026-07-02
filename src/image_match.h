#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct ImageMatchResult {
    bool found = false;
    int x = 0;
    int y = 0;
    int topLeftX = 0;
    int topLeftY = 0;
    int bottomRightX = 0;
    int bottomRightY = 0;
    double score = 0.0;
    double scale = 1.0;
};

struct ImageMatchOptions {
    double thresholdPercent = 65.0;
    double scaleMin = 1.0;
    double scaleMax = 1.0;
    double scaleStep = 0.05;
    int maxMatches = 20;
    double maxOverlap = 0.5;
};

struct ImageMatchOutput {
    bool found = false;
    int elapsedMs = 0;
    std::vector<ImageMatchResult> matches;
};

HBITMAP LoadBitmapFromFile(const std::wstring& path);
bool SaveBitmapToFile(HBITMAP bitmap, const std::wstring& path);
void DeleteBitmapHandle(HBITMAP bitmap);

HBITMAP CaptureScreenRegion(int x1, int y1, int x2, int y2);
void GetVirtualScreenRect(int& x, int& y, int& w, int& h);

ImageMatchOutput FindTemplateOnScreenMulti(
    int searchX1, int searchY1, int searchX2, int searchY2,
    HBITMAP templateBmp, const ImageMatchOptions& options);

ImageMatchOutput FindTemplateInFrozenScreenMulti(
    HBITMAP frozenScreen, int virtX, int virtY,
    int searchX1, int searchY1, int searchX2, int searchY2,
    HBITMAP templateBmp, const ImageMatchOptions& options);

ImageMatchResult FindTemplateOnScreen(
    int searchX1, int searchY1, int searchX2, int searchY2,
    HBITMAP templateBmp, double thresholdPercent, double scale,
    double scaleMax = 0.0);

ImageMatchResult FindTemplateInFrozenScreen(
    HBITMAP frozenScreen, int virtX, int virtY,
    int searchX1, int searchY1, int searchX2, int searchY2,
    HBITMAP templateBmp, double thresholdPercent, double scale,
    int* outTemplateW = nullptr, int* outTemplateH = nullptr,
    double scaleMax = 0.0);

void SendMouseWheel(int steps, bool vertical, bool horizontal, bool positive);
