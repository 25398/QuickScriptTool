/* 键鼠工坊网页键鼠桥 — MV3 service worker（WebSocket 直接连本机桥） */
const BRIDGE_VERSION = "1.0.20";
const PORT_LO = 19228;
const PORT_HI = 19240;
const RECONNECT_MS = 1500;

let ws = null;
let bridgePort = 0;
let bridgeToken = "";
/** @type {{tabId?: number, targetId?: string}|null} */
let attachedDebuggee = null;
/**
 * 壳页 debugger（与 iframe 双挂）。找图截图复用，禁止每帧 attach/detach（可见时会白屏闪）。
 * @type {{tabId?: number}|null}
 */
let pageShotDebuggee = null;
let reattachBusy = false;
let suppressDetachClear = false;
/** 视觉截图串行：页标签临时挂载时禁止并发 vision。 */
let visionBusy = false;
/** 宿主脚本会话进行中：仅此时才在 debugger 被踢后自动重挂；空闲禁止重挂（否则调试条常驻）。 */
let sessionActive = false;
let attachMeta = {
  dpr: 1,
  offsetX: 0,
  offsetY: 0,
  focus: "",
  url: "",
  via: "tab",
  pageTabId: 0,
  iframeTargetId: "",
  iframeCssX: 0,
  iframeCssY: 0,
  iframeCssW: 0,
  iframeCssH: 0,
  contentW: 0,
  contentH: 0,
  pageCssW: 0,
  pageCssH: 0,
};
/** 找图策略：rAF 镜像优先；帧号不涨视为冻帧→回退 pageclip。 */
let visionPreferPageClip = false;
let mirrorInstallTried = false;
let lastMirrorFramesSeen = -1;
let lastStatus = { state: "idle", detail: "" };
let discoverBusy = false;
let cdpLogLeft = 8;
/** 上次重载保活（screencast）时刻；热路径节流，避免找图/键鼠连环卡帧。 */
let lastHeavyWakeAt = 0;

function setStatus(state, detail) {
  lastStatus = { state, detail: detail || "" };
  chrome.storage.local.set({ bridgeStatus: lastStatus });
}

function stripBrowserSuffix(s) {
  let t = (s || "").trim();
  for (;;) {
    const m = t.match(/^(.*)(\s[-–—]\s)(.+)$/);
    if (!m) break;
    const tail = m[3];
    if (
      /Microsoft\s*Edge/i.test(tail) ||
      /Google\s*Chrome/i.test(tail) ||
      /用户配置/.test(tail) ||
      /^Profile\s*\d+/i.test(tail) ||
      /InPrivate/i.test(tail)
    ) {
      t = m[1].trim();
      continue;
    }
    break;
  }
  return t;
}

function titleMatch(hint, title) {
  if (!hint || !title) return false;
  const h = stripBrowserSuffix(hint);
  const t = stripBrowserSuffix(title);
  if (!h || !t) return false;
  if (t.includes(h) || h.includes(t)) return true;
  const core = (s) => s.split(/[-–—|]/)[0].trim();
  const hc = core(h);
  const tc = core(t);
  return !!(hc && tc && (tc.includes(hc) || hc.includes(tc)));
}

/** 标题匹配强度：越大越优先。exact=100，前缀/包含次之。 */
function titleMatchScore(hint, title) {
  if (!hint || !title) return 0;
  const h = stripBrowserSuffix(hint);
  const t = stripBrowserSuffix(title);
  if (!h || !t) return 0;
  if (h === t) return 100;
  const core = (s) => s.split(/[-–—|]/)[0].trim();
  const hc = core(h);
  const tc = core(t);
  if (hc && tc && hc === tc) return 90;
  if (t.startsWith(h) || h.startsWith(t)) return 70;
  if (tc && hc && (tc.startsWith(hc) || hc.startsWith(tc))) return 65;
  if (t.includes(h) || h.includes(t)) return 50;
  if (hc && tc && (tc.includes(hc) || hc.includes(tc))) return 40;
  return 0;
}

function isUtilityTarget(t) {
  const u = t.url || "";
  const title = t.title || "";
  if (u.startsWith("edge://") || u.startsWith("chrome://") || u.startsWith("devtools://")) return true;
  if (u.startsWith("chrome-extension://") || u.startsWith("extension://")) return true;
  if (title.includes("edge://inspect") || title.includes("chrome://inspect")) return true;
  if (u === "" && title === "") return true;
  if (title === "about:blank") return true;
  return false;
}

function gameUrlScore(url) {
  const u = (url || "").toLowerCase();
  let s = 0;
  if (!u || u === "about:blank") return -1;
  if (u.includes("h.api.4399") || u.includes("g.php?gameid") || u.includes("g.php?gameId")) s += 40;
  if (u.includes("4399")) s += 20;
  if (u.includes("7k7k")) s += 12;
  if (u.includes("minigame") || u.includes("h5game")) s += 10;
  if (u.includes("game") || u.includes("play")) s += 6;
  if (u.includes("swf") || u.includes("unity") || u.includes("wasm")) s += 5;
  if (u.includes("login") || u.includes("passport") || u.includes("account")) s -= 8;
  return s;
}

async function collectGameTargets(pageDebuggee, pageTabId, knownGameUrl) {
  const notes = [];
  const autoAttached = [];
  const onDbgEvent = (source, method, params) => {
    try {
      if (!source || source.tabId !== pageTabId) return;
      if (method === "Target.attachedToTarget" && params && params.targetInfo) {
        autoAttached.push(params.targetInfo);
      }
    } catch (_) {
      /* ignore */
    }
  };
  try {
    chrome.debugger.onEvent.addListener(onDbgEvent);
  } catch (_) {
    /* ignore */
  }

  try {
    await chrome.debugger.sendCommand(pageDebuggee, "Target.setDiscoverTargets", {
      discover: true,
    });
  } catch (e) {
    notes.push("discover:" + (e && e.message ? e.message : e));
  }
  try {
    await chrome.debugger.sendCommand(pageDebuggee, "Target.setAutoAttach", {
      autoAttach: true,
      waitForDebuggerOnStart: false,
      flatten: true,
    });
  } catch (e) {
    notes.push("autoAttach:" + (e && e.message ? e.message : e));
  }

  // 等 OOPIF / autoAttach 事件
  await new Promise((r) => setTimeout(r, 700));

  const byId = new Map();
  const add = (raw, fromPageSession) => {
    const id = raw.id || raw.targetId;
    if (!id) return;
    const url = raw.url || "";
    const type = raw.type || "";
    const tabId = raw.tabId;
    const cur = byId.get(id) || {
      id,
      url: "",
      type: "",
      tabId: undefined,
      fromPageSession: false,
    };
    if (url) cur.url = url;
    if (type) cur.type = type;
    if (typeof tabId === "number") cur.tabId = tabId;
    if (fromPageSession) cur.fromPageSession = true;
    byId.set(id, cur);
  };

  for (const t of autoAttached) add(t, true);
  notes.push("autoAttachEvents=" + autoAttached.length);

  let pageSessionCount = 0;
  try {
    const r = await chrome.debugger.sendCommand(pageDebuggee, "Target.getTargets");
    for (const t of (r && r.targetInfos) || []) {
      add(t, true);
      pageSessionCount++;
    }
  } catch (e) {
    notes.push("cdpGetTargets:" + (e && e.message ? e.message : e));
  }

  await new Promise((r) => setTimeout(r, 400));
  try {
    const r = await chrome.debugger.sendCommand(pageDebuggee, "Target.getTargets");
    for (const t of (r && r.targetInfos) || []) {
      add(t, true);
      pageSessionCount++;
    }
  } catch (_) {
    /* ignore */
  }

  for (const t of await listTargets()) add(t, false);

  try {
    chrome.debugger.onEvent.removeListener(onDbgEvent);
  } catch (_) {
    /* ignore */
  }

  const known = (knownGameUrl || "").trim();
  const knownL = known.toLowerCase();
  const all = [...byId.values()].map((t) => {
    let score = gameUrlScore(t.url);
    if (t.type === "iframe") score += 8;
    if (t.type === "page" && /g\.php|h\.api\.4399/i.test(t.url || "")) score += 12;
    if (typeof t.tabId === "number" && t.tabId === pageTabId) score += 30;
    if (typeof t.tabId === "number" && t.tabId !== pageTabId) score -= 200;
    if (t.fromPageSession) score += 40;
    if (known && t.url) {
      const u = t.url;
      if (u === known) score += 80;
      else if (knownL && u.toLowerCase() === knownL) score += 80;
      else if (knownL.length > 24 && u.toLowerCase().includes(knownL.slice(0, 48))) score += 35;
      if (knownL.includes("gameid=") && u.toLowerCase().includes("gameid=")) score += 15;
      if (u.includes("h.api.4399.com")) score += 20;
    }
    if (t.type === "page" && typeof t.tabId === "number" && t.tabId === pageTabId) {
      if (!/g\.php|h\.api\.4399/i.test(t.url || "")) score -= 30;
    }
    return { ...t, score };
  });

  // 硬过滤：必须属于本 tab（tabId 或本页 autoAttach/getTargets 会话）。
  let scoped = all.filter((t) => {
    if (t.score < 10) return false;
    if (typeof t.tabId === "number") return t.tabId === pageTabId;
    return t.fromPageSession === true;
  });

  // 会话 API 被拒时：仅允许「与壳页 DOM iframe.src 完全一致」的 target，仍拒绝其它窗同站不同 URL。
  if (!scoped.length && known) {
    scoped = all.filter((t) => {
      if (t.score < 10) return false;
      if (typeof t.tabId === "number" && t.tabId !== pageTabId) return false;
      return (t.url || "") === known || (t.url || "").toLowerCase() === knownL;
    });
    if (scoped.length) notes.push("fallbackExactDomSrc=" + scoped.length);
  }

  const list = scoped.sort((a, b) => b.score - a.score);

  const hint = all
    .filter((t) => /4399|g\.php|game|client-zmxyol|h5zm/i.test(t.url || "") || t.type === "iframe")
    .slice(0, 8)
    .map(
      (t) =>
        `${t.type}:${t.score}:tab=${typeof t.tabId === "number" ? t.tabId : "-"}:ps=${t.fromPageSession ? 1 : 0}:${(t.url || "").slice(0, 52)}`
    );
  notes.push("candidates=" + list.length);
  notes.push("all=" + byId.size);
  notes.push("pageSession=" + pageSessionCount);
  notes.push("scoped=" + scoped.length);
  if (hint.length) notes.push("hint=" + hint.join(" || "));
  else notes.push("hint=none");
  if (!list.length && all.some((t) => t.score >= 10)) {
    notes.push("refuseBrowserWideIframe");
  }
  return { list, notes };
}

async function tryAttachTarget(targetId) {
  const dbg = { targetId };
  try {
    await chrome.debugger.attach(dbg, "1.3");
    return { ok: true, debuggee: dbg, err: "" };
  } catch (e) {
    const msg = String(e && e.message ? e.message : e);
    if (/already\s*attached/i.test(msg)) {
      return { ok: true, debuggee: dbg, err: "already" };
    }
    return { ok: false, debuggee: null, err: msg };
  }
}

/**
 * 找图最小化 / 切桌面时 Edge 常把 debugger 踢掉；用上次 attach 元数据静默挂回。
 * 不擦除 attachMeta（与 detachDebugger 不同）。
 */
async function ensureAttached() {
  if (attachedDebuggee) return { ok: true, recovered: false };
  if (reattachBusy) {
    return { ok: false, error: "NO_TAB", message: "正在重连 debugger", version: BRIDGE_VERSION };
  }
  const tabId = Number(attachMeta.pageTabId) || 0;
  if (!tabId && !attachMeta.iframeTargetId) {
    return { ok: false, error: "NO_TAB", message: "尚未 attach", version: BRIDGE_VERSION };
  }
  reattachBusy = true;
  suppressDetachClear = true;
  try {
    const targetId = attachMeta.iframeTargetId || "";
    if (targetId) {
      const r = await tryAttachTarget(targetId);
      if (r.ok) {
        attachedDebuggee = r.debuggee;
        attachMeta.focus = "iframe-target";
        attachMeta.via = "iframe";
        pageShotDebuggee = null;
        mirrorInstallTried = false;
        try {
          await ensureCanvasMirrorInstalled();
        } catch (_) {
          /* optional */
        }
        setStatus("attached", "auto-reattach iframe");
        return { ok: true, recovered: true, version: BRIDGE_VERSION };
      }
    }
    if (!tabId) {
      return { ok: false, error: "NO_TAB", message: "尚未 attach", version: BRIDGE_VERSION };
    }
    const pageDebuggee = { tabId };
    try {
      await chrome.debugger.attach(pageDebuggee, "1.3");
    } catch (e) {
      const msg = String(e && e.message ? e.message : e);
      if (!/already\s*attached/i.test(msg)) {
        return { ok: false, error: "NO_TAB", message: msg || "尚未 attach", version: BRIDGE_VERSION };
      }
    }
    let detachedPageForIframe = false;
    try {
      const { list } = await collectGameTargets(pageDebuggee, tabId, attachMeta.url || "");
      if (list && list.length > 0) {
        try {
          await chrome.debugger.detach(pageDebuggee);
          detachedPageForIframe = true;
        } catch (_) {
          /* ignore */
        }
        for (const cand of list.slice(0, 6)) {
          const r = await tryAttachTarget(cand.id);
          if (!r.ok) continue;
          attachedDebuggee = r.debuggee;
          attachMeta.iframeTargetId = String(cand.id || "");
          attachMeta.url = cand.url || attachMeta.url;
          attachMeta.focus = "iframe-target";
          attachMeta.via = "iframe";
          try {
            await chrome.debugger.sendCommand(attachedDebuggee, "Emulation.setFocusEmulationEnabled", {
              enabled: true,
            });
          } catch (_) {
            /* optional */
          }
          await readIframeContentSize(attachedDebuggee);
          pageShotDebuggee = null;
          mirrorInstallTried = false;
          try {
            await ensureCanvasMirrorInstalled();
          } catch (_) {
            /* optional */
          }
          setStatus("attached", "auto-reattach iframe(rediscover)");
          return { ok: true, recovered: true, version: BRIDGE_VERSION };
        }
      }
    } catch (_) {
      /* fall through to page */
    }
    if (detachedPageForIframe) {
      try {
        await chrome.debugger.attach(pageDebuggee, "1.3");
      } catch (e2) {
        const msg2 = String(e2 && e2.message ? e2.message : e2);
        if (!/already\s*attached/i.test(msg2)) {
          return { ok: false, error: "NO_TAB", message: msg2 || "尚未 attach", version: BRIDGE_VERSION };
        }
      }
    }
    attachedDebuggee = pageDebuggee;
    attachMeta.focus = attachMeta.focus === "iframe-target" ? "page" : (attachMeta.focus || "page");
    attachMeta.via = "tab";
    setStatus("attached", "auto-reattach page");
    return { ok: true, recovered: true, version: BRIDGE_VERSION };
  } finally {
    suppressDetachClear = false;
    reattachBusy = false;
  }
}

async function listTargets() {
  try {
    return await chrome.debugger.getTargets();
  } catch (_) {
    return [];
  }
}

async function pickBrowsePage(titleHint, opts) {
  const o = opts && typeof opts === "object" ? opts : {};
  const pages = (await listTargets()).filter(
    (t) => t.type === "page" && typeof t.tabId === "number" && !isUtilityTarget(t)
  );
  const candidates = pages.map((p) => p.title || p.url || "(无标题)");
  const urlHint = String(o.urlHint || "").trim().toLowerCase();
  const hintCore = urlHint.replace(/^https?:\/\//, "").replace(/\/$/, "");

  let focusedWinId = -1;
  let activeTabId = -1;
  try {
    const fw = await chrome.windows.getLastFocused({ populate: true });
    if (fw && typeof fw.id === "number") focusedWinId = fw.id;
    const act = (fw.tabs || []).find((t) => t.active);
    if (act && typeof act.id === "number") activeTabId = act.id;
  } catch (_) {
    /* ignore */
  }

  const urlScore = (p) => {
    if (!urlHint) return 0;
    const u = String(p.url || "").toLowerCase();
    if (!u) return 0;
    const strip = (s) => String(s || "").replace(/^https?:\/\//, "").replace(/\/$/, "");
    if (strip(u) === strip(urlHint)) return 200;
    try {
      const wantRaw = urlHint.startsWith("http") ? urlHint : "https://" + urlHint;
      const want = new URL(wantRaw);
      const got = new URL(u);
      if (want.host === got.host) {
        const wp = (want.pathname || "/").replace(/\/$/, "") || "/";
        const gp = (got.pathname || "/").replace(/\/$/, "") || "/";
        if (wp === gp) return 180;
        if (wp === "/" && gp !== "/") return 15;
        if (wp !== "/" && gp.startsWith(wp)) return 120;
      }
    } catch (_) {
      /* ignore */
    }
    if (hintCore && hintCore.length > 16 && u.includes(hintCore)) return 50;
    return 0;
  };

  const attachedId = Number(attachMeta.pageTabId) || 0;
  const scored = pages.map((p) => {
    const us = urlScore(p);
    const ts = titleHint ? titleMatchScore(titleHint, p.title || "") : 0;
    const active = p.tabId === activeTabId ? 50 : 0;
    // clickRef/search 导航时 urlHint 是「要去的地址」，尚未打开；优先留在已附着的标签，避免 tabs.create。
    const stay = attachedId && p.tabId === attachedId ? 90 : 0;
    return { page: p, total: us + ts + active + stay, us, ts };
  });
  scored.sort((a, b) => b.total - a.total);
  if (scored.length && scored[0].total > 0) {
    return {
      page: scored[0].page,
      candidates,
      pickNote: `browse score=${scored[0].total} url=${scored[0].us} title=${scored[0].ts} active=${activeTabId} win=${focusedWinId}`,
    };
  }

  if (activeTabId > 0) {
    let page = pages.find((p) => p.tabId === activeTabId);
    if (!page) {
      try {
        const t = await chrome.tabs.get(activeTabId);
        if (t && typeof t.id === "number" && !isUtilityTarget({ url: t.url || "", title: t.title || "" })) {
          page = { tabId: t.id, title: t.title || "", url: t.url || "", type: "page" };
        }
      } catch (_) {
        /* ignore */
      }
    }
    if (page) {
      return { page, candidates, pickNote: "browse:activeTab" };
    }
  }
  if (pages.length === 1) return { page: pages[0], candidates, pickNote: "browse:only" };
  return {
    page: null,
    candidates,
    error: pages.length ? "AMBIGUOUS" : "NO_TAB",
    message: "未找到可抓取的网页标签（打开目标页后重试）",
  };
}

async function pickPage(titleHint, opts) {
  const hint = (titleHint || "").trim();
  const o = opts && typeof opts === "object" ? opts : {};
  if (o.preferPage) return pickBrowsePage(hint, o);
  const pages = (await listTargets()).filter(
    (t) => t.type === "page" && typeof t.tabId === "number" && !isUtilityTarget(t)
  );
  const candidates = pages.map((p) => p.title || p.url || "(无标题)");
  if (!hint) {
    if (pages.length === 1) return { page: pages[0], candidates };
    return { page: null, candidates, error: pages.length ? "AMBIGUOUS" : "NO_TAB" };
  }

  const matched = pages
    .map((p) => ({ page: p, titleScore: titleMatchScore(hint, p.title || "") }))
    .filter((x) => x.titleScore > 0);
  if (!matched.length) return { page: null, candidates };

  let focusedWinId = -1;
  try {
    const fw = await chrome.windows.getLastFocused();
    if (fw && typeof fw.id === "number") focusedWinId = fw.id;
  } catch (_) {
    /* ignore */
  }

  const boundL = Number(o.boundLeft);
  const boundT = Number(o.boundTop);
  const boundR = Number(o.boundRight);
  const boundB = Number(o.boundBottom);
  const hasBound =
    Number.isFinite(boundL) &&
    Number.isFinite(boundT) &&
    Number.isFinite(boundR) &&
    Number.isFinite(boundB) &&
    boundR > boundL &&
    boundB > boundT;
  const preferUnfocused = o.preferUnfocused === true || o.preferUnfocused === 1;

  const scored = [];
  for (const m of matched) {
    let winId = -1;
    let geomScore = 0;
    let unfocusedBonus = 0;
    try {
      const tab = await chrome.tabs.get(m.page.tabId);
      if (tab && typeof tab.windowId === "number") winId = tab.windowId;
      if (hasBound && winId >= 0) {
        const w = await chrome.windows.get(winId);
        if (w && Number.isFinite(w.left) && Number.isFinite(w.top)) {
          const ww = Number(w.width) || 0;
          const wh = Number(w.height) || 0;
          const cx = boundL + (boundR - boundL) / 2;
          const cy = boundT + (boundB - boundT) / 2;
          const wx = Number(w.left) + ww / 2;
          const wy = Number(w.top) + wh / 2;
          const dist = Math.hypot(cx - wx, cy - wy);
          // 同屏最大化窗 dist≈0；另一显示器/位置则拉开差距。
          geomScore = Math.max(0, 80 - Math.min(80, dist / 20));
        }
      }
      if (preferUnfocused && focusedWinId >= 0 && winId >= 0 && winId !== focusedWinId) {
        unfocusedBonus = 35;
      } else if (!preferUnfocused && focusedWinId >= 0 && winId === focusedWinId) {
        unfocusedBonus = 25;
      }
    } catch (_) {
      /* tab/window 可能已关 */
    }
    const urlBonus = Math.max(0, Math.min(30, gameUrlScore(m.page.url || "")));
    scored.push({
      page: m.page,
      winId,
      total: m.titleScore + geomScore + unfocusedBonus + urlBonus,
      titleScore: m.titleScore,
      geomScore,
      unfocusedBonus,
      urlBonus,
    });
  }

  scored.sort((a, b) => b.total - a.total);
  const best = scored[0];
  const second = scored[1];
  // 多个同名标签：仍返回最高分供旧路径；宿主应用 listPages + 截图校验消歧。
  return {
    page: best.page,
    candidates,
    pickNote: `score=${best.total} title=${best.titleScore} geom=${best.geomScore} unfocus=${best.unfocusedBonus} url=${best.urlBonus} win=${best.winId} n=${scored.length}`,
    ambiguous: !!(second && best.titleScore >= 40 && second.titleScore >= 40),
  };
}

/** 列出候选标签：优先 chrome.tabs/windows（含其它虚拟桌面），再合并 debugger targets。 */
async function listPages(titleHint) {
  const hint = (titleHint || "").trim();
  const byTab = new Map();

  const add = (tabId, title, url, windowId, titleScore, urlScore) => {
    if (typeof tabId !== "number" || tabId <= 0) return;
    const u = url || "";
    if (
      u.startsWith("edge://") ||
      u.startsWith("chrome://") ||
      u.startsWith("devtools://") ||
      u.startsWith("chrome-extension://") ||
      u.startsWith("extension://")
    ) {
      return;
    }
    const prev = byTab.get(tabId);
    const row = {
      tabId,
      title: title || "",
      url: (u || "").slice(0, 180),
      windowId: typeof windowId === "number" ? windowId : -1,
      titleScore: titleScore || 0,
      urlScore: urlScore || 0,
    };
    if (!prev || row.titleScore + row.urlScore > prev.titleScore + prev.urlScore) {
      byTab.set(tabId, row);
    }
  };

  // 1) tabs API：通常能看到其它虚拟桌面上的 Edge 窗（debugger.getTargets 常漏）。
  try {
    const wins = await chrome.windows.getAll({ populate: true });
    for (const w of wins || []) {
      const winId = typeof w.id === "number" ? w.id : -1;
      for (const t of w.tabs || []) {
        if (typeof t.id !== "number") continue;
        const title = t.title || "";
        const url = t.url || "";
        const ts = hint ? titleMatchScore(hint, title) : 0;
        const us = Math.max(0, gameUrlScore(url));
        if (!hint || ts > 0 || us >= 18) add(t.id, title, url, winId, ts, us);
      }
    }
  } catch (_) {
    /* ignore */
  }
  try {
    const tabs = await chrome.tabs.query({});
    for (const t of tabs || []) {
      if (typeof t.id !== "number") continue;
      const title = t.title || "";
      const url = t.url || "";
      const ts = hint ? titleMatchScore(hint, title) : 0;
      const us = Math.max(0, gameUrlScore(url));
      if (!hint || ts > 0 || us >= 18) {
        add(t.id, title, url, t.windowId, ts, us);
      }
    }
  } catch (_) {
    /* ignore */
  }

  // 2) debugger targets 补充
  for (const p of await listTargets()) {
    if (p.type !== "page" || typeof p.tabId !== "number" || isUtilityTarget(p)) continue;
    const title = p.title || "";
    const url = p.url || "";
    const ts = hint ? titleMatchScore(hint, title) : 0;
    const us = Math.max(0, gameUrlScore(url));
    if (!hint || ts > 0 || us >= 18) {
      let windowId = -1;
      try {
        const tab = await chrome.tabs.get(p.tabId);
        if (tab && typeof tab.windowId === "number") windowId = tab.windowId;
      } catch (_) {}
      add(p.tabId, title, url, windowId, ts, us);
    }
  }

  let matched = [...byTab.values()];
  // 有标题提示时：先保留标题命中；若没有，再退回高 url 分（避免漏掉宏桌面窗）。
  if (hint) {
    const titled = matched.filter((m) => m.titleScore > 0);
    if (titled.length) matched = titled;
    else matched = matched.filter((m) => m.urlScore >= 18);
  }
  matched.sort(
    (a, b) => b.titleScore + b.urlScore - (a.titleScore + a.urlScore) || b.titleScore - a.titleScore
  );

  // 每个 windowId 至少保留分最高的一个，避免同窗多标签刷屏；再按分取前 8。
  const bestPerWin = new Map();
  const noWin = [];
  for (const m of matched) {
    if (m.windowId < 0) {
      noWin.push(m);
      continue;
    }
    const prev = bestPerWin.get(m.windowId);
    if (!prev || m.titleScore + m.urlScore > prev.titleScore + prev.urlScore) {
      bestPerWin.set(m.windowId, m);
    }
  }
  matched = [...bestPerWin.values(), ...noWin];
  matched.sort(
    (a, b) => b.titleScore + b.urlScore - (a.titleScore + a.urlScore) || b.titleScore - a.titleScore
  );
  matched = matched.slice(0, 8);

  return {
    ok: true,
    version: BRIDGE_VERSION,
    count: matched.length,
    tabIds: matched.map((m) => m.tabId),
    windowIds: matched.map((m) => m.windowId),
    titles: matched.map((m) => m.title),
    urls: matched.map((m) => m.url),
    titleScores: matched.map((m) => m.titleScore),
    source: "tabs+targets",
  };
}

async function detachDebugger() {
  sessionActive = false;
  visionPreferPageClip = false;
  mirrorInstallTried = false;
  lastMirrorFramesSeen = -1;
  suppressDetachClear = true;
  try {
    if (pageShotDebuggee) {
      try {
        await chrome.debugger.detach(pageShotDebuggee);
      } catch (_) {
        /* ignore */
      }
      pageShotDebuggee = null;
    }
    if (attachedDebuggee) {
      try {
        await chrome.debugger.detach(attachedDebuggee);
      } catch (_) {
        /* ignore */
      }
      attachedDebuggee = null;
    }
  } finally {
    suppressDetachClear = false;
  }
  attachMeta = {
    dpr: 1,
    offsetX: 0,
    offsetY: 0,
    focus: "",
    url: "",
    via: "tab",
    pageTabId: 0,
    iframeTargetId: "",
    iframeCssX: 0,
    iframeCssY: 0,
    iframeCssW: 0,
    iframeCssH: 0,
    contentW: 0,
    contentH: 0,
    pageCssW: 0,
    pageCssH: 0,
  };
}

async function prepareFocusAndMetrics(pageDebuggee) {
  const meta = {
    dpr: 1,
    offsetX: 0,
    offsetY: 0,
    focus: "page",
    url: "",
    via: "tab",
    pageTabId: 0,
    iframeTargetId: "",
    iframeCssX: 0,
    iframeCssY: 0,
    iframeCssW: 0,
    iframeCssH: 0,
    contentW: 0,
    contentH: 0,
    pageCssW: 0,
    pageCssH: 0,
  };
  try {
    await chrome.debugger.sendCommand(pageDebuggee, "Emulation.setFocusEmulationEnabled", {
      enabled: true,
    });
  } catch (_) {
    /* optional */
  }
  try {
    // 最小化/后台时 Chronium 会冻 rAF；强制 active 生命周期保 H5 有反应。
    await chrome.debugger.sendCommand(pageDebuggee, "Page.setWebLifecycleState", {
      state: "active",
    });
  } catch (_) {
    /* optional */
  }
  // 不要 Page.bringToFront：会把已最小化的 Edge 从「鼠标宏」桌面唤醒，
  // 导致用户桌面任务栏/多桌面预览仍能看到该窗口。
  try {
    const dprRes = await chrome.debugger.sendCommand(pageDebuggee, "Runtime.evaluate", {
      expression:
        "({dpr:Number(window.devicePixelRatio)||1,w:innerWidth||0,h:innerHeight||0})",
      returnByValue: true,
    });
    const dv = dprRes && dprRes.result && dprRes.result.value;
    if (dv && typeof dv === "object") {
      const dpr = Number(dv.dpr);
      if (dpr > 0.1 && dpr < 8) meta.dpr = dpr;
      meta.pageCssW = Number(dv.w) || 0;
      meta.pageCssH = Number(dv.h) || 0;
    }
  } catch (_) {
    /* keep 1 */
  }
  try {
    const focusRes = await chrome.debugger.sendCommand(pageDebuggee, "Runtime.evaluate", {
      expression: `(() => {
        const score = (u) => {
          u = (u || "").toLowerCase();
          let s = 0;
          if (u.includes("4399")) s += 20;
          if (u.includes("game") || u.includes("play")) s += 6;
          if (u.includes("login") || u.includes("passport")) s -= 8;
          return s;
        };
        const frames = [...document.querySelectorAll("iframe")];
        frames.sort((a, b) => score(b.src) - score(a.src));
        const f = frames.find((x) => score(x.src) > 0) || frames[0];
        if (f) {
          // 勿 scrollIntoView / 勿真实 focus：跨虚拟桌面时会把用户视图拽到宏桌面。
          // Emulation.setFocusEmulationEnabled 已足够投递键鼠。
          const r = f.getBoundingClientRect();
          return {
            focus: "iframe",
            x: r.left,
            y: r.top,
            w: r.width,
            h: r.height,
            src: f.src || "",
          };
        }
        const c = document.querySelector("canvas");
        if (c) {
          const r = c.getBoundingClientRect();
          return { focus: "canvas", x: r.left, y: r.top, w: r.width, h: r.height, src: "" };
        }
        return { focus: "window", x: 0, y: 0, w: innerWidth, h: innerHeight, src: location.href || "" };
      })()`,
      returnByValue: true,
    });
    const v = focusRes && focusRes.result && focusRes.result.value;
    if (v && typeof v === "object") {
      meta.focus = String(v.focus || "page");
      meta.url = String(v.src || "");
      // 仅当后续改 attach 到 iframe 视口时才需要 offset；默认仍按顶层页坐标投递
      meta.offsetX = 0;
      meta.offsetY = 0;
      meta.iframeCssX = Number(v.x) || 0;
      meta.iframeCssY = Number(v.y) || 0;
      meta.iframeCssW = Number(v.w) || 0;
      meta.iframeCssH = Number(v.h) || 0;
    }
  } catch (_) {
    /* ignore */
  }
  return meta;
}

async function attachTab(titleHint, opts) {
  const o = opts && typeof opts === "object" ? opts : {};
  const preferPage = !!o.preferPage;
  const wantTabId = Number(o.tabId);
  let page = null;
  let candidates = [];
  let pickNote = "";
  let error = "";
  let message = "";

  if (Number.isFinite(wantTabId) && wantTabId > 0) {
    const pages = (await listTargets()).filter(
      (t) => t.type === "page" && typeof t.tabId === "number" && !isUtilityTarget(t)
    );
    candidates = pages.map((p) => p.title || p.url || "(无标题)");
    page = pages.find((p) => p.tabId === wantTabId) || null;
    pickNote = `tabId=${wantTabId}`;
    if (!page) {
      // tabs.query 能看到、debugger.getTargets 暂无：仍按 tabId 合成目标。
      try {
        const t = await chrome.tabs.get(wantTabId);
        if (t && typeof t.id === "number") {
          page = {
            tabId: t.id,
            title: t.title || "",
            url: t.url || "",
            type: "page",
          };
          pickNote = `tabId=${wantTabId}:tabsApi`;
        }
      } catch (_) {
        /* ignore */
      }
    }
    if (!page) {
      error = "NO_TAB";
      message = `指定 tabId=${wantTabId} 不存在`;
    }
  } else {
    const picked = await pickPage(titleHint, o);
    page = picked.page;
    candidates = picked.candidates || [];
    pickNote = picked.pickNote || "";
    error = picked.error || "";
    message = picked.message || "";
  }

  if (!page) {
    const errCode = error === "AMBIGUOUS" ? "AMBIGUOUS" : "NO_TAB";
    setStatus("no-tab", message || `未匹配到目标页（可见 ${candidates.length} 个）`);
    return {
      ok: false,
      error: errCode,
      message: message || (preferPage ? "未找到可抓取的网页标签" : "未找到匹配的游戏标签页"),
      candidates: (candidates || []).slice(0, 12),
      version: BRIDGE_VERSION,
    };
  }

  await detachDebugger();

  const pageDebuggee = { tabId: page.tabId };
  try {
    await chrome.debugger.attach(pageDebuggee, "1.3");
  } catch (e) {
    return {
      ok: false,
      error: "DEBUGGER_DENIED",
      message: String(e && e.message ? e.message : e),
      version: BRIDGE_VERSION,
    };
  }

  let meta = await prepareFocusAndMetrics(pageDebuggee);
  let debuggee = pageDebuggee;
  let attachedVia = "tab";
  let attachNote = "";
  let stamped = false;

  // 宿主消歧：在切到 iframe 前改壳页 title，Win32 GetWindowText(绑定窗) 可核对。
  // 先剥掉残留 QST 戳记，避免多次 attach 叠成 QST…:QST…:原标题。
  const stampMarker = typeof o.stampMarker === "string" ? o.stampMarker : "";
  if (stampMarker) {
    try {
      await chrome.debugger.sendCommand(pageDebuggee, "Runtime.evaluate", {
        expression: `(()=>{const m=${JSON.stringify(String(stampMarker).slice(0, 48))};try{let t=String(document.title||"");while(/^QST[0-9A-Fa-f]{8}:/.test(t))t=t.replace(/^QST[0-9A-Fa-f]{8}:/,"");document.title=m+t;}catch(e){}return m;})()`,
        returnByValue: true,
      });
      stamped = true;
    } catch (e) {
      attachNote += "stampFail:" + (e && e.message ? e.message : e);
    }
  }

  // 网页 Agent（observePage）要壳页 DOM：禁止被游戏 iframe 抢走 debugger。
  // 先点一下游戏 iframe 中心，尽量激活（仍可能因跨域不够，后面会挂 iframe target）
  if (!preferPage) {
  try {
    const ix = (Number(meta.iframeCssX) || 0) + Math.max(10, (Number(meta.iframeCssW) || 0) / 2);
    const iy = (Number(meta.iframeCssY) || 0) + Math.max(10, (Number(meta.iframeCssH) || 0) / 2);
    if (ix > 0 && iy > 0) {
      for (const type of ["mousePressed", "mouseReleased"]) {
        await chrome.debugger.sendCommand(pageDebuggee, "Input.dispatchMouseEvent", {
          type,
          x: ix,
          y: iy,
          button: "left",
          buttons: type === "mousePressed" ? 1 : 0,
          clickCount: 1,
          pointerType: "mouse",
        });
      }
    }
  } catch (_) {
    /* ignore */
  }
  }

  // 4399 游戏在跨域 iframe：必须 chrome.debugger.attach(targetId)，否则键鼠只打在壳页。
  // 关键：父页仍 attach 时，Edge 常拒绝再挂 iframe → 必须先 detach 壳页再挂 targetId。
  try {
    if (preferPage) {
      attachNote += "|preferPage";
      throw new Error("skip-iframe");
    }
    const { list, notes } = await collectGameTargets(pageDebuggee, page.tabId, meta.url || "");
    attachNote = notes.join(";");
    const topUrls = list.slice(0, 5).map((t) => `${t.type}:${t.score}:${(t.url || "").slice(0, 80)}`);
    if (topUrls.length) attachNote += "|top=" + topUrls.join(" || ");

    let attached = false;
    if (list.length > 0) {
      try {
        await chrome.debugger.detach(pageDebuggee);
      } catch (_) {
        /* ignore */
      }
      for (const cand of list.slice(0, 8)) {
        const r = await tryAttachTarget(cand.id);
        if (!r.ok) {
          attachNote += `|fail:${String(cand.id).slice(0, 8)}:${r.err}`;
          continue;
        }
        debuggee = r.debuggee;
        attachedVia = "iframe";
        meta.url = cand.url || meta.url;
        meta.focus = "iframe-target";
        meta.via = "iframe";
        meta.iframeTargetId = String(cand.id || "");
        // 宿主坐标仍是「整页客户区」像素；投递时在 adjustMouseParams 里减壳页 iframe 原点并映射到内容尺寸。
        meta.offsetX = 0;
        meta.offsetY = 0;
        try {
          await chrome.debugger.sendCommand(debuggee, "Emulation.setFocusEmulationEnabled", {
            enabled: true,
          });
        } catch (_) {
          /* optional */
        }
        try {
          await chrome.debugger.sendCommand(debuggee, "Runtime.evaluate", {
            expression:
              "(() => { try { const c=document.querySelector('canvas'); return location.href; } catch(e) { return String(e); } })()",
            returnByValue: true,
          });
        } catch (_) {
          /* optional */
        }
        try {
          const sizeRes = await chrome.debugger.sendCommand(debuggee, "Runtime.evaluate", {
            expression:
              "({w:innerWidth||0,h:innerHeight||0,dpr:Number(devicePixelRatio)||1})",
            returnByValue: true,
          });
          const sz = sizeRes && sizeRes.result && sizeRes.result.value;
          if (sz && typeof sz === "object") {
            meta.contentW = Number(sz.w) || 0;
            meta.contentH = Number(sz.h) || 0;
            const idpr = Number(sz.dpr);
            if (idpr > 0.1 && idpr < 8) meta.dpr = idpr;
          }
        } catch (_) {
          /* optional */
        }
        attached = true;
        attachNote += `|ok:${cand.type}:${(cand.url || "").slice(0, 60)}`;
        attachNote += `|iframeCss=${Math.round(meta.iframeCssX)},${Math.round(meta.iframeCssY)} ${Math.round(meta.iframeCssW)}x${Math.round(meta.iframeCssH)}`;
        attachNote += `|content=${Math.round(meta.contentW)}x${Math.round(meta.contentH)}`;
        // 找图优先 iframe canvas（无白闪）；不再常驻双挂壳页——
        // Page.captureScreenshot 即使常驻双挂，可见窗仍会白屏（1.1.20 已证伪）。
        pageShotDebuggee = null;
        attachNote += "|pageShot=off(mirror-first)";
        break;
      }
      if (!attached) {
        // 挂 iframe 全失败：重新挂回壳页，至少还能调试
        try {
          await chrome.debugger.attach(pageDebuggee, "1.3");
          debuggee = pageDebuggee;
          attachNote += "|reattach=tab";
        } catch (e2) {
          attachNote += "|reattachFail:" + (e2 && e2.message ? e2.message : e2);
        }
      }
    }
    if (!attached) {
      attachNote += "|fallback=tab";
      meta.offsetX = 0;
      meta.offsetY = 0;
    }
  } catch (e) {
    const msg = String(e && e.message ? e.message : e);
    if (msg !== "skip-iframe")
      attachNote = "collect:" + msg;
  }

  attachedDebuggee = debuggee;
  meta.via = attachedVia;
  meta.pageTabId = page.tabId;
  attachMeta = meta;
  sessionActive = true;
  cdpLogLeft = 8;
  visionPreferPageClip = false;
  mirrorInstallTried = false;

  // WebGL 找图：挂 rAF 镜像（游戏画完立刻拷帧），避免 Page.captureScreenshot 白闪。
  if (attachedVia === "iframe" || meta.focus === "iframe-target") {
    try {
      const mir = await ensureCanvasMirrorInstalled();
      attachNote += mir.ok
        ? `|mirror=${mir.already ? "reuse" : "on"}`
        : `|mirrorFail:${String(mir.err || "?").slice(0, 40)}`;
    } catch (eM) {
      attachNote += "|mirrorEx:" + String(eM && eM.message ? eM.message : eM).slice(0, 40);
    }
  }

  // 宿主约 180ms 后读 HWND 标题校验戳记；之后清掉（iframe 上改不了壳页 title）。
  if (stamped) {
    setTimeout(() => {
      void clearShellTitleStamp();
    }, 700);
  }

  const detail =
    `${page.title || ""} | via=${attachedVia} dpr=${meta.dpr} focus=${meta.focus}` +
    ` iframe=${Math.round(meta.iframeCssX)},${Math.round(meta.iframeCssY)}` +
    ` ${Math.round(meta.iframeCssW)}x${Math.round(meta.iframeCssH)}` +
    ` content=${Math.round(meta.contentW)}x${Math.round(meta.contentH)}`;
  setStatus("attached", `v${BRIDGE_VERSION} ${detail}`);
  return {
    ok: true,
    tabId: page.tabId,
    title: page.title || "",
    version: BRIDGE_VERSION,
    via: attachedVia,
    dpr: meta.dpr,
    focus: meta.focus,
    url: meta.url || "",
    pickNote: pickNote || "",
    stamped: stamped,
    iframeCssX: meta.iframeCssX,
    iframeCssY: meta.iframeCssY,
    iframeCssW: meta.iframeCssW,
    iframeCssH: meta.iframeCssH,
    contentW: meta.contentW,
    contentH: meta.contentH,
    // 宿主 MapCanvas→surface / 鼠标映射依赖 pageCss；漏传则 pageCss=0 → 找图落点整窗拉伸点偏。
    pageCssW: meta.pageCssW,
    pageCssH: meta.pageCssH,
    note: attachNote.slice(0, 480),
  };
}

/** 壳页注入：量 iframe 矩形 / 清标题戳记。禁止 debugger soft-swap（会弄死 MV3 WS）。 */
async function runInShellTab(tabId, func, args) {
  if (!tabId || typeof chrome.scripting === "undefined" || !chrome.scripting.executeScript) {
    throw new Error("scripting unavailable");
  }
  const results = await chrome.scripting.executeScript({
    target: { tabId },
    world: "MAIN",
    func,
    args: args || [],
  });
  return results && results[0] ? results[0].result : null;
}

function shellMeasureLayoutFn() {
  const score = (u) => {
    u = (u || "").toLowerCase();
    let s = 0;
    if (u.includes("4399")) s += 20;
    if (u.includes("game") || u.includes("play")) s += 6;
    if (u.includes("login") || u.includes("passport")) s -= 8;
    return s;
  };
  const frames = [...document.querySelectorAll("iframe")];
  frames.sort((a, b) => score(b.src) - score(a.src));
  const f = frames.find((x) => score(x.src) > 0) || frames[0];
  let focus = "window";
  let x = 0;
  let y = 0;
  let w = innerWidth || 0;
  let h = innerHeight || 0;
  let src = location.href || "";
  if (f) {
    const r = f.getBoundingClientRect();
    focus = "iframe";
    x = r.left;
    y = r.top;
    w = r.width;
    h = r.height;
    src = f.src || "";
  } else {
    const c = document.querySelector("canvas");
    if (c) {
      const r = c.getBoundingClientRect();
      focus = "canvas";
      x = r.left;
      y = r.top;
      w = r.width;
      h = r.height;
      src = "";
    }
  }
  return {
    focus,
    x,
    y,
    w,
    h,
    src,
    dpr: Number(window.devicePixelRatio) || 1,
    pageW: innerWidth || 0,
    pageH: innerHeight || 0,
  };
}

function shellClearStampFn() {
  try {
    let t = String(document.title || "");
    while (/^QST[0-9A-Fa-f]{8}:/.test(t)) t = t.replace(/^QST[0-9A-Fa-f]{8}:/, "");
    document.title = t;
  } catch (e) {}
  return true;
}

/** 用 scripting 清壳页标题戳记（不 detach debugger，不断桥）。 */
async function clearShellTitleStamp() {
  const tabId = Number(attachMeta.pageTabId) || 0;
  if (!tabId) return;
  try {
    await runInShellTab(tabId, shellClearStampFn);
  } catch (_) {
    /* 忽略：下次 attach 会再剥前缀 */
  }
}

async function readIframeContentSize(debuggee) {
  try {
    const sizeRes = await chrome.debugger.sendCommand(debuggee, "Runtime.evaluate", {
      expression: "({w:innerWidth||0,h:innerHeight||0,dpr:Number(devicePixelRatio)||1})",
      returnByValue: true,
    });
    const sz = sizeRes && sizeRes.result && sizeRes.result.value;
    if (sz && typeof sz === "object") {
      attachMeta.contentW = Number(sz.w) || 0;
      attachMeta.contentH = Number(sz.h) || 0;
      const idpr = Number(sz.dpr);
      if (idpr > 0.1 && idpr < 8) attachMeta.dpr = idpr;
    }
  } catch (_) {
    /* optional */
  }
}

/// 重测壳页 iframe 矩形：只用 scripting，保持 iframe debugger 不断开（防 MV3 断桥）。
async function refreshLayout() {
  const tabId = Number(attachMeta.pageTabId) || 0;
  if (!tabId) {
    return { ok: false, error: "NO_TAB", message: "无 pageTabId", version: BRIDGE_VERSION };
  }
  const via = attachMeta.via || "tab";
  try {
    const measured = await runInShellTab(tabId, shellMeasureLayoutFn);
    if (!measured || typeof measured !== "object") {
      return {
        ok: false,
        error: "LAYOUT_FAIL",
        message: "scripting 未返回布局",
        version: BRIDGE_VERSION,
      };
    }
    attachMeta.iframeCssX = Number(measured.x) || 0;
    attachMeta.iframeCssY = Number(measured.y) || 0;
    attachMeta.iframeCssW = Number(measured.w) || 0;
    attachMeta.iframeCssH = Number(measured.h) || 0;
    const dpr = Number(measured.dpr);
    if (dpr > 0.1 && dpr < 8) attachMeta.dpr = dpr;
    attachMeta.pageCssW = Number(measured.pageW) || 0;
    attachMeta.pageCssH = Number(measured.pageH) || 0;
    if (measured.src) attachMeta.url = String(measured.src);
  } catch (e) {
    return {
      ok: false,
      error: "LAYOUT_FAIL",
      message: String(e && e.message ? e.message : e),
      version: BRIDGE_VERSION,
    };
  }

  // 内容尺寸仍从已挂着的 iframe debugger 读，勿 soft-swap。
  if (attachedDebuggee && (attachMeta.focus === "iframe-target" || attachMeta.iframeTargetId)) {
    await readIframeContentSize(attachedDebuggee);
  } else {
    const ready = await ensureAttached();
    if (ready.ok && attachedDebuggee) await readIframeContentSize(attachedDebuggee);
  }

  const detail =
    `layout via=${via}/scripting dpr=${attachMeta.dpr}` +
    ` iframe=${Math.round(attachMeta.iframeCssX)},${Math.round(attachMeta.iframeCssY)}` +
    ` ${Math.round(attachMeta.iframeCssW)}x${Math.round(attachMeta.iframeCssH)}` +
    ` content=${Math.round(attachMeta.contentW)}x${Math.round(attachMeta.contentH)}`;
  setStatus("attached", `v${BRIDGE_VERSION} ${detail}`);
  return {
    ok: true,
    version: BRIDGE_VERSION,
    via,
    dpr: attachMeta.dpr,
    focus: attachMeta.focus,
    iframeCssX: attachMeta.iframeCssX,
    iframeCssY: attachMeta.iframeCssY,
    iframeCssW: attachMeta.iframeCssW,
    iframeCssH: attachMeta.iframeCssH,
    contentW: attachMeta.contentW,
    contentH: attachMeta.contentH,
    pageCssW: attachMeta.pageCssW,
    pageCssH: attachMeta.pageCssH,
  };
}

function mapHostToTarget(hostX, hostY, surfaceW, surfaceH) {
  const dpr = attachMeta.dpr > 0.1 ? attachMeta.dpr : 1;
  const ox = Number(attachMeta.iframeCssX) || 0;
  const oy = Number(attachMeta.iframeCssY) || 0;
  const ew = Number(attachMeta.iframeCssW) || 0;
  const eh = Number(attachMeta.iframeCssH) || 0;
  const cw = Number(attachMeta.contentW) || ew;
  const ch = Number(attachMeta.contentH) || eh;
  const pageCssW = Number(attachMeta.pageCssW) || Math.max(ew + ox, 1);
  const pageCssH = Number(attachMeta.pageCssH) || Math.max(eh + oy, 1);
  let scaleX = dpr;
  let scaleY = dpr;
  if (surfaceW > 100 && pageCssW > 100) {
    const rx = surfaceW / pageCssW;
    if (rx > 0.5 && rx < 4) scaleX = rx;
  }
  if (surfaceH > 100 && pageCssH > 100) {
    const ry = surfaceH / pageCssH;
    if (ry > 0.5 && ry < 4) scaleY = ry;
  }

  let x = hostX;
  let y = hostY;
  if (attachMeta.focus === "iframe-target") {
    const ix0 = ox * scaleX;
    const iy0 = oy * scaleY;
    const iw = ew * scaleX;
    const ih = eh * scaleY;
    if (iw > 1 && ih > 1 && cw > 1 && ch > 1) {
      x = ((hostX - ix0) / iw) * cw;
      y = ((hostY - iy0) / ih) * ch;
    } else if (surfaceW > 1 && surfaceH > 1 && cw > 1 && ch > 1) {
      x = (hostX / surfaceW) * cw;
      y = (hostY / surfaceH) * ch;
    } else {
      x = hostX / scaleX - ox;
      y = hostY / scaleY - oy;
    }
    // 宿主若误把点映射到 iframe 外（负坐标），钳回内容区，避免点击落空。
    if (cw > 1 && ch > 1) {
      if (x < 0) x = 0;
      if (y < 0) y = 0;
      if (x > cw - 1) x = cw - 1;
      if (y > ch - 1) y = ch - 1;
    }
  } else {
    x = hostX / scaleX - (Number(attachMeta.offsetX) || 0);
    y = hostY / scaleY - (Number(attachMeta.offsetY) || 0);
  }
  return {
    x,
    y,
    scaleX,
    scaleY,
    pageX: hostX / scaleX,
    pageY: hostY / scaleY,
  };
}

function adjustMouseParams(params) {
  const p = Object.assign({}, params || {});
  const surfaceW = Number(p.surfaceW) || 0;
  const surfaceH = Number(p.surfaceH) || 0;
  const hostX = Number(p.x);
  const hostY = Number(p.y);
  delete p.surfaceW;
  delete p.surfaceH;
  if (typeof hostX === "number" && typeof hostY === "number" && !Number.isNaN(hostX)) {
    const m = mapHostToTarget(hostX, hostY, surfaceW, surfaceH);
    p.x = m.x;
    p.y = m.y;
    p._hostX = hostX;
    p._hostY = hostY;
    p._mappedX = m.x;
    p._mappedY = m.y;
  }
  if (p.type === "mousePressed") {
    if (p.button === "left") p.buttons = 1;
    else if (p.button === "right") p.buttons = 2;
    else if (p.button === "middle") p.buttons = 4;
  } else if (p.type === "mouseReleased" || p.type === "mouseMoved") {
    if (p.type === "mouseReleased") p.buttons = 0;
  }
  if (!p.pointerType) p.pointerType = "mouse";
  return p;
}

function sleepMs(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

async function dispatchMouse(type, x, y, button) {
  const btn = button || "none";
  const pressed = type === "mousePressed";
  const params = {
    type,
    x,
    y,
    button: type === "mouseMoved" ? "none" : btn,
    buttons: pressed ? (btn === "right" ? 2 : 1) : 0,
    clickCount: type === "mouseMoved" ? 0 : 1,
    pointerType: "mouse",
  };
  if (type === "mouseMoved") {
    delete params.clickCount;
    params.button = "none";
    params.buttons = 0;
  }
  await chrome.debugger.sendCommand(attachedDebuggee, "Input.dispatchMouseEvent", params);
}

/** 部分 H5/造梦系只吃触摸；与鼠标一并投递。 */
async function dispatchTouchClick(x, y) {
  try {
    await chrome.debugger.sendCommand(attachedDebuggee, "Input.dispatchTouchEvent", {
      type: "touchStart",
      touchPoints: [{ x, y, radiusX: 2, radiusY: 2, force: 1, id: 0 }],
    });
    await sleepMs(30);
    await chrome.debugger.sendCommand(attachedDebuggee, "Input.dispatchTouchEvent", {
      type: "touchEnd",
      touchPoints: [],
    });
    return true;
  } catch (_) {
    return false;
  }
}

/** CDP Input 对部分 H5/canvas 菜单无效时，在 iframe 文档内再补一枪 DOM 合成点击。 */
async function domClickAt(x, y) {
  try {
    const res = await chrome.debugger.sendCommand(attachedDebuggee, "Runtime.evaluate", {
      expression: `(() => {
        const x = ${Number(x)}, y = ${Number(y)};
        const el = document.elementFromPoint(x, y);
        if (!el) return { ok: false, reason: "no-el" };
        const fire = (Ctor, type, buttons) => {
          try {
            el.dispatchEvent(new Ctor(type, {
              bubbles: true, cancelable: true, view: window,
              clientX: x, clientY: y, screenX: x, screenY: y,
              button: 0, buttons, detail: type === "click" ? 1 : 0,
              pointerId: 1, pointerType: "mouse", isPrimary: true,
            }));
          } catch (e) {}
        };
        fire(PointerEvent, "pointerdown", 1);
        fire(MouseEvent, "mousedown", 1);
        fire(PointerEvent, "pointerup", 0);
        fire(MouseEvent, "mouseup", 0);
        fire(MouseEvent, "click", 0);
        return {
          ok: true,
          tag: el.tagName || "",
          id: el.id || "",
          cls: String(el.className || "").slice(0, 60),
        };
      })()`,
      returnByValue: true,
      awaitPromise: false,
    });
    return (res && res.result && res.result.value) || { ok: false, reason: "no-result" };
  } catch (e) {
    return { ok: false, reason: String(e && e.message ? e.message : e) };
  }
}

/** 宿主专用：move / click，返回 host→目标 映射，便于判断是坐标问题还是点击未送达。 */
async function handleMouseCommand(msg) {
  const ready = await ensureAttached();
  if (!ready.ok || !attachedDebuggee) {
    return {
      ok: false,
      error: ready.error || "NO_TAB",
      message: ready.message || "尚未 attach",
      version: BRIDGE_VERSION,
    };
  }
  const hostX = Number(msg.x) || 0;
  const hostY = Number(msg.y) || 0;
  const surfaceW = Number(msg.surfaceW) || 0;
  const surfaceH = Number(msg.surfaceH) || 0;
  const button = msg.button === "right" ? "right" : msg.button === "middle" ? "middle" : "left";
  const action = String(msg.action || "click");
  const m = mapHostToTarget(hostX, hostY, surfaceW, surfaceH);
  const x = m.x;
  const y = m.y;

  try {
    // 用户点了其它窗口后页面可能失焦；软恢复焦点仿真，不 detach、不抢系统前台。
    try {
      await chrome.debugger.sendCommand(attachedDebuggee, "Emulation.setFocusEmulationEnabled", {
        enabled: true,
      });
    } catch (_) {}
    await dispatchMouse("mouseMoved", x, y, "none");
    // touch 在 reply 后异步补发（见 mouse 消息处理），避免 await 卡住桥超时。
    // DOM 合成点击已停用（同步 Runtime.evaluate 会卡游戏主线程）；CDP Input 为主路径。
    const touch = false;
    if (action === "click") {
      // 热路径只发 CDP 三连；同步 touch/DOM 会卡住游戏主线程导致宿主超时。
      await dispatchMouse("mousePressed", x, y, button);
      await sleepMs(16);
      await dispatchMouse("mouseReleased", x, y, button);
    } else if (action === "down") {
      await dispatchMouse("mousePressed", x, y, button);
    } else if (action === "up") {
      await dispatchMouse("mouseReleased", x, y, button);
    }
    if (cdpLogLeft > 0) {
      cdpLogLeft -= 1;
      setStatus(
        "attached",
        `mouse ${action} host=(${hostX},${hostY}) -> (${Math.round(x)},${Math.round(y)})`
      );
    }
    return {
      ok: true,
      version: BRIDGE_VERSION,
      action,
      hostX,
      hostY,
      mappedX: x,
      mappedY: y,
      scaleX: m.scaleX,
      scaleY: m.scaleY,
      focus: attachMeta.focus,
      touch,
      dom: { ok: true, skipped: true },
    };
  } catch (e) {
    return {
      ok: false,
      error: "CDP_ERROR",
      message: String(e && e.message ? e.message : e),
      version: BRIDGE_VERSION,
      hostX,
      hostY,
      mappedX: x,
      mappedY: y,
    };
  }
}

async function b64JpegToBytes(dataB64) {
  // 禁止逐字节 atob 循环（大图会拖死 MV3 / 超时）。
  const res = await fetch("data:image/jpeg;base64," + dataB64);
  const buf = await res.arrayBuffer();
  return new Uint8Array(buf);
}

async function postShotJpeg(bin) {
  const url =
    `http://127.0.0.1:${bridgePort}/qst/shot?token=${encodeURIComponent(bridgeToken)}`;
  const resp = await fetch(url, {
    method: "POST",
    headers: {
      "Content-Type": "image/jpeg",
      "X-Qst-Token": bridgeToken,
    },
    body: bin,
    cache: "no-store",
  });
  if (!resp.ok) {
    throw new Error(`HTTP shot ${resp.status}`);
  }
}

async function cdpShotB64(debuggee, opts) {
  try {
    await chrome.debugger.sendCommand(debuggee, "Page.enable", {});
  } catch (_) {
    /* optional */
  }
  // 勿 setWebLifecycleState / setDefaultBackgroundColorOverride：可见窗上会闪白。
  const r = await chrome.debugger.sendCommand(debuggee, "Page.captureScreenshot", opts);
  return r && r.data ? String(r.data) : "";
}

function sleepMs(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

/**
 * 在 iframe 内安装镜像（VER=8）。
 * VER=8：像素指纹门控 —— 内容不变不 frames++（杜绝假新帧锁定匹配度）。
 */
async function ensureCanvasMirrorInstalled() {
  if (!attachedDebuggee) return { ok: false, err: "no-debuggee" };
  try {
    const res = await chrome.debugger.sendCommand(attachedDebuggee, "Runtime.evaluate", {
      expression: `(() => {
        const VER = 8;
        if (!window.__qstVisSpoofed) {
          try {
            Object.defineProperty(document, "hidden", { configurable: true, get: () => false });
            Object.defineProperty(document, "visibilityState", { configurable: true, get: () => "visible" });
          } catch (_) {}
          window.__qstVisSpoofed = true;
          try { document.dispatchEvent(new Event("visibilitychange")); } catch (_) {}
        }
        if (window.__qstMirrorInstalled && window.__qstMirrorVer === VER && window.__qstMirrorCopy) {
          return {
            ok: true,
            already: true,
            frames: window.__qstMirrorFrames | 0,
            mean: window.__qstMirrorMean || 0,
            fp: String(window.__qstMirrorFp || ""),
          };
        }
        const pick = () => {
          let best = null;
          let area = 0;
          for (const c of document.querySelectorAll("canvas")) {
            const w = c.width | 0;
            const h = c.height | 0;
            const a = w * h;
            if (w >= 64 && h >= 64 && a > area) {
              best = c;
              area = a;
            }
          }
          return best;
        };
        if (typeof window.__qstMirrorFrames !== "number") window.__qstMirrorFrames = 0;
        if (typeof window.__qstMirrorMean !== "number") window.__qstMirrorMean = 0;
        if (typeof window.__qstMirrorFp !== "string") window.__qstMirrorFp = "";
        window.__qstNeedMirror = false;
        window.__qstMirrorBusy = 0;

        const fingerprint = (pixels, w, h, mean) => {
          let fp = (w * 131 + h) | 0;
          const step = Math.max(32, ((w * h * 4) / 1500) | 0) & ~3;
          for (let i = 0; i < pixels.length; i += step) {
            fp = (fp + pixels[i] * 3 + pixels[i + 1] * 5 + pixels[i + 2] * 7 + (i | 0)) | 0;
          }
          return (fp >>> 0).toString(16) + ":" + w + "x" + h + ":" + Math.round(mean);
        };

        const commitPixels = (pixels, w, h) => {
          if (!pixels || w < 64 || h < 64) return false;
          if (window.__qstMirrorBusy === 2) return false;
          let sum = 0;
          let n = 0;
          const step = Math.max(16, ((w * h * 4) / 2000) | 0) & ~3;
          for (let i = 0; i < pixels.length; i += step) {
            sum += pixels[i] + pixels[i + 1] + pixels[i + 2];
            n += 1;
          }
          const mean = n > 0 ? sum / (n * 3) : 0;
          if (mean < 6) return false;
          const fp = fingerprint(pixels, w, h, mean);
          // 内容未变：不涨 frames（假新帧已证伪）
          if (fp === window.__qstMirrorFp && (window.__qstMirrorFrames | 0) > 0) {
            return false;
          }
          window.__qstMirrorBusy = 1;
          try {
            const row = w * 4;
            if (!window.__qstGoodRGBA || window.__qstGoodW !== w || window.__qstGoodH !== h) {
              window.__qstGoodRGBA = new Uint8ClampedArray(w * h * 4);
              window.__qstGoodW = w;
              window.__qstGoodH = h;
            }
            const dst = window.__qstGoodRGBA;
            for (let y = 0; y < h; y++) {
              const srcOff = (h - 1 - y) * row;
              const dstOff = y * row;
              dst.set(pixels.subarray(srcOff, srcOff + row), dstOff);
            }
            window.__qstMirrorMean = mean;
            window.__qstMirrorFp = fp;
            window.__qstMirrorFrames = (window.__qstMirrorFrames | 0) + 1;
            return true;
          } finally {
            window.__qstMirrorBusy = 0;
          }
        };

        window.__qstSnapshotGl = function (gl) {
          try {
            if (!gl || !gl.canvas) return false;
            if (window.__qstMirrorBusy === 2) return false;
            const w = (gl.drawingBufferWidth || gl.canvas.width) | 0;
            const h = (gl.drawingBufferHeight || gl.canvas.height) | 0;
            if (w < 64 || h < 64) return false;
            if (!gl.__qstPix || gl.__qstPixW !== w || gl.__qstPixH !== h) {
              gl.__qstPix = new Uint8Array(w * h * 4);
              gl.__qstPixW = w;
              gl.__qstPixH = h;
            }
            const prevFb = gl.getParameter(gl.FRAMEBUFFER_BINDING);
            if (prevFb) gl.bindFramebuffer(gl.FRAMEBUFFER, null);
            gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, gl.__qstPix);
            if (prevFb) gl.bindFramebuffer(gl.FRAMEBUFFER, prevFb);
            return commitPixels(gl.__qstPix, w, h);
          } catch (e) {
            window.__qstMirrorErr = String(e && e.message ? e.message : e);
            return false;
          }
        };

        const hookClear = (proto) => {
          if (!proto || proto.__qstClearHookedV8) return;
          const orig = proto.clear;
          if (typeof orig !== "function") return;
          proto.clear = function (mask) {
            if (window.__qstNeedMirror) {
              try { window.__qstSnapshotGl(this); } catch (_) {}
            }
            return orig.call(this, mask);
          };
          proto.__qstClearHookedV8 = true;
        };
        hookClear(window.WebGLRenderingContext && window.WebGLRenderingContext.prototype);
        hookClear(window.WebGL2RenderingContext && window.WebGL2RenderingContext.prototype);

        const copy = () => {
          if (!window.__qstNeedMirror) return;
          if (window.__qstMirrorBusy === 2) return;
          const c = pick();
          if (!c) return;
          try {
            const gl = c.getContext("webgl2") || c.getContext("webgl");
            if (gl) window.__qstSnapshotGl(gl);
          } catch (e) {
            window.__qstMirrorErr = String(e && e.message ? e.message : e);
          }
        };
        window.__qstMirrorCopy = copy;

        const origRAF =
          window.__qstOrigRAF
          || window.requestAnimationFrame.bind(window);
        window.__qstOrigRAF = origRAF;
        window.requestAnimationFrame = function (cb) {
          return origRAF(function (t) {
            let ret;
            try {
              ret = cb(t);
            } finally {
              try {
                if (typeof window.__qstMirrorCopy === "function") window.__qstMirrorCopy();
              } catch (_) {}
            }
            return ret;
          });
        };
        window.__qstMirrorWrapped = true;
        window.__qstMirrorWrapVer = VER;
        window.__qstMirrorInstalled = true;
        window.__qstMirrorVer = VER;
        return {
          ok: true,
          already: false,
          frames: window.__qstMirrorFrames | 0,
          mean: window.__qstMirrorMean || 0,
          fp: String(window.__qstMirrorFp || ""),
        };
      })()`,
      returnByValue: true,
    });
    const v = res && res.result && res.result.value;
    if (v && v.ok) {
      mirrorInstallTried = true;
      return {
        ok: true,
        already: !!v.already,
        frames: Number(v.frames) || 0,
        mean: Number(v.mean) || 0,
        fp: String(v.fp || ""),
      };
    }
    return { ok: false, err: "evaluate-fail" };
  } catch (e) {
    return { ok: false, err: String(e && e.message ? e.message : e) };
  }
}

/**
 * 编码只读 CPU 侧 __qstGoodRGBA（不碰可能被回收的 GPU 2D canvas）。
 */
async function mirrorEncodeOnPage(errs) {
  const res = await chrome.debugger.sendCommand(attachedDebuggee, "Runtime.evaluate", {
    expression: `(() => {
      const frames = window.__qstMirrorFrames | 0;
      const mean = Number(window.__qstMirrorMean) || 0;
      const rgba = window.__qstGoodRGBA;
      const w = window.__qstGoodW | 0;
      const h = window.__qstGoodH | 0;
      if (!rgba || w < 64 || h < 64 || frames < 1) {
        return { ok: false, err: "no-frames", frames: frames, mean: mean };
      }
      if (mean < 6) {
        return { ok: false, err: "black", frames: frames, mean: mean, w: w, h: h };
      }
      window.__qstMirrorBusy = 2;
      try {
        const staging = document.createElement("canvas");
        staging.width = w;
        staging.height = h;
        const sctx = staging.getContext("2d");
        const img = sctx.createImageData(w, h);
        img.data.set(rgba);
        sctx.putImageData(img, 0, 0);
        const pw = Math.min(64, w);
        const ph = Math.min(64, h);
        const probe = sctx.getImageData(0, 0, pw, ph).data;
        let sum = 0;
        let n = 0;
        for (let i = 0; i < probe.length; i += 16) {
          sum += probe[i] + probe[i + 1] + probe[i + 2];
          n += 1;
        }
        const sm = n > 0 ? sum / (n * 3) : 0;
        if (sm < 6) {
          return { ok: false, err: "black-cpu", frames: frames, mean: sm, w: w, h: h };
        }
        const url = staging.toDataURL("image/jpeg", 0.82);
        const idx = url.indexOf(",");
        if (idx < 0 || url.length < 128) {
          return { ok: false, err: "empty-jpeg", frames: frames, mean: mean };
        }
        const data = url.slice(idx + 1);
        window.__qstJpeg = data;
        window.__qstJpegLen = data.length | 0;
        return {
          ok: true,
          len: data.length | 0,
          w: w,
          h: h,
          frames: frames,
          mean: mean,
          fp: String(window.__qstMirrorFp || ""),
          q: 0.82,
        };
      } finally {
        window.__qstMirrorBusy = 0;
      }
    })()`,
    returnByValue: true,
  });
  if (res && res.exceptionDetails) {
    const ed = res.exceptionDetails;
    errs.push("mirror:encode:" + String((ed.exception && ed.exception.description) || ed.text || "ex").slice(0, 120));
    return null;
  }
  return res && res.result ? res.result.value : null;
}

/**
 * 保活：默认轻量（lifecycle + 可见性伪装 + 踢 rAF）。
 * heavy=true 才短暂 screencast（逼屏外 compositor）；且 ≥450ms 节流。
 * 证伪：每次找图/按键都 startScreencast×120ms → 观看宏桌面时 WebGL 卡帧、找图 1–2s+。
 * @param {{heavy?: boolean, forceHeavy?: boolean}} [opts]
 */
async function wakeTargetLifecycle(opts) {
  if (!attachedDebuggee) return;
  const wantHeavy = !!(opts && (opts.heavy || opts.forceHeavy));
  const now = Date.now();
  const heavyOk =
    wantHeavy
    && (opts.forceHeavy || now - lastHeavyWakeAt >= 450);
  try {
    await chrome.debugger.sendCommand(attachedDebuggee, "Emulation.setFocusEmulationEnabled", {
      enabled: true,
    });
  } catch (_) {
    /* optional */
  }
  try {
    await chrome.debugger.sendCommand(attachedDebuggee, "Page.setWebLifecycleState", {
      state: "active",
    });
  } catch (_) {
    /* optional */
  }
  const tabId = Number(attachMeta.pageTabId) || 0;
  if (tabId) {
    try {
      await chrome.debugger.sendCommand({ tabId }, "Emulation.setFocusEmulationEnabled", {
        enabled: true,
      });
    } catch (_) {
      /* shell may not be attached */
    }
    try {
      await chrome.debugger.sendCommand({ tabId }, "Page.setWebLifecycleState", {
        state: "active",
      });
    } catch (_) {
      /* shell may not be attached */
    }
    // 短暂 screencast 逼 compositor 出帧；禁 window.focus（会切宏桌面）。
    if (heavyOk) {
      lastHeavyWakeAt = now;
      try {
        await chrome.debugger.sendCommand({ tabId }, "Page.startScreencast", {
          format: "jpeg",
          quality: 20,
          maxWidth: 160,
          maxHeight: 90,
          everyNthFrame: 1,
        });
        await sleepMs(50);
        await chrome.debugger.sendCommand({ tabId }, "Page.stopScreencast");
      } catch (_) {
        try {
          await chrome.debugger.sendCommand({ tabId }, "Page.stopScreencast");
        } catch (__) {
          /* ignore */
        }
      }
    }
  }
  // 最小化停放：伪装可见性 + 踢 rAF；宿主禁 SoftRestore（切屏）。
  try {
    await chrome.debugger.sendCommand(attachedDebuggee, "Runtime.evaluate", {
      expression: `(() => {
        try {
          if (!window.__qstVisSpoofed) {
            try {
              Object.defineProperty(document, "hidden", { configurable: true, get: () => false });
              Object.defineProperty(document, "visibilityState", { configurable: true, get: () => "visible" });
            } catch (_) {
              /* already defined */
            }
            window.__qstVisSpoofed = true;
            try { document.dispatchEvent(new Event("visibilitychange")); } catch (_) {}
          }
          try {
            if (typeof document.hasFocus === "function") {
              Object.defineProperty(document, "hasFocus", { configurable: true, value: () => true });
            }
          } catch (_) {}
          let n = 0;
          const kick = () => {
            n += 1;
            if (n < 8) {
              (window.__qstOrigRAF || requestAnimationFrame)(kick);
            }
          };
          (window.__qstOrigRAF || requestAnimationFrame)(kick);
          try {
            if (!window.__qstKeepAliveTimer) {
              window.__qstKeepAliveTimer = setInterval(() => {
                try { document.dispatchEvent(new Event("visibilitychange")); } catch (_) {}
              }, 1000);
            }
          } catch (_) {}
          return true;
        } catch (e) {
          return false;
        }
      })()`,
      returnByValue: true,
    });
  } catch (_) {
    /* optional */
  }
}

async function mirrorPullChunks(len, errs) {
  const chunkSize = 240000;
  const parts = [];
  let offset = 0;
  while (offset < len) {
    const res = await chrome.debugger.sendCommand(attachedDebuggee, "Runtime.evaluate", {
      expression: `(() => {
        const s = window.__qstJpeg || "";
        const off = ${offset};
        const n = ${chunkSize};
        return { ok: true, part: s.slice(off, off + n) };
      })()`,
      returnByValue: true,
    });
    if (res && res.exceptionDetails) {
      errs.push("mirror:chunk:" + String(res.exceptionDetails.text || "ex").slice(0, 80));
      return "";
    }
    const v = res && res.result && res.result.value;
    if (!v || !v.ok || typeof v.part !== "string") {
      errs.push("mirror:chunk-fail@" + offset);
      return "";
    }
    parts.push(v.part);
    if (v.part.length === 0) break;
    offset += v.part.length;
    if (v.part.length < chunkSize) break;
  }
  try {
    await chrome.debugger.sendCommand(attachedDebuggee, "Runtime.evaluate", {
      expression: `(() => { window.__qstJpeg = ""; window.__qstJpegLen = 0; return true; })()`,
      returnByValue: true,
    });
  } catch (_) {
    /* ignore */
  }
  return parts.join("");
}

/**
 * 从 rAF 镜像取 JPEG（分块回传）。
 * - 优先等像素指纹相对本轮 wake 发生变化（真出帧，非假 frames++）。
 * - 静态 HUD 久等不变时仍允许出一帧（本轮一次）。
 * - 不改 attachMeta.contentW。
 */
async function captureViaMirror(errs) {
  if (!attachedDebuggee) {
    errs.push("mirror:no-debuggee");
    return { dataB64: "", via: "" };
  }
  const inst = await ensureCanvasMirrorInstalled();
  if (!inst.ok) {
    errs.push("mirror:install:" + (inst.err || "?"));
    return { dataB64: "", via: "" };
  }

  const setNeed = async (on) => {
    try {
      await chrome.debugger.sendCommand(attachedDebuggee, "Runtime.evaluate", {
        expression: `(() => { window.__qstNeedMirror = ${on ? "true" : "false"}; return true; })()`,
        returnByValue: true,
      });
    } catch (_) {
      /* ignore */
    }
  };

  const peekFp = async () => {
    try {
      const res = await chrome.debugger.sendCommand(attachedDebuggee, "Runtime.evaluate", {
        expression: `({
          frames: window.__qstMirrorFrames | 0,
          fp: String(window.__qstMirrorFp || ""),
          mean: Number(window.__qstMirrorMean) || 0,
        })`,
        returnByValue: true,
      });
      return (res && res.result && res.result.value) || {};
    } catch (_) {
      return {};
    }
  };

  await setNeed(true);
  try {
    // 本轮最多一次 heavy screencast；后续轻量保活，避免连环卡帧。
    await wakeTargetLifecycle({ heavy: true });
    // 先等到有基线指纹；禁「startFp 空 → 误判 contentMoved」（假新帧 / 沿用旧图）。
    let startFp = "";
    let startFrames = 0;
    for (let boot = 0; boot < 10 && !startFp; boot++) {
      if (boot > 0) {
        await wakeTargetLifecycle();
        await sleepMs(25);
      }
      const start = await peekFp();
      startFp = String(start.fp || "");
      startFrames = Number(start.frames) || 0;
    }
    if (!startFp) {
      errs.push("mirror:no-baseline-fp");
    }

    for (let attempt = 0; attempt < 18; attempt++) {
      try {
        // 久等无新帧时再 heavy 一次；其余轻量。
        if (attempt === 6) {
          await wakeTargetLifecycle({ heavy: true });
        } else if (attempt === 0 || attempt === 12) {
          await wakeTargetLifecycle();
        }
        await sleepMs(attempt === 0 ? 30 : 28);
        const peek = await peekFp();
        const peekFpStr = String(peek.fp || "");
        const peekFrames = Number(peek.frames) || 0;
        // 必须相对本轮基线指纹变化才算真出帧（空基线不算 moved）。
        const contentMoved = !!(startFp && peekFpStr && peekFpStr !== startFp);
        const frameMoved = peekFrames > startFrames;
        if (!contentMoved && attempt < 8) {
          if (attempt === 0 || attempt === 7) {
            errs.push("mirror:wait-fp:a" + attempt + ":f" + peekFrames);
          }
          continue;
        }
        if (!contentMoved && !frameMoved && attempt < 12) {
          continue;
        }
        const meta = await mirrorEncodeOnPage(errs);
        if (!meta) continue;
        if (!meta.ok) {
          const err = String(meta.err || "fail");
          errs.push(
            "mirror:" + err
            + ":f" + (meta.frames | 0)
            + ":m" + Math.round(Number(meta.mean) || 0)
            + (meta.w ? (":" + meta.w + "x" + meta.h) : "")
          );
          if (err === "no-frames" || err.indexOf("black") === 0) continue;
          break;
        }
        const data = await mirrorPullChunks(Number(meta.len) || 0, errs);
        if (!data || data.length < 64) {
          errs.push("mirror:pull-empty");
          continue;
        }
        lastMirrorFramesSeen = Number(meta.frames) || 0;
        return {
          dataB64: data,
          via: contentMoved ? "cdp:http-mirror" : "cdp:http-mirror:static",
          shotW: Number(meta.w) || 0,
          shotH: Number(meta.h) || 0,
        };
      } catch (e) {
        errs.push("mirror:" + String(e && e.message ? e.message : e));
      }
    }
    return { dataB64: "", via: "" };
  } finally {
    await setNeed(false);
  }
}

/**
 * 壳页 clip 截图（回退）。Page.captureScreenshot 在可见 compositor 上会白闪——已证伪。
 * 仅镜像失败时使用。
 */
async function captureViaPageClip(errs) {
  const tabId = Number(attachMeta.pageTabId) || 0;
  if (!tabId) {
    errs.push("page:无 pageTabId");
    return { dataB64: "", via: "" };
  }
  const pageDbg = { tabId };
  const clip =
    Number(attachMeta.iframeCssW) >= 64 && Number(attachMeta.iframeCssH) >= 64
      ? {
          x: Number(attachMeta.iframeCssX) || 0,
          y: Number(attachMeta.iframeCssY) || 0,
          width: Number(attachMeta.iframeCssW) || 0,
          height: Number(attachMeta.iframeCssH) || 0,
          scale: 1,
        }
      : null;

  let attachedNow = false;
  try {
    const keep =
      pageShotDebuggee
      && typeof pageShotDebuggee.tabId === "number"
      && pageShotDebuggee.tabId === tabId;
    if (!keep) {
      await chrome.debugger.attach(pageDbg, "1.3");
      attachedNow = true;
      pageShotDebuggee = pageDbg;
    }
    const dbg = pageShotDebuggee || pageDbg;

    let dataB64 = await cdpShotB64(dbg, {
      format: "jpeg",
      quality: 55,
      ...(clip ? { clip } : {}),
    });
    if (!dataB64 || dataB64.length < 64) {
      dataB64 = await cdpShotB64(dbg, {
        format: "jpeg",
        quality: 55,
        fromSurface: true,
        ...(clip ? { clip } : {}),
      });
    }
    return {
      dataB64,
      via: clip ? "cdp:http-pageclip" : "cdp:http-page",
    };
  } catch (eDual) {
    errs.push("dual:" + String(eDual && eDual.message ? eDual.message : eDual));
    if (attachedNow) {
      try {
        await chrome.debugger.detach(pageDbg);
      } catch (_) {
        /* ignore */
      }
      pageShotDebuggee = null;
    }
  }
  return { dataB64: "", via: "" };
}

/**
 * 视觉截图 → HTTP POST /qst/shot。
 *
 * mirror-only（双路径已证伪，见 cdp-lessons）：
 * - 始终优先 mirror（readPixels→CPU），可见不闪、后台靠可见性伪装继续出帧。
 * - 热路径禁止 pageclip（不可见冻 Unlock；可见白闪）。
 */
async function captureScreenshot(_opts) {
  const ready = await ensureAttached();
  if (!ready.ok || !attachedDebuggee) {
    return {
      ok: false,
      error: ready.error || "NO_TAB",
      message: ready.message || "尚未 attach",
      version: BRIDGE_VERSION,
      vision: true,
    };
  }
  if (!bridgePort || !bridgeToken) {
    return {
      ok: false,
      error: "SHOT_UNAVAILABLE",
      message: "无桥端口/token",
      version: BRIDGE_VERSION,
      vision: true,
    };
  }
  if (visionBusy) {
    return {
      ok: false,
      error: "SHOT_UNAVAILABLE",
      message: "vision busy",
      version: BRIDGE_VERSION,
      vision: true,
    };
  }
  visionBusy = true;
  const errs = [];
  let dataB64 = "";
  let via = "cdp:http";
  try {
    const iframeLike =
      attachMeta.focus === "iframe-target"
      || attachMeta.via === "iframe"
      || !!attachMeta.iframeTargetId;

    // captureViaMirror 内会 heavy 一次；此处不再预 wake，避免双重 screencast。
    errs.push("path:mirror-only");

    if (iframeLike || attachedDebuggee.targetId) {
      const shot = await captureViaMirror(errs);
      if (shot.dataB64 && shot.dataB64.length >= 64) {
        dataB64 = shot.dataB64;
        via = shot.via || "cdp:http-mirror";
      }
    }
    if ((!dataB64 || dataB64.length < 64) && !iframeLike) {
      try {
        dataB64 = await cdpShotB64(attachedDebuggee, { format: "jpeg", quality: 55 });
        via = "cdp:http";
      } catch (e1) {
        errs.push("cur:" + String(e1 && e1.message ? e1.message : e1));
      }
    }
    if (!dataB64 || dataB64.length < 64) {
      errs.push("skip-pageclip:mirror-only");
    }

    if (!dataB64 || dataB64.length < 64) {
      const mfail = errs.filter((e) => String(e).indexOf("mirror:") === 0 || String(e).indexOf("path:") === 0).slice(0, 8).join("|");
      return {
        ok: false,
        error: "SHOT_UNAVAILABLE",
        message: mfail || (errs.length ? errs.join("|") : "CDP 截图空"),
        mirrorErr: errs.filter((e) => String(e).indexOf("mirror:") === 0).slice(0, 6).join("|"),
        version: BRIDGE_VERSION,
        vision: true,
      };
    }

    let bin;
    try {
      bin = await b64JpegToBytes(dataB64);
    } catch (e) {
      return {
        ok: false,
        error: "SHOT_UNAVAILABLE",
        message: "base64 解码失败:" + String(e && e.message ? e.message : e),
        version: BRIDGE_VERSION,
        vision: true,
      };
    }
    if (!bin || bin.length < 64) {
      return {
        ok: false,
        error: "SHOT_UNAVAILABLE",
        message: "JPEG 过小",
        version: BRIDGE_VERSION,
        vision: true,
      };
    }

    try {
      await postShotJpeg(bin);
    } catch (e) {
      return {
        ok: false,
        error: "SHOT_UNAVAILABLE",
        message: String(e && e.message ? e.message : e),
        version: BRIDGE_VERSION,
        vision: true,
      };
    }

    const canvasSpace =
      via.indexOf("mirror") >= 0
      || via.indexOf("canvas") >= 0
      || via.indexOf("iframe") >= 0
      || via.indexOf("pageclip") >= 0;
    return {
      ok: true,
      via,
      space: canvasSpace ? "canvas" : "page",
      shotHttp: true,
      vision: true,
      version: BRIDGE_VERSION,
      focus: attachMeta.focus || "",
      contentW: attachMeta.contentW || attachMeta.pageCssW || 0,
      contentH: attachMeta.contentH || attachMeta.pageCssH || 0,
      iframeCssX: attachMeta.iframeCssX || 0,
      iframeCssY: attachMeta.iframeCssY || 0,
      iframeCssW: attachMeta.iframeCssW || 0,
      iframeCssH: attachMeta.iframeCssH || 0,
      pageCssW: attachMeta.pageCssW || 0,
      pageCssH: attachMeta.pageCssH || 0,
      visionPath: "mirror-only",
    };
  } finally {
    visionBusy = false;
  }
}

async function sendCdp(method, params) {
  const ready = await ensureAttached();
  if (!ready.ok || !attachedDebuggee) {
    return {
      ok: false,
      error: ready.error || "NO_TAB",
      message: ready.message || "尚未 attach",
      version: BRIDGE_VERSION,
    };
  }
  let useParams = params || {};
  let hostX = null;
  let hostY = null;
  if (method === "Input.dispatchMouseEvent") {
    useParams = adjustMouseParams(useParams);
    hostX = useParams._hostX;
    hostY = useParams._hostY;
    delete useParams._hostX;
    delete useParams._hostY;
    delete useParams._mappedX;
    delete useParams._mappedY;
  }
  try {
    if (String(method).startsWith("Input.")) {
      // 键鼠只用轻量保活；screencast 留给找图，否则连按会卡帧。
      await wakeTargetLifecycle();
    }
    const result = await chrome.debugger.sendCommand(attachedDebuggee, method, useParams);
    if (cdpLogLeft > 0 && String(method).startsWith("Input.")) {
      cdpLogLeft -= 1;
      const xy =
        method === "Input.dispatchMouseEvent" && useParams
          ? ` host=(${Math.round(hostX)},${Math.round(hostY)}) ->(${Math.round(useParams.x)},${Math.round(useParams.y)})`
          : "";
      setStatus(
        "attached",
        `cdp ok ${method} ${useParams.type || ""}${xy} (${cdpLogLeft} logs left)`
      );
    }
    return {
      ok: true,
      result: result || {},
      version: BRIDGE_VERSION,
      hostX,
      hostY,
      mappedX: useParams.x,
      mappedY: useParams.y,
    };
  } catch (e) {
    return {
      ok: false,
      error: "CDP_ERROR",
      message: String(e && e.message ? e.message : e),
      version: BRIDGE_VERSION,
    };
  }
}

function qstBuildPageSnapshot(opts) {
  const light = !!(opts && opts.light);
  const maxNodes = Math.min(80, Math.max(8, (opts && opts.maxNodes) || 64));
  const query = String((opts && opts.query) || "")
    .trim()
    .toLowerCase();
  const vw = Math.max(1, window.innerWidth || 1);
  const vh = Math.max(1, window.innerHeight || 1);
  const viewport = Math.max(1, vw * vh);
  const SEL =
    'a[href],button,input:not([type="hidden"]),select,textarea,summary,option,[role="button"],[role="link"],[role="textbox"],[role="checkbox"],[role="radio"],[role="tab"],[role="menuitem"],[role="option"],[role="combobox"],[role="switch"],[role="searchbox"],[contenteditable="true"],[tabindex]:not([tabindex="-1"])';
  const isRenderable = (el) => {
    try {
      const r = el.getBoundingClientRect();
      if (r.width < 4 || r.height < 4) return false;
      const st = window.getComputedStyle(el);
      if (st.visibility === "hidden" || st.display === "none" || Number(st.opacity) === 0)
        return false;
      if (el.disabled || el.getAttribute("aria-hidden") === "true") return false;
      return true;
    } catch (_) {
      return false;
    }
  };
  const isVisible = (el) => {
    try {
      const r = el.getBoundingClientRect();
      if (!isRenderable(el)) return false;
      if (r.bottom < 0 || r.right < 0 || r.top > vh || r.left > vw) return false;
      return true;
    } catch (_) {
      return false;
    }
  };
  const onSpaceHost = String(location.hostname || "").toLowerCase().indexOf("space.bilibili.com") >= 0;
  const hrefNow = String(location.href || "").toLowerCase();
  const onSearch = hrefNow.indexOf("search.") >= 0
    || hrefNow.indexOf("/search") >= 0
    || (hrefNow.indexOf("keyword=") >= 0
      && (hrefNow.indexOf("search") >= 0 || hrefNow.indexOf("/all?") >= 0));
  const pathNow = String(location.pathname || "").toLowerCase();
  const onWatch = /(?:^|\/)video\/[^/]+/.test(pathNow)
    || pathNow.indexOf("/bangumi/") >= 0
    || pathNow === "/watch"
    || pathNow.indexOf("/watch/") === 0;
  const borderX = Math.max(0, ((window.outerWidth || 0) - vw) / 2);
  const chromeY = Math.max(0, (window.outerHeight || 0) - vh - borderX);
  const originX = (window.screenX || 0) + borderX;
  const originY = (window.screenY || 0) + chromeY;
  const isSpaceVideoLink = (el) => {
    if (!onSpaceHost) return false;
    try {
      const href = String((el.href && String(el.href)) || el.getAttribute("href") || "");
      return /\/video\/|\/bangumi\//.test(href);
    } catch (_) {
      return false;
    }
  };
  const accName = (el) => {
    try {
      const al = el.getAttribute("aria-label");
      if (al) return String(al).trim();
      const labelled = el.getAttribute("aria-labelledby");
      if (labelled) {
        const t = labelled
          .split(/\s+/)
          .map((id) => {
            const n = document.getElementById(id);
            return n ? String(n.innerText || n.textContent || "").trim() : "";
          })
          .filter(Boolean)
          .join(" ");
        if (t) return t;
      }
      if (el.labels && el.labels[0]) {
        const t = String(el.labels[0].innerText || "").trim();
        if (t) return t;
      }
      const ph = el.getAttribute("placeholder");
      if (ph) return String(ph).trim();
      const alt = el.getAttribute("alt");
      if (alt) return String(alt).trim();
      const title = el.getAttribute("title");
      if (title) return String(title).trim();
      const tx = String(el.innerText || el.textContent || "").replace(/\s+/g, " ").trim();
      return tx;
    } catch (_) {
      return "";
    }
  };
  const roleOf = (el) => {
    const role = (el.getAttribute("role") || "").trim().toLowerCase();
    if (role) return role;
    const tag = String(el.tagName || "").toLowerCase();
    if (tag === "a") return "link";
    if (tag === "button") return "button";
    if (tag === "select") return "combobox";
    if (tag === "textarea") return "textbox";
    if (tag === "summary") return "button";
    if (tag === "option") return "option";
    if (tag === "input") {
      const t = String(el.type || "text").toLowerCase();
      if (t === "checkbox") return "checkbox";
      if (t === "radio") return "radio";
      if (t === "submit" || t === "button" || t === "reset") return "button";
      if (t === "search") return "searchbox";
      return "textbox";
    }
    if (el.isContentEditable) return "textbox";
    return "generic";
  };
  const sectionPath = (el) => {
    try {
      let n = el;
      for (let i = 0; i < 10 && n; i++) {
        n = n.parentElement;
        if (!n) break;
        const role = String(n.getAttribute && n.getAttribute("role") || "").toLowerCase();
        const tag = String(n.tagName || "");
        if (role === "dialog" || role === "alertdialog" || tag === "DIALOG") {
          return (accName(n) || "对话框").slice(0, 24);
        }
        if (/^H[1-6]$/.test(tag)) {
          const t = String(n.innerText || "").replace(/\s+/g, " ").trim();
          if (t) return t.slice(0, 24);
        }
      }
    } catch (_) {}
    return "";
  };
  const inputValue = (el) => {
    try {
      const tag = String(el.tagName || "").toLowerCase();
      if (tag === "select") {
        const opt = el.selectedOptions && el.selectedOptions[0];
        return String((opt && (opt.text || opt.value)) || el.value || "").trim();
      }
      if (tag === "input" || tag === "textarea") return String(el.value || "").trim();
      if (el.isContentEditable) return String(el.innerText || "").replace(/\s+/g, " ").trim();
    } catch (_) {}
    return "";
  };
  const looksLikeIconControl = (el) => {
    try {
      const r = el.getBoundingClientRect();
      if (r.width < 16 || r.height < 12 || r.width > 220 || r.height > 96) return false;
      const st = window.getComputedStyle(el);
      if ((st.cursor || "") !== "pointer" && String(el.getAttribute("role") || "") !== "button")
        return false;
      if (st.visibility === "hidden" || st.display === "none" || Number(st.opacity) === 0)
        return false;
      return !!el.querySelector("svg, i, img");
    } catch (_) {
      return false;
    }
  };
  const collectInteractive = (root, into, seen) => {
    try {
      root.querySelectorAll(SEL).forEach((el) => {
        if (!seen.has(el)) {
          seen.add(el);
          into.push(el);
        }
      });
    } catch (_) {}
    let walked = 0;
    try {
      const all = root.querySelectorAll("*");
      for (let i = 0; i < all.length && walked < 2500; i++) {
        walked++;
        const el = all[i];
        if (el.shadowRoot) collectInteractive(el.shadowRoot, into, seen);
        if (String(el.tagName || "") === "IFRAME") {
          try {
            const doc = el.contentDocument;
            if (doc) collectInteractive(doc, into, seen);
          } catch (_) {}
        }
        if (!seen.has(el) && looksLikeIconControl(el)) {
          seen.add(el);
          into.push(el);
        }
      }
    } catch (_) {}
  };
  let mediaArea = 0;
  try {
    document.querySelectorAll("canvas,video").forEach((el) => {
      const r = el.getBoundingClientRect();
      if (r.width >= 64 && r.height >= 64) mediaArea += r.width * r.height;
    });
  } catch (_) {}
  const canvasRatio = mediaArea / viewport;
  const raw = [];
  const seen = new Set();
  collectInteractive(document, raw, seen);
  try {
    document.querySelectorAll("[aria-label],[title]").forEach((el) => {
      const al = String(el.getAttribute("aria-label") || el.getAttribute("title") || "").trim();
      if (!al || al.length > 40) return;
      const r = el.getBoundingClientRect();
      if (r.width < 4 || r.height < 4) return;
      if (r.width > 320 && r.height > 80) return;
      if (!seen.has(el)) {
        seen.add(el);
        raw.push(el);
      }
    });
  } catch (_) {}
  const candidates = raw.filter((el) => isVisible(el) || (isSpaceVideoLink(el) && isRenderable(el)));
  let pageKind = "dom";
  if (canvasRatio >= 0.5 && candidates.length <= 6) pageKind = "canvas";
  else if (canvasRatio >= 0.22) pageKind = "mixed";

  const overlayRe = /进入|开始|登录|同意|进游戏|开始游戏|进入游戏/;
  let take = candidates;
  if (pageKind === "canvas") {
    take = candidates.filter((el) => overlayRe.test(accName(el)));
    if (take.length === 0) {
      window.__qstA11y = { gen: Date.now(), pageKind: "canvas", nodes: new Map() };
      return {
        pageKind: "canvas",
        canvasRatio: Math.round(canvasRatio * 1000) / 1000,
        interactive: candidates.length,
        skippedHtml: true,
        url: String(location.href || ""),
        title: String(document.title || ""),
        vw,
        vh,
        nodes: [],
      };
    }
    pageKind = "mixed";
  }
  if (light && pageKind === "canvas") {
    window.__qstA11y = { gen: Date.now(), pageKind: "canvas", nodes: new Map() };
    return {
      pageKind: "canvas",
      canvasRatio: Math.round(canvasRatio * 1000) / 1000,
      interactive: candidates.length,
      skippedHtml: true,
      url: String(location.href || ""),
      title: String(document.title || ""),
      vw,
      vh,
      nodes: [],
    };
  }

  const headerH = Math.min(140, vh * 0.18);
  const viewportIntersect = (r) =>
    r.bottom > 0 && r.right > 0 && r.top < vh && r.left < vw;
  const isChromeEl = (el, r, role) => {
    if (role === "tab" || role === "searchbox") return true;
    if (r.y >= headerH || r.height >= 80) return false;
    try {
      let n = el;
      for (let i = 0; i < 8 && n; i++) {
        const st = window.getComputedStyle(n);
        if (st.position === "fixed" || st.position === "sticky") return true;
        n = n.parentElement;
      }
    } catch (_) {}
    return r.y < 64;
  };

  const scored = take.map((el) => {
    const name = accName(el);
    const path = sectionPath(el);
    const r = el.getBoundingClientRect();
    const inView = viewportIntersect(r);
    let href = "";
    try {
      href = String((el.href && String(el.href)) || el.getAttribute("href") || "");
    } catch (_) {}
    const hay = (name + " " + path + " " + href).toLowerCase();
    const hrefLow = href.toLowerCase();
    const role = roleOf(el);
    const chrome = isChromeEl(el, r, role);
    const isUserSpace = /space\.bilibili\.com\/\d+/.test(hrefLow)
      || /\/space\/\d+/.test(hrefLow);
    const isVideo = /\/video\/|\/bangumi\/|\/watch\?/.test(hrefLow);
    let score = 0;
    if (inView) score += 100;
    if (overlayRe.test(name)) score += 40;
    if (role === "tab") score -= 520;
    // 搜人/搜片加分只在搜索结果页；放到任意页会把作者名/推荐链抬成 e1。
    if (onSearch) {
      if (isUserSpace) score += 480;
      if (isVideo) score += 90;
    }
    if (r.width > Math.min(vw * 0.45, 420) && r.height > 80) score -= 180;
    if (query) {
      const nameHit = name.toLowerCase().indexOf(query) >= 0;
      const hayHit = hay.indexOf(query) >= 0;
      if (nameHit) {
        score += 1000;
        if (role === "link" || role === "button") score += 120;
        if (r.width > 200) score -= 350;
        if (role === "searchbox" || role === "textbox") score -= 450;
      } else if (hayHit) {
        score += 120;
      }
    } else if (role === "searchbox" || role === "textbox" || role === "combobox") {
      score += 35;
    }
    score -= Math.round(r.y / 20);
    return { el, name, path, r, href, score, inView, chrome, role, isUserSpace, isVideo };
  });
  const isMainItem = (s) => {
    if (!s.inView || s.chrome) return false;
    if (s.href && s.r.height >= 48) return true;
    return String(s.name || "").trim().length >= 4;
  };
  let contentInView = 0;
  scored.forEach((s) => {
    if (isMainItem(s)) contentInView++;
  });
  const byRead = (a, b) => a.r.y - b.r.y || a.r.x - b.r.x || b.score - a.score;
  const isHeroCard = (s) =>
    s.r.width > Math.min(vw * 0.45, 420) && s.r.height > 80;
  let queryHits = query ? 0 : -1;
  if (query) {
    scored.sort((a, b) => b.score - a.score || a.r.y - b.r.y || a.r.x - b.r.x);
    const nameHits = scored.filter(
      (s) => String(s.name || "").toLowerCase().indexOf(query) >= 0
    );
    queryHits = nameHits.length;
    if (nameHits.length) {
      const rest = scored.filter((s) => nameHits.indexOf(s) < 0);
      take = nameHits.concat(rest);
    } else {
      // 关键字不在树上：空树，让宿主走识图兜底（勿把「网页全屏」当命中）。
      take = [];
    }
  } else {
    const content = scored.filter((s) => s.inView && !s.chrome);
    const chromeNodes = scored.filter((s) => s.inView && s.chrome);
    const off = scored.filter((s) => !s.inView);
    const compactMedia = content.filter((s) => s.isVideo && !isHeroCard(s));
    if (onSearch) {
      content.sort((a, b) => {
        const aBoost = (a.isUserSpace ? 2 : 0) + (a.isVideo ? 1 : 0);
        const bBoost = (b.isUserSpace ? 2 : 0) + (b.isVideo ? 1 : 0);
        if (aBoost !== bBoost) return bBoost - aBoost;
        return byRead(a, b);
      });
      chromeNodes.sort(byRead);
      off.sort(byRead);
      take = content.concat(chromeNodes).concat(off);
    } else if (onWatch) {
      const actions = content.filter((s) => !s.isVideo && !isHeroCard(s));
      const related = content.filter((s) => s.isVideo && !isHeroCard(s));
      const heroes = content.filter((s) => isHeroCard(s));
      actions.sort(byRead);
      related.sort(byRead);
      heroes.sort(byRead);
      chromeNodes.sort(byRead);
      off.sort(byRead);
      take = actions.concat(related).concat(heroes).concat(chromeNodes).concat(off);
    } else if (compactMedia.length >= 2) {
      compactMedia.sort(byRead);
      const other = content.filter((s) => compactMedia.indexOf(s) < 0 && !isHeroCard(s));
      const heroes = content.filter((s) => isHeroCard(s));
      other.sort(byRead);
      heroes.sort(byRead);
      chromeNodes.sort(byRead);
      off.sort(byRead);
      take = compactMedia.concat(other).concat(heroes).concat(chromeNodes).concat(off);
    } else {
      content.sort((a, b) => {
        const ao = overlayRe.test(a.name) ? 1 : 0;
        const bo = overlayRe.test(b.name) ? 1 : 0;
        if (ao !== bo) return bo - ao;
        const ah = isHeroCard(a) ? 1 : 0;
        const bh = isHeroCard(b) ? 1 : 0;
        if (ah !== bh) return ah - bh;
        return byRead(a, b);
      });
      chromeNodes.sort(byRead);
      off.sort(byRead);
      take = content.concat(chromeNodes).concat(off);
    }
  }
  take = take.slice(0, maxNodes);
  const store = { gen: Date.now(), pageKind, nodes: new Map() };
  const nodes = [];
  take.forEach((item, i) => {
    const el = item.el;
    const ref = "e" + (i + 1);
    const r = item.r;
    store.nodes.set(ref, el);
    const role = roleOf(el);
    const node = {
      ref,
      role,
      name: String(item.name || "").slice(0, 80),
      path: String(item.path || "").slice(0, 24),
      x: Math.round(r.x),
      y: Math.round(r.y),
      w: Math.round(r.width),
      h: Math.round(r.height),
      inView: !!item.inView,
    };
    if (item.href) {
      let href = String(item.href);
      if (href.length > 80) href = href.slice(0, 77) + "...";
      if (!/^javascript:/i.test(href) && !/^mailto:/i.test(href)) node.href = href;
    }
    node.sx = Math.round(originX + r.x + r.width / 2);
    node.sy = Math.round(originY + r.y + r.height / 2);
    const val = inputValue(el);
    if (val && (role === "textbox" || role === "searchbox" || role === "combobox"))
      node.value = val.slice(0, 32);
    if (role === "checkbox" || role === "radio" || role === "switch") {
      try {
        node.checked = !!(el.checked || el.getAttribute("aria-checked") === "true");
      } catch (_) {}
    }
    try {
      const ap = String(el.getAttribute("aria-pressed") || "").toLowerCase();
      const ac = String(el.getAttribute("aria-checked") || "").toLowerCase();
      const asel = String(el.getAttribute("aria-selected") || "").toLowerCase();
      if (ap === "true" || ac === "true" || asel === "true") node.checked = true;
    } catch (_) {}
    nodes.push(node);
  });
  const scrollY = Math.round(window.scrollY || document.documentElement.scrollTop || 0);
  const pageHeight = Math.max(
    document.documentElement.scrollHeight || 0,
    document.body ? document.body.scrollHeight : 0,
    vh
  );
  window.__qstA11y = store;
  return {
    pageKind,
    canvasRatio: Math.round(canvasRatio * 1000) / 1000,
    interactive: candidates.length,
    skippedHtml: false,
    url: String(location.href || ""),
    title: String(document.title || ""),
    vw,
    vh,
    scrollY,
    pageHeight,
    contentInView,
    query: query || "",
    queryHits,
    nodes,
  };
}

function qstClickRef(ref, doubleClick, doClick) {
  const store = window.__qstA11y;
  if (!store || !store.nodes) return { ok: false, stale: true, reason: "no-store" };
  if (store.pageKind === "canvas") return { ok: false, reason: "canvas" };
  const el = store.nodes.get(ref);
  if (!el || !el.isConnected) return { ok: false, stale: true, reason: "stale" };
  try {
    el.scrollIntoView({ block: "nearest", inline: "nearest" });
  } catch (_) {}
  const r = el.getBoundingClientRect();
  const x = Math.round(r.x + r.width / 2);
  const y = Math.round(r.y + r.height / 2);
  const vw = Math.max(1, window.innerWidth || 1);
  const vh = Math.max(1, window.innerHeight || 1);
  const borderX = Math.max(0, ((window.outerWidth || 0) - vw) / 2);
  const chromeY = Math.max(0, (window.outerHeight || 0) - vh - borderX);
  const screenX = Math.round((window.screenX || 0) + borderX + x);
  const screenY = Math.round((window.screenY || 0) + chromeY + y);
  let name = "";
  try {
    name = String(el.getAttribute("aria-label") || el.getAttribute("title") || el.innerText || "").replace(/\s+/g, " ").trim().slice(0, 80);
  } catch (_) {}
  try {
    el.focus({ preventScroll: true });
  } catch (_) {}
  let href = "";
  try {
    href = String((el.href && String(el.href)) || el.getAttribute("href") || "");
  } catch (_) {}
  if (doClick === false) {
    return { ok: true, x, y, screenX, screenY, name, tag: String(el.tagName || ""), href };
  }
  try {
    const opts = {
      bubbles: true,
      cancelable: true,
      composed: true,
      view: window,
      clientX: x,
      clientY: y,
      button: 0,
    };
    el.dispatchEvent(new PointerEvent("pointerdown", Object.assign({ pointerId: 1 }, opts)));
    el.dispatchEvent(new MouseEvent("mousedown", opts));
    el.dispatchEvent(new PointerEvent("pointerup", Object.assign({ pointerId: 1 }, opts)));
    el.dispatchEvent(new MouseEvent("mouseup", opts));
    el.click();
    if (doubleClick) {
      el.dispatchEvent(new MouseEvent("dblclick", opts));
      el.click();
    }
  } catch (e) {
    return { ok: false, reason: String(e && e.message ? e.message : e) };
  }
  return { ok: true, x, y, screenX, screenY, name };
}

function qstFillRef(ref, text, clearFirst) {
  const store = window.__qstA11y;
  if (!store || !store.nodes) return { ok: false, stale: true, reason: "no-store" };
  if (store.pageKind === "canvas") return { ok: false, reason: "canvas" };
  const el = store.nodes.get(ref);
  if (!el || !el.isConnected) return { ok: false, stale: true, reason: "stale" };
  const value = String(text || "");
  try {
    el.scrollIntoView({ block: "nearest", inline: "nearest" });
  } catch (_) {}
  try {
    el.focus({ preventScroll: true });
  } catch (_) {}
  const tag = String(el.tagName || "").toLowerCase();
  try {
    if (tag === "select") {
      const want = value.trim().toLowerCase();
      let matched = false;
      const opts = el.options || [];
      for (let i = 0; i < opts.length; i++) {
        const o = opts[i];
        const label = String(o.text || o.value || "").trim();
        if (label.toLowerCase() === want || String(o.value || "").toLowerCase() === want) {
          el.selectedIndex = i;
          matched = true;
          break;
        }
      }
      if (!matched) {
        for (let i = 0; i < opts.length; i++) {
          const label = String(opts[i].text || "").toLowerCase();
          if (want && label.indexOf(want) >= 0) {
            el.selectedIndex = i;
            matched = true;
            break;
          }
        }
      }
      el.dispatchEvent(new Event("input", { bubbles: true }));
      el.dispatchEvent(new Event("change", { bubbles: true }));
      return { ok: true, kind: "select", matched };
    }
    if (el.isContentEditable || el.getAttribute("contenteditable") === "true") {
      if (clearFirst) el.textContent = "";
      try {
        document.execCommand("insertText", false, value);
      } catch (_) {}
      if (clearFirst && String(el.innerText || "").indexOf(value) < 0) el.textContent = value;
      el.dispatchEvent(new InputEvent("input", { bubbles: true, data: value, inputType: "insertText" }));
      return { ok: true, kind: "contenteditable" };
    }
    const proto = tag === "textarea" ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
    const desc = Object.getOwnPropertyDescriptor(proto, "value");
    const apply = (v) => {
      if (desc && desc.set) desc.set.call(el, v);
      else el.value = v;
    };
    if (clearFirst) {
      try {
        el.select();
      } catch (_) {}
      apply("");
    }
    apply(clearFirst ? value : String(el.value || "") + value);
    el.dispatchEvent(
      new InputEvent("input", { bubbles: true, data: value, inputType: "insertText" })
    );
    el.dispatchEvent(new Event("change", { bubbles: true }));
    return { ok: true, kind: "input" };
  } catch (e) {
    return { ok: false, reason: String(e && e.message ? e.message : e) };
  }
}

async function classifyAttachedFrame() {
  if (!attachedDebuggee) return null;
  try {
    const res = await chrome.debugger.sendCommand(attachedDebuggee, "Runtime.evaluate", {
      expression: `(() => {
        const vw = Math.max(1, (innerWidth || 1) * (innerHeight || 1));
        let area = 0;
        try {
          document.querySelectorAll("canvas,video").forEach((el) => {
            const r = el.getBoundingClientRect();
            if (r.width >= 64 && r.height >= 64) area += r.width * r.height;
          });
        } catch (e) {}
        return { canvasRatio: area / vw, href: String(location.href || "") };
      })()`,
      returnByValue: true,
      awaitPromise: false,
    });
    return (res && res.result && res.result.value) || null;
  } catch (e) {
    return { err: String(e && e.message ? e.message : e) };
  }
}

function mergePageKind(pageKind, pageInteractive, frameRatio) {
  if (pageKind === "mixed") return "mixed";
  if (frameRatio >= 0.5 && pageKind !== "dom") {
    if ((pageInteractive || 0) <= 6) return "canvas";
    return "mixed";
  }
  if (frameRatio >= 0.5 && (pageInteractive || 0) <= 6) return "canvas";
  if (frameRatio >= 0.22 && pageKind === "dom") return "mixed";
  return pageKind || "dom";
}

async function injectPageSnapshot(tabId, opts) {
  const o = opts && typeof opts === "object" ? opts : {};
  const inj = await chrome.scripting.executeScript({
    target: { tabId, frameIds: [0] },
    func: qstBuildPageSnapshot,
    args: [{
      light: !!o.light,
      maxNodes: 64,
      query: String(o.query || ""),
    }],
  });
  return inj && inj[0] && inj[0].result;
}

function snapshotHrefVideoCount(snap) {
  const nodes = (snap && snap.nodes) || [];
  let n = 0;
  for (let i = 0; i < nodes.length; i++) {
    const h = String((nodes[i] && nodes[i].href) || "").toLowerCase();
    if (h.indexOf("/video/") >= 0 || h.indexOf("/watch") >= 0 || h.indexOf("/bangumi/") >= 0)
      n++;
  }
  return n;
}

function snapshotLooksListing(url) {
  const u = String(url || "").toLowerCase();
  return u.indexOf("space.") >= 0 || u.indexOf("/upload") >= 0 || u.indexOf("search.") >= 0;
}

async function waitHydratedSnapshot(tabId, opts) {
  let last = null;
  const t0 = Date.now();
  while (Date.now() - t0 < 2400) {
    try {
      last = await injectPageSnapshot(tabId, opts);
    } catch (_) {
      /* keep last */
    }
    const url = String((last && last.url) || "");
    const videos = snapshotHrefVideoCount(last);
    const civ = Number(last && last.contentInView) || 0;
    const n = (last && last.nodes && last.nodes.length) || 0;
    if (snapshotLooksListing(url) && videos >= 2) return last;
    if (!snapshotLooksListing(url) && civ >= 1 && n >= 6) return last;
    await sleepMs(200);
  }
  return last;
}

async function handleObservePage(msg) {
  const force = !!msg.force;
  const light = !!msg.light && !force;
  let attachedNow = false;
  const hintIn = typeof msg.titleHint === "string" ? msg.titleHint.trim() : "";
  const urlHintIn = typeof msg.urlHint === "string" ? msg.urlHint.trim() : "";
  // ★跟随前台标签：宿主传来的 titleHint 是「当前前台标签的标题」（已剥掉窗口装饰）。
  // 若它和已附着的标签对不上，说明用户/脚本换了标签页——必须重新 attach，
  // 否则 observePage 会一直回旧标签的 DOM 树：前台明明是「历史记录」，
  // 树却是上一个页面，模型据此决策必然走错（实测连续 10 轮对着错页面操作）。
  const curTitle = String((attachMeta && attachMeta.title) || "").trim();
  const curUrl = String((attachMeta && attachMeta.url) || "").trim();
  const norm = (s) => String(s || "").toLowerCase().replace(/\s+/g, "");
  const titleMismatch = !!hintIn && !!curTitle
    && !norm(curTitle).includes(norm(hintIn))
    && !norm(hintIn).includes(norm(curTitle));
  const urlMismatch = !!urlHintIn && !!curUrl
    && norm(curUrl) !== norm(urlHintIn)
    && !norm(curUrl).startsWith(norm(urlHintIn))
    && !norm(urlHintIn).startsWith(norm(curUrl));
  const sameHintAsLastFollow = hintIn && attachMeta && attachMeta.followHint === hintIn;
  const needFollow = !msg.preferStay && (titleMismatch || urlMismatch) && !sameHintAsLastFollow;
  if (needFollow) {
    setStatus("attached", `跟随前台标签：${hintIn || urlHintIn || "(无提示)"}`);
  }
  if (!attachedDebuggee || (!msg.preferStay && attachMeta.via === "iframe") || needFollow) {
    const hint = hintIn;
    const tabId = Number(msg.tabId);
    const urlHint = urlHintIn;
    const att = await attachTab(hint, {
      preferPage: true,
      urlHint,
      tabId: Number.isFinite(tabId) && tabId > 0 ? tabId : undefined,
    });
    if (!att || !att.ok) {
      // 跟随失败不致命：保留原附着，让宿主拿到「可能是旧树」的结果而不是直接报错
      if (!needFollow) {
        return {
          ok: false,
          error: (att && att.error) || "NO_TAB",
          message: (att && att.message) || "observePage 未能 attach 标签（请确认已加载配套扩展且打开了网页）",
          version: BRIDGE_VERSION,
        };
      }
    } else {
      attachedNow = true;
      if (needFollow && attachMeta) attachMeta.followHint = hintIn;
    }
  }
  const tabId = Number(attachMeta.pageTabId) || (attachedDebuggee && attachedDebuggee.tabId) || 0;
  let frame = null;
  if (attachMeta.via === "iframe" || attachMeta.focus === "iframe-target") {
    frame = await classifyAttachedFrame();
  }
  let pageSnap = null;
  if (tabId > 0) {
    try {
      const snapOpts = { light, query: String(msg.query || "") };
      pageSnap = msg.waitHydrate
        ? await waitHydratedSnapshot(tabId, snapOpts)
        : await injectPageSnapshot(tabId, snapOpts);
    } catch (e) {
      return {
        ok: false,
        error: "SNAPSHOT_FAIL",
        message: String(e && e.message ? e.message : e),
        version: BRIDGE_VERSION,
      };
    }
  }
  if (!pageSnap || typeof pageSnap !== "object") {
    return {
      ok: false,
      error: "SNAPSHOT_FAIL",
      message: "页面快照为空（受限页或尚未加载）",
      version: BRIDGE_VERSION,
    };
  }
  const frameRatio = frame && Number(frame.canvasRatio) ? Number(frame.canvasRatio) : 0;
  const pageKind = mergePageKind(
    String(pageSnap.pageKind || "dom"),
    Number(pageSnap.interactive) || 0,
    frameRatio
  );
  const skippedHtml = pageKind === "canvas" || !!pageSnap.skippedHtml;
  const nodes = skippedHtml ? [] : pageSnap.nodes || [];
  return {
    ok: true,
    pageKind,
    canvasRatio: Number(pageSnap.canvasRatio) || 0,
    interactive: Number(pageSnap.interactive) || 0,
    skippedHtml,
    url: String(pageSnap.url || ""),
    title: String(pageSnap.title || ""),
    vw: Number(pageSnap.vw) || 0,
    vh: Number(pageSnap.vh) || 0,
    scrollY: Number(pageSnap.scrollY) || 0,
    pageHeight: Number(pageSnap.pageHeight) || 0,
    contentInView: Number.isFinite(Number(pageSnap.contentInView))
      ? Number(pageSnap.contentInView)
      : -1,
    query: String(pageSnap.query || ""),
    queryHits: Number.isFinite(Number(pageSnap.queryHits))
      ? Number(pageSnap.queryHits)
      : -1,
    nodes,
    attachedNow,
    frameCanvasRatio: frameRatio,
    version: BRIDGE_VERSION,
  };
}

function debuggerIsPage() {
  if (!attachedDebuggee) return false;
  if (attachMeta.via === "iframe" || attachMeta.focus === "iframe-target") return false;
  return true;
}

async function trustedPageClick(x, y, doubleClick) {
  if (!debuggerIsPage()) return false;
  const nx = Number(x);
  const ny = Number(y);
  if (!Number.isFinite(nx) || !Number.isFinite(ny)) return false;
  try {
    await dispatchMouse("mouseMoved", nx, ny, "none");
    const n = doubleClick ? 2 : 1;
    for (let c = 1; c <= n; c++) {
      await chrome.debugger.sendCommand(attachedDebuggee, "Input.dispatchMouseEvent", {
        type: "mousePressed",
        x: nx,
        y: ny,
        button: "left",
        buttons: 1,
        clickCount: c,
        pointerType: "mouse",
      });
      await chrome.debugger.sendCommand(attachedDebuggee, "Input.dispatchMouseEvent", {
        type: "mouseReleased",
        x: nx,
        y: ny,
        button: "left",
        buttons: 0,
        clickCount: c,
        pointerType: "mouse",
      });
    }
    return true;
  } catch (_) {
    return false;
  }
}

async function waitTabUrlChange(tabId, prevUrl, timeoutMs) {
  const prev = String(prevUrl || "").split("#")[0];
  const t0 = Date.now();
  while (Date.now() - t0 < (timeoutMs || 2000)) {
    try {
      const t = await chrome.tabs.get(tabId);
      const u = String((t && t.url) || "").split("#")[0];
      if (u && prev && u !== prev) return u;
    } catch (_) {
      /* ignore */
    }
    await sleepMs(150);
  }
  return "";
}

async function waitTabComplete(tabId, timeoutMs) {
  const t0 = Date.now();
  while (Date.now() - t0 < (timeoutMs || 8000)) {
    try {
      const t = await chrome.tabs.get(tabId);
      if (t && t.status === "complete") {
        await sleepMs(280);
        return true;
      }
    } catch (_) {
      /* ignore */
    }
    await sleepMs(120);
  }
  return false;
}

async function handleNavigatePage(msg) {
  const url = String(msg.url || "").trim();
  if (!/^https?:\/\//i.test(url)) {
    return {
      ok: false,
      error: "BAD_URL",
      message: "url 须为 http(s)",
      version: BRIDGE_VERSION,
    };
  }
  const query = String(msg.query || "");
  const hint = typeof msg.titleHint === "string" ? msg.titleHint : "";
  const urlHint = typeof msg.urlHint === "string" ? msg.urlHint : url;
  const stayTab = Number(msg.tabId);
  let tabId = Number.isFinite(stayTab) && stayTab > 0 ? stayTab : 0;
  if (!tabId) {
    tabId = Number(attachMeta.pageTabId) || (attachedDebuggee && attachedDebuggee.tabId) || 0;
  }
  if (tabId) {
    try {
      await chrome.tabs.get(tabId);
    } catch (_) {
      tabId = 0;
    }
  }
  if (!tabId) {
    const att = await attachTab(hint, { preferPage: true, urlHint });
    if (att && att.ok) {
      tabId = Number(attachMeta.pageTabId) || (attachedDebuggee && attachedDebuggee.tabId) || 0;
    }
  }
  if (!tabId) {
    try {
      const created = await chrome.tabs.create({ url, active: true });
      tabId = created && created.id;
      if (tabId) {
        await attachTab("", { preferPage: true, tabId, urlHint: url });
      }
    } catch (e) {
      return {
        ok: false,
        error: "NAV_FAIL",
        message: String(e && e.message ? e.message : e),
        version: BRIDGE_VERSION,
      };
    }
    await waitTabComplete(tabId, 10000);
    return handleObservePage({ force: false, preferStay: true, query, urlHint: url, waitHydrate: true });
  }
  if (Number(attachMeta.pageTabId) !== tabId) {
    await attachTab("", { preferPage: true, tabId, urlHint: url });
  }
  let prevUrl = "";
  try {
    const t = await chrome.tabs.get(tabId);
    prevUrl = (t && t.url) || "";
  } catch (_) {
    /* ignore */
  }
  try {
    await chrome.tabs.update(tabId, { url });
  } catch (e) {
    return {
      ok: false,
      error: "NAV_FAIL",
      message: String(e && e.message ? e.message : e),
      version: BRIDGE_VERSION,
    };
  }
  await waitTabUrlChange(tabId, prevUrl, 8000);
  await waitTabComplete(tabId, 8000);
  return handleObservePage({ force: false, preferStay: true, query, urlHint: url, waitHydrate: true });
}

async function dispatchTrustedEnter() {
  if (!attachedDebuggee) return false;
  try {
    for (const type of ["keyDown", "keyUp"]) {
      await chrome.debugger.sendCommand(attachedDebuggee, "Input.dispatchKeyEvent", {
        type,
        key: "Enter",
        code: "Enter",
        windowsVirtualKeyCode: 13,
        nativeVirtualKeyCode: 13,
        unmodifiedText: "\r",
        text: "\r",
      });
    }
    return true;
  } catch (_) {
    return false;
  }
}

async function snapshotAfterAction(opts) {
  const o = opts && typeof opts === "object" ? opts : {};
  if (o.waitNav && o.tabId && o.prevUrl) {
    await waitTabUrlChange(o.tabId, o.prevUrl, 2200);
  } else {
    await sleepMs(400);
  }
  try {
    return await handleObservePage({ force: false, light: false, preferStay: true });
  } catch (_) {
    return null;
  }
}

function refFailBody(v, fallback) {
  const stale = !!(v && v.stale);
  const reason = (v && v.reason) || fallback;
  return {
    ok: false,
    error: stale ? "STALE_REF" : reason === "canvas" ? "CANVAS" : "CLICK_FAIL",
    message: reason === "canvas" ? "canvas 页禁止 clickRef/typeRef" : String(reason),
    stale,
    version: BRIDGE_VERSION,
  };
}

function resolveNavigableHref(href, pageUrl) {
  const raw = String(href || "").trim();
  if (!raw || raw.startsWith("#") || /^javascript:/i.test(raw) || /^mailto:/i.test(raw))
    return "";
  let abs = "";
  try {
    abs = new URL(raw, pageUrl || undefined).href;
  } catch (_) {
    return "";
  }
  if (!/^https?:/i.test(abs)) return "";
  let destHost = "";
  let destPath = "/";
  let curHost = "";
  let curPath = "/";
  try {
    const dest = new URL(abs);
    destHost = String(dest.hostname || "").toLowerCase();
    destPath = dest.pathname || "/";
    const cur = new URL(String(pageUrl || abs));
    curHost = String(cur.hostname || "").toLowerCase();
    curPath = cur.pathname || "/";
  } catch (_) {
    return abs;
  }
  const onBili = curHost.indexOf("bilibili.com") >= 0 || destHost.indexOf("bilibili.com") >= 0;
  if (onBili) {
    if (/^\/space\/\d+/.test(destPath) && destHost.indexOf("space.") < 0) {
      abs = "https://space.bilibili.com/" + destPath.replace(/^\/space\//, "");
      destHost = "space.bilibili.com";
      destPath = "/" + destPath.replace(/^\/space\//, "");
    }
    if (/^\/video\//.test(destPath) && destHost.indexOf("search.") >= 0) {
      abs = "https://www.bilibili.com" + destPath;
      destHost = "www.bilibili.com";
    }
    if (/^\/bangumi\//.test(destPath) && destHost.indexOf("search.") >= 0) {
      abs = "https://www.bilibili.com" + destPath;
      destHost = "www.bilibili.com";
    }
    if (destHost.indexOf("search.") >= 0 && /^\/\d+$/.test(destPath)) return "";
  }
  if (curHost === destHost && curPath === destPath) return "";
  return abs;
}

async function handleClickRef(msg) {
  const ref = String(msg.ref || "");
  if (!/^e[1-9][0-9]{0,3}$/.test(ref)) {
    return { ok: false, error: "BAD_REF", message: "ref 无效", version: BRIDGE_VERSION };
  }
  const tabId = Number(attachMeta.pageTabId) || (attachedDebuggee && attachedDebuggee.tabId) || 0;
  if (!tabId) {
    return {
      ok: false,
      error: "NO_TAB",
      message: "尚未 observePage / attach",
      version: BRIDGE_VERSION,
    };
  }
  try {
    const prepInj = await chrome.scripting.executeScript({
      target: { tabId, frameIds: [0] },
      func: qstClickRef,
      args: [ref, false, false],
    });
    const prep = prepInj && prepInj[0] && prepInj[0].result;
    if (!prep || !prep.ok) return refFailBody(prep, "clickRef 失败");
    let tabUrl = "";
    try {
      const t = await chrome.tabs.get(tabId);
      tabUrl = (t && t.url) || "";
    } catch (_) {}
    const navUrl = resolveNavigableHref(prep.href, tabUrl);
    if (navUrl && !msg.doubleClick) {
      const nav = await handleNavigatePage({ url: navUrl, query: "", tabId });
      if (nav && nav.ok) {
        return Object.assign({}, nav, {
          ok: true,
          ref,
          clicked: true,
          navigated: true,
          screenX: prep.screenX,
          screenY: prep.screenY,
          clickName: prep.name || "",
          version: BRIDGE_VERSION,
        });
      }
    }
    let trusted = await trustedPageClick(prep.x, prep.y, !!msg.doubleClick);
    if (!trusted) {
      const inj = await chrome.scripting.executeScript({
        target: { tabId, frameIds: [0] },
        func: qstClickRef,
        args: [ref, !!msg.doubleClick, true],
      });
      const v = inj && inj[0] && inj[0].result;
      if (!v || !v.ok) return refFailBody(v, "clickRef 失败");
    }
    const snap = await snapshotAfterAction();
    if (snap && snap.ok) {
      return Object.assign({}, snap, {
        ok: true,
        ref,
        clicked: true,
        trusted,
        screenX: prep.screenX,
        screenY: prep.screenY,
        clickName: prep.name || "",
        version: BRIDGE_VERSION,
      });
    }
    return {
      ok: true,
      ref,
      x: prep.x,
      y: prep.y,
      trusted,
      version: BRIDGE_VERSION,
    };
  } catch (e) {
    return {
      ok: false,
      error: "CLICK_FAIL",
      message: String(e && e.message ? e.message : e),
      version: BRIDGE_VERSION,
    };
  }
}

async function handleTypeRef(msg) {
  const ref = String(msg.ref || "");
  if (!/^e[1-9][0-9]{0,3}$/.test(ref)) {
    return { ok: false, error: "BAD_REF", message: "ref 无效", version: BRIDGE_VERSION };
  }
  const tabId = Number(attachMeta.pageTabId) || (attachedDebuggee && attachedDebuggee.tabId) || 0;
  if (!tabId) {
    return {
      ok: false,
      error: "NO_TAB",
      message: "尚未 observePage / attach",
      version: BRIDGE_VERSION,
    };
  }
  const text = String(msg.text || "");
  const clearFirst = msg.clearFirst !== false;
  try {
    const inj = await chrome.scripting.executeScript({
      target: { tabId, frameIds: [0] },
      func: qstFillRef,
      args: [ref, text, clearFirst],
    });
    const v = inj && inj[0] && inj[0].result;
    if (!v || !v.ok) return refFailBody(v, "typeRef 失败");
    let prevUrl = "";
    try {
      const t = await chrome.tabs.get(tabId);
      prevUrl = (t && t.url) || "";
    } catch (_) {
      /* ignore */
    }
    if (msg.submit) {
      try {
        await chrome.scripting.executeScript({
          target: { tabId, frameIds: [0] },
          func: (r) => {
            const store = window.__qstA11y;
            const el = store && store.nodes && store.nodes.get(r);
            if (!el) return;
            try {
              if (el.form && typeof el.form.requestSubmit === "function") el.form.requestSubmit();
            } catch (_) {}
            el.dispatchEvent(
              new KeyboardEvent("keydown", {
                key: "Enter",
                code: "Enter",
                keyCode: 13,
                which: 13,
                bubbles: true,
                cancelable: true,
              })
            );
            el.dispatchEvent(
              new KeyboardEvent("keyup", {
                key: "Enter",
                code: "Enter",
                keyCode: 13,
                which: 13,
                bubbles: true,
                cancelable: true,
              })
            );
          },
          args: [ref],
        });
      } catch (_) {}
      await dispatchTrustedEnter();
    }
    const snap = await snapshotAfterAction(
      msg.submit ? { waitNav: true, tabId, prevUrl } : {}
    );
    if (snap && snap.ok) {
      return Object.assign({}, snap, {
        ok: true,
        ref,
        typed: true,
        version: BRIDGE_VERSION,
      });
    }
    return { ok: true, ref, typed: true, version: BRIDGE_VERSION };
  } catch (e) {
    return {
      ok: false,
      error: "TYPE_FAIL",
      message: String(e && e.message ? e.message : e),
      version: BRIDGE_VERSION,
    };
  }
}

function reply(msg, body) {
  const payload = JSON.stringify(Object.assign({ id: msg.id, type: "result" }, body));
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(payload);
    return;
  }
  chrome.runtime.sendMessage({ type: "bridgeSend", raw: payload }).catch(() => {});
}

async function onMessage(raw) {
  let msg;
  try {
    msg = JSON.parse(raw);
  } catch (_) {
    return;
  }
  const type = msg.type || "";
  if (type === "ping") {
    reply(msg, { ok: true, pong: true, version: BRIDGE_VERSION });
    return;
  }
  if (type === "hello") {
    reply(msg, {
      ok: true,
      version: BRIDGE_VERSION,
      role: "extension",
      vision: true,
      capabilities: ["vision", "mouse", "keys", "layout", "pageSnapshot", "typeRef", "navigatePage"],
    });
    setStatus("ready", `已握手 v${BRIDGE_VERSION} vision`);
    return;
  }
  if (type === "attach") {
    reply(
      msg,
      await attachTab(msg.titleHint || "", {
        boundLeft: msg.boundLeft,
        boundTop: msg.boundTop,
        boundRight: msg.boundRight,
        boundBottom: msg.boundBottom,
        preferUnfocused: msg.preferUnfocused,
        tabId: msg.tabId,
        stampMarker: msg.stampMarker,
      })
    );
    return;
  }
  if (type === "listPages") {
    reply(msg, await listPages(msg.titleHint || ""));
    return;
  }
  if (type === "layout") {
    reply(msg, await refreshLayout());
    return;
  }
  if (type === "screenshot" || type === "vision") {
    reply(msg, await captureScreenshot());
    return;
  }
  if (type === "mouse") {
    const mouseResult = await handleMouseCommand(msg);
    reply(msg, mouseResult);
    // reply 后再异步补 touch，造梦系 H5 常吃触摸；不 await 以免拖垮桥。
    if (mouseResult && mouseResult.ok && String(msg.action || "click") === "click") {
      const tx = Number(mouseResult.mappedX);
      const ty = Number(mouseResult.mappedY);
      if (Number.isFinite(tx) && Number.isFinite(ty)) {
        void dispatchTouchClick(tx, ty);
      }
    }
    return;
  }
  if (type === "detach") {
    await detachDebugger();
    setStatus("ready", "已 detach");
    reply(msg, { ok: true, version: BRIDGE_VERSION });
    return;
  }
  if (type === "cdp") {
    reply(msg, await sendCdp(msg.method || "", msg.params || {}));
    return;
  }
  if (type === "observePage") {
    reply(msg, await handleObservePage(msg));
    return;
  }
  if (type === "clickRef") {
    reply(msg, await handleClickRef(msg));
    return;
  }
  if (type === "typeRef") {
    reply(msg, await handleTypeRef(msg));
    return;
  }
  if (type === "navigatePage") {
    reply(msg, await handleNavigatePage(msg));
    return;
  }
  reply(msg, { ok: false, error: "UNKNOWN", message: type });
}

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
    const timer = setTimeout(() => finish(null), 3000);
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

async function loadAllBridgeRuntimes() {
  const out = [];
  const seen = new Set();
  const add = (info) => {
    if (!info || !info.ok || !info.token) return;
    const port = Number(info.port);
    if (port < PORT_LO || port > PORT_HI) return;
    const key = port + ":" + String(info.token);
    if (seen.has(key)) return;
    seen.add(key);
    out.push({ port, token: String(info.token) });
  };
  try {
    add(await loadBridgeRuntimeFromNative());
  } catch (_) {
    /* native host is optional for unpacked */
  }
  try {
    const res = await fetch(chrome.runtime.getURL("bridge_runtime.json"), {
      cache: "no-store",
    });
    if (res.ok) add(await res.json());
  } catch (_) {
    /* packed CRX has no host-written json */
  }
  return out;
}

function connectWebSocket(port, token) {
  return new Promise((resolve) => {
    if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
      resolve(true);
      return;
    }
    let settled = false;
    const done = (v) => {
      if (!settled) {
        settled = true;
        resolve(!!v);
      }
    };
    let socket;
    try {
      socket = new WebSocket(
        `ws://127.0.0.1:${port}/qst/ws?token=${encodeURIComponent(token)}`
      );
    } catch (e) {
      setStatus("error", `WebSocket 创建失败: ${e && e.message ? e.message : e}`);
      done(false);
      return;
    }
    socket.onopen = () => {
      ws = socket;
      bridgePort = port;
      bridgeToken = token;
      setStatus("connected", `ws port=${port} v${BRIDGE_VERSION}`);
      socket.send(JSON.stringify({ id: 0, type: "hello", token }));
      done(true);
    };
    socket.onmessage = (ev) => {
      onMessage(ev.data);
    };
    socket.onclose = (ev) => {
      if (ws === socket) {
        ws = null;
        bridgePort = 0;
        bridgeToken = "";
      }
      setStatus(
        "disconnected",
        `WS关闭 code=${ev.code} reason=${ev.reason || ""} wasClean=${ev.wasClean}`
      );
      void detachDebugger();
      done(false);
    };
    socket.onerror = () => {
      setStatus("error", `WS错误 port=${port}`);
      try {
        socket.close();
      } catch (_) {
        /* ignore */
      }
      done(false);
    };
    setTimeout(() => done(!!(ws && ws === socket && ws.readyState === WebSocket.OPEN)), 2500);
  });
}

async function tryDiscoverAndConnect() {
  if (discoverBusy) return;
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
    return;
  }
  discoverBusy = true;
  try {
    const runtimes = await loadAllBridgeRuntimes();
    if (!runtimes.length) {
      setStatus("disconnected", "等待宿主写入桥凭证");
      return;
    }
    setStatus("connecting", `v${BRIDGE_VERSION} 直连本机桥`);
    for (const runtime of runtimes) {
      if (await connectWebSocket(runtime.port, runtime.token)) return;
    }
    setStatus("waiting", "未发现键鼠工坊桥（请先运行键鼠工坊）");
  } finally {
    discoverBusy = false;
  }
}

chrome.runtime.onInstalled.addListener(() => {
  setStatus("installed", `v${BRIDGE_VERSION} 已安装`);
  tryDiscoverAndConnect();
});

chrome.runtime.onStartup.addListener(() => {
  tryDiscoverAndConnect();
});

chrome.alarms.create("qstBridgePoll", { periodInMinutes: 0.5 });
chrome.alarms.onAlarm.addListener((alarm) => {
  if (alarm.name === "qstBridgePoll") tryDiscoverAndConnect();
});

chrome.debugger.onDetach.addListener((source, reason) => {
  if (suppressDetachClear) return;

  // 壳页 shot 双挂被卸：只清 pageShot，勿拆 iframe 会话（否则键鼠断、下一帧又 attach 闪屏）。
  if (
    pageShotDebuggee
    && source
    && typeof source.tabId === "number"
    && pageShotDebuggee.tabId === source.tabId
    && !source.targetId
  ) {
    pageShotDebuggee = null;
    // 若当前主会话就是壳页 tab（无 iframe），继续走下方逻辑。
    if (
      !(
        attachedDebuggee
        && attachedDebuggee.tabId != null
        && attachedDebuggee.tabId === source.tabId
        && !attachedDebuggee.targetId
      )
    ) {
      return;
    }
  }

  if (!attachedDebuggee) return;
  const sameTab =
    attachedDebuggee.tabId != null && source.tabId === attachedDebuggee.tabId;
  const sameTarget =
    attachedDebuggee.targetId && source.targetId === attachedDebuggee.targetId;
  if (!sameTab && !sameTarget) return;
  attachedDebuggee = null;
  pageShotDebuggee = null;

  // 用户点了调试条「取消」：尊重用户，禁止自动重挂（否则黄条永远消不掉）。
  if (reason === "canceled_by_user") {
    sessionActive = false;
    setStatus("ready", "用户取消调试");
    return;
  }
  // 空闲/脚本已结束：禁止重挂。
  if (!sessionActive) {
    setStatus("ready", `debugger 已分离(${reason || "?"})`);
    return;
  }
  // 仅脚本运行中被 Edge 踢掉（最小化/切桌面等）才静默重挂。
  setStatus("connected", `debugger 已分离(${reason || "?"})，将自动重挂`);
  setTimeout(() => {
    if (!sessionActive) return;
    ensureAttached().catch(() => {});
  }, 80);
});

chrome.runtime.onMessage.addListener((msg, _sender, sendResponse) => {
  if (msg && msg.type === "getStatus") {
    sendResponse(lastStatus);
    return false;
  }
  if (msg && msg.type === "kickBridge") {
    kickBridgeConnect();
    sendResponse({ ok: true });
    return false;
  }
  if (msg && msg.type === "bridgeRecv") {
    onMessage(msg.raw);
    sendResponse({ ok: true });
    return false;
  }
  if (msg && msg.type === "reconnect") {
    closeWs();
    tryDiscoverAndConnect().then(() => sendResponse(lastStatus));
    return true;
  }
  return false;
});

let connectKickTimer = 0;
function kickBridgeConnect() {
  if (connectKickTimer) return;
  connectKickTimer = setTimeout(() => {
    connectKickTimer = 0;
    tryDiscoverAndConnect();
  }, 250);
}

if (chrome.tabs && chrome.tabs.onUpdated) {
  chrome.tabs.onUpdated.addListener((_id, info) => {
    if (info && (info.status === "complete" || info.url)) kickBridgeConnect();
  });
}
if (chrome.windows && chrome.windows.onCreated) {
  chrome.windows.onCreated.addListener(() => kickBridgeConnect());
}

// 保活：避免长时间无事件后 SW 彻底睡死（仍可能短睡，靠轮询恢复）
setInterval(() => {
  tryDiscoverAndConnect();
}, RECONNECT_MS);

tryDiscoverAndConnect();
