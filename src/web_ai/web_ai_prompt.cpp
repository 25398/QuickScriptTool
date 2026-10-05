// ──────────────────────────────────────────────────────────────────
// web_ai_prompt.cpp — 网页版 AI 后端的纯逻辑层实现（无 Win32、无网络、无状态）
//
// 只依赖 nlohmann/json 与标准库 ⇒ 可以被 `WebAiSelfTest` 单独链接、逐格断言。
// 这里**不许**加任何「读文件 / 起线程 / 打日志」的代码：一旦掺进来，自检就跑不动，
// 而这一层恰好是最容易静默出错的一层（协议解析错 = 用错坐标执行动作）。
// ──────────────────────────────────────────────────────────────────

#include "web_ai/web_ai_prompt.h"

#include "json_util.h"
#include "window_mode/window_mode_log.h"   // 出站守卫诊断（GuardOutboundUtf8）

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <string>

namespace quickscript::webai {

using json = nlohmann::json;

const char* const kToolCallsBegin = "<<<QST_TOOL_CALLS>>>";
const char* const kToolCallsEnd = "<<<END_QST_TOOL_CALLS>>>";
const char* const kLoadToolsName = "loadTools";

namespace {

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool Contains(const std::string& hay, const std::string& needle) {
    return !needle.empty() && hay.find(needle) != std::string::npos;
}

std::string TrimAscii(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    auto isWs = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    while (b < e && isWs(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && isWs(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

/// 单行化 + 截断（工具描述常带多行与 Markdown，进提示词前必须压平）
std::string OneLine(const std::string& s, size_t maxChars) {
    std::string out;
    out.reserve(s.size());
    bool prevSpace = false;
    for (char ch : s) {
        const bool ws = (ch == '\r' || ch == '\n' || ch == '\t');
        const char c = ws ? ' ' : ch;
        if (c == ' ' && prevSpace) continue;
        prevSpace = (c == ' ');
        out.push_back(c);
    }
    out = TrimAscii(out);
    if (out.size() > maxChars) {
        // ★ 按字节截断会切碎多字节字符 ⇒ 走**共享的**边界安全截断
        //   （原来这里只退续字节、没退被切断的头字节 ⇒ 半个汉字 ⇒ 请求 500 / 静默闪退）
        out = qst::jsonutil::TruncateUtf8Safe(out, maxChars, "…");
    }
    return out;
}

/// FNV-1a 64（指纹用；不需要密码学强度，只需要稳定且短）
std::string Fnv1aHex(const std::string& s) {
    unsigned long long h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= static_cast<unsigned long long>(c);
        h *= 1099511628211ULL;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", h);
    return buf;
}

}  // namespace

std::string MessageTextForPrompt(const json& msg) {
    if (!msg.is_object()) return {};
    const auto it = msg.find("content");
    if (it == msg.end() || it->is_null()) {
        // assistant 的纯工具调用消息：正文为空但有 tool_calls，给一行可读摘要
        if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
            std::string names;
            for (const auto& tc : msg["tool_calls"]) {
                std::string n;
                if (tc.contains("function") && tc["function"].is_object()) {
                    n = tc["function"].value("name", "");
                }
                if (n.empty()) n = tc.value("name", "");
                if (n.empty()) continue;
                if (!names.empty()) names += "、";
                names += n;
            }
            if (!names.empty()) return "(我调用了工具：" + names + ")";
        }
        return {};
    }
    if (it->is_string()) return it->get<std::string>();
    if (it->is_array()) {
        std::string out;
        for (const auto& part : *it) {
            if (!part.is_object()) continue;
            const std::string type = part.value("type", "");
            if (type == "text" || type == "input_text") {
                if (!out.empty()) out += "\n";
                out += part.value("text", "");
            } else if (type == "image_url" || type == "image" || type == "input_image") {
                // ⚠ 图片**不走文本通道**：这里只留占位，真正的图片要走扩展的
                //   上传通道（`[[AGENT_IMG:路径]]` → 本地临时文件 → CDP 挂到 file input）。
                std::string mime = "image";
                const auto iu = part.find("image_url");
                if (iu != part.end() && iu->is_object()) {
                    const std::string u = iu->value("url", "");
                    const size_t semi = u.find(";base64,");
                    if (u.rfind("data:", 0) == 0 && semi != std::string::npos) {
                        mime = u.substr(5, semi - 5);
                    }
                }
                if (!out.empty()) out += "\n";
                out += "[图片:" + mime + "]";
            }
        }
        return out;
    }
    return {};
}

int CountImageParts(const json& msg) {
    if (!msg.is_object()) return 0;
    const auto it = msg.find("content");
    if (it == msg.end() || !it->is_array()) return 0;
    int n = 0;
    for (const auto& part : *it) {
        if (!part.is_object()) continue;
        const std::string type = part.value("type", "");
        if (type == "image_url" || type == "image" || type == "input_image") ++n;
    }
    return n;
}

namespace {
/// 本文件的时钟（避免依赖 utils.h）
long long NowMsLocal() {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
}  // namespace

std::string MessageFingerprint(const json& msg) {
    if (!msg.is_object()) return Fnv1aHex("?");
    std::string s = msg.value("role", "");
    s += '\x1f';
    s += MessageTextForPrompt(msg);
    s += '\x1f';
    if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
        s += msg["tool_calls"].dump();
    }
    if (msg.contains("tool_call_id") && msg["tool_call_id"].is_string()) {
        s += msg["tool_call_id"].get<std::string>();
    }
    return Fnv1aHex(s);
}

std::string RenderToolList(const json& tools, int maxChars, bool* trimmed) {
    if (trimmed) *trimmed = false;
    if (!tools.is_array() || tools.empty()) return {};

    struct Row {
        std::string head;   ///< `name(params)`
        std::string desc;   ///< 描述（可丢）
    };
    std::vector<Row> rows;
    int idx = 0;
    for (const auto& t : tools) {
        if (!t.is_object()) continue;
        const json* fn = &t;
        if (t.contains("function") && t["function"].is_object()) fn = &t["function"];
        const std::string name = fn->value("name", "");
        if (name.empty()) continue;
        ++idx;

        std::string params;
        const auto pit = fn->find("parameters");
        if (pit != fn->end() && pit->is_object()) {
            const auto props = pit->find("properties");
            std::vector<std::string> required;
            if (pit->contains("required") && (*pit)["required"].is_array()) {
                for (const auto& r : (*pit)["required"]) {
                    if (r.is_string()) required.push_back(r.get<std::string>());
                }
            }
            if (props != pit->end() && props->is_object()) {
                for (auto p = props->begin(); p != props->end(); ++p) {
                    if (!params.empty()) params += ", ";
                    params += p.key();
                    std::string type = "any";
                    bool req = std::find(required.begin(), required.end(), p.key()) != required.end();
                    if (p.value().is_object()) {
                        type = p.value().value("type", "any");
                        // 枚举值很短时直接给出来（如 button=left|right），能显著减少模型猜错
                        if (p.value().contains("enum") && p.value()["enum"].is_array()) {
                            std::string enums;
                            for (const auto& e : p.value()["enum"]) {
                                std::string v = e.is_string() ? e.get<std::string>() : qst::jsonutil::DumpUtf8Safe(e);
                                if (v.size() > 12) { enums.clear(); break; }
                                if (!enums.empty()) enums += "|";
                                enums += v;
                            }
                            if (!enums.empty()) type = enums;
                        }
                    }
                    params += ":";
                    params += type;
                    if (!req) params += "?";
                }
            }
        }
        Row r;
        r.head = name + "(" + params + ")";
        r.desc = OneLine(fn->value("description", ""), 160);
        rows.push_back(std::move(r));
    }
    if (rows.empty()) return {};

    auto build = [&](bool withDesc) {
        std::string out;
        int n = 0;
        for (const auto& r : rows) {
            ++n;
            std::string line = std::to_string(n) + ". " + r.head;
            if (withDesc && !r.desc.empty()) line += " — " + r.desc;
            out += line;
            out += "\n";
        }
        return out;
    };

    std::string out = build(true);
    if (maxChars > 0 && static_cast<int>(out.size()) > maxChars) {
        // 超预算：先丢描述（**保结构**：工具名与参数名是能调用的前提，描述只是指引）
        out = build(false);
        if (trimmed) *trimmed = true;
    }
    if (maxChars > 0 && static_cast<int>(out.size()) > maxChars) {
        // 还超：按行截断，并明说被截断（不许静默丢工具）
        std::string cut;
        size_t pos = 0;
        while (pos < out.size()) {
            const size_t nl = out.find('\n', pos);
            const std::string line = out.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            if (static_cast<int>(cut.size() + line.size() + 1) > maxChars) break;
            cut += line;
            cut += "\n";
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        out = cut + "…（工具清单过长，其余已省略；如确需更多工具请在 web_ai_config.json 提高 maxToolListChars）\n";
        if (trimmed) *trimmed = true;
    }
    return out;
}

namespace {

/// 取一个工具对象的 `function` 子对象（兼容 `{name:..}` 与 `{function:{name:..}}`）
const json* ToolFn(const json& t) {
    if (!t.is_object()) return nullptr;
    if (t.contains("function") && t["function"].is_object()) return &t["function"];
    return &t;
}

/// 取参数名列表（按 `properties` 的声明顺序）
std::vector<std::string> ToolParamNames(const json& fn) {
    std::vector<std::string> out;
    const auto pit = fn.find("parameters");
    if (pit == fn.end() || !pit->is_object()) return out;
    const auto props = pit->find("properties");
    if (props == pit->end() || !props->is_object()) return out;
    for (auto p = props->begin(); p != props->end(); ++p) out.push_back(p.key());
    return out;
}

// ── 目录里「枚举取值」的两道上限（2026-10-02 修，见 LESSONS §80）────────────
//
// ⚠ 原判据是「**任一**取值 > 12 字符 ⇒ **整组**丢弃」，实测**静默丢掉 3 组**：
//     `switchWindow` 的 `default|window|backgroundWindow`（最长 16）
//     `computer` 的 13 个 action（最长 `cursor_position` = 15）
//     一组 4 个取值（最长 `hotkeyShortcut` = 14）
//   ⇒ 模型在目录里**一个取值都看不到**，只能自己编（"模型编个 button=middle"正是这条要挡的）。
// ⇒ 改成「**整组总长**预算」：整组塞得下就给 —— 这才是"省字"的本意。
// ⚠ 超预算仍然**整组丢弃**（不是截一半）：给一半会让模型以为那就是全部取值。
constexpr size_t kMaxEnumChars = 160;  ///< 整组取值的总字符预算
constexpr size_t kMaxEnumValue = 24;   ///< 单个取值上限（防个别超长项把整组拖爆）

}  // namespace

std::string RenderToolCatalog(const json& tools, int descChars, int maxChars, bool* trimmed) {
    if (trimmed) *trimmed = false;
    if (!tools.is_array() || tools.empty()) return {};

    std::string out;
    int idx = 0;
    int used = 0;
    for (const auto& t : tools) {
        const json* fn = ToolFn(t);
        if (!fn) continue;
        const std::string name = fn->value("name", "");
        if (name.empty()) continue;
        ++idx;

        // 参数：**名字** + （仅当枚举很短时）取值。
        //   ⚠ 类型/长描述一律留给 `loadTools` 之后的完整定义；
        //     但**短枚举必须留下**（`button:left|right` 才 10 来个字符，
        //     却能挡住"模型自己编一个 button=middle"这类错误 —— 性价比极高）。
        std::string pstr;
        const auto pit2 = fn->find("parameters");
        if (pit2 != fn->end() && pit2->is_object()) {
            const auto props2 = pit2->find("properties");
            if (props2 != pit2->end() && props2->is_object()) {
                for (auto p = props2->begin(); p != props2->end(); ++p) {
                    if (!pstr.empty()) pstr += ", ";
                    pstr += p.key();
                    if (!p.value().is_object()) continue;
                    const auto eit = p.value().find("enum");
                    if (eit == p.value().end() || !eit->is_array()) continue;
                    std::string enums;
                    for (const auto& e : *eit) {
                        const std::string v = e.is_string() ? e.get<std::string>() : qst::jsonutil::DumpUtf8Safe(e);
                        // ★ 两道上限（kMaxEnumValue / kMaxEnumChars，见本文件上方注释）：
                        //   ⚠ 旧写法「任一取值 > 12 ⇒ 整组丢」会静默丢掉 `switchWindow` /
                        //     `computer` 等 3 组取值 ⇒ 模型只能自己编。
                        if (v.size() > kMaxEnumValue) { enums.clear(); break; }
                        const size_t need = enums.size() + (enums.empty() ? 0 : 1) + v.size();
                        if (need > kMaxEnumChars) { enums.clear(); break; }
                        if (!enums.empty()) enums += "|";
                        enums += v;
                    }
                    if (!enums.empty()) pstr += ":" + enums;
                }
            }
        }

        std::string line = std::to_string(idx) + ". " + name + "(" + pstr + ")";
        const std::string desc = OneLine(fn->value("description", ""), descChars);
        if (!desc.empty()) line += " — " + desc;
        line += "\n";

        if (maxChars > 0 && used + static_cast<int>(line.size()) > maxChars) {
            // ⚠ 超预算就**明说被截断**，不许静默丢工具（LESSONS §21）
            out += "…（目录过长，其余工具已省略；"
                   "如确需更多请在 web_ai_config.json 提高 maxToolListChars）\n";
            if (trimmed) *trimmed = true;
            return out;
        }
        out += line;
        used += static_cast<int>(line.size());
    }
    return out;
}

std::string RenderToolDefinitions(const json& tools, const std::vector<std::string>& names) {
    if (!tools.is_array() || names.empty()) return {};

    std::string out;
    for (const auto& want : names) {
        const json* hit = nullptr;
        for (const auto& t : tools) {
            const json* fn = ToolFn(t);
            if (fn && fn->value("name", "") == want) { hit = fn; break; }
        }
        if (!hit) {
            // ⚠ 如实说"没有这个工具"，并**列出可用名字**（模型据此纠正自己）
            out += "【" + want + "】⚠ 没有这个工具（名字写错了？）。可用工具见上面的目录。\n\n";
            continue;
        }
        out += "【" + want + "】\n";
        const std::string desc = hit->value("description", "");
        if (!desc.empty()) out += "  说明：" + desc + "\n";

        const auto pit = hit->find("parameters");
        if (pit != hit->end() && pit->is_object()) {
            std::vector<std::string> required;
            if (pit->contains("required") && (*pit)["required"].is_array()) {
                for (const auto& r : (*pit)["required"]) {
                    if (r.is_string()) required.push_back(r.get<std::string>());
                }
            }
            const auto props = pit->find("properties");
            if (props != pit->end() && props->is_object()) {
                out += "  参数：\n";
                for (auto p = props->begin(); p != props->end(); ++p) {
                    std::string type = "any";
                    std::string pdesc;
                    std::string enums;
                    if (p.value().is_object()) {
                        type = p.value().value("type", "any");
                        pdesc = p.value().value("description", "");
                        if (p.value().contains("enum") && p.value()["enum"].is_array()) {
                            for (const auto& e : p.value()["enum"]) {
                                if (!enums.empty()) enums += "|";
                                enums += e.is_string() ? e.get<std::string>() : qst::jsonutil::DumpUtf8Safe(e);
                            }
                        }
                    }
                    const bool req = std::find(required.begin(), required.end(), p.key())
                        != required.end();
                    out += "    " + p.key() + " (" + type + (req ? "，必填" : "，可选") + ")";
                    if (!enums.empty()) out += " 取值：" + enums;
                    if (!pdesc.empty()) out += " —— " + OneLine(pdesc, 120);
                    out += "\n";
                }
            } else {
                out += "  参数：（无）\n";
            }
        } else {
            out += "  参数：（无）\n";
        }
        out += "\n";
    }
    return out;
}

bool TakeLoadToolNames(json& toolCalls, std::vector<std::string>& namesOut) {
    namesOut.clear();
    if (!toolCalls.is_array()) return false;

    json keep = json::array();
    bool found = false;
    for (auto& call : toolCalls) {
        if (!call.is_object()) { keep.push_back(call); continue; }
        if (call.value("name", "") != kLoadToolsName) { keep.push_back(call); continue; }

        found = true;
        // arguments 可能是对象，也可能是 JSON 字符串（本仓两种都见过）
        json args = call.value("arguments", json::object());
        if (args.is_string()) {
            const json parsed = json::parse(args.get<std::string>(), nullptr, false);
            args = parsed.is_discarded() ? json::object() : parsed;
        }
        if (!args.is_object()) continue;
        const auto it = args.find("names");
        if (it != args.end() && it->is_array()) {
            for (const auto& n : *it) {
                if (n.is_string() && !n.get<std::string>().empty()) {
                    namesOut.push_back(n.get<std::string>());
                }
            }
        } else if (it != args.end() && it->is_string()) {
            namesOut.push_back(it->get<std::string>());   // 单个名字也认
        }
    }
    toolCalls = std::move(keep);
    return found;
}

namespace {

/// ★★ **拼给模型的文本：出站守卫**（2026-09-30）。
///
/// 连续两次 500 都是"提示词里出现非法 UTF-8"：一次是动作归一化带出的坏字节，
/// 一次是我在工具清单里做**定长截断**时只退了续字节、没退被截断的头字节（留下半个汉字）。
/// 与其要求每一处拼接/截断自己小心，不如在**这个构建函数的出口**统一过一次闸：
/// 非法序列替换成 U+FFFD，并打一行诊断（指明"上游切进了多字节字符"，好去找那一处）。
/// ⚠ 只**修**不猜：不改语义、不删内容 —— 被替换的字节本来就无法被正确理解。
TurnPlan GuardOutboundUtf8(TurnPlan&& plan) {   // 按引用推进：不复制（栈/堆都省一次）
    size_t fixed = 0;
    size_t firstBad = static_cast<size_t>(-1);
    const std::string original = plan.text;   // 只在出问题时用来取证（正常路径不动它）
    std::string clean = qst::jsonutil::SanitizeUtf8(plan.text, &fixed, &firstBad);
    if (fixed > 0) {
        plan.text.swap(clean);
        plan.totalChars = static_cast<int>(plan.text.size());
        // ★★ **取证：坏字节在哪一小节、长什么样**（2026-09-30）。
        //   只报"N 字节"不够用（两次日志分别是 37/40 字节，谁也看不出是哪一段）⇒ 这里给出
        //   ① 小节名（最近的 `=====` 标题行）；② 坏字节前后一段的**十六进制**
        //   （用 hex 而不是原文：否则日志本身又被同一批坏字节污染）；③ 提示词内偏移。
        std::string section = "(未找到小节标题)";
        if (firstBad != static_cast<size_t>(-1) && firstBad <= original.size()) {
            const size_t head = original.rfind("\n=====", firstBad);
            if (head != std::string::npos) {
                const size_t eol = original.find('\n', head + 1);
                section = original.substr(head + 1,
                    (eol == std::string::npos ? 48 : (std::min)(eol - head - 1, size_t(48))));
            }
        }
        char hex[256]{};
        int used = 0;
        if (firstBad != static_cast<size_t>(-1)) {
            const size_t from = firstBad > 24 ? firstBad - 24 : 0;
            const size_t to = (std::min)(original.size(), firstBad + 40);
            for (size_t k = from; k < to && used < 230; ++k) {
                used += sprintf_s(hex + used, sizeof(hex) - static_cast<size_t>(used), "%02X ",
                    static_cast<unsigned char>(original[k]));
            }
        }
        windowmode::WindowModeLogEventf(
            L"[网页AI] 提示词出站守卫：修掉 %zu 字节非法 UTF-8（已替换为 U+FFFD）"
            L"｜小节=「%s」｜偏移=%zu｜上下文(hex)=%s",
            fixed, FromUtf8(section).c_str(), firstBad, FromUtf8(hex).c_str());
    }
    // ⚠⚠ **这一行必须是 `return plan;`**：早前一次批量文本替换把本函数的收尾也换成了
    //   `return GuardOutboundUtf8(std::move(plan));` ⇒ **自己调自己** ⇒ `0xC00000FD`
    //   （STATUS_STACK_OVERFLOW）⇒ 软件在**提示词拼好后立刻闪退**，日志恰好停在"出站守卫"那行。
    //   教训：批量替换必须**回读确认命中点**，别把函数自身的收尾也换掉。
    return plan;
}

}  // namespace
TurnPlan PlanTurn(const json& messages, const json& tools, const SessionState& session,
                  SessionState& next, const PromptBudget& budget, int imagesAttached) {    TurnPlan plan;
    if (!messages.is_array() || messages.empty()) {
        plan.error = "NO_MESSAGES";
        return GuardOutboundUtf8(std::move(plan));
    }

    // ── ① 判断这一轮是不是「同一个对话的续轮」─────────────────────
    //   判据：条数增加了，且**上一轮最后一条消息仍在原位且内容一致**。
    //   ⚠ 不能只比条数：AI 动作执行会重建请求体（关思考重试、纠偏注入都会改历史），
    //     条数可能相同而内容不同 —— 那时若当成续轮，就会把一整段历史当成"新增"重发。
    // ── ① 判「还是同一个网页对话吗」──────────────────────────────────
    //
    // ⚠⚠ **不要试图放宽这个判据**（2026-09-26 我踩过）：
    //   我一度改成"只要开过就算续轮"，结果打红了两条**真实场景**用例 ——
    //   ① `plan_history_rewrite_is_new_conversation`：引擎重建请求体时
    //      （关思考重试 / 纠偏注入 / 历史压缩）**锚点消息会被改写** ⇒ 必须当新对话重发全量；
    //   ② `plan_repeat_request_resends_fully`：同一份请求被重复提交（messages 逐字相同）
    //      ⇒ 也必须重发全量，否则"同一个动作跑第二次"会永久失败。
    //   ⇒ "判成新对话"本身**不是 bug**，它是对"网页里那份上下文已经对不上"的**正确反应**。
    //
    //   真正会让人看到"对话跳了"的，是**两个 UI 会话共用一份会话状态 / 一个网页标签页**
    //   ⇒ 那由 `sessionKey`（web_ai_backend.cpp）+ 扩展按 key 分标签页解决，不在这里。
    bool sameConversation = false;
    const int total = static_cast<int>(messages.size());
    if (session.turns > 0 && session.sentMessageCount > 0
        && session.sentMessageCount <= total) {
        const int prevLast = session.sentMessageCount - 1;
        if (prevLast < total
            && MessageFingerprint(messages[prevLast]) == session.lastFingerprint) {
            sameConversation = (total > session.sentMessageCount);
        }
    }
    // ★★ 拆成两件正交的事（见 `web_ai_prompt.h` 的 `resendFull` 说明）
    // ★★★ 历史默认**不发**（2026-10-02 用户要求，本轮核心改动）
    //
    //   用户原话："本来就在同一个对话里面，每次发送请求体的时候，还把上次回答、
    //   上上次回答都堆进去了，这就导致对话越往下，堆积的请求体越多，AI 回答就越来越慢，
    //   这些内容缓存就行，等 AI 需要的时候通过我们给定的方式从我们这里提取
    //   （再说 AI 本身就有记忆上下文的功能）"
    //
    //   ⚠⚠ 原判据是"消息指纹匹配 ⇒ 续轮"，但它**一失效就全量重发**：真机日志实证
    //     `轮次=2 新对话=0 提示词=6072 字` → `轮次=17 ... 提示词=13792 字`
    //     —— 每轮都是全量（增量应是几百字且不累积）⇒ 越聊越慢。
    //   ⇒ 现在**只看"是不是这个会话第一次"**：`turns == 0`（首次，或后端因标签页
    //     重建清零过）⇒ 全量；其余一律**只发增量**（网页对话自己持有上下文）。
    //
    //   ⚠ 已知副作用（用户明确接受）：历史被引擎重写（关思考重试 / 纠偏注入 / 历史压缩）时，
    //     网页对话里的内容会和我们这边的历史不完全一致 —— 换来的是每轮体积恒定。
    plan.resendFull = (session.turns == 0);
    plan.newConversation = (session.turns == 0);   // ★ 只有"这个会话第一次"才开新网页对话
    int start = plan.resendFull ? 0 : session.sentMessageCount;
    // ⚠ 保护：历史被压缩/变短时 `sentMessageCount` 可能 ≥ `total` ⇒ 直接切会发出**空内容**
    //   （输入框空着，站点不动或报错）⇒ 退化成"只发最后一条"，至少这一轮有内容。
    if (start >= total) start = (total > 0) ? (total - 1) : 0;

    // ── ② 首次前缀：系统设定 + 工具清单 + 协议说明 ─────────────────
    // ⚠ 前缀跟着 `resendFull` 走（**不是** `newConversation`）：内容对不上就必须重发，
    //   但这与"要不要开新网页对话"无关。
    std::string prefix;
    /// 这次到底发没发工具说明（用于更新 `next.toolsSent`）
    bool sentToolSection = false;
    /// ★ 本次工具清单的**内容指纹**（见 `SessionState.toolsFingerprint` 的说明）
    const std::string toolsFp = Fnv1aHex(tools.dump());
    /// ★★ 该不该发工具清单：**内容变了就发**（不是"每会话只发一次"）
    ///   ⚠ 这正是"其他任务跑不通"的修复 —— 只按 `turns==0` 发会让换任务时漏发。
    const bool needToolSection = !tools.is_array() || tools.empty()
        ? false : (toolsFp != session.toolsFingerprint);

    // ★★★ 前缀**每次都发**（2026-10-02 真机事故，必须改回来）
    //   `prefix` = 系统设定（"你是 Windows 宏 Agent，只输出 JSON 动作"）+ 协议 + 工具清单。
    //   ⚠⚠ 它原来只在 `resendFull` 时加。我上一轮把 `resendFull` 收紧成"只有会话首次"后，
    //     **第二个任务**（`turns>0`）就**拿不到 system 和协议** ⇒ 模型不知道自己在干什么
    //     ⇒ **开始乱猜工具名**（日志实证：`screenshot` → `computer` → `fetchWebPage` →
    //     `screenCapture` → `findImage`，5 轮全废 ⇒ `NOT_FOUND`）⇒ **别的任务被打挂**。
    //   ⇒ 系统设定与协议是"**每轮都要知道**"的东西，必须每次发；
    //     只有**工具清单**是"参考手册"（段内已有 `!session.toolsSent` 条件 ⇒ 每会话一次）✓
    if (true) {
        std::string sysText;
        for (const auto& m : messages) {
            if (!m.is_object()) continue;
            if (m.value("role", "") != "system") continue;
            const std::string t = MessageTextForPrompt(m);
            if (t.empty()) continue;
            if (!sysText.empty()) sysText += "\n\n";
            sysText += t;
        }
        bool trimmed = false;
        // ★★★ 极简模式（2026-09-26 用户要求）：
        //   首轮**连工具目录都不发**，只发 1~2 句"有这么回事" + `loadTools` 的用法。
        //   用户原话："首次对话就加 1~2 句说明一下，让 AI 意识到有这么回事就行了，
        //   等 AI 后面需要用到的时候再发这些"。
        //   ⚠ 上一版发的是"工具目录"（名字+参数名+一句话，约 3 KB）——
        //     用户仍然觉得"很长"。这一版首轮只剩 ~300 字。
        const bool catalogMode = budget.toolCatalogMode;
        const std::string toolList = catalogMode
            ? std::string()                       // ← 极简：首轮**不发**目录
            : RenderToolList(tools, budget.maxToolListChars, &trimmed);
        plan.toolsTrimmed = trimmed;

        prefix += "【键鼠工坊 · 网页 AI 桥接】下面这段是程序发来的结构化请求，不是普通聊天。\n";
        prefix += "你只能看到被写进输入框的这部分内容；请直接给出结果，不要复述本说明。\n";
        // ★★ **别让网页端的模型自己动手**（2026-09-29，用户报障后加）。
        //
        // 起因：用户用网页 AI 桥接跑「打开 B 站给有山先生点赞」时，**豆包网页版自己进了
        //   "任务模式"** —— 它自己去点页面、自己开新标签、触发 B 站 **412 风控**，
        //   回复也变成散文（"我先加载桌面工具清单，然后按步骤操作。"）而没有动作 JSON ⇒ 整轮失败。
        //   用户原话：「应该让他走我们软件的流程吧，他自己的流程都会触发风控的」。
        //
        // 为什么必须**明说**：网页端模型默认把"打开网页/点赞"当成要自己上网完成的任务；
        //   而本链路里**执行者是本软件**（鼠标/键盘/找图/扩展），模型只负责**给 JSON 动作**。
        //   不说这一句，它就会用自己的联网/浏览器能力 —— 那既不可控（风控/不可回放），
        //   又和我们的动作表完全脱节。
        prefix += "★**不要使用你自己的任何联网 / 浏览器 / 搜索 / 任务(agent)能力**："
                  "不要自己去打开网页、点击页面、调用你的工具，也不要进「任务模式」。\n"
                  "  所有操作都由**本软件**执行（鼠标/键盘/找图/扩展）。你只做一件事："
                  "**输出规定的 JSON 动作**，然后等软件把执行结果发给你。\n";
        // ★★ **别把"工具"理解成"你的函数"**（2026-09-30 实测：模型因此整轮拒绝执行）。
        //   实测原话：「协议里列的 computer / mouseClick / locateAndClick … 等键鼠工具，
        //   在我当前运行环境里都不存在（调用即报"未知工具 / Function not found），
        //   我没法把点击动作送回你本机」⇒ 然后它给了一大段解释、一个动作都不出。
        //   根因：它把清单当成**自己的原生函数**去调用。这句话把边界钉死。
        prefix += "⚠⚠ **下面列的工具不是你这边的函数**，你**不需要**拥有它们、也**不要**去"
                  "\"调用\"它们（更不要报\"未知工具/Function not found\"）："
                  "它们由**本软件在你用户的本机**执行。你只要按格式**写出**要执行的动作，"
                  "软件执行后会把结果回给你。\n";
        prefix += "⚠ **不要要求用户自己做任何事**（不要说\"请你把题目贴给我\"\"请你自己点\"）："
                  "你拿不到的信息，用规定格式要（如 computer(action=screenshot)），软件会给你。\n";
        prefix += "若你看到「任务已完成/已打开网页/触发风控」这类字样：那是**你自己**在动手，"
                  "请立刻停下 —— 本链路只需要 JSON。\n\n";
        if (!sysText.empty()) {
            prefix += "===== 系统设定（必须始终遵守）=====\n";
            prefix += sysText;
            prefix += "\n\n";
        }
        // ★★ 协议瘦身（2026-10-02）：工具清单**每会话只发一次**。
        //   ⚠ 原来它无条件跟在 `resendFull` 里 ⇒ 每次历史重写都重发
        //     （日志实证：多数请求 ~500 字，重写那次 **9099 字**）。
        //   ⚠ 标签页被重建时后端会把会话状态清零 ⇒ `toolsSent` 回 false ⇒ 自动重发 ✓
        if (catalogMode && needToolSection) {
            sentToolSection = true;
            // ★★ **首轮内联"精简工具清单"**（2026-09-30）。
            //
            // 原来只写"有 N 个工具，需要时再取清单"⇒ 实测模型**连调两轮 `loadTools`**
            //   （第 1、2 轮各烧 ~7~14 秒，共 ~21 秒）才开始干活；而且它在"要清单"
            //   这件事上打转时容易忘记任务本身。
            // 现在首轮就给**名字 + 一句话用途**（约 1~2 KB），完整参数定义仍可按需
            //   `loadTools(names=[…])` 取 —— 清单来源是**同一个 `tools` 数组**（不另抄一份）。
            std::string compact;
            int toolCount = 0;
            for (const auto& t : tools) {
                if (!t.is_object()) continue;
                const json* fn = &t;
                if (t.contains("function") && t["function"].is_object()) fn = &t["function"];
                const std::string nm = fn->value("name", "");
                if (nm.empty()) continue;
                ++toolCount;
                std::string desc = fn->value("description", "");
                // 只取第一句、截到 ~48 字节：这一节要的是"知道有什么"，不是"怎么用"
                const size_t cut = desc.find_first_of("\n。.");
                if (cut != std::string::npos) desc = desc.substr(0, cut);
                if (desc.size() > 60) {
                    // 共享的边界安全截断（同一份实现，别再各写一遍）
                    desc = qst::jsonutil::TruncateUtf8Safe(desc, 58, "…");
                }
                compact += "- ";
                compact += nm;
                if (!desc.empty()) {
                    compact += "：";
                    compact += desc;
                }
                compact += "\n";
            }
            if (toolCount > 0) {
                // ★★ **回退：只给"名字：一句话用途"**（2026-09-30 用户实测后撤回完整定义）。
                //
                //   上一版把**完整 JSON Schema 定义**内联进首轮（想省掉 loadTools 的 2~3 轮），
                //   实测**适得其反**：豆包把那段 schema 当成**它自己的原生函数**去解析，
                //   然后回了一大段拒绝——
                //     「协议里列的 computer / mouseClick / locateAndClick / observePage /
                //       activateWindow 等键鼠工具，**在我当前运行环境里都不存在**
                //       （调用即报"未知工具 / Function not found"），我没法把点击动作送回你本机」
                //   整轮报废（还烧掉两次 `loadTools` + 一次 27 秒 NO_NEW_ANSWER）。
                //   ⇒ 结论：**给网页模型的工具清单不能长得像原生 function 定义**。
                //     紧凑的"名字：用途"它照做；完整 schema 它去当函数调。
                prefix += "===== 工具（名：用途）=====\n";
            // ★★★ 只发「**怎么要清单**」，不发清单本身（2026-10-02 用户决定）。
            //
            //   ⚠ 2026-09-30 曾回退到"内联紧凑清单"，理由是"内联 schema 会被豆包当成
            //     自己的原生函数而拒绝"。**用户指出：那不是方案的问题，是"没说清楚"** ——
            //     只要首轮明确讲"这些不是你的函数、由本软件在本机执行"，就不会被拒；
            //     而且**体量比内联清单小得多**。
            //   ⇒ 所以这里的重点是**把话说清楚**（归属 + 用法 + 只调一次），不是把清单塞进去。
            prefix += "===== 工具 =====\n";
            prefix += "本软件准备了 " + std::to_string(toolCount) + " 个桌面工具"
                      "（鼠标/键盘/找图/OCR/脚本/文件等），**由本软件在你用户的本机执行**。\n";
            prefix += "⚠ 它们**不是你的函数**：你不需要拥有、也不要去调用、更不要报"
                      "「未知工具 / Function not found」；你只要**写出 JSON 动作**，"
                      "本软件会执行并把结果回给你。\n";
            prefix += "需要清单时**输出这一条**（只调一次，别反复要）：\n";
            prefix += "[{\"action\":\"loadTools\"}]\n";
            prefix += "⚠ 已经知道工具名就**直接做**，不要为了「确认一下」去要清单。\n\n";
                // ★★ **不再提供 `loadTools`**（2026-09-30 实测：它每轮白烧 11~47 秒）。
                //
                //   实测一局里它被调用 **3 次**（11.5s + 47.1s + 11.5s ≈ **70 秒**），
                //   而且模型自己给出的诊断是「软件回传的'工具结果'始终是
                //   `[错误] 未知工具：loadTools`，我发的动作根本没被执行」——
                //   它把提示词里的**示例块**照抄出来，而那条路在 Agent 工具层没有实现，
                //   于是它在"要清单"上反复撞墙、最后写一大段拒绝。
                //   ⇒ 这里**彻底不提** `loadTools`：名字与用途已经在上面，
                //     参数不确定就**按名字试一次**（回执会说缺什么），这比"先要清单"快得多。
                prefix += "⚠ 参数不确定就**直接按名字调用**：回执会告诉你缺哪个字段/取值不合法，"
                          "照着补一次即可。**不要**试图「先取一份完整清单」（没有这个步骤）。\n\n";
                // ★★★ **唯一的输出模板**（2026-10-02，用户批准后加）。
                //
                //   起因（真机实测，一连串断线都源于此）：模型**每次换一种写法**都要我们跟着适配 ——
                //   已见 7 种形状：`{"action":…,"params":{…}}` / `{"name":…}` / `{"tool":…}` /
                //   顶层坐标 / 裸对象（不套数组）/ `{"actions":[…]}` / 单键（值=字符串 或 值=对象）。
                //   认不出来就整轮空转（`（无效动作项）`、`本批完成 0 步`）。
                //   ⇒ 与其继续加兼容，不如**只给一种写法**，把形状收敛成 1 种。
                //   ⚠ 刻意**不写成 JSON Schema / function 定义**：真机实测那样写会被网页模型当成
                //     「你自己的原生函数」而整轮拒绝执行（见上面 loadTools 那段教训）。
                //     这里只是一行 JSON 例子 + 四条规则。
                prefix += "===== 输出格式（**只有这一种**）=====\n";
                prefix += "[{\"action\":\"工具名\",\"参数1\":值,\"参数2\":值}]\n";
                prefix += "· 只输出这个 JSON 数组本身：**不要**代码块围栏、不要解释、不要复述本说明。\n";
                prefix += "· 需要多个动作就放进同一个数组（**按顺序执行**）；不需要动作时才用自然语言回答。\n";
                // ★★ 压住"绕圈"（2026-10-02 真机：树里 27 题全列好了，模型却连发
                //   screenshot→lookupMacroAction→observePage，一轮 10~30s，6 轮不点一下）。
                //   树/清单在场时，查手册与复述计划都是**纯浪费**：直接做。
                prefix += "· 每轮**只做一步**：直接输出下一步要执行的动作；"
                          "**不要**解释思路、不要查手册、不要复述计划、不要先截图——"
                          "能看见目标（清单/树里有）就直接点，做完看回执再说。\n";
                prefix += "· `action` 的值必须是上面清单里的**工具名**；参数**平铺在同一层**"
                          "（不要包 `params`/`arguments`，也不要改用 `name`/`tool` 当键）。\n";
                prefix += "· 坐标用**清单标题里写明的那个 upload 截图尺寸**（同一套口径），"
                          "不要自己换算成别的空间。\n\n";
            }
        } else if (!toolList.empty() && needToolSection) {
            sentToolSection = true;
            int toolCount = 0;
            for (const auto& t : tools) {
                if (!t.is_object()) continue;
                const json* fn = &t;
                if (t.contains("function") && t["function"].is_object()) fn = &t["function"];
                if (!fn->value("name", "").empty()) ++toolCount;
            }
            prefix += "===== 你可以调用的工具（共 ";
            prefix += std::to_string(toolCount);
            prefix += catalogMode
                ? " 个；下面只是**目录**，只有名字和参数名）=====\n"
                : " 个；参数名后带 ? 的可省略）=====\n";
            prefix += toolList;
            prefix += "\n";
            prefix += "===== 调用工具的唯一格式 =====\n";
            prefix += "需要调用工具时，**只**输出下面这种块（一次可以多个调用），块外不要写多余解释：\n";
            prefix += kToolCallsBegin;
            prefix += "\n[{\"name\":\"工具名\",\"arguments\":{\"参数\":\"值\"}}]\n";
            prefix += kToolCallsEnd;
            prefix += "\n不需要调用工具时，直接用自然语言回答即可。\n";
            prefix += "接下来会由程序执行工具，并把结果作为「工具结果」发给你，你再决定下一步。\n";
            if (catalogMode) {
                // ⚠⚠ 这段必须把「忘了先 load 也能用」写清楚：
                //   否则模型一旦漏掉第 ① 步，用户看到的就是"功能坏了"（静默功能缺失）。
                prefix += "\n===== 想看某个工具的**参数细节**时 =====\n";
                prefix += "上面的目录**没有**参数类型/取值/说明。要用某个工具时，"
                          "建议先输出一次 loadTools 把它要过来：\n";
                prefix += kToolCallsBegin;
                prefix += "\n[{\"name\":\"";
                prefix += kLoadToolsName;
                prefix += "\",\"arguments\":{\"names\":[\"工具名1\",\"工具名2\"]}}]\n";
                prefix += kToolCallsEnd;
                prefix += "\n程序会把它们的完整定义发给你，你再输出真正的调用。\n";
                prefix += "⚠ 忘了这一步**不影响使用** —— 直接调用目录里的工具同样会被执行；"
                          "loadTools 只是让你少看无关内容。\n";
            }
            prefix += "\n";
        }
        // ★★★ 续轮：**只强调最关键的一行**，不重复整段协议（2026-10-02 用户要求）
        //
        //   用户原话："约束提示词要保持精简，只在首轮给个大概，不要每轮重复发，
        //   只强调最关键部分，如果有必要也做成我说的工具列表那样"。
        //
        //   ⚠ 完整协议（`===== 输出格式 =====`）已经由 `needToolSection` 控制成"首轮才发"；
        //     这里补的是"**续轮不能完全没有约束**"—— 真机事故：第二个任务拿不到任何约束
        //     ⇒ 模型乱猜工具名（screenshot/computer/fetchWebPage…）。
        //   ⇒ 续轮给**两行**：输出形状 + 最关键的纪律。
        if (!plan.resendFull) {
            prefix += "===== 提醒 =====\n";
            prefix += "只输出 JSON 动作数组 `[{\"action\":\"工具名\",...}]`；"
                      "不要解释、不要查手册、不要先截图；能看见目标就直接做。\n";
            // ★★★ **按用户描述操作**（2026-10-02 用户强调的通用性）
            //
            //   用户原话："要按用户描述来啊，**双击不等于打开**，他直接用 run+路径是有问题的。
            //   要让 web 模型**按描述操作**，保证通用性（比如如果是在某个游戏界面，
            //   没有 run+路径替代双击的方案，那这个做法就是错误的）。"
            //
            //   ⚠ 真机事故：用户说"双击桌面上的绿色图标"，模型却输出
            //     `runProgram` + **猜出来的路径** `%USERPROFILE%\Desktop\绿色图标.exe`
            //     —— 那是**另一种操作**（运行程序），不是"双击图标"：
            //     图标不一定有同名 exe，游戏/网页里更没有"路径"这回事。
            //   ⇒ 必须明确：**照描述做**；要定位就找图/看索引，不要换方案。
            prefix += "⚠ **严格按用户描述的动作做**：说双击就找图再双击，说打开程序才用 runProgram；不要把双击图标改写成猜一个路径去运行程序，那在游戏或网页界面根本不存在。\n";

            // ★★★ "**怎么查工具**"必须**每轮都给**（2026-10-02 真机事故）：
            //   我原来把这段放在 `needToolSection`（工具指纹变了才发）里 ⇒ **续轮不发**
            //   ⇒ 模型不知道有 `loadTools` 这条路 ⇒ **乱猜工具名**
            //     （日志实证：`computer` → `click` → `doubleClick`，全报"未知工具"）
            //     ⇒ 直接给出「当前可用动作列表未知，无法完成该操作」⇒ **任务失败**。
            //   ⚠ 它与"工具清单"是两回事：清单是**参考手册**（可每会话一次），
            //     而"怎么查"是**接口约定**（和"输出什么格式"同级，每轮都要给）。
            prefix += "⚠ 不确定有哪些工具时，输出 `[{\"action\":\"loadTools\"}]` 取清单"
                      "（**只调一次**，别反复要）；已经知道就直接做。\n";
            // ★★★ **脚本动作不能当工具直接调**（2026-10-02 真机，第 2 次强调）
            //
            //   用户说"点击桌面的绿色图标" ⇒ 模型查完参考后输出
            //     `[{"action":"findImage","imagePath":"images\\greenIcon.bmp",...}]`
            //   ⇒ **没有被执行**（`findImage` 是**脚本动作**，不是工具）⇒ 一下都没点 ✓
            //   ⚠ 上一轮我只把这条写进了 `loadTools` 的**清单**里 ⇒ 模型**要了清单才会看到**
            //     ⇒ 不够。这条属于"**接口约定**"，必须**每轮都给**（和 loadTools 同级）。
            prefix += "⚠ `findImage`/`mouseClick`/`keyClick`/`wait` 这类是**脚本动作**，"
                      "**不是工具**：它们不能这样直接调。要它们生效，先 "
                      "[{\"action\":\"readAgentSkill\",\"section\":\"scriptStrategy\"}] 拿规范，"
                      "再用 `planScriptActions` / `createMacroScript` **生成脚本**。\n\n";
        }
        prefix += "===== 对话 =====\n";
    }

    // ── ③ 正文：本轮新增的消息 ─────────────────────────────────────
    std::string body;
    for (int i = start; i < total; ++i) {
        const json& m = messages[i];
        if (!m.is_object()) continue;
        const std::string role = m.value("role", "");
        if (role == "system") continue;  // 已在首次前缀里
        plan.imagesStripped += CountImageParts(m);
        const std::string text = MessageTextForPrompt(m);
        if (role == "tool") {
            // 工具结果的 name 有两种来源：消息自带 name，或关联的 tool_call_id
            std::string who = m.value("name", "");
            if (who.empty()) who = m.value("tool_call_id", "");
            body += "【工具结果";
            if (!who.empty()) body += " " + who;
            body += "】\n";
            body += text.empty() ? "(空)" : text;
            body += "\n\n";
            continue;
        }
        if (role == "assistant") {
            // ⚠ 纯工具调用消息**不重发**：它已经作为上一轮的输出显示在网页对话里，
            //   再发一遍会让模型看到「自己说了两遍」，实测更容易陷入重复调用。
            if (text.empty()) continue;
            body += "【我上一轮的回答】\n";
            body += text;
            body += "\n\n";
            continue;
        }
        if (role == "user") {
            body += "【用户】\n";
            body += text.empty() ? "(空)" : text;
            body += "\n\n";
            continue;
        }
        // 未知角色（如 developer）：按普通文本发，别静默丢内容
        body += "【" + role + "】\n";
        body += text;
        body += "\n\n";
    }

    if (!sameConversation) {
        // 续轮时才提醒协议；首轮前缀里已经写得很清楚了。
    } else if (!tools.is_array() || tools.empty()) {
        // 无工具的续轮：什么都不用补
    } else {
        // ⚠ 紧凑模式从没教过 `<<<QST_TOOL_CALLS>>>` 块（那段只在完整清单分支里）⇒
        //   续轮提醒必须与**教过的那一种**一致，否则等于让模型用一个它没见过的格式。
        if (budget.toolCatalogMode) {
            body += "（继续：需要动作时仍用上面那种 JSON 数组："
                    "[{\"action\":\"工具名\",\"参数1\":值}]）\n";
        } else {
            body += "（继续：需要调用工具时仍用 " + std::string(kToolCallsBegin) + " 块）\n";
        }
    }

    // ★ 图片的话术必须**按差值**说，不能说反（说反 = 静默错误）：
    //   · 全部送到了 ⇒ 说「已附上 N 张图」（模型可以真的看图）
    //   · 一张没送到 ⇒ 才说「本通道无法回传图像」，并劝它改用文字定位
    //   · 部分送到   ⇒ 两句都说，并给出各是多少
    //   ⚠ `imagesAttached` 由调用方在上传成功后传进来（见 web_ai_image.h）。
    //     本函数**不**自己去上传 —— 纯逻辑层不许碰盘/网络。
    const int attached = (std::max)(0, (std::min)(imagesAttached, plan.imagesStripped));
    plan.imagesAttached = attached;
    const int notAttached = plan.imagesStripped - attached;
    if (attached > 0) {
        body += "\n（程序提示：本轮已通过**网页上传通道**附上 " + std::to_string(attached)
            + " 张图片，它们就在这次对话的附件里，请直接看图。）\n";
    }
    if (notAttached > 0) {
        body += "\n（程序提示：上面原本附带的 " + std::to_string(notAttached)
            + " 张图**没能送过去**（上传通道不可用或超出限额），请**不要**假装看到了它们。"
            "需要定位时改用文字：屏幕上的文字索引 / 元素索引已在上文给出，"
            "要点击某处请用可点元素的短标签作为目标。）\n";
    }

    plan.text = prefix + body;
    plan.totalChars = static_cast<int>(plan.text.size());

    if (!plan.newConversation && start >= total) {
        // 续轮但没有任何新增（历史上出现过：调用方重复提交同一份请求）
        // ⇒ 不能发空文本（编辑器会当成"没输入"），如实报错让上层重试。
        plan.error = "NO_NEW_MESSAGES";
        return GuardOutboundUtf8(std::move(plan));
    }

    if (plan.newConversation && budget.maxTotalChars > 0
        && plan.totalChars > budget.maxTotalChars) {
        // 前缀太长的唯一可控项就是工具清单 ⇒ 收紧它再拼一次（只丢描述/尾部行）
        const int tighter = (std::max)(2000, budget.maxToolListChars / 3);
        bool trimmed2 = false;
        const std::string toolList2 = RenderToolList(tools, tighter, &trimmed2);
        if (!toolList2.empty()) {
            const size_t begin = plan.text.find("===== 你可以调用的工具");
            const size_t end = plan.text.find("===== 调用工具的唯一格式", begin);
            if (begin != std::string::npos && end != std::string::npos) {
                plan.text = plan.text.substr(0, begin) + toolList2 + "\n"
                    + plan.text.substr(end);
                plan.totalChars = static_cast<int>(plan.text.size());
                plan.toolsTrimmed = true;
            }
        }
    }

    plan.valid = true;
    next.sentMessageCount = total;
    next.lastFingerprint = MessageFingerprint(messages[total - 1]);
    next.turns = session.turns + 1;
    // ★ 协议瘦身：记下这次发过的清单指纹（内容不变就不再发）
    next.toolsSent = session.toolsSent || sentToolSection;
    if (sentToolSection) next.toolsFingerprint = toolsFp;
    else next.toolsFingerprint = session.toolsFingerprint;
    return GuardOutboundUtf8(std::move(plan));
}

namespace {

/// 从 `from` 起找一个成对的 `open`/`close` 区间（跳过字符串与转义）。
/// ⚠ 不能简单用 `find(']')`：`arguments` 里的字符串可能含 `]`。
bool FindBalanced(const std::string& s, size_t from, char open, char close,
                  size_t& outBegin, size_t& outEnd) {
    const size_t b = s.find(open, from);
    if (b == std::string::npos) return false;
    int depth = 0;
    bool inStr = false;
    bool esc = false;
    for (size_t i = b; i < s.size(); ++i) {
        const char c = s[i];
        if (inStr) {
            if (esc) { esc = false; continue; }
            if (c == '\\') { esc = true; continue; }
            if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == open) ++depth;
        else if (c == close) {
            --depth;
            if (depth == 0) { outBegin = b; outEnd = i; return true; }
        }
    }
    return false;
}

}  // namespace

ParsedReply ParseReply(const std::string& replyText) {
    ParsedReply out;
    std::string content;
    bool incomplete = false;
    std::string parseError;

    // ★★ **裸 JSON 也算工具调用**（2026-10-02 修；这是一次真实回归）。
    //
    //   起因：提示词已统一成"**只输出这一种 JSON**"（紧凑模式那段模板），而本函数原来
    //   **只从 `<<<QST_TOOL_CALLS>>>` 块里**取工具调用 ⇒ 块外的一律当正文
    //   ⇒ **AI 助手 / 对话路径把 JSON 当文本显示**（用户实测报障：
    //     "AI 助手的操作路径怎么也改坏了，没办法直接操作了"，回复里只剩一段 JSON）。
    //   ⇒ 没有块时，把"看起来像工具调用的那段 JSON"**包进块里**，复用下面**同一套**
    //     解析与校验（不另写一份判据 —— 两份必然漂移）。
    std::string text = replyText;
    if (text.find(kToolCallsBegin) == std::string::npos) {
        size_t jb = 0, je = 0;
        const char openCh = text.find('[') != std::string::npos ? '[' : '{';
        const char closeCh = (openCh == '[') ? ']' : '}';
        if (FindBalanced(text, 0, openCh, closeCh, jb, je)) {
            const std::string cand = text.substr(jb, je - jb + 1);
            const json probe = json::parse(cand, nullptr, false);
            bool looksLikeCalls = false;
            if (probe.is_array() && !probe.empty() && probe.front().is_object()) {
                const auto& f = probe.front();
                looksLikeCalls = f.contains("name") || f.contains("action") || f.contains("tool")
                    || (f.size() == 1 && f.begin().value().is_object());
            } else if (probe.is_object()) {
                looksLikeCalls = probe.contains("name") || probe.contains("action")
                    || probe.contains("tool");
            }
            if (looksLikeCalls) {
                text = text.substr(0, jb) + kToolCallsBegin + cand + kToolCallsEnd
                    + text.substr(je + 1);
            }
        }
    }

    const std::string begin = kToolCallsBegin;
    const std::string end = kToolCallsEnd;
    size_t pos = 0;
    int seq = 0;
    while (pos < replyText.size()) {
        const size_t b = text.find(begin, pos);
        if (b == std::string::npos) {
            content += text.substr(pos);
            break;
        }
        content += text.substr(pos, b - pos);
        const size_t e = text.find(end, b + begin.size());
        if (e == std::string::npos) {
            incomplete = true;
            break;
        }
        std::string inner = text.substr(b + begin.size(), e - b - begin.size());
        pos = e + end.size();
        inner = TrimAscii(inner);
        if (inner.rfind("```", 0) == 0) {
            const size_t nl = inner.find('\n');
            if (nl != std::string::npos) inner = inner.substr(nl + 1);
            const size_t fence = inner.rfind("```");
            if (fence != std::string::npos) inner = inner.substr(0, fence);
            inner = TrimAscii(inner);
        }
        size_t jb = 0, je = 0;
        std::string payload;
        const size_t firstBrace = inner.find('{');
        if (FindBalanced(inner, 0, '[', ']', jb, je)
            && (firstBrace == std::string::npos || jb < firstBrace)) {
            payload = inner.substr(jb, je - jb + 1);
        } else if (FindBalanced(inner, 0, '{', '}', jb, je)) {
            payload = inner.substr(jb, je - jb + 1);
        } else {
            payload = inner;
        }
        json parsed = json::parse(payload, nullptr, false);
        if (parsed.is_discarded()) {
            if (parseError.empty()) parseError = "tool_calls JSON 解析失败";
            continue;
        }
        json arr = json::array();
        if (parsed.is_array()) arr = parsed;
        else if (parsed.is_object() && parsed.contains("tool_calls") && parsed["tool_calls"].is_array())
            arr = parsed["tool_calls"];
        else if (parsed.is_object()) arr.push_back(parsed);

        for (const auto& c : arr) {
            if (!c.is_object()) continue;
            // ⚠⚠ name 与 arguments 必须**各自独立**地解析：
            //   实测踩到过 —— 模型给出 `{"name":"mouseClick","function":{"arguments":"{...}"}}`
            //   时，若「name 非空就整段跳过 function」，嵌套里的 arguments 会被丢掉，
            //   然后被静默替换成 `{}` ⇒ **用默认参数执行动作**（在错的位置点一下）。
            //   这是本层最坏的一类失败，所以两条路径各自取、谁也不许覆盖谁。
            const json* fn = nullptr;
            if (c.contains("function") && c["function"].is_object()) fn = &c["function"];

            std::string name = c.value("name", "");
            if (name.empty() && fn) name = fn->value("name", "");
            // ★ 我们的**统一模板**用 `action` 当工具名（`{"action":"工具名",…}`）；
            //   单键形状 `{"mouseClick":{…}}` 则取那个唯一的键名。
            if (name.empty()) name = c.value("action", "");
            if (name.empty()) name = c.value("tool", "");
            if (name.empty() && c.size() == 1 && c.begin().value().is_object())
                name = c.begin().key();
            if (name.empty()) {
                if (parseError.empty()) parseError = "工具调用缺少 name";
                continue;
            }

            json args;
            bool hasArgs = false;
            if (c.contains("arguments")) {
                args = c["arguments"];
                hasArgs = true;
            } else if (fn && fn->contains("arguments")) {
                args = (*fn)["arguments"];
                hasArgs = true;
            }
            if (!hasArgs) {
                // ★ **扁平参数 / 单键形状**（2026-10-02，配合统一模板）：
                //   · 统一模板把参数**平铺在条目顶层**：`{"action":"mouseClick","x":1,"y":2}`
                //   · 单键·值=对象：`{"mouseClick":{"x":1,"y":2}}` ⇒ 那个对象就是参数
                //   · 单键·值=字符串：`{"computer":"screenshot"}` ⇒ 那个字符串是 action
                //   ⚠ 若这里沿用"没有 arguments 就当空对象"，工具会**用默认参数执行**
                //     —— 本文件自己的注释就说了那是最坏的静默失败（在错的位置点一下）。
                json flat = json::object();
                if (c.size() == 1 && c.begin().value().is_object()) {
                    flat = c.begin().value();
                } else if (c.size() == 1 && c.begin().value().is_string()) {
                    flat["action"] = c.begin().value();
                } else {
                    for (auto it = c.begin(); it != c.end(); ++it) {
                        const std::string& k = it.key();
                        if (k == "name" || k == "action" || k == "tool" || k == "function"
                            || k == "arguments" || k == "type" || k == "id") {
                            continue;
                        }
                        flat[k] = it.value();
                    }
                }
                if (!flat.empty()) {
                    args = flat;
                    hasArgs = true;
                }
            }
            if (!hasArgs) {
                // 没有 arguments 字段：按 OpenAI 的语义当空对象（工具自己的参数校验会兜住），
                // 但要**留痕**——「模型忘了写参数」和「参数就是空」在排障时含义完全不同。
                args = json::object();
                if (parseError.empty()) parseError = "工具 " + name + " 未给出 arguments（按空对象处理）";
            }
            std::string argsStr;
            if (args.is_string()) {
                const std::string raw = args.get<std::string>();
                const json check = json::parse(raw, nullptr, false);
                if (check.is_discarded() || !check.is_object()) {
                    // ⚠ 参数不是合法对象 ⇒ **丢弃这一次调用**而不是拿默认值硬跑：
                    //   用默认参数执行动作 = 在错误的位置点一下，属于最坏的静默失败。
                    if (parseError.empty()) parseError = "工具 " + name + " 的 arguments 不是合法 JSON 对象";
                    continue;
                }
                argsStr = qst::jsonutil::DumpUtf8Safe(check);
            } else if (args.is_object()) {
                argsStr = qst::jsonutil::DumpUtf8Safe(args);
            } else if (args.is_null()) {
                argsStr = "{}";
            } else {
                if (parseError.empty()) parseError = "工具 " + name + " 的 arguments 类型不支持";
                continue;
            }
            ++seq;
            json call;
            call["id"] = "call_webai_" + std::to_string(seq);
            call["type"] = "function";
            call["function"]["name"] = name;
            call["function"]["arguments"] = argsStr;
            out.toolCalls.push_back(std::move(call));
        }
    }

    out.content = TrimAscii(content);
    out.incomplete = incomplete;
    out.parseError = parseError;
    return out;
}

namespace {

long long NowSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

json BuildUsage(const CompletionMeta& meta, const ParsedReply& reply) {
    int completion = meta.completionTokens;
    if (completion <= 0) {
        completion = EstimateTokens(reply.content);
        for (const auto& c : reply.toolCalls) {
            if (c.contains("function")) completion += EstimateTokens(c["function"].dump());
        }
    }
    json usage;
    usage["prompt_tokens"] = meta.promptTokens;
    usage["completion_tokens"] = completion;
    usage["total_tokens"] = meta.promptTokens + completion;
    return usage;
}

json BuildWebAiDiag(const CompletionMeta& meta, const ParsedReply& reply) {
    json d;
    d["provider"] = meta.provider;
    d["elapsed_ms"] = meta.elapsedMs;
    d["reply_truncated"] = meta.truncated;
    d["tool_calls"] = static_cast<int>(reply.toolCalls.size());
    if (reply.incomplete) d["reply_incomplete"] = true;
    if (!reply.parseError.empty()) d["parse_error"] = reply.parseError;
    if (!meta.note.empty()) d["note"] = meta.note;
    return d;
}

}  // namespace

std::string BuildCompletionJson(const std::string& id, const std::string& model,
                                const ParsedReply& reply, const CompletionMeta& meta) {
    const bool hasTools = reply.toolCalls.is_array() && !reply.toolCalls.empty();
    json msg;
    msg["role"] = "assistant";
    // ⚠ 有工具调用时正文必须是 null 而不是 ""（与官方 API 一致；本仓消费端也按
    //   `content.empty()` 判「只有工具调用」）。
    if (hasTools && reply.content.empty()) msg["content"] = nullptr;
    else msg["content"] = reply.content;
    if (hasTools) msg["tool_calls"] = reply.toolCalls;

    json choice;
    choice["index"] = 0;
    choice["message"] = msg;
    choice["finish_reason"] = hasTools ? "tool_calls" : "stop";

    json body;
    body["id"] = id;
    body["object"] = "chat.completion";
    body["created"] = NowSeconds();
    body["model"] = model;
    body["choices"] = json::array({choice});
    body["usage"] = BuildUsage(meta, reply);
    body["x_web_ai"] = BuildWebAiDiag(meta, reply);
    return qst::jsonutil::DumpUtf8Safe(body);
}

std::string BuildCompletionSse(const std::string& id, const std::string& model,
                               const ParsedReply& reply, const CompletionMeta& meta) {
    const bool hasTools = reply.toolCalls.is_array() && !reply.toolCalls.empty();
    std::string out;
    auto chunk = [&](const json& delta, const json& finishReason) {
        json choice;
        choice["index"] = 0;
        choice["delta"] = delta;
        choice["finish_reason"] = finishReason;
        json c;
        c["id"] = id;
        c["object"] = "chat.completion.chunk";
        c["created"] = NowSeconds();
        c["model"] = model;
        c["choices"] = json::array({choice});
        // 诊断字段：消费端（agent_core）不读未知字段 ⇒ 安全。
        // 有它才能在**流式**路径上也回答「这次回答来自哪个站点、等了多久、有没有截断」。
        c["x_web_ai"] = BuildWebAiDiag(meta, reply);
        out += "data: ";
        out += qst::jsonutil::DumpUtf8Safe(c);
        out += "\n\n";
    };

    if (!reply.content.empty()) {
        json d;
        d["role"] = "assistant";
        d["content"] = reply.content;
        chunk(d, nullptr);
    }
    if (hasTools) {
        json d;
        d["role"] = "assistant";
        d["content"] = nullptr;
        json tcs = json::array();
        int idx = 0;
        for (const auto& c : reply.toolCalls) {
            json tc;
            // `index` 是消费端的分组键（agent_core.cpp:1131-1134）
            tc["index"] = idx++;
            tc["id"] = c.value("id", "");
            tc["type"] = "function";
            tc["function"]["name"] = c["function"].value("name", "");
            // ⚠ `arguments` 必须是**字符串形式的合法 JSON**：消费端硬判据
            //   `StreamToolCallArgsComplete` 会 json::parse 它（agent_core.cpp:1064-1074）。
            tc["function"]["arguments"] = c["function"].value("arguments", "{}");
            tcs.push_back(std::move(tc));
        }
        d["tool_calls"] = tcs;
        chunk(d, nullptr);
    }
    if (reply.content.empty() && !hasTools) {
        // 空回答也要给一个分片：既让消费端能收尾，也让「空」这件事**有痕迹**
        json d;
        d["role"] = "assistant";
        d["content"] = "";
        chunk(d, nullptr);
    }
    // 收尾分片：finish_reason 必须是字符串或 null（消费端在 try 块外 .get<std::string>()）
    chunk(json::object(), hasTools ? "tool_calls" : "stop");
    out += "data: [DONE]\n\n";
    return out;
}

std::string MakeCompletionId() {
    static std::atomic<unsigned> seq{0};
    const unsigned n = seq.fetch_add(1) + 1;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "chatcmpl-webai-%lld-%u",
        static_cast<long long>(NowSeconds()), n);
    return buf;
}

namespace {

/// 类型不符时记一笔（写进 mismatchOut），供调用方打日志
void NoteMismatch(const json& v, const char* key, std::string* out) {
    if (!out) return;
    *out = std::string(key) + " 期望 ";
    // 只说"实为什么类型"，不说"期望什么"（期望由调用方自己知道）
    *out += "，实为 " + std::string(v.type_name());
}

}  // namespace

std::string JsonStr(const json& o, const char* key, const std::string& dflt,
                    std::string* mismatchOut) {
    if (mismatchOut) mismatchOut->clear();
    if (!o.is_object() || !key) return dflt;
    const auto it = o.find(key);
    if (it == o.end()) return dflt;
    if (it->is_string()) return it->get<std::string>();
    NoteMismatch(*it, key, mismatchOut);
    return dflt;
}

bool JsonBool(const json& o, const char* key, bool dflt, std::string* mismatchOut) {
    if (mismatchOut) mismatchOut->clear();
    if (!o.is_object() || !key) return dflt;
    const auto it = o.find(key);
    if (it == o.end()) return dflt;
    if (it->is_boolean()) return it->get<bool>();
    NoteMismatch(*it, key, mismatchOut);
    return dflt;
}

int JsonInt(const json& o, const char* key, int dflt, std::string* mismatchOut) {
    if (mismatchOut) mismatchOut->clear();
    if (!o.is_object() || !key) return dflt;
    const auto it = o.find(key);
    if (it == o.end()) return dflt;
    if (it->is_number_integer()) return it->get<int>();
    // 数字但是浮点：容忍（取整），不算类型不符
    if (it->is_number_float()) return static_cast<int>(it->get<double>());
    NoteMismatch(*it, key, mismatchOut);
    return dflt;
}

std::string ProviderFromModelName(const std::string& modelName) {
    const std::string s = ToLowerAscii(modelName);
    if (s.empty()) return {};
    // ⚠ 顺序有意义：`doubao` 那条认 `seed`，而「qwen」里没有这些子串，互不干扰。
    // ⚠ 加站点必须**同时**改 `web_ai_config_parse.cpp` 的 `BuiltinWebAiProviderIds()`
    //   （否则设置页里看不到那个模型）与 `extension/edge/web_ai_providers.json`。
    if (Contains(s, "doubao") || Contains(s, "豆包") || Contains(s, "seed")) return "doubao";
    if (Contains(s, "deepseek") || Contains(s, "深度求索")) return "deepseek";
    if (Contains(s, "yuanbao") || Contains(s, "元宝") || Contains(s, "hunyuan")) return "yuanbao";
    if (Contains(s, "qwen") || Contains(s, "千问") || Contains(s, "通义")
        || Contains(s, "tongyi")) {
        return "qwen";
    }
    return {};
}

namespace {

/// 取 URL 的 authority 段（`http://host:port/path` → `host:port`）
std::string UrlAuthority(const std::string& url) {
    const size_t scheme = url.find("://");
    size_t b = (scheme == std::string::npos) ? 0 : scheme + 3;
    const size_t slash = url.find('/', b);
    std::string auth = url.substr(b, slash == std::string::npos ? std::string::npos : slash - b);
    const size_t at = auth.rfind('@');  // 去掉 user:pass@
    if (at != std::string::npos) auth = auth.substr(at + 1);
    return auth;
}

std::string UrlPath(const std::string& url) {
    const size_t scheme = url.find("://");
    size_t b = (scheme == std::string::npos) ? 0 : scheme + 3;
    const size_t slash = url.find('/', b);
    return slash == std::string::npos ? std::string{} : url.substr(slash);
}

std::string UrlScheme(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    return ToLowerAscii(url.substr(0, scheme));
}

}  // namespace

bool IsWebAiApiUrl(const std::string& utf8Url) {
    const std::string scheme = UrlScheme(utf8Url);
    if (scheme != "http" && scheme != "https") return false;
    std::string auth = ToLowerAscii(UrlAuthority(utf8Url));
    std::string host = auth;
    const size_t colon = auth.rfind(':');
    if (colon != std::string::npos && auth.find(']') == std::string::npos) {
        host = auth.substr(0, colon);
    }
    const bool loopback = (host == "127.0.0.1" || host == "localhost" || host == "[::1]"
        || host == "::1" || host.rfind("127.", 0) == 0);
    if (!loopback) return false;
    const std::string path = UrlPath(utf8Url);
    return path.find("/v1/chat/completions") != std::string::npos;
}

int PortFromUrl(const std::string& utf8Url) {
    const std::string auth = UrlAuthority(utf8Url);
    const size_t colon = auth.rfind(':');
    if (colon == std::string::npos) return 0;
    const std::string p = auth.substr(colon + 1);
    if (p.empty()) return 0;
    for (char c : p) {
        if (c < '0' || c > '9') return 0;
    }
    const int v = std::atoi(p.c_str());
    return (v > 0 && v < 65536) ? v : 0;
}

std::string CanonicalWebAiUrl(int port) {
    // ★ **端口 0 一律拒绝**（2026-10-02 真机：`LocalEndpointUrl()` 曾在桥未绑定时产出
    //   `http://127.0.0.1:0/...`，被档案固化后每一轮都连不上）。返回空串让调用方
    //   自己决定怎么办（`ApplyProfileOverride` 会因此**不改**档案，见那里的注释）。
    if (port <= 0 || port > 65535) return std::string();
    return "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";
}

int EstimateTokens(const std::string& utf8Text) {
    if (utf8Text.empty()) return 0;
    // 粗略：CJK 约 1 字/token，ASCII 约 4 字/token。按字节数取中间值即可，
    // 只用于 usage 展示与日志，不参与任何判断。
    return (std::max)(1, static_cast<int>(utf8Text.size() / 3));
}

}  // namespace quickscript::webai
