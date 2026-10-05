#!/usr/bin/env python3
"""web_ai_backend_smoke.py — 网页版 AI 适配层的纯逻辑冒烟测试。

被测对象：extension/edge/background.js 里的两个**注入函数**
  · qstReadAssistantReply(cfg)  按 selector 读最后一条 assistant 回复
  · qstProbeComposer(cfg)       探针：回报输入框形态

为什么能在没有浏览器的情况下测：它们都是**自包含纯函数**（只依赖 document /
window / location 这些全局），所以在 Node 里塞一套最小 DOM stub 就能跑。

⚠ 本脚本**不验证**选择器能否命中真实豆包页面 —— 那是必须连真浏览器的事
（见 docs/web-ai-backend-design.md §7 冒烟第 1 步）。这里只验证：
  ① 函数能被正确提取并在 stub 环境下运行
  ② 视口外元素**能**被读到（与 qstBuildPageSnapshot 的 isVisible 过滤相反，
     这是 docs §9 里 MEMORY.md §22 那条前科的直接回归）
  ③ 截断时**明说** truncated 与原始长度（MEMORY.md §20）
  ④ 选择器全部未命中时给出可行动的 triedSelectors，而不是静默空串
  ⑤ 探针能区分 contenteditable / 原生表单

用法：
  python tools/verify/web_ai_backend_smoke.py
退出码 0 = 全过。
"""

import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BG = ROOT / "extension" / "edge" / "background.js"
NODE = Path(r"C:\Users\冯思乾\.workbuddy-ai\binaries\node\versions\22.22.2-2\node.exe")
if not NODE.exists():
    NODE = "node"

failures = []
checks = 0


def check(name, cond, detail=""):
    global checks
    checks += 1
    if cond:
        print(f"  PASS  {name}")
    else:
        print(f"  FAIL  {name}  {detail}")
        failures.append(name)


def extract_function(src: str, name: str) -> str:
    """从 background.js 里抠出一个顶层 function 定义（按大括号配平）。"""
    m = re.search(rf"^function {re.escape(name)}\s*\(", src, re.M)
    if not m:
        raise SystemExit(f"FATAL: 找不到 function {name}")
    i = src.index("{", m.end() - 1)
    depth = 0
    j = i
    while j < len(src):
        c = src[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return src[m.start(): j + 1]
        j += 1
    raise SystemExit(f"FATAL: {name} 大括号不配平")


HARNESS = r"""
// ── 最小 DOM stub ────────────────────────────────────────────────
// 只为让注入函数跑起来：支持 querySelectorAll(sel) 的精确串匹配、
// getBoundingClientRect、getComputedStyle、isContentEditable、innerText/textContent。

function El(opts) {
  this.tagName = (opts.tagName || "DIV").toUpperCase();
  this._attrs = opts.attrs || {};
  this._rect = opts.rect || { width: 100, height: 30, top: 0, left: 0, right: 100, bottom: 30 };
  this.innerText = opts.innerText || "";
  this.textContent = opts.textContent || opts.innerText || "";
  this.value = opts.value;
  // 真实 DOM：contenteditable 属性存在 ⇒ isContentEditable 为 true
  const ce = this._attrs["contenteditable"];
  this.isContentEditable = ce === "true" || !!opts.contentEditable;
  this.disabled = !!opts.disabled;
  this._display = opts.display || "block";
  this._visibility = opts.visibility || "visible";
  this.scrollIntoViewCalls = 0;
}
El.prototype.getAttribute = function (k) {
  return Object.prototype.hasOwnProperty.call(this._attrs, k) ? this._attrs[k] : null;
};
El.prototype.getBoundingClientRect = function () { return this._rect; };
El.prototype.scrollIntoView = function () { this.scrollIntoViewCalls++; };

// selector → 元素列表。仅支持：tag、tag[attr='v']、tag[attr*='v']
function matchSelector(sel, el) {
  let m = sel.match(/^([a-z0-9]+)?((?:\[[^\]]+\])*)$/i);
  if (!m) return false;
  const tag = m[1];
  if (tag && el.tagName.toLowerCase() !== tag.toLowerCase()) return false;
  const attrPart = m[2] || "";
  const re = /\[([a-zA-Z0-9_-]+)\s*([*^$]?)=\s*['"]([^'"]*)['"]\]/g;
  let found;
  let n = 0;
  while ((found = re.exec(attrPart)) !== null) {
    n++;
    const [, key, op, val] = found;
    const actual = key === "class" ? String(el.getAttribute("class") || "") : String(el.getAttribute(key) || "");
    if (op === "*") { if (actual.indexOf(val) < 0) return false; }
    else if (op === "^") { if (actual.indexOf(val) !== 0) return false; }
    else if (op === "$") { if (!actual.endsWith(val)) return false; }
    else { if (actual !== val) return false; }
  }
  if (n === 0 && attrPart.length > 0) return false;
  return true;
}

var __els = [];
function setDom(list) { __els = list; }

// ★ 焦点：CDP 输入命令作用于「当前焦点」，所以 stub 必须能模拟 focus()
//   （qstFocusComposer 要回报 focusIsSelf —— 这是「字会不会打到别的框里」的判据）
var __activeElement = null;
El.prototype.focus = function () { __activeElement = this; };
El.prototype.click = function () { __activeElement = this; };

global.document = {
  querySelectorAll: function (sel) { return __els.filter(function (e) { return matchSelector(sel, e); }); },
  querySelector: function (sel) { var l = __els.filter(function (e) { return matchSelector(sel, e); }); return l.length ? l[0] : null; },
  title: "豆包 - 测试页",
  get activeElement() { return __activeElement; },
  // execCommand 只在「清空富文本」路径上用到；stub 里声明成功但不改内容，
  // 让被测函数走它的兜底分支（写 textContent）—— 这恰好覆盖了「框架不认 execCommand」的真实情况
  execCommand: function () { return false; },
  addEventListener: function () {},
};
global.window = {
  innerWidth: 1280,
  innerHeight: 800,
  getComputedStyle: function (el) {
    return { display: el._display || "block", visibility: el._visibility || "visible", opacity: "1" };
  },
};
global.location = { href: "https://www.doubao.com/chat/12345", hostname: "www.doubao.com" };
// InputEvent / Event：清空与写入路径都会派发它们
global.InputEvent = function (type, opts) { this.type = type; Object.assign(this, opts || {}); };
global.Event = function (type, opts) { this.type = type; Object.assign(this, opts || {}); };
El.prototype.dispatchEvent = function () { return true; };
global.__setDom = setDom;
global.__El = El;
"""


def main():
    src = BG.read_text(encoding="utf-8")

    # ── ① 提取成功性 ────────────────────────────────────────────
    print("[1] 提取注入函数")
    try:
        fn_read = extract_function(src, "qstReadAssistantReply")
        fn_probe = extract_function(src, "qstProbeComposer")
        # ★ 富文本 CDP 通道的三个页面侧函数（写入链路的前/后段）
        fn_focus_composer = extract_function(src, "qstFocusComposer")
        fn_composer_clear = extract_function(src, "qstComposerClearAndRead")
        fn_composer_read = extract_function(src, "qstComposerReadText")
        check("qstReadAssistantReply 可提取", True)
        check("qstProbeComposer 可提取", True)
        check("qstFocusComposer 可提取", True)
        check("qstComposerClearAndRead 可提取", True)
        check("qstComposerReadText 可提取", True)
    except SystemExit as e:
        print(e)
        return 1

    # 防回归：读回答**不得**含视口过滤（MEMORY.md §22 前科）
    check(
        "读回答不复用 isVisible 视口过滤",
        "isVisible" not in fn_read,
        "qstReadAssistantReply 里出现了 isVisible —— 回答滚出视口就会读不到",
    )
    check(
        "读回答不引用 __qstA11y 快照 store",
        "__qstA11y" not in fn_read,
        "读回答应走独立 selector，不该依赖 observePage 的快照 store",
    )

    # ── ①b 静态守卫：读「已 attach 的标签页 id」只有一种正确写法 ──────────
    # ⚠ 2026-09-24 实测事故（真机才暴露）：新加的 4 个网页 AI 处理器写成了
    #   `attachMeta.tabId`，而 `attachTab` 存的是 `meta.pageTabId`
    #   （background.js:1148-1149），attachMeta 里**根本没有 tabId 字段**
    #   ⇒ 四个处理器恒回 `NO_TAB: 未 attach 任何标签页`，
    #     而 `attach` 明明刚成功 —— 症状是"attach 成功但下一个调用说没 attach"，
    #     极易被误读成"扩展状态丢了 / service worker 重启了"。
    #   ⇒ 静态判据比真机复现便宜得多，而且是**确定性**的：既有 11 处都用 pageTabId。
    lines = src.splitlines()
    bad_lines = [i + 1 for i, line in enumerate(lines) if "attachMeta.tabId" in line]
    check("没有 attachMeta.tabId（既有字段名是 attachMeta.pageTabId）",
          not bad_lines, f"出现在行 {bad_lines}")
    page_tab_lines = [i + 1 for i, line in enumerate(lines) if "attachMeta.pageTabId" in line]
    check("attachMeta.pageTabId 仍是取标签页 id 的统一写法",
          len(page_tab_lines) >= 10, f"只有 {len(page_tab_lines)} 处")

    # ── ①c 静态守卫：注入页面时只能传**自包含**函数 ────────────────────
    # ⚠ 2026-09-24 真机事故：`executeScript({func: qstPrepareComposerForCdp})` ——
    #   该函数内部调用 `qstFocusComposer` / `qstComposerClearAndRead` 两个**同文件兄弟函数**，
    #   而 executeScript 只序列化 `func` 自己的源码 ⇒ 页面里 ReferenceError ⇒
    #   MV3 **resolve** 成 `{result: undefined, error:{...}}`（不是 reject）⇒
    #   只看 result 就把「函数崩了」误读成「选择器没命中」，报出误导性的 NO_COMPOSER。
    #   ⇒ 判据：组合函数**不得**直接当 func: 传；统一走 injectPageFn（它会带出 error）。
    combo_as_func = [i + 1 for i, line in enumerate(lines)
                     if re.search(r"func:\s*qstPrepareComposerForCdp\b", line)]
    check("组合函数 qstPrepareComposerForCdp 未被直接当 func: 注入",
          not combo_as_func, f"出现在行 {combo_as_func}")
    has_inject_fn = any("async function injectPageFn" in line for line in lines)
    check("injectPageFn 存在（注入一律经它，好把页面里的 error 带出来）", has_inject_fn)

    # ── ② 构造测试桩 ────────────────────────────────────────────
    cfg_doubao = {
        "inputSelectors": ["div[contenteditable='true'][role='textbox']", "textarea"],
        "responseSelectors": ["div[data-testid='message_text_content']"],
        "busySelectors": ["button[aria-label*='停止']"],
    }

    def run_case(dom_js, fn_name, cfg):
        """把桩 + 被测函数 + 用例拼成一个临时 .js 跑（避免命令行长度上限）。

        ⚠ 新增被测注入函数时，**必须**把它加进 FNS —— 否则会报
        `ReferenceError: xxx is not defined`，看着像函数没写对，实际是这儿漏了。
        """
        script = (
            HARNESS
            + "\n"
            + fn_read
            + "\n"
            + fn_probe
            + "\n"
            + fn_focus_composer
            + "\n"
            + fn_composer_clear
            + "\n"
            + fn_composer_read
            + "\n"
            + f"const CFG = {json.dumps(cfg)};\n"
            + dom_js
            + "\n"
            + f"const R = {fn_name}(CFG);\n"
            + "console.log(JSON.stringify(R));\n"
        )
        with tempfile.TemporaryDirectory() as td:
            js = Path(td) / "case.js"
            js.write_text(script, encoding="utf-8")
            p = subprocess.run(
                [str(NODE), str(js)], capture_output=True, text=True, encoding="utf-8", timeout=60
            )
        if p.returncode != 0:
            return None, (p.stderr or "").strip()
        try:
            return json.loads(p.stdout.strip().splitlines()[-1]), ""
        except Exception as e:
            return None, f"解析输出失败: {e} :: {p.stdout!r}"

    print("\n[2] 正常读回答")
    dom_normal = """
const nodes = [
  new __El({ tagName:'DIV', attrs:{'data-testid':'message_text_content'}, innerText:'第一段回答', rect:{width:600,height:40,top:100,left:0,right:600,bottom:140} }),
  new __El({ tagName:'DIV', attrs:{'data-testid':'message_text_content'}, innerText:'这是豆包的回答正文', rect:{width:600,height:120,top:200,left:0,right:600,bottom:320} }),
  new __El({ tagName:'DIV', attrs:{role:'textbox',contenteditable:'true'}, contentEditable:true, innerText:'', rect:{width:600,height:60,top:700,left:0,right:600,bottom:760} }),
];
__setDom(nodes);
"""
    r, err = run_case(dom_normal, "qstReadAssistantReply", cfg_doubao)
    if r is None:
        check("正常读回答（可运行）", False, err)
    else:
        check("取到最后一个回答节点", r.get("text") == "这是豆包的回答正文", str(r.get("text"))[:80])
        check("回报 totalBlocks", r.get("totalBlocks") == 2, str(r.get("totalBlocks")))
        check("回报 matchedBy", bool(r.get("matchedBy")), str(r.get("matchedBy")))
        check("未截断时 truncated=false", r.get("truncated") is False)
        check("inputEmpty 判据可用", r.get("inputEmpty") is True, str(r.get("inputEmpty")))
        check("busy=false（无停止按钮）", r.get("busy") is False)

    print("\n[3] ★ 视口外元素必须能读到（MEMORY.md §22 回归）")
    dom_offscreen = """
const nodes = [
  new __El({ tagName:'DIV', attrs:{'data-testid':'message_text_content'}, innerText:'屏外的长回答正文',
             // top 远超 vh=800 ⇒ 快照的 isVisible 会滤掉它
             rect:{width:600,height:200,top:5000,left:0,right:600,bottom:5200} }),
];
__setDom(nodes);
"""
    r, err = run_case(dom_offscreen, "qstReadAssistantReply", cfg_doubao)
    if r is None:
        check("屏外元素可读（可运行）", False, err)
    else:
        check(
            "★ 屏外回答仍被读到",
            r.get("ok") is True and r.get("text") == "屏外的长回答正文",
            f"这正是不复用 isVisible 的理由；got ok={r.get('ok')} text={r.get('text')!r}",
        )

    print("\n[4] busy 判据（停止生成按钮可见 ⇒ 仍在生成）")
    dom_busy = """
const nodes = [
  new __El({ tagName:'DIV', attrs:{'data-testid':'message_text_content'}, innerText:'正在生成中…', rect:{width:600,height:40,top:100,left:0,right:600,bottom:140} }),
  new __El({ tagName:'BUTTON', attrs:{'aria-label':'停止生成'}, innerText:'停止', rect:{width:40,height:40,top:750,left:900,right:940,bottom:790} }),
];
__setDom(nodes);
"""
    r, err = run_case(dom_busy, "qstReadAssistantReply", cfg_doubao)
    if r is None:
        check("busy 判据（可运行）", False, err)
    else:
        check("★ 识别到 busy=true", r.get("busy") is True, str(r.get("busy")))

    print("\n[5] 超长截断必须明说（MEMORY.md §20）")
    long_text = "字" * 15000
    dom_long = f"""
const nodes = [
  new __El({{ tagName:'DIV', attrs:{{'data-testid':'message_text_content'}}, innerText:{json.dumps(long_text)},
             rect:{{width:600,height:2000,top:100,left:0,right:600,bottom:2100}} }}),
];
__setDom(nodes);
"""
    r, err = run_case(dom_long, "qstReadAssistantReply", cfg_doubao)
    if r is None:
        check("超长截断（可运行）", False, err)
    else:
        check("★ truncated=true", r.get("truncated") is True)
        check("★ 回报原始长度 textLength", r.get("textLength") == 15000, str(r.get("textLength")))
        check("text 已截到 12000", len(r.get("text") or "") == 12000, str(len(r.get("text") or "")))

    print("\n[6] 选择器全未命中：必须给可行动的 triedSelectors，不许静默")
    dom_empty = "__setDom([]);"
    r, err = run_case(dom_empty, "qstReadAssistantReply", cfg_doubao)
    if r is None:
        check("未命中（可运行）", False, err)
    else:
        check("ok=false", r.get("ok") is False)
        check("reason=no-response-node", r.get("reason") == "no-response-node", str(r.get("reason")))
        check(
            "★ 回报 triedSelectors（改版时可行动）",
            r.get("triedSelectors") == cfg_doubao["responseSelectors"],
            str(r.get("triedSelectors")),
        )

    print("\n[7] 探针能区分富文本与原生表单")
    dom_rich = """
const nodes = [
  new __El({ tagName:'DIV', attrs:{role:'textbox',contenteditable:'true'}, contentEditable:true, rect:{width:600,height:60,top:700,left:0,right:600,bottom:760} }),
];
__setDom(nodes);
"""
    r, err = run_case(dom_rich, "qstProbeComposer", cfg_doubao)
    if r is None:
        check("探针（富文本）可运行", False, err)
    else:
        inp = (r or {}).get("input") or {}
        check("★ 探针识别 contentEditable", inp.get("contentEditable") is True, str(inp))
        check("探针回报 tagName", inp.get("tagName") == "div", str(inp.get("tagName")))
        check("探针回报 hasValue=false（富文本无 value）", inp.get("hasValue") is False, str(inp))

    print("\n[8] provider 配置与豆包 inputKind 对齐")
    prov_path = ROOT / "extension" / "edge" / "web_ai_providers.json"
    try:
        prov = json.loads(prov_path.read_text(encoding="utf-8"))
        providers = prov.get("providers") or {}
        check("web_ai_providers.json 可解析", True)
        check("含 doubao", "doubao" in providers)
        db = providers.get("doubao") or {}
        check("豆包 inputKind=richtext（与知乎调研一致）", db.get("inputKind") == "richtext",
              str(db.get("inputKind")))
        for field in ("inputSelectors", "sendButtonSelectors", "responseSelectors", "busySelectors"):
            v = db.get(field)
            check(f"豆包 {field} 非空数组", isinstance(v, list) and len(v) > 0, str(v))
        check("豆包 busySelectors 已配（README：必须配准）",
              any("停止" in s or "Stop" in s or "stop" in s for s in (db.get("busySelectors") or [])),
              str(db.get("busySelectors")))
    except Exception as e:
        check("web_ai_providers.json 可解析", False, str(e))

    print("\n[9] 富文本 CDP 通道的页面侧逻辑（写入链路的前后两段）")
    # qstFocusComposer：必须回报 focusIsSelf —— 焦点没落到输入框就不能插字
    dom_focus_ok = """
const el = new __El({ tagName:'DIV', attrs:{role:'textbox',contenteditable:'true'}, contentEditable:true,
                      rect:{width:600,height:60,top:700,left:0,right:600,bottom:760} });
el.focus = function(){ __activeElement = this; };
el.click = function(){ __activeElement = this; };
el.scrollIntoView = function(){};
__setDom([el]);
"""
    r, err = run_case(dom_focus_ok, "qstFocusComposer", cfg_doubao)
    if r is None:
        check("qstFocusComposer 可运行", False, err)
    else:
        check("★ 找到输入框", (r or {}).get("ok") is True, str(r))
        check("★ 焦点确实落到输入框（focusIsSelf）",
              (r or {}).get("focusIsSelf") is True, str(r))
        check("回报 matchedBy（可行动）", bool((r or {}).get("matchedBy")), str(r))
        check("识别为可编辑", (r or {}).get("isEditable") is True, str(r))

    # 焦点被别的元素抢走（如搜索框）⇒ 必须报 focusIsSelf=false，**不能**继续插字
    dom_focus_stolen = """
const el = new __El({ tagName:'DIV', attrs:{role:'textbox',contenteditable:'true'}, contentEditable:true,
                      rect:{width:600,height:60,top:700,left:0,right:600,bottom:760} });
const other = new __El({ tagName:'INPUT', attrs:{}, rect:{width:100,height:20,top:0,left:0,right:100,bottom:20} });
el.focus = function(){ __activeElement = other; };   // ★ 焦点没拿到
el.click = function(){};
el.scrollIntoView = function(){};
__setDom([el, other]);
"""
    r, err = run_case(dom_focus_stolen, "qstFocusComposer", cfg_doubao)
    if r is None:
        check("qstFocusComposer（焦点被抢）可运行", False, err)
    else:
        check("★★ 焦点被别处抢走时报 focusIsSelf=false（否则字会打到别的框里）",
              (r or {}).get("focusIsSelf") is False, str(r))
        check("焦点被抢时仍回报实际焦点在哪", bool((r or {}).get("activeTag")), str(r))

    # 不可见元素必须跳过（width/height 为 0 = 收起的隐藏输入框）
    dom_invisible = """
const hidden = new __El({ tagName:'TEXTAREA', attrs:{}, rect:{width:0,height:0,top:0,left:0,right:0,bottom:0} });
const real = new __El({ tagName:'DIV', attrs:{contenteditable:'true'}, contentEditable:true,
                        rect:{width:600,height:60,top:700,left:0,right:600,bottom:760} });
real.focus = function(){ __activeElement = this; };
real.click = function(){};
real.scrollIntoView = function(){};
__setDom([hidden, real]);
"""
    r, err = run_case(dom_invisible, "qstFocusComposer", cfg_doubao)
    if r is None:
        check("qstFocusComposer（含隐藏框）可运行", False, err)
    else:
        check("★ 跳过 0×0 的隐藏输入框，选中真正可见的那个",
              (r or {}).get("tagName") == "DIV", str(r))

    # qstComposerClearAndRead：清空后必须**回读**并回报长度
    dom_clear = """
const el = new __El({ tagName:'DIV', attrs:{contenteditable:'true'}, contentEditable:true,
                      rect:{width:600,height:60,top:700,left:0,right:600,bottom:760} });
el.textContent = '旧内容';
el.innerText = '旧内容';
el.focus = function(){ __activeElement = this; };
__setDom([el]);
"""
    r, err = run_case(dom_clear, "qstComposerClearAndRead", cfg_doubao)
    if r is None:
        check("qstComposerClearAndRead 可运行", False, err)
    else:
        check("★ 清空后回报 textLength（判清没清掉，不靠 API 自我声明）",
              (r or {}).get("textLength") == 0, str(r))

    # qstComposerReadText：写入后回读（这是「到底写没写进去」的唯一硬判据）
    dom_read = """
const el = new __El({ tagName:'DIV', attrs:{contenteditable:'true'}, contentEditable:true,
                      rect:{width:600,height:60,top:700,left:0,right:600,bottom:760} });
el.textContent = '你好，帮我看看这个界面';
el.innerText = '你好，帮我看看这个界面';
__setDom([el]);
"""
    r, err = run_case(dom_read, "qstComposerReadText", cfg_doubao)
    if r is None:
        check("qstComposerReadText 可运行", False, err)
    else:
        check("★ 回读到写入的内容", (r or {}).get("textLength", 0) > 0, str(r))
        check("回读内容正确", "你好" in str((r or {}).get("text")), str(r)[:120])

    # 找不到输入框时必须可行动地失败
    r, err = run_case("__setDom([]);\n", "qstComposerReadText", cfg_doubao)
    if r is None:
        check("qstComposerReadText（无输入框）可运行", False, err)
    else:
        check("★ 找不到输入框时 ok=false + reason（不静默）",
              (r or {}).get("ok") is False and bool((r or {}).get("reason")), str(r))

    print("\n[10] 新消息类型已接入分发与能力声明")
    src_bg = BG.read_text(encoding="utf-8")
    for mt in ("webAiRichType", "webAiSubmit"):
        check(f"onMessage 分发含 {mt}", f'type === "{mt}"' in src_bg, mt)
        check(f"capabilities 声明 {mt}", mt in src_bg, mt)
    check("★ 注释里不再引用不存在的 qstSubmitViaCdpEnter（此前是假注释）",
          "qstSubmitViaCdpEnter" not in src_bg, "仍有残留")
    check("qstFocusComposer 已定义", "function qstFocusComposer(" in src_bg)
    check("qstComposerClearAndRead 已定义", "function qstComposerClearAndRead(" in src_bg)
    check("qstComposerReadText 已定义", "function qstComposerReadText(" in src_bg)
    check("qstPrepareComposerForCdp 已定义", "function qstPrepareComposerForCdp(" in src_bg)

    # ── 汇总 ───────────────────────────────────────────────────
    print("\n" + "=" * 62)
    if failures:
        print(f"FAILED  {len(failures)}/{checks}")
        for f in failures:
            print("  - " + f)
        return 1
    print(f"ALL PASS  {checks}/{checks}")
    print("⚠ 本脚本只验证纯逻辑，不证明选择器能命中真实豆包页面。")
    print("  真站点冒烟见 docs/web-ai-backend-design.md §7 第 1 步。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
