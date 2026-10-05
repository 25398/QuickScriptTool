#pragma once
// ──────────────────────────────────────────────────────────────────
// recorder_diag_log.h — 录制/回放精度诊断行的落盘裁剪（纯逻辑，可自检）
//
// 【为什么必须落盘】
// `[鼠标报告]` / `[录制结束]` / `[回放保真]` 是判定「偏差在输入层还是
// 目标侧」的唯一依据。但原先它们只在**宏调试窗口已打开**时可见，而调试
// 浮窗只有 钉 / 最小化 / 关闭 —— **没有复制或导出**，日志只能靠手选。
// 于是「跑一段录制把三行发我」这个验证循环实际上不可执行。
// 落盘后：用户不必开任何窗口，直接把 `recorder_diag.log` 发出来即可。
//
// 【裁剪唯一容易错的地方是行边界】
// 文件是无限追加的，超限后要从尾部取一段定长字节写回。若直接按字节数切，
// 很可能把一行切成半行 —— 日志开头就是乱码，比不裁还糟。
// 所以这里只做一件事：给定「从尾部取到的那一块」，算出**第一条完整行的起点**。
//
// 约定：宁可多留一点。裁剪只是为了不让文件无限增长，不该为省几十字节
// 而丢掉唯一一行诊断（找不到换行 ⇒ 返回 0 = 整块全留）。
// ──────────────────────────────────────────────────────────────────

#include <cstddef>
#include <string>

namespace qst_recorder {

/// `recorder_diag.log` 的大小上限（字节）。超出后保留尾部约一半。
constexpr long long kRecorderDiagMaxBytes = 256 * 1024;

/// 裁剪时从尾部保留的宽字符数（日志是 UTF-16LE，2 字节/字符）。
///
/// ★上限是**参数**：`recorder_diag.log` 与 `ai_action_debug.log`（docs §61）共用同一条
///   公式，只是上限不同。**别在调用点另写一遍这个算术** —— 一处事实一份逻辑。
inline size_t DiagTrimKeepChars(long long maxBytes) {
    return static_cast<size_t>(maxBytes / 2) / sizeof(wchar_t);
}

inline size_t RecorderDiagTrimKeepChars() {
    return DiagTrimKeepChars(kRecorderDiagMaxBytes);
}

/// 是否该裁剪（纯函数）。`sizeBytes` = 当前文件字节数。
inline bool DiagLogNeedsTrim(long long sizeBytes, long long maxBytes) {
    return maxBytes > 0 && sizeBytes > maxBytes;
}

/// 单行裁剪（纯函数）：超长行截断，并**如实标注**丢了多少字。
///
/// 为什么必须有：裁剪是按**行边界**切的，所以一行几万字（请求体/图片 base64 之类）
/// 会把文件瞬间顶满、把其余所有行挤掉。而"悄悄截断"会让读到的人以为那就是全部 ⇒
/// 截断必须留痕（日志不许说谎）。
template <typename WStr>
WStr ClampDiagLogLine(const WStr& line, size_t maxChars) {
    if (maxChars == 0 || line.size() <= maxChars) return line;
    const size_t dropped = line.size() - maxChars;
    WStr out = line.substr(0, maxChars);
    out += L"…（本行已截断 ";
    out += std::to_wstring(dropped);
    out += L" 字）";
    return out;
}

/// 在 `buf[0..count)` 中找第一个换行**之后**的位置，即第一条完整行的起点。
///
/// - 找到换行且后面还有内容 ⇒ 返回 `换行下标 + 1`
/// - 整块没有换行 ⇒ 返回 0（整块全留）
/// - 换行恰在最后一位 ⇒ 返回 0（整块全留）
inline size_t RecorderDiagLineStart(const wchar_t* buf, size_t count) {
    if (!buf || count == 0) return 0;
    for (size_t i = 0; i < count; ++i) {
        if (buf[i] == L'\n') return (i + 1 < count) ? (i + 1) : 0;
    }
    return 0;
}

}  // namespace qst_recorder
