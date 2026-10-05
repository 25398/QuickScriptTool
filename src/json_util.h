// ──────────────────────────────────────────────────────────────────
// json_util.h — JSON 读写唯一实现（header-only，基于 nlohmann）
//
// 背景（架构评估 P1-2）：项目已统一用 nlohmann，但桥接层仍散落手写扫描：
//   webview_bridge_backend.cpp:350  JsonGetStringArray（宽字符 + token 过滤）
//   webview_bridge_backend.cpp:2547 JsonGetStringField / JsonGetStringArray
//   ai_action_service.cpp:156       ExtractJsonArrayFromText（LLM 散文里抠 JSON）
// 前两组已收敛到本文件；ExtractJsonArrayFromText 是「从自由文本里定位 JSON」
// 的独立需求（nlohmann 无法处理带散文的输入），同样上收到这里，保持只有一份。
//
// 解析策略：nlohmann 非抛异常解析（parse(..., false)）。桥接消息由
// ui/bridge.js 的 JSON.stringify 产生，恒为合法 JSON；设置对象同理。
// 解析失败按「键不存在」处理，而不是像旧的逐字符扫描那样在残破 JSON 上
// 猜出半个值——后者才是「脚本存了读不回来」类 Bug 的温床。
//
// ⚠ 但「静默按键不存在处理」本身有代价：2026-09-18 验收发现，调用方一旦构造出
// 非法 JSON（当时是 `substr(首个 '{')` 带上尾随 '}'），所有键都取不到、各字段保持
// 旧值、末尾又把旧值写回盘并报「保存成功」——用户视角是「改设置提示成功但全部
// 回滚」，且外部完全无法区分「键缺失」与「JSON 非法」。
// 因此本文件补了**解析失败诊断**（见下方 Diagnostics）：失败必须可观测。
// 注意：**不要**加「宽容模式」开关——那会让「残破 JSON 静默取到半个值」重新
// 变成默认行为（验收报告 §7.4）。
// ──────────────────────────────────────────────────────────────────
#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "utils.h"  // FromUtf8

namespace qst {
namespace jsonutil {
/// ★★ **容错序列化**：坏 UTF-8 **不抛异常**，替换成 U+FFFD。
///
/// 起因（2026-09-30 真机）：网页通道读回的页面文本里出现了**被截断的多字节字符**
///   （`0xEF` 之类），而 nlohmann 的 `json::parse` **不校验** UTF-8、`dump()` **校验**
///   ⇒ 抛 `type_error.316 invalid UTF-8 byte at index 663` ⇒ 整个请求 500
///   （`HANDLER_EXCEPTION`），宏当场结束。
/// ⇒ **模型/页面给的文本都是不可信输入**，凡是要序列化出去（回执、请求体、落盘）
///   都必须走这个函数，别直接用 `dump()`。
/// ⚠ `indent < 0` 时等价于 `dump()`（紧凑）；`indent >= 0` 时等价于 `dump(indent)`。
inline std::string DumpUtf8Safe(const nlohmann::json& j, int indent = -1) {
    return j.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

/// ★★ **把一段字节流修成合法 UTF-8**（非法序列替换成 U+FFFD），返回修好的串。
///
/// 为什么需要（2026-09-30 连续两次 500 事故）：
///   ① 归一化把模型文本里的坏字节带进动作 JSON ⇒ `dump()` 抛 316；
///   ② 我在工具清单里做**定长截断**，只退了"续字节"、没退"被截断的头字节"
///      ⇒ 留下半个汉字；而 `json` **从 string 构造时就校验** ⇒
///      `type_error.316 invalid UTF-8 byte at index 1000: 0x0A`（换行本身合法，
///      出问题的是它前面那半个字）⇒ 整个请求 500、宏当场结束。
/// ⇒ 纪律：**任何"拼给外部（模型/文件/回执）的字符串"，出站前过一次这个函数**；
///   别指望每一处 `substr`/截断都自己小心（已栽两次，症状都离原因极远）。
/// ⚠ 它只**修**不报：要留痕就统计 `*outFixedBytes`（替换掉多少字节）后自己打日志。
inline std::string SanitizeUtf8(const std::string& in, size_t* outFixedBytes = nullptr,
                                size_t* outFirstBadOffset = nullptr) {
    if (outFixedBytes) *outFixedBytes = 0;
    if (outFirstBadOffset) *outFirstBadOffset = static_cast<size_t>(-1);
    std::string out;
    out.reserve(in.size() + 8);
    size_t i = 0;
    const size_t n = in.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        size_t need = 0;
        if (c < 0x80) need = 1;
        else if ((c & 0xE0) == 0xC0) need = 2;
        else if ((c & 0xF0) == 0xE0) need = 3;
        else if ((c & 0xF8) == 0xF0) need = 4;
        else need = 0;                       // 0x80~0xBF 单独出现 / 0xF8+ ⇒ 非法
        bool ok = (need > 0) && (i + need <= n);
        if (ok && need > 1) {
            for (size_t k = 1; k < need; ++k) {   // 后续必须都是 10xxxxxx
                if ((static_cast<unsigned char>(in[i + k]) & 0xC0) != 0x80) { ok = false; break; }
            }
            if (ok && need == 2 && c < 0xC2) ok = false;    // 过长编码
            if (ok && need == 4 && c > 0xF4) ok = false;    // 超出 U+10FFFF
        }
        if (ok) {
            out.append(in, i, need);
            i += need;
            continue;
        }
        out += "\xEF\xBF\xBD";               // U+FFFD
        if (outFixedBytes) *outFixedBytes += 1;
        if (outFirstBadOffset && *outFirstBadOffset == static_cast<size_t>(-1)) {
            *outFirstBadOffset = i;          // 只记第一处：那才是要找的源头
        }
        ++i;
        // 一个坏区只出一个替换符：紧跟的续字节一起吃掉（否则一串 ）
        while (i < n && (static_cast<unsigned char>(in[i]) & 0xC0) == 0x80) {
            if (outFixedBytes) *outFixedBytes += 1;
            ++i;
        }
    }
    return out;
}


/// ★★ **按字节上限截断，但保证不切开多字节字符**（2026-09-30）。
///
/// 为什么单独做成一个函数：这个坑**同一份写法在仓库里出现过两次**（工具清单截断、
///   CollapseWhitespace 的截断），两次都只退了"续字节"`0x80~0xBF`、**没退被切断的头字节**
///   ⇒ 留下半个汉字 ⇒ 后面的 `json` 构造抛 `type_error.316` ⇒ 请求 500 或线程内
///   `std::terminate`（**静默闪退**，连崩溃日志都不写）。同一事实必须只有一份实现。
/// 判据：先退续字节，再看剩下的头字节后面够不够长（不够就一起去掉），**宁可少一个字**。
inline std::string TruncateUtf8Safe(const std::string& s, size_t maxBytes,
                                    const char* ellipsis = "…") {
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(s[n - 1]) & 0xC0) == 0x80) --n;
    if (n > 0) {
        const unsigned char lead = static_cast<unsigned char>(s[n - 1]);
        size_t need = 1;
        if ((lead & 0xE0) == 0xC0) need = 2;
        else if ((lead & 0xF0) == 0xE0) need = 3;
        else if ((lead & 0xF8) == 0xF0) need = 4;
        if (s.size() - (n - 1) < need) --n;   // 头字节后面不足 ⇒ 连它一起去掉
    }
    return s.substr(0, n) + (ellipsis ? ellipsis : "");
}

// ── 解析失败诊断（架构评估验收 A2）──────────────────────────────────
// TryParse 失败时所有 GetX 都返回 false；没有诊断就无从定位「键取不到」的
// 根因是「JSON 非法」。这里提供一条可观测通道：上下文标签 + 原文 + 偏移。
//
// 实现说明：sink 是**进程级装一次、之后只读**的回调，不是可变业务状态
// （函数内 static 在 inline 函数里保证全进程一份）。之所以不做成逐调用传参，
// 是为了不把 where 标签穿过 ~100 个既有调用点——那属于 #10 桥接契约正式化
// 的范围（届时每个命令都有请求结构体，标签自然带上）。
using ParseFailureSink = void (*)(const char* where, const std::string& text, size_t offset);

inline ParseFailureSink& ParseFailureSinkRef() {
    static ParseFailureSink sink = nullptr;
    return sink;
}

inline size_t& ParseFailureCountRef() {
    static size_t count = 0;
    return count;
}

/// 产品在启动时装一个写日志的实现（如壳的 BootLogLine）；不装则只累计计数。
inline void SetParseFailureSink(ParseFailureSink sink) { ParseFailureSinkRef() = sink; }

/// 累计解析失败次数（自检用来断言「合法输入不产生诊断」）。
inline size_t ParseFailureCount() { return ParseFailureCountRef(); }

inline void ResetParseFailureCount() { ParseFailureCountRef() = 0; }

/// 记一次解析失败。`where` 是调用点标签（如 "saveSettings.payload"）。
/// 超过 240 字节的原文会截断，避免把整条大 JSON 灌进日志。
inline void NoteParseFailure(const char* where, const std::string& text,
    size_t offset = static_cast<size_t>(-1)) {
    ++ParseFailureCountRef();
    if (ParseFailureSink sink = ParseFailureSinkRef()) {
        std::string clipped = text.size() > 240 ? text.substr(0, 240) + "…" : text;
        sink(where ? where : "?", clipped, offset);
    }
}

/// 非抛异常解析；失败时返回 false。
inline bool TryParse(const std::string& text, nlohmann::json& out) {
    out = nlohmann::json::parse(text, nullptr, false);
    return !out.is_discarded();
}

/// 是否为「可严格解析的 JSON 对象」。
/// 供调用方在批量取值前做**一次**前置校验，把「整条 payload 非法」与
/// 「单个键缺失」区分开——前者必须报出来，后者是正常情况。
inline bool IsParseableObject(const std::string& json) {
    nlohmann::json doc;
    return TryParse(json, doc) && doc.is_object();
}

/// 取顶层字符串字段。键缺失 / 类型不是 string / JSON 非法 → false。
inline bool GetStringField(const std::string& json, const char* key, std::string& out) {
    if (!key) return false;
    nlohmann::json doc;
    if (!TryParse(json, doc) || !doc.is_object()) return false;
    const auto it = doc.find(key);
    if (it == doc.end() || !it->is_string()) return false;
    out = it->get<std::string>();
    return true;
}

/// 取顶层数值字段（布尔/字符串/缺失 → false，不抛异常）。
inline bool GetNumber(const std::string& json, const char* key, double& out) {
    if (!key) return false;
    nlohmann::json doc;
    if (!TryParse(json, doc) || !doc.is_object()) return false;
    const auto it = doc.find(key);
    if (it == doc.end() || !it->is_number()) return false;
    out = it->get<double>();
    return true;
}

/// 取顶层整数字段（数值字段按截断转 int，与旧 std::stod 行为一致）。
inline bool GetInt(const std::string& json, const char* key, int& out) {
    double d = 0;
    if (!GetNumber(json, key, d)) return false;
    out = static_cast<int>(d);
    return true;
}

/// 取顶层布尔字段（非布尔 → false，不把 0/1/"true" 当布尔）。
inline bool GetBool(const std::string& json, const char* key, bool& out) {
    if (!key) return false;
    nlohmann::json doc;
    if (!TryParse(json, doc) || !doc.is_object()) return false;
    const auto it = doc.find(key);
    if (it == doc.end() || !it->is_boolean()) return false;
    out = it->get<bool>();
    return true;
}

/// 取顶层字符串数组（非字符串元素静默跳过）。
inline std::vector<std::string> GetStringArray(const std::string& json, const char* key) {    std::vector<std::string> out;
    if (!key) return out;
    nlohmann::json doc;
    if (!TryParse(json, doc) || !doc.is_object()) return out;
    const auto it = doc.find(key);
    if (it == doc.end() || !it->is_array()) return out;
    for (const auto& v : *it) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

/// 编辑器动作类型 token：仅 [A-Za-z0-9_]，长度 1..64。
inline bool IsActionTypeToken(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (const unsigned char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

/// 取「动作类型 token 数组」：过滤非法 token、去重（保序）、上限 80 项。
/// 键缺失 / 不是数组 / JSON 非法 → false（out 保持原值，与旧实现一致）。
inline bool GetActionTokenArray(const std::string& json, const char* key,
    std::vector<std::wstring>& out) {
    if (!key) return false;
    nlohmann::json doc;
    if (!TryParse(json, doc) || !doc.is_object()) return false;
    const auto it = doc.find(key);
    if (it == doc.end() || !it->is_array()) return false;
    out.clear();
    for (const auto& v : *it) {
        if (!v.is_string()) continue;
        const std::string s = v.get<std::string>();
        if (!IsActionTypeToken(s)) continue;
        if (out.size() >= 80) break;
        const std::wstring w = FromUtf8(s);
        if (std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
    }
    return true;
}

/// 取顶层对象字段，并还原成 JSON 文本（供只接受「子对象 JSON」的旧接口使用）。
///
/// 为什么需要它：桥接消息形如 `{"type":"saveSettings","settings":{...}}`，而
/// `ApplySaveSettingsJson` 只吃 settings 子对象。早期实现用
/// `substr(首个 '{')` 一直截到**末尾**，于是 payload 变成 `{...}}`（多一个外层
/// `}`）——非法 JSON。旧的手写字符扫描容忍残破 JSON 所以没暴露；换成严格解析后
/// 所有键都取不到，最终把旧值写回盘并报「保存成功」（2026-09-18 验收的 P0）。
///
/// 本函数用严格解析取**配对的**对象，键顺序、字符串里的 `}`、嵌套对象、
/// 中文与转义全部由 nlohmann 保证，不再依赖括号扫描。
///
/// `where` 非空时，遇到「整条 JSON 非法」或「顶层不是对象」会调用
/// NoteParseFailure 记一条诊断；「键缺失 / 值不是对象」属正常语义，不记。
inline bool GetSubObjectText(const std::string& json, const char* key, std::string& out,
    const char* where = nullptr) {
    if (!key) return false;
    nlohmann::json doc;
    if (!TryParse(json, doc)) {
        if (where) NoteParseFailure(where, json);
        return false;
    }
    if (!doc.is_object()) {
        if (where) NoteParseFailure(where, json);
        return false;
    }
    const auto it = doc.find(key);
    if (it == doc.end() || !it->is_object()) return false;
    // error_handler=replace：dump() 默认是 strict，遇到**非法 UTF-8** 会抛
    // type_error.316。桥接层可能收到 GBK 字节（旧设置文件/第三方写盘），
    // 这里宁可把坏字节替换成 U+FFFD，也不能让「保存设置」抛异常。
    //
    // 注：本函数有回归锁 —— tools/bridge_json_selftest.cpp 的
    // regression_trailing_outer_brace / subobject_* 共 5 条断言会覆盖
    // 「改回 substr(首个 '{') 到末尾」这种写法（A/B 实测会 5 条变红）。
    out = it->dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    return true;
}

/// ── 已解析对象的字段视图（宽键 / 宽值）────────────────────────────
/// 用途：**解析一次、多次取值**。典型场景是 src/script_io.cpp 的
/// `ParseScriptActionBlock`：一个动作块要取 ~50 个字段，旧实现每次取值都调用
/// utils.cpp 的 ExtractString/ExtractNumber/ExtractBool，而它们每次都要重新
/// `FindTopLevelJsonKeyColon` 扫整个块 —— 一个动作 50 次、1.5 万步的录制就是
/// 75 万次全块扫描。改成「整块解析一次 + 查表」既统一到 nlohmann，也更快。
///
/// 语义刻意与旧的 Extract* 对齐（这是替换的前提，不是巧合）：
///   · GetString：键缺失 / 值不是字符串 → fallback（旧实现「值为数字时会取到
///     下一个键名」是 bug，注释里已警告过，这里按正确语义处理）；
///   · GetNumber：键缺失 / 值不是数字 → fallback（旧实现用 std::stod，字符串
///     与 true/false 都会失败回落，行为一致）；
///   · GetBool：布尔值直接返回；**数字非 0 视为 true**（旧实现支持 `1`/`0`，
///     必须保留，否则 `"enabled":1` 的老脚本会静默失效）；其余 → fallback。
class WideObjectView {
public:
    explicit WideObjectView(const std::wstring& jsonText) {
        const std::string utf8 = ToUtf8(jsonText);
        ok_ = TryParse(utf8, owned_) && owned_.is_object();
        if (ok_) return;
        // ── 唯一兜底：修掉**非法转义**后重试 ──────────────────────────
        // 为什么必须有：脚本里的路径若写成单个反斜杠（`"images\a.png"`），
        // JSON 里 `\a` 是非法转义。产品自己写文件时会正确转义（`\\`），但手改过的
        // 脚本、或早期版本写出的文件可能是单反斜杠。
        // 旧的词法扫描容忍这种输入；严格解析会**整块失败 → 该动作被静默丢弃**
        // （2026-09-19 D2 实测：AgentAssistantSelfTest 的 3 个动作掉了 1 个）。
        // 静默丢数据正是本项目最忌讳的失败方式，所以这里做一次精确修复：
        // 只把「反斜杠 + 非合法转义字符」补成 `\\`，其余一律不动。
        // **刻意不做更宽的容错** —— 那会让「残破 JSON 静默取到半个值」变回默认行为。
        std::string fixed;
        fixed.reserve(utf8.size());
        for (size_t i = 0; i < utf8.size(); ++i) {
            const char c = utf8[i];
            if (c == '\\' && i + 1 < utf8.size()) {
                const char n = utf8[i + 1];
                if (n == '"' || n == '\\' || n == '/' || n == 'b' || n == 'f'
                    || n == 'n' || n == 'r' || n == 't' || n == 'u') {
                    fixed.push_back(c);
                    fixed.push_back(n);
                    ++i;
                    continue;
                }
                fixed += "\\\\";   // 非法转义 → 还原成字面反斜杠
                continue;
            }
            fixed.push_back(c);
        }
        ok_ = TryParse(fixed, owned_) && owned_.is_object();
    }

    /// ── 借用已解析对象（零拷贝）──────────────────────────────────
    /// 用途：**整份文件已经解析过一次**时，逐动作取字段不该再解析一遍。
    /// 前科（2026-10-03 实测）：`LoadScriptFileData` 先用本类解析整个 14.4MB 文件
    /// （242ms），随后又对 `ExtractJsonActionBlocks` 抠出的 5881 个块**各解析一次**
    /// （355ms）—— 同一份 JSON 被 nlohmann 解析两遍，597ms / 793ms 全是重复劳动。
    /// 传入的 `obj` 必须比本视图活得久（调用点都是遍历已解析 DOM 的引用）。
    explicit WideObjectView(const nlohmann::json& obj)
        : borrowed_(&obj), ok_(obj.is_object()) {}

    bool valid() const { return ok_; }

    bool Has(const wchar_t* key) const {
        if (!ok_ || !key) return false;
        return Doc().find(ToAsciiKey(key)) != Doc().end();
    }

    std::wstring GetString(const wchar_t* key, const std::wstring& fallback = {}) const {
        if (!ok_ || !key) return fallback;
        const auto it = Doc().find(ToAsciiKey(key));
        if (it == Doc().end() || !it->is_string()) return fallback;
        return FromUtf8(it->get<std::string>());
    }

    double GetNumber(const wchar_t* key, double fallback) const {
        if (!ok_ || !key) return fallback;
        const auto it = Doc().find(ToAsciiKey(key));
        if (it == Doc().end() || !it->is_number()) return fallback;
        return it->get<double>();
    }

    bool GetBool(const wchar_t* key, bool fallback) const {
        if (!ok_ || !key) return fallback;
        const auto it = Doc().find(ToAsciiKey(key));
        if (it == Doc().end()) return fallback;
        if (it->is_boolean()) return it->get<bool>();
        // 兼容老脚本的 `1`/`0`
        if (it->is_number()) return it->get<double>() != 0.0;
        return fallback;
    }

    /// 直接拿已解析对象（少数需要遍历数组的场景）
    const nlohmann::json& raw() const { return Doc(); }

private:
    /// 字段名都是 ASCII 字面量（调用点全是 L"..." 形式，已核对），窄化安全。
    static std::string ToAsciiKey(const wchar_t* key) {
        std::string s;
        for (const wchar_t* p = key; *p; ++p) s.push_back(static_cast<char>(*p));
        return s;
    }

    /// 生效对象：借用时用外部引用，否则用自有解析结果（无效时是 null）。
    const nlohmann::json& Doc() const { return borrowed_ ? *borrowed_ : owned_; }

    nlohmann::json owned_;
    const nlohmann::json* borrowed_ = nullptr;
    bool ok_ = false;
};

/// ★★ **抠出第一个「单个 JSON 对象」**（2026-09-30 实测事故：模型答对了、我们没认出）。
///
/// 起因：模型回的是**一个裸对象**
///   `{"action":"mouseClick","params":{"x":172,"y":498}}`
/// —— 单条动作它不套数组。而 `ExtractFirstJsonArray` 只找 `[...]` ⇒ 认不出 ⇒
///   先被当成"嘴炮"（未调用工具）、再报「API 未返回有效动作 JSON」整轮失败，
///   而**模型其实完全答对了**（用户看到"一题都没做"）。
/// 判据与数组版同源：按深度配对 `{}`，字符串内的括号不计。
inline std::wstring ExtractFirstJsonObject(const std::wstring& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != L'{') continue;
        int depth = 0;
        bool inStr = false;
        bool esc = false;
        for (size_t k = i; k < text.size(); ++k) {
            const wchar_t c = text[k];
            if (inStr) {
                if (esc) esc = false;
                else if (c == L'\\') esc = true;
                else if (c == L'"') inStr = false;
                continue;
            }
            if (c == L'"') { inStr = true; continue; }
            if (c == L'{') ++depth;
            else if (c == L'}') {
                --depth;
                if (depth == 0) return text.substr(i, k - i + 1);
            }
        }
    }
    return {};
}

/// 从自由文本（LLM 回复、带 [EXECUTED] 标记的日志）里抠出第一个 JSON 数组。/// 跳过 `[` 后不是 `{`/`[` 的方括号，按深度配对（字符串内的括号不计）。
inline std::wstring ExtractFirstJsonArray(const std::wstring& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != L'[') continue;
        size_t j = i + 1;
        while (j < text.size() && (text[j] == L' ' || text[j] == L'\t'
            || text[j] == L'\r' || text[j] == L'\n')) {
            ++j;
        }
        if (j >= text.size()) break;
        if (text[j] != L'{' && text[j] != L'[') continue;
        int depth = 0;
        bool inStr = false;
        bool esc = false;
        for (size_t k = i; k < text.size(); ++k) {
            const wchar_t c = text[k];
            if (inStr) {
                if (esc) esc = false;
                else if (c == L'\\') esc = true;
                else if (c == L'"') inStr = false;
                continue;
            }
            if (c == L'"') { inStr = true; continue; }
            if (c == L'[') ++depth;
            else if (c == L']') {
                --depth;
                if (depth == 0) return text.substr(i, k - i + 1);
            }
        }
        break;
    }
    return L"";
}

}  // namespace jsonutil
}  // namespace qst
