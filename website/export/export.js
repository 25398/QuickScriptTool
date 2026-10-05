/**
 * website/export/export.js —— 「在线脚本工坊」页面逻辑
 *
 * 只做三件事：
 *   ① 维护一个 UI 动作数组（增删改排序），并把当前状态渲染成表单
 *   ② 每次变更就调 QstWebExport.validate / scanCapabilities，把「能不能导出、缺什么」写在脸上
 *   ③ 点导出 → QstWebExport.exportExe（全部在本地完成，不发任何请求，除了取 5.5 MB 模板）
 *
 * ⚠ 动作模型里坐标一律用**归一化小数**（px/screenW）存，像素只是显示形态。
 *   理由见 qst-web-export.js 文件头说明 2：脚本文件本身就是"相对屏幕比例"的语义，
 *   存像素会在用户改分辨率时静默漂移。
 */
(function () {
  "use strict";

  var W = window.QstWebExport;
  if (!W) { alert("qst-web-export.js 没加载成功，页面无法工作。"); return; }

  var $ = function (id) { return document.getElementById(id); };

  // ── 屏幕分辨率自动探测 ───────────────────────────────────────
  // `screen.width` 是 CSS 像素、随页面缩放变化，乘 devicePixelRatio 才是物理像素。
  function detectScreen() {
    var dpr = window.devicePixelRatio || 1;
    var w = Math.round((window.screen && window.screen.width ? window.screen.width : 1920) * dpr);
    var h = Math.round((window.screen && window.screen.height ? window.screen.height : 1080) * dpr);
    if (!(w > 0) || !(h > 0)) { w = 1920; h = 1080; }
    return { w: w, h: h };
  }

  var state = {
    name: "我的在线脚本",
    screenW: 1920,
    screenH: 1080,
    actions: [],
    images: [],       // {name, data:Uint8Array, url}
    buildSeq: 0,
  };

  function screen() { return { w: state.screenW, h: state.screenH }; }
  function px2f(px, span) { return W.normalizePoint(px, span); }
  function f2px(f, span) { return W.fractionToPx(f, span); }

  // ══════════════════════════════════════════════════════════
  // 动作工厂
  // ══════════════════════════════════════════════════════════
  function newAction(type) {
    var s = screen();
    var base = { type: type, count: 1, interval: 0 };
    switch (type) {
      case "mouseClick": return Object.assign(base, { fx: 0.5, fy: 0.5, button: "left", count: 0, interval: 2 });
      case "moveMouse": return Object.assign(base, { fx: 0.5, fy: 0.5 });
      case "mouseDown":
      case "mouseUp": return Object.assign(base, { button: "left" });
      case "scrollWheel": return Object.assign(base, { direction: "down", steps: 3, count: 0, interval: 1 });
      case "keyClick": return Object.assign(base, { vk: 65, count: 0, interval: 5 });
      case "keyDown":
      case "keyUp": return Object.assign(base, { vk: 87 });
      case "quickInput": return Object.assign(base, { inputText: "hello", charInterval: 0.01 });
      case "wait": return Object.assign(base, { seconds: 1 });
      case "findImage": return Object.assign(base, { imageName: "", threshold: 65, followUp: 0, offsetX: 0, offsetY: 0, count: 0, interval: 2 });
      case "stopMacro": return base;
      default: return base;
    }
  }

  /** UI 动作 → 导出模块要的形态（像素 + 秒）。 */
  function toExportAction(a) {
    var s = screen();
    var o = { type: a.type, count: a.count, interval: a.interval };
    if (a.fx != null) { o.px = f2px(a.fx, s.w); o.py = f2px(a.fy, s.h); }
    if (a.button) o.button = a.button;
    if (a.direction) o.direction = a.direction;
    if (a.steps != null) o.steps = a.steps;
    if (a.vk != null) o.vk = a.vk;
    if (a.inputText != null) o.inputText = a.inputText;
    if (a.charInterval != null) o.charInterval = a.charInterval;
    if (a.seconds != null) o.seconds = a.seconds;
    if (a.type === "findImage") {
      o.imageName = a.imageName; o.threshold = a.threshold; o.followUp = a.followUp;
      o.offsetX = 0; o.offsetY = 0;
    }
    return o;
  }
  function exportActions() { return state.actions.map(toExportAction); }

  // ══════════════════════════════════════════════════════════
  // 渲染
  // ══════════════════════════════════════════════════════════
  var tpl = $("weRowTpl");

  function mkInput(kind, value, attrs) {
    var el = document.createElement("input");
    el.type = kind;
    if (value != null) el.value = value;
    Object.keys(attrs || {}).forEach(function (k) { el.setAttribute(k, attrs[k]); });
    return el;
  }
  function mkLabel(text, node) {
    var l = document.createElement("label");
    l.appendChild(document.createTextNode(text));
    l.appendChild(node);
    return l;
  }
  function mkSelect(options, value) {
    var sel = document.createElement("select");
    options.forEach(function (o) {
      var op = document.createElement("option");
      op.value = o.v; op.textContent = o.t;
      sel.appendChild(op);
    });
    sel.value = value;
    return sel;
  }

  function renderRowBody(a, body) {
    var s = screen();
    function coordFields() {
      body.appendChild(mkLabel("X", mkInput("number", f2px(a.fx, s.w), { min: 0, max: s.w, step: 1 })))
        .lastChild.addEventListener("change", function () { a.fx = px2f(this.value, screen().w); refresh(); });
      body.appendChild(mkLabel("Y", mkInput("number", f2px(a.fy, s.h), { min: 0, max: s.h, step: 1 })))
        .lastChild.addEventListener("change", function () { a.fy = px2f(this.value, screen().h); refresh(); });
      body.appendChild(mkLabel("（占屏幕", (function () {
        var em = document.createElement("em");
        em.style.fontStyle = "normal";
        em.style.color = "var(--text-3)";
        em.textContent = (a.fx * 100).toFixed(1) + "% , " + (a.fy * 100).toFixed(1) + "%）";
        return em;
      })()));
    }

    if (a.type === "mouseClick" || a.type === "moveMouse") {
      coordFields();
      if (a.type === "mouseClick") {
        var bs = mkSelect(Object.keys(W.BUTTONS).map(function (k) { return { v: k, t: W.BUTTONS[k] }; }), a.button);
        bs.addEventListener("change", function () { a.button = this.value; scheduleScan(); });
        body.appendChild(bs);
      }
    } else if (a.type === "mouseDown" || a.type === "mouseUp") {
      var bs2 = mkSelect(Object.keys(W.BUTTONS).map(function (k) { return { v: k, t: W.BUTTONS[k] }; }), a.button);
      bs2.addEventListener("change", function () { a.button = this.value; });
      body.appendChild(bs2);
      body.appendChild(mkText("提示", "配合另一条「鼠标按下/松开」做拖拽、长按"));
    } else if (a.type === "scrollWheel") {
      var ds = mkSelect([{ v: "up", t: "向上" }, { v: "down", t: "向下" }], a.direction);
      ds.addEventListener("change", function () { a.direction = this.value; });
      body.appendChild(ds);
      var st = mkInput("number", a.steps, { min: 1, max: 100, step: 1 });
      st.addEventListener("change", function () { a.steps = Math.max(1, parseInt(this.value, 10) || 1); });
      body.appendChild(mkLabel("格数", st));
    } else if (a.type === "keyClick" || a.type === "keyDown" || a.type === "keyUp") {
      var ks = mkSelect(W.KEY_CHOICES.map(function (k) { return { v: String(k.vk), t: k.label }; }), String(a.vk));
      ks.addEventListener("change", function () { a.vk = parseInt(this.value, 10); });
      body.appendChild(ks);
    } else if (a.type === "quickInput") {
      var it = mkInput("text", a.inputText, {});
      it.className = "we-wide";
      it.addEventListener("input", function () { a.inputText = this.value; refresh(); });
      body.appendChild(it);
    } else if (a.type === "wait") {
      var ws = mkInput("number", a.seconds, { min: 0, max: 86400, step: 0.1 });
      ws.addEventListener("change", function () { a.seconds = Math.max(0, parseFloat(this.value) || 0); refresh(); });
      body.appendChild(mkLabel("等待", ws));
      body.appendChild(mkText("", "秒"));
    } else if (a.type === "findImage") {
      var is = mkSelect([{ v: "", t: "（先在右侧上传模板图）" }].concat(
        state.images.map(function (im) { return { v: im.name, t: im.name }; })), a.imageName);
      is.addEventListener("change", function () { a.imageName = this.value; refresh(); });
      body.appendChild(is);
      var th = mkInput("number", a.threshold, { min: 1, max: 100, step: 1 });
      th.addEventListener("change", function () { a.threshold = parseInt(this.value, 10) || 65; });
      body.appendChild(mkLabel("相似度", th));
      var fu = mkSelect([{ v: "0", t: "点击命中处" }, { v: "1", t: "只移动过去" }], String(a.followUp));
      fu.addEventListener("change", function () { a.followUp = parseInt(this.value, 10); });
      body.appendChild(fu);
    } else if (a.type === "stopMacro") {
      body.appendChild(mkText("", "立刻结束整个脚本"));
    }
  }

  function mkText(_l, text) {
    var sp = document.createElement("span");
    sp.style.fontSize = "12px"; sp.style.color = "var(--text-3)";
    sp.textContent = text;
    return sp;
  }

  function render() {
    var host = $("weActions");
    host.innerHTML = "";
    if (!state.actions.length) {
      var d = document.createElement("div");
      d.className = "we-empty";
      d.textContent = "还没有动作 —— 先点上面任意一个预设，或从下面添加。";
      host.appendChild(d);
    }
    state.actions.forEach(function (a, i) {
      var row = tpl.content.firstElementChild.cloneNode(true);
      row.querySelector(".we-row-no").textContent = String(i + 1);
      row.querySelector(".we-row-label").textContent = (W.ACTIONS[a.type] || {}).label || a.type;

      renderRowBody(a, row.querySelector(".we-row-body"));

      var supportsRepeat = (W.ACTIONS[a.type] || {}).repeat !== false || a.type === "findImage";
      var cIn = row.querySelector(".we-count-input");
      var iIn = row.querySelector(".we-interval-input");
      cIn.value = a.count;
      iIn.value = a.interval;
      cIn.title = "执行次数；0 = 一直重复";
      if (!supportsRepeat) {
        row.querySelector(".we-repeat").style.display = "none";
      } else {
        cIn.addEventListener("change", function () {
          a.count = Math.max(0, Math.min(W.MAX_CLICK_COUNT, parseInt(this.value, 10) || 0));
          this.value = a.count; refresh();
        });
        iIn.addEventListener("change", function () {
          a.interval = Math.max(0, parseFloat(this.value) || 0);
          this.value = a.interval; refresh();
        });
      }

      row.querySelectorAll(".we-icon-btn").forEach(function (b) {
        b.addEventListener("click", function () {
          var act = b.getAttribute("data-act");
          if (act === "del") state.actions.splice(i, 1);
          else if (act === "up" && i > 0) { var t = state.actions[i - 1]; state.actions[i - 1] = a; state.actions[i] = t; }
          else if (act === "down" && i < state.actions.length - 1) { var t2 = state.actions[i + 1]; state.actions[i + 1] = a; state.actions[i] = t2; }
          refresh();
        });
      });
      host.appendChild(row);
    });

    var n = state.actions.length;
    var c = $("weCount");
    c.textContent = n + " / " + W.MAX_ACTIONS;
    c.className = "we-count" + (n > W.MAX_ACTIONS ? " over" : "");
  }

  // ══════════════════════════════════════════════════════════
  // 校验 + 提示
  // ══════════════════════════════════════════════════════════
  var scanTimer = 0;
  function scheduleScan() { clearTimeout(scanTimer); scanTimer = setTimeout(updateScan, 60); }

  function updateScan() {
    var el = $("weScan");
    // ⚠ file:// 下**一定**导不出来（浏览器禁止 fetch 本地文件）。
    //   与其等用户点了导出再看一句 `Failed to fetch`，不如一进来就把话说明白 ——
    //   这是 2026-09-28 用户实测踩到的第一件事。
    if (W.isFileProtocol()) {
      el.innerHTML = "<b style='color:#b91c1c'>当前是直接双击打开的（file://），"
        + "浏览器不允许这样读取播放器模板，导出一定失败。</b><br>"
        + "请改用本地服务器打开：在仓库根目录运行 "
        + "<code>powershell -ExecutionPolicy Bypass -File tools\\serve_website.ps1</code>，"
        + "然后访问它打印出的地址（例如 <code>http://127.0.0.1:8090/export/export.html</code>）。";
      $("weExport").disabled = true;
      $("weExport").style.opacity = ".55";
      $("weExport").style.cursor = "not-allowed";
      return;
    }
    var v = W.validate(exportActions());
    var flat = W.buildScriptActions(exportActions(), screen());
    var scan = W.scanCapabilities(flat, state.images);

    var html = "";
    html += "动作 <b>" + flat.length + "</b> 条";
    if (scan.imageActions) html += " · 找图/找色 <b>" + scan.imageActions + "</b> 处";
    if (scan.selfContainedOnly) html += " · <span class='ok'>不依赖任何外部组件，最省心</span>";
    html += "<br>预计体积：约 <b>" + (5.5 + (scan.imageActions ? state.images.length * 0.3 : 0)).toFixed(1) + " MB</b>（播放器约 5.5 MB）";

    if (scan.needsOpenCv) {
      html += "<br><span class='warn'>找图动作需要目标电脑有图像识别组件：装了「键鼠工坊」客户端会自动复用；没装则这类动作会跳过。</span>";
    }
    if (v.errors.length) {
      html += "<ul>" + v.errors.map(function (e) { return "<li class='err'>" + esc(e) + "</li>"; }).join("") + "</ul>";
    } else if (v.warnings.length) {
      html += "<ul>" + v.warnings.map(function (e) { return "<li class='warn'>" + esc(e) + "</li>"; }).join("") + "</ul>";
    }
    el.innerHTML = html;
    $("weExport").disabled = !v.ok;
    $("weExport").style.opacity = v.ok ? "1" : ".55";
    $("weExport").style.cursor = v.ok ? "pointer" : "not-allowed";
  }

  function esc(s) {
    return String(s).replace(/[&<>"]/g, function (c) {
      return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c];
    });
  }

  function setStatus(text, kind) {
    var el = $("weStatus");
    el.innerHTML = text ? esc(text) : "";
    el.style.color = kind === "err" ? "#b91c1c" : (kind === "ok" ? "#0f766e" : "");
  }

  // ── 导出进度 ─────────────────────────────────────────────────
  function setProgress(pct, text) {
    $("weProgress").hidden = false;
    $("weProgressBar").style.width = Math.max(0, Math.min(100, Math.round(pct * 100))) + "%";
    if (text) $("weProgressTxt").innerHTML = text;
  }

  function fmtSize(n) {
    var v = Number(n) || 0;
    if (v >= 1024 * 1024) return (v / 1024 / 1024).toFixed(2) + " MB";
    if (v >= 1024) return Math.round(v / 1024) + " KB";
    return v + " B";
  }

  /** 把导出模块的进度事件翻译成人话（各阶段权重：下载 0~90%，打包/组装 90~100%）。 */
  function renderProgress(p) {
    if (!p) return;
    if (p.phase === "player") {
      var r = p.ratio || 0;
      setProgress(r * 0.9, p.cached || p.local
        ? "播放器模板已在本地缓存，跳过下载。"
        : "正在下载播放器模板 <b>" + fmtSize(p.loaded) + " / "
          + (p.total ? fmtSize(p.total) : "?") + "</b>（" + Math.round(r * 100) + "%）");
    } else if (p.phase === "pack") {
      if (p.ratio >= 1) setProgress(0.96, "脚本包已生成 <b>" + fmtSize(p.payloadBytes) + "</b>");
      else setProgress(0.9, "正在打包脚本…");
    } else if (p.phase === "assemble") {
      setProgress(0.98, "正在把脚本追加到播放器上…");
    } else if (p.phase === "done") {
      setProgress(1, "完成 <b>" + fmtSize(p.exeBytes) + "</b>");
    }
  }

  function refresh() { render(); scheduleScan(); }

  // ══════════════════════════════════════════════════════════
  // 模板图
  // ══════════════════════════════════════════════════════════
  function uniqueImageName(raw) {
    var base = W.sanitizeEntryName(raw) || "template.png";
    if (!/\.(png|bmp|jpe?g)$/i.test(base)) base += ".png";
    var used = {};
    state.images.forEach(function (im) { used[im.name] = 1; });
    if (!used[base]) return base;
    var dot = base.lastIndexOf(".");
    var stem = dot > 0 ? base.slice(0, dot) : base;
    var ext = dot > 0 ? base.slice(dot) : "";
    for (var i = 2; i < 1000; i++) {
      var cand = stem + "_" + i + ext;
      if (!used[cand]) return cand;
    }
    return "template_" + Date.now() + base;
  }

  function renderImages() {
    var ul = $("weImgList");
    ul.innerHTML = "";
    state.images.forEach(function (im, i) {
      var li = document.createElement("li");
      var img = document.createElement("img");
      img.src = im.url; img.alt = im.name;
      li.appendChild(img);
      var nm = document.createElement("span");
      nm.className = "nm"; nm.textContent = im.name;
      li.appendChild(nm);
      var del = document.createElement("button");
      del.type = "button"; del.textContent = "移除";
      del.addEventListener("click", function () {
        try { URL.revokeObjectURL(im.url); } catch (e) {}
        state.images.splice(i, 1);
        state.actions.forEach(function (a) {
          if (a.type === "findImage" && a.imageName === im.name) a.imageName = "";
        });
        renderImages(); refresh();
      });
      li.appendChild(del);
      ul.appendChild(li);
    });
  }

  function readFiles(files) {
    var list = Array.prototype.slice.call(files || []);
    if (!list.length) return Promise.resolve();
    var i = 0;
    function step() {
      if (i >= list.length) return Promise.resolve();
      var f = list[i++];
      if (state.images.length >= W.MAX_IMAGES) {
        setStatus("模板图最多 " + W.MAX_IMAGES + " 张，多余的已忽略。", "err");
        return Promise.resolve();
      }
      return f.arrayBuffer().then(function (buf) {
        var data = new Uint8Array(buf);
        var name = uniqueImageName(f.name);
        state.images.push({ name: name, data: data, url: URL.createObjectURL(f) });
        // 若脚本里已经有"缺图的找图动作"，顺手补上第一张
        state.actions.forEach(function (a) {
          if (a.type === "findImage" && !a.imageName) a.imageName = name;
        });
        renderImages(); refresh();
        return step();
      });
    }
    return step();
  }

  // ══════════════════════════════════════════════════════════
  // 初始化
  // ══════════════════════════════════════════════════════════
  function fillAddSelect() {
    var sel = $("weAddType");
    var groups = {};
    Object.keys(W.ACTIONS).forEach(function (t) {
      var g = W.ACTIONS[t].group || "其他";
      (groups[g] = groups[g] || []).push(t);
    });
    Object.keys(groups).forEach(function (g) {
      var og = document.createElement("optgroup");
      og.label = g;
      groups[g].forEach(function (t) {
        var op = document.createElement("option");
        op.value = t;
        op.textContent = W.ACTIONS[t].label;
        og.appendChild(op);
      });
      sel.appendChild(og);
    });
    sel.value = "mouseClick";
    updateAddHint();
  }

  function updateAddHint() {
    var t = $("weAddType").value;
    $("weAddHint").textContent = (W.ACTIONS[t] || {}).hint || "";
  }

  function renderTemplates() {
    var host = $("weTemplates");
    host.innerHTML = "";
    W.templates(state.screenW, state.screenH).forEach(function (t) {
      var b = document.createElement("button");
      b.type = "button";
      b.className = "we-tpl";
      var ttl = document.createElement("b"); ttl.textContent = t.name;
      b.appendChild(ttl);
      b.appendChild(document.createTextNode(t.desc));
      b.addEventListener("click", function () {
        // 预设里的坐标按"生成时的屏幕"算，这里换算回归一化小数存起来
        state.actions = t.actions.map(function (a) {
          var ui = newAction(a.type);
          Object.keys(a).forEach(function (k) { ui[k] = a[k]; });
          if (a.px != null) { ui.fx = px2f(a.px, state.screenW); ui.fy = px2f(a.py, state.screenH); }
          return ui;
        });
        state.name = t.name;
        $("weName").value = state.name;
        setStatus("已套用预设：" + t.name);
        refresh();
      });
      host.appendChild(b);
    });
  }

  function init() {
    var d = detectScreen();
    state.screenW = d.w; state.screenH = d.h;
    $("weScreenW").value = d.w;
    $("weScreenH").value = d.h;
    fillAddSelect();
    renderTemplates();
    renderImages();
    // 默认给一个"能直接导出的最小可用脚本"
    state.actions = W.templates(d.w, d.h)[1].actions.map(function (a) {
      var ui = newAction(a.type);
      Object.keys(a).forEach(function (k) { ui[k] = a[k]; });
      ui.fx = px2f(a.px, d.w); ui.fy = px2f(a.py, d.h);
      return ui;
    });
    state.name = "我的在线脚本";
    refresh();

    $("weAddBtn").addEventListener("click", function () {
      if (state.actions.length >= W.MAX_ACTIONS) {
        setStatus("网页版最多 " + W.MAX_ACTIONS + " 个动作。", "err");
        return;
      }
      state.actions.push(newAction($("weAddType").value));
      refresh();
      var host = $("weActions");
      if (host.lastElementChild) host.lastElementChild.scrollIntoView({ block: "nearest" });
    });
    $("weAddType").addEventListener("change", updateAddHint);

    $("weName").addEventListener("input", function () { state.name = this.value || "我的在线脚本"; });

    function screenChanged() {
      var w = Math.max(640, parseInt($("weScreenW").value, 10) || 1920);
      var h = Math.max(480, parseInt($("weScreenH").value, 10) || 1080);
      state.screenW = w; state.screenH = h;
      renderTemplates();
      refresh();
    }
    $("weScreenW").addEventListener("change", screenChanged);
    $("weScreenH").addEventListener("change", screenChanged);

    $("weImages").addEventListener("change", function () {
      readFiles(this.files).then(function () { this.value = ""; }.bind(this));
    });

    $("weCopyJson").addEventListener("click", function () {
      var v = W.validate(exportActions());
      if (!v.ok) { setStatus(v.errors.join("；"), "err"); return; }
      var json = W.buildScriptFile({
        name: state.name, uiActions: exportActions(),
        screenW: state.screenW, screenH: state.screenH,
      });
      var done = function () { setStatus("脚本 JSON 已复制到剪贴板。", "ok"); };
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(json).then(done, function () { setStatus("复制失败，请检查浏览器权限。", "err"); });
      } else {
        setStatus("这个浏览器不支持剪贴板写入。", "err");
      }
    });

    $("weExport").addEventListener("click", function () {
      var btn = this;
      var v = W.validate(exportActions());
      if (!v.ok) { setStatus(v.errors.join("；"), "err"); return; }
      if (W.isFileProtocol()) { updateScan(); return; }

      // ── 进度 + 可中止 ─────────────────────────────────────────
      // 取模板要下 5.5 MB（慢网络上好几秒），组装本身是毫秒级 ——
      // 所以进度条盯的是下载那段；期间「取消导出」可随时中止（AbortController）。
      var ctrl = (typeof AbortController === "function") ? new AbortController() : null;
      var canceled = false;
      btn.disabled = true;
      btn.textContent = "导出中…";
      $("weCancel").hidden = false;
      $("weProgress").hidden = false;
      setProgress(0, "正在下载播放器模板…");
      setStatus("全部在你的电脑上完成，脚本与图片不会上传。");

      var onCancel = function () {
        canceled = true;
        if (ctrl) { try { ctrl.abort(); } catch (e) {} }
        setProgress(0, "已中止。");
        setStatus("已取消导出。", "err");
      };
      $("weCancel").addEventListener("click", onCancel, { once: true });

      function finish() {
        btn.textContent = "导出为独立 EXE";
        $("weCancel").hidden = true;
        $("weCancel").removeEventListener("click", onCancel);
        scheduleScan();
      }

      W.exportExe({
        name: state.name,
        uiActions: exportActions(),
        images: state.images.map(function (im) { return { name: im.name, data: im.data }; }),
        screenW: state.screenW, screenH: state.screenH,
        loopWholeScript: $("weLoopWhole").checked,
        lowPerformanceMode: $("weLowPerf").checked,
        signal: ctrl ? ctrl.signal : null,
        onProgress: function (p) { renderProgress(p); },
      }).then(function (r) {
        setProgress(1, "已交给浏览器下载。");
        setStatus("已导出 " + r.filename + "（" + (r.bytes.length / 1024 / 1024).toFixed(2)
          + " MB，共 " + r.stats.actionCount + " 个动作）。双击即可运行，按 F9 停止。", "ok");
        setTimeout(function () { $("weProgress").hidden = true; finish(); }, 900);
      }).catch(function (e) {
        if (canceled || W.isAbort(e)) { finish(); return; }
        setStatus("导出失败：" + (e && e.message ? e.message : e), "err");
        $("weProgress").hidden = true;
        finish();
      });
    });
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else init();
})();
