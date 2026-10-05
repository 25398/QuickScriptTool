"""web_ai_log_tail.py — 把「网页 AI」相关日志的最后若干行打出来（给非技术用户用）。

为什么单独做这个：网页 AI 的排查**全靠日志**（软件闪退时没有任何界面提示），
而 `window_mode_debug.log` 有几 MB，用户不知道该复制哪一段。
本脚本只挑 `[网页AI]` 开头的行（外加进程启动分隔线），默认最后 40 行。

用法：
  web_ai_log.cmd                 # 最后 40 行
  web_ai_log.cmd --n 80          # 最后 80 行
  web_ai_log.cmd --all           # 全部（很长）
  web_ai_log.cmd --out log.txt   # 同时存一份到文件
"""

import argparse
import io
import os
import sys

# 日志里属于「网页 AI」链路的前缀（多写几个：不同阶段用的标记不同）
KEYS = ("[网页AI]", "[扩展桥]", "[窗口/后台窗口模式] 配套扩展")


def find_log(explicit):
    if explicit:
        return explicit
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, "..", ".."))
    for cand in (
        os.path.join(root, "build", "Release", "window_mode_debug.log"),
        os.path.join(root, "window_mode_debug.log"),
    ):
        if os.path.exists(cand):
            return cand
    return os.path.join(root, "build", "Release", "window_mode_debug.log")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default="", help="日志路径（默认自动找 build/Release/window_mode_debug.log）")
    ap.add_argument("--n", type=int, default=40, help="打印最后多少行匹配项（默认 40）")
    ap.add_argument("--all", action="store_true", help="打印全部匹配项")
    ap.add_argument("--out", default="", help="同时写一份到该文件")
    args = ap.parse_args()

    path = find_log(args.log)
    if not os.path.exists(path):
        print("[!!] 找不到日志：%s" % path)
        print("     软件至少跑过一次才会有这个文件。")
        return 2

    try:
        with io.open(path, "r", encoding="utf-8", errors="replace", newline="") as f:
            lines = f.read().splitlines()
    except Exception as e:
        print("[!!] 读日志失败：%s: %s" % (type(e).__name__, e))
        return 2

    hits = [ln for ln in lines if any(k in ln for k in KEYS)]
    if not args.all:
        hits = hits[-max(1, args.n):]

    print("=" * 78)
    print(" 网页 AI 日志（最后 %s 条匹配）" % ("全部" if args.all else len(hits)))
    print(" 文件：%s" % path)
    print(" 总行数：%d；匹配行数：%d" % (len(lines), len(hits)))
    print("=" * 78)
    for ln in hits:
        print(ln)
    print("=" * 78)
    print("把上面这一整段复制给我即可。")
    print()
    print("⚠ 排查闪退时最有用的是**最后 10 行**：崩溃前最后一条日志指向的就是出事的函数。")

    if args.out:
        try:
            with io.open(args.out, "w", encoding="utf-8", newline="") as f:
                f.write("\n".join(hits) + "\n")
            print("（已另存到 %s）" % args.out)
        except Exception as e:
            print("[!!] 写文件失败：%s" % e)
    return 0


if __name__ == "__main__":
    sys.exit(main())
