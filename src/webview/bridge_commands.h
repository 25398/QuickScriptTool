// ──────────────────────────────────────────────────────────────────
// bridge_commands.h — JS ↔ C++ 桥接命令契约（唯一事实来源）
//
// 为什么需要它（架构评估 #10；验收报告 §7.2 C 段）：
//   2026-09-18 那次 P0（saveSettings 静默失效）就长在这条缝上——桥接层是
//   「JS 发的形状」与「C++ 解析的严格度」之间的隐式契约，两侧各写各的
//   字符串字面量，没有任何东西校验它们对得上。
//   本表把 **C++ 入站分派的命令面**显式声明出来，由
//   tools/bridge_contract_selftest.cpp 双向校验：
//     · 表里每条命令，C++ 分派里必须真的有分支；
//     · 表里每条命令，JS 侧必须真的会发（防"加了 C++ 分支但没人调"）；
//     · C++ 分派里的每条命令，必须要么在表里、要么在下面的例外表里
//       （防"加了 JS 调用但忘了 C++ 分支"→ 静默无响应）。
//
// 维护约定（改桥接时照做）：
//   1. 新增 JS→C++ 命令：先在 bridge.js（或 debug.html / agent.html）加发送方，
//      再在 kBridgeJsCommands 登记，最后在 qst_webview_shell.cpp 加入站分派。
//   2. 若 C++ 分支先落地、JS 侧还没接：登记到 kBridgeCppOnlyCommands 并写清理由。
//      测试会盯着——一旦 JS 开始发它，就必须升级成正式命令。
//   3. **不要**把动作 JSON 的 `type`（loop/if/wait/mouseClick…）当桥接命令。
//      那些是脚本动作类型，走 script_types.h 的 ActionType。
// ──────────────────────────────────────────────────────────────────
#pragma once

#include <cstddef>

namespace qst {
namespace webview {

/// 一条 JS→C++ 桥接命令。
struct BridgeJsCommand {
    /// 线上字符串（JS `post({type: ...})` 与 C++ `type == "..."` 必须逐字一致）
    const wchar_t* name;
    /// 发它的 JS 文件（相对 ui/）；目前只有 bridge.js / debug.html / agent.html / app.js
    const wchar_t* sender;
};

/// C++ 入站分派接受、但**当前没有任何 JS 发**的命令。
/// 只应有两类：历史别名（保留兼容）、C++→JS 消息名被顺带接受。
/// 出现第三类（真死分支）时应当在确认后删掉分派，而不是往这里堆。
struct BridgeCppOnlyCommand {
    const wchar_t* name;
    const wchar_t* reason;
};

// ── JS→C++ 命令表（106 条，2026-09-18 由源码双向核对得出）──────────
inline constexpr BridgeJsCommand kBridgeJsCommands[] = {
    {L"agentSaveDraft", L"bridge.js"},
    {L"agentWindow.close", L"app.js"},
    {L"agentWindow.contentReady", L"agent.html"},
    {L"agentWindow.drag", L"app.js"},
    {L"agentWindow.minimize", L"app.js"},
    {L"app.closingAck", L"app.js"},
    {L"applyOptimizeRecording", L"bridge.js"},
    {L"applyTheme", L"bridge.js"},
    {L"beginHotkeyCapture", L"bridge.js"},
    {L"browsePath", L"bridge.js"},
    {L"cancelAgentMessage", L"bridge.js"},
    {L"captureActionKey", L"bridge.js"},
    {L"captureGlobalHotkey", L"bridge.js"},
    {L"captureScriptHotkey", L"bridge.js"},
    {L"captureTemplateScreenshot", L"bridge.js"},
    {L"checkUpgrade", L"bridge.js"},
    {L"createLibraryFolder", L"bridge.js"},
    {L"crosshairPick", L"bridge.js"},
    {L"debugScript", L"bridge.js"},
    {L"debugWindow.contentReady", L"debug.html"},
    {L"debugWindow.drag", L"debug.html"},
    {L"debugWindow.minimize", L"debug.html"},
    {L"debugWindow.ready", L"debug.html"},
    {L"debugWindowClosed", L"bridge.js"},
    {L"debugWindowSetTopmost", L"bridge.js"},
    {L"deleteAgentConversation", L"bridge.js"},
    {L"deleteLibraryFolder", L"bridge.js"},
    {L"deleteScheduledTask", L"bridge.js"},
    {L"deleteScript", L"bridge.js"},
    {L"endHotkeyCapture", L"bridge.js"},
    {L"exportScript", L"bridge.js"},
    {L"findImageCrop", L"bridge.js"},
    {L"findImageMatch", L"bridge.js"},
    {L"formatHotkey", L"bridge.js"},
    {L"getAppBranding", L"bridge.js"},
    {L"getClickerStatus", L"bridge.js"},
    {L"getEngineStatus", L"bridge.js"},
    {L"getGlobalHotkey", L"bridge.js"},
    {L"getHomeState", L"bridge.js"},
    {L"hideWindowModePreview", L"bridge.js"},
    {L"importScript", L"bridge.js"},
    {L"installDriver", L"bridge.js"},
    {L"installOcr", L"bridge.js"},
    {L"listAgentChanges", L"bridge.js"},
    {L"listAgentConversations", L"bridge.js"},
    {L"listLibraryFolders", L"bridge.js"},
    {L"listRecordings", L"bridge.js"},
    {L"listScheduledTasks", L"bridge.js"},
    {L"listScripts", L"bridge.js"},
    {L"loadOptimizeRecording", L"bridge.js"},
    {L"moveScriptToFolder", L"bridge.js"},
    {L"openAgentConversation", L"bridge.js"},
    {L"openAgentWindow", L"bridge.js"},
    {L"openEditor", L"bridge.js"},
    {L"openRecordingOptimize", L"bridge.js"},
    {L"openScheduledTasks", L"bridge.js"},
    {L"openSettingsData", L"bridge.js"},
    {L"openThemeCustom", L"bridge.js"},
    {L"pasteAgentClipboard", L"bridge.js"},
    {L"peekScriptActions", L"bridge.js"},
    {L"pickImageFile", L"bridge.js"},
    {L"pickScreenDrag", L"bridge.js"},
    {L"pickScreenRegion", L"bridge.js"},
    {L"pickTemplateDrag", L"bridge.js"},
    {L"previewScriptActions", L"bridge.js"},
    {L"queryVhidStatus", L"bridge.js"},
    {L"readImageDataUrl", L"bridge.js"},
    {L"renameLibraryFolder", L"bridge.js"},
    {L"renameScript", L"bridge.js"},
    {L"resolveImagePath", L"bridge.js"},
    {L"restoreSettingsDefaults", L"bridge.js"},
    {L"revertAgentChange", L"bridge.js"},
    {L"runScript", L"bridge.js"},
    {L"saveClipboardImage", L"bridge.js"},
    {L"saveEditor", L"bridge.js"},
    {L"saveImageAs", L"bridge.js"},
    {L"saveScheduledTask", L"bridge.js"},
    {L"saveSettings", L"bridge.js"},
    {L"sendAgentMessage", L"bridge.js"},
    {L"setActiveHomeTab", L"bridge.js"},
    {L"setGlobalHotkey", L"bridge.js"},
    {L"setHomeSelection", L"bridge.js"},
    {L"setItemLibraryFolder", L"bridge.js"},
    {L"setRecorderMode", L"bridge.js"},
    {L"setScheduledTasksGlobalDisabled", L"bridge.js"},
    {L"setScriptHotkey", L"bridge.js"},
    {L"setTypingHotkeysMuted", L"bridge.js"},
    {L"shell.contentReady", L"app.js"},
    {L"showDebugWindow", L"bridge.js"},
    {L"showWindowModePreview", L"bridge.js"},
    {L"startClicker", L"bridge.js"},
    {L"startRecord", L"bridge.js"},
    {L"stopClicker", L"bridge.js"},
    {L"stopRecord", L"bridge.js"},
    {L"stopScript", L"bridge.js"},
    {L"systemReboot", L"bridge.js"},
    {L"testOcr", L"bridge.js"},
    {L"themeCatalog", L"bridge.js"},
    {L"themeCss.apply", L"app.js"},
    {L"window.close", L"bridge.js"},
    {L"window.drag", L"bridge.js"},
    {L"window.minimize", L"bridge.js"},
    {L"window.modeReady", L"bridge.js"},
    {L"window.setHomeSize", L"bridge.js"},
    {L"window.setMode", L"bridge.js"},
    {L"window.uncloak", L"bridge.js"},
};
inline constexpr size_t kBridgeJsCommandCount =
    sizeof(kBridgeJsCommands) / sizeof(kBridgeJsCommands[0]);

// ── C++ 入站分派接受、JS 不发的命令（7 条）─────────────────────────
// 这些不是 bug，但**必须有人解释**；测试会强制这里每条都有理由，
// 并且一旦 JS 开始发同名命令，就会提示把它升级进上表。
inline constexpr BridgeCppOnlyCommand kBridgeCppOnlyCommands[] = {
    {L"setMode", L"历史别名：等价 window.setMode（qst_webview_shell.cpp:2917 用 || 接受）"},
    {L"modeReady", L"历史别名：等价 window.modeReady（:3036）"},
    {L"close", L"历史别名：等价 window.close（:2898）"},
    {L"drag", L"历史别名：等价 window.drag（:2913）"},
    {L"minimize", L"历史别名：等价 window.minimize（:2894）"},
    {L"agentWindow.setTopmost", L"疑似死分支：无任何 JS 发它；agent 窗置顶由 C++ 侧直接调，"
                                L"JS 侧只有 debugWindowSetTopmost（:2634）"},
    {L"debugWindow.needTheme", L"C++→JS 消息名（PostToJs 在 :2218/:2323，app.js:12659 接收）；"
                               L"入站分派在 :2216 把它当 debugWindow.ready 的别名接受"},
};
inline constexpr size_t kBridgeCppOnlyCommandCount =
    sizeof(kBridgeCppOnlyCommands) / sizeof(kBridgeCppOnlyCommands[0]);

// ── 查询 ─────────────────────────────────────────────────────────
/// ASCII 逐字比较（命令名全是 ASCII，避免为一个比较引入 std::string 依赖）。
inline bool BridgeNameEquals(const wchar_t* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != static_cast<wchar_t>(*b)) return false;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

/// 是否在已声明的命令面里（正式表 ∪ 例外表）。
///
/// 壳在处理不了的命令上用它区分两种「收不到分支」：
///   · false → JS 发了**未登记**的命令（JS 与 C++ 漂移，本类问题的最常见形状）；
///   · true  → 已登记却没有分支（表与实现漂移，正常应被 BridgeContractSelfTest 拦住）。
/// 两种情况都会写 webview_boot.log，便于离线定位——静默丢弃是 2026-09-18 那次
/// P0 最难发现的原因，桥接层不允许再出现「无痕失败」。
inline bool IsDeclaredBridgeCommand(const char* name) {
    if (!name || !*name) return false;
    for (const auto& c : kBridgeJsCommands) {
        if (BridgeNameEquals(c.name, name)) return true;
    }
    for (const auto& c : kBridgeCppOnlyCommands) {
        if (BridgeNameEquals(c.name, name)) return true;
    }
    return false;
}

}  // namespace webview
}  // namespace qst
