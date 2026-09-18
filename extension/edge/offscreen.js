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

async function loadBridgeRuntimeFromNative() {
  return await new Promise((resolve) => {
    let port;
    try {
      port = chrome.runtime.connectNative("com.quickscripttool.bridge");
    } catch (_) {
      resolve(null);
      return;
    }
    let done = false;
    const finish = (v) => {
      if (done) return;
      done = true;
      try {
        port.disconnect();
      } catch (_) {
        /* ignore */
      }
      resolve(v);
    };
    const timer = setTimeout(() => finish(null), 1500);
    port.onMessage.addListener((msg) => {
      clearTimeout(timer);
      if (msg && msg.ok && msg.token && msg.port) finish(msg);
      else finish(null);
    });
    port.onDisconnect.addListener(() => {
      clearTimeout(timer);
      finish(null);
    });
    try {
      port.postMessage({ type: "getBridge" });
    } catch (_) {
      finish(null);
    }
  });
}

async function loadBridgeRuntime() {
  try {
    const res = await fetch(chrome.runtime.getURL("bridge_runtime.json"), {
      cache: "no-store",
    });
    if (res.ok) {
      const info = await res.json();
      if (info && info.ok && info.token && info.port) return info;
    }
  } catch (_) {
    /* packed CRX has no host-written json */
  }
  return loadBridgeRuntimeFromNative();
}

async function tryDiscoverAndConnect() {
  if (discoverBusy) return;
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
    return;
  }
  discoverBusy = true;
  try {
    const runtime = await loadBridgeRuntime();
    if (!runtime) {
      setStatus("waiting", "等待宿主写入桥凭证");
      return;
    }
    const port = Number(runtime.port);
    if (port < PORT_LO || port > PORT_HI) return;
    try {
        const ctrl = new AbortController();
        const timer = setTimeout(() => ctrl.abort(), 400);
        const res = await fetch(`http://127.0.0.1:${port}/qst/status`, {
          signal: ctrl.signal,
          cache: "no-store",
        });
        clearTimeout(timer);
        if (!res.ok) return;
        const info = await res.json();
        if (!info || !info.ok) return;
        const wsBase = info.ws || `ws://127.0.0.1:${port}/qst/ws`;
        setStatus("connecting", `port=${port}`);
        await new Promise((resolve) => {
          const socket = new WebSocket(`${wsBase}?token=${encodeURIComponent(runtime.token)}`);
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
            socket.send(JSON.stringify({ id: 0, type: "hello", token: runtime.token }));
            done();
          };
          socket.onmessage = (ev) => {
            const raw = String(ev.data || "");
            chrome.runtime.sendMessage({ type: "bridgeRecv", raw }).catch(() => {});
            forwardToSw(raw);
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
      /* status/ws failed this round */
    }
    setStatus("waiting", "未发现键鼠工坊桥（请先运行键鼠工坊并启动窗口模式）");
  } finally {
    discoverBusy = false;
  }
}

chrome.runtime.onMessage.addListener((msg, _sender, sendResponse) => {
  if (msg && msg.type === "offscreenReconnect") {
    closeWs();
    tryDiscoverAndConnect().then(() =>
      sendResponse({ ok: true, ws: !!(ws && ws.readyState === WebSocket.OPEN) })
    );
    return true;
  }
  if (msg && msg.type === "offscreenEnsure") {
    tryDiscoverAndConnect().then(() =>
      sendResponse({ ok: true, ws: !!(ws && ws.readyState === WebSocket.OPEN) })
    );
    return true;
  }
  if (msg && msg.type === "offscreenPing") {
    sendResponse({
      ok: true,
      ws: !!(ws && ws.readyState === WebSocket.OPEN),
    });
    return false;
  }
  if (msg && msg.type === "bridgeSend") {
    const raw = typeof msg.raw === "string" ? msg.raw : JSON.stringify(msg.raw || {});
    if (ws && ws.readyState === WebSocket.OPEN) {
      try {
        ws.send(raw);
        sendResponse({ ok: true });
      } catch (e) {
        sendResponse({ ok: false, error: String(e && e.message ? e.message : e) });
      }
    } else {
      sendResponse({ ok: false, error: "no_ws" });
    }
    return false;
  }
  return false;
});

setInterval(() => {
  tryDiscoverAndConnect();
}, RECONNECT_MS);

tryDiscoverAndConnect();
