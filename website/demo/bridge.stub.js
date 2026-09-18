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
  var macros = [
    {
      id: "demo-macro-1",
      path: "示例宏/自动签到.json",
      name: "自动签到.json",
      folder: "示例宏",
      hotkey: "F9",
      actionCount: 12,
      meta: "12 动作 · 08-05",
    },
    {
      id: "demo-macro-2",
      path: "游戏辅助/循环刷图.json",
      name: "循环刷图.json",
      folder: "游戏辅助",
      hotkey: "",
      actionCount: 38,
      meta: "38 动作 · 08-03",
    },
    {
      id: "demo-macro-3",
      path: "办公自动化/表格录入.json",
      name: "表格录入.json",
      folder: "办公自动化",
      hotkey: "Ctrl+F9",
      actionCount: 26,
      meta: "26 动作 · 08-01",
    },
    {
      id: "demo-macro-4",
      path: "新手入门/示例宏.json",
      name: "示例宏.json",
      folder: "",
      hotkey: "F8",
      actionCount: 8,
      meta: "8 动作 · 07-30",
    },
  ];

  var recordings = [
    {
      id: "demo-rec-1",
      path: "录制/登录流程录制.json",
      name: "登录流程录制.json",
      folder: "录制",
      hotkey: "",
      actionCount: 86,
      recordTime: "1分24秒",
      meta: "86 动作 · 1分24秒",
    },
    {
      id: "demo-rec-2",
      path: "录制/表单填写.json",
      name: "表单填写.json",
      folder: "",
      hotkey: "",
      actionCount: 42,
      recordTime: "38秒",
      meta: "42 动作 · 38秒",
    },
  ];

  var chats = [
    {
      id: "demo-chat-1",
      name: "示例：帮我写一个自动签到脚本",
      folder: "",
      meta: "3 条消息 · 昨天",
    },
    {
      id: "demo-chat-2",
      name: "新手入门：什么是窗口模式",
      folder: "",
      meta: "2 条消息 · 08-02",
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
      enablePlaybackCount: false,
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
      name: "每日签到",
      kind: 1,
      filePath: "示例宏/自动签到.json",
      fileDisplayName: "自动签到.json",
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
      name: "每周报表截图",
      kind: 0,
      filePath: "录制/登录流程录制.json",
      fileDisplayName: "登录流程录制.json",
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
  function sampleActions() {
    return [
      {
        type: "moveMouse",
        name: "移动鼠标到",
        remark: "移动到「登录」按钮",
        x: 640,
        y: 480,
        duration: 0.2,
        indent: 0,
        randomX: 0,
        randomY: 0,
      },
      {
        type: "mouseClick",
        name: "鼠标点击",
        remark: "左键点击登录",
        button: "left",
        clickCount: 1,
        duration: 0.01,
        indent: 0,
      },
      {
        type: "wait",
        name: "等待",
        remark: "等待页面加载",
        duration: 1.2,
        indent: 0,
      },
      {
        type: "loop",
        name: "循环",
        remark: "重复 3 次收尾动作",
        loopCount: 3,
        indent: 0,
      },
      {
        type: "moveMouse",
        name: "移动鼠标到",
        remark: "确认弹窗",
        x: 520,
        y: 400,
        duration: 0.15,
        indent: 1,
      },
      {
        type: "mouseClick",
        name: "鼠标点击",
        remark: "点确定",
        button: "left",
        duration: 0.01,
        indent: 1,
      },
      {
        type: "wait",
        name: "等待",
        remark: "弹窗关闭",
        duration: 0.8,
        indent: 1,
      },
      {
        type: "endLoop",
        name: "跳出循环",
        remark: "",
        indent: 1, // 与循环体同级，位于 loop 容器内
      },
      {
        type: "if",
        name: "条件-如果",
        remark: "如果变量 {done} == 1",
        conditionExpr: "{done} == 1",
        indent: 0,
      },
      {
        type: "keyClick",
        name: "按键点击",
        remark: "回车确认",
        keyText: "Enter",
        keyVk: 13,
        duration: 0.01,
        indent: 1,
      },
      {
        type: "else",
        name: "条件-否则",
        remark: "",
        indent: 0,
      },
      {
        type: "quickInput",
        name: "快捷输入",
        remark: "输入完成标记",
        inputText: "done",
        charInterval: 0.01,
        indent: 1,
      },
      {
        type: "stopMacro",
        name: "结束宏运行",
        remark: "",
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
      var mode = m.mode;
      if (mode == null) {
        if (/循环刷图/.test(p)) mode = 1;
        else if (/表格录入/.test(p)) mode = 2;
        else mode = 0;
      }
      var wm = m.windowMode;
      if (!wm) {
        if (mode === 1) {
          wm = {
            enabled: 1,
            executionKind: "hiddenDesktop",
            selectMethod: "useEditorWindowClass",
            targetExePath: "C:\\Game\\game.exe",
            windowName: "游戏窗口",
            windowClassName: "UnityWndClass",
            fakeFocusEnabled: 0,
          };
        } else if (mode === 2) {
          wm = {
            enabled: 1,
            executionKind: "backgroundWindow",
            selectMethod: "useEditorWindowClass",
            targetExePath: "C:\\Office\\EXCEL.EXE",
            windowName: "工作簿",
            windowClassName: "XLMAIN",
            fakeFocusEnabled: 1,
          };
        } else {
          wm = {
            enabled: 0,
            executionKind: "hiddenDesktop",
            selectMethod: "selectOnStartup",
            targetExePath: "",
            fakeFocusEnabled: 0,
          };
        }
      }
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
    var name = String(payload.name || "").replace(/\.json$/i, "") + ".json";
    var existing = findMacro(p);
    if (existing) {
      existing.name = name;
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
      path: p || ("新建宏/" + name),
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
          var rn = String(msg.name || "").replace(/\.json$/i, "") + ".json";
          macros = macros.map(function (m) {
            if (String(m.path) === rp || String(m.id) === rp) {
              m.name = rn;
              m.path = (m.folder ? m.folder + "/" : "") + rn;
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
      case "exportScript":
        fail(type, DEMO_HINT);
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
            name: "网页体验录制.json",
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

      case "createLibraryFolder":
      case "renameLibraryFolder":
      case "deleteLibraryFolder":
      case "moveScriptToFolder":
      case "setItemLibraryFolder":
        fail(type, DEMO_HINT);
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

  function mergeSettings(next) {
    ["click", "playback", "other", "windowMode", "home", "ai"].forEach(function (sec) {
      if (next[sec] && typeof next[sec] === "object") {
        if (!settings[sec]) settings[sec] = {};
        Object.keys(next[sec]).forEach(function (k) {
          settings[sec][k] = next[sec][k];
        });
      }
    });
    // 产品 saveSettings / 编辑器设置保存是扁平字段，需写回 other（否则再 openSettingsData 会冲掉）
    var otherFlat = [
      "themeId",
      "useCustomTheme",
      "customMainColor",
      "customAccentColor",
      "preferDirect2D",
      "holdThresholdSeconds",
      "autoHideMainWindow",
      "playSoundOnStart",
      "playSoundOnEnd",
      "hideBottomRightTip",
      "closeToTray",
      "showFloatBall",
      "autoStartOnBoot",
      "resolveImeConflict",
      "editorDefaultView",
      "visualLoopWrap",
      "visualBlockCallWires",
      "visualIfWrap",
      "visualBlockWrap",
      "visualJumpWires",
      "visualShowGrid",
      "visualShowCardId",
      "editorActionOrder",
      "editorHiddenActions",
    ];
    if (!settings.other) settings.other = {};
    otherFlat.forEach(function (k) {
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

  function libraryFoldersFor(kind) {
    if (kind === "macro") return ["示例宏", "游戏辅助", "办公自动化"];
    if (kind === "rec") return ["录制"];
    if (kind === "sched") return ["每日任务"];
    if (kind === "ai") return ["新手入门"];
    return [];
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
          enablePlaybackCount: false,
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
  }, 60);
})();
