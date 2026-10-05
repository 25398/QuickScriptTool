"""从「回放逐步日志」重算「键态停摆」那一刻的**脚本持键集**，验证清理逻辑误伤了哪些键。

为什么需要它：
    `ClearStaleArrowSoftKeys()` 的破坏性只有在「它清掉的键正好是脚本正按着的」时才成立。
    光看日志里 `已清方向键陈旧位（1 个）` 看不出清的是哪个键 —— 必须**重放按键流**，
    把那一刻的持键影子集（= 已按下未松开）重建出来，才能证明「清掉的是脚本意图」。

用法:
    python tools/verify/probe_maple_stall_clears_held.py "C:/Users/.../后台宏 (2).txt"
    python tools/verify/probe_maple_stall_clears_held.py <log> <log2> ...   # 可批量

判据（修复前的坏状态）：
    VERDICT=ROOT_CAUSE_CONFIRMED  ⇔ 存在「被清的方向键 ∈ 脚本持键集」
    VERDICT=NO_INTENT_LOSS        ⇔ 每个停摆点清掉的都是无人认领的陈旧位（正确行为）
"""
import os
import re
import sys

# 日志里的键名 → 语义类别（只关心方向键与"是否方向键"）
ARROW_NAMES = {"→": "VK_RIGHT", "←": "VK_LEFT", "↑": "VK_UP", "↓": "VK_DOWN"}

RE_KEY = re.compile(r"^\[(\d+)\]键盘(按下|松开)(.+?)\s*$")
RE_STALL = re.compile(r"键态停摆.*?已清方向键陈旧位（(\d+) 个）.*?持键 (\d+) 个")
RE_RESUME = re.compile(r"轮询恢复.*?补发按下 (\d+) 个，共持键 (\d+) 个")


def parse(path):
    """返回 (events, stalls)。events = [(idx, down, key)]；stalls = [(lineNo, clearedN, heldN, heldSet)]"""
    events, stalls = [], []
    held = []            # 有序的持键列表（可含重复，用集合语义去重）
    held_set = set()
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.rstrip("\n")
            m = RE_KEY.match(line)
            if m:
                idx, act, key = int(m.group(1)), m.group(2), m.group(3).strip()
                if act == "按下":
                    if key not in held_set:
                        held.append(key)
                        held_set.add(key)
                else:
                    if key in held_set:
                        held.remove(key)
                        held_set.discard(key)
                events.append((idx, act == "按下", key))
                continue
            m = RE_STALL.search(line)
            if m:
                # 记**位置**（不是动作号）：下面要按它取"停摆点之前的那一条按键"
                stalls.append((lineno, int(m.group(1)), int(m.group(2)),
                               sorted(held_set), len(events) - 1))
                continue
            m = RE_RESUME.search(line)
            if m:
                stalls.append((lineno, -1, int(m.group(2)),
                               sorted(held_set), len(events) - 1))
    return events, stalls


def report(path):
    events, stalls = parse(path)
    print("=" * 78)
    print("FILE=%s" % os.path.basename(path))
    print("  逐步事件=%d 条；停摆/恢复点=%d 个" % (len(events), len(stalls)))
    confirmed = False
    for lineno, cleared, heldN, held, pos in stalls:
        arrows_held = [k for k in held if k in ARROW_NAMES]
        # ⚠ 口径差：**修复前**的产品把看门狗日志打在「把当次按键写进影子集」**之前**，
        #   所以日志里的 `持键 N 个` 不含当次那个键。探针按文件顺序重放，已经含了 ⇒
        #   必须把当次键减掉/加回去，才是产品当时的真实视图。
        in_flight, in_flight_down = (events[pos][2], events[pos][1]) if pos >= 0 else (None, None)
        action_no = events[pos][0] if pos >= 0 else -1
        product_view = list(held)
        if in_flight is not None:
            if in_flight_down and in_flight in product_view:
                product_view.remove(in_flight)          # 按下：产品还没插入
            elif (not in_flight_down) and in_flight not in product_view:
                product_view.append(in_flight)          # 松开：产品还没删除
        arrows_product = [k for k in product_view if k in ARROW_NAMES]
        if cleared >= 0:
            print("  L%-5d [停摆] 停在动作 [%d] 之后：日志称 持键=%d、清掉=%d"
                  % (lineno, action_no, heldN, cleared))
            print("          重放持键集 = %s" % sorted(held))
            print("          产品当时视图（扣掉当次按键 '%s' %s）= %s"
                  % (in_flight, "按下" if in_flight_down else "松开", sorted(product_view)))
            match = (len(product_view) == heldN)
            print("          持键数自洽 = %s" % ("是 ✅" if match else "否 ← 模型与代码不符，要复核"))
            if arrows_product and cleared > 0:
                victims = arrows_product[:cleared]
                print("          ⚠⚠ 清掉的 %d 个方向键里有脚本正按着的: %s" % (cleared, victims))
                confirmed = True
            else:
                print("          ✅ 无脚本意图被误清")
        else:
            print("  L%-5d [恢复] 停在动作 [%d] 之后：日志称 持键=%d" % (lineno, action_no, heldN))
            print("          重放持键集 = %s" % sorted(held))
            print("          产品当时视图 = %s（自洽=%s）"
                  % (sorted(product_view), "是 ✅" if len(product_view) == heldN else "否"))
    print("  VERDICT=%s" % ("ROOT_CAUSE_CONFIRMED" if confirmed else "NO_INTENT_LOSS"))
    return confirmed


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    any_confirmed = False
    for p in sys.argv[1:]:
        if not os.path.exists(p):
            print("MISSING %s" % p)
            continue
        any_confirmed = report(p) or any_confirmed
    print("=" * 78)
    print("OVERALL=%s" % ("ROOT_CAUSE_CONFIRMED" if any_confirmed else "NO_INTENT_LOSS"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
