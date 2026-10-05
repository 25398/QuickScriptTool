/**
 * website/export/qst-web-export.js
 * ══════════════════════════════════════════════════════════════════
 * 纯浏览器端「在线写脚本 → 导出独立 EXE」的核心实现。
 *
 * 一句话原理
 * ──────────
 * 导出 = **固定播放器模板 + 尾部追加脚本包**（与本仓 `src/script_package.cpp`
 * 的 `AppendPayloadToExe` 完全同一套格式）：
 *
 *     [QstPlayer.exe][payload.zip][32 字节尾标]
 *     尾标 = "QSTPKG01"(8) + payloadOffset(u64 LE) + payloadSize(u64 LE) + FNV1a64(u64 LE)
 *
 * 所以**不需要编译器、不需要任何服务端算力**：网页 `fetch` 模板 → 拼 ZIP → 追加 →
 * `Blob` 下载。服务器只发一个静态文件，0 CPU / 0 临时文件 / 0 用户数据落盘。
 *
 * ⚠ 三条不能想当然的约束（都来自读端的真实实现，不是猜的）
 * ────────────────────────────────────────────────────────────
 * 1) **payload 必须是 stored（method 0）ZIP**。读端是 `src/utils.cpp` 的
 *    `ExtractZipFile`，它按 `compSize` **原样拷贝字节、根本不解压** ——
 *    压缩过的条目会被当成"就是这些字节"写出去，产物直接坏掉。
 *    好处是网页端连压缩库都不需要（见 `buildStoredZip`）。
 *
 * 2) **坐标在文件里是归一化小数（0~1），不是像素**。见 `src/script_io.cpp`：
 *    文件带 `coordMeta` 时，动作的 `x`/`y` 被当作 `nx`/`ny` 读；
 *    执行时 `x_exec = vsX + round(nx * 当前屏幕宽)`（`src/coord_space.cpp`）。
 *    ⇒ 写像素进去会被 `ScriptNormValuesLookLikePixels` 判成旧格式并**重新解释**，
 *      点击位置整个跑偏。本模块统一在 `normalizePoint` 里换算。
 *
 * 3) **FNV-1a 的 offset basis 是本仓自有的 `1469598103934665603`**，
 *    不是标准 FNV 的 `14695981039346656037`（少一位）。必须逐字一致，
 *    否则尾标哈希与 C++ 不同 —— 运行时目录名会变（不致命），但会让
 *    "同一个脚本重复导出命中同一个运行目录"这条性质失效，且难以察觉。
 *
 * 谁在用
 * ──────
 *   · `website/export/export.js`  —— 独立「在线脚本工坊」页
 *   · `website/demo/bridge.stub.js` —— 产品 Demo 里的「导出为独立 EXE」对话框
 * 两边共用本文件，保证"网页导出的 exe"与"软件导出的 exe"是同一种产物。
 *
 * 本文件不依赖任何第三方库，可在 Node 下 require（便于离线验证产物）。
 * ══════════════════════════════════════════════════════════════════
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) {
    module.exports = factory();
  } else {
    root.QstWebExport = factory();
  }
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  // ── 格式常量（与本仓 C++ 一一对应，改名/改值前先去那边核对）──────
  var TAIL_MAGIC = "QSTPKG01";           // src/script_package.cpp: kPayloadMagic
  var TAIL_SIZE = 32;                    // 8 magic + 8 offset + 8 size + 8 hash
  var FNV_OFFSET_BASIS = 1469598103934665603n;   // ⚠ 见文件头说明 3
  var FNV_PRIME = 1099511628211n;
  var MASK64 = 0xFFFFFFFFFFFFFFFFn;
  var SCRIPT_SCHEMA_VERSION = 2;         // src/script_io.h: kScriptSchemaVersion
  var INPUT_TIMING_VERSION = 2;          // src/recorder_timeline.h: kInputTimingVersionExplicitWaits
  var REF_WIDTH = 2560;                  // src/coord_space.cpp: StandardScriptCoordMeta
  var REF_HEIGHT = 1440;
  var ROOT_ENTRY = "script.json";
  var MANIFEST_ENTRY = "package.json";
  var MAX_ACTIONS = 128;                 // 网页版硬上限（用户可感知的产品约束）
  var MAX_CLICK_COUNT = 100000;          // src/script_io.cpp: clickCount 上限 100000
  var MAX_IMAGES = 8;

  /** 版本号只用于界面展示与排查，不参与格式。 */
  var VERSION = "1.0.0";

  // ══════════════════════════════════════════════════════════════
  // ① 动作白名单（网页版支持的动作）
  // ══════════════════════════════════════════════════════════════
  // `repeat` 表示该动作在脚本文件里走「clickCount + duration（重复间隔）」这条语义
  // （见 src/script_action_builder.cpp 的「★ 重复间隔语义」）：
  //   clickCount = 执行次数，duration = **相邻两次之间**的间隔；
  //   次数为 1 时完全不等待，首前/末后也不插等待。
  // 不支持 clickCount 的动作要重复，只能用 loop + wait 包起来。
  var ACTIONS = {
    mouseClick: {
      label: "鼠标点击", group: "鼠标", repeat: true,
      hint: "在屏幕指定位置按下并松开鼠标键",
    },
    moveMouse: {
      label: "移动鼠标", group: "鼠标", repeat: false,
      hint: "只把光标移到指定位置，不点击",
    },
    mouseDown: {
      label: "鼠标按下", group: "鼠标", repeat: false,
      hint: "按住不放（配合「鼠标松开」做拖拽/长按）",
    },
    mouseUp: {
      label: "鼠标松开", group: "鼠标", repeat: false,
      hint: "松开之前按住的鼠标键",
    },
    scrollWheel: {
      label: "滚动滚轮", group: "鼠标", repeat: true,
      hint: "向上/向下滚动若干格",
    },
    keyClick: {
      label: "按键点击", group: "键盘", repeat: true,
      hint: "按下并松开一个按键",
    },
    keyDown: {
      label: "按键按下", group: "键盘", repeat: false,
      hint: "按住不放（配合「按键松开」做长按/走位）",
    },
    keyUp: {
      label: "按键松开", group: "键盘", repeat: false,
      hint: "松开之前按住的按键",
    },
    quickInput: {
      label: "输入文本", group: "键盘", repeat: false,
      hint: "按字符输入一段文本（走 Unicode，不受输入法影响）",
    },
    wait: {
      label: "等待", group: "流程", repeat: false,
      hint: "原地等待指定秒数",
    },
    findImage: {
      label: "找图点击", group: "识别", repeat: false,
      hint: "在屏幕上找一张模板图，命中后点击/移动（需目标电脑装有图像识别组件）",
    },
    stopMacro: {
      label: "结束脚本", group: "流程", repeat: false,
      hint: "立刻结束整个脚本（放在最后可让「一直重复」的脚本有个出口）",
    },
  };

  /** 鼠标键 → 与 C++ `button` 字段一致的字符串。 */
  var BUTTONS = {
    left: "左键",
    right: "右键",
    middle: "中键",
    x1: "侧键1",
    x2: "侧键2",
  };

  // ══════════════════════════════════════════════════════════════
  // ② 虚拟键码表（照抄 src/utils.cpp 的 VkName，保证 keyText 显示一致）
  // ══════════════════════════════════════════════════════════════
  // ⚠ `keyVk` 才是真正生效的字段：`VirtualKeyFromKeyText` 只认方向键与「空格键」，
  //   字母/数字/F 键都必须靠 keyVk（见 src/utils.cpp: NormalizeScriptKeyVk）。
  var VK_NAMES = (function () {
    var m = {};
    for (var c = 65; c <= 90; c++) m[c] = String.fromCharCode(c);          // A-Z
    for (var d = 48; d <= 57; d++) m[d] = String.fromCharCode(d);          // 0-9
    for (var f = 1; f <= 24; f++) m[0x70 + f - 1] = "F" + f;               // F1-F24
    var named = {
      0x08: "Backspace", 0x09: "Tab", 0x0d: "Enter", 0x1b: "Esc", 0x20: "空格键",
      0x14: "CapsLock", 0x90: "NumLock", 0x91: "ScrollLock", 0x13: "Pause",
      0x2c: "截屏键", 0x2d: "Ins", 0x2e: "Del", 0x24: "Home", 0x23: "End",
      0x21: "PgUp", 0x22: "PgDn", 0x25: "←", 0x26: "↑", 0x27: "→", 0x28: "↓",
      0x5b: "LWin", 0x5c: "RWin", 0xa2: "LCtrl", 0xa3: "RCtrl",
      0xa4: "LAlt", 0xa5: "RAlt", 0xa0: "LShift", 0xa1: "RShift", 0x5d: "Apps",
      0x6a: "Num*", 0x6b: "Num+", 0x6d: "Num-", 0x6e: "Num.", 0x6f: "Num/",
      0xba: ";", 0xbb: "=", 0xbc: ",", 0xbd: "-", 0xbe: ".", 0xbf: "/",
      0xc0: "`", 0xdb: "[", 0xdc: "\\", 0xdd: "]", 0xde: "'", 0x0c: "Clear",
    };
    for (var k in named) if (Object.prototype.hasOwnProperty.call(named, k)) m[k] = named[k];
    for (var n = 0; n <= 9; n++) m[0x60 + n] = "Num" + n;                  // 小键盘
    return m;
  })();

  /** 键码 → 产品里的显示名（与 VkName 一致；未知码回落成「按键NN」）。 */
  function vkName(vk) {
    return VK_NAMES[vk] || ("按键" + vk);
  }

  /** 界面下拉里可选的键（常用集，够覆盖用户需求，且不至于让人翻不到）。 */
  var KEY_CHOICES = [
    { vk: 0x20, label: "空格键" }, { vk: 0x0d, label: "Enter" },
    { vk: 0x1b, label: "Esc" }, { vk: 0x09, label: "Tab" },
    { vk: 0x08, label: "Backspace" }, { vk: 0x2e, label: "Del" },
    { vk: 0x25, label: "←" }, { vk: 0x26, label: "↑" },
    { vk: 0x27, label: "→" }, { vk: 0x28, label: "↓" },
    { vk: 0x24, label: "Home" }, { vk: 0x23, label: "End" },
    { vk: 0x21, label: "PgUp" }, { vk: 0x22, label: "PgDn" },
    { vk: 0xa0, label: "LShift" }, { vk: 0xa2, label: "LCtrl" }, { vk: 0xa4, label: "LAlt" },
  ].concat((function () {
    var out = [];
    for (var c = 65; c <= 90; c++) out.push({ vk: c, label: String.fromCharCode(c) });
    for (var d = 48; d <= 57; d++) out.push({ vk: d, label: String.fromCharCode(d) });
    for (var f = 1; f <= 12; f++) out.push({ vk: 0x70 + f - 1, label: "F" + f });
    return out;
  })());

  // ══════════════════════════════════════════════════════════════
  // ③ 小工具：CRC32 / FNV-1a 64 / 字节写入
  // ══════════════════════════════════════════════════════════════

  var CRC_TABLE = (function () {
    var t = new Uint32Array(256);
    for (var i = 0; i < 256; i++) {
      var c = i;
      for (var j = 0; j < 8; j++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
      t[i] = c >>> 0;
    }
    return t;
  })();

  /** 与 src/utils.cpp 的 ComputeCrc32 逐位一致的 CRC-32（IEEE 802.3）。 */
  function crc32(bytes) {
    var crc = 0xFFFFFFFF;
    for (var i = 0; i < bytes.length; i++) {
      crc = (crc >>> 8) ^ CRC_TABLE[(crc ^ bytes[i]) & 0xFF];
    }
    return (crc ^ 0xFFFFFFFF) >>> 0;
  }

  /**
   * FNV-1a 64。⚠ offset basis 用本仓自有常量（见文件头说明 3）。
   * 返回 BigInt（u64 超出 Number 安全整数范围，不能用 Number）。
   */
  function fnv1a64(bytes) {
    var h = FNV_OFFSET_BASIS;
    for (var i = 0; i < bytes.length; i++) {
      h = ((h ^ BigInt(bytes[i])) * FNV_PRIME) & MASK64;
    }
    return h;
  }

  function utf8Bytes(str) {
    if (typeof TextEncoder !== "undefined") return new TextEncoder().encode(str);
    return new Uint8Array(Buffer.from(str, "utf8"));   // Node 兜底
  }

  function writeU16(view, at, v) { view.setUint16(at, v & 0xFFFF, true); }
  function writeU32(view, at, v) { view.setUint32(at, v >>> 0, true); }
  function writeU64(view, at, v) {
    view.setUint32(at, Number(v & 0xFFFFFFFFn), true);
    view.setUint32(at + 4, Number((v >> 32n) & 0xFFFFFFFFn), true);
  }

  // ══════════════════════════════════════════════════════════════
  // ④ 条目名净化（对齐 src/script_package.cpp: SanitizeEntryFileName）
  // ══════════════════════════════════════════════════════════════
  function sanitizeEntryName(name) {
    var out = String(name == null ? "" : name).trim();
    var slash = Math.max(out.lastIndexOf("\\"), out.lastIndexOf("/"));
    if (slash >= 0) out = out.slice(slash + 1);
    out = out.replace(/[\u0000-\u001f<>:"/\\|?*]/g, "_");
    while (out.length && (out.slice(-1) === "." || out.slice(-1) === " ")) out = out.slice(0, -1);
    return out;
  }

  // ══════════════════════════════════════════════════════════════
  // ⑤ stored ZIP 打包
  // ══════════════════════════════════════════════════════════════
  /**
   * 生成一个 **全部 stored（method 0，不压缩）** 的 ZIP。
   * 为什么必须不压缩：读端 `ExtractZipFile` 按 compSize 原样拷贝，不解压。
   *
   * @param {Array<{name:string, data:Uint8Array|string}>} entries
   * @returns {Uint8Array}
   */
  function buildStoredZip(entries) {
    var prepared = entries.map(function (e) {
      var nameBytes = utf8Bytes(e.name);
      var data = (typeof e.data === "string") ? utf8Bytes(e.data) : e.data;
      if (!(data instanceof Uint8Array)) data = new Uint8Array(data);
      return { nameBytes: nameBytes, data: data, crc: crc32(data) };
    });

    var localSize = 0, cdSize = 0;
    prepared.forEach(function (p) {
      localSize += 30 + p.nameBytes.length + p.data.length;
      cdSize += 46 + p.nameBytes.length;
    });

    var total = localSize + cdSize + 22;
    var out = new Uint8Array(total);
    var view = new DataView(out.buffer);
    var offset = 0;
    var localOffsets = [];

    prepared.forEach(function (p) {
      localOffsets.push(offset);
      writeU32(view, offset + 0, 0x04034b50);
      writeU16(view, offset + 4, 20);          // versionNeeded
      writeU16(view, offset + 6, 0);           // flags：不带 UTF-8 位，与本仓写出端一致
      writeU16(view, offset + 8, 0);           // method = 0 (stored)
      writeU16(view, offset + 10, 0);          // modTime
      writeU16(view, offset + 12, 0);          // modDate
      writeU32(view, offset + 14, p.crc);
      writeU32(view, offset + 18, p.data.length);
      writeU32(view, offset + 22, p.data.length);
      writeU16(view, offset + 26, p.nameBytes.length);
      writeU16(view, offset + 28, 0);          // extraLen
      out.set(p.nameBytes, offset + 30);
      out.set(p.data, offset + 30 + p.nameBytes.length);
      offset += 30 + p.nameBytes.length + p.data.length;
    });

    var cdStart = offset;
    prepared.forEach(function (p, i) {
      writeU32(view, offset + 0, 0x02014b50);
      writeU16(view, offset + 4, 20);          // versionMadeBy
      writeU16(view, offset + 6, 20);          // versionNeeded
      writeU16(view, offset + 8, 0);           // flags
      writeU16(view, offset + 10, 0);          // method
      writeU16(view, offset + 12, 0);          // modTime
      writeU16(view, offset + 14, 0);          // modDate
      writeU32(view, offset + 16, p.crc);
      writeU32(view, offset + 20, p.data.length);
      writeU32(view, offset + 24, p.data.length);
      writeU16(view, offset + 28, p.nameBytes.length);
      writeU16(view, offset + 30, 0);          // extraLen
      writeU16(view, offset + 32, 0);          // commentLen
      writeU16(view, offset + 34, 0);          // diskStart
      writeU16(view, offset + 36, 0);          // internalAttr
      writeU32(view, offset + 38, 0);          // externalAttr
      writeU32(view, offset + 42, localOffsets[i]);
      out.set(p.nameBytes, offset + 46);
      offset += 46 + p.nameBytes.length;
    });

    writeU32(view, offset + 0, 0x06054b50);
    writeU16(view, offset + 4, 0);
    writeU16(view, offset + 6, 0);
    writeU16(view, offset + 8, prepared.length);
    writeU16(view, offset + 10, prepared.length);
    writeU32(view, offset + 12, cdSize);
    writeU32(view, offset + 16, cdStart);
    writeU16(view, offset + 20, 0);
    return out;
  }

  // ══════════════════════════════════════════════════════════════
  // ⑥ 坐标归一化
  // ══════════════════════════════════════════════════════════════
  /**
   * 像素 → 文件里的归一化小数。见文件头说明 2。
   * @param {number} px 屏幕绝对像素
   * @param {number} span 屏幕宽或高（虚拟屏尺寸）
   */
  function normalizePoint(px, span) {
    if (!(span > 0)) return 0;
    var v = Number(px) / Number(span);
    if (!isFinite(v)) return 0;
    // 留一点余量给「边界外一点点」的写法（引擎容忍 nx>1，但 >1.5 会被判成旧像素格式）
    return Math.max(-0.5, Math.min(1.5, v));
  }

  function fractionToPx(frac, span) {
    var v = Math.round(Number(frac) * Number(span));
    return isFinite(v) ? v : 0;
  }

  // ══════════════════════════════════════════════════════════════
  // ⑦ UI 动作 → 脚本动作（flat + indent）
  // ══════════════════════════════════════════════════════════════
  function baseAction(type) {
    return {
      type: type, text: "", remark: "", indent: 0,
      x: 0, y: 0, randomX: 0, randomY: 0,
      endX: 0, endY: 0, randomEndX: 0, randomEndY: 0,
      button: "left",
      keyText: "", keyVk: 0,
      holdLeftWin: 0, holdRightWin: 0, holdLeftCtrl: 0, holdRightCtrl: 0,
      holdLeftAlt: 0, holdRightAlt: 0, holdLeftShift: 0, holdRightShift: 0,
      clickCount: 1, duration: 0.01, randomDuration: 0,
      loopCount: 1,
      inputText: "", charInterval: 0.01, parseEscapes: 0,
      scrollVertical: 1, scrollHorizontal: 0, scrollSteps: 1, scrollDirection: 0,
      searchFullScreen: 1,
      imagePath: "", imageUseVar: 0,
      matchThreshold: 65, perfectMatch: 0,
      imageScale: 1, imageScaleMin: 1, imageScaleMax: 1,
      findImageFollowUp: 0, offsetX: 0, offsetY: 0,
      findUntilFound: 0, findTimeExpr: "0", matchVarName: "matchRet",
    };
  }

  function clampInt(v, lo, hi, dflt) {
    var n = parseInt(v, 10);
    if (!isFinite(n)) return dflt;
    return Math.max(lo, Math.min(hi, n));
  }
  function clampNum(v, lo, hi, dflt) {
    var n = parseFloat(v);
    if (!isFinite(n)) return dflt;
    return Math.max(lo, Math.min(hi, n));
  }

  /**
   * 把一个 UI 动作展开成 0~n 条脚本动作（flat，indent 已填好）。
   *
   * 重复语义（正是用户要的「每隔 N 秒…」）：
   *   count === 1            → 裸动作（不等待）
   *   count  >  1 且 repeat  → 动作(clickCount=count, duration=interval)
   *                            引擎语义：首前/末后都不等，两次之间等 interval
   *   count === 0（一直）或 !repeat
   *                          → loop(-1 或 count) { 动作; wait interval }
   */
  function expandAction(ui, ctx) {
    var t = ui.type;
    if (!ACTIONS[t]) throw new Error("不支持的动作类型：" + t);
    var count = clampInt(ui.count, 0, MAX_CLICK_COUNT, 1);
    var interval = clampNum(ui.interval, 0, 3600, 0);
    var out = [];

    var body = null;
    if (t === "mouseClick") {
      body = baseAction("mouseClick");
      body.x = normalizePoint(ui.px, ctx.screenW);
      body.y = normalizePoint(ui.py, ctx.screenH);
      body.button = BUTTONS[ui.button] ? ui.button : "left";
      body.clickCount = count > 0 ? count : 1;
      body.duration = interval;
    } else if (t === "moveMouse") {
      body = baseAction("moveMouse");
      body.x = normalizePoint(ui.px, ctx.screenW);
      body.y = normalizePoint(ui.py, ctx.screenH);
      body.duration = 0;
    } else if (t === "mouseDown" || t === "mouseUp") {
      body = baseAction(t);
      body.button = BUTTONS[ui.button] ? ui.button : "left";
    } else if (t === "scrollWheel") {
      body = baseAction("scrollWheel");
      body.scrollDirection = ui.direction === "up" ? 0 : 1;
      body.scrollSteps = clampInt(ui.steps, 1, 100, 3);
      body.clickCount = count > 0 ? count : 1;
      body.duration = interval;
    } else if (t === "keyClick") {
      body = baseAction("keyClick");
      body.keyVk = clampInt(ui.vk, 1, 255, 65);
      body.keyText = vkName(body.keyVk);
      body.clickCount = count > 0 ? count : 1;
      body.duration = interval;
    } else if (t === "keyDown" || t === "keyUp") {
      body = baseAction(t);
      body.keyVk = clampInt(ui.vk, 1, 255, 65);
      body.keyText = vkName(body.keyVk);
    } else if (t === "quickInput") {
      body = baseAction("quickInput");
      body.inputText = String(ui.inputText == null ? "" : ui.inputText);
      body.charInterval = clampNum(ui.charInterval, 0, 10, 0.01);
      body.parseEscapes = ui.parseEscapes ? 1 : 0;
    } else if (t === "wait") {
      body = baseAction("wait");
      body.duration = clampNum(ui.seconds, 0, 86400, 1);
    } else if (t === "findImage") {
      body = baseAction("findImage");
      body.imagePath = String(ui.imageName || "");
      body.matchThreshold = clampInt(ui.threshold, 1, 100, 65);
      body.findImageFollowUp = clampInt(ui.followUp, 0, 1, 0);
      body.offsetX = normalizePoint(ui.offsetX, ctx.screenW);
      body.offsetY = normalizePoint(ui.offsetY, ctx.screenH);
      body.findTimeExpr = "0";
      body.matchVarName = "matchRet";
    } else if (t === "stopMacro") {
      body = baseAction("stopMacro");
      body.duration = 0;
    }

    if (!body) throw new Error("动作 " + t + " 缺实现");

    // ⚠ 三条分支的**顺序**不能换：
    //   count===1 必须最先判，否则下面的"用 clickCount 表达重复"会把单次也写成 clickCount=1
    //   （语义没错但文件里多一个冗余字段，且会掩盖后面 count===0 的漏判）；
    //   而 count===0（一直重复）**不能**落进 count<=1 那一支 —— 那正是第一版的真 bug：
    //   「一直重复的鼠标点击」被展开成"点一次就结束"，网页上说导出成功、双击却只点一下。
    var canRepeatInline = ACTIONS[t].repeat && count > 1;
    if (count === 1 || canRepeatInline) {
      out.push(body);
      return out;
    }

    // loop 形式：0 表示一直重复（引擎 loopCount = -1）
    var loop = baseAction("loop");
    loop.loopCount = count > 0 ? count : -1;
    loop.duration = 0;
    body.indent = 1;
    out.push(loop);
    out.push(body);
    if (interval > 0) {
      var w = baseAction("wait");
      w.indent = 1;
      w.duration = interval;
      out.push(w);
    }
    return out;
  }

  /** UI 动作数组 → 脚本 flat 动作数组（顺带补 `no`、校验上限）。 */
  function buildScriptActions(uiActions, ctx) {
    var list = Array.isArray(uiActions) ? uiActions : [];
    var flat = [];
    for (var i = 0; i < list.length; i++) {
      var part = expandAction(list[i], ctx);
      for (var j = 0; j < part.length; j++) flat.push(part[j]);
    }
    flat.forEach(function (a, idx) { a.no = idx + 1; });
    return flat;
  }

  // ══════════════════════════════════════════════════════════════
  // ⑦b 产品编辑器的动作 → 脚本动作（Demo 里"导出当前编辑的脚本"走这条）
  // ══════════════════════════════════════════════════════════════
  /**
   * 网页 Demo 支持导出的动作（产品编辑器里那些"纯本机、无外部依赖"的）。
   * 其余一律**明确拒绝**并说明原因 —— 不许静默丢掉用户的动作。
   */
  var EDITOR_ALLOWED = {
    mouseClick: 1, moveMouse: 1, mouseDown: 1, mouseUp: 1, scrollWheel: 1,
    keyClick: 1, keyDown: 1, keyUp: 1, quickInput: 1, hotkeyShortcut: 1,
    wait: 1, loop: 1, endLoop: 1, if: 1, else: 1, stopMacro: 1,
  };
  /** 明确拒绝时给用户的理由（按类型）。 */
  var EDITOR_REJECT_REASON = {
    findImage: "找图需要模板图，请到「在线脚本工坊」上传图片后再导出",
    multiMatch: "多图匹配请到「在线脚本工坊」导出",
    watchImage: "找图监视请到「在线脚本工坊」导出",
    textRecognition: "文字识别（OCR）不在网页版导出范围内，请用 Windows 客户端",
    getColor: "取色不在网页版导出范围内，请用 Windows 客户端",
    findColor: "找色不在网页版导出范围内，请用 Windows 客户端",
    colorMatch: "颜色比对不在网页版导出范围内，请用 Windows 客户端",
    mouseDrag: "拖拽不在网页版导出范围内，请用 Windows 客户端",
    runMacro: "嵌套宏不在网页版导出范围内（网页版没有脚本库）",
    runBlock: "自定义块不在网页版导出范围内",
    defineBlock: "自定义块不在网页版导出范围内",
    mousePlayback: "录制回放不在网页版导出范围内（网页版不录制）",
    runProgram: "启动程序不在网页版导出范围内",
    openFile: "打开文件不在网页版导出范围内",
    openWebpage: "打开网页不在网页版导出范围内",
    closeProgram: "关闭程序不在网页版导出范围内",
    activateWindow: "激活窗口不在网页版导出范围内",
    aiTextAnalysis: "AI 动作需要 API Key，网页版导出不包含",
    aiImageAnalysis: "AI 动作需要 API Key，网页版导出不包含",
    aiActionExecute: "AI 动作需要 API Key，网页版导出不包含",
    varCompute: "变量计算不在网页版导出范围内，请用 Windows 客户端",
    goto: "跳转不在网页版导出范围内，请用 Windows 客户端",
    lockScreenshot: "截图锁定不在网页版导出范围内",
    unlockScreenshot: "截图锁定不在网页版导出范围内",
    customText: "未知动作",
  };

  /** 带屏幕坐标的字段（像素 → 归一化）。与 C++ SyncNormFieldsFromPixelsAction 对齐。 */
  var PIXEL_FIELDS = ["x", "y", "randomX", "randomY", "endX", "endY",
    "randomEndX", "randomEndY", "searchX1", "searchY1", "searchX2", "searchY2",
    "offsetX", "offsetY", "imageRegionX1", "imageRegionY1", "imageRegionX2", "imageRegionY2",
    "aiSearchX1", "aiSearchY1", "aiSearchX2", "aiSearchY2"];
  /** 用宽还是高归一化（x 类用宽、y 类用高）。 */
  function pixelSpanFor(field) {
    return /Y\d?$|^y$|offsetY|EndY/i.test(field) ? "h" : "w";
  }

  /**
   * 产品编辑器的 flat 动作（像素坐标 + indent 嵌套）→ 脚本文件动作。
   * @returns {{actions:Array, rejected:Array<{type,reason}>, count:number}}
   */
  function convertEditorActions(editorActions, ctx) {
    var list = Array.isArray(editorActions) ? editorActions : [];
    var out = [], rejected = [];
    list.forEach(function (src) {
      var t = String(src && src.type || "");
      if (!t) return;
      if (!EDITOR_ALLOWED[t]) {
        rejected.push({ type: t, reason: EDITOR_REJECT_REASON[t] || ("网页版不支持「" + t + "」") });
        return;
      }
      var a = baseAction(t);
      // 只搬 baseAction 里认识的字段（白名单），别把编辑器的显示字段（name/selected…）带进脚本
      Object.keys(src).forEach(function (k) {
        if (k === "type" || k === "name" || k === "selected" || k === "children") return;
        if (Object.prototype.hasOwnProperty.call(a, k)) a[k] = src[k];
      });
      a.type = t;
      a.indent = clampInt(src.indent, 0, 8, 0);
      // 坐标：编辑器里是当前屏幕像素，文件里必须是归一化小数（见文件头说明 2）
      PIXEL_FIELDS.forEach(function (f) {
        if (src[f] == null) return;
        var span = pixelSpanFor(f) === "h" ? ctx.screenH : ctx.screenW;
        a[f] = normalizePoint(src[f], span);
      });
      if (t === "keyClick" || t === "keyDown" || t === "keyUp") {
        a.keyVk = clampInt(src.keyVk, 0, 255, a.keyVk);
        if (!a.keyText && a.keyVk) a.keyText = vkName(a.keyVk);
        if (!a.keyVk && a.keyText) a.keyVk = 0;   // 交给引擎按 keyText 处理
      }
      if (t === "mouseClick" || t === "mouseDown" || t === "mouseUp") {
        a.button = BUTTONS[src.button] ? src.button : "left";
      }
      if (t === "mouseClick" || t === "keyClick" || t === "hotkeyShortcut") {
        a.clickCount = clampInt(src.clickCount, 1, MAX_CLICK_COUNT, 1);
        a.duration = clampNum(src.duration, 0, 3600, 0.01);
      }
      if (t === "wait") a.duration = clampNum(src.duration, 0, 86400, 1);
      if (t === "loop") a.loopCount = clampInt(src.loopCount, -1, MAX_CLICK_COUNT, 1);
      out.push(a);
    });
    out.forEach(function (a, i) { a.no = i + 1; });
    return { actions: out, rejected: rejected, count: out.length };
  }


  // ══════════════════════════════════════════════════════════════
  // ⑧ 能力体检（对齐 src/script_package.cpp 的 ScanActionCapabilities）
  // ══════════════════════════════════════════════════════════════
  function scanCapabilities(flatActions, images) {
    var scan = {
      totalActions: flatActions.length,
      imageActions: 0, ocrActions: 0, aiActions: 0,
      externalActions: 0, windowMode: false, hasHotkey: false,
      images: Array.isArray(images) ? images.length : 0,
    };
    flatActions.forEach(function (a) {
      var t = a.type;
      if (t === "findImage" || t === "multiMatch" || t === "watchImage"
          || t === "findColor" || t === "colorMatch") scan.imageActions++;
      else if (t === "getColor" || t === "mouseDrag") { if (a.imageLocate) scan.imageActions++; }
      else if (t === "textRecognition") { scan.ocrActions++; if (a.ocrRegionByImage) scan.imageActions++; }
      else if (t === "aiTextAnalysis" || t === "aiImageAnalysis" || t === "aiActionExecute") scan.aiActions++;
      else if (t === "runProgram" || t === "openFile" || t === "openWebpage") scan.externalActions++;
    });
    scan.needsOpenCv = scan.imageActions > 0;
    scan.needsOcr = scan.ocrActions > 0;
    scan.needsAi = scan.aiActions > 0;
    scan.selfContainedOnly = !scan.needsOpenCv && !scan.needsOcr && !scan.needsAi;
    return scan;
  }

  // ══════════════════════════════════════════════════════════════
  // ⑨ 校验
  // ══════════════════════════════════════════════════════════════
  function validate(uiActions) {
    var list = Array.isArray(uiActions) ? uiActions : [];
    var errors = [], warnings = [];
    if (!list.length) errors.push("脚本里还没有任何动作，先加一条吧。");
    if (list.length > MAX_ACTIONS) {
      errors.push("动作数 " + list.length + " 超过网页版上限 " + MAX_ACTIONS + " 条。");
    }
    var images = 0;
    list.forEach(function (a, i) {
      var at = "第 " + (i + 1) + " 条（" + ((ACTIONS[a.type] || {}).label || a.type) + "）";
      if (!ACTIONS[a.type]) { errors.push(at + " 不是网页版支持的动作。"); return; }
      var count = clampInt(a.count, 0, MAX_CLICK_COUNT, 1);
      if (a.count != null && String(a.count) !== "" && count === 0 && ACTIONS[a.type].repeat === false
          && a.type !== "wait" && a.type !== "findImage" && a.type !== "stopMacro") {
        // 不支持 clickCount 的动作用 loop 表达"一直重复"，是允许的；这里只提示
        warnings.push(at + " 会一直重复执行，请确认这是你要的。");
      }
      if (a.type === "findImage") {
        images++;
        if (!a.imageName) errors.push(at + " 还没选模板图。");
      }
      if (a.type === "quickInput" && !String(a.inputText || "").length) {
        errors.push(at + " 的输入文本是空的。");
      }
      if ((a.type === "mouseClick" || a.type === "moveMouse")
          && (!isFinite(Number(a.px)) || !isFinite(Number(a.py)))) {
        errors.push(at + " 的坐标不合法。");
      }
    });
    if (images > MAX_IMAGES) errors.push("模板图最多 " + MAX_IMAGES + " 张。");
    if (list.length > 0 && list.length > 64) {
      warnings.push("动作较多（" + list.length + " 条），建议先用 2~3 条验证效果再补全。");
    }
    return { ok: errors.length === 0, errors: errors, warnings: warnings };
  }

  // ══════════════════════════════════════════════════════════════
  // ⑩ 脚本文件 JSON
  // ══════════════════════════════════════════════════════════════
  function pad2(n) { return (n < 10 ? "0" : "") + n; }

  function nowStamp() {
    var d = new Date();
    return d.getFullYear() + "/" + pad2(d.getMonth() + 1) + "/" + pad2(d.getDate())
      + " " + pad2(d.getHours()) + ":" + pad2(d.getMinutes()) + ":" + pad2(d.getSeconds());
  }

  /** 与 windowmode::DefaultWindowModeConfig 序列化后的形状一致（全关）。 */
  function defaultWindowMode() {
    return {
      enabled: 0,
      executionKind: "hiddenDesktop",
      targetExePath: "", targetWindowTitle: "",
      coordSpace: "screenAbsolute",
      windowRelativeCoordinates: 0,
      recordClientWidth: 0, recordClientHeight: 0,
      autoLaunchTarget: 0, launchArgs: "",
      selectMethod: "selectOnStartup",
      windowName: "", windowClassName: "", childWindowClassName: "",
      useTopLevelWindow: 1,
      targetPickX: 0, targetPickY: 0,
      allowForegroundInputFallback: 0,
      fakeFocusEnabled: 0,
      inputStrategy: "auto",
      cdpPort: 9222,
    };
  }

  /**
   * 组装 script.json 的文本。
   * @param {{name:string, actions:Array, screenW:number, screenH:number, uiActions:Array}} opts
   *   `uiActions` = 工坊的动作模型（会被展开）；
   *   `actions`   = 已经展开好的 flat 动作（Demo 直接给编辑器动作时用，二选一）。
   */
  function buildScriptFile(opts) {
    var o = opts || {};
    var screenW = Number(o.screenW) > 0 ? Number(o.screenW) : 1920;
    var screenH = Number(o.screenH) > 0 ? Number(o.screenH) : 1080;
    var uiActions = Array.isArray(o.uiActions) ? o.uiActions : [];

    var ctx = { screenW: screenW, screenH: screenH };
    var flat = Array.isArray(o.actions) ? o.actions : buildScriptActions(uiActions, ctx);

    var doc = {
      v: SCRIPT_SCHEMA_VERSION,
      scriptName: String(o.name || "在线脚本"),
      recordTime: nowStamp(),
      inputTimingVersion: INPUT_TIMING_VERSION,
      hotkeyText: "", hotkeyVk: 0, hotkeyModifiers: 0, hotkeyHold: 0,
      breakoutTimeSeconds: 0,
      // ⚠ refWidth/refHeight 恒为 2560×1440（StandardScriptCoordMeta）；
      //   captureWidth/Height 才是"导出时用户屏幕"，只影响模板图缩放。
      coordMeta: {
        version: 1,
        space: "screenVirtual",
        refOriginX: 0, refOriginY: 0,
        refWidth: REF_WIDTH, refHeight: REF_HEIGHT,
        refDpi: 96,
        captureWidth: Math.round(screenW),
        captureHeight: Math.round(screenH),
      },
      windowMode: defaultWindowMode(),
      actions: flat,
    };
    return JSON.stringify(doc, null, 2);
  }

  /** 清单（package.json）。形状对齐 scriptpkg::BuildPackageManifestJson。 */
  function buildManifest(opts) {
    var o = opts || {};
    return JSON.stringify({
      v: 1,
      root: ROOT_ENTRY,
      scripts: [],
      images: (o.images || []).map(function (im) { return im.name; }),
      player: {
        v: 1,
        needOpenCv: o.scan && o.scan.needsOpenCv ? 1 : 0,
        needOcr: o.scan && o.scan.needsOcr ? 1 : 0,
        needFakeFocus: 0,
        // 网页版一律「走软件」：自带要把 64.6 MB 的 opencv_world4100.dll 塞进 payload，
        // 那是发送方的带宽与目标机的磁盘，不是服务器的事 —— 但值得让用户显式选择，
        // 所以这里按 opts.bundledOpenCv 走。
        bundledOpenCv: o.bundledOpenCv ? 1 : 0,
        bundledOcr: 0,
        bundledFakeFocus: 0,
        createdBy: "web-" + VERSION,
      },
    }, null, 2);
  }

  /**
   * 运行时设置快照（`rt/app_settings.json`）。
   *
   * 两条路，按"有没有真设置"分：
   *
   * **① 有完整设置（官网 Demo）→ 原样嵌入。**
   *   与客户端导出**完全一致**：客户端把 `SerializeAppSettings(settings, true)` 的整份
   *   `app_settings.json` 打进 `rt\`，播放器按 `AppDir()\app_settings.json` 读 ⇒
   *   "导出时软件里是什么设置，exe 里就是什么设置"。
   *   用户在 Demo 设置对话框里改的回放次数 / 随机间隔 / 点击上限 / 低性能模式 / 倍速…
   *   必须一起进 exe —— **这是用户 2026-09-28 明确要求的**（此前网页版只带了两个键，
   *   于是"设置里选了什么"和"导出的 exe 怎么跑"是两回事）。
   *   ⚠ 不认识的键会被 `LoadAppSettings` 忽略（它先取 `DefaultAppSettings()` 再逐字段覆盖），
   *     所以多带键不会把别的设置清零；少带键则保持默认 —— 两个方向都安全。
   *
   * **② 没有设置（「在线脚本工坊」页没有设置界面）→ 只钉住"播放次数"。**
   *   ⚠ 这一步**不能省**：`PlaybackTabSettings::enablePlaybackCount` 默认 `false` = **无限循环**
   *     （`src/app_settings.h:48`；引擎在 `engine_script_run.cpp:8000` 按它决定 break）。
   *     包里不带这个文件，任何"线性、跑完就该退出"的脚本都会一直循环下去 ——
   *     而**在浏览器里完全看不出来**（产物能跑、字节也对）。2026-09-28 实测踩到。
   */
  function buildSettingsJson(opts) {
    var o = opts || {};
    // ① 完整设置原样嵌入（Demo 路径）
    if (o.settings && typeof o.settings === "object") {
      return JSON.stringify(o.settings, null, 2);
    }
    // ② 只钉播放次数（工坊路径）
    var loopWhole = !!o.loopWholeScript;
    return JSON.stringify({
      playback: {
        enablePlaybackCount: !loopWhole,
        playbackCount: loopWhole ? 0 : clampInt(o.playbackCount, 1, 100000, 1),
        lowPerformanceMode: !!o.lowPerformanceMode,
      },
    }, null, 2);
  }

  // ══════════════════════════════════════════════════════════════
  // ⑪ payload 与最终 EXE
  // ══════════════════════════════════════════════════════════════
  /**
   * @param {{scriptJson:string, manifestJson:string, images:Array<{name,data}>,
   *          runtimeFiles:Array<{name,data}>}} opts
   */
  function buildPayload(opts) {
    var o = opts || {};
    var entries = [];
    // 顺序与产品导出一致：script.json → 嵌套脚本 → 图片 → 清单；
    // rt\ 前缀的条目会被播放器分发到运行时目录根。
    entries.push({ name: ROOT_ENTRY, data: o.scriptJson });
    (o.images || []).forEach(function (im) {
      entries.push({ name: im.name, data: im.data });
    });
    (o.runtimeFiles || []).forEach(function (f) {
      entries.push({ name: "rt/" + f.name, data: f.data });
    });
    entries.push({ name: MANIFEST_ENTRY, data: o.manifestJson });
    return buildStoredZip(entries);
  }

  /**
   * 把 payload 追加到播放器模板后面并写 32 字节尾标。
   * 与 src/script_package.cpp 的 AppendPayloadToExe 完全同格式。
   */
  function assembleExe(playerBytes, payloadBytes) {
    var player = (playerBytes instanceof Uint8Array) ? playerBytes : new Uint8Array(playerBytes);
    var payload = (payloadBytes instanceof Uint8Array) ? payloadBytes : new Uint8Array(payloadBytes);
    if (!player.length) throw new Error("播放器模板是空的");
    if (!payload.length) throw new Error("脚本包是空的");

    var offset = player.length;
    var out = new Uint8Array(offset + payload.length + TAIL_SIZE);
    out.set(player, 0);
    out.set(payload, offset);

    var tailAt = offset + payload.length;
    for (var i = 0; i < TAIL_MAGIC.length; i++) out[tailAt + i] = TAIL_MAGIC.charCodeAt(i);
    var view = new DataView(out.buffer, out.byteOffset, out.byteLength);
    writeU64(view, tailAt + 8, BigInt(offset));
    writeU64(view, tailAt + 16, BigInt(payload.length));
    writeU64(view, tailAt + 24, fnv1a64(payload));
    return out;
  }

  /** 读回自己写的尾标（自检 / 验证用，与 C++ ReadPayloadInfo 同判据）。 */
  function readTail(exeBytes) {
    var b = exeBytes;
    if (b.length <= TAIL_SIZE) return null;
    var at = b.length - TAIL_SIZE;
    var magic = "";
    for (var i = 0; i < 8; i++) magic += String.fromCharCode(b[at + i]);
    if (magic !== TAIL_MAGIC) return null;
    var view = new DataView(b.buffer, b.byteOffset, b.byteLength);
    var lo = function (o) { return BigInt(view.getUint32(at + o, true)); };
    var hi = function (o) { return BigInt(view.getUint32(at + o + 4, true)); };
    var offset = lo(8) | (hi(8) << 32n);
    var size = lo(16) | (hi(16) << 32n);
    var hash = lo(24) | (hi(24) << 32n);
    return {
      offset: offset, size: size, hash: hash,
      valid: (offset + size + BigInt(TAIL_SIZE)) === BigInt(b.length),
    };
  }

  // ══════════════════════════════════════════════════════════════
  // ⑫ 取模板 / 下载（浏览器侧）
  // ══════════════════════════════════════════════════════════════
  /**
   * 本文件自己的 URL 前缀。
   * ⚠ 模板路径**必须相对脚本自身**解析，不能相对"当前页面"：
   *   工坊页在 `/export/export.html`，若写 `export/player/...` 会解析成
   *   `/export/export/player/...` ⇒ 404（浏览器自检页第一次跑就撞上了）。
   *   相对脚本解析则把站点部署在任意子目录都成立。
   */
  var SCRIPT_BASE = (function () {
    try {
      if (typeof document !== "undefined") {
        var cur = document.currentScript;
        if (cur && cur.src) return cur.src.replace(/[?#].*$/, "").replace(/[^/]*$/, "");
        var all = document.getElementsByTagName("script");
        for (var i = all.length - 1; i >= 0; i--) {
          var src = all[i].src || "";
          if (/qst-web-export\.js/.test(src)) return src.replace(/[?#].*$/, "").replace(/[^/]*$/, "");
        }
      }
    } catch (e) {}
    return "";
  })();

  var PLAYER_URL = SCRIPT_BASE ? (SCRIPT_BASE + "player/QstPlayer.exe") : "export/player/QstPlayer.exe";
  var playerCache = null;

  function setPlayerUrl(url) { PLAYER_URL = String(url); playerCache = null; }

  /** 当前页面是不是用 `file://` 直接打开的（双击 html 文件）。 */
  function isFileProtocol() {
    try {
      return typeof location !== "undefined" && location.protocol === "file:";
    } catch (e) {
      return false;
    }
  }

  /**
   * 把「取模板失败」翻译成**用户能照着做**的话。
   *
   * 为什么要专门做这件事（2026-09-28 用户实测反馈）：
   *   直接双击 `demo/index.html` 打开时，浏览器**禁止 fetch 本地文件**
   *   （file:// 的 fetch 一律失败，且报错只有干巴巴的 `Failed to fetch`），
   *   用户看到的就是一个"导出失败"，完全不知道是"没起服务器"还是"模板没上传"。
   *   这两种成因的处置完全不同，所以必须分开说。
   */
  function describePlayerError(status, target) {
    var where = String(target || PLAYER_URL);
    if (isFileProtocol()) {
      return "导出失败：网页是直接双击打开的（file://），浏览器不允许这样读取模板文件。"
        + "请用本地服务器打开 —— 在仓库根目录运行："
        + " powershell -ExecutionPolicy Bypass -File tools\\serve_website.ps1"
        + " 然后访问它打印出的地址（例如 http://127.0.0.1:8090/demo.html）。";
    }
    if (status === 404) {
      return "导出失败：服务器上找不到播放器模板 " + where
        + "（HTTP 404）。部署时要把 website\\export\\player\\QstPlayer.exe 一起上传 —— "
        + "它被 .gitignore 忽略，不是跟着代码走的；本地可用 tools\\sync_web_export_player.ps1 生成。";
    }
    if (status) {
      return "导出失败：下载播放器模板返回 HTTP " + status + "（" + where + "）。";
    }
    return "导出失败：读不到播放器模板 " + where
      + "。请确认该文件已上传，且页面是通过 http:// 打开的（不是双击文件）。";
  }

  /**
   * 取模板：显式给了字节就用给的，否则按 URL fetch（带进程内缓存 + 进度 + 可中止）。
   */
  function resolvePlayer(o) {
    if (o && o.playerBytes) {
      var b = o.playerBytes;
      report(o.onProgress, { phase: "player", loaded: 1, total: 1, ratio: 1, local: true });
      return Promise.resolve(b instanceof Uint8Array ? b : new Uint8Array(b));
    }
    return fetchPlayer(o && o.playerUrl, { onProgress: o && o.onProgress, signal: o && o.signal });
  }

  /** fetch 播放器模板，进程内缓存（5.5 MB，下载一次够用）。 */
  function fetchPlayer(url, opts) {
    var o = opts || {};
    var target = url || PLAYER_URL;
    if (playerCache && (!url || url === PLAYER_URL)) {
      report(o.onProgress, { phase: "player", loaded: playerCache.length, total: playerCache.length, ratio: 1, cached: true });
      return Promise.resolve(playerCache);
    }
    // file:// 下 fetch 必定失败，不必等它抛一个看不懂的 TypeError
    if (isFileProtocol() && !url) {
      return Promise.reject(new Error(describePlayerError(0, target)));
    }
    report(o.onProgress, { phase: "player", loaded: 0, total: 0, ratio: 0 });
    // ⚠ 缓存策略必须是 `default`，**不能**用 `force-cache`：
    //   force-cache 会无视 HTTP 新鲜度直接用本地副本 ⇒ 发版换了 QstPlayer.exe 之后，
    //   老用户仍然拿旧模板导出 —— 产物照样能跑，只是跑的是**旧引擎**，完全看不出来
    //   （这正是 AGENTS.md 里"模板与产品脱节"那类静默事故）。
    //   用 default 让 nginx 的 ETag/Last-Modified 正常生效：跨发版必然拿到新的，
    //   同一次发版内则是廉价的 304。页面内的 playerCache 仍然保证"一次加载只下一次"。
    return fetch(target, { cache: "default", signal: o.signal }).then(function (res) {
      if (!res.ok) throw new Error(describePlayerError(res.status, target));
      var total = 0;
      try { total = Number(res.headers.get("content-length")) || 0; } catch (e) { total = 0; }
      // ⚠ 必须**流式**读，不能直接 res.arrayBuffer()：
      //   5.5 MB 在慢网络上要几秒，而 arrayBuffer() 期间拿不到任何进度 ——
      //   用户只看到一个卡住的按钮（这正是用户反馈"不如客户端迅速"的观感来源：
      //   组装本身是毫秒级，慢的是这一段下载）。
      if (!res.body || typeof res.body.getReader !== "function") {
        return res.arrayBuffer().then(function (buf) {
          report(o.onProgress, { phase: "player", loaded: buf.byteLength, total: buf.byteLength, ratio: 1 });
          return buf;
        });
      }
      var reader = res.body.getReader();
      var chunks = [];
      var loaded = 0;
      return (function pump() {
        return reader.read().then(function (r) {
          if (r.done) {
            var out = new Uint8Array(loaded);
            var at = 0;
            chunks.forEach(function (c) { out.set(c, at); at += c.length; });
            report(o.onProgress, { phase: "player", loaded: loaded, total: total || loaded, ratio: 1 });
            return out.buffer;
          }
          chunks.push(r.value);
          loaded += r.value.length;
          report(o.onProgress, {
            phase: "player", loaded: loaded, total: total,
            ratio: total > 0 ? Math.min(1, loaded / total) : 0,
          });
          return pump();
        });
      })();
    }, function (netErr) {
      if (isAbort(netErr)) throw abortError();
      // 网络层失败（离线 / CORS / file:// / 被扩展拦）
      var e = new Error(describePlayerError(0, target));
      e.cause = netErr;
      throw e;
    }).then(function (buf) {
      var bytes = new Uint8Array(buf);
      // 模板必须是一个 PE（MZ 头）。防"服务器 404 页面被当模板"这类静默错误。
      if (bytes.length < 1024 || bytes[0] !== 0x4D || bytes[1] !== 0x5A) {
        throw new Error("导出失败：播放器模板不是有效的 exe（" + target
          + "）。多半是服务器把它当文本改了，或上传的是别的东西。");
      }
      if (url === PLAYER_URL || !url) playerCache = bytes;
      return bytes;
    });
  }

  // ── 进度 / 中止 ───────────────────────────────────────────────
  /** 中止错误（各 UI 用同一个判据识别，别各写各的）。 */
  function abortError() {
    var e = new Error("已取消导出");
    e.name = "AbortError";
    e.aborted = true;
    return e;
  }
  function isAbort(err) {
    return !!err && (err.name === "AbortError" || err.aborted === true
      || (typeof err === "object" && err.code === 20));
  }
  function throwIfAborted(signal) {
    if (signal && signal.aborted) throw abortError();
  }
  /** 进度回调是外部传进来的，绝不能因为 UI 抛异常把导出带崩。 */
  function report(fn, info) {
    if (typeof fn !== "function") return;
    try { fn(info); } catch (e) { /* UI 的错不该影响导出 */ }
  }

  /** 触发浏览器下载。 */
  function downloadBytes(bytes, filename, mime) {
    var blob = new Blob([bytes], { type: mime || "application/octet-stream" });
    var url = URL.createObjectURL(blob);
    var a = document.createElement("a");
    a.href = url;
    a.download = filename;
    a.style.display = "none";
    document.body.appendChild(a);
    a.click();
    setTimeout(function () {
      try { document.body.removeChild(a); } catch (e) {}
      URL.revokeObjectURL(url);
    }, 1500);
    return blob.size;
  }

  function safeFileName(name) {
    var s = String(name || "在线脚本").replace(/[\\/:*?"<>|]/g, "_").trim();
    if (!s) s = "在线脚本";
    return s.slice(0, 60);
  }

  /**
   * 一站式：UI 动作 → 可执行 exe 字节。
   * `opts.playerBytes` 可直接给模板字节（Node 验证 / 已缓存场景），否则按 `opts.playerUrl` 取。
   * @returns {Promise<{bytes:Uint8Array, filename:string, scan:Object, scriptJson:string}>}
   */
  function buildExe(opts) {
    var o = opts || {};
    var v = validate(o.uiActions);
    if (!v.ok) {
      var err = new Error(v.errors.join("\n"));
      err.validation = v;
      return Promise.reject(err);
    }
    var screenW = Number(o.screenW) > 0 ? Number(o.screenW) : 1920;
    var screenH = Number(o.screenH) > 0 ? Number(o.screenH) : 1080;
    var ctx = { screenW: screenW, screenH: screenH };
    var flat = buildScriptActions(o.uiActions, ctx);
    var images = (o.images || []).slice(0, MAX_IMAGES).map(function (im, i) {
      var name = sanitizeEntryName(im.name) || ("template_" + (i + 1) + ".png");
      return { name: name, data: im.data };
    });
    var scan = scanCapabilities(flat, images);
    var scriptJson = buildScriptFile({
      name: o.name, uiActions: o.uiActions, screenW: screenW, screenH: screenH,
    });
    var manifestJson = buildManifest({ images: images, scan: scan, bundledOpenCv: o.bundledOpenCv });
    // ⚠ 设置快照必须进包，否则 enablePlaybackCount 取默认 false=无限循环（见 buildSettingsJson）
    var settingsJson = buildSettingsJson({
      settings: o.settings,
      loopWholeScript: o.loopWholeScript,
      playbackCount: o.playbackCount,
      lowPerformanceMode: o.lowPerformanceMode,
    });
    var runtimeFiles = (o.runtimeFiles || []).concat([
      { name: "app_settings.json", data: settingsJson },
    ]);
    report(o.onProgress, { phase: "pack", ratio: 0 });
    var payload = buildPayload({
      scriptJson: scriptJson, manifestJson: manifestJson,
      images: images, runtimeFiles: runtimeFiles,
    });
    report(o.onProgress, { phase: "pack", ratio: 1, payloadBytes: payload.length });
    throwIfAborted(o.signal);

    return resolvePlayer(o).then(function (player) {
      throwIfAborted(o.signal);
      report(o.onProgress, { phase: "assemble", ratio: 0.5 });
      var bytes = assembleExe(player, payload);
      report(o.onProgress, { phase: "done", ratio: 1, exeBytes: bytes.length });
      return {
        bytes: bytes,
        filename: safeFileName(o.name) + ".exe",
        scan: scan,
        scriptJson: scriptJson,
        settingsJson: settingsJson,
        validation: v,
        stats: {
          playerBytes: player.length,
          payloadBytes: payload.length,
          exeBytes: bytes.length,
          actionCount: flat.length,
          imageCount: images.length,
        },
      };
    });
  }

  /** buildExe + 触发下载。 */
  function exportExe(opts) {
    return buildExe(opts).then(function (r) {
      downloadBytes(r.bytes, r.filename, "application/vnd.microsoft.portable-executable");
      return r;
    });
  }

  /**
   * 从**产品编辑器的动作**导出（官网 Demo 的「导出为独立 EXE」对话框走这条）。
   *
   * 与 `buildExe` 的区别只有一处：动作不经过工坊的"重复规格展开"，
   * 直接沿用编辑器给的类型/字段/indent（loop、if/else 这些结构因此能原样保留）。
   * 不在白名单里的动作**不静默丢弃**，而是收集到 `rejected` 让调用方告知用户。
   */
  function buildExeFromEditor(opts) {
    var o = opts || {};
    var screenW = Number(o.screenW) > 0 ? Number(o.screenW) : 1920;
    var screenH = Number(o.screenH) > 0 ? Number(o.screenH) : 1080;
    var ctx = { screenW: screenW, screenH: screenH };
    var conv = convertEditorActions(o.editorActions, ctx);

    if (!conv.count) {
      var e = new Error(conv.rejected.length
        ? ("脚本里没有网页版能导出的动作：" + conv.rejected.map(function (r) { return r.type; }).join("、"))
        : "脚本里还没有任何动作。");
      e.rejected = conv.rejected;
      return Promise.reject(e);
    }
    if (conv.count > MAX_ACTIONS) {
      var e2 = new Error("动作数 " + conv.count + " 超过网页版上限 " + MAX_ACTIONS + " 条。");
      e2.rejected = conv.rejected;
      return Promise.reject(e2);
    }

    var scan = scanCapabilities(conv.actions, []);
    var scriptJson = buildScriptFile({
      name: o.name, actions: conv.actions, screenW: screenW, screenH: screenH,
    });
    var manifestJson = buildManifest({ images: [], scan: scan });
    // ⚠ 设置快照：Demo 走这条时必须把**用户当前的整份设置**带进 exe（与客户端导出一致）
    var settingsJson = buildSettingsJson({
      settings: o.settings,
      loopWholeScript: o.loopWholeScript,
      playbackCount: o.playbackCount,
      lowPerformanceMode: o.lowPerformanceMode,
    });
    report(o.onProgress, { phase: "pack", ratio: 0 });
    var payload = buildPayload({
      scriptJson: scriptJson, manifestJson: manifestJson, images: [],
      runtimeFiles: [{ name: "app_settings.json", data: settingsJson }],
    });
    report(o.onProgress, { phase: "pack", ratio: 1, payloadBytes: payload.length });
    throwIfAborted(o.signal);

    return resolvePlayer(o).then(function (player) {
      throwIfAborted(o.signal);
      report(o.onProgress, { phase: "assemble", ratio: 0.5 });
      var bytes = assembleExe(player, payload);
      report(o.onProgress, { phase: "done", ratio: 1, exeBytes: bytes.length });
      return {
        bytes: bytes,
        filename: safeFileName(o.name) + ".exe",
        scan: scan,
        scriptJson: scriptJson,
        settingsJson: settingsJson,
        rejected: conv.rejected,
        stats: {
          playerBytes: player.length,
          payloadBytes: payload.length,
          exeBytes: bytes.length,
          actionCount: conv.actions.length,
          imageCount: 0,
        },
      };
    });
  }

  // ══════════════════════════════════════════════════════════════
  // ⑬ 预设模板（让"起码能用"这件事一键可达）
  // ══════════════════════════════════════════════════════════════
  function templates(screenW, screenH) {
    var cx = Math.round(screenW / 2), cy = Math.round(screenH / 2);
    return [
      {
        id: "key-every-5s",
        name: "每隔 5 秒按一次 A",
        desc: "无限循环：按一下 A 键，等 5 秒，再来一次。",
        actions: [
          { type: "keyClick", vk: 65, count: 0, interval: 5 },
        ],
      },
      {
        id: "click-every-2s",
        name: "每隔 2 秒左键点一下屏幕",
        desc: "无限循环：在屏幕正中央左键单击一次，等 2 秒，再来一次。",
        actions: [
          { type: "mouseClick", px: cx, py: cy, button: "left", count: 0, interval: 2 },
        ],
      },
      {
        id: "click-10-times",
        name: "在同一位置连点 10 次",
        desc: "每 0.5 秒点一次，共 10 次，然后结束。",
        actions: [
          { type: "mouseClick", px: cx, py: cy, button: "left", count: 10, interval: 0.5 },
          { type: "stopMacro", count: 1, interval: 0 },
        ],
      },
      {
        id: "hold-and-release",
        name: "按住 W 两秒再松开",
        desc: "演示「按键按下 / 等待 / 按键松开」三件套（走位、长按都靠它）。",
        actions: [
          { type: "keyDown", vk: 87, count: 1, interval: 0 },
          { type: "wait", seconds: 2, count: 1, interval: 0 },
          { type: "keyUp", vk: 87, count: 1, interval: 0 },
        ],
      },
    ];
  }

  return {
    VERSION: VERSION,
    MAX_ACTIONS: MAX_ACTIONS,
    MAX_IMAGES: MAX_IMAGES,
    MAX_CLICK_COUNT: MAX_CLICK_COUNT,
    ACTIONS: ACTIONS,
    BUTTONS: BUTTONS,
    KEY_CHOICES: KEY_CHOICES,
    vkName: vkName,
    sanitizeEntryName: sanitizeEntryName,
    crc32: crc32,
    fnv1a64: fnv1a64,
    buildStoredZip: buildStoredZip,
    normalizePoint: normalizePoint,
    fractionToPx: fractionToPx,
    buildScriptActions: buildScriptActions,
    buildScriptFile: buildScriptFile,
    buildManifest: buildManifest,
    buildSettingsJson: buildSettingsJson,
    convertEditorActions: convertEditorActions,
    EDITOR_REJECT_REASON: EDITOR_REJECT_REASON,
    scanCapabilities: scanCapabilities,
    validate: validate,
    buildPayload: buildPayload,
    assembleExe: assembleExe,
    readTail: readTail,
    fetchPlayer: fetchPlayer,
    setPlayerUrl: setPlayerUrl,
    isFileProtocol: isFileProtocol,
    describePlayerError: describePlayerError,
    abortError: abortError,
    isAbort: isAbort,
    downloadBytes: downloadBytes,
    buildExe: buildExe,
    buildExeFromEditor: buildExeFromEditor,
    exportExe: exportExe,
    templates: templates,
  };
});
