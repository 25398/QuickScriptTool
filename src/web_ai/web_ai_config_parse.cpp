// ──────────────────────────────────────────────────────────────────
// web_ai_config_parse.cpp — 网页版 AI 配置的**纯解析**（无 Win32、无桥依赖）
//
// ⚠ 为什么单独一个文件：`web_ai_config.cpp` 要问桥拿端口/token（依赖
//   `ExtBridgeServer`），而解析这一段必须能在**无桌面、无桥、无网**的环境里被
//   `WebAiSelfTest` 逐格断言。混在一起就只能靠链接桩，那种自检等于自己骗自己。
// ──────────────────────────────────────────────────────────────────

#include "web_ai/web_ai_config.h"
#include "web_ai/web_ai_prompt.h"   // IsWebAiApiUrl（纯逻辑）

#include <windows.h>

#include <nlohmann/json.hpp>

namespace quickscript::webai {

namespace {

/// 本文件**故意不 include `utils.h`**（那会把整个工具集拖进来，也会让本文件
/// 失去"纯解析、可单独链接"的性质）⇒ 自带一个最小的去空白。
std::wstring TrimW(const std::wstring& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r'
                     || s[e - 1] == L'\n')) {
        --e;
    }
    return s.substr(b, e - b);
}

/// 0 或非法值 ⇒ 用默认值；超出上下限 ⇒ 夹到边界（**不报错**：配置是给人手写的，
/// 一个写错的数字不该让整个功能失效，但也不许把 999999 当真）
int ClampInt(int v, int lo, int hi, int fallback) {
    if (v <= 0) return fallback;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

}  // namespace

bool ExeNameIsSelfTestOrProbe(const std::wstring& exeName) {
    std::wstring lower;
    lower.reserve(exeName.size());
    for (wchar_t c : exeName) lower.push_back(static_cast<wchar_t>(towlower(c)));
    auto endsWith = [&](const wchar_t* suffix) {
        const size_t n = wcslen(suffix);
        return lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0;
    };
    return endsWith(L"selftest.exe")             // *SelfTest.exe
        || endsWith(L"qstrecorderlogictest.exe") // 录制自检的产物名（刻意避开 Recorder* 命名）
        || endsWith(L"probe.exe");               // 诊断/探针 exe
}

bool CurrentProcessIsSelfTestOrProbe() {
    static const bool cached = []() {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
        const std::wstring full(path);
        const size_t slash = full.find_last_of(L"\\/");
        const std::wstring name = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
        return ExeNameIsSelfTestOrProbe(name);
    }();
    return cached;
}

UserConfig ParseUserConfigJson(const std::string& jsonUtf8In, bool fileExists) {
    UserConfig c;
    c.fileExists = fileExists;
    if (!fileExists || jsonUtf8In.empty()) return c;
    // ⚠⚠ 必须容忍 **UTF-8 BOM**：这个文件是给人手写的，而
    //   记事本「另存为 UTF-8」与 PowerShell `Set-Content -Encoding UTF8`
    //   **都会写 BOM**（实测踩到：配置看着完全正确，功能却静默不开启）。
    //   nlohmann 的 json::parse 遇到 BOM 直接判失败 ⇒ 表现为"文件存在但等于没开"。
    std::string jsonUtf8 = jsonUtf8In;
    if (jsonUtf8.size() >= 3
        && static_cast<unsigned char>(jsonUtf8[0]) == 0xEF
        && static_cast<unsigned char>(jsonUtf8[1]) == 0xBB
        && static_cast<unsigned char>(jsonUtf8[2]) == 0xBF) {
        jsonUtf8.erase(0, 3);
    }
    const nlohmann::json j = nlohmann::json::parse(jsonUtf8, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        // ⚠ 坏 JSON **不算开启**（宁可功能不生效，也不许拿半份配置去驱动浏览器）
        c.enabled = false;
        return c;
    }
    c.enabled = j.value("enabled", false);
    if (j.contains("provider") && j["provider"].is_string()) {
        const std::string p = j["provider"].get<std::string>();
        if (!p.empty()) c.provider = p;
    }
    if (j.contains("modelName") && j["modelName"].is_string()) {
        const std::string m = j["modelName"].get<std::string>();
        if (!m.empty()) c.modelName = m;
    }
    c.idleTimeoutMs = ClampInt(j.value("idleTimeoutMs", 0), 1000, 120000, 8000);
    c.maxTotalMs = ClampInt(j.value("maxTotalMs", 0), 5000, 900000, 180000);
    c.maxToolListChars = ClampInt(j.value("maxToolListChars", 0), 500, 200000, 12000);
    c.maxTotalPromptChars = ClampInt(j.value("maxTotalPromptChars", 0), 2000, 400000, 24000);
    // ★ 读回答时是否临时前台化（**默认 false**，见 web_ai_config.h 的说明）
    if (j.contains("activateTabDuringRun") && j["activateTabDuringRun"].is_boolean()) {
        c.activateTabDuringRun = j["activateTabDuringRun"].get<bool>();
    }

    // ★ 渐进披露开关（默认 true）。⚠ 只认**显式 false**：写错/缺省一律当开启
    //   （否则一个拼错的键会让上下文体积悄悄涨回 10 倍）
    if (j.contains("toolCatalogMode") && j["toolCatalogMode"].is_boolean()) {
        c.toolCatalogMode = j["toolCatalogMode"].get<bool>();
    }
    // ★ 窗口反代：要暴露到模型下拉里的客户端 id（如 ["doubao-app"]）
    //   ⚠ 默认空：判据未校准前注入 = 让用户选到一个必然失败的东西
    if (j.contains("windowClients") && j["windowClients"].is_array()) {
        c.windowClients.clear();
        for (const auto& v : j["windowClients"]) {
            if (v.is_string()) {
                const std::string s = v.get<std::string>();
                if (!s.empty()) c.windowClients.push_back(s);
            }
        }
    }
    return c;
}

const wchar_t* const kWebAiModelName = L"web";

const std::vector<std::wstring>& BuiltinWebAiProviderIds() {
    // ⚠ 顺序 = **自动挑选**的顺序（`web` 模型逐个探测时按这个次序）。
    //   豆包放第一（已真机打通）。这里与 `extension/edge/web_ai_providers.json` 的
    //   providers 键**必须一致**；漏掉的站点会**根本不参与自动挑选**。
    static const std::vector<std::wstring> kIds = {
        L"doubao", L"deepseek", L"yuanbao", L"qwen",
    };
    return kIds;
}

std::wstring WebAiModelNameForProvider(const std::wstring& providerId) {
    return TrimW(providerId) + L"-web";
}

bool IsLegacyProviderModelName(const std::wstring& modelName) {
    const std::wstring want = TrimW(modelName);
    if (want.empty()) return false;
    for (const auto& pid : BuiltinWebAiProviderIds()) {
        if (want == WebAiModelNameForProvider(pid)) return true;
    }
    return false;
}

bool IsBuiltinWebAiModelName(const std::wstring& modelName) {
    const std::wstring want = TrimW(modelName);
    if (want.empty()) return false;
    if (want == kWebAiModelName) return true;   // ★ 主形态
    return IsLegacyProviderModelName(want);     // 旧形态仍认得（迁移期）
}

/// 本文件不 include `utils.h` ⇒ 自带一个最小 UTF-8 转换（只用于回环 URL 判定，
/// 内容是纯 ASCII，转换安全）。
std::string ToUtf8W(const std::wstring& w) {
    std::string o;
    o.reserve(w.size());
    for (const wchar_t c : w) o.push_back(c < 128 ? static_cast<char>(c) : '?');
    return o;
}

void ApplyProfileToSettings(quickscript::AiApiSettings& ai,
                            const std::wstring& model,
                            const std::wstring& url,
                            const std::wstring& key,
                            bool& selectedOut) {    selectedOut = false;
    const std::wstring want = TrimW(model);
    if (want.empty()) return;

    // ── ① 档案**始终**存在于 savedModels ─────────────────────────────
    //   ⇒ 用户在「设置 → AI助手」的模型下拉里**总能看到**这个选项，
    //     不需要先去建一个 `web_ai_config.json`（那正是"设置里没有这个选项"的原因）。
    //   ⚠ 端口/token 每次启动都变 ⇒ 档案里的旧值必须刷新（否则静默 401 / 打到旧端口）。
    bool found = false;
    for (auto& m : ai.savedModels) {
        if (m.modelName == want) {
            found = true;
            m.apiUrl = url;
            if (!key.empty()) m.apiKey = key;
            break;
        }
    }
    if (!found) {
        quickscript::AiModelProfile p;
        p.modelName = want;
        p.apiUrl = url;
        p.apiKey = key;
        p.temperature = 0.3;
        p.maxTokens = 4096;
        // ⚠ **追加在末尾**，不插队：`ResolveAiModelName` 在没有 `ai.modelName` 时
        //   会取 `savedModels` 的**第一个**。插队会把用户自己的模型挤掉，
        //   等于"选了个网页 AI 模型，结果所有动作都走了浏览器"。
        ai.savedModels.push_back(p);
    }

    // ── ② 用户**选中**了它吗？（这是「免 API Key」的启用判据）─────────
    selectedOut = (TrimW(ai.modelName) == want);
    if (!selectedOut) {
        // 没选中 ⇒ 什么都不改。⚠ 尤其**不删**档案（用户随时可能再选回来），
        //   也**不**动 `ai.enabled`（总闸归用户管）。
        return;
    }

    // ── ③ 选中了 ⇒ 打开 AI 总闸并把顶层 API 指向本机端点 ─────────────
    //   产品里 6 处硬闸都看 `ai.enabled`（如 engine_host_window.h:13481）；
    //   用户既然选了「用本机浏览器的 AI」，总闸就该是开的。
    //   ⚠ 只在内存副本上改：**不落盘**，除非用户自己去设置页点保存。
    ai.enabled = true;
    ai.apiUrl = url;
    if (!key.empty()) ai.apiKey = key;
}

void MigrateAndApplyProfiles(quickscript::AiApiSettings& ai,
                             const std::wstring& url,
                             const std::wstring& key,
                             bool& selectedOut,
                             const std::vector<std::string>& windowClients) {
    selectedOut = false;

    // ── ① 迁移：清掉旧的「每站点一个模型名」档案（`doubao-web` …）──────
    //   判据必须**同时**满足两条才敢认为那条档案是我们注入的：
    //     ① 名字是旧形态（`<站点id>-web`）  ② apiUrl 是本机回环端点
    //   用户自己起名/自己填 URL 的档案**一律不动**（判严不判松）。
    //   ⚠ 选中过旧档案的用户要顺手改成新的 `web`，否则他会「模型没了」。
    bool wasLegacySelected = false;
    for (auto it = ai.savedModels.begin(); it != ai.savedModels.end();) {
        if (IsLegacyProviderModelName(it->modelName) && IsWebAiApiUrl(ToUtf8W(it->apiUrl))) {
            if (TrimW(ai.modelName) == TrimW(it->modelName)) wasLegacySelected = true;
            it = ai.savedModels.erase(it);
        } else {
            ++it;
        }
    }
    if (wasLegacySelected) ai.modelName = kWebAiModelName;

    // ── ② 注入**唯一**的 `web` 档案 ─────────────────────────────────
    //   ★ 用户 2026-09-25 的要求：下拉里只留一个 `web`，站点由运行时自动挑
    //     （已打开的优先 → 已登录的其次 → 都没有就提示去登录）。
    //     此前每站点一条档案，用户看花眼。
    ApplyProfileToSettings(ai, kWebAiModelName, url, key, selectedOut);

    // ── ③ 窗口反代客户端档案（用户点名才注入）────────────────────────
    //   模型名 = 客户端 id（如 `doubao-app`）—— 后端就是按这个名字把请求
    //   路由到窗口通道的（见 `HandleChatCompletionViaWindow`）。
    //   ⚠ 只注入 `savedModels`，**不动** `ai.modelName`/`ai.enabled`：
    //     窗口反代要抢前台、且判据未校准时必然失败，不该悄悄成为默认模型。
    //   ⚠ 也**不动** `selectedOut`：那个量表示"用户选了本机 AI 当默认"，
    //     与"列表里多了一个可选客户端"是两件事。
    for (const auto& id : windowClients) {
        if (id.empty()) continue;
        const std::wstring want(id.begin(), id.end());
        bool exists = false;
        for (const auto& m : ai.savedModels) {
            if (m.modelName == want) { exists = true; break; }
        }
        if (exists) continue;
        quickscript::AiModelProfile p;
        p.modelName = want;
        p.apiUrl = url;
        p.apiKey = key;
        p.temperature = 0.3;
        p.maxTokens = 4096;
        ai.savedModels.push_back(p);
    }
}

}  // namespace quickscript::webai
