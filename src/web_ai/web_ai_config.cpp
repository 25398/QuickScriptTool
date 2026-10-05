// ──────────────────────────────────────────────────────────────────
// web_ai_config.cpp — 网页版 AI 的开关与档案改写实现
//
// ⚠ 本文件**唯一**读盘的地方是 `Config()`（进程内缓存一次）。
//   其余全是纯函数式改写，便于在自检里逐条钉住「谁被改了、谁没被改」。
// ──────────────────────────────────────────────────────────────────

#include "web_ai/web_ai_config.h"

#include "window_mode/ext_bridge/ext_bridge_server.h"
#include "window_mode/window_mode_log.h"   // WindowModeLogEventf（端点 URL 的端口兜底要留痕）
#include "utils.h"
#include "web_ai/web_ai_prompt.h"

#include <algorithm>
#include <atomic>
#include <mutex>

#include <nlohmann/json.hpp>

namespace quickscript::webai {

using json = nlohmann::json;

namespace {

std::wstring ConfigFilePath() {
    return AppDir() + L"\\web_ai_config.json";
}

UserConfig g_cfg;
bool g_loaded = false;
std::mutex g_mu;

/// ★ 用户把「默认模型」选成了网页 AI 吗？
///
/// 由 `EnsureProfileInSettings` 在每次设置加载后刷新（它是唯一能同时看到
/// 「站点配置」与「用户设置」的地方）。
///
/// ⚠ 为什么需要这个运行时标志（而不是只看配置文件）：
///   产品形态要求「**无需配置任何 API 就能用**」—— 用户在设置页的模型下拉里
///   选中「网页版 AI」本身就是**显式配置**，不该再要求他去手工建一个 json 文件。
///   配置文件那条路保留（向后兼容 + 给高级用户改端口/超时用），但不再是唯一入口。
std::atomic<bool> g_userSelected{false};

}  // namespace

const UserConfig& Config() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_loaded) {
        UserConfig c;
        // ⚠ 自检/诊断进程不读用户配置（否则用例结果取决于机器上有没有那个文件）
        if (!CurrentProcessIsSelfTestOrProbe()) {
            const std::wstring path = ConfigFilePath();
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                const std::wstring text = ReadAll(path);
                c = ParseUserConfigJson(ToUtf8(text), true);
            }
            c.configPath = ToUtf8(path);
        }
        g_cfg = c;
        g_loaded = true;
    }
    return g_cfg;
}

void ReloadUserConfig() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_loaded = false;
}

bool Enabled() {
    // 两条入口，任一成立即开启：
    //   ① 用户在设置里把默认模型选成了网页 AI（**免 API Key**，这是主路径）
    //   ② 配置文件显式开启（向后兼容 + 高级用户改参数用）
    return g_userSelected.load() || Config().enabled;
}

std::wstring ModelNameW() {
    return FromUtf8(Config().modelName);
}

bool IsWebAiModelName(const std::wstring& modelName) {
    const std::wstring want = Trim(modelName);
    if (want.empty()) return false;
    // ★ 任一内置站点的档案名都算（`doubao-web` / `deepseek-web` / `yuanbao-web` / `qwen-web`）
    if (IsBuiltinWebAiModelName(want)) return true;
    if (want == ModelNameW()) return true;
    // 用户可能自己起了名字（如「网页豆包」）：只要名字能映射到站点且 URL 是回环，
    // `ApplyProfileOverride` 也会认。这里只判「名字层面能不能确定是网页 AI」。
    const std::string provider = ProviderFromModelName(ToUtf8(want));
    if (provider.empty()) return false;
    return provider == Config().provider;
}

std::string BridgeToken() {
    return windowmode::ExtBridgeServer::Instance().Token();
}

std::string LocalEndpointUrl() {
    // ★★ **端点 URL 绝不允许出现端口 0**（2026-10-02 真机实测的"连不上"根因）。
    //
    //   事故链：桥没起来（或绑失败）时 `Port()` 返回 0 ⇒ 这里产出
    //   `http://127.0.0.1:0/v1/chat/completions` ⇒ `ApplyProfileOverride` 把它写进档案；
    //   而"是不是网页端点"是按**路径**认的（`IsWebAiApiUrl`）⇒ 之后每一轮都继续把它
    //   改写成 `:0` ⇒ **永远连不上**。用户看到的自证信息正是：
    //   `档案 URL=http://127.0.0.1:0/v1/chat/completions；桥运行时端口=19228`。
    //   ⇒ 三道闸：① 这里先确保桥在跑（`Start()` 幂等，已在跑立刻返回）；
    //             ② 仍拿不到端口就退回默认端口（宁可指错端口，也不给一个必死的 0）；
    //             ③ `CanonicalWebAiUrl` / `ApplyProfileOverride` 各自再拦一道。
    auto& bridge = windowmode::ExtBridgeServer::Instance();
    int port = bridge.Port();
    if (port <= 0) {
        std::wstring err;
        if (bridge.Start(err) && bridge.Port() > 0) {
            port = bridge.Port();
        } else {
            port = 19228;   // 桥端口范围 19228-19240 的起点（见 ExtBridgeServer::BindPort）
            windowmode::WindowModeLogEventf(
                L"[网页AI] ★ 桥端口不可用（Start 失败：%s）⇒ 端点暂用默认 %d（绝不用 0）",
                err.c_str(), port);
        }
    }
    return CanonicalWebAiUrl(port);
}

bool UserSelectedWebAiModel() {
    return g_userSelected.load();
}

void EnsureProfileInSettings(quickscript::AiApiSettings& ai) {
    // ⚠ 全部逻辑都在**纯函数** `MigrateAndApplyProfiles` 里（`web_ai_config_parse.cpp`），
    //   这里只负责把桥的**当前**端口/token 喂进去。
    //   ⚠⚠ 不要再把逻辑挪回这里：自检只能调纯函数，
    //     留在这一层 = "注入了哪个模型名"没人测（实测：变异不变红 = 永真断言）。
    bool selected = false;
    MigrateAndApplyProfiles(ai, FromUtf8(LocalEndpointUrl()), FromUtf8(BridgeToken()), selected,
        Config().windowClients);
    g_userSelected.store(selected);
}

bool ApplyProfileOverride(quickscript::AiModelProfile& profile) {
    const std::string urlUtf8 = ToUtf8(profile.apiUrl);
    const bool isWeb = IsWebAiModelName(profile.modelName) || IsWebAiApiUrl(urlUtf8);
    if (!isWeb) return false;

    const std::string canonical = LocalEndpointUrl();
    // ★ **拿不到有效端点就别动档案**（2026-10-02）：宁可留着上一轮的 URL，
    //   也不能把 `:0` 这种必死地址写进去 —— 它会被下一轮继续认成"网页端点"而反复固化。
    if (canonical.empty()) return false;
    bool changed = false;
    const std::wstring urlW = FromUtf8(canonical);
    if (profile.apiUrl != urlW) {
        profile.apiUrl = urlW;
        changed = true;
    }
    const std::string token = BridgeToken();
    if (!token.empty()) {
        const std::wstring keyW = FromUtf8(token);
        if (profile.apiKey != keyW) {
            profile.apiKey = keyW;
            changed = true;
        }
    }
    if (Trim(profile.modelName).empty()) {
        profile.modelName = ModelNameW();
        changed = true;
    }
    return changed;
}

}  // namespace quickscript::webai
