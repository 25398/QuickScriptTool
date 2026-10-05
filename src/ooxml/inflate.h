#pragma once
// ──────────────────────────────────────────────────────────────────
// inflate.h — 原始 DEFLATE（RFC 1951）解压
//
// 为什么自己写（而不是引入 zlib）：
//   · OOXML（.xlsx/.docx/.pptx）本质是 **zip 容器**，而真实 Office 文件的条目用的是
//     **deflate（method 8）**。本仓原有的 zip 代码（`utils.cpp` 的 `ExtractZipFile` /
//     `ReadTextFromZip`）只按 `compSize` **原样拷贝字节、根本不解压** ——
//     也就是说它读不了任何真实 Office 文件。
//   · 产品原则：**不依赖别人的软件/库运行**。而且 `third_party/opencv-src` 里没有可复用的
//     zlib 源码，引入一份就要多维护一份第三方代码 + 许可证声明。
//   · 本仓已有自研 `ComputeCrc32` + zip 读写（`utils.cpp`），风格上是一致的。
//
// 范围：**只做解压**（读 Office 文件必需）。写的时候用 method 0（stored）——
//       Excel/Word 完全接受，省掉一整个压缩器的实现与风险。
//
// 纯逻辑：不碰文件系统、不碰 Windows API，可以直接进「只链 qst_utils」的自检档。
// ──────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string>
#include <vector>

namespace qst {
namespace ooxml {

/// 解压一段**原始** deflate 流（不含 zlib 头尾）。
/// `expectedSize` > 0 时作为输出预分配与**结果长度校验**（zip 的 central directory 里
/// 有 uncompSize，一定要用上：它能把「解压器有 bug」和「文件本来就坏」区分开）。
/// 返回 false 时 `err` 给出可读原因，`out` 内容无意义。
bool InflateRaw(const uint8_t* src, size_t srcSize, size_t expectedSize,
    std::vector<uint8_t>& out, std::string& err);

/// 便捷重载：输入按 vector、expectedSize 未知（0）。
bool InflateRaw(const std::vector<uint8_t>& src, std::vector<uint8_t>& out, std::string& err);

}  // namespace ooxml
}  // namespace qst
