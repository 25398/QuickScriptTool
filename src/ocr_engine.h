// ──────────────────────────────────────────────────────────────────
// ocr_engine.h — RapidOCR (ONNX) 文字识别；旧版 PaddleOCR 仍可作回退
// ──────────────────────────────────────────────────────────────────
#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <string>

#include "ocr_result.h"

enum class OcrEnvState {
    Ready,
    NotInstalled,
    MissingHelper,
    MissingDeps,
};

struct OcrEnvStatus {
    OcrEnvState state = OcrEnvState::NotInstalled;
    std::wstring detail;
};

// OCR Python 解释器路径（固定 venv）
std::wstring OcrPythonPath();

// 检测 OCR 运行环境（verifyImport=true 时会尝试 import rapidocr / paddleocr，较慢）
OcrEnvStatus CheckOcrEnvironment(bool verifyImport = false);

// 一键安装/修复 OCR 依赖（自动安装 Python 3.12、创建 venv 并 pip install），messageOut 为结果说明
using OcrInstallProgressFn = std::function<void(int percent, const std::wstring& status)>;
bool RunOcrInstall(std::wstring& messageOut, OcrInstallProgressFn onProgress = nullptr);

// 宏运行期间复用同一 OCR worker 进程（引用计数，全部 Release 后关闭）
void EnsureOcrSession();
void ReleaseOcrSession();
bool IsOcrSessionActive();

// 解析 worker 输出的 JSON（自检 / 调试）
OcrEngineOutput ParseOcrEngineJson(const std::string& jsonRaw);

// 对屏幕区域执行 OCR（支持锁定截图）
OcrEngineOutput RunOcrOnScreenRegion(
    int searchX1, int searchY1, int searchX2, int searchY2,
    HBITMAP frozenScreen = nullptr, int frozenVirtX = 0, int frozenVirtY = 0,
    bool digitsOnly = false);

// 对已有位图执行 OCR（坐标偏移用于将识别框映射回屏幕坐标）
OcrEngineOutput RunOcrOnBitmap(
    HBITMAP bitmap, int coordOffsetX, int coordOffsetY, bool digitsOnly = false);

// 拼接所有识别行为单个字符串
std::wstring ConcatOcrLines(const OcrEngineOutput& output);

// 文字查找命中结果：命中行 + **匹配度**（0~100，与找图 `.matchData` 同一把尺）
struct OcrTextMatch {
    OcrTextLine line;
    /// 0 = 未命中；精确/归一化/邻行拼接命中 = 100；模糊命中 = 相似度百分比（≥82）
    int matchData = 0;
};

// 在 OCR 结果中查找目标文字（精确子串 → 全半角/去空白 → 邻行拼接 → 模糊），
// 连同匹配度一起返回。★「文字查找 + 保存匹配度」落进变量 `.matchData` 的就是它。
std::optional<OcrTextMatch> FindTextInOcrLinesScored(
    const OcrEngineOutput& output, const std::wstring& target);

// 兼容薄封装：只要命中的那一行（叠层高亮等不需要匹配度的地方用）。
// ⚠ 它内部就是上面那个函数，**命中判定只有一份实现**，别在这里另写一遍。
std::optional<OcrTextLine> FindTextInOcrLines(
    const OcrEngineOutput& output, const std::wstring& target);

// 将 OCR 查找结果转为变量结构
OcrVarResult MakeOcrSearchVarResult(const OcrTextLine& line, int matchData);
OcrVarResult MakeOcrSearchMissingVarResult();
OcrVarResult MakeOcrTextVarResult(const std::wstring& text);

// 把 HBITMAP 存成 **PNG**（GDI+ 编码）。
// ⚠ 别用 `SaveBitmapToFile` 存大图 —— 它写的是**未压缩 BMP**（1024 宽就 ~2.6MB），
//    本函数给的是 PNG（同尺寸通常小十几倍）。`zoom` 工具回传放大图走的就是这条。
bool SaveHbitmapPng(HBITMAP bitmap, const std::wstring& path);

/// 把 HBITMAP 存成 **JPEG**（`zoom` 回传放大图靠它）：质量与送图链路**同一套**参数
/// （q82 + 长边 ≤ `maxLongEdge`），于是**磁盘上的字节数就是模型拿到的字节数**。
///
/// ★★为什么 `zoom` 不用 PNG（实测教训）：交出去的图**最终**会被附件链路重新编码成
///   「JPEG q82 + 长边 ≤ 1280」再进请求。写 PNG 等于 ① 先在磁盘上造一个巨大的中间产物
///   （实测一张 1150×725 的游戏裁剪是 **1365 KB**），② 回执只能拿 PNG 的字节数报成本，
///   而模型手里那张是缩过、重编过的 JPEG ⇒ **回执描述的和交出去的不是同一个东西**
///   （本仓反复踩的那条：交出去的和消费者拿到的不同时，回执必须按后者写）。
///   这里**一次就编成交付形态**；`outWidth/outHeight/outBytes` 回的是**真正落盘那张**的数值，
///   回执直接引用它们，不可能再说谎。
/// ⚠ 内部经 OpenCV `imencode`（已守 `OpenCvAvailable()`）；无 OpenCV 时返回 false，
///   调用方必须**如实报失败**，别假装拿到了图。
bool SaveHbitmapJpeg(HBITMAP bitmap, const std::wstring& path, int quality, int maxLongEdge,
    int* outWidth = nullptr, int* outHeight = nullptr, size_t* outBytes = nullptr);
