/**
 * website/demo/bridge.stub.js
 *
 * 官网 Demo 专用桥接桩：在浏览器（无 WebView2 / 无 qstBridge 原生宿主）环境下
 * 为 ui/bridge.js 提供 window.qstBridge，让整套产品前端可以离线跑起来。
 *
 * 约定：
 * - 不注入任何真实键鼠、录制、找图、OCR、窗口捕获能力（浏览器中也不允许）。
 * - 对强依赖原生的操作返回 { ok:false, detail: DEMO_HINT } 并弹出体验版提示。
 * - 对列表/设置/主题/定时任务返回预置假数据；开始/停止连点、运行/停止宏、
 *   录制开关用本地假状态机模拟，界面会“看起来是活的”。
 * - 通过 window.parent.postMessage 通知官网 demo.html 当前模式（home/editor），
 *   让外层按 1552x960 / 1800x1230 等比缩放产品窗口。
 */
(function () {
  "use strict";

  var DEMO_HINT = "网页体验版仅演示界面；完整能力请下载 Windows 客户端";
  var CLICKER_HINT = "演示模式：已模拟开始连点（网页版不注入真实键鼠）";
  var RECORD_HINT = "演示模式：已模拟开始录制（网页版不捕获真实键鼠）";

  var DESIGN = {
    home: { w: 1552, h: 960 },
    editor: { w: 1800, h: 1230 },
    opt: { w: 1640, h: 1140 },
  };

  var inIframe = (function () {
    try {
      return window.self !== window.top;
    } catch (_) {
      return false;
    }
  })();

  var agentShell =
    document.documentElement.classList.contains("agent-shell") ||
    /(?:^|\/)agent\.html(?:$|\?)/i.test(
      String(window.location.pathname || window.location.href || "")
    );

  /* ---------- 假数据 ---------- */
  // ⚠ 脚本的 `name` 是**不带 .json 的显示名**，`path` 才带扩展名。
  //   这是产品定的口径：后端列表入口用 `data.scriptName`（存盘时已 StripJsonExtension），
  //   兜底才用 `path.stem()`（同样没有扩展名）—— 见 webview_bridge_backend.cpp:3796。
  //   Demo 早期给 name 加了 .json，于是编辑器的「宏名称」显示成「表格录入.json」，
  //   比软件里多一个后缀（用户 2026-09-28 指出）。
  var macros = [
    {
      id: "demo-macro-1",
      path: "示例宏.json",
      name: "示例宏",
      folder: "",
      hotkey: "F9",
      actionCount: 11,
      meta: "11 动作 · 鼠标 / 按键 / 循环 / 条件",
    },
  ];

  var recordings = [
    {
      id: "demo-rec-1",
      path: "录制/登录流程录制.json",
      name: "登录流程录制",
      folder: "录制",
      hotkey: "",
      actionCount: 12,
      recordTime: "3秒",
      meta: "12 动作 · 3秒",
    },
  ];

  var chats = [
    {
      id: "demo-chat-1",
      name: "示例：帮我写一个自动签到脚本",
      folder: "",
      meta: "3 条消息 · 昨天",
    },
  ];

  var settings = {
    click: {
      enableRandomInterval: true,
      randomIntervalMaxSeconds: 0.5,
      enablePressReleaseInterval: true,
      pressReleaseIntervalSeconds: 0.02,
      enableCoordinateJitter: true,
      jitterX: 3,
      jitterY: 3,
      enableFixedCoordinates: false,
      fixedX: 0,
      fixedY: 0,
      enableClickCountLimit: false,
      clickCountLimit: 1000,
    },
    playback: {
      enablePlaybackCount: true,
      playbackCount: 1,
      enablePlaybackInterval: false,
      playbackIntervalMinSeconds: 0.5,
      playbackIntervalMaxSeconds: 1.5,
      enableDebugOutputWindow: false,
      autoOutputKeyFunctionDebug: true,
      recordingClickCaptureEnabled: true,
      recordingClickCaptureHalfSize: 32,
      enablePlaybackSpeed: false,
      playbackSpeed: 1,
      foregroundInputBackend: 0,
      scheduledTaskConflictPolicy: 0,
      scheduledTaskAutoResume: false,
    },
    other: {
      themeId: 7, // 官网 Demo 默认主题：极光 Arctic（产品 8 款主题中 id=7）
      useCustomTheme: 0,
      autoHideMainWindow: true,
      playSoundOnStart: true,
      playSoundOnEnd: true,
      hideBottomRightTip: false,
      closeToTray: true,
      showFloatBall: true,
      autoStartOnBoot: false,
      resolveImeConflict: true,
      editorDefaultView: "code",
      visualLoopWrap: true,
      visualBlockCallWires: true,
      visualIfWrap: true,
      visualBlockWrap: true,
      visualJumpWires: true,
      visualShowGrid: true,
      visualShowCardId: true,
      editorActionOrder: [],
      editorHiddenActions: [],
      holdThresholdSeconds: 0.12,
    },
    windowMode: {
      showPreviewThumbnail: true,
      previewRefreshMs: 500,
      blockRunWhenUnhealthy: true,
      allowForegroundInputFallback: false,
    },
    home: {
      clickerButton: 0,
      clickerIntervalMode: 1,
      clickerCustomInterval: 0.1,
      recorderInputMode: 0,
      activeTab: 0,
      clickerScrollOffset: 0,
      macroScrollOffset: 0,
      recorderScrollOffset: 0,
      scriptCustomScrollOffset: 0,
    },
    ai: {
      enabled: true,
      apiUrl: "https://api.openai.com/v1/chat/completions",
      apiKey: "sk-demo-****（网页版不展示真实密钥）",
      modelName: "gpt-4o-mini",
      temperature: 0.3,
      maxTokens: 4096,
      savedModels: [
        {
          apiUrl: "https://api.openai.com/v1/chat/completions",
          apiKey: "sk-demo",
          modelName: "gpt-4o-mini",
          temperature: 0.3,
          maxTokens: 4096,
        },
        {
          apiUrl: "https://api.deepseek.com/v1/chat/completions",
          apiKey: "sk-demo",
          modelName: "deepseek-chat",
          temperature: 0.3,
          maxTokens: 4096,
        },
      ],
    },
  };

  /* 与产品 app_theme.cpp kThemes[] 一致：7 个经典主题 + 极光 Arctic（id 0-7） */
  var themes = [
    { id: 0, name: "翠绿暖橙", main: "#40a863", accent: "#ff9a48", light: "#e8f8ef" },
    { id: 1, name: "晴空蓝", main: "#4e94d2", accent: "#eb9428", light: "#e6f0f8" },
    { id: 2, name: "活力橙", main: "#e69134", accent: "#14a8bc", light: "#fff4e4" },
    { id: 3, name: "珊瑚橙", main: "#e47658", accent: "#f0c448", light: "#faece8" },
    { id: 4, name: "梦幻紫", main: "#ab47bc", accent: "#ffb74d", light: "#f3e5f5" },
    { id: 5, name: "青柠薄荷", main: "#26a69a", accent: "#ff9800", light: "#e0f7fa" },
    { id: 6, name: "樱花粉", main: "#ec407a", accent: "#ffc107", light: "#fce4ec" },
    { id: 7, name: "极光 Arctic", main: "#1aa6d6", accent: "#2dd4bf", light: "#e8f4fc" },
  ];

  var tasks = [
    {
      id: "st-1",
      name: "每天早上跑一次示例宏",
      kind: 1,
      filePath: "示例宏.json",
      fileDisplayName: "示例宏",
      frequency: 1,
      timeLabel: "08:00:00",
      status: 0,
      hour: 8,
      minute: 0,
      second: 0,
      weekDays: 31,
    },
    {
      id: "st-2",
      name: "每周跑一次示例录制",
      kind: 0,
      filePath: "录制/登录流程录制.json",
      fileDisplayName: "登录流程录制",
      frequency: 2,
      timeLabel: "09:30:00",
      status: 1,
      hour: 9,
      minute: 30,
      second: 0,
      weekDays: 1,
    },
  ];

  var fakeState = {
    mode: "home",
    clicking: false,
    recording: false,
    running: false,
    runningName: "",
    executedSteps: 0,
    hotkey: { text: "F8", vk: 119, modifiers: 0, hold: false },
    clickBtn: 0,
    intervalMode: 1,
    customInterval: 0.1,
    intervalLabel: "高效模式 · 100 ms",
    newMacroSeq: 1,
    agentSeq: 1,
  };

  var agentOpenSent = false;

  /* ---------- 工具函数 ---------- */
  function emit(detail, delay) {
    var ms = delay == null ? 25 : delay;
    setTimeout(function () {
      try {
        window.dispatchEvent(new CustomEvent("qst:bridge", { detail: detail }));
      } catch (_) {}
    }, ms);
  }

  function ok(type, extra, delay) {
    var msg = { type: type + ".result", ok: true };
    if (extra) {
      for (var k in extra) {
        if (Object.prototype.hasOwnProperty.call(extra, k)) msg[k] = extra[k];
      }
    }
    emit(msg, delay);
  }

  function fail(type, detail) {
    emit({ type: type + ".result", ok: false, detail: detail || DEMO_HINT });
  }

  function toast(text) {
    emit({ type: "engine.toast", text: text }, 10);
  }

  /**
   * 网页 Demo 里"用户的屏幕分辨率"。
   * `screen.width` 是 CSS 像素、随页面缩放变化，乘 devicePixelRatio 才是物理像素 ——
   * 与产品用 `GetSystemMetrics(SM_CXVIRTUALSCREEN)` 拿到的量纲一致。
   * （换算关系的说明见 website/export/qst-web-export.js 文件头"坐标"那一条。）
   */
  function demoScreenSize() {
    var dpr = window.devicePixelRatio || 1;
    var w = Math.round((window.screen && window.screen.width ? window.screen.width : 1920) * dpr);
    var h = Math.round((window.screen && window.screen.height ? window.screen.height : 1080) * dpr);
    if (!(w > 0)) w = 1920;
    if (!(h > 0)) h = 1080;
    return { w: w, h: h };
  }

  function notifyParent(msg) {
    if (!inIframe || !window.parent) return;
    try {
      var payload = { source: "qst-demo" };
      for (var k in msg) {
        if (Object.prototype.hasOwnProperty.call(msg, k)) payload[k] = msg[k];
      }
      window.parent.postMessage(payload, "*");
    } catch (_) {}
  }

  function sendClientSize(mode) {
    var d = DESIGN[mode] || DESIGN.home;
    emit({ type: "window.clientSize", clientW: d.w, clientH: d.h, dpi: 96 });
  }

  function setMode(mode) {
    var m = DESIGN[mode] ? mode : "home";
    fakeState.mode = m;
    var d = DESIGN[m];
    notifyParent({ type: "mode", mode: m, w: d.w, h: d.h });
    sendClientSize(m);
    ok("window.setMode", { mode: m, clientW: d.w, clientH: d.h });
  }

  function engineStatus() {
    return {
      ok: true,
      clicking: fakeState.clicking ? 1 : 0,
      running: fakeState.running ? 1 : 0,
      recording: fakeState.recording ? 1 : 0,
      breakoutPaused: 0,
      debugging: 0,
      debugPaused: 0,
      runningMode: 0,
      currentScript: fakeState.running ? fakeState.runningName : "",
      executedSteps: fakeState.running ? fakeState.executedSteps : 0,
    };
  }

  function clickerStatus() {
    return {
      running: fakeState.clicking,
      button: fakeState.clickBtn,
      intervalMode: fakeState.intervalMode,
      customInterval: fakeState.customInterval,
      intervalLabel: fakeState.intervalLabel,
    };
  }

  function findMacro(idOrPath) {
    var key = String(idOrPath || "");
    return macros.find(function (m) {
      return String(m.id) === key || String(m.path) === key;
    });
  }

  /* ---------- 示例宏动作（编辑器假数据） ---------- */
  /**
   * 官网 Demo 的**唯一**示例脚本。
   *
   * 设计意图（2026-09-28 按用户要求重写）：
   *   旧版 13 条动作、场景是"登录/弹窗/输入 done"，用户看完不知道这软件到底怎么用；
   *   而且示例太多太杂，没人有耐心看。
   *   现在只留一条**能一眼读完**的脚本，把该露的能力各露一次：
   *     基本动作（移动鼠标 / 鼠标点击 / 等待 / 按键点击）
   *     + 循环（带循环变量 i）
   *     + 如果 / 否则
   *     + 结束宏运行
   *   `loopVarName: "i"` 与后面的 `{i} >= 2` 是**成对**的 —— 顺手把"循环变量能拿到条件里用"
   *   这件事演示清楚（不是随手写个不存在的变量）。
   */
  function sampleActions() {
    return [
      {
        type: "moveMouse",
        name: "移动鼠标到",
        remark: "把鼠标移到屏幕中间",
        x: 960,
        y: 540,
        randomX: 0,
        randomY: 0,
        duration: 0,
        indent: 0,
      },
      {
        type: "mouseClick",
        name: "鼠标点击",
        remark: "左键点一下",
        button: "left",
        clickCount: 1,
        duration: 0.01,
        indent: 0,
      },
      {
        type: "wait",
        name: "等待",
        remark: "等界面响应",
        duration: 1,
        indent: 0,
      },
      {
        type: "loop",
        name: "循环",
        remark: "下面两步重复 3 次，循环次数存进变量 i",
        loopCount: 3,
        loopVarName: "i",
        indent: 0,
      },
      {
        type: "keyClick",
        name: "按键点击",
        remark: "按一下 A",
        keyText: "A",
        keyVk: 65,
        clickCount: 1,
        duration: 0.01,
        indent: 1,
      },
      {
        type: "wait",
        name: "等待",
        remark: "间隔 0.5 秒",
        duration: 0.5,
        indent: 1,
      },
      {
        type: "if",
        name: "条件-如果",
        remark: "如果已经循环了 2 次以上",
        conditionExpr: "{i} >= 2",
        indent: 0,
      },
      {
        type: "keyClick",
        name: "按键点击",
        remark: "回车确认",
        keyText: "Enter",
        keyVk: 13,
        clickCount: 1,
        duration: 0.01,
        indent: 1,
      },
      {
        type: "else",
        name: "条件-否则",
        remark: "还没到次数就取消",
        indent: 0,
      },
      {
        type: "keyClick",
        name: "按键点击",
        remark: "按 Esc 取消",
        keyText: "Esc",
        keyVk: 27,
        clickCount: 1,
        duration: 0.01,
        indent: 1,
      },
      {
        type: "stopMacro",
        name: "结束宏运行",
        remark: "整个脚本到此结束",
        indent: 0,
      },
    ];
  }

  /* 录制优化界面的示例录制数据（fillOptUi 期望 actions/path/name/durationSeconds） */
  function sampleOptRecording(path) {
    var name = String(path || "登录流程录制.json").split(/[/\\]/).pop();
    return {
      path: path,
      name: name.replace(/\.json$/i, ""),
      durationSeconds: 3.42,
      actions: [
        { type: "moveMouse", name: "移动鼠标到", remark: "移动到「登录」按钮", x: 640, y: 480, duration: 0.2, indent: 0 },
        { type: "mouseClick", name: "鼠标点击", remark: "左键点击登录", button: "left", duration: 0.01, indent: 0 },
        { type: "wait", name: "等待", remark: "等待页面加载", duration: 1.2, indent: 0 },
        { type: "moveMouse", name: "移动鼠标到", remark: "移动至输入框", x: 520, y: 360, duration: 0.15, indent: 0 },
        { type: "mouseClick", name: "鼠标点击", remark: "聚焦用户名输入框", button: "left", duration: 0.01, indent: 0 },
        { type: "keyClick", name: "按键点击", remark: "输入账号", keyText: "A", keyVk: 65, duration: 0.01, indent: 0 },
        { type: "scrollWheel", name: "滚动滚轮", remark: "向下滚动列表", scrollSteps: 3, scrollVertical: 1, scrollHorizontal: 0, scrollDirection: 1, duration: 0.2, indent: 0 },
        { type: "wait", name: "等待", remark: "等待列表刷新", duration: 0.8, indent: 0 },
        { type: "moveMouse", name: "移动鼠标到", remark: "移动到「确认」按钮", x: 700, y: 520, duration: 0.2, indent: 0 },
        { type: "mouseClick", name: "鼠标点击", remark: "确认提交", button: "left", duration: 0.01, indent: 0 },
        { type: "wait", name: "等待", remark: "等待提交完成", duration: 0.5, indent: 0 },
        { type: "keyClick", name: "按键点击", remark: "回车关闭弹窗", keyText: "Enter", keyVk: 13, duration: 0.01, indent: 0 },
      ],
    };
  }

  function scriptForPath(path) {
    var p = String(path || "");
    var rec = recordings.find(function (r) {
      return String(r.path) === p || String(r.id) === p;
    });
    if (rec) {
      return {
        path: rec.path,
        name: rec.name,
        mode: rec.mode || 0,
        windowMode: rec.windowMode || {
          enabled: 0,
          executionKind: "hiddenDesktop",
          selectMethod: "selectOnStartup",
          targetExePath: "",
          fakeFocusEnabled: 0,
        },
        breakoutTimeSeconds: rec.breakoutTimeSeconds != null ? rec.breakoutTimeSeconds : 1.5,
        actions: rec.actions || sampleOptRecording(rec.path).actions,
      };
    }
    var m = findMacro(path);
    if (m) {
      // 示例脚本一律「默认模式」（前台、屏幕绝对坐标）—— 与 sampleActions() 的意图一致。
      // 旧版按脚本名硬编码 循环刷图→独立桌面 / 表格录入→后台窗口，
      // 那套需要三条不同场景的假脚本；现在只留一条最简示例，这里也就不再分叉。
      var mode = m.mode != null ? m.mode : 0;
      var wm = m.windowMode || {
        enabled: 0,
        executionKind: "hiddenDesktop",
        selectMethod: "selectOnStartup",
        targetExePath: "",
        fakeFocusEnabled: 0,
      };
      return {
        path: m.path,
        name: m.name,
        mode: mode,
        windowMode: wm,
        breakoutTimeSeconds:
          m.breakoutTimeSeconds != null ? m.breakoutTimeSeconds : mode === 0 ? 2 : 0,
        actions: m.actions || sampleActions(),
      };
    }
    return {
      path: "新建宏/新建宏.json",
      name: "新建宏",
      mode: 0,
      windowMode: {
        enabled: 0,
        executionKind: "hiddenDesktop",
        selectMethod: "selectOnStartup",
        targetExePath: "",
        fakeFocusEnabled: 0,
      },
      breakoutTimeSeconds: 0,
      actions: [],
    };
  }

  function upsertMacro(payload) {
    var p = String(payload.path || "");
    // ⚠ name 是**不带 .json 的显示名**（产品口径见 macros 定义处的说明）。
    //   用户可能自己在输入框里敲了 .json，这里按产品后端一样 StripJsonExtension。
    var name = String(payload.name || "").replace(/\.json$/i, "");
    var existing = findMacro(p) || macros.find(function (m) { return m.name === name; });
    if (existing) {
      existing.name = name;
      existing.path = (existing.folder ? existing.folder + "/" : "") + name + ".json";
      existing.actionCount = Array.isArray(payload.actions) ? payload.actions.length : 0;
      existing.meta =
        existing.actionCount + " 动作 · " + new Date().toLocaleDateString("zh-CN");
      if (Array.isArray(payload.actions)) existing.actions = payload.actions;
      if (payload.mode != null) existing.mode = payload.mode;
      if (payload.windowMode) existing.windowMode = payload.windowMode;
      if (payload.breakoutTimeSeconds != null)
        existing.breakoutTimeSeconds = payload.breakoutTimeSeconds;
      return existing;
    }
    var folder = "";
    var idx = p.lastIndexOf("/");
    if (idx > 0) folder = p.slice(0, idx);
    var item = {
      id: "demo-macro-new-" + fakeState.newMacroSeq++,
      path: (folder ? folder + "/" : "") + name + ".json",
      name: name,
      folder: folder,
      hotkey: "",
      actionCount: Array.isArray(payload.actions) ? payload.actions.length : 0,
      meta:
        (Array.isArray(payload.actions) ? payload.actions.length : 0) +
        " 动作 · " +
        new Date().toLocaleDateString("zh-CN"),
      actions: Array.isArray(payload.actions) ? payload.actions : [],
      mode: payload.mode != null ? payload.mode : 0,
      windowMode: payload.windowMode || {
        enabled: 0,
        executionKind: "hiddenDesktop",
        selectMethod: "selectOnStartup",
        targetExePath: "",
        fakeFocusEnabled: 0,
      },
      breakoutTimeSeconds:
        payload.breakoutTimeSeconds != null ? payload.breakoutTimeSeconds : 0,
    };
    macros.unshift(item);
    return item;
  }

  /* ---------- 消息路由 ---------- */
  function route(msg) {
    var type = String(msg.type || "");

    switch (type) {
      case "shell.contentReady":
      case "agentWindow.contentReady":
        notifyParent({ type: "ready", mode: fakeState.mode, w: DESIGN[fakeState.mode].w, h: DESIGN[fakeState.mode].h });
        if (agentShell && type === "agentWindow.contentReady" && !agentOpenSent) {
          // 桌面壳在收到 contentReady 后会回推 agentWindow.open；
          // 网页版由桩代为模拟，让 AI 对话窗口自动打开一个示例会话。
          agentOpenSent = true;
          setTimeout(function () {
            emit({ type: "agentWindow.open", id: "", createNew: 1 });
          }, 250);
        }
        return;

      case "window.setMode":
        setMode(msg.mode || "home");
        return;

      case "window.modeReady":
      case "window.uncloak":
        ok("window." + (type === "window.modeReady" ? "modeReady" : "uncloak"));
        return;

      case "window.minimize":
        toast("网页体验版无最小化按钮；窗口管理请下载 Windows 客户端体验");
        ok("window.minimize", { ok: true });
        return;

      case "window.close":
        toast(DEMO_HINT);
        ok("window.close", { ok: true });
        return;

      case "window.drag":
        return; // 拖拽窗口是原生能力，浏览器中静默忽略

      case "listScripts":
        ok("listScripts", { scripts: macros });
        return;

      case "listRecordings":
        ok("listRecordings", { recordings: recordings });
        return;

      case "listAgentConversations":
        ok("listAgentConversations", { conversations: chats });
        return;

      case "getHomeState":
        ok("getHomeState", { activeTab: 0, selectedScriptPath: "", selectedRecordingPath: "" });
        return;

      case "themeCatalog":
        ok("themeCatalog", { themes: themes });
        return;

      case "getEngineStatus":
        emit(Object.assign({ type: "getEngineStatus.result", ok: true }, engineStatus()));
        return;

      case "getClickerStatus":
        ok("getClickerStatus", { status: clickerStatus() });
        return;

      case "getAppBranding":
        ok("getAppBranding", {
          branding: {
            name: "键鼠工坊",
            version: "（网页体验版）",
            website: "https://www.quickscripttool.cloud/",
            contact: "24353623@qq.com",
            qq: "2163074732",
          },
        });
        return;

      case "getGlobalHotkey":
        ok("getGlobalHotkey", {
          hotkeyText: fakeState.hotkey.text,
          hotkeyVk: fakeState.hotkey.vk,
          hotkeyHold: fakeState.hotkey.hold,
        });
        return;

      case "setGlobalHotkey":
        fakeState.hotkey.text = String(msg.hotkeyText || fakeState.hotkey.text);
        fakeState.hotkey.vk = msg.hotkeyVk | 0 || fakeState.hotkey.vk;
        fakeState.hotkey.hold = !!msg.hotkeyHold;
        ok("setGlobalHotkey", {
          hotkeyText: fakeState.hotkey.text,
          hotkeyVk: fakeState.hotkey.vk,
          hotkeyHold: fakeState.hotkey.hold,
        });
        return;

      case "openSettingsData":
        ok("openSettingsData", { settings: settings });
        return;

      case "saveSettings":
        if (msg.settings && typeof msg.settings === "object") {
          mergeSettings(msg.settings);
        }
        ok("saveSettings", { settings: settings });
        return;

      case "restoreSettingsDefaults":
        settings = defaultSettings();
        ok("restoreSettingsDefaults", { settings: settings });
        return;

      case "openEditor":
        ok("openEditor", { script: scriptForPath(msg.path) });
        return;

      // 专业模式右侧的「动作预览」。旧实现没这个 case ⇒ 调用发出去没有任何 .result 回来，
      // 预览区**永远空着**（界面不报错、也不显示，最难查的一类缺口）。
      case "previewScriptActions":
        {
          var pvScript = scriptForPath(msg.path);
          var pvActs = Array.isArray(pvScript.actions) ? pvScript.actions : [];
          var pvMax = Math.max(1, Number(msg.max) || 16);
          ok("previewScriptActions", {
            reqId: msg.reqId || "",
            actionCount: pvActs.length,
            names: pvActs.slice(0, pvMax).map(actionPreviewName),
          });
        }
        return;

      // 窗口 Agents（把客户端窗口登记成模型）：网页版给一份示例档案 + 窗口列表，
      // 让设置页那套「准星拾取 / 绑定」UI 能完整走一遍，只是数据是假的。
      case "windowAgentList":
        ok("windowAgentList", { payload: windowAgentPayload() });
        return;

      case "windowAgentBind":
        {
          var bindId = String(msg.client || "");
          var bound = !msg.unbind;
          if (bound && !bindId) {
            // 按进程名新建骨架档案（与产品一致：仍需要探针校准）
            bindId = "wa-demo-" + Date.now();
            waClients.push({
              id: bindId,
              label: String(msg.process || "新窗口应用"),
              processNames: [String(msg.process || "")],
              bound: true,
              calibrated: false,
            });
          } else if (bindId) {
            waClients = waClients.map(function (c) {
              if (String(c.id) === bindId) c.bound = bound;
              return c;
            });
          }
          setTimeout(function () {
            ok("windowAgentBind", { payload: { ok: true, bound: bound, client: bindId } });
          }, 120);
        }
        return;

      // 虚拟 HID 驱动状态：网页版永远"未安装"（浏览器里本来就没有内核驱动），
      // 让设置页显示成真实客户端「没装驱动」时的样子。
      case "queryVhidStatus":
        ok("queryVhidStatus", {
          ok: true, driverReady: false, driverNeedsUpdate: false,
          rebootPending: false, packagePresent: false, hvciEnabled: false,
          lastExitCode: 0, demo: true,
        });
        return;

      case "setRecorderMode":
        fakeState.recWindowMode = msg.mode != null ? msg.mode | 0 : fakeState.recWindowMode;
        ok("setRecorderMode", { mode: fakeState.recWindowMode });
        return;

      case "window.setHomeSize":
        // 网页版的"窗口"就是 iframe；按请求的尺寸回一次 clientSize，
        // 让外层的等比缩放跟着变（不然模式切换后窗口尺寸对不上）。
        emit({
          type: "window.clientSize",
          clientW: Number(msg.w) || DESIGN.home.w,
          clientH: Number(msg.h) || DESIGN.home.h,
          dpi: 96,
        });
        ok("window.setHomeSize", {});
        return;

      // ── 浏览器里**做不到**的：明确说清楚，别让用户以为是坏了 ──────
      case "pickScreenDrag":
        fail(type, "拖动框选屏幕区域需要原生截图能力；网页体验版请下载 Windows 客户端。");
        return;

      case "pickTemplateDrag":
        fail(type, "拖动框选模板图需要原生截图能力；网页体验版请下载 Windows 客户端。");
        return;

      case "systemReboot":
        // 绝不真的重启用户的电脑 —— 这条无论如何都不能"演示"。
        fail(type, "网页体验版不会重启你的电脑；需要重启请自行操作。");
        return;

      case "peekScriptActions":
        {
          var peek = scriptForPath(msg.path);
          ok("peekScriptActions", {
            reqId: msg.reqId || "",
            actions: peek.actions || [],
            mode: peek.mode || 0,
            breakoutTimeSeconds: peek.breakoutTimeSeconds || 0,
            windowMode: peek.windowMode || {},
          });
        }
        return;

      case "editorVarItems":
        {
          // 网页体验版**近似**回一份变量清单。
          //
          // ⚠ 客户端里这条命令由引擎算（C++ `QuickInputVarItemsJson`，与「构建/运行宏」
          //   同一份实现）—— 网页版没有引擎，这里只做演示，**不是**第二份事实来源：
          //   产品路径永远走 C++，这里漏了什么也只会影响 Demo 的下拉显示。
          var demoItems = [];
          var demoSeen = {};
          var pushVar = function (code, tip) {
            if (!code || demoSeen[code]) return;
            demoSeen[code] = 1;
            demoItems.push({ code: code, insert: "{" + code + "}", tip: tip });
          };
          var demoActs = Array.isArray(msg.actions) ? msg.actions : [];
          demoActs.forEach(function (a) {
            if (!a || a._preview) return;
            var t = a.type || "";
            var n = String(a.matchVarName || "").trim();
            if (t === "findImage" && n) {
              if ((a.findImageFollowUp | 0) === 3) {
                pushVar(n, "图片变量");
              } else {
                [".matchData", ".x", ".y", ".x1", ".y1", ".cx", ".cy"].forEach(function (p) {
                  pushVar(n + p, "找图" + p);
                });
              }
            } else if (t === "multiMatch" && n) {
              pushVar(n + ".count", "多图匹配命中个数");
              pushVar(n + "[n]", "多图匹配第 n 处");
              [".matchData", ".x", ".y", ".x1", ".y1", ".cx", ".cy", ".hit", ".hitName"].forEach(
                function (p) {
                  pushVar(n + "[0]" + p, "第一处" + p);
                }
              );
            } else if (t === "textRecognition" && n) {
              if ((a.ocrResultMode | 0) === 1) {
                pushVar(n, "文字查找是否找到(0/1)");
                pushVar(n + ".matchData", "文字查找匹配度(0~100)");
                [".x", ".y", ".x1", ".y1"].forEach(function (p) {
                  pushVar(n + p, "命中文字框" + p);
                });
              } else {
                pushVar(n, "文字识别结果(字符串)");
              }
            } else if (t === "getCursorPos" && n) {
              pushVar(n + ".x", "光标横坐标");
              pushVar(n + ".y", "光标纵坐标");
            } else if ((t === "getColor" || t === "findColor" || t === "colorMatch") && n) {
              pushVar(n, "颜色(#RRGGBB)");
              pushVar(n + ".matchData", "匹配度(0~100)");
              pushVar(n + ".x", "颜色位置X");
              pushVar(n + ".y", "颜色位置Y");
            } else if (t === "loop" && a.loopVarName) {
              pushVar(String(a.loopVarName).trim(), "循环变量");
            } else if (t === "timerRecordTime" && (a.loopVarName || a.matchVarName)) {
              pushVar(String(a.loopVarName || a.matchVarName).trim(), "计时器变量");
            } else if (
              (t === "aiTextAnalysis" || t === "aiImageAnalysis" || t === "aiActionExecute") &&
              a.aiOutputVarName
            ) {
              pushVar(String(a.aiOutputVarName).trim(), "AI输出变量");
            }
          });
          ok("editorVarItems", { reqId: msg.reqId || "", items: demoItems });
        }
        return;

      case "saveEditor":
        {
          var saved = upsertMacro(msg);
          ok("saveEditor", { scripts: macros, path: saved.path });
        }
        return;

      case "deleteScript":
        {
          var dp = String(msg.path || "");
          var before = macros.length;
          macros = macros.filter(function (m) {
            return String(m.path) !== dp && String(m.id) !== dp;
          });
          ok("deleteScript", { scripts: macros, changed: macros.length !== before });
        }
        return;

      case "renameScript":
        {
          var rp = String(msg.path || "");
          // 与产品 RenameScriptFile 同口径：传进来的是**裸名**，用户可能自己带 .json；
          // 存进 name 的是裸名，path 才补 .json（webview_bridge_backend.cpp:1776-1786）。
          var rn = String(msg.name || "").replace(/\.json$/i, "").replace(/[<>:"/\\|?*]/g, "_");
          if (!rn) { fail("renameScript", "名称不能为空"); return; }
          macros = macros.map(function (m) {
            if (String(m.path) === rp || String(m.id) === rp) {
              m.name = rn;
              m.path = (m.folder ? m.folder + "/" : "") + rn + ".json";
            }
            return m;
          });
          var renamed = macros.find(function (m) {
            return String(m.name) === rn;
          });
          ok("renameScript", { scripts: macros, path: renamed ? renamed.path : rp });
        }
        return;

      case "setScriptHotkey":
        {
          var hp = String(msg.path || "");
          var hk = String(msg.hotkeyText || "");
          macros = macros.map(function (m) {
            if (String(m.path) === hp || String(m.id) === hp) m.hotkey = hk;
            return m;
          });
          ok("setScriptHotkey", { scripts: macros, path: hp });
        }
        return;

      case "importScript":
        fail(type, DEMO_HINT);
        return;

      // ── 导出为独立 EXE：网页版是**真做**的 ───────────────────────
      // 原理与产品的 `--export-exe` 完全同一套格式：
      //     [QstPlayer.exe][脚本包 zip][32 字节尾标]
      // 差别只在"谁来拼"：产品在本地拼，网页在**浏览器里**拼 ——
      // 服务器只发一个静态的播放器模板，脚本与图片一个字节都不上传。
      // 实现见 website/export/qst-web-export.js（与「在线脚本工坊」共用同一份代码）。
      case "scanScriptForExport":
        {
          var sx = window.QstWebExport;
          if (!sx) { fail(type, "网页导出模块没加载（export/qst-web-export.js）"); return; }
          var sScript = scriptForPath(msg.path);
          var sScreen = demoScreenSize();
          var sConv = sx.convertEditorActions(sScript.actions || [], {
            screenW: sScreen.w, screenH: sScreen.h,
          });
          var sScan = sx.scanCapabilities(sConv.actions, []);
          ok("scanScriptForExport", {
            scriptName: sScript.name || "",
            totalActions: sScan.totalActions,
            imageActions: sScan.imageActions,
            ocrActions: sScan.ocrActions,
            aiActions: sScan.aiActions,
            externalActions: sScan.externalActions,
            windowMode: 0,
            hasHotkey: 0,
            nestedScripts: 0,
            selfContainedOnly: sScan.selfContainedOnly ? 1 : 0,
            templateAvailable: 1,
            templateBytes: 5768192,          // 网页模板实际大小，导出前会按 fetch 到的字节复核
            openCvBytes: 0,
            fakeFocusBytes: 0,
            missingRefs: [],
            // 网页版特有：白名单之外的动作不是"没找到"，而是"这个版本不导出"
            rejectedActions: sConv.rejected,
            rejectedCount: sConv.rejected.length,
          });
        }
        return;

      case "exportScriptAsExe":
        {
          var ex = window.QstWebExport;
          if (!ex) { fail(type, "网页导出模块没加载（export/qst-web-export.js）"); return; }
          // file:// 下浏览器禁止 fetch 本地文件 ⇒ 导出必定失败。
          // 早点给出**能照着做**的话，别让用户对着 `Failed to fetch` 猜。
          if (ex.isFileProtocol && ex.isFileProtocol()) {
            fail("exportScriptAsExe",
              "这个页面是直接双击打开的（file://），浏览器不允许读取播放器模板。"
              + "请在仓库根目录运行 tools\\serve_website.ps1，用 http://127.0.0.1:8090/demo.html 打开后再试。");
            return;
          }
          var eScript = scriptForPath(msg.path);
          var eScreen = demoScreenSize();
          // ── 进度 + 可中止 ───────────────────────────────────────
          // 取模板要下 5.5 MB，慢网络上好几秒；旧实现只弹一句 toast 然后干等，
          // 用户既看不到进展也没法取消（用户 2026-09-28 反馈"不如客户端迅速"）。
          // 组装本身是毫秒级 —— 慢的**只有**这一段下载，所以进度条要盯的就是它。
          var expAbort = (typeof AbortController === "function") ? new AbortController() : null;
          exportAbortController = expAbort;
          showExportProgress(true);
          setExportProgress(0, "正在下载播放器模板…");
          ex.buildExeFromEditor({
            name: (eScript.name || "在线脚本").replace(/\.json$/i, ""),
            editorActions: eScript.actions || [],
            screenW: eScreen.w, screenH: eScreen.h,
            // ★ 设置继承：把用户当前在「设置」里改的那一份原样打进 exe 的 rt\app_settings.json，
            //   与客户端导出 `SerializeAppSettings(...)` 的行为一致。
            settings: settings,
            signal: expAbort ? expAbort.signal : null,
            onProgress: function (p) { onExportProgress(p); },
          }).then(function (r) {
            setExportProgress(1, "正在交给浏览器下载…");
            ex.downloadBytes(r.bytes, r.filename,
              "application/vnd.microsoft.portable-executable");
            ok("exportScriptAsExe", {
              bytes: r.stats.exeBytes,
              payloadBytes: r.stats.payloadBytes,
              skipped: 0,
              usedOpenCv: 0,
              usedFakeFocus: 0,
              settingsEmbedded: 1,
              aiKeyEmbedded: 0,
              needsAi: 0,
              missingRefs: [],
              rejected: r.rejected,
            });
            setTimeout(function () { showExportProgress(false); }, 600);
            if (r.rejected && r.rejected.length) {
              setTimeout(function () {
                toast("已导出；有 " + r.rejected.length + " 个动作网页版不支持，已跳过："
                  + r.rejected.map(function (x) { return x.type; }).join("、"));
              }, 400);
            }
          }).catch(function (e) {
            setTimeout(function () { showExportProgress(false); }, 400);
            if (ex.isAbort && ex.isAbort(e)) {
              // 用户自己点的取消 —— 不是错误，别用红色语气报
              fail("exportScriptAsExe", "cancelled");
              setTimeout(function () { toast("已取消导出"); }, 60);
              return;
            }
            fail("exportScriptAsExe", (e && e.message) ? e.message : "网页版导出失败");
          });
        }
        return;

      case "exportScript":
        // 产品里这个是「导出 zip 脚本包」（设置里勾了"导出默认为 zip"才走）。
        // 网页版不提供 zip 包，直接引导到我们真正做得成的 EXE 导出。
        fail(type, "网页版不导出 zip 脚本包；请用「导出为独立 EXE」，或下载 Windows 客户端。");
        return;

      case "openRecordingOptimize":
        {
          var optPath = String(msg.path || "录制/登录流程录制.json");
          ok("openRecordingOptimize", { useHtml: true, path: optPath });
          setTimeout(function () {
            toast("演示模式：优化界面使用示例录制数据");
          }, 900);
        }
        return;

      case "loadOptimizeRecording":
        ok("loadOptimizeRecording", {
          recording: sampleOptRecording(String(msg.path || "录制/登录流程录制.json")),
        });
        return;

      case "applyOptimizeRecording":
        ok("applyOptimizeRecording", {
          converted: 2,
          skipped: 1,
          recordings: recordings,
        });
        return;

      case "startClicker":
        fakeState.clicking = true;
        fakeState.clickBtn = msg.button != null ? msg.button | 0 : fakeState.clickBtn;
        fakeState.intervalMode = msg.intervalMode != null ? msg.intervalMode | 0 : fakeState.intervalMode;
        fakeState.customInterval = msg.customInterval != null ? Number(msg.customInterval) : fakeState.customInterval;
        fakeState.intervalLabel =
          fakeState.intervalMode === 2
            ? "极限模式 · 10 ms"
            : fakeState.intervalMode === 0
              ? "自定义 · " + (fakeState.customInterval * 1000).toFixed(0) + " ms"
              : "高效模式 · 100 ms";
        ok("startClicker", { status: clickerStatus() });
        toast(CLICKER_HINT);
        return;

      case "stopClicker":
        fakeState.clicking = false;
        ok("stopClicker", { status: clickerStatus() });
        toast("演示模式：已模拟停止连点");
        return;

      case "runScript":
        {
          var target = findMacro(msg.id);
          fakeState.running = true;
          fakeState.runningName = target ? String(target.name).replace(/\.json$/i, "") : "示例宏";
          fakeState.executedSteps = 0;
          ok("runScript", {});
          setTimeout(function () {
            fakeState.executedSteps = 3;
            emit(Object.assign({ type: "getEngineStatus.result", ok: true }, engineStatus()));
          }, 500);
          setTimeout(function () {
            toast("演示模式：已模拟开始运行「" + fakeState.runningName + "」（不注入真实键鼠）");
          }, 800);
        }
        return;

      case "stopScript":
        fakeState.running = false;
        fakeState.executedSteps = 0;
        ok("stopScript", {});
        setTimeout(function () {
          emit(Object.assign({ type: "getEngineStatus.result", ok: true }, engineStatus()));
        }, 60);
        setTimeout(function () {
          toast("演示模式：已模拟停止运行");
        }, 500);
        return;

      case "debugScript":
        fail(type, DEMO_HINT);
        return;

      case "startRecord":
        fakeState.recording = true;
        ok("startRecord", {});
        setTimeout(function () {
          toast(RECORD_HINT);
        }, 700);
        return;

      case "stopRecord":
        fakeState.recording = false;
        {
          var newRec = {
            id: "demo-rec-new-" + Date.now(),
            path: "录制/网页体验录制.json",
            name: "网页体验录制",
            folder: "录制",
            hotkey: "",
            actionCount: 42,
            recordTime: "36秒",
            meta: "42 动作 · 36秒",
          };
          recordings.unshift(newRec);
          ok("stopRecord", { path: newRec.path, actionCount: 42, recordings: recordings });
          setTimeout(function () {
            toast("演示模式：已生成一条示例录制（网页版未捕获真实键鼠）");
          }, 1000);
        }
        return;

      case "listScheduledTasks":
        ok("listScheduledTasks", { data: { tasks: tasks, globalDisabled: false } });
        return;

      case "saveScheduledTask":
        {
          if (msg.update && msg.id) {
            tasks = tasks.map(function (t) {
              return String(t.id) === String(msg.id) ? applyTaskPayload(t, msg) : t;
            });
          } else {
            var nt = applyTaskPayload(
              { id: "st-new-" + Date.now(), name: "新建任务", kind: 1, frequency: 1, timeLabel: "08:00:00", status: 0, weekDays: 31 },
              msg
            );
            tasks.unshift(nt);
          }
          ok("saveScheduledTask", { data: { tasks: tasks, globalDisabled: false } });
        }
        return;

      case "deleteScheduledTask":
        tasks = tasks.filter(function (t) {
          return String(t.id) !== String(msg.id || "");
        });
        ok("deleteScheduledTask", { data: { tasks: tasks, globalDisabled: false } });
        return;

      case "setScheduledTasksGlobalDisabled":
        ok("setScheduledTasksGlobalDisabled", {
          data: { tasks: tasks, globalDisabled: !!msg.disabled },
        });
        return;

      case "openScheduledTasks":
        ok("openScheduledTasks", {});
        return;

      case "beginHotkeyCapture":
        ok("beginHotkeyCapture", { holdThresholdSeconds: 0.2 });
        return;

      case "endHotkeyCapture":
        ok("endHotkeyCapture", {});
        return;

      case "setTypingHotkeysMuted":
        ok("setTypingHotkeysMuted", { muted: msg.muted ? 1 : 0 });
        return;

      case "captureActionKey":
      case "captureGlobalHotkey":
      case "captureScriptHotkey":
        toast("按键捕获需要真实全局钩子；网页体验版仅演示界面");
        ok(type, {});
        return;

      case "formatHotkey":
        ok("formatHotkey", {
          hotkeyText: String(msg.hotkeyText || (msg.hotkeyVk ? "F8" : "")),
          hotkeyHold: msg.hotkeyHold ? 1 : 0,
        });
        return;

      case "pickScreenRegion":
      case "crosshairPick":
      case "findImageCrop":
      case "findImageMatch":
      case "captureTemplateScreenshot":
      case "testOcr":
      case "pickImageFile":
      case "browsePath":
      case "saveImageAs":
      case "saveClipboardImage":
      case "pasteAgentClipboard":
      case "installDriver":
      case "installOcr":
      case "showDebugWindow":
      case "openThemeCustom":
      case "showWindowModePreview":
        fail(type, DEMO_HINT);
        return;

      case "hideWindowModePreview":
        ok("hideWindowModePreview", {});
        return;

      case "checkUpgrade":
        ok("checkUpgrade", { stub: true, detail: "网页体验版不检查更新" });
        return;

      case "readImageDataUrl":
        ok("readImageDataUrl", { dataUrl: "", reqId: String(msg.reqId || "") });
        return;

      case "resolveImagePath":
        ok("resolveImagePath", { path: String(msg.path || "") });
        return;

      case "applyTheme":
        applyThemeMsg(msg);
        ok("applyTheme", { settings: settings });
        return;

      case "debugWindowClosed":
      case "debugWindowSetTopmost":
        ok(type, {});
        return;

      case "setActiveHomeTab":
      case "setHomeSelection":
        ok(type, {});
        return;

      case "listLibraryFolders":
        ok("listLibraryFolders", {
          kind: String(msg.kind || "macro"),
          folders: libraryFoldersFor(String(msg.kind || "macro")),
        });
        return;

      // ── 目录管理：网页版**真做**（内存态）────────────────────────
      // 旧实现一律 fail(DEMO_HINT) ⇒ 专业模式里"新建目录/改名/删除/移动脚本"全是死的，
      // 用户感觉"网页版缺一半功能"。这些是纯数据操作，浏览器完全做得成。
      case "createLibraryFolder":
        {
          var ck = normKind(msg.kind);
          var cf = normFolderPath(msg.folder);
          if (!cf) { fail(type, "目录名不能为空"); return; }
          if (folderExists(ck, cf)) { fail(type, "目录已存在：" + cf); return; }
          (extraFolders[ck] || (extraFolders[ck] = [])).push(cf);
          ok(type, { kind: msg.kind, folders: libraryFoldersFor(ck) });
        }
        return;

      case "renameLibraryFolder":
        {
          var rk = normKind(msg.kind);
          var rfrom = normFolderPath(msg.folder);
          var rto = normFolderPath(msg.name);
          if (!rfrom || !rto) { fail(type, "目录名不能为空"); return; }
          if (!folderExists(rk, rfrom)) { fail(type, "目录不存在：" + rfrom); return; }
          if (folderExists(rk, rto)) { fail(type, "目标目录已存在：" + rto); return; }
          rewriteItemFolders(rk, rfrom, rto);
          // 子目录也要跟着改名（先长后短，避免父目录改名把子目录路径改花）
          var subs = libraryFoldersFor(rk)
            .filter(function (f) { return f.indexOf(rfrom + "/") === 0; })
            .sort(function (a, b) { return b.length - a.length; });
          subs.forEach(function (f) {
            (extraFolders[rk] || (extraFolders[rk] = [])).push(rto + f.slice(rfrom.length));
          });
          extraFolders[rk] = (extraFolders[rk] || [])
            .map(function (f) { return f === rfrom ? rto : (f.indexOf(rfrom + "/") === 0 ? rto + f.slice(rfrom.length) : f); })
            .filter(function (f, i, arr) { return arr.indexOf(f) === i; });
          ok(type, {
            kind: msg.kind, folders: libraryFoldersFor(rk),
            scripts: macros, recordings: recordings,
          });
        }
        return;

      case "deleteLibraryFolder":
        {
          var dk = normKind(msg.kind);
          var df = normFolderPath(msg.folder);
          if (!df) { fail(type, "目录名不能为空"); return; }
          // 目录里的条目**移到根**而不是删掉（与产品一致：删目录不该销毁脚本）
          var moved = rewriteItemFolders(dk, df, "");
          extraFolders[dk] = (extraFolders[dk] || []).filter(function (f) {
            return f !== df && f.indexOf(df + "/") !== 0;
          });
          ok(type, {
            kind: msg.kind, folders: libraryFoldersFor(dk), moved: moved,
            scripts: macros, recordings: recordings,
          });
          if (moved > 0) setTimeout(function () { toast("已删除目录，其中 " + moved + " 个条目已移到根目录"); }, 200);
        }
        return;

      case "moveScriptToFolder":
        {
          var mpath = String(msg.path || "");
          var mfolder = normFolderPath(msg.folder);
          var target = findMacro(mpath) ||
            recordings.find(function (r) { return String(r.path) === mpath || String(r.id) === mpath; });
          if (!target) { fail(type, "找不到条目：" + mpath); return; }
          target.folder = mfolder;
          if (mfolder && !folderExists(normKind(target.id && /^demo-rec/.test(target.id) ? "rec" : "macro"), mfolder)) {
            var mk = /^demo-rec/.test(String(target.id)) ? "rec" : "macro";
            (extraFolders[mk] || (extraFolders[mk] = [])).push(mfolder);
          }
          ok(type, { folders: libraryFoldersFor("macro"), scripts: macros, recordings: recordings });
        }
        return;

      case "setItemLibraryFolder":
        {
          var sk = normKind(msg.kind);
          var sid = String(msg.id || "");
          var sfolder = normFolderPath(msg.folder);
          var hit = itemsOfKind(sk).find(function (it) {
            return String(it.id) === sid || String(it.path) === sid;
          });
          if (!hit) { fail(type, "找不到条目：" + sid); return; }
          hit.folder = sfolder;
          if (sfolder && !folderExists(sk, sfolder)) {
            (extraFolders[sk] || (extraFolders[sk] = [])).push(sfolder);
          }
          ok(type, {
            kind: msg.kind, folders: libraryFoldersFor(sk),
            scripts: macros, recordings: recordings,
          });
        }
        return;

      case "openAgentWindow":
        if (agentShell) {
          ok("openAgentWindow", {});
          return;
        }
        {
          var win = null;
          try {
            win = window.open("agent.html?mode=agent", "qst-agent-demo");
          } catch (_) {}
          if (win) {
            toast("已打开 AI 对话演示窗口（网页版为假对话流，不会调用真实模型）");
          } else {
            toast("浏览器拦截了弹窗，请允许本站弹窗以体验 AI 对话演示");
          }
          ok("openAgentWindow", {});
        }
        return;

      case "agentWindow.minimize":
        toast("网页体验版无最小化功能；请在桌面客户端体验");
        ok("agentWindow.minimize", {});
        return;

      case "openAgentConversation":
        ok("openAgentConversation", {
          conversation: {
            id: "demo-agent-" + fakeState.agentSeq++,
            name: "示例对话",
            modelName: settings.ai.modelName || "gpt-4o-mini",
            messages: [
              {
                role: "assistant",
                content:
                  "你好，我是键鼠工坊的 AI 脚本助手（网页体验版）。\n\n你可以直接描述需求，例如：“帮我写一个每 3 秒点击一次（1000, 500）的循环脚本”。我会演示如何生成可编辑的宏动作列表。",
              },
            ],
            draft: "",
            attachments: [],
          },
          panelKey: String(msg.panelKey || ""),
        });
        return;

      case "sendAgentMessage":
        simulateAgentReply(msg);
        return;

      case "cancelAgentMessage":
        ok("cancelAgentMessage", {});
        return;

      case "agentSaveDraft":
        ok("agentSaveDraft", {});
        return;

      case "listAgentChanges":
        ok("listAgentChanges", { changes: [] });
        return;

      case "revertAgentChange":
        ok("revertAgentChange", {});
        return;

      case "deleteAgentConversation":
        {
          var cid = String(msg.id || "");
          chats = chats.filter(function (c) {
            return String(c.id) !== cid;
          });
          ok("deleteAgentConversation", { conversations: chats });
        }
        return;

      default:
        // 未知/未列出的消息：给一个安全回包，避免前端等待超时
        ok(type, { stub: true });
        return;
    }
  }

  function applyTaskPayload(task, msg) {
    task.name = String(msg.name || task.name);
    task.kind = msg.kind != null ? msg.kind | 0 : task.kind;
    task.frequency = msg.frequency != null ? msg.frequency | 0 : task.frequency;
    task.status = msg.status != null ? msg.status | 0 : task.status;
    task.filePath = String(msg.filePath || task.filePath);
    task.fileDisplayName = task.filePath.split(/[/\\]/).pop();
    var hh = msg.hour != null ? msg.hour | 0 : task.hour | 0;
    var mm = msg.minute != null ? msg.minute | 0 : task.minute | 0;
    var ss = msg.second != null ? msg.second | 0 : task.second | 0;
    task.hour = hh;
    task.minute = mm;
    task.second = ss;
    task.timeLabel =
      String(hh).padStart(2, "0") + ":" + String(mm).padStart(2, "0") + ":" + String(ss).padStart(2, "0");
    if (msg.weekDays != null) task.weekDays = msg.weekDays | 0;
    return task;
  }

  /**
   * 扁平设置键 → `[段, 字段]` 路由表。
   *
   * ⚠⚠ 这张表**必须与产品后端一一对应**（2026-09-28 用户反馈「导出还是没走设置」）：
   *   `ui/app.js` 的 `collectSettings()` 发出去的是**扁平**键（`playbackCount`、
   *   `enablePlaybackCount`、`lowPerformanceMode`…），而 `fillSettings()` 读的是**嵌套**
   *   （`s.playback.playbackCount`）—— 也就是后端的契约是「**收扁平、回嵌套**」。
   *   旧实现只认嵌套 + 一份手写的 `otherFlat` 白名单 ⇒ 播放次数/间隔/低性能模式…
   *   这些扁平键**全被静默丢掉**：设置在界面上改了，`settings` 对象没变，
   *   于是导出的 exe 用的是**旧值**（用户看到的就是"导出没走设置"）。
   *
   *   表是从 `src/webview/webview_bridge_backend.cpp` 的 SaveSettings 里**逐条抽出来的**
   *   （86 条 + `savedModelsClear` 单独处理），并由
   *   `tools/verify/demo_settings_parity.js` 反向校验 —— C++ 加了新设置而这里没跟，
   *   守卫会直接报红，不会静默漂移。**别手写白名单**，那个教训已经吃过一次。
   */
  var FLAT_FIELD_SECTION = {
    activeTab:                          ["home", "activeTab"],
    aiEnabled:                          ["ai", "enabled"],
    aiFastPaths:                        ["playback", "aiFastPaths"],
    allowForegroundInputFallback:       ["windowMode", "allowForegroundInputFallback"],
    autoHideMainWindow:                 ["other", "autoHideMainWindow"],
    autoOutputKeyFunctionDebug:         ["playback", "autoOutputKeyFunctionDebug"],
    autoStartOnBoot:                    ["other", "autoStartOnBoot"],
    blockRunWhenUnhealthy:              ["windowMode", "blockRunWhenUnhealthy"],
    clickCountLimit:                    ["click", "clickCountLimit"],
    clickerButton:                      ["home", "clickerButton"],
    clickerCustomInterval:              ["home", "clickerCustomInterval"],
    clickerIntervalMode:                ["home", "clickerIntervalMode"],
    clickerScrollOffset:                ["home", "clickerScrollOffset"],
    closeToTray:                        ["other", "closeToTray"],
    customAccentColor:                  ["other", "customAccentColor"],
    customMainColor:                    ["other", "customMainColor"],
    editorAutoSaveOnExit:               ["other", "editorAutoSaveOnExit"],
    editorDisableModifyButton:          ["other", "editorDisableModifyButton"],
    editorEnableBatchInsert:            ["other", "editorEnableBatchInsert"],
    editorHideCoordVars:                ["other", "editorHideCoordVars"],
    editorHideFixedVars:                ["other", "editorHideFixedVars"],
    editorMultiResultPlaceholderOnly:   ["other", "editorMultiResultPlaceholderOnly"],
    editorSearchAllActions:             ["other", "editorSearchAllActions"],
    enableClickCountLimit:              ["click", "enableClickCountLimit"],
    enableCoordinateJitter:             ["click", "enableCoordinateJitter"],
    enableDebugOutputWindow:            ["playback", "enableDebugOutputWindow"],
    enableFakeFocusInjection:           ["windowMode", "enableFakeFocusInjection"],
    enableFixedCoordinates:             ["click", "enableFixedCoordinates"],
    enableHidDriverSimulation:          ["playback", "enableHidDriverSimulation"],
    enablePlaybackCount:                ["playback", "enablePlaybackCount"],
    enablePlaybackInterval:             ["playback", "enablePlaybackInterval"],
    enablePlaybackSpeed:                ["playback", "enablePlaybackSpeed"],
    enablePressReleaseInterval:         ["click", "enablePressReleaseInterval"],
    enableRandomInterval:               ["click", "enableRandomInterval"],
    findImageGpuAccel:                  ["playback", "findImageGpuAccel"],
    fixedX:                             ["click", "fixedX"],
    fixedY:                             ["click", "fixedY"],
    floatBallDocked:                    ["other", "floatBallDocked"],
    floatBallEdge:                      ["other", "floatBallEdge"],
    floatBallXRatio:                    ["other", "floatBallXRatio"],
    floatBallYRatio:                    ["other", "floatBallYRatio"],
    foregroundInputBackend:             ["playback", "foregroundInputBackend"],
    hideBottomRightTip:                 ["other", "hideBottomRightTip"],
    hideInjectedModule:                 ["windowMode", "hideInjectedModule"],
    holdThresholdSeconds:               ["other", "holdThresholdSeconds"],
    injectionTechnique:                 ["windowMode", "injectionTechnique"],
    jitterX:                            ["click", "jitterX"],
    jitterY:                            ["click", "jitterY"],
    lowPerformanceMode:                 ["playback", "lowPerformanceMode"],
    macroScrollOffset:                  ["home", "macroScrollOffset"],
    maxTokens:                          ["ai", "maxTokens"],
    playSoundOnEnd:                     ["other", "playSoundOnEnd"],
    playSoundOnStart:                   ["other", "playSoundOnStart"],
    playbackCount:                      ["playback", "playbackCount"],
    playbackIntervalMaxSeconds:         ["playback", "playbackIntervalMaxSeconds"],
    playbackIntervalMinSeconds:         ["playback", "playbackIntervalMinSeconds"],
    playbackSpeed:                      ["playback", "playbackSpeed"],
    preferDirect2D:                     ["other", "preferDirect2D"],
    pressReleaseIntervalSeconds:        ["click", "pressReleaseIntervalSeconds"],
    previewRefreshMs:                   ["windowMode", "previewRefreshMs"],
    randomIntervalMaxSeconds:           ["click", "randomIntervalMaxSeconds"],
    recorderCaptureScope:               ["home", "recorderCaptureScope"],
    recorderInputMode:                  ["home", "recorderInputMode"],
    recorderScrollOffset:               ["home", "recorderScrollOffset"],
    recorderWindowMode:                 ["home", "recorderWindowMode"],
    recordingClickCaptureEnabled:       ["playback", "recordingClickCaptureEnabled"],
    recordingClickCaptureHalfSize:      ["playback", "recordingClickCaptureHalfSize"],
    resolveImeConflict:                 ["other", "resolveImeConflict"],
    scheduledTaskAutoResume:            ["playback", "scheduledTaskAutoResume"],
    scheduledTaskConflictPolicy:        ["playback", "scheduledTaskConflictPolicy"],
    scriptCustomScrollOffset:           ["home", "scriptCustomScrollOffset"],
    showFloatBall:                      ["other", "showFloatBall"],
    showPreviewThumbnail:               ["windowMode", "showPreviewThumbnail"],
    spreadRelativeMovePackets:          ["playback", "spreadRelativeMovePackets"],
    temperature:                        ["ai", "temperature"],
    themeId:                            ["other", "themeId"],
    uiScaleFactor:                      ["other", "uiScaleFactor"],
    useCustomTheme:                     ["other", "useCustomTheme"],
    visualBlockCallWires:               ["other", "visualBlockCallWires"],
    visualBlockWrap:                    ["other", "visualBlockWrap"],
    visualIfWrap:                       ["other", "visualIfWrap"],
    visualJumpWires:                    ["other", "visualJumpWires"],
    visualLoopWrap:                     ["other", "visualLoopWrap"],
    visualShowCardId:                   ["other", "visualShowCardId"],
    visualShowGrid:                     ["other", "visualShowGrid"],
    visualWatchWrap:                    ["other", "visualWatchWrap"],

    // ── 后端用**另外的路径**处理、不在上面那张 Get* 表里的扁平原键 ──────────
    // 这几条不能省：`collectSettings()` 一样会把它们发过来，桩不接住就会丢
    // （AI 配置一丢，设置页再打开时模型/密钥就"变回默认"了）。
    // tools/verify/demo_settings_parity.js 的 STUB_ONLY 记着它们为什么不在 C++ 表里。
    //
    // ⚠ 其中 enableWindowTimeScale / exportScriptAsZip 原本**连产品后端都漏了映射**
    //   （界面发了、没人接、保存后静默失效）—— 是这条判据把它们揪出来并已在
    //   `webview_bridge_backend.cpp` 的 SaveSettings 里补上的。所以它们现在**也在** C++ 表里。
    enableWindowTimeScale:              ["windowMode", "enableWindowTimeScale"],
    exportScriptAsZip:                  ["other", "exportScriptAsZip"],
    editorActionOrder:                  ["other", "editorActionOrder"],
    editorHiddenActions:                ["other", "editorHiddenActions"],
    editorCustomActionOrder:            ["other", "editorCustomActionOrder"],
    editorCustomHiddenActions:          ["other", "editorCustomHiddenActions"],
    editorCatalogPreset:                ["other", "editorCatalogPreset"],
    uiMode:                             ["home", "uiMode"],
    apiUrl:                             ["ai", "apiUrl"],
    apiKey:                             ["ai", "apiKey"],
    modelName:                          ["ai", "modelName"],
    savedModels:                        ["ai", "savedModels"],
  };

  function mergeSettings(next) {
    // ① 嵌套形态（openSettingsData / 主题弹窗那类调用会回传整段）
    ["click", "playback", "other", "windowMode", "home", "ai"].forEach(function (sec) {
      if (next[sec] && typeof next[sec] === "object") {
        if (!settings[sec]) settings[sec] = {};
        Object.keys(next[sec]).forEach(function (k) {
          settings[sec][k] = next[sec][k];
        });
      }
    });
    // ② 扁平形态（collectSettings 发过来的就是这一种）—— 按 C++ 抽出的表路由
    Object.keys(next).forEach(function (k) {
      var t = FLAT_FIELD_SECTION[k];
      if (!t) return;
      if (!settings[t[0]]) settings[t[0]] = {};
      settings[t[0]][t[1]] = next[k];
    });
    // ③ 语义特殊的两条：它们不是普通赋值
    if (next.savedModelsClear) settings.ai && (settings.ai.savedModels = []);
    if (Array.isArray(next.savedModels) && settings.ai) settings.ai.savedModels = next.savedModels;
    // ④ 编辑器设置的兜底字段（C++ 侧走的是另一条持久化路径，这里保底不丢）
    if (!settings.other) settings.other = {};
    ["editorDefaultView", "editorActionOrder", "editorHiddenActions", "editorCatalogPreset"]
      .forEach(function (k) {
        if (Object.prototype.hasOwnProperty.call(next, k)) settings.other[k] = next[k];
      });
  }

  function applyThemeMsg(msg) {
    if (!settings.other) settings.other = {};
    if (msg.useCustomTheme) {
      settings.other.useCustomTheme = 1;
      if (msg.customMainColor != null) settings.other.customMainColor = msg.customMainColor | 0;
      if (msg.customAccentColor != null) settings.other.customAccentColor = msg.customAccentColor | 0;
      // 主题弹窗确定按钮：themeId=0 + useCustomTheme=1，保留 themeId 以便取消自定义回退
      if (msg.themeId != null) settings.other.themeId = msg.themeId | 0;
    } else {
      settings.other.useCustomTheme = 0;
      if (msg.themeId != null) settings.other.themeId = msg.themeId | 0;
    }
  }

  /**
   * 专业模式的**目录树数据源**。
   *
   * ⚠⚠ 只能从**真实条目**推导，绝不能写死（2026-09-28 用户反馈「极简模式和专业模式的脚本库
   *   没有统一」就是这个）：
   *   旧实现返回写死的 `["示例宏","游戏辅助","办公自动化"]`，而 `macros` 里只有一条
   *   **无目录**的「示例宏」⇒ 专业模式显示 3 个空目录、脚本一条都不在里面；
   *   极简模式却正常列出 1 条脚本 —— 同一份数据两种视图对不上，用户看到的就是"库不一样"。
   *   这正是 AGENTS.md §43 那条：「静态清单 × 动态数据必须同一份事实」——
   *   目录列表要从 items 的 folder 字段**算出来**，用户新建的目录另存一份。
   */
  var extraFolders = { macro: [], rec: [], sched: [], ai: [] };

  /** 目录路径归一化（与 pro-mode.js 的 normFolder 同一把尺：反斜杠→斜杠、去首尾斜杠）。 */
  function normFolderPath(f) {
    return String(f == null ? "" : f).replace(/\\/g, "/").replace(/^\/+|\/+$/g, "");
  }

  /** 动作的显示名（预览列表用）。示例动作自带 name；没有就按类型给个中文名。 */
  function actionPreviewName(a) {
    if (!a) return "";
    if (a.name) return String(a.name);
    var t = String(a.type || "");
    var labels = {
      moveMouse: "移动鼠标到", mouseClick: "鼠标点击", mouseDown: "鼠标按下",
      mouseUp: "鼠标松开", scrollWheel: "滚动滚轮", keyClick: "按键点击",
      keyDown: "键盘按下", keyUp: "键盘松开", quickInput: "快捷输入",
      wait: "等待", loop: "循环", endLoop: "跳出循环", if: "条件-如果",
      else: "条件-否则", stopMacro: "结束宏运行", findImage: "找图点击",
      runMacro: "运行鼠标宏", mousePlayback: "运行录制回放",
    };
    return labels[t] || t || "动作";
  }

  /**
   * 窗口 Agents 的假档案 / 窗口列表（只为了让设置页那套 UI 能走通）。
   */
  var waClients = [
    {
      id: "wa-demo-doubao", label: "豆包客户端",
      processNames: ["Doubao.exe"], bound: true, calibrated: true,
    },
  ];
  function windowAgentPayload() {
    return {
      clients: waClients,
      windows: [
        { process: "Doubao.exe", title: "豆包" },
        { process: "Cursor.exe", title: "Cursor" },
        { process: "WindowsTerminal.exe", title: "终端" },
      ],
      demo: true,
    };
  }

  // ── 导出进度 UI ────────────────────────────────────────────────
  // 为什么由**桩**注入而不是改 ui\index.html：那是产品前端，动它就会跟着发版进客户端；
  // 而客户端导出是本机 IO（毫秒级），根本不需要进度条。这是**网页独有**的补充件。
  // 注入点选在 #expScan 之后、#expSize 之前 —— 不打断原有布局顺序。
  var exportAbortController = null;
  var lastExportPct = -1;

  function ensureExportProgressUI() {
    if (document.getElementById("qstExpProgress")) return true;
    var body = document.querySelector("#ov-export-exe .dlg-body");
    if (!body) return false;
    if (!document.getElementById("qstExpProgressCss")) {
      var st = document.createElement("style");
      st.id = "qstExpProgressCss";
      st.textContent =
        "#qstExpProgress{margin:10px 0 2px;display:none}"
        + "#qstExpProgress.on{display:block}"
        + "#qstExpProgress .bar{height:8px;border-radius:999px;background:rgba(26,166,214,.14);"
        + "border:1px solid var(--line,#c9d7e6);overflow:hidden}"
        + "#qstExpProgress .bar i{display:block;height:100%;width:0;border-radius:999px;"
        + "background:linear-gradient(90deg,var(--ice-400,#38bdf8),var(--mint-400,#2dd4bf));"
        + "transition:width .18s ease}"
        + "#qstExpProgress .txt{margin-top:6px;font-size:calc(12px * var(--qst-u,1.5));"
        + "color:var(--text-3,#7e95ab)}"
        + "#qstExpProgress .txt b{color:var(--text-2,#4a6178)}";
      document.head.appendChild(st);
    }
    var box = document.createElement("div");
    box.id = "qstExpProgress";
    box.innerHTML = '<div class="bar"><i></i></div><div class="txt"></div>';
    var anchor = document.getElementById("expSize");
    if (anchor && anchor.parentNode === body) body.insertBefore(box, anchor);
    else body.appendChild(box);
    return true;
  }

  function showExportProgress(on) {
    if (!ensureExportProgressUI()) return;
    var box = document.getElementById("qstExpProgress");
    if (box) box.className = on ? "on" : "";
    var okBtn = document.getElementById("btnExportExeOk");
    if (okBtn) {
      okBtn.disabled = !!on;
      okBtn.textContent = on ? "导出中…" : "导出";
    }
    // 「取消」按钮平时只是关弹窗（data-close）；导出中让它先**中止**再关。
    var cancelBtn = document.querySelector("#ov-export-exe .dlg-foot .btn.cancel");
    if (cancelBtn && !cancelBtn._qstAbortWired) {
      cancelBtn._qstAbortWired = true;
      cancelBtn.addEventListener("click", function () {
        if (exportAbortController && !exportAbortController.signal.aborted) {
          try { exportAbortController.abort(); } catch (e) {}
          toast("已中止导出");
        }
      }, true);   // 捕获阶段：先中止，再让原有 data-close 关弹窗
    }
    if (!on) { lastExportPct = -1; exportAbortController = null; }
  }

  function setExportProgress(pct, text) {
    if (!ensureExportProgressUI()) return;
    var box = document.getElementById("qstExpProgress");
    if (!box) return;
    var fill = box.querySelector(".bar i");
    var txt = box.querySelector(".txt");
    if (fill) fill.style.width = Math.max(0, Math.min(100, Math.round(pct * 100))) + "%";
    if (txt && text) txt.innerHTML = text;
  }

  /** 把 qst-web-export 的进度事件翻译成人话（阶段 + 百分比 + 已下载体积）。 */
  function onExportProgress(p) {
    if (!p) return;
    if (p.phase === "player") {
      var pct = p.ratio || 0;
      // 下载阶段占整体进度的 0~90%（后面打包/组装是毫秒级，留 10% 给它们）
      setExportProgress(pct * 0.9, p.cached || p.local
        ? "播放器模板已在本地缓存"
        : "正在下载播放器模板 <b>" + fmtMB(p.loaded) + " / "
          + (p.total ? fmtMB(p.total) : "?") + "</b>（" + Math.round(pct * 100) + "%）");
    } else if (p.phase === "pack") {
      if (p.ratio >= 1) {
        setExportProgress(0.96, "脚本包已生成 <b>" + fmtMB(p.payloadBytes) + "</b>");
      } else {
        setExportProgress(0.9, "正在打包脚本与设置…");
      }
    } else if (p.phase === "assemble") {
      setExportProgress(0.98, "正在把脚本追加到播放器上…");
    } else if (p.phase === "done") {
      setExportProgress(1, "完成 <b>" + fmtMB(p.exeBytes) + "</b>");
    }
  }

  function fmtMB(n) {
    var v = Number(n) || 0;
    if (v >= 1024 * 1024) return (v / 1024 / 1024).toFixed(2) + " MB";
    if (v >= 1024) return Math.round(v / 1024) + " KB";
    return v + " B";
  }

  /** kind 别名归一（前端有的地方传 scripts/recordings）。 */
  function normKind(kind) {
    var k = String(kind || "macro");
    if (k === "scripts" || k === "script") return "macro";
    if (k === "recordings" || k === "recording") return "rec";
    if (k === "tasks") return "sched";
    if (k === "chats" || k === "conversations") return "ai";
    return k;
  }

  function itemsOfKind(kind) {
    var k = normKind(kind);
    if (k === "macro") return macros;
    if (k === "rec") return recordings;
    if (k === "sched") return tasks;
    if (k === "ai") return chats;
    return [];
  }

  function libraryFoldersFor(kind) {
    var k = normKind(kind);
    var set = {};
    itemsOfKind(k).forEach(function (it) {
      var f = normFolderPath(it.folder);
      if (f) set[f] = 1;
    });
    (extraFolders[k] || []).forEach(function (f) {
      var n = normFolderPath(f);
      if (n) set[n] = 1;
    });
    return Object.keys(set).sort();
  }

  /** 目录是否存在（含"仅由条目隐式存在"的情况）。 */
  function folderExists(kind, folder) {
    return libraryFoldersFor(kind).indexOf(normFolderPath(folder)) >= 0;
  }

  /** 把目录路径（含其子孙）整体改名 / 删除时同步所有条目的 folder 字段。 */
  function rewriteItemFolders(kind, from, to) {
    var src = normFolderPath(from);
    var dst = normFolderPath(to);
    if (!src) return 0;
    var n = 0;
    itemsOfKind(kind).forEach(function (it) {
      var f = normFolderPath(it.folder);
      if (f === src || f.indexOf(src + "/") === 0) {
        it.folder = dst ? (dst + f.slice(src.length)) : f.slice(src.length + 1);
        n++;
      }
    });
    return n;
  }

  function defaultSettings() {
    return JSON.parse(
      JSON.stringify({
        click: {
          enableRandomInterval: false,
          randomIntervalMaxSeconds: 0.2,
          enablePressReleaseInterval: true,
          pressReleaseIntervalSeconds: 0.02,
          enableCoordinateJitter: false,
          jitterX: 2,
          jitterY: 2,
          enableFixedCoordinates: false,
          fixedX: 0,
          fixedY: 0,
          enableClickCountLimit: false,
          clickCountLimit: 500,
        },
        playback: {
          enablePlaybackCount: true,
          playbackCount: 1,
          enablePlaybackInterval: false,
          playbackIntervalMinSeconds: 0.5,
          playbackIntervalMaxSeconds: 1.5,
          enableDebugOutputWindow: false,
          autoOutputKeyFunctionDebug: true,
          recordingClickCaptureEnabled: true,
          recordingClickCaptureHalfSize: 32,
          enablePlaybackSpeed: false,
          playbackSpeed: 1,
          foregroundInputBackend: 0,
          scheduledTaskConflictPolicy: 0,
          scheduledTaskAutoResume: false,
        },
        other: {
          themeId: 7,
          useCustomTheme: 0,
          autoHideMainWindow: true,
          playSoundOnStart: true,
          playSoundOnEnd: true,
          hideBottomRightTip: true,
          closeToTray: true,
          showFloatBall: true,
          autoStartOnBoot: false,
          resolveImeConflict: true,
          editorDefaultView: "code",
          visualLoopWrap: true,
          visualBlockCallWires: true,
          visualIfWrap: true,
          visualBlockWrap: true,
          visualJumpWires: true,
          visualShowGrid: true,
          visualShowCardId: true,
          editorActionOrder: [],
          editorHiddenActions: [],
          holdThresholdSeconds: 0.12,
        },
        windowMode: {
          showPreviewThumbnail: true,
          previewRefreshMs: 500,
          blockRunWhenUnhealthy: true,
          allowForegroundInputFallback: false,
        },
        home: {
          clickerButton: 0,
          clickerIntervalMode: 1,
          clickerCustomInterval: 0.1,
          recorderInputMode: 0,
          activeTab: 0,
          clickerScrollOffset: 0,
          macroScrollOffset: 0,
          recorderScrollOffset: 0,
          scriptCustomScrollOffset: 0,
        },
        ai: {
          enabled: false,
          apiUrl: "",
          apiKey: "",
          modelName: "",
          temperature: 0.3,
          maxTokens: 4096,
          savedModels: [],
        },
      })
    );
  }

  function simulateAgentReply(msg) {
    var panelKey = String(msg.panelKey || "");
    var text = String(msg.text || "").trim();
    var replyText =
      "（网页体验版假对话流）\n\n我已经理解了你的需求：" +
      (text ? "“" + text.slice(0, 80) + "”" : "你的描述") +
      "。\n\n在桌面客户端中，我会调用工具为你生成或修改宏：\n1. 解析需求并规划动作序列；\n2. 调用 buildScriptActions 规范生成脚本；\n3. 支持读取/写入脚本与定时任务，并展示变更记录供你一键撤销。\n\n当前网页版只演示界面流程，真实 AI 对话请下载 Windows 客户端体验。";

    var parts = replyText.split("\n");
    parts.forEach(function (part, i) {
      setTimeout(function () {
        emit({
          type: "sendAgentMessage.delta",
          delta: part + (i < parts.length - 1 ? "\n" : ""),
          panelKey: panelKey,
        }, 120 + i * 90);
      }, 0);
    });
    setTimeout(function () {
      emit({
        type: "sendAgentMessage.result",
        ok: true,
        reply: replyText,
        id: "demo-agent-" + fakeState.agentSeq,
        name: "示例对话",
        panelKey: panelKey,
      }, 160 + parts.length * 90);
    }, 0);
  }

  /* ---------- 入口 ---------- */
  window.qstBridge = {
    post: function (payload) {
      var msg = payload;
      if (typeof payload === "string") {
        try {
          msg = JSON.parse(payload);
        } catch (_) {
          return;
        }
      }
      if (!msg || typeof msg !== "object") return;
      route(msg);
    },
  };

  // 启动即同步一次窗口尺寸与模式，让 app.js 按设计稿尺寸工作
  setTimeout(function () {
    sendClientSize(fakeState.mode);
    notifyParent({ type: "ready", mode: fakeState.mode, w: DESIGN[fakeState.mode].w, h: DESIGN[fakeState.mode].h });
    // ⚠ file:// 下"导出为独立 EXE"必定失败（浏览器禁止 fetch 本地文件）。
    //   与其让用户点完导出再对着 `Failed to fetch` 猜，不如一进来就说清楚。
    if (window.QstWebExport && window.QstWebExport.isFileProtocol
        && window.QstWebExport.isFileProtocol()) {
      setTimeout(function () {
        toast("本地直接双击打开（file://）：界面可以看，但「导出为独立 EXE」用不了。"
          + "请用 tools\\serve_website.ps1 起本地服务器后从 http://127.0.0.1:8090/demo.html 打开。");
      }, 1200);
    }
  }, 60);
})();
