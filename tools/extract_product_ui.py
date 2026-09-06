# -*- coding: utf-8 -*-
"""Extract product-only UI from docs/ui-redesign-mockup.html into ui/index.html."""
from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "docs" / "ui-redesign-mockup.html"
OUT = ROOT / "ui" / "index.html"

text = SRC.read_text(encoding="utf-8")

m = re.search(r"<style>(.*?)</style>", text, re.S)
if not m:
    raise SystemExit("no <style>")
css = m.group(1)

for pat in (
    r"\.guide\{[^}]*\}",
    r"\.palette-strip\{[^}]*\}",
    r"\.migrate-note\{[^}]*\}",
    r"\.guide,[^{]*\{[^}]*\}",
    r"\.guide b\{[^}]*\}",
    r"\.palette-strip span\{[^}]*\}",
    r"body\.editor-open \.guide,body\.editor-open \.palette-strip,body\.editor-open[^{]*\{[^}]*\}",
    r"body\.editor-open\s+to\{[^}]*\}\}?",
    r"body::before\{.*?\}",
    r"body::after\{.*?\}",
    r"@keyframes auroraDrift\{.*?\}",
):
    css = re.sub(pat, "", css, flags=re.S)

# Product body never uses mockup page padding
css = re.sub(
    r"body\.editor-open\{[^}]*\}",
    "body.editor-open{padding:0}",
    css,
    count=1,
)

css = re.sub(
    r"body\{[^}]*min-height:100vh[^}]*\}",
    "/* body product defaults applied above */",
    css,
    count=1,
    flags=re.S,
)

css = css.replace(
    '--font: "IBM Plex Sans SC", "IBM Plex Sans", "Microsoft YaHei UI", sans-serif;',
    '--font: "Microsoft YaHei UI", "Microsoft YaHei", "Segoe UI", sans-serif;',
)
css = css.replace(
    '--mono: "JetBrains Mono", ui-monospace, monospace;',
    '--mono: Consolas, "Courier New", ui-monospace, monospace;',
)

product_css_head = """
html, body {
  height: 100%;
  width: 100%;
  margin: 0;
  padding: 0;
  overflow: hidden;
  background: var(--sky-100);
  font-family: "Microsoft YaHei UI", "Microsoft YaHei", "Segoe UI", sans-serif;
  color: var(--text);
}
body::before, body::after { display: none !important; content: none !important; }
.app {
  width: 100% !important;
  height: 100% !important;
  min-height: 100% !important;
  max-width: none !important;
  max-height: none !important;
  border-radius: 0 !important;
  box-shadow: none !important;
  margin: 0 !important;
}
.guide, .palette-strip, .migrate-note, .demo-note,
.tray-float, .tray-menu, .overlay-demo, .shot-stage { display: none !important; }
"""

app_start = text.find('<div class="app" id="app">')
if app_start < 0:
    raise SystemExit("app not found")
script_start = text.rfind("<script>")
chunk = text[app_start:script_start]

chunk = re.sub(r'\s*data-toast="[^"]*演示[^"]*"', "", chunk)
chunk = re.sub(r'\s*data-toast="设置已保存"', "", chunk)

for marker in (
    "<!-- 阶段 B：",
    "<!-- OVERLAYS 演示",
    '<div class="tray-float"',
    '<div class="overlay-demo"',
    '<div class="shot-stage"',
):
    cut = chunk.find(marker)
    if cut >= 0:
        chunk = chunk[:cut]
        break

# Always strip leftover demo chrome if cut missed
chunk = re.sub(
    r'<div class="tray-float"[\s\S]*?(?=<script|$)',
    "",
    chunk,
    count=1,
)
chunk = re.sub(r'<div class="tray-menu"[\s\S]*?</div>\s*', "", chunk, count=1)

chunk = chunk.replace(">QuickScript</span>", ">键鼠工坊</span>", 1)
chunk = chunk.replace('id="titleText">QuickScript', 'id="titleText">键鼠工坊')
chunk = chunk.replace("QuickScript — 设置", "键鼠工坊 — 设置")
chunk = chunk.replace(
    "版本 1.x.x · 键鼠自动化 · 极光 UI 原型",
    "键鼠自动化",
)
chunk = re.sub(r'(id="edName"[^>]*>)[^<]*', r"\1", chunk, count=1)
chunk = re.sub(r'\s*data-toast="[^"]*"', "", chunk)


def empty_list(html: str, list_id: str) -> str:
    m2 = re.search(rf'<div class="card-list" id="{list_id}">', html)
    if not m2:
        return html
    start = m2.end()
    i = start
    depth = 1
    while i < len(html) and depth:
        if html.startswith("<div", i):
            depth += 1
            i = html.find(">", i) + 1
            continue
        if html.startswith("</div>", i):
            depth -= 1
            if depth == 0:
                return html[:start] + "\n          " + html[i:]
            i += 6
            continue
        i += 1
    return html


for lid in ("macroList", "recList", "aiList", "clickList"):
    chunk = empty_list(chunk, lid)

html = f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>键鼠工坊</title>
<style>
{product_css_head}
{css}
</style>
<link rel="stylesheet" href="shell.css" />
</head>
<body>
{chunk}
<script src="bridge.js"></script>
<script src="app.js"></script>
</body>
</html>
"""

OUT.write_text(html, encoding="utf-8")
print(f"Wrote {OUT} ({OUT.stat().st_size} bytes)")
