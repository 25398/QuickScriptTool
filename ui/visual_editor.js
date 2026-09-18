/**
 * ui/visual_editor.js — 宏编辑器可视化画布（LCA 风格呈现，数据仍是 indent 动作列表）
 */
(function () {
  "use strict";

  var CARD_W = 210;
  var CARD_H = 72;
  var LOOP_W = 168;
  var GLUE = 10;
  var GAP_X = 88;
  var GAP_Y = 80;
  var STUB = 28;
  var BOX_PAD = 8;
  var PAD = 72;
  var WRAP_PAD = 22;
  var BACK_PAD = 52;
  var BADGE_H = 22;
  var LANE = 18;
  var WRAP_EDGE = 12;
  var WIRE_PAD = 10;
  var GRID = 18;
  var WORLD_MIN_W = 2200;
  var WORLD_MIN_H = 1400;
  var ZOOM_MIN = 0.3;
  var ZOOM_MAX = 2.5;
  var ZOOM_FACTOR = 1.15;
  var UNDO_MAX = 50;

  var vis = {
    mode: false,
    zoom: 1,
    undo: [],
    clipboard: null,
    pendingScroll: null,
    drag: null,
    lastMenuWorld: null,
    skipAutoFit: false,
    needAlign: false,
    links: [],
    linksReady: false,
    start: { x: PAD, y: PAD },
    selStart: false,
    selEdge: null,
    graphTouched: false,
    loopMarks: [],
    wireRoutes: {},
    _edgePts: {},
    hudHideTimer: null,
  };

  function A() {
    return window.AppShell || {};
  }
  function $(sel, root) {
    if (A().$ && !root) return A().$(sel);
    return (root || document).querySelector(sel);
  }
  function state() {
    return A().state || {};
  }
  function actions() {
    return state().editorActions || [];
  }
  function toast(msg) {
    if (typeof A().toast === "function") A().toast(msg);
  }
  function clamp(v, lo, hi) {
    return Math.max(lo, Math.min(hi, v));
  }
  function clampIndent(v) {
    return typeof A().clampIndent === "function" ? A().clampIndent(v) : Math.max(0, v | 0);
  }
  function subtreeEnd(i) {
    return typeof A().subtreeEnd === "function" ? A().subtreeEnd(i) : i + 1;
  }
  function isContainer(type) {
    return typeof A().isSubtreeContainerType === "function"
      ? A().isSubtreeContainerType(type)
      : type === "loop" || type === "if" || type === "else" || type === "defineBlock" || type === "watchImage";
  }
  function isDeclContainerType(type) {
    return type === "defineBlock" || type === "watchImage";
  }
  function isTopDeclContainer(a) {
    return !!(a && isDeclContainerType(a.type) && !(a.indent | 0));
  }
  function host() {
    return $("#visualCanvasHost");
  }
  function scrollEl() {
    return $("#visualScroll") || host();
  }
  function worldEl() {
    return $("#visualWorld");
  }
  function sizerEl() {
    return $("#visualSizer");
  }
  function leftCol() {
    var h = host();
    return h ? h.closest(".left-col") : null;
  }
  function isVisual() {
    return !!vis.mode;
  }
  function snap(v) {
    return Math.round(v / GRID) * GRID;
  }
  function sameEdge(a, b) {
    return !!(a && b && a.from === b.from && a.to === b.to && (a.kind || "seq") === (b.kind || "seq"));
  }
  function linkKey(from) {
    return from === "start" ? "start" : from | 0;
  }

  function isHistoryHeld() {
    return !!vis.holdUndo;
  }

  function isInteracting() {
    return !!vis.drag;
  }

  function afterListDelete() {
    pruneBrokenLoops();
  }

  function collectHistory() {
    var el = scrollEl();
    try {
      return {
        zoom: vis.zoom,
        scrollX: el ? el.scrollLeft : 0,
        scrollY: el ? el.scrollTop : 0,
        links: JSON.parse(JSON.stringify(vis.links || [])),
        loopMarks: JSON.parse(JSON.stringify(vis.loopMarks || [])),
        wireRoutes: vis.wireRoutes ? JSON.parse(JSON.stringify(vis.wireRoutes)) : {},
        start: { x: vis.start.x | 0, y: vis.start.y | 0 },
        selStart: !!vis.selStart,
        selEdge: vis.selEdge ? JSON.parse(JSON.stringify(vis.selEdge)) : null,
        graphTouched: !!vis.graphTouched,
        linksReady: !!vis.linksReady,
      };
    } catch (_) {
      return null;
    }
  }

  function applyHistory(h) {
    if (!h || typeof h !== "object") return;
    var copyH = h;
    try {
      copyH = JSON.parse(JSON.stringify(h));
    } catch (_) {}
    vis.zoom = typeof copyH.zoom === "number" ? copyH.zoom : vis.zoom;
    vis.pendingScroll = { x: copyH.scrollX | 0, y: copyH.scrollY | 0 };
    vis.links = normalizeLinks(copyH.links);
    vis.linksReady = copyH.linksReady != null ? !!copyH.linksReady : true;
    vis.loopMarks = Array.isArray(copyH.loopMarks) ? copyH.loopMarks : [];
    vis.wireRoutes = copyH.wireRoutes && typeof copyH.wireRoutes === "object" ? copyH.wireRoutes : {};
    if (copyH.start && typeof copyH.start === "object") {
      vis.start = { x: copyH.start.x | 0, y: copyH.start.y | 0 };
    }
    vis.selStart = !!copyH.selStart;
    vis.selEdge = copyH.selEdge || null;
    vis.graphTouched = !!copyH.graphTouched;
    vis.skipAutoFit = true;
  }

  function setClipboard(a) {
    if (!a) {
      vis.clipboard = null;
      return;
    }
    vis.clipboard = JSON.parse(JSON.stringify(a));
    delete vis.clipboard._preview;
  }

  function hasClipboardCard() {
    if (vis.clipboard) return true;
    var clip = state().editorClipboard;
    return !!(clip && clip[0] && clip[0][0]);
  }

  function pushUndo() {
    if (vis.holdUndo) return;
    if (typeof A().pushEditorHistory === "function") A().pushEditorHistory();
  }

  function discardLastUndo() {
    if (typeof A().discardEditorHistory === "function") A().discardEditorHistory();
  }

  function restoreUndo() {
    if (typeof A().undoEditorHistory === "function") A().undoEditorHistory();
  }

  function restoreRedo() {
    if (typeof A().redoEditorHistory === "function") A().redoEditorHistory();
  }

  function deleteSelectedEdgeIfAny() {
    if (!vis.selEdge) return false;
    deleteEdge(vis.selEdge);
    return true;
  }

  function displayName(a) {
    if (typeof A().displayActionName === "function") return A().displayActionName(a);
    return (a && (a.name || a.type)) || "动作";
  }

  function otherPref(key, defaultOn) {
    var o = (state().settings && state().settings.other) || {};
    if (o[key] == null) return defaultOn !== false;
    return !!o[key];
  }

  function cardTitleText(a, i) {
    var name = a && a.type === "loop" ? loopBadgeText(a) : displayName(a);
    if (!otherPref("visualShowCardId", true)) return name;
    return name + " (ID: " + (i + 1) + ")";
  }

  function cardPos(a) {
    var x = a && typeof a._vx === "number" ? a._vx : 0;
    var y = a && typeof a._vy === "number" ? a._vy : 0;
    return { x: x, y: y };
  }

  function hasAnyLayout(list) {
    for (var i = 0; i < list.length; i++) {
      if (typeof list[i]._vx === "number" || typeof list[i]._vy === "number") return true;
    }
    return false;
  }

  function firstMainIndex(list) {
    var i = 0;
    while (i < list.length) {
      if (isTopDeclContainer(list[i])) {
        i = subtreeEnd(i);
        continue;
      }
      if ((list[i].indent | 0) === 0) return i;
      i += 1;
    }
    return -1;
  }

  function nextSiblingIndex(list, i) {
    if (i < 0 || i >= list.length) return -1;
    var end = subtreeEnd(i);
    var indent = list[i].indent | 0;
    if (end < list.length && (list[end].indent | 0) === indent) return end;
    return -1;
  }

  function directChildren(list, i) {
    var kids = [];
    if (i < 0 || i >= list.length || !isContainer(list[i].type)) return kids;
    var end = subtreeEnd(i);
    var childIndent = (list[i].indent | 0) + 1;
    var j = i + 1;
    while (j < end) {
      if ((list[j].indent | 0) === childIndent) {
        kids.push(j);
        j = subtreeEnd(j);
      } else {
        j += 1;
      }
    }
    return kids;
  }

  function findElseIndex(list, ifIdx) {
    if (!list[ifIdx] || list[ifIdx].type !== "if") return -1;
    var bodyEnd = subtreeEnd(ifIdx);
    if (bodyEnd < list.length && list[bodyEnd].type === "else" && (list[bodyEnd].indent | 0) === (list[ifIdx].indent | 0)) {
      return bodyEnd;
    }
    return -1;
  }

  function kidsNoElse(list, parent) {
    return directChildren(list, parent).filter(function (k) {
      return list[k] && list[k].type !== "else";
    });
  }

  function hasIfFalseLink(idx) {
    var i;
    for (i = 0; i < vis.links.length; i++) {
      if (vis.links[i].from !== idx) continue;
      var k = vis.links[i].kind || "seq";
      if (k === "ifFalse" || k === "fail") return true;
    }
    return false;
  }

  function remapFilteredActions(list, keepFn) {
    if (!Array.isArray(vis.links)) vis.links = [];
    if (!Array.isArray(vis.loopMarks)) vis.loopMarks = [];
    var next = [];
    var map = {};
    var dropped = {};
    var i;
    for (i = 0; i < list.length; i++) {
      if (!keepFn(i, list[i])) {
        dropped[i] = true;
        continue;
      }
      map[i] = next.length;
      next.push(list[i]);
    }
    if (next.length === list.length) return false;
    var st = state();
    st.editorActions = next;
    if (st.actionSel >= 0) {
      if (dropped[st.actionSel]) st.actionSel = -1;
      else if (map[st.actionSel] != null) st.actionSel = map[st.actionSel];
    }
    vis.links = vis.links
      .filter(function (l) {
        var fOk = l.from === "start" || (typeof l.from === "number" && map[l.from] != null);
        return fOk && map[l.to] != null;
      })
      .map(function (l) {
        return {
          from: l.from === "start" ? "start" : map[l.from],
          to: map[l.to],
          kind: l.kind || "seq",
        };
      });
    vis.loopMarks = vis.loopMarks
      .filter(function (m) {
        return map[m.loop] != null && map[m.head] != null && map[m.tail] != null;
      })
      .map(function (m) {
        return { loop: map[m.loop], head: map[m.head], tail: map[m.tail] };
      });
    remapWireRoutes(function (from, to, kind) {
      var fOk = from === "start" || map[from] != null;
      if (!fOk || map[to] == null) return null;
      return { from: from === "start" ? "start" : map[from], to: map[to], kind: kind };
    });
    return true;
  }

  function pruneEmptyElseActions(list) {
    if (!list || !list.length) return false;
    return remapFilteredActions(list, function (i, a) {
      return !(a && a.type === "else" && kidsNoElse(list, i).length === 0);
    });
  }

  function siblingIfGroup(list, ifIdx) {
    if (!list[ifIdx] || list[ifIdx].type !== "if") {
      return { group: ifIdx >= 0 ? [ifIdx] : [], after: -1, end: ifIdx + 1 };
    }
    var indent = list[ifIdx].indent | 0;
    var group = [ifIdx];
    var j = subtreeEnd(ifIdx);
    while (j < list.length) {
      var ind = list[j].indent | 0;
      if (ind < indent) break;
      if (ind > indent) {
        j += 1;
        continue;
      }
      if (list[j].type === "else") {
        j = subtreeEnd(j);
        continue;
      }
      if (list[j].type === "if") {
        group.push(j);
        j = subtreeEnd(j);
        continue;
      }
      break;
    }
    var after = -1;
    if (j < list.length && (list[j].indent | 0) === indent && list[j].type !== "else") after = j;
    return { group: group, after: after, end: j };
  }

  function nodeW(a) {
    return CARD_W;
  }

  function loopBadgeText(a) {
    if (!a) return "循环";
    var n = a.loopCount | 0;
    if (n < 0) return "循环 · 无限";
    return "循环 · " + n + "次";
  }

  function estimateTextH(text, fontPx, lineH, innerW) {
    var s = String(text || "");
    if (!s) return lineH;
    var w = 0;
    var lines = 1;
    var i;
    for (i = 0; i < s.length; i++) {
      var code = s.charCodeAt(i);
      if (code === 10) {
        lines += 1;
        w = 0;
        continue;
      }
      w += code > 127 ? fontPx : fontPx * 0.56;
      if (w > innerW) {
        lines += 1;
        w = code > 127 ? fontPx : fontPx * 0.56;
      }
    }
    return Math.min(6, lines) * lineH;
  }

  function estimateCardH(list, i) {
    var a = list && list[i];
    if (!a) return CARD_H;
    if (typeof a._vh === "number" && a._vh > 16) return a._vh;
    var inner = CARD_W - 36;
    var title = cardTitleText(a, i);
    var th = estimateTextH(title, 12.5, 17, inner);
    var rh = estimateTextH((a.remark || "").trim(), 11, 15, inner);
    var h = Math.max(CARD_H, 24 + th + 4 + rh);
    if (isLoopHead(list, i)) h += BADGE_H;
    return h;
  }

  function visCardH(list, i) {
    return estimateCardH(list, i);
  }

  function nodeBox(list, idx) {
    if (idx === "start") {
      var s = startPos();
      return { l: s.x, t: s.y, r: s.x + CARD_W, b: s.y + CARD_H };
    }
    if (typeof idx !== "number" || !list[idx]) return null;
    if (isHiddenCard(list, idx)) return null;
    var p = cardPos(list[idx]);
    return { l: p.x, t: p.y, r: p.x + nodeW(list[idx]), b: p.y + estimateCardH(list, idx) };
  }

  function enterTarget(list, i) {
    if (i < 0 || i >= list.length || !list[i]) return i;
    if (list[i].type === "loop") {
      var kids = directChildren(list, i);
      return kids.length ? enterTarget(list, kids[0]) : i;
    }
    return i;
  }

  function spineTail(list, loopIdx) {
    var kids = directChildren(list, loopIdx);
    if (!kids.length) return -1;
    var last = kids[kids.length - 1];
    if (list[last] && list[last].type === "loop") return spineTail(list, last);
    return last;
  }

  function loopMarkByHead(head) {
    for (var i = 0; i < vis.loopMarks.length; i++) {
      if (vis.loopMarks[i].head === head) return vis.loopMarks[i];
    }
    return null;
  }

  function isLoopHead(list, i) {
    if (loopMarkByHead(i)) return true;
    if (i <= 0 || !list[i - 1] || list[i - 1].type !== "loop") return false;
    var kids = directChildren(list, i - 1);
    return kids.length && enterTarget(list, kids[0]) === i;
  }

  function isLoopTail(list, i) {
    for (var m = 0; m < vis.loopMarks.length; m++) {
      if (vis.loopMarks[m].tail === i) return true;
    }
    if (i < 0 || !list[i]) return false;
    for (var j = 0; j < list.length; j++) {
      if (list[j].type !== "loop") continue;
      if (spineTail(list, j) === i) return true;
    }
    return false;
  }

  function glueLoopToHead(loopIdx, headIdx) {
    var list = actions();
    if (!list[loopIdx] || !list[headIdx]) return;
    var hp = cardPos(list[headIdx]);
    list[loopIdx]._vx = hp.x;
    list[loopIdx]._vy = hp.y;
  }

  function loopMarkByLoop(idx) {
    for (var i = 0; i < vis.loopMarks.length; i++) {
      if (vis.loopMarks[i].loop === idx) return vis.loopMarks[i];
    }
    return null;
  }

  function isAbsorbedLoop(list, i) {
    if (!list[i] || list[i].type !== "loop") return false;
    if (directChildren(list, i).length) return true;
    return !!loopMarkByLoop(i);
  }

  function isHiddenCard(list, i) {
    if (!list[i]) return false;
    if (list[i].type === "else") return true;
    return isAbsorbedLoop(list, i);
  }

  function cycleMemberSet(head, tail) {
    var list = actions();
    var members = {};
    if (typeof head !== "number" || head < 0 || !list[head]) return members;
    if (typeof tail !== "number" || tail < 0 || !list[tail]) return members;
    var i;
    for (i = 0; i < list.length; i++) {
      if (!list[i] || list[i].type === "loop") continue;
      if (canReach(head, i) && canReach(i, tail)) members[i] = true;
    }
    members[head] = true;
    members[tail] = true;
    return members;
  }

  function isExternalFrom(from, members) {
    if (from === "start") return true;
    if (typeof from !== "number") return true;
    return !members[from];
  }

  function externalEntryTargets(members) {
    var seen = {};
    var targets = [];
    var i;
    for (i = 0; i < vis.links.length; i++) {
      var l = vis.links[i];
      var kind = l.kind || "seq";
      if (!isFlowKind(kind)) continue;
      if (!members[l.to]) continue;
      if (!isExternalFrom(l.from, members)) continue;
      if (seen[l.to]) continue;
      seen[l.to] = true;
      targets.push(l.to);
    }
    return targets;
  }

  function loopEntryBlockReason(from, to, kind) {
    kind = kind || "seq";
    if (kind === "call" || kind === "jump") return "";
    var mi;
    for (mi = 0; mi < vis.loopMarks.length; mi++) {
      var m = vis.loopMarks[mi];
      var members = cycleMemberSet(m.head, m.tail);
      if (kind === "back") {
        if (from === m.tail && to !== m.head) return "循环回边只能连到首节点";
        continue;
      }
      if (!members[to]) continue;
      if (isExternalFrom(from, members) && to !== m.head) return "外部动作只能连接到循环首节点";
    }
    return "";
  }

  function loopHeadBlockReason(head) {
    var list = actions();
    if (head < 0 || !list[head] || list[head].type === "loop") return "无法将循环卡片设为首节点";
    if (loopMarkByHead(head)) return "该节点已是循环首节点";
    var cyc = findCycleTail(head);
    if (!cyc) return "请先把循环体末尾连回该节点";
    var members = cycleMemberSet(head, cyc.tail);
    var mi;
    for (mi = 0; mi < vis.loopMarks.length; mi++) {
      var m = vis.loopMarks[mi];
      if (m.head === head) continue;
      var em = cycleMemberSet(m.head, m.tail);
      if (em[head] && members[m.head]) return "该闭环已有首节点";
    }
    var targets = externalEntryTargets(members);
    if (targets.length > 1) return "闭环存在多个切入点，请先只保留连到同一节点的外部连线";
    if (targets.length === 1 && targets[0] !== head) return "请将外部切入的节点设为首节点";
    return "";
  }

  function repairLoopEntries() {
    var changed = false;
    var mi;
    for (mi = 0; mi < vis.loopMarks.length; mi++) {
      var m = vis.loopMarks[mi];
      var members = cycleMemberSet(m.head, m.tail);
      var li;
      for (li = 0; li < vis.links.length; li++) {
        var l = vis.links[li];
        var kind = l.kind || "seq";
        if (kind === "back") {
          if (l.from === m.tail && l.to !== m.head) {
            l.to = m.head;
            changed = true;
          }
          continue;
        }
        if (!isFlowKind(kind) || !members[l.to] || l.to === m.head) continue;
        if (!isExternalFrom(l.from, members)) continue;
        l.to = m.head;
        changed = true;
      }
    }
    if (!changed) return false;
    var seen = {};
    vis.links = vis.links.filter(function (l) {
      var key = linkKey(l.from) + ">" + l.to + ">" + (l.kind || "seq");
      if (seen[key]) return false;
      seen[key] = true;
      return true;
    });
    return true;
  }

  function isOnCyclicChain(idx) {
    var list = actions();
    if (idx < 0 || !list[idx] || list[idx].type === "loop") return false;
    var stack = {};
    function dfs(i) {
      if (typeof i !== "number" || i < 0 || i >= list.length) return false;
      if (stack[i]) return i === idx;
      stack[i] = true;
      var li;
      for (li = 0; li < vis.links.length; li++) {
        var l = vis.links[li];
        if (l.from !== i) continue;
        var kind = l.kind || "seq";
        if (kind === "call" || kind === "jump") continue;
        if (l.to === idx) return true;
        if (dfs(l.to)) return true;
      }
      stack[i] = false;
      return false;
    }
    return dfs(idx);
  }

  function canReach(from, to, opts) {
    opts = opts || {};
    var seen = {};
    var q = [from];
    while (q.length) {
      var cur = q.pop();
      if (cur === to) return true;
      if (cur === "start" || seen[cur]) continue;
      seen[cur] = true;
      for (var i = 0; i < vis.links.length; i++) {
        var l = vis.links[i];
        if (l.from !== cur) continue;
        var kind = l.kind || "seq";
        if (kind === "back" || kind === "call" || kind === "jump") continue;
        q.push(l.to);
      }
    }
    return false;
  }

  function nextTopLevel(list, start) {
    var i = start + 1;
    while (i < list.length && (list[i].indent | 0) > 0) i += 1;
    if (i < list.length && isTopDeclContainer(list[i])) return i;
    return i;
  }

  function autoLayout(list) {
    if (!list || !list.length) {
      vis.start = { x: PAD, y: PAD };
      return;
    }
    var i = 0;
    var blockX = PAD;
    var blockY = PAD;
    var maxBlockBottom = PAD;
    while (i < list.length) {
      if (isTopDeclContainer(list[i])) {
        var end = subtreeEnd(i);
        layoutRange(list, i, end, blockX, blockY);
        var box = rangeBBox(list, i, end);
        blockX = box.r + WRAP_PAD + GAP_X;
        maxBlockBottom = Math.max(maxBlockBottom, box.b + WRAP_PAD);
        i = end;
      } else {
        i += 1;
      }
    }
    var mainY = maxBlockBottom > PAD ? maxBlockBottom + GAP_Y * 2 : PAD;
    vis.start = { x: PAD, y: mainY };
    var mainX = PAD + CARD_W + GAP_X;
    i = 0;
    while (i < list.length) {
      if (isTopDeclContainer(list[i])) {
        i = subtreeEnd(i);
        continue;
      }
      if ((list[i].indent | 0) !== 0) {
        i += 1;
        continue;
      }
      if (list[i].type === "else") {
        i = subtreeEnd(i);
        continue;
      }
      var endMain = nextTopLevel(list, i);
      layoutRange(list, i, endMain, mainX, mainY);
      var boxMain = rangeBBox(list, i, endMain);
      mainX = boxMain.r + GAP_X;
      i = endMain;
    }
  }

  function layoutIfCard(list, ii, stackX, cursorY) {
    list[ii]._vx = stackX;
    list[ii]._vy = cursorY;
    var elseIdx = findElseIndex(list, ii);
    var thenEnd = elseIdx >= 0 ? elseIdx : subtreeEnd(ii);
    var thenX = stackX + CARD_W + GAP_X;
    if (ii + 1 < thenEnd) layoutRange(list, ii + 1, thenEnd, thenX, cursorY);
    var rowBottom = rangeBBox(list, ii, thenEnd).b;
    if (elseIdx >= 0) {
      var elseEnd = subtreeEnd(elseIdx);
      var elseY = cursorY + estimateCardH(list, ii) + GAP_Y;
      list[elseIdx]._vx = stackX;
      list[elseIdx]._vy = cursorY;
      if (elseIdx + 1 < elseEnd) {
        layoutRange(list, elseIdx + 1, elseEnd, thenX, elseY);
      }
      rowBottom = Math.max(rowBottom, rangeBBox(list, elseIdx + 1, elseEnd).b);
    }
    return rowBottom;
  }

  function layoutRange(list, start, end, originX, originY) {
    var x = originX;
    var i = start;
    var baseIndent = list[start] ? list[start].indent | 0 : 0;
    var prevX = originX;
    var prevY = originY;
    var prevH = CARD_H;
    var stackUnder = false;
    while (i < end && i < list.length) {
      var indent = list[i].indent | 0;
      if (indent < baseIndent) break;
      if (indent > baseIndent) {
        i += 1;
        continue;
      }
      if (list[i].type === "else") {
        i = subtreeEnd(i);
        continue;
      }
      if (list[i].type === "loop") {
        var bodyEnd = subtreeEnd(i);
        var kids = directChildren(list, i);
        if (kids.length) layoutRange(list, kids[0], bodyEnd, x, originY);
        else x += CARD_W + GAP_X;
        var head = enterTarget(list, i);
        if (head !== i && list[head]) {
          list[i]._vx = cardPos(list[head]).x;
          list[i]._vy = cardPos(list[head]).y;
        } else {
          list[i]._vx = x;
          list[i]._vy = originY;
        }
        var box = rangeBBox(list, i, bodyEnd);
        x = box.r + GAP_X;
        i = bodyEnd;
        stackUnder = false;
      } else if (list[i].type === "if") {
        var g = siblingIfGroup(list, i);
        var stackX = stackUnder ? prevX : x;
        var cursorY = stackUnder ? prevY + prevH + GAP_Y : originY;
        var gi;
        for (gi = 0; gi < g.group.length; gi++) {
          cursorY = layoutIfCard(list, g.group[gi], stackX, cursorY) + GAP_Y;
        }
        var lastIf = g.group[g.group.length - 1];
        var lastElse = findElseIndex(list, lastIf);
        var lastEnd = lastElse >= 0 ? subtreeEnd(lastElse) : subtreeEnd(lastIf);
        var gBox = rangeBBox(list, g.group[0], lastEnd);
        x = Math.max(x, gBox.r + GAP_X);
        i = g.after >= 0 ? g.after : lastEnd;
        if (!stackUnder) {
          stackUnder = true;
          prevX = list[g.group[0]]._vx;
          prevY = list[g.group[0]]._vy;
          prevH = estimateCardH(list, g.group[0]);
        }
      } else if (isDeclContainerType(list[i].type)) {
        list[i]._vx = x + WRAP_PAD;
        list[i]._vy = originY + WRAP_PAD;
        var bEnd = subtreeEnd(i);
        if (i + 1 < bEnd) {
          layoutRange(list, i + 1, bEnd, x + WRAP_PAD, originY + WRAP_PAD + CARD_H + WRAP_PAD);
        }
        var bb = rangeBBox(list, i, bEnd);
        x = bb.r + WRAP_PAD + GAP_X;
        i = bEnd;
        stackUnder = false;
      } else {
        list[i]._vx = x;
        list[i]._vy = originY;
        prevX = x;
        prevY = originY;
        prevH = estimateCardH(list, i);
        stackUnder = true;
        x += CARD_W + GAP_X;
        i += 1;
      }
    }
  }

  function rangeBBox(list, start, end) {
    var l = 1e9;
    var t = 1e9;
    var r = 0;
    var b = 0;
    var any = false;
    for (var i = start; i < end && i < list.length; i++) {
      if (list[i].type === "loop" || list[i].type === "else") continue;
      var p = cardPos(list[i]);
      var w = nodeW(list[i]);
      var h = visCardH(list, i);
      any = true;
      l = Math.min(l, p.x);
      t = Math.min(t, p.y);
      r = Math.max(r, p.x + w);
      b = Math.max(b, p.y + h);
    }
    if (!any) return { l: 0, t: 0, r: CARD_W, b: CARD_H };
    return { l: l, t: t, r: r, b: b };
  }

  function visibleCardRect(list, i) {
    if (!list[i]) return null;
    if (isHiddenCard(list, i)) return null;
    var p = cardPos(list[i]);
    return { i: i, l: p.x, t: p.y, r: p.x + nodeW(list[i]), b: p.y + visCardH(list, i) };
  }

  function aabbOfRects(rects) {
    var l = 1e9;
    var t = 1e9;
    var r = 0;
    var b = 0;
    var i;
    for (i = 0; i < rects.length; i++) {
      l = Math.min(l, rects[i].l);
      t = Math.min(t, rects[i].t);
      r = Math.max(r, rects[i].r);
      b = Math.max(b, rects[i].b);
    }
    return { l: l, t: t, r: r, b: b };
  }

  function inflateRect(r, pad) {
    return { l: r.l - pad, t: r.t - pad, r: r.r + pad, b: r.b + pad };
  }

  function rectsOverlap(a, b) {
    return !(a.r <= b.l || b.r <= a.l || a.b <= b.t || b.b <= a.t);
  }

  function vOverlap(a, b) {
    return Math.min(a.b, b.b) - Math.max(a.t, b.t);
  }

  function foreignHitsBox(box, foreign, pad) {
    var hit = pad ? inflateRect(box, pad) : box;
    var i;
    for (i = 0; i < foreign.length; i++) {
      if (rectsOverlap(hit, foreign[i])) return true;
    }
    return false;
  }

  function collectWrapMemberRects(list, start, end) {
    var rects = [];
    var i;
    for (i = start; i < end && i < list.length; i++) {
      var r = visibleCardRect(list, i);
      if (r) rects.push(r);
    }
    return rects;
  }

  function collectForeignCardRects(list, start, end) {
    var rects = [];
    var i;
    for (i = 0; i < list.length; i++) {
      if (i >= start && i < end) continue;
      var r = visibleCardRect(list, i);
      if (r) rects.push(r);
    }
    return rects;
  }

  function groupRectsByRow(rects) {
    var rows = [];
    var sorted = rects.slice().sort(function (a, b) {
      return a.t - b.t || a.l - b.l;
    });
    var i;
    for (i = 0; i < sorted.length; i++) {
      var r = sorted[i];
      var k;
      var hit = -1;
      var best = 0;
      for (k = 0; k < rows.length; k++) {
        var ov = vOverlap(r, rows[k].band);
        var need = Math.min(r.b - r.t, rows[k].band.b - rows[k].band.t) * 0.45;
        if (ov > need && ov > best) {
          best = ov;
          hit = k;
        }
      }
      if (hit < 0) {
        rows.push({ band: { t: r.t, b: r.b }, rects: [r] });
      } else {
        rows[hit].rects.push(r);
        rows[hit].band.t = Math.min(rows[hit].band.t, r.t);
        rows[hit].band.b = Math.max(rows[hit].band.b, r.b);
      }
    }
    return rows;
  }

  function rowWrapSegments(memberRects, foreign) {
    var sorted = memberRects.slice().sort(function (a, b) {
      return a.l - b.l;
    });
    if (!sorted.length) return [];
    var segs = [];
    var cur = [sorted[0]];
    var i;
    for (i = 1; i < sorted.length; i++) {
      var joined = cur.concat([sorted[i]]);
      var box = aabbOfRects(joined);
      if (foreignHitsBox(box, foreign, WRAP_PAD)) {
        segs.push(aabbOfRects(cur));
        cur = [sorted[i]];
      } else {
        cur = joined;
      }
    }
    segs.push(aabbOfRects(cur));
    return segs;
  }

  function mergeWrapRects(rects, foreign) {
    var out = rects.slice();
    var changed = true;
    while (changed) {
      changed = false;
      var i;
      for (i = 0; i < out.length; i++) {
        var j;
        for (j = i + 1; j < out.length; j++) {
          var box = aabbOfRects([out[i], out[j]]);
          if (foreignHitsBox(box, foreign, WRAP_PAD)) continue;
          out.splice(j, 1);
          out[i] = box;
          changed = true;
          break;
        }
        if (changed) break;
      }
    }
    return out;
  }

  function tightWrapRects(list, start, end) {
    var members = collectWrapMemberRects(list, start, end);
    if (!members.length) return [];
    var foreign = collectForeignCardRects(list, start, end);
    var all = aabbOfRects(members);
    if (!foreignHitsBox(all, foreign, WRAP_PAD)) return [all];
    var rows = groupRectsByRow(members);
    var out = [];
    var ri;
    for (ri = 0; ri < rows.length; ri++) {
      var segs = rowWrapSegments(rows[ri].rects, foreign);
      var si;
      for (si = 0; si < segs.length; si++) {
        var s = segs[si];
        if (s.r - s.l < 8 || s.b - s.t < 8) continue;
        out.push(s);
      }
    }
    return mergeWrapRects(out, foreign);
  }

  function wrapBoxes(list) {
    var boxes = [];
    var loopOn = otherPref("visualLoopWrap", true);
    var blockOn = otherPref("visualBlockWrap", true);
    var watchOn = otherPref("visualWatchWrap", true);
    var ifOn = otherPref("visualIfWrap", true);
    var i;
    for (i = 0; i < list.length; i++) {
      var t = list[i].type;
      var allow = false;
      if (t === "loop") allow = loopOn;
      else if (t === "defineBlock") allow = blockOn;
      else if (t === "watchImage") allow = watchOn;
      else if (t === "if") allow = ifOn;
      if (t === "else") continue;
      if (!allow) continue;
      var end = subtreeEnd(i);
      var indent = list[i].indent | 0;
      if ((t === "loop" || t === "if" || t === "else") && end <= i + 1) {
        if (t === "loop") {
          var emptyMark = loopMarkByLoop(i);
          if (emptyMark) {
            var members = cycleMemberSet(emptyMark.head, emptyMark.tail);
            var memberRects = [];
            var mk;
            for (mk in members) {
              if (!Object.prototype.hasOwnProperty.call(members, mk)) continue;
              var vr = visibleCardRect(list, mk | 0);
              if (vr) memberRects.push(vr);
            }
            if (memberRects.length) {
              var bb = aabbOfRects(memberRects);
              boxes.push({
                i: i,
                type: t,
                indent: indent,
                l: bb.l - WRAP_PAD,
                t: bb.t - WRAP_PAD,
                w: bb.r - bb.l + WRAP_PAD * 2,
                h: bb.b - bb.t + WRAP_PAD * 2,
              });
            }
          }
        }
        continue;
      }
      var rects = tightWrapRects(list, i, end);
      if (!rects.length) continue;
      var ri;
      for (ri = 0; ri < rects.length; ri++) {
        var r = rects[ri];
        boxes.push({
          i: i,
          type: t,
          indent: indent,
          l: r.l - WRAP_PAD,
          t: r.t - WRAP_PAD,
          w: r.r - r.l + WRAP_PAD * 2,
          h: r.b - r.t + WRAP_PAD * 2,
        });
      }
    }
    boxes.sort(function (a, b) {
      if (a.indent !== b.indent) return a.indent - b.indent;
      return a.i - b.i;
    });
    vis._wrapBorderXs = [];
    var wi;
    for (wi = 0; wi < boxes.length; wi++) {
      vis._wrapBorderXs.push(boxes[wi].l);
      vis._wrapBorderXs.push(boxes[wi].l + boxes[wi].w);
    }
    return boxes;
  }

  var PORT_Y = 36;

  function viewMargin() {
    var el = scrollEl();
    return {
      x: el ? Math.max(280, Math.ceil(el.clientWidth / vis.zoom)) : 480,
      y: el ? Math.max(200, Math.ceil(el.clientHeight / vis.zoom)) : 360,
    };
  }

  function contentExtent(list) {
    var l = vis.start.x;
    var t = vis.start.y;
    var r = vis.start.x + CARD_W;
    var b = vis.start.y + CARD_H;
    var i;
    for (i = 0; i < list.length; i++) {
      if (isHiddenCard(list, i)) continue;
      var p = cardPos(list[i]);
      l = Math.min(l, p.x);
      t = Math.min(t, p.y);
      r = Math.max(r, p.x + nodeW(list[i]));
      b = Math.max(b, p.y + visCardH(list, i));
    }
    var wraps = wrapBoxes(list);
    for (i = 0; i < wraps.length; i++) {
      l = Math.min(l, wraps[i].l);
      t = Math.min(t, wraps[i].t);
      r = Math.max(r, wraps[i].l + wraps[i].w);
      b = Math.max(b, wraps[i].t + wraps[i].h);
    }
    return { l: l, t: t, r: r, b: b };
  }

  function shiftWorld(dx, dy) {
    dx = dx || 0;
    dy = dy || 0;
    if (!dx && !dy) return;
    vis.start.x += dx;
    vis.start.y += dy;
    var list = actions();
    var i;
    for (i = 0; i < list.length; i++) {
      if (typeof list[i]._vx === "number") list[i]._vx += dx;
      if (typeof list[i]._vy === "number") list[i]._vy += dy;
    }
    if (vis.wireRoutes) {
      Object.keys(vis.wireRoutes).forEach(function (k) {
        var pts = vis.wireRoutes[k];
        if (!pts) return;
        vis.wireRoutes[k] = pts.map(function (p) {
          return [p[0] + dx, p[1] + dy];
        });
      });
    }
    if (vis.drag) {
      if (typeof vis.drag.startX === "number") vis.drag.startX += dx;
      if (typeof vis.drag.startY === "number") vis.drag.startY += dy;
      if (typeof vis.drag.ox === "number") vis.drag.ox += dx;
      if (typeof vis.drag.oy === "number") vis.drag.oy += dy;
      if (vis.drag.from && typeof vis.drag.from.x === "number") {
        vis.drag.from.x += dx;
        vis.drag.from.y += dy;
      }
      if (vis.drag.cur && typeof vis.drag.cur.x === "number") {
        vis.drag.cur.x += dx;
        vis.drag.cur.y += dy;
      }
      if (vis.drag.pts) {
        vis.drag.pts.forEach(function (p) {
          p[0] += dx;
          p[1] += dy;
        });
      }
      if (vis.drag.origins) {
        Object.keys(vis.drag.origins).forEach(function (k) {
          vis.drag.origins[k].x += dx;
          vis.drag.origins[k].y += dy;
        });
      }
    }
    var el = scrollEl();
    if (el) {
      el.scrollLeft += dx * vis.zoom;
      el.scrollTop += dy * vis.zoom;
    }
  }

  function padWorldForView(list) {
    var m = viewMargin();
    var minX = vis.start.x;
    var minY = vis.start.y;
    var i;
    for (i = 0; i < list.length; i++) {
      if (isHiddenCard(list, i)) continue;
      var p = cardPos(list[i]);
      minX = Math.min(minX, p.x);
      minY = Math.min(minY, p.y);
    }
    var dx = minX < m.x ? m.x - minX : 0;
    var dy = minY < m.y ? m.y - minY : 0;
    if (dx || dy) shiftWorld(dx, dy);
  }

  function contentSize(list) {
    var m = viewMargin();
    var box = contentExtent(list);
    return {
      w: Math.max(WORLD_MIN_W, box.r + m.x),
      h: Math.max(WORLD_MIN_H, box.b + m.y + BACK_PAD),
    };
  }

  function pulseZoomHud() {
    ensureHud();
    var hud = $("#visualHud");
    if (!hud) return;
    hud.textContent = Math.round(vis.zoom * 100) + "%";
    hud.classList.add("show");
    if (vis.hudHideTimer) clearTimeout(vis.hudHideTimer);
    vis.hudHideTimer = setTimeout(function () {
      hud.classList.remove("show");
      vis.hudHideTimer = null;
    }, 1400);
  }

  function applyZoomTransform() {
    var world = worldEl();
    var sizer = sizerEl();
    var list = actions();
    padWorldForView(list);
    var size = contentSize(list);
    if (world) {
      world.style.width = size.w + "px";
      world.style.height = size.h + "px";
      world.style.transform = "scale(" + vis.zoom + ")";
    }
    if (sizer) {
      sizer.style.width = Math.max(size.w * vis.zoom, 1) + "px";
      sizer.style.height = Math.max(size.h * vis.zoom, 1) + "px";
    }
    var edges = $("#visualEdges");
    if (edges) {
      edges.setAttribute("width", String(size.w));
      edges.setAttribute("height", String(size.h));
      edges.style.width = size.w + "px";
      edges.style.height = size.h + "px";
    }
  }

  function clientToWorld(clientX, clientY) {
    var world = worldEl();
    if (world) {
      var wr = world.getBoundingClientRect();
      if (wr.width > 0 && wr.height > 0) {
        return {
          x: (clientX - wr.left) / vis.zoom,
          y: (clientY - wr.top) / vis.zoom,
        };
      }
    }
    var el = scrollEl();
    if (!el) return { x: 0, y: 0 };
    var r = el.getBoundingClientRect();
    return {
      x: (el.scrollLeft + (clientX - r.left - el.clientLeft)) / vis.zoom,
      y: (el.scrollTop + (clientY - r.top - el.clientTop)) / vis.zoom,
    };
  }

  function viewportCenterWorld() {
    var el = scrollEl();
    if (!el) return { x: PAD, y: PAD };
    var r = el.getBoundingClientRect();
    return clientToWorld(r.left + r.width / 2, r.top + r.height / 2);
  }

  function setZoomAt(next, clientX, clientY) {
    var el = scrollEl();
    if (!el) return;
    next = clamp(next, ZOOM_MIN, ZOOM_MAX);
    var r = el.getBoundingClientRect();
    var cx = clientX - r.left;
    var cy = clientY - r.top;
    var wx = (el.scrollLeft + cx) / vis.zoom;
    var wy = (el.scrollTop + cy) / vis.zoom;
    var prev = vis.zoom;
    vis.zoom = next;
    applyZoomTransform();
    el.scrollLeft = wx * vis.zoom - cx;
    el.scrollTop = wy * vis.zoom - cy;
    if (Math.round(prev * 100) !== Math.round(vis.zoom * 100)) pulseZoomHud();
  }

  function panBy(dx, dy) {
    var el = scrollEl();
    if (!el) return;
    el.scrollLeft += dx;
    el.scrollTop += dy;
  }

  function maybeAutoPan(ev) {
    var el = scrollEl();
    if (!el) return;
    var r = el.getBoundingClientRect();
    var m = 36;
    var spd = 14;
    var dx = 0;
    var dy = 0;
    if (ev.clientX < r.left + m) dx = -spd;
    else if (ev.clientX > r.right - m) dx = spd;
    if (ev.clientY < r.top + m) dy = -spd;
    else if (ev.clientY > r.bottom - m) dy = spd;
    if (dx || dy) panBy(dx, dy);
  }

  function fitView() {
    var el = scrollEl();
    var list = actions();
    if (!el) {
      vis.zoom = 1;
      applyZoomTransform();
      return;
    }
    var box = list.length ? rangeBBox(list, 0, list.length) : { l: vis.start.x, t: vis.start.y, r: vis.start.x + CARD_W, b: vis.start.y + CARD_H };
    box.l = Math.min(box.l, vis.start.x);
    box.t = Math.min(box.t, vis.start.y);
    box.r = Math.max(box.r, vis.start.x + CARD_W);
    box.b = Math.max(box.b, vis.start.y + CARD_H);
    var wraps = wrapBoxes(list);
    for (var i = 0; i < wraps.length; i++) {
      box.l = Math.min(box.l, wraps[i].l);
      box.t = Math.min(box.t, wraps[i].t);
      box.r = Math.max(box.r, wraps[i].l + wraps[i].w);
      box.b = Math.max(box.b, wraps[i].t + wraps[i].h);
    }
    var vw = Math.max(80, el.clientWidth - 48);
    var vh = Math.max(80, el.clientHeight - 48);
    var bw = Math.max(1, box.r - box.l + PAD);
    var bh = Math.max(1, box.b - box.t + PAD);
    var prev = vis.zoom;
    vis.zoom = clamp(Math.min(vw / bw, vh / bh), ZOOM_MIN, 1.2);
    applyZoomTransform();
    el.scrollLeft = Math.max(0, box.l * vis.zoom - (el.clientWidth - (box.r - box.l) * vis.zoom) / 2);
    el.scrollTop = Math.max(0, box.t * vis.zoom - (el.clientHeight - (box.b - box.t) * vis.zoom) / 2);
    if (Math.round(prev * 100) !== Math.round(vis.zoom * 100)) pulseZoomHud();
  }

  function centerOnWorld(wx, wy) {
    var el = scrollEl();
    if (!el) return;
    el.scrollLeft = wx * vis.zoom - el.clientWidth / 2;
    el.scrollTop = wy * vis.zoom - el.clientHeight / 2;
  }

  function centerOnStart() {
    var p = startPos();
    centerOnWorld(p.x + CARD_W / 2, p.y + CARD_H / 2);
  }

  function centerOnCard(i) {
    var list = actions();
    if (!list[i]) {
      centerOnStart();
      return;
    }
    if (list[i].type === "loop") {
      var kids = directChildren(list, i);
      if (kids.length) i = enterTarget(list, kids[0]);
      else {
        var loopM = loopMarkByLoop(i);
        if (loopM) i = loopM.head;
      }
    }
    if (!list[i] || list[i].type === "loop") {
      centerOnStart();
      return;
    }
    var p = cardPos(list[i]);
    centerOnWorld(p.x + nodeW(list[i]) / 2, p.y + visCardH(list, i) / 2);
  }

  function applyEntryView() {
    vis.zoom = 1;
    vis.pendingScroll = null;
    vis.skipAutoFit = true;
    applyZoomTransform();
    var target = vis.forceView;
    vis.forceView = null;
    function go() {
      if (target === "start" || target == null || target === "") centerOnStart();
      else centerOnCard(target | 0);
      vis.skipAutoFit = false;
    }
    requestAnimationFrame(go);
  }

  function geomEdgePorts(list, ln) {
    var kind = ln.kind || "seq";
    var fromPort = "out";
    if (kind === "back") fromPort = "outBack";
    else if (kind === "ifTrue") fromPort = "ifTrue";
    else if (kind === "ifFalse" || kind === "fail") fromPort = "ifFalse";
    else if (kind === "guard") {
      fromPort = list[ln.from] && list[ln.from].type === "if" ? "ifTrue" : "out";
    } else if (isLoopTail(list, ln.from) && kind === "seq") fromPort = "outExit";
    var toPort = "in";
    if (kind === "back") toPort = "inBack";
    else if (isLoopHead(list, ln.to) && (kind === "seq" || kind === "guard")) toPort = "inEnter";
    var p1 = ln.from === "start" ? portPointStart("out") : list[ln.from] ? portPoint(list[ln.from], fromPort) : null;
    var p2 = list[ln.to] ? portPoint(list[ln.to], toPort) : null;
    return { p1: p1, p2: p2, kind: kind };
  }

  function alignHorizontalWires(list) {
    if (!list || !vis.links || !vis.links.length) return false;
    var shifted = {};
    var changed = false;
    var q = ["start"];
    var seen = { start: true };
    var guard = 0;
    while (q.length && guard++ < 8000) {
      var cur = q.shift();
      var li;
      for (li = 0; li < vis.links.length; li++) {
        var ln = vis.links[li];
        if (ln.from !== cur) continue;
        var kind = ln.kind || "seq";
        if (kind === "back" || kind === "jump" || kind === "call") continue;
        if (!seen[ln.to]) {
          seen[ln.to] = true;
          q.push(ln.to);
        }
        if (shifted[ln.to] || !list[ln.to] || list[ln.to].type === "loop") continue;
        var ports = geomEdgePorts(list, ln);
        if (!ports || !ports.p1 || !ports.p2) continue;
        if (ports.p2.x <= ports.p1.x + 20) continue;
        var dy = ports.p1.y - ports.p2.y;
        if (Math.abs(dy) < 0.5 || Math.abs(dy) > 48) continue;
        var srcTop = ln.from === "start" ? vis.start.y : cardPos(list[ln.from]).y;
        var dstTop = cardPos(list[ln.to]).y;
        if (Math.abs(srcTop - dstTop) > 56) continue;
        list[ln.to]._vy = (list[ln.to]._vy || 0) + dy;
        shifted[ln.to] = true;
        changed = true;
        var mark = loopMarkByHead(ln.to);
        if (mark) glueLoopToHead(mark.loop, mark.head);
      }
    }
    return changed;
  }

  function visualSelectedIndices() {
    var st = state();
    var out = [];
    var i;
    if (st.batchMode && st.batchSel) {
      for (i = 0; i < actions().length; i++) {
        if (st.batchSel[i]) out.push(i);
      }
      if (out.length >= 2) return out;
    }
    if (st.actionSel >= 0) return [st.actionSel];
    return out;
  }

  function isCardSelected(i) {
    var st = state();
    if (st.batchMode && st.batchSel && st.batchSel[i]) return true;
    return st.actionSel === i;
  }

  function startPos() {
    return { x: vis.start.x | 0, y: vis.start.y | 0 };
  }

  function portPoint(a, which) {
    var p = cardPos(a);
    var w = nodeW(a);
    var list = actions();
    var idx = list.indexOf(a);
    var badge = idx >= 0 && isLoopHead(list, idx) ? BADGE_H : 0;
    var h = idx >= 0 ? visCardH(list, idx) : CARD_H + badge;
    var body = Math.max(24, h - badge);
    var y0 = p.y + badge;
    var mid = y0 + Math.min(PORT_Y, body * 0.5);
    var hi = y0 + body * 0.3;
    var lo = y0 + body * 0.7;
    if (which === "inEnter") return { x: p.x, y: hi };
    if (which === "inBack") return { x: p.x, y: lo };
    if (which === "outBack") return { x: p.x + w, y: lo };
    if (which === "outExit" || which === "ifTrue") return { x: p.x + w, y: hi };
    if (which === "ifFalse") return { x: p.x + w, y: lo };
    if (which === "in") return { x: p.x, y: mid };
    return { x: p.x + w, y: mid };
  }

  function portPointStart(which) {
    var p = startPos();
    var y = p.y + CARD_H / 2;
    if (which === "in") return { x: p.x, y: y };
    return { x: p.x + CARD_W, y: y };
  }

  function readDomPort(idx, dataPort) {
    var wrap = $("#visualCards");
    if (!wrap) return null;
    var sel = idx === "start" ? '.vcard[data-i="start"]' : '.vcard[data-i="' + idx + '"]';
    var card = wrap.querySelector(sel);
    if (!card) return null;
    var port = dataPort ? card.querySelector('.vport[data-port="' + dataPort + '"]') : null;
    if (!port) {
      if (
        dataPort === "out" ||
        dataPort === "outExit" ||
        dataPort === "outBack" ||
        dataPort === "ifTrue" ||
        dataPort === "ifFalse"
      ) {
        port = card.querySelector(".vport.out");
      } else {
        port = card.querySelector(".vport.in");
      }
    }
    if (!port) return null;
    var r = port.getBoundingClientRect();
    if (r.width < 0.5 && r.height < 0.5) return null;
    return clientToWorld(r.left + r.width / 2, r.top + r.height / 2);
  }

  function portOf(idx, which) {
    var d = readDomPort(idx, which);
    if (d) return d;
    if (idx === "start") return portPointStart(which === "in" ? "in" : "out");
    var list = actions();
    if (!list[idx]) return null;
    return portPoint(list[idx], which);
  }

  function wireBoxes(list) {
    var boxes = [];
    var i;
    for (i = 0; i < list.length; i++) {
      if (isHiddenCard(list, i)) continue;
      var p = cardPos(list[i]);
      boxes.push({
        l: p.x,
        t: p.y,
        r: p.x + nodeW(list[i]),
        b: p.y + visCardH(list, i),
      });
    }
    var sp = startPos();
    boxes.push({ l: sp.x, t: sp.y, r: sp.x + CARD_W, b: sp.y + CARD_H });
    return boxes;
  }

  function inflateBox(b, pad) {
    if (!b) return null;
    pad = pad == null ? BOX_PAD : pad;
    return { l: b.l - pad, t: b.t - pad, r: b.r + pad, b: b.b + pad };
  }

  function sameBox(a, b) {
    return !!(a && b && Math.abs(a.l - b.l) < 1 && Math.abs(a.t - b.t) < 1 && Math.abs(a.r - b.r) < 1 && Math.abs(a.b - b.b) < 1);
  }

  function segHitsOne(x1, y1, x2, y2, b) {
    if (!b) return false;
    var horiz = Math.abs(y1 - y2) < 1;
    if (horiz) {
      if (y1 <= b.t || y1 >= b.b) return false;
      return Math.max(x1, x2) > b.l && Math.min(x1, x2) < b.r;
    }
    if (x1 <= b.l || x1 >= b.r) return false;
    return Math.max(y1, y2) > b.t && Math.min(y1, y2) < b.b;
  }

  function segHitsBoxes(x1, y1, x2, y2, boxes, ignoreA, ignoreB) {
    var i;
    for (i = 0; i < boxes.length; i++) {
      var b = boxes[i];
      if (sameBox(b, ignoreA) || sameBox(b, ignoreB)) continue;
      if (segHitsOne(x1, y1, x2, y2, b)) return true;
    }
    return false;
  }

  function segsOverlapH(y, x1, x2, lanes) {
    var a = Math.min(x1, x2);
    var c = Math.max(x1, x2);
    var i;
    for (i = 0; i < lanes.h.length; i++) {
      var s = lanes.h[i];
      if (Math.abs(s.y - y) < LANE * 0.7 && Math.min(s.b, c) - Math.max(s.a, a) > 6) return true;
    }
    return false;
  }

  function segsOverlapV(x, y1, y2, lanes) {
    var a = Math.min(y1, y2);
    var c = Math.max(y1, y2);
    var i;
    for (i = 0; i < lanes.v.length; i++) {
      var s = lanes.v[i];
      if (Math.abs(s.x - x) < LANE * 0.7 && Math.min(s.b, c) - Math.max(s.a, a) > 6) return true;
    }
    return false;
  }

  function pathHitsCards(pts, boxes, srcBox, dstBox) {
    var i;
    for (i = 0; i < pts.length - 1; i++) {
      var ignA = i === 0 ? srcBox : null;
      var ignB = i === pts.length - 2 ? dstBox : null;
      if (segHitsBoxes(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], boxes, ignA, ignB)) return true;
    }
    return false;
  }

  function pathLanePenalty(pts, lanes) {
    var n = 0;
    var i;
    for (i = 1; i < pts.length - 2; i++) {
      var ax = pts[i][0];
      var ay = pts[i][1];
      var bx = pts[i + 1][0];
      var by = pts[i + 1][1];
      if (Math.abs(ay - by) < 1 && segsOverlapH(ay, ax, bx, lanes)) n += Math.abs(bx - ax);
      if (Math.abs(ax - bx) < 1 && segsOverlapV(ax, ay, by, lanes)) n += Math.abs(by - ay);
    }
    return n;
  }

  function pathLen(pts) {
    var n = 0;
    var i;
    for (i = 0; i < pts.length - 1; i++) {
      n += Math.abs(pts[i + 1][0] - pts[i][0]) + Math.abs(pts[i + 1][1] - pts[i][1]);
    }
    return n;
  }

  function pathClear(pts, boxes, lanes, srcBox, dstBox) {
    if (pathHitsCards(pts, boxes, srcBox, dstBox)) return false;
    var i;
    for (i = 1; i < pts.length - 2; i++) {
      var ax = pts[i][0];
      var ay = pts[i][1];
      var bx = pts[i + 1][0];
      var by = pts[i + 1][1];
      if (Math.abs(ay - by) < 1 && segsOverlapH(ay, ax, bx, lanes)) return false;
      if (Math.abs(ax - bx) < 1 && segsOverlapV(ax, ay, by, lanes)) return false;
    }
    return true;
  }

  function commitPath(pts, lanes) {
    var i;
    var d = "M " + pts[0][0] + " " + pts[0][1];
    for (i = 0; i < pts.length - 1; i++) {
      var ax = pts[i][0];
      var ay = pts[i][1];
      var bx = pts[i + 1][0];
      var by = pts[i + 1][1];
      if (Math.abs(ay - by) < 1) lanes.h.push({ y: ay, a: Math.min(ax, bx), b: Math.max(ax, bx) });
      else lanes.v.push({ x: ax, a: Math.min(ay, by), b: Math.max(ay, by) });
      d += " L " + bx + " " + by;
    }
    return d;
  }

  function nearWrapX(x, lanes) {
    var xs = lanes && lanes.wrapX;
    if (!xs) return false;
    var i;
    for (i = 0; i < xs.length; i++) {
      if (Math.abs((xs[i] | 0) - x) < WRAP_EDGE) return true;
    }
    return false;
  }

  function pickHLane(y0, x1, x2, boxes, lanes, srcBox, dstBox) {
    var yBase = snap(y0);
    var n;
    for (n = 0; n <= 48; n++) {
      var cand = n === 0 ? [yBase] : [yBase + n * LANE, yBase - n * LANE];
      var c;
      for (c = 0; c < cand.length; c++) {
        var y = cand[c];
        if (!segHitsBoxes(x1, y, x2, y, boxes, srcBox, dstBox) && !segsOverlapH(y, x1, x2, lanes)) return y;
      }
    }
    return yBase;
  }

  function pickVLane(x0, y1, y2, boxes, lanes, srcBox, dstBox, preferDir) {
    var xBase = snap(x0);
    var dir = preferDir || 0;
    var n;
    for (n = 0; n <= 48; n++) {
      var cand;
      if (n === 0) cand = [xBase];
      else if (dir < 0) cand = [xBase - n * LANE, xBase + n * LANE];
      else cand = [xBase + n * LANE, xBase - n * LANE];
      var c;
      for (c = 0; c < cand.length; c++) {
        var x = cand[c];
        if (nearWrapX(x, lanes) && n > 0) continue;
        if (!segHitsBoxes(x, y1, x, y2, boxes, srcBox, dstBox) && !segsOverlapV(x, y1, y2, lanes)) return x;
      }
    }
    return xBase;
  }

  function stackedGapY(src, dst) {
    if (!src || !dst) return null;
    if (dst.t >= src.b - 1 && dst.t - src.b >= 14) return (src.b + dst.t) / 2;
    if (src.t >= dst.b - 1 && src.t - dst.b >= 14) return (dst.b + src.t) / 2;
    return null;
  }

  function aroundY(dir, y1, y2, s1x, s2x, boxes) {
    var y = dir > 0 ? Math.max(y1, y2) + 24 : Math.min(y1, y2) - 24;
    var xL = Math.min(s1x, s2x);
    var xR = Math.max(s1x, s2x);
    var i;
    for (i = 0; i < boxes.length; i++) {
      var b = boxes[i];
      if (b.r < xL - 6 || b.l > xR + 6) continue;
      if (dir > 0) y = Math.max(y, b.b + 16);
      else y = Math.min(y, b.t - 16);
    }
    return y;
  }

  function makeOrthoPts(raw) {
    var out = [];
    var i;
    for (i = 0; i < raw.length; i++) {
      var p = [raw[i][0], raw[i][1]];
      if (!out.length) {
        out.push(p);
        continue;
      }
      var prev = out[out.length - 1];
      if (Math.abs(prev[0] - p[0]) < 1 && Math.abs(prev[1] - p[1]) < 1) continue;
      if (Math.abs(prev[0] - p[0]) >= 1 && Math.abs(prev[1] - p[1]) >= 1) {
        out.push([p[0], prev[1]]);
      }
      out.push(p);
    }
    return out;
  }

  function routeManhattanPts(x1, y1, x2, y2, kind, boxes, lanes, srcBox, dstBox) {
    boxes = boxes || [];
    lanes = lanes || { h: [], v: [] };
    if (!lanes.wrapX) lanes.wrapX = vis._wrapBorderXs || [];
    var inflated = [];
    var i;
    for (i = 0; i < boxes.length; i++) inflated.push(inflateBox(boxes[i], BOX_PAD));
    var srcI = inflateBox(srcBox, BOX_PAD);
    var dstI = inflateBox(dstBox, BOX_PAD);
    var reverse = kind === "back" || x2 < x1 - 8;
    var s1x = x1 + STUB;
    var s2x = x2 - STUB;
    var p1 = [x1, y1];
    var p2 = [x2, y2];
    var s1 = [s1x, y1];
    var s2 = [s2x, y2];
    var cands = [];

    function consider(raw) {
      var pts = makeOrthoPts(raw);
      if (pts.length < 2) return;
      if (pathHitsCards(pts, inflated, srcI, dstI)) return;
      cands.push(pts);
    }

    function bestCand() {
      if (!cands.length) return null;
      cands.sort(function (a, b) {
        var la = pathLen(a);
        var lb = pathLen(b);
        if (Math.abs(la - lb) > 4) return la - lb;
        var pa = pathLanePenalty(a, lanes);
        var pb = pathLanePenalty(b, lanes);
        if (pa !== pb) return pa - pb;
        return a.length - b.length;
      });
      return cands[0];
    }

    if (Math.abs(y1 - y2) < 2 && !reverse && s1x <= s2x + 8) {
      consider([p1, p2]);
      consider([p1, s1, s2, p2]);
    }
    if (!reverse && s1x <= s2x + 24) {
      consider([p1, s1, [s1x, y2], s2, p2]);
      consider([p1, s1, [s2x, y1], s2, p2]);
      consider([p1, s1, [Math.round((s1x + s2x) / 2), y1], [Math.round((s1x + s2x) / 2), y2], s2, p2]);
      var k;
      for (k = 0; k <= 8; k++) {
        consider([p1, s1, [s1x + k * 8, y1], [s1x + k * 8, y2], s2, p2]);
      }
    } else {
      consider([p1, s1, [Math.min(s1x, s2x) - 24, y1], [Math.min(s1x, s2x) - 24, y2], s2, p2]);
    }

    var hit = bestCand();
    if (hit) return hit;

    var gapY = stackedGapY(srcBox, dstBox);
    if (gapY != null) {
      consider([p1, s1, [s1x, y1], [s1x, gapY], [s2x, gapY], [s2x, y2], s2, p2]);
      hit = bestCand();
      if (hit) return hit;
    }

    var yBot = aroundY(1, y1, y2, s1x, s2x, inflated);
    var yTop = aroundY(-1, y1, y2, s1x, s2x, inflated);
    consider([p1, s1, [s1x, y1], [s1x, yTop], [s2x, yTop], [s2x, y2], s2, p2]);
    consider([p1, s1, [s1x, y1], [s1x, yBot], [s2x, yBot], [s2x, y2], s2, p2]);
    hit = bestCand();
    if (hit) return hit;

    var aroundTop = makeOrthoPts([p1, s1, [s1x, y1], [s1x, yTop], [s2x, yTop], [s2x, y2], s2, p2]);
    var aroundBot = makeOrthoPts([p1, s1, [s1x, y1], [s1x, yBot], [s2x, yBot], [s2x, y2], s2, p2]);
    if (y2 < y1) return pathLen(aroundTop) <= pathLen(aroundBot) ? aroundTop : aroundBot;
    return pathLen(aroundBot) <= pathLen(aroundTop) ? aroundBot : aroundTop;
  }

  function routeManhattan(x1, y1, x2, y2, kind, boxes, lanes, srcBox, dstBox) {
    var pts = routeManhattanPts(x1, y1, x2, y2, kind, boxes, lanes, srcBox, dstBox);
    return commitPath(pts, lanes || { h: [], v: [] });
  }

  function routeKey(ln) {
    return linkKey(ln.from) + ">" + ln.to + ">" + (ln.kind || "seq");
  }

  function forgetRoutesTouching(ids) {
    if (!vis.wireRoutes) return;
    var set = {};
    var i;
    for (i = 0; i < ids.length; i++) set[ids[i]] = true;
    Object.keys(vis.wireRoutes).forEach(function (k) {
      var p = parseRouteKey(k);
      if (!p) return;
      if (set[p.from] || set[p.to] || (p.from === "start" && set.start)) delete vis.wireRoutes[k];
    });
  }

  function parseRouteKey(k) {
    var sp = String(k || "").split(">");
    if (sp.length < 3) return null;
    var kind = sp[sp.length - 1];
    var to = sp[sp.length - 2] | 0;
    var from = sp.slice(0, sp.length - 2).join(">");
    if (from !== "start") from = from | 0;
    return { from: from, to: to, kind: kind };
  }

  function remapWireRoutes(mapFn) {
    var src = vis.wireRoutes || {};
    var next = {};
    Object.keys(src).forEach(function (k) {
      var p = parseRouteKey(k);
      if (!p) return;
      var n = mapFn(p.from, p.to, p.kind);
      if (!n) return;
      next[routeKey(n)] = src[k];
    });
    vis.wireRoutes = next;
  }

  function customCornersToPts(p1, p2, corners) {
    var raw = [[p1.x, p1.y]];
    var i;
    for (i = 0; i < corners.length; i++) {
      if (Array.isArray(corners[i]) && corners[i].length >= 2) raw.push([corners[i][0], corners[i][1]]);
    }
    raw.push([p2.x, p2.y]);
    return makeOrthoPts(raw);
  }

  function interiorCorners(pts) {
    if (!pts || pts.length < 3) return [];
    return pts.slice(1, pts.length - 1).map(function (p) {
      return [Math.round(p[0]), Math.round(p[1])];
    });
  }

  function routeLink(p1, p2, kind, boxes, lanes, srcBox, dstBox, custom) {
    var auto = routeManhattanPts(p1.x, p1.y, p2.x, p2.y, kind, boxes, lanes, srcBox, dstBox);
    if (custom && custom.length) {
      var pts = customCornersToPts(p1, p2, custom);
      if (!pathHitsCards(pts, boxes, srcBox, dstBox) && pathLen(pts) <= pathLen(auto) * 1.35 + 48) {
        return { pts: pts, d: commitPath(pts, lanes) };
      }
    }
    return { pts: auto, d: commitPath(auto, lanes) };
  }

  function nearestSeg(pts, x, y) {
    var best = 0;
    var bestD = 1e15;
    var i;
    for (i = 0; i < pts.length - 1; i++) {
      var ax = pts[i][0];
      var ay = pts[i][1];
      var bx = pts[i + 1][0];
      var by = pts[i + 1][1];
      var dx = bx - ax;
      var dy = by - ay;
      var len2 = dx * dx + dy * dy || 1;
      var t = clamp(((x - ax) * dx + (y - ay) * dy) / len2, 0, 1);
      var px = ax + t * dx;
      var py = ay + t * dy;
      var d = (x - px) * (x - px) + (y - py) * (y - py);
      if (d < bestD) {
        bestD = d;
        best = i;
      }
    }
    return best;
  }

  function dragWirePts(pts, seg, wx, wy) {
    if (!pts || pts.length < 2) return pts;
    pts = pts.map(function (p) {
      return [p[0], p[1]];
    });
    if (pts.length < 4) {
      var y = snap(wy);
      var p0 = pts[0];
      var pn = pts[pts.length - 1];
      return makeOrthoPts([p0, [p0[0] + STUB, p0[1]], [p0[0] + STUB, y], [pn[0] - STUB, y], [pn[0] - STUB, pn[1]], pn]);
    }
    var i = clamp(seg, 1, pts.length - 3);
    var horiz = Math.abs(pts[i][1] - pts[i + 1][1]) < 2;
    if (horiz) {
      var ny = snap(wy);
      pts[i][1] = ny;
      pts[i + 1][1] = ny;
    } else {
      var nx = snap(wx);
      pts[i][0] = nx;
      pts[i + 1][0] = nx;
    }
    return makeOrthoPts(pts);
  }

  function boxesOverlap(a, b, gap) {
    gap = gap || 0;
    return a.l < b.r + gap && a.r + gap > b.l && a.t < b.b + gap && a.b + gap > b.t;
  }

  function resolveCardOverlaps(list) {
    var ids = [];
    var i;
    for (i = 0; i < list.length; i++) {
      if (isHiddenCard(list, i)) continue;
      ids.push(i);
    }
    function boxOf(id) {
      if (id === "start") return nodeBox(list, "start");
      return nodeBox(list, id);
    }
    ids.push("start");
    var gap = 18;
    var guard = 0;
    var changed = false;
    while (guard++ < 80) {
      var moved = false;
      var a;
      var b;
      for (a = 0; a < ids.length; a++) {
        for (b = a + 1; b < ids.length; b++) {
          var ba = boxOf(ids[a]);
          var bb = boxOf(ids[b]);
          if (!ba || !bb || !boxesOverlap(ba, bb, gap)) continue;
          var lower = ba.t <= bb.t ? ids[b] : ids[a];
          var upper = lower === ids[a] ? ids[b] : ids[a];
          if (lower === "start") {
            lower = upper;
            upper = "start";
          }
          var bu = boxOf(upper);
          var newY = bu.b + gap;
          if (lower === "start") vis.start.y = newY;
          else list[lower]._vy = newY;
          moved = true;
          changed = true;
        }
      }
      if (!moved) break;
    }
    return changed;
  }

  function measureDomCardHeights(list) {
    var wrap = $("#visualCards");
    if (!wrap) return;
    var nodes = wrap.querySelectorAll(".vcard");
    var i;
    for (i = 0; i < nodes.length; i++) {
      var id = nodes[i].dataset.i;
      if (id === "start") continue;
      var ni = id | 0;
      if (!list[ni]) continue;
      var r = nodes[i].getBoundingClientRect();
      if (r.height > 8) list[ni]._vh = r.height / vis.zoom;
    }
  }

  function applyCardDomPos(list) {
    var wrap = $("#visualCards");
    if (!wrap) return;
    var i;
    for (i = 0; i < list.length; i++) {
      if (isHiddenCard(list, i)) continue;
      var node = wrap.querySelector('.vcard[data-i="' + i + '"]');
      if (!node) continue;
      var p = cardPos(list[i]);
      node.style.left = p.x + "px";
      node.style.top = p.y + "px";
    }
    var startNode = wrap.querySelector('.vcard[data-i="start"]');
    if (startNode) {
      var s = startPos();
      startNode.style.left = s.x + "px";
      startNode.style.top = s.y + "px";
    }
  }

  function orthoPath(x1, y1, x2, y2, kind) {
    return routeManhattan(x1, y1, x2, y2, kind, vis._routeBoxes || [], vis._routeLanes || { h: [], v: [] });
  }

  function normalizeLinks(raw) {
    if (!Array.isArray(raw)) return [];
    var out = [];
    for (var i = 0; i < raw.length; i++) {
      var l = raw[i];
      if (!l) continue;
      var from = l.from === "start" ? "start" : l.from | 0;
      var to = l.to | 0;
      if (from !== "start" && !(from >= 0)) continue;
      if (!(to >= 0)) continue;
      out.push({ from: from, to: to, kind: l.kind || "seq" });
    }
    return out;
  }

  function generateDefaultLinks(list) {
    pruneEmptyElseActions(list);
    list = actions();
    vis.links = [];
    vis.loopMarks = [];
    var blockByName = {};
    var i;
    for (i = 0; i < list.length; i++) {
      if (list[i].type === "defineBlock" && list[i].blockName) {
        blockByName[String(list[i].blockName)] = i;
      }
    }

    function contKindOf(from) {
      if (from !== "start" && list[from] && list[from].type === "if") return "ifTrue";
      return "seq";
    }

    function fanIfs(from, ifIdx, withContinue) {
      var g = siblingIfGroup(list, ifIdx);
      var gi;
      for (gi = 0; gi < g.group.length; gi++) {
        vis.links.push({ from: from, to: g.group[gi], kind: "guard" });
      }
      if (withContinue && g.after >= 0) {
        vis.links.push({ from: from, to: enterTarget(list, g.after), kind: contKindOf(from) });
      }
      return g;
    }

    function wireIfInterior(ifIdx) {
      var thenKids = kidsNoElse(list, ifIdx);
      var elseIdx = findElseIndex(list, ifIdx);
      if (thenKids.length) {
        if (list[thenKids[0]].type === "if") fanIfs(ifIdx, thenKids[0], true);
        else vis.links.push({ from: ifIdx, to: enterTarget(list, thenKids[0]), kind: "ifTrue" });
      }
      if (elseIdx >= 0) {
        var elseKids = kidsNoElse(list, elseIdx);
        if (elseKids.length) {
          vis.links.push({ from: ifIdx, to: enterTarget(list, elseKids[0]), kind: "ifFalse" });
          chainKids(elseIdx);
        }
      }
      chainKids(ifIdx);
    }

    var skipIncoming = {};

    function consumeIfGroup(firstIf, from) {
      var g = siblingIfGroup(list, firstIf);
      if (from === "start" || (typeof from === "number" && from >= 0)) {
        fanIfs(from, firstIf, true);
        if (g.after >= 0) skipIncoming[enterTarget(list, g.after)] = true;
      }
      var gi;
      for (gi = 0; gi < g.group.length; gi++) wireIfInterior(g.group[gi]);
      return g;
    }

    function chainKids(parent) {
      var kids = kidsNoElse(list, parent);
      var lastNonIf = -1;
      var k = 0;
      while (k < kids.length) {
        var kid = kids[k];
        if (list[kid].type === "if") {
          var gIf = siblingIfGroup(list, kid);
          if (lastNonIf >= 0) consumeIfGroup(kid, lastNonIf);
          else {
            var gi;
            for (gi = 0; gi < gIf.group.length; gi++) wireIfInterior(gIf.group[gi]);
          }
          while (k < kids.length && gIf.group.indexOf(kids[k]) >= 0) k++;
          continue;
        }
        if (list[kid].type === "loop") {
          var head = enterTarget(list, kid);
          var tail = spineTail(list, kid);
          if (head >= 0 && tail >= 0) {
            vis.loopMarks.push({ loop: kid, head: head, tail: tail });
            vis.links.push({ from: tail, to: head, kind: "back" });
          }
          if (lastNonIf >= 0 && head >= 0 && !skipIncoming[head]) {
            vis.links.push({ from: lastNonIf, to: head, kind: "seq" });
          }
          skipIncoming[head] = false;
          chainKids(kid);
          lastNonIf = tail >= 0 ? tail : lastNonIf;
          k++;
          continue;
        }
        var destKid = enterTarget(list, kid);
        if (lastNonIf >= 0 && !skipIncoming[destKid]) {
          vis.links.push({ from: lastNonIf, to: destKid, kind: "seq" });
        }
        skipIncoming[destKid] = false;
        lastNonIf = kid;
        k++;
      }
    }

    var tops = [];
    i = 0;
    while (i < list.length) {
      if (isTopDeclContainer(list[i])) {
        chainKids(i);
        i = subtreeEnd(i);
        continue;
      }
      if ((list[i].indent | 0) !== 0 || list[i].type === "else") {
        i += 1;
        continue;
      }
      tops.push(i);
      i = subtreeEnd(i);
    }
    var lastNonIf = -1;
    var leadingIfsDone = false;
    if (tops.length) {
      if (list[tops[0]].type === "if") {
        consumeIfGroup(tops[0], "start");
        leadingIfsDone = true;
      } else {
        vis.links.push({ from: "start", to: enterTarget(list, tops[0]), kind: "seq" });
      }
    }
    for (i = 0; i < tops.length; i++) {
      var t = tops[i];
      if (list[t].type === "if") {
        var gTop = siblingIfGroup(list, t);
        if (lastNonIf >= 0) consumeIfGroup(t, lastNonIf);
        else if (!leadingIfsDone) consumeIfGroup(t, "start");
        leadingIfsDone = false;
        while (i + 1 < tops.length && gTop.group.indexOf(tops[i + 1]) >= 0) i++;
        continue;
      }
      leadingIfsDone = false;
      if (list[t].type === "loop") {
        var th = enterTarget(list, t);
        var tt = spineTail(list, t);
        if (th >= 0 && tt >= 0) {
          vis.loopMarks.push({ loop: t, head: th, tail: tt });
          vis.links.push({ from: tt, to: th, kind: "back" });
        }
        if (lastNonIf >= 0 && th >= 0 && !skipIncoming[th]) vis.links.push({ from: lastNonIf, to: th, kind: "seq" });
        skipIncoming[th] = false;
        chainKids(t);
        lastNonIf = tt >= 0 ? tt : lastNonIf;
        continue;
      }
      var destTop = enterTarget(list, t);
      if (lastNonIf >= 0 && !skipIncoming[destTop]) vis.links.push({ from: lastNonIf, to: destTop, kind: "seq" });
      skipIncoming[destTop] = false;
      lastNonIf = t;
    }
    for (i = 0; i < list.length; i++) {
      if (list[i].type === "goto") {
        var step = parseInt(String(list[i].gotoStepExpr || "").trim(), 10);
        if (step >= 1 && step <= list.length) vis.links.push({ from: i, to: step - 1, kind: "jump" });
      }
      if (list[i].type === "runBlock" && list[i].blockName && blockByName[list[i].blockName] != null) {
        vis.links.push({ from: i, to: blockByName[list[i].blockName], kind: "call" });
      }
    }
    vis.linksReady = true;
  }

  function outgoingSeq(from) {
    var key = linkKey(from);
    for (var i = 0; i < vis.links.length; i++) {
      if (linkKey(vis.links[i].from) === key && (vis.links[i].kind || "seq") === "seq") return vis.links[i];
    }
    return null;
  }

  function removeOutgoingKind(from, kind) {
    var key = linkKey(from);
    kind = kind || "seq";
    vis.links = vis.links.filter(function (l) {
      return !(linkKey(l.from) === key && (l.kind || "seq") === kind);
    });
  }

  function wouldCycle(from, to) {
    if (from === "start") return false;
    if (from === to) return true;
    var seen = {};
    var q = [to];
    while (q.length) {
      var cur = q.pop();
      if (cur === from) return true;
      if (seen[cur]) continue;
      seen[cur] = true;
      for (var i = 0; i < vis.links.length; i++) {
        if ((vis.links[i].kind || "seq") !== "seq") continue;
        if (vis.links[i].from === cur) q.push(vis.links[i].to);
      }
    }
    return false;
  }

  function connectSeq(from, to, kind) {
    var list = actions();
    if (to < 0 || to >= list.length) return false;
    if (from !== "start" && (from < 0 || from >= list.length)) return false;
    if (list[to] && (list[to].type === "loop" || list[to].type === "else" || isDeclContainerType(list[to].type))) {
      if (list[to].type === "else") {
        var eKids = kidsNoElse(list, to);
        if (!eKids.length) return false;
        to = enterTarget(list, eKids[0]);
      } else return false;
    }
    if (from !== "start" && list[from] && isDeclContainerType(list[from].type)) return false;
    kind = kind || "seq";
    if (from === to) kind = "back";
    if (kind === "seq" && list[to] && list[to].type === "if") kind = "guard";
    if (kind !== "back" && from !== "start") {
      var endA = subtreeEnd(from);
      if (to >= from && to < endA && list[from].type !== "if") return false;
    }
    var entryBlock = loopEntryBlockReason(from, to, kind);
    if (entryBlock) {
      toast(entryBlock);
      return false;
    }
    pushUndo();
    vis.graphTouched = true;
    if (kind === "guard" || kind === "ifTrue") {
      vis.links = vis.links.filter(function (l) {
        return !(linkKey(l.from) === linkKey(from) && l.to === to && (l.kind || "seq") === kind);
      });
    } else {
      removeOutgoingKind(from, kind);
    }
    vis.links.push({ from: from, to: to, kind: kind });
    vis.linksReady = true;
    vis.selEdge = { from: from, to: to, kind: kind };
    return true;
  }

  function deleteEdge(edge) {
    if (!edge) return;
    var next = vis.links.filter(function (l) {
      return !sameEdge(l, edge);
    });
    if (next.length === vis.links.length) return;
    pushUndo();
    vis.graphTouched = true;
    vis.links = next;
    vis.selEdge = null;
    if (vis.wireRoutes) delete vis.wireRoutes[routeKey(edge)];
    pruneBrokenLoops();
    pruneEmptyElseActions(actions());
    renderCanvas();
  }

  function reachableSet(list) {
    var keep = {};
    var i;
    for (i = 0; i < list.length; i++) {
      if (isTopDeclContainer(list[i])) {
        var end = subtreeEnd(i);
        for (var j = i; j < end; j++) keep[j] = true;
      }
    }
    var q = [];
    for (i = 0; i < vis.links.length; i++) {
      if (vis.links[i].from !== "start") continue;
      var sk = vis.links[i].kind || "seq";
      if (sk === "seq" || sk === "guard") q.push(vis.links[i].to);
    }
    var seen = {};
    function addWithFamily(idx) {
      if (idx < 0 || idx >= list.length || seen[idx]) return;
      seen[idx] = true;
      keep[idx] = true;
      if (isContainer(list[idx].type)) {
        var endC = subtreeEnd(idx);
        var c;
        for (c = idx; c < endC; c++) keep[c] = true;
        var elseIdx = findElseIndex(list, idx);
        if (elseIdx >= 0 && hasIfFalseLink(idx)) addWithFamily(elseIdx);
      }
      for (var li = 0; li < vis.links.length; li++) {
        var kind = vis.links[li].kind || "seq";
        if (kind === "back" || kind === "call" || kind === "jump") continue;
        if (vis.links[li].from === idx) q.push(vis.links[li].to);
      }
      var mark = loopMarkByHead(idx);
      if (mark && mark.loop >= 0) keep[mark.loop] = true;
    }
    while (q.length) addWithFamily(q.pop());
    return keep;
  }

  function orphanIndices(list) {
    var keep = reachableSet(list);
    var orphans = [];
    for (var i = 0; i < list.length; i++) {
      if (!keep[i]) orphans.push(i);
    }
    return orphans;
  }

  function dropUnreachable(list) {
    var keep = reachableSet(list);
    var next = [];
    var map = {};
    var i;
    for (i = 0; i < list.length; i++) {
      if (keep[i]) {
        map[i] = next.length;
        next.push(list[i]);
      }
    }
    vis.links = vis.links
      .filter(function (l) {
        var fOk = l.from === "start" || keep[l.from];
        return fOk && keep[l.to];
      })
      .map(function (l) {
        return {
          from: l.from === "start" ? "start" : map[l.from],
          to: map[l.to],
          kind: l.kind || "seq",
        };
      });
    var st = state();
    st.editorActions = next;
    if (st.actionSel >= 0) {
      if (!keep[st.actionSel]) st.actionSel = -1;
      else st.actionSel = map[st.actionSel];
    }
    vis.selEdge = null;
    vis.graphTouched = true;
    vis.loopMarks = vis.loopMarks
      .filter(function (m) {
        return keep[m.loop] && keep[m.head] && keep[m.tail];
      })
      .map(function (m) {
        return { loop: map[m.loop], head: map[m.head], tail: map[m.tail] };
      });
    remapWireRoutes(function (from, to, kind) {
      var fOk = from === "start" || keep[from];
      if (!fOk || !keep[to]) return null;
      return { from: from === "start" ? "start" : map[from], to: map[to], kind: kind };
    });
  }

  function remapLoopMarksInsert(pos) {
    vis.loopMarks.forEach(function (m) {
      if (m.loop >= pos) m.loop += 1;
      if (m.head >= pos) m.head += 1;
      if (m.tail >= pos) m.tail += 1;
    });
  }

  function remapLoopMarksDelete(index, count) {
    vis.loopMarks = vis.loopMarks.filter(function (m) {
      var hit =
        (m.loop >= index && m.loop < index + count) ||
        (m.head >= index && m.head < index + count) ||
        (m.tail >= index && m.tail < index + count);
      return !hit;
    });
    vis.loopMarks.forEach(function (m) {
      if (m.loop >= index + count) m.loop -= count;
      if (m.head >= index + count) m.head -= count;
      if (m.tail >= index + count) m.tail -= count;
    });
  }

  function isFlowKind(kind) {
    kind = kind || "seq";
    return kind === "seq" || kind === "ifTrue" || kind === "ifFalse" || kind === "fail" || kind === "guard";
  }

  function isMarkedLoopClose(from, to) {
    var i;
    for (i = 0; i < vis.loopMarks.length; i++) {
      if (vis.loopMarks[i].tail === from && vis.loopMarks[i].head === to) return true;
    }
    return false;
  }

  function stripMarkedLoopSeqCloses() {
    vis.links = vis.links.filter(function (l) {
      var k = l.kind || "seq";
      if (k === "back" || k === "call" || k === "jump") return true;
      return !isMarkedLoopClose(l.from, l.to);
    });
  }

  function invalidateLinksFromListEdit() {
    vis.linksReady = false;
    vis.selEdge = null;
    vis.links = [];
    vis.loopMarks = [];
    vis.wireRoutes = {};
    vis.graphTouched = false;
  }

  function findUnmarkedCycle(list) {
    var seen = {};
    var stack = {};
    function dfs(i) {
      if (i < 0 || i >= list.length) return false;
      if (stack[i]) return true;
      if (seen[i]) return false;
      seen[i] = true;
      stack[i] = true;
      for (var li = 0; li < vis.links.length; li++) {
        var l = vis.links[li];
        if (l.from !== i) continue;
        var kind = l.kind || "seq";
        if (kind === "back" || kind === "call" || kind === "jump") continue;
        if (isMarkedLoopClose(l.from, l.to)) continue;
        if (dfs(l.to)) return true;
      }
      stack[i] = false;
      return false;
    }
    var startTo = [];
    for (var s = 0; s < vis.links.length; s++) {
      if (vis.links[s].from === "start") startTo.push(vis.links[s].to);
    }
    for (s = 0; s < startTo.length; s++) {
      if (dfs(startTo[s])) return true;
    }
    return false;
  }

  function hasUnmarkedBack() {
    var i;
    var m;
    for (i = 0; i < vis.links.length; i++) {
      var l = vis.links[i];
      if ((l.kind || "seq") !== "back") continue;
      var ok = false;
      for (m = 0; m < vis.loopMarks.length; m++) {
        if (vis.loopMarks[m].head === l.to && vis.loopMarks[m].tail === l.from) {
          ok = true;
          break;
        }
      }
      if (!ok) return true;
    }
    return false;
  }

  function rebuildOrderFromLinks() {
    var list = actions();
    var order = [];
    var seen = {};
    var i;
    for (i = 0; i < list.length; i++) {
      if (isTopDeclContainer(list[i])) {
        var end = subtreeEnd(i);
        for (var j = i; j < end; j++) {
          seen[j] = true;
          order.push(list[j]);
        }
      }
    }
    function outLink(from, kind) {
      for (var li = 0; li < vis.links.length; li++) {
        if (vis.links[li].from === from && (vis.links[li].kind || "seq") === kind) return vis.links[li];
      }
      return null;
    }
    function outLinks(from, kind) {
      var arr = [];
      for (var li = 0; li < vis.links.length; li++) {
        if (vis.links[li].from === from && (vis.links[li].kind || "seq") === kind) arr.push(vis.links[li]);
      }
      return arr;
    }
    function walkGuards(from, indent) {
      var gs = outLinks(from, "guard");
      var gi;
      for (gi = 0; gi < gs.length; gi++) {
        if (list[gs[gi].to] && list[gs[gi].to].type === "if") walkIf(gs[gi].to, indent);
        else walk(gs[gi].to, indent);
      }
    }
    function isJoin(idx) {
      var n = 0;
      var li;
      for (li = 0; li < vis.links.length; li++) {
        var l = vis.links[li];
        if (l.to !== idx) continue;
        var kind = l.kind || "seq";
        if (kind === "back" || kind === "call" || kind === "jump") continue;
        n += 1;
      }
      return n >= 2;
    }
    function emitLoop(mark, indent) {
      if (!list[mark.loop]) return;
      if (!seen[mark.loop]) {
        list[mark.loop].indent = indent;
        order.push(list[mark.loop]);
        seen[mark.loop] = true;
      }
      var cur = mark.head;
      var guard = 0;
      while (cur >= 0 && cur < list.length && guard++ < 4000) {
        var nested = loopMarkByHead(cur);
        if (nested && cur !== mark.head) {
          emitLoop(nested, indent + 1);
          if (cur === mark.tail) break;
          var afterInner = outLink(nested.tail, "seq");
          cur = afterInner && afterInner.to !== mark.head ? afterInner.to : -1;
          continue;
        }
        if (list[cur] && list[cur].type === "if" && !seen[cur]) {
          walkIf(cur, indent + 1);
        } else if (!seen[cur]) {
          list[cur].indent = indent + 1;
          order.push(list[cur]);
          seen[cur] = true;
          walkGuards(cur, indent + 1);
        }
        if (cur === mark.tail) break;
        var n = outLink(cur, "seq") || outLink(cur, "ifTrue");
        if (!n || n.kind === "back") break;
        cur = n.to;
      }
      var kids = kidsNoElse(list, mark.loop);
      var ki;
      for (ki = 0; ki < kids.length; ki++) {
        if (seen[kids[ki]]) continue;
        walk(kids[ki], indent + 1);
      }
    }
    function walk(idx, indent) {
      if (idx < 0 || idx >= list.length || seen[idx]) return;
      if (list[idx].type === "loop") {
        walk(enterTarget(list, idx), indent);
        return;
      }
      var mark = loopMarkByHead(idx);
      if (mark && list[mark.loop]) {
        emitLoop(mark, indent);
        var ex = outLink(mark.tail, "seq");
        if (ex && ex.to !== mark.head) walk(ex.to, indent);
        return;
      }
      if (list[idx].type === "if") {
        walkIf(idx, indent);
        return;
      }
      list[idx].indent = indent;
      order.push(list[idx]);
      seen[idx] = true;
      walkGuards(idx, indent);
      var nxt = outLink(idx, "seq");
      if (nxt) walk(nxt.to, indent);
    }
    function walkBranch(idx, indent) {
      if (idx < 0 || idx >= list.length) return -1;
      if (isJoin(idx) || seen[idx]) return idx;
      var cur = idx;
      var guard = 0;
      while (cur >= 0 && cur < list.length && guard++ < 4000) {
        if (seen[cur] || isJoin(cur)) return cur;
        var nested = loopMarkByHead(cur);
        if (nested) {
          emitLoop(nested, indent);
          var ex = outLink(nested.tail, "seq");
          cur = ex && ex.to !== nested.head ? ex.to : -1;
          continue;
        }
        if (list[cur].type === "if") {
          walkIf(cur, indent);
          return -1;
        }
        list[cur].indent = indent;
        order.push(list[cur]);
        seen[cur] = true;
        walkGuards(cur, indent);
        var n = outLink(cur, "seq") || outLink(cur, "ifTrue");
        if (!n || n.kind === "back") return -1;
        cur = n.to;
      }
      return cur;
    }
    function walkIf(idx, indent) {
      if (idx < 0 || idx >= list.length || seen[idx]) return;
      list[idx].indent = indent;
      order.push(list[idx]);
      seen[idx] = true;
      var trues = outLinks(idx, "ifTrue");
      var f = outLink(idx, "ifFalse") || outLink(idx, "fail");
      var ti;
      for (ti = 0; ti < trues.length; ti++) {
        var tTo = trues[ti].to;
        if (list[tTo] && list[tTo].type === "if") walkIf(tTo, indent + 1);
        else walkBranch(tTo, indent + 1);
      }
      walkGuards(idx, indent + 1);
      var elseStart = -1;
      if (f && list[f.to]) {
        if (list[f.to].type === "else") {
          var ek = outLink(f.to, "seq");
          if (ek) elseStart = ek.to;
        } else {
          elseStart = f.to;
        }
      }
      if (elseStart >= 0) {
        var elseIdx = findElseIndex(list, idx);
        if (elseIdx < 0) {
          var elseAct =
            typeof A().defaultAction === "function" ? A().defaultAction("else", "") : { type: "else" };
          elseAct.type = "else";
          elseAct.indent = indent;
          delete elseAct._preview;
          order.push(elseAct);
        } else if (!seen[elseIdx]) {
          list[elseIdx].indent = indent;
          order.push(list[elseIdx]);
          seen[elseIdx] = true;
        }
        if (list[elseStart] && list[elseStart].type === "if") walkIf(elseStart, indent + 1);
        else walkBranch(elseStart, indent + 1);
      }
      var tkids = kidsNoElse(list, idx);
      var tki;
      for (tki = 0; tki < tkids.length; tki++) {
        if (seen[tkids[tki]]) continue;
        walk(tkids[tki], indent + 1);
      }
    }
    var startLinks = [];
    for (i = 0; i < vis.links.length; i++) {
      if (vis.links[i].from === "start") startLinks.push(vis.links[i]);
    }
    var si;
    for (si = 0; si < startLinks.length; si++) {
      if ((startLinks[si].kind || "seq") === "guard") walkIf(startLinks[si].to, 0);
    }
    for (si = 0; si < startLinks.length; si++) {
      if ((startLinks[si].kind || "seq") === "guard") continue;
      walk(startLinks[si].to, 0);
    }
    var st = state();
    var selAct = st.actionSel >= 0 ? list[st.actionSel] : null;
    st.editorActions = order;
    if (selAct) {
      var ni = order.indexOf(selAct);
      st.actionSel = ni;
    }
    vis.linksReady = false;
    var idxMap = {};
    for (i = 0; i < list.length; i++) {
      var nidx = order.indexOf(list[i]);
      if (nidx >= 0) idxMap[i] = nidx;
    }
    remapWireRoutes(function (from, to, kind) {
      var nf = from === "start" ? "start" : idxMap[from];
      var nt = idxMap[to];
      if (nf == null || nt == null) return null;
      return { from: nf, to: nt, kind: kind };
    });
    generateDefaultLinks(order);
  }

  function commitGraphToList(done, confirmMsg) {
    var list = actions();
    if (!vis.mode) {
      if (done) done(true);
      return;
    }
    if (!vis.linksReady) generateDefaultLinks(actions());
    stripMarkedLoopSeqCloses();
    pruneEmptyElseActions(actions());
    list = actions();
    function finishOk() {
      if (typeof A().loadEditDraftFromSelection === "function") A().loadEditDraftFromSelection();
      if (done) done(true);
    }
    function leaveUsingList() {
      vis.linksReady = false;
      vis.selEdge = null;
      vis.links = [];
      vis.loopMarks = [];
      vis.wireRoutes = {};
      vis.graphTouched = false;
      finishOk();
    }
    function apply() {
      try {
        dropUnreachable(actions());
        rebuildOrderFromLinks();
        vis.graphTouched = true;
        if (vis.mode) renderCanvas();
        finishOk();
      } catch (err) {
        toast("代码化未能按连线写回，已按当前动作列表显示");
        leaveUsingList();
      }
    }
    if (findUnmarkedCycle(list) || hasUnmarkedBack()) {
      if (typeof A().askConfirm === "function") {
        A().askConfirm("存在未标记的环。确定按当前动作列表代码化？（未闭合的连线不会写回列表；若要保留循环请先右键「设为首节点」）", leaveUsingList);
        return;
      }
      toast("存在未标记的环，已按当前动作列表代码化");
      leaveUsingList();
      return;
    }
    var orphans = orphanIndices(actions());
    if (!orphans.length) {
      apply();
      return;
    }
    if (typeof A().askConfirm !== "function") {
      apply();
      return;
    }
    A().askConfirm(confirmMsg || "与初始节点无关的流程会直接删除，确定继续？", apply);
  }

  function findCycleTail(head) {
    var incoming = vis.links.filter(function (l) {
      return l.to === head && l.from !== "start";
    });
    var bi;
    for (bi = 0; bi < incoming.length; bi++) {
      var src = incoming[bi].from;
      if (typeof src !== "number") continue;
      if (canReach(head, src)) return { tail: src, link: incoming[bi] };
    }
    return null;
  }

  function findLoopContext(idx) {
    var m = loopMarkByHead(idx);
    if (m) return m;
    var i;
    for (i = 0; i < vis.loopMarks.length; i++) {
      if (vis.loopMarks[i].loop === idx) return vis.loopMarks[i];
    }
    var list = actions();
    if (!list[idx]) return null;
    if (list[idx].type === "loop") {
      var kids = directChildren(list, idx);
      var head = kids.length ? enterTarget(list, kids[0]) : idx;
      var tail = spineTail(list, idx);
      return { loop: idx, head: head, tail: tail < 0 ? head : tail };
    }
    if (idx > 0 && list[idx - 1] && list[idx - 1].type === "loop") {
      var lp = idx - 1;
      var kids2 = directChildren(list, lp);
      if (kids2.length && enterTarget(list, kids2[0]) === idx) {
        var t2 = spineTail(list, lp);
        return { loop: lp, head: idx, tail: t2 < 0 ? idx : t2 };
      }
    }
    var best = null;
    var bestSize = 1e9;
    for (i = 0; i < vis.loopMarks.length; i++) {
      var mark = vis.loopMarks[i];
      var members = cycleMemberSet(mark.head, mark.tail);
      if (!members[idx]) continue;
      var size = 0;
      var k;
      for (k in members) {
        if (Object.prototype.hasOwnProperty.call(members, k)) size += 1;
      }
      if (size < bestSize) {
        best = mark;
        bestSize = size;
      }
    }
    return best;
  }

  function unwrapLoopAt(idx, opts) {
    opts = opts || {};
    var list = actions();
    var mark = findLoopContext(idx);
    if (!mark || !list[mark.loop] || list[mark.loop].type !== "loop") return false;
    var loop = mark.loop;
    if (!opts.skipUndo) pushUndo();
    vis.graphTouched = true;
    vis.links = vis.links.filter(function (l) {
      return !((l.kind || "seq") === "back" && l.from === mark.tail && l.to === mark.head);
    });
    vis.loopMarks = vis.loopMarks.filter(function (x) {
      return x.loop !== loop && x.head !== mark.head;
    });
    var end = subtreeEnd(loop);
    var j;
    for (j = loop + 1; j < end; j++) list[j].indent = clampIndent((list[j].indent | 0) - 1);
    list.splice(loop, 1);
    remapDelete(loop, 1);
    var st = state();
    if (st.actionSel === loop) {
      st.actionSel = -1;
      st.editDraft = null;
    } else if (st.actionSel > loop) st.actionSel -= 1;
    if (!opts.silent) {
      if (typeof A().renderEditorActions === "function") A().renderEditorActions(actions());
      else renderCanvas();
    }
    return true;
  }

  function pruneBrokenLoops() {
    var again = true;
    var guard = 0;
    while (again && guard++ < 32) {
      again = false;
      var marks = vis.loopMarks.slice();
      var mi;
      for (mi = 0; mi < marks.length; mi++) {
        var m = marks[mi];
        var hasBack = vis.links.some(function (l) {
          return (l.kind || "seq") === "back" && l.from === m.tail && l.to === m.head;
        });
        if (hasBack && canReach(m.head, m.tail)) continue;
        if (unwrapLoopAt(m.loop, { skipUndo: true, silent: true })) again = true;
      }
    }
  }

  function markAsLoopHead(head, opts) {
    opts = opts || {};
    var list = actions();
    if (head < 0 || !list[head] || list[head].type === "loop") return false;
    var cyc = findCycleTail(head);
    if (!cyc) {
      toast("请先把循环体末尾连回该节点");
      return false;
    }
    var tail = cyc.tail;
    var backLink = cyc.link;
    if (loopMarkByHead(head)) {
      toast("该节点已是循环首节点");
      return false;
    }
    var block = loopHeadBlockReason(head);
    if (block) {
      toast(block);
      return false;
    }
    if (!opts.skipUndo) pushUndo();
    var loopIdx = -1;
    if (head > 0 && list[head - 1] && list[head - 1].type === "loop") {
      var kids = directChildren(list, head - 1);
      if (kids.length && enterTarget(list, kids[0]) === head) loopIdx = head - 1;
    }
    if (loopIdx < 0 && !(opts.loopTemplate && typeof opts.loopTemplate === "object")) {
      var ri;
      for (ri = 0; ri < list.length; ri++) {
        if (list[ri].type !== "loop") continue;
        if (loopMarkByLoop(ri)) continue;
        if (directChildren(list, ri).length) continue;
        loopIdx = ri;
        break;
      }
    }
    if (loopIdx < 0) {
      var loopAct =
        opts.loopTemplate && typeof opts.loopTemplate === "object"
          ? Object.assign({}, opts.loopTemplate)
          : typeof A().defaultAction === "function"
            ? A().defaultAction("loop", "")
            : { type: "loop", indent: 0 };
      loopAct.type = "loop";
      delete loopAct._preview;
      var hp = cardPos(list[head]);
      loopAct._vx = hp.x;
      loopAct._vy = hp.y;
      var indent = list[head].indent | 0;
      if (!A().insertEditorAction(loopAct, head, indent)) {
        if (!opts.skipUndo) discardLastUndo();
        return false;
      }
      if (!opts.skipUndo) discardLastUndo();
      loopIdx = head;
      head += 1;
      if (tail >= loopIdx) tail += 1;
    }
    backLink.kind = "back";
    vis.loopMarks = vis.loopMarks.filter(function (m) {
      return m.head !== head && m.loop !== loopIdx;
    });
    vis.loopMarks.push({ loop: loopIdx, head: head, tail: tail });
    vis.graphTouched = true;
    vis.links.forEach(function (l) {
      if (l.to === loopIdx) l.to = head;
      if (l.from === loopIdx) l.from = head;
    });
    repairLoopEntries();
    glueLoopToHead(loopIdx, head);
    if (typeof A().renderEditorActions === "function") A().renderEditorActions(actions());
    else renderCanvas();
    toast("已设为循环首节点");
    return true;
  }

  function attachLoopCardTo(loopIdx, head) {
    var list = actions();
    if (loopIdx < 0 || !list[loopIdx] || list[loopIdx].type !== "loop") return false;
    if (directChildren(list, loopIdx).length) {
      toast("请拖拽空的循环卡片到闭环节点上");
      return false;
    }
    if (head < 0 || !list[head] || list[head].type === "loop") return false;
    if (!isOnCyclicChain(head)) {
      toast("只能内嵌到已闭合的动作链上");
      return false;
    }
    if (loopMarkByHead(head)) {
      toast("该节点已是循环首节点");
      return false;
    }
    var attachBlock = loopHeadBlockReason(head);
    if (attachBlock) {
      toast(attachBlock);
      return false;
    }
    var tpl = JSON.parse(JSON.stringify(list[loopIdx]));
    var headAdj = loopIdx < head ? head - 1 : head;
    pushUndo();
    vis.holdUndo = true;
    list.splice(loopIdx, 1);
    remapDelete(loopIdx, 1);
    var ok = markAsLoopHead(headAdj, { skipUndo: true, loopTemplate: tpl });
    vis.holdUndo = false;
    if (!ok) {
      if (typeof A().revertLastEditorHistory === "function") A().revertLastEditorHistory();
      else restoreUndo();
      return false;
    }
    return true;
  }

  function cancelLoopHead(head) {
    if (!unwrapLoopAt(head)) toast("没有可删除的循环");
  }

  function remapInsert(pos) {
    vis.links.forEach(function (l) {
      if (typeof l.from === "number" && l.from >= pos) l.from += 1;
      if (l.to >= pos) l.to += 1;
    });
    if (vis.selEdge) {
      if (typeof vis.selEdge.from === "number" && vis.selEdge.from >= pos) vis.selEdge.from += 1;
      if (vis.selEdge.to >= pos) vis.selEdge.to += 1;
    }
    remapLoopMarksInsert(pos);
    remapWireRoutes(function (from, to, kind) {
      if (typeof from === "number" && from >= pos) from += 1;
      if (to >= pos) to += 1;
      return { from: from, to: to, kind: kind };
    });
  }

  function remapDelete(index, count) {
    count = count | 0;
    if (count < 1) return;
    var lo = index;
    var hi = index + count;
    function inDel(v) {
      return typeof v === "number" && v >= lo && v < hi;
    }
    var incoming = [];
    var outgoing = [];
    var i;
    for (i = 0; i < vis.links.length; i++) {
      var l = vis.links[i];
      var fDel = inDel(l.from);
      var tDel = inDel(l.to);
      if (!fDel && tDel) incoming.push(l);
      else if (fDel && !tDel) outgoing.push(l);
    }
    var seqOut = null;
    for (i = 0; i < outgoing.length; i++) {
      var ok = outgoing[i].kind || "seq";
      if (!isFlowKind(ok) || ok === "guard") continue;
      if (!seqOut || ok === "seq") seqOut = outgoing[i];
    }
    if (!seqOut) {
      for (i = 0; i < outgoing.length; i++) {
        if (isFlowKind(outgoing[i].kind)) {
          seqOut = outgoing[i];
          break;
        }
      }
    }
    var bridges = [];
    var seenBr = {};
    if (seqOut) {
      for (i = 0; i < incoming.length; i++) {
        var inc = incoming[i];
        if (!isFlowKind(inc.kind)) continue;
        if (inc.from === seqOut.to) continue;
        var bk = inc.kind || "seq";
        var key = linkKey(inc.from) + ">" + seqOut.to + ">" + bk;
        if (seenBr[key]) continue;
        seenBr[key] = true;
        bridges.push({ from: inc.from, to: seqOut.to, kind: bk });
      }
    }
    vis.links = vis.links.filter(function (l) {
      return !inDel(l.from) && !inDel(l.to);
    });
    for (i = 0; i < bridges.length; i++) vis.links.push(bridges[i]);
    vis.links.forEach(function (l) {
      if (typeof l.from === "number" && l.from >= hi) l.from -= count;
      if (l.to >= hi) l.to -= count;
    });
    if (vis.selEdge) {
      var sf = vis.selEdge.from;
      var st = vis.selEdge.to;
      var fBad = typeof sf === "number" && sf >= index && sf < index + count;
      var tBad = st >= index && st < index + count;
      if (fBad || tBad) vis.selEdge = null;
      else {
        if (typeof sf === "number" && sf >= index + count) vis.selEdge.from = sf - count;
        if (st >= index + count) vis.selEdge.to = st - count;
      }
    }
    remapLoopMarksDelete(index, count);
    remapWireRoutes(function (from, to, kind) {
      var fBad = typeof from === "number" && from >= index && from < index + count;
      var tBad = to >= index && to < index + count;
      if (fBad || tBad) return null;
      if (typeof from === "number" && from >= index + count) from -= count;
      if (to >= index + count) to -= count;
      return { from: from, to: to, kind: kind };
    });
  }

  function remapMove(fromIdx, end, insert) {
    var n = actions().length;
    var len = end - fromIdx;
    var map = new Array(n);
    var i;
    for (i = 0; i < n; i++) {
      if (i >= fromIdx && i < end) map[i] = insert + (i - fromIdx);
      else if (i < fromIdx) map[i] = i >= insert ? i + len : i;
      else map[i] = i - len >= insert ? i : i - len;
    }
    vis.links.forEach(function (l) {
      if (typeof l.from === "number" && map[l.from] != null) l.from = map[l.from];
      if (map[l.to] != null) l.to = map[l.to];
    });
    vis.loopMarks.forEach(function (m) {
      if (map[m.loop] != null) m.loop = map[m.loop];
      if (map[m.head] != null) m.head = map[m.head];
      if (map[m.tail] != null) m.tail = map[m.tail];
    });
    if (vis.selEdge) {
      if (typeof vis.selEdge.from === "number" && map[vis.selEdge.from] != null) vis.selEdge.from = map[vis.selEdge.from];
      if (map[vis.selEdge.to] != null) vis.selEdge.to = map[vis.selEdge.to];
    }
    remapWireRoutes(function (from, to, kind) {
      var nf = from === "start" ? "start" : map[from];
      var nt = map[to];
      if (nf == null || nt == null) return null;
      return { from: nf, to: nt, kind: kind };
    });
  }

  function onInsert(pos) {
    if (!vis.mode) {
      invalidateLinksFromListEdit();
      return;
    }
    if (!vis.linksReady) return;
    remapInsert(pos);
  }
  function onDelete(index, count) {
    if (!vis.mode) {
      invalidateLinksFromListEdit();
      return;
    }
    if (!vis.linksReady) return;
    remapDelete(index, count);
  }
  function onMove(fromIdx, end, insert) {
    if (!vis.mode) {
      invalidateLinksFromListEdit();
      return;
    }
    if (!vis.linksReady) return;
    remapMove(fromIdx, end, insert);
  }
  function onReplace(start, oldCount, newCount) {
    if (!vis.mode) {
      invalidateLinksFromListEdit();
      return;
    }
    if (!vis.linksReady) return;
    remapDelete(start, oldCount);
    for (var i = 0; i < newCount; i++) remapInsert(start + i);
  }

  function ensureCardPos(list) {
    for (var i = 0; i < list.length; i++) {
      if (typeof list[i]._vx !== "number") {
        list[i]._vx = vis.start.x + CARD_W + GAP_X + (i % 8) * (CARD_W + 24);
        list[i]._vy = vis.start.y + Math.floor(i / 8) * (CARD_H + 24);
      }
      if (typeof list[i]._vy !== "number") list[i]._vy = vis.start.y;
    }
  }

  function esc(s) {
    if (typeof A().esc === "function") return A().esc(s);
    return String(s == null ? "" : s)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  function edgePorts(list, ln) {
    var kind = ln.kind || "seq";
    var fromPort = "out";
    if (ln.from === "start") fromPort = "out";
    else if (kind === "back") fromPort = "outBack";
    else if (kind === "ifTrue") fromPort = "ifTrue";
    else if (kind === "ifFalse" || kind === "fail") fromPort = "ifFalse";
    else if (kind === "guard") {
      fromPort = list[ln.from] && list[ln.from].type === "if" ? "ifTrue" : "out";
    } else if (isLoopTail(list, ln.from) && kind === "seq") fromPort = "outExit";
    var toPort = "in";
    if (kind === "back") toPort = "inBack";
    else if (isLoopHead(list, ln.to) && (kind === "seq" || kind === "guard")) toPort = "inEnter";
    var p1 = ln.from === "start" ? portOf("start", fromPort) : portOf(ln.from, fromPort);
    var p2 = portOf(ln.to, toPort);
    return { p1: p1, p2: p2, kind: kind };
  }

  function edgeSvg(list) {
    var defs =
      '<defs><marker id="qst-arrow" viewBox="0 0 8 8" markerWidth="7" markerHeight="7" refX="6" refY="4" orient="auto"><path d="M0,1 L0,7 L8,4 z" fill="#3b82f6"></path></marker></defs>';
    var boxes = wireBoxes(list);
    var lanes = { h: [], v: [], wrapX: vis._wrapBorderXs || [] };
    vis._routeBoxes = boxes;
    vis._routeLanes = { h: [], v: [] };
    vis._edgePts = {};
    var fwd = [];
    var back = [];
    var e;
    for (e = 0; e < vis.links.length; e++) {
      var ln = vis.links[e];
      if (ln.to < 0 || ln.to >= list.length) continue;
      if (isHiddenCard(list, ln.to)) continue;
      if (typeof ln.from === "number" && isHiddenCard(list, ln.from)) continue;
      var kind0 = ln.kind || "seq";
      if (kind0 !== "call" && kind0 !== "jump") {
        if (list[ln.to] && isDeclContainerType(list[ln.to].type)) continue;
        if (typeof ln.from === "number" && list[ln.from] && isDeclContainerType(list[ln.from].type)) continue;
      }
      if (kind0 === "call" && !otherPref("visualBlockCallWires", true)) continue;
      if (kind0 === "jump" && !otherPref("visualJumpWires", true)) continue;
      var pts = edgePorts(list, ln);
      if (!pts.p1 || !pts.p2) continue;
      var item = { ln: ln, pts: pts };
      if ((pts.kind || "seq") === "back" || pts.p2.x < pts.p1.x - 18) back.push(item);
      else fwd.push(item);
    }
    fwd.sort(function (a, b) {
      var ay = Math.abs(a.pts.p1.y - a.pts.p2.y);
      var by = Math.abs(b.pts.p1.y - b.pts.p2.y);
      if (ay !== by) return ay - by;
      return Math.abs(a.pts.p1.x - a.pts.p2.x) - Math.abs(b.pts.p1.x - b.pts.p2.x);
    });
    var ordered = fwd.concat(back);
    var paths = defs;
    var oi;
    for (oi = 0; oi < ordered.length; oi++) {
      var it = ordered[oi];
      var sel = vis.selEdge && sameEdge(vis.selEdge, it.ln);
      var kind = it.pts.kind;
      var key = routeKey(it.ln);
      var srcBox = nodeBox(list, it.ln.from);
      var dstBox = nodeBox(list, it.ln.to);
      var routed;
      if (vis.drag && vis.drag.kind === "wire" && vis.drag.key === key && vis.drag.pts && vis.drag.pts.length) {
        routed = { pts: vis.drag.pts, d: commitPath(vis.drag.pts, lanes) };
      } else {
        routed = routeLink(it.pts.p1, it.pts.p2, kind, boxes, lanes, srcBox, dstBox, vis.wireRoutes && vis.wireRoutes[key]);
      }
      vis._edgePts[key] = routed.pts;
      var d = routed.d;
      var handles = "";
      if (sel && routed.pts.length >= 4) {
        var hi;
        for (hi = 1; hi < routed.pts.length - 2; hi++) {
          var mx = (routed.pts[hi][0] + routed.pts[hi + 1][0]) / 2;
          var my = (routed.pts[hi][1] + routed.pts[hi + 1][1]) / 2;
          handles +=
            '<circle class="vedge-handle" data-seg="' +
            hi +
            '" cx="' +
            mx +
            '" cy="' +
            my +
            '" r="5.5"></circle>';
        }
      }
      paths +=
        '<g class="visual-edge-g' +
        (sel ? " sel" : "") +
        '" data-from="' +
        esc(String(it.ln.from)) +
        '" data-to="' +
        it.ln.to +
        '" data-kind="' +
        esc(kind) +
        '">' +
        '<path class="visual-edge-hit" d="' +
        d +
        '" />' +
        '<path class="visual-edge ' +
        kind +
        (sel ? " sel" : "") +
        '" marker-end="url(#qst-arrow)" d="' +
        d +
        '" />' +
        handles +
        "</g>";
    }
    vis._routeLanes = lanes;
    if (vis.drag && vis.drag.kind === "port" && vis.drag.cur) {
      paths +=
        '<path class="visual-edge temp" d="' +
        routeManhattan(
          vis.drag.from.x,
          vis.drag.from.y,
          vis.drag.cur.x,
          vis.drag.cur.y,
          vis.drag.portKind || "seq",
          boxes,
          { h: lanes.h.slice(), v: lanes.v.slice(), wrapX: (lanes.wrapX || []).slice() }
        ) +
        '" />';
    }
    return paths;
  }

  function renderWraps(list) {
    var el = $("#visualWraps");
    if (!el) return;
    var boxes = wrapBoxes(list);
    var html = "";
    for (var i = 0; i < boxes.length; i++) {
      var b = boxes[i];
      html +=
        '<div class="vwrap ' +
        esc(b.type) +
        '" data-i="' +
        b.i +
        '" style="left:' +
        b.l +
        "px;top:" +
        b.t +
        "px;width:" +
        b.w +
        "px;height:" +
        b.h +
        'px"></div>';
    }
    el.innerHTML = html;
  }

  function applyDisplayPrefs() {
    var world = worldEl();
    if (world) world.classList.toggle("no-grid", !otherPref("visualShowGrid", true));
    if (vis.mode) renderCanvas();
  }

  function renderCanvas() {
    if (!vis.mode) return;
    var worldGrid = worldEl();
    if (worldGrid) worldGrid.classList.toggle("no-grid", !otherPref("visualShowGrid", true));
    var list = actions();
    var cards = $("#visualCards");
    var edgesSvg = $("#visualEdges");
    var world = worldEl();
    if (!cards || !edgesSvg || !world) return;
    if (list.length && !hasAnyLayout(list)) autoLayout(list);
    ensureCardPos(list);
    if (!vis.linksReady) generateDefaultLinks(list);
    list = actions();
    repairLoopEntries();
    for (var g = 0; g < vis.loopMarks.length; g++) {
      glueLoopToHead(vis.loopMarks[g].loop, vis.loopMarks[g].head);
    }
    for (var ei = 0; ei < list.length; ei++) {
      if (list[ei].type !== "if") continue;
      var eIdx = findElseIndex(list, ei);
      if (eIdx < 0) continue;
      var ip = cardPos(list[ei]);
      list[eIdx]._vx = ip.x;
      list[eIdx]._vy = ip.y;
    }
    applyZoomTransform();
    renderWraps(list);
    var sel = state().actionSel;
    var html = "";
    var sp = startPos();
    html +=
      '<div class="vcard start' +
      (vis.selStart ? " sel" : "") +
      '" data-i="start" style="left:' +
      sp.x +
      "px;top:" +
      sp.y +
      'px">' +
      '<div class="vcard-dots"><i class="g"></i><i class="b"></i><i class="r"></i></div>' +
      '<div class="vcard-title">' +
      (otherPref("visualShowCardId", true) ? "线程起点 (ID: 0)" : "线程起点") +
      "</div>" +
      '<div class="vcard-remark">主流程从这里开始</div>' +
      '<div class="vport out" data-port="out"></div>' +
      "</div>";
    if (!list.length) {
      html += '<div class="visual-empty">右键空白处添加步骤</div>';
    }
    for (var i = 0; i < list.length; i++) {
      var a = list[i];
      if (isHiddenCard(list, i)) continue;
      var p = cardPos(a);
      var title = cardTitleText(a, i);
      var remark = a.type === "loop" ? "拖到闭合动作链上即可内嵌" : (a.remark || "").trim();
      var cls = "vcard";
      if (a.type === "loop") cls += " loop-tag";
      var mark = loopMarkByHead(i);
      var head = isLoopHead(list, i);
      var tail = isLoopTail(list, i);
      if (a.type === "if") cls += " container if";
      else if (a.type === "else") cls += " container else";
      else if (a.type === "defineBlock" || a.type === "watchImage") {
        cls += " container defineBlock";
        if (a.type === "watchImage") cls += " watchImage";
      }
      if (head) cls += " is-loop-head";
      if (isCardSelected(i) || (mark && isCardSelected(mark.loop))) cls += " sel";
      if (mark && isCardSelected(mark.loop)) cls += " sel-loop";
      var w = nodeW(a);
      var ports = "";
      if (a.type === "if") {
        ports =
          '<div class="vport in" data-port="in"></div>' +
          '<div class="vport out if-true" data-port="ifTrue" title="成立"></div>' +
          '<div class="vport out if-false" data-port="ifFalse" title="不成立"></div>';
      } else {
        if (head) {
          ports +=
            '<div class="vport in enter" data-port="inEnter" title="切入循环"></div>' +
            '<div class="vport in back" data-port="inBack" title="循环回边"></div>';
        } else {
          ports += '<div class="vport in" data-port="in"></div>';
        }
        if (tail) {
          ports +=
            '<div class="vport out exit" data-port="outExit" title="切出循环"></div>' +
            '<div class="vport out back" data-port="outBack" title="回到首节点"></div>';
        } else {
          ports += '<div class="vport out" data-port="out"></div>';
        }
      }
      if (a.type === "loop" || a.type === "defineBlock" || a.type === "watchImage") ports = "";
      var badge = "";
      if (head && mark) {
        badge =
          '<button type="button" class="vloop-badge" data-loop="' +
          mark.loop +
          '" title="点击编辑循环参数">' +
          esc(loopBadgeText(list[mark.loop])) +
          "</button>";
      } else if (head && i > 0 && list[i - 1] && list[i - 1].type === "loop") {
        badge =
          '<button type="button" class="vloop-badge" data-loop="' +
          (i - 1) +
          '" title="点击编辑循环参数">' +
          esc(loopBadgeText(list[i - 1])) +
          "</button>";
      }
      html +=
        '<div class="' +
        cls +
        '" data-i="' +
        i +
        '" style="left:' +
        p.x +
        "px;top:" +
        p.y +
        "px;width:" +
        w +
        "px;min-height:" +
        visCardH(list, i) +
        'px">' +
        badge +
        '<div class="vcard-dots"><i class="g"></i><i class="b"></i><i class="r"></i></div>' +
        '<div class="vcard-title">' +
        esc(title) +
        "</div>" +
        '<div class="vcard-remark">' +
        esc(remark) +
        "</div>" +
        ports +
        "</div>";
    }
    cards.innerHTML = html;
    void cards.offsetHeight;
    measureDomCardHeights(list);
    if (vis.needAlign && !vis.drag) {
      vis.needAlign = false;
      if (alignHorizontalWires(list)) applyCardDomPos(list);
    }
    if (!vis.drag && resolveCardOverlaps(list)) applyCardDomPos(list);
    edgesSvg.innerHTML = edgeSvg(list);
    if (vis.pendingScroll && scrollEl()) {
      scrollEl().scrollLeft = vis.pendingScroll.x;
      scrollEl().scrollTop = vis.pendingScroll.y;
      vis.pendingScroll = null;
    }
  }

  function refreshCard(index) {
    if (!vis.mode) return;
    renderCanvas();
  }

  function syncSelectionUi() {
    if (!vis.mode) return;
    var wrap = $("#visualCards");
    if (!wrap) return;
    var sel = state().actionSel;
    var nodes = wrap.querySelectorAll(".vcard");
    for (var i = 0; i < nodes.length; i++) {
      var id = nodes[i].dataset.i;
      if (id === "start") {
        nodes[i].classList.toggle("sel", vis.selStart);
        continue;
      }
      var ni = id | 0;
      var mark = loopMarkByHead(ni);
      var on = isCardSelected(ni) || (mark && isCardSelected(mark.loop));
      nodes[i].classList.toggle("sel", on);
      nodes[i].classList.toggle("sel-loop", !!(mark && isCardSelected(mark.loop)));
    }
  }

  function clearSelection() {
    var st = state();
    var prev = st.actionSel;
    if (st.batchMode && typeof A().enterEditorBatch === "function") A().enterEditorBatch(false);
    st.actionSel = -1;
    st.editDraft = null;
    vis.selStart = false;
    vis.selEdge = null;
    if (typeof A().showAddTypePreview === "function") A().showAddTypePreview();
    if (typeof A().updateEditorSelectionUi === "function") A().updateEditorSelectionUi(prev, -1);
    else syncSelectionUi();
    renderCanvas();
  }

  function selectStart() {
    var st = state();
    if (st.batchMode && typeof A().enterEditorBatch === "function") A().enterEditorBatch(false);
    var prev = st.actionSel;
    st.actionSel = -1;
    st.editDraft = null;
    vis.selStart = true;
    vis.selEdge = null;
    if (typeof A().showAddTypePreview === "function") A().showAddTypePreview();
    if (typeof A().updateEditorSelectionUi === "function") A().updateEditorSelectionUi(prev, -1);
    syncSelectionUi();
    renderCanvas();
  }

  function revealActionCard(i) {
    var wrap = $("#visualCards");
    if (!wrap) return;
    var el = wrap.querySelector('.vcard[data-i="' + i + '"]');
    if (!el) {
      var m;
      for (m = 0; m < vis.loopMarks.length; m++) {
        if (vis.loopMarks[m].loop === i) {
          el = wrap.querySelector('.vcard[data-i="' + vis.loopMarks[m].head + '"]');
          break;
        }
      }
    }
    if (el && el.scrollIntoView) el.scrollIntoView({ block: "nearest", inline: "nearest" });
  }

  function selectAction(i, opts) {
    var st = state();
    if (st.batchMode && typeof A().enterEditorBatch === "function") A().enterEditorBatch(false);
    vis.selStart = false;
    vis.selEdge = null;
    if (!(opts && opts.keepDraft && st.actionSel === i && st.editDraft && !st.addPreview)) {
      st.addPreview = null;
    }
    if (typeof A().selectEditorAction === "function") {
      A().selectEditorAction(i, opts);
    } else {
      var prev = st.actionSel;
      st.actionSel = i;
      if (!(opts && opts.keepDraft && prev === i && st.editDraft)) {
        if (typeof A().loadEditDraftFromSelection === "function") A().loadEditDraftFromSelection();
      }
      if (typeof A().updateEditorSelectionUi === "function") A().updateEditorSelectionUi(prev, i);
      else syncSelectionUi();
    }
    renderEdgesOnly();
    if (!vis.skipReveal) revealActionCard(i);
  }

  function selectEdge(edge) {
    vis.selEdge = edge;
    vis.selStart = false;
    var st = state();
    var prev = st.actionSel;
    st.actionSel = -1;
    st.editDraft = null;
    if (typeof A().showAddTypePreview === "function") A().showAddTypePreview();
    if (typeof A().updateEditorSelectionUi === "function") A().updateEditorSelectionUi(prev, -1);
    var el = host();
    if (el && el.focus) el.focus({ preventScroll: true });
    renderCanvas();
  }

  function snapshotNewAction() {
    var st = state();
    var remarkEl = $("#edRemark");
    var remark = remarkEl ? (remarkEl.textContent || "").trim() : "";
    var a = null;
    if (st.addPreview && (!st.addActionType || st.addPreview.type === st.addActionType)) {
      if (typeof A().readParamPanelInto === "function") A().readParamPanelInto(st.addPreview);
      a = Object.assign({}, st.addPreview);
    } else if (st.actionSel >= 0 && st.editorActions && st.editorActions[st.actionSel]) {
      var cur = typeof A().editorParamAction === "function" ? A().editorParamAction() : st.editDraft;
      if (cur) {
        if (typeof A().readParamPanelInto === "function") A().readParamPanelInto(cur);
        a = Object.assign({}, cur);
      }
    }
    if (!a) {
      if (typeof A().defaultAction !== "function") return null;
      a = A().defaultAction(st.addActionType || "moveMouse", remark);
    }
    delete a._preview;
    delete a._vx;
    delete a._vy;
    a.remark = remark;
    return a;
  }

  function addCardAfter(i) {
    if (typeof A().insertEditorAction !== "function") return;
    var st = state();
    var list = actions();
    if (!list[i]) return;
    var remarkEl = $("#edRemark");
    var remark = remarkEl ? (remarkEl.textContent || "").trim() : "";
    var act = null;
    if (st.addPreview && st.addPreview._preview) {
      if (typeof A().readParamPanelInto === "function") A().readParamPanelInto(st.addPreview);
      act = Object.assign({}, st.addPreview);
    } else if (st.editDraft) {
      if (typeof A().readParamPanelInto === "function") A().readParamPanelInto(st.editDraft);
      st.editDraft.remark = remark;
      act = JSON.parse(JSON.stringify(st.editDraft));
    } else if (typeof A().defaultAction === "function") {
      act = A().defaultAction(st.addActionType || "moveMouse", remark);
    }
    if (!act) return;
    if (act.type === "else") {
      toast("否则分支请从「如果」的黄色端口连出");
      return;
    }
    delete act._preview;
    delete act._vx;
    delete act._vy;
    act.remark = remark;
    var pos = subtreeEnd(i);
    var indent = clampIndent(list[i].indent | 0);
    if (isDeclContainerType(act.type)) {
      pos = 0;
      indent = 0;
    }
    if (!A().insertEditorAction(act, pos, indent)) return;
    vis.graphTouched = true;
    vis.linksReady = true;
    if (act.type !== "loop" && !isDeclContainerType(act.type)) {
      var old = outgoingSeq(i);
      var kind = list[i].type === "if" ? "ifTrue" : "seq";
      removeOutgoingKind(i, kind === "ifTrue" ? "ifTrue" : "seq");
      vis.links.push({ from: i, to: pos, kind: kind });
      if (old && old.to !== pos) vis.links.push({ from: pos, to: old.to, kind: old.kind || "seq" });
    }
    st.addPreview = null;
    vis.selStart = false;
    selectAction(Math.min(pos, actions().length - 1));
    if (typeof A().renderEditorActions === "function") A().renderEditorActions(actions());
  }

  function modifySelectedCard() {
    if (typeof A().commitModifySelected === "function") A().commitModifySelected();
    else toast("无法修改卡片");
  }

  function addCurrentActionAt(worldX, worldY) {
    if (typeof A().insertEditorAction !== "function") return;
    var act = snapshotNewAction();
    if (!act) return;
    if (act.type === "else") {
      toast("否则分支请从「如果」的黄色端口连出");
      return;
    }
    act._vx = snap(worldX - CARD_W / 2);
    act._vy = snap(worldY - CARD_H / 2);
    var st = state();
    var list = actions();
    var pos = list.length;
    var indent = 0;
    var fromSel = -1;
    var fromStart = vis.selStart;
    if (!fromStart && st.actionSel >= 0 && st.actionSel < list.length) {
      fromSel = st.actionSel;
      pos = subtreeEnd(fromSel);
      indent = clampIndent(list[fromSel].indent | 0);
    } else if (fromStart) {
      pos = firstMainIndex(list);
      if (pos < 0) pos = list.length;
      indent = 0;
    }
    if (isDeclContainerType(act.type)) {
      pos = 0;
      indent = 0;
      fromSel = -1;
      fromStart = false;
    }
    if (act.type === "loop") {
      fromSel = -1;
      fromStart = false;
    }
    if (!A().insertEditorAction(act, pos, indent)) return;
    vis.graphTouched = true;
    if (fromStart) {
      var old = outgoingSeq("start");
      removeOutgoingKind("start", "seq");
      vis.links.push({ from: "start", to: pos, kind: "seq" });
      if (old && old.to !== pos) vis.links.push({ from: pos, to: old.to, kind: "seq" });
    } else if (fromSel >= 0) {
      var oldSel = outgoingSeq(fromSel);
      removeOutgoingKind(fromSel, "seq");
      vis.links.push({ from: fromSel, to: pos, kind: "seq" });
      if (oldSel && oldSel.to !== pos) vis.links.push({ from: pos, to: oldSel.to, kind: "seq" });
    }
    vis.linksReady = true;
    st.actionSel = Math.min(pos, actions().length - 1);
    st.addPreview = null;
    vis.selStart = false;
    if (typeof A().loadEditDraftFromSelection === "function") A().loadEditDraftFromSelection();
    if (typeof A().renderEditorActions === "function") A().renderEditorActions(actions());
  }

  function pasteAt(worldX, worldY) {
    var src = vis.clipboard;
    if (!src) {
      var clip = state().editorClipboard;
      if (clip && clip[0] && clip[0][0]) src = clip[0][0];
    }
    if (!src) {
      toast("剪贴板为空");
      return;
    }
    var copy = Object.assign({}, src);
    delete copy._preview;
    copy._vx = snap(worldX);
    copy._vy = snap(worldY);
    var st = state();
    var list = actions();
    var pos = list.length;
    var indent = 0;
    if (st.actionSel >= 0 && st.actionSel < list.length) {
      pos = subtreeEnd(st.actionSel);
      indent = clampIndent(list[st.actionSel].indent | 0);
    }
    if (!A().insertEditorAction(copy, pos, indent)) return;
    vis.graphTouched = true;
    st.actionSel = Math.min(pos, actions().length - 1);
    if (typeof A().loadEditDraftFromSelection === "function") A().loadEditDraftFromSelection();
    if (typeof A().renderEditorActions === "function") A().renderEditorActions(actions());
  }

  function captureCardClipboard(i) {
    var a = actions()[i];
    if (!a) return false;
    vis.clipboard = JSON.parse(JSON.stringify(a));
    delete vis.clipboard._preview;
    var st = state();
    if (st) st.editorClipboard = [[JSON.parse(JSON.stringify(vis.clipboard))]];
    return true;
  }

  function copyCard(i) {
    if (!captureCardClipboard(i)) return;
    toast("已复制卡片");
  }

  function cutCard(i) {
    if (!captureCardClipboard(i)) return false;
    deleteCard(i);
    toast("已剪切卡片");
    return true;
  }

  function deleteCard(i) {
    var list = actions();
    if (list[i] && list[i].type === "loop") {
      if (directChildren(list, i).length) {
        unwrapLoopAt(i);
        return;
      }
      if (typeof A().deleteSubtreeAt !== "function") return;
      A().deleteSubtreeAt(i);
      vis.graphTouched = true;
      if (typeof A().renderEditorActions === "function") A().renderEditorActions(actions());
      return;
    }
    if (typeof A().deleteSubtreeAt !== "function") return;
    if (list[i] && list[i].type === "if") {
      var elseI = findElseIndex(list, i);
      if (elseI > i) A().deleteSubtreeAt(elseI);
    }
    A().deleteSubtreeAt(i);
    vis.graphTouched = true;
    pruneBrokenLoops();
    if (typeof A().renderEditorActions === "function") A().renderEditorActions(actions());
  }

  function changeCardId(i) {
    var n = actions().length;
    if (typeof A().askInput !== "function") return;
    A().askInput("修改卡片ID（1–" + n + "）", String(i + 1), function (text) {
      var dest = parseInt(String(text || "").trim(), 10);
      if (!(dest >= 1 && dest <= n)) {
        toast("请输入有效序号");
        return;
      }
      var destIndex = dest - 1;
      if (destIndex === i) return;
      var len = subtreeEnd(i) - i;
      if (destIndex > n - len) destIndex = n - len;
      var insertIndex = destIndex < i ? destIndex : destIndex + len;
      if (!A().moveActionSubtree(i, insertIndex, clampIndent(actions()[i].indent | 0))) return;
      vis.graphTouched = true;
      state().actionSel = destIndex;
      if (typeof A().loadEditDraftFromSelection === "function") A().loadEditDraftFromSelection();
      if (typeof A().renderEditorActions === "function") A().renderEditorActions(actions());
    });
  }

  function editRemark(i) {
    var a = actions()[i];
    if (!a || typeof A().askInput !== "function") return;
    A().askInput(
      "备注",
      a.remark || "",
      function (text) {
        a.remark = String(text || "");
        var st = state();
        if (st.editDraft && st.actionSel === i) st.editDraft.remark = a.remark;
        if (typeof A().renderEditorActions === "function") A().renderEditorActions(actions());
      },
      { allowEmpty: true }
    );
  }

  function triggerSave() {
    if (typeof A().saveEditorNow === "function") {
      var stay = typeof A().editorAutoSaveOn === "function" && A().editorAutoSaveOn();
      A().saveEditorNow({
        intent: stay ? "autosave" : "manual",
        skipVisualConfirm: !!stay,
      });
      return;
    }
    var btn = $("#btnEdSave");
    if (btn) btn.click();
  }

  function orderIndicesByLinks(indices) {
    var ids = [];
    var set = {};
    (indices || []).forEach(function (i) {
      i = i | 0;
      if (i >= 0 && !set[i]) {
        set[i] = true;
        ids.push(i);
      }
    });
    if (ids.length <= 1) return ids;
    var und = {};
    var edges = 0;
    vis.links.forEach(function (ln) {
      var k = ln.kind || "seq";
      if (k === "back" || k === "jump" || k === "call") return;
      if (ln.from === "start") return;
      var a = ln.from | 0;
      var b = ln.to | 0;
      if (!set[a] || !set[b]) return;
      if (!und[a]) und[a] = [];
      if (!und[b]) und[b] = [];
      if (und[a].indexOf(b) < 0) {
        und[a].push(b);
        und[b].push(a);
        edges += 1;
      }
    });
    function byId(a, b) {
      return a - b;
    }
    if (!edges) {
      ids.sort(byId);
      return ids;
    }
    ids.sort(byId);
    var seen = {};
    var stack = [ids[0]];
    seen[ids[0]] = true;
    var comp = [];
    while (stack.length) {
      var u = stack.pop();
      comp.push(u);
      (und[u] || []).forEach(function (v) {
        if (!seen[v]) {
          seen[v] = true;
          stack.push(v);
        }
      });
    }
    if (comp.length !== ids.length || edges !== ids.length - 1) {
      ids.sort(byId);
      return ids;
    }
    var ends = [];
    var degOk = true;
    ids.forEach(function (i) {
      var d = (und[i] || []).length;
      if (d > 2) degOk = false;
      if (d <= 1) ends.push(i);
    });
    if (!degOk || ends.length > 2 || !ends.length) {
      ids.sort(byId);
      return ids;
    }
    var head = ends[0];
    if (ends.length === 2 && ends[1] < head) head = ends[1];
    var order = [];
    var prev = -1;
    var cur = head;
    var guard = 0;
    while (cur >= 0 && guard++ < 4000) {
      order.push(cur);
      var nxts = und[cur] || [];
      var nxt = -1;
      var ni;
      for (ni = 0; ni < nxts.length; ni++) {
        if (nxts[ni] !== prev) {
          nxt = nxts[ni];
          break;
        }
      }
      if (nxt < 0) break;
      prev = cur;
      cur = nxt;
    }
    if (order.length !== ids.length) {
      ids.sort(byId);
      return ids;
    }
    return order;
  }

  function mapIndexAfterMerge(oldIdx, ranges, insertPos, insertedCount) {
    var offsetInBody = 0;
    var r;
    for (r = 0; r < ranges.length; r++) {
      var rg = ranges[r];
      if (oldIdx >= rg.start && oldIdx < rg.end) return insertPos + 1 + offsetInBody + (oldIdx - rg.start);
      offsetInBody += rg.end - rg.start;
    }
    var removedBefore = 0;
    for (r = 0; r < ranges.length; r++) {
      if (ranges[r].end <= oldIdx) removedBefore += ranges[r].end - ranges[r].start;
    }
    var idx = oldIdx - removedBefore;
    if (idx >= insertPos) idx += insertedCount;
    return idx;
  }

  function remapGraphAfterMerge(result) {
    if (!result || !result.ranges) return;
    var ranges = result.ranges;
    var insertPos = result.insertPos | 0;
    var insertedCount = result.insertedCount | 0;
    function mapNode(v) {
      if (v === "start") return "start";
      if (typeof v !== "number") return v;
      return mapIndexAfterMerge(v, ranges, insertPos, insertedCount);
    }
    vis.links = vis.links
      .map(function (l) {
        return { from: mapNode(l.from), to: mapNode(l.to) | 0, kind: l.kind || "seq" };
      })
      .filter(function (l) {
        var fOk = l.from === "start" || (typeof l.from === "number" && l.from >= 0);
        return fOk && l.to >= 0;
      });
    vis.loopMarks = vis.loopMarks
      .map(function (m) {
        return { loop: mapNode(m.loop) | 0, head: mapNode(m.head) | 0, tail: mapNode(m.tail) | 0 };
      })
      .filter(function (m) {
        return m.loop >= 0 && m.head >= 0 && m.tail >= 0;
      });
    remapWireRoutes(function (from, to, kind) {
      var nf = mapNode(from);
      var nt = mapNode(to);
      if (nf == null || nt == null) return null;
      return { from: nf, to: nt | 0, kind: kind };
    });
    vis.linksReady = true;
  }

  function afterMerge(containerIndex, result) {
    var list = actions();
    if (result && result.ranges) remapGraphAfterMerge(result);
    ensureCardPos(list);
    var a = list[containerIndex];
    if (a && a.type !== "loop" && typeof a._vx !== "number") {
      var kids = directChildren(list, containerIndex);
      var ref = kids.length ? kids[0] : containerIndex + 1;
      if (list[ref]) {
        var hp = cardPos(list[ref]);
        a._vx = hp.x - CARD_W - GAP_X;
        a._vy = hp.y;
      }
    }
    if (a && a.type === "loop") {
      var kids2 = directChildren(list, containerIndex);
      if (kids2.length) {
        var head = enterTarget(list, kids2[0]);
        var tail = spineTail(list, containerIndex);
        vis.loopMarks = vis.loopMarks.filter(function (m) {
          return m.loop !== containerIndex;
        });
        if (head >= 0 && tail >= 0) {
          vis.loopMarks.push({ loop: containerIndex, head: head, tail: tail });
          var hasBack = vis.links.some(function (l) {
            return (l.kind || "seq") === "back" && l.from === tail && l.to === head;
          });
          if (!hasBack) vis.links.push({ from: tail, to: head, kind: "back" });
          var ck;
          for (ck = 0; ck < kids2.length - 1; ck++) {
            var cf = enterTarget(list, kids2[ck]);
            var ct = enterTarget(list, kids2[ck + 1]);
            if (cf === ct || outgoingSeq(cf)) continue;
            vis.links.push({ from: cf, to: ct, kind: "seq" });
          }
        }
        vis.links = vis.links.filter(function (l) {
          if (l.to === containerIndex || l.from === containerIndex) return false;
          return true;
        });
      }
    }
    vis.linksReady = true;
    vis.graphTouched = true;
    vis.needAlign = true;
    var focus = containerIndex;
    if (a && a.type === "loop") {
      var kids3 = directChildren(list, containerIndex);
      if (kids3.length) focus = enterTarget(list, kids3[0]);
    }
    if (typeof A().enterEditorBatch === "function") A().enterEditorBatch(false);
    selectAction(focus);
    renderCanvas();
  }

  function mergeFromVisual() {
    var sel = orderIndicesByLinks(visualSelectedIndices());
    if (sel.length < 2) {
      toast("请至少选中两个卡片");
      return;
    }
    if (typeof A().mergeVisualSelection !== "function") return;
    A().mergeVisualSelection(sel);
  }

  function testFlowFromVisual() {
    var sel = orderIndicesByLinks(visualSelectedIndices());
    var i = sel.length ? sel[0] : state().actionSel;
    if (i >= 0 && typeof A().startDebugFrom === "function") A().startDebugFrom(i, false);
  }

  function groupMoveIndices(anchor) {
    var sel = visualSelectedIndices();
    if (sel.indexOf(anchor) < 0) return [anchor];
    if (sel.length < 2) return [anchor];
    var list = actions();
    var skip = {};
    sel.forEach(function (i) {
      if (!list[i] || !isDeclContainerType(list[i].type)) return;
      var end = subtreeEnd(i);
      var j;
      for (j = i + 1; j < end; j++) skip[j] = true;
    });
    return sel.filter(function (i) {
      return !skip[i];
    });
  }

  function setMarqueeBox(x0, y0, x1, y1) {
    var el = $("#visualMarquee");
    var hostEl = host();
    if (!el || !hostEl) return;
    var r = hostEl.getBoundingClientRect();
    var l = Math.min(x0, x1) - r.left;
    var t = Math.min(y0, y1) - r.top;
    var w = Math.abs(x1 - x0);
    var h = Math.abs(y1 - y0);
    el.style.display = "block";
    el.style.left = l + "px";
    el.style.top = t + "px";
    el.style.width = w + "px";
    el.style.height = h + "px";
  }

  function hideMarquee() {
    var el = $("#visualMarquee");
    if (el) el.style.display = "none";
  }

  function cardsInClientRect(x0, y0, x1, y1) {
    var wrap = $("#visualCards");
    if (!wrap) return [];
    var l = Math.min(x0, x1);
    var t = Math.min(y0, y1);
    var r = Math.max(x0, x1);
    var b = Math.max(y0, y1);
    var out = [];
    var nodes = wrap.querySelectorAll(".vcard");
    var i;
    for (i = 0; i < nodes.length; i++) {
      var id = nodes[i].dataset.i;
      if (id === "start") continue;
      var br = nodes[i].getBoundingClientRect();
      if (br.right < l || br.left > r || br.bottom < t || br.top > b) continue;
      out.push(id | 0);
    }
    return out;
  }

  function applyMultiSelection(indices) {
    if (typeof A().setEditorVisualSelection === "function") A().setEditorVisualSelection(indices);
    else if (indices.length === 1) selectAction(indices[0]);
    else if (!indices.length) clearSelection();
    syncSelectionUi();
    renderCanvas();
  }

  function showBlankMenu(ev, world) {
    vis.lastMenuWorld = world;
    var items = [
      { t: "添加步骤", v: "add" },
      { t: "粘贴卡片 (Ctrl+V)", v: "paste", disabled: !hasClipboardCard() },
      { t: "撤销 (Ctrl+Z)", v: "undo" },
      { t: "恢复 (Ctrl+Y)", v: "redo" },
      { t: "保存工作流", v: "save" },
      { t: "适应视图", v: "fit" },
      { t: "分开重叠卡片", v: "unstack" },
      { t: "恢复全部自动布线", v: "reroute" },
    ];
    A().showPopup(
      { clientX: ev.clientX, clientY: ev.clientY },
      items,
      function (it) {
        if (!it) return;
        if (it.v === "add") addCurrentActionAt(world.x, world.y);
        else if (it.v === "paste") pasteAt(world.x, world.y);
        else if (it.v === "undo") restoreUndo();
        else if (it.v === "redo") restoreRedo();
        else if (it.v === "save") triggerSave();
        else if (it.v === "fit") fitView();
        else if (it.v === "unstack") {
          pushUndo();
          if (resolveCardOverlaps(actions())) renderCanvas();
          else discardLastUndo();
        } else if (it.v === "reroute") {
          pushUndo();
          vis.wireRoutes = {};
          renderCanvas();
        }
      },
      { preferUp: true }
    );
  }

  function showCardMenu(ev, i) {
    var list = actions();
    var a = list[i];
    var multi = visualSelectedIndices();
    if (multi.length >= 2 && isCardSelected(i)) {
      A().showPopup(
        { clientX: ev.clientX, clientY: ev.clientY },
        [
          { t: "并入步骤", v: "merge" },
          { t: "测试流程", v: "testFlow" },
        ],
        function (it) {
          if (!it) return;
          if (it.v === "merge") mergeFromVisual();
          else if (it.v === "testFlow") testFlowFromVisual();
        },
        { preferUp: true }
      );
      return;
    }
    var items = [];
    var canPaste = hasClipboardCard();
    if (a && a.type === "loop") {
      items.push(
        { t: "修改卡片", v: "modify" },
        { t: "复制卡片 (Ctrl+C)", v: "copy" },
        { t: "剪切卡片 (Ctrl+X)", v: "cut" },
        { t: "粘贴卡片 (Ctrl+V)", v: "paste", disabled: !canPaste },
        { t: "删除循环", v: "unhead" }
      );
    } else {
      if (loopMarkByHead(i) || isLoopHead(list, i) || findLoopContext(i)) {
        items.push({ t: "删除循环", v: "unhead" });
      } else if (isOnCyclicChain(i)) {
        items.push({ t: "设为首节点", v: "head" });
      }
      items.push(
        { t: "添加卡片", v: "add" },
        { t: "修改卡片", v: "modify" },
        { t: "转到代码化", v: "toCode" },
        { t: "备注", v: "remark" },
        { t: "修改卡片ID", v: "id" },
        { t: "复制卡片 (Ctrl+C)", v: "copy" },
        { t: "剪切卡片 (Ctrl+X)", v: "cut" },
        { t: "粘贴卡片 (Ctrl+V)", v: "paste", disabled: !canPaste },
        { t: "测试流程", v: "testFlow" },
        { t: "删除卡片", v: "del" }
      );
    }
    A().showPopup(
      { clientX: ev.clientX, clientY: ev.clientY },
      items,
      function (it) {
        if (!it) return;
        if (it.v === "head") markAsLoopHead(i);
        else if (it.v === "unhead") unwrapLoopAt(i);
        else if (it.v === "add") addCardAfter(i);
        else if (it.v === "modify") modifySelectedCard(); else if (it.v === "toCode") goToCodeFromCard(i);
        else if (it.v === "remark") editRemark(i);
        else if (it.v === "id") changeCardId(i);
        else if (it.v === "copy") copyCard(i);
        else if (it.v === "cut") cutCard(i);
        else if (it.v === "paste") {
          var p = cardPos(actions()[i] || {});
          pasteAt(p.x + CARD_W + GAP_X, p.y);
        }
        else if (it.v === "testFlow" && typeof A().startDebugFrom === "function") A().startDebugFrom(i, false);
        else if (it.v === "del") deleteCard(i);
      },
      { preferUp: true }
    );
  }

  function showStartMenu(ev) {
    A().showPopup(
      { clientX: ev.clientX, clientY: ev.clientY },
      [
        { t: "适应视图", v: "fit" },
        { t: "撤销 (Ctrl+Z)", v: "undo" },
        { t: "恢复 (Ctrl+Y)", v: "redo" },
      ],
      function (it) {
        if (!it) return;
        if (it.v === "fit") fitView();
        else if (it.v === "undo") restoreUndo();
        else if (it.v === "redo") restoreRedo();
      },
      { preferUp: true }
    );
  }

  function showEdgeMenu(ev, edge) {
    vis.selEdge = edge;
    renderCanvas();
    A().showPopup(
      { clientX: ev.clientX, clientY: ev.clientY },
      [{ t: "恢复自动布线", v: "auto" }, { t: "删除连接", v: "del" }],
      function (it) {
        if (!it) return;
        if (it.v === "auto") {
          pushUndo();
          if (vis.wireRoutes) delete vis.wireRoutes[routeKey(edge)];
          renderCanvas();
        } else if (it.v === "del") deleteEdge(edge);
      },
      { preferUp: true }
    );
  }

  function edgeFromEvent(ev) {
    var t = ev.target;
    if (!t) return null;
    var g = t.closest ? t.closest(".visual-edge-g") : null;
    if (!g) return null;
    var from = g.getAttribute("data-from");
    return {
      from: from === "start" ? "start" : from | 0,
      to: g.getAttribute("data-to") | 0,
      kind: g.getAttribute("data-kind") || "seq",
    };
  }

  function cardFromEvent(ev) {
    var el = ev.target && ev.target.closest ? ev.target.closest(".vcard") : null;
    if (!el) return null;
    if (el.dataset.i === "start") return { el: el, i: "start" };
    return { el: el, i: el.dataset.i | 0 };
  }

  function cardIndexAtPoint(clientX, clientY) {
    var hit = document.elementFromPoint(clientX, clientY);
    var card = hit && hit.closest ? hit.closest(".vcard") : null;
    if (!card || card.dataset.i === "start") return -1;
    return card.dataset.i | 0;
  }

  function hitInputCard(clientX, clientY) {
    var wrap = $("#visualCards");
    if (!wrap) return -1;
    var nodes = wrap.querySelectorAll(".vcard");
    for (var i = 0; i < nodes.length; i++) {
      if (nodes[i].dataset.i === "start") continue;
      if (nodes[i].classList.contains("loop-tag")) continue;
      if (nodes[i].classList.contains("defineBlock")) continue;
      var ports = nodes[i].querySelectorAll(".vport.in");
      var p;
      for (p = 0; p < ports.length; p++) {
        var r = ports[p].getBoundingClientRect();
        var pad = 10;
        if (clientX >= r.left - pad && clientX <= r.right + pad && clientY >= r.top - pad && clientY <= r.bottom + pad) {
          return nodes[i].dataset.i | 0;
        }
      }
    }
    var hit = document.elementFromPoint(clientX, clientY);
    var card = hit && hit.closest ? hit.closest(".vcard") : null;
    if (!card || card.dataset.i === "start" || card.classList.contains("loop-tag")
      || card.classList.contains("defineBlock")) return -1;
    return card.dataset.i | 0;
  }

  function moveSubtreeDom(index, nx, ny) {
    var list = actions();
    var a = list[index];
    if (!a) return;
    var ox = cardPos(a).x;
    var oy = cardPos(a).y;
    var dx = nx - ox;
    var dy = ny - oy;
    var end = isDeclContainerType(a.type) ? subtreeEnd(index) : index + 1;
    var cards = $("#visualCards");
    for (var j = index; j < end; j++) {
      list[j]._vx = cardPos(list[j]).x + dx;
      list[j]._vy = cardPos(list[j]).y + dy;
      if (cards) {
        var node = cards.querySelector('.vcard[data-i="' + j + '"]');
        if (node) {
          node.style.left = list[j]._vx + "px";
          node.style.top = list[j]._vy + "px";
        }
      }
    }
  }

  function moveCardVisual(index, nx, ny) {
    var list = actions();
    var a = list[index];
    if (!a) return;
    if (isDeclContainerType(a.type)) {
      moveSubtreeDom(index, nx, ny);
      return;
    }
    if (a.type === "loop") {
      var mark = null;
      var m;
      for (m = 0; m < vis.loopMarks.length; m++) {
        if (vis.loopMarks[m].loop === index) mark = vis.loopMarks[m];
      }
      var lp = cardPos(a);
      var dx = nx - lp.x;
      var dy = ny - lp.y;
      moveSubtreeDom(index, nx, ny);
      if (mark && list[mark.head]) {
        var hp = cardPos(list[mark.head]);
        moveSubtreeDom(mark.head, hp.x + dx, hp.y + dy);
      }
      return;
    }
    moveSubtreeDom(index, nx, ny);
    var mh = loopMarkByHead(index);
    if (mh) glueLoopToHead(mh.loop, mh.head);
  }

  function portKindFromEl(port) {
    var p = port && port.dataset ? port.dataset.port : "out";
    if (p === "outBack") return "back";
    if (p === "ifTrue") return "ifTrue";
    if (p === "ifFalse") return "ifFalse";
    return "seq";
  }

  function onHostWheel(ev) {
    if (!vis.mode) return;
    ev.preventDefault();
    var factor = ev.deltaY < 0 ? ZOOM_FACTOR : 1 / ZOOM_FACTOR;
    setZoomAt(vis.zoom * factor, ev.clientX, ev.clientY);
  }

  function onHostContextMenu(ev) {
    if (!vis.mode) return;
    ev.preventDefault();
    ev.stopPropagation();
    if (vis.ignoreContextMenu) {
      vis.ignoreContextMenu = false;
      return;
    }
    if (vis.drag && vis.drag.kind === "marquee") return;
    var world = clientToWorld(ev.clientX, ev.clientY);
    var edge = edgeFromEvent(ev);
    if (edge) {
      showEdgeMenu(ev, edge);
      return;
    }
    var card = cardFromEvent(ev);
    var badge = ev.target && ev.target.closest ? ev.target.closest(".vloop-badge") : null;
    if (badge) {
      var loopI = badge.getAttribute("data-loop") | 0;
      selectAction(loopI, { keepDraft: true });
      showCardMenu(ev, loopI);
      return;
    }
    if (card) {
      if (card.i === "start") {
        selectStart();
        showStartMenu(ev);
      } else {
        var multi = visualSelectedIndices();
        if (!(multi.length >= 2 && isCardSelected(card.i))) selectAction(card.i, { keepDraft: true });
        showCardMenu(ev, card.i);
      }
    } else {
      showBlankMenu(ev, world);
    }
  }

  function onHostMouseDown(ev) {
    if (!vis.mode) return;
    var el = host();
    var sc = scrollEl();
    if (!el) return;
    if (ev.button === 1) {
      ev.preventDefault();
      vis.drag = {
        kind: "pan",
        x: ev.clientX,
        y: ev.clientY,
        sl: sc.scrollLeft,
        st: sc.scrollTop,
      };
      el.classList.add("is-panning");
      return;
    }
    if (ev.button === 2) {
      var onCard = cardFromEvent(ev);
      var onEdge = edgeFromEvent(ev);
      var onPort = ev.target && ev.target.closest ? ev.target.closest(".vport") : null;
      if (!onCard && !onEdge && !onPort) {
        ev.preventDefault();
        vis.drag = {
          kind: "marquee",
          x0: ev.clientX,
          y0: ev.clientY,
          x1: ev.clientX,
          y1: ev.clientY,
          additive: !!(ev.ctrlKey || ev.metaKey),
          moved: false,
        };
      }
      return;
    }
    if (ev.button !== 0) return;
    var handle = ev.target && ev.target.closest ? ev.target.closest(".vedge-handle") : null;
    var edge = edgeFromEvent(ev);
    if (handle || edge) {
      if (!edge) {
        var g = handle && handle.closest ? handle.closest(".visual-edge-g") : null;
        if (g) {
          var from = g.getAttribute("data-from");
          edge = {
            from: from === "start" ? "start" : from | 0,
            to: g.getAttribute("data-to") | 0,
            kind: g.getAttribute("data-kind") || "seq",
          };
        }
      }
      if (!edge) return;
      ev.preventDefault();
      ev.stopPropagation();
      selectEdge(edge);
      var key = routeKey(edge);
      var pts = (vis._edgePts && vis._edgePts[key]) || [];
      var world = clientToWorld(ev.clientX, ev.clientY);
      var seg = handle ? handle.getAttribute("data-seg") | 0 : nearestSeg(pts, world.x, world.y);
      vis.drag = {
        kind: "wire",
        edge: edge,
        key: key,
        pts: pts.map(function (p) {
          return [p[0], p[1]];
        }),
        seg: seg,
        moved: false,
        ox: world.x,
        oy: world.y,
      };
      if (el) el.classList.add("is-dragging");
      return;
    }
    var port = ev.target && ev.target.closest ? ev.target.closest(".vport") : null;
    if (port && port.classList.contains("out")) {
      var card = port.closest(".vcard");
      var id = card ? card.dataset.i : "";
      ev.preventDefault();
      ev.stopPropagation();
      var pk = portKindFromEl(port);
      var pName = port.dataset.port || "out";
      if (id === "start") {
        selectStart();
        vis.drag = {
          kind: "port",
          fromIdx: "start",
          from: portOf("start", "out") || portPointStart("out"),
          cur: portOf("start", "out") || portPointStart("out"),
          portKind: pk,
        };
      } else {
        var i = id | 0;
        var a = actions()[i];
        if (!a) return;
        selectAction(i);
        var fromPt = portOf(i, pName) || portPoint(a, pName);
        vis.drag = {
          kind: "port",
          fromIdx: i,
          from: fromPt,
          cur: fromPt,
          portKind: pk,
        };
      }
      return;
    }
    var cardHit = cardFromEvent(ev);
    if (cardHit) {
      if (ev.target && ev.target.closest && ev.target.closest(".vport")) return;
      ev.preventDefault();
      var badge = ev.target && ev.target.closest ? ev.target.closest(".vloop-badge") : null;
      if (badge) {
        var loopI = badge.getAttribute("data-loop") | 0;
        var headI = cardHit.i | 0;
        selectAction(loopI);
        var hp = cardPos(actions()[headI] || {});
        vis.drag = {
          kind: "card",
          i: headI,
          dx: clientToWorld(ev.clientX, ev.clientY).x - hp.x,
          dy: clientToWorld(ev.clientX, ev.clientY).y - hp.y,
          moved: false,
          startX: hp.x,
          startY: hp.y,
          toggleOff: false,
        };
        return;
      }
      if (cardHit.i === "start") {
        var alreadyStart = vis.selStart;
        if (!alreadyStart) selectStart();
        var sp = startPos();
        vis.drag = {
          kind: "start",
          dx: clientToWorld(ev.clientX, ev.clientY).x - sp.x,
          dy: clientToWorld(ev.clientX, ev.clientY).y - sp.y,
          moved: false,
          startX: sp.x,
          startY: sp.y,
          toggleOff: alreadyStart && !(ev.ctrlKey || ev.metaKey),
        };
        return;
      }
      var hitI = cardHit.i | 0;
      var ctrl = !!(ev.ctrlKey || ev.metaKey);
      var wasOnly = !ctrl && !state().batchMode && state().actionSel === hitI;
      if (ctrl) {
        var cur = visualSelectedIndices();
        if (!cur.length && state().actionSel >= 0) cur = [state().actionSel];
        var at = cur.indexOf(hitI);
        if (at >= 0) cur.splice(at, 1);
        else cur.push(hitI);
        applyMultiSelection(cur);
      } else if (!(visualSelectedIndices().length >= 2 && isCardSelected(hitI))) {
        if (!wasOnly) selectAction(hitI);
      }
      var a2 = actions()[hitI];
      var p2 = cardPos(a2);
      var group = groupMoveIndices(hitI);
      var origins = {};
      group.forEach(function (gi) {
        var gp = cardPos(actions()[gi] || {});
        origins[gi] = { x: gp.x, y: gp.y };
      });
      vis.drag = {
        kind: "card",
        i: hitI,
        group: group.length > 1 ? group : null,
        origins: origins,
        dx: clientToWorld(ev.clientX, ev.clientY).x - p2.x,
        dy: clientToWorld(ev.clientX, ev.clientY).y - p2.y,
        moved: false,
        startX: p2.x,
        startY: p2.y,
        toggleOff: wasOnly,
      };
      return;
    }
    vis.drag = {
      kind: "pan",
      x: ev.clientX,
      y: ev.clientY,
      sl: sc.scrollLeft,
      st: sc.scrollTop,
    };
    el.classList.add("is-panning");
  }

  function onMouseMove(ev) {
    if (!vis.drag || !vis.mode) return;
    var el = host();
    var sc = scrollEl();
    if (vis.drag.kind === "pan" && sc) {
      sc.scrollLeft = vis.drag.sl - (ev.clientX - vis.drag.x);
      sc.scrollTop = vis.drag.st - (ev.clientY - vis.drag.y);
      return;
    }
    if (vis.drag.kind === "marquee") {
      vis.drag.x1 = ev.clientX;
      vis.drag.y1 = ev.clientY;
      if (!vis.drag.moved && (Math.abs(ev.clientX - vis.drag.x0) > 4 || Math.abs(ev.clientY - vis.drag.y0) > 4)) {
        vis.drag.moved = true;
      }
      if (vis.drag.moved) setMarqueeBox(vis.drag.x0, vis.drag.y0, vis.drag.x1, vis.drag.y1);
      return;
    }
    if (vis.drag.kind === "start") {
      var ws = clientToWorld(ev.clientX, ev.clientY);
      var nx = ws.x - vis.drag.dx;
      var ny = ws.y - vis.drag.dy;
      if (ev.shiftKey) {
        if (!vis.drag.axis) {
          vis.drag.axis = Math.abs(nx - vis.drag.startX) >= Math.abs(ny - vis.drag.startY) ? "h" : "v";
        }
        if (vis.drag.axis === "h") ny = vis.drag.startY;
        else nx = vis.drag.startX;
      } else vis.drag.axis = null;
      if (!vis.drag.moved && (Math.abs(nx - vis.drag.startX) > 2 || Math.abs(ny - vis.drag.startY) > 2)) {
        vis.drag.moved = true;
        pushUndo();
      }
      vis.start = { x: nx, y: ny };
      var startNode = $("#visualCards") && $("#visualCards").querySelector('.vcard[data-i="start"]');
      if (startNode) {
        startNode.style.left = nx + "px";
        startNode.style.top = ny + "px";
        startNode.classList.add("is-moving");
      }
      maybeAutoPan(ev);
      applyZoomTransform();
      renderEdgesOnly();
      return;
    }
    if (vis.drag.kind === "card") {
      var w = clientToWorld(ev.clientX, ev.clientY);
      var nx2 = w.x - vis.drag.dx;
      var ny2 = w.y - vis.drag.dy;
      if (ev.shiftKey) {
        if (!vis.drag.axis) {
          vis.drag.axis = Math.abs(nx2 - vis.drag.startX) >= Math.abs(ny2 - vis.drag.startY) ? "h" : "v";
        }
        if (vis.drag.axis === "h") ny2 = vis.drag.startY;
        else nx2 = vis.drag.startX;
      } else vis.drag.axis = null;
      if (!vis.drag.moved && (Math.abs(nx2 - vis.drag.startX) > 2 || Math.abs(ny2 - vis.drag.startY) > 2)) {
        vis.drag.moved = true;
        pushUndo();
      }
      if (vis.drag.group && vis.drag.origins) {
        var gdx = nx2 - vis.drag.startX;
        var gdy = ny2 - vis.drag.startY;
        vis.drag.group.forEach(function (gi) {
          var o = vis.drag.origins[gi];
          if (o) moveCardVisual(gi, o.x + gdx, o.y + gdy);
        });
      } else {
        moveCardVisual(vis.drag.i, nx2, ny2);
      }
      var node = $("#visualCards") && $("#visualCards").querySelector('.vcard[data-i="' + vis.drag.i + '"]');
      if (node) node.classList.add("is-moving");
      maybeAutoPan(ev);
      applyZoomTransform();
      renderWraps(actions());
      renderEdgesOnly();
      return;
    }
    if (vis.drag.kind === "port") {
      vis.drag.cur = clientToWorld(ev.clientX, ev.clientY);
      maybeAutoPan(ev);
      renderEdgesOnly();
      return;
    }
    if (vis.drag.kind === "wire") {
      var ww = clientToWorld(ev.clientX, ev.clientY);
      if (!vis.drag.moved && (Math.abs(ww.x - vis.drag.ox) > 2 || Math.abs(ww.y - vis.drag.oy) > 2)) {
        vis.drag.moved = true;
        pushUndo();
      }
      vis.drag.pts = dragWirePts(vis.drag.pts, vis.drag.seg, ww.x, ww.y);
      if (!vis.wireRoutes) vis.wireRoutes = {};
      vis.wireRoutes[vis.drag.key] = interiorCorners(vis.drag.pts);
      vis.graphTouched = true;
      maybeAutoPan(ev);
      renderEdgesOnly();
    }
  }

  function renderEdgesOnly() {
    var list = actions();
    var edgesSvg = $("#visualEdges");
    if (!edgesSvg) return;
    edgesSvg.innerHTML = edgeSvg(list);
  }

  function onMouseUp(ev) {
    if (!vis.drag) return;
    var el = host();
    if (el) {
      el.classList.remove("is-panning");
      el.classList.remove("is-dragging");
    }
    var drag = vis.drag;
    vis.drag = null;
    if (drag.kind === "marquee") {
      hideMarquee();
      vis.ignoreContextMenu = true;
      if (drag.moved) {
        var hit = cardsInClientRect(drag.x0, drag.y0, drag.x1, drag.y1);
        if (hit.length) {
          if (drag.additive) {
            var cur = visualSelectedIndices();
            hit.forEach(function (i) {
              if (cur.indexOf(i) < 0) cur.push(i);
            });
            applyMultiSelection(cur);
          } else {
            applyMultiSelection(hit);
          }
        }
      } else {
        showBlankMenu(ev, clientToWorld(ev.clientX, ev.clientY));
      }
      return;
    }
    if (drag.kind === "start") {
      var startNode = $("#visualCards") && $("#visualCards").querySelector('.vcard[data-i="start"]');
      if (startNode) startNode.classList.remove("is-moving");
      if (drag.moved) {
        vis.start = { x: snap(vis.start.x), y: snap(vis.start.y) };
        forgetRoutesTouching(["start"]);
      } else if (drag.toggleOff) {
        clearSelection();
        return;
      }
      applyZoomTransform();
      renderCanvas();
      return;
    }
    if (drag.kind === "card") {
      var node = $("#visualCards") && $("#visualCards").querySelector('.vcard[data-i="' + drag.i + '"]');
      if (node) node.classList.remove("is-moving");
      if (drag.moved) {
        if (drag.group && drag.group.length && drag.origins) {
          var o0 = drag.origins[drag.i];
          var gx = o0 ? snap(cardPos(actions()[drag.i]).x) - o0.x : 0;
          var gy = o0 ? snap(cardPos(actions()[drag.i]).y) - o0.y : 0;
          drag.group.forEach(function (gi) {
            var o = drag.origins[gi];
            if (o) moveCardVisual(gi, o.x + gx, o.y + gy);
          });
        } else {
          var a = actions()[drag.i];
          if (a) moveCardVisual(drag.i, snap(cardPos(a).x), snap(cardPos(a).y));
        }
        forgetRoutesTouching(drag.group && drag.group.length ? drag.group.concat(["start"]) : [drag.i, "start"]);
        var listDrop = actions();
        if (listDrop[drag.i] && listDrop[drag.i].type === "loop" && !directChildren(listDrop, drag.i).length) {
          if (node) node.style.pointerEvents = "none";
          var drop = cardIndexAtPoint(ev.clientX, ev.clientY);
          if (node) node.style.pointerEvents = "";
          if (drop >= 0 && drop !== drag.i) {
            attachLoopCardTo(drag.i, drop);
            return;
          }
        }
      } else if (drag.toggleOff) {
        clearSelection();
        return;
      }
      applyZoomTransform();
      renderCanvas();
      return;
    }
    if (drag.kind === "wire") {
      if (drag.moved && vis.wireRoutes) vis.wireRoutes[drag.key] = interiorCorners(drag.pts);
      renderCanvas();
      return;
    }
    if (drag.kind === "port") {
      var toIdx = hitInputCard(ev.clientX, ev.clientY);
      if (toIdx >= 0 && (toIdx !== drag.fromIdx || drag.fromIdx !== "start")) {
        if (!connectSeq(drag.fromIdx, toIdx, drag.portKind || "seq")) renderCanvas();
        else if (typeof A().renderEditorActions === "function") A().renderEditorActions(actions());
        else renderCanvas();
      } else renderCanvas();
    }
  }

  function onHostDblClick(ev) {
    if (!vis.mode) return;
    if (cardFromEvent(ev) || edgeFromEvent(ev)) return;
    fitView();
  }

  function onHostKeyDown(ev) {
    if (!vis.mode) return;
    var t = ev.target;
    if (t && (t.isContentEditable || t.tagName === "INPUT" || t.tagName === "TEXTAREA")) return;
    if ((ev.ctrlKey || ev.metaKey) && (ev.key === "c" || ev.key === "C")) {
      if (state().actionSel >= 0) {
        ev.preventDefault();
        copyCard(state().actionSel);
      }
      return;
    }
    if ((ev.ctrlKey || ev.metaKey) && (ev.key === "x" || ev.key === "X")) {
      if (state().actionSel >= 0) {
        ev.preventDefault();
        cutCard(state().actionSel);
      }
      return;
    }
    if ((ev.ctrlKey || ev.metaKey) && (ev.key === "v" || ev.key === "V")) {
      ev.preventDefault();
      var at = vis.lastMenuWorld || viewportCenterWorld();
      pasteAt(at.x, at.y);
      return;
    }
    if ((ev.ctrlKey || ev.metaKey) && (ev.key === "0" || ev.key === "1")) {
      ev.preventDefault();
      fitView();
      return;
    }
    if (ev.key === "Escape") {
      ev.preventDefault();
      clearSelection();
      renderCanvas();
      return;
    }
    if (ev.key === "Delete" || ev.key === "Backspace") {
      if (vis.selEdge) {
        ev.preventDefault();
        deleteEdge(vis.selEdge);
        return;
      }
    }
    var step = ev.shiftKey ? 80 : 40;
    if (ev.key === "ArrowLeft") {
      ev.preventDefault();
      panBy(-step, 0);
    } else if (ev.key === "ArrowRight") {
      ev.preventDefault();
      panBy(step, 0);
    } else if (ev.key === "ArrowUp") {
      ev.preventDefault();
      panBy(0, -step);
    } else if (ev.key === "ArrowDown") {
      ev.preventDefault();
      panBy(0, step);
    }
  }

  function setDisplayImportant(el, hide) {
    if (!el) return;
    if (hide) el.style.setProperty("display", "none", "important");
    else el.style.removeProperty("display");
  }

  function syncChrome() {
    var col = leftCol();
    var btn = $("#btnVisualMode");
    var editor = $("#editor");
    var app = $("#app");
    if (col) col.classList.toggle("is-visual", vis.mode);
    if (editor) editor.classList.toggle("is-visual", vis.mode);
    if (app) app.classList.toggle("visual-editor", vis.mode);
    setDisplayImportant($("#actionTable"), vis.mode);
    setDisplayImportant($("#editorAddBar"), vis.mode);
    if (btn) btn.textContent = vis.mode ? "代码化" : "可视化";
  }

  function setMode(on) {
    on = !!on;
    if (on && typeof A().flushPendingLiveEdit === "function") A().flushPendingLiveEdit();
    if (state().batchMode && typeof A().enterEditorBatch === "function") A().enterEditorBatch(false);
    vis.mode = on;
    if (!on) {
      vis.selStart = false;
      vis.selEdge = null;
      vis.forceView = null;
      if (!vis.graphTouched) vis.linksReady = false;
    }
    syncChrome();
    if (on) {
      vis.pendingScroll = null;
      vis.needAlign = true;
      pruneEmptyElseActions(actions());
      var fresh = actions().length && !hasAnyLayout(actions());
      if (fresh) autoLayout(actions());
      if (!vis.linksReady) generateDefaultLinks(actions());
      if (fresh) alignHorizontalWires(actions());
      if (vis.forceView == null) vis.forceView = "start";
      renderCanvas();
      var el = host();
      if (el) el.focus({ preventScroll: true });
      applyEntryView();
    } else if (typeof A().renderEditorActions === "function") {
      A().renderEditorActions(actions());
    }
  }

  function tryLeaveVisual() {
    if (!vis.graphTouched) {
      pruneEmptyElseActions(actions());
      setMode(false);
      return;
    }
    commitGraphToList(function (ok) {
      if (ok) setMode(false);
    }, "与初始节点无关的流程会直接删除，确定代码化？");
  }

  function goToCodeFromCard(index) {
    var list = actions();
    var act = list[index];
    function afterLeave() {
      var next = actions();
      var ni = act ? next.indexOf(act) : -1;
      if (ni >= 0 && typeof A().selectEditorAction === "function") A().selectEditorAction(ni);
    }
    if (!vis.graphTouched) {
      pruneEmptyElseActions(actions());
      setMode(false);
      afterLeave();
      return;
    }
    commitGraphToList(function (ok) {
      if (!ok) return;
      setMode(false);
      afterLeave();
    }, "与初始节点无关的流程会直接删除，确定代码化？");
  }

  function toggleMode() {
    if (vis.mode) tryLeaveVisual();
    else {
      vis.forceView = "start";
      setMode(true);
    }
  }

  function enterVisual(focus) {
    vis.forceView = focus == null || focus === "start" ? "start" : focus | 0;
    setMode(true);
    if (focus != null && focus !== "start") {
      vis.skipReveal = true;
      selectAction(focus | 0);
      vis.skipReveal = false;
    }
  }

  function reset() {
    vis.mode = false;
    vis.zoom = 1;
    vis.undo = [];
    vis.clipboard = null;
    vis.pendingScroll = null;
    vis.drag = null;
    vis.skipAutoFit = false;
    vis.links = [];
    vis.linksReady = false;
    vis.start = { x: PAD, y: PAD };
    vis.selStart = false;
    vis.selEdge = null;
    vis.graphTouched = false;
    vis.loopMarks = [];
    vis.wireRoutes = {};
    vis._edgePts = {};
    syncChrome();
  }

  function applyLayout(layout) {
    if (!layout || typeof layout !== "object") return;
    var list = actions();
    var nodes = Array.isArray(layout.nodes) ? layout.nodes : [];
    for (var i = 0; i < list.length; i++) {
      var n = nodes[i];
      if (n && typeof n === "object") {
        if (typeof n.x === "number") list[i]._vx = n.x;
        if (typeof n.y === "number") list[i]._vy = n.y;
      }
    }
    if (layout.start && typeof layout.start === "object") {
      vis.start = { x: layout.start.x | 0, y: layout.start.y | 0 };
    }
    if (Array.isArray(layout.links)) {
      vis.links = normalizeLinks(layout.links);
      vis.linksReady = true;
      vis.graphTouched = true;
    } else {
      vis.linksReady = false;
    }
    if (Array.isArray(layout.loopMarks)) vis.loopMarks = layout.loopMarks.slice();
    vis.wireRoutes = layout.routes && typeof layout.routes === "object" ? layout.routes : {};
    repairLoopEntries();
  }

  function applyOpenView(preferVisual) {
    if (!preferVisual) return;
    vis.forceView = "start";
    setMode(true);
  }

  function collectLayout() {
    var list = actions();
    var el = scrollEl();
    return {
      viewMode: vis.mode ? "visual" : "list",
      zoom: vis.zoom,
      scrollX: el ? el.scrollLeft : 0,
      scrollY: el ? el.scrollTop : 0,
      start: { x: Math.round(vis.start.x), y: Math.round(vis.start.y) },
      nodes: list.map(function (a) {
        var p = cardPos(a);
        return { x: Math.round(p.x), y: Math.round(p.y) };
      }),
      routes: vis.wireRoutes || {},
    };
  }

  function collectLayoutIfAny() {
    if (!vis.mode && !hasAnyLayout(actions()) && !vis.linksReady) return undefined;
    return collectLayout();
  }

  function onActionsChanged() {
    if (!vis.mode) return;
    renderCanvas();
  }

  function ensureHud() {
    var el = host();
    if (!el) return;
    var hud = $("#visualHud");
    if (!hud) {
      hud = document.createElement("div");
      hud.id = "visualHud";
      hud.className = "visual-hud";
      hud.setAttribute("aria-hidden", "true");
    }
    if (hud.parentNode !== el) el.appendChild(hud);
  }

  function bindUi() {
    var btn = $("#btnVisualMode");
    if (btn && !btn._qstVis) {
      btn._qstVis = true;
      btn.addEventListener("click", function () {
        toggleMode();
      });
    }
    var el = host();
    if (el && !el._qstVis) {
      el._qstVis = true;
      el.addEventListener("wheel", onHostWheel, { passive: false });
      el.addEventListener("contextmenu", onHostContextMenu);
      el.addEventListener("mousedown", onHostMouseDown);
      el.addEventListener("keydown", onHostKeyDown);
      el.addEventListener("dblclick", onHostDblClick);
      el.addEventListener("auxclick", function (ev) {
        if (ev.button === 1) ev.preventDefault();
      });
    }
    ensureHud();
    if (!document._qstVisMove) {
      document._qstVisMove = true;
      document.addEventListener("mousemove", onMouseMove);
      document.addEventListener("mouseup", onMouseUp);
    }
    syncChrome();
  }

  window.QstVisualEditor = {
    isVisual: isVisual,
    setMode: setMode,
    enterVisual: enterVisual,
    toggleMode: toggleMode,
    reset: reset,
    render: renderCanvas,
    applyDisplayPrefs: applyDisplayPrefs,
    onActionsChanged: onActionsChanged,
    syncSelectionUi: syncSelectionUi,
    refreshCard: refreshCard,
    pushUndo: pushUndo,
    discardLastUndo: discardLastUndo,
    collectHistory: collectHistory,
    applyHistory: applyHistory,
    isHistoryHeld: isHistoryHeld,
    isInteracting: isInteracting,
    afterListDelete: afterListDelete,
    deleteCard: deleteCard,
    cutCard: cutCard,
    deleteSelectedEdgeIfAny: deleteSelectedEdgeIfAny,
    setClipboard: setClipboard,
    collectLayout: collectLayout,
    collectLayoutIfAny: collectLayoutIfAny,
    applyLayout: applyLayout,
    applyOpenView: applyOpenView,
    selectAction: selectAction,
    afterMerge: afterMerge,
    orderIndicesByLinks: orderIndicesByLinks,
    commitGraphToList: commitGraphToList,
    fitView: fitView,
    onInsert: onInsert,
    onDelete: onDelete,
    onMove: onMove,
    onReplace: onReplace,
  };

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", bindUi);
  } else {
    bindUi();
  }
})();
