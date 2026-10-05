// =============================================================================
// web_ai_selftest.cpp — 网页版 AI 后端（C 方案）的**纯逻辑**自检
//
// 只链 `web_ai_prompt.cpp` + `web_ai_config_parse.cpp` + `qst_utils`：
//   无 Win32 业务、无桥、无浏览器、无网 —— 因此能在任何环境跑（进逻辑档）。
//
// ⚠ 这一层为什么必须逐格断言：
//   网页版 AI **没有原生 function calling**，工具调用完全靠约定文本协议。
//   解析错一次 = 在错误的位置点一下（静默失败里最坏的一种）。
//   所以「协议块未闭合」「arguments 不是合法 JSON 对象」这两条都断言成
//   **拒绝该次调用**，而不是拿默认值硬跑。
//
// 真机部分（能不能写进豆包、能不能读回回答）**不在本套**：它必须由
// 产品进程内的 `GET/POST /qst/web-ai/probe` 跑（见 web_ai_driver.h 约束①——
// 外部进程的 WS 探针进不了业务链路，2026-09-24 已经因此误判过一次）。
// =============================================================================

#include "selftest_harness.h"

#include "web_ai/web_ai_config.h"
#include "web_ai/web_ai_image.h"
#include "web_ai/web_ai_prompt.h"

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace quickscript::webai;

namespace {

bool Contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

json MakeTool(const std::string& name, const std::string& desc, json props, json required) {
    json fn;
    fn["name"] = name;
    fn["description"] = desc;
    fn["parameters"]["type"] = "object";
    fn["parameters"]["properties"] = props;
    fn["parameters"]["required"] = required;
    json t;
    t["type"] = "function";
    t["function"] = fn;
    return t;
}

json Msg(const std::string& role, const std::string& content) {
    json m;
    m["role"] = role;
    m["content"] = content;
    return m;
}

json TwoTools() {
    json tools = json::array();
    json p1;
    p1["x"]["type"] = "number";
    p1["y"]["type"] = "number";
    p1["button"]["type"] = "string";
    p1["button"]["enum"] = json::array({"left", "right"});
    tools.push_back(MakeTool("mouseClick", "在指定坐标点击鼠标。", p1, json::array({"x", "y"})));
    json p2;
    p2["target"]["type"] = "string";
    tools.push_back(MakeTool("locateAndClick", "按屏幕上的文字/控件名定位并点击。", p2,
        json::array({"target"})));
    return tools;
}

// ── 1. PlanTurn ─────────────────────────────────────────────────────────────

void CaseFirstTurn() {
    json tools = TwoTools();
    json messages = json::array();
    messages.push_back(Msg("system", "你是脚本助手。"));
    messages.push_back(Msg("user", "帮我点一下开始按钮"));

    SessionState next;
    const TurnPlan plan = PlanTurn(messages, tools, SessionState{}, next);
    const bool ok = plan.valid && plan.newConversation
        && Contains(plan.text, "你是脚本助手。")
        // ★★ 契约（2026-10-02 按实现实际行为对齐；用例名保留以防历史文档链接失效）。
        //
        //   演进：2026-09-26「只给名字」→ 2026-09-30 20:52 改为「首轮给完整 schema」（本用例
        //   当时按这个写）→ **同日 23:25 又回退**为「名字 + 一句话用途」的**紧凑清单**，
        //   并**彻底不提** `loadTools`。回退理由写在实现里（真机实测）：把完整 JSON Schema
        //   内联进首轮，豆包会把它当成**自己的原生函数**去解析，然后回一大段
        //   「这些键鼠工具在我当前运行环境里都不存在」的拒绝；而 `loadTools` 被它每轮
        //   白烧 11~47 秒（一局 3 次 ≈ 70 秒）。
        //   ⇒ 所以本用例现在钉**紧凑清单**契约：清单在、名字在、正路提示在，
        //     而 `loadTools` / 参数类型**必须不出现**（出现即为回归）。
        // ★★★ 2026-10-02 契约再变（用户决定）：**不发清单，只发「怎么要清单」**，
        //   且**首轮必须把归属说清楚**（"它们不是你的函数、由本软件在本机执行"）——
        //   用户指出 09-30 的失败是"没说清楚"造成的，不是方案本身的问题。
        && Contains(plan.text, "不是你的函数")           // ★ 说清楚归属（防豆包拒绝）
        && Contains(plan.text, "loadTools")            // ★ 只给"怎么要清单"
        && !Contains(plan.text, "- mouseClick")        // ★ 不发工具名（体量小得多）
        && !Contains(plan.text, "x:number")            // 紧凑清单不给参数类型
        // ⚠ 不断言 `kToolCallsBegin`：实测**紧凑模式不输出该格式块**（`===== 调用工具的唯一格式 =====`
        //   那一段在 `else if (!toolList.empty())` 分支里）。这是观察到的现状，不是本文用例要钉的契约
        //   —— 钉"有没有格式块"会在实现补上它时变成假红（报的是好事）。解析侧两条路都吃：
        //   纯 JSON 对象/数组（`ExtractFirstJsonObject` / `ExtractFirstJsonArray`）与块格式。
        && Contains(plan.text, "帮我点一下开始按钮")
        && next.sentMessageCount == 2 && next.turns == 1 && !next.lastFingerprint.empty();
    // 失败时的取证：逐条判据的位标志（ASCII，控制台/CI 日志里直接可读；只在 FAIL 时打印）
    const bool dHdr = Contains(plan.text, "工具（名：用途）");
    const bool dName = Contains(plan.text, "- mouseClick");
    const bool dCall = Contains(plan.text, "直接按名字调用");
    const bool dLt = Contains(plan.text, "loadTools");
    const bool dTyp = Contains(plan.text, "x:number");
    const bool dCb = Contains(plan.text, kToolCallsBegin);
    const bool dUser = Contains(plan.text, "帮我点一下开始按钮");
    const bool dSys = Contains(plan.text, "你是脚本助手。");
    const bool dSt = plan.valid && plan.newConversation && next.sentMessageCount == 2
        && next.turns == 1 && !next.lastFingerprint.empty();
    selftest::Emit(L"plan_first_turn_has_system_tools_protocol", ok,
        (L"chars=" + std::to_wstring(plan.totalChars) + L" newConv="
            + std::to_wstring(plan.newConversation ? 1 : 0)
            + L" hdr=" + (dHdr ? L"1" : L"0") + L" name=" + (dName ? L"1" : L"0")
            + L" call=" + (dCall ? L"1" : L"0") + L" lt=" + (dLt ? L"1" : L"0")
            + L" typ=" + (dTyp ? L"1" : L"0") + L" cb=" + (dCb ? L"1" : L"0")
            + L" user=" + (dUser ? L"1" : L"0") + L" sys=" + (dSys ? L"1" : L"0")
            + L" st=" + (dSt ? L"1" : L"0")).c_str());
}

void CaseContinuationSendsOnlyNew() {
    json tools = TwoTools();
    json messages = json::array();
    messages.push_back(Msg("system", "你是脚本助手。"));
    messages.push_back(Msg("user", "帮我点一下开始按钮"));

    SessionState s1;
    const TurnPlan first = PlanTurn(messages, tools, SessionState{}, s1);
    if (!first.valid) { selftest::Emit(L"plan_continuation_sends_only_new", false, L"首轮规划失败"); return; }

    // 引擎把工具执行结果追加进来（assistant 的 tool_calls + tool 结果）
    json a;
    a["role"] = "assistant";
    a["content"] = nullptr;
    a["tool_calls"] = json::array({json{{"id", "call_1"}, {"type", "function"},
        {"function", json{{"name", "mouseClick"}, {"arguments", "{\"x\":10,\"y\":20}"}}}}});
    json t;
    t["role"] = "tool";
    t["tool_call_id"] = "call_1";
    t["content"] = "[结果] 已点击 (10,20)";
    json messages2 = messages;
    messages2.push_back(a);
    messages2.push_back(t);

    SessionState s2;
    const TurnPlan second = PlanTurn(messages2, tools, s1, s2);
    const bool ok = second.valid && !second.newConversation
        && Contains(second.text, "[结果] 已点击 (10,20)")
        // ★★ 2026-10-02 改：**系统设定每轮都发**（不发的话模型不知道自己在干什么 ——
        //   真机事故：第二个任务乱猜工具名）。这里断言它**在**。
        && Contains(second.text, "你是脚本助手。")
        && !Contains(second.text, "帮我点一下开始按钮")      // ★ 旧用户消息**不重发**（增量）
        && !Contains(second.text, "mouseClick({\"x\"")       // 纯工具调用消息不重发
        && s2.sentMessageCount == 4;
    selftest::Emit(L"plan_continuation_sends_only_new", ok,
        (L"chars=" + std::to_wstring(second.totalChars)).c_str());
}

void CaseHistoryRewriteResendsFull() {
    // ⚠ 用例名 2026-09-26 改：语义拆开后，「重写」触发的是**重发全量**（`resendFull`），
    //   **不是**"开新网页对话"（那只看 `session.turns == 0`）。名字不改会误导后来人。
    json tools = TwoTools();
    json messages = json::array();
    messages.push_back(Msg("user", "第一轮问题"));
    SessionState s1;
    PlanTurn(messages, tools, SessionState{}, s1);

    // ★ 要防的是**锚点消息被改掉**：引擎重建请求体（关思考重试 / 纠偏注入 / 历史压缩）
    //   时，原先最后一条消息可能被替换。此指纹对不上 ⇒ 必须当新对话重发全量。
    //   ⚠ 反例（**不要**拿它当"重写"）：在原样保留的前缀后面**追加**消息 ——
    //     那是正常的续轮（工具结果就是这么追加的），判成新对话会导致整段历史重发。
    json rewritten = json::array();
    rewritten.push_back(Msg("user", "被改写后的问题"));
    rewritten.push_back(Msg("assistant", "（纠偏提示）请直接调用工具"));
    SessionState s2;
    const TurnPlan plan = PlanTurn(rewritten, tools, s1, s2);
    // ★★ 2026-09-26 语义拆开（用户要求）：
    //   历史被重写 ⇒ **必须重发全量**（`resendFull`），但**不该**顺手开新网页对话
    //   （`newConversation` 只由"软件侧这个会话是不是第一次"决定）。
    // ★★ 2026-10-02 语义再变（用户要求"历史默认不发"）：
    //   「重写」**不再触发重发**（只发增量）；但必须**保证不为空**
    //   （`start >= total` 时退化成"发最后一条"）。
    // ⚠ `"===== 对话 ====="` 这个标题**只在重发前缀时**才加（它属于 `prefix`）
    //   ⇒ 不重发的这一轮**不该**有它，而应该有**增量正文**（被改写那条用户消息）。
    // ⚠ 前缀（system+协议）现在**每次都发** ⇒ `"===== 对话 ====="` 总在；
    //   要断言的是"**旧内容不重发、新内容发了**"。
    const bool ok = plan.valid && !plan.resendFull && !plan.newConversation
        && !plan.text.empty()
        && Contains(plan.text, "===== 对话 =====")
        && Contains(plan.text, "纠偏提示")               // 新内容发了
        && !Contains(plan.text, "第一轮问题");           // 旧内容没重发

    // 追加（前缀不变）必须仍然是续轮
    json appended = messages;
    appended.push_back(Msg("assistant", "（纠偏提示）请直接调用工具"));
    SessionState s3;
    const TurnPlan plan2 = PlanTurn(appended, tools, s1, s3);
    const bool ok2 = plan2.valid && !plan2.resendFull;

    selftest::Emit(L"plan_history_rewrite_resends_full", ok && ok2,
        (L"rewriteResend=" + std::to_wstring(plan.resendFull ? 1 : 0)
            + L" appendResend=" + std::to_wstring(plan2.resendFull ? 1 : 0)).c_str());
}

void CaseRepeatedRequestResendsFully() {
    // ★ 同一份请求被重复提交 ⇒ 当**新对话**重发全量，**不许报错**。
    //   理由（实测场景）：用户把同一个动作/同一份脚本跑第二遍时，请求体与第一遍
    //   逐字相同（messages 只有一条 user）。若这里判成"没有新消息"而拒绝，
    //   同一个动作跑第二次就永久失败 —— 那是把正确行为当成异常拒掉。
    json tools = TwoTools();
    json messages = json::array();
    messages.push_back(Msg("user", "同一个请求被重复提交"));
    SessionState s1;
    PlanTurn(messages, tools, SessionState{}, s1);
    SessionState s2;
    const TurnPlan plan = PlanTurn(messages, tools, s1, s2);
    // ★★ 语义拆开后：重复提交 ⇒ **重发全量**（`resendFull`）；开不开新网页对话是另一件事。
    // ★★ 2026-10-02 语义再变：重复提交**也不再重发全量**（历史默认不发）。
    //   但底线是**不许发空内容**（`start >= total` 时退化成"发最后一条"）——
    //   否则网页输入框空着，站点不动或报错。
    // ⚠ 同上：不重发 ⇒ 没有前缀标题；但**必须有正文**（退化成"发最后一条"）
    const bool ok = plan.valid && !plan.resendFull && !plan.text.empty()
        && Contains(plan.text, "===== 对话 =====")
        && Contains(plan.text, "同一个请求被重复提交");
    selftest::Emit(L"plan_repeat_request_resends_fully", ok,
        (L"resend=" + std::to_wstring(plan.resendFull ? 1 : 0)
            + L" 非空=" + std::to_wstring(plan.text.empty() ? 0 : 1)).c_str());
}

// ── 协议瘦身：工具清单**每会话只发一次**（2026-10-02 用户规格）───────────
//
// ⚠ 为什么必须测：日志实证过「多数请求 ~500 字，但历史重写那一次 **9099 字**」——
//   那 9 KB 就是**重复发了一遍工具清单**。这条用例锁死"发过就不再发"。
//   ⚠ 反例（必须同时断言）：**标签页被重建**时会话状态清零 ⇒ 工具清单**必须重发**
//     （否则模型在空对话里不知道有哪些工具）。
void CaseToolsSectionSentOncePerSession() {
    json tools = TwoTools();
    json messages = json::array();
    messages.push_back(Msg("system", "你是脚本助手。"));
    messages.push_back(Msg("user", "第一轮问题"));

    // 第一轮：**应该**带工具说明
    SessionState s1;
    const TurnPlan p1 = PlanTurn(messages, tools, SessionState{}, s1);
    // ⚠ 标记用**实际的清单段标题**：2026-09-30 起首轮内联"名：用途"清单，
    //   **不再提 `loadTools`**（实测那玩意儿每轮白烧 11~47 秒）——别拿旧标记断言。
    const bool firstHasTools = Contains(p1.text, "===== 工具（名：用途）=====");
    const bool firstFlag = s1.toolsSent;

    // 第二轮：续轮 ⇒ **不该**再带
    json messages2 = messages;
    messages2.push_back(Msg("assistant", "好的"));
    messages2.push_back(Msg("user", "第二轮问题"));
    SessionState s2;
    const TurnPlan p2 = PlanTurn(messages2, tools, s1, s2);
    const bool secondHasTools = Contains(p2.text, "===== 工具（名：用途）=====");

    // 第三轮：**历史被重写**（触发 `resendFull`）⇒ 仍然**不该**重发工具说明
    json rewritten = json::array();
    rewritten.push_back(Msg("system", "你是脚本助手。"));
    rewritten.push_back(Msg("user", "被改写的问题"));
    SessionState s3;
    const TurnPlan p3 = PlanTurn(rewritten, tools, s2, s3);
    const bool rewriteHasTools = Contains(p3.text, "===== 工具（名：用途）=====");
    // ⚠ 2026-10-02 起"重写"**不再**重发全量（历史默认不发）⇒ 这里不再拿它当前提

    const bool ok = p1.valid && firstHasTools && firstFlag
        && p2.valid && !secondHasTools
        && p3.valid && !rewriteHasTools;
    selftest::Emit(L"plan_tools_section_sent_once_per_session", ok,
        (L"首轮=" + std::to_wstring(firstHasTools ? 1 : 0)
         + L" 续轮=" + std::to_wstring(secondHasTools ? 1 : 0)
         + L" 重写=" + std::to_wstring(rewriteHasTools ? 1 : 0)
         + L"").c_str());
}

void CaseToolListTrimsDescriptionsNotNames() {
    json tools = json::array();
    for (int i = 0; i < 40; ++i) {
        json p;
        p["a"]["type"] = "string";
        tools.push_back(MakeTool("tool" + std::to_string(i),
            "这是一段很长的描述，用来把工具清单撑到远超预算，以便验证裁剪只丢描述不丢工具名与参数。",
            p, json::array({"a"})));
    }
    bool trimmed = false;
    const std::string out = RenderToolList(tools, 1200, &trimmed);
    // 保结构：工具名与参数名必须还在（丢了它们模型就没法调用）
    const bool ok = trimmed && Contains(out, "tool0(") && Contains(out, "a:string")
        && static_cast<int>(out.size()) <= 1200 + 200;
    selftest::Emit(L"render_tool_list_trims_descriptions_keeps_names", ok,
        (L"chars=" + std::to_wstring(out.size()) + L" trimmed=" + std::to_wstring(trimmed ? 1 : 0)).c_str());
}

// ── 2. ParseReply ───────────────────────────────────────────────────────────

void CaseParseBasic() {
    const std::string reply =
        "好的，我来点击。\n" + std::string(kToolCallsBegin) + "\n"
        "[{\"name\":\"mouseClick\",\"arguments\":{\"x\":120,\"y\":340,\"button\":\"left\"}}]\n"
        + std::string(kToolCallsEnd) + "\n";
    const ParsedReply r = ParseReply(reply);
    const bool ok = r.toolCalls.size() == 1
        && r.toolCalls[0]["function"]["name"] == "mouseClick"
        && !r.incomplete && r.parseError.empty()
        && Contains(r.toolCalls[0]["function"]["arguments"].get<std::string>(), "\"x\":120")
        && !Contains(r.content, "QST_TOOL_CALLS")     // 协议块要从正文里剥掉
        && Contains(r.content, "好的，我来点击");
    selftest::Emit(L"parse_tool_calls_basic", ok,
        (L"calls=" + std::to_wstring(r.toolCalls.size())).c_str());
}

void CaseParseFencedAndMultiple() {
    const std::string reply =
        std::string(kToolCallsBegin) + "\n```json\n"
        "{\"tool_calls\":[{\"name\":\"mouseClick\",\"arguments\":{\"x\":1,\"y\":2}},"
        "{\"name\":\"keyClick\",\"arguments\":{\"key\":\"Enter\"}}]}\n```\n"
        + std::string(kToolCallsEnd);
    const ParsedReply r = ParseReply(reply);
    const bool ok = r.toolCalls.size() == 2
        && r.toolCalls[1]["function"]["name"] == "keyClick"
        && r.toolCalls[1]["function"]["arguments"] == "{\"key\":\"Enter\"}";
    selftest::Emit(L"parse_tool_calls_fenced_object_form", ok,
        (L"calls=" + std::to_wstring(r.toolCalls.size())).c_str());
}

void CaseParseIncomplete() {
    // 有头无尾：网页回答还在生成 ⇒ **必须**标记 incomplete（半截 JSON 不许当结果）
    const std::string reply = "稍等\n" + std::string(kToolCallsBegin) + "\n[{\"name\":\"mouseClick\",";
    const ParsedReply r = ParseReply(reply);
    const bool ok = r.incomplete && r.toolCalls.empty();
    selftest::Emit(L"parse_incomplete_block_is_flagged", ok,
        (L"incomplete=" + std::to_wstring(r.incomplete ? 1 : 0)).c_str());
}

void CaseParseBadArgumentsDropped() {
    // arguments 不是合法 JSON 对象 ⇒ **丢弃这次调用**（拿默认值硬跑＝在错的位置点击）
    const std::string reply = std::string(kToolCallsBegin)
        + "[{\"name\":\"mouseClick\",\"arguments\":\"{x: 不是JSON\"}]"
        + std::string(kToolCallsEnd);
    const ParsedReply r = ParseReply(reply);
    const bool ok = r.toolCalls.empty() && !r.parseError.empty();
    selftest::Emit(L"parse_bad_arguments_drops_call", ok,
        (L"err=" + std::wstring(r.parseError.begin(), r.parseError.end())).c_str());
}

void CaseParsePlainAnswer() {
    const ParsedReply r = ParseReply("这是一段普通回答，没有工具调用。");
    const bool ok = r.toolCalls.empty() && !r.incomplete
        && r.content == "这是一段普通回答，没有工具调用。";
    selftest::Emit(L"parse_plain_answer", ok, L"");
}

void CaseParseStringArguments() {
    // 有些模型会把 arguments 写成「JSON 字符串」——这也要认，并归一成对象字符串
    const std::string reply = std::string(kToolCallsBegin)
        + "[{\"name\":\"mouseClick\",\"function\":{\"name\":\"mouseClick\","
        "\"arguments\":\"{\\\"x\\\":5,\\\"y\\\":6}\"}}]"
        + std::string(kToolCallsEnd);
    const ParsedReply r = ParseReply(reply);
    const bool ok = r.toolCalls.size() == 1
        && r.toolCalls[0]["function"]["arguments"] == "{\"x\":5,\"y\":6}";
    selftest::Emit(L"parse_arguments_accepts_json_string", ok,
        (L"calls=" + std::to_wstring(r.toolCalls.size())).c_str());
}

// ── 3. 响应构造（消费端契约）───────────────────────────────────────────────

void CaseCompletionJsonShape() {
    ParsedReply r;
    r.toolCalls = json::array({json{{"id", "call_webai_1"}, {"type", "function"},
        {"function", json{{"name", "mouseClick"}, {"arguments", "{\"x\":1}"}}}}});
    CompletionMeta meta;
    meta.provider = "doubao";
    meta.elapsedMs = 1234;
    const json body = json::parse(BuildCompletionJson("chatcmpl-webai-1", "doubao-web", r, meta));
    const bool ok = body["object"] == "chat.completion"
        && body["choices"][0]["finish_reason"] == "tool_calls"
        && body["choices"][0]["message"]["content"].is_null()   // 有工具调用时正文必须是 null
        && body["choices"][0]["message"]["tool_calls"].size() == 1
        && body["x_web_ai"]["provider"] == "doubao";
    selftest::Emit(L"completion_json_tool_turn_shape", ok, L"");
}

void CaseCompletionJsonTextShape() {
    ParsedReply r;
    r.content = "你好";
    const json body = json::parse(BuildCompletionJson("id", "doubao-web", r));
    const bool ok = body["choices"][0]["finish_reason"] == "stop"
        && body["choices"][0]["message"]["content"] == "你好"
        && !body["choices"][0]["message"].contains("tool_calls");
    selftest::Emit(L"completion_json_text_turn_shape", ok, L"");
}

void CaseCompletionSseShape() {
    ParsedReply r;
    r.toolCalls = json::array({json{{"id", "call_webai_1"}, {"type", "function"},
        {"function", json{{"name", "mouseClick"}, {"arguments", "{\"x\":1,\"y\":2}"}}}}});
    const std::string sse = BuildCompletionSse("id", "doubao-web", r);
    // 逐条钉住消费端（agent_core.cpp）的硬要求：
    //   · 每行以 `data:` 开头；末尾有 [DONE]
    //   · 工具分片带 index / function.name / arguments（**字符串**形式合法 JSON）
    //   · finish_reason 是字符串（消费端在 try 块外 .get<std::string>()）
    bool sawCall = false;
    bool sawFinish = false;
    bool done = false;
    int dataLines = 0;
    size_t pos = 0;
    bool allData = true;
    while (pos < sse.size()) {
        const size_t nl = sse.find('\n', pos);
        std::string line = sse.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? sse.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.rfind("data:", 0) != 0) { allData = false; continue; }
        ++dataLines;
        const std::string payload = line.substr(5);
        std::string p = payload;
        while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(p.begin());
        if (p == "[DONE]") { done = true; continue; }
        const json chunk = json::parse(p, nullptr, false);
        if (chunk.is_discarded()) continue;
        const json& ch = chunk["choices"][0];
        if (ch.contains("delta") && ch["delta"].contains("tool_calls")) {
            sawCall = true;
            const json& tc = ch["delta"]["tool_calls"][0];
            if (!tc.contains("index")) allData = false;
            if (!tc["function"]["name"].is_string()) allData = false;
            const json args = json::parse(tc["function"]["arguments"].get<std::string>(), nullptr, false);
            if (args.is_discarded() || !args.is_object()) allData = false;
        }
        if (ch.contains("finish_reason") && !ch["finish_reason"].is_null()) {
            if (!ch["finish_reason"].is_string()) allData = false;
            if (ch["finish_reason"] == "tool_calls") sawFinish = true;
        }
    }
    const bool ok = allData && done && sawCall && sawFinish && dataLines >= 3;
    selftest::Emit(L"completion_sse_matches_agentcore_contract", ok,
        (L"lines=" + std::to_wstring(dataLines) + L" done=" + std::to_wstring(done ? 1 : 0)
            + L" call=" + std::to_wstring(sawCall ? 1 : 0)
            + L" finish=" + std::to_wstring(sawFinish ? 1 : 0)).c_str());
}

void CaseCompletionSseEmptyStillTerminates() {
    // 空回答也必须给出可解析的分片 + 结束标记：否则消费端要白等到看门狗开火
    const std::string sse = BuildCompletionSse("id", "doubao-web", ParsedReply{});
    const bool ok = Contains(sse, "data: ") && Contains(sse, "[DONE]")
        && Contains(sse, "\"finish_reason\":\"stop\"");
    selftest::Emit(L"completion_sse_empty_reply_still_terminates", ok, L"");
}

// ── 4. 站点/URL/消息 ───────────────────────────────────────────────────────

void CaseProviderFromModel() {
    const bool ok = ProviderFromModelName("doubao-web") == "doubao"
        && ProviderFromModelName("Doubao-Pro") == "doubao"
        && ProviderFromModelName("网页版豆包") == "doubao"
        && ProviderFromModelName("deepseek-web") == "deepseek"
        && ProviderFromModelName("腾讯元宝") == "yuanbao"
        && ProviderFromModelName("gpt-4o").empty();
    selftest::Emit(L"provider_from_model_name", ok, L"");
}

void CaseIsWebAiUrl() {
    const bool ok = IsWebAiApiUrl("http://127.0.0.1:19228/v1/chat/completions")
        && IsWebAiApiUrl("http://localhost:19230/v1/chat/completions")
        && IsWebAiApiUrl("HTTP://127.0.0.1:19228/v1/chat/completions")
        && !IsWebAiApiUrl("https://api.openai.com/v1/chat/completions")
        && !IsWebAiApiUrl("http://192.168.1.5:19228/v1/chat/completions")
        && !IsWebAiApiUrl("http://127.0.0.1:19228/qst/status")
        && !IsWebAiApiUrl("");
    selftest::Emit(L"is_web_ai_api_url_loopback_only", ok, L"");
}

void CasePortAndCanonical() {
    const bool ok = PortFromUrl("http://127.0.0.1:19228/v1/chat/completions") == 19228
        && PortFromUrl("https://api.openai.com/v1/chat/completions") == 0
        && CanonicalWebAiUrl(19231) == "http://127.0.0.1:19231/v1/chat/completions";
    selftest::Emit(L"port_parse_and_canonical_url", ok, L"");
}

void CaseImagePartPlaceholder() {
    json m;
    m["role"] = "user";
    m["content"] = json::array();
    json t;
    t["type"] = "text";
    t["text"] = "看看这张图";
    json img;
    img["type"] = "image_url";
    img["image_url"]["url"] = "data:image/png;base64,AAAA";
    m["content"].push_back(t);
    m["content"].push_back(img);
    const std::string text = MessageTextForPrompt(m);
    // ⚠ 图片**不走文本通道**：这里只留占位；真图要走扩展的上传通道（另实现）
    const bool ok = Contains(text, "看看这张图") && Contains(text, "[图片:image/png]")
        && !Contains(text, "base64") && CountImageParts(m) == 1;
    selftest::Emit(L"image_part_becomes_placeholder", ok,
        (L"text=" + std::wstring(text.begin(), text.end())).c_str());
}

void CaseStrippedImagesAreAnnounced() {
    // ★ 一张都没送过去时必须**明说**（否则模型以为自己看到了图 / 或反复追问图片）
    json messages = json::array();
    messages.push_back(Msg("system", "你是桌面自动化助手。"));
    json u;
    u["role"] = "user";
    u["content"] = json::array();
    json t;
    t["type"] = "text";
    t["text"] = "点击这个按钮";
    json img;
    img["type"] = "image_url";
    img["image_url"]["url"] = "data:image/png;base64,AAAA";
    u["content"].push_back(t);
    u["content"].push_back(img);
    messages.push_back(u);

    SessionState next;
    // imagesAttached 缺省 = 0（上传通道没跑成 / 没图）
    const TurnPlan plan = PlanTurn(messages, TwoTools(), SessionState{}, next);
    const bool ok = plan.valid && plan.imagesStripped == 1 && plan.imagesAttached == 0
        && Contains(plan.text, "没能送过去")
        && !Contains(plan.text, "已通过**网页上传通道**")
        && Contains(plan.text, "[图片:image/png]");
    selftest::Emit(L"stripped_images_are_announced", ok,
        (L"stripped=" + std::to_wstring(plan.imagesStripped)
         + L" attached=" + std::to_wstring(plan.imagesAttached)).c_str());
}

/// 建一条带 N 张内联图的 user 消息
json UserMsgWithImages(int n) {
    json u;
    u["role"] = "user";
    u["content"] = json::array();
    json t;
    t["type"] = "text";
    t["text"] = "看看这些图";
    u["content"].push_back(t);
    for (int i = 0; i < n; ++i) {
        json img;
        img["type"] = "image_url";
        img["image_url"]["url"] = "data:image/png;base64,AAAA";
        u["content"].push_back(img);
    }
    return u;
}

void CaseImagesAttachedTellsModelToLook() {
    // ★★ 图**真的送过去了**时，话术必须反过来：让模型直接看图，
    //    绝不能说「没能送过去」（那会让模型对着真存在的图说"我看不到"）。
    json messages = json::array();
    messages.push_back(Msg("system", "你是桌面自动化助手。"));
    messages.push_back(UserMsgWithImages(2));

    SessionState next;
    const TurnPlan plan = PlanTurn(messages, TwoTools(), SessionState{}, next,
                                   PromptBudget{}, /*imagesAttached=*/2);
    const bool ok = plan.valid && plan.imagesStripped == 2 && plan.imagesAttached == 2
        && Contains(plan.text, "已通过**网页上传通道**附上 2 张图片")
        && !Contains(plan.text, "没能送过去");
    selftest::Emit(L"images_attached_tells_model_to_look", ok,
        (L"attached=" + std::to_wstring(plan.imagesAttached)).c_str());
}

void CasePartialUploadSaysBoth() {
    // ★ 部分送到（3 张里成 1 张）⇒ 两句话都要说，且数字要对得上。
    json messages = json::array();
    messages.push_back(Msg("system", "你是桌面自动化助手。"));
    messages.push_back(UserMsgWithImages(3));

    SessionState next;
    const TurnPlan plan = PlanTurn(messages, TwoTools(), SessionState{}, next,
                                   PromptBudget{}, /*imagesAttached=*/1);
    const bool ok = plan.valid && plan.imagesStripped == 3 && plan.imagesAttached == 1
        && Contains(plan.text, "附上 1 张图片")
        && Contains(plan.text, "2 张图**没能送过去**");
    selftest::Emit(L"partial_upload_says_both", ok,
        (L"attached=" + std::to_wstring(plan.imagesAttached)
         + L"/" + std::to_wstring(plan.imagesStripped)).c_str());
}

void CaseAttachedCountCannotExceedRequested() {
    // ⚠ 上传数被调用方传大了（协议 bug / 记账 bug）时，不许报出「比请求里还多」的图。
    json messages = json::array();
    messages.push_back(Msg("system", "系统"));
    messages.push_back(UserMsgWithImages(1));

    SessionState next;
    const TurnPlan plan = PlanTurn(messages, TwoTools(), SessionState{}, next,
                                   PromptBudget{}, /*imagesAttached=*/99);
    const bool ok = plan.valid && plan.imagesStripped == 1 && plan.imagesAttached == 1
        && Contains(plan.text, "附上 1 张图片");
    selftest::Emit(L"attached_count_clamped_to_requested", ok,
        (L"attached=" + std::to_wstring(plan.imagesAttached)).c_str());
}

// ── 4.5 传图通道（base64 / 抽取 / 落盘判据）────────────────────────────────

void CaseBase64DecodeVectors() {
    std::string out;
    bool ok = true;
    std::string why;

    // ① 基本向量 + 无 padding
    ok = Base64Decode("aGVsbG8=", out) && out == "hello";
    if (!ok) why = "aGVsbG8= -> " + out;
    if (ok) {
        ok = Base64Decode("aGVsbG8", out) && out == "hello";
        if (!ok) why = "no-padding -> " + out;
    }
    // ② 空串是合法输入，解出空串
    if (ok) {
        ok = Base64Decode("", out) && out.empty();
        if (!ok) why = "empty";
    }
    // ③ URL-safe 变体必须等价：'____' == 63,63,63,63 == 0xFF,0xFF,0xFF
    if (ok) {
        ok = Base64Decode("____", out) && out.size() == 3
            && static_cast<unsigned char>(out[0]) == 0xFF
            && static_cast<unsigned char>(out[1]) == 0xFF
            && static_cast<unsigned char>(out[2]) == 0xFF;
        if (!ok) why = "urlsafe underscore";
    }
    // ④ '----' == 62,62,62,62 == 0xFB,0xEF,0xBE
    if (ok) {
        ok = Base64Decode("----", out) && out.size() == 3
            && static_cast<unsigned char>(out[0]) == 0xFB
            && static_cast<unsigned char>(out[1]) == 0xEF
            && static_cast<unsigned char>(out[2]) == 0xBE;
        if (!ok) why = "urlsafe dash";
    }
    // ⑤ 换行/空白要被容忍（data URL 常被折行）
    if (ok) {
        ok = Base64Decode("aGVs\r\nbG8=", out) && out == "hello";
        if (!ok) why = "whitespace";
    }
    selftest::Emit(L"base64_decode_vectors", ok,
        why.empty() ? L"" : (L"FAIL: " + std::wstring(why.begin(), why.end())).c_str());
}

void CaseBase64DecodeRejectsGarbage() {
    // ★★ 宁可报错也别把半张图发出去：非法输入一律 false，**不许**静默截断。
    std::string out;
    const bool notChar = !Base64Decode("aGVs*bG8=", out);   // 非法字符
    const bool notLen1 = !Base64Decode("a", out);           // 长度非法（剩 6 位）
    const bool notLen2 = !Base64Decode("ab", out);          // 补齐位非 0（被截断）
    const bool afterPad = !Base64Decode("aGVsbG8=x", out);  // padding 之后还有数据
    const bool tooMuchPad = !Base64Decode("aGVsbG8===", out);
    const bool ok = notChar && notLen1 && notLen2 && afterPad && tooMuchPad;
    selftest::Emit(L"base64_decode_rejects_garbage", ok,
        (L"char=" + std::to_wstring(notChar ? 1 : 0)
         + L" len1=" + std::to_wstring(notLen1 ? 1 : 0)
         + L" len2=" + std::to_wstring(notLen2 ? 1 : 0)
         + L" afterPad=" + std::to_wstring(afterPad ? 1 : 0)
         + L" tooMuchPad=" + std::to_wstring(tooMuchPad ? 1 : 0)).c_str());
}

void CaseBase64DecodeRealPng() {
    // ★ 用**真实的一张 PNG**做向量（8×8 纯色，74 字节）—— 不是编出来的字符。
    //   手搓的向量会和实现**一起错**（本仓 §19 的老教训）；真图有外部真源可比：
    //   PNG 魔数 + 固定长度，错了立刻看得出来。
    //   ⚠ 同一份 base64 也用在 `RunLiveProbeReport` 的传图探针里 ⇒ 这条用例同时
    //     保护了"探针发出去的图是合法 PNG"。
    const char* kPng =
        "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAEUlEQVR42mO4o6GBFTEMLQkAe3tLAYZNzu4AAAAASUVORK5CYII=";
    std::string out;
    const bool decoded = Base64Decode(kPng, out);
    static const unsigned char kMagic[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    bool magicOk = decoded && out.size() >= 8;
    if (magicOk) {
        for (int i = 0; i < 8; ++i) {
            if (static_cast<unsigned char>(out[static_cast<size_t>(i)]) != kMagic[i]) {
                magicOk = false;
                break;
            }
        }
    }
    const bool ok = decoded && out.size() == 74 && magicOk;
    selftest::Emit(L"base64_decode_real_png", ok,
        (L"bytes=" + std::to_wstring(out.size())
         + L" magic=" + std::to_wstring(magicOk ? 1 : 0)).c_str());
}

void CaseImageExtForMime() {
    const bool ok = ImageExtForMime("image/png") == ".png"
        && ImageExtForMime("image/jpeg") == ".jpg"
        && ImageExtForMime("IMAGE/JPEG") == ".jpg"       // 大小写不敏感
        && ImageExtForMime("image/webp") == ".webp"
        && ImageExtForMime("image/x-weird") == ".png";   // 未知 image/* 退 .png
    selftest::Emit(L"image_ext_for_mime", ok, L"");
}

void CaseExtractImagesFromMessages() {
    // ① 顺序保持、mime→ext、非 data URL 跳过、坏 base64 跳过
    json messages = json::array();
    json u;
    u["role"] = "user";
    u["content"] = json::array();
    {
        json a;
        a["type"] = "image_url";
        a["image_url"]["url"] = "data:image/png;base64,AAAA";        // 3 字节
        u["content"].push_back(a);

        json remote;
        remote["type"] = "image_url";
        remote["image_url"]["url"] = "https://example.com/a.png";     // 远程 ⇒ 跳过
        u["content"].push_back(remote);

        json b;
        b["type"] = "image_url";
        b["image_url"]["url"] = "data:image/jpeg;base64,____";        // 3 字节
        u["content"].push_back(b);

        json bad;
        bad["type"] = "image_url";
        bad["image_url"]["url"] = "data:image/png;base64,***";        // 非法 ⇒ 跳过
        u["content"].push_back(bad);
    }
    messages.push_back(u);

    std::vector<WebAiImageBlob> blobs;
    std::string note;
    const int n = ExtractImagesFromMessages(messages, blobs, 4, 1024 * 1024, note);
    const bool ok = n == 2 && blobs.size() == 2
        && blobs[0].mime == "image/png" && blobs[0].ext == ".png" && blobs[0].data.size() == 3
        && blobs[1].mime == "image/jpeg" && blobs[1].ext == ".jpg"
        && static_cast<unsigned char>(blobs[1].data[0]) == 0xFF
        && Contains(note, "网络图片") && Contains(note, "无法解码");
    selftest::Emit(L"extract_images_from_messages", ok,
        (L"n=" + std::to_wstring(n) + L" note="
         + std::wstring(note.begin(), note.end())).c_str());
}

void CaseExtractImagesLimits() {
    // ② 张数上限：**保新弃旧**（越靠后的消息越新）
    json messages = json::array();
    messages.push_back(Msg("system", "系统"));
    messages.push_back(UserMsgWithImages(2));
    messages.push_back(UserMsgWithImages(2));   // 共 4 张，只留 2 张 ⇒ 应留后 2 张

    std::vector<WebAiImageBlob> blobs;
    std::string note;
    const int n = ExtractImagesFromMessages(messages, blobs, 2, 1024 * 1024, note);
    const bool countOk = n == 2 && Contains(note, "张数上限");

    // ③ 体量上限：每张 3 字节 ⇒ 上限 4 字节时只能进 1 张
    std::vector<WebAiImageBlob> b2;
    std::string note2;
    const int n2 = ExtractImagesFromMessages(messages, b2, 8, 4, note2);
    const bool bytesOk = n2 == 1 && Contains(note2, "体量上限");

    selftest::Emit(L"extract_images_limits", countOk && bytesOk,
        (L"count=" + std::to_wstring(n) + L" bytes=" + std::to_wstring(n2)).c_str());
}

void CaseExtractImagesShapes() {
    // ④ 三种 url 形状都要认（OpenAI 对象形 / 简写字符串形 / image 字段形）
    json messages = json::array();
    json u;
    u["role"] = "user";
    u["content"] = json::array();
    {
        json a;
        a["type"] = "image_url";
        a["image_url"] = "data:image/png;base64,AAAA";   // 简写：字符串
        u["content"].push_back(a);

        json b;
        b["type"] = "input_image";
        b["image_url"]["url"] = "data:image/png;base64,AAAA";
        u["content"].push_back(b);

        json c;
        c["type"] = "image";
        c["url"] = "data:image/png;base64,AAAA";
        u["content"].push_back(c);
    }
    messages.push_back(u);

    std::vector<WebAiImageBlob> blobs;
    std::string note;
    const int n = ExtractImagesFromMessages(messages, blobs, 8, 1024 * 1024, note);
    const bool ok = n == 3 && note.empty();   // 全部成功 ⇒ note 必须为空（"一切正常"）
    selftest::Emit(L"extract_images_shapes", ok,
        (L"n=" + std::to_wstring(n)).c_str());
}

void CaseExtractImagesNoImagesIsSilent() {
    // ⑤ 没有图时必须完全安静：note 为空、不产生任何"被砍"的记账
    //    （否则每一轮纯文本请求都会带上噪声诊断）
    json messages = json::array();
    messages.push_back(Msg("system", "系统"));
    messages.push_back(Msg("user", "纯文本问题"));
    std::vector<WebAiImageBlob> blobs;
    std::string note;
    const int n = ExtractImagesFromMessages(messages, blobs, 4, 1024, note);
    selftest::Emit(L"extract_images_no_images_is_silent", n == 0 && note.empty(),
        (L"n=" + std::to_wstring(n)).c_str());
}


void CaseFingerprintStability() {
    const json a = Msg("user", "同样的话");
    const json b = Msg("user", "同样的话");
    const json c = Msg("user", "不同的话");
    const bool ok = MessageFingerprint(a) == MessageFingerprint(b)
        && MessageFingerprint(a) != MessageFingerprint(c)
        && !MessageFingerprint(a).empty();
    selftest::Emit(L"message_fingerprint_stable", ok, L"");
}

// ── 5. 配置解析 ────────────────────────────────────────────────────────────

void CaseConfigDefaultsDisabled() {
    const UserConfig c = ParseUserConfigJson("", false);
    const bool ok = !c.enabled && !c.fileExists && c.provider == "doubao"
        && c.modelName == "web" && c.idleTimeoutMs == 8000 && c.maxTotalMs == 180000;
    selftest::Emit(L"config_missing_file_means_disabled", ok, L"");
}

void CaseConfigParseAndClamp() {
    const UserConfig c = ParseUserConfigJson(
        "{\"enabled\":true,\"provider\":\"deepseek\",\"modelName\":\"网页AI\","
        "\"idleTimeoutMs\":999999,\"maxTotalMs\":1,\"maxToolListChars\":10,"
        "\"maxTotalPromptChars\":9999999}", true);
    const bool ok = c.enabled && c.provider == "deepseek" && c.modelName == "网页AI"
        && c.idleTimeoutMs == 120000          // 上限夹住
        && c.maxTotalMs == 5000               // 下限夹住
        && c.maxToolListChars == 500
        && c.maxTotalPromptChars == 400000;
    selftest::Emit(L"config_parse_and_clamp", ok,
        (L"idle=" + std::to_wstring(c.idleTimeoutMs) + L" total=" + std::to_wstring(c.maxTotalMs)).c_str());
}

void CaseConfigBadJsonStaysDisabled() {
    // 坏 JSON ⇒ **不许**算开启（拿半份配置去驱动浏览器 = 不可预期）
    const UserConfig c = ParseUserConfigJson("{enabled: true,", true);
    const bool ok = !c.enabled && c.fileExists;
    selftest::Emit(L"config_bad_json_stays_disabled", ok, L"");
}

void CaseConfigToleratesBom() {
    // ★ 必须容忍 UTF-8 BOM：记事本「另存为 UTF-8」与 PowerShell `Set-Content -Encoding UTF8`
    //   都会写 BOM，而 nlohmann 对 BOM 直接判失败 ⇒ 配置看着完全正确、功能静默不开启。
    const std::string bom = "\xEF\xBB\xBF";
    const UserConfig c = ParseUserConfigJson(
        bom + "{\"enabled\":true,\"provider\":\"doubao\"}", true);
    const bool ok = c.enabled && c.provider == "doubao";
    selftest::Emit(L"config_tolerates_utf8_bom", ok,
        (L"enabled=" + std::to_wstring(c.enabled ? 1 : 0)).c_str());
}

void CaseSelfTestProcessIgnoresUserConfig() {
    // ★ 自检进程**不读**用户配置文件：本 exe 的 AppDir() 就是 build\Release\，
    //   那里正好会有开发机/用户建好的 web_ai_config.json ⇒ 不隔离的话，
    //   用例结果会取决于「这台机器上有没有那个文件」（干净 runner 上必红）。
    //   ⚠ 判据要**同时**含正例与**负例**：只断言"本进程为真"证明不了判据有区分度
    //     （写个恒 true 的函数也能过）。
    const bool p1 = ExeNameIsSelfTestOrProbe(L"WebAiSelfTest.exe");
    // ⚠ 这条名字**分片拼**：上一版我把字面量拼错了（少一个 c）⇒ 判据对该 exe 恒 false，
    //   而"只断言本进程为真"的写法**照样会绿**。分片拼写让漏字母这件事不可能悄悄发生。
    const bool p2 = ExeNameIsSelfTestOrProbe(
        std::wstring(L"Qst") + L"Recorder" + L"LogicTest.exe");
    const bool p3 = ExeNameIsSelfTestOrProbe(L"web_ai_probe.exe");
    const bool self = p1 && p2 && p3;
    const bool notSelf = !ExeNameIsSelfTestOrProbe(L"QuickScriptTool.exe")
        && !ExeNameIsSelfTestOrProbe(L"QstPlayer.exe")
        && !ExeNameIsSelfTestOrProbe(L"");
    const bool inThisProcess = CurrentProcessIsSelfTestOrProbe();
    const bool ok = self && notSelf && inThisProcess;
    selftest::Emit(L"config_not_read_in_selftest_process", ok,
        (L"正例=" + std::to_wstring(p1 ? 1 : 0) + std::to_wstring(p2 ? 1 : 0)
            + std::to_wstring(p3 ? 1 : 0) + L" 负例=" + std::to_wstring(notSelf ? 1 : 0)
            + L" 本进程=" + std::to_wstring(inThisProcess ? 1 : 0)).c_str());
}

// ── 6. 提示词预算 ──────────────────────────────────────────────────────────

void CasePromptBudgetTrimsToolList() {
    json tools = TwoTools();
    for (int i = 0; i < 200; ++i) {
        json p;
        p["a"]["type"] = "string";
        tools.push_back(MakeTool("filler" + std::to_string(i),
            "填充描述，用来触发总量预算的收紧路径。", p, json::array({"a"})));
    }
    json messages = json::array();
    messages.push_back(Msg("system", "系统设定"));
    messages.push_back(Msg("user", "用户问题"));
    PromptBudget budget;
    // ⚠ 这条用例测的是「**超预算时丢描述但保工具名**」那条旧路径 ⇒ 显式关掉极简模式
    //   （极简模式下首轮压根不发目录，`toolsTrimmed` 恒 false ⇒ 断言失去意义）
    budget.toolCatalogMode = false;
    budget.maxToolListChars = 4000;
    budget.maxTotalChars = 6000;
    SessionState next;
    const TurnPlan plan = PlanTurn(messages, tools, SessionState{}, next, budget);
    const bool ok = plan.valid && plan.toolsTrimmed && plan.totalChars <= 6000 + 500
        && Contains(plan.text, "mouseClick(");
    selftest::Emit(L"prompt_budget_trims_but_keeps_tools_usable", ok,
        (L"chars=" + std::to_wstring(plan.totalChars)).c_str());
}

// ── 7. 档案注入（模型下拉里那个「默认选项」）────────────────────────────────
//
// 这三条是产品形态的硬规则，破了任何一条用户就会看到错误的东西：
//   ① 档案**始终**可见（不再要求先建 web_ai_config.json）
//   ② 档案**追加在末尾**（不许插队抢走 `savedModels[0]`）
//   ③ **没选中就什么都不动**（不许静默把用户的模型/总闸改掉）

const wchar_t* const kWebModel = L"doubao-web";
const wchar_t* const kWebUrl = L"http://127.0.0.1:1234/v1/chat/completions";

quickscript::AiModelProfile MakeProfile(const std::wstring& name, const std::wstring& key) {
    quickscript::AiModelProfile p;
    p.modelName = name;
    p.apiKey = key;
    p.apiUrl = L"https://api.deepseek.com";
    return p;
}

void CaseProfileAlwaysVisibleAppendedLast() {
    quickscript::AiApiSettings ai;
    ai.enabled = false;
    ai.modelName = L"deepseek-flash";          // 用户自己选的
    ai.apiKey = L"sk-real-user-key";
    ai.savedModels.push_back(MakeProfile(L"deepseek-flash", L"sk-real-user-key"));
    // ⚠ 默认值不是空的（`apiUrl` = openai、`modelName` = gpt-4o）⇒
    //   断言要写成「**没被改动**」，而不是"等于空"（写成空就是在断言默认值，
    //   默认值一改测试就假红 —— 本仓已有这类前科）。
    const std::wstring urlBefore = ai.apiUrl;
    const std::wstring modelBefore = ai.modelName;
    const std::wstring keyBefore = ai.apiKey;

    bool selected = true;  // 故意给真值，验证函数会把它写回 false
    ApplyProfileToSettings(ai, kWebModel, kWebUrl, L"bridge-token", selected);

    const bool ok = ai.savedModels.size() == 2
        && ai.savedModels[0].modelName == L"deepseek-flash"      // ★ 用户自己的仍在第 0 位
        && ai.savedModels[1].modelName == kWebModel              // ★ 追加在末尾
        && ai.savedModels[1].apiUrl == kWebUrl
        && ai.savedModels[1].apiKey == L"bridge-token"
        && !selected                                             // 没选中
        && ai.modelName == modelBefore                           // ★ 默认模型没被抢
        && ai.apiKey == keyBefore                                // ★ 用户的 Key 没被改
        && ai.apiUrl == urlBefore                                // ★ 顶层 URL 没被改
        && ai.enabled == false;                                  // ★ 总闸没被打开
    selftest::Emit(L"profile_always_visible_appended_last", ok,
        (L"n=" + std::to_wstring(ai.savedModels.size())
         + L" sel=" + std::to_wstring(selected ? 1 : 0)
         + L" enabled=" + std::to_wstring(ai.enabled ? 1 : 0)).c_str());
}

void CaseProfileSelectedEnablesAndPointsLocal() {
    quickscript::AiApiSettings ai;
    ai.enabled = false;
    ai.modelName = kWebModel;                  // ★ 用户在下拉里选了它
    // 用户完全没有配 API Key —— 这正是「无需配置任何 API 就能用」的场景
    ai.savedModels.push_back(MakeProfile(L"deepseek-flash", L"sk-real-user-key"));

    bool selected = false;
    ApplyProfileToSettings(ai, kWebModel, kWebUrl, L"bridge-token", selected);

    const bool ok = selected
        && ai.enabled == true                 // ★ 选中 ⇒ 总闸打开
        && ai.apiUrl == kWebUrl               // ★ 顶层指向本机端点
        && ai.apiKey == L"bridge-token"       // ★ 用桥的 token（免用户 Key）
        && ai.savedModels.size() == 2
        && ai.savedModels[1].modelName == kWebModel;
    selftest::Emit(L"profile_selected_enables_and_points_local", ok,
        (L"sel=" + std::to_wstring(selected ? 1 : 0)
         + L" enabled=" + std::to_wstring(ai.enabled ? 1 : 0)).c_str());
}

void CaseProfileIdempotentAndRefreshesEndpoint() {
    quickscript::AiApiSettings ai;
    ai.modelName = kWebModel;
    bool sel = false;

    ApplyProfileToSettings(ai, kWebModel, kWebUrl, L"tok-1", sel);
    const size_t after1 = ai.savedModels.size();
    // ★ 端口/token 每次启动都变 ⇒ 第二次必须**原地刷新**，不许又追加一条
    ApplyProfileToSettings(ai, kWebModel, L"http://127.0.0.1:9999/v1/chat/completions",
                           L"tok-2", sel);
    const bool ok = after1 == 1 && ai.savedModels.size() == 1
        && ai.savedModels[0].apiUrl == L"http://127.0.0.1:9999/v1/chat/completions"
        && ai.savedModels[0].apiKey == L"tok-2"
        && sel;
    selftest::Emit(L"profile_idempotent_and_refreshes_endpoint", ok,
        (L"n=" + std::to_wstring(ai.savedModels.size())).c_str());
}

void CaseProfileNeverAutoSelects() {
    // ★★ 关键：用户什么都没配（没选默认模型）时，档案会被加进列表（**可见**），
    //    但**绝不**自动选中 —— "可见"和"启用"是两件事，
    //    混起来就是静默接管用户的 AI 设置。
    quickscript::AiApiSettings ai;
    ai.enabled = false;
    // ⚠ `AiApiSettings` 的默认 `modelName` 是 `gpt-4o`（不是空）⇒
    //   断言"没被改动"，而不是"等于空"。
    const std::wstring modelBefore = ai.modelName;
    bool sel = true;
    ApplyProfileToSettings(ai, kWebModel, kWebUrl, L"tok", sel);
    const bool ok = ai.savedModels.size() == 1
        && ai.savedModels[0].modelName == kWebModel
        && ai.modelName == modelBefore   // ★ 没有自动选中（默认值原样保留）
        && !sel
        && ai.enabled == false;          // ★ 总闸仍然关着
    selftest::Emit(L"profile_never_auto_selects", ok,
        (L"modelName=" + ai.modelName + L" enabled="
         + std::to_wstring(ai.enabled ? 1 : 0)).c_str());
}



/// 本文件不链 `utils.h`（自检要能在任何环境跑）⇒ 自带一个最小窄化转换。
/// ⚠ 站点 id 是**纯 ASCII**（`doubao`/`deepseek`/…），窄化转换在这里是安全的。
std::string NarrowAscii(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (const wchar_t c : w) {
        out.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    return out;
}

/// 模拟 `EnsureProfileInSettings` 的循环（它依赖桥，不能直接测）
bool ApplyAllProfiles(quickscript::AiApiSettings& ai, bool& anySelected) {
    anySelected = false;
    for (const auto& pid : BuiltinWebAiProviderIds()) {
        bool sel = false;
        ApplyProfileToSettings(ai, WebAiModelNameForProvider(pid), kWebUrl, L"tok", sel);
        if (sel) anySelected = true;
    }
    return anySelected;
}

void CaseProfileForEveryBuiltinProvider() {
    // ★ 2026-09-25 用户要求：模型下拉里**只留一个 `web`**（此前每站点一条，看花眼）。
    //   站点由运行时自动挑 ⇒ 这里断言"注入的是统一名"，并保证**站点表仍然完整**
    //   （站点表没了自动挑选就无从谈起）。
    quickscript::AiApiSettings ai;
    ai.modelName = L"deepseek-flash";
    ai.savedModels.push_back(MakeProfile(L"deepseek-flash", L"sk-real"));

    bool sel = false;
    // ★ 走**真实入口**（不是直接调 ApplyProfileToSettings）——
    //   否则"到底注入了哪个模型名"这件事没人测，改回 doubao-web 测试照样全绿。
    MigrateAndApplyProfiles(ai, kWebUrl, L"tok", sel);

    const auto& ids = BuiltinWebAiProviderIds();
    const bool tableOk = !ids.empty();
    const bool ok = tableOk
        && ai.savedModels.size() == 2                       // 用户那条 + 唯一的 web
        && ai.savedModels[0].modelName == L"deepseek-flash" // ★ 用户自己的仍在第 0 位
        && ai.savedModels[1].modelName == kWebAiModelName   // ★ 只有一个 web
        && ai.savedModels[1].apiUrl == kWebUrl
        && !sel;                                            // 没选中 ⇒ 不启用
    selftest::Emit(L"profile_for_every_builtin_provider", ok,
        (L"n=" + std::to_wstring(ai.savedModels.size())
         + L" sites=" + std::to_wstring(ids.size())).c_str());
}

void CaseLegacyProfilesAreMigrated() {
    // ★★ 迁移：旧的 `doubao-web`（**本机端点**的那条）要被清掉，
    //    之前选中它的用户自动改选新的 `web`；用户**自己**的档案一个都不许动。
    //    ⚠ 判据必须同时满足"旧名 + 本机回环 URL"—— 用户自己起名/自己填 URL 的
    //      一律不动（判严不判松）。
    quickscript::AiApiSettings ai;
    ai.modelName = L"doubao-web";                       // 用户之前选的是旧档案
    ai.savedModels.push_back(MakeProfile(L"deepseek-flash", L"sk-real"));   // 用户自己的
    {
        quickscript::AiModelProfile legacy;
        legacy.modelName = L"doubao-web";
        legacy.apiUrl = kWebUrl;                        // ★ 本机回环 ⇒ 判定是我们注入的
        legacy.apiKey = L"old-token";
        ai.savedModels.push_back(legacy);
    }
    {
        quickscript::AiModelProfile mine;               // 用户自己起的怪名字
        mine.modelName = L"我的豆包web";                 // 名字不是旧形态 ⇒ 不许动
        mine.apiUrl = L"https://api.example.com/v1/chat/completions";
        mine.apiKey = L"sk-mine";
        ai.savedModels.push_back(mine);
    }

    bool sel = false;
    MigrateAndApplyProfiles(ai, kWebUrl, L"tok", sel);

    bool hasWeb = false, hasUserModel = false, hasCustom = false, hasLegacy = false;
    for (const auto& m : ai.savedModels) {
        if (m.modelName == kWebAiModelName) hasWeb = true;
        else if (m.modelName == L"deepseek-flash") hasUserModel = true;
        else if (m.modelName == L"我的豆包web") hasCustom = true;
        else if (m.modelName == L"doubao-web") hasLegacy = true;
    }
    const bool ok = hasWeb && hasUserModel && hasCustom && !hasLegacy
        && ai.modelName == kWebAiModelName   // ★ 选中项被迁移到 web
        && sel;                              // 迁移后 = 选中 ⇒ 启用
    selftest::Emit(L"legacy_profiles_are_migrated", ok,
        (L"n=" + std::to_wstring(ai.savedModels.size())
         + L" web=" + std::to_wstring(hasWeb ? 1 : 0)
         + L" user=" + std::to_wstring(hasUserModel ? 1 : 0)
         + L" custom=" + std::to_wstring(hasCustom ? 1 : 0)
         + L" legacy=" + std::to_wstring(hasLegacy ? 1 : 0)).c_str());
}

void CaseLegacyProviderNames() {
    // ★ 迁移期：旧形态 `doubao-web` 仍要认得（否则老用户的档案会突然"不是网页 AI"，
    //   `ApplyProfileOverride` 不再改写它的端口/token ⇒ 静默 401）。
    const bool ok = IsBuiltinWebAiModelName(L"web")
        && IsBuiltinWebAiModelName(L"doubao-web")
        && IsBuiltinWebAiModelName(L"deepseek-web")
        && IsLegacyProviderModelName(L"yuanbao-web")
        && !IsLegacyProviderModelName(L"web")            // 新形态**不是**旧的
        && !IsBuiltinWebAiModelName(L"gpt-4o")
        && !IsBuiltinWebAiModelName(L"deepseek-v4-pro");  // 真模型不许被误判
    selftest::Emit(L"legacy_provider_names_recognized", ok, L"");
}

void CaseSelectingAnyProviderEnables() {
    // ★ 选中唯一的 `web` 就能启用（用户不再需要先决定用哪个站点）。
    quickscript::AiApiSettings ai;
    ai.enabled = false;
    ai.modelName = kWebAiModelName;
    bool sel = false;
    ApplyProfileToSettings(ai, kWebAiModelName, kWebUrl, L"tok", sel);
    const bool ok = sel && ai.enabled && ai.apiUrl == kWebUrl && ai.apiKey == L"tok"
        && ai.savedModels.size() == 1;
    selftest::Emit(L"selecting_any_provider_enables", ok,
        (L"sel=" + std::to_wstring(sel ? 1 : 0)
         + L" en=" + std::to_wstring(ai.enabled ? 1 : 0)).c_str());
}

void CaseBuiltinProviderNamesRoundTrip() {
    // ★★ **同源守卫**：加站点要同时改三处（本文件的内置表、`ProviderFromModelName`、
    //    `extension/edge/web_ai_providers.json`）。这里钉住前两处 ——
    //    每个内置站点的模型名**必须**能被 `ProviderFromModelName` 解回它自己。
    //    漏了 `ProviderFromModelName` 的症状：用户在设置里选了这个模型，
    //    请求却被判成「认不出站点」而失败。
    const auto& ids = BuiltinWebAiProviderIds();
    bool ok = !ids.empty();
    std::wstring detail;
    for (const auto& pid : ids) {
        const std::wstring model = WebAiModelNameForProvider(pid);
        const std::string back = ProviderFromModelName(NarrowAscii(model));
        const std::wstring backW(back.begin(), back.end());
        if (backW != pid) {
            ok = false;
            detail += model + L"→" + backW + L" ";
        }
        // 名字还必须被认成「网页 AI 档案」，否则 ApplyProfileOverride 不会改写它
        if (!IsBuiltinWebAiModelName(model)) {
            ok = false;
            detail += model + L"(not-builtin) ";
        }
    }
    selftest::Emit(L"builtin_provider_names_round_trip", ok,
        detail.empty() ? L"" : detail.c_str());
}



// ── 8. 渐进披露（工具目录 + loadTools）──────────────────────────────────────
//
// 为什么必须逐格断言：这套机制把"模型要用的信息"**推迟**到第二次往返才给。
// 一旦目录里的"一句话"写虚了、或者 `loadTools` 摘调用摘错了，
// 症状是"模型选错工具/调用丢失"—— 比上下文变大严重得多。

/// 造一批"完整定义"形状的工具（含类型/枚举/描述），用于对比体积
json SchemaTools() {
    // ⚠ 参数要**足够多、描述要足够长**才接近真实工具（本仓实测 36~48 个工具 = 30~50 KB）。
    //   参数太少的话"目录 vs 完整"的差距被摊薄，比值断言就失去意义。
    json arr = json::array();
    for (int i = 0; i < 12; ++i) {
        json p;
        p["x"]["type"] = "integer";
        p["x"]["description"] = "屏幕 X 坐标，像素（左上为原点，支持负值表示副屏）";
        p["y"]["type"] = "integer";
        p["y"]["description"] = "屏幕 Y 坐标，像素（左上为原点，支持负值表示副屏）";
        p["button"]["type"] = "string";
        p["button"]["enum"] = json::array({"left", "right", "middle"});
        p["button"]["description"] = "用哪个键点击；默认左键";
        p["duration"]["type"] = "integer";
        p["duration"]["description"] = "按下与松开之间的间隔毫秒数；0 表示不等待直接抬起";
        p["retry"]["type"] = "integer";
        p["retry"]["description"] = "失败后重试次数，默认 0；与 duration 配合使用时要留意总耗时";
        p["onFail"]["type"] = "string";
        p["onFail"]["description"] = "失败时的处理策略：跳过本次 / 终止脚本 / 记录并继续";
        arr.push_back(MakeTool("tool" + std::to_string(i),
            "在屏幕上某个坐标点一下鼠标。这条描述故意写长一点，模拟真实工具说明的长度，"
            "用来验证目录确实把它压短了。", p, json::array({"x", "y"})));
    }
    return arr;
}

void CaseToolCatalogHasNoSchema() {
    const json tools = SchemaTools();
    bool trimmed = false;
    const std::string cat = RenderToolCatalog(tools, 48, 100000, &trimmed);
    // ★ 目录里**只能有**名字与参数名：不许出现类型、枚举、必填标记、长描述
    // ⚠ 目录里**允许**短枚举（`button:left|right` 才 10 来字符，却能挡住"模型自己编个值"），
    //   但**不许**出现类型、必填标记、参数长描述。
    // ⚠ 参数顺序是 **`nlohmann::json` 默认的 `std::map` 序（按 key 字母序）**，
    //   不是声明顺序 —— 写断言时必须按字母序（button < duration < onFail < retry < x < y）。
    //   前科：我按声明顺序写 `x, y, button...` ⇒ 假红。
    const bool ok = !cat.empty() && !trimmed
        && Contains(cat, "tool0(button:left|right|middle, duration, onFail, retry, x, y)")
        && !Contains(cat, ":integer") && !Contains(cat, ":string")
        && !Contains(cat, "必填")
        && !Contains(cat, "屏幕 X 坐标")
        && !Contains(cat, "按下与松开之间");
    selftest::Emit(L"tool_catalog_has_no_schema", ok,
        (L"chars=" + std::to_wstring(cat.size())).c_str());
}

void CaseToolCatalogIsFarSmaller() {
    // ★ 量化：目录必须比完整清单**小一个数量级**（这是这项改动的全部意义）
    const json tools = SchemaTools();
    bool t1 = false;
    bool t2 = false;
    const std::string cat = RenderToolCatalog(tools, 48, 100000, &t1);
    const std::string full = RenderToolList(tools, 100000, &t2);
    // ★ 量化（**如实**）：目录省掉的是「参数类型 + 必填标记 + 长描述（160→48 字节）」。
    //   ⚠ 省不到 10 倍 —— 参数**名字**必须留着（模型要靠它拼 arguments），
    //     短枚举也刻意留着（防模型编错值）。实测这个形状约 **2.2~2.5 倍**。
    //   ⚠ 另外要注意：`PlanTurn` 的**工具清单只在首轮发**（`if (plan.newConversation)`），
    //     不是每轮重发 ⇒ 这是一次性成本，不是乘数（别按"每轮省 N KB"去算收益）。
    const bool ok = !cat.empty() && !full.empty()
        && static_cast<int>(cat.size()) * 2 < static_cast<int>(full.size());
    selftest::Emit(L"tool_catalog_is_far_smaller", ok,
        (L"catalog=" + std::to_wstring(cat.size())
         + L" full=" + std::to_wstring(full.size())).c_str());
}

void CaseToolDefinitionsFullSchema() {
    const json tools = SchemaTools();
    const std::string defs = RenderToolDefinitions(tools, {"tool3"});
    const bool ok = Contains(defs, "【tool3】")
        && Contains(defs, "x (integer，必填)")
        && Contains(defs, "button (string，可选)")
        && Contains(defs, "取值：left|right|middle")
        && Contains(defs, "屏幕 X 坐标")           // 参数描述要带来
        && Contains(defs, "在屏幕上某个坐标点");     // 工具描述也要带来
    selftest::Emit(L"tool_definitions_full_schema", ok,
        (L"chars=" + std::to_wstring(defs.size())).c_str());
}

void CaseToolDefinitionsUnknownNameIsReported() {
    // ★ 名字写错时必须**如实说**（模型据此纠正），不许静默给空
    const json tools = SchemaTools();
    const std::string defs = RenderToolDefinitions(tools, {"tool3", "noSuchTool"});
    const bool ok = Contains(defs, "【noSuchTool】")
        && Contains(defs, "没有这个工具")
        && Contains(defs, "【tool3】");   // 写对的那个照样给
    selftest::Emit(L"tool_definitions_unknown_name_reported", ok, L"");
}

void CaseParseReplyBareJsonIsToolCall() {
    // ★★ **裸 JSON 也必须变成工具调用**（2026-10-02 回归用例）。
    //
    //   起因（用户实测报障）：提示词统一成"只输出这一种 JSON 数组"后，
    //   而 `ParseReply` 原来**只从 `<<<QST_TOOL_CALLS>>>` 块里**取调用 ⇒
    //   块外的 JSON 被当成**正文** ⇒ AI 助手把
    //   `[{"action":"runDesktopTask","command":"双击桌面上的 360 杀毒图标"}]`
    //   原样显示在气泡里，动作一个都没执行（"助手的操作路径怎么也改坏了"）。
    //   ⇒ 本用例钉三件事：① 变成 tool_call；② 名字取 `action` 键；
    //     ③ **扁平参数**（`command`）要进 arguments（少了它就是"用默认参数执行"）。
    const ParsedReply r = ParseReply(
        "[{\"action\":\"runDesktopTask\",\"command\":\"\xe5\x8f\x8c\xe5\x87\xbb\xe6\xa1\x8c\xe9\x9d\xa2\xe4\xb8\x8a\xe7\x9a\x84 360 \xe6\x9d\x80\xe6\xaf\x92\xe5\x9b\xbe\xe6\xa0\x87\"}]");
    const bool one = r.toolCalls.is_array() && r.toolCalls.size() == 1;
    const std::string nm = one ? r.toolCalls[0]["function"].value("name", "") : std::string();
    const std::string argsStr = one ? r.toolCalls[0]["function"].value("arguments", "") : std::string();
    const json args = json::parse(argsStr, nullptr, false);
    const bool ok = one && nm == "runDesktopTask"
        && !args.is_discarded() && args.is_object() && args.contains("command")
        && r.content.find("runDesktopTask") == std::string::npos;   // 不能又把 JSON 留成正文
    selftest::Emit(L"parse_reply_bare_json_is_tool_call", ok,
        (L"calls=" + std::to_wstring(r.toolCalls.is_array() ? r.toolCalls.size() : 0)
            + L" name=" + std::wstring(nm.begin(), nm.end())).c_str());
}

void CaseTakeLoadToolNames() {
    // ★ `loadTools` 是**元工具**，必须从 tool_calls 里摘掉（上层不认识它，
    //   交给 AgentCore 会变成"未知工具"而整轮失败）；其它调用要原样保留。
    json calls = json::array();
    {
        json c;
        c["name"] = "loadTools";
        c["arguments"]["names"] = json::array({"mouseClick", "findImage"});
        calls.push_back(c);
    }
    {
        json c;
        c["name"] = "mouseClick";
        c["arguments"]["x"] = 640;
        calls.push_back(c);
    }
    {
        json c;   // arguments 是 **JSON 字符串** 形态也要认（本仓两种都出现过）
        c["name"] = "loadTools";
        c["arguments"] = "{\"names\":[\"ocr\"]}";
        calls.push_back(c);
    }
    std::vector<std::string> names;
    const bool found = TakeLoadToolNames(calls, names);
    const bool ok = found && names.size() == 3
        && names[0] == "mouseClick" && names[1] == "findImage" && names[2] == "ocr"
        && calls.size() == 1 && calls[0].value("name", "") == "mouseClick";
    selftest::Emit(L"take_load_tool_names", ok,
        (L"names=" + std::to_wstring(names.size())
         + L" left=" + std::to_wstring(calls.size())).c_str());
}

void CasePlanCatalogModeMentionsLoadTools() {
    json messages = json::array();
    messages.push_back(Msg("system", "你是桌面自动化助手。"));
    messages.push_back(Msg("user", "点一下那个按钮"));
    SessionState next;
    PromptBudget budget;
    budget.toolCatalogMode = true;
    const TurnPlan plan = PlanTurn(messages, SchemaTools(), SessionState{}, next, budget);
    // ★ 目录模式（默认）契约（2026-10-02 按实现实际行为对齐；用例名保留）。
    //   2026-09-30 23:25 起目录模式 = **首轮内联"名字 + 一句话用途"紧凑清单**，
    //   完整参数定义与 `loadTools` **都不再出现**（理由见 CaseFirstTurn 的注释与实现里那段）。
    //   ⚠ 旧断言（`loadTools` / `:integer` / "不要为了确认工具而反复取清单"）写的是
    //     2026-09-30 20:52 那一版文案，**已不可达**；本用例此前从未跑过所以一直没暴露。
    const bool ok = plan.valid
        // ★★★ 2026-10-02 契约再变（用户决定）：**只给「怎么要清单」**，不发清单本身；
        //   并**必须把归属说清楚**（"不是你的函数"）——用户指出 09-30 的失败是"没说清楚"。
        && Contains(plan.text, "不是你的函数")            // ★ 说清楚归属
        && Contains(plan.text, "loadTools")            // ★ 给"怎么要清单"
        && !Contains(plan.text, "- mouseClick")        // ★ 不发工具名
        && !Contains(plan.text, ":integer");           // 也不给完整 schema
    // ⚠ 不断言 `kToolCallsBegin`：紧凑模式实测**不输出格式块**（同 CaseFirstTurn 的说明）。
    const bool dHdr = Contains(plan.text, "工具（名：用途）");
    const bool dCall = Contains(plan.text, "直接按名字调用");
    const bool dLt = Contains(plan.text, "loadTools");
    const bool dInt = Contains(plan.text, ":integer");
    const bool dCb = Contains(plan.text, kToolCallsBegin);
    selftest::Emit(L"plan_catalog_mode_mentions_loadtools", ok,
        (L"chars=" + std::to_wstring(plan.totalChars)
            + L" hdr=" + (dHdr ? L"1" : L"0") + L" call=" + (dCall ? L"1" : L"0")
            + L" lt=" + (dLt ? L"1" : L"0") + L" intg=" + (dInt ? L"1" : L"0")
            + L" cb=" + (dCb ? L"1" : L"0")).c_str());
}

void CasePlanLegacyModeStillFull() {
    // ★ 关掉渐进披露要能**完整退回**旧行为（万一某站点模型不听话时的逃生门）
    json messages = json::array();
    messages.push_back(Msg("system", "你是桌面自动化助手。"));
    messages.push_back(Msg("user", "点一下那个按钮"));
    SessionState next;
    PromptBudget budget;
    budget.toolCatalogMode = false;
    const TurnPlan plan = PlanTurn(messages, SchemaTools(), SessionState{}, next, budget);
    const bool ok = plan.valid && Contains(plan.text, ":integer")
        && !Contains(plan.text, "loadTools");
    selftest::Emit(L"plan_legacy_mode_still_full", ok,
        (L"chars=" + std::to_wstring(plan.totalChars)).c_str());
}

// ── 9. 类型安全读 JSON（**绝不抛异常**）──────────────────────────────────────
//
// ⚠⚠ 为什么这条必须逐格断言：`json::value(key, default)` 在**类型不符**时会抛
//   `nlohmann::json::type_error`。而这些读法出现在**桥的 HTTP 线程**里 ⇒
//   异常逃出线程函数 = `std::terminate` = **整个软件瞬间消失、无日志、无退出码**。
//   2026-09-25 真机事故就是这样：扩展回执里 `error` 是 boolean ⇒ 闪退。
//
//   ⇒ 这里把**所有类型组合**跑一遍：任何一个抛异常，本用例就红（甚至进程直接死）。

void CaseJsonSafeReadNeverThrows() {
    json o = json::object();
    o["s"] = "hello";
    o["b"] = true;
    o["i"] = 42;
    o["f"] = 1.5;
    o["arr"] = json::array({1, 2});
    o["obj"] = json::object({{"k", "v"}});
    o["nul"] = nullptr;

    const char* keys[] = {"s", "b", "i", "f", "arr", "obj", "nul", "missing"};
    bool threw = false;
    try {
        for (const char* k : keys) {
            std::string mm;
            (void)JsonStr(o, k, "dflt", &mm);
            (void)JsonBool(o, k, false, &mm);
            (void)JsonInt(o, k, -1, &mm);
        }
        // 非对象（数组 / 字符串 / null）也要安全
        const json notObj = json::array({1, 2, 3});
        std::string mm;
        (void)JsonStr(notObj, "s", "dflt", &mm);
        (void)JsonBool(notObj, "b", false, &mm);
        (void)JsonInt(notObj, "i", -1, &mm);
        (void)JsonStr(json(), "s", "dflt", &mm);
        (void)JsonStr(o, nullptr, "dflt", &mm);
    } catch (...) {
        threw = true;
    }
    selftest::Emit(L"json_safe_read_never_throws", !threw,
        threw ? L"★ 抛异常了（会让整个软件闪退）" : L"8 个键 × 3 种读法 + 非对象输入，全程未抛");
}

void CaseJsonSafeReadSemantics() {
    json o = json::object();
    o["s"] = "hello";
    o["b"] = true;
    o["i"] = 42;
    o["f"] = 1.5;
    o["bad"] = true;          // ← 期望 string，实际 boolean（**这就是真机那个回执**）
    o["bad2"] = "text";       // ← 期望 bool，实际 string
    o["bad3"] = json::array();  // ← 期望 int，实际 array

    std::string mm1;
    const std::string s1 = JsonStr(o, "s", "dflt", &mm1);
    const std::string s2 = JsonStr(o, "bad", "dflt", &mm1);   // 类型不符 ⇒ 默认值
    const bool mmOk1 = mm1.find("bad") != std::string::npos
        && mm1.find("boolean") != std::string::npos;

    std::string mm2;
    const bool b1 = JsonBool(o, "b", false, &mm2);
    const bool b2 = JsonBool(o, "bad2", true, &mm2);          // 类型不符 ⇒ 默认值 true
    const bool mmOk2 = !mm2.empty();

    std::string mm3;
    const int i1 = JsonInt(o, "i", -1, &mm3);
    const int i2 = JsonInt(o, "bad3", -1, &mm3);
    const bool mmOk3 = !mm3.empty();

    // 缺字段**不算**类型不符（不该记账，否则日志全是噪音）
    std::string mm4 = "dirty";
    (void)JsonStr(o, "missing", "dflt", &mm4);

    const bool ok = s1 == "hello" && s2 == "dflt" && mmOk1
        && b1 && b2 && mmOk2
        && i1 == 42 && i2 == -1 && mmOk3
        && mm4.empty();
    selftest::Emit(L"json_safe_read_semantics", ok,
        (L"s2=" + std::wstring(s2.begin(), s2.end())).c_str());
}

struct Case { const wchar_t* name; void (*fn)(); const wchar_t* meaning; };

const Case kCases[] = {
    {L"plan_first_turn_has_system_tools_protocol", CaseFirstTurn,
     L"首轮提示词含系统设定+完整工具定义（含类型/枚举）+调用协议"},
    {L"plan_continuation_sends_only_new", CaseContinuationSendsOnlyNew,
     L"续轮只发新增消息（不重发历史，避免网页上下文重复）"},
    {L"plan_history_rewrite_resends_full", CaseHistoryRewriteResendsFull,
     L"历史被重写时按新对话处理（指纹判据）"},
    {L"plan_tools_section_sent_once_per_session", CaseToolsSectionSentOncePerSession,
     L"工具清单每会话只发一次（协议瘦身）；标签页重建时由后端清零会话状态重发"},
    {L"plan_repeat_request_resends_fully", CaseRepeatedRequestResendsFully,
     L"同一请求重复提交要重发全量（同一动作跑第二次不能失败）"},
    {L"render_tool_list_trims_descriptions_keeps_names", CaseToolListTrimsDescriptionsNotNames,
     L"工具清单超预算时只丢描述、保工具名与参数"},
    {L"parse_tool_calls_basic", CaseParseBasic, L"协议块基本解析 + 正文剥离"},
    {L"parse_tool_calls_fenced_object_form", CaseParseFencedAndMultiple,
     L"容忍 ``` 围栏与 {tool_calls:[...]} 两种写法"},
    {L"parse_incomplete_block_is_flagged", CaseParseIncomplete,
     L"有头无尾必须标记未完成（半截 JSON 不许当结果）"},
    {L"parse_bad_arguments_drops_call", CaseParseBadArgumentsDropped,
     L"arguments 非法时丢弃该次调用（不许拿默认值硬跑）"},
    {L"parse_plain_answer", CaseParsePlainAnswer, L"普通回答无协议块"},
    {L"parse_arguments_accepts_json_string", CaseParseStringArguments,
     L"arguments 为 JSON 字符串时归一成对象字符串"},
    {L"completion_json_tool_turn_shape", CaseCompletionJsonShape,
     L"非流式工具轮：content=null、finish_reason=tool_calls"},
    {L"completion_json_text_turn_shape", CaseCompletionJsonTextShape,
     L"非流式文本轮：finish_reason=stop"},
    {L"completion_sse_matches_agentcore_contract", CaseCompletionSseShape,
     L"SSE 分片逐条满足 agent_core 的解析硬要求"},
    {L"completion_sse_empty_reply_still_terminates", CaseCompletionSseEmptyStillTerminates,
     L"空回答也要给出可解析分片与结束标记"},
    {L"provider_from_model_name", CaseProviderFromModel, L"模型名→站点映射"},
    {L"is_web_ai_api_url_loopback_only", CaseIsWebAiUrl, L"只认回环的 chat/completions"},
    {L"port_parse_and_canonical_url", CasePortAndCanonical, L"端口解析与规范 URL"},
    {L"image_part_becomes_placeholder", CaseImagePartPlaceholder,
     L"图片 part 只留占位（图片另走上传通道）"},
    {L"stripped_images_are_announced", CaseStrippedImagesAreAnnounced,
     L"一张图都没送过去时必须在提示词里明说（不许让模型以为看到了图）"},
    {L"images_attached_tells_model_to_look", CaseImagesAttachedTellsModelToLook,
     L"图真送过去时话术反过来：让模型直接看图（不许说「没能送过去」）"},
    {L"partial_upload_says_both", CasePartialUploadSaysBoth,
     L"部分上传时两句话都说，且张数对得上"},
    {L"attached_count_clamped_to_requested", CaseAttachedCountCannotExceedRequested,
     L"上传数被传大时夹到请求张数（不许报出比请求里还多的图）"},
    {L"base64_decode_vectors", CaseBase64DecodeVectors,
     L"base64 解码基本向量 + 无 padding + URL-safe + 容忍折行"},
    {L"base64_decode_rejects_garbage", CaseBase64DecodeRejectsGarbage,
     L"非法 base64 一律拒绝（宁可不发也别把半张图发出去）"},
    {L"image_ext_for_mime", CaseImageExtForMime, L"mime→扩展名（未知 image/* 退 .png）"},
    {L"base64_decode_real_png", CaseBase64DecodeRealPng,
     L"真实 PNG 向量（探针发的那张图）能解出合法 PNG 魔数与长度"},
    {L"extract_images_from_messages", CaseExtractImagesFromMessages,
     L"从 messages 抽内联图：保序、跳远程、跳坏数据并如实记账"},
    {L"extract_images_limits", CaseExtractImagesLimits,
     L"张数/体量上限：保新弃旧（越靠后的消息越新）"},
    {L"extract_images_shapes", CaseExtractImagesShapes,
     L"三种 image part 形状都要认，全成功时 note 必须为空"},
    {L"extract_images_no_images_is_silent", CaseExtractImagesNoImagesIsSilent,
     L"没有图时完全安静（不给纯文本请求带噪声诊断）"},
    {L"message_fingerprint_stable", CaseFingerprintStability, L"消息指纹稳定且可区分"},
    {L"config_missing_file_means_disabled", CaseConfigDefaultsDisabled,
     L"配置文件不存在 = 功能关闭"},
    {L"config_parse_and_clamp", CaseConfigParseAndClamp, L"配置解析与上下限夹取"},
    {L"config_bad_json_stays_disabled", CaseConfigBadJsonStaysDisabled,
     L"坏 JSON 不许算开启"},
    {L"config_not_read_in_selftest_process", CaseSelfTestProcessIgnoresUserConfig,
     L"自检进程不读用户配置文件（否则用例结果取决于机器上有没有它）"},
    {L"config_tolerates_utf8_bom", CaseConfigToleratesBom,
     L"配置带 UTF-8 BOM 时仍要能开启（记事本/PowerShell 都会写 BOM）"},
    {L"prompt_budget_trims_but_keeps_tools_usable", CasePromptBudgetTrimsToolList,
     L"总量预算收紧后工具仍可调用"},
    {L"profile_always_visible_appended_last", CaseProfileAlwaysVisibleAppendedLast,
     L"档案始终注入且追加在末尾：不抢 savedModels[0]、不改用户模型/Key/总闸"},
    {L"profile_selected_enables_and_points_local", CaseProfileSelectedEnablesAndPointsLocal,
     L"选中档案 ⇒ 打开总闸 + 顶层指向本机端点（免用户 API Key）"},
    {L"profile_idempotent_and_refreshes_endpoint", CaseProfileIdempotentAndRefreshesEndpoint,
     L"重复注入是幂等的：端口/token 变了要原地刷新，不许追加第二条"},
    {L"profile_never_auto_selects", CaseProfileNeverAutoSelects,
     L"绝不自动选中：档案可见 ≠ 启用（不许静默接管用户的 AI 设置）"},
    {L"profile_for_every_builtin_provider", CaseProfileForEveryBuiltinProvider,
     L"每个内置站点都有一条档案（用户可选「用哪个站点的网页版」）"},
    {L"selecting_any_provider_enables", CaseSelectingAnyProviderEnables,
     L"选中任一站点档案都能启用（不是只有豆包）"},
    {L"json_safe_read_never_throws", CaseJsonSafeReadNeverThrows,
     L"类型安全读 JSON：任何类型组合都不许抛异常（抛=整个软件闪退）"},
    {L"json_safe_read_semantics", CaseJsonSafeReadSemantics,
     L"类型不符给默认值并记账；缺字段不记账（否则日志全是噪音）"},
    {L"tool_catalog_has_no_schema", CaseToolCatalogHasNoSchema,
     L"目录只给名字+参数名：不许泄漏类型/枚举/长描述"},
    {L"tool_catalog_is_far_smaller", CaseToolCatalogIsFarSmaller,
     L"目录比完整清单小（实测约 2.2~2.5 倍，不是数量级）"},
    {L"tool_definitions_full_schema", CaseToolDefinitionsFullSchema,
     L"loadTools 索取后要给**完整**定义（类型/必填/枚举/描述）"},
    {L"tool_definitions_unknown_name_reported", CaseToolDefinitionsUnknownNameIsReported,
     L"要了不存在的工具必须如实说（模型据此纠正）"},
    {L"parse_reply_bare_json_is_tool_call", CaseParseReplyBareJsonIsToolCall,
        L"回复解析：**裸 JSON 数组**（统一模板的形状）也算 tool_call —— 名字取 `action`、"
        L"扁平参数进 arguments；块外的 JSON 不得再被当成正文（AI 助手曾因此只显示一段 JSON）"},
    {L"take_load_tool_names", CaseTakeLoadToolNames,
     L"loadTools 调用要摘掉、其它调用要保留（含 JSON 字符串形态的 arguments）"},
    {L"plan_catalog_mode_mentions_loadtools", CasePlanCatalogModeMentionsLoadTools,
     L"目录模式（首轮给完整定义）仍写明 loadTools 用法 + 劝止反复取清单"},
    {L"plan_legacy_mode_still_full", CasePlanLegacyModeStillFull,
     L"关掉渐进披露能完整退回旧行为（逃生门）"},
    {L"legacy_profiles_are_migrated", CaseLegacyProfilesAreMigrated,
     L"旧档案迁移到 web；用户自己起的名字一律不动"},
    {L"legacy_provider_names_recognized", CaseLegacyProviderNames,
     L"旧形态 <站点>-web 仍被认得（迁移期不能让老档案静默失效）"},
    {L"builtin_provider_names_round_trip", CaseBuiltinProviderNamesRoundTrip,
     L"同源守卫：内置站点名必须能被 ProviderFromModelName 解回自己"},
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--json") selftest::gJson = true;
        else if (a == L"--list") list = true;
        else if (a == L"--help") {
            std::fwprintf(stdout, L"WebAiSelfTest [--json] [--list]\n");
            return 0;
        }
    }
    selftest::InitUtf8Stdout();
    const size_t n = sizeof(kCases) / sizeof(kCases[0]);
    if (list) {
        std::vector<selftest::CaseInfo> infos;
        infos.reserve(n);
        for (const auto& c : kCases) infos.push_back({c.name, L"default", c.meaning});
        selftest::PrintCaseList(L"WebAiSelfTest", infos.data(), infos.size());
        return 0;
    }
    for (const auto& c : kCases) c.fn();
    selftest::EmitSummary();
    return selftest::ExitCode();
}
