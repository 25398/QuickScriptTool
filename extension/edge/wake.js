/* 任意 http(s) 页加载时唤醒 MV3 SW，让它去连本机桥。 */
try {
  chrome.runtime.sendMessage({ type: "kickBridge" });
} catch (_) {
  /* ignore */
}
