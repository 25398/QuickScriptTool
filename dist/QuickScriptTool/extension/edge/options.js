function kindFor(state) {
  const s = (state || "").toLowerCase();
  if (s === "attached" || s === "connected" || s === "ready") return "ok";
  if (s === "no-tab" || s === "error" || s === "disconnected") return "bad";
  return "wait";
}

function labelFor(state) {
  const map = {
    idle: "空闲",
    installed: "已安装",
    waiting: "等待鼠大侠",
    connecting: "正在连接",
    connected: "已连接桥",
    ready: "就绪",
    attached: "已附着游戏页",
    disconnected: "已断开",
    "no-tab": "未匹配标签",
    error: "错误",
  };
  return map[state] || state || "未知";
}

function render(status) {
  const stateEl = document.getElementById("state");
  const detail = document.getElementById("detail");
  if (!status) {
    stateEl.textContent = "未知";
    stateEl.dataset.kind = "wait";
    detail.textContent = "";
    return;
  }
  const state = status.state || "未知";
  stateEl.textContent = labelFor(state);
  stateEl.dataset.kind = kindFor(state);
  detail.textContent = status.detail || "";
}

function refresh() {
  chrome.runtime.sendMessage({ type: "getStatus" }, (res) => {
    if (res) render(res);
  });
  chrome.storage.local.get("bridgeStatus", (data) => {
    if (data && data.bridgeStatus) render(data.bridgeStatus);
  });
}

document.getElementById("reconnect").addEventListener("click", () => {
  const stateEl = document.getElementById("state");
  stateEl.textContent = "重新连接中…";
  stateEl.dataset.kind = "wait";
  chrome.runtime.sendMessage({ type: "reconnect" }, (res) => {
    render(res || {});
  });
});

refresh();
setInterval(refresh, 1200);
