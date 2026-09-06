/**
 * ui/pro-mode.js — 专业模式（设计稿样式主界面）
 * 与极简模式完全隔离：不触碰极简模式的选中/运行状态；
 * 文件夹 = 真实目录（scripts/、recordings/、library/sched|ai）；
 * 条目数据来自 AppShell.state，极简模式读同一批文件故不丢数据。
 */
(function () {
  "use strict";
  var A = window.AppShell;
  var S = {
    mode: "simple",
    dirLists: { macro: [], rec: [], sched: [], ai: [] }, // 相对路径列表（/ 分隔）
    pinned: [],             // 脚本 path
    view: { kind: "all" },  // all | fav | folder(kind:path)
    selected: null,         // {kind:"item", id} | {kind:"view", id}
    tabSel: {},             // kind -> folderPath|null
    expanded: {},           // key -> true
    query: "",
    filter: "all",
    clipboard: null,        // {mode,kind,id} id=folder path or item id
    built: false,
    activePage: "library",
    pendingRefresh: false,
  };

  var KINDS = ["macro", "rec", "sched", "ai"];
  var TYPE = {
    macro: { label: "宏", page: "macro", icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M6 3h12v5H6zM6 11h8v10H6z"/></svg>' },
    rec: { label: "录制", page: "recorder", icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="4"/><path d="M8 3h8M6 6h12"/></svg>' },
    sched: { label: "定时", page: "sched", icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/></svg>' },
    ai: { label: "AI 对话", page: "ai", icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 18l4-8 3 4 3-6 4 10"/></svg>' },
  };
  var PAGE_KIND = { recorder: "rec", macro: "macro", sched: "sched", ai: "ai" };
  var FOLDER_SVG = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 6h6l2 2h10v11H3z"/></svg>';
  var GRID_SVG = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M4 4h6v6H4zM14 4h6v6h-6zM4 14h6v6H4zM14 14h6v6h-6z"/></svg>';
  var STAR_SVG = '<svg viewBox="0 0 24 24" fill="currentColor" stroke="none"><path d="M12 2l2.9 6.3 6.9.8-5.1 4.7 1.4 6.8L12 17.2 5.9 20.6l1.4-6.8L2.2 9.1l6.9-.8z"/></svg>';

  /* ── 持久化（仅模式/收藏；文件夹树来自真实目录） ── */
  function loadPersist() {
    try {
      S.mode = localStorage.getItem("qst.uiMode") || "simple";
      var p = localStorage.getItem("qst.proPinned");
      S.pinned = p ? JSON.parse(p) : [];
    } catch (e) {
      S.mode = "simple"; S.pinned = [];
    }
  }
  function savePersist() {
    try {
      localStorage.setItem("qst.uiMode", S.mode);
      localStorage.setItem("qst.proPinned", JSON.stringify(S.pinned));
    } catch (e) {}
  }
  var _folderFetchGen = 0;
  var _folderFetchPending = 0;
  function refreshDirLists() {
    if (!window.qst || !qst.listLibraryFolders) return;
    _folderFetchGen++;
    var gen = _folderFetchGen;
    _folderFetchPending = KINDS.length;
    KINDS.forEach(function (k) { qst.listLibraryFolders(k); });
    // 超时兜底：防止某 kind 无回调时永不刷新
    clearTimeout(refreshDirLists._t);
    refreshDirLists._t = setTimeout(function () {
      if (gen === _folderFetchGen && _folderFetchPending > 0) {
        _folderFetchPending = 0;
        onDataChanged();
      }
    }, 400);
  }

  /* ── 数据源（来自极简模式共享的引擎状态；folder 为真实/逻辑相对路径） ── */
  function st() { return A.state; }
  function normFolder(f) {
    return String(f || "").replace(/\\/g, "/").replace(/^\/+|\/+$/g, "");
  }
  function listOf(kind) {
    var out = [];
    if (kind === "macro") {
      (st().macros || []).forEach(function (m) {
        var id = A.itemPath(m);
        out.push({ id: id, name: String(m.name || "").replace(/\.json$/i, ""), type: "macro", folder: normFolder(m.folder), hotkey: m.hotkey || "", meta: (m.actionCount != null ? m.actionCount + " 动作" : "") + (m.recordTime ? " · " + m.recordTime : ""), ref: m });
      });
    } else if (kind === "rec") {
      (st().recordings || []).forEach(function (r) {
        var id = A.itemPath(r);
        out.push({ id: id, name: String(r.name || "").replace(/\.json$/i, ""), type: "rec", folder: normFolder(r.folder), hotkey: r.hotkey || "", meta: (r.actionCount != null ? r.actionCount + " 动作" : "") + (r.recordTime ? " · " + r.recordTime : ""), ref: r });
      });
    } else if (kind === "sched") {
      (st().schedTasks || []).forEach(function (t) {
        out.push({ id: String(t.id || ""), name: t.name || "定时任务", type: "sched", folder: normFolder(t.folder), hotkey: "", meta: t.timeLabel || t.frequencyLabel || "", ref: t });
      });
    } else if (kind === "ai") {
      (st().chats || []).forEach(function (c) {
        var id = String(c.id || c.path || c.name || "");
        out.push({ id: id, name: c.name || "对话", type: "ai", folder: normFolder(c.folder), hotkey: "", meta: c.meta || "", ref: c });
      });
    }
    return out;
  }
  function allItems() {
    var out = [];
    KINDS.forEach(function (k) { out = out.concat(listOf(k)); });
    return out;
  }
  var _itemIndex = {}; // kind -> { id: item }
  var _folderCache = {}; // kind -> { roots, stamp }
  var _previewCache = {}; // id -> { names, actionCount, stamp }
  var _previewWait = {}; // reqId -> itemId
  var _previewGen = 0;
  function listStamp(kind) {
    var n = 0;
    if (kind === "macro") n = (st().macros || []).length;
    else if (kind === "rec") n = (st().recordings || []).length;
    else if (kind === "sched") n = (st().schedTasks || []).length;
    else if (kind === "ai") n = (st().chats || []).length;
    return n + "|" + ((S.dirLists[kind] || []).join("\0"));
  }
  function invalidateCaches() {
    _itemIndex = {};
    _folderCache = {};
  }
  function findItem(kind, id) {
    if (!_itemIndex[kind]) {
      var map = {};
      listOf(kind).forEach(function (x) { map[x.id] = x; });
      _itemIndex[kind] = map;
    }
    return _itemIndex[kind][id] || null;
  }
  /** 库芯片筛选仅作用于「脚本库」页；宏/录制等目录页不受影响 */
  function libTypeFilter() {
    if (S.activePage !== "library") return "all";
    return S.filter || "all";
  }
  function matchesLibType(item) {
    var f = libTypeFilter();
    return f === "all" || item.type === f;
  }
  function matchesQuery(item) {
    var q = (S.activePage === "library" ? S.query : "").trim().toLowerCase();
    if (!q) return true;
    return item.name.toLowerCase().indexOf(q) >= 0 ||
      (item.hotkey && item.hotkey.toLowerCase().indexOf(q) >= 0) ||
      TYPE[item.type].label.toLowerCase().indexOf(q) >= 0;
  }
  function visible(item) {
    return matchesLibType(item) && matchesQuery(item);
  }
  function globalHotkeyShort() {
    var t = "";
    if (st()) t = String(st().hotkey || "").replace(/（长按）$/, "").trim();
    if (!t || t === "无") return "";
    return t;
  }
  function itemHotkeyLabel(it) {
    if (it && it.hotkey) return String(it.hotkey);
    var g = globalHotkeyShort();
    return g ? ("通用热键：" + g) : "未设置";
  }
  /** 与 count 同源：按 folderList 挂上的 id 取条目，避免 path 比较不一致 */
  function itemsOfFolderNode(kind, f) {
    var out = [];
    (f.items || []).forEach(function (id) {
      var it = findItem(kind, id);
      if (it && visible(it)) out.push(it);
    });
    return out;
  }
  function isPinned(id) { return S.pinned.indexOf(id) >= 0; }
  function togglePin(id) {
    var i = S.pinned.indexOf(id);
    if (i >= 0) S.pinned.splice(i, 1); else S.pinned.push(id);
    savePersist();
  }

  /* ── 文件夹工具（由真实目录路径构建树；id = 相对路径） ── */
  function folderList(kind) {
    var stamp = listStamp(kind);
    var hit = _folderCache[kind];
    if (hit && hit.stamp === stamp) return hit.roots;
    var paths = (S.dirLists[kind] || []).slice();
    listOf(kind).forEach(function (it) {
      if (it.folder && paths.indexOf(it.folder) < 0) paths.push(it.folder);
      // 补齐祖先路径
      if (it.folder) {
        var parts = it.folder.split("/");
        var acc = "";
        for (var i = 0; i < parts.length; i++) {
          acc = acc ? acc + "/" + parts[i] : parts[i];
          if (paths.indexOf(acc) < 0) paths.push(acc);
        }
      }
    });
    paths.sort();
    var map = {};
    var roots = [];
    paths.forEach(function (p) {
      var name = p.indexOf("/") >= 0 ? p.slice(p.lastIndexOf("/") + 1) : p;
      var node = { id: p, name: name, items: [], subs: [] };
      map[p] = node;
      var slash = p.lastIndexOf("/");
      if (slash < 0) roots.push(node);
      else {
        var parent = p.slice(0, slash);
        if (map[parent]) map[parent].subs.push(node);
        else roots.push(node);
      }
    });
    listOf(kind).forEach(function (it) {
      if (!it.folder) return;
      var n = map[it.folder];
      if (n) n.items.push(it.id);
    });
    _folderCache[kind] = { stamp: stamp, roots: roots, map: map };
    return roots;
  }
  function findFolder(kind, id) {
    folderList(kind); // ensure cache
    var map = (_folderCache[kind] && _folderCache[kind].map) || {};
    return map[id] || null;
  }
  function folderChain(kind, id) {
    if (!id) return [];
    var parts = String(id).split("/");
    var chain = [];
    var acc = "";
    for (var i = 0; i < parts.length; i++) {
      acc = acc ? acc + "/" + parts[i] : parts[i];
      var f = findFolder(kind, acc);
      if (f) chain.push(f);
      else chain.push({ id: acc, name: parts[i], items: [], subs: [] });
    }
    return chain;
  }
  function countFolder(kind, f) {
    var n = itemsOfFolderNode(kind, f).length;
    (f.subs || []).forEach(function (s) { n += countFolder(kind, s); });
    return n;
  }
  function folderHasVisible(kind, f) {
    return countFolder(kind, f) > 0;
  }
  function rootItems(kind) {
    return listOf(kind).filter(function (x) { return !x.folder && visible(x); });
  }
  function folderItems(kind, folder) {
    if (folder && typeof folder === "object" && Array.isArray(folder.items)) {
      return itemsOfFolderNode(kind, folder);
    }
    var path = typeof folder === "string" ? folder : (folder && folder.id);
    return listOf(kind).filter(function (x) { return x.folder === path && visible(x); });
  }
  function itemFolderId(kind, id) {
    var it = findItem(kind, id);
    return it && it.folder ? it.folder : null;
  }
  function moveItemToFolder(kind, id, folderPath) {
    folderPath = normFolder(folderPath);
    if (kind === "macro" || kind === "rec") {
      if (window.qst && qst.moveScriptToFolder) qst.moveScriptToFolder(id, folderPath);
    } else if (window.qst && qst.setItemLibraryFolder) {
      qst.setItemLibraryFolder(kind, id, folderPath);
    }
  }
  function newFolder(kind, parentPath, name) {
    name = String(name || "新建文件夹").trim();
    if (!name) return null;
    var path = parentPath ? (normFolder(parentPath) + "/" + name) : name;
    if (window.qst && qst.createLibraryFolder) qst.createLibraryFolder(kind, path);
    S.expanded["f:" + kind + ":" + path] = true;
    if (parentPath) S.expanded["f:" + kind + ":" + parentPath] = true;
    var list = S.dirLists[kind] || (S.dirLists[kind] = []);
    if (list.indexOf(path) < 0) list.push(path);
    return { id: path, name: name, items: [], subs: [] };
  }
  function deleteFolder(kind, id) {
    if (window.qst && qst.deleteLibraryFolder) qst.deleteLibraryFolder(kind, id);
    KINDS.forEach(function (k) { if (S.tabSel[k] === id) S.tabSel[k] = null; });
  }

  /* ── DOM 构建 ── */
  function build() {
    if (S.built) return;
    S.built = true;
    var root = A.$("#proHome");
    if (!root) return;
    var segs = [
      ["library", "脚本库", GRID_SVG],
      ["clicker", "连点", '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="9"/><path d="M8 12h8M12 8v8"/></svg>'],
      ["recorder", "录制", TYPE.rec.icon],
      ["macro", "宏", TYPE.macro.icon],
      ["sched", "定时", TYPE.sched.icon],
      ["ai", "脚本定制", TYPE.ai.icon],
      ["settings", "设置", '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="3"/><path d="M19 12a7 7 0 0 0-.1-1.2l2-1.6-2-3.4-2.4 1a7 7 0 0 0-2-1.2L14 3h-4l-.4 2.6a7 7 0 0 0-2 1.2l-2.4-1-2 3.4 2 1.6A7 7 0 0 0 5 12c0 .4 0 .8.1 1.2l-2 1.6 2 3.4 2.4-1a7 7 0 0 0 2 1.2L10 21h4l.4-2.6a7 7 0 0 0 2-1.2l2.4 1 2-3.4-2-1.6c.1-.4.1-.8.1-1.2z"/></svg>'],
    ];
    var top = '<div class="p-top">' + segs.map(function (s, i) {
      return '<div class="p-seg' + (i === 0 ? " on" : "") + '" data-page="' + s[0] + '">' + s[2] + s[1] + '</div>';
    }).join("") + '</div>';

    var lib =
      '<div class="p-page on" data-page="library"><div class="p-lib">' +
        '<div class="p-panel"><div class="p-head">' +
          '<div class="p-title">' + GRID_SVG + '脚本库<span class="p-cnt" id="proLibCnt">0 项</span></div>' +
          '<div class="p-search">' + '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="11" cy="11" r="7"/><path d="M21 21l-4.3-4.3"/></svg>' +
            '<input id="proSearch" type="text" placeholder="搜索名称或热键" autocomplete="off" /><kbd>Ctrl K</kbd></div>' +
          '<div class="p-chips" id="proChips">' +
            '<span class="p-chip on" data-f="all">全部</span><span class="p-chip" data-f="macro">宏</span><span class="p-chip" data-f="rec">录制</span>' +
          '</div>' +
        '</div><div class="p-tree" id="proLibTree"></div></div>' +
        '<div class="p-panel"><div class="p-crumbs" id="proCrumbs"></div><div class="p-detail" id="proDetail"></div></div>' +
      '</div></div>';

    // 连点：内嵌极简连点视图（参数可点）；录制/宏/脚本定制：文件树 + 极简底栏 CTA
    function dirPage(page, kind, ctaHostId, fallbackCta) {
      var cta = ctaHostId
        ? '<div class="pro-cta-host" id="' + ctaHostId + '"></div>'
        : '<div class="p-cta" data-act="' + fallbackCta.act + '"><div class="shine" aria-hidden="true"></div><div class="p-cta-main"><h2>' + fallbackCta.title + '</h2><p>' + fallbackCta.sub + '</p></div><div class="cta-side"><div class="cta-key">' + fallbackCta.key + '</div></div></div>';
      return '<div class="p-page col pro-dir" data-page="' + page + '"><div class="p-panel" style="flex:1">' +
        '<div class="p-crumbs" id="proCrumbs-' + kind + '"></div><div class="p-tree" id="proTree-' + kind + '"></div>' +
      '</div>' + cta + '</div>';
    }
    var pages =
      lib +
      '<div class="p-page col pro-dir pro-clicker" data-page="clicker">' +
        '<div class="pro-embed-host" id="proEmbedClicker"></div>' +
        '<div class="pro-cta-host" id="proCtaHost-clicker"></div>' +
      '</div>' +
      dirPage("recorder", "rec", "proCtaHost-rec", null) +
      dirPage("macro", "macro", "proCtaHost-macro", null) +
      dirPage("sched", "sched", null, { act: "schedNew", title: "新建定时任务", sub: "选择脚本并设置执行时间", key: "+" }) +
      dirPage("ai", "ai", "proCtaHost-ai", null) +
      '<div class="p-page col pro-settings" data-page="settings"><div class="pro-settings-host" id="proSettingsHost"></div></div>';
    root.innerHTML = top + pages + '<div class="p-foot"><a id="proFootHk">修改全局启停热键</a></div>';

    // 事件：顶部 Tab
    A.$$("#proHome .p-seg").forEach(function (seg) {
      seg.addEventListener("click", function () {
        switchPage(seg.dataset.page);
      });
    });
    // 搜索 / 筛选（搜索防抖，减轻卡顿）
    var searchTimer = 0;
    A.$("#proSearch").addEventListener("input", function () {
      S.query = A.$("#proSearch").value;
      clearTimeout(searchTimer);
      searchTimer = setTimeout(function () { renderLibTree(); }, 120);
    });
    A.$("#proChips").addEventListener("click", function (e) {
      var chip = e.target.closest(".p-chip");
      if (!chip) return;
      A.$$("#proChips .p-chip").forEach(function (c) { c.classList.remove("on"); });
      chip.classList.add("on");
      S.filter = chip.dataset.f || "all";
      // 筛选与当前浏览的文件夹类型不一致时，退出该文件夹，避免「角标有数、详情 0 项」
      if (S.view.kind === "folder" && S.filter !== "all") {
        var kv = folderKeyParts(S.view.id);
        if (kv.kind && kv.kind !== S.filter) {
          S.view = { kind: "all" };
          S.selected = null;
        }
      }
      renderLibTree();
      renderCrumbs();
      renderDetail();
    });
    // 库树
    A.$("#proLibTree").addEventListener("click", onLibTreeClick);
    A.$("#proLibTree").addEventListener("dblclick", onLibTreeDblClick);
    // 目录页树（宏 / 录制 / 定时 / AI）
    ["macro", "rec", "sched", "ai"].forEach(function (k) {
      var el = A.$("#proTree-" + k);
      if (!el) return;
      el.addEventListener("click", function (e) { onDirTreeClick(e, k); });
      el.addEventListener("dblclick", function (e) { onDirTreeDblClick(e, k); });
      initDrag(el, k);
    });
    initDrag(A.$("#proLibTree"), "lib");
    // 详情/内容区
    A.$("#proDetail").addEventListener("click", onDetailClick);
    A.$("#proDetail").addEventListener("dblclick", onDetailDblClick);
    // CTA（仅定时任务设计稿底栏）
    A.$$("#proHome .p-cta").forEach(function (cta) {
      cta.addEventListener("click", function () { onCta(cta.dataset.act); });
    });
    var footHk = A.$("#proFootHk");
    if (footHk) footHk.addEventListener("click", function () {
      if (typeof A.openGlobalHotkeyCapture === "function") A.openGlobalHotkeyCapture();
      else A.openOv("hotkey");
    });
    A.$$("#proHome .p-crumbs").forEach(function (bar) {
      bar.addEventListener("click", onCrumbClick);
    });
    // 真实目录列表回调
    window.addEventListener("qst:bridge", onProBridge);
    // 全局右键
    document.addEventListener("contextmenu", onCtxMenu, true);
    document.addEventListener("click", function (e) {
      if (!e.target.closest("#proCtx")) hideCtx();
    });
    document.addEventListener("keydown", function (e) {
      if (e.key === "Escape") hideCtx();
      if ((e.ctrlKey || e.metaKey) && !e.altKey && (e.key === "k" || e.key === "K")) {
        if (S.mode !== "pro") return;
        if (document.querySelector(".overlay.show")) return;
        var inp = A.$("#proSearch");
        if (!inp) return;
        e.preventDefault();
        inp.focus();
        inp.select();
      }
    });
    A.$("#proCtx").addEventListener("click", function (e) {
      var it = e.target.closest(".ci");
      if (!it) return;
      var payload = A.$("#proCtx")._payload || {};
      hideCtx();
      doAction(it.dataset.act, payload);
    });
    bindScrollHideCtx();
  }

  function bindScrollHideCtx() {
    var sels = ["#proLibTree", "#proDetail", "#proTree-macro", "#proTree-rec", "#proTree-sched", "#proTree-ai"];
    sels.forEach(function (sel) {
      var el = A.$(sel);
      if (el) el.addEventListener("scroll", hideCtx, { passive: true });
    });
    window.addEventListener("scroll", hideCtx, true);
  }

  /* ── 内嵌：连点整页 / 各页极简 CTA / 设置对话框 ── */
  var _homes = {};
  function rememberHome(el) {
    if (!el || !el.id || _homes[el.id]) return;
    _homes[el.id] = { parent: el.parentNode, next: el.nextSibling };
  }
  function reparent(el, host) {
    if (!el || !host) return;
    rememberHome(el);
    host.appendChild(el);
    if (el.classList && el.classList.contains("view")) el.classList.add("active");
  }
  function restoreEl(el) {
    if (!el || !el.id || !_homes[el.id]) return;
    var h = _homes[el.id];
    if (!h.parent) return;
    if (h.next && h.next.parentNode === h.parent) h.parent.insertBefore(el, h.next);
    else h.parent.appendChild(el);
    if (el.classList && el.classList.contains("view") && el.id !== "view-clicker") {
      el.classList.remove("active");
    }
  }
  function restoreAllEmbeds() {
    // CTA 先还原到各自 view 内，再还原 view
    ["ctaClicker", "ctaRec", "ctaMacro", "ctaAi"].forEach(function (id) {
      restoreEl(document.getElementById(id));
    });
    restoreEl(document.getElementById("view-clicker"));
    var dlg = document.querySelector("#proSettingsHost .dlg");
    var ov = document.getElementById("ov-settings");
    if (dlg && ov && dlg.parentNode !== ov) ov.appendChild(dlg);
    if (ov) {
      ov.classList.remove("pro-embedded", "show");
      ov.style.visibility = "";
    }
  }
  function syncEmbedForPage(page) {
    var need = {
      clicker: true,
      recorder: true,
      macro: true,
      ai: true,
      settings: true,
    };
    var sameEmbed = (S._embedPage === page);
    // 仅在嵌入需求变化时搬 DOM，避免每次切 Tab 全量 restore/reparent
    if (!sameEmbed) {
      var prev = S._embedPage;
      S._embedPage = page;
      var foot = A.$("#proHome .p-foot");
      if (foot) foot.hidden = page === "settings";

      var needsEmbed = !!need[page];
      var prevNeeds = !!need[prev];
      if (needsEmbed || prevNeeds) restoreAllEmbeds();

      if (page === "clicker") {
        reparent(document.getElementById("view-clicker"), A.$("#proEmbedClicker"));
        reparent(document.getElementById("ctaClicker"), A.$("#proCtaHost-clicker"));
      } else if (page === "recorder") {
        reparent(document.getElementById("ctaRec"), A.$("#proCtaHost-rec"));
      } else if (page === "macro") {
        reparent(document.getElementById("ctaMacro"), A.$("#proCtaHost-macro"));
      } else if (page === "ai") {
        reparent(document.getElementById("ctaAi"), A.$("#proCtaHost-ai"));
      } else if (page === "settings") {
        embedSettings(S._settingsStab || "other");
      }
    } else if (page === "settings") {
      // 同页再点设置：仅切 pane
      embedSettings(S._settingsStab || "other");
    }

    // 无论是否搬 DOM：每次进入功能 Tab 都必须把引擎 activeHomeTab / 选中同步到极简同款语义
    syncEngineForProPage(page);

    if (page === "sched" && window.qst && typeof qst.listScheduledTasks === "function") {
      qst.listScheduledTasks();
    }
    if (typeof A.updateCtas === "function") A.updateCtas();
  }

  /** 专业模式页 ↔ 引擎热键分流：只打 bridge，绝不走 setTab（避免搬 DOM / exitEditor / 写盘风暴） */
  function syncEngineForProPage(page) {
    if (!A.state) return;
    A.state._userPickedHomeTab = true;
    if (!window.qst) return;
    var setTabState = function (tab, eng, path) {
      A.state.tab = tab;
      if (typeof qst.setActiveHomeTab === "function") qst.setActiveHomeTab(eng);
      if (typeof qst.setHomeSelection === "function") qst.setHomeSelection(eng, path || "");
      if (typeof A.syncEngineHomeSelection === "function") {
        // 与 dedupe 键对齐，避免随后 list* 再打一次
        A.syncEngineHomeSelection._lastKey = tab + "|" + eng + "|" + (path || "");
      }
      if (typeof A.updateCtas === "function") A.updateCtas();
    };
    if (page === "clicker") {
      A.state.macroSel = -1;
      A.state.recSel = -1;
      setTabState("clicker", 0, "");
      return;
    }
    if (page === "recorder") {
      var rpath = "";
      if (S.selected && S.selected.kind === "item") {
        var rit = findItem("rec", S.selected.id) || (function () {
          var x = findItemBySel(S.selected.id);
          return x && x.type === "rec" ? x : null;
        })();
        if (rit && A.state.recordings) {
          var ri = -1;
          for (var j = 0; j < A.state.recordings.length; j++) {
            if (A.itemPath(A.state.recordings[j]) === rit.id) { ri = j; break; }
          }
          A.state.recSel = ri;
          A.state.macroSel = -1;
          if (ri >= 0) rpath = A.itemPath(A.state.recordings[ri]);
        } else {
          A.state.recSel = -1;
        }
      } else {
        A.state.recSel = -1;
      }
      setTabState("recorder", 1, rpath);
      return;
    }
    if (page === "macro") {
      var mpath = "";
      if (S.selected && S.selected.kind === "item") {
        var mit = findItem("macro", S.selected.id) || (function () {
          var x = findItemBySel(S.selected.id);
          return x && x.type === "macro" ? x : null;
        })();
        if (mit && A.state.macros) {
          var mi = -1;
          for (var i = 0; i < A.state.macros.length; i++) {
            if (A.itemPath(A.state.macros[i]) === mit.id) { mi = i; break; }
          }
          A.state.macroSel = mi;
          A.state.recSel = -1;
          if (mi >= 0) mpath = A.itemPath(A.state.macros[mi]);
        }
      }
      setTabState("macro", 2, mpath);
      return;
    }
    if (page === "ai") {
      setTabState("ai", 3, "");
      return;
    }
    // 脚本库 / 定时 / 设置
    if (page === "library" || page === "sched" || page === "settings") {
      if (S.selected && S.selected.kind === "item") {
        var lit = findItemBySel(S.selected.id);
        if (lit && lit.type === "macro") {
          S.activePage = page;
          // 复用宏分支
          var mi2 = -1;
          var macros = A.state.macros || [];
          for (var k = 0; k < macros.length; k++) {
            if (A.itemPath(macros[k]) === lit.id) { mi2 = k; break; }
          }
          A.state.macroSel = mi2;
          A.state.recSel = -1;
          setTabState("macro", 2, mi2 >= 0 ? A.itemPath(macros[mi2]) : "");
          return;
        }
        if (lit && lit.type === "rec") {
          var ri2 = -1;
          var recs = A.state.recordings || [];
          for (var n = 0; n < recs.length; n++) {
            if (A.itemPath(recs[n]) === lit.id) { ri2 = n; break; }
          }
          A.state.recSel = ri2;
          A.state.macroSel = -1;
          setTabState("recorder", 1, ri2 >= 0 ? A.itemPath(recs[ri2]) : "");
          return;
        }
      }
      A.state.macroSel = -1;
      A.state.recSel = -1;
      // 未选中脚本：全局（非专属）热键必须无反应。
      // 引擎 Recorder 空选中 = 开始录制；Clicker = 开始连点；Macro 空选中 = 无操作。
      // 专属热键仍走脚本/录制自身注册，不受此分流影响。
      setTabState("macro", 2, "");
    }
  }
  function isIdleHotkeyPage(page) {
    return page === "library" || page === "sched" || page === "settings";
  }
  function resyncEngine() {
    if (S.mode !== "pro") return;
    syncEngineForProPage(S.activePage || "library");
  }
  function embedSettings(stab) {
    var host = A.$("#proSettingsHost");
    var ov = document.getElementById("ov-settings");
    var dlg = ov && ov.querySelector(".dlg");
    if (!host || !dlg) return;
    if (!dlg.id) dlg.id = "settingsDlg";
    rememberHome(dlg);
    host.appendChild(dlg);
    ov.classList.add("pro-embedded");
    ov.classList.remove("show");
    // 脱离 .overlay.show 后仍须可见（.dlg 默认 opacity:0）
    dlg.style.opacity = "1";
    dlg.style.transform = "none";
    if (typeof A.setSettingsTab === "function") A.setSettingsTab(stab || "other");
    if (window.AppShell && typeof window.AppShell.syncUiModeRadios === "function") {
      window.AppShell.syncUiModeRadios();
    }
    if (window.qst && typeof qst.openSettingsData === "function") qst.openSettingsData();
  }
  function openSettings(stab) {
    S._settingsStab = stab || "other";
    switchPage("settings");
  }

  function syncSimpleSelection(it) {
    if (!it || !A.state) return;
    if (it.type === "macro") {
      var macros = A.state.macros || [];
      var mi = -1;
      for (var i = 0; i < macros.length; i++) {
        if (A.itemPath(macros[i]) === it.id) { mi = i; break; }
      }
      A.state.macroSel = mi;
      A.state.recSel = -1;
      A.state.tab = "macro";
      A.state._userPickedHomeTab = true;
      if (window.qst) {
        if (typeof qst.setActiveHomeTab === "function") qst.setActiveHomeTab(2);
        if (typeof qst.setHomeSelection === "function") {
          qst.setHomeSelection(2, mi >= 0 ? A.itemPath(macros[mi]) : "");
        }
      }
    } else if (it.type === "rec") {
      var recs = A.state.recordings || [];
      var ri = -1;
      for (var j = 0; j < recs.length; j++) {
        if (A.itemPath(recs[j]) === it.id) { ri = j; break; }
      }
      A.state.recSel = ri;
      A.state.macroSel = -1;
      A.state.tab = "recorder";
      A.state._userPickedHomeTab = true;
      if (window.qst) {
        if (typeof qst.setActiveHomeTab === "function") qst.setActiveHomeTab(1);
        if (typeof qst.setHomeSelection === "function") {
          qst.setHomeSelection(1, ri >= 0 ? A.itemPath(recs[ri]) : "");
        }
      }
    } else {
      return;
    }
    if (typeof A.updateCtas === "function") A.updateCtas();
  }
  /** 仅切换选中 class，避免整树 innerHTML */
  function patchTreeSelection(rootSel) {
    var roots = rootSel
      ? [A.$(rootSel)].filter(Boolean)
      : [A.$("#proLibTree"), A.$("#proTree-macro"), A.$("#proTree-rec"), A.$("#proTree-ai")].filter(Boolean);
    var selId = S.selected && S.selected.kind === "item" ? S.selected.id : "";
    roots.forEach(function (root) {
      var nodes = root.querySelectorAll(".t-node.item");
      for (var i = 0; i < nodes.length; i++) {
        var n = nodes[i];
        var id = n.getAttribute("data-item") || "";
        if (id === selId) n.classList.add("on");
        else n.classList.remove("on");
      }
      // 视图节点（常用）
      var views = root.querySelectorAll("[data-view]");
      for (var j = 0; j < views.length; j++) {
        var v = views[j];
        var on = S.selected && S.selected.kind === "view" && S.selected.id === v.getAttribute("data-view");
        v.classList.toggle("on", !!on);
      }
    });
  }

  function switchPage(page) {
    if (S.activePage === "settings" && page !== "settings") {
      if (window.AppShell && typeof window.AppShell.syncUiModeRadios === "function") {
        window.AppShell.syncUiModeRadios();
      }
    }
    if (S.activePage === page && S._embedPage === page) {
      // 同页再点：刷新数据，并强制再同步引擎 Tab（热键分流依赖）
      syncEngineForProPage(page);
      if (page === "library") { renderLibTree(); renderCrumbs(); renderDetail(); }
      else if (PAGE_KIND[page]) { renderDir(PAGE_KIND[page]); }
      if (typeof A.updateCtas === "function") A.updateCtas();
      return;
    }
    S.activePage = page;
    A.$$("#proHome .p-page").forEach(function (p) { p.classList.remove("on"); });
    A.$$("#proHome .p-seg").forEach(function (s) { s.classList.toggle("on", s.dataset.page === page); });
    var el = A.$('#proHome .p-page[data-page="' + page + '"]');
    if (el) el.classList.add("on");
    syncEmbedForPage(page);
    if (page === "library") { renderLibTree(); renderCrumbs(); renderDetail(); }
    else if (PAGE_KIND[page]) { renderDir(PAGE_KIND[page]); }
  }

  /* ── 渲染：库树 ── */
  function libFolders() {
    // 库树只展示宏 + 录制的文件夹
    var out = [];
    ["macro", "rec"].forEach(function (k) {
      folderList(k).forEach(function (f) {
        var copy = JSON.parse(JSON.stringify(f));
        copy._kind = k;
        out.push(copy);
      });
    });
    return out;
  }
  function treeNodeHtml(cls, pad, twist, ico, name, count, attrs, content) {
    return '<div class="t-node ' + cls + '" style="padding-left:' + pad + 'px"' + (attrs || "") + '>' +
      '<span class="t-twist ' + twist + '">▶</span>' + ico +
      '<span class="t-name">' + name + '</span>' + (count ? '<span class="t-count">' + count + '</span>' : "") + (content || "") +
    '</div>';
  }
  function folderNode(kind, f, depth) {
    var key = "f:" + kind + ":" + f.id;
    var open = !!S.expanded[key];
    var kids = "";
    var direct = itemsOfFolderNode(kind, f);
    var cnt = countFolder(kind, f);
    var hasKids = (f.subs || []).length > 0 || (f.items || []).length > 0;
    if (open) {
      (f.subs || []).forEach(function (s) { kids += folderNode(kind, s, depth + 1); });
      direct.forEach(function (it) { kids += itemNode(it, depth + 1); });
    }
    return treeNodeHtml("", 14 + depth * 18, open ? "open" : hasKids ? "" : "leaf",
      '<span class="t-ico folder">' + FOLDER_SVG + '</span>', escHtml(f.name), cnt, ' data-folder="' + kind + ":" + f.id + '"') + kids;
  }
  function itemNode(it, depth) {
    var on = S.selected && S.selected.kind === "item" && S.selected.id === it.id ? "on" : "";
    var cut = S.clipboard && S.clipboard.mode === "cut" && S.clipboard.id === it.id ? " cut" : "";
    var hk = it.hotkey ? '<span class="hk-pill">' + escHtml(it.hotkey) + '</span>' : "";
    return treeNodeHtml("item " + on + cut, 14 + depth * 18, "leaf", '<span class="t-ico ' + it.type + '">' + TYPE[it.type].icon + '</span>', escHtml(it.name), "", ' data-item="' + escAttr(it.id) + '" draggable="true"', hk);
  }
  function renderLibTree() {
    var el = A.$("#proLibTree");
    var html = "";
    var fav = allItems().filter(function (x) { return (x.type === "macro" || x.type === "rec") && isPinned(x.id) && visible(x); });
    var favOn = S.selected && S.selected.kind === "view" && S.selected.id === "fav";
    html += treeNodeHtml(favOn ? "on" : "", 14, S.expanded.fav ? "open" : "", '<span class="t-ico fav">' + STAR_SVG + '</span>', "常用", fav.length, ' data-view="fav"');
    if (S.expanded.fav) fav.forEach(function (it) { html += itemNode(it, 1); });
    html += '<div class="g-label">' + FOLDER_SVG + '<span>文件夹</span></div>';
    var folderHtml = "";
    var typeFilter = libTypeFilter();
    ["macro", "rec"].forEach(function (k) {
      // 宏/录制芯片：只展示该类型目录树；空文件夹（无本类型条目）不显示
      if (typeFilter !== "all" && typeFilter !== k) return;
      folderList(k).forEach(function (f) {
        if (typeFilter !== "all" && !folderHasVisible(k, f)) return;
        folderHtml += folderNode(k, f, 0);
      });
    });
    if (folderHtml) html += folderHtml;
    else if (typeFilter !== "all") html += '<div style="padding:6px 14px;color:var(--text-3);font-size:12px">无匹配文件夹</div>';
    else html = html.replace('<div class="g-label">' + FOLDER_SVG + '<span>文件夹</span></div>', '');
    var roots = [];
    ["macro", "rec"].forEach(function (k) {
      if (typeFilter !== "all" && typeFilter !== k) return;
      rootItems(k).forEach(function (it) { roots.push(it); });
    });
    if (roots.length) {
      html += '<div class="g-label">' + GRID_SVG + '<span>未分类</span></div>';
      roots.forEach(function (it) { html += itemNode(it, 0); });
    }
    el.innerHTML = html || '<div class="f-empty">右键此处新建文件夹</div>';
    A.$("#proLibCnt").textContent = allItems().filter(function (x) {
      return (x.type === "macro" || x.type === "rec") && visible(x);
    }).length + " 项";
  }

  /* ── 渲染：库右侧（内容 / 详情） ── */
  function renderCrumbs() {
    var parts = [];
    if (S.view.kind === "fav") parts = [{ id: "fav", name: "常用" }];
    else if (S.view.kind === "folder") {
      var kv = folderKeyParts(S.view.id);
      folderChain(kv.kind, kv.path).forEach(function (f) { parts.push({ id: kv.kind + ":" + f.id, name: f.name }); });
    }
    A.$("#proCrumbs").innerHTML = '<span class="p-crumb" data-jump="home">脚本库</span>' +
      parts.map(function (p) {
        return '<span class="p-sep">/</span><span class="p-crumb on" data-jump="' + p.id + '">' + escHtml(p.name) + '</span>';
      }).join("");
  }
  function fRow(it) {
    var star = (it.type === "macro" || it.type === "rec")
      ? '<span class="ra star ' + (isPinned(it.id) ? "on" : "") + '" data-act="pin" data-id="' + escAttr(it.id) + '">' + STAR_SVG + '</span>' : "";
    var setHk = (it.type === "macro" || it.type === "rec") && !it.hotkey
      ? '<span class="ra" data-act="setHk" data-id="' + escAttr(it.id) + '" title="设置热键">' + '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="7" width="18" height="10" rx="2"/><path d="M8 11v2M12 11v2M16 11v2"/></svg></span>' : "";
    var openLabel = it.type === "macro" ? "编辑" : it.type === "rec" ? "优化" : it.type === "sched" ? "编辑" : "对话";
    var on = S.selected && S.selected.kind === "item" && S.selected.id === it.id ? "on" : "";
    return '<div class="f-row ' + on + '" data-item="' + escAttr(it.id) + '">' +
      '<span class="f-ico ' + it.type + '">' + TYPE[it.type].icon + '</span>' +
      '<div class="f-main"><span class="f-name">' + escHtml(it.name) + '</span>' + (it.hotkey ? '<span class="hk-pill">' + escHtml(it.hotkey) + '</span>' : "") + '</div>' +
      '<span class="f-meta">' + escHtml(it.meta) + '</span>' +
      '<span class="f-acts">' + star + setHk +
        '<span class="ra" data-act="open" data-id="' + escAttr(it.id) + '" title="' + openLabel + '">' + '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M17 3l4 4L8 20l-5 1 1-5z"/></svg></span>' +
        '<span class="ra del" data-act="del" data-id="' + escAttr(it.id) + '" title="删除">' + '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 6h18M8 6V4h8v2M6 6l1 14h10l1-14"/></svg></span>' +
      '</span></div>';
  }
  function contentsHtml(name, items) {
    return '<div class="d-folder-head">' +
      '<div class="s-name">' + escHtml(name) + '</div>' +
      '<div class="d-fcount">共 ' + items.length + ' 项</div></div>' +
      '<div class="d-folder-list">' +
      (items.length ? items.map(fRow).join("") : '<div class="f-empty compact">此文件夹为空</div>') +
      '</div>';
  }
  function detailHtml(it) {
    var star = (it.type === "macro" || it.type === "rec")
      ? '<span class="ra star ' + (isPinned(it.id) ? "on" : "") + '" data-act="pin">' + STAR_SVG + '</span>' : "";
    var cells;
    if (it.type === "sched") cells = [["执行时间", escHtml(it.meta)], ["状态", "启用"], ["类型", "定时任务"]];
    else if (it.type === "ai") cells = [["消息", escHtml(it.meta)], ["类型", "AI 对话"], ["状态", "就绪"]];
    else cells = [["动作数", (it.ref && it.ref.actionCount != null ? it.ref.actionCount : 0) + " 步"], ["类型", it.type === "macro" ? "鼠标宏" : "录制"], ["热键", escHtml(itemHotkeyLabel(it))]];
    var primary = it.type === "ai" ? '<button class="btn-p" data-act="open">打开对话</button>'
      : it.type === "sched" ? '<button class="btn-p" data-act="open">编辑定时</button>'
      : '<button class="btn-p" data-act="run">运行</button>';
    var openBtn = (it.type === "macro" || it.type === "rec")
      ? '<button class="act-btn" data-act="open">' + (it.type === "macro" ? "编辑" : "优化") + '</button>' : "";
    var renameBtn = (it.type === "macro" || it.type === "rec")
      ? '<button class="act-btn" data-act="rename">重命名</button>' : "";
    var previewTitle = it.type === "ai" ? "对话" : it.type === "sched" ? "任务" : "动作预览";
    return '<div class="d-top"><div class="d-ico ' + it.type + '">' + TYPE[it.type].icon + '</div>' +
      '<div><div class="d-name">' + escHtml(it.name) + star + '</div>' +
      '<div class="d-sub">' + escHtml(it.meta) + '</div></div></div>' +
      '<div class="d-info">' + cells.map(function (c) { return '<div class="d-cell"><div class="k">' + c[0] + '</div><div class="v">' + c[1] + '</div></div>'; }).join("") + '</div>' +
      '<div class="d-actions">' + primary + openBtn + renameBtn +
        '<button class="act-btn danger" data-act="del">删除</button></div>' +
      '<div class="d-preview"><div class="d-sec">' + previewTitle + '</div>' +
      '<div class="pv-rows">' + previewRowsHtml(it) + '</div></div>';
  }
  function previewStamp(it) {
    if (!it || !it.ref) return "";
    return String(it.ref.actionCount != null ? it.ref.actionCount : "") + "|" + String(it.ref.recordTime || "");
  }
  function previewRowsHtml(it) {
    if (it.type === "ai") {
      return '<div class="pv-row">你：' + escHtml(it.name) + '</div><div class="pv-row">AI：已生成脚本，等待确认…</div>';
    }
    if (it.type === "sched") {
      return '<div class="pv-row">' + escHtml(it.meta || "定时任务") + '</div>';
    }
    var cache = _previewCache[it.id];
    if (cache && cache.stamp !== previewStamp(it)) cache = null;
    if (!cache) return '<div class="pv-row pv-loading">正在读取动作…</div>';
    if (cache.error) return '<div class="pv-row">' + escHtml(cache.error) + '</div>';
    var names = cache.names || [];
    if (!names.length) return '<div class="pv-row">暂无动作</div>';
    var html = names.map(function (n, i) {
      return '<div class="pv-row"><span class="pv-i">' + (i + 1) + '</span>' + escHtml(n) + '</div>';
    }).join("");
    var rest = (cache.actionCount | 0) - names.length;
    if (rest > 0) html += '<div class="pv-row pv-more">其余 ' + rest + ' 步</div>';
    return html;
  }
  function requestActionPreview(it) {
    if (!it || (it.type !== "macro" && it.type !== "rec")) return;
    var cache = _previewCache[it.id];
    if (cache && cache.stamp === previewStamp(it)) return;
    var path = (it.ref && (it.ref.path || it.ref.id)) || it.id;
    if (!path || !window.qst || typeof qst.previewScriptActions !== "function") {
      _previewCache[it.id] = { names: [], actionCount: 0, stamp: previewStamp(it), error: "无法读取脚本动作" };
      var rowsEl = A.$("#proDetail") && A.$("#proDetail").querySelector(".pv-rows");
      if (rowsEl) rowsEl.innerHTML = previewRowsHtml(it);
      return;
    }
    var reqId = "pv-" + (++_previewGen) + "-" + Date.now().toString(16);
    _previewWait[reqId] = it.id;
    qst.previewScriptActions(path, reqId, 16);
  }
  function applyPreviewResult(msg) {
    var reqId = String(msg.reqId || "");
    var itemId = _previewWait[reqId];
    delete _previewWait[reqId];
    if (!itemId) return;
    var it = findItemBySel(itemId);
    var stamp = it ? previewStamp(it) : "";
    if (!msg.ok) {
      _previewCache[itemId] = { names: [], actionCount: 0, stamp: stamp, error: msg.detail || "读取失败" };
    } else {
      _previewCache[itemId] = {
        names: Array.isArray(msg.names) ? msg.names : [],
        actionCount: msg.actionCount | 0,
        stamp: stamp,
      };
    }
    if (!S.selected || S.selected.kind !== "item" || S.selected.id !== itemId) return;
    var box = A.$("#proDetail");
    if (!box) return;
    var rows = box.querySelector(".pv-rows");
    var fresh = findItemBySel(itemId);
    if (rows && fresh) rows.innerHTML = previewRowsHtml(fresh);
  }
  function renderDetail() {
    var el = A.$("#proDetail");
    // 左树/列表选中脚本 → 右侧详情（优先于文件夹列表）
    if (S.selected && S.selected.kind === "item") {
      var it = findItemBySel(S.selected.id);
      el.innerHTML = it ? detailHtml(it) : '<div class="d-summary"><div class="s-name">脚本库</div><div class="s-line">单击脚本查看详情</div></div>';
      if (it) requestActionPreview(it);
      return;
    }
    if (S.view.kind === "folder") {
      var kv = folderKeyParts(S.view.id);
      var f = findFolder(kv.kind, kv.path);
      if (f) { el.innerHTML = contentsHtml(f.name, folderItems(kv.kind, f)); return; }
    }
    if (S.view.kind === "fav") {
      el.innerHTML = contentsHtml("常用", allItems().filter(function (x) { return (x.type === "macro" || x.type === "rec") && isPinned(x.id); }));
      return;
    }
    el.innerHTML = '<div class="d-summary"><div class="s-name">脚本库</div><div class="s-line">单击左侧脚本查看详情</div><div class="s-hint">单击文件夹显示其内容 · 双击脚本打开</div></div>';
  }
  /** 右侧列表就地切换选中（仅文件夹列表可见且无详情时） */
  function patchDetailListSelection() {
    var root = A.$("#proDetail");
    if (!root) return;
    var list = root.querySelector(".d-folder-list");
    if (!list) return;
    var selId = S.selected && S.selected.kind === "item" ? S.selected.id : "";
    var rows = list.querySelectorAll(".f-row[data-item]");
    for (var i = 0; i < rows.length; i++) {
      var r = rows[i];
      if ((r.getAttribute("data-item") || "") === selId) r.classList.add("on");
      else r.classList.remove("on");
    }
  }
  function patchItemSelection(treeRoot) {
    if (treeRoot) patchTreeSelection(treeRoot);
    else patchTreeSelection("#proLibTree");
    patchDetailListSelection();
  }
  function stripJsonExt(name) {
    return String(name || "").replace(/\.json$/i, "");
  }
  function findItemBySel(id) {
    var hit = null;
    KINDS.forEach(function (k) { var it = findItem(k, id); if (it) hit = it; });
    return hit;
  }

  /* ── 目录页（录制/宏/定时/脚本定制） ── */
  function renderSchedPane() {
    var tree = A.$("#proTree-sched");
    var crumbs = A.$("#proCrumbs-sched");
    if (!tree) return;
    if (crumbs) {
      crumbs.innerHTML = '<span class="p-crumb" data-jump="home">脚本库</span><span class="p-sep">/</span><span class="p-crumb on">定时</span>';
    }
    var tasks = A.state && Array.isArray(A.state.schedTasks) ? A.state.schedTasks : [];
    var gOn = !!(A.state && A.state.schedGlobalDisabled);
    var FREQ = ["每小时", "每日", "每周", "自定义", "间隔"];
    var head =
      '<div class="pro-sched-toolbar">' +
        '<h3>任务列表</h3>' +
        '<span class="chk' + (gOn ? " on" : "") + '" id="proSchedGlobalDisable"><i>' + (gOn ? "✓" : "") + '</i>禁用所有任务</span>' +
        '<button type="button" class="btn primary" id="proSchedCreate">任务创建</button>' +
      '</div>' +
      '<div class="pro-sched-table">' +
        '<div class="th"><span>任务名称</span><span>类型</span><span>文件</span><span>频率</span><span>时间</span><span>状态</span><span>操作</span></div>';
    var rows = tasks.map(function (t) {
      var kind = (t.kind | 0) === 0 ? "鼠标录制" : "鼠标宏";
      var freq = FREQ[t.frequency | 0] || "自定义";
      var st = (t.status | 0) === 0 ? "启用" : "禁用";
      return '<div class="tr" data-sched-id="' + escAttr(String(t.id || "")) + '">' +
        "<span>" + escHtml(t.name || "") + "</span><span>" + kind + "</span>" +
        "<span>" + escHtml(stripJsonExt(t.fileDisplayName || t.filePath || "")) + "</span>" +
        "<span>" + freq + "</span><span>" + escHtml(t.timeLabel || "") + "</span>" +
        '<span class="act" data-sched="status" title="点击切换启用/禁用">' + st + "</span>" +
        '<span class="act"><span data-sched="edit">编辑</span> <span data-sched="del">删除</span></span></div>';
    }).join("");
    tree.innerHTML = head + (rows || '<div class="tr empty-row"><span>暂无定时任务</span></div>') + "</div>";
    var createBtn = A.$("#proSchedCreate");
    if (createBtn && !createBtn._bound) {
      createBtn._bound = true;
      createBtn.addEventListener("click", function () {
        if (typeof A.openSchedEditor === "function") A.openSchedEditor(null);
      });
    }
    var gEl = A.$("#proSchedGlobalDisable");
    if (gEl && !gEl._bound) {
      gEl._bound = true;
      gEl.addEventListener("click", function () {
        var on = !gEl.classList.contains("on");
        gEl.classList.toggle("on", on);
        var i = gEl.querySelector("i");
        if (i) i.textContent = on ? "✓" : "";
        if (window.qst) qst.setScheduledTasksGlobalDisabled(on);
      });
    }
    if (!tree._schedBound) {
      tree._schedBound = true;
      tree.addEventListener("click", function (e) {
        var act = e.target.closest("[data-sched]");
        var row = e.target.closest("[data-sched-id]");
        if (!act || !row) return;
        var id = row.getAttribute("data-sched-id");
        var task = null;
        (A.state.schedTasks || []).forEach(function (t) {
          if (String(t.id || "") === id) task = t;
        });
        if (act.dataset.sched === "edit" && task && typeof A.openSchedEditor === "function") {
          A.openSchedEditor(task);
        } else if (act.dataset.sched === "status" && task && typeof A.toggleSchedTaskStatus === "function") {
          A.toggleSchedTaskStatus(task);
        } else if (act.dataset.sched === "del" && id) {
          A.askConfirm("确定删除该定时任务？", function () {
            if (window.qst) qst.deleteScheduledTask(id);
          });
        }
      });
    }
  }
  function renderDir(kind) {
    if (kind === "sched") { renderSchedPane(); return; }
    var tree = A.$("#proTree-" + kind);
    var crumbs = A.$("#proCrumbs-" + kind);
    if (!tree) return;
    var curId = S.tabSel[kind];
    var cur = curId ? findFolder(kind, curId) : null;
    var folders = cur ? (cur.subs || []) : folderList(kind);
    var items = cur ? folderItems(kind, cur) : rootItems(kind);
    var html = "";
    if (folders.length) {
      html += '<div class="g-label">' + FOLDER_SVG + '<span>文件夹</span></div>';
      folders.forEach(function (f) { html += folderNode(kind, f, 0); });
    }
    if (items.length) {
      html += '<div class="g-label">' + GRID_SVG + '<span>' + (cur ? "项目" : "未分类") + '</span></div>';
      items.forEach(function (it) { html += itemNode(it, 0); });
    }
    tree.innerHTML = html || '<div class="f-empty">此文件夹为空</div>';
    if (crumbs) renderDirCrumbs(kind);
  }
  function renderDirCrumbs(kind) {
    var bar = A.$("#proCrumbs-" + kind);
    if (!bar) return;
    var parts = [{ id: "type:" + kind, name: TYPE[kind].label }];
    var curId = S.tabSel[kind];
    if (curId) folderChain(kind, curId).forEach(function (f) { parts.push({ id: "folder:" + kind + ":" + f.id, name: f.name }); });
    bar.innerHTML = '<span class="p-crumb" data-jump="home">脚本库</span>' +
      parts.map(function (p, i) {
        return '<span class="p-sep">/</span><span class="p-crumb' + (i === parts.length - 1 ? " on" : "") + '" data-jump="' + p.id + '">' + escHtml(p.name) + '</span>';
      }).join("");
  }

  /* ── 事件 ── */
  // 双击窗口：偏短（约 200ms），先判双击再判取消选中，无需延迟 deselect
  var DBL_MS = 200;
  var _itemClickArm = { id: "", t: 0 };
  var _folderClickArm = { id: "", t: 0 };
  function itemDomId(el) {
    if (!el) return "";
    return el.getAttribute("data-item") || (el.dataset && el.dataset.item) || "";
  }
  function takeItemDoubleClick(id) {
    id = String(id || "");
    if (!id) return false;
    var now = Date.now();
    if (_itemClickArm.id === id && (now - _itemClickArm.t) <= DBL_MS) {
      _itemClickArm = { id: "", t: 0 };
      return true;
    }
    _itemClickArm = { id: id, t: now };
    return false;
  }
  function takeFolderDoubleClick(id) {
    id = String(id || "");
    if (!id) return false;
    var now = Date.now();
    if (_folderClickArm.id === id && (now - _folderClickArm.t) <= DBL_MS) {
      _folderClickArm = { id: "", t: 0 };
      return true;
    }
    _folderClickArm = { id: id, t: now };
    return false;
  }
  function clearItemSelectionUi(treeRoot) {
    S.selected = null;
    patchItemSelection(treeRoot || "#proLibTree");
    if (A.state) { A.state.macroSel = -1; A.state.recSel = -1; }
    if (typeof A.updateCtas === "function") A.updateCtas();
    if (isIdleHotkeyPage(S.activePage)) {
      // 脚本库等页取消选中：不能沿用上一档 Recorder 空选中（否则 F8 会开录）
      syncEngineForProPage(S.activePage);
    } else if (typeof A.syncEngineHomeSelection === "function") {
      A.syncEngineHomeSelection(true);
    }
  }
  function openItemById(id, kindHint) {
    var it = kindHint ? findItem(kindHint, id) : null;
    if (!it) it = findItemBySel(id);
    if (it) openAction(it);
  }
  function enterLibFolder(folderKey) {
    if (!folderKey) return;
    S.expanded["f:" + folderKey] = true;
    S.view = { kind: "folder", id: folderKey };
    S.selected = null;
    renderLibTree(); renderCrumbs(); renderDetail();
    syncEngineForProPage("library");
    var detail = A.$("#proDetail");
    if (detail) detail.scrollTop = 0;
  }
  function enterDirFolder(kind, folderPath) {
    if (!kind || !folderPath) return;
    S.expanded["f:" + kind + ":" + folderPath] = true;
    S.tabSel[kind] = folderPath;
    S.selected = null;
    renderDir(kind);
    var tree = A.$("#proTree-" + kind);
    if (tree) tree.scrollTop = 0;
  }
  function onLibTreeClick(e) {
    var viewEl = e.target.closest("[data-view]");
    var folderEl = e.target.closest("[data-folder]");
    var itemEl = e.target.closest("[data-item]");
    var twist = e.target.closest(".t-twist");
    if (viewEl) {
      if (twist) { S.expanded.fav = !S.expanded.fav; renderLibTree(); return; }
      S.view = { kind: "fav" };
      S.selected = { kind: "view", id: "fav" };
      patchTreeSelection("#proLibTree");
      renderCrumbs(); renderDetail();
      syncEngineForProPage("library");
      return;
    }
    if (folderEl) {
      if (twist) {
        var key = "f:" + folderEl.dataset.folder;
        S.expanded[key] = !S.expanded[key];
        renderLibTree(); // 展开需插入子节点
        return;
      }
      var fk = folderEl.dataset.folder || "";
      if (takeFolderDoubleClick(fk)) {
        enterLibFolder(fk);
        return;
      }
      S.view = { kind: "folder", id: fk };
      S.selected = null;
      patchTreeSelection("#proLibTree");
      renderCrumbs(); renderDetail();
      syncEngineForProPage("library");
      return;
    }
    if (itemEl) {
      var id = itemDomId(itemEl);
      if (takeItemDoubleClick(id)) {
        openItemById(id);
        return;
      }
      if (S.selected && S.selected.kind === "item" && S.selected.id === id) {
        clearItemSelectionUi("#proLibTree");
        renderDetail();
        return;
      }
      S.selected = { kind: "item", id: id };
      patchTreeSelection("#proLibTree");
      renderDetail();
      syncEngineForProPage("library");
    }
  }
  function onLibTreeDblClick(e) {
    var folderEl = e.target.closest("[data-folder]");
    var itemEl = e.target.closest("[data-item]");
    if (folderEl) {
      // 资源管理器语义：双击文件夹 = 进入该文件夹
      S.expanded["f:" + folderEl.dataset.folder] = true;
      S.view = { kind: "folder", id: folderEl.dataset.folder };
      S.selected = null;
      renderLibTree(); renderCrumbs(); renderDetail();
      syncEngineForProPage("library");
      var detail = A.$("#proDetail");
      if (detail) detail.scrollTop = 0;
    } else if (itemEl) {
      // 备选：若未重绘仍能收到原生 dblclick
      openItemById(itemDomId(itemEl));
    }
  }
  function onDirTreeClick(e, kind) {
    var twist = e.target.closest(".t-twist");
    var folderEl = e.target.closest("[data-folder]");
    var itemEl = e.target.closest("[data-item]");
    var treeSel = "#proTree-" + kind;
    if (folderEl) {
      if (twist) {
        var key = "f:" + folderEl.dataset.folder;
        S.expanded[key] = !S.expanded[key];
        renderDir(kind); // 展开需插入子节点
        return;
      }
      var rawFolder = folderEl.dataset.folder || "";
      if (takeFolderDoubleClick(rawFolder)) {
        var fpEnter = folderKeyParts(rawFolder);
        enterDirFolder(kind, fpEnter.path || rawFolder);
        return;
      }
      S.selected = null;
      // 点文件夹 = 取消脚本选中，全局热键回到「未选中→录制/新建」语义
      if ((kind === "macro" || kind === "rec") && A.state) {
        if (kind === "macro") A.state.macroSel = -1;
        else A.state.recSel = -1;
        if (typeof A.updateCtas === "function") A.updateCtas();
        if (typeof A.syncEngineHomeSelection === "function") A.syncEngineHomeSelection();
      }
      patchTreeSelection(treeSel);
      return;
    }
    if (itemEl) {
      var id = itemDomId(itemEl);
      if (takeItemDoubleClick(id)) {
        openItemById(id, kind);
        return;
      }
      if (S.selected && S.selected.kind === "item" && S.selected.id === id) {
        clearItemSelectionUi(treeSel);
        return;
      }
      S.selected = { kind: "item", id: id };
      patchTreeSelection(treeSel);
      var selIt = findItem(kind, id);
      if (selIt) syncSimpleSelection(selIt);
    }
  }
  function folderKeyParts(raw) {
    var s = String(raw || "");
    var i = s.indexOf(":");
    if (i < 0) return { kind: "", path: s };
    return { kind: s.slice(0, i), path: s.slice(i + 1) };
  }
  function onDirTreeDblClick(e, kind) {
    var folderEl = e.target.closest("[data-folder]");
    var itemEl = e.target.closest("[data-item]");
    if (folderEl) {
      var fp = folderKeyParts(folderEl.dataset.folder);
      S.expanded["f:" + folderEl.dataset.folder] = true;
      S.tabSel[kind] = fp.path;
      S.selected = null;
      renderDir(kind);
      A.$("#proTree-" + kind).scrollTop = 0;
    } else if (itemEl) {
      openItemById(itemDomId(itemEl), kind);
    }
  }
  function onCrumbClick(e) {
    var jump = e.target.closest("[data-jump]");
    if (!jump) return;
    var id = jump.dataset.jump;
    if (id === "home") {
      switchPage("library");
      S.view = { kind: "all" };
      S.selected = null;
      renderLibTree(); renderCrumbs(); renderDetail();
      syncEngineForProPage("library");
      return;
    }
    if (id.indexOf("type:") === 0) {
      var kind = id.slice(5);
      S.tabSel[kind] = null;
      renderDir(kind);
      return;
    }
    if (id.indexOf("folder:") === 0) {
      var bits = id.split(":");
      S.tabSel[bits[1]] = bits[2];
      renderDir(bits[1]);
      return;
    }
    if (id === "fav") {
      S.view = { kind: "fav" };
      S.selected = { kind: "view", id: "fav" };
      renderLibTree(); renderCrumbs(); renderDetail();
      syncEngineForProPage("library");
      return;
    }
    if (id.indexOf("macro:") === 0 || id.indexOf("rec:") === 0) {
      S.view = { kind: "folder", id: id };
      S.selected = null;
      renderLibTree(); renderCrumbs(); renderDetail();
      syncEngineForProPage("library");
    }
  }
  function onDetailClick(e) {
    var actEl = e.target.closest("[data-act]");
    if (actEl) {
      var item = actEl.dataset.id ? findItemBySel(actEl.dataset.id) : (S.selected && S.selected.kind === "item" ? findItemBySel(S.selected.id) : null);
      if (!item) return;
      if (actEl.dataset.act === "pin") { togglePin(item.id); renderLibTree(); renderDetail(); }
      else if (actEl.dataset.act === "run") runAction(item);
      else if (actEl.dataset.act === "open") openAction(item);
      else if (actEl.dataset.act === "setHk") setHotkey(item);
      else if (actEl.dataset.act === "rename") renameItem(item);
      else if (actEl.dataset.act === "del") delItem(item);
      return;
    }
    var itemEl = e.target.closest("[data-item]");
    if (!itemEl) return;
    var did = itemDomId(itemEl);
    if (takeItemDoubleClick(did)) {
      openItemById(did);
      return;
    }
    if (S.selected && S.selected.kind === "item" && S.selected.id === did) {
      clearItemSelectionUi("#proLibTree");
      renderDetail();
      return;
    }
    S.selected = { kind: "item", id: did };
    patchTreeSelection("#proLibTree");
    renderDetail();
    syncEngineForProPage("library");
  }
  function onDetailDblClick(e) {
    var itemEl = e.target.closest("[data-item]");
    if (!itemEl) return;
    openItemById(itemDomId(itemEl));
  }

  /* ── 动作 ── */
  function runAction(it) { if (window.qst) qst.runScript(it.id); toast("开始运行 " + it.name); }
  function openAction(it) {
    if (!it) return;
    var path = (it.ref && (it.ref.path || it.ref.id)) || it.id;
    var name = String((it.ref && it.ref.name) || it.name || "").replace(/\.json$/i, "");
    if (it.type === "macro") {
      if (typeof A.enterEditor === "function") A.enterEditor(name, path);
      else if (window.qst) qst.openEditor(path || "");
      return;
    }
    if (it.type === "rec") {
      if (window.qst && typeof qst.openRecordingOptimize === "function") {
        if (A.state) A.state._modeTransEndOn = "loadOpt";
        qst.openRecordingOptimize(path, name);
      }
      return;
    }
    if (it.type === "sched") {
      if (typeof A.openSchedEditor === "function") A.openSchedEditor(it.ref);
      return;
    }
    if (it.type === "ai") {
      var aiId = (it.ref && (it.ref.id || it.ref.path)) || it.id;
      if (typeof A.openAgentSession === "function") A.openAgentSession(aiId, false);
      else if (window.qst && typeof qst.openAgentWindow === "function") {
        qst.openAgentWindow({ id: String(aiId || ""), createNew: 0 });
      }
    }
  }
  function setHotkey(it) { A.openHotkeyFor(it.ref); }
  function renameItem(it) {
    if (it.type === "sched") { A.openSchedEditor(it.ref); return; }
    if (it.type === "ai") { A.openAgentSession(it.id, false); return; }
    A.openRename(it.ref, it.type === "rec" ? "rec" : "macro");
  }
  function delItem(it) {
    A.askConfirm("确定删除「" + it.name + "」？", function () {
      if (it.type === "sched") { if (window.qst) qst.deleteScheduledTask(it.id); }
      else if (it.type === "ai") { if (window.qst) qst.deleteAgentConversation(it.id); }
      else if (window.qst) qst.deleteScript(it.ref.path);
    });
  }
  function onCta(act) {
    if (act === "macroNew") A.enterEditor("", "");
    else if (act === "schedNew") {
      if (typeof A.openSchedEditor === "function") A.openSchedEditor(null);
      else A.openOv("sched");
    }
  }

  /* ── 右键菜单 ── */
  function showCtx(x, y, items, payload) {
    var el = A.$("#proCtx");
    el.innerHTML = items.map(function (it) {
      return it.sep ? '<div class="cs"></div>'
        : '<div class="ci ' + (it.danger ? "danger" : "") + '" data-act="' + it.act + '">' + (it.icon || "") + it.label + '</div>';
    }).join("");
    el._payload = payload || {};
    el.classList.add("show");
    var r = el.getBoundingClientRect();
    var left = Math.min(x, window.innerWidth - r.width - 8);
    var top = Math.min(y, window.innerHeight - r.height - 8);
    el.style.left = Math.max(4, left) + "px";
    el.style.top = Math.max(4, top) + "px";
  }
  function hideCtx() { var el = A.$("#proCtx"); if (el) { el.classList.remove("show"); el._payload = null; } }
  function ctxIcon(act) {
    return { run: '<svg viewBox="0 0 24 24" fill="currentColor" stroke="none"><path d="M7 4.5v15l13-7.5z"/></svg>',
      pin: STAR_SVG,
      open: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M17 3l4 4L8 20l-5 1 1-5z"/></svg>',
      setHk: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="7" width="18" height="10" rx="2"/><path d="M8 11v2M12 11v2M16 11v2"/></svg>',
      import: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 3v12M7 10l5 5 5-5"/><path d="M4 17v3h16v-3"/></svg>',
      export: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 15V3M7 8l5-5 5 5"/><path d="M4 17v3h16v-3"/></svg>',
      rename: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M4 20h16M6 16l6-11 6 11"/></svg>',
      del: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 6h18M8 6V4h8v2M6 6l1 14h10l1-14"/></svg>',
      cut: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="6" cy="6" r="2.5"/><circle cx="6" cy="18" r="2.5"/><path d="M8.5 8.5L20 19M8.5 15.5L20 5"/></svg>',
      copy: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="9" y="9" width="12" height="12" rx="2"/><path d="M5 15V5a2 2 0 0 1 2-2h10"/></svg>',
      paste: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 3v12M7 10l5 5 5-5"/><path d="M4 17v3h16v-3"/></svg>',
      openFolder: FOLDER_SVG,
      newFolder: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 6h6l2 2h10v11H3z"/><path d="M12 12v6M9 15h6"/></svg>',
      newItem: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 5v14M5 12h14"/></svg>' }[act] || "";
  }
  function onCtxMenu(e) {
    var pro = A.$("#proHome");
    if (!pro || pro.hidden || !pro.contains(e.target)) { hideCtx(); return; }
    // 内嵌极简连点整页 / 设置：不抢右键；底栏 CTA 允许
    if (e.target.closest(".pro-embed-host, .pro-settings-host")) { hideCtx(); return; }
    e.preventDefault();
    var itemEl = e.target.closest("[data-item]");
    var folderEl = e.target.closest("[data-folder]");
    var kind = folderEl ? folderKeyParts(folderEl.dataset.folder).kind : (S.activePage !== "library" ? PAGE_KIND[S.activePage] : "macro");
    if (itemEl) {
      var it = findItemBySel(itemEl.dataset.item);
      if (!it) return;
      S.selected = { kind: "item", id: it.id };
      renderLibTree(); renderDirIfActive();
      var m = [];
      if (it.type === "macro" || it.type === "rec") {
        m.push({ act: "run", label: "运行", icon: ctxIcon("run") });
        m.push({ act: "pin", label: isPinned(it.id) ? "取消收藏" : "收藏", icon: ctxIcon("pin") });
      }
      m.push({ act: "open", label: it.type === "macro" ? "编辑" : it.type === "rec" ? "优化" : it.type === "sched" ? "编辑" : "对话", icon: ctxIcon("open") });
      if (it.type === "macro" || it.type === "rec") {
        m.push({ act: "setHk", label: "设置热键", icon: ctxIcon("setHk") });
        m.push({ sep: true });
        m.push({ act: "import", label: "导入", icon: ctxIcon("import") });
        m.push({ act: "export", label: "导出", icon: ctxIcon("export") });
      }
      m.push({ sep: true });
      m.push({ act: "rename", label: "重命名", icon: ctxIcon("rename") });
      m.push({ act: "del", label: "删除", icon: ctxIcon("del"), danger: true });
      showCtx(e.clientX, e.clientY, m, { kind: it.type, item: it });
      return;
    }
    if (folderEl) {
      var fk = folderKeyParts(folderEl.dataset.folder);
      var f = findFolder(fk.kind, fk.path);
      if (!f) return;
      var fm = [
        { act: "openFolder", label: "打开", icon: ctxIcon("openFolder") },
        { sep: true },
        { act: "cutFolder", label: "剪切", icon: ctxIcon("cut") },
        { act: "copyFolder", label: "复制", icon: ctxIcon("copy") },
      ];
      if (S.clipboard) fm.push({ act: "paste", label: "粘贴", icon: ctxIcon("paste") });
      fm.push({ sep: true });
      fm.push({ act: "renameFolder", label: "重命名", icon: ctxIcon("rename") });
      fm.push({ act: "delFolder", label: "删除", icon: ctxIcon("del"), danger: true });
      showCtx(e.clientX, e.clientY, fm, { kind: fk.kind, folder: f });
      return;
    }
    // 空白
    var bm = [];
    if (S.clipboard) bm.push({ act: "paste", label: "粘贴", icon: ctxIcon("paste") });
    var blankKind = S.activePage !== "library" ? PAGE_KIND[S.activePage]
      : (S.filter === "rec" || S.filter === "macro" ? S.filter : "macro");
    var label = blankKind === "ai" ? "新建对话" : blankKind === "sched" ? "新建定时任务" : blankKind === "rec" ? "新建录制" : blankKind === "macro" ? "新建宏" : "新建脚本";
    bm.push({ act: "newFolder", label: "新建文件夹", icon: ctxIcon("newFolder") });
    bm.push({ act: "newItem", label: label, icon: ctxIcon("newItem") });
    showCtx(e.clientX, e.clientY, bm, { kind: blankKind });
  }
  function renderDirIfActive() {
    if (PAGE_KIND[S.activePage]) renderDir(PAGE_KIND[S.activePage]);
  }

  /* ── 动作分发 ── */
  function doAction(act, payload) {
    var kind = payload.kind || "macro";
    if (act === "newItem") {
      if (kind === "ai") A.openAgentSession("", true);
      else if (kind === "sched") {
        if (typeof A.openSchedEditor === "function") A.openSchedEditor(null);
        else A.openOv("sched");
      }
      else if (kind === "rec") {
        if (window.qst) {
          qst.startRecord({ recorderWindowMode: (st().recWindowMode | 0) });
        }
        toast("开始录制");
      }
      else A.enterEditor("", "");
      return;
    }
    if (act === "newFolder") {
      var parent = S.activePage !== "library" ? (S.tabSel[kind] || null)
        : (S.view.kind === "folder" ? folderKeyParts(S.view.id).path : null);
      if (typeof A.askInput === "function") {
        A.askInput("新建文件夹", "新建文件夹", function (name) {
          if (!name || !name.trim()) return;
          newFolder(kind, parent, name.trim());
          toast("已创建文件夹");
          setTimeout(function () { refreshDirLists(); onDataChanged(); }, 80);
        });
      }
      return;
    }
    if (act === "openFolder") {
      if (S.activePage !== "library") { S.tabSel[kind] = payload.folder.id; renderDir(kind); }
      else {
        S.view = { kind: "folder", id: kind + ":" + payload.folder.id };
        S.selected = null;
        renderLibTree(); renderCrumbs(); renderDetail();
        syncEngineForProPage("library");
      }
      return;
    }
    if (act === "cutFolder") { S.clipboard = { mode: "cut", kind: kind, id: payload.folder.id }; toast("已剪切文件夹"); renderLibTree(); renderDirIfActive(); return; }
    if (act === "copyFolder") { toast("真实目录模式暂不支持复制文件夹"); return; }
    if (act === "paste") { pasteFolder(payload); return; }
    if (act === "renameFolder") {
      if (typeof A.askInput === "function") {
        A.askInput("重命名文件夹", payload.folder.name, function (nv) {
          if (!nv || !nv.trim() || !window.qst || !qst.renameLibraryFolder) return;
          qst.renameLibraryFolder(kind, payload.folder.id, nv.trim());
          toast("已重命名");
          setTimeout(function () { refreshDirLists(); onDataChanged(); }, 80);
        });
      }
      return;
    }
    if (act === "delFolder") {
      A.askConfirm("确定删除文件夹「" + payload.folder.name + "」？（其中的脚本会移到上级/未分类，不会丢失）", function () {
        deleteFolder(kind, payload.folder.id);
        toast("已删除文件夹");
        setTimeout(function () { refreshDirLists(); onDataChanged(); }, 80);
      });
      return;
    }
    var item = payload.item;
    if (!item) return;
    if (act === "run") runAction(item);
    else if (act === "pin") { togglePin(item.id); toast(isPinned(item.id) ? "已收藏" : "已取消收藏"); renderLibTree(); renderDetail(); }
    else if (act === "open") openAction(item);
    else if (act === "setHk") setHotkey(item);
    else if (act === "import") { if (window.qst) qst.importScript(item.type); }
    else if (act === "export") { if (window.qst) qst.exportScript(item.ref.path); }
    else if (act === "rename") renameItem(item);
    else if (act === "del") delItem(item);
  }
  function pasteFolder(payload) {
    var kb = S.clipboard;
    if (!kb || kb.mode !== "cut") { toast("请先剪切文件夹"); return; }
    var target = S.activePage !== "library" ? (S.tabSel[kb.kind] || "")
      : (S.view.kind === "folder" ? folderKeyParts(S.view.id).path : "");
    if (target === kb.id || (target && target.indexOf(kb.id + "/") === 0)) {
      toast("不能移动到自身内部"); return;
    }
    var name = kb.id.indexOf("/") >= 0 ? kb.id.slice(kb.id.lastIndexOf("/") + 1) : kb.id;
    var dest = target ? (target + "/" + name) : name;
    // 重命名到新路径 = 移动目录
    if (window.qst && qst.renameLibraryFolder) {
      // 先在目标父级创建同名，再依赖 rename 无法跨父级：用 create+逐项移动过于复杂，
      // 对物理目录：renameLibraryFolder 只改末段名。跨目录移动：重建路径。
      if (!target) {
        // 移到根：若已在根则无操作
        var slash = kb.id.lastIndexOf("/");
        if (slash < 0) { toast("已在根目录"); S.clipboard = null; return; }
        qst.renameLibraryFolder(kb.kind, kb.id, name);
      } else {
        // 简化：创建目标文件夹后把条目迁入，再删源文件夹
        qst.createLibraryFolder(kb.kind, dest);
        listOf(kb.kind).forEach(function (it) {
          if (it.folder === kb.id || (it.folder && it.folder.indexOf(kb.id + "/") === 0)) {
            var rest = it.folder === kb.id ? "" : it.folder.slice(kb.id.length + 1);
            var nf = rest ? (dest + "/" + rest) : dest;
            moveItemToFolder(kb.kind, it.id, nf);
          }
        });
        qst.deleteLibraryFolder(kb.kind, kb.id);
      }
      toast("已移动文件夹");
    }
    S.clipboard = null;
    setTimeout(function () { refreshDirLists(); onDataChanged(); }, 120);
  }

  /* ── 拖拽 ── */
  var drag = null;
  function initDrag(el, treeKind) {
    el.addEventListener("dragstart", function (e) {
      var itemEl = e.target.closest("[data-item]");
      if (!itemEl) return;
      e.dataTransfer.effectAllowed = "move";
      e.dataTransfer.setData("text/plain", itemEl.dataset.item);
      drag = { id: itemEl.dataset.item, tree: treeKind };
      itemEl.classList.add("cut");
    });
    el.addEventListener("dragend", function () {
      drag = null;
      if (el._dropOn) { el._dropOn.classList.remove("drop-on"); el._dropOn = null; }
      A.$$("#proHome .t-node.cut").forEach(function (n) { if (!S.clipboard || S.clipboard.id !== n.dataset.item) n.classList.remove("cut"); });
      renderLibTree(); renderDirIfActive();
    });
    el.addEventListener("dragover", function (e) {
      if (!drag) return;
      e.preventDefault();
      e.dataTransfer.dropEffect = "move";
      var t = e.target.closest("[data-folder]") || e.target.closest("[data-item]") || el;
      if (el._dropOn === t) return;
      if (el._dropOn) el._dropOn.classList.remove("drop-on");
      t.classList.add("drop-on");
      el._dropOn = t;
    });
    el.addEventListener("drop", function (e) {
      if (!drag) return;
      e.preventDefault();
      var folderEl = e.target.closest("[data-folder]");
      var itemEl = e.target.closest("[data-item]");
      var src = findItemBySel(drag.id);
      var srcKind = src ? src.type : null;
      if (srcKind) {
        var targetKind = folderEl ? folderKeyParts(folderEl.dataset.folder).kind : (treeKind === "lib" ? srcKind : treeKind);
        if (targetKind !== srcKind) { toast("不能跨类型移动"); }
        else if (folderEl) {
          moveItemToFolder(srcKind, drag.id, folderKeyParts(folderEl.dataset.folder).path);
          toast("已移动");
          setTimeout(function () { onDataChanged(); }, 80);
        } else if (itemEl) {
          var targetFolder = itemFolderId(srcKind, itemEl.dataset.item) || "";
          moveItemToFolder(srcKind, drag.id, targetFolder);
          toast("已移动");
          setTimeout(function () { onDataChanged(); }, 80);
        } else {
          moveItemToFolder(srcKind, drag.id, "");
          toast("已移到未分类");
          setTimeout(function () { onDataChanged(); }, 80);
        }
      }
      drag = null;
      A.$$("#proHome .drop-on").forEach(function (n) { n.classList.remove("drop-on"); });
    });
  }

  /* ── 工具 ── */
  function escHtml(s) { return A.esc(String(s == null ? "" : s)); }
  function escAttr(s) { return String(s == null ? "" : s).replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;"); }
  function toast(msg) { A.toast(msg); }

  /* ── 对外 API ── */
  function mode() { return S.mode; }
  function setMode(m) {
    S.mode = m === "pro" ? "pro" : "simple";
    savePersist();
  }
  function onProBridge(ev) {
    var msg = ev.detail || {};
    var type = msg.type || "";
    if (type === "saveEditor.result" || type === "deleteScript.result"
      || type === "renameScript.result" || type === "importScript.result"
      || type === "applyOptimizeRecording.result") {
      _previewCache = {};
      _previewWait = {};
    }
    if (type === "previewScriptActions.result") {
      applyPreviewResult(msg);
      return;
    }
    if (type === "listLibraryFolders.result" && msg.ok) {
      var k = msg.kind === "scripts" ? "macro" : msg.kind === "recordings" ? "rec" : msg.kind;
      if (KINDS.indexOf(k) >= 0) {
        S.dirLists[k] = (msg.folders || []).map(normFolder);
        invalidateCaches();
        if (_folderFetchPending > 0) _folderFetchPending--;
        // 等本轮 4 个 kind 到齐再刷一次，避免风暴重绘
        if (_folderFetchPending <= 0) onDataChanged();
      }
      return;
    }
    if (type === "createLibraryFolder.result" || type === "renameLibraryFolder.result"
      || type === "deleteLibraryFolder.result" || type === "moveScriptToFolder.result"
      || type === "setItemLibraryFolder.result") {
      if (msg.ok) {
        if (msg.scripts && A.state) A.state.macros = msg.scripts;
        if (msg.recordings && A.state) A.state.recordings = msg.recordings;
        invalidateCaches();
        // 只刷新受影响类型的目录 + 对应脚本列表，避免全量风暴
        var kindHint = msg.kind === "scripts" ? "macro" : msg.kind === "recordings" ? "rec" : (msg.kind || "");
        if (kindHint && window.qst && qst.listLibraryFolders) qst.listLibraryFolders(kindHint);
        else refreshDirLists();
        if (window.qst) {
          if (kindHint === "rec") qst.listRecordings();
          else if (kindHint === "macro") qst.listScripts();
          else {
            qst.listScripts();
            qst.listRecordings();
          }
        }
      } else if (msg.detail) toast(msg.detail);
    }
  }
  function buildAndShow() {
    build();
    var root = A.$("#proHome");
    if (root) root.hidden = false;
    refreshDirLists();
    switchPage(S.activePage || "library");
    if (!st()._gotScripts && window.qst) qst.listScripts();
    if (!st()._gotRecordings && window.qst) qst.listRecordings();
    if (window.qst && typeof qst.listScheduledTasks === "function") qst.listScheduledTasks();
    if (window.qst) qst.listAgentConversations();
  }
  function hide() {
    restoreAllEmbeds();
    S._embedPage = null;
    var root = A.$("#proHome");
    if (root) root.hidden = true;
    hideCtx();
  }
  function onDataChanged() {
    if (S.mode !== "pro" || !S.built) return;
    invalidateCaches();
    clearTimeout(onDataChanged._t);
    onDataChanged._t = setTimeout(function () {
      onDataChanged._t = 0;
      if (S.activePage === "library") {
        renderLibTree();
        renderCrumbs();
        renderDetail();
      } else if (PAGE_KIND[S.activePage]) {
        renderDirIfActive();
      }
      // CTA 不随纯数据刷新重写，避免无谓 DOM 写入
    }, 100);
  }
  function sync() {
    if (S.mode !== "pro") { hide(); return; }
    buildAndShow();
  }

  loadPersist();
  window.ProMode = {
    mode: mode,
    setMode: setMode,
    sync: sync,
    hide: hide,
    onDataChanged: onDataChanged,
    openSettings: openSettings,
    switchPage: switchPage,
    resyncEngine: resyncEngine,
  };
  // 重载场景下 app.js 可能在 pro-mode.js 之前执行 init：这里再幂等应用一次模式
  if (window.AppShell && typeof window.AppShell.applyUiMode === "function") {
    window.AppShell.applyUiMode();
  }
})();
