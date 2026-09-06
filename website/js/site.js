/**
 * website/js/site.js — 官网交互：
 * 1. 移动端导航展开
 * 2. Demo iframe 按产品设计稿（home 1552x960 / editor 1800x1230 / opt 1640x1140）
 *    等比缩放，并根据 iframe 内 stub 推送的模式自动切换尺寸
 * 3. 页脚年份
 */
(function () {
  "use strict";

  var DESIGN = {
    home: { w: 1552, h: 960 },
    editor: { w: 1800, h: 1230 },
    opt: { w: 1640, h: 1140 },
  };

  /* ---------- 导航 ---------- */
  function setupNav() {
    var nav = document.querySelector(".site-nav");
    var toggle = document.querySelector(".nav-toggle");
    if (!nav || !toggle) return;
    toggle.addEventListener("click", function () {
      nav.classList.toggle("open");
    });
    document.querySelectorAll(".nav-links a").forEach(function (a) {
      a.addEventListener("click", function () {
        nav.classList.remove("open");
      });
    });
  }

  /* ---------- Demo iframe 缩放 ---------- */
  function setupDemoFrame() {
    var stage = document.querySelector("[data-demo-stage]");
    var frame = document.querySelector("[data-demo-frame]");
    if (!stage || !frame) return;

    var mode = "home";
    var frameReady = false;

    function apply() {
      var d = DESIGN[mode] || DESIGN.home;
      var avail = stage.clientWidth;
      if (!avail) return;
      var scale = Math.min(1, avail / d.w);
      frame.style.width = d.w + "px";
      frame.style.height = d.h + "px";
      frame.style.transform = "scale(" + scale + ")";
      frame.style.transformOrigin = "0 0";
      stage.style.height = Math.round(d.h * scale) + "px";
    }

    function onMessage(ev) {
      var data = ev.data;
      if (!data || data.source !== "qst-demo") return;
      if (data.type === "ready") {
        frameReady = true;
        if (data.mode && DESIGN[data.mode]) mode = data.mode;
        apply();
        return;
      }
      if (data.type === "mode" && DESIGN[data.mode]) {
        mode = data.mode;
        apply();
      }
    }

    window.addEventListener("message", onMessage);
    window.addEventListener("resize", apply);
    if (frameReady) apply();
    // 兜底：iframe 加载完成或 1.2s 后各刷一次
    frame.addEventListener("load", apply);
    setTimeout(apply, 1200);
    setTimeout(apply, 2600);
  }

  /* ---------- 页脚年份 ---------- */
  function setupYear() {
    document.querySelectorAll("[data-year]").forEach(function (el) {
      el.textContent = String(new Date().getFullYear());
    });
  }

  function init() {
    setupNav();
    setupDemoFrame();
    setupYear();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
