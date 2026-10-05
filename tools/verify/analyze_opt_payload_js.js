// 量化「录制优化列表」载荷在 **JS 侧**的成本：现状（美化 JSON）vs 列式（键名只出现一次）。
//
// 存在的意义：**证明载荷优化不值得做**（2026-10-03）。
//   · 列式能把载荷从 14.4MB 压到 0.95MB，`JSON.parse` 23ms → 2ms；
//   · 但列式还要把 5881 个对象**物化**出来（JSON.parse 是顺带做的）—— 实测 ~20ms
//     ⇒ JS 侧合计 22ms ≈ 23ms，**打平**。
//   · 却要搭上「displayActionName 跨 20+ 类型读 40+ 字段，漏一个就静默错标签」的风险。
// ⇒ 瓶颈在 C++ 解析，不在载荷。详见 docs/recording-optimize-performance.md、红线 63。
//
// 用法（只读，不改任何东西）：
//   node tools/verify/analyze_opt_payload_js.js [脚本.json]
const fs = require("fs");
const path = require("path");

const FILE = process.argv[2] ||
  "D:/other/software/build/Release/recordings/键鼠录制-1791021669.json";
const raw = fs.readFileSync(FILE, "utf8").replace(/^\uFEFF/, "");
const doc = JSON.parse(raw);
const acts = doc.actions;
console.log("动作数:", acts.length);

// ── 现状载荷：C++ 发的是**缩进美化**的每条动作 JSON ──────────────
// 复刻 WriteActionJson 的形态（这里用 2 空格缩进近似；实际缩进更宽）
const curParts = acts.map((a) => JSON.stringify(a, null, 2));
const cur = '{"path":"p","name":"n","durationSeconds":1,"actions":[' +
  curParts.join(",") + "]}";
console.log("现状载荷(美化) :", (Buffer.byteLength(cur) / 1048576).toFixed(2), "MB");

// 紧凑版（同一批字段，去掉缩进）
const compact = '{"path":"p","name":"n","durationSeconds":1,"actions":[' +
  acts.map((a) => JSON.stringify(a)).join(",") + "]}";
console.log("紧凑载荷       :", (Buffer.byteLength(compact) / 1048576).toFixed(2), "MB");

// ── 列式：键名只出现一次 ────────────────────────────────────────
const PROJ = ("type remark duration x y keyText button scrollSteps customText text " +
  "moveFromVar moveVarExprX moveVarExprY endX endY imageLocate randomDuration " +
  "loopFromVar loopVarExpr loopCount blockName shortcutPreset inputText " +
  "scrollVertical scrollHorizontal scrollDirection findImageFollowUp perfectMatch " +
  "matchThreshold imageScaleMin imageScaleMax multiMatchMode watchMode " +
  "watchPollSeconds resumeAfterWatch ocrResultMode ocrFollowUp conditionExpr " +
  "gotoStepExpr targetPath loopVarName matchVarName aiPrompt aiOutputVarName " +
  "computeCode clickCount holdLeftWin holdRightWin holdLeftCtrl holdRightCtrl " +
  "holdLeftAlt holdRightAlt holdLeftShift holdRightShift").split(" ");
const cols = PROJ.slice();
const data = {};
for (const c of cols) data[c] = acts.map((a) => (c in a ? a[c] : null));
const colar = JSON.stringify({ path: "p", name: "n", durationSeconds: 1, n: acts.length, cols, data });
console.log("列式载荷       :", (Buffer.byteLength(colar) / 1048576).toFixed(2), "MB");

// ── 计时 ────────────────────────────────────────────────────────
function best(fn, n = 5) {
  let b = Infinity;
  for (let i = 0; i < n; i++) { const t = process.hrtime.bigint(); fn(); const d = Number(process.hrtime.bigint() - t) / 1e6; if (d < b) b = d; }
  return b;
}
const tCur = best(() => JSON.parse(cur));
const tCol = best(() => JSON.parse(colar));

// 列式还要把 5881 个对象**物化**出来（JSON.parse 是顺带做的）
const parsedCol = JSON.parse(colar);
const tMat = best(() => {
  const n = parsedCol.n, cs = parsedCol.cols, d = parsedCol.data;
  const out = new Array(n);
  for (let i = 0; i < n; i++) {
    const o = {};
    for (let c = 0; c < cs.length; c++) o[cs[c]] = d[cs[c]][i];
    out[i] = o;
  }
  return out;
});

console.log("\nJSON.parse(现状 14.4MB) ≈", tCur.toFixed(0), "ms");
console.log("JSON.parse(列式 0.95MB) ≈", tCol.toFixed(0), "ms");
console.log("列式物化 5881×" + cols.length + " 个属性 ≈", tMat.toFixed(0), "ms");
console.log("⇒ JS 侧合计: 现状 " + tCur.toFixed(0) + " ms  vs  列式 " + (tCol + tMat).toFixed(0) + " ms");
