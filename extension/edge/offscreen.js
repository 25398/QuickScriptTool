/* 常驻页：持有 WebSocket，避免 MV3 service worker 休眠把桥掐断。 */

const PORT_LO = 19228;
const PORT_HI = 19240;
const RECONNECT_MS = 2000;
/**
 * native 兜底（connectNative）会拉起一个完整宿主进程，而本页由 2s 轮询驱动。
 * 只在读不到 bridge_runtime.json（打包 CRX）时才用，且两次之间至少间隔这么久。
 */
const NATIVE_PROBE_MIN_MS = 30000;
let lastNativeProbeAt = 0;

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

/**
 * 30s 内**拿到过**的 token：说明宿主已经处理过它 —— 宿主每次启动都会换 token，
 * 所以「同一 token 又被拒一次」= 它已经是陈旧凭证，不必再拿它去撞第二次。
 */
let lastSeenToken = "";
/** 上一轮凭证尝试是否全部失败（true ⇒ 下一轮允许问 native 兜底）。 */
let lastCredsFailed = false;

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

/**
 * 读宿主写的 `bridge_runtime.json`。
 *
 * @param allowNative 是否允许在「文件不可用 / 文件里的 token 已废」时问 native 兜底
 *
 * ⚠⚠ 为什么要有这个参数，以及 `allowNative` **只能**在上一次真的连失败之后为 true
 *   （2026-09-27 真机事故，本文件最贵的一课）：
 *   上一版是函数里自己造了一个 `nativeProbeWanted`，而 `tryDiscoverAndConnect()`
 *   在**进流程的第一步**就把它置成了 true —— 于是这里对刚读出来的新鲜 token
 *   `!nativeProbeWanted` 恒为 false ⇒ **这条"零进程开销"的直读路径从来没生效过**。
 *   后果不是"多起一个进程"这么轻：native 探测带 30 秒节流（`NATIVE_PROBE_MIN_MS`），
 *   而宿主每次启动都会换 token ⇒ 宿主换了 token 之后，扩展在这 30 秒里**拿着陈旧凭证
 *   空转**（日志只有一句 `WS token 不匹配 (qLen=32 expectLen=32)`），
 *   宿主的 `WaitForExtension` 等不到就判定"扩展离线"，
 *   于是它去 `ShellExecute(msedge.exe)` —— **用户已经在用浏览器，看到的就是"又开了一个
 *   窗口/新标签页"**，哪怕脚本的目标窗口就在旁边好好的。
 *   ⇒ 规则：**先读文件（免费），只有它给的凭证被"用过了且失败"才允许问 native（要起进程）。**
 */
async function loadBridgeRuntime(allowNative) {
  try {
    const res = await fetch(chrome.runtime.getURL("bridge_runtime.json"), {
      cache: "no-store",
    });
    if (res.ok) {
      const info = await res.json();
      if (info && info.ok && info.token && info.port) {
        // 宿主处理过、且上一轮凭证是失败的 ⇒ 这份文件是旧的，直接落到 native。
        const stale = allowNative && lastCredsFailed
          && String(info.token) === lastSeenToken;
        if (!stale) return info;
      }
    }
  } catch (_) {
    /* packed CRX has no host-written json */
  }
  if (!allowNative) return null;
  // native 兜底会拉起一个完整宿主进程；本函数由 2s 轮询驱动 ⇒ 必须节流。
  const now = Date.now();
  if (now - lastNativeProbeAt < NATIVE_PROBE_MIN_MS) return null;
  lastNativeProbeAt = now;
  return loadBridgeRuntimeFromNative();
}

/** 桥在不在（不带 token 的只读探针；少一次 TLS/端口超时的重试）。 */
async function bridgeStatusAlive(port) {
  try {
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), 400);
    const res = await fetch(`http://127.0.0.1:${port}/qst/status`, {
      signal: ctrl.signal,
      cache: "no-store",
    });
    clearTimeout(timer);
    if (!res.ok) return false;
    const info = await res.json();
    return !!(info && info.ok);
  } catch (_) {
    return false;
  }
}

/** 用一份凭证连一次桥。返回是否真的连上（`ws` 指向它）。 */
function connectWithRuntime(port, token) {
  return new Promise((resolve) => {
    let socket;
    try {
      socket = new WebSocket(`ws://127.0.0.1:${port}/qst/ws?token=${encodeURIComponent(token)}`);
    } catch (_) {
      resolve(false);
      return;
    }
    let settled = false;
    const done = (ok) => {
      if (settled) return;
      settled = true;
      resolve(!!ok);
    };
    socket.onopen = () => {
      ws = socket;
      setStatus("connected", `ws port=${port} (offscreen)`);
      socket.send(JSON.stringify({ id: 0, type: "hello", token }));
      done(true);
    };
    socket.onmessage = (ev) => {
      const raw = String(ev.data || "");
      chrome.runtime.sendMessage({ type: "bridgeRecv", raw }).catch(() => {});
      forwardToSw(raw);
    };
    socket.onclose = () => {
      if (ws === socket) ws = null;
      setStatus("disconnected", "WebSocket 已断开，正在重连…");
      done(false);
    };
    socket.onerror = () => {
      try {
        socket.close();
      } catch (_) {
        /* ignore */
      }
      done(false);
    };
    setTimeout(() => done(ws === socket && socket.readyState === WebSocket.OPEN), 1500);
  });
}

async function tryDiscoverAndConnect() {
  if (discoverBusy) return;
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
    return;
  }
  discoverBusy = true;
  try {
    // ① 先拿"新鲜凭证"：文件直读优先，只有它不可用/已废时 native 才会被问到。
    const allowNative = lastCredsFailed;
    let runtime = await loadBridgeRuntime(allowNative);
    let port = runtime ? Number(runtime.port) : 0;
    if (runtime && (port < PORT_LO || port > PORT_HI)) {
      runtime = null;
      port = 0;
    }
    if (!runtime) {
      // ⚠ 这一轮**不能**判定为"凭证失败"：我们压根没拿文件里的 token 去试过。
      //   把它当成失败会让下一轮立刻解禁 native 探测（30s 节流形同虚设）。
      setStatus("waiting", "等待宿主写入桥凭证");
      return;
    }
    // ② 桥端口先探活一次（只读、不带 token）：活才值得去撞 WS。
    if (!(await bridgeStatusAlive(port))) {
      if (!allowNative) {
        setStatus("waiting", "未发现键鼠工坊桥（请先运行键鼠工坊并启动窗口模式）");
        return;
      }
      // 本机桥不在（宿主没跑 / 端口不对）⇒ 允许问一次 native 兜底。
      lastCredsFailed = true;
      const fresh = await loadBridgeRuntime(true);
      const freshPort = fresh ? Number(fresh.port) : 0;
      if (!fresh || freshPort < PORT_LO || freshPort > PORT_HI) return;
      runtime = fresh;
      port = freshPort;
    }
    // ③ 真连一次；失败就明确记成"凭证失败"，让下次拿到新鲜 token。
    setStatus("connecting", `port=${port}`);
    const ok = await connectWithRuntime(port, String(runtime.token));
    lastCredsFailed = !ok;
    if (ok) {
      lastSeenToken = String(runtime.token);
      return;
    }
    setStatus("waiting", "桥拒绝了凭证（宿主可能刚重启）⇒ 立刻取一份新的重试");
  } finally {
    discoverBusy = false;
  }
}

/**
 * 用户/宿主显式要求"现在就重连"（选项页「重新连接」、`offscreenEnsure`）。
 *
 * ⚠ 必须**先解除两个节流/记忆**再发现：否则"重新连接"只是把同一个陈旧 token
 *   再撞一次（`lastSeenToken`/`lastCredsFailed` 判它是旧的），用户会觉得按钮没用。
 *   显式意图优先于节流 —— 这正是这个按钮存在的意义。
 */
async function forceReconnect() {
  lastNativeProbeAt = 0;
  lastCredsFailed = true;
  lastSeenToken = "";
  closeWs();
  await tryDiscoverAndConnect();
}

chrome.runtime.onMessage.addListener((msg, _sender, sendResponse) => {
  if (msg && msg.type === "offscreenReconnect") {
    forceReconnect().then(() =>
      sendResponse({ ok: true, ws: !!(ws && ws.readyState === WebSocket.OPEN) })
    );
    return true;
  }
  if (msg && msg.type === "offscreenEnsure") {
    forceReconnect().then(() =>
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
