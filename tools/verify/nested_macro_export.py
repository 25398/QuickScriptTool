#!/usr/bin/env python3
"""导出的 exe —— **嵌套宏**能否在别人电脑上跑通。

用法（在仓库根目录）：
    python tools/verify/nested_macro_export.py

构造一棵三层嵌套 + 一个「只用 blockName 引用」的脚本树，按导出侧的真实 zip 布局
（`script.json` / `scripts\\<name>.json` / 扁平图片 / `package.json` / `rt\\*`）打包，
追加到播放器模板尾部，然后在一个**全新的 LOCALAPPDATA**（模拟别人电脑）下运行。

期望：四个标记文件全部出现 ⇒ 多层嵌套 + 名字引用在目标机都能解析。

⚠ 两个容易踩的点（都会让这个脚本"看起来失败"，其实是构造问题）：
  1. **嵌套宏里不要放 `stopMacro`** —— 引擎语义是「`stopMacro` 终止**整次运行**」
     （`StopMacroShouldEndEntireRun`），放在子脚本里会把父脚本也一起结束。
     软件里同样如此，不是导出引入的差异。
  2. 导出侧的图片条目是**扁平基名**，靠播放器统一挪进 `scripts\\images\\`。

依赖：已构建的 build\\Release\\tools\\player\\QstPlayer.exe
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

MAGIC = b"QSTPKG01"
FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
MASK = 0xFFFFFFFFFFFFFFFF

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(REPO, "build", "Release")
TEMPLATE = os.path.join(BUILD, "tools", "player", "QstPlayer.exe")
WORK = tempfile.mkdtemp(prefix="qst_nested_export_")
OUT = os.path.join(WORK, "nested.exe")


def fnv1a64(data: bytes) -> int:
    h = FNV_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & MASK
    return h


def marker(name: str) -> str:
    return os.path.join(WORK, name + ".txt")


def act(t, **kw):
    a = {"type": t, "indent": 0}
    a.update(kw)
    return a


def run_program(name: str):
    return act("runProgram", targetPath="cmd.exe",
               inputText="/c echo OK> " + marker(name))


def script(name: str, actions) -> str:
    return json.dumps({
        "scriptName": name, "recordTime": "2026-09-19 22:00:00",
        "inputTimingMode": 2, "inputTimingVersion": 2,
        "hotkeyVk": 0, "hotkeyModifiers": 0, "hotkeyText": "", "hotkeyHold": 0,
        "windowMode": {"enabled": 0}, "actions": actions,
    }, ensure_ascii=False)


def build_payload_zip(zip_path: str) -> None:
    # 第 3 层（叶子）
    grand = script("grand", [run_program("grand")])
    # 第 2 层：引用孙脚本（裸文件名）+ 引用一张图片
    child = script("child", [
        run_program("child"),
        act("runMacro", targetPath="grand.json", blockName=""),
        act("findImage", imagePath="images/nested.bmp", matchThreshold=90,
            findImageFollowUp=0, searchFullScreen=1),
    ])
    # 兄弟脚本：**只用 blockName 引用**（targetPath 空）
    sibling = script("sibling", [run_program("sibling")])
    # 根
    root = script("root", [
        act("runMacro", targetPath="child.json", blockName=""),
        run_program("root"),
        act("runMacro", targetPath="", blockName="sibling"),
        act("stopMacro"),
    ])

    manifest = {
        "v": 1, "root": "script.json",
        "scripts": [
            {"entry": "scripts\\child.json", "ref": "child.json", "name": "child"},
            {"entry": "scripts\\grand.json", "ref": "grand.json", "name": "grand"},
            {"entry": "scripts\\sibling.json", "ref": "sibling.json", "name": "sibling"},
        ],
        "images": ["nested.bmp"],
        "player": {"v": 1, "needOpenCv": 1, "needOcr": 0, "needFakeFocus": 0,
                   "bundledOpenCv": 0, "bundledOcr": 0, "bundledFakeFocus": 0,
                   "createdBy": "verify"},
    }
    settings = json.dumps({
        "playback": {"enablePlaybackCount": False},
        "other": {"playSoundOnStart": False, "playSoundOnEnd": False},
    }, ensure_ascii=False)

    # 最小的合法 BMP（1x1 24bpp）：只为验证图片会被释放到 scripts\images\
    bmp = (b"BM" + struct.pack("<IHHI", 58, 0, 0, 54)
           + struct.pack("<IIIHHIIIIII", 40, 1, 1, 1, 24, 0, 4, 2835, 2835, 0, 0)
           + b"\x00\x00\xff\x00")

    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_STORED) as z:
        z.writestr("script.json", root.encode("utf-8"))
        z.writestr("scripts\\child.json", child.encode("utf-8"))
        z.writestr("scripts\\grand.json", grand.encode("utf-8"))
        z.writestr("scripts\\sibling.json", sibling.encode("utf-8"))
        z.writestr("nested.bmp", bmp)
        z.writestr("package.json", json.dumps(manifest, ensure_ascii=False).encode("utf-8"))
        z.writestr("rt\\app_settings.json", settings.encode("utf-8"))


def player_alive() -> bool:
    r = subprocess.run(["tasklist", "/FI", "IMAGENAME eq QstPlayer.exe", "/NH"],
                       capture_output=True, text=True, errors="replace", timeout=15)
    return "QstPlayer.exe" in (r.stdout or "")


def main() -> int:
    if not os.path.isfile(TEMPLATE):
        print("找不到播放器模板：" + TEMPLATE)
        print("请先构建：cmake --build build --config Release --target QstScriptPlayer")
        return 2

    for f in os.listdir(WORK):
        p = os.path.join(WORK, f)
        if os.path.isfile(p):
            open(p, "w").close()

    tmpzip = os.path.join(WORK, "payload.zip")
    build_payload_zip(tmpzip)
    payload = open(tmpzip, "rb").read()
    shutil.copyfile(TEMPLATE, OUT)
    base = os.path.getsize(OUT)
    with open(OUT, "ab") as f:
        f.write(payload)
        f.write(MAGIC)
        f.write(struct.pack("<Q", base))
        f.write(struct.pack("<Q", len(payload)))
        f.write(struct.pack("<Q", fnv1a64(payload)))
    h = fnv1a64(payload)

    la = os.path.join(WORK, "la_%016x" % h)
    os.makedirs(la, exist_ok=True)
    rt = os.path.join(la, "QstPlayer", "rt", "%016x" % h)
    env = dict(os.environ)
    env["LOCALAPPDATA"] = la
    print("工作目录：" + WORK)

    proc = None
    for _ in range(10):
        try:
            proc = subprocess.Popen([OUT], env=env, cwd=WORK)
            break
        except PermissionError:
            time.sleep(1.5)
    if proc is None:
        print("!! 无法启动（文件被杀软占用）")
        return 1

    t0 = time.time()
    seen = False
    while time.time() - t0 < 40:
        if player_alive():
            seen = True
        elif seen:
            break
        time.sleep(0.4)
    exited = seen and not player_alive()
    print("播放器自行退出：%s  用时 %.1fs" % (exited, time.time() - t0))
    if not exited:
        subprocess.run(["taskkill", "/F", "/IM", "QstPlayer.exe"],
                       capture_output=True, errors="replace")

    results = {}
    print("--- 各层是否执行 ---")
    for name in ("child", "grand", "sibling", "root"):
        results[name] = os.path.isfile(marker(name))
        print("   %-8s -> %s" % (name, "执行了" if results[name] else "**没执行**"))

    nested_ok = os.path.isfile(os.path.join(rt, "scripts", "scripts", "grand.json"))
    img_ok = os.path.isfile(os.path.join(rt, "scripts", "images", "nested.bmp"))
    print("嵌套脚本落在 scripts\\scripts\\ ：%s" % nested_ok)
    print("嵌套脚本引用的图片落在 scripts\\images\\ ：%s" % img_ok)

    ok = all(results.values()) and nested_ok and img_ok
    print("结论：" + ("多层嵌套 + 名字引用在导出的 exe 里都能跑通"
                     if ok else "**有失败项**"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
