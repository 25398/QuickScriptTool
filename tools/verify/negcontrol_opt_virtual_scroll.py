"""负对照（变异测试）：把已知缺陷装回**副本**，确认两条优化列表校验真的会红。

为什么必须有它
──────────────
「断言全绿」本身不构成证据 —— 一个永远绿的断言等于没有断言。本脚本把历史缺陷
逐条装回去，验证：
  ① 至少有一条断言变红（覆盖到了）
  ② 红的正是**预期的那一条**（不是碰巧被别的断言挡住）

⚠⚠ 绝不改 ui/ 本体：有并行会话把 ui/ 同步进 build/Release/ui/，变异版会进共享产物。
   本脚本只**读** ui/，写到 build/_tmp/ 下的副本，用环境变量指过去：
     QST_APP_JS / QST_INDEX_HTML  → 纯逻辑校验
     QST_PROBE_SRC                → 端到端探针

用法：
    python tools/verify/negcontrol_opt_virtual_scroll.py            # 全部（含 200 万条探针，约 1 分钟）
    python tools/verify/negcontrol_opt_virtual_scroll.py --logic    # 只跑纯逻辑变异（秒级）
    python tools/verify/negcontrol_opt_virtual_scroll.py --probe    # 只跑探针变异
"""
import io
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UI = os.path.join(ROOT, "ui")
APP_SRC = os.path.join(UI, "app.js")
IDX_SRC = os.path.join(UI, "index.html")

MUT_UI = os.path.join(ROOT, "build", "_tmp", "negcontrol_ui")
LOGIC_TEST = os.path.join(ROOT, "tools", "verify", "opt_list_virtual_scroll.js")
PROBE_TEST = os.path.join(ROOT, "tools", "verify", "probe_opt_list_virtual_scroll.py")

PY = sys.executable
NODE = os.environ.get("QST_NODE") or "node"


def read(p):
    with io.open(p, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write(p, s):
    with io.open(p, "w", encoding="utf-8", newline="") as f:
        f.write(s)


# ── 变异定义：每条 = (名字, 期望变红的断言关键字, 变换函数) ────────────
# 变换函数对 app.js 源码做替换；返回原文说明锚点没命中（脚本会报 SKIP）。

NAIVE_RATIO_OLD = (
    "const virtualSize = cap;\n"
    "    const realMax = Math.max(0, actualSize - view);\n"
    "    const virtMax = Math.max(0, virtualSize - view);"
)
NAIVE_RATIO_NEW = (
    "const virtualSize = cap;\n"
    "    return { actualSize, virtualSize, ratio: cap / actualSize };"
)

LOGIC_MUTATIONS = [
    (
        "L1 压缩映射退回「按总高比例」（ratio=virtualSize/actualSize）",
        "最后一行落在视口内",
        lambda a: a.replace(
            "ratio: realMax > 0 && virtMax > 0 ? virtMax / realMax : 1,", "ratio: virtMax / realMax,"
        ).replace(NAIVE_RATIO_OLD, NAIVE_RATIO_NEW),
    ),
    (
        "L2 渲染路径把视口真实容量算成 viewH/ratio（多渲染 1/ratio 倍）",
        "不再把视口真实容量算成 viewH/ratio",
        lambda a: a.replace(
            "const actualOffset = v.ratio === 1 ? scrollTop : scrollTop / v.ratio;\n"
            "    const w = computeOptWindow(actualOffset, viewH, v.rowH, v.total, OPT_VIRT_OVERSCAN);",
            "const actualOffset = v.ratio === 1 ? scrollTop : scrollTop / v.ratio;\n"
            "    const viewActual = v.ratio === 1 ? viewH : viewH / v.ratio;\n"
            "    const w = computeOptWindow(actualOffset, viewActual, v.rowH, v.total, OPT_VIRT_OVERSCAN);",
        ),
    ),
    (
        "L3 压缩上限放宽到 40M（超过浏览器 33.5M 上限 ⇒ 又滚不到底）",
        "压缩上限在跨浏览器安全区内",
        lambda a: a.replace(
            "const OPT_MAX_VSPACE_PX = 16_000_000;", "const OPT_MAX_VSPACE_PX = 40_000_000;"
        ),
    ),
    (
        "L4 computeOptScale 不再夹非有限值（Infinity 透传成 NaN）",
        "退化输入不产生 NaN",
        lambda a: a.replace(
            "const actualSize =\n"
            "      Number.isFinite(n) && n > 0 && Number.isFinite(h) && h > 0 ? n * h : 0;",
            "const actualSize = n > 0 ? n * h : 0;",
        ),
    ),
    (
        "L5 去掉换录制时重置关键操作查找游标（从上一份录制的行号往下找）",
        "换录制时重置关键操作查找游标",
        lambda a: a.replace("state._optKeySearchIdx = null;", ""),
    ),
    (
        "L6 去掉渲染时的实时选择态（退回渲染期快照 ⇒ 全选只选到已加载部分）",
        "渲染期不再有选择态快照",
        lambda a: a.replace(
            "const selSet = optSelSet(); // 实时读取 ⇒ 全选后滚到哪儿都是勾选态",
            "const selSet = new Set(state.optSelected); // 变异：渲染期快照",
        ),
    ),
]

# 静态守卫（CSS）单独一组：只改 index.html
CSS_MUTATIONS = [
    (
        "L7 去掉 .opt-list 的 overflow-anchor:none（滚动锚定导致窗口跳动）",
        "禁用滚动锚定",
        "position:relative;overflow-anchor:none}",
        "position:relative}",
    ),
]

PROBE_MUTATIONS = [
    (
        "P1 压缩映射退回「按总高比例」⇒ 滚到底看不到最后一行",
        "C7 压缩态滚到底能看到最后一条",
        lambda a: a.replace(
            "ratio: realMax > 0 && virtMax > 0 ? virtMax / realMax : 1,", "ratio: virtMax / realMax,"
        ).replace(NAIVE_RATIO_OLD, NAIVE_RATIO_NEW),
    ),
    (
        "P2 渲染路径把视口真实容量算成 viewH/ratio ⇒ DOM 随压缩比膨胀",
        "C4 压缩态首屏仍只渲染几十行",
        lambda a: a.replace(
            "const actualOffset = v.ratio === 1 ? scrollTop : scrollTop / v.ratio;\n"
            "    const w = computeOptWindow(actualOffset, viewH, v.rowH, v.total, OPT_VIRT_OVERSCAN);",
            "const actualOffset = v.ratio === 1 ? scrollTop : scrollTop / v.ratio;\n"
            "    const viewActual = v.ratio === 1 ? viewH : viewH / v.ratio;\n"
            "    const w = computeOptWindow(actualOffset, viewActual, v.rowH, v.total, OPT_VIRT_OVERSCAN);",
        ),
    ),
    (
        "P4 去掉「面板已关就丢弃晚到结果」的保护 ⇒ 关窗后仍被填入",
        "E3 面板已关时，晚到的结果被丢弃",
        lambda a: a.replace(
            '      if (!document.body.classList.contains("opt-open")) return;\n', ""
        ),
    ),
]

# 探针级 CSS 变异：只改 index.html，靠场景 D（真 DOM 斑马纹稳定性）才能验到。
# 锚点 = 本轮修复：三条编辑器斑马纹规则限定在 .action-list 内。
# 变异 = 撤销限定 ⇒ 全局 `.arow:nth-child(even)` (0,4,0) 泄漏进优化列表
#        ⇒ 窗口起点奇偶随滚动翻转 ⇒ 同一行底色不断换（用户报障点）。
PROBE_CSS_MUTATIONS = [
    (
        "P3 撤销 `.action-list` 限定 ⇒ 全局斑马纹泄漏进优化列表（滚动时行变色）",
        "D7 滚动一行后同一动作序号的底色不变",
        lambda h: h.replace(".action-list .arow:nth-child(", ".arow:nth-child("),
    ),
]


def run(cmd, env_extra):
    env = dict(os.environ)
    env.update(env_extra)
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace",
                       env=env, cwd=ROOT, timeout=1800)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def red_lines(out):
    return [ln.strip() for ln in out.splitlines() if ln.strip().startswith(("\u2716", "- "))]


def report(name, expect_kw, rc, out):
    reds = red_lines(out)
    hit = any(expect_kw in r for r in reds)
    if rc == 0:
        print("  \u2716 %s\n      变异后仍全绿 ⇒ 断言覆盖不到该缺陷" % name)
        return False
    if not hit:
        print("  \u26a0 %s\n      变红了，但红的不是预期断言（期望含「%s」）：" % (name, expect_kw))
        for r in reds[:5]:
            print("        %s" % r)
        return False
    print("  \u2714 %s\n      → 预期断言已红（共红 %d 项）" % (name, len(reds)))
    return True


def main():
    only_logic = "--logic" in sys.argv
    only_probe = "--probe" in sys.argv

    base_app = read(APP_SRC)
    base_idx = read(IDX_SRC)
    os.makedirs(MUT_UI, exist_ok=True)
    # 副本里除 app.js / index.html 外都要与 ui/ 一致（字体等），先整目录复制一次
    shutil.copytree(UI, MUT_UI, dirs_exist_ok=True)

    ok = True

    if not only_probe:
        print("== 纯逻辑校验的负对照（tools/verify/opt_list_virtual_scroll.js）==")
        for name, kw, fn in LOGIC_MUTATIONS:
            app = fn(base_app)
            if app == base_app:
                print("  [SKIP] %s —— 锚点未命中（源码文本变了？）" % name)
                ok = False
                continue
            write(os.path.join(MUT_UI, "app.js"), app)
            write(os.path.join(MUT_UI, "index.html"), base_idx)
            rc, out = run([NODE, LOGIC_TEST],
                          {"QST_APP_JS": os.path.join(MUT_UI, "app.js"),
                           "QST_INDEX_HTML": os.path.join(MUT_UI, "index.html")})
            ok = report(name, kw, rc, out) and ok

        for name, kw, old, new in CSS_MUTATIONS:
            idx = base_idx.replace(old, new)
            if idx == base_idx:
                print("  [SKIP] %s —— 锚点未命中" % name)
                ok = False
                continue
            write(os.path.join(MUT_UI, "app.js"), base_app)
            write(os.path.join(MUT_UI, "index.html"), idx)
            rc, out = run([NODE, LOGIC_TEST],
                          {"QST_APP_JS": os.path.join(MUT_UI, "app.js"),
                           "QST_INDEX_HTML": os.path.join(MUT_UI, "index.html")})
            ok = report(name, kw, rc, out) and ok

    if not only_logic:
        print("\n== 端到端探针的负对照（tools/verify/probe_opt_list_virtual_scroll.py）==")
        print("   （含 200 万条场景，每次约 10~30s）")
        for name, kw, fn in PROBE_MUTATIONS:
            app = fn(base_app)
            if app == base_app:
                print("  [SKIP] %s —— 锚点未命中" % name)
                ok = False
                continue
            shutil.copytree(UI, MUT_UI, dirs_exist_ok=True)
            write(os.path.join(MUT_UI, "app.js"), app)
            rc, out = run([PY, PROBE_TEST], {"QST_PROBE_SRC": MUT_UI})
            ok = report(name, kw, rc, out) and ok

        for name, kw, fn in PROBE_CSS_MUTATIONS:
            idx = fn(base_idx)
            if idx == base_idx:
                print("  [SKIP] %s —— 锚点未命中" % name)
                ok = False
                continue
            shutil.copytree(UI, MUT_UI, dirs_exist_ok=True)   # 先铺全量，再覆盖 index.html
            write(os.path.join(MUT_UI, "index.html"), idx)
            rc, out = run([PY, PROBE_TEST], {"QST_PROBE_SRC": MUT_UI})
            ok = report(name, kw, rc, out) and ok

    print("\n副本目录（可删）：%s" % MUT_UI)
    print("\n%s" % ("负对照全部按预期变红 ✔" if ok else "负对照存在未达标项 ✘"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
