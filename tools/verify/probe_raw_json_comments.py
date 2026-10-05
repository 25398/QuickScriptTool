# -*- coding: utf-8 -*-
"""守卫：C++ **raw string 字面量**（`R"( ... )"`）里不许出现 `//` 注释。

为什么需要这条守卫（2026-10-02 真实踩坑，LESSONS §81）：
    工具的参数 schema 是这样写的：
        tool.parameters_json = LR"({ "type": "object", ... })";
    ⇒ `R"( ... )"` 里的**一切都是字面内容**，`//` 不是 C++ 注释，而是 JSON 文本的一部分。
    ⇒ `agent_core.cpp` 会 `json::parse(parameters_json)`，**解析失败时静默回退成空 schema**
      （`catch (json::parse_error&) { parameters = {type:object, properties:{}} }`）。
    ⇒ 症状：**工具还在、参数没了**，不报错、不崩溃、编译全绿 —— 只有模型开始瞎猜参数。

    我就是在给 `readScriptReference` 的 section 加 `enum` 时把说明写成了 `//`，
    差一点把整段 schema 静默打掉。⇒ 立成守卫。

用法：
    python tools/verify/probe_raw_json_comments.py            # 扫 src/
    python tools/verify/probe_raw_json_comments.py <路径...>   # 只扫指定文件
    python tools/verify/probe_raw_json_comments.py --selftest  # 负对照：证明这个守卫**能红**
退出码：0 = 干净；1 = 有 raw string 里含 `//`。
"""
from __future__ import annotations

import io
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# ── 负对照夹具（`--selftest` 用）─────────────────────────────────────────────
#   ⚠ 守卫必须**先证明它能红**（LESSONS §30）：一个永远绿的守卫等于没有守卫。
#   夹具里故意放两条：① 违规（parameters_json 的 raw string 里有 `//`）
#                     ② 合法（普通提示词 raw string 里的 `//`，**不许**被报）
SELFTEST_FIXTURE = '''\
void Fixture() {
    tool.parameters_json = LR"({
        "type": "object",   // 违规行
        "properties": {}
    })";
    const wchar_t* kPrompt = LR"(
        示例：{"a":1}   // 合法：给人看的说明
    )";
}
'''

# C++ raw string：R"delim( ... )delim"，delim 可为空
RAW_RE = re.compile(r'R"(?P<delim>[^()\\\s]{0,16})\((?P<body>.*?)\)(?P=delim)"', re.S)

# ★ 只查**会被 json::parse 的**那些 raw string：`parameters_json = R"( ... )"`
#   ⚠ 别扩大成全仓 raw string —— 提示词文本（如 `kSkillScriptStrategy`）里
#     **故意**用 `//` 写 JSON 示例注释，那是给人/模型看的，不是给 json::parse 的。
#     把那种也报出来 = 假红 ⇒ 守卫很快就会被无视（LESSONS §30 的教训）。
#   ⚠ 也**不能**只 grep `parameters_json` 再 grep `//`（跨语句会串台）；
#     必须先把「赋值给 parameters_json 的那个 raw string」精确切出来。
PARAM_RAW_RE = re.compile(
    r'parameters_json\s*=\s*L?R"(?P<delim>[^()\\\s]{0,16})\((?P<body>.*?)\)(?P=delim)"', re.S)


def scan_text(path: str, text: str) -> list[tuple[int, str]]:
    """返回 [(行号, 违规行内容)]。"""
    hits: list[tuple[int, str]] = []
    for m in PARAM_RAW_RE.finditer(text):
        body = m.group("body")
        base_line = text[: m.start()].count("\n") + 1
        for i, line in enumerate(body.split("\n")):
            # `//` 出现在 raw string 里 ⇒ JSON 文本被污染（URL 里的 http:// 例外）
            s = line.strip()
            if s.startswith("//"):
                hits.append((base_line + i, line.strip()))
            elif "//" in line and "://" not in line:
                hits.append((base_line + i, line.strip()))
    return hits


def iter_sources(roots: list[str]):
    for root in roots:
        if os.path.isfile(root):
            yield root
            continue
        for dirpath, _dirs, files in os.walk(root):
            if os.sep + "build" + os.sep in dirpath + os.sep:
                continue
            for fn in files:
                if fn.endswith((".cpp", ".h", ".hpp")):
                    yield os.path.join(dirpath, fn)


def run_selftest() -> int:
    """负对照：夹具里 1 条违规 + 1 条合法 ⇒ 必须**恰好**报出违规那一条。"""
    hits = scan_text("<fixture>", SELFTEST_FIXTURE)
    lines = [ln for ln, _ in hits]
    ok = len(hits) == 1 and lines == [3]
    print("selftest: 命中 %d 条（期望 1 条，且是第 3 行）→ %s"
          % (len(hits), "OK" if ok else "FAIL"))
    for ln, content in hits:
        print("     L%-4d %s" % (ln, content[:80]))
    if ok:
        print("RESULT=SELFTEST_OK  守卫能红，且不误报提示词里的 `//`")
        return 0
    print("RESULT=SELFTEST_FAIL  守卫失效（要么不会红，要么误报）")
    return 1


def main() -> int:
    args = sys.argv[1:]
    if args and args[0] == "--selftest":
        return run_selftest()
    roots = args if args else [os.path.join(REPO, "src")]
    bad = 0
    checked = 0
    for path in iter_sources(roots):
        try:
            text = io.open(path, encoding="utf-8", errors="replace", newline="").read()
        except OSError:
            continue
        if 'R"(' not in text and 'LR"(' not in text:
            continue
        checked += 1
        hits = scan_text(path, text)
        if not hits:
            continue
        bad += 1
        rel = os.path.relpath(path, REPO)
        print("!! %s" % rel)
        for line_no, content in hits:
            print("     L%-6d %s" % (line_no, content[:100]))
    print()
    if bad:
        print("RESULT=BAD  %d 个文件的 `parameters_json` raw string 里写了 `//`"
              "（会被当成 JSON 文本 ⇒ json::parse 失败 ⇒ **静默**回退成空 schema）" % bad)
        return 1
    print("RESULT=CLEAN  扫了 %d 个含 raw string 的源文件，`parameters_json` 未被 `//` 污染"
          % checked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
