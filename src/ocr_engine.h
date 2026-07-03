// ──────────────────────────────────────────────────────────────────
// ocr_engine.h — PaddleOCR 文字识别引擎封装
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

// PaddleOCR Python 解释器路径（固定 venv）
std::wstring OcrPythonPath();

// 检测 OCR 运行环境（verifyImport=true 时会尝试 import paddleocr，较慢）
OcrEnvStatus CheckOcrEnvironment(bool verifyImport = false);

// 一键安装/修复 OCR 依赖（创建 venv 并 pip install），messageOut 为结果说明
using OcrInstallProgressFn = std::function<void(int percent, const std::wstring& status)>;
bool RunOcrInstall(std::wstring& messageOut, OcrInstallProgressFn onProgress = nullptr);

// 宏运行期间复用同一 Python/PaddleOCR 进程（引用计数，全部 Release 后关闭）
void EnsureOcrSession();
void ReleaseOcrSession();
bool IsOcrSessionActive();

// 对屏幕区域执行 OCR（支持锁定截图）
OcrEngineOutput RunOcrOnScreenRegion(
    int searchX1, int searchY1, int searchX2, int searchY2,
    HBITMAP frozenScreen = nullptr, int frozenVirtX = 0, int frozenVirtY = 0,
    bool digitsOnly = false);

// 拼接所有识别行为单个字符串
std::wstring ConcatOcrLines(const OcrEngineOutput& output);

// 在 OCR 结果中查找目标文字（子串匹配），返回第一个匹配行
std::optional<OcrTextLine> FindTextInOcrLines(
    const OcrEngineOutput& output, const std::wstring& target);

// 将 OCR 查找结果转为变量结构
OcrVarResult MakeOcrSearchVarResult(const OcrTextLine& line, bool found);
OcrVarResult MakeOcrTextVarResult(const std::wstring& text);
