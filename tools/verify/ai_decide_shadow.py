#!/usr/bin/env python3
"""AI 决策表 —— 离线影子测试 / 置信度校准（docs/ai-action-exec-optimization.md §23）

用法（在仓库根目录）：
    python tools/verify/ai_decide_shadow.py --selftest
    python tools/verify/ai_decide_shadow.py --log <player.log 或 webview_boot.log>
    python tools/verify/ai_decide_shadow.py --log a.log b.log --json out.json
    python tools/verify/ai_decide_shadow.py --log a.log --jevs

它回答的唯一问题（也是「要不要接本地 Jev 判断后端」的前置条件）：
    **判断表说「我有 0.68 把握」时，实际对了几成？换个阈值会不会更准？**

为什么必须先做这一步：
  · 把判决交给一个模型（本地 Jev / 小模型）之前，得先知道现在的规则错在哪、
    错多少。没有基线就没有「更准」这个结论，只有感觉。
  · 判断表的 `confidence` 是「有多少本地证据」，**不是准确率**（见 ai_decide.h）。
    唯一的校准办法就是拿真实日志回测。

本工具做三件事：
  1. **解析**——从日志里取 `[判断] <name> verdict=… conf=… why=… | sig …` 行，
     取出判决、置信度、理由，以及**输入信号**（信号是离线复算的前提）。
  2. **复算**——用 Python 重实现 `AiDecideVisionGate` / `AiDecideGameForeground`，
     断言与日志里的判决逐条一致。**不一致就是红**：要么 C++ 改了而这里没同步，
     要么日志是旧版本。这条闸保证「在 Python 里调阈值」这个动作是有意义的。
  3. **校准**——按 (判决, 置信度桶) 统计后续实际结果，直接读出：
     · 门控放行后的识图失败率（放行是不是放错了）
     · 拦下之后是否真的走了控件树/文字直达（拦得有没有用）
     · 哪条判据命中最频繁却最不可靠（该改的阈值就在那一行）

依赖：**只要标准库**（与 tools/verify 下其它脚本一致）。

⚠ 日志里的判决行包含输入信号，是**从 2026-09-20 的版本起**才有的。
  更早的日志只有判决没有信号，只能统计分布、无法复算 —— 工具会明确报出来，
  不会假装能算。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import Counter, defaultdict

# ── 判定表常量：必须与 src/ai_decide.cpp 顶部保持一致 ──────────────────────
# 改 C++ 那边的阈值就必须改这里，否则 --selftest 与复算闸会红。
K_BUSY_DENY = 0.10
K_BUSY_DENY_WHILE_CHANGING = 0.04
K_GAME_BUSY = 0.03
K_GAME_BUSY_WHILE_CHANGING = 0.02

CONF_HIGH = 0.70          # kAiDecideConfHigh
CONF_LOW = 0.40           # kAiDecideConfLow
GAME_CONF_DECISIVE = 0.80  # kAiGameConfDecisive

# ── 判定行解析 ────────────────────────────────────────────────────────────
# 形如：
# [判断] vision_gate verdict=deny_busy_fallback conf=0.55 why=… | sig page=- br=1 …
DECISION_RE = re.compile(
    r"\[判断\]\s+(?P<name>[a-z_]+)\s+verdict=(?P<verdict>[a-z_]+)\s+"
    r"conf=(?P<conf>[0-9.]+)\s+why=(?P<why>.*?)(?:\s*\|\s*(?P<sig>sig\s.*))?$"
)
SIG_RE = re.compile(r"(?P<key>[a-z_]+)=(?P<val>[^\s]+)")

# ── 真值（后续事件）：从日志里的诊断行取 ──────────────────────────────────
# 这些是引擎自己写的事实，不是我们的猜测。
#
# ⚠⚠ **这里每一条都必须是现在真的会打出来的行**（批 E 清理）。
#   原先还有三条 —— 「布局记忆命中」「布局记忆记一笔未生效」「定位缓存作废：点击后画面无变化」
#   —— 它们属于**批 B 已整体撤销**的布局记忆 / 定位模板缓存（`src/ai_ui_layout.*` /
#   `src/ai_locate_cache.*` 连文件一起没了），所以那三条**恒假**：日志里永远匹配不到，
#   归因表里永远 0 样本，而报告照样把它当「一条真值信号」印出来。
#   那不是「少了个信号」，那是**验证工具自己在说谎** —— 它给出的 0 会被人读成
#   「没有发生」，而不是「这条判据已经不存在了」。
#   ⇒ 规则：**删能力的时候回来 grep 这里**（`grep -F` 每条 pattern 的原文），
#     恒假的信号必须删掉或换成**现存**的等价信号，不许留着凑数。
TRUTH_PATTERNS = {
    # 文字直达命中：本地 OCR 索引直接给出坐标 → 证明确实不需要识图
    "ocr_direct_hit": re.compile(r"文字直点：本地 OCR 索引命中"),
    # DOM / UIA 精确点击：控件树路线真的可用
    "dom_click": re.compile(r"DOM 优先：树上命中"),
    "uia_click": re.compile(r"UIA 优先：命中「"),
    # 遮挡校验拦住：定位点不属于前台窗口 → 那一次识图定位是错的
    "occlusion_blocked": re.compile(r"遮挡校验失败"),
    # 识图定位失败
    "locate_fail": re.compile(r"locateAndClick.*失败|\[错误\]\s*定位"),
}


def parse_log(path: str) -> dict:
    """把一份日志解析成 {decisions, truths, stats}。"""
    decisions = []
    truths = defaultdict(list)
    line_no = 0
    total_lines = 0
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line_no += 1
            total_lines += 1
            line = raw.rstrip("\n").rstrip("\r")
            m = DECISION_RE.search(line)
            if m:
                sig = {}
                if m.group("sig"):
                    for sm in SIG_RE.finditer(m.group("sig")):
                        sig[sm.group("key")] = sm.group("val")
                decisions.append({
                    "file": os.path.basename(path),
                    "line": line_no,
                    "name": m.group("name"),
                    "verdict": m.group("verdict"),
                    "conf": float(m.group("conf")),
                    "why": m.group("why").strip(),
                    "sig": sig,
                    "has_sig": bool(sig),
                })
                continue
            for key, pat in TRUTH_PATTERNS.items():
                if pat.search(line):
                    truths[key].append(line_no)
                    break
    return {
        "file": path,
        "total_lines": total_lines,
        "decisions": decisions,
        "truths": truths,
    }


def attach_truth(parsed: dict) -> None:
    """给每条判断挂上「从它到下一条判断之间」发生的事实。

    两条例行规则：

    ① **用「相邻判断」划窗，不用固定行数**。初版用 40 行窗口，会把**后面几条
       判断**产生的后续事件算到这一条头上 —— 实测算出「8 次放行 / 7 次遮挡校验
       失败」这种不可能的账。判断行本身就是天然的区段边界。

    ② **同一帧上的多条判断共享该帧之后的证据**（`sig` 完全相同）。
       引擎一次动作会先把视觉闸、游戏前台等几条判断一起打完再动手，所以只有
       紧跟动手那次判断本来能拿到后续事件；若只认这一条，前面几条判断永远
       没有样本，校准表会系统性失真。同一份输入信号的判断本就同源，共享证据
       是对齐的（`attribution = "shared_signals"` 标明来源，便于核对）。
    """
    truths = sorted((ln, k) for k, lines in parsed["truths"].items() for ln in lines)
    decs = sorted(parsed["decisions"], key=lambda d: d["line"])

    for i, d in enumerate(decs):
        lo = d["line"]
        hi = decs[i + 1]["line"] if i + 1 < len(decs) else float("inf")
        d["after"] = [k for ln, k in truths if lo < ln < hi]
        d["attribution"] = "direct" if d["after"] else "none"

    # ② 按「信号段」归因。
    #
    #    为什么需要：引擎一次动作会先把视觉闸、游戏前台等几条判断**一起打完**再动手，
    #    所以只有紧跟动手的那条本来能拿到后续事件；只认这一条的话，前面几条判断
    #    永远没有样本，校准表系统性失真。
    #
    #    正确口径不是「连续的相同信号」而是**段**：一段 = 同一份信号从出现到它被
    #    另一份信号取代为止，段内所有判断共享「从段末到下一条**信号不同**的判断
    #    之间」的事件。
    #    ⚠ 两种错误归因都踩过，别再走回去：
    #      · 按固定行数开窗 → 把后面几条判断的事件算到这一条头上（8 次放行算出
    #        3.125 的比率，不可能）；
    #      · 按「连续相同信号」成组、组末就截窗 → 组后常紧跟同一帧的别的判断，
    #        真正的动作在其后，窗口被截空（拦下组永远 0 样本）。
    def sig_key(d):
        return tuple(sorted(d["sig"].items())) if d["has_sig"] else None

    n = len(decs)
    bounds = []          # (start_idx, end_idx_exclusive)
    s = 0
    for i in range(1, n + 1):
        if i == n or sig_key(decs[i]) != sig_key(decs[s]):
            bounds.append((s, i))
            s = i

    def evidence_between(lo_line, hi_line, claimed):
        return [k for ln, k in truths
                if lo_line < ln < hi_line and (ln, k) not in claimed]

    for si, (a, b) in enumerate(bounds):
        lo = decs[b - 1]["line"]
        hi = decs[bounds[si + 1][0]]["line"] if si + 1 < len(bounds) else float("inf")
        claimed = set()
        for m in range(b, bounds[si + 1][0] if si + 1 < len(bounds) else n):
            claimed.update(decs[m]["after"])
        shared = evidence_between(lo, hi, claimed)
        for m in range(a, b):
            if shared:
                decs[m]["after"] = list(shared)
                decs[m]["attribution"] = "shared_signals"
            elif not decs[m]["after"]:
                decs[m]["attribution"] = "none"

    # ③ 末段没有后续事件时，允许借用紧随其后的段已经拿到的证据
    #    （同一帧的最后一条判断往往就是动手那条，它的事件落在下一段边界之后）。
    for m in range(n):
        if decs[m]["after"]:
            continue
        for k in range(m + 1, n):
            if decs[k]["after"]:
                decs[m]["after"] = list(decs[k]["after"])
                decs[m]["attribution"] = "next_segment"
                break
            if sig_key(decs[k]) != sig_key(decs[m]):
                # 已经跨过了一个信号不同的判断还没找到证据 → 不借，避免远程错配
                break


# ── 复算：Python 侧的判定表（必须与 src/ai_decide.cpp 等价）────────────────
def _truthy(v: str) -> bool:
    return v == "1"


def _f(v: str, default: float = -1.0) -> float:
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def replay_vision_gate(sig: dict) -> tuple[str, float, str]:
    """复算 AiDecideVisionGate。返回 (verdict, confidence, why)。"""
    page = sig.get("page", "-")
    if page == "-":
        page = ""
    br = _truthy(sig.get("br", "0"))
    self_drawn = _truthy(sig.get("self", "0"))
    web = _truthy(sig.get("web", "0"))
    hooks = _truthy(sig.get("hooks", "0"))
    busy = _f(sig.get("busy", "-1.000"))
    changing = _truthy(sig.get("changing", "0"))
    del self_drawn  # 行④只要求「不是浏览器前台」；自绘标记不参与（与 C++ 一致）

    kind_dom = page in ("dom", "mixed")
    kind_canvas = page == "canvas"
    page_busy = busy >= K_BUSY_DENY or (changing and busy >= K_BUSY_DENY_WHILE_CHANGING)
    vision_only_way = kind_canvas or not br

    # 行顺序即优先级，与 C++ 的 rows[] 一一对应
    if kind_dom:
        verdict, conf, why = "allow", 0.90, "页型是 dom/mixed"
    elif kind_canvas:
        verdict, conf, why = "allow", 0.90, "画布页"
    elif web:
        verdict, conf, why = "allow", 0.75, "本次已是网页会话"
    elif not br:
        verdict, conf, why = "allow", 0.90, "前台不是浏览器窗口"
    elif br and page_busy:
        verdict, conf, why = "deny_busy_fallback", 0.55, "浏览器前台页型未知且画面高动态"
    else:
        verdict, conf, why = "allow", 0.68, "浏览器前台页型未知但画面安静"

    if verdict == "deny_busy_fallback" and not hooks:
        conf = min(conf, 0.50)
    del vision_only_way
    return verdict, conf, why


def replay_game_foreground(sig: dict) -> tuple[str, float, str]:
    """复算 AiDecideGameForeground。返回 (verdict, confidence, why)。"""
    page = sig.get("page", "-")
    if page == "-":
        page = ""
    br = _truthy(sig.get("br", "0"))
    self_drawn = _truthy(sig.get("self", "0"))
    sheet = _truthy(sig.get("sheet", "0"))
    busy = _f(sig.get("busy", "-1.000"))
    changing = _truthy(sig.get("changing", "0"))

    kind_canvas = page == "canvas"
    busy_motion = busy >= K_GAME_BUSY or (changing and busy >= K_GAME_BUSY_WHILE_CHANGING)

    if kind_canvas:
        return "game_canvas_page", 0.90, "画布页"
    if br:
        return "not_game", 0.85, "前台是浏览器"
    if sheet:
        return "not_game", 0.85, "前台是表格/办公软件"
    if self_drawn:
        return "game_no_tree", 0.88, "前台自绘（子窗口≈0）"
    if busy_motion:
        return "game_by_motion", 0.55, "画面持续重绘"
    return "not_game", 0.50, "无本地证据"


REPLAY = {
    "vision_gate": replay_vision_gate,
    "game_foreground": replay_game_foreground,
}

# 判决名字需要归一：C++ 写的是 allow / deny_busy_fallback /
# game_no_tree / not_game / game_by_motion / game_canvas_page
CANON = {
    "game_no_tree": "game_no_tree",
    "game_canvas_page": "game_canvas_page",
    "game_by_motion": "game_by_motion",
    "not_game": "not_game",
}


def conf_tier(conf: float) -> str:
    if conf >= CONF_HIGH:
        return "high"
    if conf >= CONF_LOW:
        return "medium"
    return "low"


# ── 报告 ──────────────────────────────────────────────────────────────────
def check_replay(decisions: list) -> tuple[int, int, list]:
    """复算闸：Python 判定表 vs 日志里的判决。返回 (对比数, 不一致数, 明细)。"""
    compared = 0
    mismatched = 0
    details = []
    for d in decisions:
        fn = REPLAY.get(d["name"])
        if fn is None or not d["has_sig"]:
            continue
        compared += 1
        if d["name"] == "vision_gate":
            v, conf, _ = fn(d["sig"])
            ok = (v == d["verdict"])
            if ok and not (abs(conf - d["conf"]) < 0.005):
                ok = False
        else:
            v, conf, _ = fn(d["sig"])
            want = CANON.get(d["verdict"], d["verdict"])
            ok = (v == want) and (abs(conf - d["conf"]) < 0.005)
        if not ok:
            mismatched += 1
            details.append({
                "line": d["line"],
                "name": d["name"],
                "log_verdict": d["verdict"],
                "log_conf": d["conf"],
                "replay": fn(d["sig"]),
                "sig": d["sig"],
            })
    return compared, mismatched, details


def build_report(parsed_list: list) -> dict:
    all_decisions = []
    for p in parsed_list:
        attach_truth(p)
        all_decisions.extend(p["decisions"])

    report = {
        "files": [p["file"] for p in parsed_list],
        "total_lines": sum(p["total_lines"] for p in parsed_list),
        "decisions": len(all_decisions),
        "with_signal": sum(1 for d in all_decisions if d["has_sig"]),
        "without_signal": sum(1 for d in all_decisions if not d["has_sig"]),
    }

    compared, mismatched, details = check_replay(all_decisions)
    report["replay_compared"] = compared
    report["replay_mismatched"] = mismatched
    report["replay_details"] = details[:20]

    # 按 (判断名, 判决, 置信度桶) 分桶
    buckets = defaultdict(lambda: {"n": 0, "after": Counter()})
    for d in all_decisions:
        key = (d["name"], d["verdict"], conf_tier(d["conf"]))
        b = buckets[key]
        b["n"] += 1
        for t in d.get("after", []):
            b["after"][t] += 1
    report["buckets"] = [
        {
            "decision": k[0], "verdict": k[1], "tier": k[2],
            "n": v["n"],
            "after": dict(v["after"].most_common()),
        }
        for k, v in sorted(buckets.items(), key=lambda kv: (-kv[1]["n"], kv[0]))
    ]

    # 按 why 前缀聚合：哪条判据命中最频繁
    whys = Counter()
    for d in all_decisions:
        whys[(d["name"], d["verdict"], d["why"][:24])] += 1
    report["top_reasons"] = [
        {"decision": k[0], "verdict": k[1], "why": k[2], "n": n}
        for k, n in whys.most_common(12)
    ]

    # ── 关键校准：门控**放行**之后的识图结果 ────────────────────────────
    # 放行 = 我们说要识图。若后续频繁出现「遮挡校验失败 / 定位失败」，
    # 说明这次放行是错的（画面上根本没有可点的东西 / 点错了地方）。
    #
    # ⚠ 计数口径是**按判断条数**（`n_bad` = 有这类后续事件的判断条数），
    #   不是事件条数 —— 混用会把比率算成 >1（初版就踩了：8 次放行算出 3.125）。
    # ⚠ 批 E 前这里还有两个「布局记忆作废 / 无变化」信号，随批 B 撤销的能力一起删了
    #   （它们恒假，见 TRUTH_PATTERNS 上方说明）。
    BAD_AFTER_ALLOW = ("occlusion_blocked", "locate_fail")
    n_allow = 0
    n_allow_bad = 0
    bad_after_allow = Counter()
    for d in all_decisions:
        if d["name"] != "vision_gate" or d["verdict"] != "allow":
            continue
        n_allow += 1
        hits = [t for t in d.get("after", []) if t in BAD_AFTER_ALLOW]
        if hits:
            n_allow_bad += 1
        for t in hits:
            bad_after_allow[t] += 1
    report["allow_total"] = n_allow
    report["allow_bad_decisions"] = n_allow_bad
    report["allow_bad_followups"] = dict(bad_after_allow)

    # ── 关键校准：门控**拦下**之后有没有真的走别的路 ────────────────────
    # 拦下 = 我们说「别识图，去点树」。若后续出现了 DOM/UIA 命中或文字直达，
    # 说明这次拦截是有价值的；若什么都没有，模型可能被拦在原地（那是负收益）。
    GOOD_AFTER_DENY = ("dom_click", "uia_click", "ocr_direct_hit")
    n_deny = 0
    n_deny_good = 0
    good_after_deny = Counter()
    for d in all_decisions:
        if d["name"] != "vision_gate" or d["verdict"] != "deny_busy_fallback":
            continue
        n_deny += 1
        hits = [t for t in d.get("after", []) if t in GOOD_AFTER_DENY]
        if hits:
            n_deny_good += 1
        for t in hits:
            good_after_deny[t] += 1
    report["deny_total"] = n_deny
    report["deny_good_decisions"] = n_deny_good
    report["deny_good_followups"] = dict(good_after_deny)

    # 证据覆盖率：没有后续事件的判断**不进**任何比率（否则分母注水）。
    # 这个数字直接决定结论可不可信，所以单独报出来。
    with_ev = sum(1 for d in all_decisions if d.get("after"))
    report["evidence_coverage"] = {
        "decisions": len(all_decisions),
        "with_followup_evidence": with_ev,
        "shared_signals": sum(1 for d in all_decisions
                              if d.get("attribution") == "shared_signals"),
        "ratio": round(with_ev / len(all_decisions), 3) if all_decisions else 0.0,
    }

    # ── 建议：哪些桶的证据支持收紧/放松 ────────────────────────────────
    advice = []
    if n_allow:
        rate = n_allow_bad / n_allow
        advice.append({
            "what": "vision_gate.allow",
            "metric": "放行后出现错点/无变化的比例",
            "value": round(rate, 3),
            "n": n_allow,
            "note": ("偏高 → 放行门槛偏松，考虑对「中置信」桶收紧"
                     if rate > 0.15 else "目前没有证据要求收紧"),
        })
    if n_deny:
        rate = n_deny_good / n_deny
        advice.append({
            "what": "vision_gate.deny_busy_fallback",
            "metric": "拦下后真的走了控件树/文字直达的比例",
            "value": round(rate, 3),
            "n": n_deny,
            "note": ("偏低 → 拦下后模型没找到别的路，考虑放宽（等于白拦）"
                     if rate < 0.3 else "拦下确实换来了别的路线"),
        })
    # 样本量护栏：别拿 3 条判断去调阈值
    for a in advice:
        if a["n"] < 20:
            a["note"] += f" ⚠ 样本仅 {a['n']} 条，不足以据此改阈值"
    report["advice"] = advice
    return report


def print_report(rep: dict) -> None:
    print("=" * 74)
    print("AI 决策表 离线影子测试 / 置信度校准")
    print("=" * 74)
    print(f"日志: {', '.join(rep['files'])}")
    print(f"总行数: {rep['total_lines']}   判断行: {rep['decisions']}"
          f"（带信号 {rep['with_signal']} / 无信号 {rep['without_signal']}）")
    if rep["without_signal"]:
        print("  ⚠ 有判断行没有输入信号 —— 那是旧版本日志，**无法复算**。"
              "复算闸只覆盖带信号的行。")
    print()

    if rep["replay_compared"]:
        mark = "OK" if rep["replay_mismatched"] == 0 else "**不一致**"
        print(f"[复算闸] {mark}：比对 {rep['replay_compared']} 条，"
              f"不一致 {rep['replay_mismatched']} 条")
        for d in rep["replay_details"]:
            print(f"    L{d['line']} {d['name']}: 日志={d['log_verdict']}"
                  f"/{d['log_conf']}  复算={d['replay'][0]}/{d['replay'][1]:.2f}"
                  f"  sig={d['sig']}")
    else:
        print("[复算闸] 没有可复算的行（日志里没有输入信号）")
    print()

    ev = rep.get("evidence_coverage", {})
    if ev:
        print(f"[证据覆盖] {ev['with_followup_evidence']}/{ev['decisions']} 条判断"
              f"拿到了后续事件（{ev['ratio']:.0%}），其中 {ev['shared_signals']} 条"
              f"靠「同帧信号相同」共享")
        if ev["ratio"] < 0.5:
            print("    ⚠ 覆盖不足一半：下面的比率分母很小，只能当定性参考")
    print()

    print(f"[门控放行] 共 {rep['allow_total']} 次；其中 {rep.get('allow_bad_decisions', 0)} 次"
          f"放行后出现「错点/无变化/定位失败」")
    for k, v in rep["allow_bad_followups"].items():
        print(f"    {k}: {v} 次判断")
    print()
    print(f"[门控拦下] 共 {rep['deny_total']} 次；其中 {rep.get('deny_good_decisions', 0)} 次"
          f"拦下后真的换了路线")
    for k, v in rep["deny_good_followups"].items():
        print(f"    {k}: {v} 次判断")
    print()

    print("[分桶：判决 × 置信度档]")
    for b in rep["buckets"]:
        after = ", ".join(f"{k}×{v}" for k, v in list(b["after"].items())[:4]) or "-"
        print(f"    {b['decision']:<16} {b['verdict']:<20} {b['tier']:<7}"
              f" n={b['n']:<5} 后续: {after}")
    print()

    print("[命中最多的判据]")
    for r in rep["top_reasons"]:
        print(f"    n={r['n']:<5} {r['decision']}/{r['verdict']}  {r['why']}")
    print()

    print("[阈值建议（基于本次日志，样本量小就别当真）]")
    for a in rep["advice"]:
        print(f"    {a['what']}: {a['metric']} = {a['value']} (n={a['n']}) → {a['note']}")
    print()


# ── 自检：解析 + 复算的固定用例（不依赖任何真实日志）──────────────────────
SELFTEST_LOG = """\
AI动作执行 [deepseek-flash]：动作 JSON 解析完成
  [诊断] 本地判断 4 条：
    [判断] vision_gate verdict=allow conf=0.90 why=前台不是浏览器窗口：没有网页树可退，视觉是唯一手段 | sig page=- br=0 self=1 sheet=0 web=0 hooks=0 busy=0.420 changing=1
  [诊断] 遮挡校验失败：定位点不属于前台窗口，已拦截点击
  [诊断] UIA 优先：命中「开始」[(100,200)-(160,230)]
AI动作执行 [deepseek-flash]：动作 JSON 解析完成
  [诊断] 本地判断 3 条：
    [判断] vision_gate verdict=deny_busy_fallback conf=0.50 why=浏览器前台页型未知且画面高动态：先 observePage 拿树 | sig page=- br=1 self=0 sheet=0 web=0 hooks=0 busy=0.420 changing=1
  [诊断] DOM 优先：树上命中「登录」[ref=e12]
AI动作执行：动作 JSON 解析完成
    [判断] vision_gate verdict=allow conf=0.90 why=页型是 dom/mixed：有控件树可点，视觉仍可用（优先树） | sig page=dom br=1 self=0 sheet=0 web=0 hooks=1 busy=0.500 changing=1
    [判断] game_foreground verdict=not_game conf=0.85 why=前台是浏览器：有 DOM 可退，不判游戏 | sig page=dom br=1 self=0 sheet=0 web=0 hooks=1 busy=0.500 changing=1
    [判断] game_foreground verdict=game_no_tree conf=0.88 why=前台自绘（子窗口≈0）：没有控件树，视觉是唯一手段 | sig page=- br=0 self=1 sheet=0 web=0 hooks=0 busy=0.010 changing=0
    [判断] game_foreground verdict=game_by_motion conf=0.55 why=画面持续重绘：疑似游戏/模拟器/视频前台（只有这条定量证据） | sig page=- br=0 self=0 sheet=0 web=0 hooks=0 busy=0.050 changing=0
    [判断] game_foreground verdict=not_game conf=0.50 why=无动/静态证据（画面覆盖率未达阈值且前台有控件树）：按非游戏处理 | sig page=- br=0 self=0 sheet=0 web=0 hooks=0 busy=0.010 changing=0
    [判断] game_foreground verdict=game_canvas_page conf=0.90 why=画布页：DOM 无可用节点，视觉是唯一手段 | sig page=canvas br=1 self=0 sheet=0 web=0 hooks=1 busy=0.900 changing=1
    [判断] vision_gate verdict=allow conf=0.68 why=浏览器前台页型未知但画面安静：识图代价可控（无硬证据，属兜底放行） | sig page=- br=1 self=0 sheet=0 web=0 hooks=1 busy=0.010 changing=0
    [判断] vision_gate verdict=allow conf=0.75 why=本次已是网页会话：视觉兜底可用 | sig page=- br=1 self=0 sheet=0 web=1 hooks=1 busy=-1.000 changing=0
    [判断] vision_gate verdict=allow conf=0.90 why=画布页：DOM 没有可用节点，视觉是唯一手段 | sig page=canvas br=1 self=0 sheet=0 web=0 hooks=1 busy=0.900 changing=1
"""


def run_selftest() -> int:
    import tempfile
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    failures = []

    def check(name, cond, detail=""):
        if not cond:
            failures.append(f"{name} {detail}".strip())
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}{(' — ' + detail) if detail and not cond else ''}")

    print("ai_decide_shadow selftest:")

    # ① 解析
    with tempfile.TemporaryDirectory() as td:
        path = os.path.join(td, "fake.log")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(SELFTEST_LOG)
        parsed = parse_log(path)

    decs = parsed["decisions"]
    check("parse_count", len(decs) == 11, f"got {len(decs)}")
    check("parse_all_have_signal", all(d["has_sig"] for d in decs))
    check("parse_verdict", decs[0]["verdict"] == "allow", decs[0]["verdict"])
    check("parse_conf", abs(decs[1]["conf"] - 0.50) < 1e-9, str(decs[1]["conf"]))
    check("parse_sig_page", decs[3]["sig"].get("page") == "dom", str(decs[3]["sig"]))
    check("parse_sig_dash_page", decs[0]["sig"].get("page") == "-", str(decs[0]["sig"]))

    # ② 真值提取
    #    ⚠ 批 E：这里原先还查 `layout_invalidate` / `layout_hit`，而那两个信号随批 B
    #    撤销的能力一起恒假了（断言照样"过"是假绿的一种：它证明的只是「字符串出现在我
    #    自己写的日志里」）。现在改成查**现存**信号。
    check("truth_occlusion_blocked", "occlusion_blocked" in parsed["truths"],
          str(dict(parsed["truths"])))
    check("truth_dom_click", "dom_click" in parsed["truths"])
    check("truth_uia_click", "uia_click" in parsed["truths"])

    # ③ 复算闸：合成日志里的每一行都必须复算得出来（与 C++ 一致）
    compared, mismatched, details = check_replay(decs)
    check("replay_all_compared", compared == len(decs), f"{compared}/{len(decs)}")
    check("replay_no_mismatch", mismatched == 0, json.dumps(details, ensure_ascii=False))

    # ④ 逐条钉死复算规则（这些等价于 C++ 的 decide_table 断言）
    cases = [
        ("dom", {"page": "dom", "br": "1", "busy": "0.500", "changing": "1"},
         ("allow", 0.90)),
        ("canvas_busy", {"page": "canvas", "br": "1", "busy": "0.900", "changing": "1"},
         ("allow", 0.90)),
        ("web_session", {"page": "-", "br": "1", "web": "1"}, ("allow", 0.75)),
        ("desktop", {"page": "-", "br": "0", "self": "1", "busy": "0.900",
                     "changing": "1"}, ("allow", 0.90)),
        # 浏览器 + 高动态 + 无钩子 → 拦且置信度被压到 0.50
        ("browser_busy_nohooks", {"page": "-", "br": "1", "busy": "0.420",
                                  "changing": "1", "hooks": "0"},
         ("deny_busy_fallback", 0.50)),
        # 有钩子则保持 0.55
        ("browser_busy_hooks", {"page": "-", "br": "1", "busy": "0.420",
                                "changing": "1", "hooks": "1"},
         ("deny_busy_fallback", 0.55)),
        # 阈值边界：恰好 0.10 算高动态（带钩子 → 不被压低）
        ("browser_at_0.10", {"page": "-", "br": "1", "busy": "0.100", "hooks": "1"},
         ("deny_busy_fallback", 0.55)),
        # 0.04 但已稳定 → 放行（两段式阈值）
        ("browser_0.04_stable", {"page": "-", "br": "1", "busy": "0.040",
                                 "changing": "0", "hooks": "1"}, ("allow", 0.68)),
        ("browser_0.04_changing", {"page": "-", "br": "1", "busy": "0.040",
                                   "changing": "1", "hooks": "1"},
         ("deny_busy_fallback", 0.55)),
        # 无钩子时拦下的把握要压低（「先拿树」这条退路可能不存在）
        ("browser_busy_nohooks_clamped", {"page": "-", "br": "1", "busy": "0.100"},
         ("deny_busy_fallback", 0.50)),
        # 兜底行必须低于 High 边界（否则被判成「有硬证据」而闭嘴）
        ("fallback_below_high", {"page": "-", "br": "1", "busy": "0.010"}, ("allow", 0.68)),
    ]
    for name, sig, (wv, wc) in cases:
        v, c, _ = replay_vision_gate(sig)
        check(f"gate_{name}", v == wv and abs(c - wc) < 1e-9, f"got {v}/{c} want {wv}/{wc}")

    game_cases = [
        ("canvas", {"page": "canvas"}, ("game_canvas_page", 0.90)),
        ("browser", {"page": "-", "br": "1", "busy": "0.500"}, ("not_game", 0.85)),
        ("sheet", {"page": "-", "sheet": "1", "busy": "0.500"}, ("not_game", 0.85)),
        ("self_drawn", {"page": "-", "self": "1", "busy": "0.900"}, ("game_no_tree", 0.88)),
        # ★主判据优先：自绘 + 静止画面也必须算游戏
        ("self_drawn_still", {"page": "-", "self": "1", "busy": "0.010"},
         ("game_no_tree", 0.88)),
        ("motion", {"page": "-", "busy": "0.050"}, ("game_by_motion", 0.55)),
        ("at_0.03", {"page": "-", "busy": "0.030"}, ("game_by_motion", 0.55)),
        ("quiet", {"page": "-", "busy": "0.010"}, ("not_game", 0.50)),
    ]
    for name, sig, (wv, wc) in game_cases:
        v, c, _ = replay_game_foreground(sig)
        check(f"game_{name}", v == wv and abs(c - wc) < 1e-9, f"got {v}/{c} want {wv}/{wc}")

    # ⑤ settle 短节拍门槛：只有结构性证据 + 够把握才配跳
    def decisive(v, c):
        if v not in ("game_no_tree", "game_canvas_page"):
            return False
        return c >= GAME_CONF_DECISIVE

    check("decisive_no_tree", decisive("game_no_tree", 0.88))
    check("decisive_canvas", decisive("game_canvas_page", 0.90))
    check("decisive_motion_not", not decisive("game_by_motion", 0.55))
    check("decisive_motion_high_not", not decisive("game_by_motion", 0.90))
    check("decisive_gate_range", 0.55 < GAME_CONF_DECISIVE <= 0.88,
          str(GAME_CONF_DECISIVE))

    # ⑥ 置信度分档边界
    check("tier_low", conf_tier(0.39) == "low")
    check("tier_medium", conf_tier(0.40) == "medium" and conf_tier(0.69) == "medium")
    check("tier_high", conf_tier(0.70) == "high")

    # ③ 归因口径：这两种错法都踩过，必须钉死（改 attach_truth 时先看这几条）
    attach_truth(parsed)
    decs = sorted(parsed["decisions"], key=lambda d: d["line"])

    # 每次判断的后续事件数不得超过该日志里真值事件总数 —— 固定行数窗口那版
    # 会把后面判断的事件重复计入，比率直接算出 >1。
    total_truths = sum(len(v) for v in parsed["truths"].values())
    max_after = max(len(d["after"]) for d in decs)
    check("attribution_no_duplication", max_after <= total_truths,
          f"max_after={max_after} total_truths={total_truths}")

    # 「浏览器高动态拦下」那组必须拿到 DOM/UIA/文字直达的证据：
    # 段末截窗那版会让它永远 0 样本（而它后面明明有 DOM 命中）。
    deny_ds = [d for d in decs if d["verdict"] == "deny_busy_fallback"]
    check("attribution_deny_group_has_evidence",
          bool(deny_ds) and any(d["after"] for d in deny_ds),
          str([d["after"] for d in deny_ds]))
    deny_kinds = {k for d in deny_ds for k in d["after"]}
    check("attribution_deny_gets_dom_click", "dom_click" in deny_kinds, str(deny_kinds))

    # 覆盖率必须报出来（决定结论可不可信的那个数字）
    rep0 = build_report([parsed])
    check("evidence_coverage_present", "evidence_coverage" in rep0)
    check("evidence_coverage_ratio_sane",
          0.0 <= rep0["evidence_coverage"]["ratio"] <= 1.0,
          str(rep0["evidence_coverage"]))

    # 比率必须按「判断条数」算，不能按事件条数（否则 >1）
    check("allow_rate_not_over_one",
          rep0["allow_bad_decisions"] <= rep0["allow_total"],
          f"{rep0['allow_bad_decisions']}/{rep0['allow_total']}")
    check("deny_rate_not_over_one",
          rep0["deny_good_decisions"] <= rep0["deny_total"],
          f"{rep0['deny_good_decisions']}/{rep0['deny_total']}")

    print()
    if failures:
        print(f"SELFTEST FAILED ({len(failures)}):")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("SELFTEST OK")
    return 0


def probe_jev(endpoint: str, decisions: list, timeout: float) -> dict:
    """可选：把「判断 + 输入信号」丢给一个本地 /v1/systemone 兼容服务，
    看它会不会给出同样的判决（以及它的 confidence 长什么样）。

    ⚠ 默认**不跑**；只有显式 --jevs 才走。官方 Jev 是纯文本、不能看图，
      且不支持微调、CJK 需自测 —— 所以在拿到影子测试数据之前，
      它只是「候选」，不是「接进来的后端」。
    """
    import urllib.error
    import urllib.request

    url = endpoint.rstrip("/") + "/v1/systemone"
    out = {"endpoint": url, "asked": 0, "answered": 0, "errors": []}
    for d in decisions:
        if not d["has_sig"]:
            continue
        question = {
            "type": "choice",
            "instructions": (
                "Decide whether the automation engine should be allowed to do "
                "a vision-based locate-and-click on the current foreground "
                "window, or whether it must be denied and fall back to the "
                "accessibility tree / keyboard."
            ),
            "criteria": {
                "allow": "vision locate is usable or is the only option",
                "deny_busy_fallback": "deny vision; use the control tree instead",
            },
        }
        body = {
            "model": "jev-latest",
            "state": {"signals": d["sig"], "reason_from_rules": d["why"]},
            "questions": {"gate": question},
        }
        req = urllib.request.Request(
            url, data=json.dumps(body).encode("utf-8"),
            headers={"Content-Type": "application/json",
                     "Authorization": "Bearer local-dev"})
        out["asked"] += 1
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                payload = json.loads(resp.read().decode("utf-8"))
            ans = payload.get("answers", {}).get("gate", {})
            out["answered"] += 1
            agree = ans.get("choice") == d["verdict"]
            out.setdefault("rows", []).append({
                "line": d["line"], "rule_verdict": d["verdict"],
                "model_choice": ans.get("choice"),
                "model_confidence": ans.get("confidence"),
                "agree": agree,
            })
        except (urllib.error.URLError, OSError, ValueError) as exc:
            out["errors"].append(f"L{d['line']}: {exc}")
            if len(out["errors"]) >= 3:
                out["errors"].append("（连续失败，提前停止 —— 服务没起？）")
                break
    if out.get("rows"):
        rows = out["rows"]
        out["agreement"] = sum(1 for r in rows if r["agree"]) / len(rows)
    return out


def main(argv=None) -> int:
    # 中文 + ⚠ 在 GBK 控制台会直接 UnicodeEncodeError 把工具打崩（实测踩过）。
    # 报告里有中文，必须显式把输出流切成 UTF-8，别依赖宿主代码页。
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass

    ap = argparse.ArgumentParser(
        description="AI 决策表离线影子测试 / 置信度校准（docs §23）")
    ap.add_argument("--log", nargs="*", default=[],
                    help="一份或多份日志（player.log / webview_boot.log）")
    ap.add_argument("--json", dest="json_out", default="",
                    help="把报告写成 JSON")
    ap.add_argument("--selftest", action="store_true",
                    help="跑内置固定用例（解析 + 复算），不读真实日志")
    ap.add_argument("--jevs", action="store_true",
                    help="额外把判断丢给本地 /v1/systemone 兼容服务对比（默认关）")
    ap.add_argument("--jev-endpoint", default="http://127.0.0.1:8080",
                    help="本地兼容服务地址（默认 8080）")
    ap.add_argument("--jev-timeout", type=float, default=10.0)
    args = ap.parse_args(argv)

    if args.selftest:
        return run_selftest()

    if not args.log:
        ap.print_help()
        print("\n提示：先跑 --selftest 验证解析/复算逻辑，再喂真实日志。")
        return 2

    missing = [p for p in args.log if not os.path.isfile(p)]
    if missing:
        print(f"找不到日志：{', '.join(missing)}", file=sys.stderr)
        return 2

    parsed_list = [parse_log(p) for p in args.log]
    rep = build_report(parsed_list)
    print_report(rep)

    if args.jevs:
        all_decisions = [d for p in parsed_list for d in p["decisions"]]
        print("=" * 74)
        print("本地 Jev 兼容后端对比（--jevs）")
        print("=" * 74)
        probe = probe_jev(args.jev_endpoint, all_decisions, args.jev_timeout)
        rep["jev_probe"] = probe
        print(f"端点: {probe['endpoint']}   提问 {probe['asked']}  应答 {probe['answered']}")
        for e in probe.get("errors", [])[:5]:
            print(f"    错误: {e}")
        if "agreement" in probe:
            print(f"与规则判决一致率: {probe['agreement']:.3f}")
            for r in probe.get("rows", [])[:20]:
                print(f"    L{r['line']}: 规则={r['rule_verdict']:<20}"
                      f" 模型={r['model_choice']} conf={r['model_confidence']}")
        print()
        print("⚠ 一致率高**不等于**更准 —— 两者都可能在同一个样本上错。")
        print("  要比准确率，得先有真值标签（本工具用门控后的实际结果做代理指标）。")

    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as fh:
            json.dump(rep, fh, ensure_ascii=False, indent=2)
        print(f"报告已写入 {args.json_out}")

    # 复算不一致 = 红（C++ 改了而 Python 没同步，或日志版本不对）
    if rep["replay_mismatched"]:
        print(f"**复算闸失败**：{rep['replay_mismatched']} 条与 C++ 判定表不一致。"
              "请同步 tools/verify/ai_decide_shadow.py 的 replay_* 常量/规则。",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
