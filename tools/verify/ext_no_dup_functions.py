"""ext_no_dup_functions.py — 扩展 JS 的**同名函数覆盖**静态检查。

为什么需要它（2026-09-25 真机闪退事故）：
    `extension/edge/background.js` 里原本已有一个 `waitTabComplete`（返回 **boolean**）。
    我又写了一个同名函数（返回**字符串**）放在更靠前的位置。
    JS 的**函数声明会提升，后定义的赢** ⇒ 我的新函数**从来没生效**，
    调用点拿到的是 boolean ⇒ 回执里 `status: true` ⇒
    宿主按字符串读它 ⇒ 抛 `type_error` ⇒ 异常逃出桥的 HTTP 线程 ⇒ **整个软件静默闪退**。

    ⚠ `node --check` **抓不到**这个 —— 重复声明在 JS 里是**合法**的（静默覆盖）。
      所以必须有这条静态检查。

用法：
    python tools/verify/ext_no_dup_functions.py          # 检查 extension/edge/*.js
    python tools/verify/ext_no_dup_functions.py --all    # 连 .html 里的内联脚本一起（暂不支持）
退出码：0 = 无重复；1 = 有重复（打印位置）
"""

import argparse
import io
import os
import re
import sys

# 顶层（行首无缩进）的声明形式
PATTERNS = [
    re.compile(r'^(?:async\s+)?function\s+([A-Za-z_$][\w$]*)\s*\('),
    re.compile(r'^(?:const|let|var)\s+([A-Za-z_$][\w$]*)\s*=\s*(?:async\s*)?(?:function\b|\()'),
]


def scan(path):
    """返回 {名字: [行号...]}（只统计出现 >1 次的）"""
    with io.open(path, 'r', encoding='utf-8', errors='replace', newline='') as f:
        lines = f.read().splitlines()
    seen = {}
    for i, ln in enumerate(lines, 1):
        for pat in PATTERNS:
            m = pat.match(ln)
            if m:
                seen.setdefault(m.group(1), []).append(i)
                break
    return {k: v for k, v in seen.items() if len(v) > 1}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default='', help='扩展目录（默认自动找 extension/edge）')
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, '..', '..'))
    d = args.dir or os.path.join(root, 'extension', 'edge')
    if not os.path.isdir(d):
        print('[!!] 找不到扩展目录：%s' % d)
        return 2

    bad = 0
    checked = 0
    for name in sorted(os.listdir(d)):
        if not name.endswith('.js'):
            continue
        # .bak-* 之类不查
        if '.bak' in name:
            continue
        path = os.path.join(d, name)
        checked += 1
        dups = scan(path)
        if dups:
            bad += 1
            print('[!!] %s 有**同名覆盖**（后定义者赢，前面的静默失效）：' % name)
            for fn, lines in sorted(dups.items()):
                print('       %s  出现在行 %s' % (fn, ', '.join(str(x) for x in lines)))
            print('       ⇒ 删掉多余的那份，或改名。⚠ `node --check` 抓不到这个。')

    print('=' * 70)
    if bad:
        print('检查了 %d 个 .js，**%d 个有同名覆盖** —— 必须修（否则改动会静默失效）。' % (checked, bad))
        return 1
    print('检查了 %d 个 .js，无同名覆盖 ✓' % checked)
    return 0


if __name__ == '__main__':
    sys.exit(main())
