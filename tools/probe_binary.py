#!/usr/bin/env python3
"""probe_binary.py — 在 exe / obj / dll 里搜特征串，用于「修复到底有没有进产物」的取证

为什么需要它
------------
本仓的纪律是「说『修好了』之前必须能指出产物里的具体证据」。最容易踩的坑是拿
**源码改了** 当 **产物里有** —— 编译失败、增量没重编、链的是旧 obj，都会让这两件事
不一致。判据只能是**产物本身**。

`strings` 之类的工具不够用，因为 MSVC 的字符串可能是 ASCII，也可能是 UTF-16LE；
而且我们常常要「确认某个串**不**在产物里」（例如「旧版本的诊断日志不该出现在新 exe 里」），
所以两种编码都要查、并且要把 0 命中显式报出来。

用法
----
    python tools/probe_binary.py <binary> <needle> [needle ...]

例：

    # 悬浮球修复的标记是否进了产品 exe
    python tools/probe_binary.py build/Release/QuickScriptTool.exe \\
        SHQueryUserNotificationState SHELL32.dll

    # 诊断日志串（只在真有动作时打印）是否进了 exe
    python tools/probe_binary.py build/Release/QuickScriptTool.exe "scope refresh tab="

输出：每个 needle 一行 `[FOUND]/[absent]` + ASCII / UTF-16LE 各自的命中次数。

注意
----
* 符号名（如 `RefreshDedicatedHotkeyScope`）在 **.obj** 里通常有（COFF 符号表），
  但在 **Release exe** 里一般没有（除非带 PDB）—— 所以「exe 里搜不到符号名」是正常的，
  要搜的是**字符串字面量**。
* 找不到 PDB 时别指望符号，改用「产品里本来就有的字符串字面量」当标记。
"""
import sys


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    path = argv[1]
    needles = argv[2:]
    with open(path, "rb") as f:
        data = f.read()
    print("file = %s" % path)
    print("size = %d bytes" % len(data))
    for n in needles:
        # ⚠ 必须用严格编码：`encode("ascii", "ignore")` 会把纯非 ASCII 的串
        #    静默压成 b""，而 `data.count(b"")` 返回 len(data)+1 ⇒ **任何中文特征串
        #    都会被假报 FOUND**（实测 "需用户确认" 得到 ascii=6488065 = 文件大小+1）。
        #    取证工具假报 = 比没有工具更糟，所以这里显式区分「不适用」。
        try:
            needle_ascii = n.encode("ascii")
        except UnicodeEncodeError:
            needle_ascii = None
        ascii_hit = None if needle_ascii is None else data.count(needle_ascii)
        utf16_hit = data.count(n.encode("utf-16-le"))
        verdict = "FOUND" if ((ascii_hit or 0) or utf16_hit) else "absent"
        ascii_txt = "n/a" if ascii_hit is None else str(ascii_hit)
        print("  [%-6s] %-42r ascii=%s utf16le=%d" % (verdict, n, ascii_txt, utf16_hit))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
