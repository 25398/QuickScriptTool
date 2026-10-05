#!/usr/bin/env python3
"""导出的 exe —— 行为一致性验证矩阵。

用法（在仓库根目录）：
    python tools/verify/player_behavior_matrix.py

它会现场构造几个「导出的 exe」（模板 + 尾部 payload），跑起来观察实际行为，
用来验证 **exe 的效果与在软件里跑同一个脚本一致**。

⚠ 为什么要有这个脚本：这些行为横跨「引擎 worker 循环 / 设置快照 / 播放器生命周期」，
  没有任何单元测试能覆盖 —— 而它们恰恰是最容易在改动中被静默破坏的地方。

当前覆盖：
  A. 有 stopMacro + 未勾回放次数   → 1 轮就停
  B. 有 stopMacro + 回放次数 = 5    → **1 轮就停**（stopMacro 先命中，两个约束共同生效）
  C. 无 stopMacro + 回放次数 = 3    → 3 轮后停
  D. 无 stopMacro + 未勾回放次数    → 无限循环（不会自己退出）

依赖：已构建的 build\\Release\\tools\\player\\QstPlayer.exe
注意：会真实启动播放器进程；D 用例会在超时后 taskkill。
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

MAGIC = b"QSTPKG01"
FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
MASK = 0xFFFFFFFFFFFFFFFF

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(REPO, "build", "Release")
TEMPLATE = os.path.join(BUILD, "tools", "player", "QstPlayer.exe")
WORK = tempfile.mkdtemp(prefix="qst_player_matrix_")
OUT = os.path.join(WORK, "case.exe")
MARK = os.path.join(WORK, "runs.txt")


def fnv1a64(data: bytes) -> int:
    h = FNV_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & MASK
    return h


def build_case(with_stop: bool, count_enabled: bool, count: int) -> int:
    """把「脚本 + 设置快照」追加到播放器模板尾部，返回 payload 哈希。"""
    actions = [
        {"type": "wait", "indent": 0, "duration": 0.3},
        {"type": "runProgram", "indent": 0, "targetPath": "cmd.exe",
         "inputText": "/c echo RAN>> " + MARK},
    ]
    if with_stop:
        actions.append({"type": "stopMacro", "indent": 0})

    script = {
        "scriptName": "约束矩阵",
        "recordTime": "2026-09-19 21:40:00",
        "inputTimingVersion": 2,
        "hotkeyVk": 0, "hotkeyModifiers": 0, "hotkeyText": "", "hotkeyHold": 0,
        "windowMode": {"enabled": 0},
        "actions": actions,
    }
    player = {"v": 1, "needOpenCv": 0, "needOcr": 0, "needFakeFocus": 0,
              "bundledOpenCv": 1, "bundledOcr": 1, "bundledFakeFocus": 1, "createdBy": "verify"}
    manifest = {"v": 1, "root": "script.json", "scripts": [], "images": [], "player": player}
    settings = json.dumps({
        "playback": {
            "enablePlaybackCount": bool(count_enabled),
            "playbackCount": int(count),
            "enablePlaybackInterval": False,
        },
        # 关掉提示音，避免 CI/无人值守时吵
        "other": {"playSoundOnStart": False, "playSoundOnEnd": False},
    }, ensure_ascii=False)

    zp = os.path.join(WORK, "p.zip")
    with zipfile.ZipFile(zp, "w", compression=zipfile.ZIP_STORED) as z:
        z.writestr("script.json", json.dumps(script, ensure_ascii=False).encode("utf-8"))
        z.writestr("package.json", json.dumps(manifest, ensure_ascii=False).encode("utf-8"))
        z.writestr("rt\\app_settings.json", settings.encode("utf-8"))

    payload = open(zp, "rb").read()
    shutil.copyfile(TEMPLATE, OUT)
    base = os.path.getsize(OUT)
    with open(OUT, "ab") as f:
        f.write(payload)
        f.write(MAGIC)
        f.write(struct.pack("<Q", base))
        f.write(struct.pack("<Q", len(payload)))
        f.write(struct.pack("<Q", fnv1a64(payload)))
    return fnv1a64(payload)


def player_alive() -> bool:
    r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq QstPlayer.exe", "/NH"],
                       capture_output=True, text=True, errors="replace", timeout=15)
    return "QstPlayer.exe" in (r.stdout or "")


def run_case(label, with_stop, count_enabled, count, expect_runs, expect_exit, budget=20.0):
    h = build_case(with_stop, count_enabled, count)
    la = os.path.join(WORK, "la_%s_%016x" % (abs(hash(label)) % 997, h))
    os.makedirs(la, exist_ok=True)
    if os.path.isfile(MARK):
        open(MARK, "w").close()

    env = dict(os.environ)
    env["LOCALAPPDATA"] = la

    proc = None
    for _ in range(10):
        try:
            proc = subprocess.Popen([OUT], env=env, cwd=WORK)
            break
        except PermissionError:
            time.sleep(1.5)   # 刚写出的 exe 可能被杀软短暂占用

    t0 = time.time()
    seen = False
    while time.time() - t0 < budget:
        if player_alive():
            seen = True
        elif seen:
            break
        time.sleep(0.4)
    exited = seen and not player_alive()
    elapsed = round(time.time() - t0, 1)
    if not exited:
        subprocess.run(["taskkill", "/F", "/IM", "QstPlayer.exe"],
                       capture_output=True, errors="replace")

    runs = 0
    if os.path.isfile(MARK):
        runs = sum(1 for line in open(MARK, encoding="utf-8", errors="replace")
                   if line.strip() == "RAN")

    ok = (exited == expect_exit)
    ok = ok and (runs == expect_runs if expect_runs is not None else runs >= 3)
    want = ("轮数=%d" % expect_runs) if expect_runs is not None else "轮数>=3"
    print("[%s] 实际轮数=%d 自行退出=%s 用时=%ss（期望 %s 退出=%s）-> %s"
          % (label, runs, exited, elapsed, want, expect_exit, "OK" if ok else "FAIL"))
    return ok


def main() -> int:
    if not os.path.isfile(TEMPLATE):
        print("找不到播放器模板：" + TEMPLATE)
        print("请先构建：cmake --build build --config Release --target QstScriptPlayer")
        return 2

    print("工作目录：" + WORK)
    results = [
        run_case("A 有stopMacro+次数关", True, False, 1, 1, True),
        run_case("B 有stopMacro+次数=5", True, True, 5, 1, True),
        run_case("C 无stopMacro+次数=3", False, True, 3, 3, True),
        run_case("D 无stopMacro+次数关", False, False, 1, None, False, budget=12.0),
    ]
    print("结果：" + ("全部符合预期" if all(results) else "有不符预期的用例"))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
