// ──────────────────────────────────────────────────────────────────
// base64.h — Base64 编解码唯一实现（header-only）
//
// 背景（架构评估 P1-2）：项目里曾有 3 份各自手写的 Base64：
//   agent_attachment.cpp:29 / ai_action_service.cpp:96,128 / ocr_engine.cpp:482
// 行为一致但必须手工同步。这里收敛为唯一实现，三处改为 using 引用。
//
// 选 header-only 的理由：这三份分别编进 script_core_common（ocr_engine）、
// qst_engine（agent_attachment / ai_action_service），还有 SelfTest 直接重编
// 被测源；放头文件可避免给每个 target 追加源文件。
// ──────────────────────────────────────────────────────────────────
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace qst {
namespace base64 {

/// 标准 Base64 字母表编码（带 '=' 填充），无换行。
inline std::string Encode(const unsigned char* data, size_t len) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    if (!data && len != 0) return out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t n = (static_cast<uint32_t>(data[i]) << 16)
            | ((i + 1 < len) ? static_cast<uint32_t>(data[i + 1]) << 8 : 0)
            | ((i + 2 < len) ? static_cast<uint32_t>(data[i + 2]) : 0);
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.push_back(i + 1 < len ? kTable[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? kTable[n & 63] : '=');
    }
    return out;
}

inline std::string Encode(const std::vector<uint8_t>& data) {
    return Encode(data.empty() ? nullptr : data.data(), data.size());
}

/// 解码；忽略非字母表字符，遇 '=' 截断（与既有三份实现语义一致）。
inline std::vector<uint8_t> Decode(const std::string& data) {
    auto rev = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve((data.size() / 4) * 3);
    int buf = 0;
    int bits = 0;
    for (char c : data) {
        if (c == '=') break;
        const int v = rev(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

}  // namespace base64
}  // namespace qst
