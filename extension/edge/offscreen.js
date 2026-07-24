/* 常驻页：持有 WebSocket，避免 MV3 service worker 休眠把桥掐断。 */

const PORT_LO = 19228;
const PORT_HI = 19240;
const RECONNECT_MS = 2000;

let ws = null;
let discoverBusy = false;
let swPort = null;
let pendingToSw = [];

function setStatus(state, detail) {
  chrome.storage.local.set({ bridgeStatus: { state, detail: detail || "" } });
  if (swPort) {
    try {
      swPort.postMessage({ type: "status", state, detail: detail || "" });
    } catch (_) {
      /* ignore */
    }
  }
}

function forwardToSw(raw) {
  if (swPort) {
    try {
      swPort.postMessage({ type: "recv", raw });
      return;
    } catch (_) {
      /* fall through queue */
    }
  }
  if (pendingToSw.length < 64) pendingToSw.push(raw);
}

function bindSwPort(port) {
  swPort = port;
  port.onMessage.addListener((msg) => {
    if (!msg || msg.type !== "send") return;
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(typeof msg.raw === "string" ? msg.raw : JSON.stringify(msg.raw));
    }
  });
  port.onDisconnect.addListener(() => {
    if (swPort === port) swPort = null;
  });
  const queued = pendingToSw.slice();
  pendingToSw = [];
  for (const raw of queued) {
    try {
      port.postMessage({ type: "recv", raw });
    } catch (_) {
      pendingToSw.push(raw);
    }
  }
}

chrome.runtime.onConnect.addListener((port) => {
  if (port.name === "qst-bridge") bindSwPort(port);
});

function closeWs() {
  if (ws) {
    try {
      ws.close();
    } catch (_) {
      /* ignore */
    }
  }
  ws = null;
}

async function tryDiscoverAndConnect() {
  if (discoverBusy) return;
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
    return;
  }
  discoverBusy = true;
  try {
    for (let port = PORT_LO; port <= PORT_HI; ++port) {
      try {
        const ctrl = new AbortController();
        const timer = setTimeout(() => ctrl.abort(), 400);
        const res = await fetch(`http://127.0.0.1:${port}/qst/status`, {
          signal: ctrl.signal,
          cache: "no-store",
        });
        clearTimeout(timer);
        if (!res.ok) continue;
        const info = await res.json();
        if (!info || !info.ok || !info.token || !info.ws) continue;
        setStatus("connecting", `port=${port}`);
        await new Promise((resolve) => {
          const socket = new WebSocket(`${info.ws}?token=${encodeURIComponent(info.token)}`);
          let settled = false;
          const done = () => {
            if (!settled) {
              settled = true;
              resolve();
            }
          };
          socket.onopen = () => {
            ws = socket;
            setStatus("connected", `ws port=${port} (offscreen)`);
            socket.send(JSON.stringify({ id: 0, type: "hello", token: info.token }));
            done();
          };
          socket.onmessage = (ev) => {
            forwardToSw(String(ev.data || ""));
          };
          socket.onclose = () => {
            if (ws === socket) ws = null;
            setStatus("disconnected", "WebSocket 已断开，正在重连…");
            done();
          };
          socket.onerror = () => {
            try {
              socket.close();
            } catch (_) {
              /* ignore */
            }
            done();
          };
          setTimeout(done, 1500);
        });
        if (ws && ws.readyState === WebSocket.OPEN) return;
      } catch (_) {
        /* next port */
      }
    }
    setStatus("waiting", "未发现鼠大侠桥（请先运行鼠大侠并启动窗口模式）");
  } finally {
    discoverBusy = false;
  }
}

chrome.runtime.onMessage.addListener((msg, _sender, sendResponse) => {
  if (msg && msg.type === "offscreenReconnect") {
    closeWs();
    tryDiscoverAndConnect().then(() => sendResponse({ ok: true }));
    return true;
  }
  if (msg && msg.type === "offscreenPing") {
    sendResponse({
      ok: true,
      ws: !!(ws && ws.readyState === WebSocket.OPEN),
    });
    return false;
  }
  return false;
});

setInterval(() => {
  tryDiscoverAndConnect();
}, RECONNECT_MS);

tryDiscoverAndConnect();
