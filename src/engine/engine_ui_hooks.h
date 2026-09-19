// ──────────────────────────────────────────────────────────────────
// engine_ui_hooks.h — 引擎 → 壳 的唯一出口（依赖倒置）
//
// 为什么要有这层（架构评估 P1-5；验收报告 §7.2 B1）：
//   此前引擎直接调用定义在**壳**里的 4 个符号
//     qst::webview::PostToWebUi / HotkeyLogLine /
//     NotifyWebDebugWindowSetting / SyncHomeSelectionCache
//   （定义在 webview_bridge_backend.cpp 与 qst_webview_shell.cpp，均属
//   QstWebViewShell 目标，不在 qst_engine 内）。
//   后果：**qst_engine 无法脱离壳链接** —— 没有任何 SelfTest 能链引擎，
//   这才是「引擎主循环零单测」的机制性根因，而不是「没人写测试」。
//
//   倒置后：引擎自带 no-op 默认实现（本文件的 .cpp 提供转发定义），
//   壳在 wWinMain 顶部 SetUiBridgeHooks() 装上真实现。
//   引擎从此不引用任何壳侧符号，自检可以直接链 qst_engine。
//
// 约定：**新增引擎→壳 的调用一律走本结构体**，不要再让引擎 include
// 壳的头文件并直接调用（那会重新把依赖钉回去）。
// ──────────────────────────────────────────────────────────────────
#pragma once

#include <functional>
#include <string>

namespace qst {
namespace engine {

/// 引擎需要壳提供的 UI 能力。成员全部可空；未装 = no-op。
struct UiBridgeHooks {
    /// 向 WebView 推一条 JSON 消息（壳侧实现 = 取已注册的 JS poster 投递）。
    std::function<void(std::string)> postToWebUi;
    /// 引擎热键诊断日志（壳侧实现 = 写 webview_boot.log，HOTKEY: 前缀）。
    std::function<void(const std::string&)> hotkeyLogLine;
    /// 宏调试窗被用户关闭后同步 bridge 缓存并推 settings.changed。
    std::function<void(bool)> notifyWebDebugWindowSetting;
    /// 引擎 SaveHomeState 后同步选中缓存，避免后续 quietSaveSettings 用旧 path 覆盖磁盘。
    std::function<void(const std::wstring&, const std::wstring&, int)> syncHomeSelectionCache;
};

/// 壳在 wWinMain 顶部调用（装上真实现）。未调用时引擎用 no-op。
/// 合并语义：只覆盖**非空**成员 —— 壳的真实现分布在两个 TU
/// （webview_bridge_backend.cpp 与 qst_webview_shell.cpp），
/// 各自注册自己那部分，互不覆盖。
void SetUiBridgeHooks(UiBridgeHooks hooks);

/// 取当前钩子（供自检断言「是否被调用」；未装时成员为空）。
UiBridgeHooks& UiHooks();

/// 是否已装（自检/诊断用）。
bool UiBridgeHooksInstalled();

}  // namespace engine

// ── 引擎侧的转发定义（**定义在 engine_ui_hooks.cpp**）──────────────
// 名字与签名保持不变，这样引擎侧既有调用点（engine_host_window.h 的
// HotkeyDiagLog / PostToWebUi / NotifyWebDebugWindowSetting，
// engine_settings_reload.cpp 的 SyncHomeSelectionCache）无需改动。
// 声明放在这里而不是 webview_bridge_backend.h：那是壳的头文件，
// 引擎不该为了调用自己的出口去 include 壳。
namespace webview {

void PostToWebUi(std::string jsonUtf8);
void HotkeyLogLine(const std::string& line);
void NotifyWebDebugWindowSetting(bool enabled);
void SyncHomeSelectionCache(const std::wstring& selectedScriptPath,
    const std::wstring& selectedRecordingPath, int activeTab);

}  // namespace webview
}  // namespace qst
