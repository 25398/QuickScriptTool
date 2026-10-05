/**
 * tools/verify/demo_bridge_coverage.js
 * ══════════════════════════════════════════════════════════════════
 * 官网 Demo 的桥接桩覆盖率守卫。
 *
 * 为什么必须有它（2026-09-28 用户反馈「极简模式和专业模式的脚本库没有统一」）：
 *   产品前端 `ui/` 通过 `window.qst.<方法>()` 调后端，网页 Demo 由
 *   `website/demo/bridge.stub.js` 顶替后端。**桩没实现的方法不会报错** ——
 *   调用发出去、没有任何 `.result` 回来，界面就"空着"或"少一块"。
 *   这类缺口在浏览器里**完全静默**，只能靠比对 API 面来发现。
 *   实测就是这么找到的：专业模式的脚本库走 `listLibraryFolders()`（目录树），
 *   而桩里根本没这个 case ⇒ 专业模式的库列表是空的，与极简模式不一致。
 *
 * 判据：
 *   1. `ui/bridge.js` 里出现的每个 `type: "xxx"` 都必须能在桩里找到处理
 *      （要么 `case "xxx"`，要么在 `ALIASES` 里映射到别的 case）；
 *   2. 反过来，桩里的 case 若在 `bridge.js` 里不存在，报告为"多余"
 *      （不算失败，但通常说明前端已经改名/删掉了）。
 *
 * 用法：
 *   node tools/verify/demo_bridge_coverage.js
 *   node tools/verify/demo_bridge_coverage.js --list     # 打印全部已覆盖方法
 * ══════════════════════════════════════════════════════════════════
 */
"use strict";

const fs = require("fs");
const path = require("path");

const repoRoot = path.resolve(__dirname, "..", "..");
const bridgePath = path.join(repoRoot, "ui", "bridge.js");
const stubPath = path.join(repoRoot, "website", "demo", "bridge.stub.js");

const bridge = fs.readFileSync(bridgePath, "utf8");
const stub = fs.readFileSync(stubPath, "utf8");

/** bridge.js 里对外暴露的桥接方法名（= 它 post 出去的 type）。 */
function collectBridgeTypes(text) {
  const out = new Set();
  for (const m of text.matchAll(/type:\s*"([A-Za-z][A-Za-z0-9_.]*)"/g)) out.add(m[1]);
  // 少数地方是 type: 变量（如 window.xxx 的动态前缀），这里只收字面量
  return out;
}

/** 桩里 `case "xxx":` 处理的消息类型。 */
function collectStubCases(text) {
  const out = new Set();
  for (const m of text.matchAll(/case\s+"([A-Za-z][A-Za-z0-9_.]*)"/g)) out.add(m[1]);
  return out;
}

/**
 * 桩里"同一个 case 顶多个 type"的写法：`case "a": case "b":` 会被上面一起收到，
 * 但 `.result` 的类型名要与请求同名，所以还支持显式别名表。
 * 这里解析桩里的 `ALIASES`（如果将来加了）。
 */
function collectAliases(text) {
  const out = new Map();
  const m = text.match(/var\s+BRIDGE_ALIASES\s*=\s*\{([\s\S]*?)\};/);
  if (!m) return out;
  for (const e of m[1].matchAll(/"([^"]+)"\s*:\s*"([^"]+)"/g)) out.set(e[1], e[2]);
  return out;
}

const bridgeTypes = collectBridgeTypes(bridge);
const stubCases = collectStubCases(stub);
const aliases = collectAliases(stub);

// 前端从不"调用"、只接收的消息（结果推送 / 事件），不算缺口
const NOT_REQUESTED = new Set([
  "engine.toast", "window.clientSize", "agentWindow.open",
]);

const missing = [...bridgeTypes]
  .filter((t) => !stubCases.has(t) && !aliases.has(t) && !NOT_REQUESTED.has(t))
  .sort();
const extra = [...stubCases].filter((t) => !bridgeTypes.has(t)).sort();

const listOnly = process.argv.includes("--list");

console.log("=== demo_bridge_coverage ===");
console.log(`  ui/bridge.js 暴露的请求类型 : ${bridgeTypes.size}`);
console.log(`  bridge.stub.js 处理的 case  : ${stubCases.size}`);
console.log(`  显式别名                    : ${aliases.size}`);

if (listOnly) {
  console.log("\n[已覆盖]");
  for (const t of [...bridgeTypes].sort()) {
    if (stubCases.has(t) || aliases.has(t)) console.log("  " + t);
  }
}

if (missing.length) {
  console.log(`\n[缺口] 前端会调用、但桩里没有任何处理（界面会静默少一块）：${missing.length}`);
  for (const t of missing) console.log("  - " + t);
} else {
  console.log("\n[缺口] 无 —— 前端调用的每个桥接方法，桩里都有处理。");
}

if (extra.length) {
  console.log(`\n[多余] 桩里处理、但 bridge.js 已不再发送（前端可能已改名/删除）：${extra.length}`);
  for (const t of extra) console.log("  ? " + t);
}

console.log("");
if (missing.length) {
  console.log(`FAILED: ${missing.length} 个桥接方法没有桩实现`);
  process.exit(1);
}
console.log("OK: 桥接桩覆盖了前端的全部调用面");
