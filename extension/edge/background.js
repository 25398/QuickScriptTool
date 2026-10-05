/* 键鼠工坊网页键鼠桥 — MV3 service worker（WebSocket 直接连本机桥） */
const BRIDGE_VERSION = "1.0.43";
const PORT_LO = 19228;
const PORT_HI = 19240;
const RECONNECT_MS = 1500;
/**
 * native 宿主（com.quickscripttool.bridge）的清单 path 指向主程序 exe，Chromium 每次
 * connectNative 都会用管道拉起一个**完整进程**（并在宿主里重写一次注册表）。
 * 本文件由 1.5s 轮询驱动 ⇒ 宿主没运行时会造成每天上万次进程创建。
 * 因此：优先读宿主写入的 bridge_runtime.json（侧载扩展零进程），
 * 只有读不到（打包 CRX）才问 native，且两次之间至少间隔 NATIVE_PROBE_MIN_MS。
 */
const NATIVE_PROBE_MIN_MS = 30000;
let lastNativeProbeAt = 0;
/**
 * 「需要问 native 兜底」标志：宿主每次启动都会换随机 token，若 bridge_runtime.json
 * 因故没被重写，扩展会一直拿着陈旧 token 连不上 —— 因此连接失败后必须允许再问一次
 * native，否则等于把发现能力弄丢。连上后清零。
 *
 * ⚠⚠ 与之配套的是 `lastRejectedToken`（2026-09-27 真机）：只有"这个 token 已经被宿主
 *   拒过"才算陈旧。**不能**把"上一轮没连上"当成陈旧 —— 宿主没在跑时每一轮都连不上，
 *   那样每 1.5s 就会 connectNative 拉起一个完整宿主进程（本文件顶部那条注释警告的
 *   正是这件事），浏览器里会攒出一串看不懂的进程。
 *   宿主每次启动都换 token ⇒ 「同一 token 又失败一次」才是可靠且廉价的陈旧判据。
 */
let nativeProbeWanted = false;
/** 上一次被桥拒掉的 token（`""` = 没有）。 */
let lastRejectedToken = "";

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

/// 浏览器**内置页**（edge://… / chrome://…）扩展无权注入 content script，
/// 这是 Chrome/Edge 的硬限制，不是配置问题。但用户/模型常在这里反复试：
/// 抓不到 → 转 zoom 看截图 → 标题是省略号截断的 → 再 zoom …… 白烧好几轮。
/// 所以回执必须**直接说穿**并给出唯一可行的替代路线（UIA）。
const BUILTIN_PAGE_HINT =
  "⚠ 当前只有浏览器内置页（edge:// / chrome://）在命中范围内。" +
  "扩展**无法**在这类页面上注入脚本 —— 这是 Chrome/Edge 的硬限制，重试无效。" +
  "请改走 UIA 路线：listUiControls（必要时加 typeFilter=\"列表项\"）拿到条目名，" +
  "再用 invokeUiControl 触发；标题请读 UIA 名字（界面上是省略号截断的，别 zoom 看截图）。";

async function pickBrowsePage(titleHint, opts) {
  const o = opts && typeof opts === "object" ? opts : {};
  const allTargets = await listTargets();
  const pages = allTargets.filter(
    (t) => t.type === "page" && typeof t.tabId === "number" && !isUtilityTarget(t)
  );
  // 命中范围内有没有浏览器内置页？有的话，模型这次的失败几乎必然是因为它 ——
  // 回执里必须说穿，否则它会以为「只是没匹配上」，继续 zoom 烧 token。
  const builtinTabs = allTargets.filter(
    (t) => t.type === "page" && typeof t.tabId === "number"
      && /^(edge|chrome|devtools|chrome-extension|extension):\/\//.test(t.url || "")
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
  // 只剩内置页 ⇒ 这就是失败的真正原因，把候选列成 URL 并附上替代路线。
  if (builtinTabs.length > 0 && pages.length === 0) {
    return {
      page: null,
      candidates: builtinTabs.map((t) => t.url || "(无标题)"),
      error: "NO_TAB",
      message: BUILTIN_PAGE_HINT,
      builtinOnly: true,
    };
  }
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
  const allTargets = await listTargets();
  const pages = allTargets.filter(
    (t) => t.type === "page" && typeof t.tabId === "number" && !isUtilityTarget(t)
  );
  // 同 pickBrowsePage：只剩内置页时，必须把「扩展无权注入」说穿（见 BUILTIN_PAGE_HINT）。
  const builtinOnly = pages.length === 0
    && allTargets.some((t) => t.type === "page"
      && /^(edge|chrome|devtools|chrome-extension|extension):\/\//.test(t.url || ""));
  const builtinFallback = builtinOnly
    ? { page: null, candidates: [], error: "NO_TAB", message: BUILTIN_PAGE_HINT, builtinOnly: true }
    : null;
  const candidates = pages.map((p) => p.title || p.url || "(无标题)");
  if (!hint) {
    if (pages.length === 1) return { page: pages[0], candidates };
    if (builtinFallback) return builtinFallback;
    return { page: null, candidates, error: pages.length ? "AMBIGUOUS" : "NO_TAB" };
  }

  const matched = pages
    .map((p) => ({ page: p, titleScore: titleMatchScore(hint, p.title || "") }))
    .filter((x) => x.titleScore > 0);
  if (!matched.length) return builtinFallback || { page: null, candidates };

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
  let builtinOnly = false;

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
    builtinOnly = picked.builtinOnly === true;
  }

  if (!page) {
    const errCode = error === "AMBIGUOUS" ? "AMBIGUOUS" : "NO_TAB";
    setStatus("no-tab", message || `未匹配到目标页（可见 ${candidates.length} 个）`);
    const out = {
      ok: false,
      error: errCode,
      message: message || (preferPage ? "未找到可抓取的网页标签" : "未找到匹配的游戏标签页"),
      candidates: (candidates || []).slice(0, 12),
      version: BRIDGE_VERSION,
    };
    // ★ 说穿「这是浏览器内置页」：宿主可据此直接劝模型走 UIA，不必重试本工具。
    if (builtinOnly) {
      out.builtinPage = true;
      out.hint = BUILTIN_PAGE_HINT;
    }
    return out;
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

  // ★★★ **容器标识**（2026-10-03 方向 1：抓取的通用性）
  //
  //   问题：原来分组靠 **y 坐标**（宿主侧几何聚类）⇒ 页面一滚动 y 全变 ⇒ 漏题 / 重复 ✓
  //   做法：往上找**第一个含 ≥2 个同构孩子的祖先** —— 那就是"列表项容器"
  //     （题 / 表格行 / 消息 / 卡片 都符合这个形状）⇒ 用它的 **标签+class+序号** 做 id ✓
  //   ⚠ 这个 id **不依赖坐标** ⇒ 对滚动**天然稳定** ✓
  //   ⚠ 通用性：判据是**结构**（同构兄弟）而不是 `radio/checkbox` 这种语义 ⇒
  //     换任何列表型页面都成立，**不针对"练习题"写死** ✓
  function qstListContainerKey(el) {
    let p = el.parentElement;
    for (let d = 0; p && d < 8; ++d, p = p.parentElement) {
      const kids = Array.from(p.children || []);
      let same = 0;
      for (const c of kids) {
        if (c.tagName === el.tagName && String(c.className) === String(el.className)) ++same;
      }
      if (same >= 2) {
        const pp = p.parentElement;
        const idx = pp ? Array.from(pp.children).indexOf(p) : 0;
        const cls = String(p.className || "").split(/\s+/).filter(Boolean).slice(0, 2).join(".");
        return p.tagName + (cls ? "." + cls : "") + "@" + idx;
      }
    }
    return "";
  }
  const light = !!(opts && opts.light);
  // ★★ 曾经这里是 `Math.min(80, ... || 64)`，并且在**排序之后** `take.slice(0, maxNodes)`。
  //    后果（实测，超星「第1章概述练习题」50 题作业页）：节点按「可视区优先、阅读顺序」
  //    排好序后砍到 64 条，而一道多选题的「题干 + 4~5 个选项」就吃掉 5~6 条 ⇒
  //    **只有第 1~2 题进得了树，第 3 题以后整片消失**。宿主那侧看到的计数是
  //    `n=21`、标注「屏外0 · 下17.6屏」—— 树自洽、看不出丢东西，
  //    于是模型的行为退化成「盲滚 + 截图抄」，还抄不全。
  //
  //    教训：**列表型页面的枚举上限不能是一个「猜的常数」**。页有多长我们不知道，
  //    但**丢了多少我们必须知道并说出来**。所以这里不再默认硬砍：
  //      · 默认 **全量**（由宿主按自己的字符预算分页/截断，那边才知道 token 成本）；
  //      · 确实要限流时才用 maxNodes，且**必须**回报 total/truncated 供调用方分页。
  const askMax = Number((opts && opts.maxNodes) || 0);
  const maxNodes = askMax > 0 ? Math.max(1, Math.min(20000, Math.floor(askMax))) : 0;
  // 分页起点（宿主传 offset，配合 maxNodes 逐段取，长页才可能被完整枚举）
  const offset = Math.max(0, Math.floor(Number((opts && opts.offset) || 0)));
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
  // ★★ 这里曾经是 `raw.filter((el) => isVisible(el) || (isSpaceVideoLink(el) && isRenderable(el)))`。
  //    `isVisible` 里有一条 `r.top > vh || r.left > vw ⇒ false` —— 它把**所有屏外元素**
  //    在候选集阶段就丢掉了。后果（实测，超星 50 题作业页）：整页 300+ 个可枚举控件，
  //    候选集只剩可视区内那 20~24 个 ⇒ `enumerateTotal` 与 `nodes` 一起被压死，
  //    宿主回执永远是「可视23 · 屏外0」，**看起来自洽**，模型根本看不出「还有 30 题没给」。
  //    然后它就退化成「盲滚 → 换一棵新树 → 再截一张图验收」，21 次截屏、20 次滚动，
  //    成本和时间全耗在这个循环上（那次任务花了 3.77 CNY）。
  //
  //    教训：**「在不在可视区」是排版信息，不是可用性判据**。
  //    长列表页的正确做法是「全量枚举 + 由调用方按 token 预算分页」，
  //    分页参数（offset/maxNodes）和字符预算的关系宿主才清楚，扩展不该替它做决定。
  //    ⇒ 候选集只排除「压根不可渲染」的（display:none / 尺寸为 0 / disabled），
  //      **屏外元素保留**，交给下面的 `inView` 标记 + 排序 + 宿主分页。
  const candidates = raw.filter(
    (el) => isRenderable(el) || (isSpaceVideoLink(el) && isRenderable(el))
  );
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
  // ★ enumerateTotal = 排序后**完整**候选数（分页前的总量）；
  //   returned = 本次真正返回的条数。两者一起回报，调用方才知道「有没有漏」。
  const enumerateTotal = take.length;
  const from = Math.min(offset, enumerateTotal);
  if (maxNodes > 0) take = take.slice(from, from + maxNodes);
  else if (from > 0) take = take.slice(from);
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
      // ★ 容器标识（见上面 qstListContainerKey）——宿主按它分组 ⇒ 对滚动稳定 ✓
      group: qstListContainerKey(el),
      w: Math.round(r.width),
      h: Math.round(r.height),
      inView: !!item.inView,
      // ★★ `absent` = 与视口**完全不相交**（必须滚动才看得见）。
      //    它与 `inView` 用的是**同一个判据**（`viewportIntersect`）⇒ 当前恒有
      //    `absent === !inView`。之所以两个都发，是因为**语义不同、将来可能分开**：
      //      · `inView`   —— 宿主拿它算「可视 N」；
      //      · `absent`   —— 宿主拿它算「屏外 M」（`PageSnapshotNodeAbsent`）。
      //    分开的理由：以后若要支持「横跨可视区边界」的精细区分（部分可见的算可视，
      //    完全在外面的才算屏外），只需改这一行；宿主侧不用动，
      //    且**旧扩展不发 absent 时会回落到 y/vh 推断**，行为不变。
      //    ⚠ 别把它当成「与 inView 反义」去简化掉 —— 宿主的旧扩展兼容路径依赖它的缺失。
      absent: !(
        r.bottom > 0 && r.right > 0 && r.top < vh && r.left < vw
      ),
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
    // ★覆盖度回执：让宿主能判断「这棵树是不是被砍过」，并据此分页。
    //   enumerateTotal 是**排序后、分页前的完整候选数**（本次页面上真正可枚举的量）；
    //   returned 是本次返回条数；nextOffset>0 表示还有下一段可继续取。
    //   ⚠ 不回报这两个数 = 又把「丢了多少」藏起来，模型只能靠「下17.6屏」瞎猜。
    enumerateTotal,
    offset: from,
    returned: nodes.length,
    nextOffset: from + nodes.length < enumerateTotal ? from + nodes.length : 0,
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

// ════════════════════════════════════════════════════════════════════
// 网页版 AI 适配层（Web AI Backend）
// 见 docs/web-ai-backend-design.md。
//
// 架构参考 chen-squared/browser-ai-bridge（MIT）：provider 配置化 selector +
// 「忙闲判据 + 内容稳定」双条件判回答完成 + 提交确认信号。
//
// ⚠ 与既有代码的分工：
//   · 输入框写入仍走 typeRef/qstFillRef（已有 contenteditable 分支），
//     但富文本框架（TipTap/Lexical/ProseMirror）通常不认原生 input 事件，
//     故另提供 **CDP 真实按键**通道：handleWebAiRichType（本文件下方实现）。
//   · 读回答**不能**复用 qstBuildPageSnapshot：它的 isVisible() 只收视口内元素，
//     回答区滚出视口就取不到（MEMORY.md §22 同类前科）。这里按 selector 取，
//     并主动滚到回答区。
// ════════════════════════════════════════════════════════════════════

/// provider 配置缓存（web_ai_providers.json，随扩展目录分发）
let webAiProvidersCache = null;

async function loadWebAiProviders() {
  if (webAiProvidersCache) return webAiProvidersCache;
  try {
    const url = chrome.runtime.getURL("web_ai_providers.json");
    const res = await fetch(url);
    const j = await res.json();
    webAiProvidersCache = (j && j.providers) || {};
  } catch (e) {
    webAiProvidersCache = {};
  }
  return webAiProvidersCache;
}

async function resolveWebAiProvider(nameOrHint) {
  const all = await loadWebAiProviders();
  const want = String(nameOrHint || "").trim().toLowerCase();
  if (!want) return null;
  if (all[want]) return { id: want, ...all[want] };
  for (const key of Object.keys(all)) {
    const p = all[key];
    const hint = String(p.urlHint || "").toLowerCase();
    const label = String(p.label || "").toLowerCase();
    if (hint && want.indexOf(hint) >= 0) return { id: key, ...p };
    if (label && want.indexOf(label) >= 0) return { id: key, ...p };
  }
  return null;
}

/// 注入：按 provider selector 读取最后一条 assistant 回复。
/// 返回 { ok, text, blocks, busy, truncated, totalBlocks, matchedBy }。
/// ⚠ 与 qstBuildPageSnapshot 的关键差异：**不做视口可见性过滤**，
///   因为回答文本是"读内容"而不是"点控件"，屏外文本同样有效（MEMORY.md §22）。
function qstReadAssistantReply(cfg) {
  const c = cfg || {};
  const responseSelectors = c.responseSelectors || [];
  const busySelectors = c.busySelectors || [];
  const inputSelectors = c.inputSelectors || [];

  // ★★ 排除"推荐追问 / 相关推荐"这类**干扰块**（2026-10-02 真机）
  //   用户截图实证：豆包的推荐问题（"如何使用…？"那三行）被我们当成了**回答内容**
  //   —— 它们在回答容器**下方**，而 `pickVisible` 取的是"最后一个可见的" ⇒ 正好选中它们。
  //   ⚠ 各站点的 class 差异很大 ⇒ **可配置**：provider 里加 `replyExcludeSelectors`，
  //     这里给一组**通用默认**（按语义匹配 suggest/related/recommend/guess 这类命名）。
  const excludeSelectors = [].concat(
    c.replyExcludeSelectors || [],
    ["[class*='suggest']", "[class*='related']", "[class*='recommend']",
     "[class*='guess']", "[class*='followup']", "[class*='question-list']",
     // ★ 豆包实证（2026-10-02 用户给的 DOM）：
     //   · 推荐追问：<span class="… suggest-list-item-title">…？</span>（含 suggest ⇒ 通用规则已覆盖）
     //   · 搜索进度：<div class="… text-dbx-text-secondary">搜索 2 个关键词，参考 11 篇资料</div>
     //     ⚠ 后者 class 里**没有语义关键词** ⇒ 只能按它精确排。
     "[class*='text-dbx-text-secondary']"]
  );
  const isExcluded = (el) => {
    for (const sel of excludeSelectors) {
      try { if (el.closest(sel)) return true; } catch (_) { /* 选择器不合法就跳过 */ }
    }
    return false;
  };

  const pickVisible = (sels) => {
    for (const sel of sels) {
      let list = [];
      try {
        list = Array.from(document.querySelectorAll(sel));
      } catch (_) {
        continue;
      }
      if (!list.length) continue;
      // 取最后一个「有实际尺寸」的（不要求视口内）
      for (let i = list.length - 1; i >= 0; i--) {
        const el = list[i];
        try {
          const r = el.getBoundingClientRect();
          if (r.width < 4 || r.height < 4) continue;
          const st = window.getComputedStyle(el);
          if (st.display === "none" || st.visibility === "hidden") continue;
          if (isExcluded(el)) continue;   // ★ 推荐追问等干扰块，不算回答
        } catch (_) {
          continue;
        }
        return { el, matchedBy: sel };
      }
    }
    return null;
  };

  const busy = (() => {
    for (const sel of busySelectors) {
      try {
        const el = document.querySelector(sel);
        if (!el) continue;
        const r = el.getBoundingClientRect();
        if (r.width >= 4 && r.height >= 4) return true;
        const st = window.getComputedStyle(el);
        if (st.display !== "none" && st.visibility !== "hidden") return true;
      } catch (_) {}
    }
    return false;
  })();

  // ★★ **绝不把"我们自己的提示词"当回答**（2026-09-30 实测事故）。
  //
  //   现象：宿主读到 `原始回复：【键鼠工坊・网页 AI 桥接】…`（4843 字符 = 我们刚写进去的
  //   提示词），而页面上模型的**真回答**（`{"computer":"screenshot"}`）就在下面 ——
  //   说明 responseSelectors 命中了**用户气泡**（页面把用户消息也渲染成了可匹配的消息块）。
  //   ⇒ 排除"含我们横幅标识"的节点（`键鼠工坊` / `网页 AI 桥接` 只可能出现在我们发的内容里），
  //     并在命中被排除时**改选最后一条干净的消息块**；一条干净块都没有就**如实回报**
  //     `only-echo-blocks`（宿主据此报 REPLY_ECHOES_PROMPT，而不是拿回显去解析动作）。
  const qstHasOurBanner = (el) => {
    try {
      const t = String(el.innerText || el.textContent || "");
      return t.includes("键鼠工坊") || t.includes("网页 AI 桥接");
    } catch (_) {
      return false;
    }
  };
  let hit = pickVisible(responseSelectors);
  if (hit && qstHasOurBanner(hit.el)) {
    const cand = [];
    for (const sel of responseSelectors) {
      try {
        for (const el of document.querySelectorAll(sel)) cand.push(el);
      } catch (_) {}
    }
    const clean = Array.from(new Set(cand)).filter((el) => !qstHasOurBanner(el)
      && String(el.innerText || el.textContent || "").trim().length > 0);
    if (clean.length) {
      hit = { el: clean[clean.length - 1], matchedBy: String(hit.matchedBy || "") + "+skipEcho" };
    } else {
      return {
        ok: false,
        busy: busy,
        reason: "only-echo-blocks",
        triedSelectors: responseSelectors,
        url: String(location.href || ""),
        title: String(document.title || ""),
      };
    }
  }
  if (!hit) {
    return {
      ok: false,
      busy: busy,
      reason: "no-response-node",
      triedSelectors: responseSelectors,
      url: String(location.href || ""),
      title: String(document.title || ""),
    };
  }

  // 收集全部块（用于判断是否还在追加），并滚到最后一屏
  const all = [];
  for (const sel of responseSelectors) {
    try {
      const list = Array.from(document.querySelectorAll(sel));
      for (const el of list) all.push(el);
    } catch (_) {}
  }
  const uniq = Array.from(new Set(all));
  const texts = [];
  for (const el of uniq) {
    const t = String(el.innerText || el.textContent || "").trim();
    if (t) texts.push(t);
  }

  // ★★ 剔除"推荐追问"等干扰块（2026-10-02 真机）
  //   用户给的实证 DOM：`<span class="title-sm8Wop suggest-list-item-title">…？</span>`
  //   ⚠⚠ 干扰块通常是**被选中容器的子孙**（不是容器本身）⇒ 上面 `pickVisible` 里的
  //     `closest()` 检查**查不到**它 ⇒ 必须**从文本里减去**这些子孙的文本。
  const rawText0 = String(hit.el.innerText || hit.el.textContent || "").trim();
  const rawText = (() => {
    if (!rawText0) return rawText0;
    let out = rawText0;
    for (const sel of excludeSelectors) {
      let subs = [];
      try { subs = Array.from(hit.el.querySelectorAll(sel)); } catch (_) { continue; }
      for (const sub of subs) {
        const st = String(sub.innerText || sub.textContent || "").trim();
        if (st && out.indexOf(st) >= 0) out = out.split(st).join("");
      }
    }
    return out.trim();
  })();
  // 上限保护：超长时截断并**明说**（MEMORY.md §20：枚举型回执必须带覆盖度）
  const MAX_CHARS = 12000;
  const truncated = rawText.length > MAX_CHARS;
  const text = truncated ? rawText.slice(0, MAX_CHARS) : rawText;

  try {
    hit.el.scrollIntoView({ block: "end" });
  } catch (_) {}

  // 输入框是否为空（提交确认信号之一）
  // ⚠ 三种形态都要认：原生表单有 value；富文本有 innerText；而某些富文本实现
  //   两者都不可靠（isContentEditable 为 false 且 value 为 undefined）⇒ 兜底
  //   按 textContent 判。判不了就报 null（**不要**瞎猜成 false，否则宿主会把
  //   「判不出」当成「没清空」，导致提交确认永远失败）。
  let inputEmpty = null;
  try {
    const inp = (() => {
      for (const sel of inputSelectors) {
        try {
          const el = document.querySelector(sel);
          if (el) return el;
        } catch (_) {}
      }
      return null;
    })();
    if (inp) {
      const ce = inp.isContentEditable
        || inp.getAttribute("contenteditable") === "true"
        || inp.getAttribute("contenteditable") === "";
      let v = null;
      if (ce) v = String(inp.innerText || inp.textContent || "");
      else if (typeof inp.value === "string") v = inp.value;
      else v = String(inp.innerText || inp.textContent || "");
      inputEmpty = v.trim().length === 0;
    }
  } catch (_) {}

  return {
    ok: true,
    text: text,
    textLength: rawText.length,
    truncated: truncated,
    totalBlocks: texts.length,
    busy: busy,
    inputEmpty: inputEmpty,
    matchedBy: hit.matchedBy,
    url: String(location.href || ""),
    title: String(document.title || ""),
  };
}

/// 注入：探针 —— 回报输入框的形态（tagName / contenteditable / role），
/// 用于判断站点是否改版、以及富文本 vs 原生表单（见 docs §5.1 冒烟第 1 步）。
function qstProbeComposer(cfg) {
  const c = cfg || {};
  const inputSelectors = c.inputSelectors || [];
  const sendSelectors = c.sendButtonSelectors || [];
  const probe = (sels) => {
    for (const sel of sels) {
      let el = null;
      try {
        el = document.querySelector(sel);
      } catch (_) {
        continue;
      }
      if (!el) continue;
      const r = (() => {
        try {
          return el.getBoundingClientRect();
        } catch (_) {
          return { width: 0, height: 0 };
        }
      })();
      return {
        matchedBy: sel,
        tagName: String(el.tagName || "").toLowerCase(),
        role: String(el.getAttribute("role") || ""),
        contentEditable: !!el.isContentEditable,
        hasValue: typeof el.value === "string",
        width: Math.round(r.width),
        height: Math.round(r.height),
        visible: r.width >= 4 && r.height >= 4,
      };
    }
    return null;
  };
  return {
    ok: true,
    input: probe(inputSelectors),
    sendButton: probe(sendSelectors),
    url: String(location.href || ""),
    title: String(document.title || ""),
  };
}

async function injectWebAiProbe(tabId, cfg) {
  const r = await injectPageFn(tabId, qstProbeComposer, [cfg || {}]);
  if (r.error) return { ok: false, error: "INJECT_FAILED", message: r.error };
  return r.result;
}

async function injectReadAssistantReply(tabId, cfg) {
  const r = await injectPageFn(tabId, qstReadAssistantReply, [cfg || {}]);
  if (r.error) return { ok: false, reason: "inject:" + r.error };
  return r.result;
}

// ────────────────────────────────────────────────────────────────────
// ★★ 富文本写入：CDP 真实按键通道（qstFillRef 的兜底/替代）
//
// 为什么需要它：qstFillRef 的 contenteditable 分支用 execCommand("insertText")
// + 派发一个 input 事件。对**普通 contenteditable** 够用，但豆包这类
// TipTap / Lexical / ProseMirror / React 富文本**通常不认**：
//   · execCommand 已废弃，部分框架拦截并忽略；
//   · 直接写 textContent 绕过框架的虚拟 DOM ⇒ 状态与 DOM 不一致，提交时被清空；
//   · 框架监听自己的合成事件系统，手搓 InputEvent 进不去。
//
// 这里改用 Chrome DevTools Protocol 的**真实输入**：
//   ① Input.insertText —— 走浏览器输入管线，会触发真实 beforeinput/input
//   ② 逐字 Input.dispatchKeyEvent —— 更接近人手，兜底最顽固的编辑器
//   ③ 提交按 Enter（Input.dispatchKeyEvent，key=Enter）
//
// ⚠ 必须先 focus 目标元素，否则按键会打到别处（CDP 输入作用于**当前焦点**）。
// ⚠ 与 MEMORY.md §17 的关系：那条讲的是**宿主 PostMessage** 不向子窗转发；
//   这里走的是浏览器内部输入管线，不受影响。
// ⚠ 与 §26 的关系：判「写没写进去」要**回读元素内容**，不能只看 API 返回 ok。
// ────────────────────────────────────────────────────────────────────

/// 注入一个**自包含**页面函数，并把「页面里抛错」这件事如实带出来。
///
/// ⚠⚠ 两个必踩的坑（2026-09-24 真机实测各踩一次）：
///   ① `executeScript({func: X})` 只注入 X 的源码 ⇒ X 里调同文件的兄弟函数
///      在页面里是 `ReferenceError`。**只能注入自包含函数**（只依赖全局）。
///   ② 被注入函数抛错时，MV3 的 `executeScript` 是 **resolve** 成
///      `{result: undefined, error: {...}}`（不是 reject）⇒ 只看 `result` 就会把
///      「函数在页面里崩了」误读成「选择器没命中 / 元素不存在」，
///      于是错误信息指向完全错误的方向（实测报成了误导性的 NO_COMPOSER）。
async function injectPageFn(tabId, fn, args) {
  const inj = await chrome.scripting.executeScript({
    target: { tabId, frameIds: [0] },
    func: fn,
    args: args || [],
  });
  const first = inj && inj[0];
  const err = first && first.error
    ? String((first.error && first.error.message) || first.error)
    : "";
  return { result: first ? first.result : undefined, error: err };
}

/// 把 selector 列表解析成元素并 focus。返回 {ok, matchedBy, tag, isEditable}
function qstFocusComposer(cfg) {
  const sels = [].concat(
    (cfg && cfg.inputSelectors) || [],
    ["div[contenteditable='true'][role='textbox']", "div[contenteditable='true']", "textarea"]
  );
  const tried = [];
  for (const s of sels) {
    if (!s) continue;
    tried.push(s);
    let el = null;
    try {
      el = document.querySelector(s);
    } catch (_) {
      continue;
    }
    if (!el) continue;
    const r = el.getBoundingClientRect();
    const visible = (r.width > 0 && r.height > 0);
    // ★★ 不可见**不再跳过**（2026-09-26 真机）。
    //   为什么：`getBoundingClientRect()` 在**后台标签页**里恒为 `0x0`（页面没渲染），
    //   于是"可见性"判据把**已登录、但页面在后台**的站点一律判成"没有输入框"：
    //     · 探测阶段 ⇒ `webAiPickProvider` 全站失败 ⇒ **`NO_WEB_AI_LOGIN`**
    //       （用户原话："我已经登录了呀，我点开网页还是登录态"）
    //     · 写入阶段 ⇒ **`NO_COMPOSER`**（用户原话："发消息都发不了"）
    //   而 CDP 的 `Input.insertText` / `dispatchKeyEvent` **本来就不要求元素可见**
    //   ⇒ 可见只是"优先选择"的依据，**不能当门槛**。
    //   ⚠ 保留 `visible` 标记：**读回答**要站点真的渲染，诊断时用得上。
    try {
      el.scrollIntoView({ block: "center", behavior: "instant" });
    } catch (_) {
      try { el.scrollIntoView(); } catch (__) { /* ignore */ }
    }
    try {
      el.focus();
    } catch (_) { /* ignore */ }
    // 富文本常需要点击才进入编辑态（focus 不够）
    try {
      el.click();
    } catch (_) { /* ignore */ }
    const active = document.activeElement;
    const editable = !!(el.isContentEditable
      || el.getAttribute("contenteditable") === "true"
      || el.tagName === "TEXTAREA"
      || el.tagName === "INPUT");
    return {
      ok: true,
      matchedBy: s,
      tagName: el.tagName,
      isEditable: editable,
      visible: visible,
      // ⚠ 焦点是否真的落到它身上：CDP 按键打给「当前焦点」，这条必须核实
      focusIsSelf: active === el,
      activeTag: active ? active.tagName : "",
      activeSnippet: active && active !== el ? String(active.className || "").slice(0, 60) : "",
    };
  }
  return { ok: false, reason: "no-composer", triedSelectors: tried };
}

/// 清空输入框（CDP 前的准备）。返回清空后读回的内容长度。
function qstComposerClearAndRead(cfg) {
  const sels = [].concat(
    (cfg && cfg.inputSelectors) || [],
    ["div[contenteditable='true'][role='textbox']", "div[contenteditable='true']", "textarea"]
  );
  for (const s of sels) {
    if (!s) continue;
    let el = null;
    try { el = document.querySelector(s); } catch (_) { continue; }
    if (!el) continue;
    try {
      if (el.isContentEditable || el.getAttribute("contenteditable") === "true") {
        // 用 execCommand 清（比直接写 textContent 更友好；框架不认时走兜底）
        el.focus();
        try { document.execCommand("selectAll", false, null); } catch (_) {}
        try { document.execCommand("delete", false, null); } catch (_) {}
        const after = String(el.innerText || el.textContent || "");
        if (after.length > 0) {
          // execCommand 没清掉（框架拦截/废弃 API）⇒ 退回直接写。
          // ⚠ innerText 与 textContent 都要清：两者是不同属性，
          //   只写一个会让「回读」读到旧值 ⇒ 后面误判成「有残留」。
          el.textContent = "";
          if (typeof el.innerText !== "undefined") el.innerText = "";
          el.dispatchEvent(new InputEvent("input", { bubbles: true, data: "", inputType: "deleteContentBackward" }));
        }
      } else if (el.tagName === "TEXTAREA" || el.tagName === "INPUT") {
        const proto = el.tagName === "TEXTAREA" ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
        const d = Object.getOwnPropertyDescriptor(proto, "value");
        if (d && d.set) d.set.call(el, ""); else el.value = "";
        el.dispatchEvent(new InputEvent("input", { bubbles: true, data: "", inputType: "deleteContentBackward" }));
      }
    } catch (_) { /* ignore */ }
    // ⚠ 回读优先级：富文本看 innerText/textContent，原生表单看 value。
    //   与 qstComposerReadText 保持**同一套读法**，否则「写完回读」和「清完回读」
    //   会得出互相矛盾的结论（这是本函数第一次写错的地方）。
    const isRich = !!(el.isContentEditable || el.getAttribute("contenteditable") === "true");
    const now = String(isRich
      ? ((typeof el.innerText === "string" && el.innerText) || el.textContent || "")
      : (el.value || ""));
    return { ok: true, matchedBy: s, textLength: now.length, text: now.slice(0, 120) };
  }
  return { ok: false, reason: "no-composer" };
}

/// 读回输入框当前内容（判「写没写进去」用，见 §26：要盯被保护对象本身）
function qstComposerReadText(cfg) {
  const sels = [].concat(
    (cfg && cfg.inputSelectors) || [],
    ["div[contenteditable='true'][role='textbox']", "div[contenteditable='true']", "textarea"]
  );
  for (const s of sels) {
    if (!s) continue;
    let el = null;
    try { el = document.querySelector(s); } catch (_) { continue; }
    if (!el) continue;
    const isRich = !!(el.isContentEditable || el.getAttribute("contenteditable") === "true");
    // ⚠ 与 qstComposerClearAndRead 用**同一套读法**（两处必须一致，
    //   否则「清完回读」与「写完回读」会给出互相矛盾的结论）
    const t = String(isRich
      ? ((typeof el.innerText === "string" && el.innerText) || el.textContent || "")
      : (el.value || ""));
    return { ok: true, matchedBy: s, textLength: t.length, text: t.slice(0, 300) };
  }
  return { ok: false, reason: "no-composer" };
}

/// 富文本写入主流程（在 page 里跑，配合 CDP 使用）。
/// ⚠ 分两段调用：① 先 focus（本函数）② 再 CDP insertText
///   因为 CDP 的输入命令作用于**当前焦点**，必须先把焦点搬过去。
function qstPrepareComposerForCdp(cfg) {
  const f = qstFocusComposer(cfg);
  if (!f.ok) return f;
  const cleared = qstComposerClearAndRead(cfg);
  return { ...f, cleared };
}

/// 轮询读回答，直到「内容稳定 且 不忙」或超时。
/// 照抄 browser-ai-bridge 的三个 timeout 分工（见 docs §2.3(2)）：
///   progressIdleTimeoutMs  内容无变化且不忙持续超时 ⇒ 放弃
///   maxGenerationTimeoutMs 总时长上限（防卡死）
async function handleWebAiReadReply(msg) {
  const provider = await resolveWebAiProvider(msg.provider || msg.urlHint || "");
  if (!provider) {
    return { ok: false, error: "NO_PROVIDER", message: String(msg.provider || "") };
  }
  const tabId = webAiTargetTabId(msg);
  if (!tabId) return { ok: false, error: "NO_TAB", message: "未 attach 任何标签页" };

  const cfg = {
    inputSelectors: provider.inputSelectors || [],
    responseSelectors: provider.responseSelectors || [],
    busySelectors: provider.busySelectors || [],
  };
  const idleTimeout = Number(msg.progressIdleTimeoutMs || provider.progressIdleTimeoutMs || 30000);
  const maxTotal = Number(msg.maxGenerationTimeoutMs || provider.maxGenerationTimeoutMs || 600000);
  const pollMs = 500;

  const t0 = Date.now();
  let lastText = "";
  let lastChangeAt = Date.now();
  let stableSince = 0;
  let lastSnap = null;
  let seenAnyText = false;

  while (Date.now() - t0 < maxTotal) {
    let snap = null;
    try {
      snap = await injectReadAssistantReply(tabId, cfg);
    } catch (e) {
      return { ok: false, error: "INJECT_FAILED", message: String((e && e.message) || e) };
    }
    if (!snap) return { ok: false, error: "NO_RESULT" };
    if (snap.ok === false && snap.reason === "no-response-node") {
      // 还没出回答节点：不算失败，继续等（首个 token 之前本来就没有容器）
      lastSnap = snap;
      if (Date.now() - t0 > idleTimeout) {
        return {
          ok: false,
          error: "NO_RESPONSE_NODE",
          message: "等待回答容器超时",
          triedSelectors: snap.triedSelectors || [],
        };
      }
      await sleepMs(pollMs);
      continue;
    }
    lastSnap = snap;
    const text = String(snap.text || "");
    if (text !== lastText) {
      lastText = text;
      lastChangeAt = Date.now();
      if (text) seenAnyText = true;
    }
    // 完成判据：不忙 且 内容已稳定 ≥1.2s 且 已有内容
    const idleFor = Date.now() - lastChangeAt;
    if (!snap.busy && seenAnyText && idleFor >= 1200) {
      return {
        ok: true,
        text: lastText,
        textLength: Number(snap.textLength) || lastText.length,
        truncated: !!snap.truncated,
        totalBlocks: Number(snap.totalBlocks) || 0,
        provider: provider.id,
        url: snap.url || "",
        title: snap.title || "",
        elapsedMs: Date.now() - t0,
        matchedBy: snap.matchedBy || "",
      };
    }
    // 空闲超时：既没变化也不忙，长时间没有进展
    if (!snap.busy && idleFor >= idleTimeout) {
      if (seenAnyText) {
        return {
          ok: true,
          text: lastText,
          textLength: Number(snap.textLength) || lastText.length,
          truncated: !!snap.truncated,
          totalBlocks: Number(snap.totalBlocks) || 0,
          provider: provider.id,
          url: snap.url || "",
          title: snap.title || "",
          elapsedMs: Date.now() - t0,
          idleTimeoutHit: true,
          matchedBy: snap.matchedBy || "",
        };
      }
      return {
        ok: false,
        error: "IDLE_NO_CONTENT",
        message: "空闲超时且未读到任何回答内容",
        busy: !!snap.busy,
      };
    }
    await sleepMs(pollMs);
  }
  return {
    ok: seenAnyText,
    error: seenAnyText ? "" : "MAX_TIMEOUT",
    text: lastText,
    message: seenAnyText ? "" : "超过总时长上限仍未读到回答",
    truncated: !!(lastSnap && lastSnap.truncated),
    elapsedMs: Date.now() - t0,
  };
}

async function handleWebAiProbe(msg) {
  const provider = await resolveWebAiProvider(msg.provider || msg.urlHint || "");
  if (!provider) {
    const all = await loadWebAiProviders();
    return {
      ok: false,
      error: "NO_PROVIDER",
      message: String(msg.provider || ""),
      knownProviders: Object.keys(all || {}),
    };
  }
  const tabId = webAiTargetTabId(msg);
  if (!tabId) return { ok: false, error: "NO_TAB", message: "未 attach 任何标签页" };
  const cfg = {
    inputSelectors: provider.inputSelectors || [],
    sendButtonSelectors: provider.sendButtonSelectors || [],
  };
  const r = await injectWebAiProbe(tabId, cfg);
  return { ok: true, provider: provider.id, probe: r };
}

/// 列出所有已知 provider（供宿主做站点选择）
async function handleWebAiProviders() {
  const all = await loadWebAiProviders();
  const out = [];
  for (const key of Object.keys(all || {})) {
    const p = all[key] || {};
    out.push({
      id: key,
      label: p.label || key,
      url: p.url || "",
      urlHint: p.urlHint || "",
      inputKind: p.inputKind || "unknown",
    });
  }
  return { ok: true, providers: out, version: BRIDGE_VERSION };
}

// ────────────────────────────────────────────────────────────────────
// ★★ 富文本写入（CDP 真实按键）—— 攻克「豆包输入框能不能写进去」这一关
//
// 三段式，每段都可独立判成败（不要只看最后一步）：
//   ① prepare：page 里找输入框 + focus + 清空   → 拿到 matchedBy / focusIsSelf
//   ② insert ：CDP Input.insertText             → 走浏览器输入管线
//   ③ verify ：page 里回读内容                  → **必须回读**，API 返回 ok 不算数
// 若 ② 后回读为空 ⇒ 自动降级「逐字 dispatchKeyEvent」（模拟人手）
//
// ⚠ focusIsSelf=false 时**不要**继续插字：CDP 按键会打到别的元素上
//   （典型症状：字进去了但进的是搜索框，页面看着"没反应"）。必须先报错。
// ────────────────────────────────────────────────────────────────────

async function handleWebAiRichType(msg) {
  const provider = await resolveWebAiProvider(msg.provider || msg.urlHint || "");
  if (!provider) {
    const all = await loadWebAiProviders();
    return {
      ok: false,
      error: "NO_PROVIDER",
      message: String(msg.provider || ""),
      knownProviders: Object.keys(all || {}),
    };
  }
  const tabId = webAiTargetTabId(msg);
  if (!tabId) return { ok: false, error: "NO_TAB", message: "未 attach 任何标签页" };
  const text = String(msg.text || "");
  const cfg = { inputSelectors: provider.inputSelectors || [] };
  const debuggee = webAiCdpTarget(msg);

  const out = {
    ok: false,
    provider: provider.id,
    textLength: text.length,
    steps: {},
  };

  // ── ① prepare（page 侧，**分两次注入自包含函数**）──
  // ⚠⚠ 这里踩过一次坑，务必按本写法：`chrome.scripting.executeScript({func: X})`
  //   只把 **X 自己的源码** 序列化后注入页面 —— X 里调用**同文件里的兄弟函数**
  //   在页面里是 `ReferenceError`（那些函数并不存在于页面作用域）。
  //   `qstPrepareComposerForCdp` 正是这种"组合函数"（内部调 qstFocusComposer +
  //   qstComposerClearAndRead），直接当 func: 传过去必然在页面里崩。
  // ⇒ 只注入**自包含**函数（只依赖 document/window 这些全局），一次一个。
  let prep = null;
  try {
    const f = await injectPageFn(tabId, qstFocusComposer, [cfg]);
    if (f.error) {
      out.steps.prepareError = f.error;
      out.error = "PREPARE_FAILED";
      out.message = "页面里 focus 输入框时抛错：" + f.error;
      return out;
    }
    const fv = f.result;
    out.steps.prepare = fv;
    if (!fv || !fv.ok) {
      out.error = "NO_COMPOSER";
      out.message = "没找到可见的输入框（selector 全未命中）";
      return out;
    }
    if (!fv.focusIsSelf) {
      // ★ 焦点没落到输入框 ⇒ 继续插字会打到别处。这是必须报出来的硬错误。
      out.error = "FOCUS_NOT_ON_COMPOSER";
      out.message = "输入框没拿到焦点，CDP 按键会打到别的元素上（先别插字）"
        + (fv.activeSnippet ? "；当前焦点在：" + fv.activeSnippet : "");
      return out;
    }
    const c = await injectPageFn(tabId, qstComposerClearAndRead, [cfg]);
    if (c.error) out.steps.clearError = c.error;
    prep = { ...fv, cleared: c.result };
  } catch (e) {
    out.error = "PREPARE_FAILED";
    out.message = String((e && e.message) || e);
    return out;
  }
  out.steps.prepare = prep;

  // ── ② insert（CDP 真实输入）──
  // 优先 Input.insertText：一次性、最快，且走真实输入管线。
  //
  // ★★ **调试会话掉了就重挂**（2026-09-30 实测 `WRITE_FAILED`）：
  //   宿主收到的错误是
  //     `steps=fallbackKeyEvents: Debugger is not attached to the tab with id: 1738895684`
  //   —— `webAiDebuggee` 里记着"已挂"，但 CDP 侧实际已经断开（另一个调试器挂过/被摘、
  //   标签被替换、DevTools 打开过都会这样）⇒ `Input.insertText` 与逐字按键**全部写不进去**
  //   ⇒ 整轮报废（用户看到的"又提前掉了"）。
  //   ⇒ 写之前先探一下（一次廉价的 Runtime.evaluate），探不到就 **detach+attach 重挂**；
  //     真写失败且错误里含 "not attached" 时，**再重挂并重试一次**。
  const ensureCdpAttached = async () => {
    try {
      await chrome.debugger.sendCommand(debuggee, "Runtime.evaluate", {
        expression: "1", returnByValue: true,
      });
      return true;
    } catch (_) { /* 没挂/已掉 ⇒ 往下重挂 */ }
    try { await chrome.debugger.detach(debuggee); } catch (_) {}
    try {
      await chrome.debugger.attach(debuggee, "1.3");
      out.steps.reattached = true;
      return true;
    } catch (e) {
      out.steps.reattachError = String((e && e.message) || e);
      return false;
    }
  };
  await ensureCdpAttached();
  let inserted = false;
  let insertErr = "";
  const tryInsert = async () => {
    try {
      await chrome.debugger.sendCommand(debuggee, "Input.insertText", { text });
      return "";
    } catch (e) {
      return String((e && e.message) || e);
    }
  };
  insertErr = await tryInsert();
  inserted = !insertErr;
  if (!inserted && /not attached/i.test(insertErr)) {
    // 再重挂一次 + 重写一次（这是可恢复故障，不该报废整轮）
    out.steps.retryAfterReattach = true;
    await ensureCdpAttached();
    insertErr = await tryInsert();
    inserted = !insertErr;
  }
  out.steps.insertText = { ok: inserted, error: insertErr || undefined };

  // ── ③ verify（page 侧回读）──
  const readBack = async () => {
    const r = await injectPageFn(tabId, qstComposerReadText, [cfg]);
    if (r.error) return { ok: false, reason: "inject:" + r.error };
    return r.result;
  };
  let read = await readBack();
  out.steps.verify = read;
  const wroteOk = !!(read && read.ok && read.textLength > 0);

  // ── ②b 降级：逐字真实按键（insertText 无效时）──
  if (!wroteOk) {
    out.steps.fallbackKeyEvents = { attempted: true };
    try {
      // 先再清一次，避免 insertText 部分写进去造成重复
      const c2 = await injectPageFn(tabId, qstComposerClearAndRead, [cfg]);
      if (c2.error) out.steps.fallbackKeyEvents.clearError = c2.error;
      for (const ch of Array.from(text)) {
        await chrome.debugger.sendCommand(debuggee, "Input.dispatchKeyEvent", {
          type: "keyDown",
          text: ch,
          unmodifiedText: ch,
          key: ch,
        });
        await chrome.debugger.sendCommand(debuggee, "Input.dispatchKeyEvent", {
          type: "keyUp",
          key: ch,
        });
      }
      read = await readBack();
      out.steps.verifyAfterFallback = read;
      out.steps.fallbackKeyEvents.ok = !!(read && read.ok && read.textLength > 0);
    } catch (e) {
      out.steps.fallbackKeyEvents.ok = false;
      out.steps.fallbackKeyEvents.error = String((e && e.message) || e);
    }
  }

  const final = out.steps.verifyAfterFallback || out.steps.verify;
  const ok = !!(final && final.ok && final.textLength > 0);
  out.ok = ok;
  if (!ok) {
    out.error = "WRITE_FAILED";
    out.message =
      "CDP insertText 与逐字按键都没写进去 —— selector 可能命中了只读装饰元素，"
      + "或该编辑器需要更底层的输入方式";
  } else {
    out.wroteBack = final.textLength;
  }
  return out;
}

/// 提交（按 Enter）。⚠ 与写入分开，便于宿主在确认写入后再提交。
async function handleWebAiSubmit(msg) {
  const tabId = webAiTargetTabId(msg);
  if (!tabId) return { ok: false, error: "NO_TAB", message: "未 attach 任何标签页" };
  const debuggee = webAiCdpTarget(msg);
  try {
    await chrome.debugger.sendCommand(debuggee, "Input.dispatchKeyEvent", {
      type: "keyDown",
      key: "Enter",
      code: "Enter",
      windowsVirtualKeyCode: 13,
      nativeVirtualKeyCode: 13,
    });
    await chrome.debugger.sendCommand(debuggee, "Input.dispatchKeyEvent", {
      type: "keyUp",
      key: "Enter",
      code: "Enter",
      windowsVirtualKeyCode: 13,
      nativeVirtualKeyCode: 13,
    });
    return { ok: true, via: "cdp-enter" };
  } catch (e) {
    return { ok: false, error: "SUBMIT_FAILED", message: String((e && e.message) || e) };
  }
}

// ────────────────────────────────────────────────────────────────────
// ★★ 网页版 AI 的**独立调试会话**与**专属标签页**
//
// 两个必须分开的东西（2026-09-25 真机事故后加的）：
//
//  ① **调试会话要独立**：脚本回放 / 找图用全局 `attachedDebuggee`，而 `attachTab()`
//     开头就 `await detachDebugger()` —— 网页 AI 若走这条全局路，会**把正在跑的脚本的
//     调试会话拆掉**（用户实测原话：「扩展桥自动关闭调试了」）。
//     ⇒ 网页 AI 自己持一条 `webAiDebuggee`，谁也不动谁。
//
//  ② **标签页要专属**：此前 `EnsureAttached` 挑的是「第一个 URL 匹配的页面」——
//     那往往正是**用户自己正在跟豆包聊的那个对话**。后果是事故级的：
//       · 模型能看到用户的历史对话（隐私 + 上下文污染）；
//       · 我们的请求被追加进用户的主对话，用户看到的"回答"混着两边内容。
//     ⇒ 每个站点有且只有一个**属于本软件**的标签页，tabId 持久化在 `chrome.storage.local`；
//       用户的标签页我们**一个都不碰**。
// ────────────────────────────────────────────────────────────────────

/** 网页 AI 专属的 debugger 会话（与脚本/找图的 `attachedDebuggee` 完全隔离） */
let webAiDebuggee = null;

/** `chrome.storage.local` 里存「我们的标签页」的键：`{ [ownKey]: tabId }` */
const WEB_AI_OWN_TAB_KEY = "qstWebAiOwnTab";

/** ★★ 记「上次挑中且可用的站点」。
 *
 *  ⚠⚠ 为什么必须有（2026-09-26 真机，用户报"发消息半天没反应"+"开一堆标签页"）：
 *    原来的自动挑选是「**逐个打开 4 个站点**探测，每个等 20 秒」
 *    ⇒ 一次提问要 **80 秒**，而且**开 4 个标签页**（用户切到浏览器看到一片狼藉）。
 *    ⇒ 改成：**记住上次成功的站点**，之后**直接用它、零探测**；
 *      只有第一次（或上次那个失效了）才真的去试。
 */
const WEB_AI_LAST_OK_KEY = "qstWebAiLastOk";

/** ★★ 「我们的标签页」的 key：**按会话分**，不是按站点。
 *
 *  ⚠⚠ 为什么不按站点分（2026-09-26 真机）：UI 里同一个模型可以开多个对话，
 *    若共用一个标签页 ⇒ 两个对话的内容落在**同一个网页对话**里互相污染，
 *    用户看到"一个对话在回复后被打到了另一个网页端对话"。
 *    会话 key 由宿主带下来（UI 的 `agentTabs[].key` → 请求体 `user` → 这里的 `sessionKey`）。
 *
 *  ⚠ 老前端 / 探针不带 `sessionKey` ⇒ **回退到 provider.id**（保持老行为，不会更差）。
 */
function webAiOwnKey(provider, msg) {
  const k = String((msg && msg.sessionKey) || "").trim();
  return k ? (String(provider.id || "") + "#" + k) : String(provider.id || "");
}

/** 网页 AI 的目标 tabId：**优先我们自己的**，没有才退回旧的 attachMeta（兼容老路径） */
function webAiTargetTabId(msg) {
  if (webAiDebuggee && typeof webAiDebuggee.tabId === "number" && webAiDebuggee.tabId > 0) {
    return webAiDebuggee.tabId;
  }
  return Number((msg && msg.tabId) || (attachMeta && attachMeta.pageTabId) || 0);
}

/** 网页 AI 的 CDP 目标：我们自己的会话优先（见上方 ①） */
function webAiCdpTarget(msg) {
  if (webAiDebuggee && typeof webAiDebuggee.tabId === "number" && webAiDebuggee.tabId > 0) {
    return webAiDebuggee;
  }
  const tabId = Number((msg && msg.tabId) || (attachMeta && attachMeta.pageTabId) || 0);
  return { tabId };
}

async function loadOwnTabMap() {
  try {
    const got = await chrome.storage.local.get(WEB_AI_OWN_TAB_KEY);
    const m = got && got[WEB_AI_OWN_TAB_KEY];
    return m && typeof m === "object" ? m : {};
  } catch (_) {
    return {};
  }
}

async function saveOwnTabId(providerId, tabId) {
  try {
    const m = await loadOwnTabMap();
    if (Number(tabId) > 0) m[providerId] = Number(tabId);
    else delete m[providerId];
    await chrome.storage.local.set({ [WEB_AI_OWN_TAB_KEY]: m });
  } catch (_) {
    /* 存储失败不影响本次会话（只是下次启动会另开一个标签页） */
  }
}

/** 按 urlHint 穷举返回**所有**匹配标签页。
 *
 *  ⚠⚠ 为什么不能复用 `listPages`：那是给「挑游戏窗」用的 —— 按游戏 URL 打分、
 *    **每个窗口只留分最高的一个**、最后**只返回前 8 条**。
 *    豆包这类站点 URL 得 0 分，会被游戏页挤掉 ⇒ 宿主看到"页面明明开着却说没有"。
 *    这里只做**域名匹配**，不排序、不截断、不丢弃。
 */
async function findProviderTabs(provider) {
  const hint = String((provider && provider.urlHint) || "").toLowerCase();
  const out = [];
  if (!hint) return out;
  let tabs = [];
  try {
    tabs = await chrome.tabs.query({});
  } catch (_) {
    return out;
  }
  for (const t of tabs || []) {
    if (typeof t.id !== "number" || t.id <= 0) continue;
    const u = String(t.url || "").toLowerCase();
    if (!u || u.indexOf(hint) < 0) continue;
    if (u.startsWith("edge://") || u.startsWith("chrome://") || u.startsWith("devtools://")
        || u.startsWith("chrome-extension://")) {
      continue;
    }
    out.push({ tabId: t.id, title: t.title || "", url: t.url || "",
               windowId: typeof t.windowId === "number" ? t.windowId : -1,
               status: t.status || "" });
  }
  return out;
}

/** 打开一个**属于我们**的标签页；浏览器一个窗口都没有时，先建一个**不抢焦点**的窗口。
 *
 *  ⚠⚠ 为什么必须抽成公共函数（2026-09-26 真机）：
 *    `EnsureTab` 与 `PickProvider` **都要开标签页**，我分开写了两份 ⇒
 *    只给其中一份加了"没有窗口时建窗口"的兜底 ⇒ 另一份仍然抛异常 ⇒
 *    **探针能成功（走 EnsureTab）、正常提问却失败（走 PickProvider）** ——
 *    用户看到的症状是 `NO_WEB_AI_LOGIN`（因为逐个站点探测全失败）。
 *    ⇒ **同一件事只留一处实现**，别写第二份。
 *
 *  @return {ok, tabId, error?, message?}
 */
async function webAiOpenOwnTab(provider, ownKey, active) {
  const baseUrl = String(provider.url || "").trim();
  let tabId = 0;
  try {
    const t = await chrome.tabs.create({ url: baseUrl, active: active === true });
    tabId = Number(t && t.id) || 0;
  } catch (e) {
    const em = String((e && e.message) || e);
    // ★ 浏览器**一个窗口都没有**时 `tabs.create` 会抛异常（用户"没打开浏览器"，
    //   进程在后台跑、扩展 service worker 还活着）⇒ 退一步建窗口再试。
    //   ⚠ `focused: false` ⇒ **不抢用户的前台**。
    try {
      const w = await chrome.windows.create({ url: baseUrl, focused: false });
      const wt = (w && w.tabs) ? w.tabs : [];
      tabId = Number((wt[0] && wt[0].id) || 0);
    } catch (e2) {
      return {
        ok: false,
        tabId: 0,
        error: "OPEN_FAILED",
        message: "新建标签页失败：" + em + "；建窗口重试也失败："
                 + String((e2 && e2.message) || e2),
      };
    }
  }
  if (!tabId) {
    return { ok: false, tabId: 0, error: "OPEN_FAILED", message: "tabs.create 未返回 id" };
  }
  if (ownKey) await saveOwnTabId(ownKey, tabId);
  return { ok: true, tabId: tabId };
}

/** 等页面**真的**渲染出可用的输入框（不是只等 `status === "complete"`）。
 *
 *  ⚠⚠ 为什么必须等（2026-09-26 真机）：
 *    `waitTabComplete` 只等**文档加载完**（`status === "complete"`），
 *    但豆包这类 **SPA 的输入框是 JS 挂载的** ⇒ complete 时输入框**还不存在**
 *    ⇒ 紧接着的 `webAiRichType` 报 `NO_COMPOSER: 没找到可见的输入框`。
 *
 *    实测对照（同一天）：
 *      · 探针：`EnsureTab` 新建标签页 → 等 2.4s → 写文字 ⇒ **成功**（回读 5 字）
 *      · 正常提问：`NewChat` 导航 → **立刻**写文字 ⇒ **NO_COMPOSER**
 *    ⇒ 差别就是"有没有给页面渲染时间"。
 *
 *  @return 输入框可用返回 `"ok"`；否则返回最后一次的原因（供日志/诊断）
 */
async function webAiWaitComposer(tabId, cfg, timeoutMs) {
  const deadline = Date.now() + (timeoutMs || 15000);
  let last = "";
  while (Date.now() < deadline) {
    try {
      const r = await injectPageFn(tabId, qstFocusComposer, [cfg || {}]);
      if (r && !r.error && r.result && r.result.ok) return "ok";
      last = r && r.error ? String(r.error) : "NO_COMPOSER";
    } catch (e) {
      last = String((e && e.message) || e);
    }
    await new Promise((r) => setTimeout(r, 300));
  }
  return last || "NO_COMPOSER";
}

/** 让页面**以为**自己是可见的（尽量**不抢前台**也能渲染）。
 *
 *  ⚠ 为什么需要：用户明确抱怨"占用了界面前台，使用此功能时用户无法正常使用浏览器和电脑"
 *    （2026-09-26）。前台化确实能让站点渲染，但代价是**抢走用户的屏幕**。
 *
 *  ⚠⚠ **它只能骗过"读 `document.visibilityState` 决定要不要渲染"的站点**；
 *    浏览器层面的渲染节流是内部行为，改不了 ⇒ 真读不到回答时**才**回退到前台化
 *    （由宿主的 `activateTabDuringRun` 控制，默认关）。
 */
async function webAiInstallVisibilityOverride(debuggee) {
  const src = [
    "(() => { try {",
    "  Object.defineProperty(document, 'visibilityState',",
    "    { get: () => 'visible', configurable: true });",
    "  Object.defineProperty(document, 'hidden',",
    "    { get: () => false, configurable: true });",
    "  document.addEventListener('visibilitychange',",
    "    (e) => e.stopImmediatePropagation(), true);",
    "} catch (e) {} })();",
  ].join("\n");
  const notes = [];
  // ① 之后每次导航都自动装
  try {
    await chrome.debugger.sendCommand(debuggee, "Page.addScriptToEvaluateOnNewDocument",
      { source: src });
  } catch (e) {
    notes.push("newdoc:" + String((e && e.message) || e));
  }
  // ② 当前这一页也立刻装上
  try {
    await chrome.debugger.sendCommand(debuggee, "Runtime.evaluate", { expression: src });
  } catch (e) {
    notes.push("now:" + String((e && e.message) || e));
  }
  return notes;
}

/** 让**后台标签页**尽可能保持"活着"（不冻结、不省电节流）。
 *
 *  为什么必须做：后台标签页里 `document.visibilityState === 'hidden'`，
 *  Chrome 会冻结/节流渲染 ⇒ 站点的流式回答**不往 DOM 里写**
 *  ⇒ 我们等半天读不到回复（用户实测：「必须我亲手手点网页前台…软件才能抓到对方的回复」）。
 *
 *  ⚠ 这两条**只能缓解**，不能把 `visibilityState` 变成 `visible`
 *    ⇒ 真读不到时还要靠 `webAiActivateTab` 临时前台化（见下）。
 */
async function keepTargetAlive(debuggee) {
  const notes = [];
  try {
    await chrome.debugger.sendCommand(debuggee, "Emulation.setFocusEmulationEnabled", {
      enabled: true,
    });
  } catch (e) {
    notes.push("focus:" + String((e && e.message) || e));
  }
  try {
    await chrome.debugger.sendCommand(debuggee, "Page.setWebLifecycleState", {
      state: "active",
    });
  } catch (e) {
    notes.push("lifecycle:" + String((e && e.message) || e));
  }
  // ★ 再尽量**不抢前台**地让站点肯渲染（见 webAiInstallVisibilityOverride 的说明）
  const vnotes = await webAiInstallVisibilityOverride(debuggee);
  for (const n of vnotes) notes.push("vis:" + n);
  return notes;
}

/** 列出该站点当前打开的页面（宿主的 `webAiFindPage` 用） */
async function handleWebAiFindPage(msg) {
  const provider = await resolveWebAiProvider(msg.provider || msg.urlHint || "");
  if (!provider) {
    const all = await loadWebAiProviders();
    return {
      ok: false,
      error: "NO_PROVIDER",
      message: String(msg.provider || ""),
      knownProviders: Object.keys(all || {}),
      version: BRIDGE_VERSION,
    };
  }
  const pages = await findProviderTabs(provider);
  return { ok: true, provider: provider.id, count: pages.length, pages, version: BRIDGE_VERSION };
}

/** 拿到**属于本软件**的站点标签页（没有就新建），并把网页 AI 的调试会话挂上去。
 *
 *  @param msg.active 新建时是否切到前台（默认 true：第一次用往往还要登录，后台开用户会看不到）
 *  @return {ok, tabId, created, url, attached, attachError}
 */
async function handleWebAiEnsureTab(msg) {
  const provider = await resolveWebAiProvider(msg.provider || msg.urlHint || "");
  if (!provider) {
    const all = await loadWebAiProviders();
    return {
      ok: false,
      error: "NO_PROVIDER",
      message: String(msg.provider || ""),
      knownProviders: Object.keys(all || {}),
      version: BRIDGE_VERSION,
    };
  }
  const baseUrl = String(provider.url || "").trim();
  if (!provider.urlHint || !/^https?:\/\//i.test(baseUrl)) {
    return { ok: false, error: "BAD_PROVIDER",
             message: "站点配置缺 url/urlHint", version: BRIDGE_VERSION };
  }

  const ownKey = webAiOwnKey(provider, msg);
  const map = await loadOwnTabMap();
  const known = Number(map[ownKey]) || 0;
  let tabId = 0;
  let created = false;
  let url = "";

  // ① 我们那个还在、且还在这个站点上？⇒ 就是它
  //   ⚠ 必须同时校验 URL：用户可能把那个标签页导航去别处了，tabId 还在但不能用。
  if (known > 0) {
    try {
      const t = await chrome.tabs.get(known);
      // ⚠⚠ 用 `pendingUrl` 兜底（2026-09-26 真机：用户看到**两个豆包标签页**）：
      //   `chrome.tabs.create({url})` 返回后、页面还没导航完时，`t.url` 是 `about:blank`
      //   （目标 URL 在 `t.pendingUrl` 里）⇒ 只比对 `t.url` 会判"这个标签页不是该站点的"
      //   ⇒ **又开一个** ⇒ 每次提问多一个标签页。
      const u = String((t && (t.url || t.pendingUrl)) || "").toLowerCase();
      const hint = String(provider.urlHint || "").toLowerCase();
      if (u && hint && u.indexOf(hint) >= 0) {
        tabId = known;
        url = t.url;
      } else if (!u || u === "about:blank" || u === "edge://newtab/"
                 || (t && t.status && t.status !== "complete")) {
        // ★ 我们**刚创建、还没导航到站点**的标签页 ⇒ 照样复用（否则必然重复开）
        tabId = known;
        url = t.url || baseUrl;
      }
    } catch (_) {
      /* 已关闭 ⇒ 走新建 */
    }
  }

  // ② 没有 ⇒ 新建一个属于我们的（走公共函数，兜底逻辑只有一份）
  if (!tabId) {
    const opened = await webAiOpenOwnTab(provider, ownKey, msg.active);
    if (!opened.ok) {
      return { ok: false, error: opened.error, message: opened.message,
               version: BRIDGE_VERSION };
    }
    tabId = opened.tabId;
    created = true;
    url = baseUrl;
  }

  // ②b 新建的标签页**必须等它加载完**再往下走
  //   ⚠ `waitTabComplete` 返回的是 **boolean**（见它自己的实现），不是状态字符串。
  let ready = true;
  if (created) {
    ready = await waitTabComplete(tabId, 25000);
  }

  // ③ 挂上**我们自己的**调试会话（不碰脚本回放那条全局的）+ 保活
  let attached = false;
  let attachError = "";
  let aliveNotes = [];
  try {
    if (!webAiDebuggee || webAiDebuggee.tabId !== tabId) {
      try { await chrome.debugger.detach(webAiDebuggee); } catch (_) { /* 没挂过 */ }
      await chrome.debugger.attach({ tabId }, "1.3");
      webAiDebuggee = { tabId };
    }
    attached = true;
    aliveNotes = await keepTargetAlive(webAiDebuggee);
  } catch (e) {
    attachError = String((e && e.message) || e);
    webAiDebuggee = null;
  }

  // ④ 页面可见性诊断
  //   ⚠⚠ 注意：我们注入过 `visibilityState` 覆盖（`webAiInstallVisibilityOverride`），
  //     所以这里的值是**被模拟过的**，不能当"真实可见性"用 —— 只作参考。
  let pageState = null;
  try {
    const v = await injectPageFn(tabId, function () {
      return {
        visibility: String(document.visibilityState || ""),
        hidden: !!document.hidden,
        hasFocus: !!document.hasFocus(),
      };
    }, []);
    pageState = (v && v.result) || null;
  } catch (_) {
    /* 诊断失败不影响主流程 */
  }

  // ★ 双保险：返回前确认输入框真的可用（新建给足时间，复用只等一会儿）
  const composerReady = await webAiWaitComposer(
    tabId, { inputSelectors: provider.inputSelectors || [] }, created ? 15000 : 4000);

  // ⚠ 回执是**跨进程协议** ⇒ 每个字段都显式定型（`String`/`Number`/`!!`）
  return {
    ok: true,
    provider: String(provider.id || ""),
    ownKey: ownKey,
    composer: composerReady,
    tabId: Number(tabId) || 0,
    created: !!created,
    url: String(url || ""),
    status: created ? (ready ? "complete" : "timeout") : undefined,
    attached: !!attached,
    attachError: attachError ? String(attachError) : undefined,
    aliveNotes: aliveNotes.length ? aliveNotes.map((x) => String(x)) : undefined,
    pageState: pageState || undefined,
    version: BRIDGE_VERSION,
  };
}

/** 开新对话：把我们自己的标签页导航到站点入口，并等**输入框渲染出来**。
 *
 *  ⚠⚠ 2026-09-26 事故：上一版编辑时**替换范围吃掉了这个函数头**，
 *    导致它的函数体被并进 `handleWebAiEnsureTab`，而 `target` 在那边**未定义**
 *    ⇒ 走到 `chrome.tabs.update(tabId, { url: target })` 就 `ReferenceError`。
 *    ⚠ `node --check` **完全通过**（两个函数拼成一个，语法照样合法）——
 *    这是第三类"语法检查抓不到"的事故，靠 `tools/verify/ext_lint.py` 才发现。
 */
async function handleWebAiNewChat(msg) {
  const provider = await resolveWebAiProvider(msg.provider || msg.urlHint || "");
  if (!provider) {
    return { ok: false, error: "NO_PROVIDER", message: String(msg.provider || ""),
             version: BRIDGE_VERSION };
  }
  const target = String(msg.newChatUrl || provider.newChatUrl || provider.url || "").trim();
  if (!/^https?:\/\//i.test(target)) {
    return { ok: false, error: "BAD_URL", message: "新对话 URL 非法", version: BRIDGE_VERSION };
  }
  let tabId = Number(msg.tabId) || 0;
  if (!tabId) {
    const map = await loadOwnTabMap();
    tabId = Number(map[webAiOwnKey(provider, msg)]) || 0;
  }
  if (!tabId) {
    return { ok: false, error: "NO_TAB",
             message: "还没有属于本软件的标签页（先调 webAiEnsureTab）", version: BRIDGE_VERSION };
  }
  try {
    await chrome.tabs.update(tabId, { url: target });
  } catch (e) {
    return { ok: false, error: "NAV_FAIL", message: String((e && e.message) || e),
             version: BRIDGE_VERSION };
  }

  // 等页面进入 complete（最多 25s）。⚠ 不等的话后面的 attach/insertText 会打在半截页面上。
  // ⚠ 返回值是 **boolean**（见 `waitTabComplete` 实现）—— 回执里转成字符串，
  //   别把 boolean 直接塞进 `status`（宿主按字符串读，会抛类型异常 ⇒ 闪退）。
  const ready = await waitTabComplete(tabId, 25000);
  // ★★ `complete` **不等于**"输入框已经渲染出来"（SPA 的输入框是 JS 挂载的）
  //   ⇒ 这里必须再等输入框真的可用，否则紧接着的写文字会报 NO_COMPOSER。
  const composer = await webAiWaitComposer(tabId, {
    inputSelectors: provider.inputSelectors || [],
  }, 15000);
  return {
    ok: true,
    tabId: Number(tabId) || 0,
    url: String(target || ""),
    status: ready ? "complete" : "timeout",
    composer: composer,
    version: BRIDGE_VERSION,
  };
}

/** ★ 自动挑一个能用的站点（宿主的 `web` 模型走这条）。
 *
 *  规则（用户 2026-09-25 指定）：
 *    ① **已经开着**的站点优先 —— 你开着豆包就用豆包，开着元宝就用元宝；
 *    ② 都没开 ⇒ 按内置顺序**逐个打开探测**，第一个"能拿到输入框"的（≈ 已登录）就用它；
 *       探测失败的标签页**由我们关掉**（不给你留一堆登录页）；
 *    ③ 全都不行 ⇒ 报 `NO_WEB_AI_LOGIN`，宿主在界面上提示"至少登录一个网页 AI"。
 *
 *  ⚠ 「已登录」的判据就是**页面上能找到可见的输入框** —— 不用去猜各家 cookie 名，
 *    而且这正是我们后续真正要用的东西（找不到输入框本来也用不了）。
 */
async function handleWebAiPickProvider(msg) {
  const all = await loadWebAiProviders();
  const ids = Object.keys(all || {});
  if (!ids.length) {
    return { ok: false, error: "NO_PROVIDER", message: "扩展没读到 web_ai_providers.json",
             version: BRIDGE_VERSION };
  }

  // ① 已经开着页面？⇒ 用第一个（按 JSON 里的顺序，豆包在前）
  const opened = [];
  for (const id of ids) {
    const p = { id, ...(all[id] || {}) };
    const tabs = await findProviderTabs(p);
    if (tabs.length) {
      opened.push({ provider: id, tabId: tabs[0].tabId, url: tabs[0].url, reason: "open" });
    }
  }
  if (opened.length) {
    return { ok: true, picked: opened[0], candidates: opened, reason: "open",
             version: BRIDGE_VERSION };
  }

  // ② ★ 上次成功的站点 ⇒ **直接用它，不探测**（这一步把 80 秒变成 0 秒）
  try {
    const got = await chrome.storage.local.get(WEB_AI_LAST_OK_KEY);
    const lastOk = String((got && got[WEB_AI_LAST_OK_KEY]) || "");
    if (lastOk && all[lastOk]) {
      return {
        ok: true,
        picked: { provider: lastOk, tabId: 0, url: String((all[lastOk] || {}).url || ""),
                  reason: "lastOk" },
        reason: "lastOk",
        version: BRIDGE_VERSION,
      };
    }
  } catch (_) { /* 读不到就当没有 */ }

  // ③ 都没有 ⇒ 逐个打开探测（**只在第一次**；失败的标签页立刻关掉）
  //   ⚠ 每个只等 8 秒（不是 20）—— 4 个站点最多 32 秒，而不是 80 秒。
  const tried = [];
  for (const id of ids) {
    const p = { id, ...(all[id] || {}) };
    if (!p.urlHint || !/^https?:\/\//i.test(String(p.url || ""))) {
      tried.push({ provider: id, ok: false, why: "BAD_PROVIDER" });
      continue;
    }
    // ★★ 先看这个会话是不是**已经有我们的标签页**了（哪怕它还在加载）
    //   —— 不先看就会重复开（用户看到两个豆包标签页，2026-09-26 真机）。
    {
      const key0 = webAiOwnKey(p, msg);
      const m0 = await loadOwnTabMap();
      const k0 = Number(m0[key0]) || 0;
      if (k0 > 0) {
        try {
          const t0 = await chrome.tabs.get(k0);
          const u0 = String((t0 && (t0.url || t0.pendingUrl)) || "").toLowerCase();
          const hint0 = String(p.urlHint || "").toLowerCase();
          if (u0.indexOf(hint0) >= 0 || !u0 || u0 === "about:blank"
              || (t0 && t0.status && t0.status !== "complete")) {
            const w0 = await webAiWaitComposer(k0, { inputSelectors: p.inputSelectors || [] }, 8000);
            if (w0 === "ok") {
              try { await chrome.storage.local.set({ [WEB_AI_LAST_OK_KEY]: id }); } catch (_) {}
              tried.push({ provider: id, ok: true, tabId: k0, url: p.url, why: "own-tab" });
              return { ok: true,
                       picked: { provider: id, tabId: k0, url: p.url, reason: "own-tab" },
                       tried, version: BRIDGE_VERSION };
            }
          }
        } catch (_) { /* 已关闭 ⇒ 走新建 */ }
      }
    }

    // ★ 走公共函数（浏览器没有窗口时会自动建一个不抢焦点的窗口）——
    //   ⚠ 这里原本自己写了一份 `tabs.create`，**漏了那个兜底** ⇒ 没有窗口时全站失败。
    // ⚠ 变量名用 `openRes`：上面已有一个 `opened`（"已经开着的站点"数组），
    //   重名会遮蔽，以后改代码时极易看错。
    const openRes = await webAiOpenOwnTab(p, webAiOwnKey(p, msg), msg.active);
    if (!openRes.ok) {
      tried.push({ provider: id, ok: false, why: openRes.error + ": " + (openRes.message || "") });
      continue;
    }
    let tabId = openRes.tabId;
    try {
      await waitTabComplete(tabId, 25000);

      // 能不能找到可见的输入框 = 能不能用（≈ 已登录）
      const cfg = { inputSelectors: p.inputSelectors || [] };
      // ★ 等输入框**真的渲染出来**（`waitTabComplete` 只等文档加载完，
      //   SPA 的输入框是 JS 挂载的 ⇒ 立刻探测必然失败 ⇒ 全站判"没登录"）。
      //   ⚠ 判据已放宽到"不可见也算"（见 `qstFocusComposer`），所以后台标签页也能探测到。
      const waitRes = await webAiWaitComposer(tabId, cfg, 8000);
      const found = (waitRes === "ok");
      // ⚠⚠ 这里**只能**用 `waitRes`。2026-09-26 真机事故：我上一版把上面那行换成
      //   `webAiWaitComposer` 后**漏改了这一行**，`r` 已经不存在 ⇒ `ReferenceError`
      //   ⇒ 每个站点的探测都失败 ⇒ 用户看到 `NO_WEB_AI_LOGIN`
      //   （而探针走的是 `EnsureTab`，不受影响 ⇒ 表现为"探针能成功、提问不行"）。
      tried.push({ provider: id, ok: found, tabId, url: p.url,
                   why: found ? "login" : String(waitRes || "NO_COMPOSER") });
      if (found) {
        // ★ 记下"上次成功的站点" ⇒ 之后直接用它，不再探测
        try { await chrome.storage.local.set({ [WEB_AI_LAST_OK_KEY]: id }); } catch (_) {}
        // 标签页已在 `webAiOpenOwnTab` 里登记成"我们的"（按会话 key）
        return { ok: true, picked: { provider: id, tabId, url: p.url, reason: "login" },
                 tried, version: BRIDGE_VERSION };
      }
    } catch (e) {
      tried.push({ provider: id, ok: false, why: String((e && e.message) || e) });
    }
    // 探测失败 ⇒ 关掉，不留垃圾（这是我们自己开的）
    if (tabId) {
      try { await chrome.tabs.remove(tabId); } catch (_) { /* ignore */ }
      await saveOwnTabId(webAiOwnKey(p, msg), 0);
    }
  }

  return {
    ok: false,
    error: "NO_WEB_AI_LOGIN",
    message: "没找到可用的网页 AI：豆包 / DeepSeek / 元宝 都没有已登录的页面",
    tried,
    providers: ids,
    version: BRIDGE_VERSION,
  };
}

/** 临时把我们的标签页切到前台（读回答需要它真的渲染）。
 *
 *  为什么必须：后台标签页 `visibilityState === 'hidden'` ⇒ 站点的流式回答不往 DOM 写
 *  ⇒ 读不到回复（用户实测：「必须我亲手手点网页前台…软件才能抓到对方的回复」）。
 *  `keepTargetAlive` 只能缓解，不能把 hidden 变成 visible。
 *
 *  @return {ok, prevTabId, prevWindowId} —— 宿主读完要调 `webAiRestoreTab` 还回去。
 */
async function handleWebAiActivateTab(msg) {
  let tabId = Number(msg.tabId) || 0;
  if (!tabId && webAiDebuggee) tabId = Number(webAiDebuggee.tabId) || 0;
  if (!tabId) {
    const provider = await resolveWebAiProvider(msg.provider || "");
    if (provider) {
      const map = await loadOwnTabMap();
      tabId = Number(map[provider.id]) || 0;
    }
  }
  if (!tabId) return { ok: false, error: "NO_TAB", version: BRIDGE_VERSION };

  let prevTabId = 0;
  let prevWindowId = -1;
  try {
    const t = await chrome.tabs.get(tabId);
    prevWindowId = typeof t.windowId === "number" ? t.windowId : -1;
    const act = await chrome.tabs.query({ active: true, windowId: prevWindowId });
    if (act && act.length && typeof act[0].id === "number") prevTabId = act[0].id;
  } catch (_) { /* ignore */ }

  try {
    if (prevTabId === tabId) {
      return { ok: true, alreadyActive: true, tabId, prevTabId, prevWindowId,
               version: BRIDGE_VERSION };
    }
    await chrome.tabs.update(tabId, { active: true });
    if (prevWindowId >= 0) {
      try { await chrome.windows.update(prevWindowId, { focused: true }); } catch (_) {}
    }
    return { ok: true, alreadyActive: false, tabId, prevTabId, prevWindowId,
             version: BRIDGE_VERSION };
  } catch (e) {
    return { ok: false, error: "ACTIVATE_FAILED", message: String((e && e.message) || e),
             version: BRIDGE_VERSION };
  }
}

/** 把前台还给用户原来那个标签页（只在"我们那个还是活动标签页"时还，避免抢回用户已切走的）。 */
async function handleWebAiRestoreTab(msg) {
  const prevTabId = Number(msg.prevTabId) || 0;
  const ourTabId = Number(msg.tabId) || 0;
  if (!prevTabId || prevTabId === ourTabId) {
    return { ok: true, skipped: true, version: BRIDGE_VERSION };
  }
  try {
    if (ourTabId) {
      const cur = await chrome.tabs.query({ active: true });
      if (cur && cur.length && Number(cur[0].id) !== ourTabId) {
        // 用户自己已经切走了 ⇒ 别抢回来
        return { ok: true, skipped: "user-moved", version: BRIDGE_VERSION };
      }
    }
    await chrome.tabs.update(prevTabId, { active: true });
    return { ok: true, restored: prevTabId, version: BRIDGE_VERSION };
  } catch (e) {
    return { ok: false, error: "RESTORE_FAILED", message: String((e && e.message) || e),
             version: BRIDGE_VERSION };
  }
}

// ── 传图（网页版 AI 的图片通道）─────────────────────────────────────
// 宿主把请求里的 data URL 落成临时文件，这里用 CDP 挂到页面的 <input type=file>。
//
// ⚠⚠ 为什么不能用 JS 直接塞：`input.files` 是**只读** FileList；用 DataTransfer
//   造个 File 塞进去，页面大多不认（那不是"用户选择"的结果，且部分框架在 change
//   里校验事件来源）。CDP 的 `DOM.setFileInputFiles` 走浏览器进程的**真实文件选择
//   管线**，派发的事件与真人选文件完全一致 —— 这是唯一可靠的路子。
//
// 两条路（先 A 后 B，A 覆盖绝大多数站点）：
//   A. 页面上**已经存在** <input type=file>（含 open shadow root）⇒ 直接挂。
//   B. 找不到 ⇒ 说明输入框是「点附件按钮时才动态创建」的 ⇒
//      用 `Page.setInterceptFileChooserDialog` 拦住文件选择器 → 点一下附件按钮 →
//      从 `Page.fileChooserOpened` 事件里取 backendNodeId → 再挂。
//      （富文本编辑器站点常见 B 形态：input 用完即删）
//
// ⚠ 回读校验是**必须**的：`DOM.setFileInputFiles` 不报错 ≠ 页面认了。
//   有些站点在 change 里校验类型/大小并**静默清空** input ⇒ 必须读 `files.length`。

/** 自包含：数一数页面上**用户可见的图片缩略图**（`blob:` / `data:` 的 img）。
 *
 *  ⚠⚠ 为什么这是**传图是否成功**的正确判据：
 *    站点（豆包等）的附件流程是「读走 `files[0]` → 上传/本地预览 → **主动清空 input**」
 *    —— 清空是为了让用户能再选同一个文件。所以挂载后 `input.files.length === 0`
 *    **完全可能是成功的**（2026-09-26 真机踩到：我们据此判失败，其实图已经挂上了）。
 *    而"图真的挂上了"会有一个**与类名无关**的硬信号：
 *    页面里出现了新的 `blob:` / `data:` 图片元素（缩略图）。
 *    ⇒ 用它当判据，比"input 里还有没有文件"可靠得多。
 */
function qstCountThumbs() {
  let n = 0;
  try {
    const imgs = document.querySelectorAll("img");
    for (const im of imgs) {
      const src = String(im.getAttribute("src") || im.src || "");
      if (src.startsWith("blob:") || src.startsWith("data:image")) n++;
    }
  } catch (_) {}
  return n;
}

/** 自包含：列出页面上**所有** file input 的概要（诊断 + 择优）。
 *
 *  ⚠ 为什么必须"全列"：2026-09-25 真机 `UPLOAD_NOT_ACCEPTED`（挂上了但页面把 files 清空）
 *    有两种完全不同的可能 —— ① 我们**选错了** input（页面上有多个）；
 *    ② 站点**读走就清空**（很多站点读完 files[0] 后主动清空，让用户能再选同一个文件，
 *    此时 `files.length===0` 其实是**成功**）。不把全部 input 列出来就分不清。
 */
function qstListFileInputs(cfg) {
  const sels = [].concat((cfg && cfg.fileInputSelectors) || [], ["input[type='file']"]);
  const seen = [];
  const walk = (root, depth) => {
    for (const s of sels) {
      let list = [];
      try { list = Array.from(root.querySelectorAll(s)); } catch (_) { continue; }
      for (const el of list) if (seen.indexOf(el) < 0) seen.push(el);
    }
    if (depth > 0) {
      let nodes = [];
      try { nodes = root.querySelectorAll("*"); } catch (_) { return; }
      for (const n of nodes) if (n.shadowRoot) walk(n.shadowRoot, depth - 1);
    }
  };
  walk(document, 3);
  const out = [];
  for (let i = 0; i < seen.length; i++) {
    const el = seen[i];
    let w = 0, h = 0;
    try { const r = el.getBoundingClientRect(); w = Math.round(r.width); h = Math.round(r.height); } catch (_) {}
    let fc = -1;
    try { fc = el.files ? el.files.length : -1; } catch (_) {}
    out.push({
      idx: i,
      accept: String(el.getAttribute("accept") || ""),
      multiple: !!el.multiple,
      disabled: !!el.disabled,
      visible: (w > 0 && h > 0),
      w: w, h: h,
      name: String(el.getAttribute("name") || ""),
      id: String(el.id || ""),
      cls: String(el.className || "").slice(0, 60),
      fileCount: fc,
    });
  }
  return { count: out.length, inputs: out };
}

/** 自包含：按 `idx` 取第 idx 个 file input（**返回元素本身**，供 CDP 拿 objectId）。
 *
 *  排序规则：`accept` 含 image 的优先 → 不限类型的次之 → 可见的优先 → 离输入框近的优先。
 *  ⚠ 排序只是"先试哪个"，**不是判据** —— 真正的判据是挂上之后页面认不认（回读）。
 */
function qstPickFileInput(cfg, idx) {
  const sels = [].concat((cfg && cfg.fileInputSelectors) || [], ["input[type='file']"]);
  const seen = [];
  const walk = (root, depth) => {
    for (const s of sels) {
      let list = [];
      try { list = Array.from(root.querySelectorAll(s)); } catch (_) { continue; }
      for (const el of list) if (seen.indexOf(el) < 0) seen.push(el);
    }
    if (depth > 0) {
      let nodes = [];
      try { nodes = root.querySelectorAll("*"); } catch (_) { return; }
      for (const n of nodes) if (n.shadowRoot) walk(n.shadowRoot, depth - 1);
    }
  };
  walk(document, 3);

  // 参考点：输入框（附件 input 通常在它附近）
  let anchor = null;
  for (const s of ((cfg && cfg.inputSelectors) || [])) {
    try { const el = document.querySelector(s); if (el) { anchor = el; break; } } catch (_) {}
  }
  let ar = null;
  try { ar = anchor ? anchor.getBoundingClientRect() : null; } catch (_) {}

  const score = (el) => {
    let v = 0;
    const acc = String(el.getAttribute("accept") || "").toLowerCase();
    if (acc.indexOf("image") >= 0) v += 100;
    else if (!acc) v += 10;
    if (el.multiple) v += 5;
    if (el.disabled) v -= 200;
    try {
      const r = el.getBoundingClientRect();
      if (r.width > 0 && r.height > 0) v += 3;
      if (ar) {
        const dy = Math.abs((r.top + r.height / 2) - (ar.top + ar.height / 2));
        v += Math.max(0, 60 - Math.round(dy / 10));   // 越近越高
      }
    } catch (_) {}
    return v;
  };
  const ranked = seen.slice().sort((a, b) => score(b) - score(a));
  const el = ranked[idx];
  return el || null;
}

/** 自包含：找 file input（先按配置的 selector，再全文档兜底；含 open shadow root） */
function qstFindFileInput(cfg) {
  const sels = [].concat((cfg && cfg.fileInputSelectors) || [], ["input[type='file']"]);
  const search = (root, depth) => {
    for (const s of sels) {
      if (!s) continue;
      try {
        const el = root.querySelector(s);
        if (el) return el;
      } catch (_) {
        /* 非法 selector 跳过 */
      }
    }
    if (depth > 0) {
      let all = [];
      try {
        all = root.querySelectorAll("*");
      } catch (_) {
        return null;
      }
      for (const n of all) {
        const sr = n.shadowRoot;
        if (sr) {
          const hit = search(sr, depth - 1);
          if (hit) return hit;
        }
      }
    }
    return null;
  };
  return search(document, 3);
}

/** 自包含：读回 file input 的状态（诊断用） */
function qstReadFileInputState(cfg) {
  const el = qstFindFileInput(cfg);
  if (!el) {
    let n = 0;
    try {
      n = document.querySelectorAll("input[type='file']").length;
    } catch (_) {
      /* ignore */
    }
    return { ok: false, reason: "NO_INPUT", inputCount: n };
  }
  const files = [];
  try {
    for (let i = 0; i < el.files.length; i++) {
      files.push({ name: el.files[i].name, size: el.files[i].size });
    }
  } catch (_) {
    /* ignore */
  }
  return {
    ok: files.length > 0,
    count: files.length,
    files,
    accept: el.getAttribute("accept") || "",
    multiple: !!el.multiple,
    disabled: !!el.disabled,
  };
}

/** 自包含：点一下站点的「附件 / 上传」按钮（为 B 路触发文件选择器） */
function qstClickFileButton(cfg) {
  const sels = (cfg && cfg.fileButtonSelectors) || [];
  const hit = [];
  for (const s of sels) {
    if (!s) continue;
    let el = null;
    try {
      el = document.querySelector(s);
    } catch (_) {
      hit.push(s + " (bad selector)");
      continue;
    }
    if (!el) {
      hit.push(s + " (miss)");
      continue;
    }
    try {
      el.click();
      return { ok: true, matchedBy: s, tried: hit };
    } catch (e) {
      hit.push(s + " (click: " + String((e && e.message) || e) + ")");
    }
  }
  return { ok: false, tried: hit };
}

async function handleWebAiUploadImages(msg) {
  const provider = await resolveWebAiProvider(msg.provider || msg.urlHint || "");
  if (!provider) {
    const all = await loadWebAiProviders();
    return {
      ok: false,
      error: "NO_PROVIDER",
      message: String(msg.provider || ""),
      knownProviders: Object.keys(all || {}),
      version: BRIDGE_VERSION,
    };
  }
  const tabId = webAiTargetTabId(msg);
  if (!tabId) {
    return { ok: false, error: "NO_TAB", message: "未 attach 任何标签页", version: BRIDGE_VERSION };
  }

  const files = (Array.isArray(msg.files) ? msg.files : [])
    .map((s) => String(s || "").trim())
    .filter(Boolean);
  if (!files.length) {
    return { ok: false, error: "NO_FILES", message: "没有给出任何本地图片路径", version: BRIDGE_VERSION };
  }

  const cfg = {
    inputSelectors: provider.inputSelectors || [],
    fileInputSelectors: provider.fileInputSelectors || [],
    fileButtonSelectors: provider.fileButtonSelectors || [],
  };
  const debuggee = webAiCdpTarget(msg);
  const out = {
    ok: false,
    provider: String(provider.id || ""),
    fileCount: files.length,
    steps: {},
    version: BRIDGE_VERSION,
  };

  const evalValue = async (expr) => {
    const r = await chrome.debugger.sendCommand(debuggee, "Runtime.evaluate", {
      expression: expr,
      returnByValue: true,
    });
    return r && r.result ? r.result.value : undefined;
  };
  const readBack = async () => {
    try {
      return await evalValue(
        "(" + qstReadFileInputState.toString() + ")(" + JSON.stringify(cfg) + ")");
    } catch (e) {
      return { ok: false, reason: "inject:" + String((e && e.message) || e) };
    }
  };

  // ── 诊断：页面上到底有几个 file input + 挂载前的缩略图数 ──────────────
  //   ⚠ 这两条是"选错 input"与"站点读走就清空"唯一的分辨手段，必须有。
  try {
    out.steps.listBefore = await evalValue(
      "(" + qstListFileInputs.toString() + ")(" + JSON.stringify(cfg) + ")");
  } catch (e) {
    out.steps.listError = String((e && e.message) || e);
  }
  const countThumbs = async () => {
    try {
      return await evalValue("(" + qstCountThumbs.toString() + ")()");
    } catch (_) {
      return -1;
    }
  };
  const thumbBefore = await countThumbs();
  out.steps.thumbsBefore = thumbBefore;

  // ── 路 A：**逐个**候选 input 尝试（不是只试第一个）──────────────────
  //   前科（2026-09-26 真机 `UPLOAD_NOT_ACCEPTED`）：只试第一个 + 只在最后回读一次
  //   ⇒ 选错 input 就直接报失败，而且分不清"选错了"和"页面读走就清空"。
  const total = (out.steps.listBefore && out.steps.listBefore.count) || 0;
  const tried = [];
  for (let i = 0; i < Math.min(total, 3); i++) {
    let objId = "";
    try {
      const r = await chrome.debugger.sendCommand(debuggee, "Runtime.evaluate", {
        expression:
          "(" + qstPickFileInput.toString() + ")(" + JSON.stringify(cfg) + "," + i + ")",
        returnByValue: false,
      });
      objId = (r && r.result && r.result.objectId) || "";
    } catch (e) {
      tried.push({ idx: i, error: String((e && e.message) || e) });
      continue;
    }
    if (!objId) {
      tried.push({ idx: i, error: "no-objectId" });
      continue;
    }
    try {
      await chrome.debugger.sendCommand(debuggee, "DOM.setFileInputFiles", { files, objectId: objId });
    } catch (e) {
      tried.push({ idx: i, error: String((e && e.message) || e) });
      continue;
    }
    const st = await readBack();
    const thumbNow = await countThumbs();
    // ★ 成功判据（任一成立）：
    //   ① input 里还留着文件（少数站点）；
    //   ② **页面上出现了新的缩略图**（`blob:`/`data:` img 变多）——
    //      这是"站点读走文件并渲染了预览"的硬信号，与类名无关。
    const okNow = !!(st && st.ok) || (thumbBefore >= 0 && thumbNow > thumbBefore);
    tried.push({ idx: i, after: st ? st.count : -1, thumbs: thumbNow, ok: okNow });
    if (okNow) {
      out.ok = true;
      out.via = (st && st.ok) ? ("direct#" + i) : ("direct#" + i + "+thumb");
      out.verifiedCount = (st && st.count) || 0;
      out.steps.tried = tried;
      out.steps.thumbsAfter = thumbNow;
      out.steps.listAfter = await evalValue(
        "(" + qstListFileInputs.toString() + ")(" + JSON.stringify(cfg) + ")");
      return out;
    }
  }
  out.steps.tried = tried;

  // ── 路 B：拦截文件选择器 + 点附件按钮（input 是"点按钮才动态创建"时走这条）──
  let backendNodeId = 0;
  const onEv = (source, method, params) => {
    if (method === "Page.fileChooserOpened" && params && !backendNodeId) {
      backendNodeId = Number(params.backendNodeId) || 0;
    }
  };
  try {
    try { chrome.debugger.onEvent.addListener(onEv); } catch (_) { /* ignore */ }
    await chrome.debugger.sendCommand(debuggee, "Page.setInterceptFileChooserDialog", { enabled: true });
    out.steps.fileButton = (await evalValue(
      "(" + qstClickFileButton.toString() + ")(" + JSON.stringify(cfg) + ")")) || null;
    await new Promise((r) => setTimeout(r, 3000));
    out.steps.chooser = { backendNodeId: backendNodeId, got: backendNodeId > 0 };
    if (backendNodeId) {
      await chrome.debugger.sendCommand(debuggee, "DOM.setFileInputFiles", {
        files,
        backendNodeId: backendNodeId,
      });
      const st = await readBack();
      const thumbNow = await countThumbs();
      out.steps.afterChooser = st;
      out.steps.thumbsAfter = thumbNow;
      if ((st && st.ok) || (thumbBefore >= 0 && thumbNow > thumbBefore)) {
        out.ok = true;
        out.via = (st && st.ok) ? "fileChooser" : "fileChooser+thumb";
        out.verifiedCount = (st && st.count) || 0;
        return out;
      }
    }
  } catch (e) {
    out.steps.chooserError = String((e && e.message) || e);
  } finally {
    try { chrome.debugger.onEvent.removeListener(onEv); } catch (_) { /* ignore */ }
    try {
      await chrome.debugger.sendCommand(debuggee, "Page.setInterceptFileChooserDialog", { enabled: false });
    } catch (_) { /* 老版本 Chrome 没这个方法；不影响主流程 */ }
  }

  out.steps.listAfter = await evalValue(
    "(" + qstListFileInputs.toString() + ")(" + JSON.stringify(cfg) + ")");

  // ── 挂上了但被清空：**可能是"读走就清空"（成功）**，也可能是真丢弃 ──
  //   ⚠ 不猜：如实报 `ACCEPTED_THEN_CLEARED` + 全部诊断，由宿主决定怎么说。
  out.error = "ACCEPTED_THEN_CLEARED";
  out.message =
    "CDP 已把文件挂到 file input 上，但既没回读到 files，也没看到新的缩略图。" +
    "可能是选错了 input、或站点把文件丢弃了。详见 steps 诊断" +
    "（`listBefore/listAfter` 是页面上所有 file input 的概要，`tried` 是逐个尝试的结果）。";
  return out;
}

/// 打开（或复用）目标站点页面。
///
/// 用途：用户在「设置 → AI助手」里选了网页版 AI 模型后，**不需要自己去开页面** ——
/// 宿主的第一次请求发现页面不在，就让扩展把它开出来。
///
/// ⚠ 只在「一个都没开」时才真的 `tabs.create`：已经开着就复用（不重复开、
///   不动用户当前那个标签页的滚动位置）。判据用 provider 的 `urlHint`（域名片段），
///   而不是完整 URL —— 站点会带各种 path/query（`/chat/xxx?conversation=...`）。
async function handleWebAiOpenPage(msg) {
  const provider = await resolveWebAiProvider(msg.provider || msg.urlHint || "");
  if (!provider) {
    const all = await loadWebAiProviders();
    return {
      ok: false,
      error: "NO_PROVIDER",
      message: String(msg.provider || ""),
      knownProviders: Object.keys(all || {}),
      version: BRIDGE_VERSION,
    };
  }
  const url = String(msg.url || provider.url || "").trim();
  if (!/^https?:\/\//i.test(url)) {
    return { ok: false, error: "BAD_URL", message: "url 须为 http(s)", version: BRIDGE_VERSION };
  }

  // ① 已经开着？直接复用（**不**新建、**不**导航 —— 用户可能正在那个页面上）
  //   ⚠ 用 `findProviderTabs`（穷举、不截断），不要用 `listPages`（游戏打分 + 每窗留一 + 前 8）
  const found = await findProviderTabs(provider);
  if (found.length) {
    return {
      ok: true,
      reused: true,
      tabId: found[0].tabId,
      url: found[0].url,
      version: BRIDGE_VERSION,
    };
  }

  // ② 新建标签页。
  //    ⚠⚠ `active` 默认 **false**（2026-09-26 改）：原来写的是 `msg.active !== false`
  //      ⇒ `undefined !== false` 为 **true** ⇒ **默认就抢前台**，
  //      用户反复抱怨"占用了界面前台，无法正常使用浏览器和电脑"。
  //      ⇒ 想让它前台化必须**显式**传 `active: true`。
  try {
    const active = msg.active === true;
    const created = await chrome.tabs.create({ url, active });
    return {
      ok: true,
      reused: false,
      tabId: created && created.id,
      url,
      version: BRIDGE_VERSION,
    };
  } catch (e) {
    return {
      ok: false,
      error: "OPEN_FAILED",
      message: String((e && e.message) || e),
      version: BRIDGE_VERSION,
    };
  }
}

async function injectPageSnapshot(tabId, opts) {
  const o = opts && typeof opts === "object" ? opts : {};
  // ★★★ 节点上限**默认放开**（2026-10-03 用户报"看题只看半边"）
  //
  //   ⚠⚠ 原来是**硬编码 64** ⇒ 一屏 4~6 题的选择题页面**根本装不下**
  //     （每题 = 题干 + 4 个选项 ⇒ 5 题就要 ~25 条，加上导航/按钮轻松超 64）
  //     ⇒ **屏幕外的选项不在快照里** ⇒ 模型只在可见选项里选 ⇒ **必然答错** ✓
  //   ⇒ 默认改成 600，并**允许调用方传 `maxNodes` 覆盖** ✓
  //   ⚠ 上限仍保留（防超大页面撑爆提示词）：600 ≈ "一屏题量 × 20 倍"的安全值 ✓
  //   ⚠ 通用性：**不要**针对"练习题"写死，任何长列表页（题/表格/消息流）都受益 ✓
  const askMax = Number(o.maxNodes || 0);
  const maxNodes = askMax > 0 ? Math.max(1, Math.min(4000, Math.floor(askMax))) : 600;
  const inj = await chrome.scripting.executeScript({
    target: { tabId, frameIds: [0] },
    func: qstBuildPageSnapshot,
    args: [{
      light: !!o.light,
      maxNodes,
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
      capabilities: ["vision", "mouse", "keys", "layout", "pageSnapshot", "typeRef", "navigatePage", "readAssistantReply", "webAiProbe", "webAiRichType", "webAiSubmit", "webAiUploadImages", "webAiOpenPage", "webAiFindPage", "webAiEnsureTab", "webAiNewChat", "webAiPickProvider", "webAiActivateTab", "webAiRestoreTab"],
    });
    setStatus("ready", `已握手 v${BRIDGE_VERSION} vision`);
    // ★★★ SW 重启 / 掉线重连后**自动恢复调试会话**（2026-10-02 真机事故）
    //
    //   ⚠⚠ 根因：`attachMeta` / `attachedDebuggee` 是**模块级变量** ——
    //     扩展 Service Worker 一重启（掉线重连、空闲回收）就**全丢**，
    //     于是 `clickRef` / `typeRef` 等**所有需要会话的 handler** 都报
    //     `NO_TAB: 尚未 observePage / attach`（真机：收割循环跑到第 5 题突然失败）✓
    //   ⇒ 握手成功后**自动重新 attach** 到「我们那个标签页」——
    //     它在 `chrome.storage` 里**是持久化的**（`qstWebAiOwnTab`），SW 重启不会丢 ✓
    //   ⚠ 标签页可能已被用户关掉 ⇒ 先 `chrome.tabs.get` 确认存在再 attach。
    //   ⚠ 失败**不致命**（下次 observePage 会重新 attach）⇒ 静默 catch。
    void (async () => {
      try {
        const st = await chrome.storage.local.get([WEB_AI_OWN_TAB_KEY]);
        const tid = Number(st && st[WEB_AI_OWN_TAB_KEY]) || 0;
        if (!tid) return;
        let alive = false;
        try { await chrome.tabs.get(tid); alive = true; } catch (_) { alive = false; }
        if (!alive) return;
        await attachTab("", { tabId: tid });
        setStatus("ready", `已握手 v${BRIDGE_VERSION}（已恢复会话 tab=${tid}）`);
      } catch (_) { /* 恢复失败不致命 */ }
    })();
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
  // ── 网页版 AI 适配层（见 docs/web-ai-backend-design.md）──
  if (type === "webAiProviders") {
    reply(msg, await handleWebAiProviders());
    return;
  }
  if (type === "webAiProbe") {
    reply(msg, await handleWebAiProbe(msg));
    return;
  }
  if (type === "readAssistantReply") {
    reply(msg, await handleWebAiReadReply(msg));
    return;
  }
  if (type === "webAiRichType") {
    reply(msg, await handleWebAiRichType(msg));
    return;
  }
  if (type === "webAiSubmit") {
    reply(msg, await handleWebAiSubmit(msg));
    return;
  }
  if (type === "webAiUploadImages") {
    reply(msg, await handleWebAiUploadImages(msg));
    return;
  }
  if (type === "webAiOpenPage") {
    reply(msg, await handleWebAiOpenPage(msg));
    return;
  }
  if (type === "webAiFindPage") {
    reply(msg, await handleWebAiFindPage(msg));
    return;
  }
  if (type === "webAiEnsureTab") {
    reply(msg, await handleWebAiEnsureTab(msg));
    return;
  }
  if (type === "webAiNewChat") {
    reply(msg, await handleWebAiNewChat(msg));
    return;
  }
  if (type === "webAiPickProvider") {
    reply(msg, await handleWebAiPickProvider(msg));
    return;
  }
  if (type === "webAiActivateTab") {
    reply(msg, await handleWebAiActivateTab(msg));
    return;
  }
  if (type === "webAiRestoreTab") {
    reply(msg, await handleWebAiRestoreTab(msg));
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
  // ① 宿主每次启动都会重写 bridge_runtime.json；侧载扩展直接读它 —— 零进程开销。
  //    （host 侧 WriteUtf8AclFile 的 DACL 含 (A;;FR;;;AU)，普通用户令牌可读。）
  try {
    const res = await fetch(chrome.runtime.getURL("bridge_runtime.json"), {
      cache: "no-store",
    });
    if (res.ok) add(await res.json());
  } catch (_) {
    /* packed CRX has no host-written json */
  }
  // ② 文件拿不到，或**文件里那个 token 已经被桥拒过**才问 native host，且必须节流。
  //
  //   ⚠⚠ 这里刻意**不**用 `nativeProbeWanted`（"上一轮没连上"）：宿主没在跑时每一轮都
  //     连不上，用它就变成每 1.5s `connectNative` 拉一个完整宿主进程
  //     （见文件顶部那条注释警告的现象）。陈旧判据要盯**被拒的那个 token**：
  //     宿主每次启动都换 token ⇒ 「同一 token 又失败一次」才是可靠信号。
  const fileTok = out.length ? out[0].token : "";
  const fileLooksStale = !!fileTok && fileTok === lastRejectedToken;
  if (out.length === 0 || fileLooksStale) {
    const now = Date.now();
    if (now - lastNativeProbeAt >= NATIVE_PROBE_MIN_MS) {
      lastNativeProbeAt = now;
      try {
        add(await loadBridgeRuntimeFromNative());
      } catch (_) {
        /* native host is optional for unpacked */
      }
    }
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
      // ★★ 自报浏览器（`userAgent`）：宿主据此记住"**上次连过桥的浏览器**"，
      //   下次浏览器没开时优先把它拉起来（一键式）。
      //   ⚠ 扩展**拿不到自己的 exe 路径**（浏览器安全限制）⇒ 只能报 UA，由宿主解析品牌串。
      socket.send(JSON.stringify({
        id: 0,
        type: "hello",
        token,
        browser: String((typeof navigator !== "undefined" && navigator.userAgent) || ""),
      }));
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
      nativeProbeWanted = true;  // 没有任何候选 -> 下一轮允许问 native
      setStatus("disconnected", "等待宿主写入桥凭证");
      return;
    }
    setStatus("connecting", `v${BRIDGE_VERSION} 直连本机桥`);
    for (const runtime of runtimes) {
      if (await connectWebSocket(runtime.port, runtime.token)) {
        nativeProbeWanted = false;
        lastRejectedToken = "";
        // ★ 记下**这一次连上用的**凭证：`/qst/shot` 等取图路由要拿它拼 URL
        //   （`bridgeToken` 原本只在 onopen 里赋值，重连后若走别的分支就会留着旧值）。
        bridgePort = runtime.port;
        bridgeToken = runtime.token;
        return;
      }
    }
    // 全部连不上：把**文件里那个** token 记为"已被拒" ⇒ 下一轮才允许问 native
    // （判据见 `loadAllBridgeRuntimes`：盯被拒的 token，不盯"上一轮失败"）。
    lastRejectedToken = runtimes[0].token;
    nativeProbeWanted = true;
    setStatus("waiting", "未发现键鼠工坊桥（请先运行键鼠工坊）");
  } finally {
    discoverBusy = false;
  }
}

// ★★ 启动痕迹（2026-09-26）：扩展"离线"时**完全看不出卡在哪一步** ——
//   下面这对 log 让 `edge://extensions/` 的 Service Worker 控制台直接告诉你答案：
//     · 只看到「启动」没看到「注册完成」 ⇒ **中间某一步抛异常**（后面的注册全没生效）
//     · 两条都在、却仍连不上桥 ⇒ 问题在**连接**（端口/token/浏览器没开），不在加载
console.log("[qst] SW 启动 v" + BRIDGE_VERSION);

chrome.runtime.onInstalled.addListener(() => {  setStatus("installed", `v${BRIDGE_VERSION} 已安装`);
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
    // ★ 用户显式按了「重新连接」⇒ 解除两个节流/记忆：否则只是把**同一个陈旧 token**
    //   再撞一次（`loadAllBridgeRuntimes` 会判它"还没被拒过"而不问 native），
    //   用户会觉得这个按钮没用 —— 而它正是扩展离线时唯一的自助出口。
    lastNativeProbeAt = 0;
    lastRejectedToken = "";
    nativeProbeWanted = true;
    closeWs();
    tryDiscoverAndConnect().then(() => sendResponse(lastStatus));
    return true;
  }
  return false;
});

console.log("[qst] SW 注册完成（消息/定时/调试器监听都已就绪）");

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
