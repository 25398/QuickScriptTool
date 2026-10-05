"""负对照：证明 `run_hook_race.py` 的断言**真的会红**（本项目硬纪律）。

做法（与 `negcontrol_opt_virtual_scroll.py` 同思路）：
    用 `legacy/` 里**修复前**（git HEAD 原样副本）的 `fake_focus_hook.h/.cpp`
    另编一个 `race_test_legacy.exe`，跑同一套压力场景，断言：

        | 档位   | 修复前（legacy）      | 修复后（仓库当前实现） |
        |--------|----------------------|----------------------|
        | 1 线程 | PASS（无并发）        | PASS                 |
        | 2 线程 | **必须红**            | PASS                 |
        | 8 线程 | **必须红（多为段错误）** | PASS               |

    只要 legacy 那两档没有红，就说明测试**覆盖不到**这个缺陷（断言写松了 / 场景不触发），
    本次修复的"已验证"结论就不成立 —— 脚本会以非 0 退出。

⚠ 为什么不能只在 `src/` 上做变异：本仓有并行会话把 `src/` 同步进共享产物，
  变异版会污染别人。所以负对照必须落在**副本**上（这里就是 `legacy/`）。

用法: python tools/verify/hook_race/negcontrol_hook_race.py
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
LEGACY = os.path.join(HERE, "legacy")
DROP = ("HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy",
        "ALL_PROXY", "all_proxy", "NO_PROXY", "no_proxy")

RUNS = [(1, 4), (2, 4), (8, 4)]


def env():
    return {k: v for k, v in os.environ.items() if k not in DROP}


def cmake(*args):
    p = subprocess.run(["cmake", *args], cwd=ROOT, env=env(),
                       capture_output=True, text=True, errors="replace")
    if p.returncode != 0:
        print((p.stdout or "") + (p.stderr or ""))
        raise SystemExit("cmake 失败: cmake " + " ".join(args))


def run(exe, threads, seconds):
    p = subprocess.run([exe, str(threads), str(seconds)], cwd=os.path.dirname(exe),
                       env=env(), capture_output=True, text=True, errors="replace")
    out = ((p.stdout or "") + (p.stderr or "")).strip()
    m = re.search(r"RESULT (\w+)", out)
    return (m.group(1) if m else "CRASH"), p.returncode, out


def build(src_dir, out_dir, target):
    cmake("-S", os.path.relpath(src_dir, ROOT), "-B", os.path.relpath(out_dir, ROOT),
          "-G", "Visual Studio 17 2022", "-A", "x64")
    cmake("--build", os.path.relpath(out_dir, ROOT), "--config", "Debug",
          "--target", target)


def main():
    # ① 修复版：必须全绿（顺带确保它已构建）
    build(HERE, os.path.join(HERE, "out"), "race_test")
    fixed_exe = os.path.join(HERE, "out", "Debug", "race_test.exe")

    # ② 修复前：必须红
    build(LEGACY, os.path.join(HERE, "out_legacy"), "race_test_legacy")
    legacy_exe = os.path.join(HERE, "out_legacy", "Debug", "race_test_legacy.exe")

    for exe, label in ((fixed_exe, "修复后"), (legacy_exe, "修复前(legacy)")):
        if not os.path.exists(exe):
            raise SystemExit("未找到 " + exe)

    print("%-18s %-8s %-8s %s" % ("实现", "线程", "结果", "退出码/统计"))
    print("-" * 78)
    bad = []
    for exe, label in ((fixed_exe, "修复后"), (legacy_exe, "修复前(legacy)")):
        for threads, seconds in RUNS:
            verdict, rc, out = run(exe, threads, seconds)
            stat = re.search(r"calls=(\d+) detour=(\d+) av=(\d+) badResult=(\d+) missedDetour=(-?\d+)", out)
            info = ("calls=%s av=%s missed=%s" % (stat.group(1), stat.group(3), stat.group(5))
                    if stat else "no summary (进程被带走)")
            print("%-18s %-8s %-8s rc=%-10s %s" % (label, threads, verdict, rc, info))

            if label == "修复后":
                if verdict != "PASS":
                    bad.append("修复后 %d 线程竟然 %s" % (threads, verdict))
            else:
                # 修复前：1 线程允许 PASS（无并发），2/8 线程**必须红**
                if threads == 1 and verdict != "PASS":
                    bad.append("修复前 1 线程意外 %s（应无并发问题）" % verdict)
                if threads >= 2 and verdict == "PASS":
                    bad.append("修复前 %d 线程竟然 PASS —— 负对照不成立，测试覆盖不到该缺陷" % threads)

    print("-" * 78)
    if bad:
        print("NEGCONTROL FAILED:")
        for b in bad:
            print("  - " + b)
        return 1
    print("NEGCONTROL OK: 修复前 2/8 线程必红、修复后全绿 —— 断言确实钉得住这个缺陷")
    return 0


if __name__ == "__main__":
    sys.exit(main())
