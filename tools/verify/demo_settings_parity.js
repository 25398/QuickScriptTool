/**
 * tools/verify/demo_settings_parity.js
 * ══════════════════════════════════════════════════════════════════
 * 官网 Demo 的「设置」与产品 `app_settings.json` 的**字段对齐**守卫。
 *
 * 为什么必须有它（2026-09-28 用户反馈「网页 demo，导出还是没走设置」）：
 *   产品前端的设置往返是**不对称**的 ——
 *     · `ui/app.js` 的 `collectSettings()` 发出去的是**扁平**键（`playbackCount`、
 *       `enablePlaybackCount`、`lowPerformanceMode`…）；
 *     · `fillSettings()` 读回来的是**嵌套**（`s.playback.playbackCount`）。
 *   也就是后端契约是「**收扁平、回嵌套**」（`webview_bridge_backend.cpp` 的
 *   SaveSettings 里有一条条 `GetX(json, "key") → s.section.field` 的映射）。
 *   而 Demo 的桩早先只认嵌套 + 一份**手写的** otherFlat 白名单 ⇒ 播放次数、间隔、
 *   低性能模式等扁平键**全被静默丢掉**：界面上改了设置、`settings` 对象没变、
 *   导出的 exe 用旧值。用户看到的就是"导出没走设置"，而浏览器里**一个字都看不出来**。
 *
 * 三条判据（都从源码抽取，不手抄）：
 *   ① C++ SaveSettings 的「扁平键 → 段.字段」表 ↔ 桩里的 `FLAT_FIELD_SECTION` 必须**双向一致**；
 *   ② `collectSettings()` 能发出的每个扁平键都必须被桩路由（否则又被静默丢掉）；
 *   ③ 桩的 `defaultSettings()` 段/键名必须是 C++ 写出端真实存在的（不写"假设置"）。
 *
 * 用法：
 *   node tools/verify/demo_settings_parity.js
 *   node tools/verify/demo_settings_parity.js --list
 * ══════════════════════════════════════════════════════════════════
 */
"use strict";

const fs = require("fs");
const path = require("path");

const repoRoot = path.resolve(__dirname, "..", "..");
const read = (p) => fs.readFileSync(path.join(repoRoot, p), "utf8");

const cppStore = read("src/app_settings_store.cpp");
const cppBackend = read("src/webview/webview_bridge_backend.cpp");
const stub = read("website/demo/bridge.stub.js");
const appJs = read("ui/app.js");

/** 取一个函数的函数体（按花括号配平）。 */
function bodyOf(src, sig) {
  const i = src.indexOf(sig);
  if (i < 0) return "";
  const open = src.indexOf("{", i);
  let depth = 0;
  for (let k = open; k < src.length; k++) {
    if (src[k] === "{") depth++;
    else if (src[k] === "}") { depth--; if (depth === 0) return src.slice(open, k + 1); }
  }
  return src.slice(open);
}

// ── ① C++：扁平键 → 段.字段 ─────────────────────────────────────
/** 定位承载扁平映射的那个函数（锚点是那句注释）。 */
function cppSaveSettingsBody() {
  const anchor = cppBackend.indexOf("flat keys from collectSettings");
  if (anchor < 0) throw new Error("webview_bridge_backend.cpp 里找不到扁平设置映射的锚点");
  // 往上找函数起点
  let start = cppBackend.lastIndexOf("\nbool ", anchor);
  if (start < 0) start = cppBackend.lastIndexOf("\nstd::", anchor);
  if (start < 0) start = 0;
  return bodyOf(cppBackend.slice(start), cppBackend.slice(start, start + 120).split("{")[0]);
}

function cppFlatMap() {
  const body = cppSaveSettingsBody();
  const map = {};
  // ⚠ 取值器**不止 GetBool/GetInt/GetNumber/GetStr** —— 数组字段走
  //   `GetActionTokenArray`（editorActionOrder / editorHiddenActions / …）。
  //   第一版漏了它，于是把四个**其实已被处理**的键误报成"没归宿"。
  //   结尾允许 `,` / `=` / `)`：数组形式是 `GetActionTokenArray(json, "k", s.sec.field);`
  //   没有赋值号，用 `=` 收尾会整批漏掉。
  const re = /Get(?:Bool|Int|Number|Str(?:ing)?|ActionTokenArray)\s*\(\s*settingsObjJson\s*,\s*"([A-Za-z][A-Za-z0-9_]*)"[\s\S]{0,120}?s\.(\w+)\.(\w+)\s*[,=)]/g;
  let m;
  while ((m = re.exec(body))) if (!map[m[1]]) map[m[1]] = m[2] + "." + m[3];
  return map;
}

// ── ② 桩：FLAT_FIELD_SECTION ────────────────────────────────────
function stubFlatMap() {
  const i = stub.indexOf("var FLAT_FIELD_SECTION");
  if (i < 0) throw new Error("bridge.stub.js 里找不到 FLAT_FIELD_SECTION");
  const body = bodyOf(stub.slice(i), "FLAT_FIELD_SECTION");
  const map = {};
  for (const m of body.matchAll(/([A-Za-z][A-Za-z0-9_]*)\s*:\s*\[\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\]/g)) {
    map[m[1]] = m[2] + "." + m[3];
  }
  return map;
}

/** 桩特殊处理、不走表的键（有语义，不能是普通赋值）。 */
const STUB_SPECIAL = new Set(["savedModelsClear"]);

/**
 * 后端用**另一条路径**处理、因而不在上面的 `Get*` 表里的扁平键。
 * 桩仍然必须接住（前端不管后端怎么实现，它就是把整包发过来）——
 * 所以判据 ① 只要求「C++ → 桩」完整，桩多出来的这些在此登记理由即可。
 */
const STUB_ONLY = {
  uiMode: "home.uiMode —— 后端在同函数后面的字符串提取块里单独处理（getTopStr）",
  apiUrl: "ai.apiUrl —— 同上（字符串提取块）",
  apiKey: "ai.apiKey —— 同上",
  modelName: "ai.modelName —— 同上",
  savedModels: "ai.savedModels —— 数组，后端单独处理且要排在字符串匹配之前",
  editorCatalogPreset: "other.editorCatalogPreset —— 同上（getTopStr）",
};

// ── ③ app.js：collectSettings 能发出的扁平键 ────────────────────
function collectSettingsKeys() {
  const body = bodyOf(appJs, "function collectSettings()");
  const keys = new Set();
  for (const m of body.matchAll(/assignChk\(out,\s*"([A-Za-z][A-Za-z0-9_]*)"/g)) keys.add(m[1]);
  for (const m of body.matchAll(/\bout\.([A-Za-z][A-Za-z0-9_]*)\s*=/g)) keys.add(m[1]);
  // `const out = { a: 1, b: 2 }` 里的顶层键（只看第一层，靠缩进 4/6 空格区分嵌套）
  const lit = body.match(/const out = \{([\s\S]*?)\n    \};/);
  if (lit) {
    for (const m of lit[1].matchAll(/^\s{6}([A-Za-z][A-Za-z0-9_]*)\s*:/gm)) keys.add(m[1]);
  }
  // 已知的展开来源（这两个 helper 的键也在 C++ 表里，直接从表里反查即可）
  return keys;
}

/** 桩的 defaultSettings() 段/键（用于第 ③ 条判据）。 */
function stubSettingsSections() {
  const body = bodyOf(stub, "function defaultSettings()");
  const start = body.indexOf("JSON.stringify({");
  const obj = body.slice(start);
  const out = {};
  const segs = [...obj.matchAll(/([A-Za-z][A-Za-z0-9_]*)\s*:\s*\{/g)];
  segs.forEach((s, idx) => {
    const to = idx + 1 < segs.length ? segs[idx + 1].index : obj.length;
    const chunk = obj.slice(s.index, to);
    const keys = new Set();
    for (const k of chunk.matchAll(/^\s*([A-Za-z][A-Za-z0-9_]*)\s*:/gm)) keys.add(k[1]);
    keys.delete(s[1]);
    out[s[1]] = keys;
  });
  return out;
}

/** C++ 写出端的「段 → 键」。 */
function cppWriterSections() {
  const from = cppStore.indexOf("std::wstring SerializeAppSettings");
  const to = cppStore.indexOf("bool SaveAppSettings");
  const body = cppStore.slice(from, to > from ? to : undefined);
  const names = [...body.matchAll(/\\"([A-Za-z][A-Za-z0-9_]*)\\"\s*:\s*\{/g)].map((m) => m[1]);
  const fns = [...body.matchAll(/Write(\w+)\s*\(\s*file\b/g)].map((m) => "Write" + m[1]);
  const out = {};
  for (let i = 0; i < fns.length; i++) {
    const fn = bodyOf(cppStore.slice(cppStore.indexOf("void " + fns[i] + "(")), "void " + fns[i] + "(");
    if (!fn) continue;
    const keys = new Set();
    // 普通字段：file << L"    \"key\": " << ...
    for (const k of fn.matchAll(/\\"\s*([A-Za-z][A-Za-z0-9_]*)\\"/g)) keys.add(k[1]);
    // 数组字段：WriteJsonStringArrayField(file, L"key", s.other.key) —— 键**不带转义引号**，
    // 第一版只扫普通字段，于是 editorActionOrder / editorHiddenActions 被误判成
    // "C++ 写出端不存在"（实际写在 app_settings_store.cpp:535-536）。
    for (const k of fn.matchAll(/Write\w+\s*\(\s*file\s*,\s*L"([A-Za-z][A-Za-z0-9_]*)"/g)) keys.add(k[1]);
    if (!names[i] || !keys.size) continue;
    out[names[i]] = keys;
  }
  return out;
}

// ══════════════════════════════════════════════════════════════════
const cppMap = cppFlatMap();
const stubMap = stubFlatMap();
const csKeys = collectSettingsKeys();
const writer = cppWriterSections();
const demoDefaults = stubSettingsSections();
const listOnly = process.argv.includes("--list");

let problems = 0;
const bad = (t) => { console.log("  - " + t); problems++; };

console.log("=== demo_settings_parity ===");
console.log(`  C++ 扁平映射      : ${Object.keys(cppMap).length} 条`);
console.log(`  桩 FLAT_FIELD_SECTION : ${Object.keys(stubMap).length} 条`);
console.log(`  collectSettings 扁平键 : ${csKeys.size} 个`);

if (listOnly) {
  console.log("\n[C++ 扁平映射]");
  for (const k of Object.keys(cppMap).sort()) console.log("  " + k.padEnd(34) + " -> " + cppMap[k]);
  console.log("\n[collectSettings 能发出的键]");
  console.log("  " + [...csKeys].sort().join(", "));
}

// ① 双向一致
console.log("\n[判据 ①] C++ 扁平映射 ↔ 桩 FLAT_FIELD_SECTION 双向一致");
const missInStub = Object.keys(cppMap).filter((k) => !stubMap[k] && !STUB_SPECIAL.has(k)).sort();
const extraInStub = Object.keys(stubMap)
  .filter((k) => !cppMap[k] && !STUB_ONLY[k]).sort();
const mismatched = Object.keys(cppMap)
  .filter((k) => stubMap[k] && stubMap[k] !== cppMap[k]).sort();
if (missInStub.length) {
  console.log(`  C++ 有、桩没路由（设置会被静默丢掉）：${missInStub.length}`);
  for (const k of missInStub) bad(`${k} -> ${cppMap[k]}`);
}
if (extraInStub.length) {
  console.log(`  桩有、C++ 没有、也没登记理由（写了也没人读）：${extraInStub.length}`);
  for (const k of extraInStub) bad(`${k} -> ${stubMap[k]}`);
}
if (mismatched.length) {
  console.log(`  路由目标不一致：${mismatched.length}`);
  for (const k of mismatched) bad(`${k}: 桩=${stubMap[k]} vs C++=${cppMap[k]}`);
}
if (!missInStub.length && !extraInStub.length && !mismatched.length) {
  console.log(`  OK —— ${Object.keys(cppMap).length} 条 C++ 映射逐条一致`
    + `（桩另有 ${Object.keys(STUB_ONLY).length} 条登记为"后端另路处理"）`);
}

// ② collectSettings 的每个键都要有归宿
console.log("\n[判据 ②] collectSettings 发出的每个扁平键都被路由");
const unrouted = [...csKeys]
  .filter((k) => !stubMap[k] && !STUB_SPECIAL.has(k)
    // 编辑器设置类字段在 C++ 走另一条持久化路径，桩里保底直写 other
    && !/^editor/.test(k))
  .sort();
if (unrouted.length) {
  console.log(`  未路由：${unrouted.length}`);
  for (const k of unrouted) bad(`collectSettings 会发 "${k}"，但桩不会存它`);
} else {
  console.log(`  OK —— ${csKeys.size} 个键全部有归宿`);
}

// ③ 默认设置里不能有 C++ 不认的段/键
console.log("\n[判据 ③] 桩 defaultSettings() 的段/键在 C++ 写出端真实存在");
for (const sec of Object.keys(demoDefaults)) {
  const w = writer[sec];
  if (!w) { bad(`桩有段 "${sec}"，但 C++ 写出端没有这一段`); continue; }
  const unknown = [...demoDefaults[sec]].filter((k) => !w.has(k) && !/^savedModels/.test(k)).sort();
  if (unknown.length) for (const k of unknown) bad(`${sec}.${k} 在 C++ 写出端不存在`);
}
console.log("  （段：" + Object.keys(demoDefaults).join(", ") + "）");

console.log("");
if (problems) {
  console.log(`FAILED: ${problems} 处不对齐 —— 网页导出的 exe 会**静默**用默认值`);
  process.exit(1);
}
console.log("OK: Demo 设置与产品 app_settings.json 完全对齐");
