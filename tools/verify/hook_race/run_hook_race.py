"""复现 / 回归：fake_focus 内联钩的「临时还原 + 重写跳转」多线程写入竞态。

背景（2026-10-03 用户报障「后台窗口模式/窗口模式导致一些窗口闪退」）：
    `CallThroughOriginal()` 为了调用原函数会「还原 12 字节 → 调用 → 重写 12 字节
    绝对跳转」，而 x86-64 上 12 字节写入**不是原子的**。目标进程（Unity / UE 这类
    多线程游戏）在多个线程上同时调用被钩函数时会取到「半个跳转」⇒
    `mov rax, <垃圾>; jmp rax` ⇒ **目标进程 0xC0000005 闪退**（用户日志的 exit 码）。
    同一路径还会静默**漏钩**（还原窗口期调用绕过 detour）—— 表现为「后台输入时灵时不灵」。

为什么单独做一个工程：
    这是**并发时序**缺陷，纯逻辑断言（`WindowModeSelfTest` 那种）钉不住；
    必须真起多个线程去调被钩函数。所以直接编译
    `src/window_mode/fake_focus/fake_focus_hook.cpp` 的**真实实现**（+ MinHook）。

判据（三档都必须 PASS）：
    · 修复前（临时还原）：1 线程 PASS、2 线程 av>0（0xC0000005）+ 漏钩 ~26 万、8 线程直接段错误
    · 修复后（MinHook trampoline）：1/2/8 线程全部 av=0、漏钩=0、返回值全对

用法:
    python tools/verify/hook_race/run_hook_race.py            # 跑 1 / 2 / 8 三档
    python tools/verify/hook_race/run_hook_race.py 2 5        # 自定义：2 线程 5 秒
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
OUT = os.path.join(HERE, "out")
# 本机四份代理变量会让 MSBuild 抛 MSB6001（与代码无关），从子进程环境里剔掉。
DROP = ("HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy",
        "ALL_PROXY", "all_proxy", "NO_PROXY", "no_proxy")

DEFAULT_RUNS = [(1, 5), (2, 5), (8, 5)]


def env():
    return {k: v for k, v in os.environ.items() if k not in DROP}


def cmake(*args):
    p = subprocess.run(["cmake", *args], cwd=ROOT, env=env(),
                       capture_output=True, text=True, errors="replace")
    if p.returncode != 0:
        print((p.stdout or "") + (p.stderr or ""))
        raise SystemExit("cmake 失败: cmake " + " ".join(args))


def main():
    runs = DEFAULT_RUNS
    if len(sys.argv) > 1:
        runs = [(int(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else 5)]

    cmake("-S", os.path.relpath(HERE, ROOT), "-B", os.path.relpath(OUT, ROOT),
          "-G", "Visual Studio 17 2022", "-A", "x64")
    cmake("--build", os.path.relpath(OUT, ROOT), "--config", "Debug")

    exe = os.path.join(OUT, "Debug", "race_test.exe")
    if not os.path.exists(exe):
        raise SystemExit("未找到 " + exe)

    bad = []
    for threads, seconds in runs:
        p = subprocess.run([exe, str(threads), str(seconds)], cwd=os.path.dirname(exe),
                           env=env(), capture_output=True, text=True, errors="replace")
        out = ((p.stdout or "") + (p.stderr or "")).strip()
        m = re.search(r"RESULT (\w+)", out)
        verdict = m.group(1) if m else "CRASH"
        stat = re.search(r"calls=(\d+) detour=(\d+) av=(\d+) badResult=(\d+) missedDetour=(-?\d+)", out)
        print("[%d 线程 %ds] %-6s rc=%d  %s" % (
            threads, seconds, verdict, p.returncode,
            ("calls=%s av=%s bad=%s missed=%s" % (stat.group(1), stat.group(3),
                                                  stat.group(4), stat.group(5))) if stat else out[:120]))
        if verdict != "PASS":
            bad.append((threads, verdict))

    if bad:
        print("\nFAIL: %s" % ", ".join("%d 线程 -> %s" % b for b in bad))
        return 1
    print("\nPASS: 全部档位 av=0 / 漏钩=0（目标函数头不再被并发改写）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
