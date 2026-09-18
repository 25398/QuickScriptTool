/**
 * ui/app.js — product UI logic (real bridge data; no「待并入」on main paths)
 */
(function () {
  const AGENT_SHELL =
    document.documentElement.classList.contains("agent-shell") ||
    /(?:\?|&)mode=agent(?:&|$)/.test(String(location.search || "")) ||
    /(?:^|\/)agent\.html(?:$|\?)/i.test(String(location.pathname || location.href || ""));
  // agent.html 仅壳层；对话骨架由 openAgentSession 按需插入，避免开屏连闪
  if (AGENT_SHELL) document.documentElement.classList.add("agent-shell");
  if (AGENT_SHELL) document.documentElement.classList.add("agent-booting");

  const $ = (sel, root) => (root || document).querySelector(sel);
  const $$ = (sel, root) => Array.from((root || document).querySelectorAll(sel));

  const state = {
    tab: "clicker",
    macros: [],
    recordings: [],
    chats: [],
    macroSel: -1,
    recSel: -1,
    aiSel: -1,
    clicking: false,
    recording: false,
    macroRunning: false,
    clickBtn: 0,
    intervalMode: 1,
    customInterval: 0.1,
    intervalLabel: "高效模式 · 100 ms",
    hotkey: "F8",
    hotkeyVk: 119,
    hotkeyModifiers: 0,
    hotkeyHold: false,
    settings: null,
    editor: false,
    editorPath: "",
    editorName: "",
    editorActions: [],
    actionSel: -1,
    batchMode: false,
    batchSel: {},
    editorUndo: [],
    editorRedo: [],
    editorClipboard: null,
    // 对齐原生 popupAction_ 默认 sel=0（移动鼠标）；打开已有宏时由首条动作覆盖
    addActionType: "moveMouse",
    addPreview: null,
    editDraft: null, // 选中动作的右栏草稿；点「修改」才写回列表（对齐原生）
    recMode: 0,
    recWindowMode: 0, // 0=全屏模式 1=窗口模式（窗口相对录制）
    uiMode: "simple", // 极简 / 专业（专业模式主界面为设计稿样式）
    hideBottomRightTip: true,
    pendingPath: "",
    pendingKind: "",
    hotkeyDraft: { text: "F8", vk: 119, modifiers: 0, hold: false },
    collapsedContainers: {},
    agentId: "",
    agentModel: "",
    editorMode: 0,
    windowMode: { enabled: 0, executionKind: "hiddenDesktop", selectMethod: "selectOnStartup", targetExePath: "", fakeFocusEnabled: 0, coordSpace: "screenAbsolute" },
    agentBusy: false,
    agentAttachments: [],
    agentTabs: [],
    agentTabKey: "",
    agentMinimized: false,
    breakoutPaused: false,
    _runningMode: 0,
    _runningWindowMode: null,
    _executedSteps: 0,
    _runningMacroName: "",
    _editorSnapshot: "",
    _pendingRunKind: "",
    _gotScripts: false,
    _gotRecordings: false,
    _gotSettings: false,
    _homeRestored: false,
    // ── 编辑器调试（仅缓存，关闭编辑器即清空）──────────────
    editorBreakpoints: {}, // action index → true
    debugHotkey: { text: "F9", vk: 0x78, modifiers: 0 },
    debugging: false,
    debugPaused: false,
    debugStepMode: false,
  };

  const PRODUCT_NAME = "键鼠工坊";

  const BTN_LABEL = ["左键", "中键", "右键"];
  const INTERVALS = [
    { t: "极限模式 · 10 ms", mode: 2 },
    { t: "高效模式 · 100 ms", mode: 1 },
    { t: "自定义…", mode: 0, custom: true },
  ];
  const REC_MODES = [
    { t: "自动识别", mode: 0 },
    { t: "绝对坐标", mode: 1 },
    { t: "相对坐标", mode: 2 },
    { t: "图片定位", mode: 3 },
  ];
  // 假焦点注入技术（仅展示“独立”方案与“复合”方案；
  // 被复合覆盖的单独项不再上菜单，命令行/自测仍可用）
  const WM_TECH_LIST = [
    { v: 0, t: "经典远线程", d: "最普通的注入方式，先拿它做基线对照" },
    { v: 1, t: "NtCreateThreadEx", d: "换种方式建线程，能绕过一部分 API 检测" },
    { v: 2, t: "APC 注入", d: "借目标自己的线程干活，不新建线程" },
    { v: 6, t: "窗口消息钩子", d: "走系统消息钩子路径，看起来像正常软件" },
    { v: 7, t: "手动映射+线程劫持", d: "组合拳：不新建线程，模块列表也看不到" },
    { v: 8, t: "XOR+映射+劫持", d: "最隐蔽全套：加密文件+手动映射+借线程执行" },
    { v: 10, t: "映像节映射+线程劫持", d: "按系统映像方式映射，内存里像正常加载的 DLL，也不新建线程" },
  ];
  const ED_MODES = [
    { t: "默认模式", v: 0 },
    { t: "窗口模式", v: 1 },
    { t: "后台窗口模式", v: 2 },
  ];
  const NESTED_USE_MODES = [
    { t: "默认模式", v: 0 },
    { t: "窗口模式", v: 1 },
    { t: "后台窗口模式", v: 2 },
    { t: "继承模式", v: 3 },
  ];
  const WM_METHODS = [
    { t: "启动时使用当前所在窗口", v: "selectOnStartup" },
    { t: "使用宏编辑时获取到的窗口类名", v: "useEditorWindowClass" },
    { t: "不选择窗口", v: "noSelect" },
  ];
  const WM_METHOD_LABEL_ALIASES = {
    "启动时选择窗口（当前聚焦窗）": "selectOnStartup",
    "启动时获取鼠标位置的窗口": "selectOnStartup",
    "启动时使用当前所在窗口": "selectOnStartup",
    "使用宏编辑时获取到的窗口类名": "useEditorWindowClass",
    "不选择窗口": "noSelect",
  };
  const ACTION_TYPES = [
    { t: "移动鼠标到", v: "moveMouse" },
    { t: "等待", v: "wait" },
    { t: "鼠标点击", v: "mouseClick" },
    { t: "运行录制回放", v: "mousePlayback" },
    { t: "运行鼠标宏", v: "runMacro" },
    { t: "鼠标按下", v: "mouseDown" },
    { t: "鼠标松开", v: "mouseUp" },
    { t: "滚动滚轮", v: "scrollWheel" },
    { t: "按键点击", v: "keyClick" },
    { t: "键盘按下", v: "keyDown" },
    { t: "键盘松开", v: "keyUp" },
    { t: "快捷按键", v: "hotkeyShortcut" },
    { t: "快捷输入", v: "quickInput" },
    { t: "循环", v: "loop" },
    { t: "跳出循环", v: "endLoop" },
    { t: "定义宏指令块", v: "defineBlock" },
    { t: "运行宏指令块", v: "runBlock" },
    { t: "找图", v: "findImage" },
    { t: "获取颜色", v: "getColor" },
    { t: "找色", v: "findColor" },
    { t: "颜色匹配", v: "colorMatch" },
    { t: "文字识别", v: "textRecognition" },
    { t: "条件-如果", v: "if" },
    { t: "条件-否则", v: "else" },
    { t: "锁定截屏", v: "lockScreenshot" },
    { t: "解锁截屏", v: "unlockScreenshot" },
    { t: "结束宏运行", v: "stopMacro" },
    { t: "运行程序", v: "runProgram" },
    { t: "关闭程序", v: "closeProgram" },
    { t: "打开网页", v: "openWebpage" },
    { t: "打开文件", v: "openFile" },
    { t: "激活窗口", v: "activateWindow" },
    { t: "计时器记录时间", v: "timerRecordTime" },
    { t: "AI文字分析", v: "aiTextAnalysis" },
    { t: "AI图片分析", v: "aiImageAnalysis" },
    { t: "AI动作执行", v: "aiActionExecute" },
    { t: "获取当前光标位置", v: "getCursorPos" },
    { t: "跳转", v: "goto" },
    { t: "相对移动鼠标", v: "moveMouseRelative" },
  ];
  const MERGE_CONTAINER_TYPES = ACTION_TYPES.filter(
    (a) => a.v === "loop" || a.v === "defineBlock" || a.v === "if" || a.v === "else"
  );
  let _mergeUiOn = false;

  function knownActionTypeMap() {
    const m = new Map();
    ACTION_TYPES.forEach((a) => m.set(a.v, a));
    return m;
  }

  function normalizeActionCatalog(other) {
    const known = ACTION_TYPES.map((a) => a.v);
    const knownSet = new Set(known);
    const rawOrder = Array.isArray(other && other.editorActionOrder) ? other.editorActionOrder : [];
    const order = [];
    const seen = new Set();
    for (const t of rawOrder) {
      if (typeof t !== "string" || !knownSet.has(t) || seen.has(t)) continue;
      seen.add(t);
      order.push(t);
    }
    for (const t of known) {
      if (!seen.has(t)) order.push(t);
    }
    const rawHidden = Array.isArray(other && other.editorHiddenActions)
      ? other.editorHiddenActions
      : [];
    const hidden = [];
    const hiddenSeen = new Set();
    for (const t of rawHidden) {
      if (typeof t !== "string" || !knownSet.has(t) || hiddenSeen.has(t)) continue;
      hiddenSeen.add(t);
      hidden.push(t);
    }
    if (hidden.length >= order.length && order.length) {
      const keep = order[0];
      const idx = hidden.indexOf(keep);
      if (idx >= 0) hidden.splice(idx, 1);
    }
    return { order, hidden };
  }

  function pickerActionTypes(extraType) {
    const other = (state.settings && state.settings.other) || {};
    const { order, hidden } = normalizeActionCatalog(other);
    const hiddenSet = new Set(hidden);
    const byV = knownActionTypeMap();
    const list = [];
    for (const v of order) {
      if (hiddenSet.has(v) && v !== extraType) continue;
      const a = byV.get(v);
      if (a) list.push(a);
    }
    if (extraType && !list.some((a) => a.v === extraType)) {
      const a = byV.get(extraType);
      if (a) list.push(a);
    }
    return list.length ? list : ACTION_TYPES.slice();
  }

  function pickerMergeContainerTypes() {
    const allowed = new Set(["loop", "defineBlock", "if", "else"]);
    const list = pickerActionTypes().filter((a) => allowed.has(a.v));
    return list.length ? list : MERGE_CONTAINER_TYPES.slice();
  }

  function firstVisibleActionType() {
    const t = pickerActionTypes()[0];
    return (t && t.v) || "moveMouse";
  }

  function syncEditorActionTypeCombo() {
    const shown = typeof editorParamAction === "function" ? editorParamAction() : null;
    const keep = shown && shown.type ? String(shown.type) : "";
    const hidden = new Set(
      normalizeActionCatalog((state.settings && state.settings.other) || {}).hidden
    );
    const prev = state.addActionType;
    if (state.addActionType && hidden.has(state.addActionType) && state.addActionType !== keep) {
      state.addActionType = firstVisibleActionType();
    }
    const typeCombo = $("#edActionType");
    if (!typeCombo) return;
    const mergeOn = typeof isMergeEligible === "function" && isMergeEligible();
    const types = mergeOn ? pickerMergeContainerTypes() : pickerActionTypes(state.addActionType);
    let cur = types.find((a) => a.v === state.addActionType);
    if (!cur) {
      cur = types[0];
      if (cur) state.addActionType = cur.v;
    }
    if (cur) typeCombo.textContent = cur.t;
    if (state.editor && state.addPreview && prev !== state.addActionType && prev !== keep) {
      showAddTypePreview();
    }
  }

  function toast(msg) {
    const el = $("#toast");
    const text = String(msg == null ? "" : msg).trim();
    // 内部落盘竞态文案，勿打扰用户（选中/自动 quietSave 时偶发）
    if (!el || !text || /SaveAppSettings\s*failed/i.test(text)) return;
    el.textContent = text;
    el.classList.add("show");
    clearTimeout(toast._t);
    toast._t = setTimeout(() => el.classList.remove("show"), 2200);
  }

  let _debugPinned = true;
  let _debugMin = false;
  const DEBUG_LOG_MAX = 4000;

  function appendDebugLogLines(lines) {
    const log = $("#debugLog");
    if (!log || !lines || !lines.length) return;
    const chunk = lines.map((t) => String(t || "")).join("\n");
    if (!chunk) return;
    log.textContent += (log.textContent ? "\n" : "") + chunk;
    // 行数计数：仅在超预算时整段裁剪，避免每条消息都全量 split/join（O(n²)）
    log._qstLineCount = (log._qstLineCount | 0) + lines.length;
    if (log._qstLineCount > DEBUG_LOG_MAX + 200) {
      const parts = log.textContent.split("\n");
      log.textContent = parts.slice(Math.max(0, parts.length - DEBUG_LOG_MAX)).join("\n");
      log._qstLineCount = DEBUG_LOG_MAX;
    }
    log.scrollTop = log.scrollHeight;
  }

  function showDebugFloat(opts) {
    // 产品路径：独立调试 WebView 顶层窗（可拖出主壳）；本浮层仅 HTML 预览兜底
    if (window.qst) return;
    const el = $("#debugFloat");
    if (!el) return;
    el.hidden = false;
    const clear = !(opts && opts.keepLog);
    el.classList.add("show");
    el.setAttribute("aria-hidden", "false");
    if (clear) {
      const log = $("#debugLog");
      if (log) {
        log.textContent = "";
        log._qstLineCount = 0;
      }
    }
    el.classList.toggle("minimized", !!_debugMin);
    const pin = $("#btnDebugPin");
    if (pin) pin.classList.toggle("on", !!_debugPinned);
  }

  function hideDebugFloat() {
    const el = $("#debugFloat");
    if (!el) return;
    el.classList.remove("show");
    el.setAttribute("aria-hidden", "true");
    el.hidden = true;
  }

  function closeDebugFloatByUser() {
    hideDebugFloat();
    setChk($("#setDebugWin"), false);
    if (state.settings && state.settings.playback) {
      state.settings.playback.enableDebugOutputWindow = false;
    }
    if (window.qst) qst.debugWindowClosed();
  }

  /*
   * UI 设计要求（用户确认；细节见 ui/index.html :root 注释）：
   * - 主界面客户区 1552×960（黄金分割）；宏编辑器 1800×1230（HWND 由壳保证）
   * - 键鼠录制优化弹窗 1640×1140（壳 setMode opt）
   * - 设置 / AI 助手组件与主界面同一 --qst-u=1.5，禁止 JS zoom
   */
  const SHELL_CLIENT = {
    home: { w: 1552, h: 960 },
    editor: { w: 1800, h: 1230 },
    opt: { w: 1640, h: 1140 },
  };

  function applyShellScale() {
    // 仅同步客户区记录；组件大小由 CSS --qst-u 负责，不做运行时 zoom
    const editor = !!(
      state.editor ||
      ($("#app") && $("#app").classList.contains("editor-mode"))
    );
    const opt = !!document.body.classList.contains("opt-open");
    const design = editor
      ? SHELL_CLIENT.editor
      : opt
        ? SHELL_CLIENT.opt
        : SHELL_CLIENT.home;
    if (!(state._clientW | 0)) state._clientW = design.w;
    if (!(state._clientH | 0)) state._clientH = design.h;
    const app = $("#app");
    document.documentElement.style.zoom = "1";
    if (app) {
      app.style.zoom = "1";
      app.style.width = "";
      app.style.height = "";
      app.style.transform = "";
    }
    state._shellZoom = 1;
  }

  function esc(s) {
    return String(s == null ? "" : s)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  function typeLabel(type) {
    const hit = ACTION_TYPES.find((a) => a.v === type);
    if (hit) return hit.t;
    const map = {
      timerRecordTime: "计时器记录时间",
      getCursorPos: "获取当前光标位置",
      moveMouseRelative: "相对移动鼠标",
      lockScreenshot: "锁定截屏",
      unlockScreenshot: "解锁截屏",
      stopMacro: "结束宏运行",
      endLoop: "跳出循环",
      moveMouse: "移动鼠标到",
      mousePlayback: "运行录制回放",
      runMacro: "运行鼠标宏",
      hotkeyShortcut: "快捷按键",
      defineBlock: "定义宏指令块",
      runBlock: "运行宏指令块",
      findImage: "找图",
      getColor: "获取颜色",
      findColor: "找色",
      colorMatch: "颜色匹配",
      textRecognition: "文字识别",
      if: "条件-如果",
      else: "条件-否则",
      aiTextAnalysis: "AI文字分析",
      aiImageAnalysis: "AI图片分析",
      aiActionExecute: "AI动作执行",
    };
    return map[type] || type || "动作";
  }

  function f3(value) {
    const n = Number(value);
    if (!Number.isFinite(n)) return "0.000";
    return n.toFixed(3);
  }

  function holdText(a) {
    const parts = [];
    if (a.holdLeftWin || a.holdRightWin) parts.push("Win");
    if (a.holdLeftCtrl || a.holdRightCtrl) parts.push("Ctrl");
    if (a.holdLeftAlt || a.holdRightAlt) parts.push("Alt");
    if (a.holdLeftShift || a.holdRightShift) parts.push("Shift");
    return parts.join("+");
  }

  function buttonText(btn) {
    if (btn === "right") return "右键";
    if (btn === "middle") return "中键";
    if (btn === "x1") return "X1键";
    if (btn === "x2") return "X2键";
    return "左键";
  }

  function repeatInfo(a) {
    return (
      "[重复" +
      (a.clickCount ?? 1) +
      "次间隔" +
      f3(a.duration) +
      "+随机" +
      f3(a.randomDuration) +
      "秒]"
    );
  }

  const SHORTCUT_LABELS = [
    "Ctrl+C(拷贝)",
    "Ctrl+V(粘贴)",
    "Ctrl+X(剪切)",
    "Ctrl+S(保存)",
    "Ctrl+F(查找)",
    "Alt+F4(关闭窗口)",
    "Win+D(所有最小化)",
    "Win+R(打开运行)",
    "Ctrl+Alt+Delete",
  ];

  const RUN_PROGRAM_LABELS = [
    "选择文件",
    "记事本",
    "计算器",
    "画图",
    "文件管理器",
    "命令行",
    "PowerShell",
    "进程管理器",
    "注册表编辑器",
    "服务",
    "计算机管理",
    "控制面板",
    "设置",
  ];

  function runProgramDisplayName(a) {
    const preset = a.shortcutPreset | 0;
    if (preset > 0 && preset < RUN_PROGRAM_LABELS.length) return RUN_PROGRAM_LABELS[preset];
    const path = String(a.targetPath || "").trim();
    if (!path) return "未选择";
    const file = path.split(/[/\\]/).pop() || path;
    return file.replace(/\.[^.]+$/, "") || file;
  }

  /** 对齐原生 ActionName()（action_utils.cpp） */
  function displayActionName(a) {
    if (!a) return "动作";
    const t = a.type || "";
    // customText/text 仅自定义文本动作用作显示名；wait/找图等若误写入窗口标题会污染列表
    if (t === "customText") {
      const customOnly = String(a.customText || a.text || "").trim();
      if (customOnly) return customOnly;
    }
    const holds = holdText(a);
    switch (t) {
      case "moveMouse":
        if (a.moveFromVar) {
          return "移动鼠标到(" + (a.moveVarExprX || "") + "," + (a.moveVarExprY || "") + ")";
        }
        return "移动鼠标到(" + (a.x | 0) + "," + (a.y | 0) + ")";
      case "moveMouseRelative":
        return "相对移动鼠标(" + (a.x | 0) + "," + (a.y | 0) + ")";
      case "mouseDown":
        return "鼠标按下" + (holds ? holds + "+" : "") + buttonText(a.button);
      case "mouseUp":
        return "鼠标松开" + (holds ? holds + "+" : "") + buttonText(a.button);
      case "mouseClick":
        return (
          "鼠标点击" +
          (holds ? holds + "+" : "") +
          buttonText(a.button) +
          repeatInfo(a)
        );
      case "keyDown":
        return "键盘按下" + (holds ? holds + "+" : "") + (a.keyText || "");
      case "keyUp":
        return "键盘松开" + (holds ? holds + "+" : "") + (a.keyText || "");
      case "keyClick":
        return (
          "按键点击" +
          (holds ? holds + "+" : "") +
          (a.keyText || "") +
          repeatInfo(a)
        );
      case "wait":
        return "等待 " + f3(a.duration) + "秒+随机0~" + f3(a.randomDuration) + "秒";
      case "loop":
        if (a.loopFromVar && a.loopVarExpr) return "循环[" + a.loopVarExpr + "]";
        return (a.loopCount | 0) < 0
          ? "循环[无限循环]"
          : "循环[" + (a.loopCount | 0) + "次]";
      case "endLoop":
        return "跳出循环";
      case "defineBlock":
        return "定义宏指令块[" + (a.blockName || "未命名") + "]";
      case "runBlock":
        return "运行宏指令块[" + (a.blockName || "未命名") + "]" + repeatInfo(a);
      case "runMacro":
        return "运行鼠标宏[" + (a.blockName || "未选择") + "]" + repeatInfo(a);
      case "mousePlayback":
        return "运行录制回放[" + (a.blockName || "未选择") + "]" + repeatInfo(a);
      case "hotkeyShortcut": {
        const idx = Math.max(0, Math.min(SHORTCUT_LABELS.length - 1, a.shortcutPreset | 0));
        return "快捷按键[" + SHORTCUT_LABELS[idx] + "]" + repeatInfo(a);
      }
      case "quickInput": {
        let preview = String(a.inputText || "");
        if (preview.length > 24) preview = preview.slice(0, 24) + "...";
        return "快捷输入[" + preview + "]" + repeatInfo(a);
      }
      case "scrollWheel": {
        let type = "";
        if (a.scrollVertical) type += "垂直";
        if (a.scrollHorizontal) type += (type ? "+" : "") + "水平";
        if (!type) type = "垂直";
        const dir = (a.scrollDirection | 0) === 0 ? "向上/左" : "向下/右";
        return (
          "滚动滚轮[" + type + "," + dir + "," + (a.scrollSteps | 0) + "步]" + repeatInfo(a)
        );
      }
      case "findImage": {
        const fu = a.findImageFollowUp | 0;
        const follow =
          fu === 1
            ? "移动到"
            : fu === 2
              ? "保存匹配度"
              : fu === 3
                ? "保存图片"
                : "点击";
        const thr = matchThresholdPercent(a.matchThreshold);
        return (
          "找图[" +
          follow +
          "," +
          (a.perfectMatch ? "完美匹配" : "匹配>" + thr + "%") +
          ",缩放" +
          f3(a.imageScaleMin ?? 1) +
          "-" +
          f3(a.imageScaleMax ?? 1) +
          "]"
        );
      }
      case "textRecognition": {
        const mode = (a.ocrResultMode | 0) === 1 ? "文字查找" : "获取文字";
        const follow =
          (a.ocrFollowUp | 0) === 1
            ? "移动到"
            : (a.ocrFollowUp | 0) === 2
              ? "保存变量"
              : "点击";
        return "文字识别[" + mode + "," + follow + "]";
      }
      case "if": {
        let cond = String(a.conditionExpr || "").replace(/[\r\n]/g, " ");
        if (cond.length > 36) cond = cond.slice(0, 36) + "...";
        return "如果[条件:" + cond + "]";
      }
      case "else":
        return "否则";
      case "lockScreenshot":
        return "锁定截屏";
      case "unlockScreenshot":
        return "解锁截屏";
      case "stopMacro":
        return "结束宏运行";
      case "goto":
        return "跳转到第[" + (a.gotoStepExpr || "未填写") + "]步";
      case "runProgram":
        return "打开程序[" + runProgramDisplayName(a) + "]";
      case "closeProgram": {
        let target = String(a.targetPath || "");
        if (target.length > 36) target = target.slice(0, 36) + "...";
        return "关闭程序[" + (target || "未选择") + "]";
      }
      case "openWebpage": {
        let url = String(a.targetPath || "");
        if (url.length > 36) url = url.slice(0, 36) + "...";
        return "打开网页[" + (url || "未填写") + "]";
      }
      case "openFile": {
        let path = String(a.targetPath || "");
        if (path.length > 36) path = path.slice(0, 36) + "...";
        return "打开文件[" + (path || "未选择") + "]";
      }
      case "activateWindow": {
        let m = String(a.targetPath || "");
        if (m.length > 36) m = m.slice(0, 36) + "...";
        return "激活窗口[" + (m || "未填写") + "]";
      }
      case "timerRecordTime":
        return "计时器记录时间[" + (a.loopVarName || "未命名") + "]";
      case "getCursorPos":
        return "获取当前光标位置→[" + (a.matchVarName || "未命名") + "]";
      case "aiTextAnalysis":
      case "aiImageAnalysis":
      case "aiActionExecute": {
        let preview = String(a.aiPrompt || "").replace(/[\r\n]/g, " ");
        if (preview.length > 30) preview = preview.slice(0, 30) + "...";
        if (t === "aiActionExecute") return "AI动作执行: " + preview + "...";
        const label = t === "aiTextAnalysis" ? "AI文字分析" : "AI图片分析";
        return label + ": " + preview + " → 变量[" + (a.aiOutputVarName || "") + "]";
      }
      case "customText":
        return String(a.customText || a.text || "").trim() || "自定义文本";
      default:
        return typeLabel(t);
    }
  }

  function syncActionListRowName(index) {
    const i = index | 0;
    if (i < 0 || !state.editorActions[i]) return;
    const row = document.querySelector(`#actionList .arow[data-i="${i}"] .aname-text`);
    if (row) row.textContent = displayActionName(state.editorActions[i]);
    const V = window.QstVisualEditor;
    if (V && typeof V.refreshCard === "function") V.refreshCard(i);
    const remarkCell = document.querySelector(`#actionList .arow[data-i="${i}"] span:nth-child(3)`);
    // 备注列在 aname 后；批量/选中时 class 不同，稳妥用整行刷新时更新
    void remarkCell;
  }

  function loadEditDraftFromSelection() {
    if (state.actionSel < 0 || !state.editorActions[state.actionSel]) {
      state.editDraft = null;
      return null;
    }
    state.editDraft = JSON.parse(JSON.stringify(state.editorActions[state.actionSel]));
    delete state.editDraft._preview;
    return state.editDraft;
  }

  function captureParamPanelIntoDraft() {
    const remark = ($("#edRemark")?.textContent || "").trim();
    if (state.addPreview) {
      readParamPanelInto(state.addPreview);
      state.addPreview.remark = remark;
      return;
    }
    if (state.actionSel >= 0 && state.editDraft) {
      readParamPanelInto(state.editDraft);
      state.editDraft.remark = remark;
    }
  }

  /** 当前参数面板动作：添加预览 或 选中项草稿（未点修改前不碰列表） */
  function editorParamAction() {
    if (state.addPreview) return state.addPreview;
    if (state.actionSel >= 0) {
      if (!state.editDraft) loadEditDraftFromSelection();
      return state.editDraft;
    }
    return null;
  }

  function syncFormIntoSelectedBeforeSave() {
    // 对齐原生 SyncFormIntoActionsBeforeRun：保存前把右栏草稿写入选中项
    if (state.actionSel < 0 || !state.editorActions[state.actionSel]) return;
    if (!state.editDraft) return;
    readParamPanelInto(state.editDraft);
    const remark = ($("#edRemark")?.textContent || "").trim();
    state.editDraft.remark = remark;
    const prev = state.editorActions[state.actionSel];
    if (state.editDraft.type === "defineBlock" && !isValidBlockName(state.editDraft.blockName)) return;
    if (
      state.editDraft.type === "endLoop" &&
      !hasLoopParentAt(state.editorActions, state.actionSel, prev.indent | 0)
    ) {
      return;
    }
    const next = Object.assign({}, state.editDraft);
    next.indent = prev.indent;
    if (typeof prev._vx === "number") next._vx = prev._vx;
    if (typeof prev._vy === "number") next._vy = prev._vy;
    if (prev.no != null) next.no = prev.no;
    if (prev.originalNo != null) next.originalNo = prev.originalNo;
    next.name = typeLabel(next.type);
    delete next._preview;
    state.editorActions[state.actionSel] = next;
    state.editDraft = JSON.parse(JSON.stringify(next));
  }

  function commitModifySelected() {
    // 对齐原生 ModifySelected：仅此时写回动作列表
    if (state.actionSel < 0 || !state.editorActions[state.actionSel]) {
      toast("请先选中一条动作");
      return false;
    }
    if (!state.editDraft) loadEditDraftFromSelection();
    if (!state.editDraft) return false;
    readParamPanelInto(state.editDraft);
    const remark = ($("#edRemark")?.textContent || "").trim();
    state.editDraft.remark = remark;
    const prev = state.editorActions[state.actionSel];
    if (state.editDraft.type === "defineBlock") {
      if (!validateDefineBlockName(state.editDraft.blockName, state.actionSel)) return false;
    }
    if (
      state.editDraft.type === "endLoop" &&
      !hasLoopParentAt(state.editorActions, state.actionSel, prev.indent | 0)
    ) {
      toast(END_LOOP_NEEDS_LOOP_MSG);
      return false;
    }
    const next = Object.assign({}, state.editDraft);
    next.indent = prev.indent;
    if (typeof prev._vx === "number") next._vx = prev._vx;
    if (typeof prev._vy === "number") next._vy = prev._vy;
    if (prev.no != null) next.no = prev.no;
    if (prev.originalNo != null) next.originalNo = prev.originalNo;
    next.name = typeLabel(next.type);
    delete next._preview;
    state.editorActions[state.actionSel] = next;
    state.editDraft = JSON.parse(JSON.stringify(next));
    state.addPreview = null;
    state._skipReadback = true;
    renderEditorActions(state.editorActions);
    state._skipReadback = false;
    return true;
  }

  function clampIndent(v) {
    return Math.max(0, Math.min(16, v | 0));
  }

  function normalizeLoadedActions(actions) {
    if (!Array.isArray(actions)) return [];
    return actions.map((a) => {
      const copy = Object.assign({}, a);
      copy.type = copy.type || "wait";
      copy.name = typeLabel(copy.type);
      copy.indent = clampIndent(copy.indent);
      // customText：磁盘/桥接 JSON 字段为 text；面板读写 customText
      if (copy.type === "customText") {
        const ct = String(copy.customText || copy.text || "").trim();
        copy.customText = ct || "自定义文本";
        copy.text = copy.customText;
      }
      // timerRecordTime：旧 Web 误用 matchVarName
      if (copy.type === "timerRecordTime" && !copy.loopVarName && copy.matchVarName) {
        copy.loopVarName = copy.matchVarName;
      }
      // parseEscapes：缺省 0；旧 JSON 缺字段时按不解析
      if (copy.parseEscapes == null) copy.parseEscapes = 0;
      if (copy.matchThreshold != null) {
        let thr = Number(copy.matchThreshold);
        if (thr > 0 && thr <= 1) thr *= 100;
        if (!(thr > 0)) thr = 65;
        if (thr < 1) thr = 1;
        if (thr > 100) thr = 100;
        copy.matchThreshold = thr;
      }
      return copy;
    });
  }

  function defaultAction(type, remark) {
    const t = type || "wait";
    const base = {
      type: t,
      name: typeLabel(t),
      remark: remark || "",
      text: "",
      no: 0,
      indent: 0,
      x: 0,
      y: 0,
      randomX: 0,
      randomY: 0,
      button: "left",
      keyText: "A",
      keyVk: 65,
      clickCount: 1,
      duration: 0.05,
      randomDuration: 0,
      loopCount: 1,
      searchX1: 0,
      searchY1: 0,
      searchX2: 0,
      searchY2: 0,
      searchFullScreen: 1,
      imagePath: "",
      matchThreshold: 65,
      perfectMatch: 0,
      imageScale: 1,
      imageScaleMin: 1,
      imageScaleMax: 1,
      findImageFollowUp: 0,
      offsetX: 0,
      offsetY: 0,
      inputText: "",
      charInterval: 0.01,
      parseEscapes: 0,
      scrollSteps: 1,
      scrollDirection: 0,
      scrollVertical: 1,
      scrollHorizontal: 0,
    };
    if (t === "wait") base.duration = 0.5;
    if (t === "findImage") {
      base.duration = 0.1;
      base.searchFullScreen = 1;
      base.matchThreshold = 65;
      applyFullScreenSearchCoords(base);
    }
    if (t === "getColor") {
      base.matchVarName = "colorRet";
    }
    if (t === "findColor") {
      base.colorR = 255;
      base.colorG = 0;
      base.colorB = 0;
      base.colorTolerance = 16;
      base.searchFullScreen = 1;
      base.findImageFollowUp = 2;
      base.matchVarName = "colorRet";
      applyFullScreenSearchCoords(base);
    }
    if (t === "colorMatch") {
      base.colorR = 255;
      base.colorG = 0;
      base.colorB = 0;
      base.colorTolerance = 16;
      base.matchVarName = "colorRet";
    }
    if (t === "mouseClick") base.duration = 0.01;
    if (t === "keyClick") base.duration = 0.01;
    if (t === "mousePlayback") {
      base.blockName = "";
      base.targetPath = "";
      base.clickCount = 1;
      base.duration = 0.01;
      base.randomDuration = 0;
      base.playbackSpeed = 1;
      base.useMode = 3;
      base.breakoutTimeSeconds = 0;
      base.nestedWindowMode = {};
    }
    if (t === "runMacro") {
      base.blockName = "";
      base.targetPath = "";
      base.clickCount = 1;
      base.duration = 0.01;
      base.randomDuration = 0;
      base.useMode = 3;
      base.breakoutTimeSeconds = 0;
      base.nestedWindowMode = {};
    }
    if (t === "loop") base.loopCount = -1;
    if (t === "quickInput") {
      base.inputText = "hello";
      base.charInterval = 0.01;
      base.parseEscapes = 0;
    }
    if (t === "hotkeyShortcut") {
      base.keyText = "Ctrl+C";
      base.shortcutPreset = 0;
    }
    if (t === "defineBlock" || t === "runBlock") {
      base.blockName = "block1";
      if (t === "runBlock") {
        base.clickCount = 1;
        base.duration = 0.01;
        base.randomDuration = 0;
      }
    }
    if (t === "runProgram" || t === "closeProgram" || t === "openFile" || t === "activateWindow")
      base.targetPath = "";
    if (t === "activateWindow") base.targetPath = "Excel";
    if (t === "openWebpage") base.targetPath = "https://";
    if (t === "goto") base.gotoStepExpr = "1";
    if (t === "if") base.conditionExpr = "a == 1";
    if (t === "textRecognition") {
      base.searchFullScreen = 1;
      base.ocrResultMode = 0;
      base.imageRegionX1 = 0;
      base.imageRegionY1 = 0;
      base.imageRegionX2 = 0;
      base.imageRegionY2 = 0;
      applyFullScreenSearchCoords(base);
    }
    if (t === "aiTextAnalysis" || t === "aiImageAnalysis" || t === "aiActionExecute") {
      base.aiPrompt = "";
      base.aiOutputVarName = t === "aiImageAnalysis" ? "aiImgResult" : "aiResult";
      base.aiOutputType = 0;
      base.aiContextMode = 0;
      base.aiModelName = state.agentModel || "";
      base.aiTimeoutSec = 30;
      base.aiFallbackValue = "";
      base.aiImageScale = t === "aiActionExecute" ? 0.5 : 1;
      base.aiRegionByImage = 0;
      base.aiTargetImagePath = "";
      base.aiSearchX1 = 0;
      base.aiSearchY1 = 0;
      base.aiSearchX2 = 0;
      base.aiSearchY2 = 0;
      base.aiSearchRegion = 0;
      base.imageRegionX1 = 0;
      base.imageRegionY1 = 0;
      base.imageRegionX2 = 0;
      base.imageRegionY2 = 0;
      base.searchFullScreen = 1;
      applyFullScreenSearchCoords(base, "aiSearch");
      base.matchThreshold = 65;
      base.imageScaleMin = 0.9;
      base.imageScaleMax = 1.1;
      base.aiWithImage = t === "aiActionExecute" ? 0 : 1;
      if (t === "aiActionExecute") {
        base.aiLogicConvert = 0;
        base.aiLogicBlockName = "";
      }
    }
    if (t === "getCursorPos") base.matchVarName = "a";
    if (t === "timerRecordTime") base.loopVarName = "t";
    return base;
  }

  function openOv(id, stab) {
    // 专业模式：设置内嵌主界面，不弹遮罩层
    if (id === "settings" && state.uiMode === "pro" && window.ProMode) {
      if (typeof window.ProMode.openSettings === "function") {
        window.ProMode.openSettings(stab || "other");
        return;
      }
    }
    if (id === "agent") {
      const agentWasOpen =
        !!$("#ov-agent")?.classList.contains("show") && !state.agentMinimized;
      closeAllOv({ keepAgent: true });
      const ov = $("#ov-agent");
      if (ov) {
        ov.classList.add("show");
        ov.classList.remove("agent-min");
      }
      state.agentMinimized = false;
      const dock = $("#agentMinDock");
      if (dock) dock.hidden = true;
      if (!agentWasOpen && !state.agentTabs.length) {
        /* caller will create tabs via openAgentSession */
      }
      return;
    }
    if ($("#ov-agent")?.classList.contains("show") && !state.agentMinimized) {
      minimizeAgentWindow();
    }
    const wasOpt = !!$("#ov-opt")?.classList.contains("show");
    closeAllOv({ keepCurtain: id === "opt", keepAgent: true });
    const ov = document.getElementById("ov-" + id);
    if (!ov) return;
    ov.classList.add("show");
    if (id === "opt") {
      document.body.classList.add("opt-open");
      beginProgressiveLoad("opt");
      // 立刻切到优化窗尺寸（cloak 中），内容就绪后再 modeReady 揭开
      state._optModeReady = false;
      state._optContentReady = false;
      state._modeTransEndOn = "loadOpt";
      if (window.qst) qst.setMode("opt");
      else {
        state._optModeReady = true;
        endProgressiveLoad(true);
      }
    } else if (wasOpt || document.body.classList.contains("opt-open")) {
      document.body.classList.remove("opt-open");
      endProgressiveLoad(true);
      if (!state.editor && window.qst) {
        state._pendingUncloak = true;
        qst.setMode("home");
      }
    }
    if (id === "interval") {
      const inp = $("#ov-interval .inp");
      if (inp) {
        const v = Number(state.customInterval);
        inp.textContent = Number.isFinite(v) && v > 0 ? String(v) : "0.1";
      }
    }
    if (id === "settings") {
      if (stab) setSettingsTab(stab);
      syncUiModeRadios();
      if (window.qst) qst.openSettingsData();
    }
  }

  function tryRevealOptMode() {
    if (!state._optModeReady || !state._optContentReady) return;
    state._optModeReady = false;
    state._optContentReady = false;
    endModeTransition("loadOpt");
  }

  function closeAllOv(opts) {
    const keepCurtain = opts && opts.keepCurtain;
    const keepAgent = opts && opts.keepAgent;
    if (state.pendingKind === "askInput") {
      state._askInputOk = null;
      state._askInputAllowEmpty = false;
      state.pendingKind = null;
    }
    const closingHotkey = !!$("#ov-hotkey")?.classList.contains("show");
    const closingActionKey = !!$("#ov-action-key")?.classList.contains("show");
    const closingCrop = !!$("#ov-crop")?.classList.contains("show");
    const closingOpt = !!$("#ov-opt")?.classList.contains("show");
    const closingSettings = !!$("#ov-settings")?.classList.contains("show");
    $$(".overlay").forEach((o) => {
      if (keepAgent && o.id === "ov-agent") return;
      o.classList.remove("show");
      o.style.visibility = "";
      if (o.id === "ov-agent") o.classList.remove("agent-min");
    });
    hidePopup();
    state._nestedWmPick = false;
    requestAnimationFrame(syncTypingHotkeyMute);
    if (closingHotkey) stopWebHotkeyCapture();
    if (closingActionKey) stopActionKeyCapture();
    if (closingCrop) stopWebCropDrag();
    if (closingOpt || document.body.classList.contains("opt-open")) {
      document.body.classList.remove("opt-open");
      state._optModeReady = false;
      state._optContentReady = false;
      if (!keepCurtain) {
        endProgressiveLoad(true);
        if (!state.editor && window.qst) {
          state._pendingUncloak = true;
          qst.setMode("home");
        }
      }
    }
    // 极简：未保存关闭设置弹窗时还原模式单选草稿
    if (closingSettings) syncUiModeRadios();
  }

  function setSettingsTab(stab) {
    $$("#settingsTabs .st-tab").forEach((t) =>
      t.classList.toggle("active", t.dataset.stab === stab)
    );
    // 专业模式会把设置 dlg 挪出 #ov-settings，不能只查 overlay 下的 pane。
    // 勿动 #ov-editor-settings：否则会把编辑器设置页的 .active 清掉，内容 display:none。
    document.querySelectorAll(".set-pane[data-pane]").forEach((p) => {
      if (p.closest("#ov-editor-settings")) return;
      p.classList.toggle("active", p.dataset.pane === stab);
    });
  }

  function quietSaveSettings(partial) {
    if (!window.qst || partial == null) return;
    state._quietSaveSettings = true;
    qst.saveSettings(partial || {});
  }

  function showContentSkeleton(listId, n) {
    const list = $("#" + listId);
    if (!list) return;
    const count = Math.max(4, n | 0);
    list.innerHTML = Array.from({ length: count }, () => '<div class="skel-row" aria-hidden="true"></div>').join("");
  }

  /** 内容已写入后：清模糊，并通知壳层揭开 DWM cloak（无整窗蓝遮罩） */
  function revealModeAfterPaint() {
    endProgressiveLoad(true);
    document.body.classList.remove("editor-booting");
    requestAnimationFrame(() => {
      requestAnimationFrame(() => {
        if (window.qst && typeof qst.modeReady === "function") qst.modeReady();
        else if (window.qst && typeof qst.uncloak === "function") qst.uncloak();
      });
    });
  }

  function beginProgressiveLoad(kind) {
    endProgressiveLoad(true);
    state._progressiveKind = kind || "";
    state._progressiveToken = (state._progressiveToken | 0) + 1;
    document.body.classList.remove("progressive-editor", "progressive-opt");
    document.body.classList.add("progressive-load", "progressive-" + kind);
    state._actionListRendering = false;
    if (kind === "editor") {
      showContentSkeleton("actionList", 10);
      if ($("#edCount")) $("#edCount").textContent = "…";
      // 右栏立刻清晰显示固定「移动鼠标」预览（不参与列表模糊）
      state.addActionType = firstVisibleActionType();
      state.actionSel = -1;
      state.addPreview = null;
      state.editDraft = null;
      const typeCombo = $("#edActionType");
      if (typeCombo) {
        const cur =
          pickerActionTypes(state.addActionType).find((a) => a.v === state.addActionType) ||
          ACTION_TYPES[0];
        typeCombo.textContent = cur.t;
      }
      showAddTypePreview();
    } else if (kind === "opt") {
      showContentSkeleton("optList", 14);
      if ($("#optSelCount")) $("#optSelCount").textContent = "…";
      if ($("#optSelN")) $("#optSelN").textContent = "0";
    }
  }

  function endProgressiveLoad(/* silent */) {
    document.body.classList.remove(
      "progressive-load",
      "progressive-editor",
      "progressive-opt"
    );
    state._progressiveKind = "";
  }

  /** @deprecated 兼容旧调用：仅 editor/opt 走列表区模糊骨架 */
  function beginModeTransition(_msg, endOn) {
    state._modeTransEndOn = endOn || "";
    if (endOn === "openEditor") beginProgressiveLoad("editor");
    else if (endOn === "loadOpt") beginProgressiveLoad("opt");
  }

  function endModeTransition(reason) {
    const want = state._modeTransEndOn;
    if (reason !== "force" && want && reason !== want) return;
    state._modeTransEndOn = "";
    if (reason === "force") {
      endProgressiveLoad(true);
      document.body.classList.remove("editor-booting");
      if (window.qst && typeof qst.modeReady === "function") qst.modeReady();
      return;
    }
    revealModeAfterPaint();
  }

  function appendActionRowsChunked(listEl, htmlChunks, onDone) {
    if (!listEl) {
      if (onDone) onDone();
      return;
    }
    const token = state._progressiveToken | 0;
    let i = 0;
    const step = () => {
      if ((state._progressiveToken | 0) !== token) return;
      if (i >= htmlChunks.length) {
        if (onDone) onDone();
        return;
      }
      listEl.insertAdjacentHTML("beforeend", htmlChunks[i]);
      i += 1;
      if (i < htmlChunks.length) requestAnimationFrame(step);
      else if (onDone) onDone();
    };
    step();
  }

  function setTab(tab, opts) {
    const silent = opts && opts.silent;
    if (state.editor) {
      // silent：启动恢复 / 列表重匹配不得把刚打开的编辑器关掉
      if (silent) return;
      exitEditor();
    }
    const map = { clicker: 0, recorder: 1, macro: 2, ai: 3 };
    const engTab = map[tab] != null ? map[tab] : 0;
    const same = state.tab === tab;

    if (!silent) state._userPickedHomeTab = true;
    if (opts && opts.userPick) state._userPickedHomeTab = true;

    if (!same) {
      state.tab = tab;
      $$("#homeNav .tab").forEach((t) =>
        t.classList.toggle("active", t.dataset.tab === tab)
      );
      $$(".view").forEach((v) =>
        v.classList.toggle("active", v.id === "view-" + tab)
      );
    }

    // 引擎 Tab：仅在 Tab 变化或显式 resync 时推送，杜绝同 Tab 刷桥/写盘
    if (!same || (opts && opts.resync)) {
      if (window.qst && typeof qst.setActiveHomeTab === "function") {
        qst.setActiveHomeTab(engTab);
      }
      syncEngineHomeSelection(true);
      if (!silent) quietSaveSettings({ activeTab: engTab });
    }

    if (!silent || !same) updateCtas();
  }

  function normPathKey(p) {
    return String(p || "")
      .replace(/\//g, "\\")
      .replace(/\\+$/, "")
      .toLowerCase();
  }

  function pathEquals(a, b) {
    const ka = normPathKey(a);
    const kb = normPathKey(b);
    return !!ka && ka === kb;
  }

  function findByPath(list, path) {
    const key = normPathKey(path);
    if (!key) return -1;
    return (list || []).findIndex((it) => normPathKey(itemPath(it)) === key);
  }

  function rememberSelectionPaths() {
    const m = state.macroSel >= 0 ? state.macros[state.macroSel] : null;
    const r = state.recSel >= 0 ? state.recordings[state.recSel] : null;
    state._selMacroPath = m ? itemPath(m) : "";
    state._selRecPath = r ? itemPath(r) : "";
  }

  /** 列表刷新后按路径重匹配选中（FindFirstFile 顺序会变，不能靠 index） */
  function rematchListSelectionByPath() {
    if (state._selMacroPath) {
      const i = findByPath(state.macros, state._selMacroPath);
      state.macroSel = i;
      if (i < 0) state._selMacroPath = "";
    } else if (state.macroSel >= state.macros.length) {
      state.macroSel = -1;
    }
    if (state._selRecPath) {
      const i = findByPath(state.recordings, state._selRecPath);
      state.recSel = i;
      if (i < 0) state._selRecPath = "";
    } else if (state.recSel >= state.recordings.length) {
      state.recSel = -1;
    }
  }

  function selectionPersistFields() {
    const m = state.macroSel >= 0 ? state.macros[state.macroSel] : null;
    const r = state.recSel >= 0 ? state.recordings[state.recSel] : null;
    // 宏/录制互斥：有宏选中则清空录制路径（与引擎 SaveHomeState 一致）
    if (m) {
      return { selectedScriptPath: itemPath(m), selectedRecordingPath: "" };
    }
    if (r) {
      return { selectedScriptPath: "", selectedRecordingPath: itemPath(r) };
    }
    return { selectedScriptPath: "", selectedRecordingPath: "" };
  }

  function syncEngineHomeSelection(force) {
    // 启动恢复完成前勿用空选中覆盖引擎 RestoreHomeState 的结果
    if (!state._homeRestored) return;
    if (!window.qst) return;
    rememberSelectionPaths();
    let key = state.tab + "|";
    if (state.tab === "macro") {
      if (typeof qst.setHomeSelection !== "function") return;
      const m = state.macroSel >= 0 ? state.macros[state.macroSel] : null;
      const path = m ? itemPath(m) : "";
      key += "2|" + path;
      if (!force && syncEngineHomeSelection._lastKey === key) return;
      syncEngineHomeSelection._lastKey = key;
      qst.setHomeSelection(2, path);
    } else if (state.tab === "recorder") {
      if (typeof qst.setHomeSelection !== "function") return;
      const r = state.recSel >= 0 ? state.recordings[state.recSel] : null;
      const path = r ? itemPath(r) : "";
      key += "1|" + path;
      if (!force && syncEngineHomeSelection._lastKey === key) return;
      syncEngineHomeSelection._lastKey = key;
      qst.setHomeSelection(1, path);
    } else if (state.tab === "ai") {
      key += "3|";
      if (!force && syncEngineHomeSelection._lastKey === key) return;
      syncEngineHomeSelection._lastKey = key;
      // 清引擎脚本选中，避免热键仍走宏
      if (typeof qst.setHomeSelection === "function") qst.setHomeSelection(3, "");
      else if (typeof qst.setActiveHomeTab === "function") qst.setActiveHomeTab(3);
    } else {
      // clicker：切 Tab 并清空引擎选中
      key += "0|";
      if (!force && syncEngineHomeSelection._lastKey === key) return;
      syncEngineHomeSelection._lastKey = key;
      if (typeof qst.setHomeSelection === "function") qst.setHomeSelection(0, "");
      else if (typeof qst.setActiveHomeTab === "function") qst.setActiveHomeTab(0);
    }
  }

  /** 连点选中时合并同步，避免 UI 线程被连续 SaveHomeState 堵住 */
  function syncEngineHomeSelectionDebounced() {
    rememberSelectionPaths();
    clearTimeout(syncEngineHomeSelectionDebounced._t);
    syncEngineHomeSelectionDebounced._t = setTimeout(() => {
      syncEngineHomeSelectionDebounced._t = 0;
      syncEngineHomeSelection();
    }, 60);
  }

  /** 仅切换 selected class，避免整表 innerHTML 造成连点卡顿 */
  function updateListSelectionUi() {
    const macroEl = $("#macroList");
    if (macroEl) {
      $$(".card[data-kind='macro']", macroEl).forEach((card) => {
        card.classList.toggle("selected", (+card.dataset.i) === state.macroSel);
      });
    }
    const recEl = $("#recList");
    if (recEl) {
      $$(".card[data-kind='rec']", recEl).forEach((card) => {
        card.classList.toggle("selected", (+card.dataset.i) === state.recSel);
      });
    }
  }

  function selectPathInLists(path, preferTab) {
    const p = String(path || "");
    if (!p) return false;
    const mi = findByPath(state.macros, p);
    const ri = findByPath(state.recordings, p);
    if (preferTab === "recorder" || (preferTab !== "macro" && ri >= 0 && mi < 0)) {
      if (ri < 0) return false;
      state.recSel = ri;
      state.macroSel = -1;
      setTab("recorder", { silent: true });
      syncEngineHomeSelection();
      renderLists();
      updateCtas();
      return true;
    }
    if (mi >= 0) {
      state.macroSel = mi;
      state.recSel = -1;
      setTab("macro", { silent: true });
      syncEngineHomeSelection();
      renderLists();
      updateCtas();
      return true;
    }
    if (ri >= 0) {
      state.recSel = ri;
      state.macroSel = -1;
      setTab("recorder", { silent: true });
      syncEngineHomeSelection();
      renderLists();
      updateCtas();
      return true;
    }
    return false;
  }

  function applyHomeStateMsg(msg) {
    const tabMap = ["clicker", "recorder", "macro", "ai"];
    let tabIdx = msg.activeTab | 0;
    if (tabIdx < 0 || tabIdx > 3) tabIdx = 0;
    const scriptPath = msg.selectedScriptPath || "";
    const recPath = msg.selectedRecordingPath || "";
    state.macroSel = -1;
    state.recSel = -1;
    state._selMacroPath = "";
    state._selRecPath = "";
    // 与引擎互斥：优先录制路径，其次宏路径
    if (recPath) {
      const i = findByPath(state.recordings, recPath);
      if (i >= 0) {
        state.recSel = i;
        state._selRecPath = itemPath(state.recordings[i]);
      }
    }
    if (state.recSel < 0 && scriptPath) {
      const i = findByPath(state.macros, scriptPath);
      if (i >= 0) {
        state.macroSel = i;
        state._selMacroPath = itemPath(state.macros[i]);
      }
    }
    // 用户已手动切过主页 Tab（含 AI）时，勿用「有选中脚本→鼠标宏」盖掉
    if (!state._userPickedHomeTab) {
      if (state.macroSel >= 0 && tabIdx !== 3) tabIdx = 2;
      else if (state.recSel >= 0 && tabIdx !== 3) tabIdx = 1;
    } else {
      tabIdx = ({ clicker: 0, recorder: 1, macro: 2, ai: 3 }[state.tab] != null
        ? { clicker: 0, recorder: 1, macro: 2, ai: 3 }[state.tab]
        : tabIdx);
    }
    // 先标记已恢复，再 setTab→sync，避免被「恢复前禁同步」挡住
    state._homeRestored = true;
    if (!state.editor) {
      setTab(tabMap[tabIdx] || "clicker", { silent: true });
    }
    renderLists();
    updateCtas();
    // 专业模式脚本库等页有自己的热键分流；勿让恢复的 Recorder 空选中把 F8 变成开录
    if (window.ProMode && typeof window.ProMode.resyncEngine === "function"
        && window.ProMode.mode && window.ProMode.mode() === "pro") {
      window.ProMode.resyncEngine();
    }
  }

  function maybeRestoreHomeSelection() {
    if (state._homeRestored || state._homeRestoreRequested) return;
    if (!state._gotScripts || !state._gotRecordings || !state._gotSettings) return;
    state._homeRestoreRequested = true;
    if (window.qst && typeof qst.getHomeState === "function") {
      qst.getHomeState();
      return;
    }
    // 无 bridge 时回退 settings.home
    const home = (state.settings && state.settings.home) || {};
    applyHomeStateMsg({
      activeTab: home.activeTab | 0,
      selectedScriptPath: home.selectedScriptPath || "",
      selectedRecordingPath: home.selectedRecordingPath || "",
    });
  }

  function syncRecModeUi() {
    const el = $("#recMode");
    if (el) {
      const m = REC_MODES.find((x) => x.mode === state.recMode) || REC_MODES[0];
      el.textContent = m.t + " ▾";
      el.title = (state.recMode | 0) === 3
        ? "图片定位：点击前截模板，自动转找图"
        : m.t;
    }
    const hint = $("#recModeHint");
    if (hint) hint.hidden = (state.recMode | 0) !== 3;
    const wmEl = $("#recWindowMode");
    if (wmEl) wmEl.textContent = (state.recWindowMode | 0) === 1 ? "窗口模式 ▾" : "全屏模式 ▾";
    const wmHint = $("#recWindowHint");
    if (wmHint) wmHint.hidden = (state.recWindowMode | 0) !== 1;
  }

  function syncTitleBar() {
    const titleEl = $("#titleText");
    if (!titleEl) return;
    if (state.editor) {
      titleEl.innerHTML = `${PRODUCT_NAME} <em>— 宏编辑器</em>`;
    } else {
      titleEl.textContent = PRODUCT_NAME;
    }
    syncSettingsGear();
  }

  function syncSettingsGear() {
    const btn = $("#btnSettings");
    if (!btn) return;
    const inEditor = !!state.editor;
    const pro = (window.ProMode ? window.ProMode.mode() : state.uiMode) === "pro";
    btn.hidden = pro && !inEditor;
    btn.title = inEditor ? "鼠标宏编辑设置" : "设置";
    btn.setAttribute("aria-label", btn.title);
  }

  /** 极简 / 专业模式切换：只切壳与可见区域，两套交互逻辑互不触碰 */
  let _homeCenteredOnce = false;
  function syncUiModeRadios() {
    const cur = window.ProMode ? window.ProMode.mode() : state.uiMode || "simple";
    state.uiMode = cur;
    $$("#uiModeRadios .radio").forEach((r) => {
      r.classList.toggle("on", r.dataset.mode === cur);
    });
  }
  function readUiModeDraft() {
    const on = document.querySelector("#uiModeRadios .radio.on");
    return on && on.dataset.mode === "pro" ? "pro" : "simple";
  }
  /** 将设置里选中的模式落到界面（仅「保存」调用） */
  function commitUiModeFromDraft() {
    const m = readUiModeDraft();
    const cur = window.ProMode ? window.ProMode.mode() : state.uiMode;
    if (m === cur) {
      syncUiModeRadios();
      return false;
    }
    state.uiMode = m;
    if (window.ProMode) window.ProMode.setMode(m);
    applyUiMode({ center: false });
    return true;
  }
  function applyUiMode(opts) {
    if (window.ProMode) state.uiMode = window.ProMode.mode();
    const pro = state.uiMode === "pro";
    const root = $("#proHome");
    if (root) root.hidden = !pro;
    const nav = $("#homeNav");
    const views = $("#views");
    if (nav) nav.style.display = pro ? "none" : "";
    if (views) views.style.display = pro ? "none" : "";
    document.documentElement.classList.toggle("pro-ui", !!pro);
    syncSettingsGear();
    // 仅首次打开主界面居中；之后（含模式切换）保持原位
    const center =
      opts && Object.prototype.hasOwnProperty.call(opts, "center")
        ? !!opts.center
        : !_homeCenteredOnce;
    if (window.qst && typeof qst.setHomeSize === "function") {
      qst.setHomeSize(SHELL_CLIENT.home.w, SHELL_CLIENT.home.h, { center });
      if (center) _homeCenteredOnce = true;
    }
    if (pro && window.ProMode) window.ProMode.sync();
    else if (window.ProMode) window.ProMode.hide();
    // 专业模式会跳过列表渲染，骨架卡会残留；切回极简后必须强制刷列表（含文件夹内脚本）
    if (!pro) {
      renderLists();
      if (window.qst) {
        if (!state._gotScripts) qst.listScripts();
        if (!state._gotRecordings) qst.listRecordings();
        qst.listAgentConversations();
      }
    }
    syncUiModeRadios();
    // 只由主壳 + ProMode 写回 uiMode。助手窗没有 pro-mode.js，state 默认 simple，
    // 若这里静默保存会把专业模式冲成极简，录制倍速勾选失效。
    if (
      !AGENT_SHELL &&
      window.ProMode &&
      window.qst &&
      typeof quietSaveSettings === "function" &&
      state._gotSettings
    ) {
      quietSaveSettings({ uiMode: pro ? "pro" : "simple" });
    }
  }

  function hotkeyShort(text) {
    return String(text || "F8").replace(/（长按）$/, "").trim() || "F8";
  }

  /** 启停热键展示：鼠标左键不写「（长按）」，逻辑仍用 hotkeyHold */
  function formatHotkeyDisplay(text, hold) {
    const base = String(text || "")
      .replace(/（长按）$/, "")
      .trim();
    if (!base) return hold ? "（捕获中…）" : "无";
    if (base === "鼠标左键") return base;
    if (hold) return base + "（长按）";
    return base;
  }

  function isVisibleTypingEl(el) {
    if (!el || el.nodeType !== 1) return false;
    if (el.closest && el.closest(".overlay") && !el.closest(".overlay.show")) return false;
    if (!(el.offsetWidth || el.offsetHeight || (el.getClientRects && el.getClientRects().length))) {
      return false;
    }
    return true;
  }

  function isEditableTypingTarget(el) {
    if (!el) return false;
    if (el.nodeType !== 1) el = el.parentElement;
    if (!el || !isVisibleTypingEl(el)) return false;
    if (el.isContentEditable) return true;
    const host =
      el.closest && el.closest("[contenteditable='true'], [contenteditable='']");
    if (host && isVisibleTypingEl(host)) return true;
    const tag = (el.tagName || "").toLowerCase();
    if (tag === "textarea") return !(el.disabled || el.readOnly);
    if (tag !== "input") return false;
    if (el.disabled || el.readOnly) return false;
    const type = (el.getAttribute("type") || "text").toLowerCase();
    return !/^(button|checkbox|radio|file|hidden|submit|reset|range|color|image)$/.test(
      type
    );
  }

  let _typingMuteSent = null;
  function syncTypingHotkeyMute() {
    const typing =
      !document.hidden &&
      document.hasFocus() &&
      isEditableTypingTarget(document.activeElement);
    if (_typingMuteSent === typing) return;
    _typingMuteSent = typing;
    if (window.qst && typeof qst.setTypingHotkeysMuted === "function") {
      qst.setTypingHotkeysMuted(typing ? 1 : 0);
    }
  }

  function bindTypingHotkeyMute() {
    document.addEventListener("focusin", syncTypingHotkeyMute, true);
    document.addEventListener(
      "focusout",
      () => {
        requestAnimationFrame(syncTypingHotkeyMute);
      },
      true
    );
    document.addEventListener(
      "pointerdown",
      (e) => {
        const t = e.target;
        const el =
          t && t.nodeType === 1
            ? t.closest("input, textarea, [contenteditable='true'], [contenteditable='']")
            : null;
        if (!el || !isEditableTypingTarget(el)) return;
        if (_typingMuteSent === true) return;
        _typingMuteSent = true;
        if (window.qst && typeof qst.setTypingHotkeysMuted === "function") {
          qst.setTypingHotkeysMuted(1);
        }
      },
      true
    );
    window.addEventListener("blur", syncTypingHotkeyMute);
    window.addEventListener("focus", syncTypingHotkeyMute);
    document.addEventListener("visibilitychange", syncTypingHotkeyMute);
    syncTypingHotkeyMute();
  }

  function updateCtas() {
    const btn = BTN_LABEL[state.clickBtn] || "左键";
    const run = state.clicking;
    const hk = state.hotkey || "F8";
    const hkShort = hotkeyShort(hk);
    const title = $("#ctaClickerTitle");
    const sub = $("#ctaClickerSub");
    if (title) {
      title.textContent = run
        ? `按 ${hk} 停止连点（当前${btn}）`
        : `按 ${hk} 开始${btn}连点`;
    }
    if (sub) {
      sub.textContent = `${btn} · ${state.intervalLabel} · 热键 ${hk}`;
    }
    const ready = $("#clickerReady");
    if (ready) {
      ready.textContent = run ? "运行中" : "就绪";
      ready.style.background = run ? "var(--amber-soft)" : "var(--mint-200)";
      ready.style.color = run ? "#92400e" : "#0f766e";
    }
    const pill = $("#statusPill");
    if (pill) {
      const hideTip = !!state.hideBottomRightTip;
      if (hideTip) {
        pill.classList.remove("show");
      } else if (state.breakoutPaused) {
        pill.textContent = "脱离中…";
        pill.classList.add("show");
      } else if (state.recording) {
        pill.textContent = "录制中…";
        pill.classList.add("show");
      } else if (state.macroRunning) {
        pill.textContent = "宏运行中…";
        pill.classList.add("show");
      } else if (run) {
        pill.textContent = "连点中…";
        pill.classList.add("show");
      } else {
        pill.classList.remove("show");
      }
    }
    const hud = $("#runHud");
    if (hud) {
      if (state.macroRunning) {
        hud.classList.add("show");
        const title = $("#runHudTitle");
        const meta = $("#runHudMeta");
        const mac = state.macroSel >= 0 ? state.macros[state.macroSel] : null;
        const name =
          state._runningMacroName ||
          (mac && mac.name) ||
          "宏";
        if (title) title.textContent = state.breakoutPaused ? "脱离中…" : "宏运行中";
        if (meta) {
          const runMode = state._runningMode | 0;
          const mode =
            runMode === 2
              ? "后台窗口"
              : runMode === 1
                ? "窗口模式"
                : "默认";
          const steps = state._executedSteps | 0;
          const rwm = state._runningWindowMode || {};
          const target =
            rwm.windowName ||
            rwm.windowTitle ||
            rwm.windowClassName ||
            rwm.targetExePath ||
            "";
          const pauseHint = state.breakoutPaused ? " · 脱离暂停" : "";
          const extra = target ? `<br>目标：${esc(String(target))}` : "";
          const hudBase = `${esc(name)}<br>模式：${mode}${pauseHint}`;
          meta.dataset.hudBase = hudBase;
          meta.dataset.hudExtra = extra;
          meta.innerHTML = `${hudBase} · 已执行 ${steps} 步${extra}`;
        }
      } else {
        hud.classList.remove("show");
        state._executedSteps = 0;
        state._runningMacroName = "";
        state._runningMode = 0;
      }
      const prevBtn = $("#btnRunPreview");
      if (prevBtn) {
        const wmOn = (state._runningMode | 0) > 0;
        prevBtn.style.display =
          state.macroRunning && wmOn ? "" : "none";
      }
    }

    const mac = state.macroSel >= 0 ? state.macros[state.macroSel] : null;
    const ctaMacroTitle = $("#ctaMacroTitle");
    const ctaMacroSub = $("#ctaMacroSub");
    const ctaMacroKey = $("#ctaMacroKey");
    if (!mac) {
      if (ctaMacroTitle) ctaMacroTitle.textContent = "点击创建鼠标宏";
      if (ctaMacroSub) ctaMacroSub.textContent = "打开编辑器新建脚本";
      if (ctaMacroKey) ctaMacroKey.hidden = true;
    } else {
      if (ctaMacroTitle)
        ctaMacroTitle.textContent = state.macroRunning
          ? "按热键停止运行宏"
          : "按热键开始运行宏";
      if (ctaMacroSub) {
        const own = mac.hotkey ? String(mac.hotkey) : "";
        ctaMacroSub.textContent = own
          ? `「${mac.name}」· 专属 ${own}`
          : `「${mac.name}」· 未设专属热键`;
      }
      // 底栏键位 = 全局启停（点它改全局）。专属热键只在卡片上改，避免改了 F12 却仍显示脚本的 →。
      if (ctaMacroKey) {
        ctaMacroKey.hidden = false;
        ctaMacroKey.textContent = hk;
        ctaMacroKey.title = "设置全局启停热键（选中后按此键运行该脚本）";
      }
    }

    const rec = state.recSel >= 0 ? state.recordings[state.recSel] : null;
    const ctaRecTitle = $("#ctaRecTitle");
    const ctaRecSub = $("#ctaRecSub");
    const ctaRecKey = $("#ctaRecKey");
    if (state.recording) {
      if (ctaRecTitle) ctaRecTitle.textContent = `按 ${hk} 停止录制`;
      if (ctaRecSub) ctaRecSub.textContent = "录制进行中…";
      if (ctaRecKey) {
        ctaRecKey.hidden = false;
        ctaRecKey.textContent = hkShort;
        ctaRecKey.title = "设置启停热键";
      }
    } else if (!rec) {
      if (ctaRecTitle) ctaRecTitle.textContent = `按 ${hk} 开始录制`;
      if (ctaRecSub) ctaRecSub.textContent = "未选中录制时开始新录制";
      if (ctaRecKey) ctaRecKey.hidden = true;
    } else {
      if (ctaRecTitle) ctaRecTitle.textContent = `按 ${hk} 开始回放`;
      if (ctaRecSub) {
        const own = rec.hotkey ? String(rec.hotkey) : "";
        ctaRecSub.textContent = own
          ? `已选中「${rec.name}」· 专属 ${own}`
          : `已选中「${rec.name}」`;
      }
      if (ctaRecKey) {
        ctaRecKey.hidden = false;
        ctaRecKey.textContent = hkShort;
        ctaRecKey.title = "设置全局启停热键";
      }
    }
    if ($("#ctaClickerKey")) {
      const ck = $("#ctaClickerKey");
      ck.textContent = hkShort;
      ck.title = "设置启停热键";
    }
    syncRecModeUi();
  }

  function cardHtml(item, sel, kind, i) {
    const hkOk = !!(item.hotkeyEnabled || item.hotkeyVk) && item.hotkey;
    const hk = hkOk
      ? `<span class="hotkey accent" data-act="hotkey">${esc(item.hotkey)}</span>`
      : `<span class="hotkey unset" data-act="hotkey">设热键</span>`;
    const acts =
      kind === "rec"
        ? `<span data-act="opt">优化</span><span data-act="rename">重命名</span><span data-act="del">删除</span>`
        : `<span data-act="edit">编辑</span><span data-act="rename">重命名</span><span data-act="del">删除</span>`;
    const folder = normListFolder(item.folder);
    const baseMeta =
      item.meta ||
      (item.actionCount != null
        ? `${item.actionCount} 动作 · ${item.recordTime || ""}`
        : item.recordTime || "");
    const meta = [folder ? "📁 " + folder : "", baseMeta].filter(Boolean).join(" · ");
    const titleName = String(item.name || "").replace(/\.json$/i, "");
    return `<div class="card ${sel ? "selected" : ""}" data-kind="${kind}" data-i="${i}" data-id="${esc(item.id || item.path || "")}">
      <div class="rail"></div>
      <div class="card-main"><div class="card-title-row"><div class="card-title">${esc(titleName)}</div>${hk}</div>
        <div class="card-meta">${esc(meta)}</div></div>
      <div class="card-side"><span class="sel-tag">已选中 ✓</span><div class="card-actions">${acts}</div></div>
    </div>`;
  }

  function normListFolder(f) {
    return String(f || "")
      .replace(/\\/g, "/")
      .replace(/^\/+|\/+$/g, "");
  }
  function folderHeaderHtml(kind, folder, count, open) {
    const name = folder.indexOf("/") >= 0 ? folder.slice(folder.lastIndexOf("/") + 1) : folder;
    return `<div class="list-folder ${open ? "open" : ""}" data-folder-kind="${kind}" data-folder="${esc(folder)}">
      <span class="lf-twist">${open ? "▼" : "▶"}</span>
      <span class="lf-name">${esc(name)}</span>
      <span class="lf-count">${count}</span>
    </div>`;
  }
  function renderGroupedCards(el, kind, items, selectedIdx, emptyHtml, cardFn) {
    if (!el) return;
    if (!items.length) {
      el.innerHTML = emptyHtml;
      return;
    }
    if (!state._listFolderOpen) state._listFolderOpen = {};
    const openMap = state._listFolderOpen[kind] || (state._listFolderOpen[kind] = {});
    const groups = {};
    const roots = [];
    items.forEach((it, i) => {
      const f = normListFolder(it.folder);
      if (!f) roots.push({ it, i });
      else {
        if (!groups[f]) groups[f] = [];
        groups[f].push({ it, i });
      }
    });
    let html = "";
    Object.keys(groups)
      .sort()
      .forEach((f) => {
        const open = openMap[f] !== false;
        html += folderHeaderHtml(kind, f, groups[f].length, open);
        if (open) {
          groups[f].forEach(({ it, i }) => {
            html += cardFn(it, i === selectedIdx, i);
          });
        }
      });
    roots.forEach(({ it, i }) => {
      html += cardFn(it, i === selectedIdx, i);
    });
    el.innerHTML = html;
  }
  function renderMacroList() {
    if (state.uiMode === "pro" || document.documentElement.classList.contains("pro-ui")) return;
    const macroEl = $("#macroList");
    if (!macroEl) return;
    const macroScroll = macroEl.scrollTop;
    renderGroupedCards(
      macroEl,
      "macro",
      state.macros || [],
      state.macroSel,
      `<div class="empty"><div class="ico">⌀</div><p>暂无鼠标宏，点击下方创建</p></div>`,
      (m, sel, i) => cardHtml(m, sel, "macro", i)
    );
    const want = state._macroScrollOffset != null ? state._macroScrollOffset : macroScroll;
    macroEl.scrollTop = want | 0;
    state._macroScrollOffset = macroEl.scrollTop;
  }

  function renderRecList() {
    if (state.uiMode === "pro" || document.documentElement.classList.contains("pro-ui")) return;
    const recEl = $("#recList");
    if (!recEl) return;
    const recScroll = recEl.scrollTop;
    renderGroupedCards(
      recEl,
      "rec",
      state.recordings || [],
      state.recSel,
      `<div class="empty"><div class="ico">⌀</div><p>没有已录制记录，按下面的提示开始录制吧</p></div>`,
      (r, sel, i) => cardHtml(r, sel, "rec", i)
    );
    const want = state._recScrollOffset != null ? state._recScrollOffset : recScroll;
    recEl.scrollTop = want | 0;
    state._recScrollOffset = recEl.scrollTop;
  }

  function renderAiList() {
    if (state.uiMode === "pro" || document.documentElement.classList.contains("pro-ui")) return;
    const aiEl = $("#aiList");
    if (!aiEl) return;
    const aiScroll = aiEl.scrollTop;
    renderGroupedCards(
      aiEl,
      "ai",
      state.chats || [],
      -1,
      `<div class="empty"><div class="ico">✦</div><p>暂无对话，点击下方开始</p></div>`,
      (c, _sel, i) => {
        const folder = normListFolder(c.folder);
        const meta = [folder ? "📁 " + folder : "", c.meta || ""].filter(Boolean).join(" · ");
        return `<div class="card card-ai" data-kind="ai" data-i="${i}" title="打开对话">
        <div class="rail"></div>
        <div class="card-main"><div class="card-title">${esc(c.name)}</div><div class="card-meta">${esc(meta)}</div></div>
        <div class="card-side">
          <div class="card-actions"><span data-act="del">删除</span></div>
        </div>
      </div>`;
      }
    );
    const want = state._aiScrollOffset != null ? state._aiScrollOffset : aiScroll;
    aiEl.scrollTop = want | 0;
    state._aiScrollOffset = aiEl.scrollTop;
  }

  function renderLists() {
    // 专业模式隐藏极简列表：跳过整表 innerHTML，减轻卡顿
    const pro =
      state.uiMode === "pro" || document.documentElement.classList.contains("pro-ui");
    if (!pro) {
      renderMacroList();
      renderRecList();
      renderAiList();
    }
  }

  function persistListScrollOffsets() {
    const macroEl = $("#macroList");
    const recEl = $("#recList");
    const aiEl = $("#aiList");
    if (macroEl) state._macroScrollOffset = macroEl.scrollTop | 0;
    if (recEl) state._recScrollOffset = recEl.scrollTop | 0;
    if (aiEl) state._aiScrollOffset = aiEl.scrollTop | 0;
    if (!window.qst) return;
    quietSaveSettings({
      clickerScrollOffset: state._clickerScrollOffset | 0,
      macroScrollOffset: state._macroScrollOffset | 0,
      recorderScrollOffset: state._recScrollOffset | 0,
      scriptCustomScrollOffset: state._aiScrollOffset | 0,
    });
  }

  function applyClickerStatus(st) {
    if (!st) return;
    state.clicking = !!st.running;
    if (typeof st.button === "number") state.clickBtn = st.button;
    if (typeof st.intervalMode === "number") state.intervalMode = st.intervalMode;
    if (typeof st.customInterval === "number") state.customInterval = st.customInterval;
    if (st.intervalLabel) state.intervalLabel = st.intervalLabel;
    syncClickerControls();
    updateCtas();
  }

  function syncClickerControls() {
    $$("#clickBtns .radio").forEach((r, idx) => {
      r.classList.toggle("on", idx === state.clickBtn);
    });
    const combo = $("#comboInterval");
    if (combo) combo.textContent = state.intervalLabel;
    const hk = $("#comboHotkey");
    if (hk) hk.textContent = state.hotkey;
  }

  function setChk(el, on) {
    if (!el) return;
    el.classList.toggle("on", !!on);
    // √ 由 CSS .chk.on i::after 绘制；清空 i 文本避免叠字
    const i = el.querySelector("i");
    if (i) i.textContent = "";
  }

  function syncWmInjectUi() {
    const on = !!$("#setWmEnableInject")?.classList.contains("on");
    const tech = $("#setWmInjectTech");
    if (tech) {
      tech.style.opacity = on ? "" : "0.45";
      tech.style.pointerEvents = on ? "" : "none";
    }
  }

  const PLAYBACK_SPEED_NODES = [0.25, 0.5, 0.75, 1, 1.25, 2, 4];
  const PLAYBACK_SPEED_SNAP = 0.035;

  function clampPlaybackSpeedUi(s) {
    const n = Number(s);
    if (!Number.isFinite(n)) return 1;
    if (n < 0.25) return 0.25;
    if (n > 4) return 4;
    return n;
  }

  function playbackSpeedToPos(speed) {
    const nodes = PLAYBACK_SPEED_NODES;
    const s = clampPlaybackSpeedUi(speed);
    const last = nodes.length - 1;
    for (let i = 0; i < last; i++) {
      if (s <= nodes[i + 1] || i === last - 1) {
        const span = nodes[i + 1] - nodes[i];
        const t = span <= 0 ? 0 : (s - nodes[i]) / span;
        return (i + Math.min(1, Math.max(0, t))) / last;
      }
    }
    return 0.5;
  }

  function playbackPosToSpeed(pos, snap) {
    let p = pos;
    if (!Number.isFinite(p)) p = 0.5;
    if (p < 0) p = 0;
    if (p > 1) p = 1;
    const nodes = PLAYBACK_SPEED_NODES;
    const last = nodes.length - 1;
    if (snap) {
      let best = 0;
      let bestD = 1;
      for (let i = 0; i <= last; i++) {
        const d = Math.abs(p - i / last);
        if (d < bestD) {
          bestD = d;
          best = i;
        }
      }
      if (bestD <= PLAYBACK_SPEED_SNAP) return nodes[best];
    }
    const x = p * last;
    const i = Math.min(last - 1, Math.floor(x));
    const t = x - i;
    return nodes[i] + t * (nodes[i + 1] - nodes[i]);
  }

  function formatPlaybackSpeed(s) {
    const v = clampPlaybackSpeedUi(s);
    const rounded = Math.round(v * 1000) / 1000;
    if (Math.abs(rounded - Math.round(rounded)) < 1e-6) return String(Math.round(rounded)) + "x";
    return String(rounded) + "x";
  }

  function speedSliderInnerHtml(id, scope, speed) {
    const s = clampPlaybackSpeedUi(speed == null ? 1 : Number(speed));
    return (
      `<div class="speed-slider" id="${esc(id)}" role="slider" aria-label="回放倍速" aria-valuemin="0.25" aria-valuemax="4" aria-valuenow="${s}" tabindex="0" data-speed="${s}" data-speed-scope="${esc(scope)}">` +
      `<div class="speed-slider-track">` +
      `<div class="speed-slider-fill"></div>` +
      `<div class="speed-slider-ticks"><span></span><span></span><span></span><span></span><span></span><span></span><span></span></div>` +
      `<div class="speed-slider-thumb"></div>` +
      `</div></div>`
    );
  }

  function applySpeedToSliderEl(slider, valEl, speed) {
    const s = clampPlaybackSpeedUi(speed);
    const pos = playbackSpeedToPos(s);
    if (slider) {
      slider.dataset.speed = String(s);
      slider.setAttribute("aria-valuenow", String(Math.round(s * 1000) / 1000));
      const fill = slider.querySelector(".speed-slider-fill");
      const thumb = slider.querySelector(".speed-slider-thumb");
      const pct = (pos * 100).toFixed(4) + "%";
      if (fill) fill.style.width = pct;
      if (thumb) thumb.style.left = pct;
    }
    if (valEl) valEl.textContent = formatPlaybackSpeed(s);
  }

  function setPlaybackSpeedValue(speed) {
    const s = clampPlaybackSpeedUi(speed);
    $$('.speed-slider[data-speed-scope="settings"]').forEach((slider) => {
      const wrap = slider.closest(".rec-speed-wrap, .row.speed-row") || slider.parentElement;
      const val = wrap && wrap.querySelector(".speed-val");
      applySpeedToSliderEl(slider, val, s);
    });
  }

  function readPlaybackSpeedValue() {
    const el =
      document.querySelector('.speed-slider[data-speed-scope="settings"][data-speed]') ||
      $("#setPlaySpeedSlider") ||
      $("#homeRecSpeedSlider");
    const raw = el?.dataset.speed;
    return clampPlaybackSpeedUi(raw == null ? 1 : parseFloat(raw));
  }

  function persistPlaybackSpeedSettings() {
    if (!window.qst || typeof quietSaveSettings !== "function") return;
    quietSaveSettings({
      playbackSpeed: readPlaybackSpeedValue(),
      enablePlaybackSpeed: !!$("#setPlaySpeedEn")?.classList.contains("on"),
    });
  }

  function bindSpeedSliderRoot(root, opts) {
    if (!root || root._speedBound) return;
    root._speedBound = true;
    const track = root.querySelector(".speed-slider-track") || root;
    let dragging = false;
    const readVal = opts.readValue || readPlaybackSpeedValue;
    const writeVal = opts.setValue || setPlaybackSpeedValue;

    function posFromEvent(ev) {
      const r = track.getBoundingClientRect();
      if (r.width <= 0) return 0.5;
      return (ev.clientX - r.left) / r.width;
    }
    function applyFromEvent(ev, snap) {
      writeVal(playbackPosToSpeed(posFromEvent(ev), snap));
    }

    root.addEventListener("pointerdown", (ev) => {
      if (ev.button != null && ev.button !== 0) return;
      ev.preventDefault();
      dragging = true;
      try {
        root.setPointerCapture(ev.pointerId);
      } catch (e) {}
      applyFromEvent(ev, true);
    });
    root.addEventListener("pointermove", (ev) => {
      if (!dragging) return;
      applyFromEvent(ev, true);
    });
    const endDrag = (ev) => {
      if (!dragging) return;
      dragging = false;
      applyFromEvent(ev, true);
      if (typeof opts.onCommit === "function") opts.onCommit();
    };
    root.addEventListener("pointerup", endDrag);
    root.addEventListener("pointercancel", endDrag);
    root.addEventListener("keydown", (ev) => {
      const cur = readVal();
      const last = PLAYBACK_SPEED_NODES.length - 1;
      const pos = playbackSpeedToPos(cur);
      const step = 1 / last / 8;
      let next = null;
      if (ev.key === "ArrowLeft" || ev.key === "ArrowDown") {
        ev.preventDefault();
        next = playbackPosToSpeed(pos - step, true);
      } else if (ev.key === "ArrowRight" || ev.key === "ArrowUp") {
        ev.preventDefault();
        next = playbackPosToSpeed(pos + step, true);
      } else if (ev.key === "Home") {
        ev.preventDefault();
        next = 0.25;
      } else if (ev.key === "End") {
        ev.preventDefault();
        next = 4;
      }
      if (next == null) return;
      writeVal(next);
      if (typeof opts.onCommit === "function") opts.onCommit();
    });
    writeVal(readVal());
  }

  function bindPlaybackSpeedSlider() {
    $$('.speed-slider[data-speed-scope="settings"]').forEach((root) => {
      bindSpeedSliderRoot(root, {
        setValue: setPlaybackSpeedValue,
        readValue: readPlaybackSpeedValue,
        onCommit: persistPlaybackSpeedSettings,
      });
    });
  }

  function otherFlag(other, key, defaultOn) {
    if (!other || other[key] == null) return !!defaultOn;
    return !!other[key];
  }

  function editorDefaultViewOf(other) {
    return other && other.editorDefaultView === "visual" ? "visual" : "code";
  }

  function editorVisualFromState() {
    const other = (state.settings && state.settings.other) || {};
    return {
      editorDefaultView: editorDefaultViewOf(other),
      visualLoopWrap: otherFlag(other, "visualLoopWrap", true),
      visualBlockCallWires: otherFlag(other, "visualBlockCallWires", true),
      visualIfWrap: otherFlag(other, "visualIfWrap", true),
      visualBlockWrap: otherFlag(other, "visualBlockWrap", true),
      visualJumpWires: otherFlag(other, "visualJumpWires", true),
      visualShowGrid: otherFlag(other, "visualShowGrid", true),
      visualShowCardId: otherFlag(other, "visualShowCardId", true),
      editorActionOrder: Array.isArray(other.editorActionOrder) ? other.editorActionOrder.slice() : [],
      editorHiddenActions: Array.isArray(other.editorHiddenActions)
        ? other.editorHiddenActions.slice()
        : [],
    };
  }

  function setEditorSettingsTab(tab) {
    const t = tab === "general" ? "general" : "visual";
    $$("#editorSettingsTabs .st-tab").forEach((el) =>
      el.classList.toggle("active", el.dataset.edtab === t)
    );
    $$("#ov-editor-settings .ed-set-pane").forEach((p) =>
      p.classList.toggle("active", p.dataset.edpane === t)
    );
  }

  function renderEditorActionCatalog(other) {
    const host = $("#edActionCatalog");
    if (!host) return;
    const { order, hidden } = normalizeActionCatalog(other);
    const hiddenSet = new Set(hidden);
    const byV = knownActionTypeMap();
    host.innerHTML = order
      .map((v) => {
        const a = byV.get(v);
        if (!a) return "";
        const on = !hiddenSet.has(v);
        return (
          `<div class="ed-cat-row${on ? "" : " is-hidden-type"}" data-type="${esc(v)}" draggable="true">` +
          `<span class="ed-cat-handle" title="拖动排序">⋮⋮</span>` +
          `<span class="chk${on ? " on" : ""}" data-ed-cat-vis draggable="false"><i></i></span>` +
          `<span class="ed-cat-name">${esc(a.t)}</span>` +
          `<span class="ed-cat-btns">` +
          `<button type="button" class="btn ghost" data-ed-cat-up title="上移" draggable="false">▲</button>` +
          `<button type="button" class="btn ghost" data-ed-cat-dn title="下移" draggable="false">▼</button>` +
          `</span></div>`
        );
      })
      .join("");
  }

  function collectEditorCatalogPartial() {
    const rows = $$("#edActionCatalog .ed-cat-row");
    if (!rows.length) {
      const other = (state.settings && state.settings.other) || {};
      return {
        editorActionOrder: Array.isArray(other.editorActionOrder) ? other.editorActionOrder.slice() : [],
        editorHiddenActions: Array.isArray(other.editorHiddenActions)
          ? other.editorHiddenActions.slice()
          : [],
      };
    }
    const order = [];
    const hidden = [];
    rows.forEach((row) => {
      const v = row.dataset.type;
      if (!v) return;
      order.push(v);
      if (!row.querySelector(".chk")?.classList.contains("on")) hidden.push(v);
    });
    if (hidden.length >= order.length && order.length) {
      const idx = hidden.indexOf(order[0]);
      if (idx >= 0) hidden.splice(idx, 1);
    }
    return { editorActionOrder: order, editorHiddenActions: hidden };
  }

  function bindEditorActionCatalog() {
    const host = $("#edActionCatalog");
    if (!host || host._boundCat) return;
    host._boundCat = true;
    let dragEl = null;
    host.addEventListener("click", (e) => {
      const row = e.target.closest(".ed-cat-row");
      if (!row || !host.contains(row)) return;
      const up = e.target.closest("[data-ed-cat-up]");
      const dn = e.target.closest("[data-ed-cat-dn]");
      if (up) {
        e.preventDefault();
        const prev = row.previousElementSibling;
        if (prev) host.insertBefore(row, prev);
        return;
      }
      if (dn) {
        e.preventDefault();
        const next = row.nextElementSibling;
        if (next) host.insertBefore(next, row);
        return;
      }
      const chk = e.target.closest(".chk");
      if (!chk) return;
      e.preventDefault();
      e.stopPropagation();
      const turningOff = chk.classList.contains("on");
      if (turningOff) {
        const others = $$("#edActionCatalog .ed-cat-row").filter(
          (r) => r !== row && r.querySelector(".chk.on")
        );
        if (!others.length) {
          toast("至少保留一种可添加的动作");
          return;
        }
      }
      chk.classList.toggle("on");
      const i = chk.querySelector("i");
      if (i) i.textContent = "";
      row.classList.toggle("is-hidden-type", !chk.classList.contains("on"));
    });
    host.addEventListener("dragstart", (e) => {
      const row = e.target.closest(".ed-cat-row");
      if (!row) return;
      if (e.target.closest(".chk, button")) {
        e.preventDefault();
        return;
      }
      dragEl = row;
      row.classList.add("dragging");
      try {
        e.dataTransfer.effectAllowed = "move";
        e.dataTransfer.setData("text/plain", row.dataset.type || "");
      } catch (_) {}
    });
    host.addEventListener("dragend", () => {
      if (dragEl) dragEl.classList.remove("dragging");
      dragEl = null;
    });
    host.addEventListener("dragover", (e) => {
      e.preventDefault();
      try {
        e.dataTransfer.dropEffect = "move";
      } catch (_) {}
      const over = e.target.closest(".ed-cat-row");
      if (!over || !dragEl || over === dragEl) return;
      const rect = over.getBoundingClientRect();
      const before = e.clientY < rect.top + rect.height / 2;
      host.insertBefore(dragEl, before ? over : over.nextSibling);
    });
    host.addEventListener("drop", (e) => {
      e.preventDefault();
    });
  }

  function fillEditorSettingsForm(other) {
    other = other || (state.settings && state.settings.other) || {};
    const edView = editorDefaultViewOf(other);
    $$("#edSetViewRadios .radio").forEach((r) => {
      r.classList.toggle("on", (r.dataset.editorView || "code") === edView);
    });
    setChk($("#edSetLoopWrap"), otherFlag(other, "visualLoopWrap", true));
    setChk($("#edSetCallWires"), otherFlag(other, "visualBlockCallWires", true));
    setChk($("#edSetIfWrap"), otherFlag(other, "visualIfWrap", true));
    setChk($("#edSetBlockWrap"), otherFlag(other, "visualBlockWrap", true));
    setChk($("#edSetJumpWires"), otherFlag(other, "visualJumpWires", true));
    setChk($("#edSetShowGrid"), otherFlag(other, "visualShowGrid", true));
    setChk($("#edSetShowCardId"), otherFlag(other, "visualShowCardId", true));
    renderEditorActionCatalog(other);
  }

  function collectEditorSettingsPartial() {
    return {
      editorDefaultView:
        $("#edSetViewRadios .radio.on")?.dataset.editorView === "visual" ? "visual" : "code",
      visualLoopWrap: !!$("#edSetLoopWrap")?.classList.contains("on"),
      visualBlockCallWires: !!$("#edSetCallWires")?.classList.contains("on"),
      visualIfWrap: !!$("#edSetIfWrap")?.classList.contains("on"),
      visualBlockWrap: !!$("#edSetBlockWrap")?.classList.contains("on"),
      visualJumpWires: !!$("#edSetJumpWires")?.classList.contains("on"),
      visualShowGrid: !!$("#edSetShowGrid")?.classList.contains("on"),
      visualShowCardId: !!$("#edSetShowCardId")?.classList.contains("on"),
      ...collectEditorCatalogPartial(),
    };
  }

  function applyEditorVisualPrefs() {
    const V = window.QstVisualEditor;
    if (V && typeof V.applyDisplayPrefs === "function") V.applyDisplayPrefs();
    else if (V && typeof V.render === "function") V.render();
  }

  function persistEditorSettings(partial, announce) {
    if (!state.settings) state.settings = {};
    if (!state.settings.other) state.settings.other = {};
    Object.assign(state.settings.other, partial);
    applyEditorVisualPrefs();
    syncEditorActionTypeCombo();
    if (window.qst) {
      if (announce) state._announceSettingsSave = true;
      quietSaveSettings(partial);
    } else if (announce) {
      toast("设置已保存");
    }
  }

  function openEditorSettings() {
    fillEditorSettingsForm((state.settings && state.settings.other) || {});
    setEditorSettingsTab("visual");
    bindEditorActionCatalog();
    openOv("editor-settings");
  }

  function fillSettings(s) {
    state.settings = s || {};
    const click = s.click || {};
    const other = s.other || {};
    const home = s.home || {};
    const playback = s.playback || {};
    const windowMode = s.windowMode || {};

    const pane = document.querySelector('.set-pane[data-pane="click"]');
    if (pane) {
      setChk($("#setRandomIntervalEn"), !!click.enableRandomInterval);
      if ($("#setRandomMax") && click.randomIntervalMaxSeconds != null)
        $("#setRandomMax").textContent = String(click.randomIntervalMaxSeconds);
      setChk(
        $("#setPressReleaseEn"),
        click.enablePressReleaseInterval == null ? true : !!click.enablePressReleaseInterval
      );
      if ($("#setPressRelease") && click.pressReleaseIntervalSeconds != null)
        $("#setPressRelease").textContent = String(click.pressReleaseIntervalSeconds);
      setChk($("#setJitterEn"), !!click.enableCoordinateJitter);
      if ($("#setJitterX") && click.jitterX != null) $("#setJitterX").textContent = String(click.jitterX);
      if ($("#setJitterY") && click.jitterY != null) $("#setJitterY").textContent = String(click.jitterY);
      setChk($("#setFixedCoordEn"), !!click.enableFixedCoordinates);
      if ($("#setFixedX") && click.fixedX != null) $("#setFixedX").textContent = String(click.fixedX);
      if ($("#setFixedY") && click.fixedY != null) $("#setFixedY").textContent = String(click.fixedY);
      setChk($("#setClickLimitEn"), !!click.enableClickCountLimit);
      if ($("#setClickCountLimit") && click.clickCountLimit != null)
        $("#setClickCountLimit").textContent = String(click.clickCountLimit);
    }

    setChk($("#setPlayCountEn"), !!playback.enablePlaybackCount);
    if ($("#setPlayCount") && playback.playbackCount != null)
      $("#setPlayCount").textContent = String(playback.playbackCount);
    setChk($("#setPlayIntervalEn"), !!playback.enablePlaybackInterval);
    if ($("#setPlayIntervalMin") && playback.playbackIntervalMinSeconds != null)
      $("#setPlayIntervalMin").textContent = String(playback.playbackIntervalMinSeconds);
    if ($("#setPlayIntervalMax") && playback.playbackIntervalMaxSeconds != null)
      $("#setPlayIntervalMax").textContent = String(playback.playbackIntervalMaxSeconds);
    setChk($("#setDebugWin"), !!playback.enableDebugOutputWindow);
    // 产品：独立调试窗由引擎 debugWindow.show 打开；勿弹主壳内浮层
    if (!window.qst) {
      if (playback.enableDebugOutputWindow) showDebugFloat({ keepLog: true });
      else hideDebugFloat();
    } else {
      hideDebugFloat();
    }
    setChk(
      $("#setAutoKeyDebug"),
      playback.autoOutputKeyFunctionDebug == null ? true : !!playback.autoOutputKeyFunctionDebug
    );
    setChk(
      $("#setRecCaptureEn"),
      playback.recordingClickCaptureEnabled == null ? true : !!playback.recordingClickCaptureEnabled
    );
    if ($("#setRecCaptureHalf") && playback.recordingClickCaptureHalfSize != null)
      $("#setRecCaptureHalf").textContent = String(playback.recordingClickCaptureHalfSize);
    setChk($("#setPlaySpeedEn"), !!playback.enablePlaybackSpeed);
    setPlaybackSpeedValue(playback.playbackSpeed == null ? 1 : playback.playbackSpeed);
    const backend = playback.foregroundInputBackend | 0;
    document.querySelectorAll('.set-pane[data-pane="play"] .radio[data-backend]').forEach((r) => {
      r.classList.toggle("on", (r.dataset.backend | 0) === backend);
    });
    const schedPrio = (playback.scheduledTaskConflictPolicy | 0) === 1 ? 1 : 0;
    $$("#setSchedPrioRadios .radio").forEach((r) => {
      r.classList.toggle("on", (r.dataset.schedPrio | 0) === schedPrio);
    });
    setChk($("#setSchedResumeEn"), !!playback.scheduledTaskAutoResume);

    setChk($("#setAutoHide"), other.autoHideMainWindow == null ? true : !!other.autoHideMainWindow);
    setChk($("#setPlaySound"), other.playSoundOnStart == null ? true : !!other.playSoundOnStart);
    setChk($("#setPlaySoundEnd"), other.playSoundOnEnd == null ? true : !!other.playSoundOnEnd);
    setChk($("#setHideTip"), other.hideBottomRightTip == null ? true : !!other.hideBottomRightTip);
    setChk($("#setCloseTray"), other.closeToTray == null ? true : !!other.closeToTray);
    setChk($("#setFloatBall"), other.showFloatBall == null ? true : !!other.showFloatBall);
    setChk($("#setAutoBoot"), !!other.autoStartOnBoot);
    setChk($("#setImeConflict"), !!other.resolveImeConflict);
    fillEditorSettingsForm(other);
    syncEditorActionTypeCombo();
    applyEditorVisualPrefs();
    if ($("#setHoldSec") && other.holdThresholdSeconds != null)
      $("#setHoldSec").textContent = String(other.holdThresholdSeconds);
    state.hideBottomRightTip =
      other.hideBottomRightTip == null ? true : !!other.hideBottomRightTip;

    setChk(
      $("#setWmPreviewThumb"),
      windowMode.showPreviewThumbnail == null ? true : !!windowMode.showPreviewThumbnail
    );
    if ($("#setWmPreviewMs") && windowMode.previewRefreshMs != null)
      $("#setWmPreviewMs").textContent = String(windowMode.previewRefreshMs);
    setChk(
      $("#setWmBlockUnhealthy"),
      windowMode.blockRunWhenUnhealthy == null ? true : !!windowMode.blockRunWhenUnhealthy
    );
    setChk($("#setWmFgFallback"), !!windowMode.allowForegroundInputFallback);
    setChk(
      $("#setWmEnableInject"),
      windowMode.enableFakeFocusInjection == null ? true : !!windowMode.enableFakeFocusInjection
    );
    const wmTechV =
      typeof windowMode.injectionTechnique === "number" ? windowMode.injectionTechnique : 0;
    // 旧设置可能存了已下架的单独项（线程劫持/手动映射/XOR），一律回退基线
    const wmTechItem = WM_TECH_LIST.find((x) => x.v === wmTechV);
    state._wmInjectTech = wmTechItem ? wmTechV : 0;
    const wmTechEl = $("#setWmInjectTech");
    if (wmTechEl) wmTechEl.textContent = (wmTechItem || WM_TECH_LIST[0]).t;
    setChk($("#setWmHideModule"), !!windowMode.hideInjectedModule);
    syncWmInjectUi();

    if (typeof home.clickerButton === "number") state.clickBtn = home.clickerButton;
    if (typeof home.clickerIntervalMode === "number")
      state.intervalMode = home.clickerIntervalMode;
    if (typeof home.clickerCustomInterval === "number")
      state.customInterval = home.clickerCustomInterval;
    if (typeof home.recorderInputMode === "number") state.recMode = home.recorderInputMode;
    if (typeof home.recorderWindowMode === "number") state.recWindowMode = home.recorderWindowMode;
    syncRecModeUi();
    // 启动恢复由 maybeRestoreHomeSelection 统一做；此处只缓存 settings
    if (typeof home.activeTab === "number") state._pendingActiveTab = home.activeTab;
    if (typeof home.clickerScrollOffset === "number")
      state._clickerScrollOffset = home.clickerScrollOffset;
    if (typeof home.macroScrollOffset === "number") state._macroScrollOffset = home.macroScrollOffset;
    if (typeof home.recorderScrollOffset === "number") state._recScrollOffset = home.recorderScrollOffset;
    if (typeof home.scriptCustomScrollOffset === "number")
      state._aiScrollOffset = home.scriptCustomScrollOffset;
    if (home.globalHotkeyText != null || home.globalHotkeyVk != null) {
      state.hotkeyVk = home.globalHotkeyVk | 0;
      state.hotkeyModifiers = home.globalHotkeyModifiers | 0;
      state.hotkeyHold = !!home.globalHotkeyHold;
      state.hotkey = formatHotkeyDisplay(
        state.hotkeyVk ? home.globalHotkeyText || "F8" : "",
        state.hotkeyHold
      );
      const hk = $("#comboHotkey");
      if (hk) hk.textContent = state.hotkey;
    }
    if (state.intervalMode === 2) state.intervalLabel = "极限模式 · 10 ms";
    else if (state.intervalMode === 1) state.intervalLabel = "高效模式 · 100 ms";
    else state.intervalLabel = `自定义 · ${state.customInterval}s`;

    const ai = s.ai || {};
    setChk($("#aiEnabled"), !!ai.enabled);
    if ($("#aiApiUrl") && ai.apiUrl != null) $("#aiApiUrl").textContent = ai.apiUrl;
    if ($("#aiApiKey") && ai.apiKey != null) $("#aiApiKey").textContent = ai.apiKey;
    if ($("#aiModelName") && ai.modelName != null) $("#aiModelName").textContent = ai.modelName;
    if ($("#aiTemp") && ai.temperature != null) $("#aiTemp").textContent = String(ai.temperature);
    if ($("#aiTokens") && ai.maxTokens != null) $("#aiTokens").textContent = String(ai.maxTokens);
    state.agentModel = ai.modelName || "";
    state._aiSavedModels = Array.isArray(ai.savedModels) ? ai.savedModels : [];
    // 当前模型不在已添加列表时，切到列表第一项并回填密钥（避免幽灵 gpt-4o / 空密钥）
    applyActiveAiProfileFromSaved();
    refreshAiModelCombos();
    if (AGENT_SHELL) {
      const tab = activeAgentTab();
      if (tab && !(tab.messages && tab.messages.length)) renderChat([]);
    }

    syncClickerControls();
    syncRecModeUi();
    updateCtas();
    applyThemeFromSettings(s);
    if (state.editor) applyEditorVisualPrefs();
  }

  function findAiSavedProfile(modelName) {
    const name = String(modelName || "").trim();
    const models = Array.isArray(state._aiSavedModels) ? state._aiSavedModels : [];
    if (!name) return models[0] || null;
    return models.find((m) => (m.modelName || "") === name) || null;
  }

  /** 将某条已保存模型写到设置表单 + state（含密钥） */
  function applyAiProfileToForm(p) {
    if (!p) return;
    if (p.apiUrl != null && $("#aiApiUrl")) $("#aiApiUrl").textContent = p.apiUrl;
    if (p.apiKey != null && $("#aiApiKey")) $("#aiApiKey").textContent = p.apiKey;
    if (p.modelName != null && $("#aiModelName")) $("#aiModelName").textContent = p.modelName;
    if (p.temperature != null && $("#aiTemp")) $("#aiTemp").textContent = String(p.temperature);
    if (p.maxTokens != null && $("#aiTokens")) $("#aiTokens").textContent = String(p.maxTokens);
    state.agentModel = p.modelName || state.agentModel || "";
    if (!state.settings) state.settings = {};
    if (!state.settings.ai) state.settings.ai = {};
    const ai = state.settings.ai;
    if (p.apiUrl != null) ai.apiUrl = p.apiUrl;
    if (p.apiKey != null) ai.apiKey = p.apiKey;
    if (p.modelName != null) ai.modelName = p.modelName;
    if (p.temperature != null) ai.temperature = p.temperature;
    if (p.maxTokens != null) ai.maxTokens = p.maxTokens;
  }

  function applyActiveAiProfileFromSaved() {
    const models = Array.isArray(state._aiSavedModels) ? state._aiSavedModels : [];
    if (!models.length) return;
    let p = findAiSavedProfile(state.agentModel);
    if (!p) p = models[0];
    const needSnap = !findAiSavedProfile(state.agentModel);
    const ai = (state.settings && state.settings.ai) || {};
    const needKey = !(ai.apiKey || ($("#aiApiKey")?.textContent || "").trim());
    if (needSnap || needKey) applyAiProfileToForm(p);
  }

  function resolveActiveAiConfig() {
    const ai = (state.settings && state.settings.ai) || {};
    const models = Array.isArray(state._aiSavedModels) ? state._aiSavedModels : [];
    const model = (
      state.agentModel ||
      ($("#aiModelName")?.textContent || "").trim() ||
      ai.modelName ||
      (models[0] && models[0].modelName) ||
      ""
    ).trim();
    const profile = findAiSavedProfile(model) || models[0] || null;
    // 助手独立窗里设置表单被隐藏，DOM 可能是默认空值；优先 state / 已保存配置
    const domKey = ($("#aiApiKey")?.textContent || "").trim();
    const domUrl = ($("#aiApiUrl")?.textContent || "").trim();
    let enabled;
    if (AGENT_SHELL) {
      enabled = ai.enabled != null ? !!ai.enabled : !!$("#aiEnabled")?.classList.contains("on");
    } else if ($("#aiEnabled")) {
      enabled = $("#aiEnabled").classList.contains("on");
    } else {
      enabled = !!ai.enabled;
    }
    return {
      enabled,
      apiUrl: (domUrl || (profile && profile.apiUrl) || ai.apiUrl || "").trim(),
      apiKey: (domKey || (profile && profile.apiKey) || ai.apiKey || "").trim(),
      model: model || (profile && profile.modelName) || "",
      profile,
    };
  }

  function refreshAiModelCombos() {
    const models = Array.isArray(state._aiSavedModels) ? state._aiSavedModels : [];
    const modelLabels = models.length
      ? models.map((m) => ({ t: m.modelName || "model", v: m.modelName, profile: m }))
      : [];
    state._aiModels = modelLabels;
    let label = "";
    if (models.length) {
      const hit = findAiSavedProfile(state.agentModel);
      if (hit) label = hit.modelName || "";
      else {
        label = models[0].modelName || "";
        state.agentModel = label;
        applyAiProfileToForm(models[0]);
      }
    } else {
      label = state.agentModel || "未添加模型";
    }
    if ($("#aiModelCombo")) $("#aiModelCombo").textContent = label;
    if ($("#agentModelCombo")) $("#agentModelCombo").textContent = label;
    const tab = typeof activeAgentTab === "function" ? activeAgentTab() : null;
    if (tab && label && label !== "未添加模型") tab.model = label;
  }

  function persistAiSavedModels() {
    if (!window.qst) return;
    const payload = collectSettings();
    if (!payload) return;
    // 显式删除最后一个模型时置清除标志；其余场合空数组只表示页面状态未加载，
    // 后端会保留磁盘上已保存的模型（避免 DeepSeek/豆包配置被空列表冲掉）。
    if (!Array.isArray(state._aiSavedModels) || state._aiSavedModels.length === 0) {
      payload.savedModelsClear = true;
    }
    quietSaveSettings(payload);
    refreshAiModelCombos();
  }

  const QQ_GROUP_JOIN_URL = "https://qm.qq.com/q/tB60fYgiAw";

  function applyAppBranding(b) {
    state._branding = b || {};
    const name = b.name || PRODUCT_NAME;
    const version = b.version || "";
    const website = b.website || "https://www.quickscripttool.cloud/";
    const contact = b.contact || "24353623@qq.com";
    const qq = b.qq || "2163074732";
    try {
      document.title = name;
    } catch (_) {}
    const settingsTitle = $("#settingsDlgTitle");
    if (settingsTitle) {
      const first = settingsTitle.childNodes[0];
      if (first && first.nodeType === 3) first.textContent = `${name} — 设置 `;
    }
    // 专业模式会把 dlg 挪出 #ov-settings，须全局查 about pane
    const aboutPane = document.querySelector('.set-pane[data-pane="about"]');
    const brandEl = aboutPane && aboutPane.querySelector(".brand");
    if (brandEl) {
      brandEl.innerHTML = `<span class="brand-mark"></span> ${esc(name)}${
        version ? ` <span class="brand-ver">${esc(version)}</span>` : ""
      }`;
    }
    if (aboutPane) {
      const websiteEl = aboutPane.querySelector('[data-about-val="website"]');
      if (websiteEl) {
        websiteEl.textContent = website || "—";
        if (website) websiteEl.setAttribute("href", website);
        else websiteEl.removeAttribute("href");
      }
      const contactEl = aboutPane.querySelector('[data-about-val="contact"]');
      if (contactEl) contactEl.textContent = contact || "—";
      const qqEl = aboutPane.querySelector('[data-about-val="qq"]');
      if (qqEl) {
        qqEl.textContent = qq || "2163074732";
        qqEl.setAttribute("href", QQ_GROUP_JOIN_URL);
      }
    }
  }

  function applyDriverProgress(msg) {
    const kind = msg.kind || "";
    const root =
      kind === "vhid" || kind === "virtualHid" ? $("#vhidSteps") : $("#icSteps");
    const statusEl =
      kind === "vhid" || kind === "virtualHid" ? $("#vhidStatus") : $("#icStatus");
    const bar = kind === "vhid" || kind === "virtualHid" ? $("#vhidBar") : $("#icBar");
    const pct = Math.max(0, Math.min(100, msg.percent | 0));
    const step = msg.step | 0;
    if (bar) bar.style.width = pct + "%";
    if (statusEl && msg.status) statusEl.textContent = msg.status;
    if (root) {
      $$(".driver-step", root).forEach((s) => {
        const si = s.dataset.s | 0;
        s.classList.toggle("done", si < step || (si === step && pct >= 100));
        s.classList.toggle("on", si === step && pct < 100);
      });
    }
  }

  function refreshVhidStatus(source) {
    state._vhidStatusSource = source || "dialog";
    if (window.qst) qst.queryVhidStatus();
  }

  function matchThresholdPercent(v) {
    let thr = Number(v);
    if (!(thr > 0)) thr = 65;
    if (thr <= 1) thr *= 100;
    if (thr < 1) thr = 1;
    if (thr > 100) thr = 100;
    return thr;
  }

  function waitFilterIndex() {
    const labels = ["全部", "小于", "小于等于", "大于", "大于等于", "等于", "不等于"];
    const t = ($("#optWaitFilter")?.textContent || "全部").trim();
    const i = labels.indexOf(t);
    return i >= 0 ? i : 0;
  }

  function colorrefToCss(n) {
    const v = Number(n) >>> 0;
    const r = v & 0xff;
    const g = (v >> 8) & 0xff;
    const b = (v >> 16) & 0xff;
    return `#${[r, g, b].map((x) => x.toString(16).padStart(2, "0")).join("")}`;
  }

  function fillPickOverlay(pick) {
    pick = pick || {};
    if ($("#pickX")) $("#pickX").textContent = String(pick.x | 0);
    if ($("#pickY")) $("#pickY").textContent = String(pick.y | 0);
    if ($("#pickTitle")) $("#pickTitle").textContent = pick.windowTitle || "";
    if ($("#pickClass")) $("#pickClass").textContent = pick.windowClassName || "";
    if ($("#pickChild")) $("#pickChild").textContent = pick.childWindowClassName || "";
    if ($("#pickPath")) $("#pickPath").textContent = pick.processPath || "";
    if ($("#pickDoc")) $("#pickDoc").textContent = pick.documentPath || "";
  }

  /** 对齐原生 ApplyWindowPickToConfig：documentPath 优先；否则标题「 - 」前 stem → launchArgs */
  function launchArgsFromWindowPick(pick, existingLaunchArgs) {
    pick = pick || {};
    if (pick.documentPath) return `"${pick.documentPath}"`;
    const cur = existingLaunchArgs != null ? String(existingLaunchArgs) : "";
    if (cur.trim()) return cur;
    let stem = String(pick.windowTitle || "").trim();
    const dash = stem.indexOf(" - ");
    if (dash >= 0) stem = stem.slice(0, dash);
    while (stem.startsWith("*") || stem.startsWith(" ")) stem = stem.slice(1);
    stem = stem.trim();
    if (
      stem &&
      stem.includes(".") &&
      stem !== "无标题" &&
      stem.toLowerCase() !== "untitled"
    ) {
      return `"${stem}"`;
    }
    return cur;
  }

  function applyWindowPickToNestedAction(pick, pending) {
    pick = pick || {};
    const a = editorParamAction();
    if (!a || (a.type !== "runMacro" && a.type !== "mousePlayback")) return;
    readParamPanelInto(a);
    const wm = ensureNestedWindowMode(a);
    if (pick.processPath) wm.targetExePath = pick.processPath;
    if (pick.windowTitle) {
      wm.windowTitle = pick.windowTitle;
      wm.windowName = pick.windowTitle;
      wm.targetWindowTitle = pick.windowTitle;
    }
    if (pick.windowClassName) wm.windowClassName = pick.windowClassName;
    if (pick.childWindowClassName != null)
      wm.childWindowClassName = pick.childWindowClassName || "";
    if (pick.x != null) {
      wm.pickX = pick.x | 0;
      wm.targetPickX = pick.x | 0;
    }
    if (pick.y != null) {
      wm.pickY = pick.y | 0;
      wm.targetPickY = pick.y | 0;
    }
    const nextArgs = launchArgsFromWindowPick(pick, wm.launchArgs);
    if (nextArgs) wm.launchArgs = nextArgs;
    if (
      pending === "nestedWmClass" ||
      pending === "nestedWm" ||
      wm.windowClassName ||
      wm.windowName ||
      wm.windowTitle
    ) {
      wm.selectMethod = "useEditorWindowClass";
    }
    a.nestedWindowMode = wm;
    if ((a.useMode | 0) !== 1 && (a.useMode | 0) !== 2) a.useMode = 1;
    renderParamPanel(a);
  }

  /** 对齐原生 ApplyWindowModeTargetFromPoint：写入身份并切到 useEditorWindowClass */
  function applyWindowPickToEditor(pick, pending) {
    pick = pick || {};
    state.windowMode = state.windowMode || {};
    const wm = state.windowMode;
    if (pick.processPath) wm.targetExePath = pick.processPath;
    if (pick.windowTitle) {
      wm.windowTitle = pick.windowTitle;
      wm.windowName = pick.windowTitle;
    }
    if (pick.windowClassName) wm.windowClassName = pick.windowClassName;
    if (pick.childWindowClassName != null)
      wm.childWindowClassName = pick.childWindowClassName || "";
    if (pick.x != null) {
      wm.pickX = pick.x | 0;
      wm.targetPickX = pick.x | 0;
    }
    if (pick.y != null) {
      wm.pickY = pick.y | 0;
      wm.targetPickY = pick.y | 0;
    }
    const nextArgs = launchArgsFromWindowPick(pick, wm.launchArgs);
    if (nextArgs) wm.launchArgs = nextArgs;
    // 准星找程序 / 指定窗口类：运行时用编辑时身份
    if (
      pending === "windowClass" ||
      pending === "window" ||
      wm.windowClassName ||
      wm.windowName ||
      wm.windowTitle
    ) {
      wm.selectMethod = "useEditorWindowClass";
    }
    syncEditorModeUi();
  }

  function hexToRgb(hex) {
    let h = String(hex || "").replace("#", "").trim();
    if (h.length === 3) h = h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
    const n = parseInt(h, 16);
    if (!Number.isFinite(n)) return { r: 26, g: 166, b: 214 };
    return { r: (n >> 16) & 255, g: (n >> 8) & 255, b: n & 255 };
  }

  function rgbToHex(c) {
    const clamp = (v) => Math.max(0, Math.min(255, v | 0));
    return (
      "#" +
      [clamp(c.r), clamp(c.g), clamp(c.b)]
        .map((x) => x.toString(16).padStart(2, "0"))
        .join("")
    );
  }

  function mixRgb(a, b, t) {
    return {
      r: Math.round(a.r + (b.r - a.r) * t),
      g: Math.round(a.g + (b.g - a.g) * t),
      b: Math.round(a.b + (b.b - a.b) * t),
    };
  }

  function rgbaStr(c, a) {
    return `rgba(${c.r|0},${c.g|0},${c.b|0},${a})`;
  }

  // ═══════════════════════════════════════════════════════════
  // 通用主题调色（美学算法，非按主题名手写色板）
  // 输入：main / accent / light → 输出整套 CSS 变量
  // 原则：同色相 chrome、受控明度阶梯、类似色 aurora、工作区与 chrome 连续
  // ═══════════════════════════════════════════════════════════

  function clamp01(x) {
    return Math.max(0, Math.min(1, x));
  }

  function rgbToHsl(c) {
    const r = c.r / 255, g = c.g / 255, b = c.b / 255;
    const max = Math.max(r, g, b), min = Math.min(r, g, b);
    const l = (max + min) / 2;
    if (max - min < 1e-6) return { h: 0, s: 0, l };
    const d = max - min;
    const s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
    let h = 0;
    if (max === r) h = ((g - b) / d + (g < b ? 6 : 0)) / 6;
    else if (max === g) h = ((b - r) / d + 2) / 6;
    else h = ((r - g) / d + 4) / 6;
    return { h: h * 360, s, l };
  }

  function hue2rgb(p, q, t) {
    let T = t;
    if (T < 0) T += 1;
    if (T > 1) T -= 1;
    if (T < 1 / 6) return p + (q - p) * 6 * T;
    if (T < 1 / 2) return q;
    if (T < 2 / 3) return p + (q - p) * (2 / 3 - T) * 6;
    return p;
  }

  function hslToRgb(h, s, l) {
    const H = ((h % 360) + 360) % 360 / 360;
    const S = clamp01(s), L = clamp01(l);
    if (S < 1e-6) {
      const v = Math.round(L * 255);
      return { r: v, g: v, b: v };
    }
    const q = L < 0.5 ? L * (1 + S) : L + S - L * S;
    const p = 2 * L - q;
    return {
      r: Math.round(hue2rgb(p, q, H + 1 / 3) * 255),
      g: Math.round(hue2rgb(p, q, H) * 255),
      b: Math.round(hue2rgb(p, q, H - 1 / 3) * 255),
    };
  }

  function relLuma(c) {
    const f = (v) => {
      const x = v / 255;
      return x <= 0.03928 ? x / 12.92 : Math.pow((x + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * f(c.r) + 0.7152 * f(c.g) + 0.0722 * f(c.b);
  }

  function contrastRatio(a, b) {
    const L1 = relLuma(a), L2 = relLuma(b);
    const hi = Math.max(L1, L2), lo = Math.min(L1, L2);
    return (hi + 0.05) / (lo + 0.05);
  }

  function shiftHue(h, deg) {
    return ((h + deg) % 360 + 360) % 360;
  }

  /** 由主色科学分类气质：决定 chrome 明度带，而非主题名查表 */
  function classifyThemeMood(mainHsl) {
    const h = mainHsl.h, s = mainHsl.s, l = mainHsl.l;
    // 暖玫瑰 / 粉 / 紫 / 珊瑚 / 橙：中调类似色 chrome（禁近黑）
    const warmRomantic =
      h <= 45 || h >= 320 || (h >= 260 && h <= 330) || (h >= 10 && h <= 50);
    if (warmRomantic && s >= 0.32) return "soft";
    // 蓝青冷色：深 chrome 成立（极光逻辑）
    if (h >= 175 && h <= 255 && s >= 0.28) return "deep";
    return "vivid";
  }

  /**
   * 通用调色：main+accent(+light) → 完整 UI 色板
   * - chrome：主色同色相，明度阶梯随 mood
   * - ice/mint：主色/点缀色阶
   * - aurora：类似色（±色相）+ 点缀，形成连续光谱
   * - sky/frost：主色浅染工作区，减轻「深顶栏 vs 纯白心」断裂
   */
  function buildThemePalette(mainRgb, accentRgb, lightRgb) {
    const white = { r: 255, g: 255, b: 255 };
    const black = { r: 0, g: 0, b: 0 };
    const mh = rgbToHsl(mainRgb);
    const ah = rgbToHsl(accentRgb);
    const mood = classifyThemeMood(mh);

    // chrome 明度带（相对感知）：soft 中调、vivid 中深、deep 深蓝底
    const chromeL =
      mood === "soft" ? 0.50 : mood === "deep" ? 0.16 : 0.34;
    const chromeS =
      mood === "soft"
        ? clamp01(Math.min(0.58, mh.s * 0.82 + 0.08))
        : mood === "deep"
          ? clamp01(Math.min(0.42, mh.s * 0.55 + 0.12))
          : clamp01(Math.min(0.50, mh.s * 0.70 + 0.08));

    const step = (dl, ds = 0) =>
      hslToRgb(mh.h, clamp01(chromeS + ds), clamp01(chromeL + dl));

    // 阶梯间距收紧，避免 active tab（navy-600）相对 chrome 跳色过猛
    const navy950 = step(-0.10, 0.04);
    const navy900 = step(-0.05, 0.02);
    const navy800 = step(0, 0);
    const navy700 = step(0.07, -0.02);
    const navy600 = step(0.13, mood === "soft" ? 0.04 : 0.02);

    const ice500 = mainRgb;
    const ice400 = hslToRgb(mh.h, clamp01(mh.s * 0.9), clamp01(mh.l + 0.08));
    const ice300 = hslToRgb(mh.h, clamp01(mh.s * 0.7), clamp01(mh.l + 0.22));
    const ice200 = hslToRgb(mh.h, clamp01(mh.s * 0.45), clamp01(mh.l + 0.36));

    const mint400 = accentRgb;
    const mint300 = hslToRgb(ah.h, clamp01(ah.s * 0.85), clamp01(ah.l + 0.12));
    const mint200 = hslToRgb(ah.h, clamp01(ah.s * 0.55), clamp01(ah.l + 0.28));

    // 工作区：有主色倾向的浅底，与 chrome 同色相连续
    const skyBase = lightRgb
      ? lightRgb
      : hslToRgb(mh.h, clamp01(Math.min(0.28, mh.s * 0.35 + 0.08)), 0.94);
    const sky100 = skyBase;
    const sky50 = mixRgb(skyBase, white, 0.42);
    const frost = mixRgb(skyBase, white, 0.72);

    // aurora：类似色扇面（主色 ±28°/~45°）+ 点缀，避免只有两色
    const analogDeg = mood === "deep" ? 42 : mood === "soft" ? 26 : 32;
    const auroraA = mainRgb;
    const auroraB = hslToRgb(
      shiftHue(mh.h, analogDeg),
      clamp01(Math.min(0.72, mh.s * 0.85 + 0.1)),
      clamp01(mood === "soft" ? Math.max(0.55, mh.l + 0.08) : mh.l + 0.04)
    );
    const auroraC =
      mood === "soft"
        ? hslToRgb(
            shiftHue(mh.h, -22),
            clamp01(Math.min(0.65, Math.max(ah.s, 0.4))),
            clamp01(Math.max(0.58, ah.l + 0.05))
          )
        : accentRgb;
    const auroraD = hslToRgb(mh.h, clamp01(mh.s * 0.65), clamp01(mh.l + 0.18));

    const ctaMid = mixRgb(navy800, ice400, mood === "soft" ? 0.45 : 0.28);
    const ctaEnd = mixRgb(navy600, mint300, mood === "soft" ? 0.4 : 0.32);

    // chrome 上文字：保证对比；soft 用暖白，deep 用冷白
    let chromeFg = mixRgb(white, mainRgb, mood === "soft" ? 0.06 : 0.04);
    if (contrastRatio(chromeFg, navy800) < 3.2) {
      chromeFg = white;
    }
    const chromeMuted = mixRgb(
      mixRgb(chromeFg, navy800, 0.35),
      ice200,
      mood === "soft" ? 0.35 : 0.15
    );
    const chromeIcon = mixRgb(chromeMuted, chromeFg, 0.35);
    const chromeTab = mixRgb(chromeMuted, navy800, 0.28);

    const text = mixRgb(black, mainRgb, 0.14);
    const text2 = mixRgb(mixRgb(black, mainRgb, 0.28), white, 0.32);
    const text3 = mixRgb(mixRgb(black, mainRgb, 0.22), white, 0.48);
    const line = mixRgb(sky100, mainRgb, 0.26);
    const lineStrong = mixRgb(sky100, mainRgb, 0.42);

    const amberHot = hslToRgb(
      shiftHue(ah.h, mood === "soft" ? 0 : 8),
      clamp01(Math.max(0.55, ah.s)),
      clamp01(Math.min(0.58, ah.l + 0.02))
    );
    const amberSoft = mixRgb(white, amberHot, 0.18);
    const rose = hslToRgb(
      shiftHue(mh.h, mood === "soft" ? -8 : 20),
      clamp01(Math.min(0.7, mh.s + 0.05)),
      clamp01(0.62)
    );

    return {
      mood,
      vars: {
        "--navy-950": rgbToHex(navy950),
        "--navy-900": rgbToHex(navy900),
        "--navy-800": rgbToHex(navy800),
        "--navy-700": rgbToHex(navy700),
        "--navy-600": rgbToHex(navy600),
        "--ice-500": rgbToHex(ice500),
        "--ice-400": rgbToHex(ice400),
        "--ice-300": rgbToHex(ice300),
        "--ice-200": rgbToHex(ice200),
        "--mint-400": rgbToHex(mint400),
        "--mint-300": rgbToHex(mint300),
        "--mint-200": rgbToHex(mint200),
        "--sky-100": rgbToHex(sky100),
        "--sky-50": rgbToHex(sky50),
        "--frost": rgbToHex(frost),
        "--aurora-a": rgbToHex(auroraA),
        "--aurora-b": rgbToHex(auroraB),
        "--aurora-c": rgbToHex(auroraC),
        "--aurora-d": rgbToHex(auroraD),
        "--chrome-fg": rgbToHex(chromeFg),
        "--chrome-muted": rgbToHex(chromeMuted),
        "--chrome-icon": rgbToHex(chromeIcon),
        "--chrome-tab": rgbToHex(chromeTab),
        "--text": rgbToHex(text),
        "--text-2": rgbToHex(text2),
        "--text-3": rgbToHex(text3),
        "--line": rgbToHex(line),
        "--line-strong": rgbToHex(lineStrong),
        "--glow-ice": rgbaStr(mainRgb, mood === "soft" ? 0.48 : 0.4),
        "--glow-mint": rgbaStr(accentRgb, mood === "soft" ? 0.4 : 0.35),
        "--glow-aurora": rgbaStr(auroraB, mood === "soft" ? 0.38 : 0.28),
        "--focus-ring": rgbaStr(mainRgb, 0.22),
        "--cta-mid": rgbToHex(ctaMid),
        "--cta-end": rgbToHex(ctaEnd),
        "--amber-hot": rgbToHex(amberHot),
        "--amber-soft": rgbToHex(amberSoft),
        "--rose": rgbToHex(rose),
      },
    };
  }

  function applyThemeCss(theme) {
    if (!theme) return;
    const root = document.documentElement;
    const applied = {};
    const setTracked = (k, v) => {
      root.style.setProperty(k, v);
      applied[k] = v;
    };

    const main = hexToRgb(theme.main || theme.mainColor || "#1aa6d6");
    const accent = hexToRgb(theme.accent || theme.accentColor || "#2dd4bf");
    const light = theme.light || theme.workspace
      ? hexToRgb(theme.light || theme.workspace)
      : null;

    const palette = buildThemePalette(main, accent, light);
    Object.entries(palette.vars).forEach(([k, v]) => setTracked(k, v));
    root.dataset.themeMood = palette.mood;
    if (theme.name) root.dataset.themeName = theme.name;
    pushThemeCssToDebug(applied);
  }

  function collectRootThemeVars() {
    const keys = [
      "--navy-950", "--navy-900", "--navy-800", "--navy-700", "--navy-600",
      "--ice-500", "--ice-400", "--ice-300", "--ice-200",
      "--mint-400", "--mint-300", "--mint-200",
      "--sky-100", "--sky-50", "--frost",
      "--aurora-a", "--aurora-b", "--aurora-c", "--aurora-d",
      "--cta-mid", "--cta-end",
      "--glow-ice", "--glow-mint", "--glow-aurora", "--focus-ring",
      "--line", "--line-strong", "--text", "--text-2", "--text-3", "--rose",
      "--chrome-fg", "--chrome-muted", "--chrome-icon", "--chrome-tab",
      "--amber-hot", "--amber-soft",
    ];
    const cs = getComputedStyle(document.documentElement);
    const vars = {};
    keys.forEach((k) => {
      const v = (cs.getPropertyValue(k) || "").trim();
      if (v) vars[k] = v;
    });
    return vars;
  }

  function pushThemeCssToDebug(vars) {
    if (AGENT_SHELL) return;
    if (!window.qst || typeof qst.post !== "function") return;
    const payload = vars && Object.keys(vars).length ? vars : collectRootThemeVars();
    if (!payload || !Object.keys(payload).length) return;
    qst.post({ type: "themeCss.apply", vars: payload });
  }

  function applyThemeFromSettings(s) {
    const other = (s && s.other) || {};
    state.themeId = other.themeId | 0;
    state.useCustomTheme = !!other.useCustomTheme;
    if (state.useCustomTheme) {
      applyThemeCss({
        main: colorrefToCss(other.customMainColor),
        accent: colorrefToCss(other.customAccentColor),
      });
      if ($("#themeCombo")) $("#themeCombo").textContent = "自定义";
    } else {
      const t = (state.themes || []).find((x) => x.id === state.themeId);
      if (t) {
        applyThemeCss(t);
        if ($("#themeCombo")) $("#themeCombo").textContent = t.name || "主题";
      }
    }
    renderThemeGrids();
  }

  function renderThemeGrids() {
    const themes = state.themes || [];
    ["themeGridHome", "themeGridDlg"].forEach((id) => {
      const grid = $("#" + id);
      if (!grid) return;
      grid.innerHTML = themes
        .map((t) => {
          const on =
            !state.useCustomTheme && (state.themeId | 0) === (t.id | 0) ? "on" : "";
          const pal = buildThemePalette(
            hexToRgb(t.main),
            hexToRgb(t.accent),
            t.light ? hexToRgb(t.light) : null
          ).vars;
          const sw = [
            pal["--navy-800"],
            pal["--ice-500"],
            pal["--mint-400"],
            pal["--aurora-b"],
            pal["--sky-100"],
          ];
          return `<div class="theme-card ${on}" data-theme-id="${t.id}">
            <div class="tc-name">${esc(t.name || "")}</div>
            <div class="tc-swatches">${sw
              .map((c) => `<i style="background:${esc(c)}"></i>`)
              .join("")}</div>
          </div>`;
        })
        .join("");
      grid.querySelectorAll("[data-theme-id]").forEach((card) => {
        card.addEventListener("click", () => {
          const idNum = parseInt(card.dataset.themeId, 10) || 0;
          state.themeId = idNum;
          state.useCustomTheme = false;
          if (window.qst) {
            qst.applyTheme({ themeId: idNum, useCustomTheme: 0 });
          }
          const t = themes.find((x) => x.id === idNum);
          applyThemeCss(t);
          if ($("#themeCombo")) $("#themeCombo").textContent = (t && t.name) || "主题";
          renderThemeGrids();
        });
      });
    });
  }

  function collectSettings() {
    if (!state._gotSettings) return null;
    const aiCfg = resolveActiveAiConfig();
    const out = {
      enableRandomInterval: false,
      randomIntervalMaxSeconds: 0.5,
      enablePressReleaseInterval: false,
      pressReleaseIntervalSeconds: 0.001,
      enableCoordinateJitter: false,
      jitterX: 2,
      jitterY: 2,
      enableFixedCoordinates: false,
      fixedX: 0,
      fixedY: 0,
      enableClickCountLimit: false,
      clickCountLimit: 0,
      themeId: state.themeId | 0,
      useCustomTheme: !!state.useCustomTheme,
      customMainColor: (state.settings && state.settings.other && state.settings.other.customMainColor) || 0,
      customAccentColor:
        (state.settings && state.settings.other && state.settings.other.customAccentColor) || 0,
      autoHideMainWindow: !!$("#setAutoHide")?.classList.contains("on"),
      playSoundOnStart: !!$("#setPlaySound")?.classList.contains("on"),
      playSoundOnEnd: !!$("#setPlaySoundEnd")?.classList.contains("on"),
      hideBottomRightTip: !!$("#setHideTip")?.classList.contains("on"),
      closeToTray: !!$("#setCloseTray")?.classList.contains("on"),
      showFloatBall: !!$("#setFloatBall")?.classList.contains("on"),
      autoStartOnBoot: !!$("#setAutoBoot")?.classList.contains("on"),
      resolveImeConflict: !!$("#setImeConflict")?.classList.contains("on"),
      ...editorVisualFromState(),
      // Direct2D 开关已移除：保留磁盘既有值
      preferDirect2D: !!(state.settings && state.settings.other && state.settings.other.preferDirect2D),
      holdThresholdSeconds: parseFloat($("#setHoldSec")?.textContent || "0.2") || 0.2,
      enablePlaybackCount: !!$("#setPlayCountEn")?.classList.contains("on"),
      playbackCount: parseInt($("#setPlayCount")?.textContent || "1", 10) || 0,
      enablePlaybackInterval: !!$("#setPlayIntervalEn")?.classList.contains("on"),
      playbackIntervalMinSeconds: parseFloat($("#setPlayIntervalMin")?.textContent || "0.5") || 0.5,
      playbackIntervalMaxSeconds: parseFloat($("#setPlayIntervalMax")?.textContent || "1") || 1,
      enableDebugOutputWindow: !!$("#setDebugWin")?.classList.contains("on"),
      autoOutputKeyFunctionDebug: !!$("#setAutoKeyDebug")?.classList.contains("on"),
      recordingClickCaptureEnabled: !!$("#setRecCaptureEn")?.classList.contains("on"),
      recordingClickCaptureHalfSize: parseInt($("#setRecCaptureHalf")?.textContent || "40", 10) || 40,
      enablePlaybackSpeed: !!$("#setPlaySpeedEn")?.classList.contains("on"),
      playbackSpeed: readPlaybackSpeedValue(),
      uiMode: (window.ProMode ? window.ProMode.mode() : state.uiMode) || "simple",
      foregroundInputBackend: 0,
      scheduledTaskConflictPolicy: 0,
      scheduledTaskAutoResume: false,
      clickerButton: state.clickBtn,
      clickerIntervalMode: state.intervalMode,
      clickerCustomInterval: state.customInterval,
      recorderInputMode: state.recMode | 0,
      recorderWindowMode: state.recWindowMode | 0,
      recorderCaptureScope: 1, // 固定全局捕获；窗口范围入口已移除
      activeTab: ({ clicker: 0, recorder: 1, macro: 2, ai: 3 }[state.tab] != null
        ? { clicker: 0, recorder: 1, macro: 2, ai: 3 }[state.tab]
        : 0),
      ...selectionPersistFields(),
      aiEnabled: !!aiCfg.enabled,
      apiUrl: aiCfg.apiUrl,
      apiKey: aiCfg.apiKey,
      modelName: aiCfg.model,
      temperature: parseFloat($("#aiTemp")?.textContent || "0.3") || 0.3,
      maxTokens: parseInt($("#aiTokens")?.textContent || "4096", 10) || 4096,
      savedModels: Array.isArray(state._aiSavedModels) ? state._aiSavedModels : [],
    };
    const backendOn = document.querySelector(
      '.set-pane[data-pane="play"] .radio[data-backend].on'
    );
    if (backendOn) out.foregroundInputBackend = backendOn.dataset.backend | 0;
    const schedPrioOn = $("#setSchedPrioRadios .radio.on");
    if (schedPrioOn) out.scheduledTaskConflictPolicy = schedPrioOn.dataset.schedPrio | 0;
    out.scheduledTaskAutoResume = !!$("#setSchedResumeEn")?.classList.contains("on");
    state.hideBottomRightTip = out.hideBottomRightTip;

    out.enableRandomInterval = !!$("#setRandomIntervalEn")?.classList.contains("on");
    out.randomIntervalMaxSeconds = parseFloat($("#setRandomMax")?.textContent || "0.5") || 0;
    out.enablePressReleaseInterval = !!$("#setPressReleaseEn")?.classList.contains("on");
    out.pressReleaseIntervalSeconds = parseFloat($("#setPressRelease")?.textContent || "0.001") || 0;
    out.enableCoordinateJitter = !!$("#setJitterEn")?.classList.contains("on");
    out.jitterX = parseInt($("#setJitterX")?.textContent || "0", 10) || 0;
    out.jitterY = parseInt($("#setJitterY")?.textContent || "0", 10) || 0;
    out.enableFixedCoordinates = !!$("#setFixedCoordEn")?.classList.contains("on");
    out.fixedX = parseInt($("#setFixedX")?.textContent || "0", 10) || 0;
    out.fixedY = parseInt($("#setFixedY")?.textContent || "0", 10) || 0;
    out.enableClickCountLimit = !!$("#setClickLimitEn")?.classList.contains("on");
    out.clickCountLimit = parseInt($("#setClickCountLimit")?.textContent || "0", 10) || 0;

    out.showPreviewThumbnail = !!$("#setWmPreviewThumb")?.classList.contains("on");
    {
      let ms = parseInt($("#setWmPreviewMs")?.textContent || "500", 10);
      if (!Number.isFinite(ms)) ms = 500;
      if (ms < 200) ms = 200;
      if (ms > 5000) ms = 5000;
      out.previewRefreshMs = ms;
    }
    out.blockRunWhenUnhealthy = !!$("#setWmBlockUnhealthy")?.classList.contains("on");
    out.allowForegroundInputFallback = !!$("#setWmFgFallback")?.classList.contains("on");
    {
      const injectEl = $("#setWmEnableInject");
      // 设置页未挂载时不得写成 false，否则下次运行冒险岛只会 PostMessage、角色不动。
      out.enableFakeFocusInjection = injectEl ? injectEl.classList.contains("on") : true;
    }
    out.injectionTechnique =
      typeof state._wmInjectTech === "number" ? state._wmInjectTech : 0;
    out.hideInjectedModule = !!$("#setWmHideModule")?.classList.contains("on");

    out.clickerScrollOffset = state._clickerScrollOffset | 0;
    out.macroScrollOffset = state._macroScrollOffset | 0;
    out.recorderScrollOffset = state._recScrollOffset | 0;
    out.scriptCustomScrollOffset = state._aiScrollOffset | 0;

    if (AGENT_SHELL) {
      delete out.uiMode;
      delete out.playbackSpeed;
      delete out.enablePlaybackSpeed;
    }

    if (!state.settings) state.settings = {};
    if (!state.settings.other) state.settings.other = {};
    state.settings.other.editorDefaultView = out.editorDefaultView;
    state.settings.other.visualLoopWrap = out.visualLoopWrap;
    state.settings.other.visualBlockCallWires = out.visualBlockCallWires;
    state.settings.other.visualIfWrap = out.visualIfWrap;
    state.settings.other.visualBlockWrap = out.visualBlockWrap;
    state.settings.other.visualJumpWires = out.visualJumpWires;
    state.settings.other.visualShowGrid = out.visualShowGrid;
    state.settings.other.visualShowCardId = out.visualShowCardId;
    state.settings.other.editorActionOrder = Array.isArray(out.editorActionOrder)
      ? out.editorActionOrder
      : [];
    state.settings.other.editorHiddenActions = Array.isArray(out.editorHiddenActions)
      ? out.editorHiddenActions
      : [];

    return out;
  }

  function fline(label, controlHtml) {
    if (arguments.length === 1) {
      return `<div class="fline"><span class="tl grow-lab">${esc(label)}</span></div>`;
    }
    return `<div class="fline"><span class="tl grow-lab">${esc(label)}</span>${controlHtml}</div>`;
  }
  /** 文案上一排、控件下一排（对齐「等待」） */
  function flineStack(label, controlHtml) {
    return (
      `<div class="fline-stack"><div class="stack-lab">${esc(label)}</div>` +
      `<div class="stack-ctrl">${controlHtml}</div></div>`
    );
  }
  function flineXy(aLab, aHtml, bLab, bHtml) {
    return `<div class="fline xy"><span class="tl n">${esc(aLab)}</span>${aHtml}<span class="tl n">${esc(
      bLab
    )}</span>${bHtml}</div>`;
  }
  /** 全图搜索区域：屏幕左上→右下（兼容不同分辨率；对齐原生 GetVirtualScreenRect） */
  function getScreenSearchBounds() {
    const scr = window.screen || {};
    const w = (scr.width | 0) || 1920;
    const h = (scr.height | 0) || 1080;
    return { x1: 0, y1: 0, x2: w, y2: h };
  }
  function applyFullScreenSearchCoords(a, prefix) {
    if (!a) return;
    const b = getScreenSearchBounds();
    const px = prefix || "search";
    a[px + "X1"] = b.x1;
    a[px + "Y1"] = b.y1;
    a[px + "X2"] = b.x2;
    a[px + "Y2"] = b.y2;
    a.searchFullScreen = 1;
  }
  function ensureFullScreenCoordsVisible(a, prefix) {
    if (!a || !(a.searchFullScreen | 0)) return;
    const px = prefix || "search";
    const x1 = a[px + "X1"] | 0;
    const y1 = a[px + "Y1"] | 0;
    const x2 = a[px + "X2"] | 0;
    const y2 = a[px + "Y2"] | 0;
    if (x2 <= x1 || y2 <= y1) applyFullScreenSearchCoords(a, prefix);
  }
  /** 绝对识别区域：标签 + 全图/选取区域 + X1Y1X2Y2 */
  function absSearchRegionHtml(a, prefix) {
    const px = prefix || "search";
    const fiFull = px === "aiSearch" ? "aiFull" : "full";
    // OCR/找图共用 search* 时，绝对选区勿走 findImageMatch(region)（那是相对偏移拾取）
    const fiRegion = px === "aiSearch" ? "aiRegion" : "absRegion";
    ensureFullScreenCoordsVisible(a, px === "search" ? undefined : px);
    return (
      `<div class="fline"><span class="tl grow-lab">识别区域</span><div class="inline-btns">
        <button type="button" class="btn ghost sm" data-fi="${fiFull}">全图</button>
        <button type="button" class="btn ghost sm" data-fi="${fiRegion}">选取区域</button>
      </div></div>` +
      flineXy(
        "X1",
        inpField(px + "X1", a[px + "X1"] ?? 0),
        "Y1",
        inpField(px + "Y1", a[px + "Y1"] ?? 0)
      ) +
      flineXy(
        "X2",
        inpField(px + "X2", a[px + "X2"] ?? 0),
        "Y2",
        inpField(px + "Y2", a[px + "Y2"] ?? 0)
      )
    );
  }
  /** 根据图片：选取区域位置 + 模板内相对偏移 X1Y1X2Y2 */
  function imageRegionRelHtml(a) {
    return (
      `<div class="fline"><button type="button" class="btn ghost fluid" data-fi="pickImageRegion">选取区域位置</button></div>` +
      flineXy(
        "X1",
        inpField("imageRegionX1", a.imageRegionX1 ?? 0),
        "Y1",
        inpField("imageRegionY1", a.imageRegionY1 ?? 0)
      ) +
      flineXy(
        "X2",
        inpField("imageRegionX2", a.imageRegionX2 ?? 0),
        "Y2",
        inpField("imageRegionY2", a.imageRegionY2 ?? 0)
      )
    );
  }
  function labOnly(s) {
    return `<div class="fline"><span class="tl">${esc(s)}</span></div>`;
  }
  function inpField(key, val, w) {
    let cls = "ed-md";
    if (w === "coord") cls = "ed-coord";
    else if (w === "rand") cls = "ed-rand";
    else if (w === "varx") cls = "ed-varx";
    else if (w === "sm") cls = "ed-sm";
    else if (w === "full") cls = "ed-full";
    else if (w && w >= 160) cls = "ed-lg";
    return `<div class="inp ${cls}" contenteditable="true" data-k="${esc(key)}">${esc(val)}</div>`;
  }

  function area3rowHtml(key, val) {
    return `<div class="fline"><div class="inp ed-area fixed-3row" contenteditable="true" data-k="${esc(
      key
    )}">${esc(val || "")}</div></div>`;
  }

  /** X:[主=剩余区φ] ±随机:[占同行剩余]；与变量表达式主框同宽 */
  function flineCoord(lab, key, randKey, a) {
    return (
      `<div class="fline coord-row">` +
      `<span class="tl coord-lab">${esc(lab)}</span>` +
      `<div class="coord-rest">` +
      inpField(key, a[key] ?? 0, "coord") +
      `<div class="rand-tail">` +
      `<span class="tl rand-lab">±随机:</span>` +
      inpField(randKey, a[randKey] ?? 0, "rand") +
      `</div></div></div>`
    );
  }

  function flineVarExpr(lab, key, a) {
    return (
      `<div class="fline coord-row">` +
      `<span class="tl coord-lab">${esc(lab)}</span>` +
      `<div class="coord-rest">` +
      inpField(key, a[key] || "", "varx") +
      `</div></div>`
    );
  }
  function comboField(key, text) {
    return `<div class="combo ed-full" data-k="${esc(key)}">${esc(text)}</div>`;
  }

  function chkLabeled(key, label, on) {
    return `<span class="chk ${on ? "on" : ""}" data-k="${esc(key)}"><i></i><span class="chk-lab">${esc(
      label
    )}</span></span>`;
  }

  function modBlock(a) {
    const items = [
      ["holdLeftWin", "左Win"],
      ["holdRightWin", "右Win"],
      ["holdLeftCtrl", "左Ctrl"],
      ["holdRightCtrl", "右Ctrl"],
      ["holdLeftAlt", "左Alt"],
      ["holdRightAlt", "右Alt"],
      ["holdLeftShift", "左Shift"],
      ["holdRightShift", "右Shift"],
    ];
    return `<div class="mod-grid">${items
      .map(([k, lab]) => chkLabeled(k, lab, !!a[k]))
      .join("")}</div>`;
  }

  function repeatBlock(a) {
    return (
      flineStack("循环次数", inpField("clickCount", a.clickCount ?? 1)) +
      flineStack(
        "重复间隔",
        inpField("duration", a.duration ?? 0.01) + `<span class="tl">秒</span>`
      ) +
      flineStack(
        "随机间隔",
        inpField("randomDuration", a.randomDuration ?? 0) + `<span class="tl">秒</span>`
      )
    );
  }

  function findImageModuleHtml(a, mode) {
    const isAi = mode === "ai";
    const isOcr = mode === "ocr";
    const useVarKey = isAi ? "aiImageUseVar" : "imageUseVar";
    const useVar = !!(isAi ? a.aiImageUseVar : a.imageUseVar);
    const path = isAi ? a.aiTargetImagePath || "" : a.imagePath || "";
    const previewInner =
      !useVar && path
        ? `<img class="fi-thumb" alt="" data-img-path="${esc(path)}" src="" />`
        : useVar
          ? `<span class="fi-empty">变量模式</span>`
          : "";
    const varChk = chkLabeled(useVarKey, "变量", useVar);
    // 仅找图动作顶栏带「测试」；OCR/AI 无此按钮（OCR 底部另有测试）
    const withTest = !isAi && !isOcr;
    const topRow = withTest
      ? `<div class="find-mod-top find-mod-top--with-test">
          <div class="find-mod-top-main">
            <span class="find-mod-lab">要查找的图</span>
            <span class="find-mod-var">${varChk}</span>
          </div>
          <button type="button" class="btn ghost sm" data-fi="test"${
            useVar ? " disabled" : ""
          }>测试</button>
        </div>`
      : `<div class="find-mod-top find-mod-top--no-test">
          <span class="find-mod-lab">要查找的图</span>
          <span class="find-mod-var">${varChk}</span>
        </div>`;
    const dis = useVar ? " disabled" : "";
    const side = isAi
      ? `<button type="button" class="btn ghost find-btn" data-fi="aiScreen"${dis}>屏幕截图</button>
        <button type="button" class="btn ghost find-btn" data-fi="pickAiImg"${dis}>本地图片</button>
        <button type="button" class="btn ghost find-btn" data-fi="aiClearImg">清除图片</button>`
      : `<button type="button" class="btn ghost find-btn" data-fi="screen"${dis}>屏幕截图</button>
        <button type="button" class="btn ghost find-btn" data-fi="pick"${dis}>本地图片</button>
        <button type="button" class="btn ghost find-btn" data-fi="clear">清除图片</button>`;
    return `<div class="find-mod">
      ${topRow}
      <div class="find-mod-grid">
        <div class="fm-preview" data-fi="${isAi ? "aiCrop" : "crop"}">${previewInner}</div>
        ${side}
      </div>
    </div>`;
  }

  function matchThresholdRowHtml(a) {
    const perfect = !!(a.perfectMatch | 0);
    const thr = a.matchThreshold ?? 65;
    return `<div class="fline xy thr-perfect-row${perfect ? " is-perfect" : ""}">
      <span class="tl thr-lab">范围</span>
      <span class="thr-field"><div class="inp ed-sm${
        perfect ? " is-disabled" : ""
      }" contenteditable="${perfect ? "false" : "true"}" data-k="matchThreshold">${esc(
        String(thr)
      )}</div><span class="tl">%</span></span>
      <span class="thr-gap" aria-hidden="true"></span>
      <span class="thr-perfect">${chkLabeled("perfectMatch", "完美匹配", perfect)}</span>
    </div>`;
  }

  /** 变量模式下：「变量/路径」整排输入（在范围上方） */
  function findImageVarPathHtml(a, mode) {
    const isAi = mode === "ai";
    const useVar = !!(isAi ? a.aiImageUseVar : a.imageUseVar);
    if (!useVar) return "";
    const key = isAi ? "aiTargetImagePath" : "imagePath";
    const val = isAi ? a.aiTargetImagePath || "" : a.imagePath || "";
    return labOnly("变量/路径") + area3rowHtml(key, val);
  }

  function findImageHasTemplate(a, mode) {
    const isAi = mode === "ai";
    const path = String(isAi ? a.aiTargetImagePath || "" : a.imagePath || "").trim();
    return !!path;
  }

  function findImageShowsFindTime(a) {
    const fu = a.findImageFollowUp | 0;
    if (fu === 2 || fu === 3) return findImageHasTemplate(a);
    return true;
  }

  function findImageTimeRowHtml(a) {
    return fline("时间", inpField("findTimeExpr", a.findTimeExpr || "0"));
  }

  function looksLikeFilePath(s) {
    const t = String(s || "").trim();
    if (!t) return false;
    if (t.includes("\\") || t.includes("/")) return true;
    if (t.length >= 2 && ((t[0] >= "A" && t[0] <= "Z") || (t[0] >= "a" && t[0] <= "z")) && t[1] === ":") {
      return true;
    }
    return false;
  }

  /** 编辑期推断图片变量锚框尺寸（用于合成选区）。找不到则返回 null。 */
  function inferImageVarAnchorSize(token, beforeIndex) {
    const name = String(token || "").trim();
    if (!name) return null;
    if (looksLikeFilePath(name)) {
      // 路径：交给原生读尺寸（syntheticW/H=0 + imagePath）
      return { w: 0, h: 0, pathOnly: true };
    }
    const acts = state.editorActions || [];
    const end = Math.min(acts.length, Math.max(0, beforeIndex));
    for (let i = end - 1; i >= 0; --i) {
      const p = acts[i];
      if (!p || p.type !== "findImage" || (p.findImageFollowUp | 0) !== 3) continue;
      const vn = String(p.matchVarName || "image").trim() || "image";
      if (vn !== name) continue;
      const hasTpl = findImageHasTemplate(p);
      if (!hasTpl) {
        const w = Math.abs((p.searchX2 | 0) - (p.searchX1 | 0));
        const h = Math.abs((p.searchY2 | 0) - (p.searchY1 | 0));
        if (w >= 8 && h >= 8) return { w, h };
        continue;
      }
      const rx1 = p.imageRegionX1 | 0;
      const ry1 = p.imageRegionY1 | 0;
      const rx2 = p.imageRegionX2 | 0;
      const ry2 = p.imageRegionY2 | 0;
      if (rx2 > rx1 && ry2 > ry1) return { w: rx2 - rx1, h: ry2 - ry1 };
      // 生产者有模板但无相对区：无法在编辑期得知匹配框；若模板是路径可交给原生
      const tpl = String(p.imagePath || "").trim();
      if (tpl && !p.imageUseVar && looksLikeFilePath(tpl)) return { w: 0, h: 0, pathOnly: true, path: tpl };
      if (tpl && !p.imageUseVar) return { w: 0, h: 0, pathOnly: true, path: tpl };
    }
    return null;
  }

  function buildEditorVarItems() {
    const items = [];
    const seen = new Set();
    const push = (code, tip) => {
      const c = String(code || "").trim();
      if (!c || seen.has(c)) return;
      seen.add(c);
      items.push({ t: c, v: c, code: c, insert: "{" + c + "}", d: tip || "" });
    };
    (state.editorActions || []).forEach((a) => {
      if (!a || a._preview) return;
      const t = a.type || "";
      if (t === "findImage") {
        const fu = a.findImageFollowUp | 0;
        if (fu === 3) {
          const n = (a.matchVarName || "image").trim() || "image";
          if (!n.includes("\\") && !n.includes("/") && !(n.length >= 2 && n[1] === ":")) {
            push(n, "图片变量");
          }
        } else {
          const n = (a.matchVarName || "matchRet").trim() || "matchRet";
          push(n + ".matchData", "找图匹配度");
          push(n + ".x", "找图左上角 X");
          push(n + ".y", "找图左上角 Y");
          push(n + ".x1", "找图右下角 X");
          push(n + ".y1", "找图右下角 Y");
        }
      } else if (t === "textRecognition") {
        const n = (a.matchVarName || "a").trim() || "a";
        push(n, "OCR 结果");
        push(n + ".x", "OCR 区域 X");
        push(n + ".y", "OCR 区域 Y");
      } else if (t === "loop") {
        const n = (a.loopVarName || "").trim();
        if (n) push(n, "循环计数");
      } else if (t === "getCursorPos") {
        const n = (a.matchVarName || "a").trim() || "a";
        push(n + ".x", "光标 X");
        push(n + ".y", "光标 Y");
      } else if (t === "timerRecordTime") {
        const n = (a.loopVarName || a.matchVarName || "t").trim() || "t";
        push(n, "计时秒数");
      } else if (
        t === "aiTextAnalysis" ||
        t === "aiImageAnalysis" ||
        t === "aiActionExecute"
      ) {
        const n = (a.aiOutputVarName || "aiResult").trim() || "aiResult";
        push(n, "AI 输出");
      }
    });
    // 固定变量：无论脚本是否添加了动作，始终可用
    const fixed = [
      ["ctrl:CurLoops()", "宏运行次数：当前宏从头执行的第几次（固定变量）"],
      ["ctrl:Random()", "随机变量：每次引用随机取 1~100 的整数（固定变量）"],
      ["ctrl:Hour()", "当前小时：本地时 0–23（固定变量）"],
      ["ctrl:Minute()", "当前分钟：本地时 0–59（固定变量）"],
      ["ctrl:Clipboard()", "剪贴板：条件里有内容为1否则0；输入展开文字或全部文件路径；AI图片分析/动作可附图（固定变量）"],
    ];
    fixed.forEach(([code, tip]) => push(code, tip));
    if (!items.length) {
      items.push({
        t: "（暂无可用变量）",
        v: "",
        code: "",
        d: "固定变量已内置：宏次数/随机/时分/剪贴板",
      });
    }
    return items;
  }

  function requestFindImageThumbs(root) {
    const scope = root || document;
    state._pendingThumbById = state._pendingThumbById || {};
    $$("img.fi-thumb[data-img-path]", scope).forEach((img) => {
      const p = img.dataset.imgPath;
      if (!p || !window.qst) return;
      const reqId = "thumb_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8);
      img.dataset.reqId = reqId;
      state._pendingThumbById[reqId] = img;
      qst.readImageDataUrl(p, reqId);
    });
  }

  function cssToColorref(hex) {
    const m = /^#?([0-9a-f]{6})$/i.exec(String(hex || "").trim());
    if (!m) return 0;
    const n = parseInt(m[1], 16);
    const r = (n >> 16) & 255;
    const g = (n >> 8) & 255;
    const b = n & 255;
    return r | (g << 8) | (b << 16);
  }

  function readParamPanelInto(action) {
    const panel = $("#paramPanel");
    if (!panel || !action) return;
    $$("[data-k]", panel).forEach((el) => {
      const k = el.dataset.k;
      if (!k) return;
      if (k.startsWith("_if") && k !== "_ifValue") return;
      let v = (el.textContent || "").trim();
      if (el.tagName === "INPUT" || el.tagName === "TEXTAREA") {
        v = el.value;
      }
      if (k === "_ifValue") {
        action._ifValue = v;
        return;
      }
      if (el.classList.contains("combo")) {
        if (k === "button") {
          if (v.includes("侧键2") || v.toLowerCase().includes("x2")) action.button = "x2";
          else if (v.includes("侧键1") || v.toLowerCase().includes("x1")) action.button = "x1";
          else if (v.includes("右")) action.button = "right";
          else if (v.includes("中")) action.button = "middle";
          else action.button = "left";
          return;
        }
        if (k === "aiOutputType") {
          action.aiOutputType = v.includes("数字") ? 1 : 0;
          return;
        }
        if (k === "aiContextMode") {
          const map = { 无: 0, 宏: 1, 循环: 2, 块: 3 };
          const hit = Object.keys(map).find((t) => v.includes(t));
          action.aiContextMode = hit != null ? map[hit] : 0;
          return;
        }
        if (k === "scrollDirection") {
          action.scrollDirection = v.includes("下") || v.includes("右") ? 1 : 0;
          return;
        }
        if (k === "useMode") {
          if (v.includes("继承")) action.useMode = 3;
          else if (v.includes("后台")) action.useMode = 2;
          else if (v.includes("窗口")) action.useMode = 1;
          else action.useMode = 0;
          return;
        }
        if (k === "_nwmSelectMethod") {
          action._nwmSelectMethod = v;
          return;
        }
        if (k === "findImageFollowUp") {
          if (action.type === "findColor") {
            if (v.includes("变量")) action.findImageFollowUp = 2;
            else if (v.includes("移动")) action.findImageFollowUp = 1;
            else action.findImageFollowUp = 0;
            return;
          }
          if (v.includes("图片")) action.findImageFollowUp = 3;
          else if (v.includes("匹配度") || v.includes("变量")) action.findImageFollowUp = 2;
          else if (v.includes("移动")) action.findImageFollowUp = 1;
          else action.findImageFollowUp = 0;
          return;
        }
        if (k === "ocrResultMode") {
          action.ocrResultMode = v.includes("查找") ? 1 : 0;
          return;
        }
        if (k === "ocrFollowUp") {
          if (v.includes("变量")) action.ocrFollowUp = 2;
          else if (v.includes("移动")) action.ocrFollowUp = 1;
          else action.ocrFollowUp = 0;
          return;
        }
        if (k === "shortcutPreset") {
          const n = parseInt(el.dataset.v || v, 10);
          if (!Number.isNaN(n)) action.shortcutPreset = n;
          action.keyText = v;
          return;
        }
        action[k] = v;
        if (k === "inputText" && (action.type === "findColor" || action.type === "colorMatch")) {
          const m = String(v || "").trim().replace(/^#/, "").match(/^([0-9a-fA-F]{6})$/);
          if (m) {
            const n = parseInt(m[1], 16);
            action.colorR = (n >> 16) & 255;
            action.colorG = (n >> 8) & 255;
            action.colorB = n & 255;
          } else {
            const parts = String(v || "").split(/[,，;|]/).map((s) => parseInt(s.trim(), 10));
            if (parts.length >= 3 && parts.every((x) => Number.isFinite(x))) {
              action.colorR = Math.max(0, Math.min(255, parts[0]));
              action.colorG = Math.max(0, Math.min(255, parts[1]));
              action.colorB = Math.max(0, Math.min(255, parts[2]));
            }
          }
        }
        return;
      }
      if (el.classList.contains("chk")) {
        action[k] = el.classList.contains("on") ? 1 : 0;
        return;
      }
      if (
        [
          "x",
          "y",
          "randomX",
          "randomY",
          "clickCount",
          "keyVk",
          "searchX1",
          "searchY1",
          "searchX2",
          "searchY2",
          "imageRegionX1",
          "imageRegionY1",
          "imageRegionX2",
          "imageRegionY2",
          "offsetX",
          "offsetY",
          "loopCount",
          "scrollSteps",
          "findImageFollowUp",
          "searchFullScreen",
          "aiOutputType",
          "aiContextMode",
          "aiTimeoutSec",
          "aiMaxSteps",
          "aiSearchX1",
          "aiSearchY1",
          "aiSearchX2",
          "aiSearchY2",
          "aiRegionByImage",
          "aiWithImage",
          "aiLogicConvert",
          "aiSearchRegion",
          "moveFromVar",
          "loopFromVar",
          "parseEscapes",
          "scrollVertical",
          "scrollHorizontal",
          "ocrRegionByImage",
          "ocrDigitsOnly",
          "ocrResultMode",
          "ocrFollowUp",
          "findUntilFound",
          "matchFileNameOnly",
          "holdLeftWin",
          "holdRightWin",
          "holdLeftCtrl",
          "holdRightCtrl",
          "holdLeftAlt",
          "holdRightAlt",
          "holdLeftShift",
          "holdRightShift",
          "shortcutPreset",
          "colorTolerance",
          "colorR",
          "colorG",
          "colorB",
        ].includes(k)
      ) {
        action[k] = parseInt(v, 10) || 0;
      } else if (
        [
          "duration",
          "randomDuration",
          "matchThreshold",
          "charInterval",
          "aiImageScale",
          "imageScaleMin",
          "imageScaleMax",
          "breakoutTimeSeconds",
        ].includes(k)
      ) {
        let n = parseFloat(v);
        if (!(n > 0) && k === "matchThreshold") n = 65;
        if (k === "matchThreshold") {
          if (n <= 1) n *= 100;
          if (n < 1) n = 1;
          if (n > 100) n = 100;
        }
        action[k] = n || 0;
      } else {
        action[k] = v;
      }
    });
    const edSpeed = panel.querySelector('.speed-slider[data-speed-scope="action"]');
    if (edSpeed && action.type === "mousePlayback") {
      action.playbackSpeed = clampPlaybackSpeedUi(parseFloat(edSpeed.dataset.speed));
    }
    if (action.scrollVertical && action.scrollHorizontal) {
      /* keep both if user set both */
    } else if (!action.scrollVertical && !action.scrollHorizontal) {
      action.scrollVertical = 1;
    }
    // customText → 磁盘 text（ParseScriptActionBlock 只认 text）
    if (action.type === "customText") {
      if (action.customText != null) action.text = String(action.customText);
      else if (action.text != null) action.customText = String(action.text);
    }
    if (action.type === "runBlock" && action._runBlockTyped != null) {
      const typed = String(action._runBlockTyped || "").trim();
      if (typed) action.blockName = typed;
      delete action._runBlockTyped;
    }
    if (action.type === "textRecognition" && action.ocrRegionByImage) {
      // search* 始终为绝对识别/找图区；不再因勾选强制 searchFullScreen=0
    }
    if (action.type === "aiActionExecute") {
      action.aiImageScale = 0.5;
      // 对齐原生：aiRegionByImage = aiWithImage && 勾选
      if (!action.aiWithImage) {
        action.aiRegionByImage = 0;
        action.aiTargetImagePath = "";
      }
    }
    if (action.aiTimeoutSec != null) {
      const t = Number(action.aiTimeoutSec);
      if (Number.isFinite(t)) action.aiTimeoutSec = Math.max(5, t | 0);
    }
    if (
      action.imageScaleMin != null &&
      action.imageScaleMax != null &&
      (action.type === "findImage" ||
        action.type === "textRecognition" ||
        action.type === "aiImageAnalysis" ||
        action.type === "aiActionExecute")
    ) {
      const lo = Number(action.imageScaleMin);
      const hi = Number(action.imageScaleMax);
      if (Number.isFinite(lo) && Number.isFinite(hi)) {
        action.imageScale = (lo + hi) * 0.5;
      }
    }
    if (
      action.type === "findImage" &&
      (action.findImageFollowUp | 0) === 3 &&
      !findImageHasTemplate(action)
    ) {
      action.findTimeExpr = "0";
    }
    if (action.type === "runMacro" || action.type === "mousePlayback") {
      applyNestedWmTempFields(action);
    }
  }

  function nestedUseModeLabel(mode) {
    const m = NESTED_USE_MODES.find((x) => x.v === (mode | 0));
    return (m || NESTED_USE_MODES[3]).t;
  }

  function ensureNestedWindowMode(a) {
    if (!a.nestedWindowMode || typeof a.nestedWindowMode !== "object") {
      a.nestedWindowMode = {};
    }
    const wm = a.nestedWindowMode;
    wm.selectMethod = normalizeWmSelectMethod(wm.selectMethod);
    if (wm.targetExePath == null) wm.targetExePath = "";
    if (wm.windowTitle == null) wm.windowTitle = wm.windowName || "";
    if (wm.windowName == null) wm.windowName = wm.windowTitle || "";
    if (wm.windowClassName == null) wm.windowClassName = "";
    if (wm.childWindowClassName == null) wm.childWindowClassName = "";
    if (wm.fakeFocusEnabled == null) wm.fakeFocusEnabled = 0;
    return wm;
  }

  function applyNestedWmTempFields(action) {
    const wm = ensureNestedWindowMode(action);
    if (action._nwmSelectMethod != null) {
      wm.selectMethod = resolveWmSelectMethodForSave(
        wm.selectMethod,
        action._nwmSelectMethod
      );
    }
    if (action._nwmTargetExePath != null)
      wm.targetExePath = String(action._nwmTargetExePath || "").trim();
    if (action._nwmWindowTitle != null) {
      const title = String(action._nwmWindowTitle || "").trim();
      wm.windowTitle = title;
      wm.windowName = title;
      wm.targetWindowTitle = title;
    }
    if (action._nwmWindowClassName != null)
      wm.windowClassName = String(action._nwmWindowClassName || "").trim();
    if (action._nwmFakeFocus != null)
      wm.fakeFocusEnabled = action._nwmFakeFocus ? 1 : 0;
    delete action._nwmSelectMethod;
    delete action._nwmTargetExePath;
    delete action._nwmWindowTitle;
    delete action._nwmWindowClassName;
    delete action._nwmFakeFocus;
    action.nestedWindowMode = wm;
  }

  function applyTargetScriptUseMode(a, meta) {
    if (!a || !meta) return;
    const mode = typeof meta.mode === "number" ? meta.mode | 0 : 0;
    if (mode === 1 || mode === 2) {
      a.useMode = mode;
      const src = meta.windowMode && typeof meta.windowMode === "object" ? meta.windowMode : {};
      a.nestedWindowMode = Object.assign({}, src);
      a.nestedWindowMode.enabled = 1;
      a.nestedWindowMode.executionKind =
        mode === 2 ? "backgroundWindow" : "hiddenDesktop";
      a.nestedWindowMode.selectMethod = normalizeWmSelectMethod(
        a.nestedWindowMode.selectMethod
      );
      const title =
        src.windowTitle ||
        src.windowName ||
        src.targetWindowTitle ||
        "";
      a.nestedWindowMode.windowTitle = title;
      if (!a.nestedWindowMode.windowName) {
        a.nestedWindowMode.windowName = title;
      }
    } else {
      a.useMode = 0;
      a.nestedWindowMode = {};
      if (meta.breakoutTimeSeconds != null) {
        const n = parseFloat(meta.breakoutTimeSeconds);
        a.breakoutTimeSeconds = Number.isFinite(n) && n > 0 ? n : 0;
      }
    }
  }

  function nestedUseModeHtml(a) {
    const mode = a.useMode == null ? 3 : a.useMode | 0;
    let html = flineStack("使用模式", comboField("useMode", nestedUseModeLabel(mode)));
    if (mode === 3) {
      html += `<p class="hint">*继承当前主鼠标宏的模式与窗口绑定</p>`;
      return html;
    }
    if (mode === 0) {
      html += flineStack(
        "脱离时间",
        inpField("breakoutTimeSeconds", a.breakoutTimeSeconds ?? 0, "sm") +
          `<span class="tl">秒</span>`
      );
      return html;
    }
    const wm = ensureNestedWindowMode(a);
    const method = WM_METHODS.find((m) => m.v === wm.selectMethod) || WM_METHODS[0];
    const targetLab = mode === 2 ? "目标窗口" : "目标程序";
    html += flineStack("选择窗口方式", comboField("_nwmSelectMethod", method.t));
    html += `<div class="fline-stack nwm-full"><div class="stack-lab nwm-lab"><span>${esc(
      targetLab
    )}</span>${chkLabeled(
      "_nwmFakeFocus",
      "聚焦",
      !!wm.fakeFocusEnabled,
      ' title="向目标进程注入假焦点（联机/反作弊游戏请勿使用，易被扫描）。远程桌面(mstsc)请勿开启——会搞挂客户端；mstsc 回放会自动改用前台本机键鼠"'
    )}</div><div class="stack-ctrl">${inpField(
      "_nwmTargetExePath",
      wm.targetExePath || "",
      "full"
    )}</div></div>`;
    html += `<div class="fline wrap nwm-btns">`;
    html += `<button type="button" class="btn ghost sm" data-fi="nwmBrowse">浏览</button>`;
    html += `<button type="button" class="btn ghost sm" data-fi="nwmPick">${
      mode === 2 ? "准星绑定窗口" : "准星找程序"
    }</button>`;
    html += `<button type="button" class="btn ghost sm" data-fi="nwmClass">指定窗口类</button>`;
    html += `</div>`;
    html += `<div class="fline-stack nwm-full"><div class="stack-lab">窗口标题</div><div class="stack-ctrl">${inpField(
      "_nwmWindowTitle",
      wm.windowTitle || wm.windowName || "",
      "full"
    )}</div></div>`;
    html += `<div class="fline-stack nwm-full"><div class="stack-lab">窗口类名</div><div class="stack-ctrl">${inpField(
      "_nwmWindowClassName",
      wm.windowClassName || "",
      "full"
    )}</div></div>`;
    return html;
  }

  function chkField(key, on) {
    return `<div class="chk ${on ? "on" : ""}" data-k="${esc(key)}"><i>✓</i></div>`;
  }

  function renderParamPanel(a) {
    const panel = $("#paramPanel");
    const tag = $("#paramTag");
    if (!panel) return;
    if (!a) {
      panel.innerHTML =
        '<div class="panel-empty">在左侧选中一条动作后，在此编辑参数</div>';
      if (tag) tag.textContent = "未选择";
      return;
    }
    // 详情栏标题只用简短类型名；带参数的完整名仅出现在左侧动作列表
    const brief = typeLabel(a.type);
    if (tag) tag.textContent = brief;
    const t = a.type || "";
    let html = `<div class="pf-sec"><div class="param-flow">`;
    // 备注仅在列表下方 #edRemark，右栏不再重复

    if (t === "moveMouse") {
      html += labOnly("移动到(左上角为0,0)");
      html += flineCoord("X:", "x", "randomX", a);
      html += flineCoord("Y:", "y", "randomY", a);
      html += `<div class="fline"><button type="button" class="btn primary crosshair-btn" data-fi="pickCoord">拖动准星获取坐标</button></div>`;
      html += `<div class="fline wrap">${chkLabeled(
        "moveFromVar",
        "来自变量表达式",
        !!a.moveFromVar
      )}</div>`;
      if (a.moveFromVar) {
        html += flineVarExpr("X:", "moveVarExprX", a);
        html += flineVarExpr("Y:", "moveVarExprY", a);
        html += `<p class="hint">*提示:可使用来自找图、找色，获取颜色，文字识别保存到变量中的值</p>`;
      }
    } else if (t === "moveMouseRelative") {
      html += labOnly("相对位移(像素,可负)");
      html += flineCoord("dx:", "x", "randomX", a);
      html += flineCoord("dy:", "y", "randomY", a);
      html += `<p class="hint">*提示:Raw 逐包录制；回放关加速+绝对时间轴+高优先级。请重新录制后验证</p>`;
    } else if (t === "mouseClick" || t === "mouseDown" || t === "mouseUp") {
      const btnMap = {
        left: "左键",
        right: "右键",
        middle: "中键",
        x1: "侧键1",
        x2: "侧键2",
      };
      const btnText = btnMap[a.button] || "左键";
      html += labOnly("选择鼠标键");
      html += fline("", comboField("button", btnText));
      html += labOnly("同时按住");
      html += modBlock(a);
      if (t === "mouseClick") html += repeatBlock(a);
    } else if (t === "wait") {
      html += flineStack(
        "等待时间",
        inpField("duration", a.duration ?? 0.5) + `<span class="tl">秒</span>`
      );
      html += flineStack(
        "最大随机时间",
        inpField("randomDuration", a.randomDuration ?? 0) + `<span class="tl">秒</span>`
      );
      html += `<p class="hint">*提示:等待总时间=等待时间+随机(0~最大随机时间)</p>`;
    } else if (t === "keyClick" || t === "keyDown" || t === "keyUp") {
      const keyLab = a.keyText || "捕获按键";
      html += labOnly("选择键盘按键");
      html += `<div class="fline"><button type="button" class="btn primary fluid" data-fi="captureKey">${esc(keyLab)}</button></div>`;
      html += labOnly("同时按住");
      html += modBlock(a);
      if (t === "keyClick") html += repeatBlock(a);
    } else if (t === "getColor") {
      html += flineXy("X", inpField("x", a.x ?? 0), "Y", inpField("y", a.y ?? 0));
      html += fline("保存到", inpField("matchVarName", a.matchVarName || "colorRet"));
      html += `<p class="hint">*提示:读取屏幕坐标颜色，写入变量（#RRGGBB）</p>`;
    } else if (t === "findColor") {
      const followLabs = ["点击", "鼠标移动到", "保存到变量"];
      const fu = Math.min(2, Math.max(0, a.findImageFollowUp | 0));
      ensureFullScreenCoordsVisible(a);
      html += fline("目标颜色", inpField("inputText", a.inputText || `#${((a.colorR|0)<<16|(a.colorG|0)<<8|(a.colorB|0)).toString(16).padStart(6,"0")}`));
      html += fline("容差", inpField("colorTolerance", a.colorTolerance ?? 16));
      html += `<div class="fline"><span class="tl grow-lab">搜索区域</span><div class="inline-btns">
        <button type="button" class="btn ghost sm" data-fi="full">全图</button>
        <button type="button" class="btn ghost sm" data-fi="region">选取区域</button>
      </div></div>`;
      html += flineXy("X1", inpField("searchX1", a.searchX1 ?? 0), "Y1", inpField("searchY1", a.searchY1 ?? 0));
      html += flineXy("X2", inpField("searchX2", a.searchX2 ?? 0), "Y2", inpField("searchY2", a.searchY2 ?? 0));
      html += fline("后续操作", comboField("findImageFollowUp", followLabs[fu]));
      html += fline("变量名", inpField("matchVarName", a.matchVarName || "colorRet"));
    } else if (t === "colorMatch") {
      html += flineXy("X", inpField("x", a.x ?? 0), "Y", inpField("y", a.y ?? 0));
      html += fline("目标颜色", inpField("inputText", a.inputText || `#${((a.colorR|0)<<16|(a.colorG|0)<<8|(a.colorB|0)).toString(16).padStart(6,"0")}`));
      html += fline("容差", inpField("colorTolerance", a.colorTolerance ?? 16));
      html += fline("变量名", inpField("matchVarName", a.matchVarName || "colorRet"));
    } else if (t === "findImage") {
      const followLabs = ["点击", "鼠标移动到", "保存匹配度", "保存图片"];
      const fu = Math.min(3, Math.max(0, a.findImageFollowUp | 0));
      const hasTpl = findImageHasTemplate(a);
      const regionLab = fu === 3 && !hasTpl ? "截图区域" : "找图区域";
      ensureFullScreenCoordsVisible(a);
      html += `<div class="fline"><span class="tl grow-lab">${esc(regionLab)}</span><div class="inline-btns">
        <button type="button" class="btn ghost sm" data-fi="full">全图</button>
        <button type="button" class="btn ghost sm" data-fi="region">选取区域</button>
      </div></div>`;
      html += flineXy(
        "X1",
        inpField("searchX1", a.searchX1 ?? 0),
        "Y1",
        inpField("searchY1", a.searchY1 ?? 0)
      );
      html += flineXy(
        "X2",
        inpField("searchX2", a.searchX2 ?? 0),
        "Y2",
        inpField("searchY2", a.searchY2 ?? 0)
      );
      html += findImageModuleHtml(a);
      html += findImageVarPathHtml(a);
      html += matchThresholdRowHtml(a);
      html += flineXy(
        "最小",
        inpField("imageScaleMin", a.imageScaleMin ?? 1),
        "最大",
        inpField("imageScaleMax", a.imageScaleMax ?? 1)
      );
      if (fu === 3 && hasTpl) {
        html += imageRegionRelHtml(a);
      }
      html += fline("后续操作", comboField("findImageFollowUp", followLabs[fu]));
      if (fu === 2) {
        html += flineStack(
          "匹配度保存到",
          inpField("matchVarName", a.matchVarName || "matchRet")
        );
      } else if (fu === 3) {
        const saveImg =
          !a.matchVarName || a.matchVarName === "matchRet" ? "image" : a.matchVarName;
        if (a.matchVarName !== saveImg) a.matchVarName = saveImg;
        html += labOnly("图片保存到");
        html += area3rowHtml("matchVarName", saveImg);
      } else {
        html += flineXy(
          "X偏",
          inpField("offsetX", a.offsetX ?? 0),
          "Y偏",
          inpField("offsetY", a.offsetY ?? 0)
        );
        html += `<div class="fline"><button type="button" class="btn ghost fluid" data-fi="offset">选择偏移点击位置</button></div>`;
      }
      if (findImageShowsFindTime(a)) html += findImageTimeRowHtml(a);
    } else if (t === "quickInput") {
      const parseOn = a.parseEscapes == null ? false : !!a.parseEscapes;
      html += `<div class="fline"><span class="tl">要输入的文字</span>${chkLabeled(
        "parseEscapes",
        "解析转义符",
        parseOn
      )}</div>`;
      html += area3rowHtml("inputText", a.inputText || "");
      html += `<div class="fline"><button type="button" class="btn ghost fluid" data-fi="insertVar" data-target="inputText">插入变量</button></div>`;
      html += `<p class="hint">*解析转义同时作用于插入的变量。未勾选时，变量里的换行/Tab 会丢掉，不输入。</p>`;
      html += flineStack(
        "字输入间隔(秒)",
        inpField("charInterval", a.charInterval ?? 0.01)
      );
      html += repeatBlock(a);
    } else if (t === "loop") {
      html += flineStack("循环次数", inpField("loopCount", a.loopCount ?? -1));
      html += `<p class="hint">*提示:-1表示无限循环</p>`;
      html += flineStack(
        "循环变量命名",
        inpField("loopVarName", a.loopVarName || "")
      );
      html += `<div class="fline wrap">${chkLabeled(
        "loopFromVar",
        "来自变量表达式",
        !!a.loopFromVar
      )}</div>`;
      if (a.loopFromVar) {
        html += flineStack(
          "次数表达式",
          inpField("loopVarExpr", a.loopVarExpr || "")
        );
      }
    } else if (t === "scrollWheel") {
      html += `<div class="fline scroll-axis">${chkLabeled(
        "scrollVertical",
        "垂直",
        a.scrollVertical !== 0 && a.scrollVertical !== false
      )}${chkLabeled("scrollHorizontal", "水平", !!a.scrollHorizontal)}</div>`;
      html += flineStack("滚动步数", inpField("scrollSteps", a.scrollSteps ?? 1));
      const dirLab = (a.scrollDirection | 0) === 1 ? "向下/右" : "向上/左";
      html += flineStack("滚动方向", comboField("scrollDirection", dirLab));
      html += repeatBlock(a);
    } else if (t === "hotkeyShortcut") {
      const SHORTCUT_PRESETS = [
        "Ctrl+C(拷贝)",
        "Ctrl+V(粘贴)",
        "Ctrl+X(剪切)",
        "Ctrl+S(保存)",
        "Ctrl+F(查找)",
        "Alt+F4(关闭窗口)",
        "Win+D(所有最小化)",
        "Win+R(打开运行)",
        "Ctrl+Alt+Delete",
      ];
      const idx = Math.min(
        SHORTCUT_PRESETS.length - 1,
        Math.max(0, a.shortcutPreset | 0)
      );
      const lab = a.keyText || SHORTCUT_PRESETS[idx];
      html += fline("快捷按键", comboField("shortcutPreset", lab));
      html += repeatBlock(a);
    } else if (t === "textRecognition") {
      const searchMode = (a.ocrResultMode | 0) === 1;
      const saveVar = (a.ocrFollowUp | 0) === 2;
      const regionByImage = !!a.ocrRegionByImage;
      html += `<div class="fline"><button type="button" class="btn primary fluid" data-fi="ocrInstall">安装/修复 OCR 插件</button></div>`;
      html += `<div class="fline wrap">${chkLabeled(
        "ocrRegionByImage",
        "根据图片选取区域",
        regionByImage
      )}${chkLabeled("ocrDigitsOnly", "纯数字", !!a.ocrDigitsOnly)}</div>`;
      if (regionByImage) {
        html += findImageModuleHtml(a, "ocr");
        html += findImageVarPathHtml(a, "ocr");
        html += matchThresholdRowHtml(a);
        html += flineXy(
          "最小",
          inpField("imageScaleMin", a.imageScaleMin ?? 1),
          "最大",
          inpField("imageScaleMax", a.imageScaleMax ?? 1)
        );
        html += imageRegionRelHtml(a);
      }
      html += absSearchRegionHtml(a);
      const ocrMode = searchMode ? "文字查找" : "获取文字";
      html += fline("结果处理", comboField("ocrResultMode", ocrMode));
      if (searchMode) {
        html += fline(
          "查找文字",
          `<div class="row" style="gap:6px;flex:1;min-width:0">${inpField(
            "ocrSearchText",
            a.ocrSearchText || "",
            120
          )}<button type="button" class="btn ghost sm" data-fi="insertVar" data-target="ocrSearchText">插入变量</button></div>`
        );
      }
      const ocrFuLabs = ["点击", "鼠标移动到", "保存到变量"];
      const ofu = Math.min(2, Math.max(0, a.ocrFollowUp | 0));
      html += fline("后续操作", comboField("ocrFollowUp", ocrFuLabs[ofu]));
      if (searchMode) {
        html += `<div class="fline wrap">${chkLabeled(
          "findUntilFound",
          "直到找到为止",
          !!a.findUntilFound
        )}</div>`;
      }
      if (!saveVar) {
        html += flineXy(
          "X偏",
          inpField("offsetX", a.offsetX ?? 0),
          "Y偏",
          inpField("offsetY", a.offsetY ?? 0)
        );
        if (!searchMode) {
          html += `<div class="fline"><button type="button" class="btn ghost fluid" data-fi="ocrOffset">选择偏移位置</button></div>`;
        }
      } else {
        html += flineStack("结果保存到", inpField("matchVarName", a.matchVarName || "a"));
      }
      html += `<div class="fline"><button type="button" class="btn primary crosshair-btn" data-fi="ocrTest">测试</button></div>`;
    } else if (t === "customText") {
      html += fline("显示文本", inpField("customText", a.customText || a.text || "", 180));
    } else if (t === "if") {
      const vars = buildEditorVarItems();
      const varLab = (a._ifVarLabel || vars[0]?.t || "请选择").replace(/^（/, "请选择 · ");
      const opLab = a._ifOpLabel || "等于";
      const connLab = a._ifConnLabel || "并且(and)";
      html += fline("变量", `<div class="combo ed-full" data-k="_ifVar">${esc(
        vars[0] && vars[0].code ? varLab : "请选择"
      )}</div>`);
      html += fline(
        "判断",
        `<div class="combo ed-full" data-k="_ifOp">${esc(opLab)}</div>`
      );
      html += fline("值", inpField("_ifValue", a._ifValue ?? "0", 160));
      html += labOnly("多条件连接方式");
      html += fline(
        "",
        `<div class="combo ed-full" data-k="_ifConn">${esc(connLab)}</div>`
      );
      html += `<div class="fline"><button type="button" class="btn primary fluid" data-fi="ifAdd">添加判断条件</button></div>`;
      html += labOnly("判断条件");
      html += `<div class="fline"><div class="inp ed-area" contenteditable="true" data-k="conditionExpr">${esc(
        a.conditionExpr || ""
      )}</div></div>`;
      html += `<p class="hint">*提示:如需要更复杂的条件判断，请导出脚本操作</p>`;
    } else if (t === "mousePlayback") {
      const recLabel =
        a.blockName ||
        (a.targetPath ? a.targetPath.split(/[/\\]/).pop() : "") ||
        "请选择用于回放的键鼠录制";
      html += flineStack("录制脚本", comboField("blockName", recLabel));
      html += nestedUseModeHtml(a);
      const spd = a.playbackSpeed == null ? 1 : a.playbackSpeed;
      html +=
        `<div class="fline-stack ed-speed-stack"><div class="stack-lab">回放倍速</div>` +
        `<div class="stack-ctrl">${speedSliderInnerHtml("edPlaySpeedSlider", "action", spd)}` +
        `<span class="speed-val">${esc(formatPlaybackSpeed(spd))}</span></div></div>`;
      html += repeatBlock(a);
    } else if (t === "runMacro") {
      const macLabel =
        a.blockName ||
        (a.targetPath ? a.targetPath.split(/[/\\]/).pop() : "") ||
        "请选择用于运行的鼠标宏";
      html += flineStack("鼠标宏", comboField("blockName", macLabel));
      html += nestedUseModeHtml(a);
      html += repeatBlock(a);
    } else if (t === "defineBlock") {
      html += flineStack(
        "宏指令块名称",
        inpField("blockName", a.blockName || "block1")
      );
      html += `<p class="hint">*提示:块名称不能重复；须以字母开头</p>`;
    } else if (t === "runBlock") {
      const blockNames = [];
      const seenBlk = new Set();
      (state.editorActions || []).forEach((x) => {
        if (!x || x.type !== "defineBlock") return;
        const n = String(x.blockName || "").trim();
        if (!n || seenBlk.has(n)) return;
        seenBlk.add(n);
        blockNames.push(n);
      });
      const cur = a.blockName || blockNames[0] || "block1";
      html += fline("要运行的块名称", comboField("blockName", cur));
      if (!blockNames.length) {
        html += `<p class="hint">*暂无已定义块；可在下方手输名称</p>`;
        html += fline("手输块名", inpField("_runBlockTyped", cur, 160));
      } else if (!seenBlk.has(String(cur).trim())) {
        html += `<p class="hint">*当前名称不在已定义块中</p>`;
        html += fline("手输块名", inpField("_runBlockTyped", cur, 160));
      }
      html += repeatBlock(a);
    } else if (t === "runProgram") {
      const RUN_PRESETS = [
        { t: "选择文件", v: 0, path: "" },
        { t: "快捷运行-记事本", v: 1, path: "notepad.exe" },
        { t: "快捷运行-计算器", v: 2, path: "calc.exe" },
        { t: "快捷运行-画图", v: 3, path: "mspaint.exe" },
        { t: "快捷运行-文件管理器", v: 4, path: "explorer.exe" },
        { t: "快捷运行-命令行", v: 5, path: "cmd.exe" },
        { t: "快捷运行-PowerShell", v: 6, path: "powershell.exe" },
        { t: "快捷运行-进程管理器", v: 7, path: "taskmgr.exe" },
        { t: "快捷运行-注册表编辑器", v: 8, path: "regedit.exe" },
        { t: "快捷运行-服务", v: 9, path: "services.msc" },
        { t: "快捷运行-计算机管理", v: 10, path: "compmgmt.msc" },
        { t: "快捷运行-控制面板", v: 11, path: "control.exe" },
        { t: "快捷运行-设置", v: 12, path: "ms-settings:" },
      ];
      const pi = Math.min(
        RUN_PRESETS.length - 1,
        Math.max(0, a.shortcutPreset | 0)
      );
      const presetLab = RUN_PRESETS[pi].t;
      const pathShow =
        pi > 0 ? RUN_PRESETS[pi].path : a.targetPath || "";
      html += flineStack("快捷运行", comboField("runProgramPreset", presetLab));
      html += labOnly("程序路径");
      html += `<div class="fline">${inpField("targetPath", pathShow, "full")}</div>`;
      html += `<div class="fline"><button type="button" class="btn ghost fluid" data-fi="browseExe">选择程序</button></div>`;
      html += `<div class="fline"><button type="button" class="btn primary crosshair-btn" data-fi="pickProg">拖动准星查找程序</button></div>`;
      html += labOnly("运行参数");
      html += `<div class="fline">${inpField("inputText", a.inputText || "", "full")}</div>`;
      if (pi > 0) {
        html += `<p class="hint">*当前为快捷预设：运行时忽略下方自定义路径，使用「${esc(
          RUN_PRESETS[pi].path
        )}」</p>`;
      }
    } else if (t === "closeProgram") {
      html += labOnly("程序路径");
      html += `<div class="fline">${inpField("targetPath", a.targetPath || "", "full")}</div>`;
      html += `<div class="fline"><button type="button" class="btn ghost fluid" data-fi="browseExe">选择程序</button></div>`;
      html += `<div class="fline"><button type="button" class="btn primary crosshair-btn" data-fi="pickProg">拖动准星查找程序</button></div>`;
      html += `<div class="fline wrap">${chkLabeled(
        "matchFileNameOnly",
        "仅匹配文件名",
        !!a.matchFileNameOnly
      )}</div>`;
    } else if (t === "openWebpage") {
      html += labOnly("网页 URL");
      html += `<div class="fline">${inpField("targetPath", a.targetPath || "https://", "full")}</div>`;
      html += `<p class="hint">*提示：支持 http:// 或 https://</p>`;
    } else if (t === "openFile") {
      html += labOnly("文件路径");
      html += `<div class="fline">${inpField("targetPath", a.targetPath || "", "full")}</div>`;
      html += `<div class="fline"><button type="button" class="btn ghost fluid" data-fi="browseFile">选择文件</button></div>`;
    } else if (t === "activateWindow") {
      html += labOnly("窗口标题/进程关键词");
      html += `<div class="fline">${inpField("targetPath", a.targetPath || "", "full")}</div>`;
      html += `<p class="hint">*提示：子串匹配已打开窗口标题或进程名，切到前台</p>`;
    } else if (t === "goto") {
      html += flineStack("目标动作序号", inpField("gotoStepExpr", a.gotoStepExpr || "1"));
      html += `<p class="hint">*提示:支持变量名或 {变量} 表达式</p>`;
    } else if (t === "getCursorPos") {
      html += flineStack("变量命名", inpField("matchVarName", a.matchVarName || "a"));
      html += `<p class="hint">*提示: 可用 {a.x}、{a.y}</p>`;
    } else if (t === "timerRecordTime") {
      const timerVar =
        a.loopVarName ||
        a.matchVarName /* 旧 Web 误用 matchVarName */ ||
        "t";
      html += flineStack("计时器变量命名", inpField("loopVarName", timerVar));
      html += `<p class="hint">*提示: 记录经过秒数(向下取整)</p>`;
    } else if (t === "aiTextAnalysis" || t === "aiImageAnalysis" || t === "aiActionExecute") {
      const outType = (a.aiOutputType | 0) === 1 ? "数字" : "文本";
      const ctxLabels = ["无上下文", "宏上下文", "循环上下文", "块上下文"];
      const ctx = ctxLabels[Math.min(3, Math.max(0, a.aiContextMode | 0))] || ctxLabels[0];
      const modelLabel = a.aiModelName || state.agentModel || "默认模型";
      const showImageBlock =
        t === "aiImageAnalysis" || (t === "aiActionExecute" && !!a.aiWithImage);
      const showFindBlock = showImageBlock && !!a.aiRegionByImage;
      const isActionExec = t === "aiActionExecute";

      html += labOnly("提示词");
      html += `<div class="fline"><div class="inp ed-area fixed-3row" contenteditable="true" data-k="aiPrompt">${esc(
        a.aiPrompt || ""
      )}</div></div>`;
      html += `<div class="fline"><button type="button" class="btn ghost fluid" data-fi="insertVar" data-target="aiPrompt">插入变量</button></div>`;
      if (isActionExec) {
        html += flineStack("最大步骤数", inpField("aiMaxSteps", a.aiMaxSteps ?? 10));
        html += `<p class="hint">*提示：-1 表示不限制步数</p>`;
      }
      // 对齐原生：aiActionExecute 隐藏输出变量/类型
      if (!isActionExec) {
        html += flineStack(
          "输出变量名",
          inpField("aiOutputVarName", a.aiOutputVarName || "aiResult")
        );
        html += fline("输出类型", comboField("aiOutputType", outType));
      }
      html += fline("AI 模型", comboField("aiModelName", modelLabel));
      html += fline("上下文模式", comboField("aiContextMode", ctx));
      html += flineStack("超时(秒)", inpField("aiTimeoutSec", a.aiTimeoutSec ?? 30));
      html += flineStack(
        "降级值(失败时使用)",
        inpField("aiFallbackValue", a.aiFallbackValue || "")
      );

      if (isActionExec) {
        html += `<div class="fline wrap">${chkLabeled(
          "aiWithImage",
          "附带截图",
          !!a.aiWithImage
        )}</div>`;
      }

      if (showImageBlock) {
        // 原生仅 aiImageAnalysis 有缩放 UI；aiActionExecute 固定 0.5
        if (t === "aiImageAnalysis") {
          html += flineStack(
            "截屏缩放(0.1-1.0)",
            inpField("aiImageScale", a.aiImageScale ?? 1)
          );
        } else {
          a.aiImageScale = 0.5;
        }
        // 识别区域（绝对）在上；「根据图片」勾选在下，避免两套 X1Y1X2Y2 混淆
        html += absSearchRegionHtml(a, "aiSearch");
        html += `<div class="fline wrap">${chkLabeled(
          "aiRegionByImage",
          "根据图片选取区域",
          !!a.aiRegionByImage
        )}</div>`;
      }
      if (showFindBlock) {
        html += findImageModuleHtml(a, "ai");
        html += findImageVarPathHtml(a, "ai");
        html += matchThresholdRowHtml(a);
        html += flineXy(
          "最小",
          inpField("imageScaleMin", a.imageScaleMin ?? 0.9),
          "最大",
          inpField("imageScaleMax", a.imageScaleMax ?? 1.1)
        );
        html += imageRegionRelHtml(a);
      }

      // 逻辑转化：动作详情最底部；勾选与块名紧邻成组（勿插在截图/识别区域中间）
      if (isActionExec) {
        html += `<div class="param-opt-group">`;
        html += `<div class="fline wrap">${chkLabeled(
          "aiLogicConvert",
          "逻辑转化",
          !!a.aiLogicConvert
        )}</div>`;
        if (a.aiLogicConvert) {
          html += flineStack(
            "逻辑转化块名(可空自动生成)",
            inpField("aiLogicBlockName", a.aiLogicBlockName || "")
          );
        }
        html += `</div>`;
      }
    } else if (
      t === "lockScreenshot" ||
      t === "unlockScreenshot" ||
      t === "endLoop" ||
      t === "else" ||
      t === "stopMacro"
    ) {
      const hints = {
        lockScreenshot: "*提示:锁定后后续找图/OCR 使用锁定截图",
        unlockScreenshot: "*提示:与锁定截屏成对出现",
        endLoop: "*提示:须放在循环内，用于提前结束循环",
        else: "*提示:必须和「如果」成对，作为兄弟节点",
        stopMacro: "*提示:结束宏运行，等同热键停止",
      };
      html += `<p class="hint">${hints[t] || "此动作无需额外参数"}</p>`;
    } else {
      html += `<p class="hint">未知动作类型「${esc(t)}」；字段仍会原样保存。</p>`;
      html += `<pre class="hint" style="white-space:pre-wrap;max-height:240px;overflow:auto;font-size:11px">${esc(
        JSON.stringify(a, null, 2)
      )}</pre>`;
    }
    html += "</div></div>";
    panel.innerHTML = html;
    requestFindImageThumbs(panel);
    wireParamPanelEvents(a, t);
    if (!a._preview && state.actionSel >= 0) syncActionListRowName(state.actionSel);
  }

  function wireParamPanelEvents(a, t) {
    const panel = $("#paramPanel");
    if (!panel || !a) return;
    const edSpeed = panel.querySelector('.speed-slider[data-speed-scope="action"]');
    if (edSpeed && t === "mousePlayback") {
      bindSpeedSliderRoot(edSpeed, {
        readValue: () => clampPlaybackSpeedUi(parseFloat(edSpeed.dataset.speed)),
        setValue: (speed) => {
          const s = clampPlaybackSpeedUi(speed);
          a.playbackSpeed = s;
          const wrap = edSpeed.closest(".ed-speed-stack") || edSpeed.parentElement;
          applySpeedToSliderEl(edSpeed, wrap && wrap.querySelector(".speed-val"), s);
        },
      });
    }
    const btnCombo = panel.querySelector('[data-k="button"]');
    if (btnCombo) {
      btnCombo.addEventListener("click", (e) => {
        e.stopPropagation();
        showPopup(
          btnCombo,
          [
            { t: "左键", v: "left" },
            { t: "右键", v: "right" },
            { t: "中键", v: "middle" },
            { t: "侧键1", v: "x1" },
            { t: "侧键2", v: "x2" },
          ],
          (it) => {
            a.button = it.v;
            btnCombo.textContent = it.t;
            if (!a._preview && state.actionSel >= 0) syncActionListRowName(state.actionSel);
          }
        );
      });
    }
    const outCombo = panel.querySelector('[data-k="aiOutputType"]');
    if (outCombo) {
      outCombo.addEventListener("click", (e) => {
        e.stopPropagation();
        showPopup(
          outCombo,
          [
            { t: "文本", v: 0 },
            { t: "数字", v: 1 },
          ],
          (it) => {
            a.aiOutputType = it.v;
            outCombo.textContent = it.t;
          }
        );
      });
    }
    const ctxCombo = panel.querySelector('[data-k="aiContextMode"]');
    if (ctxCombo) {
      ctxCombo.addEventListener("click", (e) => {
        e.stopPropagation();
        showPopup(
          ctxCombo,
          [
            { t: "无上下文", v: 0 },
            { t: "宏上下文", v: 1 },
            { t: "循环上下文", v: 2 },
            { t: "块上下文", v: 3 },
          ],
          (it) => {
            a.aiContextMode = it.v;
            ctxCombo.textContent = it.t;
          }
        );
      });
    }
    const modelCombo = panel.querySelector('[data-k="aiModelName"]');
    if (modelCombo) {
      modelCombo.addEventListener("click", (e) => {
        e.stopPropagation();
        const items = (state._aiModels || []).length
          ? state._aiModels
          : state.agentModel
            ? [{ t: state.agentModel, v: state.agentModel }]
            : [];
        if (!items.length) {
          toast("请先在「设置 → AI助手」中添加模型");
          return;
        }
        showPopup(modelCombo, items, (it) => {
          a.aiModelName = it.v;
          modelCombo.textContent = it.t || it.v;
        });
      });
    }
    const followCombo = panel.querySelector('[data-k="findImageFollowUp"]');
    if (followCombo) {
      followCombo.addEventListener("click", (e) => {
        e.stopPropagation();
        showPopup(
          followCombo,
          [
            { t: "点击", v: 0 },
            { t: "鼠标移动到", v: 1 },
            { t: "保存匹配度", v: 2 },
            { t: "保存图片", v: 3 },
          ],
          (it) => {
            a.findImageFollowUp = it.v;
            // 在默认名之间切换：保存图片→image，保存匹配度→matchRet；自定义名保留
            const vn = (a.matchVarName || "").trim();
            if (it.v === 3 && (!vn || vn === "matchRet")) a.matchVarName = "image";
            if (it.v === 2 && (!vn || vn === "image")) a.matchVarName = "matchRet";
            followCombo.textContent = it.t;
            renderParamPanel(a);
          }
        );
      });
    }
    const ocrModeCombo = panel.querySelector('[data-k="ocrResultMode"]');
    if (ocrModeCombo) {
      ocrModeCombo.addEventListener("click", (e) => {
        e.stopPropagation();
        showPopup(
          ocrModeCombo,
          [
            { t: "获取文字", v: 0 },
            { t: "文字查找", v: 1 },
          ],
          (it) => {
            a.ocrResultMode = it.v;
            renderParamPanel(a);
          }
        );
      });
    }
    const ocrFuCombo = panel.querySelector('[data-k="ocrFollowUp"]');
    if (ocrFuCombo) {
      ocrFuCombo.addEventListener("click", (e) => {
        e.stopPropagation();
        showPopup(
          ocrFuCombo,
          [
            { t: "点击", v: 0 },
            { t: "鼠标移动到", v: 1 },
            { t: "保存到变量", v: 2 },
          ],
          (it) => {
            a.ocrFollowUp = it.v;
            renderParamPanel(a);
          }
        );
      });
    }
    const runBlockCombo = panel.querySelector('[data-k="blockName"]');
    if (runBlockCombo && t === "runBlock") {
      runBlockCombo.addEventListener("click", (e) => {
        e.stopPropagation();
        const names = [];
        const seen = new Set();
        (state.editorActions || []).forEach((x) => {
          if (!x || x.type !== "defineBlock") return;
          const n = String(x.blockName || "").trim();
          if (!n || seen.has(n)) return;
          seen.add(n);
          names.push({ t: n, v: n });
        });
        if (!names.length) {
          toast("当前脚本暂无已定义块");
          return;
        }
        showPopup(runBlockCombo, names, (it) => {
          a.blockName = it.v;
          delete a._runBlockTyped;
          runBlockCombo.textContent = it.t;
          const typed = panel.querySelector('[data-k="_runBlockTyped"]');
          if (typed) typed.textContent = it.v;
        });
      });
    }
    const dirCombo = panel.querySelector('[data-k="scrollDirection"]');
    if (dirCombo) {
      dirCombo.addEventListener("click", (e) => {
        e.stopPropagation();
        showPopup(
          dirCombo,
          [
            { t: "向上/左", v: 0 },
            { t: "向下/右", v: 1 },
          ],
          (it) => {
            a.scrollDirection = it.v;
            dirCombo.textContent = it.t;
          }
        );
      });
    }
    const hkCombo = panel.querySelector('[data-k="shortcutPreset"]');
    if (hkCombo && t === "hotkeyShortcut") {
      hkCombo.addEventListener("click", (e) => {
        e.stopPropagation();
        const presets = [
          { t: "Ctrl+C(拷贝)", v: 0 },
          { t: "Ctrl+V(粘贴)", v: 1 },
          { t: "Ctrl+X(剪切)", v: 2 },
          { t: "Ctrl+S(保存)", v: 3 },
          { t: "Ctrl+F(查找)", v: 4 },
          { t: "Alt+F4(关闭窗口)", v: 5 },
          { t: "Win+D(所有最小化)", v: 6 },
          { t: "Win+R(打开运行)", v: 7 },
          { t: "Ctrl+Alt+Delete", v: 8 },
        ];
        showPopup(hkCombo, presets, (it) => {
          a.shortcutPreset = it.v;
          a.keyText = it.t;
          hkCombo.textContent = it.t;
          hkCombo.dataset.v = String(it.v);
          if (!a._preview && state.actionSel >= 0) syncActionListRowName(state.actionSel);
        });
      });
    }
    const runPresetCombo = panel.querySelector('[data-k="runProgramPreset"]');
    if (runPresetCombo && t === "runProgram") {
      runPresetCombo.addEventListener("click", (e) => {
        e.stopPropagation();
        const presets = [
          { t: "选择文件", v: 0, path: "" },
          { t: "快捷运行-记事本", v: 1, path: "notepad.exe" },
          { t: "快捷运行-计算器", v: 2, path: "calc.exe" },
          { t: "快捷运行-画图", v: 3, path: "mspaint.exe" },
          { t: "快捷运行-文件管理器", v: 4, path: "explorer.exe" },
          { t: "快捷运行-命令行", v: 5, path: "cmd.exe" },
          { t: "快捷运行-PowerShell", v: 6, path: "powershell.exe" },
          { t: "快捷运行-进程管理器", v: 7, path: "taskmgr.exe" },
          { t: "快捷运行-注册表编辑器", v: 8, path: "regedit.exe" },
          { t: "快捷运行-服务", v: 9, path: "services.msc" },
          { t: "快捷运行-计算机管理", v: 10, path: "compmgmt.msc" },
          { t: "快捷运行-控制面板", v: 11, path: "control.exe" },
          { t: "快捷运行-设置", v: 12, path: "ms-settings:" },
        ];
        showPopup(runPresetCombo, presets, (it) => {
          a.shortcutPreset = it.v;
          if (it.v > 0) a.targetPath = it.path;
          runPresetCombo.textContent = it.t;
          renderParamPanel(a);
        });
      });
    }
    const pbCombo = panel.querySelector('[data-k="blockName"]');
    if (pbCombo && (t === "mousePlayback" || t === "runMacro")) {
      pbCombo.addEventListener("click", (e) => {
        e.stopPropagation();
        if (t === "mousePlayback") pickRecordingInto(a, pbCombo);
        else pickMacroInto(a, pbCombo);
      });
    }
    const useModeCombo = panel.querySelector('[data-k="useMode"]');
    if (useModeCombo && (t === "mousePlayback" || t === "runMacro")) {
      useModeCombo.addEventListener("click", (e) => {
        e.stopPropagation();
        const cur = a.useMode == null ? 3 : a.useMode | 0;
        const selectedIndex = Math.max(
          0,
          NESTED_USE_MODES.findIndex((m) => m.v === cur)
        );
        showPopup(
          useModeCombo,
          NESTED_USE_MODES,
          (it) => {
            readParamPanelInto(a);
            a.useMode = it.v | 0;
            if (a.useMode === 1 || a.useMode === 2) ensureNestedWindowMode(a);
            renderParamPanel(a);
          },
          { selectedIndex }
        );
      });
    }
    const nwmMethodCombo = panel.querySelector('[data-k="_nwmSelectMethod"]');
    if (nwmMethodCombo && (t === "mousePlayback" || t === "runMacro")) {
      nwmMethodCombo.addEventListener("click", (e) => {
        e.stopPropagation();
        const wm = ensureNestedWindowMode(a);
        const cur = normalizeWmSelectMethod(wm.selectMethod);
        const selectedIndex = Math.max(
          0,
          WM_METHODS.findIndex((m) => m.v === cur)
        );
        showPopup(
          nwmMethodCombo,
          WM_METHODS,
          (it) => {
            readParamPanelInto(a);
            const next = ensureNestedWindowMode(a);
            next.selectMethod = it.v;
            a.nestedWindowMode = next;
            nwmMethodCombo.textContent = it.t;
            renderParamPanel(a);
          },
          { selectedIndex, minWidth: 260 }
        );
      });
    }
    $$(".chk[data-k]", panel).forEach((el) => {
      el.addEventListener("click", (e) => {
        e.stopPropagation();
        el.classList.toggle("on");
        a[el.dataset.k] = el.classList.contains("on") ? 1 : 0;
        if (el.dataset.k === "_nwmFakeFocus") {
          const wm = ensureNestedWindowMode(a);
          wm.fakeFocusEnabled = el.classList.contains("on") ? 1 : 0;
        }
        if (el.dataset.k === "ocrRegionByImage") {
          if (el.classList.contains("on")) {
            ensureFullScreenCoordsVisible(a);
          }
          renderParamPanel(a);
        } else if (el.dataset.k === "aiRegionByImage") {
          if (el.classList.contains("on") && !(a.searchFullScreen | 0)) {
            // 勾选后仍用绝对屏幕坐标；若尚未填有效区域则默认全图
            const x1 = a.aiSearchX1 | 0;
            const y1 = a.aiSearchY1 | 0;
            const x2 = a.aiSearchX2 | 0;
            const y2 = a.aiSearchY2 | 0;
            if (x2 <= x1 || y2 <= y1) applyFullScreenSearchCoords(a, "aiSearch");
          }
          renderParamPanel(a);
        } else if (el.dataset.k === "aiWithImage") {
          if (!el.classList.contains("on")) {
            a.aiRegionByImage = 0;
            a.aiTargetImagePath = "";
          }
          renderParamPanel(a);
        } else if (el.dataset.k === "aiLogicConvert") {
          if (!el.classList.contains("on")) a.aiLogicBlockName = "";
          renderParamPanel(a);
        } else if (
          el.dataset.k === "imageUseVar" ||
          el.dataset.k === "aiImageUseVar" ||
          el.dataset.k === "perfectMatch"
        ) {
          renderParamPanel(a);
        } else if (
          el.dataset.k === "moveFromVar" ||
          el.dataset.k === "loopFromVar"
        ) {
          renderParamPanel(a);
        } else if (!a._preview && state.actionSel >= 0) {
          syncActionListRowName(state.actionSel);
        }
      });
    });
    // 参数变更时同步左侧动作名（对齐原生 ActionName 实时刷新）
    panel.querySelectorAll(".inp[data-k]").forEach((el) => {
      const sync = () => {
        readParamPanelInto(a);
        if (!a._preview && state.actionSel >= 0) syncActionListRowName(state.actionSel);
        else if (a._preview && $("#paramTag")) {
          $("#paramTag").textContent = typeLabel(a.type);
        }
      };
      el.addEventListener("input", sync);
      el.addEventListener("blur", sync);
    });
    // 条件-如果：变量 / 判断 / 连接 + 添加
    if (t === "if") {
      const IF_OPS = [
        { t: "等于", v: "==" },
        { t: "不等于", v: "!=" },
        { t: "小于", v: "<" },
        { t: "小于等于", v: "<=" },
        { t: "大于", v: ">" },
        { t: "大于等于", v: ">=" },
        { t: "包含", v: ">>" },
      ];
      const IF_CONN = [
        { t: "并且(and)", v: "and" },
        { t: "或者(or)", v: "or" },
        { t: "非(not)", v: "not" },
      ];
      const varCombo = panel.querySelector('[data-k="_ifVar"]');
      const opCombo = panel.querySelector('[data-k="_ifOp"]');
      const connCombo = panel.querySelector('[data-k="_ifConn"]');
      if (varCombo) {
        varCombo.addEventListener("click", (e) => {
          e.stopPropagation();
          const items = buildEditorVarItems().filter((x) => x.code);
          if (!items.length) {
            toast("暂无可用变量");
            return;
          }
          showPopup(varCombo, items, (it) => {
            a._ifVarCode = it.code;
            a._ifVarLabel = it.t;
            varCombo.textContent = it.t;
          });
        });
      }
      if (opCombo) {
        opCombo.addEventListener("click", (e) => {
          e.stopPropagation();
          showPopup(opCombo, IF_OPS, (it) => {
            a._ifOp = it.v;
            a._ifOpLabel = it.t;
            opCombo.textContent = it.t;
          });
        });
      }
      if (connCombo) {
        connCombo.addEventListener("click", (e) => {
          e.stopPropagation();
          showPopup(connCombo, IF_CONN, (it) => {
            a._ifConn = it.v;
            a._ifConnLabel = it.t;
            connCombo.textContent = it.t;
          });
        });
      }
    }
    wireFindImageButtons(panel, a);
  }

  function pickRecordingInto(a, anchor) {
    const open = () => {
      const items = (state.recordings || []).map((r) => ({
        t: r.name || r.path,
        v: r.path || r.id,
        name: r.name || "",
      }));
      if (!items.length) {
        toast("暂无录制");
        return;
      }
      const sel = items.findIndex(
        (it) => it.v === a.targetPath || it.name === a.blockName
      );
      showPopup(
        anchor || $("#paramPanel"),
        items,
        (it) => {
          a.targetPath = it.v;
          a.blockName = it.name || it.t;
          if (anchor) anchor.textContent = a.blockName;
          peekScriptActions(a.targetPath).then((msg) => {
            if (msg && msg.ok) applyTargetScriptUseMode(a, msg);
            renderParamPanel(a);
          });
        },
        { selectedIndex: sel >= 0 ? sel : 0, minRows: 8 }
      );
    };
    if (window.qst) {
      state._pendingPickRec = { open };
      qst.listRecordings();
      return;
    }
    open();
  }

  function pickMacroInto(a, anchor) {
    const open = () => {
      const items = (state.macros || []).map((r) => ({
        t: r.name || r.path,
        v: r.path || r.id,
        name: r.name || "",
      }));
      if (!items.length) {
        toast("暂无鼠标宏");
        return;
      }
      const sel = items.findIndex(
        (it) => it.v === a.targetPath || it.name === a.blockName
      );
      showPopup(
        anchor || $("#paramPanel"),
        items,
        (it) => {
          a.targetPath = it.v;
          a.blockName = it.name || it.t;
          if (anchor) anchor.textContent = a.blockName;
          peekScriptActions(a.targetPath).then((msg) => {
            if (msg && msg.ok) applyTargetScriptUseMode(a, msg);
            renderParamPanel(a);
          });
        },
        { selectedIndex: sel >= 0 ? sel : 0, minRows: 8 }
      );
    };
    if (window.qst) {
      state._pendingPickMacro = { open };
      qst.listScripts();
      return;
    }
    open();
  }

  function wireFindImageButtons(panel, a) {
    if (!panel || !a) return;
    panel.querySelectorAll("[data-fi]").forEach((btn) => {
      const act = btn.dataset.fi;
      if (act === "pickCoord") {
        wireCrosshairPointerDown(btn, "coordinates", "coord", () => readParamPanelInto(a));
        return;
      }
      if (act === "pickProg") {
        wireCrosshairPointerDown(btn, "programPath", "program", () => readParamPanelInto(a));
        return;
      }
      if (act === "nwmPick") {
        wireCrosshairPointerDown(btn, "windowTarget", "nestedWm", () => readParamPanelInto(a));
        return;
      }
      if (act === "nwmClass") {
        wireCrosshairPointerDown(btn, "windowTarget", "nestedWmClass", () => readParamPanelInto(a));
        return;
      }
      btn.addEventListener("click", (e) => {
        e.preventDefault();
        e.stopPropagation();
        readParamPanelInto(a);
        if (act === "clear") {
          a.imagePath = "";
          renderParamPanel(a);
          return;
        }
        if (act === "full") {
          applyFullScreenSearchCoords(a);
          renderParamPanel(a);
          return;
        }
        if (act === "absRegion") {
          state._pendingRegionTarget = "search";
          if (window.qst) qst.pickScreenRegion();
          else toast("无桥接");
          return;
        }
        if (act === "browseExe") {
          state._pendingBrowse = "exe";
          a.shortcutPreset = 0;
          if (window.qst) qst.browsePath(true);
          else toast("无桥接");
          return;
        }
        if (act === "nwmBrowse") {
          state._pendingBrowse = "nestedWmTarget";
          if (window.qst) qst.browsePath(true);
          else toast("无桥接");
          return;
        }
        if (act === "pickBlock") {
          // 兼容旧按钮；主路径已是 runBlock combo
          const names = [];
          const seen = new Set();
          (state.editorActions || []).forEach((x) => {
            if (!x || x.type !== "defineBlock") return;
            const n = String(x.blockName || "").trim();
            if (!n || seen.has(n)) return;
            seen.add(n);
            names.push({ t: n, v: n });
          });
          if (!names.length) {
            toast("当前脚本暂无已定义块");
            return;
          }
          showPopup(btn, names, (it) => {
            a.blockName = it.v;
            renderParamPanel(a);
          });
          return;
        }
        if (act === "insertVar") {
          const target = btn.dataset.target || "inputText";
          const items = buildEditorVarItems().filter((x) => x.code);
          if (!items.length) {
            toast("暂无可用变量");
            return;
          }
          showPopup(btn, items, (it) => {
            const el = panel.querySelector(`[data-k="${target}"]`);
            const insert = it.insert || "{" + it.code + "}";
            if (el) {
              const cur = el.textContent || "";
              el.textContent = cur + insert;
              a[target] = el.textContent;
            } else {
              a[target] = (a[target] || "") + insert;
              renderParamPanel(a);
            }
          });
          return;
        }
        if (act === "ocrTest" || act === "ocrOffset") {
          if (!window.qst) {
            toast("无桥接");
            return;
          }
          if (act === "ocrOffset" && (a.ocrFollowUp | 0) === 2) {
            toast("保存到变量时无需偏移");
            return;
          }
          if (act === "ocrOffset" && (a.ocrResultMode | 0) === 1) {
            toast("文字查找模式不使用偏移点击");
            return;
          }
          if (
            act === "ocrOffset" &&
            (a.ocrResultMode | 0) === 1 &&
            !String(a.ocrSearchText || "").trim()
          ) {
            toast("请先输入要在结果中查找的文字");
            return;
          }
          qst.testOcr({
            mode: act === "ocrOffset" ? "offset" : "test",
            ocrRegionByImage: a.ocrRegionByImage ? 1 : 0,
            ocrDigitsOnly: a.ocrDigitsOnly ? 1 : 0,
            searchFullScreen: a.searchFullScreen ? 1 : 0,
            searchX1: a.searchX1 | 0,
            searchY1: a.searchY1 | 0,
            searchX2: a.searchX2 | 0,
            searchY2: a.searchY2 | 0,
            imageRegionX1: a.imageRegionX1 | 0,
            imageRegionY1: a.imageRegionY1 | 0,
            imageRegionX2: a.imageRegionX2 | 0,
            imageRegionY2: a.imageRegionY2 | 0,
            matchThreshold: a.matchThreshold ?? 65,
            perfectMatch: a.perfectMatch ? 1 : 0,
            imageScaleMin: a.imageScaleMin ?? 1,
            imageScaleMax: a.imageScaleMax ?? 1,
            imagePath: a.imagePath || "",
            ocrSearchText: a.ocrSearchText || "",
            ocrResultMode: a.ocrResultMode | 0,
          });
          return;
        }
        if (act === "browseFile") {
          state._pendingBrowse = "file";
          if (window.qst) qst.browsePath(false);
          else toast("无桥接");
          return;
        }
        if (act === "captureKey") {
          openActionKeyCapture(a);
          return;
        }
        if (act === "ocrInstall") {
          state._ocrRepair = false;
          if ($("#ocrDlgTitle"))
            $("#ocrDlgTitle").childNodes[0].textContent = "键鼠工坊-插件安装 ";
          if ($("#btnOcrInstall")) {
            $("#btnOcrInstall").disabled = false;
            $("#btnOcrInstall").textContent = "安装插件";
          }
          if ($("#btnOcrRepair")) $("#btnOcrRepair").disabled = false;
          if ($("#ocrStatus"))
            $("#ocrStatus").textContent = "已就绪，点击安装开始下载安装…";
          if ($("#ocrBar")) $("#ocrBar").style.width = "0%";
          openOv("ocr");
          return;
        }
        if (act === "screen" || act === "aiScreen") {
          if (window.qst) {
            state._pendingAiTemplateShot = act === "aiScreen";
            qst.captureTemplateScreenshot();
          } else toast("无桥接");
          return;
        }
        if (!window.qst) {
          toast("无桥接");
          return;
        }
        if (act === "pick" || act === "pickAiImg") {
          state._pendingImageField = act === "pickAiImg" ? "aiTargetImagePath" : "imagePath";
          qst.pickImageFile();
          return;
        }
        if (act === "aiClearImg") {
          a.aiTargetImagePath = "";
          renderParamPanel(a);
          return;
        }
        if (act === "ifAdd") {
          const vars = buildEditorVarItems().filter((x) => x.code);
          const code =
            a._ifVarCode ||
            (vars[0] && vars[0].code) ||
            "";
          if (!code) {
            toast("请先选择变量");
            return;
          }
          const op = a._ifOp || "==";
          const conn = a._ifConn || "and";
          const valEl = panel.querySelector('[data-k="_ifValue"]');
          const value = ((valEl && valEl.textContent) || "0").trim() || "0";
          a._ifValue = value;
          const clause = code + op + value;
          let cur = (a.conditionExpr || "").replace(/\s+$/g, "");
          if (!cur.trim()) a.conditionExpr = clause;
          else a.conditionExpr = cur + " " + conn + "\n" + clause;
          renderParamPanel(a);
          return;
        }
        if (act === "aiFull") {
          a.aiSearchRegion = 0;
          applyFullScreenSearchCoords(a, "aiSearch");
          renderParamPanel(a);
          return;
        }
        if (act === "aiRegion") {
          state._pendingRegionTarget = "ai";
          qst.pickScreenRegion();
          return;
        }
        if (act === "crop") {
          if (!a.imagePath) {
            toast("请先截图或选择图片");
            return;
          }
          openWebFindImageCrop(a);
          return;
        }
        if (act === "pickImageRegion") {
          const isAi =
            !!a.aiRegionByImage &&
            (a.type === "aiImageAnalysis" || a.type === "aiActionExecute");
          const useVar = !!(isAi ? a.aiImageUseVar : a.imageUseVar);
          const path = isAi ? a.aiTargetImagePath || "" : a.imagePath || "";
          if (!path) {
            toast("请先设置要查找的图片");
            return;
          }
          const sx1 = isAi ? a.aiSearchX1 | 0 : a.searchX1 | 0;
          const sy1 = isAi ? a.aiSearchY1 | 0 : a.searchY1 | 0;
          const sx2 = isAi ? a.aiSearchX2 | 0 : a.searchX2 | 0;
          const sy2 = isAi ? a.aiSearchY2 | 0 : a.searchY2 | 0;
          const full =
            (a.searchFullScreen | 0) === 1 || sx2 <= sx1 || sy2 <= sy1;
          state._pendingImageRegionPick = true;

          if (useVar) {
            const size = inferImageVarAnchorSize(path, state.actionSel | 0);
            if (!size) {
              toast("无法推断图片尺寸，请手填 X1Y1X2Y2 相对坐标");
              state._pendingImageRegionPick = false;
              return;
            }
            const opts = { mode: "regionBySize" };
            if (size.pathOnly) {
              opts.imagePath = size.path || path;
              opts.syntheticW = 0;
              opts.syntheticH = 0;
            } else {
              opts.syntheticW = size.w;
              opts.syntheticH = size.h;
            }
            qst.findImageMatch(opts);
            return;
          }

          qst.findImageMatch({
            imagePath: path,
            mode: "region",
            searchFullScreen: full ? 1 : 0,
            searchX1: sx1,
            searchY1: sy1,
            searchX2: sx2,
            searchY2: sy2,
            matchThreshold: matchThresholdPercent(a.matchThreshold),
            perfectMatch: a.perfectMatch ? 1 : 0,
            imageScaleMin: a.imageScaleMin ?? 0.9,
            imageScaleMax: a.imageScaleMax ?? 1.1,
            maxMatches: 1,
          });
          return;
        }
        if (act === "test" || act === "offset" || act === "region") {
          if (act === "region" && !a.imagePath) {
            state._pendingRegionTarget = "search";
            qst.pickScreenRegion();
            return;
          }
          if (!a.imagePath) {
            toast("请先截图或选择图片");
            return;
          }
          const full =
            (a.searchFullScreen | 0) === 1 ||
            (a.searchX2 | 0) <= (a.searchX1 | 0) ||
            (a.searchY2 | 0) <= (a.searchY1 | 0);
          qst.findImageMatch({
            imagePath: a.imagePath,
            mode: act === "test" ? "test" : act === "offset" ? "offset" : "region",
            searchFullScreen: full ? 1 : 0,
            searchX1: a.searchX1 | 0,
            searchY1: a.searchY1 | 0,
            searchX2: a.searchX2 | 0,
            searchY2: a.searchY2 | 0,
            matchThreshold: matchThresholdPercent(a.matchThreshold),
            perfectMatch: a.perfectMatch ? 1 : 0,
            imageScaleMin: a.imageScaleMin ?? 0.9,
            imageScaleMax: a.imageScaleMax ?? 1.1,
            maxMatches: act === "offset" || act === "region" ? 1 : 20,
          });
        }
      });
    });
  }

  function normalizeWmSelectMethod(v) {
    if (v === "useEditorWindowClass" || v === "noSelect" || v === "selectOnStartup") return v;
    if (v === "mousePositionOnStartup") return "selectOnStartup";
    if (v && WM_METHOD_LABEL_ALIASES[v]) return WM_METHOD_LABEL_ALIASES[v];
    return "selectOnStartup";
  }

  function resolveWmSelectMethodForSave(raw, label) {
    if (
      raw === "useEditorWindowClass" ||
      raw === "noSelect" ||
      raw === "selectOnStartup" ||
      raw === "mousePositionOnStartup"
    ) {
      return normalizeWmSelectMethod(raw);
    }
    const fromLabel = String(label || "").trim();
    if (fromLabel) {
      const hit = WM_METHODS.find((m) => m.t === fromLabel);
      if (hit) return hit.v;
      if (WM_METHOD_LABEL_ALIASES[fromLabel]) return WM_METHOD_LABEL_ALIASES[fromLabel];
    }
    return "selectOnStartup";
  }

  function syncEditorModeUi() {
    const mode = state.editorMode | 0;
    const edMode = $("#edMode");
    const label = (ED_MODES.find((m) => m.v === mode) || ED_MODES[0]).t;
    if (edMode) edMode.textContent = label;
    const showWm = mode === 1 || mode === 2;
    if ($("#edRowDefault")) $("#edRowDefault").style.display = showWm ? "none" : "";
    if ($("#edRowWm")) $("#edRowWm").style.display = showWm ? "" : "none";
    if ($("#edTargetLab"))
      $("#edTargetLab").textContent = mode === 2 ? "目标窗口" : "目标程序";
    const wm = state.windowMode || {};
    wm.selectMethod = normalizeWmSelectMethod(wm.selectMethod);
    const method = WM_METHODS.find((m) => m.v === wm.selectMethod) || WM_METHODS[0];
    if ($("#edWmMethod")) $("#edWmMethod").textContent = method.t;

    const methodV = method.v;
    // 对齐 UpdateEditorWindowModeChrome：启动时使用当前所在窗口不显示整行目标路径
    const showPathChrome =
      showWm &&
      (methodV === "useEditorWindowClass" || methodV === "noSelect");
    const showClassBtn = showWm && methodV === "useEditorWindowClass";
    const showFocus = showWm && (mode === 1 || mode === 2); // 窗口模式 / 后台窗口模式
    const showTargetRow = showPathChrome || showClassBtn || showFocus;

    if ($("#edTargetPath")) {
      $("#edTargetPath").textContent = wm.targetExePath || "";
    }
    if ($("#edRowTarget")) $("#edRowTarget").style.display = showTargetRow ? "" : "none";
    const titleRow = $("#edTitleRow");
    if (titleRow)
      titleRow.style.display =
        showWm && methodV === "useEditorWindowClass" ? "" : "none";
    if ($("#edTargetTitle")) $("#edTargetTitle").textContent = wm.windowTitle || wm.windowName || "";
    if ($("#edTargetClass"))
      $("#edTargetClass").textContent = wm.windowClassName || "";

    // #edTarget 仅放 .exe 路径（勿糊类名/标题）
    if ($("#edTarget")) {
      $("#edTarget").textContent = wm.targetExePath || "";
      $("#edTarget").style.display = showPathChrome ? "" : "none";
    }
    const pickBtn = $("#edPickBtn");
    if (pickBtn) {
      pickBtn.style.display = showPathChrome ? "" : "none";
      pickBtn.textContent = mode === 2 ? "准星绑定窗口" : "准星找程序";
    }
    const browseBtn = $("#edBrowseBtn");
    if (browseBtn) browseBtn.style.display = showPathChrome ? "" : "none";
    const classBtn = $("#edClassBtn");
    if (classBtn) classBtn.style.display = showClassBtn ? "" : "none";
    const focusEl = $("#edFocus");
    if (focusEl) {
      focusEl.style.display = showFocus ? "" : "none";
      if (!showFocus) setChk(focusEl, false);
      else setChk(focusEl, !!wm.fakeFocusEnabled);
    }
  }

  function validateWindowModeForSave(wm) {
    const mode = state.editorMode | 0;
    if (mode === 0) return true;
    wm = wm || {};
    const method = normalizeWmSelectMethod(wm.selectMethod);
    const hasIdentity = !!(
      (wm.windowClassName && String(wm.windowClassName).trim()) ||
      (wm.windowName && String(wm.windowName).trim()) ||
      (wm.windowTitle && String(wm.windowTitle).trim())
    );
    const hasExe = !!(wm.targetExePath && String(wm.targetExePath).trim());
    const background = mode === 2;

    if (method === "useEditorWindowClass" && !hasIdentity) {
      toast("请先点击「指定窗口类」配置目标窗口，或改用其他选择窗口方式。");
      return false;
    }
    if (method === "noSelect" && !hasExe) {
      toast("请填写目标程序路径，可使用「浏览」或「准星找程序」。");
      return false;
    }
    if (method === "useEditorWindowClass" && !hasExe) {
      toast(
        "请填写目标程序路径（窗口未打开时将自动启动），或用「指定窗口类」拾取带路径的窗口。"
      );
      return false;
    }
    // 启动时使用当前所在窗口：运行时再绑窗，保存时不要求路径或身份
    if (
      background &&
      !hasExe &&
      !hasIdentity &&
      method !== "selectOnStartup"
    ) {
      toast(
        "后台窗口模式请用「启动时使用当前所在窗口」「准星绑定窗口」或「指定窗口类」选择目标窗口。"
      );
      return false;
    }
    if (
      method !== "selectOnStartup" &&
      method !== "useEditorWindowClass" &&
      method !== "noSelect" &&
      !hasIdentity &&
      !hasExe
    ) {
      toast("未获取到目标窗口信息。");
      return false;
    }
    return true;
  }

  function collectWindowModeForSave() {
    const wm = Object.assign({}, state.windowMode || {});
    const actions = Array.isArray(state.editorActions) ? state.editorActions : [];
    const anyRel = !!(wm.windowRelativeCoordinates
      || actions.some((a) => a && a.windowRelative));
    wm.enabled = state.editorMode > 0 ? 1 : 0;
    if (!wm.coordSpace) wm.coordSpace = "screenAbsolute";
    wm.executionKind = state.editorMode === 2 ? "backgroundWindow" : "hiddenDesktop";
    if (anyRel) {
      wm.coordSpace = "windowClient";
      wm.windowRelativeCoordinates = 1;
    }
    // 以 state 为准；仅当 state 无效时才回退读 combo 文案（含合并前旧文案）
    wm.selectMethod = resolveWmSelectMethodForSave(
      wm.selectMethod,
      ($("#edWmMethod")?.textContent || "").trim()
    );

    // 「不选择窗口」：清绑定身份，避免残留 pick 浮层字段绑错窗
    // 「启动时使用当前所在窗口」：运行时取前台窗，不得把上次拾取的类名/路径写进脚本
    if (wm.selectMethod === "noSelect") {
      wm.windowName = "";
      wm.windowTitle = "";
      wm.windowClassName = "";
      wm.childWindowClassName = "";
      wm.targetWindowTitle = "";
      wm.pickX = 0;
      wm.pickY = 0;
      wm.targetPickX = 0;
      wm.targetPickY = 0;
    } else if (wm.selectMethod === "selectOnStartup" && !anyRel) {
      wm.windowName = "";
      wm.windowTitle = "";
      wm.windowClassName = "";
      wm.childWindowClassName = "";
      wm.targetWindowTitle = "";
      wm.pickX = 0;
      wm.pickY = 0;
      wm.targetPickX = 0;
      wm.targetPickY = 0;
      wm.targetExePath = "";
      wm.launchArgs = "";
      wm.autoLaunchTarget = 0;
    } else {
      // 合并 pick 浮层字段（不依赖必须点「使用选择」）
      const pickTitle = ($("#pickTitle")?.textContent || "").trim();
      const pickClass = ($("#pickClass")?.textContent || "").trim();
      const pickChild = ($("#pickChild")?.textContent || "").trim();
      const pickPath = ($("#pickPath")?.textContent || "").trim();
      const pickDoc = ($("#pickDoc")?.textContent || "").trim();
      const pickX = parseInt($("#pickX")?.textContent || "", 10);
      const pickY = parseInt($("#pickY")?.textContent || "", 10);
      if (pickTitle) {
        wm.windowTitle = pickTitle;
        wm.windowName = pickTitle;
      }
      if (pickClass) wm.windowClassName = pickClass;
      if (pickChild) wm.childWindowClassName = pickChild;
      if (pickPath) wm.targetExePath = pickPath;
      if (pickDoc) {
        wm.launchArgs = `"${pickDoc}"`;
      } else if (pickTitle) {
        const stemArgs = launchArgsFromWindowPick(
          { windowTitle: pickTitle, documentPath: "" },
          wm.launchArgs
        );
        if (stemArgs) wm.launchArgs = stemArgs;
      } else if (pickPath && wm.launchArgs == null) {
        // 无文档时不硬清已有 launchArgs（可能来自上次保存）；仅在本次有 path 合并时保持
      }
      if (Number.isFinite(pickX)) {
        wm.pickX = pickX;
        wm.targetPickX = pickX;
      }
      if (Number.isFinite(pickY)) {
        wm.pickY = pickY;
        wm.targetPickY = pickY;
      }
    }

    const targetText = ($("#edTarget")?.textContent || "").trim();
    if (wm.selectMethod !== "selectOnStartup" && targetText && /\.exe$/i.test(targetText)) {
      wm.targetExePath = targetText;
    }
    const titleText = ($("#edTargetTitle")?.textContent || "").trim();
    const classText = ($("#edTargetClass")?.textContent || "").trim();
    if (wm.selectMethod !== "noSelect" && wm.selectMethod !== "selectOnStartup") {
      if (titleText) {
        wm.windowTitle = titleText;
        wm.windowName = titleText;
      }
      if (classText) wm.windowClassName = classText;
    }
    // 假焦点：窗口模式与后台窗口模式均可写（Unity 等游戏会自动启用，勾选可强制）
    if (state.editorMode === 1 || state.editorMode === 2) {
      wm.fakeFocusEnabled = $("#edFocus")?.classList.contains("on") ? 1 : 0;
    } else {
      wm.fakeFocusEnabled = 0;
    }
    if (wm.windowTitle != null && wm.windowName == null) {
      wm.windowName = wm.windowTitle;
    }
    if (wm.pickX != null && wm.targetPickX == null) wm.targetPickX = wm.pickX | 0;
    if (wm.pickY != null && wm.targetPickY == null) wm.targetPickY = wm.pickY | 0;
    if (wm.targetPickX == null) wm.targetPickX = 0;
    if (wm.targetPickY == null) wm.targetPickY = 0;
    if (wm.windowClassName == null) wm.windowClassName = "";
    if (wm.windowName == null) wm.windowName = "";
    if (wm.childWindowClassName == null) wm.childWindowClassName = "";
    if (wm.targetExePath == null) wm.targetExePath = "";
    if (wm.launchArgs == null) wm.launchArgs = "";
    return wm;
  }

  let _agentTabSeq = 0;

  function thinkingLabelHtml(streaming) {
    if (streaming) {
      return (
        '<span class="think-label" aria-label="思考中">' +
        "<i>思</i><i>考</i><i>中</i>" +
        '<span class="think-arrow">›</span></span>'
      );
    }
    return '<span class="think-label">思考过程<span class="think-arrow">›</span></span>';
  }

  /** 思考区渲染：把成对 * 加粗/斜体化，残留的 ** 标记剥掉，避免满屏裸星号 */
  function renderThinkingHtml(s) {
    let t = esc(String(s || ""));
    t = t.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
    t = t.replace(/(^|[^\w*])\*([^*\n]+)\*(?!\*)/g, "$1<em>$2</em>");
    t = t.replace(/\*\*/g, "");
    return t;
  }

  /** 思考区流式纯文本：剥离成对星号标记（性能优先，不跑完整 markdown） */
  function cleanThinkingText(s) {
    return String(s || "")
      .replace(/\*\*([^*]+)\*\*/g, "$1")
      .replace(/(^|[^\w*])\*([^*\n]+)\*(?!\*)/g, "$1$2")
      .replace(/\*\*/g, "");
  }

  function thinkingBlockHtml(text, opts) {
    const open = opts && opts.open;
    const streaming = opts && opts.streaming;
    return `<details class="bubble thinking${streaming ? " streaming" : ""}"${
      open ? " open" : ""
    }><summary>${thinkingLabelHtml(!!streaming)}</summary><div class="think-body">${renderThinkingHtml(
      text || ""
    )}</div></details>`;
  }

  function activeAgentTab() {
    return state.agentTabs.find((t) => t.key === state.agentTabKey) || null;
  }

  function findAgentTabById(id) {
    const want = String(id || "");
    if (!want) return null;
    return state.agentTabs.find((t) => String(t.id || "") === want) || null;
  }

  function agentTabForStream(msg) {
    const byId = findAgentTabById(msg && msg.id);
    if (byId) return byId;
    if (msg && msg.panelKey) {
      const byKey = state.agentTabs.find((t) => t.key === msg.panelKey);
      if (byKey) return byKey;
    }
    return activeAgentTab();
  }

  function anyAgentTabBusy() {
    return state.agentTabs.some((t) => t.busy);
  }

  function syncAgentGlobalsFromTab(tab) {
    if (!tab) {
      state.agentId = "";
      state.agentBusy = false;
      state.agentAttachments = [];
      return;
    }
    state.agentId = tab.id || "";
    state.agentBusy = !!tab.busy;
    state.agentAttachments = Array.isArray(tab.attachments) ? tab.attachments.slice() : [];
    if (tab.model) state.agentModel = tab.model;
  }

  function saveActiveAgentTabDom() {
    const tab = activeAgentTab();
    if (!tab) return;
    tab.draft = $("#chatInput") ? $("#chatInput").textContent || "" : tab.draft || "";
    tab.attachments = Array.isArray(state.agentAttachments)
      ? state.agentAttachments.slice()
      : [];
    persistAgentDraft(tab);
  }

  function setAgentEditMode(idx) {
    const tab = activeAgentTab();
    if (!tab || tab.busy) return;
    const msgs = tab.messages || [];
    const m = msgs[idx];
    if (!m || m.role !== "user") return;
    tab._editIndex = idx;
    const inp = $("#chatInput");
    if (inp) {
      inp.textContent = m.content || "";
      inp.focus();
    }
    const bar = $("#agentEditBar");
    if (bar) {
      const txt = $("#agentEditBarText");
      if (txt)
        txt.textContent = "正在编辑第 " + (idx + 1) + " 条消息，回车重发 · Esc 取消";
      bar.hidden = false;
    }
    persistAgentDraft(tab);
  }

  function clearAgentEditMode() {
    const tab = activeAgentTab();
    if (tab) tab._editIndex = -1;
    const bar = $("#agentEditBar");
    if (bar) bar.hidden = true;
  }

  /** 按当前会话的编辑态同步编辑提示条（切 tab / 打开会话后调用） */
  function syncAgentEditBar() {
    const tab = activeAgentTab();
    const idx = tab && tab._editIndex != null ? tab._editIndex : -1;
    const bar = $("#agentEditBar");
    if (!bar) return;
    const valid =
      idx >= 0 && tab && (tab.messages || [])[idx] && (tab.messages || [])[idx].role === "user";
    if (!valid) {
      if (tab) tab._editIndex = -1;
      bar.hidden = true;
      return;
    }
    const txt = $("#agentEditBarText");
    if (txt)
      txt.textContent = "正在编辑第 " + (idx + 1) + " 条消息，回车重发 · Esc 取消";
    bar.hidden = false;
  }

  let _agentDraftTimer = null;
  /** 把会话草稿（未发送文本/附件/编辑态）持久化到后端会话文件 */
  function persistAgentDraft(tab, force) {
    const target = tab || activeAgentTab();
    if (
      !target ||
      !target.id ||
      !window.qst ||
      typeof qst.saveAgentDraft !== "function"
    ) {
      return;
    }
    const isActive = target === activeAgentTab();
    target.draft = isActive
      ? $("#chatInput")
        ? $("#chatInput").textContent || ""
        : target.draft || ""
      : target.draft || "";
    const paths = (Array.isArray(target.attachments) ? target.attachments : [])
      .map((a) => (typeof a === "string" ? a : a && a.path))
      .filter((p) => p && !String(p).startsWith("clipboard-image."));
    const hasMessages = !!(target.messages && target.messages.length);
    const hasDraft = !!(String(target.draft || "").trim() || paths.length);
    // 空会话：输入防抖不落盘；关窗/关页签仍通知后端，以便删掉误入列表的空「新对话」
    if (!force && !hasMessages && !hasDraft) return;
    qst.saveAgentDraft({
      id: target.id,
      text: target.draft,
      attachments: paths,
      editIndex: target._editIndex != null ? target._editIndex : -1,
    });
  }

  function scheduleAgentDraftPersist() {
    if (_agentDraftTimer) clearTimeout(_agentDraftTimer);
    _agentDraftTimer = setTimeout(() => {
      _agentDraftTimer = null;
      persistAgentDraft();
    }, 500);
  }

  function renderAgentTabs() {
    const el = $("#agentTabs");
    if (!el) return;
    if (!state.agentTabs.length) {
      el.innerHTML = "";
      return;
    }
    el.innerHTML = state.agentTabs
      .map(
        (t) =>
          `<div class="agent-tab${t.key === state.agentTabKey ? " active" : ""}${
            t.busy ? " busy" : ""
          }" data-tab-key="${esc(t.key)}"><span class="tab-label">${esc(
            t.name || "新对话"
          )}</span><span class="tab-x" data-tab-close="${esc(t.key)}">×</span></div>`
      )
      .join("");
  }

  function minimizeAgentWindow() {
    if (AGENT_SHELL && window.qst) {
      qst.post({ type: "agentWindow.minimize" });
      return;
    }
    saveActiveAgentTabDom();
    state.agentMinimized = true;
    const ov = $("#ov-agent");
    if (ov) {
      ov.classList.add("show", "agent-min");
    }
    const dock = $("#agentMinDock");
    if (dock) {
      const tab = activeAgentTab();
      dock.textContent = tab && tab.name ? "AI · " + tab.name : "AI 助手";
      dock.hidden = false;
    }
  }

  function restoreAgentWindow() {
    state.agentMinimized = false;
    const ov = $("#ov-agent");
    if (ov) {
      ov.classList.add("show");
      ov.classList.remove("agent-min");
    }
    const dock = $("#agentMinDock");
    if (dock) dock.hidden = true;
  }

  function closeAgentWindow() {
    state.agentMinimized = false;
    persistAgentDraft(undefined, true);
    clearAgentEditMode();
    state.agentTabs = [];
    state.agentTabKey = "";
    syncAgentGlobalsFromTab(null);
    const dock = $("#agentMinDock");
    if (dock) dock.hidden = true;
    const ov = $("#ov-agent");
    if (ov) {
      ov.classList.remove("show", "agent-min");
    }
    if (AGENT_SHELL && window.qst) {
      qst.post({ type: "agentWindow.close" });
    }
  }

  function activateAgentTab(key, opts) {
    if (!(opts && opts.skipSave)) saveActiveAgentTabDom();
    const tab = state.agentTabs.find((t) => t.key === key);
    if (!tab) return;
    state.agentTabKey = key;
    syncAgentGlobalsFromTab(tab);
    renderAgentTabs();
    if (!(opts && opts.skipChat)) renderChat(tab.messages || []);
    if ($("#chatInput")) $("#chatInput").textContent = tab.draft || "";
    renderAgentAttachments();
    syncAgentSendBtn();
    syncAgentEditBar();
    if (tab.model && $("#agentModelCombo")) $("#agentModelCombo").textContent = tab.model;
    const title = $("#agentDlgTitle");
    if (title) title.textContent = "AI 脚本助手";
  }

  function closeAgentTab(key) {
    const idx = state.agentTabs.findIndex((t) => t.key === key);
    if (idx < 0) return;
    const tab = state.agentTabs[idx];
    if (tab.busy) {
      toast("请先取消或等待当前回复");
      return;
    }
    persistAgentDraft(tab, true);
    const wasActive = state.agentTabKey === key;
    state.agentTabs.splice(idx, 1);
    if (!state.agentTabs.length) {
      closeAgentWindow();
      return;
    }
    if (wasActive) {
      const next = state.agentTabs[Math.min(idx, state.agentTabs.length - 1)];
      activateAgentTab(next.key, { skipSave: true });
    } else {
      renderAgentTabs();
    }
  }

  /** AI 气泡：尽量渲染 Markdown + LaTeX，不靠 Prompt 限制模型输出格式 */
  function escAttr(s) {
    return String(s || "")
      .replace(/&/g, "&amp;")
      .replace(/"/g, "&quot;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;");
  }

  function mathPlaceholder(tex, display) {
    const t = String(tex || "").trim();
    const cls = display ? "md-math md-math-block" : "md-math";
    return `<span class="${cls}" data-display="${display ? "1" : "0"}" data-tex="${escAttr(t)}"><span class="md-math-fallback">${esc(t)}</span></span>`;
  }

  function renderMarkdownHtml(src) {
    let raw = String(src || "").replace(/\r\n/g, "\n");
    const blocks = [];
    const hold = (html) => {
      const i = blocks.length;
      blocks.push(html);
      return `\u0000BLK${i}\u0000`;
    };

    // 代码块优先（避免内部 $ * 被当成公式/强调）
    raw = raw.replace(/```([^\n`]*)\n?([\s\S]*?)```/g, (_, lang, code) =>
      hold(
        `<pre class="md-pre" data-lang="${escAttr(String(lang || "").trim())}"><code>${esc(
          String(code).replace(/\n$/, "")
        )}</code></pre>`
      )
    );

    // LaTeX / 数学：$$ $$、\[ \]、$ $、\( \)
    raw = raw.replace(/\$\$([\s\S]+?)\$\$/g, (_, tex) => hold(mathPlaceholder(tex, true)));
    raw = raw.replace(/\\\[([\s\S]+?)\\\]/g, (_, tex) => hold(mathPlaceholder(tex, true)));
    raw = raw.replace(/\\\((.+?)\\\)/g, (_, tex) => hold(mathPlaceholder(tex, false)));
    // 行内 $...$：避免单独货币符号；允许简单公式
    raw = raw.replace(/\$([^\$\n]+?)\$/g, (m, tex) => {
      const t = String(tex);
      if (!/[\\a-zA-Z{}^_=]/.test(t) && !/\d/.test(t)) return m;
      return hold(mathPlaceholder(t, false));
    });

    let t = esc(raw);
    t = t.replace(/`([^`\n]+)`/g, '<code class="md-code">$1</code>');
    t = t.replace(/~~([^~]+)~~/g, "<del>$1</del>");
    t = t.replace(/\*\*\*([^*]+)\*\*\*/g, "<strong><em>$1</em></strong>");
    t = t.replace(/\*\*([^*]+)\*\*/g, "<strong>$1</strong>");
    t = t.replace(/__([^_]+)__/g, "<strong>$1</strong>");
    t = t.replace(/(^|[^\w*])\*([^*\n]+)\*(?!\*)/g, "$1<em>$2</em>");
    t = t.replace(/(^|[^\w_])_([^_\n]+)_(?!_)/g, "$1<em>$2</em>");
    t = t.replace(
      /\[([^\]]+)\]\((https?:\/\/[^)\s]+)\)/g,
      '<a class="md-a" href="$2" target="_blank" rel="noopener noreferrer">$1</a>'
    );
    t = t.replace(/^>\s?(.+)$/gm, '<blockquote class="md-quote">$1</blockquote>');
    t = t.replace(/^######\s+(.+)$/gm, '<h6 class="md-h">$1</h6>');
    t = t.replace(/^#####\s+(.+)$/gm, '<h5 class="md-h">$1</h5>');
    t = t.replace(/^####\s+(.+)$/gm, '<h4 class="md-h">$1</h4>');
    t = t.replace(/^###\s+(.+)$/gm, '<h3 class="md-h">$1</h3>');
    t = t.replace(/^##\s+(.+)$/gm, '<h2 class="md-h">$1</h2>');
    t = t.replace(/^#\s+(.+)$/gm, '<h1 class="md-h">$1</h1>');
    t = t.replace(/^(-{3,}|\*{3,}|_{3,})\s*$/gm, '<hr class="md-hr" />');

    // GFM 表格：表头 + 分隔行 + 数据行
    t = t.replace(
      /(^|\n)((?:\|.+\|\n)+)/g,
      (m, pre, tableBlock) => {
        const lines = tableBlock.trim().split("\n").filter(Boolean);
        if (lines.length < 2) return m;
        if (!/^\|?\s*:?-{3,}/.test(lines[1].replace(/\|/g, "|"))) {
          // second line must look like |---|
          if (!lines[1].includes("---")) return m;
        }
        const splitRow = (ln) =>
          ln
            .replace(/^\|/, "")
            .replace(/\|$/, "")
            .split("|")
            .map((c) => c.trim());
        const head = splitRow(lines[0]);
        const bodyLines = lines.slice(2);
        const thead = `<thead><tr>${head.map((c) => `<th>${c}</th>`).join("")}</tr></thead>`;
        const tbody = `<tbody>${bodyLines
          .map((ln) => `<tr>${splitRow(ln).map((c) => `<td>${c}</td>`).join("")}</tr>`)
          .join("")}</tbody>`;
        return `${pre}<div class="md-table-wrap"><table class="md-table">${thead}${tbody}</table></div>`;
      }
    );

    t = t.replace(/((?:^(?:[*+\-]|\d+\.)\s+.+(?:\n|$))+)/gm, (block) => {
      const lines = block.trim().split("\n");
      const ordered = /^\d+\./.test(lines[0]);
      const tag = ordered ? "ol" : "ul";
      const items = lines
        .map((ln) => ln.replace(/^(?:[*+\-]|\d+\.)\s+/, ""))
        .map((ln) => `<li>${ln}</li>`)
        .join("");
      return `<${tag} class="md-list">${items}</${tag}>`;
    });

    t = t.replace(/\n{2,}/g, '</p><p class="md-p">');
    t = `<p class="md-p">${t}</p>`;
    t = t.replace(/([^>])\n([^<])/g, "$1<br>$2");
    t = t.replace(/\u0000BLK(\d+)\u0000/g, (_, i) => blocks[+i] || "");
    t = t.replace(/<p class="md-p">\s*<\/p>/g, "");
    t = t.replace(
      /<p class="md-p">\s*(<(?:pre|ul|ol|h[1-6]|blockquote|hr|div|table)\b)/gi,
      "$1"
    );
    t = t.replace(
      /(<\/(?:pre|ul|ol|h[1-6]|blockquote|div|table)>)\s*<\/p>/gi,
      "$1"
    );
    t = t.replace(/<p class="md-p">\s*(<hr\b[^>]*>)\s*<\/p>/gi, "$1");
    return t;
  }

  function typesetAiMarkdown(root) {
    if (!root || !window.katex) return;
    root.querySelectorAll(".md-math[data-tex]").forEach((el) => {
      if (el.dataset.typeset === "1") return;
      const tex = el.getAttribute("data-tex") || "";
      const display = el.getAttribute("data-display") === "1";
      try {
        window.katex.render(tex, el, {
          displayMode: display,
          throwOnError: false,
          strict: "ignore",
          trust: false,
        });
        el.dataset.typeset = "1";
      } catch (_) {
        /* 保留 fallback 文本 */
      }
    });
  }

  function setAiBubbleHtml(el, text) {
    if (!el) return;
    el.innerHTML = renderMarkdownHtml(text);
    typesetAiMarkdown(el);
  }

  function showChatSkeleton() {
    // 对话区不再塞假气泡占位；基本骨架=标题栏/页签/空对话框/输入区
    const log = $("#chatLog");
    if (!log) return;
    state._chatLoading = true;
    log.classList.remove("chat-flesh-in", "chat-hydrate", "chat-skel");
    log.innerHTML = "";
  }

  function clearChatSkeleton() {
    state._chatLoading = false;
    const log = $("#chatLog");
    if (log) log.classList.remove("chat-skel");
  }

  function revealChatFlesh(fromSkeleton) {
    const log = $("#chatLog");
    if (!log) return;
    // 批量灌入关掉 bubble fadeUp，避免对话区闪一下
    log.classList.add("chat-hydrate");
    requestAnimationFrame(() => {
      requestAnimationFrame(() => {
        log.classList.remove("chat-hydrate");
      });
    });
  }

  function renderChat(messages) {
    const log = $("#chatLog");
    if (!log) return;
    const fromSkel = !!state._chatLoading;
    const list = Array.isArray(messages) ? messages : [];
    const cfgErr = !list.length ? getAgentConfigError() : "";
    const nextEmpty = cfgErr ? `<div class="bubble ai">${esc(cfgErr)}</div>` : "";
    if (!list.length && !fromSkel && !log.innerHTML && !cfgErr) {
      state._chatLoading = false;
      return;
    }
    if (!list.length) {
      state._chatLoading = false;
      log.classList.remove("chat-skel", "chat-flesh-in");
      log.classList.add("chat-hydrate");
      if (log.innerHTML !== nextEmpty) log.innerHTML = nextEmpty;
      requestAnimationFrame(() => log.classList.remove("chat-hydrate"));
      return;
    }
    state._chatLoading = false;
    log.classList.remove("chat-skel");
    log.classList.add("chat-hydrate");
    log.innerHTML = list
      .map((m, idx) => {
        const isUser = m.role === "user";
        if (isUser) {
          return `<div class="bubble user"><span class="bubble-text">${esc(
            m.content || ""
          )}</span><button type="button" class="agent-edit-btn" data-edit-msg="${idx}" title="编辑并重发" aria-label="编辑并重发">✎</button></div>`;
        }
        let html = "";
        if (m.reasoning) html += thinkingBlockHtml(m.reasoning, { open: false });
        html += `<div class="bubble ai">${renderMarkdownHtml(m.content || "")}</div>`;
        return html;
      })
      .join("");
    typesetAiMarkdown(log);
    log.scrollTop = log.scrollHeight;
    revealChatFlesh(fromSkel);
  }

  function isAgentImagePath(p) {
    return /\.(png|jpe?g|gif|webp|bmp|ico)$/i.test(String(p || ""));
  }

  function agentAttachFileExt(p) {
    const m = /\.([a-z0-9]+)$/i.exec(String(p || "").split(/[/\\]/).pop() || "");
    return (m && m[1] ? m[1] : "file").slice(0, 4);
  }

  function normalizeAgentAttachment(item) {
    if (!item) return null;
    if (typeof item === "string") {
      return { path: item, previewUrl: isAgentImagePath(item) ? "" : undefined };
    }
    if (item.path) {
      const out = {
        path: String(item.path),
        previewUrl: item.previewUrl || (isAgentImagePath(item.path) ? "" : undefined),
      };
      if (item._temp) out._temp = true;
      return out;
    }
    return null;
  }

  function isTempClipboardAttach(a) {
    if (!a) return false;
    if (a._temp) return true;
    return String(a.path || "").startsWith("clipboard-image.");
  }

  function agentAttachmentPaths() {
    return (Array.isArray(state.agentAttachments) ? state.agentAttachments : [])
      .map((a) => (typeof a === "string" ? a : a && a.path))
      .filter((p) => p && !String(p).startsWith("clipboard-image."));
  }

  function addAgentAttachment(path, previewUrl) {
    const p = String(path || "").trim();
    if (!p) return;
    state.agentAttachments = state.agentAttachments || [];
    if (state.agentAttachments.some((a) => (typeof a === "string" ? a : a.path) === p)) {
      toast("已添加过该附件");
      return;
    }
    const entry = { path: p };
    if (previewUrl) entry.previewUrl = previewUrl;
    else if (isAgentImagePath(p)) entry.previewUrl = "";
    state.agentAttachments.push(entry);
    rememberConvImage(entry);
    renderAgentAttachments();
  }

  function renderAgentAttachments() {
    const el = $("#agentAttachList");
    if (!el) return;
    const list = (Array.isArray(state.agentAttachments) ? state.agentAttachments : [])
      .map(normalizeAgentAttachment)
      .filter(Boolean);
    state.agentAttachments = list;
    if (!list.length) {
      el.hidden = true;
      el.innerHTML = "";
      return;
    }
    el.hidden = false;
    el.innerHTML = list
      .map((a, i) => {
        const isImg = isAgentImagePath(a.path) || !!a.previewUrl;
        const media = isImg
          ? `<img class="agent-att-thumb" alt="" data-att-idx="${i}" data-img-path="${esc(
              a.path
            )}" src="${a.previewUrl ? esc(a.previewUrl) : ""}" />`
          : `<span class="agent-att-file" title="${esc(a.path)}">${esc(agentAttachFileExt(a.path))}</span>`;
        return `<div class="agent-att${isImg ? " agent-att-img" : ""}" title="${esc(
          a.path
        )}">${media}<button type="button" class="agent-att-x" data-rm-att="${i}" aria-label="移除">×</button></div>`;
      })
      .join("");
    requestAgentAttachThumbs(el);
  }

  function rememberConvImage(entry) {
    const tab = activeAgentTab();
    if (!tab || !entry || !entry.path) return;
    if (!isAgentImagePath(entry.path) && !entry.previewUrl) return;
    tab.convImages = Array.isArray(tab.convImages) ? tab.convImages : [];
    const hit = tab.convImages.find((x) => x.path === entry.path);
    if (hit) {
      if (entry.previewUrl) hit.previewUrl = entry.previewUrl;
      return;
    }
    tab.convImages.push({
      path: String(entry.path),
      previewUrl: entry.previewUrl || "",
    });
  }

  function collectAgentViewerImages() {
    // 预览切换只针对当前输入框附件，不含已移除项
    const map = new Map();
    const push = (a) => {
      if (!a) return;
      const path = typeof a === "string" ? a : a.path;
      if (!path) return;
      if (!isAgentImagePath(path) && !(a && a.previewUrl)) return;
      if (String(path).startsWith("clipboard-image.") && !(a && a.previewUrl)) return;
      const previewUrl = (a && a.previewUrl) || "";
      map.set(path, { path, previewUrl });
    };
    (Array.isArray(state.agentAttachments) ? state.agentAttachments : []).forEach(push);
    return Array.from(map.values());
  }

  function syncAgentImageViewerFromAttachments() {
    if (!state._imgViewer) return;
    const items = collectAgentViewerImages();
    if (!items.length) {
      closeAgentImageViewer();
      return;
    }
    const curPath =
      state._imgViewer.items && state._imgViewer.items[state._imgViewer.index]
        ? state._imgViewer.items[state._imgViewer.index].path
        : "";
    let index = items.findIndex((x) => x.path === curPath);
    if (index < 0) index = Math.min(state._imgViewer.index | 0, items.length - 1);
    state._imgViewer.items = items;
    state._imgViewer.index = Math.max(0, index);
    showAgentImageViewerItem();
  }

  function closeAgentImageViewer() {
    const ov = $("#agentImgViewer");
    if (ov) {
      ov.classList.remove("show", "aiv-multi");
      ov.hidden = true;
      ov.setAttribute("aria-hidden", "true");
    }
    state._imgViewer = null;
    document.removeEventListener("keydown", onAgentImageViewerKey);
  }

  function onAgentImageViewerKey(e) {
    if (!state._imgViewer) return;
    const zoomInp = $("#aivZoomInput");
    const typingZoom = zoomInp && document.activeElement === zoomInp;
    if (e.key === "Escape") {
      e.preventDefault();
      if (typingZoom) {
        zoomInp.blur();
        syncAgentImageViewerZoomInput();
        return;
      }
      closeAgentImageViewer();
    } else if (typingZoom) {
      return;
    } else if (e.key === "ArrowLeft") {
      e.preventDefault();
      stepAgentImageViewer(-1);
    } else if (e.key === "ArrowRight") {
      e.preventDefault();
      stepAgentImageViewer(1);
    } else if (e.key === "+" || e.key === "=") {
      e.preventDefault();
      zoomAgentImageViewer(0.15);
    } else if (e.key === "-" || e.key === "_") {
      e.preventDefault();
      zoomAgentImageViewer(-0.15);
    }
  }

  function syncAgentImageViewerZoomInput(force) {
    const inp = $("#aivZoomInput");
    const v = state._imgViewer;
    if (!inp) return;
    if (!force && document.activeElement === inp) return;
    const pct = Math.round(((v && v.scale) || 1) * 100);
    inp.value = String(pct);
  }

  function commitAgentImageViewerZoomInput() {
    const inp = $("#aivZoomInput");
    const v = state._imgViewer;
    if (!inp || !v) return;
    const raw = String(inp.value || "").trim().replace(/%/g, "");
    const n = Number(raw);
    if (!Number.isFinite(n) || n <= 0) {
      syncAgentImageViewerZoomInput(true);
      return;
    }
    v.scale = Math.min(5, Math.max(0.01, n / 100));
    applyAgentImageViewerTransform();
  }

  function applyAgentImageViewerTransform() {
    const img = $("#aivImg");
    const v = state._imgViewer;
    if (!img || !v) return;
    img.style.transform = `translate(${v.panX || 0}px, ${v.panY || 0}px) scale(${v.scale || 1})`;
    syncAgentImageViewerZoomInput();
  }

  function zoomAgentImageViewer(delta) {
    const v = state._imgViewer;
    if (!v) return;
    v.scale = Math.min(5, Math.max(0.01, (v.scale || 1) + delta));
    applyAgentImageViewerTransform();
  }

  function resetAgentImageViewerZoom() {
    const v = state._imgViewer;
    if (!v) return;
    v.scale = 1;
    v.panX = 0;
    v.panY = 0;
    applyAgentImageViewerTransform();
  }

  function stepAgentImageViewer(dir) {
    const v = state._imgViewer;
    if (!v || !v.items || v.items.length < 2) return;
    const n = v.items.length;
    v.index = (v.index + dir + n) % n;
    v.scale = 1;
    v.panX = 0;
    v.panY = 0;
    showAgentImageViewerItem();
  }

  function setAgentImageViewerNavVisible(multi) {
    ["#aivPrev", "#aivNext"].forEach((sel) => {
      const el = $(sel);
      if (!el) return;
      el.hidden = !multi;
    });
    const ov = $("#agentImgViewer");
    if (ov) ov.classList.toggle("aiv-multi", !!multi);
    const counter = $("#aivCounter");
    if (counter) {
      counter.hidden = !multi;
      if (!multi) counter.textContent = "";
    }
  }

  function showAgentImageViewerItem() {
    const v = state._imgViewer;
    const img = $("#aivImg");
    if (!v || !img) return;
    const item = v.items[v.index];
    if (!item) return;
    const multi = (v.items.length | 0) >= 2;
    const counter = $("#aivCounter");
    if (counter) {
      counter.hidden = !multi;
      counter.textContent = multi ? `${v.index + 1}/${v.items.length}` : "";
    }
    setAgentImageViewerNavVisible(multi);
    const url = item.previewUrl || "";
    if (url) {
      img.src = url;
      applyAgentImageViewerTransform();
      return;
    }
    if (window.qst && typeof qst.readImageDataUrl === "function") {
      img.src = "";
      const reqId = "aiv_" + Date.now();
      state._pendingAivReqId = reqId;
      state._pendingThumbById = state._pendingThumbById || {};
      // reuse readImageDataUrl.result via a marker on state
      state._aivPendingPath = item.path;
      qst.readImageDataUrl(item.path, reqId);
    } else {
      img.src = "";
      toast("无法加载图片");
    }
    applyAgentImageViewerTransform();
  }

  function openAgentImageViewer(startPath) {
    const items = collectAgentViewerImages();
    if (!items.length) {
      toast("没有可预览的图片");
      return;
    }
    let index = items.findIndex((x) => x.path === startPath);
    if (index < 0) index = 0;
    state._imgViewer = { items, index, scale: 1, panX: 0, panY: 0 };
    const ov = $("#agentImgViewer");
    if (ov) {
      ov.hidden = false;
      ov.classList.add("show");
      ov.setAttribute("aria-hidden", "false");
    }
    document.addEventListener("keydown", onAgentImageViewerKey);
    showAgentImageViewerItem();
  }

  async function copyAgentImageViewer() {
    const v = state._imgViewer;
    const img = $("#aivImg");
    if (!v || !img || !img.src) {
      toast("没有可复制的图片");
      return;
    }
    try {
      const res = await fetch(img.src);
      const blob = await res.blob();
      if (navigator.clipboard && window.ClipboardItem) {
        await navigator.clipboard.write([new ClipboardItem({ [blob.type || "image/png"]: blob })]);
        toast("已复制图片");
        return;
      }
    } catch (_) {}
    try {
      await navigator.clipboard.writeText(img.src);
      toast("已复制图片地址");
    } catch (_) {
      toast("复制失败");
    }
  }

  function downloadAgentImageViewer() {
    const v = state._imgViewer;
    const img = $("#aivImg");
    if (!v || !v.items || !v.items[v.index]) {
      toast("没有可下载的图片");
      return;
    }
    const item = v.items[v.index];
    const path = String(item.path || "");
    const dataUrl = (img && img.src && String(img.src).startsWith("data:")) ? img.src : (item.previewUrl || "");
    if (!window.qst || typeof qst.saveImageAs !== "function") {
      toast("无桥接");
      return;
    }
    // 真实落盘路径优先；临时粘贴项走 dataUrl
    const usePath = path && !path.startsWith("clipboard-image.") ? path : "";
    if (!usePath && !dataUrl) {
      toast("图片尚未就绪");
      return;
    }
    qst.saveImageAs({ path: usePath, dataUrl: usePath ? "" : dataUrl });
  }

  function requestAgentAttachThumbs(root) {
    const scope = root || document;
    state._pendingThumbById = state._pendingThumbById || {};
    $$("img.agent-att-thumb[data-img-path]", scope).forEach((img) => {
      if (img.getAttribute("src")) return;
      const p = img.dataset.imgPath;
      if (!p || !window.qst || typeof qst.readImageDataUrl !== "function") return;
      const reqId = "att_" + Date.now() + "_" + Math.random().toString(36).slice(2, 8);
      img.dataset.reqId = reqId;
      state._pendingThumbById[reqId] = img;
      qst.readImageDataUrl(p, reqId);
    });
  }

  function markShellReady() {
    if (AGENT_SHELL) return;
    const root = document.documentElement;
    if (root.classList.contains("shell-booting")) {
      root.classList.remove("shell-booting");
      root.classList.add("shell-ready");
      const body = document.querySelector(".views .home-body");
      const done = () => root.classList.remove("shell-ready");
      if (body) body.addEventListener("animationend", done, { once: true });
      setTimeout(done, 500);
    }
    if (window.qst && typeof qst.post === "function" && !state._shellReadyPosted) {
      if (window.__qstShellContentReadyPosted) {
        state._shellReadyPosted = true;
      } else {
        state._shellReadyPosted = true;
        qst.post({ type: "shell.contentReady" });
      }
    }
  }

  function markAgentShellReady() {
    if (!AGENT_SHELL) return;
    const root = document.documentElement;
    if (window.qst && typeof qst.post === "function") {
      state._agentChromeReadyPosted = true;
      qst.post({ type: "agentWindow.contentReady" });
    }
    if (!root.classList.contains("agent-booting") && root.classList.contains("agent-ready")) return;
    root.classList.remove("agent-booting");
    root.classList.add("agent-ready");
    const chat = $("#chatLog");
    const done = () => root.classList.remove("agent-ready");
    if (chat) chat.addEventListener("animationend", done, { once: true });
    setTimeout(done, 400);
  }

  function openAgentSession(id, createNew) {
    // 切换/新建会话前，把当前会话草稿与编辑态落盘
    saveActiveAgentTabDom();
    // 主壳：改走独立 AI WebView 顶层窗
    if (window.qst && !AGENT_SHELL && typeof qst.openAgentWindow === "function") {
      qst.openAgentWindow({
        id: String(id || ""),
        createNew: createNew ? 1 : 0,
      });
      return;
    }
    const wantId = String(id || "");
    if (!createNew && wantId) {
      const existing = findAgentTabById(wantId);
      if (existing) {
        openOv("agent");
        restoreAgentWindow();
        activateAgentTab(existing.key);
        markAgentShellReady();
        return;
      }
    }
    // 新建：复用任意空消息标签（即使已有 id），避免关窗再开出现两个「新对话」
    if (createNew || !wantId) {
      const blank = state.agentTabs.find(
        (t) => !t.busy && !(t.messages && t.messages.length)
      );
      if (blank) {
        openOv("agent");
        restoreAgentWindow();
        activateAgentTab(blank.key, { skipSave: true, skipChat: true });
        // 新建：立刻静默去掉 HTML 骨架，避免空会话也闪骨架
        renderChat([]);
        markAgentShellReady();
        {
          const cfgErr = getAgentConfigError();
          if (cfgErr) toast(cfgErr);
        }
        if (window.qst) {
          qst.openAgentConversation("", true, { peek: 0, panelKey: blank.key });
        }
        return;
      }
    }
    const key = "at" + (++_agentTabSeq);
    const tab = {
      key,
      id: wantId,
      name: createNew || !wantId ? "新对话" : "对话",
      messages: [],
      draft: "",
      attachments: [],
      model: state.agentModel || "",
      busy: false,
    };
    saveActiveAgentTabDom();
    state.agentTabs.push(tab);
    state.agentTabKey = key;
    syncAgentGlobalsFromTab(tab);
    openOv("agent");
    restoreAgentWindow();
    renderAgentTabs();
    // 分批：HTML 已有对话骨架则保留；仅新建时静默清空
    const needFlesh = !!(wantId && !createNew);
    if (needFlesh) {
      if (!state._chatLoading) showChatSkeleton();
    } else {
      // 新建：骨架→空一次完成（renderChat 内部关动画）
      renderChat([]);
    }
    if ($("#chatInput")) $("#chatInput").textContent = "";
    renderAgentAttachments();
    syncAgentSendBtn();
    const title = $("#agentDlgTitle");
    if (title) title.textContent = "AI 脚本助手";
    // 骨架可见即 ready，不锁淡入时长
    markAgentShellReady();
    {
      const cfgErr = getAgentConfigError();
      if (cfgErr) toast(cfgErr);
    }
    if (window.qst) {
      if (anyAgentTabBusy() && (createNew || !wantId)) {
        toast("助手忙碌中，请稍后再开新对话");
        closeAgentTab(key);
        return;
      }
      const peek = anyAgentTabBusy() && !createNew && !!wantId;
      qst.openAgentConversation(wantId, !!createNew || !wantId, {
        peek: peek,
        panelKey: key,
      });
    }
  }

  function syncEditorBatchChrome() {
    const batch = !!state.batchMode;
    const batchBar = $("#batchBar");
    const normalBar = $("#normalBar");
    if (batchBar) batchBar.classList.toggle("show", batch);
    if (normalBar) normalBar.style.display = batch ? "none" : "flex";
    const list = $("#actionList");
    if (list) list.classList.toggle("is-batch", batch);
  }

  function enterEditorBatch(on) {
    state.batchMode = !!on;
    state.batchSel = {};
    state._batchAnchor = -1;
    resetMergeUiChrome();
    if (on) {
      state.actionSel = -1;
      state.editDraft = null;
      state.addPreview = null;
      renderParamPanel(null);
    }
    // 列表已在时只改勾选/选中态，禁止 innerHTML 把滚动条拆掉重建
    if (!state._actionListRendering && patchActionListBatchMode(on)) {
      syncEditorBatchChrome();
      syncEditorActionCount();
      if (on) syncMergeModeUi();
      else if (state.addPreview) renderParamPanel(state.addPreview);
      else showAddTypePreview();
      syncEdRemarkFromSelection();
      return;
    }
    renderEditorActions(state.editorActions);
  }

  function selectedBatchIndices() {
    const out = [];
    for (let i = 0; i < state.editorActions.length; ++i) {
      if (state.batchSel && state.batchSel[i]) out.push(i);
    }
    return out;
  }

  function mergeBatchSelectionBefore() {
    if (!isMergeEligible()) {
      toast("请勾选至少两个同级动作");
      return false;
    }
    runMergeWithPlacement("before");
    return true;
  }

  function debugSelectedBatch(stepMode) {
    const idx = selectedBatchIndices();
    if (!idx.length) {
      toast("请先勾选动作");
      return false;
    }
    startDebugFrom(idx[0], !!stepMode);
    return true;
  }

  function prepareBatchContextSelection(index) {
    if (index < 0 || index >= (state.editorActions || []).length) return;
    if (state.batchSel && state.batchSel[index]) return;
    state.batchSel = {};
    state.batchSel[index] = true;
    state._batchAnchor = index;
    refreshEditorBatchSelectionUi();
  }

  function showEditorBatchContextMenu(clientX, clientY) {
    const hasSel = selectedBatchIndices().length > 0;
    const items = [
      { t: "剪切", v: "cut", disabled: !hasSel },
      { t: "复制", v: "copy", disabled: !hasSel },
      { t: "合并到动作", v: "merge", disabled: !isMergeEligible() },
      { t: "调试选中的步骤（单步）", v: "step", disabled: !hasSel },
      { t: "调试选中的步骤（运行）", v: "run", disabled: !hasSel },
    ];
    requestAnimationFrame(() => {
      showPopup(
        { clientX: clientX, clientY: clientY },
        items,
        (it) => {
          if (!it) return;
          if (it.v === "cut") {
            if (!cutSelectedEditorActions()) toast("请先勾选动作");
          } else if (it.v === "copy") {
            if (!copySelectedEditorActions()) toast("请先勾选动作");
          } else if (it.v === "merge") mergeBatchSelectionBefore();
          else if (it.v === "step") debugSelectedBatch(true);
          else if (it.v === "run") debugSelectedBatch(false);
        },
        { preferUp: true }
      );
    });
  }

  function setEditorVisualSelection(indices) {
    const n = (state.editorActions || []).length;
    const uniq = [];
    const seen = {};
    (indices || []).forEach((i) => {
      const v = i | 0;
      if (v >= 0 && v < n && !seen[v]) {
        seen[v] = true;
        uniq.push(v);
      }
    });
    if (uniq.length >= 2) {
      state.batchMode = true;
      state.batchSel = {};
      uniq.forEach((i) => { state.batchSel[i] = true; });
      state.actionSel = -1;
      state.editDraft = null;
      state.addPreview = null;
      syncEditorBatchChrome();
      syncMergeModeUi();
      return;
    }
    enterEditorBatch(false);
    if (uniq.length === 1) selectEditorAction(uniq[0]);
    else {
      state.actionSel = -1;
      renderParamPanel(null);
      showAddTypePreview();
    }
  }

  function mergeVisualSelection(ordered) {
    const n = (state.editorActions || []).length;
    const uniq = [];
    const seen = {};
    (ordered || []).forEach((i) => {
      const v = i | 0;
      if (v >= 0 && v < n && !seen[v]) {
        seen[v] = true;
        uniq.push(v);
      }
    });
    if (uniq.length < 2) {
      toast("请先选中至少两个动作", "warn");
      return;
    }
    state._mergeRootOrder = uniq.slice();
    state.batchMode = true;
    state.batchSel = {};
    uniq.forEach((i) => { state.batchSel[i] = true; });
    state.actionSel = -1;
    syncEditorBatchChrome();
    syncMergeModeUi();
    if (!isMergeEligible()) {
      const plan = analyzeMergeSelection(state.editorActions, selectedBatchIndices());
      toast((plan && plan.error) || "当前选择无法并入", "warn");
      state._mergeRootOrder = null;
      return;
    }
    const type = (state.addPreview && state.addPreview.type) || state.addActionType;
    const plan = analyzeMergeSelection(state.editorActions, selectedBatchIndices());
    if (type === "defineBlock" || (plan && plan.contiguous)) {
      runMergeWithPlacement(type === "defineBlock" ? "first" : "inplace");
      return;
    }
    const pop = $("#mergePlacementPopup");
    if (pop) {
      pop.classList.remove("hidden");
      const firstBtn = pop.querySelector("[data-place]");
      if (firstBtn) firstBtn.focus();
    }
  }

  function syncEdRemarkFromSelection() {
    const el = $("#edRemark");
    if (!el) return;
    if (state.actionSel >= 0 && state.editorActions[state.actionSel]) {
      el.textContent = state.editorActions[state.actionSel].remark || "";
    } else {
      el.textContent = "";
    }
  }

  function isSubtreeContainerType(type) {
    return (
      type === "loop" ||
      type === "defineBlock" ||
      type === "if" ||
      type === "else"
    );
  }

  function isExpandableContainerType(type) {
    return isSubtreeContainerType(type);
  }

  function isContainerExpanded(index) {
    return !state.collapsedContainers[index];
  }

  function isRowVisibleInTree(index) {
    const actions = state.editorActions;
    if (index < 0 || index >= actions.length) return false;
    let targetLevel = clampIndent(actions[index].indent);
    for (let i = index - 1; i >= 0; --i) {
      if (clampIndent(actions[i].indent) < targetLevel) {
        if (
          isExpandableContainerType(actions[i].type) &&
          !isContainerExpanded(i)
        ) {
          return false;
        }
        targetLevel = clampIndent(actions[i].indent);
        if (targetLevel === 0) break;
      }
    }
    return true;
  }

  function toggleContainerExpand(index) {
    if (!isExpandableContainerType(state.editorActions[index]?.type)) return;
    if (isContainerExpanded(index)) state.collapsedContainers[index] = true;
    else delete state.collapsedContainers[index];
  }

  /** 对齐原生 RemapCollapsedAfterDelete：删除 [start, end) 后重映射折叠容器索引 */
  function remapCollapsedAfterDelete(start, end) {
    const count = end - start;
    const next = {};
    for (const k of Object.keys(state.collapsedContainers)) {
      if (!state.collapsedContainers[k]) continue;
      const idx = Number(k);
      if (idx >= start && idx < end) continue; // 被删除的折叠节点直接丢弃
      next[idx >= end ? idx - count : idx] = true;
    }
    state.collapsedContainers = next;
  }

  /**
   * 对齐原生 RemapCollapsedAfterMove：移动动作块后重映射折叠容器索引。
   * insertIndex 为移除原块后的插入点（与 moveActionSubtree 内部 insert 一致）。
   */
  function remapCollapsedAfterMove(dragStart, dragEnd, insertIndex, blockSize) {
    const next = {};
    for (const k of Object.keys(state.collapsedContainers)) {
      if (!state.collapsedContainers[k]) continue;
      const idx = Number(k);
      if (idx >= dragStart && idx < dragEnd) {
        next[idx - dragStart + insertIndex] = true; // 被移动的节点映射到新位置
        continue;
      }
      let mapped = idx;
      if (insertIndex < dragStart) {
        // 向前拖：中间区间 [insertIndex, dragStart) 整体右移 blockSize
        if (mapped >= insertIndex && mapped < dragStart) mapped += blockSize;
      } else {
        // 向后拖：中间区间 [dragEnd, insertIndex + blockSize) 整体左移 blockSize
        if (mapped >= dragEnd && mapped < insertIndex + blockSize) mapped -= blockSize;
      }
      next[mapped] = true;
    }
    state.collapsedContainers = next;
  }

  /** 对齐原生 InsertAction：在 pos 插入动作后，pos 及之后的折叠索引整体 +1 */
  function shiftCollapsedAfterInsert(pos) {
    const next = {};
    for (const k of Object.keys(state.collapsedContainers)) {
      if (!state.collapsedContainers[k]) continue;
      const idx = Number(k);
      next[idx >= pos ? idx + 1 : idx] = true;
    }
    state.collapsedContainers = next;
  }

  function remapIndexDictAfterReplace(dict, start, oldCount, newCount) {
    const next = {};
    const oldEnd = start + oldCount;
    const delta = newCount - oldCount;
    for (const k of Object.keys(dict || {})) {
      if (!dict[k]) continue;
      const idx = Number(k);
      if (idx === start) continue;
      if (idx > start && idx < oldEnd) {
        // 拆容器：去掉壳后，原子节点整体前移 1
        next[idx - 1] = dict[k];
        continue;
      }
      next[idx >= oldEnd ? idx + delta : idx] = dict[k];
    }
    return next;
  }

  function isDisassemblableType(type) {
    return (
      type === "loop" ||
      type === "if" ||
      type === "else" ||
      type === "defineBlock" ||
      type === "runBlock" ||
      type === "runMacro" ||
      type === "mousePlayback"
    );
  }

  function cloneActionForInline(src) {
    const a = JSON.parse(JSON.stringify(src || {}));
    delete a.no;
    delete a.originalNo;
    delete a._preview;
    delete a._runBlockTyped;
    a.name = typeLabel(a.type);
    return a;
  }

  function remapCopiedIndents(list, destIndent) {
    if (!list.length) return list;
    let srcMin = clampIndent(list[0].indent);
    for (const a of list) srcMin = Math.min(srcMin, clampIndent(a.indent));
    const delta = (destIndent | 0) - srcMin;
    for (const a of list) a.indent = clampIndent((a.indent | 0) + delta);
    return list;
  }

  function findLastDefineBlockIndex(actions, name) {
    const key = String(name || "").trim();
    let found = -1;
    if (!key) return found;
    for (let i = 0; i < actions.length; ++i) {
      if (actions[i] && actions[i].type === "defineBlock" && String(actions[i].blockName || "").trim() === key) {
        found = i;
      }
    }
    return found;
  }

  function copyContainerBody(actions, containerIndex) {
    const end = subtreeEnd(containerIndex);
    if (end <= containerIndex + 1) return [];
    return actions.slice(containerIndex + 1, end).map(cloneActionForInline);
  }

  function applyDisassemblePatch(start, oldCount, inserted) {
    visualNoteUndo();
    state.collapsedContainers = remapIndexDictAfterReplace(
      state.collapsedContainers,
      start,
      oldCount,
      inserted.length
    );
    state.editorBreakpoints = remapIndexDictAfterReplace(
      state.editorBreakpoints,
      start,
      oldCount,
      inserted.length
    );
    state.editorActions.splice(start, oldCount, ...inserted);
    visualOnReplace(start, oldCount, inserted.length);
    state.actionSel = inserted.length ? start : -1;
    state.editDraft = null;
    state.addPreview = null;
    renderEditorActions(state.editorActions);
    if (state.actionSel >= 0) {
      loadEditDraftFromSelection();
      const a = state.editDraft;
      if (a && a.type) {
        state.addActionType = a.type;
        const typeCombo = $("#edActionType");
        const cur = ACTION_TYPES.find((x) => x.v === a.type);
        if (typeCombo && cur) typeCombo.textContent = cur.t;
      }
    } else {
      showAddTypePreview();
    }
  }

  function peekScriptActions(path) {
    return new Promise((resolve) => {
      if (!window.qst || typeof qst.peekScriptActions !== "function") {
        resolve({ ok: false, detail: "当前环境无法读取脚本" });
        return;
      }
      const reqId = "peek-" + Date.now() + "-" + Math.random().toString(16).slice(2);
      if (!state._peekWaiters) state._peekWaiters = {};
      const timer = setTimeout(() => {
        if (state._peekWaiters && state._peekWaiters[reqId]) {
          delete state._peekWaiters[reqId];
          resolve({ ok: false, detail: "读取超时" });
        }
      }, 8000);
      state._peekWaiters[reqId] = (msg) => {
        clearTimeout(timer);
        resolve(msg || { ok: false, detail: "读取失败" });
      };
      qst.peekScriptActions(path, reqId);
    });
  }

  async function disassembleEditorAction(index) {
    const actions = state.editorActions || [];
    const target = actions[index];
    if (!target || !isDisassemblableType(target.type)) {
      toast("该动作不能拆解");
      return;
    }
    const destIndent = clampIndent(target.indent);
    const t = target.type;
    if (t === "loop" || t === "if" || t === "else" || t === "defineBlock") {
      const body = copyContainerBody(actions, index);
      if (!body.length) {
        toast("没有可拆解的子动作");
        return;
      }
      remapCopiedIndents(body, destIndent);
      applyDisassemblePatch(index, subtreeEnd(index) - index, body);
      return;
    }
    if (t === "runBlock") {
      const name = String(target.blockName || "").trim();
      if (!name) {
        toast("请先选择要运行的宏指令块");
        return;
      }
      const def = findLastDefineBlockIndex(actions, name);
      if (def < 0) {
        toast("未找到宏指令块「" + name + "」的定义");
        return;
      }
      const body = copyContainerBody(actions, def);
      if (!body.length) {
        toast("宏指令块「" + name + "」没有可拆解的动作");
        return;
      }
      remapCopiedIndents(body, destIndent);
      applyDisassemblePatch(index, 1, body);
      return;
    }
    const path = String(target.targetPath || "").trim();
    if (!path) {
      toast(t === "mousePlayback" ? "请先选择用于回放的键鼠录制" : "请先选择要运行的鼠标宏");
      return;
    }
    const peeked = await peekScriptActions(path);
    if (!peeked || !peeked.ok) {
      toast((peeked && peeked.detail) || (t === "mousePlayback" ? "找不到要拆解的录制" : "找不到要拆解的鼠标宏"));
      return;
    }
    const nested = Array.isArray(peeked.actions) ? peeked.actions.map(cloneActionForInline) : [];
    if (!nested.length) {
      toast(t === "mousePlayback" ? "录制没有可拆解的动作" : "鼠标宏没有可拆解的动作");
      return;
    }
    remapCopiedIndents(nested, destIndent);
    applyDisassemblePatch(index, 1, nested);
  }

  function resetMergeUiChrome() {
    _mergeUiOn = false;
    const lbl = $("#edActionTypeLbl");
    if (lbl) lbl.textContent = "请选择要添加的宏";
  }

  function isMergeContainerType(type) {
    return type === "loop" || type === "if" || type === "else" || type === "defineBlock";
  }

  function mergeSubtreeEndOn(actions, fromIdx) {
    if (fromIdx < 0 || fromIdx >= actions.length) return fromIdx;
    if (!isSubtreeContainerType(actions[fromIdx].type)) return fromIdx + 1;
    return containerBodyEndOn(actions, fromIdx);
  }

  function containerBodyEndOn(actions, containerIndex) {
    if (containerIndex < 0 || containerIndex >= actions.length) return containerIndex + 1;
    const level = clampIndent(actions[containerIndex].indent);
    let i = containerIndex + 1;
    while (i < actions.length && clampIndent(actions[i].indent) > level) i += 1;
    return i;
  }

  function findDirectParentIndexOn(actions, index) {
    const indent = clampIndent(actions[index].indent);
    if (indent <= 0) return -1;
    for (let i = index - 1; i >= 0; --i) {
      if (clampIndent(actions[i].indent) < indent) return i;
    }
    return -1;
  }

  function mergeSelectedRoots(actions, selected) {
    const n = actions.length;
    const seen = {};
    const uniq = [];
    (selected || []).forEach((i) => {
      const idx = i | 0;
      if (idx >= 0 && idx < n && !seen[idx]) {
        seen[idx] = true;
        uniq.push(idx);
      }
    });
    uniq.sort((a, b) => a - b);
    const sel = {};
    uniq.forEach((i) => {
      sel[i] = true;
    });
    const roots = [];
    uniq.forEach((i) => {
      let covered = false;
      let p = findDirectParentIndexOn(actions, i);
      while (p >= 0) {
        if (sel[p]) {
          covered = true;
          break;
        }
        p = findDirectParentIndexOn(actions, p);
      }
      if (!covered) roots.push(i);
    });
    return roots;
  }

  function directSiblingIndices(actions, parent, childIndent) {
    const out = [];
    if (parent < 0) {
      for (let i = 0; i < actions.length; ++i) {
        if (clampIndent(actions[i].indent) === childIndent) out.push(i);
      }
      return out;
    }
    if (parent >= actions.length) return out;
    const expect = clampIndent(actions[parent].indent) + 1;
    if (expect !== childIndent) return out;
    const end = containerBodyEndOn(actions, parent);
    for (let i = parent + 1; i < end; ++i) {
      if (clampIndent(actions[i].indent) === expect) out.push(i);
    }
    return out;
  }

  function analyzeMergeSelection(actions, selected) {
    const roots = mergeSelectedRoots(actions, selected);
    if (roots.length < 2) {
      return { ok: false, error: "请勾选至少两个同级动作", roots: [] };
    }
    const indent = clampIndent(actions[roots[0]].indent);
    const parent = findDirectParentIndexOn(actions, roots[0]);
    for (let k = 1; k < roots.length; ++k) {
      const ind = clampIndent(actions[roots[k]].indent);
      const p = findDirectParentIndexOn(actions, roots[k]);
      if (ind !== indent || p !== parent) {
        return { ok: false, error: "请勾选同一父节点下的同级动作", roots };
      }
    }
    const siblings = directSiblingIndices(actions, parent, indent);
    const sib = {};
    siblings.forEach((i) => {
      sib[i] = true;
    });
    for (let r = 0; r < roots.length; ++r) {
      if (!sib[roots[r]]) {
        return { ok: false, error: "请勾选同一父节点下的同级动作", roots };
      }
    }
    const rootSet = {};
    roots.forEach((i) => {
      rootSet[i] = true;
    });
    let firstK = -1;
    let lastK = -1;
    for (let k = 0; k < siblings.length; ++k) {
      if (!rootSet[siblings[k]]) continue;
      if (firstK < 0) firstK = k;
      lastK = k;
    }
    let contiguous = firstK >= 0;
    if (contiguous) {
      for (let k = firstK; k <= lastK; ++k) {
        if (!rootSet[siblings[k]]) {
          contiguous = false;
          break;
        }
      }
    }
    return { ok: true, error: "", roots, parent, indent, contiguous };
  }

  function isMergeEligible() {
    if (!state.batchMode) return false;
    const plan = analyzeMergeSelection(state.editorActions || [], selectedBatchIndices());
    return !!(plan && plan.ok);
  }

  function elseHasPrecedingIf(actions, insertPos, elseIndent) {
    for (let i = insertPos - 1; i >= 0; --i) {
      const ind = clampIndent(actions[i].indent);
      if (ind < elseIndent) return false;
      if (ind === elseIndent) return actions[i].type === "if";
    }
    return false;
  }

  function remainingCountBefore(origIndex, ranges) {
    let count = 0;
    for (let i = 0; i < origIndex; ++i) {
      let extracted = false;
      for (let r = 0; r < ranges.length; ++r) {
        if (i >= ranges[r].start && i < ranges[r].end) {
          extracted = true;
          break;
        }
      }
      if (!extracted) count += 1;
    }
    return count;
  }

  function mergeSelectedIntoContainer(actions, selected, container, placement) {
    if (!isMergeContainerType(container.type)) {
      return { ok: false, error: "该动作不能作为并入容器" };
    }
    const plan = analyzeMergeSelection(actions, selected);
    if (!plan.ok) return { ok: false, error: plan.error };
    if (placement === "inplace" && !plan.contiguous) {
      return { ok: false, error: "所选动作不连续，请选择插入位置" };
    }
    const asDefine = container.type === "defineBlock";
    const destIndent = asDefine ? 0 : plan.indent;
    const ranges = [];
    const body = [];
    let roots = plan.roots.slice();
    if (state._mergeRootOrder && state._mergeRootOrder.length) {
      const keep = {};
      roots.forEach((i) => {
        keep[i] = true;
      });
      const ordered = [];
      state._mergeRootOrder.forEach((i) => {
        if (keep[i]) {
          ordered.push(i);
          delete keep[i];
        }
      });
      roots.forEach((i) => {
        if (keep[i]) ordered.push(i);
      });
      roots = ordered;
    }
    for (let r = 0; r < roots.length; ++r) {
      const root = roots[r];
      const end = mergeSubtreeEndOn(actions, root);
      ranges.push({ start: root, end });
      for (let i = root; i < end; ++i) body.push(cloneActionForInline(actions[i]));
    }
    remapCopiedIndents(body, destIndent + 1);
    const n = actions.length;
    let extracted = 0;
    ranges.forEach((rg) => {
      extracted += rg.end - rg.start;
    });
    let insertPos = 0;
    if (asDefine) insertPos = 0;
    else if (placement === "last") insertPos = n - extracted;
    else if (placement === "first") insertPos = 0;
    else if (placement === "after") {
      insertPos = remainingCountBefore(
        mergeSubtreeEndOn(actions, plan.roots[plan.roots.length - 1]),
        ranges
      );
    } else {
      insertPos = remainingCountBefore(plan.roots[0], ranges);
    }
    insertPos = Math.max(0, Math.min(insertPos, n - extracted));

    const next = actions.slice();
    for (let i = ranges.length - 1; i >= 0; --i) {
      next.splice(ranges[i].start, ranges[i].end - ranges[i].start);
    }
    if (container.type === "else" && !elseHasPrecedingIf(next, insertPos, destIndent)) {
      return { ok: false, error: "请将「条件-否则」放在同级「条件-如果」后面" };
    }
    const head = cloneActionForInline(container);
    head.indent = destIndent;
    delete head.no;
    delete head.originalNo;
    next.splice(insertPos, 0, head, ...body);
    const endLoopErr = validateEndLoopPlacements(next);
    if (endLoopErr) return { ok: false, error: endLoopErr };
    return {
      ok: true,
      actions: next,
      containerIndex: insertPos,
      ranges,
      insertPos,
      insertedCount: 1 + body.length,
    };
  }

  function remapIndexDictAfterMerge(dict, ranges, insertPos, insertedCount) {
    const next = {};
    for (const k of Object.keys(dict || {})) {
      if (!dict[k]) continue;
      const oldIdx = Number(k);
      let offsetInBody = 0;
      let moved = false;
      for (let r = 0; r < ranges.length; ++r) {
        const rg = ranges[r];
        if (oldIdx >= rg.start && oldIdx < rg.end) {
          next[insertPos + 1 + offsetInBody + (oldIdx - rg.start)] = dict[k];
          moved = true;
          break;
        }
        offsetInBody += rg.end - rg.start;
      }
      if (moved) continue;
      let removedBefore = 0;
      for (let r = 0; r < ranges.length; ++r) {
        if (ranges[r].end <= oldIdx) removedBefore += ranges[r].end - ranges[r].start;
      }
      let idx = oldIdx - removedBefore;
      if (idx >= insertPos) idx += insertedCount;
      next[idx] = dict[k];
    }
    return next;
  }

  function applyMergeResult(result) {
    if (!result || !result.ok) return;
    state.collapsedContainers = remapIndexDictAfterMerge(
      state.collapsedContainers,
      result.ranges,
      result.insertPos,
      result.insertedCount
    );
    state.editorBreakpoints = remapIndexDictAfterMerge(
      state.editorBreakpoints,
      result.ranges,
      result.insertPos,
      result.insertedCount
    );
    delete state.collapsedContainers[result.containerIndex];
    state.editorActions = result.actions;
    state.actionSel = -1;
    state.editDraft = null;
    state.batchSel = {};
    state.batchSel[result.containerIndex] = true;
    renderEditorActions(state.editorActions);
    state._mergeRootOrder = null;
    const V = window.QstVisualEditor;
    if (V && typeof V.isVisual === "function" && V.isVisual() && typeof V.afterMerge === "function") {
      V.afterMerge(result.containerIndex, result);
    }
  }

  function syncMergeModeUi() {
    const lbl = $("#edActionTypeLbl");
    if (!state.batchMode) {
      _mergeUiOn = false;
      if (lbl) lbl.textContent = "请选择要添加的宏";
      return "normal";
    }
    const on = isMergeEligible();
    const was = _mergeUiOn;
    _mergeUiOn = on;
    if (lbl) lbl.textContent = on ? "请选择要并入的宏" : "请选择要添加的宏";
    if (on && !was) {
      if (!isMergeContainerType(state.addActionType)) state.addActionType = "loop";
      showAddTypePreview();
      return "enter";
    }
    if (!on && was) {
      state.addPreview = null;
      renderParamPanel(null);
      return "leave";
    }
    if (!on) {
      renderParamPanel(null);
      return "batch";
    }
    return "stay";
  }

  function runMergeWithPlacement(placement) {
    const remark = ($("#edRemark")?.textContent || "").trim();
    if (state.addPreview) readParamPanelInto(state.addPreview);
    const src =
      state.addPreview && isMergeContainerType(state.addPreview.type)
        ? state.addPreview
        : defaultAction(state.addActionType, remark);
    const container = cloneActionForInline(src);
    container.remark = remark;
    container.name = typeLabel(container.type);
    if (container.type === "defineBlock" && !validateDefineBlockName(container.blockName, -1)) {
      return;
    }
    const result = mergeSelectedIntoContainer(
      state.editorActions.slice(),
      selectedBatchIndices(),
      container,
      placement
    );
    if (!result.ok) {
      toast(result.error || "并入失败");
      return;
    }
    visualNoteUndo();
    applyMergeResult(result);
    state._mergeRootOrder = null;
    const remarkEl = $("#edRemark");
    if (remarkEl) remarkEl.textContent = "";
  }

  function visibleActionIndices() {
    const out = [];
    for (let i = 0; i < state.editorActions.length; ++i) {
      if (isRowVisibleInTree(i)) out.push(i);
    }
    return out;
  }

  function subtreeEnd(fromIdx) {
    const actions = state.editorActions;
    if (fromIdx < 0 || fromIdx >= actions.length) return fromIdx;
    const baseIndent = clampIndent(actions[fromIdx].indent);
    let end = fromIdx + 1;
    while (end < actions.length && clampIndent(actions[end].indent) > baseIndent) {
      end += 1;
    }
    return end;
  }

  // 一次性算好所有子树的结束下标（首个 indent <= 该容器 indent 的行索引），O(n)。
  function computeSubtreeEnds() {
    const actions = state.editorActions || [];
    const n = actions.length;
    const ends = new Array(n).fill(n);
    const stack = []; // {index, indent}
    for (let i = 0; i < n; ++i) {
      const indent = clampIndent(actions[i].indent);
      while (stack.length && stack[stack.length - 1].indent >= indent) {
        ends[stack.pop().index] = i;
      }
      stack.push({ index: i, indent });
    }
    return ends;
  }

  // O(n) 单趟：行可见性（折叠祖先隐藏）与嵌套着色覆盖容器，替代渲染时每行 O(i) 回扫。
  function precomputeRowLayout() {
    const actions = state.editorActions || [];
    const n = actions.length;
    const ends = computeSubtreeEnds();
    const visible = new Array(n).fill(true);
    const cover = new Array(n);
    const visStack = []; // {indent, end, collapsed}
    const covStack = []; // {index, indent, end}
    for (let i = 0; i < n; ++i) {
      const indent = clampIndent(actions[i].indent);
      while (visStack.length && visStack[visStack.length - 1].end <= i) visStack.pop();
      while (visStack.length && visStack[visStack.length - 1].indent >= indent) visStack.pop();
      let vis = true;
      for (const s of visStack) if (s.collapsed) { vis = false; break; }
      visible[i] = vis;

      while (covStack.length && covStack[covStack.length - 1].end <= i) covStack.pop();
      while (covStack.length && covStack[covStack.length - 1].indent >= indent) covStack.pop();
      cover[i] = covStack.map((c) => c.index);

      if (isExpandableContainerType(actions[i].type)) {
        visStack.push({ indent, end: ends[i], collapsed: !isContainerExpanded(i) });
        covStack.push({ index: i, indent, end: ends[i] });
        // 原实现把「有子节点的容器行自身」也计入着色，保持渲染一致
        if (ends[i] > i + 1) cover[i] = cover[i].concat(i);
      }
    }
    return { visible, cover, ends };
  }

  const END_LOOP_NEEDS_LOOP_MSG = "请将结束循环放在循环内";

  function hasLoopParentAt(actions, index, actionIndent) {
    let targetLevel = actionIndent | 0;
    for (let i = index - 1; i >= 0; --i) {
      const ancestor = actions[i];
      const ind = clampIndent(ancestor.indent);
      if (ind < targetLevel) {
        if (ancestor.type === "loop") return true;
        targetLevel = ind;
      }
    }
    return false;
  }

  function validateEndLoopPlacements(actions) {
    const list = actions || [];
    for (let i = 0; i < list.length; ++i) {
      if (list[i].type !== "endLoop") continue;
      if (!hasLoopParentAt(list, i, list[i].indent | 0)) {
        return `第 ${i + 1} 个动作（结束循环）：${END_LOOP_NEEDS_LOOP_MSG}`;
      }
    }
    return "";
  }

  function isValidBlockName(name) {
    const s = String(name || "").trim();
    if (!s) return false;
    if (!/^[A-Za-z]/.test(s)) return false;
    return /^[A-Za-z][A-Za-z0-9]*$/.test(s);
  }

  function isBlockNameDuplicate(name, excludeIndex) {
    const key = String(name || "").trim();
    for (let i = 0; i < state.editorActions.length; ++i) {
      if (i === excludeIndex) continue;
      const a = state.editorActions[i];
      if (a && a.type === "defineBlock" && String(a.blockName || "").trim() === key) return true;
    }
    return false;
  }

  function validateDefineBlockName(name, excludeIndex) {
    if (!isValidBlockName(name)) {
      toast("块名称只能以字母开始，后面只能包含字母和数字。");
      return false;
    }
    if (isBlockNameDuplicate(name, excludeIndex)) {
      toast("块名称不能重复。");
      return false;
    }
    return true;
  }

  const EDITOR_UNDO_MAX = 50;

  function cloneEditorJson(v) {
    return JSON.parse(JSON.stringify(v));
  }

  function isEditorVisualMode() {
    const V = window.QstVisualEditor;
    return !!(V && typeof V.isVisual === "function" && V.isVisual());
  }

  function resetEditorHistory(opts) {
    state.editorUndo = [];
    state.editorRedo = [];
    if (!(opts && opts.keepClipboard)) state.editorClipboard = null;
  }

  function captureEditorHistory() {
    const V = window.QstVisualEditor;
    let visual = null;
    try {
      visual = V && typeof V.collectHistory === "function" ? V.collectHistory() : null;
    } catch (_) {
      visual = null;
    }
    return {
      actions: cloneEditorJson(state.editorActions || []),
      actionSel: state.actionSel | 0,
      batchMode: !!state.batchMode,
      batchSel: cloneEditorJson(state.batchSel || {}),
      collapsed: cloneEditorJson(state.collapsedContainers || {}),
      breakpoints: cloneEditorJson(state.editorBreakpoints || {}),
      visual: visual,
    };
  }

  function applyEditorSnapshot(snap) {
    if (!snap) return;
    state.editorActions = Array.isArray(snap.actions) ? cloneEditorJson(snap.actions) : [];
    state.actionSel = snap.actionSel | 0;
    if (state.actionSel >= state.editorActions.length) state.actionSel = -1;
    state.batchMode = !!snap.batchMode;
    state.batchSel = snap.batchSel && typeof snap.batchSel === "object" ? cloneEditorJson(snap.batchSel) : {};
    state.collapsedContainers =
      snap.collapsed && typeof snap.collapsed === "object" ? cloneEditorJson(snap.collapsed) : {};
    state.editorBreakpoints =
      snap.breakpoints && typeof snap.breakpoints === "object" ? cloneEditorJson(snap.breakpoints) : {};
    state.addPreview = null;
    const V = window.QstVisualEditor;
    if (V && typeof V.applyHistory === "function") {
      try {
        V.applyHistory(snap.visual || null);
      } catch (_) {}
    }
    if (state.batchMode) {
      state.editDraft = null;
      syncEditorBatchChrome();
      syncMergeModeUi();
    } else {
      resetMergeUiChrome();
      state._mergeRootOrder = null;
      syncEditorBatchChrome();
      if (state.actionSel >= 0) loadEditDraftFromSelection();
      else {
        state.editDraft = null;
        showAddTypePreview();
      }
    }
    renderEditorActions(state.editorActions);
  }

  function pushEditorHistory() {
    const V = window.QstVisualEditor;
    if (V && typeof V.isHistoryHeld === "function" && V.isHistoryHeld()) return false;
    try {
      if (!state.editorUndo) state.editorUndo = [];
      state.editorUndo.push(captureEditorHistory());
      if (state.editorUndo.length > EDITOR_UNDO_MAX) state.editorUndo.shift();
      state.editorRedo = [];
      return true;
    } catch (_) {
      return false;
    }
  }

  function undoEditorHistory() {
    if (!state.editorUndo || !state.editorUndo.length) return false;
    if (!state.editorRedo) state.editorRedo = [];
    const current = captureEditorHistory();
    const prev = state.editorUndo.pop();
    state.editorRedo.push(current);
    if (state.editorRedo.length > EDITOR_UNDO_MAX) state.editorRedo.shift();
    applyEditorSnapshot(prev);
    return true;
  }

  function redoEditorHistory() {
    if (!state.editorRedo || !state.editorRedo.length) return false;
    if (!state.editorUndo) state.editorUndo = [];
    const current = captureEditorHistory();
    const next = state.editorRedo.pop();
    state.editorUndo.push(current);
    if (state.editorUndo.length > EDITOR_UNDO_MAX) state.editorUndo.shift();
    applyEditorSnapshot(next);
    return true;
  }

  function revertLastEditorHistory() {
    if (!state.editorUndo || !state.editorUndo.length) return false;
    const prev = state.editorUndo.pop();
    applyEditorSnapshot(prev);
    return true;
  }

  function discardEditorHistory() {
    if (state.editorUndo && state.editorUndo.length) state.editorUndo.pop();
  }

  function visualNoteUndo() {
    return pushEditorHistory();
  }

  function selectedEditorActionRoots() {
    const list = state.editorActions || [];
    let idxs = [];
    if (state.batchMode) idxs = selectedBatchIndices();
    if (!idxs.length && state.actionSel >= 0 && state.actionSel < list.length) {
      idxs = [state.actionSel];
    }
    if (!idxs.length) return [];
    const roots = [];
    for (let i = 0; i < idxs.length; i++) {
      const idx = idxs[i];
      let covered = false;
      for (let j = 0; j < idxs.length; j++) {
        const other = idxs[j];
        if (other < idx && subtreeEnd(other) > idx) {
          covered = true;
          break;
        }
      }
      if (!covered) roots.push(idx);
    }
    return roots;
  }

  function uniquifyPastedBlockName(name, used) {
    let base = String(name || "Block").trim();
    if (!isValidBlockName(base)) base = "Block";
    let n = base;
    let i = 2;
    while (used[n] || isBlockNameDuplicate(n, -1)) {
      n = base + i;
      i += 1;
    }
    used[n] = true;
    return n;
  }

  function pasteAnchorIndex() {
    const list = state.editorActions || [];
    if (state.batchMode) {
      const roots = selectedEditorActionRoots();
      if (roots.length) return roots[roots.length - 1];
      return -1;
    }
    if (state.actionSel >= 0 && state.actionSel < list.length) return state.actionSel;
    return -1;
  }

  function editorHasClipboard() {
    const blocks = state.editorClipboard;
    if (!blocks || !blocks.length) return false;
    for (let i = 0; i < blocks.length; i++) {
      if (blocks[i] && blocks[i].length) return true;
    }
    return false;
  }

  function resolveEditorOpRoots(opts) {
    opts = opts || {};
    const list = state.editorActions || [];
    let roots;
    if (Array.isArray(opts.roots) && opts.roots.length) roots = opts.roots.slice();
    else if (opts.index != null && opts.index >= 0) roots = [opts.index | 0];
    else roots = selectedEditorActionRoots();
    return roots.filter((i) => i >= 0 && i < list.length);
  }

  function copySelectedEditorActions(opts) {
    opts = opts || {};
    if (isEditorVisualMode() && !opts.allowVisual) return false;
    const roots = resolveEditorOpRoots(opts);
    if (!roots.length) return false;
    const blocks = [];
    for (let r = 0; r < roots.length; r++) {
      const i = roots[r];
      const end = subtreeEnd(i);
      const block = (state.editorActions || []).slice(i, end).map((a) => {
        const c = cloneEditorJson(a);
        delete c._preview;
        return c;
      });
      if (block.length) blocks.push(block);
    }
    if (!blocks.length) return false;
    state.editorClipboard = blocks;
    const V = window.QstVisualEditor;
    if (V && typeof V.setClipboard === "function") V.setClipboard(blocks[0][0]);
    const n = blocks.reduce((s, b) => s + b.length, 0);
    if (!opts.silent) toast(n > 1 ? "已复制 " + n + " 条动作" : "已复制");
    return true;
  }

  function cutSelectedEditorActions(opts) {
    opts = opts || {};
    const visual = isEditorVisualMode();
    const V = window.QstVisualEditor;
    const roots = resolveEditorOpRoots(opts);
    if (!roots.length) return false;
    if (visual && !state.batchMode && roots.length === 1 && typeof V.cutCard === "function") {
      return !!V.cutCard(roots[0]);
    }
    if (!copySelectedEditorActions({ silent: true, allowVisual: true, roots })) return false;
    const n = (state.editorClipboard || []).reduce((s, b) => s + ((b && b.length) || 0), 0);
    if (!deleteSelectedEditorActions({ skipEdge: true, roots })) return false;
    toast(n > 1 ? "已剪切 " + n + " 条动作" : "已剪切");
    return true;
  }

  function pasteEditorClipboardAfterSelection(opts) {
    opts = opts || {};
    if (isEditorVisualMode()) return false;
    const blocks = state.editorClipboard;
    if (!blocks || !blocks.length) return false;
    const list = state.editorActions || [];
    let pos;
    let destIndent;
    if (!list.length) {
      pos = 0;
      destIndent = 0;
    } else {
      const sel = opts.index != null && opts.index >= 0 ? opts.index | 0 : pasteAnchorIndex();
      if (sel < 0 || sel >= list.length) return false;
      pos = subtreeEnd(sel);
      destIndent = clampIndent(list[sel].indent);
    }
    const toInsert = [];
    const usedNames = {};
    for (let b = 0; b < blocks.length; b++) {
      const copy = cloneEditorJson(blocks[b]);
      remapCopiedIndents(copy, destIndent);
      for (let i = 0; i < copy.length; i++) {
        const a = copy[i];
        delete a._preview;
        a.name = typeLabel(a.type);
        if (a.type === "defineBlock") a.blockName = uniquifyPastedBlockName(a.blockName, usedNames);
        toInsert.push(a);
      }
    }
    if (!toInsert.length) return false;
    const combined = list.slice(0, pos).concat(toInsert).concat(list.slice(pos));
    const err = validateEndLoopPlacements(combined);
    if (err) {
      toast(err);
      return false;
    }
    visualNoteUndo();
    for (let i = 0; i < toInsert.length; i++) {
      list.splice(pos + i, 0, toInsert[i]);
      visualOnInsert(pos + i);
      shiftCollapsedAfterInsert(pos + i);
      if (isSubtreeContainerType(toInsert[i].type)) delete state.collapsedContainers[pos + i];
    }
    if (state.batchMode) {
      state.batchMode = false;
      state.batchSel = {};
      resetMergeUiChrome();
      syncEditorBatchChrome();
    }
    state.actionSel = pos;
    state.addPreview = null;
    loadEditDraftFromSelection();
    renderEditorActions(state.editorActions);
    focusEditorActionList();
    return true;
  }

  function deleteSelectedEditorActions(opts) {
    opts = opts || {};
    const V = window.QstVisualEditor;
    const visual = isEditorVisualMode();
    if (!opts.skipEdge && visual && typeof V.deleteSelectedEdgeIfAny === "function" && V.deleteSelectedEdgeIfAny()) {
      return true;
    }
    const roots = resolveEditorOpRoots(opts);
    if (!roots.length) return false;
    if (visual && !state.batchMode && roots.length === 1 && typeof V.deleteCard === "function") {
      V.deleteCard(roots[0]);
      return true;
    }
    visualNoteUndo();
    const sorted = roots.slice().sort((a, b) => b - a);
    for (let k = 0; k < sorted.length; k++) {
      const idx = sorted[k];
      if (idx < 0 || idx >= (state.editorActions || []).length) continue;
      deleteSubtreeAt(idx, { skipUndo: true });
    }
    if (state.batchMode) {
      state.batchMode = false;
      state.batchSel = {};
      resetMergeUiChrome();
      syncEditorBatchChrome();
    }
    if (visual && typeof V.afterListDelete === "function") V.afterListDelete();
    state.addPreview = null;
    renderEditorActions(state.editorActions);
    if (state.actionSel < 0 && !state.batchMode) showAddTypePreview();
    return true;
  }

  function editorShortcutBlockedByOverlay() {
    const ids = [
      "ov-hotkey",
      "ov-action-key",
      "ov-prompt",
      "ov-rename",
      "ov-crop",
      "ov-pick",
      "ov-ocr",
      "ov-opt",
      "ov-theme",
      "ov-settings",
      "ov-sched",
      "ov-sched-create",
      "ov-editor-settings",
      "ov-driver-ic",
      "ov-driver-vhid",
      "ov-interval",
    ];
    for (let i = 0; i < ids.length; i++) {
      const el = document.getElementById(ids[i]);
      if (el && el.classList.contains("show")) return true;
    }
    if ($("#ov-agent")?.classList.contains("show") && !state.agentMinimized) return true;
    const imgV = $("#agentImgViewer");
    if (imgV && !imgV.hidden && imgV.classList.contains("show")) return true;
    if (document.querySelector(".popup-menu.show")) return true;
    return false;
  }

  function onEditorActionShortcut(ev) {
    if (!state.editor) return;
    const visEd = window.QstVisualEditor;
    if (visEd && typeof visEd.isInteracting === "function" && visEd.isInteracting()) return;
    const ctrl = !!(ev.ctrlKey || ev.metaKey);
    const key = ev.key || "";
    if (!ctrl && !ev.altKey && key === "Escape") {
      if (document.querySelector(".popup-menu.show")) {
        ev.preventDefault();
        ev.stopPropagation();
        hidePopup();
        return;
      }
      if (editorShortcutBlockedByOverlay()) return;
      if (state.batchMode) {
        ev.preventDefault();
        enterEditorBatch(false);
      }
      return;
    }
    if (isEditableTypingTarget(ev.target) || isEditableTypingTarget(document.activeElement)) {
      if (
        !ctrl &&
        !ev.altKey &&
        (key === "Delete" || key === "Backspace") &&
        isEditorVisualMode()
      ) {
        const V = window.QstVisualEditor;
        if (V && typeof V.deleteSelectedEdgeIfAny === "function" && V.deleteSelectedEdgeIfAny()) {
          ev.preventDefault();
          ev.stopPropagation();
        }
      }
      return;
    }
    if (editorShortcutBlockedByOverlay()) return;
    if (ctrl && !ev.altKey && (key === "z" || key === "Z") && !ev.shiftKey) {
      ev.preventDefault();
      ev.stopPropagation();
      undoEditorHistory();
      return;
    }
    if (
      (ctrl && !ev.altKey && (key === "y" || key === "Y") && !ev.shiftKey) ||
      (ctrl && ev.shiftKey && (key === "z" || key === "Z"))
    ) {
      ev.preventDefault();
      ev.stopPropagation();
      redoEditorHistory();
      return;
    }
    if (ctrl && !ev.altKey && (key === "c" || key === "C")) {
      if (copySelectedEditorActions()) {
        ev.preventDefault();
        ev.stopPropagation();
      }
      return;
    }
    if (ctrl && !ev.altKey && (key === "x" || key === "X")) {
      if (cutSelectedEditorActions()) {
        ev.preventDefault();
        ev.stopPropagation();
      }
      return;
    }
    if (ctrl && !ev.altKey && (key === "v" || key === "V")) {
      if (pasteEditorClipboardAfterSelection()) {
        ev.preventDefault();
        ev.stopPropagation();
      }
      return;
    }
    if (!ctrl && !ev.altKey && key === "Delete") {
      if (deleteSelectedEditorActions()) {
        ev.preventDefault();
        ev.stopPropagation();
      }
      return;
    }
    if (ctrl && !ev.altKey && (key === "a" || key === "A")) {
      ev.preventDefault();
      ev.stopPropagation();
      selectAllEditorActions();
    }
  }

  function bindEditorActionShortcuts() {
    if (document._qstEditorShortcuts) return;
    document._qstEditorShortcuts = true;
    document.addEventListener("keydown", onEditorActionShortcut, true);
  }

  function focusEditorActionList() {
    try {
      if (isEditableTypingTarget(document.activeElement)) return;
      if (isEditorVisualMode()) {
        const host = $("#visualCanvasHost");
        if (host) host.focus({ preventScroll: true });
        return;
      }
      const list = $("#actionList");
      if (!list) return;
      if (list.tabIndex < 0) list.tabIndex = -1;
      list.focus({ preventScroll: true });
    } catch (_) {}
  }

  function visualOnInsert(pos) {
    const V = window.QstVisualEditor;
    if (V && typeof V.onInsert === "function") V.onInsert(pos);
  }

  function visualOnDelete(index, count) {
    const V = window.QstVisualEditor;
    if (V && typeof V.onDelete === "function") V.onDelete(index, count);
  }

  function visualOnMove(fromIdx, end, insert) {
    const V = window.QstVisualEditor;
    if (V && typeof V.onMove === "function") V.onMove(fromIdx, end, insert);
  }

  function visualOnReplace(start, oldCount, newCount) {
    const V = window.QstVisualEditor;
    if (V && typeof V.onReplace === "function") V.onReplace(start, oldCount, newCount);
  }

  function visualDiscardUndo() {
    discardEditorHistory();
  }

  function visualActionsChanged() {
    const V = window.QstVisualEditor;
    if (V && typeof V.onActionsChanged === "function") V.onActionsChanged();
  }

  function collectVisualLayoutForSave() {
    const V = window.QstVisualEditor;
    if (!V) return undefined;
    if (typeof V.collectLayoutIfAny === "function") return V.collectLayoutIfAny();
    if (typeof V.collectLayout === "function") return V.collectLayout();
    return undefined;
  }

  function editorPrefersVisual() {
    return !!(
      state.settings &&
      state.settings.other &&
      state.settings.other.editorDefaultView === "visual"
    );
  }

  function applyEditorDefaultView() {
    const V = window.QstVisualEditor;
    if (!V || !editorPrefersVisual()) return;
    if (typeof V.applyOpenView === "function") V.applyOpenView(true);
    else if (typeof V.setMode === "function") V.setMode(true);
  }

  function selectEditorAction(i, opts) {
    i = i | 0;
    if (i < 0 || i >= (state.editorActions || []).length) return;
    opts = opts || {};
    if (
      opts.keepDraft &&
      state.actionSel === i &&
      state.editDraft &&
      !state.addPreview
    ) {
      captureParamPanelIntoDraft();
      const V = window.QstVisualEditor;
      if (V && typeof V.syncSelectionUi === "function") V.syncSelectionUi();
      return;
    }
    const prevSel = state.actionSel;
    state.actionSel = i;
    state.addPreview = null;
    loadEditDraftFromSelection();
    {
      const a = state.editDraft;
      if (a && a.type) {
        state.addActionType = a.type;
        const typeCombo = $("#edActionType");
        const cur = ACTION_TYPES.find((x) => x.v === a.type);
        if (typeCombo && cur) typeCombo.textContent = cur.t;
      }
    }
    state._skipReadback = true;
    updateEditorSelectionUi(prevSel, i);
    state._skipReadback = false;
    const row = $("#actionList")?.querySelector('.arow[data-i="' + i + '"]');
    if (row && row.scrollIntoView) row.scrollIntoView({ block: "nearest" });
    focusEditorActionList();
  }

  function insertEditorAction(action, pos, indentOverride) {
    const a = Object.assign({}, action);
    delete a._preview;
    if (typeof indentOverride === "number" && indentOverride >= 0) {
      a.indent = clampIndent(indentOverride);
    } else if (a.indent == null) {
      a.indent = 0;
    } else {
      a.indent = clampIndent(a.indent);
    }
    if (a.type === "defineBlock") {
      if (!validateDefineBlockName(a.blockName, -1)) return false;
      pos = 0;
      a.indent = 0;
    }
    const list = state.editorActions;
    pos = Math.max(0, Math.min(pos | 0, list.length));
    if (a.type === "endLoop" && !hasLoopParentAt(list, pos, a.indent | 0)) {
      toast(END_LOOP_NEEDS_LOOP_MSG);
      return false;
    }
    visualNoteUndo();
    list.splice(pos, 0, a);
    visualOnInsert(pos);
    shiftCollapsedAfterInsert(pos);
    if (isSubtreeContainerType(a.type)) delete state.collapsedContainers[pos];
    return true;
  }

  function deleteSubtreeAt(index, opts) {
    if (!(opts && opts.skipUndo)) visualNoteUndo();
    const end = subtreeEnd(index);
    state.editorActions.splice(index, end - index);
    visualOnDelete(index, end - index);
    remapCollapsedAfterDelete(index, end);
    if (state.actionSel >= index && state.actionSel < end) state.actionSel = -1;
    else if (state.actionSel >= end) state.actionSel -= end - index;
  }

  function batchDeleteSelectedSubtrees(opts) {
    const selected = new Set(selectedBatchIndices());
    if (!selected.size) return 0;
    if (!(opts && opts.skipUndo)) visualNoteUndo();
    let removed = 0;
    for (let i = state.editorActions.length - 1; i >= 0; --i) {
      if (!selected.has(i)) continue;
      const end = subtreeEnd(i);
      state.editorActions.splice(i, end - i);
      visualOnDelete(i, end - i);
      remapCollapsedAfterDelete(i, end);
      removed += 1;
      // 落在已删子树内的勾选索引随之失效
      for (let j = i; j < end; ++j) selected.delete(j);
    }
    state.batchSel = {};
    state.actionSel = -1;
    return removed;
  }

  function validateEditorBeforeSave() {
    for (let i = 0; i < state.editorActions.length; ++i) {
      const a = state.editorActions[i];
      if (!a || a.type !== "defineBlock") continue;
      if (!validateDefineBlockName(a.blockName, i)) return false;
    }
    const endLoopErr = validateEndLoopPlacements(state.editorActions);
    if (endLoopErr) {
      toast(endLoopErr);
      return false;
    }
    const defined = new Set();
    state.editorActions.forEach((a) => {
      if (a && a.type === "defineBlock") {
        const n = String(a.blockName || "").trim();
        if (n) defined.add(n);
      }
    });
    for (let i = 0; i < state.editorActions.length; ++i) {
      const a = state.editorActions[i];
      if (!a || a.type !== "runBlock") continue;
      const n = String(a.blockName || "").trim();
      if (n && !defined.has(n)) {
        toast(`警告：运行块「${n}」未在本脚本中定义（仍可保存）`);
        break;
      }
    }
    for (let i = 0; i < state.editorActions.length; ++i) {
      const a = state.editorActions[i];
      if (!a || (a.type !== "runMacro" && a.type !== "mousePlayback")) continue;
      const mode = a.useMode == null ? 3 : a.useMode | 0;
      if (mode !== 1 && mode !== 2) continue;
      const wm = a.nestedWindowMode || {};
      const method = normalizeWmSelectMethod(wm.selectMethod);
      const hasIdentity = !!(
        (wm.windowClassName && String(wm.windowClassName).trim()) ||
        (wm.windowName && String(wm.windowName).trim()) ||
        (wm.windowTitle && String(wm.windowTitle).trim())
      );
      const hasExe = !!(wm.targetExePath && String(wm.targetExePath).trim());
      if (method === "useEditorWindowClass" && !hasIdentity) {
        toast("嵌套动作请先点击「指定窗口类」配置目标窗口，或改用其他选择窗口方式。");
        return false;
      }
      if (method === "noSelect" && !hasExe) {
        toast("嵌套动作请填写目标程序路径，可使用「浏览」或「准星找程序」。");
        return false;
      }
    }
    return true;
  }

  function showAddTypePreview() {
    const remark = ($("#edRemark")?.textContent || "").trim();
    const preview = defaultAction(state.addActionType, remark);
    // 与 exe 对齐：切类型刷新右栏为该类型表单（未点「添加」前不写回列表）
    preview._preview = true;
    state.addPreview = preview;
    state.editDraft = null;
    const typeCombo = $("#edActionType");
    if (typeCombo) {
      const cur =
        ACTION_TYPES.find((a) => a.v === state.addActionType) || ACTION_TYPES[0];
      typeCombo.textContent = cur.t;
    }
    renderParamPanel(preview);
  }

  function hitInsertTarget(listY, listX, excludeIdx) {
    const actions = state.editorActions;
    const visible = visibleActionIndices();
    const n = visible.length;
    if (!n) return { insertIndex: 0, indent: 0 };
    const list = $("#actionList");
    const rows = list ? $$(".arow", list) : [];
    const rowH = rows[0] ? rows[0].offsetHeight || 38 : 38;
    let slot = Math.floor(listY / rowH);
    const inRow = listY % rowH;
    if (inRow > rowH / 2) slot += 1;
    slot = Math.max(0, Math.min(slot, n));
    let insertIndex = slot >= n ? actions.length : visible[slot];
    let indent = 0;
    let prevIdx = insertIndex - 1;
    if (excludeIdx >= 0) {
      const dragEnd = subtreeEnd(excludeIdx);
      if (prevIdx >= excludeIdx && prevIdx < dragEnd) prevIdx = excludeIdx - 1;
      if (insertIndex > excludeIdx && insertIndex < dragEnd) insertIndex = dragEnd;
    }
    if (prevIdx >= 0 && prevIdx < actions.length) {
      const prev = actions[prevIdx];
      const prevIndent = clampIndent(prev.indent);
      const base = 40;
      const xIndent = clampIndent(Math.floor((listX - base) / editorTreeStepPx().drag));
      if (isSubtreeContainerType(prev.type) && xIndent > prevIndent) {
        indent = prevIndent + 1;
      } else if (prevIndent > 0) {
        indent = Math.min(xIndent, prevIndent);
      } else {
        indent = 0;
      }
    }
    // 防止把同级项插进容器体中间（对齐 HitInsertTarget）
    for (let i = insertIndex - 1; i >= 0; --i) {
      if (!isSubtreeContainerType(actions[i].type)) continue;
      const bodyEnd = subtreeEnd(i);
      const cIndent = clampIndent(actions[i].indent);
      if (insertIndex > i && insertIndex < bodyEnd && indent <= cIndent) {
        insertIndex = bodyEnd;
        indent = cIndent;
      }
      break;
    }
    return { insertIndex, indent: clampIndent(indent) };
  }

  function moveActionSubtree(fromIdx, insertIndex, targetIndent) {
    const actions = state.editorActions;
    if (fromIdx < 0 || fromIdx >= actions.length) return false;
    const end = subtreeEnd(fromIdx);
    const block = actions.slice(fromIdx, end).map((a) => Object.assign({}, a));
    const baseIndent = clampIndent(block[0].indent);
    const delta = clampIndent(targetIndent) - baseIndent;
    block.forEach((a) => {
      a.indent = clampIndent((a.indent | 0) + delta);
    });
    let insert = insertIndex;
    if (insert > fromIdx && insert < end) insert = end;
    if ((insert === fromIdx || insert === end) && clampIndent(targetIndent) === baseIndent) {
      return false;
    }
    const before = actions.slice(0, fromIdx);
    const after = actions.slice(end);
    let combined = before.concat(after);
    if (insert > fromIdx) insert -= end - fromIdx;
    insert = Math.max(0, Math.min(insert, combined.length));
    combined = combined.slice(0, insert).concat(block).concat(combined.slice(insert));
    const endLoopErr = validateEndLoopPlacements(combined);
    if (endLoopErr) {
      toast(END_LOOP_NEEDS_LOOP_MSG);
      return false;
    }
    visualNoteUndo();
    remapCollapsedAfterMove(fromIdx, end, insert, end - fromIdx);
    state.editorActions = combined;
    visualOnMove(fromIdx, end, insert);
    if (state.actionSel >= fromIdx && state.actionSel < end) {
      state.actionSel = insert + (state.actionSel - fromIdx);
    } else if (state.actionSel >= end) {
      state.actionSel -= end - fromIdx;
      if (insert <= state.actionSel) state.actionSel += end - fromIdx;
    } else if (state.actionSel >= insert) {
      state.actionSel += end - fromIdx;
    }
    return true;
  }

  function ensureActionInsertLine(list) {
    let line = list.querySelector("#actionInsertLine");
    if (!line) {
      line = document.createElement("div");
      line.id = "actionInsertLine";
      line.className = "action-insert-line";
      list.appendChild(line);
    }
    return line;
  }

  /** 对齐原生 PaintDragMarker：按可见行槽位定位橙色插入线 */
  function updateActionInsertLine(line, list, hit) {
    if (!line || !list || !hit) return;
    const rows = $$(".arow", list);
    const rowH = rows[0] ? rows[0].offsetHeight || 38 : 38;
    const visible = visibleActionIndices();
    let slot = visible.length;
    for (let i = 0; i < visible.length; i++) {
      if (visible[i] >= hit.insertIndex) {
        slot = i;
        break;
      }
    }
    const indent = clampIndent(hit.indent);
    line.style.display = "block";
    line.style.top = slot * rowH + "px";
    line.style.left = 8 + indent * editorTreeStepPx().drag + "px";
  }

  // 覆盖该行的容器索引（有子节点）：用于 10% 淡色重叠区分从属
  function nestTintContainers(rowIdx) {
    const actions = state.editorActions || [];
    const covering = [];
    for (let i = 0; i <= rowIdx && i < actions.length; i++) {
      if (!isExpandableContainerType(actions[i].type)) continue;
      const end = subtreeEnd(i);
      if (end <= i + 1) continue; // 无子节点不着色
      if (rowIdx >= i && rowIdx < end) covering.push(i);
    }
    return covering;
  }

  const NEST_TINTS = [
    "rgba(26,166,214,0.10)",
    "rgba(45,212,191,0.10)",
    "rgba(99,102,241,0.10)",
    "rgba(245,158,11,0.10)",
    "rgba(251,113,133,0.10)",
  ];

  function applyRowNestTint(row, idx, selected, covering) {
    if (!row) return;
    if (selected) {
      row.classList.remove("nest-tint");
      row.style.backgroundImage = "";
      return;
    }
    const layers = covering || nestTintContainers(idx);
    if (!layers || !layers.length) {
      row.classList.remove("nest-tint");
      row.style.backgroundImage = "";
      return;
    }
    row.classList.add("nest-tint");
    const bg = layers
      .map((ci) => {
        const c = NEST_TINTS[ci % NEST_TINTS.length];
        return `linear-gradient(${c},${c})`;
      })
      .join(",");
    row.style.backgroundImage = bg;
  }

  function editorBatchPickedCount() {
    if (!state.batchSel) return 0;
    let n = 0;
    for (const k in state.batchSel) {
      if (state.batchSel[k]) n += 1;
    }
    return n;
  }

  function syncEditorActionCount() {
    const count = $("#edCount");
    if (!count) return;
    const n = state.editorActions.length;
    count.textContent = state.batchMode
      ? n + " 项 · 已选 " + editorBatchPickedCount()
      : n + " 项";
  }

  function patchBatchRowSelected(row, selected, covering) {
    if (!row) return;
    const i = row.dataset.i | 0;
    row.classList.toggle("sel", !!selected);
    const chk = row.querySelector("[data-act='batch-toggle']");
    if (chk) chk.classList.toggle("on", !!selected);
    applyRowNestTint(row, i, selected, covering);
  }

  function refreshEditorBatchSelectionUi() {
    const list = $("#actionList");
    const rows = list ? list.querySelectorAll(".arow") : [];
    if (state._actionListRendering || (!rows.length && (state.editorActions || []).length)) {
      renderEditorActions(state.editorActions);
      return;
    }
    const layout = precomputeRowLayout();
    for (let r = 0; r < rows.length; r++) {
      const row = rows[r];
      const i = row.dataset.i | 0;
      patchBatchRowSelected(row, !!(state.batchSel && state.batchSel[i]), layout.cover[i]);
    }
    syncEditorActionCount();
    syncMergeModeUi();
  }

  function toggleEditorBatchRow(index) {
    if (!state.batchSel) state.batchSel = {};
    state.batchSel[index] = !state.batchSel[index];
    state._batchAnchor = index;
    const list = $("#actionList");
    const row = list && list.querySelector(`.arow[data-i="${index}"]`);
    if (!row || state._actionListRendering) {
      renderEditorActions(state.editorActions);
      return;
    }
    const layout = precomputeRowLayout();
    patchBatchRowSelected(row, !!state.batchSel[index], layout.cover[index]);
    syncEditorActionCount();
    syncMergeModeUi();
  }

  function applyEditorBatchRowClick(index, ev) {
    if (!state.batchSel) state.batchSel = {};
    const n = (state.editorActions || []).length;
    if (index < 0 || index >= n) return;
    if (ev && ev.shiftKey && state._batchAnchor >= 0 && state._batchAnchor < n) {
      const a = Math.min(state._batchAnchor, index);
      const b = Math.max(state._batchAnchor, index);
      for (let i = a; i <= b; i++) {
        if (isRowVisibleInTree(i)) state.batchSel[i] = true;
      }
      refreshEditorBatchSelectionUi();
      return;
    }
    toggleEditorBatchRow(index);
  }

  function setEditorBatchSelectedAll(on) {
    state.batchSel = {};
    if (on) {
      for (let i = 0; i < state.editorActions.length; i++) state.batchSel[i] = true;
      state._batchAnchor = Math.max(0, state.editorActions.length - 1);
    } else {
      state._batchAnchor = -1;
    }
    refreshEditorBatchSelectionUi();
  }

  function selectAllEditorActions() {
    const n = (state.editorActions || []).length;
    if (!n) return false;
    if (isEditorVisualMode()) {
      const idxs = [];
      for (let i = 0; i < n; i++) idxs.push(i);
      setEditorVisualSelection(idxs);
      const V = window.QstVisualEditor;
      if (V && typeof V.syncSelectionUi === "function") V.syncSelectionUi();
      if (V && typeof V.render === "function") V.render();
      return true;
    }
    if (!state.batchMode) enterEditorBatch(true);
    setEditorBatchSelectedAll(true);
    return true;
  }

  /** 进出批量：CSS 显隐勾选框，只清行选中态，不重绘整表 */
  function patchActionListBatchMode(on) {
    const list = $("#actionList");
    if (!list || !list.querySelector(".arow")) return false;
    if (!list.querySelector("[data-act='batch-toggle']")) return false;
    const layout = precomputeRowLayout();
    $$(".arow", list).forEach((row) => {
      const i = row.dataset.i | 0;
      const chk = row.querySelector("[data-act='batch-toggle']");
      if (chk) chk.classList.remove("on");
      const nowSel = !on && i === state.actionSel;
      row.classList.toggle("sel", nowSel);
      const remark = row.children[2];
      if (remark) remark.classList.toggle("muted", !(nowSel && !on));
      applyRowNestTint(row, i, nowSel, layout.cover[i]);
      if (!on) {
        const opN = row.querySelector(".op-normal");
        if (opN) {
          opN.innerHTML = nowSel
            ? '<span class="op-drag">拖动改变位置</span>'
            : '<span class="op-a" data-act="copy">复制</span><span class="op-a" data-act="del">删除</span>';
        }
      }
    });
    return true;
  }

  function editorTreeStepPx() {
    const raw = getComputedStyle(document.documentElement).getPropertyValue("--qst-u");
    const u = parseFloat(raw) || 1.5;
    return {
      name: Math.round(16 * u), // 整行名称+箭头一起缩进，体现层次
      drag: Math.round(16 * u),
    };
  }

  function renderEditorActions(actions) {
    // 对齐原生：重绘列表时不要把右栏草稿偷偷写回选中动作；只有「修改」/保存才提交
    state.editorActions = Array.isArray(actions) ? actions : state.editorActions;
    state.editorActions.forEach((a) => {
      a.indent = clampIndent(a.indent);
      // 旧 Web 误把 timerRecordTime 变量写到 matchVarName；迁到原生用的 loopVarName
      if (a && a.type === "timerRecordTime") {
        if (!a.loopVarName && a.matchVarName) a.loopVarName = a.matchVarName;
        if (a.loopVarName && a.matchVarName) delete a.matchVarName;
      }
    });
    if (state.actionSel >= state.editorActions.length) state.actionSel = -1;
    if (!state.batchSel) state.batchSel = {};
    const list = $("#actionList");
    syncEditorActionCount();
    if (window.QstVisualEditor && typeof QstVisualEditor.isVisual === "function" && QstVisualEditor.isVisual()) {
      if (state.actionSel >= 0) {
        if (!state.editDraft) loadEditDraftFromSelection();
        if (state.editDraft) renderParamPanel(state.editDraft);
      } else if (state.addPreview) renderParamPanel(state.addPreview);
      else showAddTypePreview();
      syncEdRemarkFromSelection();
      visualActionsChanged();
      return;
    }
    if (!list) return;
    list.classList.toggle("is-batch", !!state.batchMode);
    const keepScroll =
      state._actionListScrollTop != null ? state._actionListScrollTop : list.scrollTop;
    if (!list._qstScrollWired) {
      list._qstScrollWired = true;
      list.addEventListener(
        "scroll",
        () => {
          if (state._actionListRendering) return;
          state._actionListScrollTop = list.scrollTop | 0;
        },
        { passive: true }
      );
    }
    state._actionListRendering = true;
    if (!state.editorActions.length) {
      list.innerHTML = "";
      ensureActionInsertLine(list);
      if (state.batchMode) {
        syncMergeModeUi();
      } else if (state.addPreview) renderParamPanel(state.addPreview);
      else showAddTypePreview();
      syncEdRemarkFromSelection();
      syncEditorBatchChrome();
      wireActionListDrag();
      list.scrollTop = keepScroll;
      state._actionListRendering = false;
      state._actionListScrollTop = list.scrollTop | 0;
      if (document.body.classList.contains("progressive-editor")) endProgressiveLoad();
      visualActionsChanged();
      return;
    }
    // 数据已写入：揭开动作区模糊；首开可跳过错开 reveal，避免「有计数无行」闪帧
    // 箭头紧贴动作名左侧；缩进只作用在名称列，层次一眼可见
    const step = editorTreeStepPx();
    const onProgressive = document.body.classList.contains("progressive-editor");
    const revealLoad = onProgressive && !state._skipOpenReveal;
    const layout = precomputeRowLayout();
    const tintStyle = (rowIdx) => {
      const covering = layout.cover[rowIdx];
      if (!covering || !covering.length) return "";
      const bg = covering
        .map((ci) => {
          const c = NEST_TINTS[ci % NEST_TINTS.length];
          return `linear-gradient(${c},${c})`;
        })
        .join(",");
      return `background-image:${bg};`;
    };
    let revealOrd = 0;
    const rowHtml = [];
    state.editorActions.forEach((a, i) => {
      if (!layout.visible[i]) return;
      const batchOn = !!state.batchSel[i];
      const sel = state.batchMode ? batchOn : i === state.actionSel;
      const num = String(i + 1);
      const ind = clampIndent(a.indent);
      const namePad = `padding-left:${ind * step.name}px`;
      const expandable = isExpandableContainerType(a.type);
      const expanded = !expandable || isContainerExpanded(i);
      const covering = layout.cover[i];
      const tint = sel ? "" : tintStyle(i);
      const nestCls = !sel && covering && covering.length ? " nest-tint" : "";
      const revealCls = revealLoad ? " reveal-in" : "";
      const revealDelay = revealLoad
        ? `animation-delay:${Math.min(revealOrd++, 36) * 14}ms;`
        : "";
      const expHtml = expandable
        ? `<span class="exp-toggle ${expanded ? "expanded" : "collapsed"}" data-act="expand-toggle">${
            expanded ? "▼" : "▶"
          }</span>`
        : `<span class="exp-spacer"></span>`;
      const bpDot = editorBreakpointAt(i)
        ? '<span class="bp-dot" title="断点：执行完该动作后结束调试"></span>'
        : "";
      const anoHtml = `<span class="ano">${bpDot}<span class="anum">${num}</span></span>`;
      const opNormal =
        sel && !state.batchMode
          ? '<span class="op-drag">拖动改变位置</span>'
          : '<span class="op-a" data-act="copy">复制</span><span class="op-a" data-act="del">删除</span>';
      rowHtml.push(`<div class="arow ${sel ? "sel" : ""}${editorBreakpointAt(i) ? " bp" : ""}${nestCls}${revealCls}" data-i="${i}" style="${revealDelay}${sel ? "" : tint}">
          ${anoHtml}
          <span class="aname" style="${namePad}">${expHtml}<span class="chk ${batchOn ? "on" : ""}" data-act="batch-toggle"><i></i></span><span class="aname-text">${esc(displayActionName(a))}</span></span>
          <span class="${sel && !state.batchMode ? "" : "muted"}">${esc(a.remark || "")}</span>
          <span class="op"><span class="op-normal">${opNormal}</span><span class="op-batch">批量</span></span>
        </div>`);
    });

    const finishRender = () => {
      if (onProgressive) endProgressiveLoad();
      ensureActionInsertLine(list);
      if (state.batchMode) {
        syncMergeModeUi();
      } else if (state.actionSel >= 0 && !state.addPreview) {
        if (!state.editDraft) loadEditDraftFromSelection();
        renderParamPanel(state.editDraft);
      } else if (state.addPreview) {
        renderParamPanel(state.addPreview);
      } else {
        showAddTypePreview();
      }
      syncEdRemarkFromSelection();
      syncEditorBatchChrome();
      wireActionListDrag();
      list.scrollTop = keepScroll;
      state._actionListRendering = false;
      state._actionListScrollTop = list.scrollTop | 0;
      visualActionsChanged();
    };

    const CHUNK = 48;
    // 长列表分帧写入，避免一次 innerHTML 卡住点击响应
    if (!revealLoad && rowHtml.length > CHUNK) {
      if (onProgressive) endProgressiveLoad();
      state._progressiveToken = (state._progressiveToken | 0) + 1;
      list.innerHTML = "";
      const chunks = [];
      for (let i = 0; i < rowHtml.length; i += CHUNK) {
        chunks.push(rowHtml.slice(i, i + CHUNK).join(""));
      }
      appendActionRowsChunked(list, chunks, finishRender);
      return;
    }
    list.innerHTML = rowHtml.join("");
    finishRender();
  }

  /** 仅切换选中态：避免整表 innerHTML 造成连点卡顿 */
  function updateEditorSelectionUi(prevSel, nextSel) {
    const list = $("#actionList");
    if (!list) return;
    if (state.batchMode) return;
    syncEditorActionCount();
    const layout = precomputeRowLayout();
    const patchRow = (idx, selected) => {
      if (idx < 0) return;
      const row = list.querySelector(`.arow[data-i="${idx}"]`);
      if (!row) return;
      row.classList.toggle("sel", !!selected);
      const remark = row.children[2];
      if (remark) remark.classList.toggle("muted", !selected);
      applyRowNestTint(row, idx, selected, layout.cover[idx]);
      const op = row.querySelector(".op-normal") || row.querySelector(".op");
      if (op) {
        op.innerHTML = selected
          ? '<span class="op-drag">拖动改变位置</span>'
          : '<span class="op-a" data-act="copy">复制</span><span class="op-a" data-act="del">删除</span>';
      }
    };
    patchRow(prevSel, false);
    patchRow(nextSel, true);
    const V = window.QstVisualEditor;
    if (V && typeof V.syncSelectionUi === "function") V.syncSelectionUi();
    if (nextSel >= 0 && !state.addPreview) {
      if (!state.editDraft) loadEditDraftFromSelection();
      renderParamPanel(state.editDraft);
    } else if (state.addPreview) {
      renderParamPanel(state.addPreview);
    } else {
      showAddTypePreview();
    }
    syncEdRemarkFromSelection();
  }

  function wireActionListDrag() {
    const list = $("#actionList");
    if (!list) return;
    // 每次渲染后重绑：先清掉旧监听标记
    if (list._qstDragHandler) {
      list.removeEventListener("mousedown", list._qstDragHandler);
    }
    const kDragThreshold = 5;
    const onDown = (ev) => {
      if (state.batchMode || state._dragging) return;
      if (ev.button != null && ev.button !== 0) return;
      const row = ev.target.closest(".arow");
      if (!row || !list.contains(row)) return;
      if (ev.target.closest("[data-act='copy'], [data-act='del'], [data-act='expand-toggle'], .op-a, .chk, .exp-toggle")) return;
      const fromIdx = +row.dataset.i;
      if (!(fromIdx >= 0)) return;

      const startX = ev.clientX;
      const startY = ev.clientY;
      let dragMoved = false;
      let hit = null;
      const line = ensureActionInsertLine(list);
      const capturer = list;

      const onMove = (e) => {
        const dx = Math.abs(e.clientX - startX);
        const dy = Math.abs(e.clientY - startY);
        if (!dragMoved && dx < kDragThreshold && dy < kDragThreshold) return;
        if (!dragMoved) {
          dragMoved = true;
          state._dragging = true;
          state._suppressRowClick = true;
          row.classList.add("dragging");
          document.body.style.cursor = "grabbing";
          try {
            capturer.setPointerCapture?.(e.pointerId);
          } catch (_) {}
        }
        e.preventDefault();
        const rect = list.getBoundingClientRect();
        const y = e.clientY - rect.top + list.scrollTop;
        const x = e.clientX - rect.left;
        hit = hitInsertTarget(y, x, fromIdx);
        updateActionInsertLine(line, list, hit);
      };

      const onUp = (e) => {
        document.removeEventListener("mousemove", onMove, true);
        document.removeEventListener("mouseup", onUp, true);
        document.removeEventListener("pointermove", onMove, true);
        document.removeEventListener("pointerup", onUp, true);
        try {
          capturer.releasePointerCapture?.(e.pointerId);
        } catch (_) {}
        row.classList.remove("dragging");
        document.body.style.cursor = "";
        line.style.display = "none";
        state._dragging = false;
        if (!dragMoved) return;
        state._suppressRowClick = true;
        if (hit && moveActionSubtree(fromIdx, hit.insertIndex, hit.indent | 0)) {
          state._skipReadback = true;
          state.addPreview = null;
          renderEditorActions(state.editorActions);
          state._skipReadback = false;
        }
        setTimeout(() => {
          state._suppressRowClick = false;
        }, 0);
      };

      // capture 阶段保证拖出列表仍能收到事件（WebView2）
      document.addEventListener("mousemove", onMove, true);
      document.addEventListener("mouseup", onUp, true);
      document.addEventListener("pointermove", onMove, true);
      document.addEventListener("pointerup", onUp, true);
      ev.preventDefault();
    };
    list._qstDragHandler = onDown;
    list.addEventListener("mousedown", onDown);
  }

  function enterEditor(name, path) {
    beginProgressiveLoad("editor");
    state._modeTransEndOn = "openEditor";
    state._pendingModeReveal = "editor";
    state.editor = true;
    state.actionSel = -1;
    state.batchMode = false;
    state.batchSel = {};
    state.collapsedContainers = {};
    state._actionListScrollTop = 0;
    state.editorPath = path || "";
    state.editorName = name || "";
    state.editorActions = [];
    state.editDraft = null;
    state.addActionType = firstVisibleActionType();
    resetEditorHistory();
    document.body.classList.add("editor-open", "editor-booting");
    const app = $("#app");
    if (app) app.classList.add("editor-mode");
    // 同一 click 里把主页 display:none 后，WebView2 可能把剩余点击打到刚露出来的「取消」
    const ed = $("#editor");
    if (ed) {
      ed.style.pointerEvents = "none";
      requestAnimationFrame(() => {
        requestAnimationFrame(() => {
          if (state.editor && ed) ed.style.pointerEvents = "";
        });
      });
    }
    const edName = $("#edName");
    if (edName) edName.textContent = state.editorName || "…";
    resetMergeUiChrome();
    if (window.QstVisualEditor && typeof QstVisualEditor.reset === "function") {
      QstVisualEditor.reset();
    }
    const typeCombo = $("#edActionType");
    if (typeCombo) {
      syncEditorActionTypeCombo();
    }
    if (!state.addPreview) showAddTypePreview();
    syncEditorBatchChrome();
    applyShellScale();
    syncTitleBar();
    if (window.qst) {
      // 先加载脚本；等列表写完再 setMode 放大，避免主页被拉宽露出右侧空白底
      // 看门狗：后端 15s 内未回 openEditor.result 时强制收尾，避免永久卡在启动态
      if (state._editorOpenWatchdog) clearTimeout(state._editorOpenWatchdog);
      state._editorOpenWatchdog = setTimeout(() => {
        state._editorOpenWatchdog = null;
        if (document.body.classList.contains("editor-booting")) {
          document.body.classList.remove("editor-booting");
          endModeTransition("force");
          toast("编辑器打开超时，请重试");
        }
      }, 15000);
      qst.openEditor(path || "");
    } else {
      renderEditorActions([]);
      showAddTypePreview();
      applyEditorDefaultView();
      document.body.classList.remove("editor-booting");
      endModeTransition("force");
    }
  }

  function captureEditorSnapshot() {
    const scrub = (a) => {
      const c = Object.assign({}, a);
      delete c._vx;
      delete c._vy;
      return c;
    };
    return JSON.stringify({
      name: state.editorName || "",
      mode: state.editorMode | 0,
      breakout: ($("#edBreakout")?.textContent || "").trim(),
      windowMode: state.windowMode || {},
      actions: (state.editorActions || []).map(scrub),
      visualLayout: collectVisualLayoutForSave() || null,
    });
  }

  function isEditorDirty() {
    if (!state.editor) return false;
    if (!state._editorSnapshot) return false;
    return captureEditorSnapshot() !== state._editorSnapshot;
  }

  function exitEditor(force) {
    if (!force && isEditorDirty()) {
      askConfirm("有未保存的改动，确定离开编辑器？", () => exitEditor(true));
      return;
    }
    const ed = $("#editor");
    if (ed) ed.style.pointerEvents = "";
    endProgressiveLoad(true);
    state._modeTransEndOn = "";
    state._pendingModeReveal = "";
    state.editor = false;
    state.actionSel = -1;
    state.editorPath = "";
    state._editorSnapshot = "";
    // 动作详情草稿/预览缓存：退出编辑器后清掉，避免下次进入残留
    state.addPreview = null;
    state.editDraft = null;
    state.batchMode = false;
    state.batchSel = {};
    state.collapsedContainers = {};
    state.addActionType = "moveMouse";
    state.editorActions = [];
    resetEditorHistory();
    resetMergeUiChrome();
    // 调试缓存随编辑器关闭清理干净
    state.editorBreakpoints = {};
    state.debugHotkey = { text: "F9", vk: 0x78, modifiers: 0 };
    state.debugging = false;
    state.debugPaused = false;
    state.debugStepMode = false;
    renderParamPanel(null);
    if (state._editorOpenWatchdog) {
      clearTimeout(state._editorOpenWatchdog);
      state._editorOpenWatchdog = null;
    }
    const typeCombo = $("#edActionType");
    if (typeCombo) {
      const cur = ACTION_TYPES.find((a) => a.v === state.addActionType) || ACTION_TYPES[0];
      typeCombo.textContent = cur.t;
    }
    const remark = $("#edRemark");
    if (remark) remark.textContent = "";
    document.body.classList.remove("editor-open", "editor-booting");
    const app = $("#app");
    if (app) app.classList.remove("editor-mode");
    applyShellScale();
    syncTitleBar();
    if (window.qst) {
      state._pendingUncloak = true;
      qst.setMode("home");
    }
  }

  // ── 编辑器调试（断点/调试热键仅缓存，关闭编辑器即清理）────────
  function editorBreakpointAt(i) {
    return !!state.editorBreakpoints[i];
  }

  function toggleEditorBreakpoint(i) {
    if (i < 0 || i >= state.editorActions.length) return;
    if (state.editorBreakpoints[i]) delete state.editorBreakpoints[i];
    else state.editorBreakpoints[i] = true;
    renderEditorActions(state.editorActions);
  }

  function syncDebugHotkeyUi() {
    const btn = $("#btnDebugHotkey");
    if (!btn) return;
    const d = state.debugHotkey || {};
    btn.textContent = "调试热键: " + (d.text || "F9");
  }

  function openDebugHotkeyCapture() {
    state.pendingPath = "";
    state.pendingKind = "debugHotkey";
    const d = state.debugHotkey || { text: "F9", vk: 0x78, modifiers: 0 };
    state.hotkeyDraft = {
      text: d.text || "F9",
      vk: d.vk | 0 || 0x78,
      modifiers: d.modifiers | 0,
      hold: false,
    };
    const title = $("#hotkeyDlgTitle");
    if (title) title.textContent = "调试热键";
    const clearBtn = $("#btnHotkeyClear");
    const resetBtn = $("#btnHotkeyReset");
    if (clearBtn) clearBtn.style.display = "none";
    if (resetBtn) resetBtn.style.display = "none";
    syncHotkeyValUi();
    openOv("hotkey");
    startWebHotkeyCapture();
  }

  function startDebugFrom(index, stepMode) {
    if (!window.qst) {
      toast("无桥接环境");
      return;
    }
    if (!state.editorActions.length) {
      toast("动作列表为空，无法调试");
      return;
    }
    if (state.debugging || state.macroRunning) {
      toast("已有宏/调试在运行");
      return;
    }
    const go = (idx) => {
      if (idx < 0 || idx >= state.editorActions.length) idx = 0;
      syncFormIntoSelectedBeforeSave();
      const bp = Object.keys(state.editorBreakpoints)
        .map((k) => Number(k))
        .filter(
          (k) => Number.isFinite(k) && k >= 0 && k < state.editorActions.length
        )
        .sort((a, b) => a - b);
      const name =
        ($("#edName")?.textContent || "").trim() || state.editorName || "调试";
      state.debugging = true;
      state.debugPaused = false;
      state.debugStepMode = !!stepMode;
      qst.debugScript({
        path: state.editorPath || "",
        name,
        actions: state.editorActions,
        startIndex: idx | 0,
        mode: stepMode ? "step" : "run",
        editorMode: state.editorMode | 0,
        windowMode: collectWindowModeForSave(),
        breakpoints: bp,
        hotkeyVk: (state.debugHotkey && state.debugHotkey.vk) | 0,
        hotkeyModifiers: (state.debugHotkey && state.debugHotkey.modifiers) | 0,
      });
    };
    const V = window.QstVisualEditor;
    if (V && typeof V.isVisual === "function" && V.isVisual() && typeof V.commitGraphToList === "function") {
      const act =
        index >= 0 && index < state.editorActions.length ? state.editorActions[index] : null;
      V.commitGraphToList(function (ok) {
        if (!ok) return;
        let idx = act ? state.editorActions.indexOf(act) : 0;
        if (idx < 0) idx = 0;
        go(idx);
      }, "与初始节点无关的流程会直接删除，确定调试？");
      return;
    }
    go(index);
  }

  function itemPath(item) {
    return (item && (item.path || item.id)) || "";
  }

  function askConfirm(msg, onOk) {
    const el = $("#promptMsg");
    if (el) el.textContent = msg;
    state._promptOk = onOk;
    openOv("prompt");
  }

  /** 统一文本输入（替代 window.prompt），复用 ov-rename */
  function askInput(title, defaultVal, onOk, opts) {
    const titleEl = $("#ov-rename .dlg-title");
    if (titleEl) {
      titleEl.innerHTML =
        esc(title || "输入") +
        ' <div class="win-btns" style="margin-left:auto"><div class="win-btn close" data-close>×</div></div>';
    }
    const inp = $("#renameInput");
    if (inp) {
      inp.textContent = defaultVal != null ? String(defaultVal) : "";
    }
    // 必须先 openOv（内部 closeAllOv 会清 askInput），再写入 pending 状态
    openOv("rename");
    state._askInputOk = typeof onOk === "function" ? onOk : null;
    state._askInputAllowEmpty = !!(opts && opts.allowEmpty);
    state.pendingKind = "askInput";
    state.pendingPath = "";
    if (inp) {
      requestAnimationFrame(() => {
        try {
          inp.focus();
          const r = document.createRange();
          r.selectNodeContents(inp);
          const s = window.getSelection();
          s.removeAllRanges();
          s.addRange(r);
        } catch (_) {}
      });
    }
  }

  function agentToolLabel(name) {
    const map = {
      readScriptReference: "查阅脚本参考",
      readAgentSkill: "查阅助手Skill",
      listAiModels: "查看已添加AI模型",
      planScriptActions: "规划脚本动作树",
      buildScriptActions: "构建脚本动作",
      createMacroScript: "创建鼠标宏",
      optimizeRecording: "优化键鼠录制",
      deleteScriptFile: "删除脚本或录制",
      listScripts: "浏览脚本与录制列表",
      readScript: "读取脚本内容",
      writeScript: "保存脚本",
      getScriptStats: "分析脚本结构",
      optimizeScript: "优化脚本",
      listScheduledTasks: "查看定时任务",
      createScheduledTask: "创建定时任务",
      updateScheduledTask: "修改定时任务",
      deleteScheduledTask: "删除定时任务",
      listSettings: "查看当前设置",
      updateSettings: "修改设置",
      runAgentCommand: "运行白名单命令",
      listDirectory: "浏览目录",
      readAgentFile: "读取文件",
      searchAgentFiles: "搜索文件",
      writeAgentFile: "写入文件",
      copyAgentTextToClipboard: "复制文本",
      pasteAgentClipboardText: "读取剪贴板",
      listAgentChanges: "查看修改记录",
      revertAgentChange: "恢复修改",
    };
    return map[name] || name || "工具";
  }

  function clearAgentTransientStatus(tab) {
    const target = tab || activeAgentTab();
    if (!target || target.key !== state.agentTabKey) return;
    const log = $("#chatLog");
    if (!log) return;
    log.querySelectorAll(".bubble.meta.live-status").forEach((el) => el.remove());
  }

  /** 聊天区滚动：仅当用户已贴底（或接近底部）才跟随最新内容，否则保持当前视口位置。
   *  流式思考/回复/状态行增长时不会把用户正在看的位置顶走。 */
  function scrollChatSmart(log) {
    if (!log) return;
    const nearBottom =
      log.scrollHeight - log.scrollTop - log.clientHeight <= 28;
    if (nearBottom) log.scrollTop = log.scrollHeight;
  }

  /** 连接/上传等进度：同一行原地更新，避免刷屏 */
  function appendAgentStatusLine(text, tab, opts) {
    if (!text) return;
    const target = tab || activeAgentTab();
    if (!target) return;
    if (target.key !== state.agentTabKey) return;
    const log = $("#chatLog");
    if (!log) return;
    const sticky = !(opts && opts.permanent);
    if (sticky) {
      let div = log.querySelector(".bubble.meta.live-status");
      if (!div) {
        div = document.createElement("div");
        div.className = "bubble meta live-status";
        log.appendChild(div);
      }
      div.textContent = text;
      scrollChatSmart(log);
      return;
    }
    const div = document.createElement("div");
    div.className = "bubble meta";
    div.textContent = text;
    clearAgentTransientStatus(target);
    log.appendChild(div);
    scrollChatSmart(log);
  }

  function appendAgentThinkingDelta(tab, delta) {
    if (!tab || !delta) return;
    if (tab._thinkingFinalized) {
      // 新一轮思考（如工具调用后）：另开一块，不刷旧块
      tab._thinkingFinalized = false;
      tab._thinkingBuf = delta;
    } else {
      tab._thinkingBuf = (tab._thinkingBuf || "") + delta;
    }
    if (tab.key !== state.agentTabKey) return;
    const log = $("#chatLog");
    if (!log) return;
    clearAgentTransientStatus(tab);
    let el = log.querySelector(".bubble.thinking.streaming");
    if (!el) {
      el = document.createElement("details");
      el.className = "bubble thinking streaming";
      el.dataset.round = String((tab._thinkingRound = (tab._thinkingRound | 0) + 1));
      el.open = true;
      el.innerHTML =
        "<summary>" +
        thinkingLabelHtml(true) +
        '</summary><div class="think-body"></div>';
      const streamingAi = log.querySelector(".bubble.ai.streaming");
      if (streamingAi) log.insertBefore(el, streamingAi);
      else log.appendChild(el);
    }
    // 与正文流一致：rAF 合并刷新，避免每个 token 全量写 textContent
    if (!tab._thinkPaintPending) {
      tab._thinkPaintPending = true;
      requestAnimationFrame(() => {
        tab._thinkPaintPending = false;
        const cur = log.querySelector(".bubble.thinking.streaming .think-body");
        if (cur) cur.textContent = cleanThinkingText(tab._thinkingBuf);
        scrollChatSmart(log);
      });
    }
  }

  function finalizeAgentThinking(tab, reasoning) {
    if (!tab) return;
    if (tab._thinkingFinalized) {
      if (reasoning) tab._lastReasoning = reasoning;
      return;
    }
    if (reasoning) tab._thinkingBuf = reasoning;
    const text = tab._thinkingBuf || "";
    if (text) tab._lastReasoning = text;
    tab._thinkingFinalized = true;
    if (tab.key === state.agentTabKey) {
      const log = $("#chatLog");
      const el = log?.querySelector(".bubble.thinking.streaming");
      if (el) {
        el.classList.remove("streaming");
        const sum = el.querySelector("summary");
        if (sum) sum.innerHTML = thinkingLabelHtml(false);
        const body = el.querySelector(".think-body");
        if (body && text) body.innerHTML = renderThinkingHtml(text);
        el.open = false;
      } else if (text) {
        const wrap = document.createElement("div");
        wrap.innerHTML = thinkingBlockHtml(text, { open: false });
        const node = wrap.firstElementChild;
        if (node && log) {
          const streamingAi = log.querySelector(".bubble.ai.streaming");
          if (streamingAi) log.insertBefore(node, streamingAi);
          else log.appendChild(node);
        }
      }
    }
  }

  function openRename(item, kind) {
    state.pendingPath = itemPath(item);
    state.pendingKind = kind;
    state._askInputOk = null;
    const titleEl = $("#ov-rename .dlg-title");
    if (titleEl) {
      titleEl.innerHTML =
        '重命名 <div class="win-btns" style="margin-left:auto"><div class="win-btn close" data-close>×</div></div>';
    }
    const inp = $("#renameInput");
    if (inp) inp.textContent = item.name || "";
    openOv("rename");
  }

  function openGlobalHotkeyCapture() {
    state.pendingPath = "";
    state.pendingKind = "globalHotkey";
    state.hotkeyDraft = {
      text: String(state.hotkey || "")
        .replace(/（长按）$/, "")
        .trim(),
      vk: state.hotkeyVk | 0,
      modifiers: state.hotkeyModifiers | 0,
      hold: !!state.hotkeyHold,
    };
    const title = $("#hotkeyDlgTitle");
    if (title) title.textContent = "启停热键";
    const clearBtn = $("#btnHotkeyClear");
    const resetBtn = $("#btnHotkeyReset");
    if (clearBtn) clearBtn.style.display = "";
    if (resetBtn) resetBtn.style.display = "";
    syncHotkeyValUi();
    openOv("hotkey");
    startWebHotkeyCapture();
  }

  /** Web 找图模板裁切（B3）：选区后经 findImageCrop + cropX/Y/W/H 原生写盘 */
  let _cropDrag = null;
  let _cropImgNatural = { w: 0, h: 0 };
  let _cropPending = null;

  function stopWebCropDrag() {
    if (_cropDrag && _cropDrag.onMove) {
      window.removeEventListener("pointermove", _cropDrag.onMove);
      window.removeEventListener("pointerup", _cropDrag.onUp);
    }
    _cropDrag = null;
  }

  function cropImageContentRect(stageEl, imgEl) {
    const sw = stageEl.clientWidth;
    const sh = stageEl.clientHeight;
    const nw = _cropImgNatural.w || imgEl.naturalWidth || 1;
    const nh = _cropImgNatural.h || imgEl.naturalHeight || 1;
    const scale = Math.min(sw / nw, sh / nh);
    const dw = nw * scale;
    const dh = nh * scale;
    return {
      left: (sw - dw) / 2,
      top: (sh - dh) / 2,
      width: dw,
      height: dh,
      scale,
      nw,
      nh,
    };
  }

  function setCropSelCss(sel, left, top, w, h) {
    sel.style.left = left + "px";
    sel.style.top = top + "px";
    sel.style.width = w + "px";
    sel.style.height = h + "px";
  }

  function resetCropSelFull() {
    const stage = $("#cropStage");
    const img = $("#cropBgImg");
    const sel = $("#cropSel");
    if (!stage || !img || !sel) return;
    const r = cropImageContentRect(stage, img);
    setCropSelCss(sel, r.left, r.top, r.width, r.height);
  }

  function openWebFindImageCrop(a) {
    if (!a || !a.imagePath) {
      toast("请先截图或选择图片");
      return;
    }
    _cropPending = {
      imagePath: a.imagePath,
      offsetX: a.offsetX | 0,
      offsetY: a.offsetY | 0,
      followUpSaveVar: (a.findImageFollowUp | 0) === 2 ? 1 : 0,
    };
    const img = $("#cropBgImg");
    if (img) {
      img.removeAttribute("src");
      img.style.opacity = "0";
    }
    openOv("crop");
    if (!window.qst) {
      toast("无桥接");
      return;
    }
    const reqId = "crop_" + Date.now();
    state._pendingCropReqId = reqId;
    qst.readImageDataUrl(a.imagePath, reqId);
  }

  function confirmWebFindImageCrop() {
    const stage = $("#cropStage");
    const img = $("#cropBgImg");
    const sel = $("#cropSel");
    const p = _cropPending;
    if (!stage || !img || !sel || !p || !window.qst) return;
    const r = cropImageContentRect(stage, img);
    if (r.nw < 8 || r.nh < 8) {
      toast("图片过小，无法裁切");
      return;
    }
    const sl = parseFloat(sel.style.left) || 0;
    const st = parseFloat(sel.style.top) || 0;
    const sw = parseFloat(sel.style.width) || 0;
    const sh = parseFloat(sel.style.height) || 0;
    const ix0 = Math.round((sl - r.left) / r.scale);
    const iy0 = Math.round((st - r.top) / r.scale);
    const ix1 = Math.round((sl + sw - r.left) / r.scale);
    const iy1 = Math.round((st + sh - r.top) / r.scale);
    const cropX = Math.max(0, Math.min(ix0, ix1));
    const cropY = Math.max(0, Math.min(iy0, iy1));
    const cropW = Math.max(0, Math.abs(ix1 - ix0));
    const cropH = Math.max(0, Math.abs(iy1 - iy0));
    if (cropW < 8 || cropH < 8) {
      toast("裁切区域过小");
      return;
    }
    qst.findImageCrop({
      imagePath: p.imagePath,
      offsetX: p.offsetX,
      offsetY: p.offsetY,
      followUpSaveVar: p.followUpSaveVar,
      cropX,
      cropY,
      cropW,
      cropH,
    });
  }

  function bindCropStageInteractions() {
    const stage = $("#cropStage");
    const sel = $("#cropSel");
    if (!stage || !sel || stage.dataset.cropBound) return;
    stage.dataset.cropBound = "1";
    stage.addEventListener("pointerdown", (e) => {
      if (e.button !== 0) return;
      if (e.target.closest(".hud")) return;
      const img = $("#cropBgImg");
      if (!img || !img.naturalWidth) return;
      e.preventDefault();
      const rect = stage.getBoundingClientRect();
      const r = cropImageContentRect(stage, img);
      let x0 = e.clientX - rect.left;
      let y0 = e.clientY - rect.top;
      x0 = Math.max(r.left, Math.min(r.left + r.width, x0));
      y0 = Math.max(r.top, Math.min(r.top + r.height, y0));
      stopWebCropDrag();
      _cropDrag = {
        x0,
        y0,
        onMove: (ev) => {
          let x1 = ev.clientX - rect.left;
          let y1 = ev.clientY - rect.top;
          x1 = Math.max(r.left, Math.min(r.left + r.width, x1));
          y1 = Math.max(r.top, Math.min(r.top + r.height, y1));
          const L = Math.min(_cropDrag.x0, x1);
          const T = Math.min(_cropDrag.y0, y1);
          const W = Math.abs(x1 - _cropDrag.x0);
          const H = Math.abs(y1 - _cropDrag.y0);
          setCropSelCss(sel, L, T, Math.max(2, W), Math.max(2, H));
        },
        onUp: () => stopWebCropDrag(),
      };
      setCropSelCss(sel, x0, y0, 2, 2);
      window.addEventListener("pointermove", _cropDrag.onMove);
      window.addEventListener("pointerup", _cropDrag.onUp);
    });
  }

  function openHotkeyFor(item) {
    state.pendingPath = itemPath(item);
    const isRec = state.recordings.some((r) => itemPath(r) === state.pendingPath);
    state.pendingKind = isRec ? "recordingHotkey" : "scriptHotkey";
    const vk = item.hotkeyVk | 0;
    state.hotkeyDraft = {
      text: vk
        ? (item.hotkey || "").replace(/（长按）$/, "") || ""
        : "",
      vk,
      modifiers: item.hotkeyModifiers | 0,
      hold: vk ? !!(item.hotkeyHold || /长按/.test(item.hotkey || "")) : false,
    };
    const title = $("#hotkeyDlgTitle");
    if (title) title.textContent = isRec ? "录制热键" : "脚本热键";
    const clearBtn = $("#btnHotkeyClear");
    const resetBtn = $("#btnHotkeyReset");
    if (clearBtn) clearBtn.style.display = "";
    if (resetBtn) resetBtn.style.display = "";
    syncHotkeyValUi();
    openOv("hotkey");
    startWebHotkeyCapture();
  }

  let _hotkeyHoldMs = 200;
  let _hotkeyKeyHandler = null;
  let _hotkeyKeyUpHandler = null;
  let _hotkeyLastLlVk = 0;
  let _hotkeyLastLlTick = 0;
  let _hotkeyCaptureDownAt = 0;
  let _hotkeyHoldTimer = null;

  function clearHotkeyHoldTimer() {
    if (_hotkeyHoldTimer) {
      clearTimeout(_hotkeyHoldTimer);
      _hotkeyHoldTimer = null;
    }
  }

  function armHotkeyHoldTimer(vk, mods) {
    clearHotkeyHoldTimer();
    _hotkeyCaptureDownAt = Date.now();
    const ms = Math.max(50, _hotkeyHoldMs | 0);
    _hotkeyHoldTimer = setTimeout(() => {
      _hotkeyHoldTimer = null;
      if (!$("#ov-hotkey")?.classList.contains("show")) return;
      if (state.hotkeyDraft && (state.hotkeyDraft.vk | 0) === (vk | 0)) {
        applyHotkeyCaptureDraft(vk, mods, true);
      }
    }, ms);
  }

  function finalizeHotkeyHoldIfStillDown() {
    if (!_hotkeyCaptureDownAt) return;
    const d = state.hotkeyDraft;
    if (!d || !(d.vk > 0)) return;
    if (Date.now() - _hotkeyCaptureDownAt >= Math.max(50, _hotkeyHoldMs | 0)) {
      d.hold = true;
      syncHotkeyValUi();
    }
  }

  function startWebHotkeyCapture() {
    if (!window.qst) {
      toast("无桥接");
      return;
    }
    const d = state.hotkeyDraft || {};
    qst.beginHotkeyCapture({
      hotkeyText: d.text || "",
      hotkeyVk: d.vk || 0,
      hotkeyModifiers: d.modifiers || 0,
      hotkeyHold: d.hold ? 1 : 0,
      hotkeyUi: 1,
      passAll: 0,
    });
    clearHotkeyHoldTimer();
    _hotkeyCaptureDownAt = 0;
    // DOM 兜底：LL 丢包时仍能改热键。连发不得把「长按」冲成单击。
    if (_hotkeyKeyHandler) {
      window.removeEventListener("keydown", _hotkeyKeyHandler, true);
      _hotkeyKeyHandler = null;
    }
    if (_hotkeyKeyUpHandler) {
      window.removeEventListener("keyup", _hotkeyKeyUpHandler, true);
      _hotkeyKeyUpHandler = null;
    }
    _hotkeyKeyHandler = (e) => {
      if (!$("#ov-hotkey")?.classList.contains("show")) return;
      const vk = e.keyCode || e.which || 0;
      if (!vk || isModifierVk(vk) || vk === 0x2c) return;
      if (e.repeat) return;
      if (
        window.qst &&
        _hotkeyLastLlVk === vk &&
        Date.now() - _hotkeyLastLlTick < 150
      ) {
        return;
      }
      e.preventDefault();
      e.stopPropagation();
      let modifiers = 0;
      if (e.ctrlKey) modifiers |= 0x0002;
      if (e.altKey) modifiers |= 0x0001;
      if (e.shiftKey) modifiers |= 0x0004;
      if (e.metaKey) modifiers |= 0x0008;
      applyHotkeyCaptureDraft(vk, modifiers, false);
      armHotkeyHoldTimer(vk, modifiers);
    };
    _hotkeyKeyUpHandler = (e) => {
      if (!$("#ov-hotkey")?.classList.contains("show")) return;
      const vk = e.keyCode || e.which || 0;
      if (!vk || !state.hotkeyDraft || (state.hotkeyDraft.vk | 0) !== vk) return;
      if (e.repeat) return;
      // LL 抬起已经把 downAt 清零并写好 hold；这里只处理纯 DOM 兜底
      if (!_hotkeyCaptureDownAt) return;
      clearHotkeyHoldTimer();
      const held =
        Date.now() - _hotkeyCaptureDownAt >= Math.max(50, _hotkeyHoldMs | 0);
      _hotkeyCaptureDownAt = 0;
      applyHotkeyCaptureDraft(vk, state.hotkeyDraft.modifiers | 0, held);
    };
    window.addEventListener("keydown", _hotkeyKeyHandler, true);
    window.addEventListener("keyup", _hotkeyKeyUpHandler, true);
  }

  function syncHotkeyValUi() {
    const d = state.hotkeyDraft || {};
    const el = $("#hotkeyVal");
    if (el) {
      el.textContent = d.vk || d.text
        ? formatHotkeyDisplay(d.text || "无", !!d.hold)
        : "（捕获中…）";
    }
    const tip = $("#hotkeyHoldTip");
    if (tip) {
      const sec = _hotkeyHoldMs / 1000;
      tip.textContent =
        "（按住设为启停，按住 " +
        (Number.isInteger(sec) ? String(sec) : String(sec)) +
        " 秒算按住）";
    }
  }

  function stopWebHotkeyCapture() {
    // 关闭热键弹窗时清掉未消费的格式化标记：否则迟到/丢失的 formatHotkey.result
    // 会吃掉后续动作按键捕获（#ov-action-key）的显示更新，表现为前几次按键无反应
    state._pendingHotkeyDisplay = false;
    clearHotkeyHoldTimer();
    _hotkeyCaptureDownAt = 0;
    if (_hotkeyKeyHandler) {
      window.removeEventListener("keydown", _hotkeyKeyHandler, true);
      _hotkeyKeyHandler = null;
    }
    if (_hotkeyKeyUpHandler) {
      window.removeEventListener("keyup", _hotkeyKeyUpHandler, true);
      _hotkeyKeyUpHandler = null;
    }
    _hotkeyLastLlVk = 0;
    _hotkeyLastLlTick = 0;
    if (window.qst) qst.endHotkeyCapture();
  }

  function applyHotkeyCaptureDraft(vk, mods, hold) {
    if (!$("#ov-hotkey")?.classList.contains("show")) return;
    vk = vk | 0;
    if (!vk) return;
    mods = mods | 0;
    hold = !!hold;
    state.hotkeyDraft = state.hotkeyDraft || {};
    state.hotkeyDraft.vk = vk;
    state.hotkeyDraft.modifiers = mods;
    state.hotkeyDraft.hold = hold;
    // 立刻显示（不等 formatHotkey 往返），避免前几次按下像没反应
    state.hotkeyDraft.text = actionKeyTextFromVk(vk, mods);
    syncHotkeyValUi();
    state._pendingHotkeyDisplay = true;
    if (window.qst) {
      qst.formatHotkey({
        hotkeyVk: vk,
        hotkeyModifiers: mods,
        hotkeyHold: hold ? 1 : 0,
      });
    }
  }

  function applyHotkeyLlUpdate(msg) {
    const vk = msg.keyVk | 0;
    if (!vk) return;
    _hotkeyLastLlVk = vk;
    _hotkeyLastLlTick = Date.now();
    const hold = !!msg.hotkeyHold;
    const edge = String(msg.hotkeyEdge || "");
    // 缺 edge 时不得把 hold=false 当成 down：旧壳的 keyup 会再次武装，约 200ms 后误标长按。
    if (edge === "down") {
      armHotkeyHoldTimer(vk, msg.hotkeyModifiers | 0);
    } else if (edge === "up") {
      clearHotkeyHoldTimer();
      _hotkeyCaptureDownAt = 0;
    } else if (edge === "hold" || hold) {
      clearHotkeyHoldTimer();
    }
    applyHotkeyCaptureDraft(vk, msg.hotkeyModifiers | 0, hold);
  }

  let _actionKeyCapturing = false;
  let _actionKeyKeyHandler = null;
  /** @type {{ text: string, vk: number, modifiers: number }} */
  let _actionKeyDraft = { text: "", vk: 0, modifiers: 0 };
  let _actionKeyLastLlVk = 0;
  let _actionKeyLastLlTick = 0;

  function isModifierVk(vk) {
    return (
      vk === 0x10 ||
      vk === 0xa0 ||
      vk === 0xa1 || // Shift
      vk === 0x11 ||
      vk === 0xa2 ||
      vk === 0xa3 || // Ctrl
      vk === 0x12 ||
      vk === 0xa4 ||
      vk === 0xa5 // Alt
      // Win(0x5b/0x5c) 允许作为主键录入，不在此拦截
    );
  }

  function isWinVk(vk) {
    return vk === 0x5b || vk === 0x5c;
  }

  /** 对齐原生 VkName：动作按键弹层不依赖 formatHotkey 往返，按键立即出名称 */
  function vkName(vk) {
    vk = vk | 0;
    if (vk >= 0x41 && vk <= 0x5a) return String.fromCharCode(vk);
    if (vk >= 0x30 && vk <= 0x39) return String.fromCharCode(vk);
    if (vk >= 0x70 && vk <= 0x87) return "F" + (vk - 0x70 + 1);
    switch (vk) {
      case 0x20: return "空格键";
      case 0x0d: return "Enter";
      case 0x1b: return "Esc";
      case 0x09: return "Tab";
      case 0x08: return "Backspace";
      case 0x14: return "CapsLock";
      case 0x90: return "NumLock";
      case 0x91: return "ScrollLock";
      case 0x13: return "Pause";
      case 0x2c: return "截屏键";
      case 0x2d: return "Ins";
      case 0x2e: return "Del";
      case 0x24: return "Home";
      case 0x23: return "End";
      case 0x21: return "PgUp";
      case 0x22: return "PgDn";
      case 0x25: return "←";
      case 0x26: return "↑";
      case 0x27: return "→";
      case 0x28: return "↓";
      case 0x5b: return "LWin";
      case 0x5c: return "RWin";
      case 0xa2: return "LCtrl";
      case 0xa3: return "RCtrl";
      case 0xa4: return "LAlt";
      case 0xa5: return "RAlt";
      case 0xa0: return "LShift";
      case 0xa1: return "RShift";
      case 0x5d: return "Apps";
      case 0x6a: return "Num*";
      case 0x6b: return "Num+";
      case 0x6d: return "Num-";
      case 0x6e: return "Num.";
      case 0x6f: return "Num/";
      case 0x60: return "Num0";
      case 0x61: return "Num1";
      case 0x62: return "Num2";
      case 0x63: return "Num3";
      case 0x64: return "Num4";
      case 0x65: return "Num5";
      case 0x66: return "Num6";
      case 0x67: return "Num7";
      case 0x68: return "Num8";
      case 0x69: return "Num9";
      case 0xbc: return ",";
      case 0xbe: return ".";
      case 0xbd: return "-";
      case 0xbb: return "=";
      case 0xba: return ";";
      case 0xbf: return "/";
      case 0xc0: return "`";
      case 0xdb: return "[";
      case 0xdc: return "\\";
      case 0xdd: return "]";
      case 0xde: return "'";
      case 0x0c: return "Clear";
    }
    return "按键" + vk;
  }

  function actionKeyTextFromVk(vk, modifiers) {
    const parts = [];
    if (modifiers & 0x0002) parts.push("Ctrl");
    if (modifiers & 0x0001) parts.push("Alt");
    if (modifiers & 0x0004) parts.push("Shift");
    if (modifiers & 0x0008) parts.push("Win");
    parts.push(vkName(vk));
    return parts.join(" + ");
  }

  function syncActionKeyValUi() {
    const el = $("#actionKeyVal");
    if (!el) return;
    const d = _actionKeyDraft || {};
    el.textContent = d.vk || (d.text || "").trim() ? d.text || "无" : "无";
  }

  function stopActionKeyCapture() {
    if (_actionKeyKeyHandler) {
      window.removeEventListener("keydown", _actionKeyKeyHandler, true);
      _actionKeyKeyHandler = null;
    }
    _actionKeyCapturing = false;
    state._pendingActionKeyApply = false;
    state._pendingActionKeyDisplay = false;
    _actionKeyLastLlVk = 0;
    _actionKeyLastLlTick = 0;
    if (window.qst) qst.endHotkeyCapture();
  }

  /** 动作「选择键盘按键」：始终用 Web 弹层；LWin 等由原生 LL 钩子回传 actionKey.llKey */
  function openActionKeyCapture(a) {
    a = a || editorParamAction();
    if (!a) {
      toast("无动作");
      return;
    }
    stopActionKeyCapture();
    _actionKeyDraft = {
      text: a.keyText || "",
      vk: a.keyVk | 0,
      modifiers: 0,
    };
    syncActionKeyValUi();
    openOv("action-key");
    _actionKeyCapturing = true;
    if (window.qst) {
      qst.beginHotkeyCapture({
        hotkeyText: a.keyText || "",
        hotkeyVk: a.keyVk | 0,
        hotkeyModifiers: 0,
        hotkeyHold: 0,
        passAll: 1,
      });
    }
    // DOM keydown 兜底：有壳时主键走 LL（含 LWin），但若 LL 消息被拖慢/丢失，
    // 弹层里的真实按键仍能即时响应（同一按键 150ms 内去重，避免双写）
    _actionKeyKeyHandler = (e) => {
      if (!_actionKeyCapturing) return;
      const vk = e.keyCode || e.which || 0;
      if (!vk) return;
      if (isModifierVk(vk) && !isWinVk(vk)) return;
      if (vk === 0x2c) return;
      if (
        window.qst &&
        _actionKeyLastLlVk === vk &&
        Date.now() - _actionKeyLastLlTick < 150
      ) {
        return;
      }
      e.preventDefault();
      e.stopPropagation();
      let modifiers = 0;
      if (!isWinVk(vk)) {
        if (e.ctrlKey) modifiers |= 0x0002;
        if (e.altKey) modifiers |= 0x0001;
        if (e.shiftKey) modifiers |= 0x0004;
        if (e.metaKey) modifiers |= 0x0008;
      }
      _actionKeyDraft.vk = vk;
      _actionKeyDraft.modifiers = modifiers;
      _actionKeyDraft.text = actionKeyTextFromVk(vk, modifiers);
      syncActionKeyValUi();
    };
    window.addEventListener("keydown", _actionKeyKeyHandler, true);
  }

  function applyLlActionKey(msg) {
    if (!_actionKeyCapturing) return;
    const vk = msg.keyVk | 0;
    if (!vk || vk === 0x2c) return;
    const modifiers = msg.hotkeyModifiers | 0;
    _actionKeyDraft.vk = vk;
    _actionKeyDraft.modifiers = modifiers;
    _actionKeyLastLlVk = vk;
    _actionKeyLastLlTick = Date.now();
    // 立即显示键名（不等 formatHotkey 往返，避免前几次按下看起来无反应）
    _actionKeyDraft.text = actionKeyTextFromVk(vk, modifiers);
    syncActionKeyValUi();
    if (window.qst) {
      state._pendingActionKeyDisplay = true;
      qst.formatHotkey({
        hotkeyVk: vk,
        hotkeyModifiers: modifiers,
        hotkeyHold: 0,
      });
    } else {
      syncActionKeyValUi();
    }
  }

  function applyActionKeyToParam(keyText, keyVk, modifiers) {
    const a = editorParamAction();
    if (!a) return;
    a.keyText = keyText || "";
    a.keyVk = keyVk | 0;
    a.holdLeftCtrl = modifiers & 0x0002 ? 1 : 0;
    a.holdLeftAlt = modifiers & 0x0001 ? 1 : 0;
    a.holdLeftShift = modifiers & 0x0004 ? 1 : 0;
    a.holdLeftWin = modifiers & 0x0008 ? 1 : 0;
    a.holdRightCtrl = 0;
    a.holdRightAlt = 0;
    a.holdRightShift = 0;
    a.holdRightWin = 0;
    renderParamPanel(a);
  }

  function normalizeCrosshairPick(msg) {
    msg = msg || {};
    let pick = msg.pick;
    if (typeof pick === "string") {
      try {
        pick = JSON.parse(pick);
      } catch (_) {
        pick = {};
      }
    }
    pick = pick && typeof pick === "object" ? pick : {};
    if (pick.x == null && msg.x != null) pick.x = msg.x;
    if (pick.y == null && msg.y != null) pick.y = msg.y;
    if (!pick.processPath && msg.processPath) pick.processPath = msg.processPath;
    if (!pick.windowTitle && msg.windowTitle) pick.windowTitle = msg.windowTitle;
    if (!pick.windowClassName && msg.windowClassName) pick.windowClassName = msg.windowClassName;
    if (!pick.childWindowClassName && msg.childWindowClassName) {
      pick.childWindowClassName = msg.childWindowClassName;
    }
    if (!pick.documentPath && msg.documentPath) pick.documentPath = msg.documentPath;
    return pick;
  }

  /** 对齐原生：pointerdown 按住即开始准星拖拽（同步 HostObject，避免松手后再拾取） */
  function startNativeCrosshair(mode, pending) {
    state._pendingCrosshair = pending || mode;
    state._crosshairMode = mode;
    if (!window.qst) {
      toast("无桥接");
      return;
    }
    const syncHost =
      window.chrome &&
      chrome.webview &&
      chrome.webview.hostObjects &&
      chrome.webview.hostObjects.sync &&
      chrome.webview.hostObjects.sync.qst;
    if (syncHost && typeof syncHost.crosshairPick === "function") {
      try {
        const raw = syncHost.crosshairPick(String(mode || "coordinates"));
        let msg = raw;
        if (typeof raw === "string") {
          try {
            msg = JSON.parse(raw);
          } catch (_) {
            msg = { ok: false, detail: raw };
          }
        }
        window.dispatchEvent(
          new CustomEvent("qst:bridge", {
            detail: Object.assign({ type: "crosshairPick.result" }, msg || {}),
          })
        );
        return;
      } catch (err) {
        console.warn("sync crosshairPick failed, fallback async", err);
      }
    }
    qst.crosshairPick(mode);
  }

  function openCrosshairWeb(mode, pending) {
    startNativeCrosshair(mode, pending);
  }

  function startPendingCrosshairPick() {
    const mode = state._crosshairMode || "coordinates";
    startNativeCrosshair(mode, state._pendingCrosshair || mode);
  }

  /** 准星按钮：必须 pointerdown（按住拖），不能等 click（已松手） */
  function wireCrosshairPointerDown(el, mode, pending, beforeFn) {
    if (!el || el._qstCrosshairWired) return;
    el._qstCrosshairWired = true;
    el.addEventListener(
      "pointerdown",
      (e) => {
        if (e.button != null && e.button !== 0) return;
        e.preventDefault();
        e.stopPropagation();
        if (typeof beforeFn === "function") beforeFn();
        startNativeCrosshair(mode, pending);
      },
      true
    );
    el.addEventListener(
      "click",
      (e) => {
        e.preventDefault();
        e.stopPropagation();
      },
      true
    );
  }

  function currentShellZoom() {
    return 1;
  }

  /* popup */
  let popupEl = null;
  let popupAnchor = null;
  let popupUnbindScroll = null;
  let popupIgnoreCloseUntil = 0;
  function hidePopup() {
    if (popupAnchor && popupAnchor.classList) popupAnchor.classList.remove("open");
    popupAnchor = null;
    if (popupUnbindScroll) {
      popupUnbindScroll();
      popupUnbindScroll = null;
    }
    if (popupEl) {
      popupEl.remove();
      popupEl = null;
    }
  }
  /** 弹出菜单至少容纳 n 行（按 --qst-u 估算 .mi 行高） */
  function popupHeightForRows(n) {
    const u =
      parseFloat(
        getComputedStyle(document.documentElement).getPropertyValue("--qst-u")
      ) || 1.5;
    const row = Math.ceil((9 * 2 + 14) * u);
    const pad = Math.ceil(14 * u);
    return Math.max(120, row * Math.max(1, n | 0) + pad);
  }

  /** @param opts {{preferUp?:boolean, selectedIndex?:number, selected?:number, maxHeight?:number, minWidth?:number, minRows?:number}} */
  function showPopup(anchor, items, onPick, opts) {
    opts = opts || {};
    if (popupEl && popupAnchor === anchor) {
      hidePopup();
      return;
    }
    hidePopup();
    if (!anchor || !items || !items.length) return;
    popupAnchor = anchor;
    popupIgnoreCloseUntil = performance.now() + 400;
    const isPoint =
      typeof anchor.clientX === "number" && typeof anchor.clientY === "number"
        && typeof anchor.getBoundingClientRect !== "function";
    let rect;
    if (isPoint) {
      const x = anchor.clientX | 0;
      const y = anchor.clientY | 0;
      rect = { left: x, right: x, top: y, bottom: y, width: 0, height: 0 };
    } else if (typeof anchor.getBoundingClientRect === "function") {
      anchor.classList.add("open");
      rect = anchor.getBoundingClientRect();
    } else {
      return;
    }
    popupEl = document.createElement("div");
    popupEl.className = "popup-menu show" + (opts.preferUp ? " popup-up" : "");
    popupEl.setAttribute("role", "listbox");
    const vw = window.innerWidth || document.documentElement.clientWidth;
    const vh = window.innerHeight || document.documentElement.clientHeight;
    const spaceBelow = Math.max(0, vh - rect.bottom - 8);
    const spaceAbove = Math.max(0, rect.top - 8);
    // 底栏附近强制向上；其余空间不够也向上
    const preferUp =
      !!opts.preferUp || spaceBelow < 140 || (spaceBelow < spaceAbove && spaceBelow < 200);
    const gap = isPoint ? 2 : 4;
    const selectedIdx =
      opts.selectedIndex != null
        ? opts.selectedIndex | 0
        : opts.selected != null
          ? opts.selected | 0
          : -1;
    // 长列表至少露出 minRows（默认 10）；opts.maxHeight 可再收紧
    const minRows = opts.minRows > 0 ? opts.minRows | 0 : items.length > 8 ? 10 : 0;
    let optMaxH = opts.maxHeight > 0 ? opts.maxHeight | 0 : 0;
    if (!optMaxH && minRows > 0) optMaxH = popupHeightForRows(minRows);
    const spaceCap = Math.max(
      minRows > 0 ? popupHeightForRows(minRows) : 120,
      preferUp ? spaceAbove : spaceBelow || vh * 0.45
    );
    const initialMaxH = optMaxH > 0 ? Math.min(spaceCap, Math.max(optMaxH, popupHeightForRows(minRows || 1))) : spaceCap;
    popupEl.innerHTML = items
      .map((it, idx) => {
        const on = idx === selectedIdx ? " on" : "";
        if (it.d) {
          return `<div class="mi wide${on}" data-i="${idx}" role="option"><span>${esc(it.t || it.v || "")}</span><span class="d">${esc(
            it.d
          )}</span></div>`;
        }
        return `<div class="mi${on}" data-i="${idx}" role="option">${esc(it.t || it.v || "")}</div>`;
      })
      .join("");
    // 挂到当前 overlay 内，避免 body 级菜单在 WebView2 里被全屏遮罩抢走点击
    const host =
      (!isPoint && anchor && typeof anchor.closest === "function" && anchor.closest(".overlay.show")) ||
      document.querySelector(".overlay.show") ||
      document.body;
    popupEl.style.cssText =
      "position:fixed;left:0;top:0;visibility:hidden;min-width:" +
      Math.max(rect.width, opts.minWidth | 0, 168) +
      "px;max-height:" +
      initialMaxH +
      "px;overflow-y:auto;z-index:2147483000;pointer-events:auto;";
    host.appendChild(popupEl);
    void popupEl.offsetWidth;
    const pr = popupEl.getBoundingClientRect();
    let left = rect.left;
    if (left + pr.width > vw - 8) left = Math.max(8, vw - pr.width - 8);
    if (left < 8) left = 8;

    const applyMaxH = (avail) => {
      let h = Math.max(96, avail);
      if (initialMaxH > 0) h = Math.min(h, initialMaxH);
      if (optMaxH > 0) h = Math.min(h, optMaxH);
      if (minRows > 0) h = Math.min(Math.max(h, Math.min(popupHeightForRows(minRows), avail)), avail);
      return Math.max(96, h);
    };

    if (preferUp) {
      // 底边贴住触发控件顶边，向上展开；空间不够只缩短高度，菜单不离开锚点
      const maxH = applyMaxH(spaceAbove);
      popupEl.style.left = left + "px";
      popupEl.style.top = "auto";
      popupEl.style.bottom = Math.max(0, vh - rect.top + gap) + "px";
      popupEl.style.maxHeight = maxH + "px";
      popupEl.style.visibility = "visible";
      popupEl.classList.add("popup-up");
    } else {
      let top = rect.bottom + gap;
      const needH = Math.min(pr.height || initialMaxH || 160, applyMaxH(spaceBelow || vh * 0.45));
      if (top + needH > vh - 8 && spaceAbove >= 96) {
        const maxH = applyMaxH(spaceAbove);
        popupEl.style.left = left + "px";
        popupEl.style.top = "auto";
        popupEl.style.bottom = Math.max(0, vh - rect.top + gap) + "px";
        popupEl.style.maxHeight = maxH + "px";
        popupEl.style.visibility = "visible";
        popupEl.classList.add("popup-up");
      } else {
        const maxH = applyMaxH(Math.max(96, vh - top - 8));
        popupEl.style.left = left + "px";
        popupEl.style.top = top + "px";
        popupEl.style.bottom = "auto";
        popupEl.style.maxHeight = maxH + "px";
        popupEl.style.visibility = "visible";
        popupEl.classList.remove("popup-up");
      }
    }
    // 展开时滚到当前选中项（尽量靠上；末尾项自然贴底）
    if (selectedIdx >= 0) {
      const selEl = popupEl.querySelector(`.mi[data-i="${selectedIdx}"]`);
      if (selEl) {
        const maxScroll = Math.max(0, popupEl.scrollHeight - popupEl.clientHeight);
        popupEl.scrollTop = Math.min(Math.max(0, selEl.offsetTop - 2), maxScroll);
      }
    }
    let picked = false;
    const pick = (e) => {
      if (!popupEl) return;
      const item = e.target && e.target.closest ? e.target.closest(".mi") : null;
      if (!item || !popupEl.contains(item)) return;
      e.preventDefault();
      e.stopPropagation();
      if (typeof e.stopImmediatePropagation === "function") e.stopImmediatePropagation();
      if (picked) return;
      picked = true;
      const idx = +item.dataset.i;
      const chosen = items[idx];
      hidePopup();
      if (chosen && typeof onPick === "function") {
        try {
          onPick(chosen, idx);
        } catch (err) {
          console.error("popup onPick", err);
        }
      }
    };
    // 捕获阶段选中，避免 document mousedown(capture) 先关掉菜单
    popupEl.addEventListener("pointerdown", pick, true);
    popupEl.addEventListener("mousedown", pick, true);
    popupEl.addEventListener("click", pick, true);

    const onWheel = (e) => {
      if (!popupEl) return;
      if (popupEl.contains(e.target)) {
        e.preventDefault();
        e.stopPropagation();
        popupEl.scrollTop += e.deltaY;
        return;
      }
      hidePopup();
    };
    const onScroll = (e) => {
      if (!popupEl) return;
      if (e.target === popupEl || (e.target && popupEl.contains(e.target))) return;
      hidePopup();
    };
    window.addEventListener("wheel", onWheel, { capture: true, passive: false });
    window.addEventListener("scroll", onScroll, true);
    popupUnbindScroll = () => {
      window.removeEventListener("wheel", onWheel, true);
      window.removeEventListener("scroll", onScroll, true);
    };
  }

  function bindDynamic(root) {
    bindPlaybackSpeedSlider();
    $$(".chk", root || document).forEach((c) => {
      if (c._bound) return;
      if (c.closest("#optList")) return;
      if (c.closest("#edActionCatalog")) return;
      if (c.id === "schedGlobalDisable") return;
      c._bound = true;
      c.addEventListener("click", (e) => {
        e.preventDefault();
        e.stopPropagation();
        c.classList.toggle("on");
        const i = c.querySelector("i");
        if (i) i.textContent = "";
      });
    });
    $$(".sched-resume-pair, .sound-chk-pair", root || document).forEach((p) => {
      if (p._boundResume) return;
      p._boundResume = true;
      p.addEventListener("click", (e) => {
        if (e.target.closest(".chk")) return;
        const chk = p.querySelector(".chk");
        if (chk) chk.click();
      });
    });
  }

  function applyListsFromMsg(msg) {
    if (Array.isArray(msg.scripts)) {
      state.macros = msg.scripts;
    }
    if (Array.isArray(msg.recordings)) {
      state.recordings = msg.recordings;
    }
    rematchListSelectionByPath();
    renderLists();
    updateCtas();
    // 列表刷新只记忆路径；真正推引擎用防抖，避免每轮 list* 写盘卡死热键
    syncEngineHomeSelectionDebounced();
    if (window.ProMode) window.ProMode.onDataChanged();
  }

  function onBridge(ev) {
    const msg = ev.detail || {};
    const type = msg.type || "";
    if (type === "listScripts.result" && msg.ok) {
      state.macros = Array.isArray(msg.scripts) ? msg.scripts : [];
      rematchListSelectionByPath();
      state._gotScripts = true;
      renderMacroList();
      updateCtas();
      syncEngineHomeSelectionDebounced();
      maybeRestoreHomeSelection();
      if (window.ProMode) window.ProMode.onDataChanged();
      if (state._pendingPickMacro) {
        const p = state._pendingPickMacro;
        state._pendingPickMacro = null;
        if (p.open) p.open();
      }
      return;
    }
    if (type === "listRecordings.result" && msg.ok) {
      state.recordings = Array.isArray(msg.recordings) ? msg.recordings : [];
      rematchListSelectionByPath();
      state._gotRecordings = true;
      renderRecList();
      updateCtas();
      syncEngineHomeSelectionDebounced();
      maybeRestoreHomeSelection();
      if (window.ProMode) window.ProMode.onDataChanged();
      if (state._pendingPickRec) {
        const p = state._pendingPickRec;
        state._pendingPickRec = null;
        if (p.open) p.open();
      }
      return;
    }
    if (type === "listAgentConversations.result" && msg.ok) {
      state.chats = Array.isArray(msg.conversations) ? msg.conversations : [];
      renderAiList();
      if (window.ProMode) window.ProMode.onDataChanged();
      return;
    }
    if (type === "openSettingsData.result" && msg.ok) {
      fillSettings(msg.settings || {});
      state._gotSettings = true;
      maybeRestoreHomeSelection();
      return;
    }
    if (type === "settings.changed") {
      const playback = msg.playback || {};
      if (playback.enableDebugOutputWindow != null) {
        setChk($("#setDebugWin"), !!playback.enableDebugOutputWindow);
        if (!state.settings) state.settings = {};
        if (!state.settings.playback) state.settings.playback = {};
        state.settings.playback.enableDebugOutputWindow = !!playback.enableDebugOutputWindow;
        if (!window.qst) {
          if (playback.enableDebugOutputWindow) showDebugFloat({ keepLog: true });
          else hideDebugFloat();
        } else {
          hideDebugFloat();
        }
      }
      const other = msg.other || {};
      if (other.showFloatBall != null) {
        setChk($("#setFloatBall"), !!other.showFloatBall);
        if (!state.settings) state.settings = {};
        if (!state.settings.other) state.settings.other = {};
        state.settings.other.showFloatBall = !!other.showFloatBall;
      }
      return;
    }
      if (msg.text) toast(String(msg.text));
      return;
    }
    if (type === "debugWindow.show") {
      // 独立调试窗由壳 DispatchDebugUiMessage 打开；主壳忽略
      if (!window.qst) showDebugFloat({ keepLog: true });
      return;
    }
    if (type === "debugWindow.hide") {
      hideDebugFloat();
      return;
    }
    if (type === "debugWindow.clear") {
      if (!window.qst) {
        const log = $("#debugLog");
        if (log) {
          log.textContent = "";
          log._qstLineCount = 0;
        }
      }
      return;
    }
    if (type === "debugWindow.append") {
      if (!window.qst) appendDebugLogLines([msg.text || ""]);
      return;
    }
    if (type === "debugWindow.appendBatch") {
      if (!window.qst) {
        const lines = Array.isArray(msg.lines) ? msg.lines : [];
        appendDebugLogLines(lines);
      }
      return;
    }
    if (type === "openThemeCustom.useWeb") {
      syncThemeColorInputs();
      openOv("theme");
      return;
    }
    if (type === "getHomeState.result" && msg.ok) {
      const home = (state.settings && state.settings.home) || {};
      applyHomeStateMsg({
        activeTab: msg.activeTab != null ? msg.activeTab : home.activeTab | 0,
        selectedScriptPath: msg.selectedScriptPath || home.selectedScriptPath || "",
        selectedRecordingPath: msg.selectedRecordingPath || home.selectedRecordingPath || "",
      });
      return;
    }
    if (type === "themeCatalog.result" && msg.ok) {
      state.themes = Array.isArray(msg.themes) ? msg.themes : [];
      applyThemeFromSettings(state.settings || {});
      return;
    }
    if (type === "debugWindow.needTheme") {
      pushThemeCssToDebug();
      return;
    }
    if (type === "applyTheme.result" || type === "openThemeCustom.result") {
      if (!msg.ok) {
        toast(msg.detail || "主题未更改");
        return;
      }
      if (msg.settings) fillSettings(msg.settings);
      else if (msg.useCustomTheme) {
        state.useCustomTheme = true;
        if (state.settings && state.settings.other) {
          state.settings.other.useCustomTheme = true;
          state.settings.other.customMainColor = msg.customMainColor;
          state.settings.other.customAccentColor = msg.customAccentColor;
        }
        applyThemeFromSettings(state.settings || {});
      }
      toast("主题已应用");
      return;
    }
    if (type === "queryVhidStatus.result") {
      const btn = $("#btnVhidInstall");
      const statusEl = $("#vhidStatus");
      const bar = $("#vhidBar");
      const source = state._vhidStatusSource || "dialog";
      state._vhidStatusSource = "";
      if (!msg.ok) {
        if (statusEl) statusEl.textContent = "无法读取驱动安装状态";
        return;
      }
      if (btn) {
        delete btn.dataset.reboot;
        delete btn.dataset.fw;
        btn.textContent = "开始安装";
      }
      let label = "等待开始…";
      if (msg.rebootPending) {
        label = "检测到旧版安装留下的开机任务，请点「卸载并修复」，不要再重启进 BIOS";
        if (bar) bar.style.width = "40%";
      } else if (msg.driverReady) {
        label = "虚拟 HID 驱动已安装，设备可用";
        if (btn) btn.textContent = "重新安装";
        if (bar) bar.style.width = "100%";
        const root = $("#vhidSteps");
        if (root) $$(".driver-step", root).forEach((s) => s.classList.add("done"));
      } else if (!msg.installScriptPresent) {
        label = "未找到驱动安装文件，请使用完整发版包";
      } else if (msg.lastExitCode === 3) {
        label = "上次安装因驱动签名不被当前系统信任而拒绝，未改启动配置";
      } else if (msg.lastExitCode === 5) {
        label = "上次安装因内核拒载已回滚，未注册键盘/鼠标过滤驱动";
      } else if (msg.lastExitCode === 4) {
        label = "已拒绝为加载驱动去修改安全启动/测试签名";
      } else if (msg.lastExitCode === 2) {
        label = "上次安装未完成（设备未就绪），可直接点「开始安装」重试";
      } else if (msg.hvciEnabled) {
        label = "本机开启了内存完整性。未签名驱动无法加载；安装程序不会关闭该保护，请使用「系统模拟」";
      }
      if (statusEl) statusEl.textContent = label;
      if (source === "backend" && !msg.rebootPending && !msg.driverReady) {
        toast("虚拟 HID 驱动未安装。若内核拒载该签名，请使用系统模拟，不要用旧办法关安全启动");
      }
      return;
    }
    if (type === "installDriver.result") {
      const kind = msg.kind || "";
      const root =
        kind === "vhid" || kind === "virtualHid" ? $("#vhidSteps") : $("#icSteps");
      const statusEl =
        kind === "vhid" || kind === "virtualHid" ? $("#vhidStatus") : $("#icStatus");
      const bar = kind === "vhid" || kind === "virtualHid" ? $("#vhidBar") : $("#icBar");
      const btn =
        kind === "vhid" || kind === "virtualHid"
          ? $("#btnVhidInstall")
          : $("#btnIcInstall");
      if (msg.ok) {
        if (root) $$(".driver-step", root).forEach((s) => s.classList.add("done"));
        if (bar) bar.style.width = "100%";
        if (btn) {
          btn.textContent = "开始安装";
          delete btn.dataset.reboot;
          delete btn.dataset.fw;
        }
        if (msg.uninstalled) {
          if (statusEl) statusEl.textContent = msg.hint || "已卸载并清除过滤驱动残留";
          toast(msg.hint || "已卸载并修复");
          if (root) $$(".driver-step", root).forEach((s) => s.classList.remove("done"));
          if (bar) bar.style.width = "0%";
          document
            .querySelectorAll('.set-pane[data-pane="play"] .radio[data-backend]')
            .forEach((r) => {
              r.classList.toggle("on", (r.dataset.backend | 0) === 0);
            });
          if (typeof collectSettings === "function" && typeof quietSaveSettings === "function") {
            quietSaveSettings(collectSettings());
          }
          return;
        }
        const hint =
          msg.hint || "请在设置中选择注入后端并保存";
        if (statusEl) {
          statusEl.textContent = msg.probed
            ? "安装成功，设备已探测到 — " + hint
            : "安装成功（若驱动未就绪请重启）— " + hint;
        }
        // 可选：探测成功则自动勾选对应后端（仍需用户点保存）
        const backend = msg.suggestedBackend | 0;
        if (msg.probed && backend > 0) {
          document
            .querySelectorAll('.set-pane[data-pane="play"] .radio[data-backend]')
            .forEach((r) => {
              r.classList.toggle("on", (r.dataset.backend | 0) === backend);
            });
          quietSaveSettings(collectSettings());
          toast(
            (kind === "vhid" || kind === "virtualHid" ? "虚拟 HID" : "Interception") +
              " 驱动已装好，已自动切换并保存，现在可以直接使用"
          );
        } else {
          toast(hint);
        }
      } else if (msg.rebootRequired) {
        if (bar) bar.style.width = "40%";
        if (statusEl) {
          statusEl.textContent =
            msg.detail || "安装未改启动配置。请使用系统模拟或「卸载并修复」";
        }
        toast(msg.detail || "安装未完成");
      } else {
        if (btn && (kind === "vhid" || kind === "virtualHid")) {
          btn.textContent = "开始安装";
          delete btn.dataset.reboot;
          delete btn.dataset.fw;
        }
        if (statusEl) statusEl.textContent = msg.detail || "安装失败或已取消";
        toast(msg.detail || "安装失败");
      }
      return;
    }
    if (type === "systemReboot.result") {
      if (!msg.ok) toast(msg.detail || "重启失败，请手动重启");
      else toast("正在重启…");
      return;
    }
    if (type === "crosshairPick.result") {
      if (!msg.ok) {
        if (msg.detail !== "cancelled") toast(msg.detail || "拾取失败");
        state._pendingCrosshair = "";
        return;
      }
      const pick = normalizeCrosshairPick(msg);
      const pending = state._pendingCrosshair || "";
      const mode = msg.mode || state._crosshairMode || pending || "";
      if (pending === "fixedClick") {
        if ($("#setFixedX")) $("#setFixedX").textContent = String(pick.x | 0);
        if ($("#setFixedY")) $("#setFixedY").textContent = String(pick.y | 0);
        setChk($("#setFixedCoordEn"), true);
        toast(`定点 ${pick.x | 0},${pick.y | 0}`);
      } else if (mode === "coordinates" || pending === "coord") {
        const a = editorParamAction();
        if (a) {
          a.x = pick.x | 0;
          a.y = pick.y | 0;
          renderParamPanel(a);
        } else {
          toast("请先选中或添加「移动鼠标」动作");
        }
        if (a) toast(`坐标 ${a.x},${a.y}`);
      } else if (mode === "programPath" || pending === "program") {
        const a = editorParamAction();
        if (a && pick.processPath) {
          a.targetPath = pick.processPath;
          if (a.type === "runProgram") a.shortcutPreset = 0;
          renderParamPanel(a);
          toast(pick.processPath);
        } else {
          toast(pick.processPath || "未获取路径");
        }
      } else if (
        mode === "windowTarget" ||
        pending === "windowClass" ||
        pending === "window" ||
        pending === "nestedWm" ||
        pending === "nestedWmClass"
      ) {
        if (pending === "nestedWm" || pending === "nestedWmClass") {
          applyWindowPickToNestedAction(pick, pending);
          fillPickOverlay(pick);
          if (pending === "nestedWmClass" || !pick.processPath) {
            state._nestedWmPick = true;
            openOv("pick");
          } else toast(pick.processPath || pick.windowTitle || "已绑定窗口");
        } else {
          applyWindowPickToEditor(pick, pending);
          fillPickOverlay(pick);
          if (pending === "windowClass" || !pick.processPath) openOv("pick");
          else toast(pick.processPath || pick.windowTitle || "已绑定窗口");
        }
      } else {
        fillPickOverlay(pick);
        openOv("pick");
      }
      state._pendingCrosshair = "";
      return;
    }
    if (type === "browsePath.result") {
      if (!msg.ok) {
        if (msg.detail !== "cancelled") toast(msg.detail || "取消");
        return;
      }
      if (state._pendingBrowse === "agentAttach") {
        if (msg.path) {
          addAgentAttachment(msg.path);
          toast("已添加附件 " + msg.path.split(/[/\\]/).pop());
        }
        state._pendingBrowse = "";
        return;
      }
      if (state._pendingBrowse === "wmTarget") {
        if (msg.path) {
          state.windowMode = state.windowMode || {};
          state.windowMode.targetExePath = msg.path;
          if ($("#edTarget")) $("#edTarget").textContent = msg.path;
          syncEditorModeUi();
          toast("已选择程序路径");
        }
        state._pendingBrowse = "";
        return;
      }
      if (state._pendingBrowse === "nestedWmTarget") {
        const a = editorParamAction();
        if (a && (a.type === "runMacro" || a.type === "mousePlayback") && msg.path) {
          readParamPanelInto(a);
          const wm = ensureNestedWindowMode(a);
          wm.targetExePath = msg.path;
          a.nestedWindowMode = wm;
          renderParamPanel(a);
          toast("已选择程序路径");
        }
        state._pendingBrowse = "";
        return;
      }
      // 助手窗开着时 browse 回包也会到主壳：无本页待处理选择则忽略，避免写进编辑器路径
      if (!state._pendingBrowse) return;
      const a = editorParamAction();
      if (a && msg.path) {
        a.targetPath = msg.path;
        if (a.type === "runProgram") a.shortcutPreset = 0;
        renderParamPanel(a);
      }
      state._pendingBrowse = "";
      return;
    }
    if (type === "captureTemplateScreenshot.result") {
      if (!msg.ok) {
        if (msg.detail !== "cancelled") toast(msg.detail || "截图取消");
        state._pendingAiTemplateShot = false;
        return;
      }
      const a = editorParamAction();
      if (a) {
        const path = msg.imagePath || msg.resolvedPath || "";
        if (state._pendingAiTemplateShot) a.aiTargetImagePath = path;
        else a.imagePath = path;
        renderParamPanel(a);
        toast("模板已保存");
      }
      state._pendingAiTemplateShot = false;
      return;
    }
    if (type === "readImageDataUrl.result") {
      if (msg.reqId && state._pendingCropReqId && msg.reqId === state._pendingCropReqId) {
        state._pendingCropReqId = "";
        const img = $("#cropBgImg");
        if (!msg.ok) {
          toast(msg.detail || "无法加载图片");
          closeAllOv();
          return;
        }
        const url = msg.assetUrl || msg.dataUrl || "";
        if (!img || !url) {
          toast("无法加载图片");
          closeAllOv();
          return;
        }
        img.onload = () => {
          _cropImgNatural = { w: img.naturalWidth | 0, h: img.naturalHeight | 0 };
          img.style.opacity = "1";
          resetCropSelFull();
          bindCropStageInteractions();
        };
        img.onerror = () => {
          toast("无法加载图片");
          closeAllOv();
        };
        img.src = url;
        return;
      }
      if (msg.reqId && String(msg.reqId).startsWith("aiv_")) {
        if (!state._imgViewer) return;
        if (state._pendingAivReqId && msg.reqId !== state._pendingAivReqId) return;
        state._pendingAivReqId = "";
        const aivImg = $("#aivImg");
        const v = state._imgViewer;
        if (!msg.ok) {
          toast(msg.detail || "无法加载图片");
          return;
        }
        const url = msg.assetUrl || msg.dataUrl || "";
        if (!url || !aivImg) return;
        if (v && v.items && v.items[v.index]) {
          v.items[v.index].previewUrl = url;
          rememberConvImage(v.items[v.index]);
        }
        aivImg.src = url;
        applyAgentImageViewerTransform();
        return;
      }
      const byId = state._pendingThumbById || {};
      let img = msg.reqId ? byId[msg.reqId] : null;
      if (!img && msg.reqId) {
        const safe = String(msg.reqId).replace(/\\/g, "\\\\").replace(/"/g, '\\"');
        img = document.querySelector('img.fi-thumb[data-req-id="' + safe + '"]');
      }
      if (msg.reqId) delete byId[msg.reqId];
      if (!msg.ok) {
        if (img && img.parentNode) {
          const span = document.createElement("span");
          span.className = "fi-empty";
          span.textContent = msg.detail || "无法预览";
          img.replaceWith(span);
        }
        toast(msg.detail || "无法预览图片");
        return;
      }
      if (!img) return;
      const url = msg.assetUrl || msg.dataUrl || "";
      if (!url) return;
      img.onload = () => {
        img.style.display = "block";
      };
      img.onerror = () => {
        if (!img.parentNode) return;
        const span = document.createElement("span");
        span.className = "fi-empty";
        span.textContent = "预览失败";
        img.replaceWith(span);
      };
      img.src = url;
      // 同步附件缩略图到会话图库
      const p = img.dataset && img.dataset.imgPath;
      if (p) rememberConvImage({ path: p, previewUrl: url });
      return;
    }
    if (type === "actionKey.llKey") {
      applyLlActionKey(msg);
      return;
    }
    if (type === "hotkey.llUpdate") {
      applyHotkeyLlUpdate(msg);
      return;
    }
    if (type === "beginHotkeyCapture.result") {
      if (!msg.ok) {
        toast(msg.detail || "热键捕获启动失败");
        return;
      }
      if (msg.holdThresholdSeconds > 0) {
        _hotkeyHoldMs = Math.round(Number(msg.holdThresholdSeconds) * 1000) || 200;
        syncHotkeyValUi();
      }
      return;
    }
    if (
      type === "captureActionKey.result" ||
      type === "captureScriptHotkey.result" ||
      type === "captureGlobalHotkey.result"
    ) {
      // 已废弃：热键/动作键走 Web 弹层 + LL
      return;
    }
    if (type === "formatHotkey.result") {
      // 确定键：优先消费应用结果；只接受与当前草稿 VK 一致的结果，
      // 避免「按键显示结果迟到」与「确定格式化结果」互相吃掉
      if (state._pendingActionKeyApply) {
        const d = _actionKeyDraft || {};
        if (msg.ok && msg.hotkeyVk === (d.vk | 0)) {
          state._pendingActionKeyApply = false;
          applyActionKeyToParam(msg.hotkeyText || "", d.vk | 0, d.modifiers | 0);
          stopActionKeyCapture();
          const ov = $("#ov-action-key");
          if (ov) ov.classList.remove("show");
        } else if (!msg.ok) {
          state._pendingActionKeyApply = false;
          toast(msg.detail || "格式化按键失败");
        }
        return;
      }
      // 结果与最新按键一一对应：只消费与当前草稿 VK 一致的结果，
      // 过期结果（快速连按 / 关闭弹窗后才到达）不再覆盖显示或吃掉后续标记
      if (state._pendingHotkeyDisplay && $("#ov-hotkey")?.classList.contains("show")) {
        if (
          msg.ok &&
          state.hotkeyDraft &&
          msg.hotkeyVk === (state.hotkeyDraft.vk | 0)
        ) {
          state._pendingHotkeyDisplay = false;
          state.hotkeyDraft.text = msg.hotkeyText || state.hotkeyDraft.text;
          // 长按只由 LL/计时器/抬起判定。过期 formatHotkey.result（keydown 时 hold=0）
          // 若回写 hold，会把已判定的「长按」冲掉。
          syncHotkeyValUi();
        }
        return;
      }
      if (state._pendingActionKeyDisplay && _actionKeyCapturing) {
        if (msg.ok && _actionKeyDraft && msg.hotkeyVk === (_actionKeyDraft.vk | 0)) {
          state._pendingActionKeyDisplay = false;
          _actionKeyDraft.text = msg.hotkeyText || _actionKeyDraft.text;
          syncActionKeyValUi();
        }
        return;
      }
      return;
    }
    if (type === "window.clientSize") {
      state._clientW = msg.clientW | 0;
      state._clientH = msg.clientH | 0;
      applyShellScale();
      if (msg && msg.match === false) {
        console.warn(
          "[qst] client size mismatch",
          msg.clientW,
          msg.clientH,
          "expect design",
          msg.designW,
          msg.designH,
          "dpi",
          msg.dpi
        );
      }
      if (
        state.editor &&
        (state._clientW | 0) > 0 &&
        (state._clientW | 0) < 1600 &&
        window.qst &&
        !state._resizeRetry
      ) {
        state._resizeRetry = true;
        qst.setMode("editor");
        setTimeout(() => {
          state._resizeRetry = false;
        }, 800);
      }
      return;
    }
    if (type === "window.setMode.result") {
      requestAnimationFrame(syncTypingHotkeyMute);
      applyShellScale();
      if (state._clientW == null && msg.clientW) state._clientW = msg.clientW | 0;
      if (state._clientH == null && msg.clientH) state._clientH = msg.clientH | 0;
      if (msg.clientW) state._clientW = msg.clientW | 0;
      if (msg.clientH) state._clientH = msg.clientH | 0;
      // 优化窗：尺寸已到位，等列表填完再揭开（openOv 已提前 setMode）
      if (msg.mode === "opt" && document.body.classList.contains("opt-open")) {
        state._optModeReady = true;
        tryRevealOptMode();
        return;
      }
      // 进入编辑器：内容已就绪后的 setMode → 揭开遮罩并 modeReady
      if (state._pendingModeReveal) {
        const want = state._pendingModeReveal;
        if (want === msg.mode || (want === "editor" && msg.mode === "editor") || (want === "opt" && msg.mode === "opt")) {
          state._pendingModeReveal = "";
          if (state._modeTransEndOn === "openEditor" || state._modeTransEndOn === "loadOpt") {
            endModeTransition(state._modeTransEndOn);
          } else {
            revealModeAfterPaint();
          }
          return;
        }
      }
      // 启动时迟到的 home 回包：编辑器正在开，勿按主页尺寸揭开
      if (msg.mode === "home" && state.editor) {
        return;
      }
      if (state._pendingUncloak || msg.mode === "home") {
        state._pendingUncloak = false;
        requestAnimationFrame(() => {
          if (window.qst && typeof qst.modeReady === "function") qst.modeReady();
        });
      }
      if (state._modeTransEndOn === "setMode") endModeTransition("setMode");
      return;
    }
    if (type === "getEngineStatus.result" && msg.ok) {
      const wasRunning = !!state.macroRunning;
      const wasDebugging = !!state.debugging;
      const statusKeyCore = [
        msg.clicking | 0,
        msg.running | 0,
        msg.recording | 0,
        msg.breakoutPaused | 0,
        msg.debugging | 0,
        msg.debugPaused | 0,
        msg.runningMode | 0,
        msg.currentScript || "",
      ].join("|");
      const statusKey = statusKeyCore + "|" + (msg.executedSteps | 0);
      if (statusKey === state._engineStatusKey && state.macroRunning === !!msg.running) {
        // 状态未变：跳过整页 CTA 刷新，减轻卡顿
        return;
      }
      // 仅步数变化：只改 HUD 文案，避免整页 updateCtas
      if (
        state._engineStatusCore === statusKeyCore &&
        state.macroRunning &&
        !!msg.running &&
        msg.executedSteps != null
      ) {
        state._engineStatusKey = statusKey;
        state._executedSteps = msg.executedSteps | 0;
        const meta = $("#runHudMeta");
        if (meta && meta.dataset && meta.dataset.hudBase) {
          meta.innerHTML = meta.dataset.hudBase + ` · 已执行 ${state._executedSteps} 步` + (meta.dataset.hudExtra || "");
        } else {
          updateCtas();
        }
        return;
      }
      state._engineStatusCore = statusKeyCore;
      state._engineStatusKey = statusKey;
      state.clicking = !!msg.clicking;
      state.macroRunning = !!msg.running;
      state.recording = !!msg.recording;
      state.breakoutPaused = !!msg.breakoutPaused && !!msg.running;
      state.debugging = !!msg.debugging;
      state.debugPaused = !!msg.debugPaused;
      state.debugStepMode = !!msg.debugStepMode;
      if (msg.executedSteps != null) state._executedSteps = msg.executedSteps | 0;
      if (msg.currentScript) state._runningMacroName = msg.currentScript;
      if (msg.runningMode != null) state._runningMode = msg.runningMode | 0;
      if (msg.windowMode && typeof msg.windowMode === "object") {
        state._runningWindowMode = msg.windowMode;
      } else if (!state.macroRunning) {
        state._runningWindowMode = null;
      }
      if (!state.macroRunning) {
        state._executedSteps = 0;
        state._runningMode = 0;
        state._runningWindowMode = null;
        state.breakoutPaused = false;
        state._wmPreviewAutoShown = false;
        if (wasDebugging) {
          state.debugging = false;
          state.debugPaused = false;
          state.debugStepMode = false;
          toast("调试结束，已返回编辑界面");
          renderEditorActions(state.editorActions);
        }
        if (wasRunning) {
          state._runningMacroName = "";
          if ($("#wmPreview")?.classList.contains("show") && window.qst) {
            qst.hideWindowModePreview();
            $("#wmPreview")?.classList.remove("show");
          }
        }
      } else if ((state._runningMode | 0) > 0 && !state._wmPreviewAutoShown) {
        // 「运行时显示目标窗口预览缩略图」：专业/极简共用
        const wmCfg = (state.settings && state.settings.windowMode) || {};
        const wantThumb =
          wmCfg.showPreviewThumbnail != null
            ? !!wmCfg.showPreviewThumbnail
            : $("#setWmPreviewThumb")
              ? !!$("#setWmPreviewThumb").classList.contains("on")
              : true;
        if (wantThumb && window.qst) {
          state._wmPreviewAutoShown = true;
          qst.showWindowModePreview({ fromRunning: 1 });
        }
      }
      updateCtas();
      return;
    }
    if (type === "getAppBranding.result" && msg.ok) {
      applyAppBranding(msg.branding || {});
      return;
    }
    if (type === "checkUpgrade.result") {
      toast(
        msg.detail ||
          (msg.stub
            ? "当前版本未接入在线升级检查。"
            : msg.ok
              ? "检查完成。"
              : "检查失败")
      );
      return;
    }
    if (type === "installDriver.progress") {
      applyDriverProgress(msg);
      return;
    }
    if (type === "installOcr.progress") {
      const bar = $("#ocrBar");
      const track = bar && bar.parentElement;
      if (msg.indeterminate) {
        if (track) track.classList.add("indeterminate");
        if (bar) bar.style.width = "40%";
      } else {
        if (track) track.classList.remove("indeterminate");
        if (bar) bar.style.width = (msg.percent | 0) + "%";
      }
      if ($("#ocrStatus") && msg.status) $("#ocrStatus").textContent = msg.status;
      return;
    }
    if (type === "pickImageFile.result") {
      if (!msg.ok) return;
      const a = editorParamAction();
      if (a && state._pendingImageField) {
        a[state._pendingImageField] = msg.path || "";
        renderParamPanel(a);
      }
      state._pendingImageField = "";
      return;
    }
    if (type === "findImageCrop.result") {
      if (!msg.ok) {
        if (msg.detail && msg.detail !== "cancelled") toast(msg.detail);
        return;
      }
      const a = editorParamAction();
      if (a && !msg.unchanged) {
        if (msg.imagePath) a.imagePath = msg.imagePath;
        if (msg.offsetX != null) a.offsetX = msg.offsetX;
        if (msg.offsetY != null) a.offsetY = msg.offsetY;
        renderParamPanel(a);
      }
      if ($("#ov-crop")?.classList.contains("show")) {
        closeAllOv();
        if (msg.unchanged) toast("未裁切（全图）");
        else toast("裁切已应用");
      }
      return;
    }
    if (type === "findImageMatch.result") {
      if (!msg.ok) {
        if (msg.detail && msg.detail !== "cancelled") toast(msg.detail);
        return;
      }
      const a = editorParamAction();
      if (a) {
        if (msg.mode === "offset") {
          a.offsetX = msg.offsetX | 0;
          a.offsetY = msg.offsetY | 0;
        } else if (
          (msg.mode === "region" || msg.mode === "regionBySize") &&
          msg.regionValid
        ) {
          if (state._pendingImageRegionPick) {
            const ax = msg.matchTopLeftX | 0;
            const ay = msg.matchTopLeftY | 0;
            a.imageRegionX1 = (msg.searchX1 | 0) - ax;
            a.imageRegionY1 = (msg.searchY1 | 0) - ay;
            a.imageRegionX2 = (msg.searchX2 | 0) - ax;
            a.imageRegionY2 = (msg.searchY2 | 0) - ay;
            state._pendingImageRegionPick = false;
          } else if (msg.mode === "region") {
            a.searchX1 = msg.searchX1 | 0;
            a.searchY1 = msg.searchY1 | 0;
            a.searchX2 = msg.searchX2 | 0;
            a.searchY2 = msg.searchY2 | 0;
            a.searchFullScreen = 0;
          }
        }
        if (msg.mode === "test") {
          if (msg.found) {
            const score =
              msg.bestScore != null ? Math.round(Number(msg.bestScore) || 0) : null;
            toast(
              score != null
                ? `测试：已找到匹配（最高 ${score}%）`
                : "测试：已找到匹配"
            );
          } else {
            toast("测试：未找到匹配");
          }
        }
        renderParamPanel(a);
      } else if (msg.mode === "test") {
        toast(msg.found ? "测试：已找到匹配" : "测试：未找到匹配");
      }
      return;
    }
    if (type === "pickScreenRegion.result") {
      if (!msg.ok) return;
      const a = editorParamAction();
      if (a) {
        if (state._pendingRegionTarget === "ai") {
          a.aiSearchX1 = msg.x1 | 0;
          a.aiSearchY1 = msg.y1 | 0;
          a.aiSearchX2 = msg.x2 | 0;
          a.aiSearchY2 = msg.y2 | 0;
          a.searchFullScreen = 0;
        } else {
          a.searchX1 = msg.x1 | 0;
          a.searchY1 = msg.y1 | 0;
          a.searchX2 = msg.x2 | 0;
          a.searchY2 = msg.y2 | 0;
          a.searchFullScreen = 0;
        }
        renderParamPanel(a);
      }
      state._pendingRegionTarget = "";
      return;
    }
    if (type === "saveSettings.result") {
      if (msg.ok) {
        // 仅设置页点「保存」回主界面时提示；连点/Tab/模型等自动落盘不弹
        if (state._announceSettingsSave) {
          state._announceSettingsSave = false;
          toast("设置已保存");
        }
        state._quietSaveSettings = false;
        refreshAiModelCombos();
      } else {
        const quiet = !!state._quietSaveSettings;
        state._announceSettingsSave = false;
        state._quietSaveSettings = false;
        // 自动落盘失败不弹；SaveAppSettings failed 已在 toast() 过滤
        if (!quiet) toast(msg.detail || "保存失败");
      }
      return;
    }
    if (type === "showWindowModePreview.result") {
      const prev = $("#wmPreview");
      if (!msg.ok) {
        toast(msg.detail || "无法预览目标窗口");
        return;
      }
      if (prev) {
        prev.classList.add("show");
        const lab = prev.querySelector(".label");
        const target = prev.querySelector(".target");
        const wm = state.windowMode || {};
        const label =
          msg.label ||
          wm.targetExePath ||
          wm.windowClassName ||
          wm.windowTitle ||
          "目标窗口";
        if (lab) lab.textContent = "窗口模式目标 · " + label;
        if (target && msg.dataUrl) {
          target.style.backgroundImage = `url("${msg.dataUrl}")`;
          target.style.backgroundSize = "cover";
          target.style.backgroundPosition = "center";
        }
      }
      return;
    }
    if (type === "windowModePreview.frame") {
      const prev = $("#wmPreview");
      if (!prev || !prev.classList.contains("show")) return;
      const target = prev.querySelector(".target");
      if (target && msg.dataUrl) {
        target.style.backgroundImage = `url("${msg.dataUrl}")`;
        target.style.backgroundSize = "cover";
        target.style.backgroundPosition = "center";
      }
      return;
    }
    if (type === "hideWindowModePreview.result") {
      $("#wmPreview")?.classList.remove("show");
      return;
    }
    if (
      type === "startClicker.result" ||
      type === "stopClicker.result" ||
      type === "getClickerStatus.result"
    ) {
      if (!msg.ok && msg.detail) toast(msg.detail);
      applyClickerStatus(msg.status);
      return;
    }
    if (type === "runScript.result") {
      if (!msg.ok && msg.detail === "busy") {
        // 已有宏在运行：主界面“开始执行宏”按钮语义 = 再点一次立即终止当前脚本。
        // 状态消息偶发滞后时也走这里，避免只弹 busy 却不停止。
        if (window.qst) qst.stopScript();
        toast("宏正在运行，已停止当前脚本");
      } else if (!msg.ok && msg.detail) {
        toast(msg.detail);
      } else if (msg.ok) {
        toast(
          state._pendingRunKind === "recording"
            ? "录制回放已开始"
            : "宏已开始运行"
        );
      }
      state._pendingRunKind = "";
      return;
    }
    if (type === "debugScript.result") {
      if (!msg.ok) {
        state.debugging = false;
        state.debugPaused = false;
        state.debugStepMode = false;
        toast(msg.detail || "调试启动失败");
        return;
      }
      toast(
        state.debugStepMode
          ? "单步调试已开始（按调试热键执行下一步）"
          : "调试已开始（断点处结束，调试热键可暂停/继续）"
      );
      return;
    }
    if (type === "logicConvert.updated") {
      const p = (msg.path || "").replace(/\//g, "\\").toLowerCase();
      const cur = (state.editorPath || "").replace(/\//g, "\\").toLowerCase();
      if (msg.summary) toast(String(msg.summary));
      // 写回后立刻刷新动作数/列表；路径归一化后匹配则重开编辑器
      if (window.qst?.listScripts) qst.listScripts();
      const pathToOpen = state.editorPath || msg.path || "";
      if (pathToOpen && window.qst?.openEditor) {
        if (!p || !cur || p === cur) {
          qst.openEditor({ path: pathToOpen });
        }
      }
      return;
    }
    if (type === "peekScriptActions.result") {
      const reqId = String(msg.reqId || "");
      const waiter = state._peekWaiters && state._peekWaiters[reqId];
      if (waiter) {
        delete state._peekWaiters[reqId];
        waiter(msg);
      }
      return;
    }
    if (type === "openEditor.result") {
      if (state._editorOpenWatchdog) {
        clearTimeout(state._editorOpenWatchdog);
        state._editorOpenWatchdog = null;
      }
      if (!state.editor) return;
      if (!msg.ok) {
        document.body.classList.remove("editor-booting");
        endModeTransition("force");
        if (msg.detail) toast(msg.detail);
        return;
      }
      const script = msg.script || {};
      if (script.path && !Array.isArray(script.actions)) {
        // 文件存在但内容损坏：不要打开一个空编辑器假装成功
        document.body.classList.remove("editor-booting");
        endModeTransition("force");
        toast("脚本内容损坏，无法打开编辑器");
        return;
      }
      try {
      state.editorPath = script.path || state.editorPath || "";
      state.editorName = script.name || state.editorName || "";
      state.editorMode = typeof script.mode === "number" ? script.mode : 0;
      state.windowMode = script.windowMode || state.windowMode || {};
      state.windowMode.selectMethod = normalizeWmSelectMethod(state.windowMode.selectMethod);
      const wm = state.windowMode || {};
      // 准星/指定窗口类字段往返：填回 pick 浮层，避免只靠 edTarget 丢 class/title/pick
      if ($("#pickTitle"))
        $("#pickTitle").textContent = wm.windowTitle || wm.windowName || "";
      if ($("#pickClass")) $("#pickClass").textContent = wm.windowClassName || "";
      if ($("#pickChild"))
        $("#pickChild").textContent = wm.childWindowClassName || "";
      if ($("#pickPath")) $("#pickPath").textContent = wm.targetExePath || "";
      if ($("#pickX") && wm.targetPickX != null)
        $("#pickX").textContent = String(wm.targetPickX | 0);
      else if ($("#pickX") && wm.pickX != null)
        $("#pickX").textContent = String(wm.pickX | 0);
      if ($("#pickY") && wm.targetPickY != null)
        $("#pickY").textContent = String(wm.targetPickY | 0);
      else if ($("#pickY") && wm.pickY != null)
        $("#pickY").textContent = String(wm.pickY | 0);
      const edName = $("#edName");
      if (edName) edName.textContent = state.editorName;
      const br = $("#edBreakout");
      if (br && script.breakoutTimeSeconds != null)
        br.textContent = String(script.breakoutTimeSeconds);
      syncEditorModeUi();
      state.collapsedContainers = {};
      const loaded = normalizeLoadedActions(script.actions || []);
      // 打开时不选中列表项：右栏固定「移动鼠标」添加预览（与列表内容无关，渲染更快）
      state.actionSel = -1;
      state.addPreview = null;
      state.editDraft = null;
      state.addActionType = firstVisibleActionType();
      const typeCombo = $("#edActionType");
      if (typeCombo) {
        syncEditorActionTypeCombo();
      }
      // 首屏一次性写入，跳过错开 reveal（避免「有计数无行」闪帧）
      state._skipOpenReveal = true;
      renderEditorActions(loaded);
      state._skipOpenReveal = false;
      if (window.QstVisualEditor && typeof QstVisualEditor.applyLayout === "function" && script.visualLayout) {
        QstVisualEditor.applyLayout(script.visualLayout);
      }
      applyEditorDefaultView();
      syncDebugHotkeyUi();
      showAddTypePreview();
      resetEditorHistory({ keepClipboard: true });
      state._editorSnapshot = captureEditorSnapshot();
      // 内容已齐：再放大窗体（cloak 中），setMode.result 里揭开
      state._pendingModeReveal = "editor";
      if (window.qst) qst.setMode("editor");
      else endModeTransition("openEditor");
      } catch (e) {
        document.body.classList.remove("editor-booting");
        endModeTransition("force");
        toast("编辑器打开失败：" + (e && e.message ? e.message : e));
      }
      return;
    }
    if (type === "saveEditor.result") {
      if (!msg.ok) {
        toast(msg.detail || "保存失败");
        return;
      }
      toast("已保存");
      if (Array.isArray(msg.scripts)) state.macros = msg.scripts;
      state._editorSnapshot = captureEditorSnapshot();
      exitEditor(true);
      renderLists();
      updateCtas();
      if (msg.path) selectPathInLists(msg.path, "macro");
      else syncEngineHomeSelection();
      if (window.qst) qst.listScripts();
      return;
    }
    if (
      type === "deleteScript.result" ||
      type === "renameScript.result" ||
      type === "setScriptHotkey.result" ||
      type === "importScript.result"
    ) {
      if (!msg.ok) {
        if (msg.detail && msg.detail !== "cancelled") toast(msg.detail);
        // 失败也应用随响应带回的最新列表，清除“幽灵条目”（如文件已不存在）
        if (Array.isArray(msg.scripts) || Array.isArray(msg.recordings)) {
          if (Array.isArray(msg.scripts)) state.macros = msg.scripts;
          if (Array.isArray(msg.recordings)) state.recordings = msg.recordings;
          renderLists();
          syncEngineHomeSelection();
        }
        return;
      }
      applyListsFromMsg(msg);
      if (type === "importScript.result" && msg.path) {
        selectPathInLists(msg.path);
      } else if (type === "renameScript.result" && msg.path) {
        selectPathInLists(msg.path);
      } else {
        syncEngineHomeSelection();
      }
      if (type === "deleteScript.result") toast("已删除");
      if (type === "renameScript.result") toast("已重命名");
      if (type === "setScriptHotkey.result") {
        toast(
          state.pendingKind === "recordingHotkey" ? "录制热键已更新" : "脚本热键已更新"
        );
      }
      if (type === "importScript.result") toast("已导入");
      closeAllOv();
      return;
    }
    if (type === "exportScript.result") {
      if (!msg.ok) {
        if (msg.detail && msg.detail !== "cancelled") toast(msg.detail);
        return;
      }
      const skipped = msg.skipped | 0;
      if (skipped > 0) {
        const files = Array.isArray(msg.skippedFiles) ? msg.skippedFiles : [];
        const names = files
          .map((p) => String(p || "").split(/[/\\]/).pop())
          .filter(Boolean)
          .slice(0, 5);
        const more = files.length > 5 ? ` 等${files.length}个` : "";
        toast(
          names.length
            ? `导出成功，但有 ${skipped} 个文件被跳过：${names.join("、")}${more}`
            : `导出成功，但有 ${skipped} 个文件被跳过`
        );
      } else {
        toast("已导出");
      }
      return;
    }
    if (type === "testOcr.result") {
      if (!msg.ok) {
        if (msg.needInstall) {
          askConfirm("OCR 未就绪，是否打开安装/修复？", () => {
            state._ocrRepair = false;
            if ($("#ocrStatus"))
              $("#ocrStatus").textContent = "已就绪，点击安装开始下载安装…";
            if ($("#ocrBar")) $("#ocrBar").style.width = "0%";
            if ($("#btnOcrInstall")) {
              $("#btnOcrInstall").disabled = false;
              $("#btnOcrInstall").textContent = "安装插件";
            }
            openOv("ocr");
          });
          return;
        }
        if (msg.detail !== "cancelled") toast(msg.detail || "OCR 操作失败");
        return;
      }
      if (msg.mode === "offset" || msg.offsetX != null) {
        const a = editorParamAction();
        if (a && a.type === "textRecognition") {
          a.offsetX = msg.offsetX | 0;
          a.offsetY = msg.offsetY | 0;
          renderParamPanel(a);
          toast(`偏移 ${a.offsetX},${a.offsetY}`);
        }
        return;
      }
      const text = String(msg.text || "").trim();
      if (text) toast(`识别结果：${text.slice(0, 120)}`);
      else toast("OCR 测试完成（按 Esc 关闭叠层）");
      return;
    }
    if (type === "openRecordingOptimize.result") {
      if (!msg.ok) {
        endModeTransition("force");
        toast(msg.detail || "无法打开优化");
        return;
      }
      if (msg.useHtml) {
        state.optPath = msg.path || "";
        state.optSelected = [];
        state._optOrigDur = null;
        openOv("opt");
        setOptScheme(0);
        if (window.qst) qst.loadOptimizeRecording(state.optPath);
        else endModeTransition("force");
        return;
      }
      endModeTransition("force");
      if (Array.isArray(msg.recordings)) {
        state.recordings = msg.recordings;
        renderLists();
      }
      if (msg.saved) toast("优化已保存");
      return;
    }
    if (
      type === "listScheduledTasks.result" ||
      type === "saveScheduledTask.result" ||
      type === "deleteScheduledTask.result" ||
      type === "setScheduledTasksGlobalDisabled.result"
    ) {
      if (!msg.ok) {
        state.schedStatusBusy = false;
        toast(msg.detail || "定时任务操作失败");
        return;
      }
      renderSchedTable(msg.data || {});
      if (window.ProMode) window.ProMode.onDataChanged();
      if (type === "saveScheduledTask.result") {
        state.schedStatusBusy = false;
        if (!msg.statusOnly) {
          toast("任务已保存");
          closeAllOv();
          // 专业模式：留在定时资源管理器页，不弹任务列表窗
          if (state.uiMode === "pro" && window.ProMode && typeof window.ProMode.switchPage === "function") {
            window.ProMode.switchPage("sched");
          } else {
            openOv("sched");
          }
        }
        // 列表已由本次 result.data 填充；再拉一次以防引擎 Reload 后需同步
        if (window.qst) setTimeout(() => qst.listScheduledTasks(), 60);
      } else if (type !== "listScheduledTasks.result") {
        toast("已更新");
      }
      return;
    }
    if (type === "loadOptimizeRecording.result") {
      if (!msg.ok) {
        endModeTransition("force");
        toast(msg.detail || "加载录制失败");
        return;
      }
      fillOptUi(msg.recording || {}, {
        progressive: true,
        onDone: () => {
          state._optContentReady = true;
          tryRevealOptMode();
        },
      });
      return;
    }
    if (type === "applyOptimizeRecording.result") {
      if (!msg.ok) {
        toast(msg.detail || "优化失败");
        return;
      }
      if (Array.isArray(msg.recordings)) {
        state.recordings = msg.recordings;
        renderLists();
        syncEngineHomeSelection();
      } else if (window.qst) {
        qst.listRecordings();
      }
      if (window.ProMode && typeof window.ProMode.onDataChanged === "function") {
        window.ProMode.onDataChanged();
      }
      if (msg.converted != null || msg.skipped != null) {
        const n = msg.converted | 0;
        const sk = msg.skipped | 0;
        const txt =
          sk > 0 ? `已转换 ${n} 项，跳过 ${sk}` : `已转换 ${n} 项`;
        toast(msg.detail || txt);
      } else if (state._optSaveAs) {
        toast("已保存到新录制");
      } else {
        toast("优化已应用（尚未写入原录制）");
      }
      if (state._optSaveAs) {
        state._optSaveAs = false;
        closeAllOv();
        if (
          state.uiMode === "pro" &&
          window.ProMode &&
          typeof window.ProMode.switchPage === "function"
        ) {
          window.ProMode.switchPage("recorder");
        }
      } else if (msg.recording) {
        fillOptUi(msg.recording, { keepName: true });
      } else if (state.optPath && window.qst && $("#ov-opt")?.classList.contains("show")) {
        qst.loadOptimizeRecording(state.optPath);
      } else {
        closeAllOv();
      }
      return;
    }
    if (type === "restoreSettingsDefaults.result") {
      if (!msg.ok) {
        toast(msg.detail || "恢复默认失败");
        return;
      }
      fillSettings(msg.settings || {});
      toast("已恢复所有默认值");
      return;
    }
    if (type === "setRecorderMode.result") {
      if (!msg.ok) toast(msg.detail || "模式保存失败");
      else {
        state.recMode = msg.mode | 0;
        syncRecModeUi();
      }
      return;
    }
    if (type === "startRecord.result") {
      if (!msg.ok) {
        state.recording = false;
        toast(msg.detail || "无法开始录制");
        updateCtas();
        return;
      }
      state.recording = true;
      updateCtas();
      toast("录制已开始（再点按钮或热键结束）");
      return;
    }
    if (type === "stopRecord.result" || type === "recording.stopped") {
      state.recording = false;
      if (Array.isArray(msg.recordings)) {
        state.recordings = msg.recordings;
        renderLists();
      } else if (window.qst) {
        qst.listRecordings();
      }
      if (msg.path) selectPathInLists(msg.path, "recorder");
      else syncEngineHomeSelection();
      updateCtas();
      if (window.ProMode) window.ProMode.onDataChanged();
      if (!msg.ok && msg.detail) toast(msg.detail);
      else if (msg.path) toast("已保存录制（" + (msg.actionCount | 0) + " 项）");
      else if (type === "recording.stopped") toast("录制已停止");
      else toast("录制已停止");
      return;
    }
    if (type === "agentWindow.open") {
      const now = Date.now();
      if (state._agentOpenDebounce && now - state._agentOpenDebounce < 500) {
        markAgentShellReady();
        return;
      }
      state._agentOpenDebounce = now;
      const createNew = !!msg.createNew || !msg.id;
      if (createNew && AGENT_SHELL) {
        // 关窗再开时 JS 状态仍在：清掉空闲空标签，避免双「新对话」
        state.agentTabs = state.agentTabs.filter(
          (t) => t.busy || (t.messages && t.messages.length)
        );
        if (!state.agentTabs.some((t) => t.key === state.agentTabKey)) {
          state.agentTabKey = state.agentTabs[0] ? state.agentTabs[0].key : "";
        }
        renderAgentTabs();
      }
      openAgentSession(msg.id || "", createNew);
      markAgentShellReady();
      return;
    }
    if (type === "openAgentConversation.result") {
      if (!msg.ok) {
        const panelKey = msg.panelKey || "";
        if (panelKey) {
          const doomed = state.agentTabs.find((t) => t.key === panelKey);
          if (doomed && !(doomed.messages && doomed.messages.length)) {
            closeAgentTab(panelKey);
          }
        }
        if (state._chatLoading) {
          clearChatSkeleton();
          renderChat([]);
        }
        toast(msg.detail || "无法打开对话");
        markAgentShellReady();
        return;
      }
      const c = msg.conversation || {};
      const panelKey = msg.panelKey || "";
      let tab =
        (panelKey && state.agentTabs.find((t) => t.key === panelKey)) ||
        findAgentTabById(c.id) ||
        state.agentTabs.find((t) => !t.busy && !(t.messages && t.messages.length)) ||
        activeAgentTab();
      // 主壳也会收到该回包：无助手标签时不要在主壳再建一份
      if (!tab) {
        if (!AGENT_SHELL) return;
        const key = "at" + (++_agentTabSeq);
        tab = {
          key,
          id: c.id || "",
          name: c.name || "对话",
          messages: [],
          draft: "",
          attachments: [],
          model: c.modelName || state.agentModel || "",
          busy: false,
        };
        state.agentTabs.push(tab);
        state.agentTabKey = key;
      }
      tab.id = c.id || tab.id;
      tab.name = c.name || tab.name || "对话";
      tab.messages = Array.isArray(c.messages) ? c.messages : [];
      tab.draft = typeof c.draft === "string" ? c.draft : "";
      tab.attachments = Array.isArray(c.attachments)
        ? c.attachments.map((p) => ({ path: String(p) })).filter((x) => x.path)
        : [];
      tab._editIndex =
        Number.isInteger(c.editIndex) && c.editIndex >= 0 ? c.editIndex : -1;
      if (c.modelName) {
        tab.model = c.modelName;
        state.agentModel = c.modelName;
      }
      if (tab.key === state.agentTabKey) {
        syncAgentGlobalsFromTab(tab);
        renderChat(tab.messages);
        if ($("#chatInput")) $("#chatInput").textContent = tab.draft || "";
        renderAgentAttachments();
        renderAgentTabs();
        syncAgentSendBtn();
        syncAgentEditBar();
        if ($("#agentModelCombo") && tab.model) {
          $("#agentModelCombo").textContent = tab.model;
        }
        const title = $("#agentDlgTitle");
        if (title) title.textContent = "AI 脚本助手";
        markAgentShellReady();
      } else {
        renderAgentTabs();
      }
      return;
    }
    if (type === "agentConversation.title") {
      const id = String(msg.id || "");
      const name = String(msg.name || "").trim();
      if (!id || !name) return;
      const tab = findAgentTabById(id) || activeAgentTab();
      if (tab && (!tab.id || String(tab.id) === id)) {
        tab.id = id;
        tab.name = name;
        renderAgentTabs();
      }
      if (Array.isArray(msg.conversations)) {
        state.chats = msg.conversations;
        if (!AGENT_SHELL) renderLists();
      }
      return;
    }
    if (type === "sendAgentMessage.reasoningDelta") {
      appendAgentThinkingDelta(agentTabForStream(msg), msg.delta || "");
      return;
    }
    if (type === "sendAgentMessage.reasoning") {
      finalizeAgentThinking(agentTabForStream(msg), msg.reasoning || "");
      return;
    }
    if (type === "sendAgentMessage.delta") {
      const tab = agentTabForStream(msg);
      if (!tab) return;
      if (tab._thinkingBuf && !tab._thinkingFinalized && tab.key === state.agentTabKey) {
        finalizeAgentThinking(tab, tab._thinkingBuf);
      }
      if (tab.key !== state.agentTabKey) {
        // 非当前 Tab：累积到临时回复缓冲，result 时写入 messages
        tab._streamReply = (tab._streamReply || "") + (msg.delta || "");
        return;
      }
      clearAgentTransientStatus(tab);
      const log = $("#chatLog");
      if (!log) return;
      let bubble = log.querySelector(".bubble.ai.streaming");
      if (!bubble) {
        bubble = document.createElement("div");
        bubble.className = "bubble ai streaming";
        log.appendChild(bubble);
      }
      tab._streamReply = (tab._streamReply || "") + (msg.delta || "");
      // 流式阶段只更新纯文本节点，避免每个 token 全量跑 markdown/KaTeX（O(n²)）
      let plain = bubble.querySelector(".md-stream-plain");
      if (!plain) {
        plain = document.createElement("div");
        plain.className = "md-stream-plain";
        bubble.appendChild(plain);
      }
      plain.textContent = tab._streamReply;
      if (!tab._scrollPending) {
        tab._scrollPending = true;
        requestAnimationFrame(() => {
          tab._scrollPending = false;
          scrollChatSmart(log);
        });
      }
      return;
    }
    if (type === "sendAgentMessage.status") {
      appendAgentStatusLine(msg.status || "", agentTabForStream(msg));
      return;
    }
    if (type === "sendAgentMessage.tool") {
      const label = agentToolLabel(msg.name || "");
      let line = "调用工具: " + label;
      appendAgentStatusLine(line, agentTabForStream(msg), { permanent: true });
      return;
    }
    if (type === "sendAgentMessage.result") {
      const tab = agentTabForStream(msg) || activeAgentTab();
      if (tab) {
        clearAgentTransientStatus(tab);
        tab.busy = false;
        tab.id = msg.id || tab.id;
        if (msg.name) tab.name = msg.name;
        const reasoning = tab._lastReasoning || tab._thinkingBuf || "";
        tab._thinkingBuf = "";
        tab._thinkingFinalized = false;
        tab._streamReply = "";
        if (tab.key === state.agentTabKey) {
          state.agentId = tab.id || state.agentId;
          state.agentBusy = false;
          syncAgentSendBtn();
        } else {
          state.agentBusy = !!(activeAgentTab() && activeAgentTab().busy);
          syncAgentSendBtn();
          renderAgentTabs();
        }
        if (!msg.ok) {
          const cancelled =
            state._agentCancelRequested ||
            /已取消|cancelled/i.test(String(msg.detail || "")) ||
            /已取消|cancelled/i.test(String(msg.reply || ""));
          state._agentCancelRequested = false;
          toast(cancelled ? "已取消" : msg.detail || "发送失败");
          return;
        }
        if (tab.key === state.agentTabKey) {
          const log = $("#chatLog");
          const streaming = log?.querySelector(".bubble.ai.streaming");
          const shownHtml = renderMarkdownHtml(msg.reply || "");
          if (streaming) {
            streaming.classList.remove("streaming");
            streaming.innerHTML = shownHtml;
            typesetAiMarkdown(streaming);
          } else if (msg.reply) {
            if (log) {
              const div = document.createElement("div");
              div.className = "bubble ai";
              div.innerHTML = shownHtml;
              log.appendChild(div);
              typesetAiMarkdown(div);
              scrollChatSmart(log);
            }
          }
          tab.messages = tab.messages || [];
          tab.messages.push({
            role: "assistant",
            who: "AI助手",
            content: msg.reply || "",
            reasoning: reasoning || undefined,
          });
          tab._lastReasoning = "";
          renderAgentTabs();
          const title = $("#agentDlgTitle");
          if (title) title.textContent = "AI 脚本助手";
        } else if (msg.reply) {
          tab.messages = tab.messages || [];
          tab.messages.push({
            role: "assistant",
            who: "AI助手",
            content: msg.reply,
            reasoning: reasoning || undefined,
          });
          tab._lastReasoning = "";
          renderAgentTabs();
        }
      } else {
        state.agentBusy = false;
        syncAgentSendBtn();
        if (!msg.ok) {
          toast(msg.detail || "发送失败");
          return;
        }
      }
      // 主壳：仅静默同步列表，禁止 toast「已回复」（会配合 WebView 抢焦点跳回主界面）
      if (!AGENT_SHELL) {
        if (Array.isArray(msg.conversations)) state.chats = msg.conversations;
        if (Array.isArray(msg.scripts)) state.macros = msg.scripts;
        if (Array.isArray(msg.recordings)) state.recordings = msg.recordings;
        // 一次消息只重建一次列表（避免 3 次全量 innerHTML 重建造成卡顿）
        renderLists();
        syncEngineHomeSelection();
        return;
      }
      // 助手窗：不要 listRecordings / 刷主壳列表（会经 bridge 推主壳抢焦点）
      const prevPaths = new Set((state.macros || []).map((m) => itemPath(m)));
      if (Array.isArray(msg.conversations)) state.chats = msg.conversations;
      let created = [];
      if (Array.isArray(msg.scripts)) {
        state.macros = msg.scripts;
        created = msg.scripts.filter((s) => {
          const p = itemPath(s);
          return p && !prevPaths.has(p);
        });
      }
      if (Array.isArray(msg.recordings)) state.recordings = msg.recordings;
      if (created.length) {
        const s = created[0];
        const name = s.name || "新脚本";
        toast("已生成脚本：" + name);
      } else {
        const reply = String(msg.reply || "");
        const cancelled =
          state._agentCancelRequested ||
          /已取消|cancelled/i.test(reply) ||
          /已取消|cancelled/i.test(String(msg.detail || ""));
        state._agentCancelRequested = false;
        if (cancelled) toast("已取消");
      }
      return;
    }
    if (type === "cancelAgentMessage.result") {
      if (msg.ok) {
        state._agentCancelRequested = true;
        appendAgentStatusLine("正在取消…");
        syncAgentSendBtn();
        toast("已请求取消");
      } else {
        toast(msg.detail || "取消失败");
      }
      return;
    }
    if (type === "agentSaveDraft.result") {
      if (!msg.ok) toast(msg.detail || "草稿保存失败");
      return;
    }
    if (type === "listAgentChanges.result") {
      if (!msg.ok) {
        toast(msg.detail || "无法读取修改记录");
        return;
      }
      const changes = Array.isArray(msg.changes) ? msg.changes : [];
      if (!changes.length) {
        toast("暂无助手修改记录");
        return;
      }
      const btn = $("#btnAgentChanges");
      if (!btn) return;
      const items = changes.map((c) => ({
        t:
          (c.reverted ? "（已恢复）" : "恢复: ") +
          (c.title || c.tool || "修改"),
        d: (c.time || "") + " · " + (c.tool || "") + " · " + (c.path || ""),
        id: c.id,
        revertable: !c.reverted,
      }));
      showPopup(
        btn,
        items,
        (it) => {
          if (!it.revertable) {
            toast("该修改已恢复，不能重复恢复");
            return;
          }
          if (window.qst) qst.revertAgentChange(it.id);
        },
        { minWidth: 360, maxHeight: 340 }
      );
      return;
    }
    if (type === "revertAgentChange.result") {
      if (!msg.ok) {
        toast(msg.detail || "恢复失败");
        return;
      }
      if (Array.isArray(msg.scripts)) state.macros = msg.scripts;
      if (Array.isArray(msg.recordings)) state.recordings = msg.recordings;
      renderLists();
      if (window.ProMode) window.ProMode.onDataChanged();
      toast("已恢复该修改");
      return;
    }
    if (type === "pasteAgentClipboard.result") {
      if (!AGENT_SHELL && !($("#ov-agent")?.classList.contains("show") && !state.agentMinimized))
        return;
      if (!msg.ok) {
        toast(msg.detail || "剪贴板无附件");
        return;
      }
      const paths = Array.isArray(msg.paths) ? msg.paths : [];
      if (!paths.length) {
        toast("剪贴板无附件");
        return;
      }
      paths.forEach((p) => {
        if (p) addAgentAttachment(p);
      });
      toast("已添加 " + paths.length + " 个附件");
      return;
    }
    if (type === "saveClipboardImage.result") {
      if (!AGENT_SHELL && !($("#ov-agent")?.classList.contains("show") && !state.agentMinimized))
        return;
      if (!msg.ok) {
        state._pendingPastePreview = "";
        state.agentAttachments = (state.agentAttachments || []).filter((a) => !isTempClipboardAttach(a));
        renderAgentAttachments();
        toast(msg.detail || "保存图片失败");
        return;
      }
      if (msg.path) {
        const pending = state._pendingPastePreview || "";
        state._pendingPastePreview = "";
        // 用真实路径替换乐观预览，避免 clipboard-image.* 与 agent_clip_* 并存
        state.agentAttachments = (state.agentAttachments || []).filter((a) => !isTempClipboardAttach(a));
        addAgentAttachment(msg.path, pending || undefined);
        toast("已添加剪贴板图片");
      }
      return;
    }
    if (type === "saveImageAs.result") {
      if (!AGENT_SHELL && !($("#ov-agent")?.classList.contains("show") && !state.agentMinimized))
        return;
      if (!msg.ok) {
        if (msg.detail !== "cancelled") toast(msg.detail || "导出失败");
        return;
      }
      toast("图片已导出");
      return;
    }
    if (type === "deleteAgentConversation.result") {
      if (!msg.ok) {
        toast(msg.detail || "删除失败");
        return;
      }
      if (Array.isArray(msg.conversations)) state.chats = msg.conversations;
      renderLists();
      if (window.ProMode) window.ProMode.onDataChanged();
      toast("已删除对话");
      return;
    }
    if (type === "getGlobalHotkey.result" && msg.ok) {
      state.hotkeyVk = msg.hotkeyVk | 0;
      state.hotkeyModifiers = msg.hotkeyModifiers | 0;
      state.hotkeyHold = !!msg.hotkeyHold;
      state.hotkey = formatHotkeyDisplay(
        msg.hotkeyVk ? msg.hotkeyText || "F8" : "",
        state.hotkeyHold
      );
      if ($("#comboHotkey")) $("#comboHotkey").textContent = state.hotkey;
      updateCtas();
      if (window.ProMode && typeof window.ProMode.onDataChanged === "function") {
        window.ProMode.onDataChanged();
      }
      return;
    }
    if (type === "setGlobalHotkey.result") {
      if (!msg.ok) {
        toast(msg.detail || "热键设置失败");
        return;
      }
      state.hotkeyVk = msg.hotkeyVk | 0;
      if (msg.hotkeyModifiers != null) state.hotkeyModifiers = msg.hotkeyModifiers | 0;
      if (msg.hotkeyHold != null) state.hotkeyHold = !!msg.hotkeyHold;
      // 清除后 hotkeyText 为空：显示「无」，勿回退成旧值/F8
      state.hotkey = formatHotkeyDisplay(msg.hotkeyText || "", !!state.hotkeyHold);
      if ($("#comboHotkey")) $("#comboHotkey").textContent = state.hotkey;
      updateCtas();
      if (window.ProMode && typeof window.ProMode.onDataChanged === "function") {
        window.ProMode.onDataChanged();
      }
      toast(msg.hotkeyVk ? "启停热键已更新" : "启停热键已清除");
      return;
    }
    if (type === "installOcr.result") {
      state._ocrBusy = false;
      const bar = $("#ocrBar");
      const track = bar && bar.parentElement;
      if (track) track.classList.remove("indeterminate");
      if (bar) bar.style.width = msg.ok ? "100%" : "0%";
      const btn = $("#btnOcrInstall");
      const repairBtn = $("#btnOcrRepair");
      if (btn) {
        btn.disabled = false;
        btn.textContent = msg.ok ? "完成" : state._ocrRepair ? "修复/更新" : "安装插件";
      }
      if (repairBtn) repairBtn.disabled = false;
      if (msg.ok) {
        if ($("#ocrStatus")) $("#ocrStatus").textContent = msg.detail || "安装成功";
        toast("OCR 插件已安装");
      } else {
        if ($("#ocrStatus"))
          $("#ocrStatus").textContent = msg.detail || "安装失败，请重试";
        toast(msg.detail || "OCR 安装失败或已取消");
      }
      return;
    }
    if (type === "error" && msg.detail) toast(msg.detail);
  }

  function onListSelectPointerDown(e) {
    if (e.button !== 0) return;
    // 操作链路由 click 处理，避免 pointerdown+click 双触发
    if (e.target.closest("[data-act]")) return;
    const card = e.target.closest(".card");
    if (!card) return;
    const kind = card.dataset.kind;
    if (kind !== "macro" && kind !== "rec") return;
    const i = +card.dataset.i;
    if (kind === "macro") {
      state.macroSel = state.macroSel === i ? -1 : i;
      state.recSel = -1;
    } else {
      state.recSel = state.recSel === i ? -1 : i;
      state.macroSel = -1;
    }
    rememberSelectionPaths();
    updateListSelectionUi();
    updateCtas();
    // 引擎侧已轻量落盘；合并同步即可
    syncEngineHomeSelectionDebounced();
    state._listSelFromPointer = true;
  }

  function onListClick(e) {
    const card = e.target.closest(".card");
    if (!card) return;
    const kind = card.dataset.kind;
    const i = +card.dataset.i;
    const act = e.target.closest("[data-act]")?.dataset.act;
    const list =
      kind === "macro" ? state.macros : kind === "rec" ? state.recordings : state.chats;
    const item = list[i];
    if (act === "edit") {
      if (item) enterEditor(item.name, itemPath(item));
      return;
    }
    if (act === "hotkey") {
      if (item) openHotkeyFor(item);
      return;
    }
    if (act === "opt") {
      if (item && window.qst) {
        state._modeTransEndOn = "loadOpt";
        qst.openRecordingOptimize(itemPath(item), item.name || "");
      }
      return;
    }
    if (act === "chat") {
      if (item) openAgentSession(item.id || item.path, false);
      return;
    }
    if (act === "rename") {
      if (item) openRename(item, kind);
      return;
    }
    if (act === "del") {
      if (!item) return;
      if (kind === "ai") {
        askConfirm(`确定删除对话「${item.name || ""}」？`, () => {
          if (window.qst) qst.deleteAgentConversation(item.id || "");
        });
        return;
      }
      askConfirm(`确定删除「${item.name || ""}」？`, () => {
        if (window.qst) qst.deleteScript(itemPath(item));
      });
      return;
    }
    // AI 对话：点卡片即打开（无「选中」语义）
    if (kind === "ai") {
      if (item) openAgentSession(item.id || item.path, false);
      return;
    }
    // 宏/录制选中已在 pointerdown 完成
    if (state._listSelFromPointer) {
      state._listSelFromPointer = false;
      return;
    }
    if (kind === "macro") {
      state.macroSel = state.macroSel === i ? -1 : i;
      state.recSel = -1;
      rememberSelectionPaths();
    } else if (kind === "rec") {
      state.recSel = state.recSel === i ? -1 : i;
      state.macroSel = -1;
      rememberSelectionPaths();
    }
    updateListSelectionUi();
    updateCtas();
    if (kind === "macro" || kind === "rec") syncEngineHomeSelectionDebounced();
  }

  function syncAgentSendBtn() {
    const btn = $("#btnSend");
    if (!btn) return;
    btn.disabled = false;
    if (state.agentBusy) {
      btn.classList.add("cancel");
      btn.title = "取消";
      btn.setAttribute("aria-label", "取消");
      btn.innerHTML =
        '<svg viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><rect x="7" y="7" width="10" height="10" rx="1.5"/></svg>';
    } else {
      btn.classList.remove("cancel");
      btn.title = "发送";
      btn.setAttribute("aria-label", "发送");
      btn.innerHTML =
        '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 19V5M5 12l7-7 7 7"/></svg>';
    }
  }

  function getAgentConfigError() {
    const cfg = resolveActiveAiConfig();
    if (!cfg.enabled) return "请先在「设置 → AI助手」中启用 AI 脚本助手";
    if (!cfg.apiUrl) return "请先在「设置 → AI助手」中配置 API 地址";
    if (!cfg.apiKey) return "请先在「设置 → AI助手」中配置 API 密钥";
    if (!cfg.model) return "请先在「设置 → AI助手」中配置模型名称";
    return "";
  }

  function keyEventToHotkey(e) {
    const mods = [];
    let modifiers = 0;
    if (e.ctrlKey) {
      mods.push("Ctrl");
      modifiers |= 0x0002;
    }
    if (e.altKey) {
      mods.push("Alt");
      modifiers |= 0x0001;
    }
    if (e.shiftKey) {
      mods.push("Shift");
      modifiers |= 0x0004;
    }
    if (e.metaKey) {
      mods.push("Win");
      modifiers |= 0x0008;
    }
    let name = e.key;
    if (name === " ") name = "Space";
    if (name.length === 1) name = name.toUpperCase();
    if (/^F\d+$/i.test(name)) name = name.toUpperCase();
    const text = mods.length ? mods.join("+") + "+" + name : name;
    return { text, vk: e.keyCode || e.which || 0, modifiers, hold: false };
  }

  function markDriverStepsDone(kind) {
    const root =
      kind === "vhid" || kind === "virtualHid" ? $("#vhidSteps") : $("#icSteps");
    const status =
      kind === "vhid" || kind === "virtualHid" ? $("#vhidStatus") : $("#icStatus");
    const bar = kind === "vhid" || kind === "virtualHid" ? $("#vhidBar") : $("#icBar");
    if (root) $$(".driver-step", root).forEach((s) => s.classList.add("done"));
    if (status) status.textContent = "安装流程已启动（需管理员确认）";
    if (bar) bar.style.width = "100%";
  }

  const FREQ_LABELS = ["每小时", "每日", "每周", "自定义", "间隔"];
  function stripSchedFileLabel(name) {
    return String(name || "").replace(/\.json$/i, "");
  }
  function syncSchedFreqUi(freq) {
    const hourWrap = $("#schedHourWrap");
    if (hourWrap) hourWrap.style.display = freq === 0 ? "none" : "contents";
    const dateRow = $("#schedDateRow");
    if (dateRow) dateRow.style.display = freq === 3 ? "" : "none";
    $$("#ov-sched-create [data-wd]").forEach((el) => {
      el.style.display = freq === 2 ? "" : "none";
    });
  }
  function renderSchedTable(data) {
    state.schedTasks = Array.isArray(data.tasks) ? data.tasks : [];
    state.schedGlobalDisabled = !!data.globalDisabled;
    const g = $("#schedGlobalDisable");
    if (g) setChk(g, state.schedGlobalDisabled);
    const table = $("#schedTable");
    if (!table) return;
    const head =
      '<div class="th"><span>任务名称</span><span>类型</span><span>文件</span><span>频率</span><span>时间</span><span>状态</span><span>操作</span></div>';
    const rows = state.schedTasks
      .map((t) => {
        const kind = (t.kind | 0) === 0 ? "键鼠录制" : "鼠标宏";
        const freq = FREQ_LABELS[t.frequency | 0] || "自定义";
        const st = (t.status | 0) === 0 ? "启用" : "禁用";
        return `<div class="tr" data-id="${esc(t.id || "")}">
          <span>${esc(t.name || "")}</span><span>${kind}</span>
          <span>${esc(stripSchedFileLabel(t.fileDisplayName || t.filePath || ""))}</span>
          <span>${freq}</span><span>${esc(t.timeLabel || "")}</span>
          <span class="act" data-sched="status" title="点击切换启用/禁用">${st}</span>
          <span class="act"><span data-sched="edit">编辑</span> <span data-sched="del">删除</span></span>
        </div>`;
      })
      .join("");
    table.innerHTML = head + (rows || '<div class="tr"><span colspan="7">暂无定时任务</span></div>');
  }

  function openSchedEditor(task) {
    state.schedEditId = task ? task.id || "" : "";
    // 打开创建/编辑前补齐脚本列表，否则「选择文件」为空导致保存失败
    if (window.qst) {
      if (!(state.macros && state.macros.length)) qst.listScripts();
      if (!(state.recordings && state.recordings.length)) qst.listRecordings();
    }
    const title = $("#schedCreateTitle");
    if (title)
      title.childNodes[0].textContent = task ? "编辑定时任务 " : "定时任务创建 ";
    if ($("#schedName")) $("#schedName").textContent = (task && task.name) || "";
    const kind = task ? task.kind | 0 : 1;
    $$("#schedKindRow .radio").forEach((r) =>
      r.classList.toggle("on", (r.dataset.kind | 0) === kind)
    );
    const freq = task ? task.frequency | 0 : 1;
    $$("#schedFreqRow .radio").forEach((r) =>
      r.classList.toggle("on", (r.dataset.freq | 0) === freq)
    );
    const status = task ? task.status | 0 : 0;
    $$("#schedStatusRow .radio").forEach((r) =>
      r.classList.toggle("on", (r.dataset.status | 0) === status)
    );
    if ($("#schedHour")) $("#schedHour").textContent = String((task && task.hour) ?? 8);
    if ($("#schedMinute"))
      $("#schedMinute").textContent = String((task && task.minute) ?? 0);
    if ($("#schedSecond"))
      $("#schedSecond").textContent = String((task && task.second) ?? 0);
    if ($("#schedMs"))
      $("#schedMs").textContent = String((task && task.millisecond) ?? 0);
    if ($("#schedYear")) $("#schedYear").textContent = String((task && task.year) || "");
    if ($("#schedMonth")) $("#schedMonth").textContent = String((task && task.month) || "");
    if ($("#schedDay")) $("#schedDay").textContent = String((task && task.day) || "");
    const wd = task ? task.weekDays | 0 : 31;
    $$("#ov-sched-create [data-wd]").forEach((c) => {
      const bit = 1 << (c.dataset.wd | 0);
      setChk(c, (wd & bit) !== 0);
    });
    const fileLabel =
      (task && stripSchedFileLabel(task.fileDisplayName || task.filePath)) || "选择文件…";
    if ($("#schedFile")) {
      $("#schedFile").textContent = fileLabel;
      $("#schedFile").dataset.path = (task && task.filePath) || "";
    }
    syncSchedFreqUi(freq);
    openOv("sched-create");
  }

  function toggleSchedTaskStatus(task) {
    if (!task || !task.id || !window.qst) return;
    if (state.schedStatusBusy) return;
    const next = (task.status | 0) === 0 ? 1 : 0;
    state.schedStatusBusy = true;
    qst.saveScheduledTask({
      update: 1,
      statusOnly: 1,
      id: task.id,
      status: next,
    });
  }

  function collectSchedForm() {
    const kindEl = $("#schedKindRow .radio.on");
    const freqEl = $("#schedFreqRow .radio.on");
    const statusEl = $("#schedStatusRow .radio.on");
    let weekDays = 0;
    $$("#ov-sched-create [data-wd]").forEach((c) => {
      if (c.classList.contains("on")) weekDays |= 1 << (c.dataset.wd | 0);
    });
    return {
      update: state.schedEditId ? 1 : 0,
      id: state.schedEditId || "",
      name: ($("#schedName")?.textContent || "").trim(),
      kind: kindEl ? kindEl.dataset.kind | 0 : 1,
      frequency: freqEl ? freqEl.dataset.freq | 0 : 1,
      status: statusEl ? statusEl.dataset.status | 0 : 0,
      filePath: ($("#schedFile")?.dataset.path || "").trim(),
      hour: parseInt($("#schedHour")?.textContent || "8", 10) || 0,
      minute: parseInt($("#schedMinute")?.textContent || "0", 10) || 0,
      second: Math.max(0, Math.min(59, parseInt($("#schedSecond")?.textContent || "0", 10) || 0)),
      millisecond: Math.max(0, Math.min(999, parseInt($("#schedMs")?.textContent || "0", 10) || 0)),
      year: parseInt($("#schedYear")?.textContent || "0", 10) || 0,
      month: parseInt($("#schedMonth")?.textContent || "0", 10) || 0,
      day: parseInt($("#schedDay")?.textContent || "0", 10) || 0,
      weekDays,
    };
  }

  function syncOptSelCount() {
    const n = (state.optSelected || []).length;
    const total = (state.optActions || []).length;
    if ($("#optSelCount")) $("#optSelCount").textContent = `${total} · 已选 ${n}`;
    if ($("#optSelN")) $("#optSelN").textContent = String(n);
  }

  /** 对齐原生 RecordingOptimizeDialog::ActionDisplayText */
  function optActionDisplayName(a) {
    if (!a) return "动作";
    const t = a.type || "";
    if (t === "wait") {
      const sec = Number(a.duration) || 0;
      const ms = Math.round(sec * 1000);
      return "等待:" + f3(sec) + "秒(" + ms + "毫秒)";
    }
    if (t === "moveMouse") {
      return "鼠标移动到位置(" + (a.x | 0) + "," + (a.y | 0) + ")";
    }
    if (t === "moveMouseRelative") {
      return "相对移动鼠标(" + (a.x | 0) + "," + (a.y | 0) + ")";
    }
    if (t === "keyDown") return "键盘按下" + (a.keyText || "");
    if (t === "keyUp") return "键盘抬起" + (a.keyText || "");
    if (t === "mouseDown") return buttonText(a.button) + "按下";
    if (t === "mouseUp") return buttonText(a.button) + "抬起";
    if (t === "scrollWheel") return "滚轮" + (a.scrollSteps | 0) + "步";
    return displayActionName(a);
  }

  /** 录制优化：关键操作 = 除绝对/相对移动与等待外 */
  function isOptKeyOperation(a) {
    if (!a) return false;
    const t = a.type || "";
    return t !== "moveMouse" && t !== "moveMouseRelative" && t !== "wait";
  }

  function isOptMoveOrWait(a) {
    if (!a) return false;
    const t = a.type || "";
    return t === "moveMouse" || t === "moveMouseRelative" || t === "wait";
  }

  const OPT_MOVE_WAIT_CONTIG_MERGE =
    "鼠标移动合并时只能选择连续的移动和等待操作，其中不允许夹杂其他操作。";
  const OPT_MOVE_WAIT_CONTIG_COMPRESS =
    "鼠标移动压缩时只能选择连续的移动和等待操作，其中不允许夹杂其他操作。";

  /** 与后端 recopt::CollectMoveWaitRanges 对齐 */
  function collectOptMoveWaitRanges(actions, selected) {
    const acts = actions || [];
    const sel = (selected || [])
      .filter((i) => i >= 0 && i < acts.length)
      .sort((a, b) => a - b);
    if (!sel.length) return { ok: false, ranges: [] };
    const hasKey = sel.some((i) => isOptKeyOperation(acts[i]));
    if (!hasKey) {
      for (let k = 1; k < sel.length; k++) {
        if (sel[k] !== sel[k - 1] + 1) {
          return {
            ok: false,
            ranges: [],
            errMerge: OPT_MOVE_WAIT_CONTIG_MERGE,
            errCompress: OPT_MOVE_WAIT_CONTIG_COMPRESS,
          };
        }
      }
      for (const i of sel) {
        if (!isOptMoveOrWait(acts[i])) {
          return {
            ok: false,
            ranges: [],
            errMerge: OPT_MOVE_WAIT_CONTIG_MERGE,
            errCompress: OPT_MOVE_WAIT_CONTIG_COMPRESS,
          };
        }
      }
      return { ok: true, ranges: [[sel[0], sel[sel.length - 1]]] };
    }
    const ranges = [];
    let runFirst = -1;
    let runLast = -1;
    const flush = () => {
      if (runFirst >= 0) ranges.push([runFirst, runLast]);
      runFirst = -1;
      runLast = -1;
    };
    for (const i of sel) {
      if (isOptKeyOperation(acts[i])) {
        flush();
        continue;
      }
      if (runFirst < 0) {
        runFirst = runLast = i;
      } else if (i === runLast + 1) {
        runLast = i;
      } else {
        flush();
        runFirst = runLast = i;
      }
    }
    flush();
    return { ok: true, ranges };
  }

  function optRangeHasRelative(actions, lo, hi) {
    for (let i = lo; i <= hi; i++) {
      if (actions[i] && actions[i].type === "moveMouseRelative") return true;
    }
    return false;
  }

  function optRangeAbsMoveCount(actions, lo, hi) {
    let n = 0;
    for (let i = lo; i <= hi; i++) {
      if (actions[i] && actions[i].type === "moveMouse") n++;
    }
    return n;
  }

  function validateOptMerge(actions, selected) {
    if (!selected.length) return "请先选择要合并的移动/等待。";
    const col = collectOptMoveWaitRanges(actions, selected);
    if (!col.ok) return col.errMerge || OPT_MOVE_WAIT_CONTIG_MERGE;
    let applied = 0;
    let skipRel = 0;
    for (const [lo, hi] of col.ranges) {
      if (optRangeHasRelative(actions, lo, hi)) {
        skipRel++;
        continue;
      }
      if (optRangeAbsMoveCount(actions, lo, hi) < 1) continue;
      applied++;
    }
    if (applied) return "";
    if (skipRel) return "相对移动轨迹受保护，不能对 FPS 相对位移使用绝对路径合并。";
    return "选中范围内没有鼠标移动动作。";
  }

  function validateOptCompress(actions, selected) {
    if (!selected.length) return "请先选择要压缩的移动/等待。";
    const col = collectOptMoveWaitRanges(actions, selected);
    if (!col.ok) return col.errCompress || OPT_MOVE_WAIT_CONTIG_COMPRESS;
    let applied = 0;
    let skipRel = 0;
    for (const [lo, hi] of col.ranges) {
      if (optRangeHasRelative(actions, lo, hi)) {
        skipRel++;
        continue;
      }
      if (optRangeAbsMoveCount(actions, lo, hi) < 2) continue;
      applied++;
    }
    if (applied) return "";
    if (skipRel) return "相对移动轨迹受保护，不能对 FPS 相对位移使用绝对路径压缩。";
    return "选中范围内至少需要两个鼠标移动点。";
  }

  /** 只改勾选/高亮 class，避免整表 innerHTML 重绘闪烁 */
  function syncOptListSelectionUi() {
    const list = $("#optList");
    if (!list) return;
    const selSet = new Set(state.optSelected || []);
    list.querySelectorAll(".arow").forEach((row) => {
      const i = row.dataset.i | 0;
      const on = selSet.has(i);
      const hi = state.optHighlight === i;
      row.classList.toggle("checked", on);
      row.classList.toggle("focus", hi);
      const chk = row.querySelector(":scope > .chk");
      if (chk) {
        chk.classList.toggle("on", on);
        const mark = chk.querySelector("i");
        if (mark) mark.textContent = "";
      }
      const kids = row.querySelectorAll(":scope > span");
      // chk / 序号 / 动作名 / 备注
      if (kids[1]) kids[1].classList.toggle("muted", !on);
      if (kids[3]) kids[3].classList.toggle("muted", !on);
    });
    syncOptSelCount();
    if (state.optHighlight >= 0) {
      const hiRow = list.querySelector(`.arow[data-i="${state.optHighlight}"]`);
      hiRow?.scrollIntoView({ block: "nearest" });
    }
  }

  function renderOptListRows(opts) {
    const list = $("#optList");
    if (!list) return;
    if (!Array.isArray(state.optSelected)) state.optSelected = [];
    const selSet = new Set(state.optSelected);
    const actions = state.optActions || [];
    const progressive =
      !!opts?.progressive || document.body.classList.contains("progressive-opt");
    const rows = actions.map((a, i) => {
      const label = optActionDisplayName(a);
      const on = selSet.has(i);
      const hi = state.optHighlight === i;
      const keyOp = isOptKeyOperation(a);
      const tone = keyOp ? "" : i % 2 === 0 ? "tone-a" : "tone-b";
      const cls = [
        "arow",
        on ? "checked" : "",
        hi ? "focus" : "",
        keyOp ? "key-op" : "",
        tone,
        progressive ? "reveal-in" : "",
      ]
        .filter(Boolean)
        .join(" ");
      const delay = progressive ? ` style="animation-delay:${Math.min(i, 40) * 10}ms"` : "";
      return `<div class="${cls}" data-i="${i}"${delay}>
          <span class="chk ${on ? "on" : ""}" data-opt-sel="${i}"><i>${on ? "✓" : ""}</i></span>
          <span class="${on ? "" : "muted"}">${String(i + 1)}</span>
          <span class="opt-aname">${esc(label)}</span>
          <span class="${on ? "" : "muted"}">${esc(a.remark || "")}</span>
        </div>`;
    });
    const finish = () => {
      syncOptSelCount();
      if (state.optHighlight >= 0) {
        const hiRow = list.querySelector(`.arow[data-i="${state.optHighlight}"]`);
        hiRow?.scrollIntoView({ block: "nearest" });
      }
      if (opts && typeof opts.onDone === "function") opts.onDone();
    };
    const CHUNK = 48;
    if (!progressive || rows.length <= CHUNK) {
      list.innerHTML = rows.join("");
      finish();
      return;
    }
    list.innerHTML = "";
    // 先揭开模糊，再分批填入动作行（时长跟数据量/帧率走）
    endProgressiveLoad();
    const chunks = [];
    for (let i = 0; i < rows.length; i += CHUNK) {
      chunks.push(rows.slice(i, i + CHUNK).join(""));
    }
    appendActionRowsChunked(list, chunks, finish);
  }

  function fillOptUi(rec, opts) {
    state.optActions = Array.isArray(rec.actions) ? rec.actions : [];
    state.optPath = rec.path || state.optPath || "";
    state.optSelected = [];
    state.optHighlight = -1;
    if (!opts?.keepName && $("#optName")) $("#optName").textContent = rec.name || "";
    const dur = Number(rec.durationSeconds);
    const durTxt = Number.isFinite(dur) ? dur.toFixed(2) + "s" : "—";
    if ($("#optDurNow")) $("#optDurNow").textContent = durTxt;
    if ($("#optDurOrig")) {
      if (state._optOrigDur == null && Number.isFinite(dur)) state._optOrigDur = dur;
      const od = state._optOrigDur;
      $("#optDurOrig").textContent = Number.isFinite(od) ? od.toFixed(2) + "s" : durTxt;
    }
    // 壳层字段已填好；动作列表分批出现
    renderOptListRows({
      progressive: !!(opts && opts.progressive),
      onDone: opts && opts.onDone,
    });
  }

  function collectOptSelected() {
    return Array.isArray(state.optSelected) ? state.optSelected.slice() : [];
  }

  const OPT_SCHEMES = [
    { t: "批量删除", v: 0 },
    { t: "等待时间调整", v: 1 },
    { t: "鼠标移动合并", v: 2 },
    { t: "鼠标移动压缩", v: 3 },
  ];

  /** 切换优化方案面板；勿在 popup 回调里用已失效的 event.currentTarget */
  function setOptScheme(scheme) {
    const idx =
      typeof scheme === "number"
        ? scheme | 0
        : (OPT_SCHEMES.find((s) => s.t === scheme || s.v === scheme) || OPT_SCHEMES[0]).v;
    const item = OPT_SCHEMES.find((s) => s.v === idx) || OPT_SCHEMES[0];
    state.optScheme = item.v;
    const combo = $("#optScheme");
    if (combo) combo.textContent = item.t;
    const panes = $$("#ov-opt .opt-scheme-pane");
    panes.forEach((p) => {
      const on = p.dataset.scheme === item.t;
      p.classList.toggle("active", on);
      // 双保险：避免仅依赖 class 时被其它样式盖住
      p.style.display = on ? "flex" : "none";
    });
  }

  function applyOptimizePayload(extra) {
    if (!window.qst || !state.optPath) return;
    const payload = Object.assign(
      {
        path: state.optPath,
        scheme: state.optScheme | 0,
        selected: collectOptSelected(),
      },
      extra || {}
    );
    qst.applyOptimizeRecording(payload);
  }

  // 聊天框富文本粘贴的严格清洗：只保留排版标签与安全链接，剥离一切
  // 事件属性 / style / script / iframe / 媒体标签，防止粘贴 HTML 触发 XSS（页面带全量 bridge 权限）。
  function sanitizePastedHtml(html) {
    try {
      const doc = new DOMParser().parseFromString(String(html || ""), "text/html");
      const allowed = new Set([
        "B", "I", "U", "STRONG", "EM", "CODE", "PRE", "BR", "P",
        "UL", "OL", "LI", "BLOCKQUOTE", "SPAN", "DIV", "H1", "H2", "H3", "H4", "H5", "H6", "HR",
      ]);
      const drop = new Set([
        "SCRIPT", "STYLE", "IFRAME", "OBJECT", "EMBED", "LINK", "META", "FORM",
        "INPUT", "BUTTON", "SELECT", "TEXTAREA", "IMG", "SVG", "VIDEO", "AUDIO",
        "SOURCE", "CANVAS", "FRAME", "NOSCRIPT",
      ]);
      function clean(node) {
        for (let i = node.children.length - 1; i >= 0; --i) {
          const el = node.children[i];
          const tag = el.tagName;
          if (drop.has(tag)) {
            // 脚本/样式直接删；其余换成纯文本，避免残留可执行结构
            if (tag === "SCRIPT" || tag === "STYLE" || tag === "NOSCRIPT") {
              el.remove();
            } else {
              el.replaceWith(document.createTextNode(el.textContent || ""));
            }
            continue;
          }
          if (!allowed.has(tag)) {
            el.replaceWith(...Array.from(el.childNodes));
            continue;
          }
          for (const attr of Array.from(el.attributes)) {
            if (el.tagName === "A" && attr.name === "href") {
              const href = attr.value.trim().toLowerCase();
              if (/^(https?:|mailto:)/.test(href)) continue;
            }
            el.removeAttribute(attr.name);
          }
          clean(el);
        }
      }
      clean(doc.body);
      return doc.body.innerHTML;
    } catch (_) {
      // 解析失败退化为纯文本（不保留结构）
      return String(html || "")
        .replace(/<[^>]*>/g, "")
        .replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    }
  }

  function installPlainTextPaste() {
    document.addEventListener("paste", (e) => {
      const t = e.target;
      const el =
        t && t.nodeType === 1
          ? t.closest("[contenteditable=true]")
          : t && t.parentElement
            ? t.parentElement.closest("[contenteditable=true]")
            : null;
      if (!el) return;
      // 聊天框有独立粘贴（图片等），不改
      if (el.id === "chatInput" || el.classList.contains("chat-box")) return;
      if (!el.classList.contains("inp") && el.id !== "renameInput") return;
      const cd = e.clipboardData;
      if (!cd) return;
      e.preventDefault();
      let plain = cd.getData("text/plain") || "";
      if (!el.classList.contains("ed-area")) {
        plain = plain.replace(/\r\n|\r|\n/g, " ");
      }
      if (document.queryCommandSupported && document.queryCommandSupported("insertText")) {
        document.execCommand("insertText", false, plain);
      } else {
        const sel = window.getSelection();
        if (!sel || !sel.rangeCount) return;
        sel.deleteFromDocument();
        const node = document.createTextNode(plain);
        const range = sel.getRangeAt(0);
        range.insertNode(node);
        range.setStartAfter(node);
        range.collapse(true);
        sel.removeAllRanges();
        sel.addRange(range);
      }
    });
    // 失焦时若仍残留富文本标签，压成纯文本（保留 ed-area 换行）
    document.addEventListener(
      "focusout",
      (e) => {
        const el = e.target?.closest?.(".inp[contenteditable=true], #renameInput[contenteditable=true]");
        if (!el || el.id === "chatInput") return;
        const html = el.innerHTML || "";
        if (!/<(?!br\s*\/?)[^>]+>/i.test(html) && !/style\s*=/i.test(html)) return;
        if (el.classList.contains("ed-area")) {
          el.textContent = (el.innerText || el.textContent || "").replace(/\r\n/g, "\n");
        } else {
          el.textContent = (el.textContent || "").replace(/\s+/g, " ").trim();
        }
      },
      true
    );
    // 空单行框聚焦时清掉浏览器插入的 <br>/<div>，保证光标落在输入行中间
    document.addEventListener(
      "focusin",
      (e) => {
        const el = e.target?.closest?.(".inp[contenteditable=true]:not(.ed-area), #renameInput[contenteditable=true]");
        if (!el) return;
        const text = (el.textContent || "").replace(/\u200b/g, "");
        if (text.length) return;
        if (el.innerHTML && el.innerHTML !== "") {
          el.innerHTML = "";
        }
      },
      true
    );
  }

  function init() {
    window.addEventListener("qst:bridge", onBridge);
    window.addEventListener("qst:open-settings", () => {
      if (state.editor) openEditorSettings();
      else openOv("settings");
    });
    window.addEventListener("qst:exit-editor", () => exitEditor());
    installPlainTextPaste();
    bindTypingHotkeyMute();
    bindEditorActionShortcuts();
    // 极简 / 专业双模式：默认极简；切换仅在设置「保存」后生效
    if (window.ProMode) state.uiMode = window.ProMode.mode();
    applyUiMode({ center: true });
    $$("#uiModeRadios .radio").forEach((r) => {
      r.addEventListener("click", () => {
        const m = r.dataset.mode === "pro" ? "pro" : "simple";
        $$("#uiModeRadios .radio").forEach((x) =>
          x.classList.toggle("on", x.dataset.mode === m)
        );
      });
    });
    // 独立助手窗：骨架（标题/输入）可见即入场，与会话加载并行
    if (AGENT_SHELL) {
      openOv("agent");
      requestAnimationFrame(() => markAgentShellReady());
      setTimeout(() => markAgentShellReady(), 800);
    } else {
      // 主壳：首帧 navy 底 → 界面轻量入场，不挡数据加载
      requestAnimationFrame(() => markShellReady());
      setTimeout(() => markShellReady(), 600);
    }
    bindDynamic();
    syncTitleBar();

    const typeCombo = $("#edActionType");
    if (typeCombo) {
      syncEditorActionTypeCombo();
    }

    $$("#homeNav .tab").forEach((t) =>
      t.addEventListener("click", () => setTab(t.dataset.tab))
    );

    $$("#settingsTabs .st-tab").forEach((t) =>
      t.addEventListener("click", () => setSettingsTab(t.dataset.stab))
    );
    $$("#editorSettingsTabs .st-tab").forEach((t) =>
      t.addEventListener("click", () => setEditorSettingsTab(t.dataset.edtab))
    );
    bindEditorActionCatalog();

    document.addEventListener("click", (e) => {
      const open = e.target.closest("[data-open]");
      if (open) {
        e.preventDefault();
        if (open.dataset.open === "hotkey-global") {
          openGlobalHotkeyCapture();
          return;
        }
        if (open.dataset.open === "sched") {
          openOv("sched");
          if (window.qst) qst.listScheduledTasks();
          return;
        }
        if (open.dataset.open === "driver-vhid") {
          openOv("driver-vhid");
          refreshVhidStatus("dialog");
          return;
        }
        if (open.dataset.open === "opt") return;
        if (open.dataset.open === "theme") {
          syncThemeColorInputs();
          openOv("theme");
          return;
        }
        if (open.dataset.open === "pick") {
          openOv("pick");
          return;
        }
        openOv(open.dataset.open, open.dataset.stab);
        return;
      }
      if (e.target.closest("[data-close]")) {
        const inProSettings = e.target.closest("#proSettingsHost");
        closeAllOv({ keepAgent: true });
        if (inProSettings && window.ProMode && typeof window.ProMode.switchPage === "function") {
          window.ProMode.switchPage("library");
        }
        return;
      }
    });

    $$("#clickBtns .radio").forEach((r, idx) => {
      r.addEventListener("click", () => {
        state.clickBtn = idx;
        $$("#clickBtns .radio").forEach((x) => x.classList.remove("on"));
        r.classList.add("on");
        updateCtas();
        quietSaveSettings({
          clickerButton: state.clickBtn,
          clickerIntervalMode: state.intervalMode,
          clickerCustomInterval: state.customInterval,
        });
      });
    });

    $("#comboInterval")?.addEventListener("click", (e) => {
      e.stopPropagation();
      showPopup(e.currentTarget, INTERVALS, (it) => {
        if (it.custom) {
          openOv("interval");
          return;
        }
        state.intervalMode = it.mode;
        state.intervalLabel = it.t;
        $("#comboInterval").textContent = it.t;
        updateCtas();
        quietSaveSettings({
          clickerButton: state.clickBtn,
          clickerIntervalMode: state.intervalMode,
          clickerCustomInterval: state.customInterval,
        });
      });
    });

    $("#comboHotkey")?.addEventListener("click", (e) => {
      e.stopPropagation();
      const items = [
          { t: "自定义", d: "将您指定的按键设为启停热键", v: "custom" },
          { t: "鼠标左键", d: "按住左键开始连点，松开停止", v: "left" },
          { t: "鼠标中键", d: "将点击中键设为启停热键", v: "middle" },
          { t: "鼠标右键", d: "将点击右键设为启停热键", v: "right" },
          { t: "鼠标侧键1", d: "将侧键1设为启停热键", v: "x1" },
          { t: "鼠标侧键2", d: "将侧键2设为启停热键", v: "x2" },
          { t: "空格键", d: "将空格键设为启停热键", v: "space" },
        ];
      const cur = ($("#comboHotkey")?.textContent || "").trim();
      const selectedIndex = Math.max(
        0,
        items.findIndex((it) => cur.includes(it.t) || (it.v === "left" && cur.includes("左键")))
      );
      showPopup(
        e.currentTarget,
        items,
        (it) => {
          if (it.v === "custom") {
            openGlobalHotkeyCapture();
            return;
          }
          const map = {
            left: { text: "鼠标左键", vk: 0x01, hold: 1 },
            middle: { text: "鼠标中键", vk: 0x04, hold: 0 },
            right: { text: "鼠标右键", vk: 0x02, hold: 0 },
            x1: { text: "鼠标侧键1", vk: 0x05, hold: 0 },
            x2: { text: "鼠标侧键2", vk: 0x06, hold: 0 },
            space: { text: "空格键", vk: 0x20, hold: 0 },
          };
          const m = map[it.v];
          if (!m || !window.qst) return;
          state.hotkeyHold = !!m.hold;
          state.hotkey = formatHotkeyDisplay(m.text, m.hold);
          $("#comboHotkey").textContent = state.hotkey;
          updateCtas();
          qst.setGlobalHotkey({
            hotkeyText: m.text,
            hotkeyVk: m.vk,
            hotkeyModifiers: 0,
            hotkeyHold: m.hold ? 1 : 0,
          });
        },
        { preferUp: true, selectedIndex, minRows: items.length }
      );
    });

    $("#edMode")?.addEventListener("click", (e) => {
      e.stopPropagation();
      showPopup(e.currentTarget, ED_MODES, (it) => {
        state.editorMode = it.v;
        syncEditorModeUi();
      });
    });
    $("#edWmMethod")?.addEventListener("click", (e) => {
      e.stopPropagation();
      const el = e.currentTarget;
      const cur = normalizeWmSelectMethod(
        (state.windowMode && state.windowMode.selectMethod) || ""
      );
      const selectedIndex = Math.max(
        0,
        WM_METHODS.findIndex((m) => m.v === cur)
      );
      showPopup(
        el,
        WM_METHODS,
        (it) => {
          state.windowMode = state.windowMode || {};
          state.windowMode.selectMethod = it.v;
          if (el) el.textContent = it.t;
          syncEditorModeUi();
        },
        { selectedIndex, minWidth: 260 }
      );
    });
    $("#edBrowseBtn")?.addEventListener("click", () => {
      state._pendingBrowse = "wmTarget";
      if (window.qst) qst.browsePath(true);
      else toast("无桥接");
    });

    $("#agentModelCombo")?.addEventListener("click", (e) => {
      e.stopPropagation();
      const el = e.currentTarget;
      const items = state._aiModels || [];
      if (!items.length) {
        toast("请先在「设置 → AI助手」中添加模型");
        return;
      }
      showPopup(el, items, (it) => {
        state.agentModel = it.v || it.t;
        if (el) el.textContent = it.t;
        if ($("#aiModelCombo")) $("#aiModelCombo").textContent = it.t;
        const p = it.profile || findAiSavedProfile(it.v);
        if (p) applyAiProfileToForm(p);
        else if ($("#aiModelName")) $("#aiModelName").textContent = state.agentModel;
        const tab = activeAgentTab();
        if (tab) tab.model = state.agentModel;
        quietSaveSettings(collectSettings());
      });
    });
    $("#aiModelCombo")?.addEventListener("click", (e) => {
      e.stopPropagation();
      const el = e.currentTarget;
      const items = state._aiModels || [];
      if (!items.length) {
        toast("请先添加模型");
        return;
      }
      showPopup(el, items, (it) => {
        state.agentModel = it.v || it.t;
        if (el) el.textContent = it.t;
        if ($("#agentModelCombo")) $("#agentModelCombo").textContent = it.t;
        const p = it.profile || findAiSavedProfile(it.v);
        if (p) applyAiProfileToForm(p);
        else if ($("#aiModelName")) $("#aiModelName").textContent = state.agentModel;
        quietSaveSettings(collectSettings());
      });
    });

    $("#recMode")?.addEventListener("click", (e) => {
      e.stopPropagation();
      showPopup(e.currentTarget, REC_MODES, (it) => {
        state.recMode = it.mode;
        syncRecModeUi();
        if (window.qst) qst.setRecorderMode(it.mode);
      });
    });

    $("#recWindowMode")?.addEventListener("click", (e) => {
      e.stopPropagation();
      showPopup(
        e.currentTarget,
        [
          { t: "全屏模式", v: 0 },
          { t: "窗口模式", v: 1 },
        ],
        (it) => {
          state.recWindowMode = it.v | 0;
          syncRecModeUi();
          quietSaveSettings(collectSettings());
        }
      );
    });

    $("#setWmEnableInject")?.addEventListener("click", () => {
      // bindDynamic 已 toggle；下一帧同步禁用态（保证 class 已更新）
      queueMicrotask(() => {
        syncWmInjectUi();
      });
    });

    $("#setWmInjectTech")?.addEventListener("click", (e) => {
      e.stopPropagation();
      if (!$("#setWmEnableInject")?.classList.contains("on")) return;
      showPopup(e.currentTarget, WM_TECH_LIST, (it) => {
        state._wmInjectTech = it.v | 0;
        const el = $("#setWmInjectTech");
        if (el) el.textContent = it.t;
        quietSaveSettings(collectSettings());
      });
    });

    $("#btnIntervalOk")?.addEventListener("click", () => {
      const inp = $("#ov-interval .inp");
      const v = parseFloat((inp && inp.textContent) || "0.1") || 0.1;
      state.customInterval = v;
      state.intervalMode = 0;
      state.intervalLabel = `自定义 · ${v}s`;
      if ($("#comboInterval")) $("#comboInterval").textContent = state.intervalLabel;
      closeAllOv();
      updateCtas();
      quietSaveSettings({
        clickerButton: state.clickBtn,
        clickerIntervalMode: 0,
        clickerCustomInterval: v,
      });
    });

    $("#ctaClicker")?.addEventListener("click", (e) => {
      if (e.target.closest(".cta-key")) return;
      if (!window.qst) return;
      qst.setActiveHomeTab(0);
      if (state.clicking) qst.stopClicker();
      else {
        qst.startClicker({
          button: state.clickBtn,
          intervalMode: state.intervalMode,
          customInterval: state.customInterval,
        });
      }
    });
    $("#ctaClickerKey")?.addEventListener("click", (e) => {
      e.preventDefault();
      e.stopPropagation();
      openGlobalHotkeyCapture();
    });

    $("#ctaMacro")?.addEventListener("click", (e) => {
      if (e.target.closest(".cta-key")) return;
      if (state.macroSel < 0) {
        enterEditor("", "");
        return;
      }
      if (!window.qst) return;
      // 已在运行时：点击同一按钮立即终止当前脚本（等同热键停止），不再报 busy
      if (state.macroRunning) {
        qst.stopScript();
        return;
      }
      const m = state.macros[state.macroSel];
      if (m) {
        state._runningMacroName = m.name || "";
        state._pendingRunKind = "macro";
        if (typeof qst.setActiveHomeTab === "function") qst.setActiveHomeTab(2);
        if (typeof qst.setHomeSelection === "function") {
          qst.setHomeSelection(2, m.path || m.id || "");
        }
        qst.runScript(m.id || m.path);
      }
    });
    $("#ctaMacroKey")?.addEventListener("click", (e) => {
      e.preventDefault();
      e.stopPropagation();
      openGlobalHotkeyCapture();
    });

    $("#ctaRec")?.addEventListener("click", (e) => {
      if (e.target.closest("#recMode")) return;
      if (e.target.closest("#recWindowMode")) return;
      if (e.target.closest(".rec-mode-pills")) return;
      if (e.target.closest(".rec-mode-hint")) return;
      if (e.target.closest(".cta-key")) return;
      if (!window.qst) return;
      if (state.recording) {
        qst.stopRecord();
        return;
      }
      if (state.recSel >= 0) {
        const r = state.recordings[state.recSel];
        if (r) {
          state._pendingRunKind = "recording";
          if (typeof qst.setActiveHomeTab === "function") qst.setActiveHomeTab(1);
          if (typeof qst.setHomeSelection === "function") {
            qst.setHomeSelection(1, r.path || r.id || "");
          }
          qst.runScript(r.id || r.path);
        }
        return;
      }
      qst.setActiveHomeTab(1);
      qst.startRecord({ recorderWindowMode: state.recWindowMode | 0 });
    });
    $("#ctaRecKey")?.addEventListener("click", (e) => {
      e.preventDefault();
      e.stopPropagation();
      openGlobalHotkeyCapture();
    });

    $("#ctaAi")?.addEventListener("click", () => openAgentSession("", true));

    $("#btnAgentMin")?.addEventListener("click", (e) => {
      e.stopPropagation();
      minimizeAgentWindow();
    });
    $("#btnAgentClose")?.addEventListener("click", (e) => {
      e.stopPropagation();
      if (AGENT_SHELL) {
        closeAgentWindow();
        return;
      }
      const tab = activeAgentTab();
      if (tab) closeAgentTab(tab.key);
      else closeAgentWindow();
    });
    // 独立助手窗：标题栏拖动（mousedown + 壳 ReleaseCapture，与主窗一致）
    if (AGENT_SHELL) {
      $("#ov-agent .dlg-title")?.addEventListener("mousedown", (e) => {
        if (e.button !== 0) return;
        if (e.target.closest(".win-btn")) return;
        if (window.qst) qst.post({ type: "agentWindow.drag" });
      });
    }
    $("#agentMinDock")?.addEventListener("click", () => {
      restoreAgentWindow();
      openOv("agent");
    });
    $("#agentTabs")?.addEventListener("click", (e) => {
      const closeKey = e.target.closest("[data-tab-close]")?.dataset.tabClose;
      if (closeKey) {
        e.stopPropagation();
        closeAgentTab(closeKey);
        return;
      }
      const key = e.target.closest("[data-tab-key]")?.dataset.tabKey;
      if (key) activateAgentTab(key);
    });

    $("#btnSend")?.addEventListener("click", () => {
      if (!window.qst) return;
      const tab = activeAgentTab();
      if (state.agentBusy || (tab && tab.busy)) {
        state._agentCancelRequested = true;
        if (typeof qst.cancelAgentMessage === "function") qst.cancelAgentMessage();
        return;
      }
      if (anyAgentTabBusy()) {
        toast("助手忙碌中（另一会话正在回复）");
        return;
      }
      const cfgErr = getAgentConfigError();
      if (cfgErr) {
        toast(cfgErr);
        return;
      }
      const text = ($("#chatInput")?.textContent || "").trim();
      const attachPaths = agentAttachmentPaths();
      if (!text && !attachPaths.length) {
        toast("请输入内容");
        return;
      }
      const editIdx =
        tab && tab._editIndex != null && tab._editIndex >= 0
          ? tab._editIndex
          : -1;
      const editing =
        editIdx >= 0 &&
        tab &&
        (tab.messages || [])[editIdx] &&
        (tab.messages || [])[editIdx].role === "user";
      // 后端按「第几条用户消息」截断（0 起），不是消息数组下标
      let rewindUserIndex;
      if (editing) {
        const msgs = tab.messages || [];
        let u = 0;
        for (let i = 0; i < editIdx; ++i) {
          if (msgs[i] && msgs[i].role === "user") ++u;
        }
        rewindUserIndex = u;
      }
      const log = $("#chatLog");
      if (editing) {
        // 编辑重发：截断到该条消息之前，重绘后再流式回复
        tab.messages = (tab.messages || []).slice(0, editIdx);
        tab.messages.push({ role: "user", who: "你", content: text });
        clearAgentEditMode();
        renderChat(tab.messages);
      } else if (log) {
        const div = document.createElement("div");
        div.className = "bubble user";
        div.textContent = text;
        log.appendChild(div);
        scrollChatSmart(log);
      }
      if (tab) {
        tab.messages = tab.messages || [];
        if (!editing) tab.messages.push({ role: "user", who: "你", content: text });
        tab.draft = "";
        tab.busy = true;
        tab._thinkingBuf = "";
        tab._thinkingFinalized = false;
        tab._thinkingRound = 0;
        tab._lastReasoning = "";
        tab._lastReasoning = "";
      }
      if ($("#chatInput")) $("#chatInput").textContent = "";
      persistAgentDraft();
      state.agentBusy = true;
      syncAgentSendBtn();
      renderAgentTabs();
      qst.sendAgentMessage({
        id: (tab && tab.id) || state.agentId || "",
        text: text,
        model: state.agentModel || "",
        panelKey: tab ? tab.key : "",
        attachments: attachPaths,
        rewindUserIndex,
      });
      // 发送后清空输入区附件，但保留会话图库供预览切换
      (Array.isArray(state.agentAttachments) ? state.agentAttachments : []).forEach((a) =>
        rememberConvImage(normalizeAgentAttachment(a) || a)
      );
      state.agentAttachments = [];
      if (tab) tab.attachments = [];
      renderAgentAttachments();
    });

    $("#chatInput")?.addEventListener("keydown", (e) => {
      const editTab = activeAgentTab();
      if (e.key === "Escape" && editTab && editTab._editIndex >= 0) {
        e.preventDefault();
        clearAgentEditMode();
        persistAgentDraft();
        return;
      }
      if (e.key !== "Enter" || e.isComposing) return;
      if (e.shiftKey) return; // Shift+Enter 换行
      e.preventDefault();
      if (!state.agentBusy) $("#btnSend")?.click();
    });
    $("#chatInput")?.addEventListener("input", () => scheduleAgentDraftPersist());
    $("#agentEditBarClose")?.addEventListener("click", (e) => {
      e.stopPropagation();
      clearAgentEditMode();
      persistAgentDraft();
    });
    $("#chatLog")?.addEventListener("click", (e) => {
      const editBtn = e.target.closest("[data-edit-msg]");
      if (!editBtn) return;
      e.stopPropagation();
      const idx = parseInt(editBtn.dataset.editMsg, 10);
      if (Number.isFinite(idx) && idx >= 0) setAgentEditMode(idx);
    });
    $("#btnAgentChanges")?.addEventListener("click", (e) => {
      e.stopPropagation();
      if (window.qst) qst.listAgentChanges();
    });

    $("#btnAgentAttach")?.addEventListener("click", (e) => {
      e.stopPropagation();
      state._pendingBrowse = "agentAttach";
      if (window.qst) qst.browsePath(false);
      else toast("无桥接");
    });
    $("#chatInput")?.addEventListener("paste", (e) => {
      const cd = e.clipboardData;
      if (!cd) return;
      const files = cd.files;
      if (files && files.length) {
        let handled = false;
        for (let i = 0; i < files.length; ++i) {
          const f = files[i];
          if (!f) continue;
          if (String(f.type || "").startsWith("image/")) {
            e.preventDefault();
            handled = true;
            const ext = (f.type || "").includes("png") ? "png" : "jpg";
            const reader = new FileReader();
            reader.onload = () => {
              const dataUrl = String(reader.result || "");
              // 同一轮粘贴只保留一张乐观预览，避免多文件条目叠图
              state.agentAttachments = (state.agentAttachments || []).filter(
                (a) => !isTempClipboardAttach(a)
              );
              state._pendingPastePreview = dataUrl;
              if (dataUrl) {
                const entry = {
                  path: "clipboard-image." + ext,
                  previewUrl: dataUrl,
                  _temp: true,
                };
                state.agentAttachments.push(entry);
                rememberConvImage(entry);
                renderAgentAttachments();
              }
              if (window.qst && qst.saveClipboardImage) qst.saveClipboardImage(dataUrl, ext);
              else toast("无桥接");
            };
            reader.readAsDataURL(f);
            break; // 一次粘贴只取第一张图
          }
          // 非图片文件：若带 path 属性（部分环境）则直接当附件
          const filePath = f.path || "";
          if (filePath) {
            e.preventDefault();
            handled = true;
            addAgentAttachment(filePath);
          }
        }
        if (handled) return;
      }
      const text = (cd.getData("text") || "").trim();
      if (text && (/^[a-zA-Z]:[\\/]/.test(text) || text.startsWith("\\\\"))) {
        e.preventDefault();
        addAgentAttachment(text.replace(/^["']|["']$/g, ""));
        toast("已添加附件路径");
        return;
      }
      // 富文本粘贴：严格清洗后保留排版结构（剥离事件属性/style/脚本/媒体标签），
      // 防止剪贴板 HTML 触发 XSS（页面带全量 bridge 权限）。
      const html = cd.getData("text/html");
      if (html) {
        e.preventDefault();
        const cleaned = sanitizePastedHtml(html);
        if (document.queryCommandSupported && document.queryCommandSupported("insertHTML")) {
          document.execCommand("insertHTML", false, cleaned);
        } else if (text) {
          document.execCommand("insertText", false, text);
        }
      }
    });
    $("#agentAttachList")?.addEventListener("click", (e) => {
      const rm = e.target.closest("[data-rm-att]");
      if (rm) {
        e.preventDefault();
        e.stopPropagation();
        const i = rm.dataset.rmAtt | 0;
        if (Array.isArray(state.agentAttachments)) state.agentAttachments.splice(i, 1);
        renderAgentAttachments();
        syncAgentImageViewerFromAttachments();
        return;
      }
      const thumb = e.target.closest(".agent-att-thumb, .agent-att-img");
      if (thumb) {
        e.preventDefault();
        const att = thumb.closest(".agent-att");
        const img = att && att.querySelector(".agent-att-thumb");
        const path = (img && img.dataset.imgPath) || "";
        if (path) openAgentImageViewer(path);
      }
    });

    const bindAiv = () => {
      const onStep = (dir) => (e) => {
        e.preventDefault();
        e.stopPropagation();
        stepAgentImageViewer(dir);
      };
      $("#aivClose")?.addEventListener("click", () => closeAgentImageViewer());
      $("#aivPrev")?.addEventListener("click", onStep(-1));
      $("#aivNext")?.addEventListener("click", onStep(1));
      $("#aivZoomIn")?.addEventListener("click", () => zoomAgentImageViewer(0.2));
      $("#aivZoomOut")?.addEventListener("click", () => zoomAgentImageViewer(-0.2));
      $("#aivZoomReset")?.addEventListener("click", () => resetAgentImageViewerZoom());
      $("#aivCopy")?.addEventListener("click", () => copyAgentImageViewer());
      $("#aivDownload")?.addEventListener("click", () => downloadAgentImageViewer());
      const zoomInp = $("#aivZoomInput");
      if (zoomInp) {
        zoomInp.addEventListener("keydown", (e) => {
          e.stopPropagation();
          if (e.key === "Enter") {
            e.preventDefault();
            commitAgentImageViewerZoomInput();
            zoomInp.blur();
          }
        });
        zoomInp.addEventListener("change", () => commitAgentImageViewerZoomInput());
        zoomInp.addEventListener("blur", () => commitAgentImageViewerZoomInput());
        zoomInp.addEventListener("click", (e) => e.stopPropagation());
        zoomInp.addEventListener("focus", () => {
          try {
            zoomInp.select();
          } catch (_) {}
        });
      }
      $("#agentImgViewer")?.addEventListener("click", (e) => {
        if (e.target.closest(".aiv-nav, .aiv-toolbar, .aiv-footer, .aiv-frame, .aiv-zoom-wrap")) return;
        if (e.target.id === "agentImgViewer" || e.target.id === "aivStage") closeAgentImageViewer();
      });
      const stage = $("#aivStage");
      if (stage) {
        stage.addEventListener(
          "wheel",
          (e) => {
            if (!state._imgViewer) return;
            e.preventDefault();
            zoomAgentImageViewer(e.deltaY > 0 ? -0.12 : 0.12);
          },
          { passive: false }
        );
        let drag = null;
        stage.addEventListener("pointerdown", (e) => {
          if (!state._imgViewer || e.button !== 0) return;
          if (e.target.closest(".aiv-nav")) return;
          drag = { x: e.clientX, y: e.clientY, panX: state._imgViewer.panX || 0, panY: state._imgViewer.panY || 0 };
          stage.classList.add("dragging");
          stage.setPointerCapture(e.pointerId);
        });
        stage.addEventListener("pointermove", (e) => {
          if (!drag || !state._imgViewer) return;
          state._imgViewer.panX = drag.panX + (e.clientX - drag.x);
          state._imgViewer.panY = drag.panY + (e.clientY - drag.y);
          applyAgentImageViewerTransform();
        });
        stage.addEventListener("pointerup", (e) => {
          drag = null;
          stage.classList.remove("dragging");
          try {
            stage.releasePointerCapture(e.pointerId);
          } catch (_) {}
        });
      }
    };
    bindAiv();

    $("#macroList")?.addEventListener("pointerdown", onListSelectPointerDown);
    $("#recList")?.addEventListener("pointerdown", onListSelectPointerDown);
    const onListFolderClick = (e) => {
      const folderEl = e.target.closest(".list-folder");
      if (!folderEl) return;
      e.preventDefault();
      e.stopPropagation();
      const kind = folderEl.getAttribute("data-folder-kind") || "";
      const folder = folderEl.getAttribute("data-folder") || "";
      if (!kind || !folder) return;
      if (!state._listFolderOpen) state._listFolderOpen = {};
      if (!state._listFolderOpen[kind]) state._listFolderOpen[kind] = {};
      const cur = state._listFolderOpen[kind][folder] !== false;
      state._listFolderOpen[kind][folder] = !cur;
      if (kind === "macro") renderMacroList();
      else if (kind === "rec") renderRecList();
      else if (kind === "ai") renderAiList();
    };
    $("#macroList")?.addEventListener("click", onListFolderClick);
    $("#recList")?.addEventListener("click", onListFolderClick);
    $("#aiList")?.addEventListener("click", onListFolderClick);
    $("#macroList")?.addEventListener("click", onListClick);
    $("#recList")?.addEventListener("click", onListClick);
    $("#aiList")?.addEventListener("click", onListClick);
    let _scrollPersistTimer = 0;
    const onListScroll = () => {
      if (_scrollPersistTimer) clearTimeout(_scrollPersistTimer);
      _scrollPersistTimer = setTimeout(() => persistListScrollOffsets(), 400);
    };
    $("#macroList")?.addEventListener("scroll", onListScroll, { passive: true });
    $("#recList")?.addEventListener("scroll", onListScroll, { passive: true });
    $("#aiList")?.addEventListener("scroll", onListScroll, { passive: true });

    $("#btnMacroImport")?.addEventListener("click", () => {
      if (window.qst) qst.importScript("macro");
    });
    $("#btnMacroExport")?.addEventListener("click", () => {
      const m = state.macroSel >= 0 ? state.macros[state.macroSel] : null;
      if (!m) {
        toast("请先选中要导出的宏");
        return;
      }
      if (window.qst) qst.exportScript(itemPath(m));
    });
    $("#btnRecImport")?.addEventListener("click", () => {
      if (window.qst) qst.importScript("recording");
    });
    $("#btnRecExport")?.addEventListener("click", () => {
      const r = state.recSel >= 0 ? state.recordings[state.recSel] : null;
      if (!r) {
        toast("请先选中要导出的录制");
        return;
      }
      if (window.qst) qst.exportScript(itemPath(r));
    });

    $("#edActionType")?.addEventListener("click", (e) => {
      e.preventDefault();
      e.stopPropagation();
      const anchor = e.currentTarget;
      const mergeOn = isMergeEligible();
      const shown = editorParamAction();
      const curType = (shown && shown.type) || state.addActionType;
      const types = mergeOn
        ? pickerMergeContainerTypes()
        : pickerActionTypes(curType);
      const sel = types.findIndex((a) => a.v === curType);
      showPopup(
        anchor,
        types,
        (it) => {
          if (!it) return;
          state.addActionType = it.v;
          anchor.textContent = it.t;
          if (mergeOn) {
            showAddTypePreview();
            return;
          }
          // 选中动作时：切换类型 = 转换当前动作草稿（对齐原生 ActionFromForm +
          // ModifySelected）。未选中时保持“添加新动作”预览语义。
          if (
            state.actionSel >= 0 &&
            state.editorActions[state.actionSel] &&
            !state.addPreview
          ) {
            if (!state.editDraft) loadEditDraftFromSelection();
            if (state.editDraft) {
              readParamPanelInto(state.editDraft);
              const remark = ($("#edRemark")?.textContent || "").trim() || state.editDraft.remark;
              const fresh = defaultAction(it.v, remark);
              const merged = Object.assign({}, fresh, state.editDraft);
              merged.type = it.v;
              merged.name = typeLabel(it.v);
              merged.remark = remark;
              delete merged._preview;
              state.editDraft = merged;
              state.addPreview = null;
              renderParamPanel(state.editDraft);
              return;
            }
          }
          showAddTypePreview();
        },
        { minRows: 10, selectedIndex: sel >= 0 ? sel : 0 }
      );
    });

    // 右键动作：剪切/复制/粘贴 + 断点 / 从这里开始调试
    $("#actionList")?.addEventListener("contextmenu", (e) => {
      if (state.debugging) return;
      if (state.batchMode) {
        e.preventDefault();
        e.stopPropagation();
        const row = e.target.closest(".arow");
        const i = row ? +row.dataset.i : -1;
        prepareBatchContextSelection(i);
        showEditorBatchContextMenu(e.clientX, e.clientY);
        return;
      }
      const row = e.target.closest(".arow");
      e.preventDefault();
      e.stopPropagation();
      const hasClip = editorHasClipboard();
      const i = row ? +row.dataset.i : -1;
      const items = [];
      if (row && i >= 0) {
        items.push(
          { t: "剪切 (Ctrl+X)", v: "cut" },
          { t: "复制 (Ctrl+C)", v: "copy" },
          { t: "粘贴 (Ctrl+V)", v: "paste", disabled: !hasClip }
        );
        const isBp = editorBreakpointAt(i);
        items.push(
          { t: isBp ? "取消断点" : "设为断点（执行后结束调试）", v: "bp" },
          { t: "从这里开始调试（单步）", v: "step" },
          { t: "从这里开始调试（运行）", v: "run" },
          { t: "转到可视化", v: "visual" }
        );
        const act = state.editorActions[i];
        if (act && isDisassemblableType(act.type)) {
          items.push({ t: "拆解为动作", v: "disassemble" });
        }
      } else if (hasClip) {
        items.push({ t: "粘贴 (Ctrl+V)", v: "paste" });
      } else {
        return;
      }
      requestAnimationFrame(() => {
        showPopup(
          { clientX: e.clientX, clientY: e.clientY },
          items,
          (it) => {
            if (!it) return;
            if (it.v === "cut") cutSelectedEditorActions({ index: i });
            else if (it.v === "copy") copySelectedEditorActions({ index: i });
            else if (it.v === "paste") {
              if (i >= 0) pasteEditorClipboardAfterSelection({ index: i });
              else if (!(state.editorActions || []).length) pasteEditorClipboardAfterSelection();
              else pasteEditorClipboardAfterSelection({ index: state.editorActions.length - 1 });
            } else if (it.v === "bp") toggleEditorBreakpoint(i);
            else if (it.v === "step") startDebugFrom(i, true);
            else if (it.v === "run") startDebugFrom(i, false);
            else if (it.v === "visual") {
              const V = window.QstVisualEditor;
              if (V && typeof V.enterVisual === "function") V.enterVisual(i);
              else if (V && typeof V.setMode === "function") {
                V.setMode(true);
                if (typeof V.selectAction === "function") {
                  requestAnimationFrame(() => V.selectAction(i));
                }
              }
            } else if (it.v === "disassemble") disassembleEditorAction(i);
          },
          { preferUp: true }
        );
      });
    });

    $("#actionList")?.addEventListener("click", (e) => {
      if (state._suppressRowClick) {
        state._suppressRowClick = false;
        return;
      }
      if (state._dragging) return;
      const row = e.target.closest(".arow");
      if (!row) return;
      const i = +row.dataset.i;
      const act = e.target.closest("[data-act]")?.dataset.act;
      if (act === "expand-toggle") {
        toggleContainerExpand(i);
        renderEditorActions(state.editorActions);
        return;
      }
      if (!state.debugging && (e.ctrlKey || e.metaKey) && !state.batchMode && act !== "copy" && act !== "del") {
        enterEditorBatch(true);
        toggleEditorBatchRow(i);
        return;
      }
      if (state.batchMode) {
        if (act === "batch-toggle" || !act) applyEditorBatchRowClick(i, e);
        return;
      }
      if (act === "batch-toggle") return;
      if (act === "copy") {
        const src = state.editorActions[i];
        if (!src) return;
        e.preventDefault();
        e.stopPropagation();
        const point = { clientX: e.clientX, clientY: e.clientY };
        const items = [
          { t: "复制到最后", v: "last" },
          { t: "复制到最前", v: "first" },
          { t: "复制到当前项前", v: "before" },
          { t: "复制到当前项后", v: "after" },
        ];
        // 延后一帧：避免同次 click/mousedown 冒泡立刻关掉菜单
        requestAnimationFrame(() => {
          showPopup(point, items, (it) => {
            if (!it) return;
            const copy = JSON.parse(JSON.stringify(src));
            copy.name = typeLabel(copy.type);
            if (copy.type === "defineBlock") {
              copy.blockName = uniquifyPastedBlockName(copy.blockName, {});
            }
            let pos = state.editorActions.length;
            if (it.v === "first") pos = 0;
            else if (it.v === "before") pos = i;
            else if (it.v === "after") pos = subtreeEnd(i);
            else pos = state.editorActions.length;
            visualNoteUndo();
            state.editorActions.splice(pos, 0, copy);
            visualOnInsert(pos);
            shiftCollapsedAfterInsert(pos);
            if (isSubtreeContainerType(copy.type)) delete state.collapsedContainers[pos];
            state.actionSel = pos;
            state.addPreview = null;
            renderEditorActions(state.editorActions);
          }, { preferUp: true });
        });
        return;
      }
      if (act === "del") {
        deleteSubtreeAt(i);
        state.addPreview = null;
        renderEditorActions(state.editorActions);
        if (state.actionSel < 0) showAddTypePreview();
        return;
      }
      // 对齐 exe：再点已选中行 → 取消选中并恢复添加草稿（未点修改的改动丢弃）
      if (state.actionSel === i) {
        const prev = state.actionSel;
        state.actionSel = -1;
        state.editDraft = null;
        state.addPreview = null;
        const remarkEl = $("#edRemark");
        if (remarkEl) remarkEl.textContent = "";
        showAddTypePreview();
        state._skipReadback = true;
        updateEditorSelectionUi(prev, -1);
        state._skipReadback = false;
        return;
      }
      const prevSel = state.actionSel;
      state.actionSel = i;
      state.addPreview = null;
      loadEditDraftFromSelection();
      {
        const a = state.editDraft;
        if (a && a.type) {
          state.addActionType = a.type;
          const typeCombo = $("#edActionType");
          const cur = ACTION_TYPES.find((x) => x.v === a.type);
          if (typeCombo && cur) typeCombo.textContent = cur.t;
        }
      }
      state._skipReadback = true;
      updateEditorSelectionUi(prevSel, i);
      state._skipReadback = false;
      focusEditorActionList();
    });

    $("#btnEdAdd")?.addEventListener("click", (e) => {
      if (e.button !== 0) return;
      if (state.batchMode && isMergeEligible()) {
        const plan = analyzeMergeSelection(state.editorActions, selectedBatchIndices());
        const type =
          (state.addPreview && state.addPreview.type) || state.addActionType;
        if (type === "defineBlock" || (plan && plan.contiguous)) {
          runMergeWithPlacement(type === "defineBlock" ? "first" : "inplace");
          return;
        }
        const point = { clientX: e.clientX, clientY: e.clientY };
        requestAnimationFrame(() => {
          showPopup(
            point,
            [
              { t: "添加到最后", v: "last" },
              { t: "插入到最前", v: "first" },
              { t: "插入到选择项前", v: "before" },
              { t: "插入到选择项后", v: "after" },
            ],
            (it) => {
              if (!it) return;
              runMergeWithPlacement(it.v);
            },
            { preferUp: true }
          );
        });
        return;
      }

      const remark = ($("#edRemark")?.textContent || "").trim();
      let a;
      if (state.addPreview && state.addPreview.type === state.addActionType) {
        readParamPanelInto(state.addPreview);
        a = Object.assign({}, state.addPreview);
        delete a._preview;
        a.remark = remark;
        a.name = typeLabel(a.type);
      } else if (state.actionSel >= 0 && state.editorActions[state.actionSel]) {
        // 对齐原生 ActionFromForm：选中列表项时，添加的是右栏「动作详情」当前内容
        // （含尚未点「修改」的改动），而不是重置成默认空动作
        if (!state.editDraft) loadEditDraftFromSelection();
        if (state.editDraft) {
          readParamPanelInto(state.editDraft);
          a = Object.assign({}, state.editDraft);
          delete a._preview;
          a.remark = remark;
          a.name = typeLabel(a.type);
        } else {
          a = defaultAction(state.addActionType, remark);
        }
      } else {
        a = defaultAction(state.addActionType, remark);
      }

      const finishInsert = (pos, indent) => {
        if (!insertEditorAction(a, pos, indent)) return;
        state.actionSel = -1;
        state.batchMode = false;
        state.addPreview = null;
        renderEditorActions(state.editorActions);
        // 对齐原生 TryInsertActionFromForm：添加成功后清空备注栏，避免下一条动作继承
        const remarkEl = $("#edRemark");
        if (remarkEl) remarkEl.textContent = "";
        showAddTypePreview();
      };

      // DefineBlock：强制列表顶部 indent=0
      if (a.type === "defineBlock") {
        finishInsert(0, 0);
        return;
      }

      // 无选中：直接末尾，顶层缩进（勿因上一条是容器而变成其子节点）
      if (state.actionSel < 0 || state.actionSel >= state.editorActions.length) {
        finishInsert(state.editorActions.length, 0);
        return;
      }

      const sel = state.actionSel;
      const selAct = state.editorActions[sel];
      finishInsert(subtreeEnd(sel), selAct.indent | 0);
    });
    $("#btnEdAdd")?.addEventListener("contextmenu", (e) => {
      e.preventDefault();
      e.stopPropagation();
      const remark = ($("#edRemark")?.textContent || "").trim();
      let a;
      if (state.addPreview && state.addPreview.type === state.addActionType) {
        readParamPanelInto(state.addPreview);
        a = Object.assign({}, state.addPreview);
        delete a._preview;
        a.remark = remark;
        a.name = typeLabel(a.type);
      } else if (state.actionSel >= 0 && state.editorActions[state.actionSel]) {
        if (!state.editDraft) loadEditDraftFromSelection();
        if (state.editDraft) {
          readParamPanelInto(state.editDraft);
          a = Object.assign({}, state.editDraft);
          delete a._preview;
          a.remark = remark;
          a.name = typeLabel(a.type);
        } else {
          a = defaultAction(state.addActionType, remark);
        }
      } else {
        a = defaultAction(state.addActionType, remark);
      }

      const finishInsert = (pos, indent) => {
        if (!insertEditorAction(a, pos, indent)) return;
        state.actionSel = -1;
        state.batchMode = false;
        state.addPreview = null;
        renderEditorActions(state.editorActions);
        const remarkEl = $("#edRemark");
        if (remarkEl) remarkEl.textContent = "";
        showAddTypePreview();
      };

      const hasSel = state.actionSel >= 0 && state.actionSel < state.editorActions.length;
      const sel = hasSel ? state.actionSel : -1;
      const selAct = hasSel ? state.editorActions[sel] : null;
      const items = hasSel
        ? [
            { t: "添加到最后", v: "last" },
            { t: "插入到最前", v: "first" },
            { t: "插入到选择项前", v: "before" },
            { t: "插入到选择项后", v: "after" },
          ]
        : [
            { t: "添加到最后", v: "last" },
            { t: "插入到最前", v: "first" },
          ];
      if (hasSel && selAct && isSubtreeContainerType(selAct.type)) {
        items.push({ t: "添加为子节点", v: "asChild" });
      }
      const point = { clientX: e.clientX, clientY: e.clientY };
      requestAnimationFrame(() => {
        showPopup(point, items, (it) => {
          if (!it) return;
          if (a.type === "defineBlock") {
            if (it.v !== "betweenKey") {
              finishInsert(0, 0);
              return;
            }
          }
          let pos = state.editorActions.length;
          let indent = -1;
          if (it.v === "first") {
            pos = 0;
            indent = 0;
          } else if (it.v === "before" && selAct) {
            pos = sel;
            indent = selAct.indent | 0;
          } else if (it.v === "after" && selAct) {
            pos = subtreeEnd(sel);
            indent = selAct.indent | 0;
          } else if (it.v === "asChild" && selAct) {
            pos = subtreeEnd(sel);
            indent = (selAct.indent | 0) + 1;
          } else {
            pos = state.editorActions.length;
            indent = 0;
          }
          finishInsert(pos, indent);
        }, { preferUp: true });
      });
    });
    $("#btnEdRemark")?.addEventListener("click", () => {
      commitModifySelected();
    });
    $("#btnEdClear")?.addEventListener("click", () => {
        askConfirm("确定清空动作列表？", () => {
          visualNoteUndo();
          const n = state.editorActions.length;
          state.editorActions = [];
          if (n) visualOnDelete(0, n);
          state.actionSel = -1;
          state.batchMode = false;
          state.batchSel = {};
        renderEditorActions(state.editorActions);
        closeAllOv();
      });
    });
    $("#btnBatch")?.addEventListener("click", () => enterEditorBatch(true));
    $("#btnExitBatch")?.addEventListener("click", () => enterEditorBatch(false));
    $("#batchBar")?.addEventListener("click", (e) => {
      const btn = e.target.closest("button");
      if (!btn || !state.batchMode) return;
      const label = (btn.textContent || "").trim();
      if (btn.id === "btnExitBatch") return;
      if (label === "全选") {
        setEditorBatchSelectedAll(true);
        return;
      }
      if (label === "取消选择") {
        setEditorBatchSelectedAll(false);
        return;
      }
      if (label === "删除所选项") {
        const idx = selectedBatchIndices();
        if (!idx.length) {
          toast("请先勾选动作");
          return;
        }
        askConfirm("删除选中的 " + idx.length + " 条动作？", () => {
          batchDeleteSelectedSubtrees();
          renderEditorActions(state.editorActions);
        });
        return;
      }
      if (label === "复制所选项") {
        const idx = selectedBatchIndices();
        if (!idx.length) {
          toast("请先勾选动作");
          return;
        }
        const copies = idx.map((i) =>
          JSON.parse(JSON.stringify(state.editorActions[i]))
        );
        const usedNames = {};
        visualNoteUndo();
        const start = state.editorActions.length;
        copies.forEach((a, i) => {
          delete a._preview;
          a.name = typeLabel(a.type);
          if (a.type === "defineBlock") a.blockName = uniquifyPastedBlockName(a.blockName, usedNames);
          state.editorActions.push(a);
          visualOnInsert(start + i);
          shiftCollapsedAfterInsert(start + i);
        });
        state.batchSel = {};
        renderEditorActions(state.editorActions);
        toast("已复制 " + copies.length + " 条到列表末尾");
      }
    });

    document.addEventListener(
      "click",
      (e) => {
        const btn = e.target.closest('[data-open="crop"], [data-open="shot"]');
        if (!btn) return;
        e.preventDefault();
        e.stopPropagation();
        if (window.qst) qst.pickScreenRegion();
      },
      true
    );

    wireCrosshairPointerDown($("#edPickBtn"), "windowTarget", "window");
    wireCrosshairPointerDown($("#btnPickCrosshair"), "windowTarget", "window");
    wireCrosshairPointerDown($("#edClassBtn"), "windowTarget", "windowClass");
    wireCrosshairPointerDown($("#btnFixedCrosshair"), "coordinates", "fixedClick");
    $("#btnPickApply")?.addEventListener("click", () => {
      const path = ($("#pickPath")?.textContent || "").trim();
      const doc = ($("#pickDoc")?.textContent || "").trim();
      const cls = ($("#pickClass")?.textContent || "").trim();
      const title = ($("#pickTitle")?.textContent || "").trim();
      const payload = {
        processPath: path,
        documentPath: doc,
        windowClassName: cls,
        windowTitle: title,
        childWindowClassName: ($("#pickChild")?.textContent || "").trim(),
        x: parseInt($("#pickX")?.textContent || "0", 10) || 0,
        y: parseInt($("#pickY")?.textContent || "0", 10) || 0,
      };
      if (state._nestedWmPick) {
        applyWindowPickToNestedAction(payload, "nestedWmClass");
        state._nestedWmPick = false;
      } else {
        applyWindowPickToEditor(payload, "windowClass");
      }
      closeAllOv();
      toast("已填入目标窗口");
    });

    function syncThemeColorInputs() {
      const other = (state.settings && state.settings.other) || {};
      const main = state.useCustomTheme
        ? colorrefToCss(other.customMainColor)
        : (state.themes || []).find((t) => t.id === state.themeId)?.main || "#1aa6d6";
      const accent = state.useCustomTheme
        ? colorrefToCss(other.customAccentColor)
        : (state.themes || []).find((t) => t.id === state.themeId)?.accent || "#2dd4bf";
      if ($("#themeMainColor")) $("#themeMainColor").value = main;
      if ($("#themeAccentColor")) $("#themeAccentColor").value = accent;
      if ($("#swMain")) $("#swMain").style.background = main;
      if ($("#swAccent")) $("#swAccent").style.background = accent;
    }
    $("#themeMainColor")?.addEventListener("input", (e) => {
      if ($("#swMain")) $("#swMain").style.background = e.target.value;
    });
    $("#themeAccentColor")?.addEventListener("input", (e) => {
      if ($("#swAccent")) $("#swAccent").style.background = e.target.value;
    });
    $("#btnThemeRand")?.addEventListener("click", () => {
      const rand = () =>
        "#" +
        Math.floor(Math.random() * 0xffffff)
          .toString(16)
          .padStart(6, "0");
      const m = rand();
      const a = rand();
      if ($("#themeMainColor")) $("#themeMainColor").value = m;
      if ($("#themeAccentColor")) $("#themeAccentColor").value = a;
      if ($("#swMain")) $("#swMain").style.background = m;
      if ($("#swAccent")) $("#swAccent").style.background = a;
    });
    $("#btnThemeOk")?.addEventListener("click", () => {
      const mainHex = $("#themeMainColor")?.value || "#1aa6d6";
      const accentHex = $("#themeAccentColor")?.value || "#2dd4bf";
      applyThemeCss({ main: mainHex, accent: accentHex });
      if (window.qst) {
        qst.applyTheme({
          themeId: 0,
          useCustomTheme: 1,
          customMainColor: cssToColorref(mainHex),
          customAccentColor: cssToColorref(accentHex),
        });
      }
      state.useCustomTheme = true;
      if ($("#themeCombo")) $("#themeCombo").textContent = "自定义";
      closeAllOv();
    });

    $("#btnBackHome")?.addEventListener("click", () => exitEditor());
    $("#btnEdCancel")?.addEventListener("click", () => exitEditor());
    $("#btnDebugHotkey")?.addEventListener("click", () => openDebugHotkeyCapture());
    $("#btnEdSave")?.addEventListener("click", () => {
      syncFormIntoSelectedBeforeSave();
      if (!validateEditorBeforeSave()) return;
      const wm = collectWindowModeForSave();
      if (!validateWindowModeForSave(wm)) return;
      const name =
        ($("#edName")?.textContent || "").trim() || state.editorName || ("鼠标宏-" + Math.floor(Date.now() / 1000));
      if (!window.qst) {
        toast("无桥接环境");
        return;
      }
      const doSave = () => {
        qst.saveEditor({
          path: state.editorPath || "",
          name,
          breakoutTimeSeconds: parseFloat(($("#edBreakout")?.textContent || "0")) || 0,
          mode: state.editorMode | 0,
          windowMode: wm,
          actions: state.editorActions,
          visualLayout: collectVisualLayoutForSave(),
        });
      };
      const V = window.QstVisualEditor;
      if (V && typeof V.isVisual === "function" && V.isVisual() && typeof V.commitGraphToList === "function") {
        V.commitGraphToList(function (ok) {
          if (ok) doSave();
        }, "与初始节点无关的流程会直接删除，确定保存？");
        return;
      }
      doSave();
    });
    $$("[data-exit-editor]").forEach((b) =>
      b.addEventListener("click", () => exitEditor())
    );

    $("#btnRenameOk")?.addEventListener("click", () => {
      const name = ($("#renameInput")?.textContent || "").trim();
      if (state.pendingKind === "askInput") {
        const fn = state._askInputOk;
        state._askInputOk = null;
        state.pendingKind = null;
        closeAllOv();
        const allowEmpty = !!state._askInputAllowEmpty;
        state._askInputAllowEmpty = false;
        if (!name && !allowEmpty) {
          toast("名称无效");
          return;
        }
        if (typeof fn === "function") fn(name);
        return;
      }
      if (!name || !state.pendingPath) {
        toast("名称无效");
        return;
      }
      if (window.qst) qst.renameScript(state.pendingPath, name);
    });
    $("#btnPromptOk")?.addEventListener("click", () => {
      const fn = state._promptOk;
      state._promptOk = null;
      closeAllOv();
      if (typeof fn === "function") fn();
    });

    $("#btnHotkeyOk")?.addEventListener("click", () => {
      if (!window.qst) return;
      const d = state.hotkeyDraft || {};
      // 必须有 VK：仅文本会写盘成「僵尸热键」（列表有字但不触发）
      if (!(d.vk > 0)) {
        toast("请先按下热键");
        return;
      }
      finalizeHotkeyHoldIfStillDown();
      stopWebHotkeyCapture();
      if (state.pendingKind === "globalHotkey") {
        qst.setGlobalHotkey({
          hotkeyText: d.text || "",
          hotkeyVk: d.vk || 0,
          hotkeyModifiers: d.modifiers || 0,
          hotkeyHold: d.hold ? 1 : 0,
        });
        closeAllOv();
        return;
      }
      if (state.pendingKind === "debugHotkey") {
        state.debugHotkey = {
          text: d.text || "F9",
          vk: d.vk || 0x78,
          modifiers: d.modifiers || 0,
        };
        state.pendingKind = "";
        closeAllOv();
        syncDebugHotkeyUi();
        toast("调试热键已设为 " + (d.text || "F9") + "（仅本次编辑会话）");
        return;
      }
      if (!state.pendingPath) return;
      qst.setScriptHotkey({
        path: state.pendingPath,
        hotkeyText: d.text || "",
        hotkeyVk: d.vk || 0,
        hotkeyModifiers: d.modifiers || 0,
        hotkeyHold: d.hold ? 1 : 0,
      });
    });
    $("#btnCropOk")?.addEventListener("click", () => confirmWebFindImageCrop());
    $("#btnCropCancel")?.addEventListener("click", () => closeAllOv());
    $("#btnActionKeyOk")?.addEventListener("click", () => {
      const d = _actionKeyDraft || {};
      if (!(d.vk > 0)) {
        toast("请先按下按键");
        return;
      }
      if (window.qst) {
        // 对齐原生：keyText = VkName，修饰键写入「同时按住」
        state._pendingActionKeyApply = true;
        qst.formatHotkey({
          hotkeyVk: d.vk | 0,
          hotkeyModifiers: 0,
          hotkeyHold: 0,
        });
        return;
      }
      applyActionKeyToParam(d.text || "", d.vk | 0, d.modifiers | 0);
      stopActionKeyCapture();
      const ov = $("#ov-action-key");
      if (ov) ov.classList.remove("show");
    });
    $("#btnHotkeyClear")?.addEventListener("click", () => {
      if (!window.qst) return;
      stopWebHotkeyCapture();
      if (state.pendingKind === "globalHotkey") {
        // 清除=禁用，勿硬编码回 F8（可能与脚本热键撞车导致清不掉）
        qst.setGlobalHotkey({
          hotkeyText: "",
          hotkeyVk: 0,
          hotkeyModifiers: 0,
          hotkeyHold: 0,
        });
        closeAllOv();
        return;
      }
      if (!state.pendingPath) return;
      qst.setScriptHotkey({
        path: state.pendingPath,
        hotkeyText: "",
        hotkeyVk: 0,
        hotkeyModifiers: 0,
        hotkeyHold: 0,
      });
    });
    $("#btnHotkeyReset")?.addEventListener("click", () => {
      state.hotkeyDraft = { text: "F8", vk: 119, modifiers: 0, hold: false };
      syncHotkeyValUi();
      // 继续捕获，便于改完后立刻再按
      startWebHotkeyCapture();
    });
    // 脚本/启停热键：Web #ov-hotkey + LL（不再走原生 HotkeyCapture）
    $("#ov-settings .dlg-foot .btn.primary")?.addEventListener("click", () => {
      if (!window.qst) return;
      const wantDebug = !!$("#setDebugWin")?.classList.contains("on");
      state._announceSettingsSave = true;
      const payload = collectSettings();
      if (!payload) return;
      payload.uiMode = readUiModeDraft();
      qst.saveSettings(payload);
      commitUiModeFromDraft();
      if (wantDebug) {
        hideDebugFloat();
        qst.showDebugWindow();
      } else {
        hideDebugFloat();
        qst.showDebugWindow(); // ReloadSettings → HideDebugWindow
      }
      closeAllOv();
    });
    $("#btnSaveEditorSettings")?.addEventListener("click", () => {
      persistEditorSettings(collectEditorSettingsPartial(), true);
      closeAllOv();
    });
    $("#btnRestoreEditorDefaults")?.addEventListener("click", (e) => {
      e.preventDefault();
      askConfirm("确定将鼠标宏编辑设置恢复为默认？", () => {
        const defaults = {
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
        };
        fillEditorSettingsForm(defaults);
        persistEditorSettings(defaults, true);
        closeAllOv();
      });
    });
    $("#btnRestoreDefaults")?.addEventListener("click", (e) => {
      e.preventDefault();
      if (!window.qst) return;
      askConfirm("确定恢复所有默认设置？", () => {
        qst.restoreSettingsDefaults();
      });
    });

    $("#btnIcInstall")?.addEventListener("click", () => {
      if (!window.qst) return;
      applyDriverProgress({ kind: "interception", percent: 5, step: 0, status: "开始安装…" });
      qst.installDriver("interception");
    });
    $("#btnIcUninstall")?.addEventListener("click", () => {
      if (!window.qst) return;
      askConfirm("将卸载 Interception 并清除键盘/鼠标过滤驱动残留（用于修复无法开机类问题）。确定继续？", () => {
        applyDriverProgress({ kind: "interception", percent: 5, step: 0, status: "卸载并修复…" });
        qst.installDriver("interception", { uninstall: true });
      });
    });
    $("#btnVhidInstall")?.addEventListener("click", () => {
      if (!window.qst) return;
      applyDriverProgress({ kind: "vhid", percent: 5, step: 0, status: "开始安装…" });
      qst.installDriver("vhid");
    });
    $("#btnVhidUninstall")?.addEventListener("click", () => {
      if (!window.qst) return;
      askConfirm("将卸载虚拟 HID，并清除旧版安装留下的开机任务。确定继续？", () => {
        applyDriverProgress({ kind: "vhid", percent: 5, step: 0, status: "卸载并修复…" });
        qst.installDriver("vhid", { uninstall: true });
      });
    });

    $("#btnSchedCreate")?.addEventListener("click", () => {
      // 先确保宏/录制列表已加载，避免创建表单里选不了文件
      if (window.qst) {
        if (!state.macros.length) qst.listScripts();
        if (!state.recordings.length) qst.listRecordings();
      }
      openSchedEditor(null);
    });
    $("#schedTable")?.addEventListener("click", (e) => {
      const row = e.target.closest(".tr[data-id]");
      if (!row) return;
      const id = row.dataset.id;
      const act = e.target.closest("[data-sched]")?.dataset.sched;
      const task = (state.schedTasks || []).find((t) => t.id === id);
      if (act === "edit" && task) openSchedEditor(task);
      if (act === "status" && task) toggleSchedTaskStatus(task);
      if (act === "del" && id) {
        askConfirm("确定删除该定时任务？", () => {
          if (window.qst) qst.deleteScheduledTask(id);
        });
      }
    });
    $("#schedGlobalDisable")?.addEventListener("click", () => {
      const on = !$("#schedGlobalDisable").classList.contains("on");
      setChk($("#schedGlobalDisable"), on);
      if (window.qst) qst.setScheduledTasksGlobalDisabled(on);
    });
    $("#schedFreqRow")?.addEventListener("click", (e) => {
      const r = e.target.closest(".radio");
      if (!r) return;
      const prev = $("#schedFreqRow .radio.on");
      const prevFreq = prev ? prev.dataset.freq | 0 : -1;
      $$("#schedFreqRow .radio").forEach((x) => x.classList.remove("on"));
      r.classList.add("on");
      const freq = r.dataset.freq | 0;
      // 从钟点频率切到间隔时不要沿用「8 点」当成 8 小时
      if (freq === 4 && prevFreq !== 4 && $("#schedHour"))
        $("#schedHour").textContent = "0";
      syncSchedFreqUi(freq);
    });
    $("#schedKindRow")?.addEventListener("click", (e) => {
      const r = e.target.closest(".radio");
      if (!r) return;
      $$("#schedKindRow .radio").forEach((x) => x.classList.remove("on"));
      r.classList.add("on");
    });
    $("#schedStatusRow")?.addEventListener("click", (e) => {
      const r = e.target.closest(".radio");
      if (!r) return;
      $$("#schedStatusRow .radio").forEach((x) => x.classList.remove("on"));
      r.classList.add("on");
    });
    $("#schedFile")?.addEventListener("click", (e) => {
      e.stopPropagation();
      const kindEl = $("#schedKindRow .radio.on");
      const kind = kindEl ? kindEl.dataset.kind | 0 : 1;
      const items = (kind === 0 ? state.recordings : state.macros).map((x) => ({
        t: stripSchedFileLabel(x.name || x.path),
        v: x.path || x.id,
      }));
      if (!items.length) {
        toast(kind === 0 ? "暂无录制" : "暂无宏");
        return;
      }
      const el = e.currentTarget;
      showPopup(el, items, (it) => {
        if (!el) return;
        el.textContent = stripSchedFileLabel(it.t);
        el.dataset.path = it.v;
      });
    });
    $("#btnSchedSave")?.addEventListener("click", () => {
      const payload = collectSchedForm();
      const name = (payload.name || "").trim();
      if (!name) {
        toast("请输入任务名称。");
        return;
      }
      if (!payload.filePath) {
        toast("请选择要运行的文件。");
        return;
      }
      if ((payload.frequency | 0) === 2 && !(payload.weekDays | 0)) {
        toast("请至少选择一个星期。");
        return;
      }
      if ((payload.frequency | 0) === 4) {
        const total =
          (payload.hour | 0) * 3600000 +
          (payload.minute | 0) * 60000 +
          (payload.second | 0) * 1000 +
          (payload.millisecond | 0);
        if (total <= 0) {
          toast("间隔时间必须大于 0。");
          return;
        }
      }
      if ((payload.frequency | 0) === 3 && !payload.id) {
        // 自定义：新建时不可早于现在
        const y = payload.year | 0;
        const mo = payload.month | 0;
        const d = payload.day | 0;
        const hh = payload.hour | 0;
        const mm = payload.minute | 0;
        const ss = payload.second | 0;
        const ms = payload.millisecond | 0;
        if (y > 0 && mo > 0 && d > 0) {
          const when = new Date(y, mo - 1, d, hh, mm, ss, ms);
          if (when.getTime() < Date.now()) {
            toast("自定义运行时间不能早于当前时间。");
            return;
          }
        }
      }
      if (window.qst) qst.saveScheduledTask(payload);
    });

    $("#optApply")?.addEventListener("click", () => {
      if (!collectOptSelected().length) {
        toast("请先选择要删除的动作。");
        return;
      }
      applyOptimizePayload({
        scheme: 0,
        protectKeyOps: $("#optProtect")?.classList.contains("on") ? 1 : 0,
      });
    });
    $("#optApplyWait")?.addEventListener("click", () => {
      if (!collectOptSelected().length) {
        toast("请先选择要调整的动作。");
        return;
      }
      applyOptimizePayload({
        scheme: 1,
        waitValue: parseFloat($("#optWaitValue")?.textContent || "0.1") || 0.1,
        waitFilter: waitFilterIndex(),
        compareValue: parseFloat($("#optWaitCompare")?.textContent || "0.1") || 0.1,
      });
    });
    $("#optWaitFilter")?.addEventListener("click", (e) => {
      e.stopPropagation();
      const waitFilters = [
        "全部",
        "小于",
        "小于等于",
        "大于",
        "大于等于",
        "等于",
        "不等于",
      ];
      const cur = waitFilterIndex();
      showPopup(
        e.currentTarget,
        waitFilters.map((t) => ({ t })),
        (it, idx) => {
          const el = $("#optWaitFilter");
          if (el) el.textContent = it.t;
          const row = $("#optWaitCompareRow");
          if (row) row.style.display = idx === 0 ? "none" : "flex";
        },
        { selectedIndex: cur }
      );
    });
    $("#optApplyMerge")?.addEventListener("click", () => {
      const selected = collectOptSelected();
      const err = validateOptMerge(state.optActions || [], selected);
      if (err) {
        toast(err);
        return;
      }
      const mergeOn = $("#optMergeRadios .radio.on");
      const waitCalculation = (mergeOn && mergeOn.dataset.merge) || "first";
      applyOptimizePayload({
        scheme: 2,
        waitCalculation,
        mergeWaitValue: parseFloat($("#optMergeWait")?.textContent || "0.1") || 0.1,
        waitValue: parseFloat($("#optMergeWait")?.textContent || "0.1") || 0.1,
      });
    });
    $("#optApplyCompress")?.addEventListener("click", () => {
      const selected = collectOptSelected();
      const err = validateOptCompress(state.optActions || [], selected);
      if (err) {
        toast(err);
        return;
      }
      applyOptimizePayload({
        scheme: 3,
        compressWait: parseFloat($("#optCompressWait")?.textContent || "0.1") || 0.1,
        distanceThreshold: parseFloat($("#optCompressDist")?.textContent || "1") || 1,
      });
    });
    $("#optScheme")?.addEventListener("click", (e) => {
      e.stopPropagation();
      const anchor = e.currentTarget;
      showPopup(
        anchor,
        OPT_SCHEMES,
        (it) => setOptScheme(it.v),
        { selectedIndex: state.optScheme | 0, minWidth: 200 }
      );
    });
    $("#optMergeRadios")?.addEventListener("click", (e) => {
      const r = e.target.closest(".radio");
      if (!r) return;
      $$("#optMergeRadios .radio").forEach((x) => x.classList.remove("on"));
      r.classList.add("on");
      const custom = $("#optMergeCustom");
      if (custom) custom.style.display = r.dataset.merge === "fixed" ? "" : "none";
    });
    $("#optList")?.addEventListener("click", (e) => {
      const row = e.target.closest(".arow");
      if (!row || !row.closest("#optList")) return;
      const i = row.dataset.i | 0;
      if (!Array.isArray(state.optSelected)) state.optSelected = [];
      const pos = state.optSelected.indexOf(i);
      if (pos >= 0) state.optSelected.splice(pos, 1);
      else state.optSelected.push(i);
      state.optSelected.sort((a, b) => a - b);
      state.optHighlight = i;
      // 就地更新勾选态，禁止整表重绘（否则 reveal 动画导致整列闪烁）
      syncOptListSelectionUi();
    });
    $("#btnOptSaveAs")?.addEventListener("click", () => {
      if (!window.qst || !state.optPath) {
        toast("无录制路径");
        return;
      }
      const name = ($("#optName")?.textContent || "").trim();
      state._optSaveAs = true;
      qst.applyOptimizeRecording({
        path: state.optPath,
        scheme: -1,
        saveAsNew: 1,
        name: name || undefined,
        selected: [],
      });
    });
    $("#btnKeySearch")?.addEventListener("click", (e) => {
      e.stopPropagation();
      const acts = state.optActions || [];
      const items = [
        { t: "查找下一个关键操作", v: "next" },
        { t: "查找上一个关键操作", v: "prev" },
      ];
      showPopup(e.currentTarget, items, (it) => {
        let start = state._optKeySearchIdx != null ? state._optKeySearchIdx : -1;
        let found = -1;
        if (it.v === "next") {
          for (let i = start + 1; i < acts.length; ++i) {
            if (isOptKeyOperation(acts[i])) {
              found = i;
              break;
            }
          }
        } else {
          for (let i = start - 1; i >= 0; --i) {
            if (isOptKeyOperation(acts[i])) {
              found = i;
              break;
            }
          }
        }
        if (found < 0) {
          toast("未找到关键操作");
          return;
        }
        state._optKeySearchIdx = found;
        state.optHighlight = found;
        syncOptListSelectionUi();
        toast(`关键操作 #${found + 1}`);
      });
    });
    $("#btnQuickSelect")?.addEventListener("click", (e) => {
      e.stopPropagation();
      const items = [
        { t: "全选", v: 6 },
        { t: "清空选择", v: 5 },
        { t: "选择从当前选择项到最前", v: 0 },
        { t: "选择从当前选择项到最后", v: 1 },
        { t: "选择从当前选择项到下一关键操作间操作", v: 2 },
        { t: "选择从当前选择项到上一关键操作间操作", v: 3 },
        { t: "选择从当前已选两项间操作", v: 4 },
      ];
      showPopup(e.currentTarget, items, (it) => {
        const acts = state.optActions || [];
        const n = acts.length;
        if (!n) return;
        let next = Array.isArray(state.optSelected) ? state.optSelected.slice() : [];
        const selectedSet = new Set(next);
        let anchor =
          state.optHighlight >= 0
            ? state.optHighlight
            : state._optKeySearchIdx != null
              ? state._optKeySearchIdx
              : next.length
                ? next[next.length - 1]
                : 0;
        if (anchor < 0) anchor = 0;
        if (anchor >= n) anchor = n - 1;
        if (!selectedSet.has(anchor) && next.length) anchor = next[next.length - 1];

        if (it.v === 5) {
          next = [];
        } else if (it.v === 6) {
          next = Array.from({ length: n }, (_, i) => i);
        } else if (it.v === 0) {
          for (let i = 0; i <= anchor; ++i) selectedSet.add(i);
          next = Array.from(selectedSet).sort((a, b) => a - b);
        } else if (it.v === 1) {
          for (let i = anchor; i < n; ++i) selectedSet.add(i);
          next = Array.from(selectedSet).sort((a, b) => a - b);
        } else if (it.v === 2) {
          let end = n - 1;
          for (let i = anchor + 1; i < n; ++i) {
            if (isOptKeyOperation(acts[i])) {
              end = i;
              break;
            }
          }
          for (let i = anchor; i <= end; ++i) selectedSet.add(i);
          next = Array.from(selectedSet).sort((a, b) => a - b);
        } else if (it.v === 3) {
          let start = 0;
          for (let i = anchor - 1; i >= 0; --i) {
            if (isOptKeyOperation(acts[i])) {
              start = i;
              break;
            }
          }
          for (let i = start; i <= anchor; ++i) selectedSet.add(i);
          next = Array.from(selectedSet).sort((a, b) => a - b);
        } else if (it.v === 4) {
          if (next.length < 2) {
            toast("请先选择至少两项");
            return;
          }
          const sorted = next.slice().sort((a, b) => a - b);
          next = [];
          for (let i = sorted[0]; i <= sorted[sorted.length - 1]; ++i) next.push(i);
        }
        state.optSelected = next;
        syncOptListSelectionUi();
      });
    });

    $("#btnOcrInstall")?.addEventListener("click", () => {
      const btn = $("#btnOcrInstall");
      // 安装成功后主按钮变为「完成」：只关窗，勿再跑安装
      if (btn && (btn.textContent || "").trim() === "完成" && !state._ocrBusy) {
        closeAllOv({ keepAgent: true });
        return;
      }
      if (!window.qst) {
        toast("无桥接环境");
        return;
      }
      if (state._ocrBusy) return;
      const repair = !!state._ocrRepair;
      state._ocrBusy = true;
      if ($("#ocrDlgTitle"))
        $("#ocrDlgTitle").childNodes[0].textContent = repair
          ? "键鼠工坊-插件修复 "
          : "键鼠工坊-插件安装 ";
      if ($("#ocrStatus"))
        $("#ocrStatus").textContent = repair ? "正在准备修复…" : "正在准备安装…";
      const bar = $("#ocrBar");
      const track = bar && bar.parentElement;
      if (track) track.classList.remove("indeterminate");
      if (bar) bar.style.width = "0%";
      if (btn) {
        btn.disabled = true;
        btn.textContent = "安装中...";
      }
      if ($("#btnOcrRepair")) $("#btnOcrRepair").disabled = true;
      qst.installOcr(repair);
    });
    $("#btnOcrRepair")?.addEventListener("click", () => {
      if (state._ocrBusy) return;
      state._ocrRepair = true;
      if ($("#ocrDlgTitle"))
        $("#ocrDlgTitle").childNodes[0].textContent = "键鼠工坊-插件修复 ";
      if ($("#btnOcrInstall")) $("#btnOcrInstall").textContent = "修复/更新";
      if ($("#ocrStatus"))
        $("#ocrStatus").textContent = "已就绪，点击下方按钮开始修复/更新…";
      $("#btnOcrInstall")?.click();
    });

    // 调试窗勾选：产品路径 Web #debugFloat（引擎推送 debugWindow.*）
    $("#setDebugWin")?.addEventListener("click", () => {
      setTimeout(() => {
        const on = !!$("#setDebugWin")?.classList.contains("on");
        if (!window.qst) {
          if (on) showDebugFloat();
          else hideDebugFloat();
          return;
        }
        quietSaveSettings({ enableDebugOutputWindow: on ? true : false });
        if (on) {
          hideDebugFloat();
          qst.showDebugWindow();
        } else {
          hideDebugFloat();
          quietSaveSettings({ enableDebugOutputWindow: false });
          // 关设置时引擎 Hide → debugWindow.hide → 壳藏独立窗
          qst.showDebugWindow();
        }
      }, 0);
    });

    $("#btnDebugClose")?.addEventListener("click", (e) => {
      e.stopPropagation();
      closeDebugFloatByUser();
    });
    $("#btnDebugMin")?.addEventListener("click", (e) => {
      e.stopPropagation();
      _debugMin = !_debugMin;
      $("#debugFloat")?.classList.toggle("minimized", _debugMin);
    });
    $("#btnDebugPin")?.addEventListener("click", (e) => {
      e.stopPropagation();
      _debugPinned = !_debugPinned;
      $("#btnDebugPin")?.classList.toggle("on", _debugPinned);
      if (window.qst) qst.debugWindowSetTopmost(_debugPinned);
    });
    // 标题栏拖动浮层
    (() => {
      const bar = $("#debugTitleDrag");
      const box = $("#debugFloat");
      if (!bar || !box) return;
      let dragging = false;
      let ox = 0;
      let oy = 0;
      bar.addEventListener("pointerdown", (e) => {
        if (e.button !== 0) return;
        if (e.target.closest(".win-btn")) return;
        dragging = true;
        const r = box.getBoundingClientRect();
        ox = e.clientX - r.left;
        oy = e.clientY - r.top;
        bar.setPointerCapture(e.pointerId);
      });
      bar.addEventListener("pointermove", (e) => {
        if (!dragging) return;
        const x = Math.max(0, Math.min(window.innerWidth - 80, e.clientX - ox));
        const y = Math.max(0, Math.min(window.innerHeight - 40, e.clientY - oy));
        box.style.left = x + "px";
        box.style.top = y + "px";
        box.style.right = "auto";
        box.style.bottom = "auto";
      });
      bar.addEventListener("pointerup", () => {
        dragging = false;
      });
    })();

    // edClassBtn / btnFixedCrosshair：已在上方 wireCrosshairPointerDown

    document.querySelectorAll('.set-pane[data-pane="play"] .radio[data-backend]').forEach((r) => {
      r.addEventListener("click", () => {
        document
          .querySelectorAll('.set-pane[data-pane="play"] .radio[data-backend]')
          .forEach((x) => x.classList.remove("on"));
        r.classList.add("on");
        // 选虚拟 HID 时即时探测：未装驱动则引导一键安装
        if ((r.dataset.backend | 0) === 2) refreshVhidStatus("backend");
      });
    });
    $$("#setSchedPrioRadios .radio").forEach((r) => {
      r.addEventListener("click", () => {
        $$("#setSchedPrioRadios .radio").forEach((x) => x.classList.remove("on"));
        r.classList.add("on");
      });
    });
    $$("#edSetViewRadios .radio").forEach((r) => {
      r.addEventListener("click", () => {
        $$("#edSetViewRadios .radio").forEach((x) => x.classList.remove("on"));
        r.classList.add("on");
      });
    });

    // 点击设置：定点准星已 wireCrosshairPointerDown(#btnFixedCrosshair)

    // AI 模型增删
    const aiPane = document.querySelector('.set-pane[data-pane="ai"]');
    const aiHeadBtns = aiPane ? $$(".panel-head .btn", aiPane) : [];
    if (aiHeadBtns[0]) {
      aiHeadBtns[0].addEventListener("click", () => {
        const profile = {
          apiUrl: ($("#aiApiUrl")?.textContent || "").trim(),
          apiKey: ($("#aiApiKey")?.textContent || "").trim(),
          modelName: ($("#aiModelName")?.textContent || "").trim() || "model",
          temperature: parseFloat($("#aiTemp")?.textContent || "0.3") || 0.3,
          maxTokens: parseInt($("#aiTokens")?.textContent || "4096", 10) || 4096,
        };
        state._aiSavedModels = Array.isArray(state._aiSavedModels)
          ? state._aiSavedModels.slice()
          : [];
        state._aiSavedModels.push(profile);
        applyAiProfileToForm(profile);
        refreshAiModelCombos();
        persistAiSavedModels();
        toast("已添加并保存模型");
      });
    }
    if (aiHeadBtns[1]) {
      aiHeadBtns[1].addEventListener("click", () => {
        const name = ($("#aiModelName")?.textContent || "").trim();
        state._aiSavedModels = (state._aiSavedModels || []).filter(
          (m) => (m.modelName || "") !== name
        );
        refreshAiModelCombos();
        persistAiSavedModels();
        toast("已删除并保存模型");
      });
    }

    $("#btnRunStop")?.addEventListener("click", () => {
      if (!window.qst) return;
      if (state.macroRunning) qst.stopScript();
      else if (state.clicking) qst.stopClicker();
      else if (state.recording) qst.stopRecord();
    });
    $("#btnRunPreview")?.addEventListener("click", () => {
      if (!window.qst) return;
      if (state.macroRunning) {
        if ((state._runningMode | 0) <= 0) {
          toast("正在运行的脚本未启用窗口模式");
          return;
        }
        qst.showWindowModePreview({ fromRunning: 1 });
        return;
      }
      if ((state.editorMode | 0) === 0 && !(state.windowMode && state.windowMode.enabled)) {
        toast("请先在编辑器启用窗口模式");
        return;
      }
      qst.showWindowModePreview({
        editorMode: state.editorMode | 0,
        windowMode: collectWindowModeForSave(),
      });
    });
    $("#wmPreview")?.addEventListener("click", () => {
      if (window.qst) qst.hideWindowModePreview();
      else $("#wmPreview")?.classList.remove("show");
    });

    $("#themeCombo")?.addEventListener("click", (e) => {
      e.stopPropagation();
      const items = (state.themes || []).map((t) => ({ t: t.name, v: t.id }));
      items.push({ t: "自定义…", v: -1 });
      showPopup(e.currentTarget, items, (it) => {
        if (it.v < 0) {
          syncThemeColorInputs();
          openOv("theme");
          return;
        }
        state.themeId = it.v;
        state.useCustomTheme = false;
        if (window.qst) qst.applyTheme({ themeId: it.v, useCustomTheme: 0 });
      });
    });

    if (window.qst) {
      if (AGENT_SHELL) {
        qst.openSettingsData();
        qst.listAgentConversations();
        if (typeof qst.themeCatalog === "function") qst.themeCatalog();
      } else {
      qst.listScripts();
      qst.listRecordings();
      qst.listAgentConversations();
      qst.listScheduledTasks();
      qst.getClickerStatus();
      qst.getEngineStatus();
      qst.openSettingsData();
      qst.getAppBranding();
      qst.getGlobalHotkey();
      qst.themeCatalog();
      // 启动即按主页设计稿比例缩放；收到 window.clientSize 后再校正
      if (!state._clientW) {
        state._clientW = SHELL_CLIENT.home.w;
        state._clientH = SHELL_CLIENT.home.h;
      }
      applyShellScale();
      qst.setMode("home");
      }
    } else {
      renderLists();
      updateCtas();
      applyShellScale();
    }

    document.addEventListener("pointerdown", (e) => {
      if (!popupEl) return;
      if (performance.now() < popupIgnoreCloseUntil) return;
      const t = e.target;
      if (popupEl.contains(t)) return;
      if (t && t.closest && t.closest(".popup-menu")) return;
      if (t && t.closest && (t.closest(".combo") || t.closest(".mode-pill"))) return;
      hidePopup();
    }); // bubble：让菜单捕获阶段先完成选中，避免点选项却被先关掉

  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }

  // 专业模式（pro-mode.js）专用桥：只暴露只读数据与既有动作入口，不与极简模式内部状态耦合
  window.AppShell = {
    state,
    applyUiMode,
    syncUiModeRadios,
    $,
    $$,
    esc,
    escAttr,
    toast,
    setChk,
    openOv,
    setTab,
    setSettingsTab,
    updateCtas,
    syncEngineHomeSelection,
    syncEngineHomeSelectionDebounced,
    enterEditor,
    askConfirm,
    askInput,
    openRename,
    openHotkeyFor,
    openGlobalHotkeyCapture,
    itemPath,
    openAgentSession,
    openSchedEditor,
    toggleSchedTaskStatus,
    ACTION_TYPES,
    displayActionName,
    clampIndent,
    subtreeEnd,
    isSubtreeContainerType,
    insertEditorAction,
    moveActionSubtree,
    deleteSubtreeAt,
    defaultAction,
    renderEditorActions,
    renderParamPanel,
    loadEditDraftFromSelection,
    updateEditorSelectionUi,
    selectEditorAction,
    startDebugFrom,
    showPopup,
    showAddTypePreview,
    enterEditorBatch,
    setEditorVisualSelection,
    mergeVisualSelection,
    commitModifySelected,
    syncEdRemarkFromSelection,
    editorParamAction,
    readParamPanelInto,
    pushEditorHistory,
    undoEditorHistory,
    redoEditorHistory,
    revertLastEditorHistory,
    discardEditorHistory,
  };
})();
