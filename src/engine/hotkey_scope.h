#pragma once
// =============================================================================
// hotkey_scope.h — 专属热键「作用域」判据（纯逻辑，无 HWND / 无引擎状态）
// =============================================================================
// 产品与自检共用同一份判据，避免口径漂移（同 desktop_tools/float_ball_geom.h）。
//
// 背景（历史 bug，本文件要钉死的契约）：
//   专属热键（脚本 / 键鼠录制各一个）按主页 TAB 限定作用域 —— 「鼠标宏」页只认
//   脚本热键，「键鼠录制」页只认录制热键。原实现只在**启动路径**上做了作用域
//   判断，RegisterAllHotkeys 却仍照常调 RegisterHotKey ⇒ 系统直接把该物理键
//   吞掉，于是「作用域外按 P 既不起脚本、又打不出 P」。
//
// 现契约（三条，缺一即回归）：
//   ① 作用域外 ⇒ **不注册** RegisterHotKey（否则系统吞键，用户打不出该字母）
//   ② 作用域外 ⇒ LL 钩子**不吞键、不投递**，原样 CallNextHookEx 放行
//      （实现：engine_host_window.h 的 HotkeyKbProcBody 里 `if (!h.inScope) continue;`）
//   ③ 例外：有正在运行的会话（keepForStop）时**必须保留注册**，否则无法用同一个
//      热键停止正在跑的宏；停止路径不受作用域限制
// =============================================================================

namespace qst {
namespace hotkey_scope {

/// 该条目此刻是否「在作用域内」（当前页列出了它）。
///   scopeAllPages  「全部页面生效」开关打开 ⇒ 一律在作用域内
///   isRecording    true = 键鼠录制条目；false = 鼠标宏 / 脚本条目
///   onMacroTab     当前页是「鼠标宏」
///   onRecorderTab  当前页是「键鼠录制」
/// 其余页面（设置 / AI 助手 / 关于…）两类条目都不在作用域内。
inline bool DedicatedInScope(bool scopeAllPages, bool isRecording,
    bool onMacroTab, bool onRecorderTab) {
    if (scopeAllPages) return true;
    return isRecording ? onRecorderTab : onMacroTab;
}

/// 是否允许为该条目走系统级 RegisterHotKey。
/// ⚠ 作用域外必须**不注册** —— 注册后系统会直接吞掉该物理键，表现为
///   「按 P 既不起脚本、又打不出 P」。
///   keepForStop：有正在运行的会话 ⇒ 保留注册，保证同一热键还能停止。
inline bool AllowSystemRegister(bool inScope, bool keepForStop) {
    return inScope || keepForStop;
}

}  // namespace hotkey_scope
}  // namespace qst
