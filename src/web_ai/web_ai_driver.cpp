// ──────────────────────────────────────────────────────────────────
// web_ai_driver.cpp — 网页版 AI 驱动层实现（经 ExtBridgeServer 驱动扩展）
//
// 与桥的交互只有一条通道：`ExtBridgeServer::Instance().Request(type, fields, ...)`。
// ⚠ 每次调用都要**串行**（桥只有全局一个 waitingId_），见文件内的 kCallMu。
// ──────────────────────────────────────────────────────────────────

#include "web_ai/web_ai_driver.h"
#include "web_ai/web_ai_prompt.h"   // JsonStr/JsonBool/JsonInt（类型安全读，绝不抛）

#include "window_mode/ext_bridge/ext_bridge_server.h"
#include "utils.h"
#include "window_mode/window_mode_log.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

namespace quickscript::webai {

using json = nlohmann::json;

namespace {

/// ★ 全局串行锁：桥的 `waitingId_` 是**全局唯一**的（ext_bridge_server.cpp:1037），
///   两条并发请求会互相吞回执 ⇒ 表现成"莫名超时"。整个驱动层共用这一把锁。
std::mutex& CallMu() {
    static std::mutex m;
    return m;
}

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/// 把 json 对象转成 `Request()` 要的 `extraJsonFields`（去掉最外层花括号）
std::string Fields(const json& obj) {
    const std::string s = obj.dump();
    if (s.size() >= 2 && s.front() == '{' && s.back() == '}') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

std::string WideToUtf8Safe(const std::wstring& w) {
    return ToUtf8(w);
}

/// 发一条请求并解析回执。回执必须是 JSON 对象，且 `ok` 为 true 才算成功。
namespace {
/// 本线程的会话 key（见 `web_ai_driver.h`）
thread_local std::string t_sessionKey;
}  // namespace

bool RequestJson(const std::string& type, const json& fields, int timeoutMs,
                 json& out, std::string& error, std::string& message) {
    error.clear();
    message.clear();
    std::lock_guard<std::mutex> lock(CallMu());
    auto& bridge = windowmode::ExtBridgeServer::Instance();
    // ★★ 离线时**不是直接报错** —— 先让桥去尝试"一键式"启动浏览器，再等它连上。
    //
    // ⚠⚠ 2026-09-26 真机：我上一轮把"启动浏览器"加在了 `ExtBridgeServer::WaitForExtension` 里，
    //   但**这条路径压根不走 `WaitForExtension`** —— 它只看一眼 `IsExtensionConnected()`
    //   就返回 `NO_EXTENSION` ⇒ 用户看到"软件已经自动尝试把浏览器拉起来"，
    //   而**软件其实什么都没做**（文案与实际行为不符，比不写还糟）。
    //   ⇒ 离线时必须**走同一个入口**（`WaitForExtension` 里含限流的启动逻辑）。
    if (!bridge.IsExtensionConnected()) {
        std::wstring werr;
        // 首次会真的去拉浏览器（内部限流 60s），冷启动 + SW 起来 + 连桥要时间
        bridge.WaitForExtension(3000, werr);
    }
    if (!bridge.IsExtensionConnected()) {
        error = "NO_EXTENSION";
        message = "浏览器扩展没有连上本机桥";
        return false;
    }
    // ★ 桥请求的**入口/出口**都留痕：万一进程在桥里硬崩（无日志、无退出码），
    //   日志的最后一条就能指出"卡在哪个 type 上"（2026-09-25 闪退排查加的）。
    windowmode::WindowModeLogEventf(L"[网页AI] 桥请求 → %s", FromUtf8(type).c_str());
    // ★ 每个 webAi* 请求都带上会话 key —— 扩展据此**给每个会话分配自己的标签页**。
    //   不区分会话时两个 UI 会话会共用同一个网页对话（"对话被打到别处"）。
    json fieldsWithKey = fields;
    if (!fieldsWithKey.is_object()) fieldsWithKey = json::object();
    if (!t_sessionKey.empty()) fieldsWithKey["sessionKey"] = t_sessionKey;

    std::string result;
    std::wstring err;
    if (!bridge.Request(type, Fields(fieldsWithKey), result, err, timeoutMs)) {
        // ★★ 别用通用文案盖掉扩展的具体原因（2026-09-26 真机）：
        //   用户看到的是 "扩展桥请求失败: webAiPickProvider" —— **完全不知道发生了什么**，
        //   而扩展其实说了「没找到可用的网页 AI：豆包 / DeepSeek / 元宝 都没有已登录的页面」。
        //   桥把「扩展的 error 码」与「扩展的 message」用 ": " 拼在 `err` 里带回来 ⇒ 拆开。
        // ★★ 桥虽然报失败，但 `result` 里**可能带着扩展的完整回执**（`ok:false` + `steps` 诊断）。
        //   不解析它 ⇒ 调用方拿不到诊断 ⇒ 「传图诊断」这类日志**永远不写**
        //   （2026-09-26 真机：日志里一条诊断都没有，就是这个原因）。
        //   ⚠ `RequestOnSock` 在回执 `ok:false` 时**已经**把 `resultJson` 填好了，白白丢掉可惜。
        {
            const json parsed = json::parse(result, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object()) out = parsed;
        }
        const std::string full = WideToUtf8Safe(err);
        const size_t sep = full.find(": ");
        if (sep != std::string::npos && sep > 0 && sep < 64) {
            error = full.substr(0, sep);
            message = full.substr(sep + 2);
        } else {
            error = full;
            if (error.empty()) error = "REQUEST_FAILED";
            message = "扩展桥请求失败：" + type;
        }
        // ★★ 兜底文案**必须带上扩展自己说的原因**（2026-09-29 用户报障）：
        //   实测网页桥打字那一步失败，用户看到的只有 `扩展桥发送失败 / webAiRichType` ——
        //   而扩展其实回了 `error` 码（如 NO_PROVIDER / NO_TAB / PREPARE_FAILED）与 `message`，
        //   甚至 `steps` 诊断。上一步已经把回执解析进 `out` 了，这里别再丢。
        if (out.is_object()) {
            const std::string extErr = out.value("error", std::string());
            const std::string extMsg = out.value("message", std::string());
            if (!extErr.empty()) message += "（扩展 error=" + extErr;
            else message += "（扩展未给 error";
            if (!extMsg.empty()) {
                message += "，message=" + (extMsg.size() > 200 ? extMsg.substr(0, 200) : extMsg);
            }
            // ★ 连**子步骤**一起带上（2026-09-29）：`handleWebAiRichType` 失败时会回
            //   `steps{prepare/verify/fallbackKeyEvents…}` —— 光有 error 码只能知道
            //   "打字失败"，有 steps 才知道**卡在哪一小步**（找输入框？取焦点？回读？）。
            if (out.contains("steps") && out["steps"].is_object()) {
                std::string compact;
                for (auto it = out["steps"].begin(); it != out["steps"].end(); ++it) {
                    if (!compact.empty()) compact += ",";
                    compact += it.key();
                    if (it.value().is_object() && it.value().contains("error")
                        && it.value()["error"].is_string()) {
                        std::string e = it.value()["error"].get<std::string>();
                        if (e.size() > 60) e = e.substr(0, 60);
                        compact += ":" + e;
                    }
                }
                if (compact.size() > 220) compact = compact.substr(0, 220) + "…";
                if (!compact.empty()) message += "，steps=" + compact;
            }
            message += "）";
            if (!error.empty() && error != extErr && !extErr.empty()) {
                // 让上层也能按扩展的错误码分支（比通用 REQUEST_FAILED 有用得多）
                error = extErr;
            }
        } else {
            // ★★ **桥层失败：连回执都没有**（2026-09-30 实测）。
            //   末轮只有 `write_ms=1, submit_ms=0` 和命令名 `webAiRichType` ——
            //   谁也看不出是"WS 断了""超时"还是"扩展没回话"。
            //   `err` 里带着桥自己的说明（连接状态/超时/解析失败），一并报出去。
            message += "（桥层无回执：err=" + full + "）";
        }
        windowmode::WindowModeLogEventf(L"[网页AI] 桥请求失败 type=%s err=%s msg=%s",
            std::wstring(type.begin(), type.end()).c_str(), err.c_str(),
            FromUtf8(message).c_str());
        return false;
    }
    windowmode::WindowModeLogEventf(L"[网页AI] 桥请求 ← %s（回执 %d 字节）",
        FromUtf8(type).c_str(), static_cast<int>(result.size()));
    json parsed = json::parse(result, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        error = "BAD_RESULT_JSON";
        message = "扩展回执不是合法 JSON：" + result.substr(0, 200);
        return false;
    }
    out = std::move(parsed);

    // ★★ 字段类型不符时**必须记账 + 带上回执原文**。
    //   2026-09-25 真机事故：扩展回执里 `error` 是 boolean，`out.value("error", std::string(...))`
    //   抛 `type_error.302` ⇒ 异常逃出桥的 HTTP 线程 ⇒ **整个软件瞬间消失，连一行线索都没有**。
    //   现在改成：类型不符 ⇒ 用默认值继续（不抛），并把「哪个字段、实际是什么类型、回执原文」
    //   一起写进日志 —— 否则下次还是要靠猜。
    std::string mmOne;
    std::string mmAll;
    auto noteMm = [&mmAll, &mmOne]() {
        if (mmOne.empty()) return;
        if (!mmAll.empty()) mmAll += "；";
        mmAll += mmOne;
    };

    const bool extOk = JsonBool(out, "ok", false, &mmOne);
    noteMm();
    if (!extOk) {
        // 扩展侧的 error/message 原样上报（不要猜）
        error = JsonStr(out, "error", "EXT_ERROR", &mmOne);
        noteMm();
        message = JsonStr(out, "message", std::string(), &mmOne);
        noteMm();
        if (error.empty()) error = "EXT_ERROR";

        // ★★ `UNKNOWN: <type>` = **浏览器里跑的是旧版扩展**（不认识这个消息）。
        //   这是最容易误判成"功能坏了"的一种失败：用户看到的是
        //   `UNKNOWN: webAiPickProvider`，完全不知道要干什么。
        //   ⚠ 这个翻译必须放在**这一层（唯一咽喉）**：所有 webAi* 请求都会经过它，
        //     漏在某个调用点单独处理，下次加新消息又会踩（本仓 §39 的教训）。
        //   根因几乎总是：扩展改了但没点 `edge://extensions/` 的「重新加载」⟳
        //   —— 浏览器有自己的一份内存实例，文件更新了不点就不换。
        if (error == "UNKNOWN" && message.rfind("webAi", 0) == 0) {
            error = "EXT_TOO_OLD";
            message = "浏览器里的扩展是**旧版本**：它不认识「" + message + "」这条消息。"
                      "请到 edge://extensions/ 点这个扩展的「重新加载」⟳，"
                      "然后回到网页按 F5，再重试。";
            windowmode::WindowModeLogEventf(
                L"[网页AI] 扩展不认识消息 %s ⇒ 判定为扩展旧版（需重载）",
                FromUtf8(message).c_str());
        }
        if (!mmAll.empty()) {
            std::wstring raw = FromUtf8(result);
            if (raw.size() > 400) raw = raw.substr(0, 400) + L"…";
            windowmode::WindowModeLogEventf(
                L"[网页AI] ⚠ 回执字段类型异常（%s）；回执原文：%s",
                FromUtf8(mmAll).c_str(), raw.c_str());
        }
        return false;
    }
    if (!mmAll.empty()) {
        std::wstring raw = FromUtf8(result);
        if (raw.size() > 400) raw = raw.substr(0, 400) + L"…";
        windowmode::WindowModeLogEventf(
            L"[网页AI] ⚠ 回执字段类型异常（%s）；回执原文：%s",
            FromUtf8(mmAll).c_str(), raw.c_str());
    }
    return true;
}

}  // namespace

// ★ 这两个是**对外接口**（`web_ai_driver.h` 里声明在 `quickscript::webai`）
//   ⇒ 必须放在**匿名命名空间之外**：放在里面是内部链接 ⇒ 别的 TU 链不上（LNK2019）。
void SetCurrentSessionKey(const std::string& keyUtf8) {
    t_sessionKey = keyUtf8;
}

std::string CurrentSessionKey() {
    return t_sessionKey;
}

int CurrentBridgePort() {
    return windowmode::ExtBridgeServer::Instance().Port();
}

WebAiDriver& WebAiDriver::Instance() {
    static WebAiDriver inst;
    return inst;
}

bool WebAiDriver::ExtensionOnline() {
    return windowmode::ExtBridgeServer::Instance().IsExtensionConnected();
}

bool WebAiDriver::ListProviders(std::vector<WebAiProviderInfo>& out, std::string& error,
                                int timeoutMs, std::string* extVersionOut) {
    out.clear();
    if (extVersionOut) extVersionOut->clear();
    json r;
    std::string message;
    if (!RequestJson("webAiProviders", json::object(), timeoutMs, r, error, message)) return false;
    if (extVersionOut && r.contains("version") && r["version"].is_string()) {
        *extVersionOut = r["version"].get<std::string>();
    }
    if (!r.contains("providers") || !r["providers"].is_array()) {
        error = "BAD_PROVIDERS";
        message = "回执里没有 providers 数组";
        return false;
    }
    for (const auto& p : r["providers"]) {
        if (!p.is_object()) continue;
        WebAiProviderInfo info;
        info.id = JsonStr(p, "id", "");
        info.label = JsonStr(p, "label", "");
        info.url = JsonStr(p, "url", "");
        info.urlHint = JsonStr(p, "urlHint", "");
        info.inputKind = JsonStr(p, "inputKind", "");
        if (!info.id.empty()) out.push_back(std::move(info));
    }
    if (out.empty()) {
        error = "NO_PROVIDERS";
        message = "站点列表为空 ⇒ 扩展没读到 web_ai_providers.json";
        return false;
    }
    return true;
}

bool WebAiDriver::ListPages(std::vector<WebAiPageInfo>& out, std::string& error, int timeoutMs) {
    out.clear();
    json r;
    std::string message;
    json f = json::object();
    f["titleHint"] = "";
    if (!RequestJson("listPages", f, timeoutMs, r, error, message)) return false;

    // ⚠⚠ 扩展返回的是**列式数组**：`{ok, count, tabIds[], titles[], urls[], windowIds[], source}`
    //   —— **不是**对象数组。实测踩到：按对象数组解析会得到空列表并报 BAD_PAGES，
    //   于是错误信息看起来像"拿不到页面"，真实原因却是"形状对不上"。两种形状都认。
    if (r.contains("tabIds") && r["tabIds"].is_array()) {
        const json& ids = r["tabIds"];
        const json* titles = (r.contains("titles") && r["titles"].is_array()) ? &r["titles"] : nullptr;
        const json* urls = (r.contains("urls") && r["urls"].is_array()) ? &r["urls"] : nullptr;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (!ids[i].is_number_integer()) continue;
            const int tabId = ids[i].get<int>();
            if (tabId <= 0) continue;
            WebAiPageInfo info;
            info.tabId = tabId;
            if (titles && i < titles->size() && (*titles)[i].is_string())
                info.title = (*titles)[i].get<std::string>();
            if (urls && i < urls->size() && (*urls)[i].is_string())
                info.url = (*urls)[i].get<std::string>();
            out.push_back(std::move(info));
        }
        return true;
    }

    // 兜底：对象数组形状（若将来扩展改成 `pages:[{tabId,title,url}]` 也照样能用）
    const json* arr = nullptr;
    if (r.contains("pages") && r["pages"].is_array()) arr = &r["pages"];
    else if (r.contains("targets") && r["targets"].is_array()) arr = &r["targets"];
    if (!arr) {
        error = "BAD_PAGES";
        message = "回执里既没有列式 tabIds[]，也没有 pages[] 数组";
        return false;
    }
    for (const auto& p : *arr) {
        if (!p.is_object()) continue;
        WebAiPageInfo info;
        info.tabId = JsonInt(p, "tabId", 0);
        info.title = JsonStr(p, "title", "");
        info.url = JsonStr(p, "url", "");
        out.push_back(std::move(info));
    }
    return true;
}

bool WebAiDriver::FindProviderPages(const std::string& providerId,
                                    std::vector<WebAiPageInfo>& out, std::string& error,
                                    int timeoutMs) {
    out.clear();
    json f = json::object();
    f["provider"] = providerId;
    json r;
    std::string message;
    if (!RequestJson("webAiFindPage", f, timeoutMs, r, error, message)) return false;
    const json* arr = nullptr;
    if (r.contains("pages") && r["pages"].is_array()) arr = &r["pages"];
    if (!arr) {
        error = "BAD_PAGES";
        message = "回执里没有 pages 数组";
        return false;
    }
    for (const auto& p : *arr) {
        if (!p.is_object()) continue;
        WebAiPageInfo info;
        info.tabId = JsonInt(p, "tabId", 0);
        info.title = JsonStr(p, "title", "");
        info.url = JsonStr(p, "url", "");
        if (info.tabId > 0) out.push_back(std::move(info));
    }
    return true;
}

bool WebAiDriver::EnsureOwnTab(const std::string& providerId, int& tabIdOut, bool& createdOut,
                               bool& attachedOut, std::string& error, std::string& message,
                               int timeoutMs) {
    tabIdOut = 0;
    createdOut = false;
    attachedOut = false;
    error.clear();
    message.clear();

    json f = json::object();
    f["provider"] = providerId;
    json r;
    // ⚠ 超时给足：没有标签页时要新建 + 挂调试会话，站点首次加载可能慢。
    if (!RequestJson("webAiEnsureTab", f, timeoutMs, r, error, message)) {
        windowmode::WindowModeLogEventf(L"[网页AI] 取专属标签页失败 code=%s",
            FromUtf8(error).c_str());
        return false;
    }
    tabIdOut = JsonInt(r, "tabId", 0);
    createdOut = JsonBool(r, "created", false);
    attachedOut = JsonBool(r, "attached", false);
    if (tabIdOut <= 0) {
        error = "NO_TAB";
        message = "扩展没能给出站点标签页";
        return false;
    }
    ownTabId_ = tabIdOut;
    windowmode::WindowModeLogEventf(L"[网页AI] 专属标签页：tab=%d（%s，调试会话=%s）",
        tabIdOut, createdOut ? L"新建" : L"复用", attachedOut ? L"已挂" : L"未挂");
    // 页面可见性诊断（见扩展侧 keepTargetAlive 的注释：hidden ⇒ 站点不写 DOM）
    if (r.contains("pageState") && r["pageState"].is_object()) {
        const auto& ps = r["pageState"];
        windowmode::WindowModeLogEventf(L"[网页AI] 页面可见性：%s（hidden=%d focus=%d）",
            FromUtf8(JsonStr(ps, "visibility", "")).c_str(),
            JsonBool(ps, "hidden", false) ? 1 : 0, JsonBool(ps, "hasFocus", false) ? 1 : 0);
    }
    if (!attachedOut) {
        const std::string ae = JsonStr(r, "attachError");
        if (!ae.empty()) {
            windowmode::WindowModeLogEventf(L"[网页AI] 挂调试会话失败：%s",
                FromUtf8(ae).c_str());
        }
    }
    return true;
}

bool WebAiDriver::StartNewChat(const std::string& providerId, int& tabIdOut, std::string& error,
                               std::string& message, int timeoutMs) {
    windowmode::WindowModeLogEventf(L"[网页AI] 开新对话：进入（provider=%s）",
        FromUtf8(providerId).c_str());
    tabIdOut = 0;
    error.clear();
    message.clear();
    json f = json::object();
    f["provider"] = providerId;
    json r;
    if (!RequestJson("webAiNewChat", f, timeoutMs, r, error, message)) {
        windowmode::WindowModeLogEventf(L"[网页AI] 开新对话失败 code=%s",
            FromUtf8(error).c_str());
        return false;
    }
    tabIdOut = JsonInt(r, "tabId", 0);
    windowmode::WindowModeLogEventf(L"[网页AI] 已导航到新对话（tab=%d，status=%s）",
        tabIdOut, FromUtf8(JsonStr(r, "status", "")).c_str());
    return true;
}

bool WebAiDriver::PickProvider(WebAiPickResult& out, int timeoutMs) {
    out = WebAiPickResult{};
    json r;
    std::string message;
    // ⚠ 请求里不带 provider ⇒ 让扩展自己按内置顺序挑
    if (!RequestJson("webAiPickProvider", json::object(), timeoutMs, r, out.error, message)) {
        out.message = message;
        if (r.is_object() && r.contains("tried")) out.tried = r["tried"];
        windowmode::WindowModeLogEventf(L"[网页AI] 自动挑站点失败 code=%s",
            FromUtf8(out.error).c_str());
        return false;
    }
    const json picked = r.contains("picked") && r["picked"].is_object() ? r["picked"] : json();
    out.provider = JsonStr(picked, "provider", "");
    out.tabId = JsonInt(picked, "tabId", 0);
    out.reason = JsonStr(picked, "reason", "");
    if (r.contains("tried")) out.tried = r["tried"];
    // ⚠ 只要求 provider 非空：`lastOk`（用上次成功的站点）分支**不带 tabId**
    //   —— 宿主自己会用 `EnsureOwnTab` 拿到该站点的专属标签页，不需要 picked.tabId。
    if (out.provider.empty()) {
        out.error = "BAD_PICK";
        out.message = "扩展回执里没有可用的站点";
        return false;
    }
    out.ok = true;
    windowmode::WindowModeLogEventf(L"[网页AI] 自动挑中站点 %s（%s，tab=%d）",
        FromUtf8(out.provider).c_str(),
        out.reason == "open" ? L"本来开着" : L"探测到可用", out.tabId);
    return true;
}

bool WebAiDriver::ActivateTab(const std::string& providerId, int tabId, int& prevTabIdOut,
                              std::string& error, int timeoutMs) {
    windowmode::WindowModeLogEventf(L"[网页AI] 前台化：进入（tab=%d）", tabId);
    prevTabIdOut = 0;
    error.clear();
    json f = json::object();
    f["provider"] = providerId;
    if (tabId > 0) f["tabId"] = tabId;
    json r;
    std::string message;
    if (!RequestJson("webAiActivateTab", f, timeoutMs, r, error, message)) return false;
    prevTabIdOut = JsonInt(r, "prevTabId", 0);
    if (JsonBool(r, "alreadyActive", false)) {
        prevTabIdOut = 0;   // 本来就在前台 ⇒ 不需要还
        return true;
    }
    return true;
}

bool WebAiDriver::RestoreTab(int prevTabId, int ourTabId, std::string& error, int timeoutMs) {
    windowmode::WindowModeLogEventf(L"[网页AI] 还回前台：进入（prev=%d our=%d）",
        prevTabId, ourTabId);
    error.clear();
    if (prevTabId <= 0) return true;   // 没记录到原标签页，无需还
    json f = json::object();
    f["prevTabId"] = prevTabId;
    f["tabId"] = ourTabId;
    json r;
    std::string message;
    if (!RequestJson("webAiRestoreTab", f, timeoutMs, r, error, message)) return false;
    return true;
}

bool WebAiDriver::EnsureAttached(const std::string& providerId, std::string& error,
                                 int timeoutMs, bool* createdOut) {
    // ★★ 目标 = **我们自己的**那个标签页（没有就让扩展开一个），并把网页 AI 的
    //   **独立调试会话**挂上去。
    //
    //   ⚠⚠ 两条都不能退回去（2026-09-25 真机事故）：
    //   ① **不能**「取第一个 URL 匹配的页面」—— 那往往正是用户自己正在跟豆包聊的那个
    //      对话 ⇒ 模型能看到用户的私人历史，我们的请求还会被追加进用户的主对话。
    //   ② **不能**走全局的 `attach` 消息 —— `attachTab()` 开头就 `await detachDebugger()`，
    //      会把**正在跑的脚本**的调试会话拆掉（用户实测原话：「扩展桥自动关闭调试了」）。
    //      网页 AI 的调试会话由 `webAiEnsureTab` 单独挂（扩展侧 `webAiDebuggee`）。
    int tabId = 0;
    bool created = false;
    bool attached = false;
    std::string message;
    if (!EnsureOwnTab(providerId, tabId, created, attached, error, message, 30000)) {
        if (message.empty()) message = "取站点专属标签页失败";
        return false;
    }
    if (!attached) {
        error = "DEBUGGER_DENIED";
        return false;
    }
    if (createdOut) *createdOut = created;
    return true;
}

bool WebAiDriver::Probe(const std::string& providerId, json& probeOut, std::string& error,
                        int timeoutMs) {
    json f = json::object();
    f["provider"] = providerId;
    json r;
    std::string message;
    if (!RequestJson("webAiProbe", f, timeoutMs, r, error, message)) return false;
    probeOut = r.contains("probe") ? r["probe"] : json::object();
    return true;
}

bool WebAiDriver::TypeRich(const std::string& providerId, const std::string& text,
                           json& stepsOut, std::string& error, std::string& message,
                           int timeoutMs) {
    json f = json::object();
    f["provider"] = providerId;
    f["text"] = text;
    json r;
    if (!RequestJson("webAiRichType", f, timeoutMs, r, error, message)) {
        if (r.is_object() && r.contains("steps")) stepsOut = r["steps"];
        return false;
    }
    stepsOut = r.contains("steps") ? r["steps"] : json::object();
    return true;
}

bool WebAiDriver::Submit(std::string& error, std::string& message, int timeoutMs) {
    json r;
    if (!RequestJson("webAiSubmit", json::object(), timeoutMs, r, error, message)) return false;
    return true;
}

bool WebAiDriver::UploadImages(const std::string& providerId,
                               const std::vector<std::wstring>& paths,
                               json& detailOut, std::string& error, std::string& message,
                               int timeoutMs) {
    detailOut = json::object();
    if (paths.empty()) {
        error = "NO_FILES";
        message = "没有要上传的图片";
        return false;
    }
    json arr = json::array();
    for (const auto& p : paths) {
        if (p.empty()) continue;
        arr.push_back(WideToUtf8Safe(p));
    }
    if (arr.empty()) {
        error = "NO_FILES";
        message = "图片路径全为空";
        return false;
    }

    json f = json::object();
    f["provider"] = providerId;
    f["files"] = arr;
    json r;
    // ⚠ 超时给足：路 B 要先等 3s 的文件选择器事件，再挂文件，再回读。
    if (!RequestJson("webAiUploadImages", f, timeoutMs, r, error, message)) {
        if (r.is_object() && r.contains("steps")) detailOut = r["steps"];
        windowmode::WindowModeLogEventf(L"[网页AI] 传图失败 code=%s（%d 张）",
            FromUtf8(error).c_str(), static_cast<int>(arr.size()));
        // ★★ 诊断必须落日志：`tried`（逐个 input 的结果）+ `thumbsBefore/After`（缩略图数）
        //    + `listBefore/After`（页面上所有 file input 的概要）是区分
        //    「选错 input」与「站点读走就清空」的**唯一**依据。
        //    2026-09-26 真机就是靠这个才没在"猜 selector"上浪费时间。
        if (detailOut.is_object() && !detailOut.empty()) {
            std::wstring d = FromUtf8(detailOut.dump());
            if (d.size() > 900) d = d.substr(0, 900) + L"…";
            windowmode::WindowModeLogEventf(L"[网页AI] 传图诊断：%s", d.c_str());
        }
        return false;
    }
    detailOut = r.contains("steps") ? r["steps"] : json::object();
    windowmode::WindowModeLogEventf(L"[网页AI] 传图成功：%d 张（via=%s，回读 %d）",
        static_cast<int>(arr.size()),
        FromUtf8(JsonStr(r, "via", "")).c_str(), JsonInt(r, "verifiedCount", 0));
    // ★★★ 成功时也要打诊断（2026-10-02 用户报"传两张一模一样的"）：
    //   `via=direct#0+thumb` 只能说明"挂了 1 张"，**分不清**是：
    //     · **累积**（上一轮那张还在对话历史里）
    //     · **残留**（输入框附件提交后没清空）
    //   ⇒ 必须看 `steps.thumbsBefore`（**上传前**页面上已有几张缩略图）：
    //     `thumbsBefore > 0` ⇒ **残留**；`thumbsBefore == 0` 但仍两张 ⇒ **累积** ✓
    //   ⚠ 不再靠猜（见 LESSONS §76）。
    if (r.is_object() && r.contains("steps")) {
        std::string d = r["steps"].dump();
        if (d.size() > 700) d = d.substr(0, 700) + "...";
        windowmode::WindowModeLogEventf(L"[网页AI] 传图诊断：%s", FromUtf8(d).c_str());
    }
    return true;
}

bool WebAiDriver::ReadReply(const std::string& providerId, int idleTimeoutMs, int maxTotalMs,
                            WebAiReplyInfo& out, std::string& error, int timeoutMs) {
    json f = json::object();
    f["provider"] = providerId;
    f["progressIdleTimeoutMs"] = idleTimeoutMs;
    f["maxGenerationTimeoutMs"] = maxTotalMs;
    json r;
    std::string message;
    if (!RequestJson("readAssistantReply", f, timeoutMs, r, error, message)) {
        out.error = error;
        out.message = message;
        return false;
    }
    out.ok = true;
    out.text = JsonStr(r, "text", "");
    out.textLength = JsonInt(r, "textLength", out.text.size());
    out.truncated = JsonBool(r, "truncated", false);
    out.blocks = JsonInt(r, "totalBlocks", 0);
    out.provider = JsonStr(r, "provider", providerId);
    out.matchedBy = JsonStr(r, "matchedBy", "");
    out.elapsedMs = JsonInt(r, "elapsedMs", 0);
    // ★ 页面自己忙吗（扩展的 busySelectors 命中）—— 只如实接住，不改判据语义
    out.pageBusy = JsonBool(r, "busy", false);
    // ★★ **把"这次靠哪个选择器命中 / 页面忙不忙"落盘**（2026-10-01）。
    //
    //   为什么需要：给豆包写准 `responseSelectors` / `busySelectors` 必须知道**实际命中的那条**，
    //   而这两个字段此前只活在回执 JSON 里、从不写日志 ⇒ 只能靠猜选择器（正是我不肯做的）。
    //   一行、每次读回答打一次就能看清两件事：
    //     · `matchedBy` 命中的是**助手回答块**还是**用户气泡**（回显的根源就在这儿）；
    //     · `busy` 有没有命中（没命中 ⇒ 页面自忙时阈值不放宽 ⇒ 25 秒就误判"没提交"）。
    windowmode::WindowModeLogEventf(
        L"[网页AI] 读回答：matchedBy=%s busy=%d 块=%d 字=%d%s",
        FromUtf8(out.matchedBy.empty() ? std::string("(空)") : out.matchedBy).c_str(),
        out.pageBusy ? 1 : 0, out.blocks, out.textLength,
        out.truncated ? L"（已截断）" : L"");
    return true;
}

WebAiSendOutcome WebAiDriver::SendAndRead(const std::string& providerId, const std::string& text,
                                          int idleTimeoutMs, int maxTotalMs,
                                          bool newConversation, bool activateTab) {
    WebAiSendOutcome out;
    const long long t0 = NowMs();

    if (text.empty()) {
        out.error = "EMPTY_PROMPT";
        out.message = "要写进网页输入框的文本为空";
        return out;
    }
    if (!ExtensionOnline()) {
        out.error = "NO_EXTENSION";
        out.message = "浏览器扩展没有连上本机桥";
        return out;
    }

    // ── ① 拿**我们自己的**标签页 + 挂**独立**调试会话 ────────────────
    //   ★ 用户不需要自己开页面：没有属于本软件的标签页时扩展会新建一个
    //     （同一个浏览器 profile ⇒ 登录态共享，不用重新登录）。
    //   ⚠ 只重试**一次**：新建的标签页要时间加载；失败就如实报错，绝不循环（会抢焦点 + 拖时间）。
    const long long tAttach = NowMs();
    // ★ 收"这次是不是新建了标签页" —— 新建 = 网页对话是空的 ⇒ 上下文已丢
    bool tabCreated = false;
    bool attached = EnsureAttached(providerId, out.error, 20000, &tabCreated);
    if (!attached) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1800));
        tabCreated = false;
        attached = EnsureAttached(providerId, out.error, 20000, &tabCreated);
    }
    if (!attached) {
        if (out.message.empty()) {
            out.message = "没能拿到站点标签页或挂上调试会话"
                          "（请在浏览器里确认已登录该站点，然后重试）";
        }
        return out;
    }
    out.attachMs = static_cast<int>(NowMs() - tAttach);
    // ★★ 协议瘦身的兜底（2026-10-02 用户规格）：专属标签页被**重建** ⇒
    //   网页端上下文整个没了 ⇒ 后端据此**清零会话状态** ⇒ 下一轮全量重发重建上下文。
    //   ⚠ 注意：`newConversation`（主动开新对话）也算"上下文是干净的"，但它本来就会重发全量
    //     （`turns==0`）⇒ 这里只需要覆盖"被动重建"这一种。
    out.tabRecreated = tabCreated;

    // ── ①b 新对话：把我们自己的标签页导航回站点入口，拿**干净上下文** ──
    //   ⚠ 顺序：必须在 attach **之后**（attach 要先建立会话）、在写文字**之前**。
    //   ⚠ 导航会换掉 document ⇒ 导航完要重新 ensure（扩展侧会复用同一个 tabId）。
    //   ⚠ 失败**不致命**：至少还能在我们自己的标签页里继续，只是会带着上一轮上下文。
    if (newConversation) {
        int ncTab = 0;
        std::string nerr;
        std::string nmsg;
        if (StartNewChat(providerId, ncTab, nerr, nmsg, 30000)) {
            std::string aerr;
            if (!EnsureAttached(providerId, aerr)) {
                out.error = aerr.empty() ? "ATTACH_AFTER_NEWCHAT" : aerr;
                out.message = "新对话已打开，但重新建立会话失败";
                return out;
            }
        } else {
            windowmode::WindowModeLogEventf(L"[网页AI] 开新对话失败（%s），沿用当前会话",
                FromUtf8(nerr).c_str());
        }
    }

    // ── ①c 临时把我们那个标签页切到前台（**读回答需要它真的渲染**）────
    //   为什么必须（用户 2026-09-25 实测）：后台标签页 `visibilityState === 'hidden'`
    //   ⇒ Chrome 冻结/节流渲染 ⇒ 站点的流式回答**不往 DOM 写** ⇒ 等半天读不到回复。
    //   扩展侧的 `keepTargetAlive`（focus 模拟 + lifecycle active）只能缓解，
    //   **改不了 visibilityState**，所以前台化是唯一可靠手段。
    //
    //   ⚠ 用一个 RAII 守卫：保证**任何** return 路径（含中途报错）都会把前台还回去。
    //   ⚠ 还的时候只在我们那个仍是活动标签页时才还 —— 用户中途自己切走了就别抢回来。
    const int ourTab = LastOwnTabId();
    int prevTabId = 0;
    struct ForegroundGuard {
        WebAiDriver* self = nullptr;
        int prev = 0;
        int ours = 0;
        bool armed = false;
        ~ForegroundGuard() {
            if (!armed || !self) return;
            std::string e;
            self->RestoreTab(prev, ours, e, 15000);
        }
    } fg;
    if (activateTab && ourTab > 0) {
        std::string aerr;
        if (ActivateTab(providerId, ourTab, prevTabId, aerr, 15000) && prevTabId > 0) {
            fg.self = this;
            fg.prev = prevTabId;
            fg.ours = ourTab;
            fg.armed = true;
            windowmode::WindowModeLogEventf(
                L"[网页AI] 已临时前台化 tab=%d（读完自动还回 tab=%d）", ourTab, prevTabId);
        }
    }

    // ── ② 基线：先读一次「当前最后一条回答」────────────────────────
    //   ⚠⚠ 不做这一步就会**读到旧回答**：`readAssistantReply` 返回的是页面上最后
    //     一条助手消息；刚提交完、新回答还没出现时，那还是**上一轮**的内容，
    //     而它已经"内容稳定且不忙"⇒ 扩展会立刻返回它，看起来一切正常。
    //     这是本方案最危险的静默错误（模型会以为自己说的和实际发出去的不是一回事）。
    {
        WebAiReplyInfo base;
        std::string berr;
        if (ReadReply(providerId, 2500, 6000, base, berr, 30000)) {
            out.baselineBlocks = base.blocks;
            out.baselineText = base.text;
        } else {
            windowmode::WindowModeLogEventf(L"[网页AI] 基线读取失败（%s）——按「页面上没有回答」继续",
                FromUtf8(berr).c_str());
        }
    }

    // ── ③ 写入 ──────────────────────────────────────────────────────
    const long long tWrite = NowMs();
    json steps;
    std::string werr;
    std::string wmsg;
    if (!TypeRich(providerId, text, steps, werr, wmsg, 60000)) {
        out.error = werr.empty() ? "WRITE_FAILED" : werr;
        out.message = wmsg;
        out.writeMs = static_cast<int>(NowMs() - tWrite);
        return out;
    }
    out.writeMs = static_cast<int>(NowMs() - tWrite);

    // ── ④ 提交 ──────────────────────────────────────────────────────
    const long long tSubmit = NowMs();
    std::string serr;
    std::string smsg;
    if (!Submit(serr, smsg, 30000)) {
        out.error = serr.empty() ? "SUBMIT_FAILED" : serr;
        out.message = smsg;
        out.submitMs = static_cast<int>(NowMs() - tSubmit);
        return out;
    }
    out.submitMs = static_cast<int>(NowMs() - tSubmit);

    // ── ⑤ 等**新**回答出现 ─────────────────────────────────────────
    //   判据 = 「块数增加」或「文本变了」。只看 ok 是不够的（见 ②）。
    //   ⚠ 每次「还是旧回答」之后必须**睡一下**：旧回答是"内容稳定且不忙" ⇒
    //     扩展会**立刻**返回它（毫秒级）。不加延时就变成热循环，用几百次
    //     WS 往返把扩展的 service worker 打爆（而它正是全链路唯一的执行者）。
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const long long tWait = NowMs();
    const long long deadline = tWait + (std::max)(5000, maxTotalMs);
    std::string stableText;
    int stableCount = 0;
    bool sawNew = false;
    // ★★ **等待期心跳**（2026-09-29）：每 5s 打一行"在等谁、等到哪一步了"。
    //   起因（用户报障）「我这边（网页端）返回了，可软件还显示等待中，不知道什么情况」——
    //   旧实现等待期**一个字都不打**，卡住时完全无法区分：
    //     · 模型还没生成（页面忙）？ · 我们没读到新回答（NO_RESPONSE_NODE）？
    //     · 读到了但判"不稳定/协议半截"（stableCount=1 或 protocolOpen）？
    //   这三者的处置完全不同，所以心跳必须把它们**分开报**。
    long long lastBeat = 0;
    // ★★ **提交后 N 秒没开始生成 ⇒ 立刻如实失败**（2026-09-30）。
    //
    // 起因（用户报障）：「网页 Agents 都回复了还在等」/「卡死在这里」——
    //   页面明明答完了（或根本没答），我们却一路等到 90~180 秒才超时。
    // 根因：扩展**本来就会快速报** `NO_RESPONSE_NODE`（"等待回答容器超时"），
    //   但驱动把它当"新回答还没冒出来，正常"**吞掉继续等** ⇒ 快速失败被外层循环吃掉了。
    // ⇒ 给"没开始生成"一个**上限**：超过就带原因失败，把 90~180 秒变成 ~25 秒，
    //   并把"该看哪儿"直接写进错误里（提交没生效 / 选择器过期 / 标签页被切走）。
    // ⚠ 25 秒的选取：实测网页端从提交到出现回答容器要 8~20 秒（慢站点更久），
    //   卡在 15 秒会把"慢但正常"误判；25 秒仍远小于原来的 90~180 秒。
    const long long kNoNewAnswerGiveUpMs = 25000;
    std::string noNewAnswerWhy;
    bool sawPageBusy = false;   // ★ 页面自己忙过（扩展 busySelectors 命中）⇒ 阈值放宽（方案 A）
    bool sawPromptEcho = false;  // ★ 读到过"我们自己的提示词"（用户气泡）⇒ 那不算回答，继续等
    for (;;) {
        const long long now = NowMs();
        if (now >= deadline) break;
        // ★★ **页面自己在忙 ⇒ 阈值放宽**（2026-09-30，方案 A）。
        //   实测：豆包进了**它自己的任务模式**（页面显示「已处理 47 秒 / 搜索工具信息」），
        //   新回答要 40 秒以上才出现，25 秒阈值把它误判成"没提交"。
        //   判据来自**扩展的 busySelectors**（provider 配置里本就有这类选择器）⇒ 页面自忙时
        //   给到 45 秒，并且**失败时改写措辞**，让日志一眼分清是我方链路问题还是它在干自己的活。
        const long long giveUpMs = sawPageBusy ? 45000 : kNoNewAnswerGiveUpMs;
        if (!sawNew && now - tWait >= giveUpMs) {
            out.error = sawPromptEcho
                ? "PAGE_SHOWS_OUR_PROMPT"
                : (sawPageBusy ? "PAGE_BUSY_OWN_TASK" : "NO_NEW_ANSWER");
            out.message = sawPromptEcho
                ? ("提交后 " + std::to_string((now - tWait) / 1000)
                    + " 秒**页面里只读到我们自己的提示词**（命中的是用户气泡），"
                      "始终没有出现模型的回答 ⇒ 已中止（不再空等）。"
                      "常见：①模型还在处理它自己的任务（页面上有「已处理 N 秒」）；"
                      "②回答还没渲染出来，而读回答的选择器先命中了用户消息。"
                      "处置：等它跑完重试；若每次如此，说明读回答的选择器要收窄（把这条日志发给开发者）。")
                : (sawPageBusy
                ? ("提交后 " + std::to_string((now - tWait) / 1000)
                    + " 秒**页面一直在忙它自己的任务**（扩展的忙态选择器命中："
                      "「正在生成 / 已处理 N 秒」这类），始终没吐出我们这条链路的回答 ⇒ "
                      "已提前中止。这不是「提交没生效」：提示词写进去了、也提交了。"
                      "处置：等它自己跑完再重试，或换一个模型/站点；"
                      "若它进的是「任务/工作」模式，请在该页面切回普通对话。"
                    + (noNewAnswerWhy.empty() ? std::string()
                                              : (" 扩展回执：" + noNewAnswerWhy)))
                : ("提交后 " + std::to_string((now - tWait) / 1000)
                    + " 秒页面**没有出现新的回答**（也可能提交根本没生效）⇒ 已提前中止，"
                      "不再空等。常见原因：①提示词没写进输入框/没提交（输入框里可能还留着它）；"
                      "②输入框选择器过期（页面改版、或页面进了「任务/工作」等别的模式）；"
                      "③标签页被切走/关闭。"
                    + (noNewAnswerWhy.empty() ? std::string()
                                              : (" 扩展回执：" + noNewAnswerWhy))));
            out.waitMs = static_cast<int>(now - tWait);
            windowmode::WindowModeLogEventf(
                L"[网页AI] 提交后 %llds 没有新回答 ⇒ 提前失败（页%s，不再空等 %llds s）",
                (now - tWait) / 1000, (sawPageBusy ? L"面自己在忙" : L"面空闲"),
                (deadline - tWait) / 1000);
            return out;
        }
        if (now - lastBeat >= 5000) {
            lastBeat = now;
            windowmode::WindowModeLogEventf(
                L"[网页AI] 等网页回答：已 %llds / 上限 %llds；%s",
                (now - tWait) / 1000, (deadline - tWait) / 1000,
                (sawNew
                    ? (stableCount >= 2 ? L"已读到新回答（等稳定收尾）" : L"读到新回答但还在变")
                    : (sawPageBusy
                        ? L"★页面自己在忙（跑它自己的任务）⇒ 本次阈值放宽到 45s"
                        : L"页面还没吐出新回答（可能在生成，或提交没生效）")));
        }
        const int remain = static_cast<int>(deadline - now);
        WebAiReplyInfo r;
        std::string rerr;
        const int perCallIdle = (std::min)(idleTimeoutMs, (std::max)(1500, remain));
        const int perCallMax = (std::min)((std::max)(perCallIdle + 1500, 8000), remain);
        if (!ReadReply(providerId, perCallIdle, perCallMax, r, rerr, (std::min)(remain + 5000, 90000))) {
            if (rerr == "NO_RESPONSE_NODE") {
                // 记下扩展给的原因（选择器试过哪些、超时多久），失败时一并报出去
                if (r.pageBusy) sawPageBusy = true;   // ★ 没回答节点但页面在忙 ⇒ 它在干自己的活
                noNewAnswerWhy = r.message.empty() ? std::string("NO_RESPONSE_NODE") : r.message;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;  // 新回答还没冒出来 ⇒ 由上面的 25s 上限兜住（别在这里无限等）
            }
            if (rerr == "NO_TAB") {
                // 扩展的 service worker 重启过（attachMeta 丢了）⇒ 补一次 attach 再继续
                std::string aerr;
                if (EnsureAttached(providerId, aerr)) continue;
            }
            out.error = rerr.empty() ? "READ_FAILED" : rerr;
            out.message = r.message;
            out.waitMs = static_cast<int>(NowMs() - tWait);
            return out;
        }
        // ★★ **读到的是"我们自己的提示词"⇒ 那不是回答，继续等**（2026-09-30 实测）。
        //
        //   实测：页面还在 `已处理 17 秒 / 尝试调用截图工具`（模型正在干活），
        //   而 `responseSelectors` 命中了**用户气泡**（我们刚写进去的 5035 字符提示词）
        //   ⇒ 旧行为把它当"新回答"收下 ⇒ 宿主回显闸报 REPLY_ECHOES_PROMPT ⇒ **整轮作废**
        //   （用户看到"网页还在处理就直接断了"）。
        //   ⚠ 扩展侧也加了"排除含横幅的块"（更彻底），但**这里必须先兜住**：
        //     ① 扩展可能还没重载；② 真回答没渲染出来时，扩展只能选到用户气泡。
        //   ⇒ 判据只看**我们自己的横幅**（强标识，不猜语义）；命中就当"还没回答"，
        //     由上面的"没新回答"上限（25s/45s）兜住，绝不当场作废。
        {
            const bool looksLikeOurPrompt =
                r.text.find("键鼠工坊") != std::string::npos
                || r.text.find("网页 AI 桥接") != std::string::npos;
            if (looksLikeOurPrompt) {
                sawPromptEcho = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
                continue;
            }
        }
        if (r.pageBusy) sawPageBusy = true;   // ★ 只记事实：这一次读的时候页面在忙
        const bool changed = (r.blocks > out.baselineBlocks) || (r.text != out.baselineText);
        if (!changed) {
            // 还是旧回答 ⇒ 网页可能还没开始生成，继续等（**不要**提前返回旧答案）；
            // 同样由上面的 25s 上限兜住：超过就带原因失败，不再空等 90~180 秒。
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            continue;
        }
        sawNew = true;
        if (r.text == stableText) {
            ++stableCount;
        } else {
            stableText = r.text;
            stableCount = 1;
        }
        // 稳定性 + 协议完整性双判据：
        //  · 连续两次读到同一份文本 ⇒ 认为生成结束（扩展侧已有 1.2s 稳定判据，这里再叠一层）
        //  · 协议块「有头无尾」⇒ 一定还没生成完（半截 JSON 会被解析成错误的参数）
        // ★★ 第三判据（2026-10-02 真机）：豆包思考时气泡里稳定显示「正在思考」——
        //   上面两条会把它当成"已稳定"直接返回 ⇒ 宿主收到一段不是回答的文字，
        //   连续两轮后 `API 未返回有效动作 JSON` 整局结束（用户："又断开连接了"）。
        //   ⇒ 占位文案不算回答，继续等（仍由 25s 上限兜底，不会无限等）。
        {
            std::wstring only = FromUtf8(stableText);
            {
                size_t a = 0, b = only.size();
                while (a < b && (only[a] == L' ' || only[a] == L'\t'
                    || only[a] == L'\r' || only[a] == L'\n')) ++a;
                while (b > a && (only[b - 1] == L' ' || only[b - 1] == L'\t'
                    || only[b - 1] == L'\r' || only[b - 1] == L'\n')) --b;
                only = only.substr(a, b - a);
            }
            const bool placeholder = only == L"正在思考" || only == L"正在思考…"
                || only == L"思考中" || only == L"thinking" || only == L"Thinking";
            if (placeholder) {
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
                continue;
            }
        }
        const bool protocolOpen = stableText.find("<<<QST_TOOL_CALLS>>>") != std::string::npos
            && stableText.find("<<<END_QST_TOOL_CALLS>>>") == std::string::npos;
        if (stableCount >= 2 && !protocolOpen) {
            out.reply = r;
            out.reply.text = stableText;
            out.ok = true;
            out.waitMs = static_cast<int>(NowMs() - tWait);
            windowmode::WindowModeLogEventf(L"[网页AI] 收到新回答：%d 字，块 %d→%d，用时 %dms",
                static_cast<int>(stableText.size()), out.baselineBlocks, r.blocks, out.waitMs);
            return out;
        }
    }

    out.waitMs = static_cast<int>(NowMs() - tWait);
    out.error = sawNew ? "REPLY_NOT_STABLE" : "WAIT_TIMEOUT";
    out.message = sawNew
        ? "回答一直在变化（可能仍在生成），未达到稳定判据"
        : "等待新回答超时：网页可能没有开始生成（提交是否真的生效？）";
    windowmode::WindowModeLogEventf(L"[网页AI] 等回答失败（%s，等了 %dms，基线块=%d）",
        FromUtf8(out.error).c_str(), out.waitMs, out.baselineBlocks);
    (void)t0;
    return out;
}

}  // namespace quickscript::webai
