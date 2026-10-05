#include "ooxml/xlsx_doc.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>

namespace qst {
namespace ooxml {

namespace {

// ── XML 小工具（**不做通用解析器**：OOXML 的 part 结构规整，外科手术式文本处理
//    反而比「解析成 DOM 再序列化」更安全 —— 后者会丢掉我们不认识的元素）──────────

/// `&` 实体 → 原文
std::string XmlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '&') { out.push_back(s[i++]); continue; }
        const size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { out.push_back(s[i++]); continue; }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out.push_back('&');
        else if (ent == "lt") out.push_back('<');
        else if (ent == "gt") out.push_back('>');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos") out.push_back('\'');
        else if (!ent.empty() && ent[0] == '#') {
            const long code = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                ? std::strtol(ent.c_str() + 2, nullptr, 16)
                : std::strtol(ent.c_str() + 1, nullptr, 10);
            if (code > 0 && code < 0x80) {
                out.push_back(static_cast<char>(code));
            } else if (code >= 0x80 && code <= 0x7FF) {
                out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else if (code >= 0x800 && code <= 0xFFFF) {
                out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else if (code > 0xFFFF) {
                const long v = code - 0x10000;
                const unsigned cp1 = 0xD800 + (v >> 10);
                const unsigned cp2 = 0xDC00 + (v & 0x3FF);
                for (unsigned cp : {cp1, cp2}) {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
            } else {
                out.append(s, i, semi - i + 1);
            }
        } else {
            out.append(s, i, semi - i + 1);
        }
        i = semi + 1;
    }
    return out;
}

/// 原文 → `&` 实体（写 XML 时必须转义，否则一个 `&` 就能把文件写坏）
std::string XmlEncode(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

/// 从 `pos`（指向 `<`）起，找开始标签的 `>` 位置（**跳过引号内的** `>`）
size_t FindTagEnd(const std::string& xml, size_t pos) {
    char quote = 0;
    for (size_t i = pos; i < xml.size(); ++i) {
        const char c = xml[i];
        if (quote != 0) {
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '>') return i;
    }
    return std::string::npos;
}

/// 在开始标签文本里取属性值（`name="v"` 或 `name='v'`）
bool GetAttr(const std::string& startTag, const std::string& name, std::string& out) {
    out.clear();
    const std::string key = name + "=";
    size_t p = 0;
    while ((p = startTag.find(key, p)) != std::string::npos) {
        // 要求前面是空白，避免 `xr` 命中 `r=`
        if (p > 0 && startTag[p - 1] != ' ' && startTag[p - 1] != '\t'
            && startTag[p - 1] != '\n' && startTag[p - 1] != '\r') {
            p += key.size();
            continue;
        }
        const size_t q = p + key.size();
        if (q >= startTag.size()) return false;
        const char quote = startTag[q];
        if (quote != '"' && quote != '\'') { p = q; continue; }
        const size_t end = startTag.find(quote, q + 1);
        if (end == std::string::npos) return false;
        out = startTag.substr(q + 1, end - q - 1);
        return true;
    }
    return false;
}

/// 把开始标签里的某个属性删掉（用于替换 `t="..."` 时避免重复）
std::string RemoveAttr(const std::string& startTag, const std::string& name) {
    const std::string key = name + "=";
    size_t p = 0;
    while ((p = startTag.find(key, p)) != std::string::npos) {
        if (p > 0 && startTag[p - 1] != ' ' && startTag[p - 1] != '\t'
            && startTag[p - 1] != '\n' && startTag[p - 1] != '\r') {
            p += key.size();
            continue;
        }
        const size_t q = p + key.size();
        if (q >= startTag.size()) break;
        const char quote = startTag[q];
        if (quote != '"' && quote != '\'') { p = q; continue; }
        const size_t end = startTag.find(quote, q + 1);
        if (end == std::string::npos) break;
        size_t from = p;
        if (from > 0 && (startTag[from - 1] == ' ' || startTag[from - 1] == '\t')) --from;
        return startTag.substr(0, from) + startTag.substr(end + 1);
    }
    return startTag;
}

/// 取一个元素的完整文本（含标签本身）。`pos` 指向 `<`。
/// 处理自闭合 `<c .../>` 与成对 `<c ...>...</c>`（`<c>` 内部不会再有 `<c>`）。
bool ElementExtent(const std::string& xml, size_t pos, size_t& endExclusive) {
    const size_t tagEnd = FindTagEnd(xml, pos);
    if (tagEnd == std::string::npos) return false;
    if (tagEnd > pos && xml[tagEnd - 1] == '/') { endExclusive = tagEnd + 1; return true; }
    // 取标签名
    size_t nameEnd = pos + 1;
    while (nameEnd < xml.size() && xml[nameEnd] != ' ' && xml[nameEnd] != '\t'
        && xml[nameEnd] != '\n' && xml[nameEnd] != '\r' && xml[nameEnd] != '>'
        && xml[nameEnd] != '/') {
        ++nameEnd;
    }
    const std::string name = xml.substr(pos + 1, nameEnd - pos - 1);
    if (name.empty()) return false;
    const std::string closeTag = "</" + name + ">";
    const size_t close = xml.find(closeTag, tagEnd);
    if (close == std::string::npos) return false;
    endExclusive = close + closeTag.size();
    return true;
}

/// 拼接一个元素里所有 `<t>…</t>`（sharedStrings 的富文本 run、inlineStr 都要）
std::string CollectTextRuns(const std::string& xml, size_t from, size_t to) {
    std::string out;
    size_t p = from;
    while (p < to) {
        const size_t t = xml.find("<t", p);
        if (t == std::string::npos || t >= to) break;
        const size_t tagEnd = FindTagEnd(xml, t);
        if (tagEnd == std::string::npos || tagEnd >= to) break;
        if (tagEnd > t && xml[tagEnd - 1] == '/') { p = tagEnd + 1; continue; }   // <t/>
        const size_t close = xml.find("</t>", tagEnd);
        if (close == std::string::npos || close > to) break;
        out += XmlDecode(xml.substr(tagEnd + 1, close - tagEnd - 1));
        p = close + 4;
    }
    return out;
}

/// 取元素内 `<v>…</v>` 的原文（已解码）
bool ExtractV(const std::string& xml, size_t from, size_t to, std::string& out) {
    const size_t v = xml.find("<v>", from);
    if (v == std::string::npos || v >= to) return false;
    const size_t close = xml.find("</v>", v);
    if (close == std::string::npos || close > to) return false;
    out = XmlDecode(xml.substr(v + 3, close - v - 3));
    return true;
}

/// 取元素内 `<f …>…</f>` 的原文（已解码）。自闭合 `<f/>` 视为空公式。
bool ExtractF(const std::string& xml, size_t from, size_t to, std::string& out) {
    out.clear();
    const size_t f = xml.find("<f", from);
    if (f == std::string::npos || f >= to) return false;
    const size_t tagEnd = FindTagEnd(xml, f);
    if (tagEnd == std::string::npos || tagEnd > to) return false;
    if (tagEnd > f && xml[tagEnd - 1] == '/') return true;   // <f/>：有公式但空
    const size_t close = xml.find("</f>", tagEnd);
    if (close == std::string::npos || close > to) return false;
    out = XmlDecode(xml.substr(tagEnd + 1, close - tagEnd - 1));
    return true;
}

/// 找 `<sheetData …>` 的范围（innerBegin/innerEnd 指向内容区间）。
/// ⚠ `selfClosing=true` 表示原标签是 `<sheetData/>` —— 此时 innerBegin==innerEnd 且
///   **往里插东西会插到元素外面**（Excel 直接忽略）。写路径必须先把它展开成成对标签。
bool FindSheetData(const std::string& xml, size_t& innerBegin, size_t& innerEnd,
    bool* selfClosing = nullptr) {
    if (selfClosing) *selfClosing = false;
    const size_t sd = xml.find("<sheetData");
    if (sd == std::string::npos) return false;
    const size_t tagEnd = FindTagEnd(xml, sd);
    if (tagEnd == std::string::npos) return false;
    if (tagEnd > sd && xml[tagEnd - 1] == '/') {
        innerBegin = innerEnd = tagEnd + 1;
        if (selfClosing) *selfClosing = true;
        return true;
    }
    const size_t close = xml.find("</sheetData>", tagEnd);
    if (close == std::string::npos) return false;
    innerBegin = tagEnd + 1;
    innerEnd = close;
    return true;
}

struct RowSpan {
    size_t begin = 0;      ///< `<row` 的 `<`
    size_t end = 0;        ///< 元素结束（`</row>` 之后）
    int rowIndex = 0;      ///< 1 基
};

/// 枚举 sheetData 内的 `<row>`（缺 `r` 属性时按「上一个 +1」推断 —— OOXML 允许省略）
std::vector<RowSpan> EnumRows(const std::string& xml, size_t from, size_t to) {
    std::vector<RowSpan> rows;
    int prev = 0;
    size_t p = from;
    while (p < to) {
        const size_t r = xml.find("<row", p);
        if (r == std::string::npos || r >= to) break;
        const size_t tagEnd = FindTagEnd(xml, r);
        if (tagEnd == std::string::npos || tagEnd > to) break;
        RowSpan rs;
        rs.begin = r;
        if (!ElementExtent(xml, r, rs.end)) break;
        std::string rv;
        if (GetAttr(xml.substr(r, tagEnd - r + 1), "r", rv)) {
            rs.rowIndex = std::atoi(rv.c_str());
        } else {
            rs.rowIndex = prev + 1;
        }
        prev = rs.rowIndex;
        rows.push_back(rs);
        p = rs.end;
    }
    return rows;
}

struct CellSpan {
    size_t begin = 0;
    size_t end = 0;
    int col = -1;
    std::string startTag;   ///< 含 `<c …>`（含 `>`）
};

/// 枚举一个 row 内的 `<c>`（缺 `r` 时按「上一个 +1」推断）
std::vector<CellSpan> EnumCells(const std::string& xml, size_t from, size_t to) {
    std::vector<CellSpan> cells;
    int prev = -1;
    size_t p = from;
    while (p < to) {
        const size_t c = xml.find("<c", p);
        if (c == std::string::npos || c >= to) break;
        const size_t tagEnd = FindTagEnd(xml, c);
        if (tagEnd == std::string::npos || tagEnd > to) break;
        CellSpan cs;
        cs.begin = c;
        if (!ElementExtent(xml, c, cs.end)) break;
        cs.startTag = xml.substr(c, tagEnd - c + 1);
        std::string ref;
        if (GetAttr(cs.startTag, "r", ref)) {
            int col = 0, row = 0;
            if (ParseCellRef(ref, col, row)) cs.col = col;
        }
        if (cs.col < 0) cs.col = prev + 1;
        prev = cs.col;
        cells.push_back(cs);
        p = cs.end;
    }
    return cells;
}

std::string NumToText(double v) {
    // 整数写成整数（避免 "1" 变 "1.000000"），其余用 %.15g 保证往返精度
    const double rounded = (v < 0) ? -static_cast<double>(static_cast<long long>(-v + 0.5))
                                   : static_cast<double>(static_cast<long long>(v + 0.5));
    if (rounded == v && v > -1e15 && v < 1e15) {
        return std::to_string(static_cast<long long>(v));
    }
    char buf[40] = {};
    std::snprintf(buf, sizeof(buf), "%.15g", v);
    return buf;
}

// ── 日期 / 时间 ───────────────────────────────────────────────────────────────
// Excel 把日期存成**序列号**（1900 系统下 2024-01-01 = 45292）。直读只拿到数字
// ⇒ 对人和模型都是垃圾。这里把它还原成 ISO 串。
//
// 用 Howard Hinnant 的 days_from_civil / civil_from_days（公历与「1970-01-01 起算天数」
// 的互转，含闰年规则，公认正确且短）。

long long DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= (m <= 2) ? 1 : 0;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097LL + static_cast<long long>(doe) - 719468LL;
}

void CivilFromDays(long long z, int& y, unsigned& m, unsigned& d) {
    z += 719468LL;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    d = doy - (153u * mp + 2u) / 5u + 1u;
    m = mp + (mp < 10u ? 3u : -9u);
    y += (m <= 2) ? 1 : 0;
}

/// Excel 序列号 → 公历日期（**含 1900 闰年 bug**：Excel 认为 1900-02-29 存在 = 序列 60）
void SerialToCivil(double serial, bool date1904, int& y, unsigned& m, unsigned& d) {
    const long long days = static_cast<long long>(std::floor(serial));
    if (date1904) {
        // 1904 系统：序列 0 = 1904-01-01
        CivilFromDays(DaysFromCivil(1904, 1, 1) + days, y, m, d);
        return;
    }
    if (days >= 61) {
        // 1899-12-30 起算（这样序列 61 = 1900-03-01，与 Excel 一致）
        CivilFromDays(DaysFromCivil(1899, 12, 30) + days, y, m, d);
    } else if (days == 60) {
        y = 1900; m = 2; d = 29;   // 那个**不存在**的日期，Excel 显示成什么就还原成什么
    } else {
        CivilFromDays(DaysFromCivil(1899, 12, 31) + days, y, m, d);
    }
}

enum class FmtKind { General, Date, Time, DateTime };

FmtKind BuiltinFmt(int id) {
    switch (id) {
    case 14: case 15: case 16: case 17: return FmtKind::Date;
    case 18: case 19: case 20: case 21: return FmtKind::Time;
    case 22: return FmtKind::DateTime;
    case 45: case 46: case 47: return FmtKind::Time;
    default: break;
    }
    // CJK 区域的日期格式（Excel 内置表里 27~36 / 50~58 是本地化日期）
    if ((id >= 27 && id <= 36) || (id >= 50 && id <= 58)) return FmtKind::Date;
    return FmtKind::General;
}

/// 自定义格式码 → 种类。**只看 token，不做完整格式引擎**（我们只需要「是不是日期」）。
/// ⚠ `m` 是歧义的（月份 vs 分钟）：紧挨 `h` 或 `s` 才算分钟，否则算月份。
FmtKind ClassifyCustomFormat(const std::string& code) {
    bool hasDate = false, hasTime = false;
    std::string s;
    for (size_t i = 0; i < code.size(); ++i) {
        const char c = code[i];
        if (c == '"') {                       // 引号里的字面量不算 token
            while (i + 1 < code.size() && code[++i] != '"') {}
            continue;
        }
        if (c == '\\') { ++i; continue; }     // 转义下一个字符
        if (c == '[') {                       // [Red] / [$-409] / [h] / [mm]
            const size_t e = code.find(']', i);
            if (e == std::string::npos) break;
            const std::string inner = code.substr(i + 1, e - i - 1);
            if (inner.find('h') != std::string::npos || inner.find('s') != std::string::npos) {
                hasTime = true;
            }
            i = e;
            continue;
        }
        if (c == '_' || c == '*') { ++i; continue; }   // 填充/跳过字符
        s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == 'y' || c == 'd') { hasDate = true; continue; }
        if (c == 'h' || c == 's') { hasTime = true; continue; }
        if (c == 'm') {
            const char prev = (i > 0) ? s[i - 1] : '\0';
            const char next = (i + 1 < s.size()) ? s[i + 1] : '\0';
            if (prev == 'h' || next == 's') hasTime = true;
            else hasDate = true;
        }
    }
    if (hasDate && hasTime) return FmtKind::DateTime;
    if (hasDate) return FmtKind::Date;
    if (hasTime) return FmtKind::Time;
    return FmtKind::General;
}

/// 序列号 → ISO 串（按格式种类决定给日期、时间、还是两者）
std::string FormatSerial(double serial, bool date1904, FmtKind kind) {
    char buf[40] = {};
    std::string out;
    if (kind == FmtKind::Date || kind == FmtKind::DateTime) {
        int y = 0; unsigned m = 0, d = 0;
        SerialToCivil(serial, date1904, y, m, d);
        std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, m, d);
        out = buf;
    }
    if (kind == FmtKind::Time || kind == FmtKind::DateTime) {
        double frac = serial - std::floor(serial);
        if (frac < 0) frac = 0;
        long long secs = static_cast<long long>(frac * 86400.0 + 0.5);
        if (secs > 86399) secs = 86399;   // 四舍五入到 24:00:00 的边界，夹回 23:59:59
        const long long hh = secs / 3600, mm = (secs % 3600) / 60, ss = secs % 60;
        std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld", hh, mm, ss);
        if (!out.empty()) out += " ";
        out += buf;
    }
    return out;
}

}  // namespace

int ColumnNameToIndex(const std::string& name) {
    int v = 0;
    for (char c : name) {
        if (c >= 'A' && c <= 'Z') v = v * 26 + (c - 'A' + 1);
        else if (c >= 'a' && c <= 'z') v = v * 26 + (c - 'a' + 1);
        else return -1;
    }
    return v - 1;
}

std::string ColumnIndexToName(int index) {
    if (index < 0) return {};
    std::string s;
    int v = index + 1;
    while (v > 0) {
        const int rem = (v - 1) % 26;
        s.insert(s.begin(), static_cast<char>('A' + rem));
        v = (v - 1) / 26;
    }
    return s;
}

bool ParseCellRef(const std::string& ref, int& col, int& row) {
    size_t i = 0;
    while (i < ref.size() && (ref[i] == '$')) ++i;
    const size_t lettersBegin = i;
    while (i < ref.size() && ((ref[i] >= 'A' && ref[i] <= 'Z') || (ref[i] >= 'a' && ref[i] <= 'z'))) ++i;
    if (i == lettersBegin) return false;
    const int c = ColumnNameToIndex(ref.substr(lettersBegin, i - lettersBegin));
    while (i < ref.size() && ref[i] == '$') ++i;
    const size_t digitsBegin = i;
    while (i < ref.size() && ref[i] >= '0' && ref[i] <= '9') ++i;
    if (i == digitsBegin || i != ref.size()) return false;
    const int r = std::atoi(ref.c_str() + digitsBegin);
    if (c < 0 || r < 1) return false;
    col = c;
    row = r;
    return true;
}

bool XlsxDoc::LoadFromMemory(const uint8_t* data, size_t size, std::string& err) {
    loaded_ = false;
    dirty_ = false;
    sheets_.clear();
    sharedStrings_.clear();
    sharedLoaded_ = false;
    sheetXml_.clear();
    sheetXmlLoaded_.clear();
    bytes_.assign(data, data + size);

    if (!zip_.LoadFromMemory(data, size, err)) return false;

    std::string wb;
    if (!zip_.GetText("xl/workbook.xml", wb, err)) {
        err = "不是 xlsx（缺 xl/workbook.xml）：" + err;
        return false;
    }
    std::string rels;
    zip_.GetText("xl/_rels/workbook.xml.rels", rels, err);   // 没有也能靠约定回退

    // <sheets><sheet name="…" sheetId="…" r:id="rIdN"/></sheets>
    const size_t sheetsTag = wb.find("<sheets");
    if (sheetsTag == std::string::npos) {
        err = "xl/workbook.xml 里没有 <sheets>";
        return false;
    }
    const size_t sheetsEnd = wb.find("</sheets>", sheetsTag);
    size_t p = sheetsTag;
    int ordinal = 0;
    while (p < sheetsEnd) {
        const size_t s = wb.find("<sheet ", p);
        if (s == std::string::npos || s >= sheetsEnd) break;
        const size_t tagEnd = FindTagEnd(wb, s);
        if (tagEnd == std::string::npos) break;
        const std::string tag = wb.substr(s, tagEnd - s + 1);
        SheetInfo info;
        std::string name, sheetId, rid;
        GetAttr(tag, "name", name);
        GetAttr(tag, "sheetId", sheetId);
        GetAttr(tag, "r:id", rid);
        info.name = XmlDecode(name);
        info.sheetId = sheetId.empty() ? 0 : std::atoi(sheetId.c_str());
        ++ordinal;

        // 用 rels 把 rId 映射到 part 路径；映射不到就按约定回退
        info.partPath.clear();
        if (!rid.empty() && !rels.empty()) {
            const size_t rel = rels.find("Id=\"" + rid + "\"");
            if (rel != std::string::npos) {
                const size_t relEnd = FindTagEnd(rels, rels.rfind('<', rel));
                const std::string relTag = (relEnd == std::string::npos)
                    ? std::string() : rels.substr(rels.rfind('<', rel), relEnd - rels.rfind('<', rel) + 1);
                std::string target;
                GetAttr(relTag, "Target", target);
                if (!target.empty()) {
                    if (target[0] == '/') info.partPath = target.substr(1);
                    else info.partPath = "xl/" + target;
                }
            }
        }
        if (info.partPath.empty()) {
            info.partPath = "xl/worksheets/sheet" + std::to_string(ordinal) + ".xml";
        }
        sheets_.push_back(info);
        p = tagEnd;
    }
    if (sheets_.empty()) {
        err = "工作簿里没有工作表";
        return false;
    }
    sheetXml_.assign(sheets_.size(), std::string());
    sheetXmlLoaded_.assign(sheets_.size(), false);
    loaded_ = true;
    return true;
}

bool XlsxDoc::LoadFromFile(const std::wstring& path, std::string& err) {
    std::vector<uint8_t> b;
    if (!ReadZipFileBytes(path, b)) {
        err = "读文件失败";
        return false;
    }
    return LoadFromMemory(b.data(), b.size(), err);
}

const SheetInfo* XlsxDoc::FindSheet(const std::string& name) const {
    for (const auto& s : sheets_) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

bool XlsxDoc::EnsureSheetLoaded(size_t sheetIndex, std::string& err) const {
    if (sheetIndex >= sheets_.size()) {
        err = "工作表下标越界";
        return false;
    }
    if (sheetXmlLoaded_[sheetIndex]) return true;
    if (!zip_.GetText(sheets_[sheetIndex].partPath, sheetXml_[sheetIndex], err)) {
        err = "读不到工作表 XML（" + sheets_[sheetIndex].partPath + "）：" + err;
        return false;
    }
    sheetXmlLoaded_[sheetIndex] = true;
    return true;
}

/// 懒解析数字格式表。**解析失败不算错** —— 拿不到格式就当「没有特殊格式」，
/// 最坏结果是日期显示成序列号（与改动前一样），不会让整个读取失败。
void XlsxDoc::EnsureStyleTableLoaded() const {
    if (stylesLoaded_) return;
    stylesLoaded_ = true;

    std::string ignored;
    std::string styles;
    if (zip_.GetText("xl/styles.xml", styles, ignored)) {
        // 自定义格式：<numFmts><numFmt numFmtId="164" formatCode="yyyy-mm-dd"/>…</numFmts>
        const size_t nf = styles.find("<numFmts");
        if (nf != std::string::npos) {
            const size_t nfEnd = styles.find("</numFmts>", nf);
            size_t p = nf;
            while (p < nfEnd) {
                const size_t e = styles.find("<numFmt ", p);
                if (e == std::string::npos || e >= nfEnd) break;
                const size_t tagEnd = FindTagEnd(styles, e);
                if (tagEnd == std::string::npos) break;
                const std::string tag = styles.substr(e, tagEnd - e + 1);
                std::string id, code;
                if (GetAttr(tag, "numFmtId", id) && GetAttr(tag, "formatCode", code)) {
                    customNumFmts_.emplace_back(std::atoi(id.c_str()), XmlDecode(code));
                }
                p = tagEnd;
            }
        }
        // cellXfs 里 xf 的**出现顺序**就是单元格 `s` 属性的下标
        const size_t xfs = styles.find("<cellXfs");
        if (xfs != std::string::npos) {
            const size_t xfsEnd = styles.find("</cellXfs>", xfs);
            size_t p = xfs;
            while (p < xfsEnd) {
                const size_t e = styles.find("<xf ", p);
                if (e == std::string::npos || e >= xfsEnd) break;
                const size_t tagEnd = FindTagEnd(styles, e);
                if (tagEnd == std::string::npos) break;
                const std::string tag = styles.substr(e, tagEnd - e + 1);
                std::string id;
                cellXfNumFmtId_.push_back(GetAttr(tag, "numFmtId", id) ? std::atoi(id.c_str()) : 0);
                p = tagEnd;
            }
        }
    }
    // 1904 日期系统（Mac 老习惯）：<workbookPr date1904="1"/>
    std::string wb;
    if (zip_.GetText("xl/workbook.xml", wb, ignored)) {
        const size_t pr = wb.find("<workbookPr");
        if (pr != std::string::npos) {
            const size_t tagEnd = FindTagEnd(wb, pr);
            if (tagEnd != std::string::npos) {
                std::string v;
                GetAttr(wb.substr(pr, tagEnd - pr + 1), "date1904", v);
                date1904_ = (v == "1" || v == "true");
            }
        }
    }
}

std::string XlsxDoc::DisplayForStyle(int styleIndex, double value) const {
    EnsureStyleTableLoaded();
    if (styleIndex < 0 || styleIndex >= static_cast<int>(cellXfNumFmtId_.size())) return {};
    const int id = cellXfNumFmtId_[static_cast<size_t>(styleIndex)];
    FmtKind kind = BuiltinFmt(id);
    if (kind == FmtKind::General) {
        for (const auto& kv : customNumFmts_) {
            if (kv.first == id) { kind = ClassifyCustomFormat(kv.second); break; }
        }
    }
    if (kind == FmtKind::General) return {};   // 不是日期/时间 ⇒ 给原始数值（那才是真值）
    return FormatSerial(value, date1904_, kind);
}

bool XlsxDoc::GetCell(const std::string& sheetName, const std::string& ref,
    CellValue& out, std::string& err) const {
    out = CellValue{};
    err.clear();
    int col = 0, row = 0;
    if (!ParseCellRef(ref, col, row)) {
        err = "单元格引用不合法：" + ref + "（应为 A1 这种）";
        return false;
    }
    const SheetInfo* info = FindSheet(sheetName);
    if (info == nullptr) {
        err = "没有这张工作表：" + sheetName;
        return false;
    }
    const size_t idx = static_cast<size_t>(info - sheets_.data());
    if (!EnsureSheetLoaded(idx, err)) return false;

    const std::string& xml = sheetXml_[idx];
    size_t sdBegin = 0, sdEnd = 0;
    if (!FindSheetData(xml, sdBegin, sdEnd)) return true;   // 没有 sheetData = 全空
    for (const auto& rs : EnumRows(xml, sdBegin, sdEnd)) {
        if (rs.rowIndex != row) continue;
        for (const auto& cs : EnumCells(xml, rs.begin, rs.end)) {
            if (cs.col != col) continue;
            std::string t;
            GetAttr(cs.startTag, "t", t);
            // 样式下标（`s`）：决定数字要不要按日期/时间格式化
            std::string sAttr;
            const int styleIdx = GetAttr(cs.startTag, "s", sAttr) ? std::atoi(sAttr.c_str()) : -1;
            std::string v;
            const bool hasV = ExtractV(xml, cs.begin, cs.end, v);
            std::string f;
            const bool hasF = ExtractF(xml, cs.begin, cs.end, f);
            if (hasF) {
                out.kind = CellKind::Formula;
                out.formula = f;
                out.text = f;
                if (hasV) {
                    out.number = std::atof(v.c_str());
                    out.hasNumber = true;
                    out.display = DisplayForStyle(styleIdx, out.number);
                }
                return true;
            }
            if (t == "s") {
                if (!sharedLoaded_) {
                    std::string ss;
                    std::string ignored;
                    if (zip_.GetText("xl/sharedStrings.xml", ss, ignored)) {
                        size_t q = 0;
                        while (q < ss.size()) {
                            const size_t si = ss.find("<si", q);
                            if (si == std::string::npos) break;
                            size_t siEnd = 0;
                            if (!ElementExtent(ss, si, siEnd)) break;
                            const size_t siTagEnd = FindTagEnd(ss, si);
                            std::string entry;
                            if (siTagEnd != std::string::npos && ss[siTagEnd - 1] != '/') {
                                entry = CollectTextRuns(ss, siTagEnd + 1, siEnd);
                            }
                            sharedStrings_.push_back(entry);
                            q = siEnd;
                        }
                    }
                    sharedLoaded_ = true;
                }
                const long si = std::atol(v.c_str());
                if (si >= 0 && si < static_cast<long>(sharedStrings_.size())) {
                    out.kind = CellKind::Text;
                    out.text = sharedStrings_[static_cast<size_t>(si)];
                } else {
                    out.kind = CellKind::Empty;   // 悬空索引：当空，别编内容
                }
                return true;
            }
            if (t == "inlineStr") {
                out.kind = CellKind::Text;
                out.text = CollectTextRuns(xml, cs.begin, cs.end);
                return true;
            }
            if (t == "str") {   // 公式的字符串结果（无 <f> 的独立 str 少见）
                out.kind = CellKind::Text;
                out.text = v;
                return true;
            }
            if (t == "b") {
                out.kind = CellKind::Bool;
                out.boolean = (v == "1" || v == "true");
                out.number = out.boolean ? 1 : 0;
                out.hasNumber = true;
                return true;
            }
            if (t == "e") {
                out.kind = CellKind::Error;
                out.text = v;
                return true;
            }
            if (hasV) {
                out.kind = CellKind::Number;
                out.number = std::atof(v.c_str());
                out.hasNumber = true;
                out.display = DisplayForStyle(styleIdx, out.number);
                return true;
            }
            return true;   // 有 <c> 但没值 = 空
        }
        return true;   // 这一行里没有该列 = 空
    }
    return true;   // 没有这一行 = 空
}

bool XlsxDoc::SheetBounds(const std::string& sheetName, int& maxRow, int& maxCol,
    std::string& err) const {
    maxRow = -1;
    maxCol = -1;
    err.clear();
    const SheetInfo* info = FindSheet(sheetName);
    if (info == nullptr) {
        err = "没有这张工作表：" + sheetName;
        return false;
    }
    const size_t idx = static_cast<size_t>(info - sheets_.data());
    if (!EnsureSheetLoaded(idx, err)) return false;
    size_t sdBegin = 0, sdEnd = 0;
    const std::string& xml = sheetXml_[idx];
    if (!FindSheetData(xml, sdBegin, sdEnd)) return true;
    for (const auto& rs : EnumRows(xml, sdBegin, sdEnd)) {
        for (const auto& cs : EnumCells(xml, rs.begin, rs.end)) {
            if (cs.col > maxCol) maxCol = cs.col;
            if (rs.rowIndex > maxRow) maxRow = rs.rowIndex;
        }
    }
    if (maxRow > 0) --maxRow;   // 转 0 基
    return true;
}

bool XlsxDoc::DumpSheetAsTsv(const std::string& sheetName, int maxRows, int maxCols,
    std::string& out, std::string& err) const {
    out.clear();
    err.clear();
    int maxRow = 0, maxCol = 0;
    if (!SheetBounds(sheetName, maxRow, maxCol, err)) return false;
    if (maxRow < 0) return true;   // 空表
    if (maxRows > 0 && maxRow >= maxRows) maxRow = maxRows - 1;
    if (maxCols > 0 && maxCol >= maxCols) maxCol = maxCols - 1;

    for (int r = 0; r <= maxRow; ++r) {
        std::string line;
        for (int c = 0; c <= maxCol; ++c) {
            if (c > 0) line.push_back('\t');
            CellValue v;
            const std::string ref = ColumnIndexToName(c) + std::to_string(r + 1);
            if (!GetCell(sheetName, ref, v, err)) return false;
            switch (v.kind) {
            case CellKind::Text: line += v.text; break;
            // ★日期/时间给 `display`（ISO 串）；其余给原始数值
            case CellKind::Number:
                line += v.display.empty() ? NumToText(v.number) : v.display;
                break;
            case CellKind::Bool: line += v.boolean ? "TRUE" : "FALSE"; break;
            case CellKind::Formula: line += "=" + v.formula; break;
            case CellKind::Error: line += v.text; break;
            case CellKind::Empty: break;
            }
        }
        // 行尾制表符裁掉（与 read_doc.ps1 的 TrimEnd 一致，免得给模型一堆空列）
        while (!line.empty() && line.back() == '\t') line.pop_back();
        out += line;
        out.push_back('\n');
    }
    return true;
}

bool XlsxDoc::SetCell(const std::string& sheetName, const std::string& ref,
    const CellValue& value, std::string& err) {
    err.clear();
    int col = 0, row = 0;
    if (!ParseCellRef(ref, col, row)) {
        err = "单元格引用不合法：" + ref + "（应为 A1 这种）";
        return false;
    }
    const SheetInfo* info = FindSheet(sheetName);
    if (info == nullptr) {
        err = "没有这张工作表：" + sheetName;
        return false;
    }
    const size_t idx = static_cast<size_t>(info - sheets_.data());
    if (!EnsureSheetLoaded(idx, err)) return false;
    std::string& xml = sheetXml_[idx];

    // 新内容（不含 `<c …>` 外层标签）
    std::string attrsExtra;          // 要加在 `<c>` 上的属性
    std::string inner;               // 标签内容
    switch (value.kind) {
    case CellKind::Empty:
        break;
    case CellKind::Text:
        attrsExtra = " t=\"inlineStr\"";
        // xml:space="preserve"：不加的话 Excel 会吃掉首尾空格
        inner = "<is><t xml:space=\"preserve\">" + XmlEncode(value.text) + "</t></is>";
        break;
    case CellKind::Number:
        inner = "<v>" + NumToText(value.number) + "</v>";
        break;
    case CellKind::Bool:
        attrsExtra = " t=\"b\"";
        inner = std::string("<v>") + (value.boolean ? "1" : "0") + "</v>";
        break;
    case CellKind::Formula: {
        inner = "<f>" + XmlEncode(value.formula) + "</f>";
        if (!value.cachedValue.empty()) inner += "<v>" + XmlEncode(value.cachedValue) + "</v>";
        else if (value.hasNumber) inner += "<v>" + NumToText(value.number) + "</v>";
        break;
    }
    case CellKind::Error:
        attrsExtra = " t=\"e\"";
        inner = "<v>" + XmlEncode(value.text) + "</v>";
        break;
    }

    size_t sdBegin = 0, sdEnd = 0;
    bool sdSelfClosing = false;
    if (!FindSheetData(xml, sdBegin, sdEnd, &sdSelfClosing)) {
        // 极少见：worksheet 没有 <sheetData>。补一个空的最小结构（其余内容不动）。
        const size_t wsEnd = xml.find("</worksheet>");
        if (wsEnd == std::string::npos) {
            err = "worksheet XML 结构异常（没有 </worksheet>）";
            return false;
        }
        xml.insert(wsEnd, "<sheetData></sheetData>");
        if (!FindSheetData(xml, sdBegin, sdEnd, &sdSelfClosing)) {
            err = "补 <sheetData> 失败";
            return false;
        }
    }
    // ⚠ A/B 验证过：把这里改成「不展开」⇒ OoxmlSelfTest 的
    //   `xlsx_roundtrip_after_save`（A1 丢失）与 `xlsx_set_cell_inserts_in_column_order`
    //   （outOfOrderInserts=0）双双转红。
    if (sdSelfClosing) {
        // ★ `<sheetData/>` 必须**先展开**再插：否则新行会落到元素外面，
        //   文件看着「写成功了」，Excel 却当它不存在（空表是常见形态，这条必踩）。
        const size_t tagEnd = FindTagEnd(xml, xml.find("<sheetData"));
        xml.replace(xml.find("<sheetData"), tagEnd - xml.find("<sheetData") + 1,
            "<sheetData></sheetData>");
        if (!FindSheetData(xml, sdBegin, sdEnd, &sdSelfClosing)) {
            err = "展开 <sheetData/> 失败";
            return false;
        }
    }

    const auto rows = EnumRows(xml, sdBegin, sdEnd);
    const RowSpan* target = nullptr;
    for (const auto& rs : rows) {
        if (rs.rowIndex == row) { target = &rs; break; }
    }

    if (target != nullptr) {
        const auto cells = EnumCells(xml, target->begin, target->end);
        const CellSpan* hit = nullptr;
        const CellSpan* firstAfter = nullptr;
        for (const auto& cs : cells) {
            if (cs.col == col) { hit = &cs; break; }
            if (cs.col > col && firstAfter == nullptr) firstAfter = &cs;
        }
        if (hit != nullptr) {
            // ★ 保留原 `<c>` 上的其它属性（尤其 `s` 样式！）——只替换 `t` 与内容。
            std::string startTag = RemoveAttr(hit->startTag, "t");
            if (!attrsExtra.empty()) {
                // 插到 `<c` 之后（属性顺序无关紧要）
                startTag.insert(2, attrsExtra);
            }
            const std::string newCell = startTag + inner
                + (inner.empty() ? std::string() : "</c>");
            xml.replace(hit->begin, hit->end - hit->begin,
                inner.empty() ? (startTag.substr(0, startTag.size() - 1) + "/>") : newCell);
        } else {
            const std::string newCell = "<c r=\"" + ref + "\"" + attrsExtra + ">"
                + inner + "</c>";
            const size_t at = (firstAfter != nullptr) ? firstAfter->begin : (target->end - 6);
            // target->end - 6 指向 `</row>` 的 `<`
            xml.insert(at, newCell);
        }
    } else {
        // 行不存在：按行号升序插入（OOXML 不强制，但 Excel 就是这么写的）
        size_t at = sdEnd;
        for (const auto& rs : rows) {
            if (rs.rowIndex > row) { at = rs.begin; break; }
        }
        const std::string newRow = "<row r=\"" + std::to_string(row) + "\">"
            + "<c r=\"" + ref + "\"" + attrsExtra + ">" + inner + "</c>"
            + "</row>";
        xml.insert(at, newRow);
    }

    if (!zip_.SetData(sheets_[idx].partPath, std::vector<uint8_t>(xml.begin(), xml.end()), err)) {
        err = "写回工作表 part 失败：" + err;
        return false;
    }
    dirty_ = true;
    return true;
}

bool XlsxDoc::SetCellText(const std::string& sheetName, const std::string& ref,
    const std::string& utf8Text, std::string& err) {
    CellValue v;
    v.kind = CellKind::Text;
    v.text = utf8Text;
    return SetCell(sheetName, ref, v, err);
}

bool XlsxDoc::SetCellNumber(const std::string& sheetName, const std::string& ref,
    double value, std::string& err) {
    CellValue v;
    v.kind = CellKind::Number;
    v.number = value;
    v.hasNumber = true;
    return SetCell(sheetName, ref, v, err);
}

bool XlsxDoc::SetCellFormula(const std::string& sheetName, const std::string& ref,
    const std::string& formula, const std::string& cachedValue, std::string& err) {
    CellValue v;
    v.kind = CellKind::Formula;
    v.formula = formula;
    v.text = formula;
    v.cachedValue = cachedValue;
    return SetCell(sheetName, ref, v, err);
}

bool XlsxDoc::SaveToMemory(std::vector<uint8_t>& out, std::string& err) const {
    // 改过的 sheet part 已在 SetCell 时同步进 zip_；这里只需把 zip 序列化。
    // ⚠ 未改动的 part（图表/样式/图片/其它表）由 ZipArchive 原样搬运 —— 字节保留的落点。
    return zip_.SaveToMemory(out, err);
}

bool XlsxDoc::DumpWorkbookAsText(int maxRows, int maxCols, std::string& out,
    std::string& err) const {
    out.clear();
    err.clear();
    if (maxRows <= 0) maxRows = 200;
    if (maxCols <= 0) maxCols = 40;
    for (const auto& sheet : sheets_) {
        out += "## 工作表: " + sheet.name + "\n";
        int maxRow = 0, maxCol = 0;
        if (!SheetBounds(sheet.name, maxRow, maxCol, err)) return false;
        if (maxRow < 0) continue;   // 空表：只留标题行
        const bool truncated = (maxRow + 1) > maxRows;
        int lastRow = maxRow;
        if (lastRow >= maxRows) lastRow = maxRows - 1;
        int lastCol = maxCol;
        if (lastCol >= maxCols) lastCol = maxCols - 1;
        for (int r = 0; r <= lastRow; ++r) {
            std::string line;
            for (int c = 0; c <= lastCol; ++c) {
                if (c > 0) line.push_back('\t');
                CellValue v;
                const std::string ref = ColumnIndexToName(c) + std::to_string(r + 1);
                if (!GetCell(sheet.name, ref, v, err)) return false;
                switch (v.kind) {
                case CellKind::Text: line += v.text; break;
                case CellKind::Number:
                    line += v.display.empty() ? NumToText(v.number) : v.display;
                    break;
                case CellKind::Bool: line += v.boolean ? "TRUE" : "FALSE"; break;
                case CellKind::Formula:
                    // 有缓存值就给值（那是 Excel 里看到的）；没有就明说「这里有个公式」
                    if (v.hasNumber) {
                        line += v.display.empty() ? NumToText(v.number) : v.display;
                    } else {
                        line += "=" + v.formula;
                    }
                    break;
                case CellKind::Error: line += v.text; break;
                case CellKind::Empty: break;
                }
            }
            // 行尾制表符裁掉；整行空就跳过（与脚本一致）
            while (!line.empty() && line.back() == '\t') line.pop_back();
            if (line.empty()) continue;
            out += line;
            out.push_back('\n');
        }
        if (truncated) out += "…（更多行已省略）\n";
    }
    return true;
}

bool XlsxDoc::CreateNew(const std::string& sheetName, XlsxDoc& out, std::string& err) {
    err.clear();
    const std::string name = sheetName.empty() ? std::string("Sheet1") : sheetName;

    // ⚠ 这 6 个 part 是「Excel 愿意打开」的最小集合，别随手删：
    //   [Content_Types].xml 决定各 part 的类型；_rels/.rels 指向 workbook；
    //   workbook.rels 把 rId 映到 sheet 与 styles（**缺 styles 时 Excel 会报错**）。
    const std::string kHead = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
    const std::string kMainNs = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
    const std::string kRelNs = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
    const std::string kPkgRelNs = "http://schemas.openxmlformats.org/package/2006/relationships";

    ZipArchive z;
    auto add = [&](const char* part, const std::string& body) -> bool {
        return z.AddEntry(part, std::vector<uint8_t>(body.begin(), body.end()), err);
    };
    bool ok = true;
    ok = ok && add("[Content_Types].xml",
        kHead + "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
        "</Types>");
    ok = ok && add("_rels/.rels",
        kHead + "<Relationships xmlns=\"" + kPkgRelNs + "\">"
        "<Relationship Id=\"rId1\" Type=\"" + kRelNs + "/officeDocument\" Target=\"xl/workbook.xml\"/>"
        "</Relationships>");
    ok = ok && add("xl/workbook.xml",
        kHead + "<workbook xmlns=\"" + kMainNs + "\" xmlns:r=\"" + kRelNs + "\">"
        "<sheets><sheet name=\"" + XmlEncode(name) + "\" sheetId=\"1\" r:id=\"rId1\"/></sheets>"
        "</workbook>");
    ok = ok && add("xl/_rels/workbook.xml.rels",
        kHead + "<Relationships xmlns=\"" + kPkgRelNs + "\">"
        "<Relationship Id=\"rId1\" Type=\"" + kRelNs + "/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"" + kRelNs + "/styles\" Target=\"styles.xml\"/>"
        "</Relationships>");
    ok = ok && add("xl/styles.xml",
        kHead + "<styleSheet xmlns=\"" + kMainNs + "\">"
        "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
        "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill>"
        "<fill><patternFill patternType=\"gray125\"/></fill></fills>"
        "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
        "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
        "<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/></cellXfs>"
        "</styleSheet>");
    ok = ok && add("xl/worksheets/sheet1.xml",
        kHead + "<worksheet xmlns=\"" + kMainNs + "\">"
        "<sheetData></sheetData></worksheet>");
    if (!ok) {
        err = "新建 xlsx 时拼 part 失败：" + err;
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!z.SaveToMemory(bytes, err)) return false;
    return out.LoadFromMemory(bytes.data(), bytes.size(), err);
}

bool XlsxDoc::SaveToFile(const std::wstring& path, std::string& err) const {
    std::vector<uint8_t> out;
    if (!SaveToMemory(out, err)) return false;
    if (!WriteZipFileBytes(path, out)) {
        err = "写文件失败";
        return false;
    }
    return true;
}

}  // namespace ooxml
}  // namespace qst
