#!/usr/bin/env python3
"""patch_file.py — 大文件精确字符串替换（绕开编辑器/工具的文件占用）

为什么需要它
------------
本仓有多个 500KB+ 的巨型源文件（`src/engine/engine_host_window.h` 约 654KB、
`src/engine/engine_script_run.cpp` 约 484KB）。用编辑工具改这些文件时会随机报
`EBUSY: resource busy or locked, open '...'` —— 杀软实时扫描会在瞬时持有句柄，
重试也未必能过。这个脚本改走「读全文 → 校验匹配次数 → 写回」，稳定且可复核。

与编辑工具相比的额外好处
------------------------
* **CRLF 感知**：源码是 CRLF 时，补丁里写 LF 换行也能匹配（自动转换）。
* **唯一性校验**：`count` 不匹配就**拒绝写入**并退出非 0，不会误替换到第二处。
* **可批量**：一个 JSON 里放多个补丁，一次执行，全成功才退出 0。

用法
----
    python tools/patch_file.py patch.json

patch.json 格式（对象或对象数组）：

    [
      {
        "file":  "D:/other/software/src/engine/engine_hotkeys.cpp",
        "old":   "原样粘贴的待替换文本（可含中文）",
        "new":   "替换后的文本",
        "count": 1
      }
    ]

`count` 可省略，默认 1（即要求「有且仅有一处匹配」）。

约定
----
* 文件按 **UTF-8** 读写，**保留原换行风格**（不做全文件换行归一化）。
* 改完请用 `grep -n` 复核行号与内容 —— 本仓的纪律是「每步都要有证据」。
"""
import json
import sys


def apply_one(op):
    path = op["file"]
    old = op["old"]
    new = op["new"]
    expect = op.get("count", 1)
    with open(path, "r", encoding="utf-8", newline="") as f:
        s = f.read()
    # CRLF 感知：补丁按 LF 写也能命中 CRLF 源文件
    if "\r\n" in s:
        old = old.replace("\r\n", "\n").replace("\n", "\r\n")
        new = new.replace("\r\n", "\n").replace("\n", "\r\n")
    n = s.count(old)
    if n != expect:
        print("FAIL %s: found %d match(es), expected %d" % (path, n, expect))
        return False
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(s.replace(old, new))
    print("OK   %s (%d replacement)" % (path, n))
    return True


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    with open(argv[1], "r", encoding="utf-8") as f:
        ops = json.load(f)
    if isinstance(ops, dict):
        ops = [ops]
    ok = True
    for op in ops:
        if not apply_one(op):
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
