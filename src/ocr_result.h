// ──────────────────────────────────────────────────────────────────
// ocr_result.h — 文字识别结果变量
// ──────────────────────────────────────────────────────────────────
#pragma once

#include <string>
#include <vector>

enum class OcrVarMode {
    Text,    // 获取文字：变量值为识别到的字符串
    Search   // 文字查找：变量值为 0/1，附带坐标属性
};

struct OcrVarResult {
    OcrVarMode mode = OcrVarMode::Text;
    std::wstring text;
    int found = 0;
    int topLeftX = 0;
    int topLeftY = 0;
    int bottomRightX = 0;
    int bottomRightY = 0;
};

struct OcrTextLine {
    std::wstring text;
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    double confidence = 0.0;
};

struct OcrEngineOutput {
    bool success = false;
    std::wstring error;
    std::vector<OcrTextLine> lines;
    int elapsedMs = 0;
};
