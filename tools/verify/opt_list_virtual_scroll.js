/**
 * tools/verify/opt_list_virtual_scroll.js
 * ══════════════════════════════════════════════════════════════════
 * 「键鼠录制优化」动作列表虚拟滚动（窗口化）的离线校验。
 *
 * 为什么必须有它
 * ──────────────
 * 这个列表要装**几万条**录制动作（实测 5.8 万条）。旧实现是「分批追加」：
 * 滚到底就再 insertAdjacentHTML 300 行，DOM **只增不减** —— 首屏是快了，
 * 但滚到后面 DOM 累积到全量，光滚动就卡死；而且选择态用的是**渲染时的
 * 快照**（new Set(optSelected)），于是「全选」之后滚动新出现的行仍是未选中，
 * 用户看到的就是「全选只能选到已加载的部分」。
 *
 * 这些缺陷**在浏览器里肉眼看不出来**（要点开一个几万条的录制、全选、再滚到
 * 底部去数有没有勾），只能靠离线断言钉住不变量：
 *
 *   A. 窗口数学：边界夹取（start<0 / end>total / total=0 / rowH=0）、
 *      滚到最底、小数据量 —— 算错就会露白、跳行或滚不到底。
 *   B. 核心不变量：**窗口行数有上限**（DOM 恒定，防「退化成无限追加」）、
 *      **窗口必须覆盖视口**（不漏行）、**相邻滚动窗口连续**（不跳行）。
 *   C. 静态守卫：渲染期不许再出现选择态快照（new Set(state.optSelected)），
 *      且旧的追加式函数不许回来。
 *   D. 压缩映射：浏览器单元素高度上限（Chrome/Edge ≈33.5M、Safari ≈16.7M）之上
 *      必须把 spacer 高度压到上限内，否则 **scrollbar 失效、滚不到列表末尾**（静默）。
 *      不超限时必须**严格恒等**（ratio=1）⇒ 老数据量零行为变化。
 *
 * 用法：
 *   node tools/verify/opt_list_virtual_scroll.js
 *   QST_APP_JS=<副本> QST_INDEX_HTML=<副本> node tools/verify/...   # 负对照（只改副本）
 * ══════════════════════════════════════════════════════════════════
 */
"use strict";

const fs = require("fs");
const path = require("path");

const repoRoot = path.resolve(__dirname, "..", "..");
// 允许覆盖被测文件路径 ⇒ 负对照（变异测试）只改**副本**，不碰 ui/。
// ⚠ 有并行会话把 ui/ 同步进 build/Release/ui/，直接在 ui/ 上做变异会把变异版带进共享产物。
const appJsPath = process.env.QST_APP_JS || path.join(repoRoot, "ui", "app.js");
const indexHtmlPath = process.env.QST_INDEX_HTML || path.join(repoRoot, "ui", "index.html");

let passed = 0;
let failed = 0;
const failures = [];

function check(name, cond, detail) {
  if (cond) {
    passed++;
    console.log(`  \u2714 ${name}`);
  } else {
    failed++;
    failures.push(name);
    console.log(`  \u2716 ${name}${detail ? " :: " + detail : ""}`);
  }
}

function section(title) {
  console.log(`\n${title}`);
}

const src = fs.readFileSync(appJsPath, "utf8");

// ── 1. 提取纯函数 ────────────────────────────────────────────────
section("[1] 提取纯函数段");
// 注意：标记必须**整行**跳过（begin 标记行后面还有 ` ──` 尾注，直接捕获会把
// 注释正文当代码 ⇒ new Function 报 "Invalid or unexpected token"）。
const mathMatch = src.match(
  /@opt-virtual-math:begin[^\n]*\n([\s\S]*?)\/\/[^\n]*@opt-virtual-math:end/
);
check(
  "能提取到 @opt-virtual-math 纯函数段（标记被删/改名要同步改本测试）",
  !!mathMatch
);
if (!mathMatch) {
  console.log("\n无法继续：纯函数段缺失。");
  process.exit(1);
}

let computeOptWindow = null;
let computeOptScale = null;
try {
  const fns = new Function(
    mathMatch[1] + "\nreturn { computeOptWindow: computeOptWindow, computeOptScale: computeOptScale };"
  )();
  computeOptWindow = fns.computeOptWindow;
  computeOptScale = fns.computeOptScale;
} catch (e) {
  check("纯函数段可执行", false, String(e && e.message));
  process.exit(1);
}
check("computeOptWindow 是函数", typeof computeOptWindow === "function");
check(
  "computeOptScale 是函数（超长列表压缩映射，缺了就会滚不到底）",
  typeof computeOptScale === "function"
);
if (typeof computeOptWindow !== "function" || typeof computeOptScale !== "function") process.exit(1);

// 生产参数：优化窗 1640×1140，--qst-opt-u≈1.491 ⇒ 行高 ≈ 38*1.491 = 56.66px
// ⚠ overscan 必须**从源码读**，不能写死在测试里 —— 否则谁把 OPT_VIRT_OVERSCAN
//   改成 5000（DOM 又变成无上限），测试仍会绿（本测试第一版就踩了这个坑）。
const ROW_H = 38 * 1.491;
const VIEW_H = 900;

section("[1b] 生产常量绑定");
const overMatch = src.match(/const\s+OPT_VIRT_OVERSCAN\s*=\s*(\d+)/);
check("能从 app.js 读到 OPT_VIRT_OVERSCAN", !!overMatch);
if (!overMatch) {
  console.log("\n无法继续：overscan 常量缺失。");
  process.exit(1);
}
const OVER = Number(overMatch[1]);
check(
  `生产 overscan 有上限（当前 ${OVER}，要求 0..50 —— 决定 DOM 行数上限）`,
  OVER >= 0 && OVER <= 50,
  `overscan=${OVER} ⇒ 窗口会渲染约 ${Math.ceil(VIEW_H / ROW_H) + 2 * OVER} 行`
);

// 压缩上限同理必须**从源码读**：写死的话，谁把 OPT_MAX_VSPACE_PX 调到 40M
// （超过 Chrome/Edge 的 33.5M 上限 ⇒ 又滚不到底）测试仍会绿。
const capMatch = src.match(/const\s+OPT_MAX_VSPACE_PX\s*=\s*([0-9_]+)/);
check("能从 app.js 读到 OPT_MAX_VSPACE_PX", !!capMatch);
if (!capMatch) {
  console.log("\n无法继续：压缩上限常量缺失。");
  process.exit(1);
}
const MAX_VSPACE = Number(capMatch[1].replace(/_/g, ""));
// 各浏览器单元素高度上限：Chrome/Edge ≈33,554,432、Firefox ≈17,800,000、Safari ≈16,700,000。
// 官网 Demo 用的是同一份 app.js、跑在真浏览器里 ⇒ 必须取跨浏览器安全值。
check(
  `压缩上限在跨浏览器安全区内（当前 ${MAX_VSPACE.toLocaleString()}px ≤ Safari 的 ≈16.7M）`,
  MAX_VSPACE > 0 && MAX_VSPACE <= 16_700_000,
  `MAX_VSPACE=${MAX_VSPACE}`
);

const cssText = fs.readFileSync(indexHtmlPath, "utf8");
check(
  "CSS 行高仍是 38px * --qst-opt-u（optRowHeightPx 的推算基准，改了要同步两边）",
  /\.opt-list\s+\.arow\{[^}]*height:calc\(38px\s*\*\s*var\(--qst-opt-u\)\)/.test(cssText)
);

// ── 2. 窗口数学 ──────────────────────────────────────────────────
section("[2] 窗口数学与边界夹取");

const w0 = computeOptWindow(0, VIEW_H, ROW_H, 58000, OVER);
check("首屏 start=0", w0.start === 0, `start=${w0.start}`);
check("首屏 end 覆盖视口且含 overscan", w0.end >= Math.ceil(VIEW_H / ROW_H) && w0.end <= Math.ceil(VIEW_H / ROW_H) + OVER, `end=${w0.end}`);

const wMid = computeOptWindow(1000, VIEW_H, ROW_H, 58000, OVER);
check("滚动中段 start 回退 overscan", wMid.start === Math.max(0, Math.floor(1000 / ROW_H) - OVER), `start=${wMid.start}`);
check("滚动中段 end 前瞻 overscan", wMid.end === Math.ceil((1000 + VIEW_H) / ROW_H) + OVER, `end=${wMid.end}`);

const maxTop = 58000 * ROW_H - VIEW_H;
const wEnd = computeOptWindow(maxTop, VIEW_H, ROW_H, 58000, OVER);
check("滚到最底 end 夹取到 total（不越界）", wEnd.end === 58000, `end=${wEnd.end}`);
check("滚到最底窗口非空", wEnd.end > wEnd.start, `[${wEnd.start},${wEnd.end})`);

const wSmall = computeOptWindow(0, VIEW_H, ROW_H, 5, OVER);
check("小数据量 end 夹取到 total", wSmall.start === 0 && wSmall.end === 5, JSON.stringify(wSmall));

const wZero = computeOptWindow(0, VIEW_H, ROW_H, 0, OVER);
check("total=0 ⇒ 空窗口", wZero.start === 0 && wZero.end === 0, JSON.stringify(wZero));

const wZeroRow = computeOptWindow(0, VIEW_H, 0, 100, OVER);
check("rowH=0 ⇒ 空窗口（不产生 NaN/Infinity）", wZeroRow.start === 0 && wZeroRow.end === 0, JSON.stringify(wZeroRow));

const wNeg = computeOptWindow(-500, VIEW_H, ROW_H, 58000, OVER);
check("负 scrollTop 当 0 处理（橡皮筋/异常不炸）", wNeg.start === 0 && Number.isFinite(wNeg.end), JSON.stringify(wNeg));

const wNaN = computeOptWindow(NaN, VIEW_H, ROW_H, 58000, OVER);
check("NaN scrollTop 不产生 NaN 区间", Number.isFinite(wNaN.start) && Number.isFinite(wNaN.end), JSON.stringify(wNaN));

const wZeroOver = computeOptWindow(0, VIEW_H, ROW_H, 58000, 0);
check("overscan=0 仍覆盖视口", wZeroOver.start === 0 && wZeroOver.end === Math.ceil(VIEW_H / ROW_H), JSON.stringify(wZeroOver));

// ── 2b. 压缩映射（「超长列表滚不到底」的修复）────────────────────
section("[2b] 压缩映射 computeOptScale");

// 行按**真实行高**渲染 ⇒ 屏幕上装得下的行数恒为 viewH/rowH，与压缩比无关。
// 因此压缩必须对齐「可滚动区间」（realMax ↔ virtMax），**不能**按总高比例
// （ratio = virtualSize/actualSize）—— 后者滚到底时真实偏移只到
// `actualSize − viewH/ratio`，**最后一屏永远滚不到**，列表越长漏得越多。
/** 行 k 在屏幕上的 y（px）。负=在视口上方，>viewH=在视口下方。 */
const rowScreenY = (k, scrollTop, sc) =>
  k * ROW_H - (sc.ratio === 1 ? scrollTop : scrollTop / sc.ratio);

// 不超限 ⇒ 恒等。这是**零行为变化**的保证：5.8 万条（老场景）的滚动/选择/跳转
// 与压缩功能上线前逐字节等价（ratio===1 时 renderOptWindow 走的就是原公式）。
const sId = computeOptScale(58000, ROW_H, MAX_VSPACE, VIEW_H);
check(
  "不超限 ⇒ ratio=1 且 virtualSize=actualSize（老数据量零行为变化）",
  sId.ratio === 1 && sId.virtualSize === sId.actualSize &&
    Math.abs(sId.actualSize - 58000 * ROW_H) < 1e-6,
  JSON.stringify(sId)
);

// 边界：跨过上限的那一条必须开始压缩，紧邻其下必须仍恒等（判据是 <=，改成 < 会平白引入误差）
const nUnder = Math.floor(MAX_VSPACE / ROW_H);
const nOver = nUnder + 1;
const sUnder = computeOptScale(nUnder, ROW_H, MAX_VSPACE, VIEW_H);
const sOver = computeOptScale(nOver, ROW_H, MAX_VSPACE, VIEW_H);
check(
  "上限之下的最大条数 ⇒ 仍不压缩（边界不误伤）",
  sUnder.actualSize <= MAX_VSPACE && sUnder.ratio === 1,
  `n=${nUnder} actual=${sUnder.actualSize.toFixed(0)} ratio=${sUnder.ratio}`
);
check(
  "刚跨过上限 ⇒ 立刻开始压缩（不会「撑破」）",
  sOver.actualSize > MAX_VSPACE && sOver.ratio < 1 && sOver.virtualSize === MAX_VSPACE,
  `n=${nOver} actual=${sOver.actualSize.toFixed(0)} ratio=${sOver.ratio}`
);

// 超限 ⇒ 夹到上限。80 万条 × 56.66px ≈ 45.3M px > Chrome 的 33.5M ⇒ 旧实现必滚不到底。
const BIG_N = 800000;
const sBig = computeOptScale(BIG_N, ROW_H, MAX_VSPACE, VIEW_H);
check(
  "超限 ⇒ virtualSize 夹到上限（spacer 撑得开，滚动条有效）",
  sBig.virtualSize === MAX_VSPACE && sBig.actualSize > MAX_VSPACE,
  `virtualSize=${sBig.virtualSize} actualSize=${sBig.actualSize.toFixed(0)}`
);
check(
  "超限 ⇒ ratio = virtMax/realMax（对齐可滚动区间，不是总高比）",
  sBig.ratio < 1 &&
    Math.abs(sBig.ratio - (MAX_VSPACE - VIEW_H) / (sBig.actualSize - VIEW_H)) < 1e-12,
  `ratio=${sBig.ratio}`
);

const realMaxBig = sBig.actualSize - VIEW_H;
const virtMaxBig = sBig.virtualSize - VIEW_H;

// ★★ 核心不变量：滚到底时**最后一行必须落在视口内**。
//    这是「超长列表滚不到底」缺陷的判据本体；按总高比例压缩会在这里红。
const lastY = rowScreenY(BIG_N - 1, virtMaxBig, sBig);
check(
  "★★ 压缩态滚到最底 ⇒ 最后一行落在视口内 [0, viewH−rowH]",
  lastY >= 0 && lastY <= VIEW_H - ROW_H + 1e-6,
  `lastRowScreenY=${lastY.toFixed(1)} viewH=${VIEW_H}（按总高比压缩时是 ${(VIEW_H / (MAX_VSPACE / sBig.actualSize) - ROW_H).toFixed(0)}，早已滚出屏幕）`
);

// 反证（负对照）：按总高比例压缩（ratio = virtualSize/actualSize）最后一屏必然滚不到。
// 把它写成断言，是为了让「为什么必须对齐区间」永远可复现 —— 而不是一句注释。
const naiveRatio = MAX_VSPACE / sBig.actualSize;
const naiveLastY = (BIG_N - 1) * ROW_H - virtMaxBig / naiveRatio;
check(
  "反证：按总高比例压缩会让最后一行滚出屏幕（旧写法必红）",
  naiveLastY > VIEW_H,
  `naiveLastRowScreenY=${naiveLastY.toFixed(0)} > viewH=${VIEW_H}`
);

// 滚到底时窗口必须**包含最后一行**（renderOptWindow 的真实调用形态）
const wMaxC = computeOptWindow(realMaxBig, VIEW_H, ROW_H, BIG_N, OVER);
check(
  "压缩态滚到底：窗口包含最后一行（end 夹到 total）",
  wMaxC.end === BIG_N && wMaxC.start <= BIG_N - 1,
  JSON.stringify(wMaxC)
);

// 每一行都可达：∃ scrollTop ∈ [0, virtMax] 使该行落在视口内。
// 行 k 可见只需 k·rowH ∈ [0, realMax + viewH)；端点 k=total−1 落在 [viewH−rowH, viewH)。
let reachViolation = "";
const sampleRows = [0, 1, Math.floor(BIG_N / 2), BIG_N - 2, BIG_N - 1];
for (const k of sampleRows) {
  const want = Math.max(0, Math.min(virtMaxBig, k * ROW_H * sBig.ratio));
  const y = rowScreenY(k, want, sBig);
  if (!(y >= -1e-6 && y <= VIEW_H + 1e-6)) {
    reachViolation = `行 ${k} 最优 scrollTop=${want.toFixed(0)} 下 y=${y.toFixed(1)}`;
    break;
  }
}
check(
  `每个行号都可达（抽样 ${sampleRows.join("/")}，含首末行）`,
  !reachViolation,
  reachViolation
);

// 压缩态下窗口仍必须覆盖「屏幕上真正可见的行」，且行数仍有上限。
// 这里喂的是 renderOptWindow 的实际输入（压缩映射后的真实偏移），等于把整条链路串起来跑。
let coverC = "";
let capC = "";
const capRowsC = Math.ceil(VIEW_H / ROW_H) + 2 * OVER + 1;
for (let step = 0; step <= 200; ++step) {
  const s = (virtMaxBig * step) / 200;
  const off = s / sBig.ratio;
  const w = computeOptWindow(off, VIEW_H, ROW_H, BIG_N, OVER);
  if (w.end - w.start > capRowsC) {
    capC = `s=${s.toFixed(0)} 窗口 ${w.end - w.start} 行 > 上限 ${capRowsC}`;
    break;
  }
  const firstVis = Math.min(BIG_N - 1, Math.floor(off / ROW_H));
  const lastVis = Math.min(BIG_N - 1, Math.floor((off + VIEW_H - 1) / ROW_H));
  if (w.start > firstVis || w.end <= lastVis) {
    coverC = `s=${s.toFixed(0)} 窗口 [${w.start},${w.end}) 漏可见行 ${firstVis}..${lastVis}`;
    break;
  }
}
check("压缩态：任意滚动位置窗口都覆盖屏幕可见行（不漏行/不露白）", !coverC, coverC);
check("压缩态：窗口行数仍有上限（DOM 恒定，与条数无关）", !capC, capC);

// 退化输入：绝不产生 NaN / Infinity / 负数 / ratio>1（否则 spacer 高度写进 style 会炸）
const degenerate = [
  [0, ROW_H, MAX_VSPACE, VIEW_H],        // 空列表
  [58000, 0, MAX_VSPACE, VIEW_H],        // 行高 0
  [58000, ROW_H, 0, VIEW_H],             // 上限 0（配置缺失）
  [-5, ROW_H, MAX_VSPACE, VIEW_H],       // 负条数
  [NaN, ROW_H, MAX_VSPACE, VIEW_H],      // NaN 条数
  [58000, NaN, MAX_VSPACE, VIEW_H],      // NaN 行高
  [Infinity, ROW_H, MAX_VSPACE, VIEW_H], // Infinity 条数
  [BIG_N, ROW_H, MAX_VSPACE, 0],         // 视口高 0（面板尚未显示）
  [BIG_N, ROW_H, MAX_VSPACE, NaN],       // NaN 视口高
  [BIG_N, ROW_H, MAX_VSPACE, -100],      // 负视口高
];
let degenViolation = "";
for (const [n, h, cap, vh] of degenerate) {
  const s = computeOptScale(n, h, cap, vh);
  const ok =
    Number.isFinite(s.ratio) && Number.isFinite(s.virtualSize) &&
    Number.isFinite(s.actualSize) && s.ratio >= 0 && s.ratio <= 1 && s.virtualSize >= 0;
  if (!ok) { degenViolation = `(${n},${h},${cap},${vh}) ⇒ ${JSON.stringify(s)}`; break; }
}
check("退化输入不产生 NaN/负数/ratio>1", !degenViolation, degenViolation);

// 双射：压缩空间与真实空间在可滚动区间上一一对应 ⇒ 不跳段
const sBi = computeOptScale(1_000_000, ROW_H, MAX_VSPACE, VIEW_H);
const realMaxBi = sBi.actualSize - VIEW_H;
const virtMaxBi = sBi.virtualSize - VIEW_H;
let bijViolation = "";
for (let k = 0; k <= 20; ++k) {
  const actual = (realMaxBi * k) / 20;
  const scrollTop = actual * sBi.ratio;   // 真实 → 压缩
  const back = scrollTop / sBi.ratio;     // 压缩 → 真实
  if (Math.abs(back - actual) > 1e-6) { bijViolation = `actual=${actual} → ${back}`; break; }
}
check("压缩映射在可滚动区间上是双射（真实↔压缩可逆，无死区）", !bijViolation, bijViolation);
check(
  "压缩后真实可滚距离与压缩可滚距离同量级对齐（比例恒等于 ratio）",
  Math.abs(realMaxBi * sBi.ratio - virtMaxBi) < 1e-6,
  `realMax*ratio=${(realMaxBi * sBi.ratio).toFixed(3)} virtMax=${virtMaxBi.toFixed(3)}`
);

// 反证：不做压缩（ratio 强置 1）时，80 万条的真实总高确实超浏览器上限 ⇒ 旧路径必坏。
// 这条把「为什么需要压缩」钉成数字，避免以后有人觉得压缩是多余的。
check(
  "反证：80 万条不做压缩时真实总高超过 Chrome/Edge 的 33.5M 上限",
  BIG_N * ROW_H > 33_554_432,
  `naive=${(BIG_N * ROW_H / 1e6).toFixed(1)}M px`
);

// ── 3. 核心不变量 ────────────────────────────────────────────────
section("[3] 核心不变量（防回归）");

const TOTAL = 58000;
const cap = Math.ceil(VIEW_H / ROW_H) + 2 * OVER + 1;
let capViolation = "";
let coverViolation = "";

for (let step = 0; step <= 200; ++step) {
  const top = (maxTop * step) / 200;
  const w = computeOptWindow(top, VIEW_H, ROW_H, TOTAL, OVER);

  // B1. DOM 恒定：窗口行数必须有上限（谁把 overscan 去掉/改成无限追加，这里红）
  if (w.end - w.start > cap) {
    capViolation = `top=${top.toFixed(1)} 窗口 ${w.end - w.start} 行 > 上限 ${cap}`;
    break;
  }

  // B2. 覆盖视口：可见的第一行和最后一行都必须在窗口内
  const firstVisible = Math.min(TOTAL - 1, Math.floor(top / ROW_H));
  const lastVisible = Math.min(TOTAL - 1, Math.floor((top + VIEW_H - 1) / ROW_H));
  if (w.start > firstVisible || w.end <= lastVisible) {
    coverViolation = `top=${top.toFixed(1)} 窗口 [${w.start},${w.end}) 漏掉可见行 ${firstVisible}..${lastVisible}`;
    break;
  }
}

check(`窗口行数有上限（DOM 恒定，≤ ${cap} 行 / 5.8 万条动作）`, !capViolation, capViolation);
check("任意滚动位置都覆盖视口可见行（不漏行、不露白）", !coverViolation, coverViolation);

// B3. 相邻滚动连续：**步长小于一个视口**时相邻窗口必须重叠。
//     真实滚动是逐帧小步的（一帧几十 px），窗口只要每帧都覆盖可见行就不会跳行；
//     若 overscan 被改成负数/去掉保护，这里就会出现空洞。
const stepPx = VIEW_H / 2;
let contViolation = "";
let contChecked = 0;
for (let top = 0; top + VIEW_H <= maxTop + stepPx; top += stepPx) {
  const a = computeOptWindow(top, VIEW_H, ROW_H, TOTAL, OVER);
  const b = computeOptWindow(top + stepPx, VIEW_H, ROW_H, TOTAL, OVER);
  contChecked++;
  if (b.start > a.end) {
    contViolation = `top=${top.toFixed(0)}->${(top + stepPx).toFixed(0)} 出现空洞 ${a.end}..${b.start}`;
    break;
  }
}
check(
  `步长 ${stepPx}px（<视口高）滚动时相邻窗口重叠（共查 ${contChecked} 个位置，不跳行）`,
  !contViolation,
  contViolation
);

// 行高校准漂移：行高差 0.5px 时总高误差应可控
const drift = Math.abs(58000 * ROW_H - 58000 * (ROW_H + 0.5));
check(
  "行高 0.5px 误差下总高漂移可感知（说明必须做实测校准）",
  drift > 10000,
  `drift=${drift.toFixed(0)}px`
);

// ── 4. 静态守卫（防「改回去」）────────────────────────────────────
section("[4] 静态守卫");

check(
  "渲染期不再有选择态快照 new Set(state.optSelected)（这正是「全选只选到已加载部分」的根因）",
  !/new Set\(\s*state\.optSelected\s*\)/.test(src),
  "app.js 里仍有渲染期快照"
);
check(
  "渲染窗口时实时读取选择态（optSelSet()）",
  /function renderOptWindow\(/.test(src) && /renderOptWindow[\s\S]{0,1200}?optSelSet\(\)/.test(src),
  "renderOptWindow 未使用 optSelSet()"
);
check(
  "旧的追加式分批加载函数已移除（appendOptRowChunk / fillOptRowsToViewport / bindOptRowsLazyLoad）",
  !/appendOptRowChunk|fillOptRowsToViewport|bindOptRowsLazyLoad/.test(src)
);
check(
  "窗口化骨架存在（OPT_VIRT_OVERSCAN / buildOptViewport / renderOptWindow / scrollOptToIndex）",
  /const OPT_VIRT_OVERSCAN/.test(src) &&
    /function buildOptViewport/.test(src) &&
    /function renderOptWindow/.test(src) &&
    /function scrollOptToIndex/.test(src)
);
check(
  "全选/快选后重绘当前窗口（renderOptWindow(true) 紧跟 markOptSelDirty）",
  /markOptSelDirty\(\)[\s\S]{0,300}?renderOptWindow\(true\)/.test(src),
  "快速选择路径未重绘窗口"
);
check(
  "CSS 有 .opt-vspace / .opt-vwin（缺了窗口会全叠在顶部）",
  /\.opt-vspace\{/.test(cssText) && /\.opt-vwin\{/.test(cssText)
);
check(
  "行勾选态由 selSet.has(i) 决定（配合实时 optSelSet() 才能让全选覆盖未加载部分）",
  /function optRowHtml[\s\S]{0,600}?selSet\.has\(i\)/.test(src)
);

// ── 4b. 压缩映射的接线（光有纯函数、没接到渲染路径上等于没做）──────
check(
  "CSS .opt-list 禁用滚动锚定（overflow-anchor:none，防每帧重建窗口时浏览器帮调 scrollTop 造成跳动）",
  /\.opt-list\{[^}]*overflow-anchor\s*:\s*none/.test(cssText),
  "缺少 overflow-anchor:none"
);
check(
  "总高重算都走 computeOptScale（建骨架 + 行高变化 + 定义本体 ≥3 处）",
  (src.match(/computeOptScale\(/g) || []).length >= 3,
  `调用点 ${(src.match(/computeOptScale\(/g) || []).length} 处`
);
check(
  "压缩比按**视口高**对齐可滚动区间（computeOptScale 调用都带 optViewportH）",
  (src.match(/computeOptScale\([^)]*optViewportH/g) || []).length >= 2,
  `带视口参数的调用点 ${(src.match(/computeOptScale\([^)]*optViewportH/g) || []).length} 处`
);
check(
  "★ 不再把视口真实容量算成 viewH/ratio（那会让最后一屏永远滚不到）",
  !/viewH\s*\/\s*v\.ratio/.test(src),
  "app.js 里仍有 viewH / v.ratio"
);
check(
  "滚动渲染做了压缩换算（renderOptWindow 里读 v.ratio）",
  /function renderOptWindow[\s\S]{0,2000}?v\.ratio/.test(src),
  "renderOptWindow 未做压缩换算 ⇒ 超长列表会错位/露白"
);
check(
  "跳转定位做了压缩换算（scrollOptToIndex 里读 v.ratio）",
  /function scrollOptToIndex[\s\S]{0,2000}?v\.ratio/.test(src),
  "scrollOptToIndex 未做压缩换算 ⇒ 关键操作查找会跳错位置"
);
check(
  "换录制时重置关键操作查找游标（_optKeySearchIdx = null，否则从上一份的行号往下找）",
  /state\._optKeySearchIdx\s*=\s*null/.test(src)
);

// ── 5. 量化对比（证据，非断言）────────────────────────────────────
section("[5] 量化对比：5.8 万条动作的 DOM 规模");
const rowsOld = TOTAL;
const rowsNew = Math.ceil(VIEW_H / ROW_H) + 2 * OVER;
const spansPerRow = 4; // chk / 序号 / 动作名 / 备注
console.log(`  旧（分批追加，滚到底）: ${rowsOld} 行 ≈ ${rowsOld * spansPerRow} 个 DOM 节点`);
console.log(`  新（窗口化）        : ${rowsNew} 行 ≈ ${rowsNew * spansPerRow} 个 DOM 节点`);
console.log(
  `  ⇒ 节点数降为 1/${Math.round((rowsOld * spansPerRow) / (rowsNew * spansPerRow))}，与录制长度无关`
);

console.log("\n  超长录制的总高（spacer 高度）与压缩比：");
console.log("    条数        真实总高      压缩后         ratio      旧实现");
for (const N of [58000, 200000, 360000, 800000, 1000000]) {
  const sc = computeOptScale(N, ROW_H, MAX_VSPACE, VIEW_H);
  const naiveM = (N * ROW_H) / 1e6;
  const broken = N * ROW_H > 33_554_432;
  console.log(
    `    ${String(N).padStart(8)}  ${naiveM.toFixed(1).padStart(8)}M  ${(sc.virtualSize / 1e6).toFixed(1).padStart(8)}M` +
      `  ${sc.ratio.toFixed(5).padStart(9)}   ${broken ? "✖ 滚不到底" : "✔ 正常"}`
  );
}
console.log("    （360k ≈ 20ms/点连续录制 2 小时；1M 是按钮连点的量级）");

// ── 汇总 ─────────────────────────────────────────────────────────
console.log(`\n${"=".repeat(60)}`);
console.log(`通过 ${passed}，失败 ${failed}`);
if (failed) {
  console.log("失败项：");
  failures.forEach((f) => console.log("  - " + f));
  process.exit(1);
}
console.log("优化列表虚拟滚动：全部通过");
