/**
 * website/demo/agent-inline.js
 *
 * 官网 Demo：让「AI 脚本助手」在主壳页面上以内联弹窗打开，而不是新开独立页面。
 *
 * 原理：产品主壳的 openAgentSession() 在检测到 qst.openAgentWindow 存在时会请求
 * 原生打开独立 agent 窗口并直接 return。网页体验版把该能力置空，使主壳走自带
 * 的 #ov-agent 内联对话框分支（创建会话 Tab + 弹窗 + 经由 bridge.stub.js 的
 * openAgentConversation 假数据填充）。
 *
 * 本脚本必须在 bridge.js 之后、app.js 之前加载（defer 顺序执行）。
 */
(function () {
  "use strict";

  function patch() {
    if (!window.qst) return;
    // 独立 agent.html（agent-shell）自身就走内联分支，无需修改；
    // 只有主壳需要禁用“打开独立窗口”路径。
    if (!document.documentElement.classList.contains("agent-shell")) {
      window.qst.openAgentWindow = undefined;
    }
  }

  patch(); // defer 脚本顺序执行：此时 bridge.js 已完成、app.js 尚未执行
  document.addEventListener("DOMContentLoaded", patch);
})();
