#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""括号平衡扫描器（C++ 近似词法）。

用途：手工手术源码（删块/插块）之后，**精确定位**失衡的行 ——
比编译器报的 "catch 没有 try" 之类消息直接得多。

为什么需要它：2026-10-02 我用字符串/行号删块时多吃了 `}`，
编译器只在很远的地方报语法错误（`{ 未找到匹配令牌`），
要靠人肉读几百行才能找到真正少的那一个括号。

判据（近似但足够）：
  · 跳过 `//` 行注释、`/* */` 块注释、`"..."` 与 `'...'` 字面量（含转义与原始字符串的常见形态）
  · 只统计 `{` `}`；`()`、`[]` 一并统计便于交叉验证
  · 逐行输出**深度**，并在深度为负或文件结束深度非 0 时明确报出

用法：
  python tools/verify/brace_balance.py src/ai_action_service.cpp
  python tools/verify/brace_balance.py src/ai_action_service.cpp --from 2227 --to 3340
  python tools/verify/brace_balance.py src/ai_action_service.cpp --context 2
"""

import argparse
import sys


def scan(text):
    """返回 [(line_no, depth_after_line, opens, closes)]，跳过注释与字面量。"""
    depth = 0
    i = 0
    line = 1
    depth_at_line_end = {}
    in_line_comment = False
    in_block_comment = False
    in_str = None      # '"' 或 "'"
    raw_str = False
    n = len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ''
        if c == '\n':
            line += 1
            in_line_comment = False
            depth_at_line_end[line - 1] = depth
            i += 1
            continue
        if in_line_comment:
            i += 1
            continue
        if in_block_comment:
            if c == '*' and nxt == '/':
                in_block_comment = False
                i += 2
                continue
            i += 1
            continue
        if in_str:
            if raw_str:
                if c == ')' and nxt == in_str:
                    in_str = None
                    raw_str = False
                    i += 2
                    continue
                i += 1
                continue
            if c == '\\':
                i += 2
                continue
            if c == in_str:
                in_str = None
            i += 1
            continue
        # 非字符串/注释状态
        if c == '/' and nxt == '/':
            in_line_comment = True
            i += 2
            continue
        if c == '/' and nxt == '*':
            in_block_comment = True
            i += 2
            continue
        if c == 'R' and nxt == '"':
            # 原始字符串 R"delim( ... )delim"
            j = i + 2
            delim = ''
            while j < n and text[j] != '(':
                delim += text[j]
                j += 1
            in_str = '"' + delim
            raw_str = True
            i = j + 1
            continue
        if c == '"' or c == "'":
            in_str = c
            i += 1
            continue
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
        i += 1
    depth_at_line_end[line] = depth
    return depth_at_line_end


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--from', dest='start', type=int, default=1)
    ap.add_argument('--to', dest='end', type=int, default=0)
    ap.add_argument('--context', type=int, default=0,
                    help='只打印深度发生变化的行的前后文行数')
    args = ap.parse_args()

    with open(args.path, 'r', encoding='utf-8', errors='replace') as f:
        text = f.read()
    lines = text.split('\n')
    depths = scan(text)
    end = args.end or len(lines)

    prev = 0
    bad = []
    for ln in range(1, end + 1):
        d = depths.get(ln)
        if d is None:
            continue
        if args.context and ln >= args.start and d != prev:
            lo = max(args.start, ln - args.context)
            for k in range(lo, ln + 1):
                print('%6d [d=%4d] %s' % (k, depths.get(k, 0), lines[k - 1].rstrip()))
            print('-' * 60)
        if ln >= args.start and d < 0:
            bad.append((ln, d))
        prev = d

    total = depths.get(len(lines), 0)
    print('=' * 60)
    print('文件结束深度: %d （0 = 平衡）' % total)
    if bad:
        print('最早出现负深度的行: %d (d=%d)' % bad[0])
    if total != 0:
        # 打印最后 20 行的深度，便于判断是"缺 }"还是"多 }"
        print('末尾深度序列:')
        for ln in range(max(1, len(lines) - 20), len(lines) + 1):
            print('%6d [d=%4d] %s' % (ln, depths.get(ln, 0), lines[ln - 1].rstrip()[:100]))
        sys.exit(1)


if __name__ == '__main__':
    main()
