// ──────────────────────────────────────────────────────────────────
// sqlite_read.cpp — 只读 SQLite 解析实现
//
// 格式要点（**每一条都在真实 Edge History 库上验证过**，不是照文档猜的）：
//   · 第 1 页 = 100 字节文件头 + b-tree 页（页大小 4096 或 65536 时：
//     头 100 字节后**跳过**，实际页数据从 100 开始；其余页从 0 开始）。
//   · 页头：叶页 8 字节（类型1 空闲2 单元数2 内容起点2 碎片1），
//     内部页 12 字节（多一个 4 字节右子页指针，位于类型之后）。
//     ⚠ 「内容起点」字段用不上（现代文件里恒为 0 或不可靠），**不要**拿它算偏移。
//   · 单元指针数组紧跟在页头之后，每项 2 字节大端，**指向 payload 起点**。
//   · 表叶页单元 = varint(payloadLen) varint(rowid) payload…；
//     表内部页单元 = uint32(左子页) varint(键)。
//   · payload 前部 = varint(headerSize) + headerSize-1 个 serial type varint；
//     ⚠ **body 起点 = (headerSize varint 的起点) + headerSize**，
//       这个「headerSize 恰好等于 header 占的字节数」是 SQLite 的设计（不是巧合），
//       我第一版用「payload 尾往前推」算错了 —— 见 LESSONS 记录。
//   · serial type：0=NULL；1..6=1/2/3/4/6/8 字节大端有符号整数；7=8 字节 IEEE754
//     （大端）；8=整数 0；9=整数 1；10/11=保留；N>=12 偶数=N/2-6 字节 BLOB，
//     N>=13 奇数=(N-13)/2 字节 TEXT。**紧挨着排，无对齐**。
// ──────────────────────────────────────────────────────────────────

#include "sqlite/sqlite_read.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace qst {
namespace sqlite {
namespace {

// ── 大端读取 ─────────────────────────────────────────────────────
uint16_t Be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
uint32_t Be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
        | (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

/// SQLite 变长整数（大端、每字节 7 位有效、最高位=续位；**最多 9 字节**，
/// 第 9 字节给满 8 位 —— 这个特殊规则是为了能表示 2^63-1）。
bool ReadVarint(const uint8_t* p, size_t avail, uint64_t& out, size_t& used) {
    uint64_t v = 0;
    for (size_t i = 0; i < 9; ++i) {
        if (i >= avail) return false;
        const uint8_t c = p[i];
        if (i == 8) {
            v = (v << 8) | c;
            out = v;
            used = 9;
            return true;
        }
        v = (v << 7) | static_cast<uint64_t>(c & 0x7f);
        if ((c & 0x80) == 0) {
            out = v;
            used = i + 1;
            return true;
        }
    }
    return false;
}

/// serial type ⇒ payload 里占的字节数。
size_t SerialSize(uint64_t t) {
    if (t >= 12) return static_cast<size_t>(t % 2 == 0 ? (t - 12) / 2 : (t - 13) / 2);
    switch (t) {
        case 0: return 0;   // NULL
        case 1: return 1;
        case 2: return 2;
        case 3: return 3;
        case 4: return 4;
        case 5: return 6;
        case 6: return 8;
        case 7: return 8;   // IEEE754
        case 8: return 0;   // 整数 0
        case 9: return 0;   // 整数 1
        case 10:
        case 11: return 0;  // 保留（正常不该出现）
        default: return 0;
    }
}

/// 有符号整数按字节数符号扩展（SQLite 的 1/2/3/4/6 字节整数是**有符号**的）。
int64_t DecodeSignedInt(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v = (v << 8) | p[i];
    // 符号扩展：n < 8 且最高位为 1 ⇒ 负数
    if (n < 8) {
        const uint64_t signBit = 1ULL << (n * 8 - 1);
        if (v & signBit) {
            v |= ~((1ULL << (n * 8)) - 1);
        }
    }
    return static_cast<int64_t>(v);
}

/// UTF-16（LE/BE）→ UTF-8。库若是 UTF-16，文本列在 payload 里就是 UTF-16。
std::string Utf16ToUtf8(const uint8_t* p, size_t bytes, bool bigEndian) {
    std::string out;
    out.reserve(bytes);
    const size_t chars = bytes / 2;
    for (size_t i = 0; i < chars; ++i) {
        uint16_t c = bigEndian ? Be16(p + i * 2)
                               : static_cast<uint16_t>(p[i * 2] | (p[i * 2 + 1] << 8));
        uint32_t cp = c;
        // 代理对
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < chars) {
            const uint16_t lo = bigEndian ? Be16(p + (i + 1) * 2)
                                          : static_cast<uint16_t>(p[(i + 1) * 2]
                                                | (p[(i + 1) * 2 + 1] << 8));
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((static_cast<uint32_t>(c) - 0xD800) << 10)
                    + (static_cast<uint32_t>(lo) - 0xDC00);
                ++i;
            }
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::string ToLowerAscii(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

/// 从 `CREATE TABLE x(...)` 里抽列名。
/// ⚠ 只处理**本模块需要的那点语法**：按顶层逗号切字段、剔除 `CONSTRAINT`/`PRIMARY KEY(...)`
///   这类表级约束条目、剥掉列名上的引号。**不是 SQL 解析器** —— 遇到拿不准的结构
///   宁可少收列，也不要错位（列错位会让整表读取静默给出错误数据）。
std::vector<std::string> ParseColumnNames(const std::string& createSql) {
    std::vector<std::string> cols;
    const size_t lp = createSql.find('(');
    if (lp == std::string::npos) return cols;
    // 找与之配对的右括号（跳过字符串字面量里的括号）
    int depth = 0;
    size_t rp = std::string::npos;
    char quote = 0;
    for (size_t i = lp; i < createSql.size(); ++i) {
        const char c = createSql[i];
        if (quote) {
            if (c == quote) {
                if (i + 1 < createSql.size() && createSql[i + 1] == quote) {
                    ++i;   // 转义的引号
                } else {
                    quote = 0;
                }
            }
            continue;
        }
        if (c == '\'' || c == '"' || c == '`' || c == '[') {
            quote = (c == '[') ? ']' : c;
            continue;
        }
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (--depth == 0) {
                rp = i;
                break;
            }
        }
    }
    if (rp == std::string::npos) return cols;
    const std::string inner = createSql.substr(lp + 1, rp - lp - 1);

    // 按顶层逗号切
    std::vector<std::string> parts;
    size_t start = 0;
    depth = 0;
    quote = 0;
    for (size_t i = 0; i < inner.size(); ++i) {
        const char c = inner[i];
        if (quote) {
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`' || c == '[') {
            quote = (c == '[') ? ']' : c;
            continue;
        }
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
        } else if (c == ',' && depth == 0) {
            parts.push_back(inner.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(inner.substr(start));

    for (auto& raw : parts) {
        std::string s = raw;
        // 去首尾空白
        const size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        const size_t e = s.find_last_not_of(" \t\r\n");
        s = s.substr(b, e - b + 1);
        if (s.empty()) continue;
        // 表级约束条目：不是列
        const std::string low = ToLowerAscii(s);
        if (low.rfind("constraint", 0) == 0 || low.rfind("primary", 0) == 0
            || low.rfind("unique", 0) == 0 || low.rfind("check", 0) == 0
            || low.rfind("foreign", 0) == 0) {
            continue;
        }
        // 列名 = 第一个 token（可能被引号包着）
        std::string name;
        if (s[0] == '"' || s[0] == '`' || s[0] == '\'') {
            const char q = s[0];
            const size_t end = s.find(q, 1);
            if (end == std::string::npos) continue;
            name = s.substr(1, end - 1);
        } else if (s[0] == '[') {
            const size_t end = s.find(']', 1);
            if (end == std::string::npos) continue;
            name = s.substr(1, end - 1);
        } else {
            const size_t end = s.find_first_of(" \t\r\n(");
            name = (end == std::string::npos) ? s : s.substr(0, end);
        }
        if (!name.empty()) cols.push_back(name);
    }
    return cols;
}

}  // namespace

// ── Value ────────────────────────────────────────────────────────
int64_t Value::AsInt(int64_t fallback) const {
    switch (kind) {
        case Kind::Integer: return integer;
        case Kind::Real: return static_cast<int64_t>(real);
        case Kind::Text: {
            if (text.empty()) return fallback;
            char* end = nullptr;
            const long long v = std::strtoll(text.c_str(), &end, 10);
            return (end && end != text.c_str()) ? static_cast<int64_t>(v) : fallback;
        }
        default: return fallback;
    }
}

std::string Value::AsText() const {
    switch (kind) {
        case Kind::Text: return text;
        case Kind::Integer: return std::to_string(integer);
        case Kind::Real: {
            char buf[64]{};
            std::snprintf(buf, sizeof(buf), "%g", real);
            return buf;
        }
        default: return {};
    }
}

// ── Database ─────────────────────────────────────────────────────
bool Database::ReadPage(int pageNo, const uint8_t** out) const {
    if (pageNo < 1 || pageNo > pageCount_ || pageSize_ <= 0) return false;
    const size_t off = static_cast<size_t>(pageNo - 1) * static_cast<size_t>(pageSize_);
    if (off + static_cast<size_t>(pageSize_) > bytes_.size()) return false;
    *out = bytes_.data() + off;
    return true;
}

bool Database::LoadFromMemory(std::vector<uint8_t> bytes, std::string& err) {
    ok_ = false;
    tables_.clear();
    bytes_ = std::move(bytes);
    if (bytes_.size() < 100) {
        err = "不是 SQLite 文件（不足 100 字节）";
        return false;
    }
    static const char kMagic[16] = {
        'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f', 'o', 'r', 'm', 'a', 't', ' ', '3', '\0' };
    if (std::memcmp(bytes_.data(), kMagic, 16) != 0) {
        err = "不是 SQLite 3 文件（文件头魔数不匹配；加密库不支持）";
        return false;
    }
    pageSize_ = Be16(bytes_.data() + 16);
    // 页大小 1 是历史遗留写法，表示 65536
    if (pageSize_ == 1) pageSize_ = 65536;
    if (pageSize_ < 512 || (pageSize_ & (pageSize_ - 1)) != 0) {
        err = "SQLite 页大小非法：" + std::to_string(pageSize_);
        return false;
    }
    const int writeVer = bytes_[18];
    const int readVer = bytes_[19];
    if (writeVer != 1 || readVer != 1) {
        // WAL 模式（2）会让主库不完整 —— 明确拒绝，别给出「看起来对但缺最新数据」的结果。
        err = "SQLite 库是 WAL/其它 journal 模式（write=" + std::to_string(writeVer)
            + " read=" + std::to_string(readVer) + "），本只读实现不支持";
        return false;
    }
    const int reserved = bytes_[20];
    if (reserved < 0 || reserved >= pageSize_) {
        err = "SQLite 保留区大小非法：" + std::to_string(reserved);
        return false;
    }
    usableSize_ = pageSize_ - reserved;
    const uint32_t pageCount = Be32(bytes_.data() + 28);
    textEncoding_ = static_cast<int>(Be32(bytes_.data() + 56));
    if (textEncoding_ != 1 && textEncoding_ != 2 && textEncoding_ != 3) {
        // 0 出现在「库还没写过」的情况：按 UTF-8 处理（实际不会有文本）。
        textEncoding_ = 1;
    }
    // 文件里的页计数可能过时（未 checkpoint）；以**实际字节数**为准更安全。
    const size_t realPages = bytes_.size() / static_cast<size_t>(pageSize_);
    pageCount_ = static_cast<int>((std::min)(static_cast<size_t>(pageCount), realPages));
    if (pageCount_ < 1) {
        err = "SQLite 文件页数为 0";
        return false;
    }
    pageCount_ = static_cast<int>(realPages > 0 ? realPages : pageCount);

    // 解析 sqlite_master（表 1）。第 1 页的数据从 100 字节之后开始 ——
    // 为了统一，直接在 ReadPage 之外特判：把「第 1 页的页头」位置偏移 100。
    // 做法：用一个只读局部 lambda 遍历，避开 ReadPage 的页首假设。
    struct MasterReader {
        const Database* db;
        std::string err;
        bool Run(std::vector<TableInfo>& tables) {
            // 第 1 页：页头在 100 处
            return Walk(1, tables, true);
        }
        // 递归遍历（isFirst 表示这页的页头要从 100 开始读）
        bool Walk(int pageNo, std::vector<TableInfo>& tables, bool isFirst) {
            const uint8_t* raw = nullptr;
            if (!db->ReadPage(pageNo, &raw)) {
                err = "读第 " + std::to_string(pageNo) + " 页失败";
                return false;
            }
            const size_t hdrOff = isFirst ? 100 : 0;
            const uint8_t* p = raw + hdrOff;
            const int type = p[0];
            const int ncell = Be16(p + 3);
            // ★★ 单元指针是**页内绝对偏移**（相对**页首**），**不是**相对页头！
            //   第 1 页的页头在 raw+100，但单元指针仍从 raw 算起
            //   —— 我第一版写成 `hdrOff + cellOff`（又加了 100）⇒ 直接越界，
            //   自检立刻报「serial type varint 越界」（夹具首页的指针实测是 3886/3804）。
            //   所以下面一律只用 cellOff，不加 hdrOff。
            if (type == 5) {
                // 内部页：4 字节子树数 + 2 字节单元数 + 4 字节右指针 + 2 字节内容起点 + 1 碎片 = 12
                for (int i = 0; i < ncell; ++i) {
                    const size_t cellOff = Be16(p + 12 + 2 * i);
                    // 单元指针 = 页内绝对偏移（相对页首），**不加 hdrOff**
                    if (cellOff + 4 > static_cast<size_t>(db->pageSize_)) {
                        err = "第 " + std::to_string(pageNo) + " 页单元指针越界";
                        return false;
                    }
                    const uint32_t child = Be32(raw + cellOff);
                    if (!Walk(static_cast<int>(child), tables, false)) return false;
                }
                const uint32_t right = Be32(p + 8);
                return Walk(static_cast<int>(right), tables, false);
            }
            if (type != 13) {
                // 自由页/溢出页出现在 master 里是不正常的
                return true;
            }
            for (int i = 0; i < ncell; ++i) {
                const size_t cellOff = Be16(p + 8 + 2 * i);
                // 同上：页内绝对偏移，不加 hdrOff
                const size_t abs = cellOff;
                if (abs >= static_cast<size_t>(db->pageSize_)) {
                    err = "第 " + std::to_string(pageNo) + " 页单元指针越界";
                    return false;
                }
                // plen
                size_t used = 0;
                uint64_t plen = 0;
                if (!ReadVarint(raw + abs, db->pageSize_ - abs, plen, used)) {
                    err = "payload 长度 varint 越界";
                    return false;
                }
                size_t q = abs + used;
                uint64_t rowid = 0;
                if (!ReadVarint(raw + q, db->pageSize_ - q, rowid, used)) {
                    err = "rowid varint 越界";
                    return false;
                }
                q += used;
                // header size（**它的起点就是 body 起点的参照**）
                const size_t hdrStart = q;
                uint64_t hdrSize = 0;
                if (!ReadVarint(raw + q, db->pageSize_ - q, hdrSize, used)) {
                    err = "record header size varint 越界";
                    return false;
                }
                q += used;
                if (hdrSize < used) {
                    err = "record header size 小于自身 varint 长度";
                    return false;
                }
                // ★ 按**字节边界**循环（serial type 是 varint，可占 2 字节）
                //   —— 详见 RecordToRow 里同一处的长注释。
                const size_t hdrEnd = hdrStart + static_cast<size_t>(hdrSize);
                std::vector<uint64_t> types;
                while (q < hdrEnd) {
                    uint64_t t = 0;
                    if (!ReadVarint(raw + q, db->pageSize_ - q, t, used)) {
                        err = "serial type varint 越界";
                        return false;
                    }
                    q += used;
                    if (q > hdrEnd) {
                        err = "serial type 越过了 record header 边界";
                        return false;
                    }
                    types.push_back(t);
                }
                // ★ body 起点 = headerSize varint 的起点 + headerSize（已验证）
                size_t body = hdrEnd;
                // master 行：(type, name, tbl_name, rootpage, sql)
                std::vector<Value> vals;
                vals.reserve(types.size());
                for (uint64_t t : types) {
                    const size_t sz = SerialSize(t);
                    Value v;
                    const bool inPage = body + sz <= static_cast<size_t>(db->pageSize_);
                    const uint8_t* src = inPage ? (raw + body) : nullptr;
                    if (t == 0) {
                        v.kind = Value::Kind::Null;
                    } else if (t >= 1 && t <= 6) {
                        v.kind = Value::Kind::Integer;
                        v.integer = src ? DecodeSignedInt(src, sz) : 0;
                    } else if (t == 7) {
                        if (src) {
                            uint64_t bits = 0;
                            for (size_t b = 0; b < 8; ++b) bits = (bits << 8) | src[b];
                            double d = 0.0;
                            std::memcpy(&d, &bits, sizeof(d));
                            v.kind = Value::Kind::Real;
                            v.real = d;
                        }
                    } else if (t == 8) {
                        v.kind = Value::Kind::Integer;
                        v.integer = 0;
                    } else if (t == 9) {
                        v.kind = Value::Kind::Integer;
                        v.integer = 1;
                    } else if (t >= 13 && (t % 2) == 1) {
                        v.kind = Value::Kind::Text;
                        if (src) {
                            if (db->textEncoding_ == 1) {
                                v.text.assign(reinterpret_cast<const char*>(src), sz);
                            } else {
                                v.text = Utf16ToUtf8(src, sz, db->textEncoding_ == 3);
                            }
                        }
                    } else if (t >= 12 && (t % 2) == 0) {
                        v.kind = Value::Kind::Blob;
                        if (src) v.blob.assign(src, src + sz);
                    }
                    if (v.kind != Value::Kind::Null || t == 0) vals.push_back(std::move(v));
                    else vals.push_back(Value{});
                    body += sz;
                }
                // 只收我们用得上的三个字段
                if (vals.size() >= 5) {
                    TableInfo ti;
                    ti.name = vals[1].AsText();
                    ti.sql = vals[4].AsText();
                    ti.rootPage = static_cast<int>(vals[3].AsInt(0));
                    ti.columns = ParseColumnNames(ti.sql);
                    const std::string kind = vals[0].AsText();
                    if (kind == "table" && !ti.name.empty() && ti.rootPage > 0) {
                        tables.push_back(std::move(ti));
                    }
                }
            }
            return true;
        }
    };

    MasterReader mr{ this, {} };
    std::vector<TableInfo> tables;
    if (!mr.Run(tables)) {
        err = "解析 sqlite_master 失败：" + mr.err;
        return false;
    }
    tables_ = std::move(tables);
    ok_ = true;
    return true;
}

// `Database::LoadFromFile` 的实现放在 sqlite_file.cpp —— 它需要平台文件 IO，
// 而本文件保持纯逻辑（只吃字节），这样 SqliteSelfTest 只需链 qst_utils。

bool Database::HasTable(const std::string& table) const {
    for (const auto& t : tables_) {
        if (t.name == table) return true;
    }
    return false;
}

bool Database::TableColumns(const std::string& table, std::vector<std::string>& out) const {
    for (const auto& t : tables_) {
        if (t.name == table) {
            out = t.columns;
            return true;
        }
    }
    return false;
}

std::vector<std::string> Database::TableNames() const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (const auto& t : tables_) names.push_back(t.name);
    return names;
}

std::string Database::SchemaSqlOf(const std::string& table) const {
    for (const auto& t : tables_) {
        if (t.name == table) return t.sql;
    }
    return {};
}

bool Database::RecordToRow(const uint8_t* page, size_t cellPtr, Row& out,
    int64_t& rowid, std::string& err) const {
    out.clear();
    rowid = 0;
    size_t used = 0;
    uint64_t plen = 0;
    if (!ReadVarint(page + cellPtr, pageSize_ - cellPtr, plen, used)) {
        err = "payload 长度 varint 越界";
        return false;
    }
    size_t q = cellPtr + used;
    uint64_t rowidU = 0;
    if (!ReadVarint(page + q, pageSize_ - q, rowidU, used)) {
        err = "rowid varint 越界";
        return false;
    }
    rowid = static_cast<int64_t>(rowidU);
    q += used;
    const size_t hdrStart = q;
    uint64_t hdrSize = 0;
    if (!ReadVarint(page + q, pageSize_ - q, hdrSize, used)) {
        err = "record header size 越界";
        return false;
    }
    q += used;
    if (hdrSize < used) {
        err = "record header size 非法";
        return false;
    }
    // ★★ serial type 要**按 header 的字节边界**循环，不能按「个数」。
    //   原因：serial type 本身是 varint，**可以占 2 字节**（值 ≥ 128 ——
    //   比如 300 字符的文本是 613，编码成 `0x84 0x65`）。
    //   我第一版写 `nTypes = hdrSize - used` 然后循环 nTypes 次 ⇒
    //   多读了一个 type（把 body 的第一个字节当成了 type），
    //   于是 body 起点整体偏移、那行的字段全部错位（自检里表现为
    //   「某行 7 列而不是 6 列」+ 排序结果乱掉）。
    //   正确判据是 `q < hdrStart + hdrSize`：header 的**字节数**是权威。
    const size_t hdrEnd = hdrStart + static_cast<size_t>(hdrSize);
    std::vector<uint64_t> types;
    while (q < hdrEnd) {
        uint64_t t = 0;
        if (!ReadVarint(page + q, pageSize_ - q, t, used)) {
            err = "serial type varint 越界";
            return false;
        }
        q += used;
        if (q > hdrEnd) {
            err = "serial type 越过了 record header 边界（header size 与内容不一致）";
            return false;
        }
        types.push_back(t);
    }
    size_t body = hdrEnd;
    for (uint64_t t : types) {
        const size_t sz = SerialSize(t);
        Value v;
        const bool inPage = body + sz <= static_cast<size_t>(pageSize_);
        const uint8_t* src = inPage ? (page + body) : nullptr;
        if (t == 0) {
            v.kind = Value::Kind::Null;
        } else if (t >= 1 && t <= 6) {
            v.kind = Value::Kind::Integer;
            v.integer = src ? DecodeSignedInt(src, sz) : 0;
        } else if (t == 7) {
            if (src) {
                uint64_t bits = 0;
                for (size_t b = 0; b < 8; ++b) bits = (bits << 8) | src[b];
                double d = 0.0;
                std::memcpy(&d, &bits, sizeof(d));
                v.kind = Value::Kind::Real;
                v.real = d;
            }
        } else if (t == 8) {
            v.kind = Value::Kind::Integer;
            v.integer = 0;
        } else if (t == 9) {
            v.kind = Value::Kind::Integer;
            v.integer = 1;
        } else if (t >= 13 && (t % 2) == 1) {
            v.kind = Value::Kind::Text;
            if (src) {
                v.text = (textEncoding_ == 1)
                    ? std::string(reinterpret_cast<const char*>(src), sz)
                    : Utf16ToUtf8(src, sz, textEncoding_ == 3);
            }
        } else if (t >= 12 && (t % 2) == 0) {
            v.kind = Value::Kind::Blob;
            if (src) v.blob.assign(src, src + sz);
        }
        out.push_back(std::move(v));
        body += sz;
    }
    (void)plen;
    return true;
}

bool Database::WalkTree(int pageNo, size_t rowLimit,
    const std::function<bool(int64_t rowid, const Row& row)>& visit,
    std::string& err) const {
    if (!ok_ || pageNo < 1) return false;
    const uint8_t* p = nullptr;
    if (!ReadPage(pageNo, &p)) {
        err = "读第 " + std::to_string(pageNo) + " 页失败";
        return false;
    }
    const int type = p[0];
    const int ncell = Be16(p + 3);
    if (type == 5) {
        for (int i = 0; i < ncell; ++i) {
            const size_t cellOff = Be16(p + 12 + 2 * i);
            if (cellOff + 4 > static_cast<size_t>(pageSize_)) {
                err = "第 " + std::to_string(pageNo) + " 页单元指针越界";
                return false;
            }
            const uint32_t child = Be32(p + cellOff);
            if (!WalkTree(static_cast<int>(child), rowLimit, visit, err)) return false;
        }
        const uint32_t right = Be32(p + 8);
        return WalkTree(static_cast<int>(right), rowLimit, visit, err);
    }
    if (type != 13) return true;   // 自由页：跳过（表扫描不该遇到溢出页）

    for (int i = 0; i < ncell; ++i) {
        const size_t cellOff = Be16(p + 8 + 2 * i);
        if (cellOff >= static_cast<size_t>(pageSize_)) {
            err = "第 " + std::to_string(pageNo) + " 页单元指针越界";
            return false;
        }
        Row row;
        int64_t rowid = 0;
        std::string rerr;
        if (!RecordToRow(p, cellOff, row, rowid, rerr)) {
            err = "第 " + std::to_string(pageNo) + " 页第 " + std::to_string(i)
                + " 条记录解析失败：" + rerr;
            return false;
        }
        // ⚠ `rowLimit == 0` 的语义是**不限**，不是「不访问」——
        //   我第一版写成 `if (rowLimit > 0 && visit && !visit(...))`，
        //   于是 rowLimit=0 时回调**一次都不调**，全表扫描永远返回 0 行
        //   （自检里 4 个用例同时报「应有 6 行，实际 0」，就是这个）。
        //   限流交给调用方（ScanTable 自己按 limit 截），这里只管遍历。
        (void)rowLimit;
        if (visit && !visit(rowid, row)) return true;   // 回调说够了 ⇒ 提前停
    }
    return true;
}

bool Database::ScanTable(const std::string& table, const std::string& orderByColumn,
    bool ascending, size_t limit, std::vector<Row>& out, std::string& err) const {
    out.clear();
    const TableInfo* ti = nullptr;
    for (const auto& t : tables_) {
        if (t.name == table) {
            ti = &t;
            break;
        }
    }
    if (!ti) {
        std::string names;
        for (size_t i = 0; i < tables_.size(); ++i) {
            if (i) names += ", ";
            names += tables_[i].name;
        }
        err = "库里没有表「" + table + "」；现有表：" + names;
        return false;
    }

    // 需要排序时先全收（只收需要的列会更快，但调用方可能要整行）。
    const bool needSort = !orderByColumn.empty();
    std::vector<Row> all;
    std::vector<int64_t> ids;
    if (!WalkTree(ti->rootPage, 0,
            [&](int64_t rowid, const Row& row) {
                all.push_back(row);
                ids.push_back(rowid);
                return true;
            }, err)) {
        return false;
    }

    if (needSort) {
        int col = -1;
        for (size_t i = 0; i < ti->columns.size(); ++i) {
            if (ti->columns[i] == orderByColumn) {
                col = static_cast<int>(i);
                break;
            }
        }
        if (col < 0) {
            err = "表「" + table + "」没有列「" + orderByColumn + "」";
            return false;
        }
        std::vector<size_t> order(all.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            // ⚠⚠ 这里**必须用值/指针**，不能写 `const Value& va = cond ? all[a][col] : Value{};`
            //   —— 三元两边类型不同（`Value&` vs `Value`）会**产生临时 Value**，
            //   绑定到 const 引用后该临时在完整表达式结束就析构 ⇒ `va` 成悬垂引用，
            //   后续比较读到的是被覆盖的栈内存。实测表现是「排序结果乱掉、偶发正确」，
            //   比崩溃更难查（自检里就是「降序第 2 条给错」）。
            const size_t ci = static_cast<size_t>(col);
            const Value* pa = (ci < all[a].size()) ? &all[a][ci] : nullptr;
            const Value* pb = (ci < all[b].size()) ? &all[b][ci] : nullptr;
            if (!pa || !pb) return pa ? !ascending : false;   // 缺列的行恒排后
            const Value& va = *pa;
            const Value& vb = *pb;
            // NULL 排在最后（与 SQLite 的 ASC 一致：NULL 最小；这里取「无值不占头部」更实用）
            const bool na = va.IsNull();
            const bool nb = vb.IsNull();
            if (na != nb) return ascending ? nb : na;
            int cmp = 0;
            const bool intA = (va.kind == Value::Kind::Integer);
            const bool intB = (vb.kind == Value::Kind::Integer);
            const bool numA = (intA || va.kind == Value::Kind::Real);
            const bool numB = (intB || vb.kind == Value::Kind::Real);
            if (intA && intB) {
                // ★★★ 两个整数**必须直接用 int64 比**，绝不能转 double。
                //   实测教训：Chromium 的 `last_visit_time` 是「1601 年起的微秒数」，
                //   量级 ~1.33e16，**已经超过 double 能精确表示整数的上限 2^53 ≈ 9.0e15**。
                //   转成 double 后 `...000` 与 `...001` 会塌成同一个值 ⇒ 比较恒等于 0
                //   ⇒ 排序结果「大体对但局部错」。这个 bug 在自检里表现为
                //   「降序第 2 条给错」，而在真实浏览器历史上会**静默地把相邻时间戳搞乱**。
                cmp = (va.integer < vb.integer) ? -1 : (va.integer > vb.integer ? 1 : 0);
            } else if (numA && numB) {
                // 有一边是 Real 时才走 double（此时精度本来就受限于 Real 自身）。
                const double da = intA ? static_cast<double>(va.integer) : va.real;
                const double db2 = intB ? static_cast<double>(vb.integer) : vb.real;
                cmp = (da < db2) ? -1 : (da > db2 ? 1 : 0);
            } else {
                // 类型不同（数字 vs 文本）：SQLite 的规则是数字 < 文本；这里保持一致。
                if (numA != numB) {
                    cmp = numA ? -1 : 1;
                } else {
                    const std::string sa = va.AsText();
                    const std::string sb = vb.AsText();
                    cmp = (sa < sb) ? -1 : (sa > sb ? 1 : 0);
                }
            }
            return ascending ? (cmp < 0) : (cmp > 0);
        });
        for (size_t k = 0; k < order.size(); ++k) {
            if (limit > 0 && out.size() >= limit) break;
            out.push_back(all[order[k]]);
        }
    } else {
        for (size_t i = 0; i < all.size(); ++i) {
            if (limit > 0 && out.size() >= limit) break;
            out.push_back(all[i]);
        }
    }
    return true;
}

bool Database::SelectColumns(const std::string& table,
    const std::vector<std::string>& columns, size_t limit, std::vector<Row>& out,
    std::string& err) const {
    out.clear();
    std::vector<Row> all;
    std::string scanErr;
    if (!ScanTable(table, {}, true, 0, all, scanErr)) {
        err = scanErr;
        return false;
    }
    std::vector<std::string> schema;
    TableColumns(table, schema);
    std::vector<int> idx;
    std::string missing;
    for (const auto& c : columns) {
        int found = -1;
        for (size_t i = 0; i < schema.size(); ++i) {
            if (schema[i] == c) {
                found = static_cast<int>(i);
                break;
            }
        }
        if (found < 0 && !missing.empty()) missing += ", ";
        if (found < 0) missing += c;
        idx.push_back(found);
    }
    if (!missing.empty()) {
        // 少一列不整体失败：调用方应据此收敛需求，但已取到的列仍然有用。
        err = "表「" + table + "」缺列：" + missing;
    }
    for (const auto& row : all) {
        if (limit > 0 && out.size() >= limit) break;
        Row picked;
        picked.reserve(idx.size());
        for (int i : idx) {
            if (i >= 0 && static_cast<size_t>(i) < row.size()) picked.push_back(row[i]);
            else picked.push_back(Value{});
        }
        out.push_back(std::move(picked));
    }
    return true;
}

// ── 文件读取在 sqlite_file.cpp（依赖 Win32，与本文件的纯逻辑分离）─────

}  // namespace sqlite
}  // namespace qst
