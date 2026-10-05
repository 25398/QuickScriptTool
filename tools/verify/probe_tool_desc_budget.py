#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""probe_tool_desc_budget.py — 看「工具描述在**网页 AI 路径**下还剩多少」.

起因（2026-10-02，A2）：
  给 `completeTask` 的 description **末尾**加了一段「收尾判据」（用户报
  "已经完成我要的操作了，但他不懂验收"）。加完源码里能搜到、自检也绿，
  但用户那头**看不出任何变化** —— 于是要问一句「到底生效了吗」。

根因：
  `src/web_ai/web_ai_prompt.cpp` 的 `RenderToolList()` 对**每个**描述做
      r.desc = OneLine(fn->value("description", ""), 160);
  ⚠ 160 是 **std::string 的字节数**，不是字符数 —— 中文一个字 3 字节
    ⇒ 实际只剩约 **53 个汉字**。排在描述后半段的文字**根本没发给模型**。
  （原生 API 路径不截断：`agent_core.cpp` 直接 tool["function"]["description"]，
    所以**同一份源码，两条路径下模型看到的东西不一样**。）

用法:
  python tools/verify/probe_tool_desc_budget.py                 # 默认查 completeTask
  python tools/verify/probe_tool_desc_budget.py activateWindow completeTask ...
  python tools/verify/probe_tool_desc_budget.py --budget 160 completeTask
"""
import argparse
import io
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "src", "macro_execute_tools.cpp")

# ⚠ 工具**不只在** macro_execute_tools.cpp 里定义 —— 分散在 5 个文件：
#   agent_reference.cpp(2) / agent_shell.cpp(9) / agent_tools.cpp(30+) /
#   agent_web.cpp(2) / macro_execute_tools.cpp(36)
#   ⇒ 默认**自动发现**：扫 src/ 下所有含 `tool.name = L"` 的 .cpp（漏一个文件就漏一批工具）
SRC_GLOB_DIR = os.path.join(REPO, "src")
RE_TOOLNAME_PROBE = re.compile(r'tool\.name\s*=\s*L"')

# 工具定义形如：  tool.name = L"completeTask";  ...  tool.description = L"..." L"..." ;
RE_NAME = re.compile(r'tool\.name\s*=\s*L"([^"]*)"')
RE_DESC_START = re.compile(r"tool\.description\s*=")
RE_WSTR = re.compile(r'L"((?:[^"\\]|\\.)*)"')

# ── 枚举取值：**目录里唯一能把「取值」送到模型眼前的通道**（LESSONS §80）───────
#   `RenderToolCatalog` 会印成 `readScriptReference(section:all|format|…)`。
#   ⚠ 但有**两道**上限，超了就**整组丢弃**（模型一个取值都看不到 ⇒ 只能自己编）：
#       单个取值 > kMaxEnumValue(24)  ⇒ 丢
#       整组总长 > kMaxEnumChars(160) ⇒ 丢
#     ⚠ 旧版是「任一取值 > 12 ⇒ 丢」，实测**静默丢掉 3 组**（switchWindow / computer / …）。
RE_PARAM_RAW = re.compile(
    r'parameters_json\s*=\s*L?R"(?P<delim>[^()\\\s]{0,16})\((?P<body>.*?)\)(?P=delim)"', re.S)
RE_ENUM = re.compile(r'"enum"\s*:\s*\[(.*?)\]', re.S)
RE_JSON_STR = re.compile(r'"((?:[^"\\]|\\.)*)"')

# 必须与 src/web_ai/web_ai_prompt.cpp 的常量保持一致
MAX_ENUM_CHARS = 160
MAX_ENUM_VALUE = 24


def extract_enums(path: str):
    """返回 [(工具名, 行号, [取值...])]（只取 `parameters_json` raw string 里的 enum）。"""
    text = io.open(path, encoding="utf-8", newline="").read()
    out = []
    marks = [(m.start(), m.group(1)) for m in RE_NAME.finditer(text)]
    for i, (pos, name) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else len(text)
        block = text[pos:end]
        for m in RE_PARAM_RAW.finditer(block):
            body = m.group("body")
            for em in RE_ENUM.finditer(body):
                vals = RE_JSON_STR.findall(em.group(1))
                if not vals:
                    continue
                line = text[: pos + m.start() + em.start()].count("\n") + 1
                out.append((name, line, vals))
    return out


def enum_dropped(vals):
    """复刻渲染器判据：返回 (是否丢弃, 原因)。"""
    for v in vals:
        if len(v) > MAX_ENUM_VALUE:
            return True, "单个取值 %d 字符 > %d" % (len(v), MAX_ENUM_VALUE)
    total = sum(len(v) for v in vals) + (len(vals) - 1)
    if total > MAX_ENUM_CHARS:
        return True, "整组总长 %d > %d" % (total, MAX_ENUM_CHARS)
    return False, ""


def unescape(s: str) -> str:
    # C++ 宽字符串字面量里的转义（我们只用到 \" 与 \\）
    return s.replace('\\"', '"').replace("\\\\", "\\")


def read_wide_literals(text: str, start: int) -> str:
    """从 start 起读一串 `L"..."` 并拼接，遇到**不在字符串内**的 `;` 结束。

    ⚠ 用**线性状态机**而不是正则：`(?:[^"\\]|\\.)*"` 遇到未闭合的字面量会
      灾难性回溯（第一版就是卡死在这里被 SIGTERM 掉的）。
    """
    parts = []
    i, n = start, len(text)
    while i < n:
        c = text[i]
        if c == ";":
            break
        if c == "L" and i + 1 < n and text[i + 1] == '"':
            i += 2
            buf = []
            while i < n:
                ch = text[i]
                if ch == "\\" and i + 1 < n:
                    nxt = text[i + 1]
                    buf.append(nxt if nxt not in '\\"' else nxt)
                    i += 2
                    continue
                if ch == '"':
                    i += 1
                    break
                buf.append(ch)
                i += 1
            parts.append(unescape("".join(buf)))
            continue
        i += 1
    return "".join(parts)


def extract_descriptions(path: str):
    """返回 {tool_name: description_text}（按源码里 tool.name 与随后的 description 配对）。"""
    text = io.open(path, encoding="utf-8", newline="").read()
    out = {}
    marks = [(m.start(), m.group(1)) for m in RE_NAME.finditer(text)]
    for i, (pos, name) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else len(text)
        block = text[pos:end]
        m = RE_DESC_START.search(block)
        if not m:
            continue
        desc = read_wide_literals(block, m.end())
        if desc:
            out.setdefault(name, desc)
    return out


def one_line(s: str) -> str:
    """复刻 web_ai_prompt.cpp 的 OneLine()：压平空白 + 去重空格 + trim。"""
    out, prev_space = [], False
    for ch in s:
        ws = ch in "\r\n\t"
        c = " " if ws else ch
        if c == " " and prev_space:
            continue
        prev_space = (c == " ")
        out.append(c)
    return "".join(out).strip()


def truncate_utf8_safe(s: str, max_bytes: int) -> tuple:
    """复刻 TruncateUtf8Safe：按**字节**截，且不切碎多字节字符。"""
    raw = s.encode("utf-8")
    if len(raw) <= max_bytes:
        return s, False
    cut = raw[:max_bytes]
    while cut and (cut[-1] & 0xC0) == 0x80:  # 退续字节
        cut = cut[:-1]
    if cut:
        cut = cut[:-1]  # 再退可能被切断的头字节（保守，与实现一致）
    return cut.decode("utf-8", "ignore"), True


def extract_params(path: str):
    """返回 {工具名: parameters 的 JSON 文本}（只取 `parameters_json` raw string）。"""
    text = io.open(path, encoding="utf-8", newline="").read()
    out = {}
    marks = [(m.start(), m.group(1)) for m in RE_NAME.finditer(text)]
    for i, (pos, name) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else len(text)
        m = RE_PARAM_RAW.search(text[pos:end])
        if m:
            out.setdefault(name, m.group("body"))
    return out


def render_param_str(body: str) -> str:
    """复刻 `RenderToolCatalog` 的参数段：`名字[:短枚举], 名字, …`。

    ⚠ 顺序：`nlohmann::json` 的对象是 `std::map` ⇒ **按 key 字母序**，
      不是声明顺序（LESSONS §49）。这里用 sorted() 对齐。
    ⚠ 枚举：超上限就**整组不印**（与 `enum_dropped` 同一套判据）。
    """
    try:
        j = json.loads(body)
    except Exception:
        return "?"
    props = j.get("properties") if isinstance(j, dict) else None
    if not isinstance(props, dict):
        return ""
    parts = []
    for k in sorted(props.keys()):
        p = props[k]
        s = k
        if isinstance(p, dict) and isinstance(p.get("enum"), list):
            vals = [v if isinstance(v, str) else json.dumps(v) for v in p["enum"]]
            if vals and not enum_dropped(vals)[0]:
                s += ":" + "|".join(vals)
        parts.append(s)
    return ", ".join(parts)


def discover_sources():
    """自动发现所有「定义了工具」的源文件（含 `tool.name = L"` 的 .cpp）。"""
    hits = []
    for root, _dirs, files in os.walk(SRC_GLOB_DIR):
        for fn in files:
            if not fn.endswith(".cpp"):
                continue
            p = os.path.join(root, fn)
            try:
                txt = io.open(p, encoding="utf-8", newline="").read()
            except OSError:
                continue
            if RE_TOOLNAME_PROBE.search(txt):
                hits.append(p)
    return sorted(hits)


def load_all(srcs):
    """{工具名: 描述}；多文件合并（同名以**先出现**的为准，并提示冲突）。"""
    out, dup = {}, []
    for p in srcs:
        for n, d in extract_descriptions(p).items():
            if n in out and out[n] != d:
                dup.append((n, p))
                continue
            out.setdefault(n, d)
    return out, dup


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("tools", nargs="*", default=None,
                    help="要查的工具名（默认 completeTask）")
    ap.add_argument("--all", action="store_true", help="查全部工具（按超预算幅度排序）")
    ap.add_argument("--catalog", action="store_true",
                    help="打印「目录视图」：每个工具模型实际看到的那一行（默认按 48 字节）")
    ap.add_argument("--enum", action="store_true",
                    help="查「参数取值组」：哪些 enum 会被目录渲染器**整组丢弃**（模型只能自己编）")
    ap.add_argument("--budget", type=int, default=160,
                    help="RenderToolList 的单条描述字节预算（默认 160；--catalog 时默认 48）")
    ap.add_argument("--src", default=None,
                    help="只扫这一个源文件（默认自动发现 src/ 下所有含 tool.name 的 .cpp）")
    args = ap.parse_args()

    srcs = [args.src] if args.src else discover_sources()
    descs, dup = load_all(srcs)
    params = {}
    for p in srcs:
        for n, body in extract_params(p).items():
            params.setdefault(n, body)
    if not descs:
        print("!! 没能从任何源文件提取到工具描述（扫了 %d 个文件）" % len(srcs))
        return 2
    if dup:
        # ⚠⚠ 同名工具在**两个文件**里各定义一次、描述还不一样 —— 要么是死代码，
        #    要么真会进清单（那就是**重名工具**，模型可能拿到互相矛盾的两份定义）。
        print("⚠⚠ 同名工具重复定义（描述不一致）：")
        for n, p in dup:
            print("     %s  ← 另一份在 %s" % (n, os.path.relpath(p, REPO)))
        print("   ⇒ 确认哪一份真的进了工具清单；若两份都进，就是重名缺陷。")
        print()

    if args.enum:
        rows = []
        for p in srcs:
            for name, line, vals in extract_enums(p):
                dropped, why = enum_dropped(vals)
                rows.append((dropped, name, line, vals, why, p))
        print("【取值组视图】目录渲染器上限：单个取值 ≤ %d 字符、整组 ≤ %d 字符"
              % (MAX_ENUM_VALUE, MAX_ENUM_CHARS))
        print("（超上限 ⇒ **整组丢弃**：模型在目录里一个取值都看不到，只能自己编）")
        print("扫了 %d 个源文件，共 %d 组 enum" % (len(srcs), len(rows)))
        print("=" * 78)
        bad = [r for r in rows if r[0]]
        for dropped, name, line, vals, why, p in bad:
            print("  ⚠ %-22s %s:%d" % (name, os.path.relpath(p, REPO), line))
            print("     丢弃原因: %s" % why)
            print("     取值(%d): %s" % (len(vals), "|".join(vals)))
        print("-" * 78)
        print("会被丢弃的取值组 = %d / %d" % (len(bad), len(rows)))
        for dropped, name, line, vals, why, p in rows:
            if not dropped:
                print("  ok %-22s 取值(%d): %s" % (name, len(vals), "|".join(vals)))
        print("VERDICT=%s" % ("ENUM_DROPPED" if bad else "ALL_ENUMS_VISIBLE"))
        return 0

    if args.catalog:
        # 目录（RenderToolCatalog）默认 catalogDescChars = 48 字节 —— 模型选工具时就看这些
        budget = args.budget if args.budget != 160 else 48
        print("【目录视图】catalogDescChars = %d 字节（≈%d 个汉字）—— 这是模型挑工具时看到的"
              % (budget, budget // 3))
        print("（逐行复刻真实渲染：`序号. 名字(参数名[:短枚举]) — 描述`；"
              "完整定义要模型主动 loadTools 才给）")
        print("扫了 %d 个源文件，共 %d 个工具" % (len(srcs), len(descs)))
        print("=" * 78)
        for i, (n, d) in enumerate(descs.items(), 1):
            line = one_line(d)
            seen, cut = truncate_utf8_safe(line, budget)
            ps = render_param_str(params.get(n, ""))
            print("%2d. %s(%s) — %s%s" % (i, n, ps, seen, "…" if cut else ""))
        return 0

    if args.all:
        # 按「被截掉的字节数」降序：最该关心的排最前
        rows = []
        for n, d in descs.items():
            line = one_line(d)
            seen, cut = truncate_utf8_safe(line, args.budget)
            rows.append((len(line.encode("utf-8")) - len(seen.encode("utf-8")), n, line, seen, cut))
        rows.sort(reverse=True)
        print("源文件: %s" % (os.path.relpath(args.src, REPO) if args.src
                              else "%d 个（自动发现）" % len(srcs)))
        print("单条描述预算: %d 字节；共 %d 个工具" % (args.budget, len(rows)))
        print("=" * 78)
        over = 0
        for lost, n, line, seen, cut in rows:
            if not cut:
                continue
            over += 1
            print("  ⚠ %-22s 总 %4d 字节，丢 %4d 字节（%.0f%%）"
                  % (n, len(line.encode("utf-8")), lost,
                     100.0 * lost / max(1, len(line.encode("utf-8")))))
        print("-" * 78)
        print("超预算工具 = %d / %d" % (over, len(rows)))
        print("VERDICT=%s" % ("HAS_TRUNCATED_TOOL_DESC" if over else "ALL_WITHIN_BUDGET"))
        return 0

    print("源文件: %s" % (os.path.relpath(args.src, REPO) if args.src
                          else "%d 个（自动发现）" % len(srcs)))
    print("单条描述预算: %d 字节（RenderToolList/OneLine）" % args.budget)
    print("=" * 78)
    names = args.tools or ["completeTask"]
    bad = 0
    for n in names:
        d = descs.get(n)
        if d is None:
            print("[%s] !! 源码里找不到这个工具的 description" % n)
            bad += 1
            continue
        line = one_line(d)
        raw = line.encode("utf-8")
        seen, cut = truncate_utf8_safe(line, args.budget)
        print("[%s]" % n)
        print("  描述总长 = %d 字节 / %d 字符" % (len(raw), len(line)))
        if not cut:
            print("  ✅ 整段都在预算内，模型能看到全文")
            print("  模型看到: %s" % line)
        else:
            bad += 1
            print("  ⚠⚠ 被截断！模型只看得到前 %d 字节（约 %d 个汉字）:"
                  % (args.budget, args.budget // 3))
            print("  ---- 模型实际看到 ----")
            print("  %s…" % seen)
            print("  ---- 以下内容**永远没发给模型** ----")
            print("  %s" % line[len(seen):])
        print()
    print("=" * 78)
    print("VERDICT=%s" % ("HAS_TRUNCATED_TOOL_DESC" if bad else "ALL_WITHIN_BUDGET"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
