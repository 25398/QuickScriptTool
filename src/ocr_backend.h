#pragma once
// ──────────────────────────────────────────────────────────────────
// ocr_backend.h — OCR 后端选择 + 系统自带（WinRT）OCR
//
// 为什么需要这一层：
//   产品内的 OCR 是 **Python 依赖**（`C:\paddle_env\venv` + RapidOCR/PaddleOCR），
//   导出的独立 EXE 没法把它自包含进去。而 Windows 10/11 **自带**
//   `Windows.Media.Ocr`，零安装、零体积、可离线 —— 正好满足「发给别人就能跑」。
//
// 设计：
//   · 产品内**保持 Python 后端不变**（默认 `OcrBackend::Python`，零回归风险）
//   · 播放器按导出时用户的选择调 `SetOcrBackendPreference`
//   · `Auto` = **Python 优先**，Python 不可用才回退系统 OCR。
//     这是导出「走软件」模式用的 —— 用户期望和软件里跑出一样的结果，
//     而软件里用的就是 PaddleOCR。反过来（先试系统 OCR）会让「走软件」
//     永远用不上软件里的引擎，结果与软件内不一致（实测踩过）。
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <string>
#include <vector>

#include "ocr_result.h"

enum class OcrBackend {
    Python = 0,  // 产品默认：venv + RapidOCR/PaddleOCR
    Auto = 1,    // Python 优先；不可用才回退系统 OCR（导出「走软件」模式）
    WinRt = 2,   // 只用系统 OCR（失败就把错误原样返回，便于给出可读提示）
};

/// 诊断回调：每次 OCR 完成后回调**一行可读摘要**
/// （用了哪个后端 / 成功失败 / 读到多少字 / 前若干字符）。
///
/// 为什么需要：OCR 类脚本在导出的 exe 上"不生效"时，最难判断的就是
/// **到底有没有识别到、识别成了什么**。产品内不装这个 sink（零输出），
/// 播放器把它接到 `player.log`，用户自查/客服排查都靠它。
/// 传 nullptr 关闭。
void SetOcrDiagnosticSink(void (*sink)(const std::wstring&));

/// 按偏好给出**要依次尝试的后端**（纯逻辑，便于自检）。
///   Python → {Python}
///   WinRt  → {WinRt}
///   Auto   → {Python, WinRt}   ← **Python 优先**，见下面 Auto 的说明
std::vector<OcrBackend> OcrBackendAttemptOrder(OcrBackend pref);

/// 设置后端偏好（进程级）。产品不调它 ⇒ 保持 Python。
void SetOcrBackendPreference(OcrBackend backend);
OcrBackend OcrBackendPreference();

/// 系统自带 OCR 是否可用（会真正尝试创建识别器，含语言包检查）。
/// 不可用时 `reason` 填可读原因（"缺少中文 OCR 语言包"之类）。
bool WinRtOcrAvailable(std::wstring* reason = nullptr);

/// 把 OCR 一行的**词**拼成文本：CJK 之间不加空格，拉丁词之间保留空格。
///
/// ⚠ 不能直接用 `OcrLine.Text()` —— WinRT 对中日韩会把**每个字**当成一个 word，
///   `Text()` 于是返回 `"新 建 任 务"`（汉字之间全被塞了空格），
///   下游的文本匹配 / `numbers()` 取数 / 找字全乱。实测踩过。
std::wstring JoinOcrWords(const std::vector<std::wstring>& words);

/// 用系统自带 OCR 识别一张位图。失败时 `output.error` 是可读原因。
OcrEngineOutput RunWinRtOcr(HBITMAP bitmap, bool digitsOnly);
