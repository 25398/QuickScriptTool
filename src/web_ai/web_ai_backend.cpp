// ──────────────────────────────────────────────────────────────────
// web_ai_backend.cpp — 网页版 AI 后端编排实现
// ──────────────────────────────────────────────────────────────────

#include "web_ai/web_ai_backend.h"

#include "window_mode/ext_bridge/ext_bridge_server.h"
#include "json_util.h"
#include "utils.h"
#include "web_ai/web_ai_config.h"
#include "web_ai/web_ai_driver.h"
#include "web_ai/web_ai_image.h"
#include "web_ai/web_ai_prompt.h"
#include "web_ai/window_ai_driver.h"
#include "window_mode/window_mode_log.h"

#include <chrono>
#include <map>
#include <mutex>

#include <nlohmann/json.hpp>

namespace quickscript::webai {

using json = nlohmann::json;

namespace {

std::mutex g_sessionMu;
/// ★★ 会话状态表：**键 = 会话 key**（不是 provider）。
///
/// ⚠⚠ 为什么不能用 provider 当键（2026-09-26 真机）：
///   UI 里同一个模型可以开多个对话（多个 tab）⇒ 用 provider 当键时，
///   两个对话共用「已发到第几条 / 上一条指纹」⇒ **第二轮起内容错乱**，
///   且 `newConversation` 会被误判 ⇒ **重新导航到另一个网页对话**
///   （用户原话："一个对话在回复后被打到了另一个网页端对话"）。
///
///   会话 key 由 UI 的 `agentTabs[].key`（桥消息 `panelKey`）→ `AgentCore` →
///   请求体的 `user` 字段一路带下来；拿不到时回退到 provider（老行为）。
std::map<std::string, SessionState> g_sessions;

/// ★窗口反代的**串行锁**：同一时刻只允许一轮对话进入那个客户端窗口。
///   ⚠ 窗口只有一个"当前对话"，两轮并发必然互相污染（实测：主对话与标题生成并发 ⇒
///   新对话被开两次、回答错位）。网页端靠"每会话一标签页"解决，窗口端只能串行。
std::mutex g_windowRoundMu;

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string ErrorBody(const std::string& code, const std::string& message,
                      const json& extra = json::object()) {
    json body;
    body["error"]["type"] = "web_ai_backend_error";
    body["error"]["code"] = code;
    body["error"]["message"] = message;
    if (!extra.empty()) body["error"]["detail"] = extra;
    return qst::jsonutil::DumpUtf8Safe(body);
}

PromptBudget BudgetFromConfig() {
    PromptBudget b;
    b.maxToolListChars = Config().maxToolListChars;
    b.maxTotalChars = Config().maxTotalPromptChars;
    b.toolCatalogMode = Config().toolCatalogMode;   // ★ 渐进披露（见 web_ai_prompt.h）
    return b;
}

/// ★ 解析「这次要用哪个站点」。
///   · 模型名里写了具体站点（`doubao-web` / `豆包`…）⇒ 用它（高级用户/老档案仍有效）
///   · 模型名是统一的 `web` ⇒ **自动挑**：已打开的优先 → 已登录的其次（见 `PickProvider`）
///   · 都认不出 ⇒ 退回配置里的站点
/// @param codeOut 出参：给 UI 看的错误短码（`NO_WEB_AI_LOGIN` 等），成功时清空
std::string ResolveProviderFromRequest(const json& req, std::string& codeOut, std::string& error) {
    codeOut.clear();
    error.clear();
    const std::string model = JsonStr(req, "model");
    std::string provider = ProviderFromModelName(model);
    if (!provider.empty()) return provider;

    if (IsWebAiModelName(FromUtf8(model))) {
        // ★ 统一形态 `web`：让扩展挑一个"能用的"站点
        WebAiPickResult pick;
        if (WebAiDriver::Instance().PickProvider(pick)) return pick.provider;
        codeOut = pick.error.empty() ? "NO_WEB_AI_PROVIDER" : pick.error;
        error = pick.message.empty()
            ? "没找到可用的网页 AI：请先在浏览器里登录豆包 / DeepSeek / 元宝 / 千问 之一"
            : pick.message;
        return {};
    }

    // 名字认不出时退回配置里的站点（用户可能把档案命名成「网页助手」之类）
    if (Enabled() && !Config().provider.empty()) return Config().provider;
    codeOut = "NO_PROVIDER";
    error = "无法从模型名「" + model + "」判断要用哪个网页 AI 站点"
            "（名字里应含 doubao/deepseek/元宝/qwen，或直接用 web）";
    return {};
}

/// 写进输入框 + 提交是否已经发生（决定要不要推进会话状态）
bool PromptWasSent(const WebAiSendOutcome& out) {
    if (out.ok) return true;
    return out.error == "WAIT_TIMEOUT" || out.error == "REPLY_NOT_STABLE"
        || out.error == "READ_FAILED" || out.error == "REPLY_EMPTY";
}

/// ★ 扩展版本一致性提示（**带 10s 缓存**，避免每个请求都问桥）。
///
/// 为什么需要：改扩展后**不点 `edge://extensions/` 的「重新加载」⟳**，浏览器就还在跑旧代码
/// （它有自己的一份内存实例）—— 症状是各种看不懂的 `UNKNOWN: webAiXxx`。
/// 把两个版本号带进错误里，用户一眼就知道该干什么。
/// 一致 / 拿不到版本时返回空串。
std::string ExtensionVersionHint() {
    static std::mutex mu;
    static long long checkedAt = 0;
    static std::string cached;
    std::lock_guard<std::mutex> lk(mu);
    const long long now = NowMs();
    if (checkedAt > 0 && now - checkedAt < 10000) return cached;
    cached.clear();
    checkedAt = now;

    std::string disk;
    const std::wstring manifest = windowmode::ExtensionEdgeDirectory() + L"\\manifest.json";
    if (GetFileAttributesW(manifest.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const json m = json::parse(ToUtf8(ReadAll(manifest)), nullptr, false);
        if (m.is_object()) disk = JsonStr(m, "version", "");
    }
    if (disk.empty()) return cached;

    std::vector<WebAiProviderInfo> provs;
    std::string err;
    std::string extVer;
    if (!WebAiDriver::Instance().ListProviders(provs, err, 5000, &extVer)) return cached;
    if (extVer.empty() || extVer == disk) return cached;
    cached = "浏览器里跑的扩展是 v" + extVer + "，软件自带的是 v" + disk
           + " —— 请到 edge://extensions/ 点「重新加载」⟳，再回网页按 F5";
    windowmode::WindowModeLogEventf(L"[网页AI] 扩展版本不一致：浏览器 v%s / 磁盘 v%s",
        FromUtf8(extVer).c_str(), FromUtf8(disk).c_str());
    return cached;
}

/// 窗口反代的错误码 → 人话提示（与网页端的 HintForError 同一形态）
const char* HintForWindowAiError(const std::string& code) {
    if (code == "NO_CLIENT_WINDOW")
        return "目标客户端没在运行（或窗口标题不匹配）：请先把它打开，再重试。"
               "窗口匹配规则见 <exe目录>\\window_ai_providers.json。";
    if (code == "NO_CLIENT_PROFILE") return "模型名不对应任何窗口客户端：请检查 window_ai_providers.json 的 clients。";
    if (code == "ACTIVATE_FAILED")
        return "没能把客户端切到前台：SendInput 只打前台窗口，前台被别的程序锁住时会失败"
               "（先手动点一下那个客户端再重试）。";
    if (code == "NO_INPUT")
        return "没在这个客户端窗口里找到输入框：判据需要按真实 UIA 树校准 —— "
               "跑一次 `qst_window_ai_probe.cmd`（或 POST /qst/window-ai/probe）看 dump，"
               "把 input.controlTypes / nameContains / automationIdContains 填对。"
               "⚠ Cursor 这类「编辑器 + 聊天面板」的客户端**默认是失效安全**的"
               "（requireHints 开着且名单为空）—— 这是刻意的：宁可不动，也不把提示词打进代码文件。";
    if (code == "UIA_SNAPSHOT_FAILED") return "读不到该窗口的 UIA 树：确认它在运行且没有最小化。";
    if (code == "FOCUS_FAILED")
        return "输入框拿不到键盘焦点：没有焦点时打字会打到别的窗口，所以已中止（不会盲打）。";
    if (code == "INPUT_NOT_EMPTY")
        return "输入框里已经有内容（可能是你的草稿）：已中止以免覆盖。请先清空那个输入框再重试。"
               "（本功能**不会**用 Ctrl+A 替你清空 —— 那在带编辑器的客户端里会选中整个文件。）";
    if (code == "WRITE_FAILED")
        return "三条写入通道（UIA 设值 / 剪贴板粘贴 / 逐字输入）都没写进去：该客户端可能屏蔽了"
               "自动化输入，或输入框判据命中的不是真正的编辑区。";
    if (code == "SUBMIT_FAILED") return "提交失败：可把 submitMethod 改成 button 并在 sendButtonNames 里配置发送按钮名。";
    if (code == "NO_REPLY")
        return "提交后没读到新内容：可能没发出去，或该客户端读不回文本（UIA 树里没有 TextPattern）。"
               "跑一次探针看 conversation 段有没有内容。";
    if (code == "REPLY_NOT_STABLE") return "回答一直在变：可把 idleTimeoutMs 调大一点再试。";
    return "";
}

const char* HintForError(const std::string& code) {
    // ⚠ 文案要反映**实际行为**：软件现在会**自己把浏览器拉起来**（后台启动、不抢前台），
    //   不该再让用户"自己去开浏览器"（2026-09-26 用户明确指出这是设计问题）。
    if (code == "NO_EXTENSION")
        return "浏览器扩展没连上本机桥。软件已经**自动尝试把浏览器拉起来**（后台启动，不抢前台）"
               "—— 请稍等 20~30 秒让它连回来；若仍不行：① 确认浏览器里装了这个扩展；"
               "② 到 edge://extensions/ 点「重新加载」⟳。";
    if (code.rfind("NO_TAB", 0) == 0) return "浏览器里没有打开该站点的页面：请先在 Edge 里打开对应网页并登录。";
    if (code == "NO_PROVIDER") return "扩展里没有这个站点的配置：确认 extension/edge/web_ai_providers.json 存在且已重载扩展。";
    if (code == "WRITE_FAILED" || code == "NO_COMPOSER" || code == "FOCUS_NOT_ON_COMPOSER")
        return "字写不进网页输入框：该站点的输入框选择器需要更新（extension/edge/web_ai_providers.json）。";
    if (code == "SUBMIT_FAILED") return "提交失败：网页可能拦住了 Enter，需要用发送按钮。";
    if (code == "WAIT_TIMEOUT") return "等不到网页的新回答：确认网页真的开始生成了（可打开页面看一眼）。";
    if (code == "NO_NEW_MESSAGES") return "请求里没有新消息（调用方把同一份历史重复提交了）。";
    if (code == "EXT_TOO_OLD")
        return "浏览器里的扩展是**旧版本**，不认识软件要用的新功能："
               "请到 edge://extensions/ 点这个扩展的「重新加载」⟳，然后回网页按 F5。"
               "（改了扩展必须重载 —— 浏览器有自己的一份内存实例）";
    if (code == "NO_WEB_AI_LOGIN" || code == "NO_WEB_AI_PROVIDER")
        return "没有可用的网页 AI：请在浏览器里登录「豆包 / DeepSeek / 元宝 / 千问」中的任意一个"
               "（登录一次即可，软件会自动挑已登录的那个），然后重试。";
    if (code == "DEBUGGER_DENIED")
        return "扩展没能给网页 AI 挂上调试会话：请确认没有别的调试器（如 F12 开发者工具）"
               "占着那个标签页，然后重试。";
    if (code == "NO_FILE_INPUT" || code == "UPLOAD_NOT_ACCEPTED")
        return "图片挂不上网页的上传框：该站点的上传控件选择器需要更新"
               "（extension/edge/web_ai_providers.json 的 fileInputSelectors / fileButtonSelectors）。";
    return "";
}

// ── 传图（见 web_ai_image.h）────────────────────────────────────────
// 上限取保守值：网页端对附件张数/体量都有限制，而超了往往是**静默失败**（页面直接忽略）。
// 宁可少传几张并在提示词里如实说明，也别让模型以为图都送到了。
constexpr int kMaxUploadImages = 4;
constexpr size_t kMaxUploadImageBytes = 6u * 1024u * 1024u;  // 解码后总量

/// 抽图 → 落临时文件 → 挂到网页上传框。
/// @return 真正挂上去并**被页面回读确认**的张数（0 = 一张都没成，调用方按降级处理）
int AttachImagesToPage(const std::string& provider, const json& messages, std::string& note) {
    std::vector<WebAiImageBlob> blobs;
    std::string extractNote;
    const int found = ExtractImagesFromMessages(messages, blobs, kMaxUploadImages,
        kMaxUploadImageBytes, extractNote);
    note = extractNote;
    if (found <= 0) return 0;

    // 顺手清掉上次遗留的临时图（只清我们自己目录里、超过 6 小时的）
    PruneUploadDir(6);

    std::vector<std::wstring> paths;
    for (const auto& b : blobs) {
        std::string werr;
        const std::wstring p = WriteTempImage(b, werr);
        if (p.empty()) {
            if (!note.empty()) note += "；";
            note += "落盘失败：" + werr;
            continue;
        }
        paths.push_back(p);
    }
    if (paths.empty()) return 0;

    WebAiDriver& driver = WebAiDriver::Instance();
    std::string aerr;
    // ⚠ 先 attach：扩展侧的 upload 处理器要用 attachMeta.pageTabId 才知道挂到哪个标签页。
    if (!driver.EnsureAttached(provider, aerr, 25000)) {
        if (!note.empty()) note += "；";
        note += "传图前 attach 失败：" + aerr;
        return 0;
    }
    json detail;
    std::string amsg;
    if (!driver.UploadImages(provider, paths, detail, aerr, amsg, 45000)) {
        if (!note.empty()) note += "；";
        note += "上传失败（" + aerr + "）：" + amsg;
        return 0;
    }
    return static_cast<int>(paths.size());
}

}  // namespace

// ──────────────────────────────────────────────────────────────────
// ★ 窗口反代分支（模型名 = 客户端 id/别名，如 `doubao-app` / `cursor`）
//
// 与网页端**共用**：请求解析 → PlanTurn（写什么）→ ParseReply（工具协议）
//                 → BuildCompletionJson/Sse（OpenAI 响应）。
// 只有"传输层"不同：网页端=扩展+CDP；窗口端=UIA+键鼠（本进程）。
//
// ⚠ 为什么单开一个函数而不是在 HandleChatCompletion 里加 if：
//   那条主路径正被并行会话频繁改动（传图 / loadTools / 会话 key）；
//   这里贴一段独立链路，**不碰**它的既有分支，合并冲突面最小。
// ──────────────────────────────────────────────────────────────────
bool HandleChatCompletionViaWindow(const WindowAiClientProfile& profile, const json& req,
    std::string& responseBody, bool& isSse, int& httpStatus) {
    const long long t0 = NowMs();
    const json messages = req.contains("messages") && req["messages"].is_array()
        ? req["messages"] : json::array();
    const json tools = req.contains("tools") && req["tools"].is_array()
        ? req["tools"] : json::array();
    const bool wantStream = JsonBool(req, "stream", false);
    const std::string model = JsonStr(req, "model", profile.id);

    std::string sessionKey = JsonStr(req, "user");
    if (sessionKey.empty()) sessionKey = profile.id;

    // ① 规划（imagesAttached=0 ⇒ PlanTurn 会按"图送不过去"如实说明并让模型改用文字定位，
    //    窗口通道**确实**传不了图 —— 这是事实，不是降级）
    SessionState next;
    TurnPlan plan;
    {
        std::lock_guard<std::mutex> lock(g_sessionMu);
        plan = PlanTurn(messages, tools, g_sessions[sessionKey], next, BudgetFromConfig(), 0);
    }
    if (!plan.valid) {
        responseBody = ErrorBody(plan.error.empty() ? "PLAN_FAILED" : plan.error,
            HintForError(plan.error));
        httpStatus = 400;
        return false;
    }
    // ★ 与网页端**同一行**的豁免：标题生成那类内部小请求不该开新对话
    //   （网页端在 web_ai_backend.cpp 的同名位置也有这一行；漏了它就会
    //    "每轮都新开一个对话 + 把别的会话的回答读回来" —— 用户实测报障的根因之一）。
    if (JsonBool(req, "qst_no_new_chat", false)) plan.newConversation = false;

    windowmode::WindowModeLogEventf(
        L"[窗口AI] client=%s 轮次=%d 新对话=%d 提示词=%d 字（工具裁剪=%d）",
        FromUtf8(profile.id).c_str(), next.turns, plan.newConversation ? 1 : 0,
        plan.totalChars, plan.toolsTrimmed ? 1 : 0);

    // ② 驱动窗口（**后台灌键，不抢前台** + 提交 + 等新回答）
    //
    // ⚠⚠ **一次只能跑一轮**：窗口客户端只有一个"当前对话"，两个请求同时进去必然串台
    //   （实测：主对话与标题生成并发 ⇒ 新对话被开了两次、回答互相错位）。
    //   网页端靠"每个会话一个标签页"解决；窗口端没有多标签，只能**串行**。
    WindowAiSendOutcome out;
    {
        std::lock_guard<std::mutex> winLock(g_windowRoundMu);
        if (plan.newConversation) {
            windowmode::WindowModeLogEventf(L"[窗口AI] 本轮要求新对话 ⇒ 先开新对话再问");
        }
        out = WindowAiDriver::Instance().SendAndRead(profile.id,
            FromUtf8(plan.text), Config().idleTimeoutMs, Config().maxTotalMs,
            plan.newConversation);
    }

    // 只要"写进去 + 提交"真的发生了，就推进会话状态（与网页端同一条纪律）
    const bool sent = out.ok || out.error == "NO_REPLY" || out.error == "REPLY_NOT_STABLE";
    if (sent) {
        std::lock_guard<std::mutex> lock(g_sessionMu);
        g_sessions[sessionKey] = next;
    }

    if (!out.ok) {
        json detail;
        detail["client"] = profile.id;
        detail["input"] = ToUtf8(out.inputMatchedBy);
        detail["write"] = json{{"proof", ToUtf8(WindowAiWriteProofName(out.write.proof))},
                               {"channel", ToUtf8(out.write.channel)},
                               {"readBack", ToUtf8(out.write.readBack)}};
        detail["find_ms"] = out.findMs;
        detail["activate_ms"] = out.activateMs;
        detail["focus_ms"] = out.focusMs;
        detail["write_ms"] = out.writeMs;
        detail["submit_ms"] = out.submitMs;
        detail["wait_ms"] = out.waitMs;
        detail["baseline_chars"] = out.baselineChars;
        responseBody = ErrorBody(out.error.empty() ? "WINDOW_AI_FAILED" : out.error,
            out.message.empty() ? HintForWindowAiError(out.error) : out.message, detail);
        httpStatus = 502;
        windowmode::WindowModeLogEventf(
            L"[窗口AI] 失败 code=%s（前台化=%dms 焦点=%dms 写=%dms 提交=%dms 等=%dms）",
            FromUtf8(out.error).c_str(), out.activateMs, out.focusMs, out.writeMs,
            out.submitMs, out.waitMs);
        return false;
    }

    // ③ 反解析 + 响应（与网页端同一套构造）
    ParsedReply reply = ParseReply(ToUtf8(out.replyText));
    CompletionMeta meta;
    meta.provider = profile.id;
    meta.elapsedMs = static_cast<int>(NowMs() - t0);
    meta.truncated = out.truncated;
    meta.promptTokens = EstimateTokens(plan.text);
    // 写入可信度如实带回：`unavailable` = 写下去了但读不回（**不许**说成"已验证"）
    {
        std::string n = std::string("窗口写入=") + ToUtf8(WindowAiWriteProofName(out.write.proof))
            + "（通道 " + ToUtf8(out.write.channel) + "）";
        if (out.write.proof == WindowAiWriteProof::kUnavailable) {
            n += "；该客户端读不回输入框内容，只能按通道语义推断";
        }
        meta.note = n;
    }
    if (plan.imagesStripped > 0) {
        meta.note += "；图片 " + std::to_string(plan.imagesStripped) + " 张已剥离（窗口通道传不了图）";
    }
    if (!reply.parseError.empty()) {
        meta.note += "；" + reply.parseError;
    }
    if (reply.toolCalls.empty() && reply.content.empty()) {
        responseBody = ErrorBody("REPLY_EMPTY", "窗口里读回来的内容既没有正文也没有工具调用",
            json{{"client", profile.id}, {"truncated", out.truncated}});
        httpStatus = 502;
        return false;
    }

    const std::string id = MakeCompletionId();
    isSse = wantStream;
    responseBody = wantStream ? BuildCompletionSse(id, model, reply, meta)
                              : BuildCompletionJson(id, model, reply, meta);
    httpStatus = 200;
    windowmode::WindowModeLogEventf(L"[窗口AI] 成功：正文 %d 字、工具调用 %d 个、总耗时 %dms",
        static_cast<int>(reply.content.size()), static_cast<int>(reply.toolCalls.size()),
        meta.elapsedMs);
    return true;
}

bool HandleChatCompletion(const std::string& requestJsonUtf8, std::string& responseBody,
                          bool& isSse, int& httpStatus) {
    responseBody.clear();
    isSse = false;
    httpStatus = 500;
    const long long t0 = NowMs();

    const json req = json::parse(requestJsonUtf8, nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        responseBody = ErrorBody("BAD_REQUEST", "请求体不是合法 JSON 对象");
        httpStatus = 400;
        return false;
    }

    // ★★ 窗口反代优先：模型名命中某个窗口客户端 id/别名（如 `doubao-app` / `cursor`）时
    //    走窗口通道。**必须**在网页站点解析之前判 —— 否则 `doubao-app` 会被
    //    `ProviderFromModelName` 里的 "doubao" 子串匹配抢走，跑到浏览器那条路上。
    {
        const std::string modelForRoute = JsonStr(req, "model", "");
        if (!modelForRoute.empty()) {
            const auto& winClients = WindowAiDriver::Instance().Clients();
            const int winIdx = FindWindowAiClient(winClients, modelForRoute);
            if (winIdx >= 0) {
                return HandleChatCompletionViaWindow(winClients[static_cast<size_t>(winIdx)],
                    req, responseBody, isSse, httpStatus);
            }
        }
    }

    std::string provErr;
    std::string provCode;
    const std::string provider = ResolveProviderFromRequest(req, provCode, provErr);
    // ★ 会话键：优先用请求体的 `user`（= UI 的 `agentTabs[].key`，由 `AgentCore` 带下来）。
    //   拿不到（老前端 / 探针）就退回 provider —— 保持老行为，不会更差。
    std::string sessionKey = JsonStr(req, "user");
    if (sessionKey.empty()) sessionKey = provider;
    // 交给驱动 → 随每个 webAi* 请求带给扩展（扩展按它给每个会话分标签页）
    SetCurrentSessionKey(sessionKey);
    if (provider.empty()) {
        const std::string code = provCode.empty() ? "NO_PROVIDER" : provCode;
        std::string hint = HintForError(code);
        if (hint.empty()) hint = provErr;
        // ★ 版本不一致时把**两个版本号**带进来（用户一眼知道该干什么）
        if (code == "EXT_TOO_OLD") {
            const std::string vh = ExtensionVersionHint();
            if (!vh.empty()) hint = vh + "。（" + hint + "）";
        }
        responseBody = ErrorBody(code, hint);
        httpStatus = 400;
        windowmode::WindowModeLogEventf(L"[网页AI] 选站点失败 code=%s",
            FromUtf8(code).c_str());
        return false;
    }

    const json messages = req.contains("messages") && req["messages"].is_array()
        ? req["messages"] : json::array();
    const json tools = req.contains("tools") && req["tools"].is_array()
        ? req["tools"] : json::array();
    const bool wantStream = JsonBool(req, "stream", false);
    const std::string model = JsonStr(req, "model", Config().modelName);

    if (!windowmode::ExtBridgeServer::Instance().IsExtensionConnected()) {
        responseBody = ErrorBody("NO_EXTENSION", HintForError("NO_EXTENSION"),
            json{{"running", windowmode::ExtBridgeServer::Instance().IsRunning()}});
        httpStatus = 503;
        windowmode::WindowModeLogEvent(L"[网页AI] 请求被拒：扩展未连接");
        return false;
    }

    // ── ① 传图（**必须**在 PlanTurn 之前）─────────────────────────
    //   理由：PlanTurn 要按「图到底送过去没有」组织话术（送过去=让它看图，
    //   没送过去=让它改用文字定位）。顺序反了就会说反，而说反是静默错误。
    std::string imageNote;
    int imagesAttached = 0;
    {
        imagesAttached = AttachImagesToPage(provider, messages, imageNote);
        if (!imageNote.empty()) {
            windowmode::WindowModeLogEventf(L"[网页AI] 传图：%d 张已挂上；%s",
                imagesAttached, FromUtf8(imageNote).c_str());
        }
    }

    // ── ② 规划这一轮写什么 ─────────────────────────────────────────
    SessionState next;
    TurnPlan plan;
    {
        std::lock_guard<std::mutex> lock(g_sessionMu);
        plan = PlanTurn(messages, tools, g_sessions[sessionKey], next, BudgetFromConfig(),
                        imagesAttached);
    }
    if (!plan.valid) {
        const std::string hint = HintForError(plan.error);
        responseBody = ErrorBody(plan.error.empty() ? "PLAN_FAILED" : plan.error, hint);
        httpStatus = 400;
        return false;
    }
    windowmode::WindowModeLogEventf(L"[网页AI] provider=%s 轮次=%d 新对话=%d 提示词=%d 字（工具裁剪=%d 图 %d/%d）",
        FromUtf8(provider).c_str(), next.turns, plan.newConversation ? 1 : 0,
        plan.totalChars, plan.toolsTrimmed ? 1 : 0, plan.imagesAttached, plan.imagesStripped);

    // ── ③ 驱动浏览器 ───────────────────────────────────────────────
    WebAiDriver& driver = WebAiDriver::Instance();
    // ★ 把「这一轮是不是新对话」传下去：true 时驱动会先把**我们自己的**标签页
    //   导航到站点入口，拿干净上下文（否则会在上一轮那个会话里继续，历史被重写时就错了）。
    WebAiSendOutcome out = driver.SendAndRead(provider, plan.text,
        Config().idleTimeoutMs, Config().maxTotalMs, plan.newConversation,
        Config().activateTabDuringRun);   // ★ 默认 false：不抢用户的前台

    // ★★ **"没写进去/没提交" ⇒ 自动重试一次**（2026-09-30，用户实测后加）。
    //
    //   实测末轮死在入口：`code=扩展桥发送失败 / webAiRichType, write_ms=1, submit_ms=0`
    //   —— 提示词**根本没进输入框**，属于**可恢复**故障（WS 抖动、输入框刚重建、
    //   页面还在切模式）。旧实现直接报废整轮（用户看到"跑通了又断在最后一步"）。
    //   判据用 `PromptWasSent(out)`：只有"确实没送出去"才重试 —— 送出去了就绝不重发，
    //   否则网页上会出现两条同样的提示词（会话状态也会错位）。
    if (!out.ok && !PromptWasSent(out)) {
        windowmode::WindowModeLogEventf(
            L"[网页AI] 提示词没送出去（%s）⇒ 自动重试一次",
            FromUtf8(out.error.empty() ? std::string("?") : out.error).c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        WebAiSendOutcome retry = driver.SendAndRead(provider, plan.text,
            Config().idleTimeoutMs, Config().maxTotalMs, plan.newConversation,
            Config().activateTabDuringRun);
        if (retry.ok || PromptWasSent(retry)) {
            windowmode::WindowModeLogEventf(L"[网页AI] 重试成功（首次失败=%s）",
                FromUtf8(out.error).c_str());
            out = std::move(retry);
        } else {
            windowmode::WindowModeLogEventf(L"[网页AI] 重试仍失败（%s）",
                FromUtf8(retry.error).c_str());
            out = std::move(retry);   // 报**重试后**的错误：信息更新
        }
    }

    // ⚠ 只要「写进去 + 提交」真的发生了，就必须推进会话状态：
    //   网页上已经有这条 prompt 了，下一轮若还从旧位置重发，模型会看到重复内容。
    // ★★ 后台小请求（生成标题等）**不许开新对话**：它不带会话 key ⇒
    //   会被当成新会话 ⇒ `turns==0` ⇒ 在豆点那边**又开一个对话**（2026-09-26 真机）。
    //   ⚠ 放在这里（PlanTurn 之后）而不是 PlanTurn 里：PlanTurn 是纯函数，
    //     不该知道"请求来自哪个调用方"。
    if (JsonBool(req, "qst_no_new_chat", false)) plan.newConversation = false;

    // ★★ 协议瘦身的兜底（2026-10-02 用户规格）：
    //   专属标签页被**重建**（换页签 / 扩展重启 / 对话丢了）⇒ 网页端上下文整个没了，
    //   我们这轮写进去的只是**增量** ⇒ 必须**清零会话状态**，
    //   让下一轮走 `turns==0` ⇒ 全量重发（含工具清单）重建上下文。
    //   ⚠ 不清零的后果：网页对话里只有半截内容，模型答非所问，而且**没有任何报错**。
    if (out.tabRecreated) {
        windowmode::WindowModeLogEventf(
            L"[网页AI] 专属标签页被重建 ⇒ 清零会话状态，下一轮全量重发重建上下文");
        g_sessions.erase(sessionKey);
    }

    if (PromptWasSent(out)) {
        std::lock_guard<std::mutex> lock(g_sessionMu);
        g_sessions[sessionKey] = next;
    }

    if (!out.ok) {
        json detail;
        detail["provider"] = provider;
        detail["attach_ms"] = out.attachMs;
        detail["write_ms"] = out.writeMs;
        detail["submit_ms"] = out.submitMs;
        detail["wait_ms"] = out.waitMs;
        detail["baseline_blocks"] = out.baselineBlocks;
        responseBody = ErrorBody(out.error.empty() ? "WEB_AI_FAILED" : out.error,
            out.message.empty() ? HintForError(out.error) : out.message, detail);
        httpStatus = 502;
        windowmode::WindowModeLogEventf(L"[网页AI] 失败 code=%s（attach=%dms 写=%dms 提交=%dms 等=%dms）",
            FromUtf8(out.error).c_str(), out.attachMs, out.writeMs, out.submitMs, out.waitMs);
        return false;
    }

    // ── ③c ★ **回显闸**：页面把我们发的提示词回显成了"回答" ────────────────
    //
    //   实测（2026-09-30）：`[诊断] 原始回复：【键鼠工坊・网页 AI 桥接】下面这段是程序发来的
    //   结构化请求…` —— **回复就是我们刚写进输入框的那段提示词**，模型一个字都没答。
    //   旧实现把它当回答去 ParseReply ⇒ 必然「API 未返回有效动作 JSON」，两轮后整轮失败，
    //   而日志上看不出"它其实在回显"（用户体感："又莫名断了"）。
    //   判据（只看**强标识**，不猜语义）：
    //     ① 回复里出现我们自己的横幅标识（`键鼠工坊`）；
    //     ② 或回复开头一段与我们**刚发出去的文本**开头一段相同（≥24 字符，忽略首尾空白）。
    //   ⇒ 认出来就**明确报错**，别把它当答案解析。
    {
        const std::string sent = plan.text;
        const std::string got = out.reply.text;
        std::string headGot = got.substr(0, 200);
        std::string headSent = sent.substr(0, 200);
        auto ltrim = [](std::string& s) {
            size_t i = 0;
            while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i;
            s.erase(0, i);
        };
        ltrim(headGot);
        ltrim(headSent);
        const bool banner = got.find("键鼠工坊") != std::string::npos
            || got.find("网页 AI 桥接") != std::string::npos;
        bool sameHead = false;
        if (headGot.size() >= 24 && headSent.size() >= 24) {
            sameHead = headGot.compare(0, 24, headSent, 0, 24) == 0;
        }
        if (banner || sameHead) {
            responseBody = ErrorBody("REPLY_ECHOES_PROMPT",
                "网页把我们**刚发过去的提示词**回显成了回答（模型没有真正作答）⇒ 本轮作废，"
                "不把它当动作/结论解析。常见原因：①页面把「用户消息」渲染成了一个回答块、"
                "而读回答的选择器命中了它；②模型只复述了说明（常见于它把这段当成了要抄写的内容）。"
                "处置：重跑一次；若每次都这样，请把这一行日志发给开发者（`原始回复：`开头那段）。",
                json{{"provider", provider}, {"replyChars", static_cast<int>(got.size())}});
            httpStatus = 502;
            windowmode::WindowModeLogEventf(
                L"[网页AI] ★回显闸：回复就是我们发出去的提示词（%d 字节，banner=%d sameHead=%d）⇒ 作废本轮",
                static_cast<int>(got.size()), banner ? 1 : 0, sameHead ? 1 : 0);
            return false;
        }
    }

    // ── ④ 反解析回答 ───────────────────────────────────────────────
    ParsedReply reply = ParseReply(out.reply.text);

    // ── ③b ★ 渐进披露：模型要「工具完整定义」时，在**内部**再走一轮 ──────
    //   `loadTools` 是我们自己的元工具，**AgentCore 不认识它** ⇒ 必须在这一层消化掉：
    //   把定义发回网页 → 拿到真正的调用/回答 → 再返回给上层。
    //   ⚠ 上限 2 轮（防模型反复 load 造成死循环）。
    //   ⚠ 失败**不致命**：退回"把已拿到的回答原样返回"，绝不因为 loadTools 挂了就整轮失败。
    {
        std::vector<std::string> wantNames;
        int rounds = 0;
        while (rounds < 2 && TakeLoadToolNames(reply.toolCalls, wantNames)) {
            ++rounds;
            // ★ `names` 传空 = 模型要"完整清单" ⇒ 给**目录**（名字+参数名+一句话），
            //   而不是几十 KB 的完整定义 —— 它拿到目录后可以再精确索取某几个。
            const std::string defs = wantNames.empty()
                ? RenderToolCatalog(tools, BudgetFromConfig().catalogDescChars,
                                   BudgetFromConfig().maxToolListChars, nullptr)
                : RenderToolDefinitions(tools, wantNames);
            if (defs.empty()) break;
            windowmode::WindowModeLogEventf(L"[网页AI] loadTools 第 %d 轮：%d 个工具，%d 字",
                rounds, static_cast<int>(wantNames.size()), static_cast<int>(defs.size()));
            const WebAiSendOutcome again = driver.SendAndRead(provider,
                "【工具结果 loadTools】\n" + defs + "\n请据此输出真正的调用，或直接回答。",
                Config().idleTimeoutMs, Config().maxTotalMs, /*newConversation=*/false,
                /*activateTab=*/false);   // 前台已经切过了，别来回抢
            if (!again.ok) {
                windowmode::WindowModeLogEventf(
                    L"[网页AI] loadTools 追问失败（%s），返回已有回答",
                    FromUtf8(again.error).c_str());
                break;
            }
            reply = ParseReply(again.reply.text);
            if (reply.incomplete) {
                // 追问的回答不完整 ⇒ 保留它（比丢掉强），但记一笔
                windowmode::WindowModeLogEventf(L"[网页AI] loadTools 追问的回答不完整");
            }
            wantNames.clear();
        }
    }

    CompletionMeta meta;
    meta.provider = provider;
    meta.elapsedMs = static_cast<int>(NowMs() - t0);
    meta.truncated = out.reply.truncated;
    meta.promptTokens = EstimateTokens(plan.text);
    if (plan.imagesStripped > 0) {
        // 如实留痕：说清**几张真送到了**、几张没送到。说反就是静默错误。
        std::string n = "图片 " + std::to_string(plan.imagesAttached) + "/"
            + std::to_string(plan.imagesStripped) + " 张已随消息上传";
        const int lost = plan.imagesStripped - plan.imagesAttached;
        if (lost > 0) {
            // ★★ **不确定就说"不确定"**（2026-10-02 真机实测）。
            //   扩展对"挂上又被清空"那种情形报的是 `ACCEPTED_THEN_CLEARED`，它的定义里
            //   写得很清楚：**可能是"读走即清空"（= 成功）**，也可能是真丢弃，**不猜**。
            //   而这里原来一律说「未能上传」—— 那是**把不确定说成了确定的反面**：
            //   模型据此认定"我没有图"，于是不再看图、改用自己的路子做事
            //   （用户实测："没反应""网页练习题非要走自己的路径"）。
            //   ⇒ 只在**确定失败**时才说"未能上传"；命中那种含糊错误时说"可能没上传"，
            //     并给出**唯一可靠的自证办法**（模型自己能看图，别再让它猜）。
            const bool ambiguous = imageNote.find("ACCEPTED_THEN_CLEARED") != std::string::npos;
            n += ambiguous
                ? ("（" + std::to_string(lost) + " 张**可能没上传**：文件挂上后被页面读走即清空，"
                   "也可能是真丢弃 —— 扩展无法确认。**你自己看一眼有没有图**："
                   "没有就用 observePage / 文字索引定位，别改用你自己的联网能力）")
                : ("（" + std::to_string(lost) + " 张未能上传，已提示模型改用文字定位）");
        }
        if (!imageNote.empty()) n += "；" + imageNote;
        meta.note = n;
    }
    if (reply.incomplete) meta.note = "协议块未闭合（网页回答可能被截断）";
    if (!reply.parseError.empty()) {
        meta.note = meta.note.empty() ? reply.parseError : (meta.note + "；" + reply.parseError);
    }
    if (out.reply.truncated) {
        meta.note = meta.note.empty() ? "网页回答被 12000 字上限截断" : (meta.note + "；网页侧截断");
    }
    if (reply.toolCalls.empty() && reply.content.empty()) {
        responseBody = ErrorBody("REPLY_EMPTY", "网页回答里既没有正文也没有工具调用",
            json{{"provider", provider}, {"truncated", out.reply.truncated}});
        httpStatus = 502;
        return false;
    }

    const std::string id = MakeCompletionId();
    isSse = wantStream;
    responseBody = wantStream ? BuildCompletionSse(id, model, reply, meta)
                              : BuildCompletionJson(id, model, reply, meta);
    httpStatus = 200;
    windowmode::WindowModeLogEventf(L"[网页AI] 成功：正文 %d 字、工具调用 %d 个、总耗时 %dms%s",
        static_cast<int>(reply.content.size()),
        static_cast<int>(reply.toolCalls.size()), meta.elapsedMs,
        meta.note.empty() ? L"" : (L"（" + FromUtf8(meta.note) + L"）").c_str());
    return true;
}

// ──────────────────────────────────────────────────────────────────
// 原生真机探针
// ──────────────────────────────────────────────────────────────────

std::string RunLiveProbeReport(const std::string& providerIdIn, bool doSend,
                               const std::string& sendText, bool doUpload) {
    json rep;
    json verdict = json::array();
    bool ok = true;
    const std::string provider = !providerIdIn.empty() ? providerIdIn
        : (Enabled() && !Config().provider.empty() ? Config().provider : std::string("doubao"));
    rep["provider"] = provider;

    auto& bridge = windowmode::ExtBridgeServer::Instance();
    json br;
    br["port"] = bridge.Port();
    br["running"] = bridge.IsRunning();
    br["extConnected"] = bridge.IsExtensionConnected();
    br["extClients"] = bridge.ExtensionClientCount();
    const std::string token = bridge.Token();
    br["tokenPrefix"] = token.substr(0, std::min<size_t>(8, token.size()));
    br["configPath"] = Config().configPath;
    br["configEnabled"] = Config().enabled;
    rep["bridge"] = br;

    if (!bridge.IsExtensionConnected()) {
        verdict.push_back("扩展没连上本机桥 —— 先启动软件并等扩展自动重连（约 30 秒），或点一下扩展图标。");
        rep["ok"] = false;
        rep["verdict"] = verdict;
        return qst::jsonutil::DumpUtf8Safe(rep, 2);
    }
    verdict.push_back("扩展已连接本机桥，且这次是**产品进程内**发起的请求（不是外部探针）。");

    WebAiDriver& driver = WebAiDriver::Instance();

    // [1] 站点（顺带取**扩展自报的版本号**）
    //   ★ 为什么版本号要从扩展嘴里问、而不是比两个文件的版本：
    //     浏览器加载扩展后有**自己的一份内存实例**，文件更新了但没点 ⟳ 就还在跑旧代码
    //     ⇒ 「源码版本 == build 副本版本」**证明不了**浏览器跑的是新版（已误判过）。
    //     唯一的硬证据是扩展**在回执里带上它自己的 BRIDGE_VERSION**。
    std::string diskVersion;
    {
        const std::wstring manifest = windowmode::ExtensionEdgeDirectory() + L"\\manifest.json";
        if (GetFileAttributesW(manifest.c_str()) != INVALID_FILE_ATTRIBUTES) {
            const json m = json::parse(ToUtf8(ReadAll(manifest)), nullptr, false);
            if (m.is_object()) diskVersion = JsonStr(m, "version", "");
        }
    }
    std::vector<WebAiProviderInfo> provs;
    std::string err;
    std::string extVersion;
    if (driver.ListProviders(provs, err, 10000, &extVersion)) {
        json arr = json::array();
        for (const auto& p : provs) {
            arr.push_back(json{{"id", p.id}, {"label", p.label}, {"inputKind", p.inputKind},
                               {"url", p.url}, {"urlHint", p.urlHint}});
        }
        rep["providers"] = arr;
        rep["extVersion"] = extVersion;
        rep["diskManifestVersion"] = diskVersion;
        verdict.push_back("扩展认得 " + std::to_string(provs.size()) + " 个站点（含 " + provider + "）。");
        if (!extVersion.empty()) {
            if (!diskVersion.empty() && extVersion != diskVersion) {
                verdict.push_back("⚠ 浏览器里跑的扩展是 v" + extVersion + "，磁盘上是 v" + diskVersion
                    + " ⇒ **重新加载扩展**（edge://extensions/ 点 ⟳ + 回页面按 F5）后才轮到新代码。");
                ok = false;
            } else {
                verdict.push_back("浏览器里跑的扩展版本 = v" + extVersion + "（与磁盘副本一致）。");
            }
        }
    } else {
        rep["providersError"] = err;
        verdict.push_back("列站点失败：" + err);
        ok = false;
    }

    // [2] 标签页
    //   ★★ 用 `FindProviderPages`（按域名**穷举**该站点的所有标签页），**不要**用 `ListPages`：
    //      后者是给「挑游戏窗」用的 —— 按游戏 URL 打分、每个窗口只留分最高的一个、只返回前 8 条。
    //      豆包这类站点得 0 分会被游戏页挤掉 ⇒ 探针会误报「一个页面都没有」
    //      （2026-09-25 用户实测的 `NO_TAB:doubao` 就是这个原因）。
    std::vector<WebAiPageInfo> ownPages;
    if (driver.FindProviderPages(provider, ownPages, err, 10000)) {
        json arr = json::array();
        for (const auto& p : ownPages) {
            arr.push_back(json{{"tabId", p.tabId}, {"title", p.title}, {"url", p.url}});
        }
        rep["providerPages"] = arr;
        verdict.push_back("浏览器里该站点开着 " + std::to_string(ownPages.size()) + " 个页面"
            + (ownPages.empty() ? "（软件会自动开一个属于它自己的）" : "。"));
    } else {
        rep["providerPagesError"] = err;
        verdict.push_back("查该站点标签页失败：" + err);
    }
    // 附带列一下通用可抓取页面（诊断用，**不参与判定**）
    {
        std::vector<WebAiPageInfo> all;
        std::string lerr;
        if (driver.ListPages(all, lerr, 10000)) {
            json arr = json::array();
            for (const auto& p : all) {
                arr.push_back(json{{"tabId", p.tabId}, {"title", p.title}, {"url", p.url}});
            }
            rep["pagesAll"] = arr;
        }
    }

    // [3] attach（拿到**属于本软件**的标签页并挂上独立调试会话）
    const long long tAttach = NowMs();
    const bool attached = driver.EnsureAttached(provider, err);
    rep["attach"] = json{{"ok", attached}, {"error", err},
                         {"ms", static_cast<int>(NowMs() - tAttach)}};
    if (!attached) {
        verdict.push_back("attach 失败（" + err + "）：确认浏览器里已打开该站点页面并已登录。");
        rep["ok"] = false;
        rep["verdict"] = verdict;
        return qst::jsonutil::DumpUtf8Safe(rep, 2);
    }
    verdict.push_back("已 attach 到该站点的标签页。");

    // [4] 输入框形态
    json probe;
    if (driver.Probe(provider, probe, err, 20000)) {
        rep["probe"] = probe;
        const json inp = probe.contains("input") ? probe["input"] : json();
        if (inp.is_object() && !inp.empty()) {
            const bool rich = JsonBool(inp, "contentEditable", false);
            verdict.push_back(std::string("输入框命中：") + JsonStr(inp, "matchedBy", "?")
                + "，形态=" + (rich ? "富文本(contenteditable)" : "原生表单"));
        } else {
            verdict.push_back("★ 没找到输入框：web_ai_providers.json 里该站点的 inputSelectors 对不上当前页面。");
            ok = false;
        }
    } else {
        rep["probeError"] = err;
        verdict.push_back("输入框探测失败：" + err);
        ok = false;
    }

    // [5] 写进去（干跑：只写一小段再清掉，不发消息）
    const std::string mark = "QST探测";
    json steps;
    std::string werr;
    std::string wmsg;
    const long long tWrite = NowMs();
    const bool wrote = driver.TypeRich(provider, mark, steps, werr, wmsg, 60000);
    rep["write"] = json{{"ok", wrote}, {"error", werr}, {"message", wmsg},
                        {"ms", static_cast<int>(NowMs() - tWrite)}, {"steps", steps}};
    if (wrote) {
        verdict.push_back("★ 已把探测文本写进输入框并**回读确认**（写完即清空，未发消息）。");
        json ignore;
        std::string e2;
        std::string m2;
        if (!driver.TypeRich(provider, "", ignore, e2, m2, 30000)) {
            verdict.push_back("⚠ 清空输入框失败（" + e2 + "）：请手动看一眼页面，必要时删掉那几个字。");
        }
    } else {
        verdict.push_back("★★ 写不进输入框（" + werr + "）：这是整案的关键风险点，需要按站点修选择器或换写入通道。");
        ok = false;
    }

    // [5.5] 可选：传图（**会往页面上挂一张图**，默认不做）
    //   为什么默认关：探针的契约是"只读 + 可选真发一条"，而挂图会在输入框里留下附件
    //   —— 那是在默认路径上改用户的页面状态，不合适。
    //   要验传图就显式 `{"upload": true}`；验证完请手动把那个附件删掉。
    if (doUpload) {
        // 一张 8×8 的纯色 PNG（74 字节）。⚠ 用真 PNG 而不是随便几个字节：
        //   站点可能校验图片头/尺寸，塞垃圾字节会得到"看起来像选择器问题"的假故障。
        const char* kProbePngB64 =
            "iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAAEUlEQVR42mO4o6GBFTEMLQkAe3tLAYZNzu4AAAAASUVORK5CYII=";
        WebAiImageBlob blob;
        blob.mime = "image/png";
        blob.ext = ".png";
        std::string derr;
        const bool decoded = Base64Decode(kProbePngB64, blob.data) && !blob.data.empty();
        std::wstring path;
        if (decoded) path = WriteTempImage(blob, derr);

        json up;
        up["decoded"] = decoded;
        up["pngBytes"] = static_cast<int>(blob.data.size());
        up["tempPath"] = ToUtf8(path);
        if (!decoded || path.empty()) {
            up["ok"] = false;
            up["error"] = decoded ? "WRITE_TEMP_FAILED" : "BASE64_DECODE_FAILED";
            up["message"] = derr;
            verdict.push_back("★ 传图：测试图**没落到临时文件**（" + derr + "）—— 这是宿主侧问题，不是页面问题。");
            ok = false;
        } else {
            json detail;
            std::string uerr;
            std::string umsg;
            const long long tUp = NowMs();
            const bool uploaded = driver.UploadImages(provider, {path}, detail, uerr, umsg, 45000);
            up["ok"] = uploaded;
            up["error"] = uerr;
            up["message"] = umsg;
            up["ms"] = static_cast<int>(NowMs() - tUp);
            up["steps"] = detail;
            if (uploaded) {
                verdict.push_back("★★ 传图成功：测试图已挂到页面的上传框上，且**页面回读确认**了。"
                    "（请手动把输入框里那个附件删掉）");
            } else {
                verdict.push_back("★★ 传图失败（" + uerr + "）：" + umsg
                    + " ⇒ 按站点补 `fileInputSelectors` / `fileButtonSelectors`"
                    "（extension/edge/web_ai_providers.json）。");
                ok = false;
            }
        }
        rep["upload"] = up;
    }

    // [6] 读回答（只读，识别"当前页面上有没有回答"）
    WebAiReplyInfo base;
    std::string rerr;
    if (driver.ReadReply(provider, 2500, 6000, base, rerr, 30000)) {
        rep["reply"] = json{{"ok", true}, {"blocks", base.blocks},
                            {"chars", static_cast<int>(base.text.size())},
                            {"head", base.text.substr(0, 200)},
                            {"matchedBy", base.matchedBy}};
        if (base.text.empty()) {
            verdict.push_back("页面上暂时没有助手回答（正常：还没问过）。");
        } else {
            verdict.push_back("已能读到页面上的回答（" + std::to_string(base.blocks)
                + " 块，最后一块 " + std::to_string(base.text.size()) + " 字）。");
        }
    } else {
        rep["replyError"] = rerr;
        verdict.push_back("读回答失败：" + rerr + "（若页面本来就没有回答，属正常）。");
    }

    // [7] 可选：真发一条
    if (doSend && !sendText.empty()) {
        const WebAiSendOutcome out = driver.SendAndRead(provider, sendText,
            Config().idleTimeoutMs, Config().maxTotalMs);
        json s;
        s["ok"] = out.ok;
        s["error"] = out.error;
        s["message"] = out.message;
        s["baselineBlocks"] = out.baselineBlocks;
        s["attachMs"] = out.attachMs;
        s["writeMs"] = out.writeMs;
        s["submitMs"] = out.submitMs;
        s["waitMs"] = out.waitMs;
        s["replyChars"] = static_cast<int>(out.reply.text.size());
        s["replyHead"] = out.reply.text.substr(0, 400);
        rep["send"] = s;
        if (out.ok) {
            verdict.push_back("★★★ 端到端打通：消息真的发出去了，并读回了新回答。");
        } else {
            verdict.push_back("真发一条失败（" + out.error + "）：" + out.message);
            ok = false;
        }
    }

    rep["ok"] = ok;
    rep["verdict"] = verdict;
    return qst::jsonutil::DumpUtf8Safe(rep, 2);
}

std::string RunLiveProbeReportJson(const std::string& requestBodyUtf8) {
    std::string provider;
    std::string text;
    bool doSend = false;
    bool doUpload = false;
    const json req = json::parse(requestBodyUtf8.empty() ? "{}" : requestBodyUtf8,
        nullptr, false);
    if (req.is_object()) {
        provider = JsonStr(req, "provider");
        text = JsonStr(req, "text");
        doSend = JsonBool(req, "send", false);
        doUpload = JsonBool(req, "upload", false);
    }
    return RunLiveProbeReport(provider, doSend, text, doUpload);
}

// ──────────────────────────────────────────────────────────────────
// 窗口 Agents：设置页的「准星绑定窗口」入口
//
// 语义：把某个**客户端窗口**登记成模型下拉里的一项（写进 `web_ai_config.json`
// 的 `windowClients`）。窗口本身由用户的准星指出来（原生 `CrosshairPick("window")`），
// 我们只负责"这个窗口属于哪个档案 / 要不要新建一条档案"。
// ⚠ 新建档案只是**骨架**：没有校准过的几何判据（rectHint / 操作栏）跑不通 ——
//   所以列表里如实标 `calibrated`，并在 UI 上给出校准入口（探针）。
// ──────────────────────────────────────────────────────────────────

/// 读一个 JSON 文件（不存在/坏 JSON 时返回 null）
json ReadJsonFileOrNull(const std::wstring& path) {
    const std::string raw = ToUtf8(ReadAll(path));
    if (raw.empty()) return json();
    const json j = json::parse(raw, nullptr, false);
    return j.is_discarded() ? json() : j;
}

/// 写 UTF-8 文本文件（与 `app_settings_store.cpp` 的 `WriteUtf8File` 同一套做法；
/// 那份是文件内静态的，这里自带一份，避免为几行代码新建依赖）
bool WriteUtf8FileLocal(const std::wstring& path, const std::string& utf8) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
    return ok && written == utf8.size();
}

bool WriteJsonFile(const std::wstring& path, const json& j, std::string& err) {
    if (!WriteUtf8FileLocal(path, qst::jsonutil::DumpUtf8Safe(j, 2))) {
        err = "写文件失败：" + ToUtf8(path);
        return false;
    }
    return true;
}

std::string SanitizeClientId(const std::string& processName) {
    std::string id;
    for (char c : processName) {
        if (c == '.') break;   // 去掉 .exe
        const char lc = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        if ((lc >= 'a' && lc <= 'z') || (lc >= '0' && lc <= '9')) id.push_back(lc);
        else if (lc == '-' || lc == '_' || lc == ' ') id.push_back('-');
    }
    if (id.empty()) id = "window";
    return "win-" + id;
}

/// 档案里的窗口判据校准好了吗（有几何矩形或有输入框提示词 ⇒ 至少能试）
bool ProfileLooksCalibrated(const WindowAiClientProfile& p) {
    return !p.input.rectHint.empty() || !p.input.nameContains.empty()
        || !p.input.automationIdContains.empty();
}

std::string RunWindowAgentsListJson() {
    // 每次列都重读配置：用户可能刚改过 json（与探针一致的口径）
    ReloadUserConfig();
    WindowAiDriver::Instance().ReloadClients();
    const auto& clients = WindowAiDriver::Instance().Clients();
    const auto bound = Config().windowClients;
    auto isBound = [&](const std::string& id) {
        for (const auto& b : bound) {
            if (b == id) return true;
        }
        return false;
    };
    json out;
    out["ok"] = true;
    json arr = json::array();
    for (const auto& c : clients) {
        json item;
        item["id"] = c.id;
        item["label"] = c.label;
        json pn = json::array();
        for (const auto& p : c.processNames) pn.push_back(ToUtf8(p));
        item["processNames"] = pn;
        item["bound"] = isBound(c.id);
        item["calibrated"] = ProfileLooksCalibrated(c);
        // 进程在不在跑 / 有没有可见窗口（用户在设置页最想知道的两件事）
        WindowAiTarget t;
        std::string err;
        WindowAiClientProfile probe = c;
        const bool found = WindowAiDriver::Instance().FindClientWindow(probe, t, err);
        item["running"] = found;
        if (found) {
            item["windowTitle"] = ToUtf8(t.title);
            item["foreground"] = t.foreground;
            item["restoredFromHidden"] = t.restoredFromHidden;
        } else {
            item["windowError"] = err;
        }
        arr.push_back(std::move(item));
    }
    out["clients"] = arr;
    // ★ 当前**打开的窗口**清单（选择器浮层要列出来让用户直接点，等价于准星拾取）。
    //   ⚠ 与"档案"是两件事：档案 = "能当模型的客户端"（含没在跑的）；
    //     这里 = "现在屏幕上有哪些窗口"。前端按进程名把窗口关联到档案；
    //     关联不上 ⇒ 绑定时按进程名新建一条骨架档案。
    json wins = json::array();
    int shown = 0;
    for (const auto& w : WindowAiDriver::Instance().ListCandidateWindows()) {
        if (w.title.empty() && w.processName.empty()) continue;
        wins.push_back(json{{"process", ToUtf8(w.processName)},
                            {"title", ToUtf8(w.title)},
                            {"foreground", w.foreground},
                            {"minimized", w.minimized}});
        if (++shown >= 40) break;
    }
    out["windows"] = wins;
    out["configPath"] = Config().configPath;
    return qst::jsonutil::DumpUtf8Safe(out, 2);
}

std::string RunWindowAgentBindJson(const std::string& requestBodyUtf8) {
    const json req = json::parse(requestBodyUtf8.empty() ? "{}" : requestBodyUtf8,
        nullptr, false);
    json out;
    if (!req.is_object()) {
        out["ok"] = false;
        out["error"] = "请求体不是 JSON 对象";
        return qst::jsonutil::DumpUtf8Safe(out, 2);
    }
    const std::string wantClient = JsonStr(req, "client", "");
    const std::string process = JsonStr(req, "process", "");
    const std::string title = JsonStr(req, "title", "");
    const bool unbind = JsonBool(req, "unbind", false);

    ReloadUserConfig();
    WindowAiDriver::Instance().ReloadClients();
    const auto& clients = WindowAiDriver::Instance().Clients();

    std::string id = wantClient;
    if (id.empty()) {
        if (process.empty()) {
            out["ok"] = false;
            out["error"] = "缺少 client 或 process";
            return qst::jsonutil::DumpUtf8Safe(out, 2);
        }
        // ① 先按进程名匹配已有档案（豆包 / Cursor / 终端都在内置里）
        for (const auto& c : clients) {
            for (const auto& p : c.processNames) {
                if (!p.empty() && _wcsicmp(p.c_str(), FromUtf8(process).c_str()) == 0) {
                    id = c.id;
                    break;
                }
            }
            if (!id.empty()) break;
        }
        // ② 没匹配上 ⇒ 新建一条**骨架**档案（写进 window_ai_providers.json）
        if (id.empty()) {
            id = SanitizeClientId(process);
            const std::wstring path = AppDir() + L"\\window_ai_providers.json";
            json doc = ReadJsonFileOrNull(path);
            if (!doc.is_object()) doc = json::object();
            if (!doc.contains("clients") || !doc["clients"].is_object()) doc["clients"] = json::object();
            if (!doc["clients"].contains(id)) {
                json c;
                c["label"] = process + (title.empty() ? "" : ("（" + title + "）"));
                c["processNames"] = json::array({process});
                if (!title.empty()) c["titleContains"] = json::array({title});
                c["_todo"] = "这是准星绑定时建的**骨架**：几何判据未校准 ⇒ 先用 "
                             "POST /qst/window-ai/probe 量出 input.rectHint / reply.ocrRegion，"
                             "必要时用 copyScan 标定复制按钮落点。";
                doc["clients"][id] = c;
                std::string werr;
                if (!WriteJsonFile(path, doc, werr)) {
                    out["ok"] = false;
                    out["error"] = werr;
                    return qst::jsonutil::DumpUtf8Safe(out, 2);
                }
            }
        }
    }

    // ③ 写 windowClients（绑定/解绑）
    const std::wstring cfgPath = AppDir() + L"\\web_ai_config.json";
    json cfg = ReadJsonFileOrNull(cfgPath);
    if (!cfg.is_object()) cfg = json::object();
    std::vector<std::string> ids;
    if (cfg.contains("windowClients") && cfg["windowClients"].is_array()) {
        for (const auto& v : cfg["windowClients"]) {
            if (v.is_string()) ids.push_back(v.get<std::string>());
        }
    }
    auto it = std::find(ids.begin(), ids.end(), id);
    if (unbind) {
        if (it != ids.end()) ids.erase(it);
    } else if (it == ids.end()) {
        ids.push_back(id);
    }
    json arr = json::array();
    for (const auto& s : ids) arr.push_back(s);
    cfg["windowClients"] = arr;
    std::string werr;
    if (!WriteJsonFile(cfgPath, cfg, werr)) {
        out["ok"] = false;
        out["error"] = werr;
        return qst::jsonutil::DumpUtf8Safe(out, 2);
    }
    ReloadUserConfig();
    out["ok"] = true;
    out["client"] = id;
    out["bound"] = !unbind;
    out["windowClients"] = arr;
    out["note"] = "重启（或重进设置）后模型下拉里会出现这一项；"
                  "窗口通道会抢前台，且要先用探针校准几何判据";
    return qst::jsonutil::DumpUtf8Safe(out, 2);
}

std::string RunWindowAiProbeReportJson(const std::string& requestBodyUtf8) {    const json req = json::parse(requestBodyUtf8.empty() ? "{}" : requestBodyUtf8,
        nullptr, false);
    std::string client;
    bool controls = true;
    bool dryRun = true;
    bool doSend = false;
    bool copyProbe = false;
    bool copyScan = false;
    std::wstring scanPrompt;
    std::string text;
    if (req.is_object()) {
        client = JsonStr(req, "client", "");
        controls = JsonBool(req, "controls", true);
        dryRun = JsonBool(req, "dryRun", true);
        doSend = JsonBool(req, "send", false);
        copyProbe = JsonBool(req, "copyProbe", false);
        copyScan = JsonBool(req, "copyScan", false);
        // 标定要用"我们发出去的那句话"来锁定回答锚点（没有它就只能拿最靠下的文本）
        {
            const std::string sp = JsonStr(req, "scanPrompt", "");
            // UTF-8 → UTF-16（⚠ 不能逐字节加宽：中文会变成乱码）
            if (!sp.empty()) {
                const int wn = MultiByteToWideChar(CP_UTF8, 0, sp.c_str(),
                    static_cast<int>(sp.size()), nullptr, 0);
                if (wn > 0) {
                    scanPrompt.resize(static_cast<size_t>(wn));
                    MultiByteToWideChar(CP_UTF8, 0, sp.c_str(), static_cast<int>(sp.size()),
                        scanPrompt.data(), wn);
                }
            }
        }
        text = JsonStr(req, "text", "");
    }
    // 没给 client 时列一遍配置里的客户端 id（探针最容易踩的第一步就是名字写错）
    if (client.empty()) {
        json out;
        out["ok"] = false;
        out["error"] = "缺少 client 参数";
        json ids = json::array();
        for (const auto& c : WindowAiDriver::Instance().Clients()) {
            ids.push_back(json{{"id", c.id}, {"label", c.label},
                               {"modelName", c.id}});
        }
        out["knownClients"] = ids;
        out["hint"] = "用法：{\"client\":\"doubao-app\",\"controls\":true,\"dryRun\":true}";
        return qst::jsonutil::DumpUtf8Safe(out, 2);
    }

    json rep = WindowAiDriver::Instance().ProbeReport(client, controls);
    // ★ 剪贴板通道单独探一次（**不发送任何消息**）：点"复制"→ 读剪贴板。
    //   这是验证"剪贴板优先读回答"最便宜的一步 —— 会话里已有回答，不用真发一轮。
    if (rep.value("ok", false) && copyProbe) {
        json cp;
        try {
            const auto& clients = WindowAiDriver::Instance().Clients();
            const int ci = FindWindowAiClient(clients, client);
            if (ci < 0) {
                cp["ok"] = false;
                cp["why"] = "客户端 id 不认识";
            } else {
                HWND hwnd = reinterpret_cast<HWND>(rep["target"].value("hwnd", 0ULL));
                std::wstring clip;
                std::string why;
                const bool got = WindowAiDriver::Instance().TryClipboardReply(
                    clients[static_cast<size_t>(ci)], hwnd, std::wstring(), clip, why);
                cp["ok"] = got;
                cp["chars"] = static_cast<int>(clip.size());
                cp["head"] = ToUtf8(clip.size() > 400 ? clip.substr(0, 400) : clip);
                if (!why.empty()) cp["why"] = why;
                cp["diag"] = WindowAiDriver::Instance().LastClipboardDiag();
            }
        } catch (const std::exception& e) {
            cp["ok"] = false;
            cp["why"] = std::string("异常：") + e.what();
        } catch (...) {
            cp["ok"] = false;
            cp["why"] = "未知异常";
        }
        rep["copyProbe"] = cp;
    }
    // ★ 标定扫描（`copyScan:true`）：小范围试点找「复制」图标，命中即回报偏移量。
    //   这是**唯一**能把几何落点钉准的办法（豆包客户端不把消息操作栏交给 UIA）。
    if (rep.value("ok", false) && copyScan) {
        json cs;
        try {
            const auto& clients = WindowAiDriver::Instance().Clients();
            const int ci = FindWindowAiClient(clients, client);
            if (ci < 0) {
                cs["ok"] = false;
                cs["why"] = "客户端 id 不认识";
            } else {
                HWND hwnd = reinterpret_cast<HWND>(rep["target"].value("hwnd", 0ULL));
                std::wstring reply;
                POINT pt{};
                std::vector<double> off;
                std::string why;
                std::vector<WindowAiDriver::CopyScanHit> hits;
                const bool found = WindowAiDriver::Instance().ScanCopyButton(
                    clients[static_cast<size_t>(ci)], hwnd, scanPrompt, reply, pt, off, why, &hits);
                cs["ok"] = found;
                if (found) {
                    cs["point"] = json::array({pt.x, pt.y});
                    cs["offset"] = json::array({off[0], off[1]});
                    cs["replyChars"] = static_cast<int>(reply.size());
                    cs["replyHead"] = ToUtf8(reply.size() > 300 ? reply.substr(0, 300) : reply);
                    cs["hint"] = "把 offset 写进 window_ai_providers.json 的 reply.copyButtonOffset";
                    json hs = json::array();
                    for (const auto& h : hits) {
                        hs.push_back(json{{"point", json::array({h.point.x, h.point.y})},
                                         {"offset", json::array({h.offset[0], h.offset[1]})},
                                         {"chars", static_cast<int>(h.text.size())},
                                         {"text", ToUtf8(h.text.size() > 200 ? h.text.substr(0, 200) : h.text)}});
                    }
                    cs["candidates"] = hs;
                } else {
                    cs["why"] = why;
                }
            }
        } catch (const std::exception& e) {
            cs["ok"] = false;
            cs["why"] = std::string("异常：") + e.what();
        } catch (...) {
            cs["ok"] = false;
            cs["why"] = "未知异常";
        }
        rep["copyScan"] = cs;
    }
    // 干跑写入（默认开：探针的主要目的就是回答"能不能把字写进去"）
    if (rep.value("ok", false) && dryRun) {
        const WindowAiDryRun run = WindowAiDriver::Instance().DryRunWrite(client, L"QST探测");
        json w;
        w["focused"] = run.focused;
        w["input"] = ToUtf8(run.inputMatchedBy);
        w["proof"] = ToUtf8(WindowAiWriteProofName(run.write.proof));
        w["channel"] = ToUtf8(run.write.channel);
        w["readBack"] = ToUtf8(run.write.readBack);
        w["clearedAfter"] = run.clearedAfter;
        w["preExistingText"] = ToUtf8(run.preExistingText);
        w["postWriteOcrHead"] = ToUtf8(run.postWriteOcrHead);
        if (!run.ocrError.empty()) w["ocrError"] = run.ocrError;
        json tried = json::array();
        for (const auto& t : run.write.tried) tried.push_back(ToUtf8(t));
        w["tried"] = tried;
        if (!run.error.empty()) w["error"] = run.error;
        if (!run.message.empty()) w["message"] = run.message;
        rep["dryRun"] = w;
    }
    // 可选：真发一条（**会真的发消息**）
    if (rep.value("ok", false) && doSend && !text.empty()) {
        const WindowAiSendOutcome out = WindowAiDriver::Instance().SendAndRead(client,
            FromUtf8(text), Config().idleTimeoutMs, Config().maxTotalMs);
        json s;
        s["ok"] = out.ok;
        s["error"] = out.error;
        s["message"] = out.message;
        s["input"] = ToUtf8(out.inputMatchedBy);
        s["writeProof"] = ToUtf8(WindowAiWriteProofName(out.write.proof));
        s["writeChannel"] = ToUtf8(out.write.channel);
        s["mode"] = ToUtf8(out.mode);
        s["replySource"] = out.replySource;
        s["baselineChars"] = out.baselineChars;
        s["replyChars"] = out.replyChars;
        s["replyHead"] = ToUtf8(out.replyText.substr(0, 400));
        s["activateMs"] = out.activateMs;
        s["focusMs"] = out.focusMs;
        s["writeMs"] = out.writeMs;
        s["submitMs"] = out.submitMs;
        s["waitMs"] = out.waitMs;
        rep["send"] = s;
    }
    return qst::jsonutil::DumpUtf8Safe(rep, 2);
}

}  // namespace quickscript::webai
