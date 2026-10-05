"""ext_lint.py —— 扩展 JS 的**静态检查**（未定义变量 / 重复声明 / 函数覆盖 …）。

## 为什么需要它（2026-09-26 一天内栽了两次同类事故）

`node --check` **只查语法**。而这两类问题语法上完全合法：

| 事故 | 现象 | `node --check` |
|---|---|---|
| **同名函数覆盖** | 函数声明提升、后定义的赢 ⇒ 我新加的 `waitTabComplete` **从来没跑过** | ✅ 全绿 |
| **漏改一处引用** | 把 `const r = ...` 换成别的后，紧跟的 `r && r.error` 还在用 `r` ⇒ **`ReferenceError`** | ✅ 全绿 |

⇒ 第二类的**症状离原因极远**（JS 里一个没改全的变量名 ⇒ 表现为"探针全绿、正常提问全红"），
  我为此白烧了一整轮。**必须用真正的解析器做作用域检查。**

## 依赖

`eslint@8`（装在本机隔离目录 `~/.workbuddy-ai/binaries/node/workspace`，**不污染用户环境**）。
缺了会打印 `ESLINT_MISSING` 并退出码 3 —— 那是**环境问题**，不是代码问题，别当成红。

用法：
    python tools/verify/ext_lint.py                 # 查 extension/edge/*.js
    python tools/verify/ext_lint.py --dir <路径>
退出码：0 = 干净；1 = 有错；3 = 环境缺 eslint
"""

import argparse
import io
import os
import subprocess
import sys

NODE = r'C:\Users\冯思乾\.workbuddy-ai\binaries\node\versions\22.22.2-2\node.exe'
NODE_MODULES = r'C:\Users\冯思乾\.workbuddy-ai\binaries\node\workspace\node_modules'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default='', help='扩展目录（默认自动找 extension/edge）')
    args = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, '..', '..'))
    ext_dir = args.dir or os.path.join(root, 'extension', 'edge')
    linter = os.path.join(here, 'ext_lint_js.js')

    if not os.path.isdir(ext_dir):
        print('[!!] 找不到扩展目录：%s' % ext_dir)
        return 2
    if not os.path.exists(NODE):
        print('[!!] 找不到 node：%s' % NODE)
        return 3

    targets = []
    for name in sorted(os.listdir(ext_dir)):
        if not name.endswith('.js'):
            continue
        if '.bak' in name:            # 备份不查
            continue
        targets.append(os.path.join(ext_dir, name))
    if not targets:
        print('[!!] %s 下没有 .js' % ext_dir)
        return 2

    env = dict(os.environ)
    env['NODE_PATH'] = NODE_MODULES
    try:
        r = subprocess.run([NODE, linter] + targets, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=180)
    except subprocess.TimeoutExpired:
        print('[!!] 检查超时（180s）')
        return 2

    out = r.stdout.decode('utf-8', 'replace').strip()
    if 'ESLINT_MISSING' in out:
        print('[!!] 环境缺 eslint（**不是代码问题**）：')
        print('     ' + out)
        print('     装法：cd "%s" && npm install --no-audit --no-fund eslint@8'
              % os.path.dirname(NODE_MODULES))
        return 3

    print(out)
    print('=' * 70)
    if r.returncode == 0:
        print('扩展 JS 静态检查通过 ✓')
    else:
        print('★ 有静态错误 —— 这类问题 `node --check` **抓不到**，但会在运行时炸。')
        print('  （历史事故：同名函数覆盖 ⇒ 新代码从没跑过；漏改变量 ⇒ ReferenceError）')
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
