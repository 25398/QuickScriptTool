/* 鼠大侠工具栏弹窗 — 列出/运行/停止本机 scripts\*.json（按当前标签过滤同类） */
const PORT_LO = 19228;
const PORT_HI = 19240;
const VERSION = chrome.runtime.getManifest().version;
const STORAGE_KEY = "scriptsEnabled";

const el = {
  status: document.getElementById("statusText"),
  list: document.getElementById("scriptList"),
  empty: document.getElementById("emptyState"),
  count: document.getElementById("scriptCount"),
  toggle: document.getElementById("masterToggle"),
  section: document.getElementById("sectionToggle"),
  ver: document.getElementById("verText"),
  btnSettings: document.getElementById("btnSettings"),
  btnRefresh: document.getElementById("btnRefresh"),
  linkOptions: document.getElementById("linkOptions"),
};

/** @type {{port:number, token:string, running:boolean, currentScript:string}|null} */
let bridge = null;
/** @type {Array<{name:string, file:string, path:string, windowModeEnabled?:boolean, windowName?:string, windowClassName?:string, inputStrategy?:string}>} */
let scripts = [];
/** @type {Array<typeof scripts[0]>} */
let visibleScripts = [];
let busy = false;
/** 总开关：启用后可从弹窗运行脚本；关闭则禁止运行并停止当前脚本 */
let scriptsEnabled = true;
let toggleUserChanging = false;

el.ver.textContent = `v${VERSION}`;

function setStatus(text, kind) {
  el.status.textContent = text || "";
  el.status.classList.remove("err", "ok");
  if (kind) el.status.classList.add(kind);
}

function openOptions(e) {
  if (e) e.preventDefault();
  chrome.runtime.openOptionsPage();
}

el.btnSettings.addEventListener("click", openOptions);
el.linkOptions.addEventListener("click", openOptions);
el.btnRefresh.addEventListener("click", () => refreshAll());
el.section.addEventListener("click", () => {
  const open = el.section.classList.toggle("open");
  el.list.classList.toggle("collapsed", !open);
});

async function loadEnabled() {
  try {
    const data = await chrome.storage.local.get(STORAGE_KEY);
    if (typeof data[STORAGE_KEY] === "boolean") {
      scriptsEnabled = data[STORAGE_KEY];
    } else {
      scriptsEnabled = true;
    }
  } catch (_) {
    scriptsEnabled = true;
  }
}

async function saveEnabled(on) {
  scriptsEnabled = !!on;
  try {
    await chrome.storage.local.set({ [STORAGE_KEY]: scriptsEnabled });
  } catch (_) {
    /* ignore */
  }
}

el.toggle.addEventListener("change", async () => {
  if (!bridge) {
    el.toggle.checked = false;
    return;
  }
  toggleUserChanging = true;
  const wantOn = !!el.toggle.checked;
  await saveEnabled(wantOn);
  if (!wantOn && bridge.running) {
    await stopScript();
  }
  syncToggle();
  renderList();
  if (!wantOn) {
    setStatus("已禁用脚本运行", "err");
  } else if (bridge.running) {
    setStatus(
      bridge.currentScript
        ? `运行中：${bridge.currentScript}`
        : "脚本运行中",
      "ok"
    );
  } else {
    setStatus(`已连接 · 端口 ${bridge.port}`, "ok");
  }
  toggleUserChanging = false;
});

async function discoverBridge() {
  for (let port = PORT_LO; port <= PORT_HI; ++port) {
    try {
      const ctrl = new AbortController();
      const timer = setTimeout(() => ctrl.abort(), 350);
      const res = await fetch(`http://127.0.0.1:${port}/qst/status`, {
        signal: ctrl.signal,
        cache: "no-store",
      });
      clearTimeout(timer);
      if (!res.ok) continue;
      const info = await res.json();
      if (!info || !info.ok || !info.token) continue;
      return {
        port,
        token: String(info.token),
        running: !!info.running,
        currentScript: info.currentScript ? String(info.currentScript) : "",
      };
    } catch (_) {
      /* next */
    }
  }
  return null;
}

async function fetchScripts(b) {
  const res = await fetch(
    `http://127.0.0.1:${b.port}/qst/scripts?token=${encodeURIComponent(b.token)}`,
    { cache: "no-store" }
  );
  const data = await res.json();
  if (!res.ok || !data || !data.ok) {
    throw new Error((data && data.error) || `HTTP ${res.status}`);
  }
  return Array.isArray(data.scripts) ? data.scripts : [];
}

async function getActiveTab() {
  try {
    const tabs = await chrome.tabs.query({ active: true, currentWindow: true });
    return tabs && tabs[0] ? tabs[0] : null;
  } catch (_) {
    return null;
  }
}

function isBrowserTabUrl(url) {
  if (!url) return false;
  return /^https?:/i.test(url) || /^edge:/i.test(url) || /^chrome:/i.test(url);
}

function isChromiumClass(cls) {
  const c = String(cls || "").toLowerCase();
  return c.startsWith("chrome_widgetwin") || c.startsWith("chrome_");
}

function titleMatchesWindowName(tabTitle, windowName) {
  const title = String(tabTitle || "").toLowerCase();
  const wn = String(windowName || "").toLowerCase().trim();
  if (!wn) return false;
  if (title.includes(wn)) return true;
  // 标题含游戏名、windowName 含更长官网标题时的双向弱匹配
  const stem = wn.length > 16 ? wn.slice(0, 16) : wn;
  if (stem.length >= 4 && title.includes(stem)) return true;
  if (title.length >= 6 && wn.includes(title.slice(0, Math.min(12, title.length)))) {
    return true;
  }
  return false;
}

/** 当前标签「同类」：启用窗口模式且（标题命中 windowName，或 Chrome/CDP 脚本匹配浏览器标签）。 */
function scriptMatchesTab(s, tab) {
  if (!s || !s.windowModeEnabled) return false;
  if (!tab) return true; // 无法读标签时不过滤掉窗口模式脚本
  const title = tab.title || "";
  const url = tab.url || "";
  const browser = isBrowserTabUrl(url);
  const wn = s.windowName || "";
  const cls = s.windowClassName || "";
  const strat = String(s.inputStrategy || "").toLowerCase();
  const cdpLike = strat === "cdp" || (strat === "auto" && isChromiumClass(cls));

  if (wn) {
    return titleMatchesWindowName(title, wn);
  }
  if (browser && (isChromiumClass(cls) || cdpLike)) {
    return true;
  }
  return false;
}

async function applyTabFilter() {
  const tab = await getActiveTab();
  const matched = scripts.filter((s) => scriptMatchesTab(s, tab));
  // 运行中的脚本始终可见，避免过滤后找不到停止按钮
  const runningFile = bridge && bridge.currentScript ? bridge.currentScript : "";
  const running = !!(bridge && bridge.running);
  if (running && runningFile) {
    for (const s of scripts) {
      const isActive =
        runningFile === s.file ||
        runningFile === s.name ||
        (s.path && runningFile === s.path) ||
        (s.path && runningFile.endsWith("\\" + s.file)) ||
        (s.path && runningFile.endsWith("/" + s.file));
      if (isActive && !matched.some((m) => m.path === s.path && m.file === s.file)) {
        matched.unshift(s);
      }
    }
  }
  visibleScripts = matched;
  return tab;
}

async function runScript(pathOrFile) {
  if (!bridge || busy) return;
  if (!scriptsEnabled) {
    setStatus("请先打开顶栏启用开关", "err");
    return;
  }
  busy = true;
  try {
    const res = await fetch(`http://127.0.0.1:${bridge.port}/qst/run`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ token: bridge.token, path: pathOrFile }),
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok || !data.ok) {
      const err = (data && data.error) || `HTTP ${res.status}`;
      if (err === "busy") setStatus("已有脚本在运行", "err");
      else setStatus(`运行失败: ${err}`, "err");
      return;
    }
    setStatus("已开始运行", "ok");
    await refreshState();
  } catch (e) {
    setStatus("无法连接鼠大侠", "err");
  } finally {
    busy = false;
  }
}

async function stopScript() {
  if (!bridge || busy) return;
  busy = true;
  try {
    const res = await fetch(`http://127.0.0.1:${bridge.port}/qst/stop`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ token: bridge.token }),
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok || !data.ok) {
      setStatus(`停止失败: ${(data && data.error) || res.status}`, "err");
      return;
    }
    setStatus("已发送停止", "ok");
    await refreshState();
  } catch (_) {
    setStatus("无法连接鼠大侠", "err");
  } finally {
    busy = false;
  }
}

function renderList() {
  el.list.querySelectorAll(".item").forEach((n) => n.remove());
  const runningFile = bridge && bridge.currentScript ? bridge.currentScript : "";
  const running = !!(bridge && bridge.running);
  const list = visibleScripts;

  el.count.textContent = list.length ? String(list.length) : "";
  if (!list.length) {
    el.empty.style.display = "";
    if (!bridge) {
      el.empty.textContent = "请先启动鼠大侠";
    } else if (!scripts.length) {
      el.empty.textContent = "暂无脚本";
    } else {
      el.empty.textContent = "当前标签无匹配的窗口模式脚本";
    }
    return;
  }
  el.empty.style.display = "none";

  for (const s of list) {
    const row = document.createElement("div");
    row.className = "item";
    const isActive =
      running &&
      runningFile &&
      (runningFile === s.file ||
        runningFile === s.name ||
        (s.path && runningFile === s.path) ||
        (s.path && runningFile.endsWith("\\" + s.file)) ||
        (s.path && runningFile.endsWith("/" + s.file)));
    if (isActive) row.classList.add("active");
    if (!scriptsEnabled && !isActive) row.style.opacity = "0.45";

    const name = document.createElement("div");
    name.className = "name";
    name.textContent = s.name || s.file || "(未命名)";
    name.title = s.path || s.file || "";
    row.appendChild(name);

    if (isActive) {
      const stop = document.createElement("button");
      stop.type = "button";
      stop.className = "stop-btn";
      stop.textContent = "停止";
      stop.addEventListener("click", (ev) => {
        ev.stopPropagation();
        stopScript();
      });
      row.appendChild(stop);
    } else {
      const hint = document.createElement("span");
      hint.className = "run-hint";
      hint.textContent = scriptsEnabled ? "运行" : "已禁用";
      row.appendChild(hint);
      if (scriptsEnabled) {
        row.addEventListener("click", () => {
          runScript(s.path || s.file);
        });
      }
    }
    el.list.appendChild(row);
  }
}

function syncToggle() {
  if (toggleUserChanging) return;
  const connected = !!bridge;
  el.toggle.disabled = !connected;
  // 总开关 = 是否启用脚本，不是「是否正在运行」
  el.toggle.checked = connected && scriptsEnabled;
  el.toggle.title = scriptsEnabled
    ? "已启用：可从列表运行脚本；关闭可停止当前运行"
    : "已禁用：无法从弹窗运行脚本";
}

async function refreshState() {
  if (!bridge) return;
  try {
    const res = await fetch(`http://127.0.0.1:${bridge.port}/qst/status`, {
      cache: "no-store",
    });
    if (!res.ok) throw new Error("status");
    const info = await res.json();
    if (!info || !info.ok) throw new Error("bad");
    bridge.running = !!info.running;
    bridge.currentScript = info.currentScript ? String(info.currentScript) : "";
    if (info.token) bridge.token = String(info.token);
    await applyTabFilter();
    syncToggle();
    renderList();
    if (!scriptsEnabled) {
      setStatus("已禁用脚本运行", "err");
    } else if (bridge.running) {
      setStatus(
        bridge.currentScript
          ? `运行中：${bridge.currentScript}`
          : "脚本运行中",
        "ok"
      );
    } else {
      setStatus(
        visibleScripts.length
          ? `已连接 · ${visibleScripts.length} 个同类脚本`
          : `已连接 · 端口 ${bridge.port}`,
        "ok"
      );
    }
  } catch (_) {
    bridge = null;
    scripts = [];
    visibleScripts = [];
    syncToggle();
    renderList();
    setStatus("请先启动鼠大侠", "err");
  }
}

async function refreshAll() {
  setStatus("正在连接鼠大侠…");
  await loadEnabled();
  bridge = await discoverBridge();
  if (!bridge) {
    scripts = [];
    visibleScripts = [];
    syncToggle();
    renderList();
    setStatus("请先启动鼠大侠", "err");
    return;
  }
  try {
    scripts = await fetchScripts(bridge);
    await applyTabFilter();
    syncToggle();
    renderList();
    if (!scriptsEnabled) {
      setStatus("已禁用脚本运行", "err");
    } else if (bridge.running) {
      setStatus(
        bridge.currentScript
          ? `运行中：${bridge.currentScript}`
          : "脚本运行中",
        "ok"
      );
    } else {
      setStatus(
        visibleScripts.length
          ? `已连接 · ${visibleScripts.length} 个同类脚本`
          : "已连接 · 当前标签无匹配脚本",
        "ok"
      );
    }
  } catch (e) {
    scripts = [];
    visibleScripts = [];
    syncToggle();
    renderList();
    setStatus(`读取脚本失败: ${e && e.message ? e.message : e}`, "err");
  }
}

refreshAll();
setInterval(() => {
  if (bridge) refreshState();
}, 1500);
