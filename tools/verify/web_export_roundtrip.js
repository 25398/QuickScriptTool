/**
 * tools/verify/web_export_roundtrip.js
 * ══════════════════════════════════════════════════════════════════
 * 网页在线导出（website/export/qst-web-export.js）的纯逻辑校验。
 *
 * 为什么必须有它
 * ──────────────
 * 这条链路的产物是一个 **二进制 exe**：ZIP 布局、尾标字节序、FNV 常量、
 * 坐标归一化——任何一处写错，网页上"导出成功"、下载下来双击却什么都不发生
 * （或者点到别的地方）。这些错误**在浏览器里完全看不出来**，只能靠离线断言。
 *
 * 断言分三层：
 *   A. 与 C++ 参考实现的**定值对齐**：CRC32 / FNV-1a64 拿真实 payload 对哈希
 *      （期望值来自 build\Release\QuickScriptTool.exe --export-exe 的产物尾标）。
 *   B. ZIP 结构与尾标：自己写一个最小 ZIP 读回器，逐条核对名字/偏移/内容，
 *      再核对尾标 offset/size/hash 与 `[player][payload][tail]` 的边界关系。
 *   C. 脚本 JSON 语义：重复间隔展开成 clickCount+duration 还是 loop+wait、
 *      坐标是否为归一化小数（写成像素会被引擎当旧格式重解释 ⇒ 点错地方）。
 *
 * 用法：
 *   node tools/verify/web_export_roundtrip.js
 *   node tools/verify/web_export_roundtrip.js --player build\Release\QstPlayer.exe
 *   node tools/verify/web_export_roundtrip.js --player <exe> --out build\_webexport\probe.exe
 *     （--out 会把产物写盘，便于交给真机双击 / 播放器运行验证）
 * ══════════════════════════════════════════════════════════════════
 */
"use strict";

const fs = require("fs");
const path = require("path");

const repoRoot = path.resolve(__dirname, "..", "..");
const WebExport = require(path.join(repoRoot, "website", "export", "qst-web-export.js"));

let passed = 0, failed = 0;
const failures = [];

function check(name, cond, detail) {
  if (cond) { passed++; console.log(`  \u2714 ${name}`); }
  else { failed++; failures.push(name); console.log(`  \u2716 ${name}${detail ? " :: " + detail : ""}`); }
}

function argValue(flag) {
  const i = process.argv.indexOf(flag);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : null;
}

// ── 最小 ZIP 读回器（只认 stored，够验证我们自己的产物）──────────
function readStoredZip(buf) {
  // EOCD 从尾部往前找（并校验自洽）——与 C++ 侧的判据一致
  let eocd = -1;
  for (let i = buf.length - 22; i >= Math.max(0, buf.length - 65557); i--) {
    if (buf.readUInt32LE(i) === 0x06054b50) {
      const cdSize = buf.readUInt32LE(i + 12);
      const cdOff = buf.readUInt32LE(i + 16);
      const commentLen = buf.readUInt16LE(i + 20);
      if (cdOff + cdSize === i && i + 22 + commentLen === buf.length) { eocd = i; break; }
    }
  }
  if (eocd < 0) throw new Error("no self-consistent EOCD");
  const total = buf.readUInt16LE(eocd + 10);
  const cdOff = buf.readUInt32LE(eocd + 16);
  const entries = [];
  let p = cdOff;
  for (let i = 0; i < total; i++) {
    if (buf.readUInt32LE(p) !== 0x02014b50) throw new Error("bad central header at " + p);
    const method = buf.readUInt16LE(p + 10);
    const crc = buf.readUInt32LE(p + 16);
    const compSize = buf.readUInt32LE(p + 20);
    const uncompSize = buf.readUInt32LE(p + 24);
    const nameLen = buf.readUInt16LE(p + 28);
    const extraLen = buf.readUInt16LE(p + 30);
    const commentLen = buf.readUInt16LE(p + 32);
    const localOff = buf.readUInt32LE(p + 42);
    const name = buf.slice(p + 46, p + 46 + nameLen).toString("utf8");
    // local header
    if (buf.readUInt32LE(localOff) !== 0x04034b50) throw new Error("bad local header for " + name);
    const lNameLen = buf.readUInt16LE(localOff + 26);
    const lExtraLen = buf.readUInt16LE(localOff + 28);
    const dataOff = localOff + 30 + lNameLen + lExtraLen;
    entries.push({
      name, method, crc, compSize, uncompSize,
      data: buf.slice(dataOff, dataOff + compSize),
      localOff,
    });
    p += 46 + nameLen + extraLen + commentLen;
  }
  return entries;
}

(async function main() {
  console.log("=== web_export_roundtrip ===");

  // ══ A. 与 C++ 参考实现的定值对齐 ═════════════════════════════
  console.log("\n[A] 哈希/CRC 与 C++ 参考实现对齐");

  // 真实参考：build\Release\QuickScriptTool.exe --export-exe 产出的 payload
  const refZip = path.join(repoRoot, ".tmp", "webexport-probe", "payload.zip");
  const REF_HASH = 1850181869054415507n;   // 该 payload 在产物尾标里的 hash
  if (fs.existsSync(refZip)) {
    const ref = new Uint8Array(fs.readFileSync(refZip));
    check("fnv1a64 与 C++ 尾标一致（真实导出产物）",
      WebExport.fnv1a64(ref) === REF_HASH,
      `got=${WebExport.fnv1a64(ref)} want=${REF_HASH}`);
    // CRC32 定值：对 "123456789" 的 IEEE CRC-32 是 0xCBF43926
    const probe = new TextEncoder().encode("123456789");
    check("crc32 定值向量 0xCBF43926", WebExport.crc32(probe) === 0xCBF43926,
      "got=0x" + WebExport.crc32(probe).toString(16));
  } else {
    console.log("  (跳过：未找到参考 payload，先跑一次 --export-exe 即可启用这两条)");
  }

  // ══ B. ZIP 结构与尾标 ════════════════════════════════════════
  console.log("\n[B] ZIP 布局 / 尾标");

  const scriptJson = WebExport.buildScriptFile({
    name: "验证脚本", screenW: 1920, screenH: 1080,
    uiActions: [
      { type: "mouseClick", px: 960, py: 540, button: "left", count: 0, interval: 2 },
      { type: "keyClick", vk: 65, count: 3, interval: 5 },
    ],
  });
  const manifestJson = WebExport.buildManifest({
    images: [], scan: WebExport.scanCapabilities([], []),
  });
  const settingsJson = WebExport.buildSettingsJson({});
  const payload = WebExport.buildPayload({
    scriptJson, manifestJson, images: [],
    runtimeFiles: [{ name: "app_settings.json", data: settingsJson }],
  });
  const zipEntries = readStoredZip(Buffer.from(payload));

  check("payload 里恰好三条条目（script.json + rt/app_settings.json + package.json）",
    zipEntries.length === 3, "got=" + zipEntries.length);
  check("条目全部 stored（method=0）—— 读端不解压，压缩了就坏",
    zipEntries.every((e) => e.method === 0));
  check("条目名写的是 UTF-8 字节且能读回",
    zipEntries[0].name === "script.json"
    && zipEntries[1].name === "rt/app_settings.json"
    && zipEntries[2].name === "package.json",
    zipEntries.map((e) => e.name).join(","));
  check("条目 CRC 与内容自洽",
    zipEntries.every((e) => WebExport.crc32(e.data) === e.crc));
  check("条目内容能还原成原 JSON",
    zipEntries[0].data.toString("utf8") === scriptJson);
  check("EOCD 自洽（centralDirOffset+size == EOCD 位置）", true);   // readStoredZip 已强制

  // ⚠ 这条是 2026-09-28 真机跑出来的：不带设置快照时，exe **永远不退出**。
  //   `PlaybackTabSettings::enablePlaybackCount` 默认 false = 无限循环
  //   （src/app_settings.h:48 + engine_script_run.cpp:8000），而播放器读
  //   `AppDir()\app_settings.json`。网页导出必须显式把这个开关钉死。
  const st = JSON.parse(settingsJson);
  check("设置快照必须显式关掉「无限循环」（否则线性脚本永不退出）",
    st.playback && st.playback.enablePlaybackCount === true
    && st.playback.playbackCount === 1,
    JSON.stringify(st));
  check("设置快照只覆盖 playback 段（其余键保持默认，不清零别的设置）",
    Object.keys(st).length === 1 && st.playback && Object.keys(st.playback).length >= 2,
    Object.keys(st).join(","));
  const stLoop = JSON.parse(WebExport.buildSettingsJson({ loopWholeScript: true }));
  check("勾选「整段循环」时 enablePlaybackCount=false（引擎侧 = 无限循环）",
    stLoop.playback.enablePlaybackCount === false);

  // 把 payload 的**条目数据**塞进一个假 EOCD 签名，验证读端不会认错
  const forged = WebExport.buildStoredZip([
    { name: "a.bin", data: new Uint8Array([0x50, 0x4B, 0x05, 0x06, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3]) },
    { name: "script.json", data: "{}" },
  ]);
  let forgedOk = false;
  try { forgedOk = readStoredZip(Buffer.from(forged)).length === 2; } catch (e) { forgedOk = false; }
  check("条目数据伪造 EOCD 签名后仍能正确定位真 EOCD", forgedOk);

  // ══ C. 脚本 JSON 语义 ═══════════════════════════════════════
  console.log("\n[C] 脚本 JSON 语义");

  const doc = JSON.parse(scriptJson);
  check("schema 版本 v=2", doc.v === 2, "got=" + doc.v);
  check("coordMeta.ref 恒为 2560x1440（产品的标准参考系）",
    doc.coordMeta.refWidth === 2560 && doc.coordMeta.refHeight === 1440);
  check("coordMeta.capture 记录用户屏幕（只影响模板图缩放）",
    doc.coordMeta.captureWidth === 1920 && doc.coordMeta.captureHeight === 1080);

  // 无限重复的 mouseClick ⇒ loop(-1) { mouseClick; wait 2 }
  const loopIdx = doc.actions.findIndex((a) => a.type === "loop");
  check("count=0 展开成 loop 容器", loopIdx === 0, "idx=" + loopIdx);
  check("loopCount=-1 表示一直重复", doc.actions[loopIdx].loopCount === -1);
  const inner = doc.actions[loopIdx + 1];
  check("loop 体是 mouseClick 且 indent=1",
    inner.type === "mouseClick" && inner.indent === 1, inner.type + "/" + inner.indent);
  check("坐标写成**归一化小数**（960/1920=0.5, 540/1080=0.5）—— 写成像素会被当旧格式重解释",
    inner.x === 0.5 && inner.y === 0.5, `x=${inner.x} y=${inner.y}`);
  const waitInner = doc.actions[loopIdx + 2];
  check("loop 体里有 wait=2 秒（重复间隔靠它，不是 duration）",
    waitInner.type === "wait" && Math.abs(waitInner.duration - 2) < 1e-9,
    waitInner.type + "/" + waitInner.duration);

  // 有限次重复的 keyClick ⇒ clickCount + duration（引擎的重复间隔语义）
  const keyAct = doc.actions.find((a) => a.type === "keyClick");
  check("count=N(>1) 且动作支持 clickCount ⇒ 不套 loop，直接 clickCount+duration",
    !!keyAct && keyAct.clickCount === 3 && Math.abs(keyAct.duration - 5) < 1e-9,
    keyAct ? `clickCount=${keyAct.clickCount} duration=${keyAct.duration}` : "missing");
  check("keyVk 写的是真键码（65=A）；keyText 只是显示名",
    !!keyAct && keyAct.keyVk === 65 && keyAct.keyText === "A");
  check("全部动作的 no 连续且从 1 起",
    doc.actions.every((a, i) => a.no === i + 1));

  // ══ D. 尾标 / 组装 ══════════════════════════════════════════
  console.log("\n[D] 模板追加与尾标");

  const playerPath = argValue("--player") || path.join(repoRoot, "build", "Release", "QstPlayer.exe");
  const outPath = argValue("--out");

  if (fs.existsSync(playerPath)) {
    const player = new Uint8Array(fs.readFileSync(playerPath));
    const exe = WebExport.assembleExe(player, payload);
    const tail = WebExport.readTail(exe);
    check("MZ 头保留（产物仍是 PE）", exe[0] === 0x4D && exe[1] === 0x5A);
    check("尾标 magic 可读回", !!tail);
    check("尾标 offset == 模板长度", !!tail && tail.offset === BigInt(player.length),
      tail ? `got=${tail.offset} want=${player.length}` : "no tail");
    check("尾标 size == payload 长度", !!tail && tail.size === BigInt(payload.length));
    check("offset+size+32 == 文件长度（C++ ReadPayloadInfo 的完整性判据）",
      !!tail && tail.valid === true);
    check("尾标 hash == payload 的 FNV-1a64",
      !!tail && tail.hash === WebExport.fnv1a64(payload));
    check("模板字节一字未改（追加不覆写）",
      exe.slice(0, player.length).every((b, i) => b === player[i]));

    if (outPath) {
      fs.mkdirSync(path.dirname(outPath), { recursive: true });
      fs.writeFileSync(outPath, Buffer.from(exe));
      console.log(`  → 已写出 ${outPath}（${exe.length} 字节；模板 ${player.length} + 包 ${payload.length} + 尾标 32）`);
    }
  } else {
    console.log(`  (跳过：找不到播放器模板 ${playerPath}；用 --player 指定)`);
  }

  // ══ F. Demo「导出当前编辑的脚本」那条路（产品编辑器动作）══════
  console.log("\n[F] 编辑器动作 → 脚本（官网 Demo 的导出对话框走这条）");

  // 形状照抄 website/demo/bridge.stub.js 的 sampleActions()：像素坐标 + indent 嵌套
  const editorActions = [
    { type: "moveMouse", x: 640, y: 480, duration: 0.2, indent: 0 },
    { type: "mouseClick", button: "left", clickCount: 1, duration: 0.01, indent: 0 },
    { type: "wait", duration: 1.2, indent: 0 },
    { type: "loop", loopCount: 3, indent: 0 },
    { type: "keyClick", keyText: "Enter", keyVk: 13, clickCount: 1, duration: 0.01, indent: 1 },
    { type: "endLoop", indent: 1 },
    { type: "stopMacro", indent: 0 },
  ];
  const conv = WebExport.convertEditorActions(editorActions, { screenW: 1920, screenH: 1080 });
  check("编辑器动作被原样转换（含 loop/endLoop 结构）", conv.count === 7 && conv.rejected.length === 0,
    `count=${conv.count} rejected=${conv.rejected.length}`);
  check("像素坐标被换算成归一化小数（640/1920=1/3, 480/1080=4/9）",
    Math.abs(conv.actions[0].x - 640 / 1920) < 1e-9 && Math.abs(conv.actions[0].y - 480 / 1080) < 1e-9,
    `x=${conv.actions[0].x} y=${conv.actions[0].y}`);
  check("indent 嵌套层级保留（loop 体在 indent=1）",
    conv.actions[3].type === "loop" && conv.actions[3].indent === 0
    && conv.actions[4].indent === 1 && conv.actions[5].type === "endLoop");
  check("loopCount 原样保留", conv.actions[3].loopCount === 3);
  check("编辑器的显示字段（name/selected）不会漏进脚本",
    conv.actions.every((a) => a.name === undefined && a.selected === undefined));
  check("动作编号连续", conv.actions.every((a, i) => a.no === i + 1));

  const convRej = WebExport.convertEditorActions([
    { type: "mouseClick", button: "left", x: 10, y: 10, clickCount: 1, duration: 0.01, indent: 0 },
    { type: "aiActionExecute", indent: 0 },
    { type: "runMacro", indent: 0 },
    { type: "findImage", indent: 0 },
  ], { screenW: 1920, screenH: 1080 });
  check("不支持的动作**不静默丢弃**，而是进 rejected 并带可读理由",
    convRej.count === 1 && convRej.rejected.length === 3
    && convRej.rejected.every((r) => r.reason && r.reason.length > 4),
    JSON.stringify(convRej.rejected));
  check("AI 动作的拒绝理由说明是 API Key 原因",
    /API Key/.test((convRej.rejected.find((r) => r.type === "aiActionExecute") || {}).reason || ""));

  // ══ G. 端到端：编辑器动作 → 完整 exe 字节 ═══════════════════
  console.log("\n[G] buildExeFromEditor 端到端");
  if (fs.existsSync(playerPath)) {
    const player = new Uint8Array(fs.readFileSync(playerPath));
    const built = await WebExport.buildExeFromEditor({
      name: "验证-编辑器导出", editorActions, screenW: 1920, screenH: 1080, playerBytes: player,
    });
    const t2 = WebExport.readTail(built.bytes);
    check("产物尾标自洽", !!t2 && t2.valid === true);
    check("产物仍是 PE", built.bytes[0] === 0x4D && built.bytes[1] === 0x5A);
    const ents = readStoredZip(Buffer.from(built.bytes.slice(
      Number(t2.offset), Number(t2.offset + t2.size))));
    check("payload 含 script.json + rt/app_settings.json + package.json",
      ents.length === 3 && ents[1].name === "rt/app_settings.json",
      ents.map((e) => e.name).join(","));
    check("文件名是 .exe", /\.exe$/.test(built.filename), built.filename);
  } else {
    console.log("  (跳过：找不到播放器模板)");
  }

  // ══ H. 设置继承 / 进度 / 中止 ═══════════════════════════════
  console.log("\n[H] 设置继承 + 进度 + 中止");

  // 用户在 Demo「设置」里改过的那一份（形状照抄 app_settings.json）
  const userSettings = {
    click: { enableRandomInterval: true, randomIntervalMaxSeconds: 0.8, enableClickCountLimit: true, clickCountLimit: 250 },
    playback: { enablePlaybackCount: true, playbackCount: 7, lowPerformanceMode: true, playbackSpeed: 2 },
    other: { themeId: 3, playSoundOnStart: false },
    windowMode: { blockRunWhenUnhealthy: false },
    home: { activeTab: 2 },
  };
  check("给了完整设置就**原样**嵌入（不裁剪、不改名）",
    JSON.parse(WebExport.buildSettingsJson({ settings: userSettings }))
      .playback.playbackCount === 7
    && JSON.parse(WebExport.buildSettingsJson({ settings: userSettings })).click.clickCountLimit === 250);
  check("设置里的 enablePlaybackCount=false 也照样带进去（与客户端一致，不偷偷改成 1）",
    JSON.parse(WebExport.buildSettingsJson({
      settings: { playback: { enablePlaybackCount: false, playbackCount: 1 } },
    })).playback.enablePlaybackCount === false);
  check("没给设置时回落到'只钉播放次数'（工坊页）",
    JSON.parse(WebExport.buildSettingsJson({})).playback.enablePlaybackCount === true);

  if (fs.existsSync(playerPath)) {
    const player = new Uint8Array(fs.readFileSync(playerPath));

    // 进度事件必须按 阶段 走完 且 单调不减
    const seen = [];
    const built = await WebExport.buildExeFromEditor({
      name: "验证-设置继承", editorActions, screenW: 1920, screenH: 1080, playerBytes: player,
      settings: userSettings,
      onProgress: (p) => seen.push(p.phase + ":" + (p.ratio || 0)),
    });
    const ent2 = readStoredZip(Buffer.from(built.bytes.slice(
      Number(WebExport.readTail(built.bytes).offset),
      Number(WebExport.readTail(built.bytes).offset + WebExport.readTail(built.bytes).size))));
    const stEntry = ent2.find((e) => e.name === "rt/app_settings.json");
    check("payload 里的 rt/app_settings.json 就是用户当前那份设置",
      !!stEntry && JSON.parse(stEntry.data.toString("utf8")).playback.playbackCount === 7,
      stEntry ? stEntry.data.toString("utf8").slice(0, 80) : "missing");
    check("userSettings 对象没有被就地改写（导出不该污染界面状态）",
      userSettings.playback.playbackCount === 7 && Object.keys(userSettings).length === 5);
    check("进度事件覆盖 pack→assemble→done 三个阶段",
      seen.some((s) => s.startsWith("pack")) && seen.some((s) => s.startsWith("assemble"))
      && seen.some((s) => s.startsWith("done")), seen.join(" | "));

    // 中止：已经 abort 的 signal 必须在开始组装前就拒掉
    const ac = new AbortController();
    ac.abort();
    let abortedOk = false;
    try {
      await WebExport.buildExeFromEditor({
        name: "验证-中止", editorActions, screenW: 1920, screenH: 1080, playerBytes: player,
        signal: ac.signal,
      });
    } catch (e) { abortedOk = WebExport.isAbort(e) === true; }
    check("已中止的 signal 会让导出以 AbortError 结束（且能被 isAbort 认出）", abortedOk);
  } else {
    console.log("  (跳过：找不到播放器模板)");
  }

  // ══ E. 校验闸 ═══════════════════════════════════════════════
  console.log("\n[E] 校验闸");
  check("空脚本被拒", WebExport.validate([]).ok === false);
  const tooMany = new Array(WebExport.MAX_ACTIONS + 1).fill(0)
    .map(() => ({ type: "wait", seconds: 1, count: 1, interval: 0 }));
  check(`超过 ${WebExport.MAX_ACTIONS} 条被拒`, WebExport.validate(tooMany).ok === false);
  const exactlyMax = tooMany.slice(0, WebExport.MAX_ACTIONS);
  check(`恰好 ${WebExport.MAX_ACTIONS} 条通过`, WebExport.validate(exactlyMax).ok === true);
  check("找图动作缺模板图被拒",
    WebExport.validate([{ type: "findImage", imageName: "", count: 1, interval: 0 }]).ok === false);
  check("不认识的动作品类被拒",
    WebExport.validate([{ type: "aiActionExecute", count: 1, interval: 0 }]).ok === false);

  console.log(`\n=== ${passed} passed / ${failed} failed ===`);
  if (failed) { console.log("FAILED: " + failures.join(" | ")); process.exit(1); }
})().catch((e) => {
  console.error("harness error: " + (e && e.stack ? e.stack : e));
  process.exit(2);
});
