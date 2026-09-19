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

/// 从自由文本（LLM 回复、带 [EXECUTED] 标记的日志）里抠出第一个 JSON 数组。
/// 跳过 `[` 后不是 `{`/`[` 的方括号，按深度配对（字符串内的括号不计）。
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
