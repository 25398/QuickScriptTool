"""列出与「构建 / 产品占用」相关的进程，用于判定 LNK1104 的真实成因。

用法：
    python tools/verify/probe_build_lock.py                # 只看编译链进程
    python tools/verify/probe_build_lock.py QuickScriptTool QstPlayer   # 额外关注的产品进程

为什么需要它：
    `LNK1104 无法打开 X` 有三种完全不同的成因，处理方式相反 ——
      ① 别的会话正在同一 build\\ 里构建  → 等静默再重试，**别并发**
      ② 有人正开着目标 exe              → 等它关闭，**别强杀**（那是用户的东西）
      ③ 上一个链接进程残留              → 等几秒
    盲猜会误杀用户进程。本脚本给出事实。

约定（见 .cursor/skills/window-mode-debug、.workbuddy-ai/memory/LESSONS.md §1）：
    连续 3 次探测为 0 才算「静默」，再开始构建。
"""
import ctypes
import ctypes.wintypes as wt
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

BUILD_TOOLS = {"msbuild.exe", "cl.exe", "link.exe", "lib.exe", "vctip.exe",
               "mspdbsrv.exe", "cmake.exe", "ninja.exe"}


def enum_processes():
    buf = (wt.DWORD * 8192)()
    need = wt.DWORD()
    if not psapi.EnumProcesses(buf, ctypes.sizeof(buf), ctypes.byref(need)):
        raise SystemExit("EnumProcesses failed")
    for i in range(need.value // ctypes.sizeof(wt.DWORD)):
        pid = buf[i]
        if not pid:
            continue
        h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not h:
            continue
        try:
            sz = wt.DWORD(1024)
            p = ctypes.create_unicode_buffer(1024)
            if k32.QueryFullProcessImageNameW(h, 0, p, ctypes.byref(sz)):
                yield pid, p.value
        finally:
            k32.CloseHandle(h)


def main():
    extra = {a.lower() + ("" if a.lower().endswith(".exe") else ".exe")
             for a in sys.argv[1:]}
    build_hits, extra_hits = [], []
    for pid, path in enum_processes():
        name = path.rsplit("\\", 1)[-1].lower()
        if name in BUILD_TOOLS:
            build_hits.append((pid, path))
        elif name in extra:
            extra_hits.append((pid, path))

    print("BUILD_PROCS=%d" % len(build_hits))
    for pid, path in build_hits:
        print("   PID=%-7d %s" % (pid, path))
    if extra:
        print("TARGET_PROCS=%d" % len(extra_hits))
        for pid, path in extra_hits:
            print("   PID=%-7d %s" % (pid, path))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
