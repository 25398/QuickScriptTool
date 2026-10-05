// ──────────────────────────────────────────────────────────────────
// ocr_result.h — 文字识别结果变量
// ──────────────────────────────────────────────────────────────────
#pragma once

#include <string>
#include <vector>

enum class OcrVarMode {
    Text,    // 获取文字：变量值为识别到的字符串
    Search   // 文字查找：变量值为 0/1，附带匹配度（.matchData）与坐标属性
};

struct OcrVarResult {
    OcrVarMode mode = OcrVarMode::Text;
    std::wstring text;
    int found = 0;
    /// ★文字查找的**匹配度（0~100）**，与找图 `.matchData` **同一把尺**
    /// （精确/归一化/邻行拼接命中记 100，模糊命中记相似度百分比，未命中记 0）。
    /// ⚠ 它**不是** `{变量}` 的值 —— `{变量}` 仍是 0/1，匹配度读 `{变量}.matchData`。
    ///   把两者合并会让既有脚本的 `if({变量})` / `{变量} > 0` 语义悄悄改变。
    int matchData = 0;
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
    /// ★**实际**给出这行结果的 OCR 后端（`软件引擎` = Python venv 里的
    /// RapidOCR/PaddleOCR；`系统OCR` = Windows WinRT）。
    ///
    /// 为什么要带上它（docs §70）：`RequestOcrOnHbitmap` 是按偏好顺序**依次尝试**的
    /// （Auto 档：Python 优先，不可用才回退系统 OCR），而日志原先只说
    /// 「OCR 返回 10843ms（34 行）」——**哪个引擎答的、有没有偷偷回退**，一个字都没有。
    /// 两个后端的耗时能差一个数量级，这正是「慢」的第一现场；回退一旦发生，
    /// 没有这一行就只能靠猜。判据从这里出，别在别处重算（重算就可能与真值不同）。
    std::wstring backend;
};
