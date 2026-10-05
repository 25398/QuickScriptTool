// inflate.cpp — RFC 1951 解压。实现思路是经典的「canonical Huffman + 逐位下降」：
// 不做查表加速（Office 的 XML part 通常几十 KB，逐位解码完全够用），
// 换取的是**代码短、判据清楚、容易逐格自检**。

#include "ooxml/inflate.h"

#include <cstring>

namespace qst {
namespace ooxml {

namespace {

constexpr int kMaxCodeBits = 15;

/// 长度码 257..285 的基准值与额外位数（RFC 1951 §3.2.5）
const uint16_t kLenBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258,
};
const uint8_t kLenExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};
/// 距离码 0..29
const uint16_t kDistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
};
const uint8_t kDistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};
/// 动态块里「码长码」的传输顺序（RFC 1951 §3.2.7）—— **不是** 0..18 的顺序，写错必炸
const uint8_t kCodeLenOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/// canonical Huffman 表：count[len] = 该长度的码有几个；symbol[] = 按码序排好的符号
struct Huffman {
    short count[kMaxCodeBits + 1] = {};
    short symbol[288] = {};
};

/// 位读取器：DEFLATE 是 **LSB-first**（每个字节的低位先出）
struct BitReader {
    const uint8_t* in = nullptr;
    size_t len = 0;
    size_t pos = 0;
    uint32_t buf = 0;
    int cnt = 0;
    bool eof = false;

    int GetBit() {
        if (cnt == 0) {
            if (pos >= len) { eof = true; return -1; }
            buf = in[pos++];
            cnt = 8;
        }
        const int b = static_cast<int>(buf & 1u);
        buf >>= 1;
        --cnt;
        return b;
    }
    /// 取 `need` 位（≤16），低位在前
    int GetBits(int need) {
        int v = 0;
        for (int i = 0; i < need; ++i) {
            const int b = GetBit();
            if (b < 0) return -1;
            v |= b << i;
        }
        return v;
    }
    /// 丢弃到字节边界（stored 块要求）
    bool AlignToByte() {
        cnt = 0;
        buf = 0;
        return true;
    }
};

/// 由码长表建 canonical 码表。返回 0 = 完整；>0 = 不完整（仍然可用，缺的码当错误）；
/// <0 = **过订阅**（码长表本身坏了）。
int BuildHuffman(Huffman& h, const short* lengths, int n) {
    for (int i = 0; i <= kMaxCodeBits; ++i) h.count[i] = 0;
    for (int i = 0; i < n; ++i) h.count[lengths[i]]++;
    if (h.count[0] == n) return 0;   // 全 0：这张表没有码（合法，比如没有距离码）
    int left = 1;
    for (int len = 1; len <= kMaxCodeBits; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;   // 过订阅
    }
    short offs[kMaxCodeBits + 1] = {};
    for (int len = 1; len < kMaxCodeBits; ++len) offs[len + 1] = offs[len] + h.count[len];
    for (int i = 0; i < n; ++i) {
        if (lengths[i]) h.symbol[offs[lengths[i]]++] = static_cast<short>(i);
    }
    return left;
}

/// 从流里解一个符号（逐位下降）。返回 -1 = 流坏/越界。
int DecodeSymbol(BitReader& br, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxCodeBits; ++len) {
        const int b = br.GetBit();
        if (b < 0) return -1;
        code |= b;
        const int count = h.count[len];
        if (code - count < first) {
            const int sym = index + (code - first);
            if (sym < 0 || sym >= 288) return -1;
            return h.symbol[sym];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/// 建「固定 Huffman」两张表（RFC 1951 §3.2.6）
void BuildFixedTables(Huffman& lit, Huffman& dist) {
    short litLen[288];
    for (int i = 0; i < 144; ++i) litLen[i] = 8;
    for (int i = 144; i < 256; ++i) litLen[i] = 9;
    for (int i = 256; i < 280; ++i) litLen[i] = 7;
    for (int i = 280; i < 288; ++i) litLen[i] = 8;
    BuildHuffman(lit, litLen, 288);
    short distLen[30];
    for (int i = 0; i < 30; ++i) distLen[i] = 5;
    BuildHuffman(dist, distLen, 30);
}

}  // namespace

bool InflateRaw(const uint8_t* src, size_t srcSize, size_t expectedSize,
    std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    err.clear();
    if (src == nullptr || srcSize == 0) {
        err = "deflate 流为空";
        return false;
    }
    if (expectedSize > 0) out.reserve(expectedSize);

    BitReader br;
    br.in = src;
    br.len = srcSize;

    Huffman fixedLit{}, fixedDist{};
    bool fixedReady = false;

    for (;;) {
        const int last = br.GetBits(1);
        if (last < 0) { err = "deflate 流在块头前结束"; return false; }
        const int type = br.GetBits(2);
        if (type < 0) { err = "deflate 流在块类型处结束"; return false; }

        if (type == 0) {
            // ── stored（未压缩）块 ──
            br.AlignToByte();
            if (br.pos + 4 > br.len) { err = "stored 块头越界"; return false; }
            const uint32_t len = static_cast<uint32_t>(br.in[br.pos])
                | (static_cast<uint32_t>(br.in[br.pos + 1]) << 8);
            const uint32_t nlen = static_cast<uint32_t>(br.in[br.pos + 2])
                | (static_cast<uint32_t>(br.in[br.pos + 3]) << 8);
            br.pos += 4;
            if ((len ^ 0xFFFFu) != nlen) { err = "stored 块 LEN/NLEN 校验失败"; return false; }
            if (br.pos + len > br.len) { err = "stored 块数据越界"; return false; }
            out.insert(out.end(), br.in + br.pos, br.in + br.pos + len);
            br.pos += len;
        } else if (type == 1 || type == 2) {
            // ── Huffman 块 ──
            Huffman lit{}, dist{};
            if (type == 1) {
                if (!fixedReady) { BuildFixedTables(fixedLit, fixedDist); fixedReady = true; }
                lit = fixedLit;
                dist = fixedDist;
            } else {
                const int hlit = br.GetBits(5);
                const int hdist = br.GetBits(5);
                const int hclen = br.GetBits(4);
                if (hlit < 0 || hdist < 0 || hclen < 0) { err = "动态块头越界"; return false; }
                const int nlit = hlit + 257;
                const int ndist = hdist + 1;
                const int nclen = hclen + 4;
                if (nlit > 286 || ndist > 30) { err = "动态块码表数量越界"; return false; }

                short clen[19] = {};
                for (int i = 0; i < nclen; ++i) {
                    const int v = br.GetBits(3);
                    if (v < 0) { err = "动态块码长表越界"; return false; }
                    clen[kCodeLenOrder[i]] = static_cast<short>(v);
                }
                Huffman clTable{};
                if (BuildHuffman(clTable, clen, 19) < 0) { err = "码长码表过订阅"; return false; }

                short lengths[288 + 30] = {};
                const int total = nlit + ndist;
                int i = 0;
                while (i < total) {
                    const int sym = DecodeSymbol(br, clTable);
                    if (sym < 0) { err = "解动态块码长时流结束"; return false; }
                    if (sym < 16) {
                        lengths[i++] = static_cast<short>(sym);
                    } else if (sym == 16) {
                        if (i == 0) { err = "码长重复码出现在首位"; return false; }
                        const int rep = br.GetBits(2);
                        if (rep < 0) { err = "码长重复码越界"; return false; }
                        const short prev = lengths[i - 1];
                        for (int k = 0; k < 3 + rep && i < total; ++k) lengths[i++] = prev;
                    } else if (sym == 17) {
                        const int rep = br.GetBits(3);
                        if (rep < 0) { err = "码长重复码越界"; return false; }
                        for (int k = 0; k < 3 + rep && i < total; ++k) lengths[i++] = 0;
                    } else {   // 18
                        const int rep = br.GetBits(7);
                        if (rep < 0) { err = "码长重复码越界"; return false; }
                        for (int k = 0; k < 11 + rep && i < total; ++k) lengths[i++] = 0;
                    }
                }
                if (BuildHuffman(lit, lengths, nlit) < 0) { err = "字面量表过订阅"; return false; }
                if (BuildHuffman(dist, lengths + nlit, ndist) < 0) { err = "距离表过订阅"; return false; }
            }

            // ── 解符号 ──
            for (;;) {
                const int sym = DecodeSymbol(br, lit);
                if (sym < 0) { err = "解字面量时流结束"; return false; }
                if (sym < 256) {
                    out.push_back(static_cast<uint8_t>(sym));
                    continue;
                }
                if (sym == 256) break;   // 块结束
                const int li = sym - 257;
                if (li >= 29) { err = "长度码越界"; return false; }
                const int extra = br.GetBits(kLenExtra[li]);
                if (extra < 0) { err = "长度额外位越界"; return false; }
                const size_t length = static_cast<size_t>(kLenBase[li]) + static_cast<size_t>(extra);

                const int dsym = DecodeSymbol(br, dist);
                if (dsym < 0 || dsym >= 30) { err = "距离码越界"; return false; }
                const int dextra = br.GetBits(kDistExtra[dsym]);
                if (dextra < 0) { err = "距离额外位越界"; return false; }
                const size_t distance = static_cast<size_t>(kDistBase[dsym])
                    + static_cast<size_t>(dextra);
                if (distance == 0 || distance > out.size()) {
                    err = "距离超出已输出内容（流损坏）";
                    return false;
                }
                // 逐字节复制：源区间与目标区间可能**重叠**（这是 LZ77 的核心），
                // 所以不能用 memcpy，必须一字节一字节来。
                const size_t start = out.size() - distance;
                for (size_t k = 0; k < length; ++k) out.push_back(out[start + k]);
            }
        } else {
            err = "deflate 块类型 3 是保留值（流损坏）";
            return false;
        }

        if (last == 1) break;
    }

    if (expectedSize > 0 && out.size() != expectedSize) {
        err = "解压后长度与 zip 头记录的不一致（期望 " + std::to_string(expectedSize)
            + "，实得 " + std::to_string(out.size()) + "）";
        return false;
    }
    return true;
}

bool InflateRaw(const std::vector<uint8_t>& src, std::vector<uint8_t>& out, std::string& err) {
    return InflateRaw(src.data(), src.size(), 0, out, err);
}

}  // namespace ooxml
}  // namespace qst
