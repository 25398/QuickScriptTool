/**
 * ext_lint_js.js —— 用 ESLint 的 **Linter API** 对扩展 JS 做静态检查（不做任何自动修复）。
 *
 * 为什么需要它（2026-09-26 一天内栽了两次同类事故）：
 *   ① **同名函数覆盖**：函数声明提升，后定义的赢 ⇒ 我新加的 `waitTabComplete`
 *      **从来没跑过**（`node --check` 抓不到，重复声明在 JS 里合法）。
 *   ② **漏改一处引用**：把 `const r = await injectPageFn(...)` 换成别的之后，
 *      紧跟着的 `r && r.error` 还在用 `r` ⇒ **`ReferenceError`**，
 *      只在**运行时**炸 ⇒ 表现为"探针全绿、正常提问全红"（用户视角极难联想）。
 *
 *   ⇒ `node --check` 只查**语法**。这两类都是**作用域/引用**问题，必须用真正的解析器。
 *
 * 用法（由 `ext_lint.py` 调用）：
 *   node ext_lint_js.js <file.js> [file2.js ...]
 * 输出：每行一条 `文件:行:列 规则 消息`；无问题则输出 `OK`。
 */
'use strict';

const fs = require('fs');
const path = require('path');

let Linter;
try {
  ({ Linter } = require('eslint'));
} catch (e) {
  console.log('ESLINT_MISSING: ' + e.message);
  process.exit(3);
}

const linter = new Linter();

// 规则只挑**高价值、低噪音**的：
//   · no-undef        —— 未定义变量（就是这次 `ReferenceError` 那一类）
//   · no-redeclare    —— 同一作用域重复声明
//   · no-dupe-*       —— 重复参数/键/类成员
//   · no-func-assign  —— 覆盖函数声明（和"同名覆盖"同源）
//   · 其余几条是"几乎必然是写错"的语法级错误
const RULES = {
  'no-undef': 'error',
  'no-redeclare': 'error',
  'no-dupe-args': 'error',
  'no-dupe-keys': 'error',
  'no-dupe-class-members': 'error',
  'no-func-assign': 'error',
  'no-const-assign': 'error',
  'no-obj-calls': 'error',
  'no-unreachable': 'error',
  'no-unsafe-negation': 'error',
  'use-isnan': 'error',
  'valid-typeof': 'error',
  'no-self-assign': 'error',
  'no-self-compare': 'error',
  'no-sparse-arrays': 'error',
  'no-cond-assign': 'error',
  'no-constant-condition': 'error',
};

const BASE_CONFIG = {
  parserOptions: { ecmaVersion: 2022, sourceType: 'script' },
  // ⚠ 用 ESLint 内置 env：`webextensions` 提供 `chrome`，`browser` 提供 DOM 全局，
  //   `serviceworker` 提供 MV3 service worker 里那几个。**不要**手写 globals 清单
  //   —— 漏一个就会误报，误报多了这个守卫就没人看了。
  env: { browser: true, es2022: true, webextensions: true, serviceworker: true },
  rules: RULES,
};

let bad = 0;
let checked = 0;

for (const file of process.argv.slice(2)) {
  let code;
  try {
    code = fs.readFileSync(file, 'utf8');
  } catch (e) {
    console.log(`${file}:0:0 read-error ${e.message}`);
    bad++;
    continue;
  }
  checked++;
  let messages;
  try {
    messages = linter.verify(code, BASE_CONFIG, { filename: path.resolve(file) });
  } catch (e) {
    console.log(`${file}:0:0 lint-crash ${e.message}`);
    bad++;
    continue;
  }
  // 只报 error 级（warning 噪音大，这个守卫要保持干净）
  const errs = messages.filter((m) => m.severity === 2);
  for (const m of errs) {
    console.log(`${file}:${m.line}:${m.column} ${m.ruleId || 'parse'} ${m.message}`);
  }
  bad += errs.length;
}

if (bad === 0) {
  console.log(`OK 检查了 ${checked} 个文件`);
  process.exit(0);
}
console.log(`TOTAL ${bad}`);
process.exit(1);
