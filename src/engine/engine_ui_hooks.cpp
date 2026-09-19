// ──────────────────────────────────────────────────────────────────
// engine_ui_hooks.cpp — 钩子存储 + qst::webview::* 的转发定义
//
// 关键点：**qst::webview::PostToWebUi / HotkeyLogLine /
// NotifyWebDebugWindowSetting / SyncHomeSelectionCache 的定义在这里**，
// 而不是在壳里。引擎调这些名字时不再产生对壳的链接依赖；
// 壳通过 SetUiBridgeHooks() 注入真实现。
//
// 默认行为是 no-op（钩子未装时）：引擎可以在没有 UI 的进程里跑（自检、
// 未来的 headless runner），而不是链接失败。
// ──────────────────────────────────────────────────────────────────
#include "engine/engine_ui_hooks.h"

#include <string>
#include <utility>

namespace qst {
namespace engine {

namespace {
UiBridgeHooks& Storage() {
    // 进程级单例：只在启动时写一次，之后只读（壳在 wWinMain 顶部装）。
    static UiBridgeHooks hooks;
    return hooks;
}
}  // namespace

// 合并语义：只覆盖**非空**成员。
// 理由：壳的真实现分布在两个 TU（webview_bridge_backend.cpp 的三个 +
// qst_webview_shell.cpp 的热键日志），各自注册自己那部分，互不覆盖。
void SetUiBridgeHooks(UiBridgeHooks hooks) {
    UiBridgeHooks& dst = Storage();
    if (hooks.postToWebUi) dst.postToWebUi = std::move(hooks.postToWebUi);
    if (hooks.hotkeyLogLine) dst.hotkeyLogLine = std::move(hooks.hotkeyLogLine);
    if (hooks.notifyWebDebugWindowSetting) {
        dst.notifyWebDebugWindowSetting = std::move(hooks.notifyWebDebugWindowSetting);
    }
    if (hooks.syncHomeSelectionCache) {
        dst.syncHomeSelectionCache = std::move(hooks.syncHomeSelectionCache);
    }
}

UiBridgeHooks& UiHooks() { return Storage(); }

bool UiBridgeHooksInstalled() {
    const UiBridgeHooks& h = Storage();
    return static_cast<bool>(h.postToWebUi) || static_cast<bool>(h.hotkeyLogLine)
        || static_cast<bool>(h.notifyWebDebugWindowSetting)
        || static_cast<bool>(h.syncHomeSelectionCache);
}

}  // namespace engine

// ── 转发定义（声明见 engine/engine_ui_hooks.h）──────────────────────
// 注意：上面只关掉了 namespace engine，此处仍在 namespace qst 内。
// 保持原命名空间与签名不变，这样引擎侧调用点与壳侧调用点都无需改。
namespace webview {

void PostToWebUi(std::string jsonUtf8) {
    auto& hooks = engine::UiHooks();
    if (hooks.postToWebUi) hooks.postToWebUi(std::move(jsonUtf8));
}

void HotkeyLogLine(const std::string& line) {
    auto& hooks = engine::UiHooks();
    if (hooks.hotkeyLogLine) hooks.hotkeyLogLine(line);
}

void NotifyWebDebugWindowSetting(bool enabled) {
    auto& hooks = engine::UiHooks();
    if (hooks.notifyWebDebugWindowSetting) hooks.notifyWebDebugWindowSetting(enabled);
}

void SyncHomeSelectionCache(const std::wstring& selectedScriptPath,
    const std::wstring& selectedRecordingPath, int activeTab) {
    auto& hooks = engine::UiHooks();
    if (hooks.syncHomeSelectionCache) {
        hooks.syncHomeSelectionCache(selectedScriptPath, selectedRecordingPath, activeTab);
    }
}

}  // namespace webview
}  // namespace qst
