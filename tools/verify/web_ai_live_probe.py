#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""web_ai_live_probe.py — 网页版 AI 真机冒烟（给非技术用户双击用）。

═══ 这一版为什么整体重写了（2026-09-24，重要）═══

上一版想「从外部用 WebSocket 连上桥，再发 ping / webAiProviders / webAiRichType
等消息去驱动浏览器里的豆包」。**那条路在原理上不成立**，不是配置问题：

  · 桥的 `HandleClient`（`ext_bridge_server.cpp`）只把**入站**消息当成
    `result` / `ready` **回执**来匹配它在等的那个 id，**从不把外部消息转给扩展**；
  · 真正发给扩展的请求只能由**桥自己**用 `Request()` 发出，而 `Request()` 只发给
    **连在本进程监听器上**的扩展 —— 也就是说：**只有产品进程自己**能驱动扩展；
  · 更糟的是：外部连接一旦发了合法 `hello` 就会被登记成「一路扩展」
    （`extSocks_.push_back`），而 `Request()` 对非 attach 类型取的是
    **最新那条连接** ⇒ 探针会**顶替**真扩展去接收请求，而它自己不会应答。

所以上一版 `[0.5]` 之后的每一步都在白等超时（每步 30 秒），看起来像"卡住"。

**现在改走产品自带的原生探针**：产品进程里新增了
`POST /qst/web-ai/probe`（见 `src/web_ai/web_ai_backend.cpp` 的 `RunLiveProbeReport`），
它在**进程内**用 `ExtBridgeServer::Request()` 真跑一遍全链路，把每一步的结果
（站点 / 标签页 / attach / 输入框形态 / 能不能写进去 / 能不能读回回答 / 可选真发一条）
作为一份 JSON 报告回给本脚本。本脚本只负责把它翻译成人话。

用法：
  web_ai_probe.cmd                      # 只读检查（往输入框写一句再清掉，不发消息）
  web_ai_probe.cmd --send "你好"        # 真发一条（会点发送）
  web_ai_probe.cmd --upload             # 额外验传图（会往输入框挂一张 8×8 测试图）
  web_ai_probe.cmd --sync               # 只把扩展源码同步到 build 副本
  web_ai_probe.cmd --port 19228         # 手动指定桥端口（配置过期时用）

退出码：0 = 关键检查全过；2 = 桥/扩展不在；1 = 有失败项。
"""

import argparse
import json
import os
import sys
import time
import urllib.request
from pathlib import Path

# ── 控制台编码 ──────────────────────────────────────────────────────
# ⚠ 必须显式设成 UTF-8：本机 ANSI 代码页是 GB2312，而报告里会出现 ⚠ / 中文标点，
#   按默认编码 print 会直接抛 UnicodeEncodeError（**探针自己崩掉**，2026-09-24 实测）。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

OK = "  [OK]  "
BAD = "  [!!]  "
WARN = "  [??]  "

SKIP_COPY = {"bridge_runtime.json"}


def hr():
    print("-" * 66)


def find_bridge_config():
    cands = []
    la = os.environ.get("LOCALAPPDATA")
    if la:
        cands.append(Path(la) / "QuickScriptTool" / "ext_bridge.json")
    here = Path(__file__).resolve().parents[2]
    cands.append(here / "extension" / "edge" / "bridge_runtime.json")
    for c in cands:
        if c.exists():
            try:
                j = json.loads(c.read_text(encoding="utf-8"))
                if j.get("port"):
                    return c, j
            except Exception:
                continue
    return None, None


def sync_extension_to_build():
    repo = Path(__file__).resolve().parents[2]
    src = repo / "extension" / "edge"
    dest = repo / "build" / "Release" / "extension" / "edge"
    if not (src / "manifest.json").exists():
        return None, f"源码目录缺 manifest.json：{src}"
    try:
        dest.mkdir(parents=True, exist_ok=True)
    except Exception as e:
        return None, f"建不了目标目录：{e}"
    changed = []
    for root, _dirs, files in os.walk(src):
        rel = Path(root).relative_to(src)
        (dest / rel).mkdir(parents=True, exist_ok=True)
        for fn in files:
            if fn in SKIP_COPY:
                continue
            s = Path(root) / fn
            d = dest / rel / fn
            try:
                sb = s.read_bytes()
            except Exception as e:
                return None, f"读 {s} 失败：{e}"
            need = True
            if d.exists():
                try:
                    need = d.read_bytes() != sb
                except Exception:
                    need = True
            if need:
                try:
                    d.write_bytes(sb)
                    changed.append(str((rel / fn).as_posix()))
                except Exception as e:
                    return None, f"写 {d} 失败：{e}"
    return changed, str(dest)


def make_opener():
    """★ 必须绕开本机系统代理。

    实测：本机设了系统代理（127.0.0.1:49755），它会给 127.0.0.1 的请求回
    **502 Bad Gateway** —— 那是代理回的，不是软件回的 ⇒ 会把「软件没开」
    误判成「软件故障」。回环地址本来就不该走代理。
    """
    return urllib.request.build_opener(urllib.request.ProxyHandler({}))


def http_json(opener, url, payload=None, timeout=30):
    data = None
    headers = {}
    if payload is not None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers)
    with opener.open(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def print_report(rep):
    """把原生探针的 JSON 报告翻译成人话。返回 (fatal 列表, warn 列表)"""
    fatal, warn = [], []

    br = rep.get("bridge") or {}
    print(f"  [OK]  桥：port={br.get('port')} 扩展在线={br.get('extConnected')}"
          f"（{br.get('extClients')} 路）token={br.get('tokenPrefix')}…")
    cfgp = br.get("configPath") or ""
    if cfgp:
        exists = "存在" if br.get("configEnabled") is not None else "?"
        print(f"        网页 AI 配置文件：{cfgp}"
              f"（{'已开启' if br.get('configEnabled') else '未开启/不存在'}）")
    print(f"        进程内驱动 = {br.get('extConnected')} "
          f"（本报告由**产品进程自己**发起，不是外部探针）")

    print()
    print("[1] 已知站点（webAiProviders）")
    ext_v = rep.get("extVersion") or "?"
    disk_v = rep.get("diskManifestVersion") or "?"
    print(f"        浏览器里跑的扩展版本 = v{ext_v}   磁盘上的 manifest = v{disk_v}")
    if ext_v not in ("?", "") and disk_v not in ("?", "") and ext_v != disk_v:
        print(WARN + "两边不一致 ⇒ 浏览器里跑的**不是**磁盘上那份代码。")
        print("        到 edge://extensions/ 点「重新加载」⟳，再回豆包页按 F5，然后重跑。")
    provs = rep.get("providers")
    if provs is None:
        print(BAD + f"没拿到站点列表：{rep.get('providersError')}")
        fatal.append("webAiProviders 失败")
    else:
        for p in provs:
            print(f"        · {p.get('label')} ({p.get('id')})  inputKind={p.get('inputKind')}")
        if not provs:
            print(BAD + "站点列表是空的 ⇒ 扩展没读到 web_ai_providers.json")
            fatal.append("providers 为空")

    print()
    print("[2] 该站点的标签页（webAiFindPage，按域名穷举、不截断）")
    own = rep.get("providerPages")
    if own is None:
        print(WARN + f"没拿到页面列表：{rep.get('providerPagesError')}")
    else:
        for p in own:
            print(f"        · tabId={p.get('tabId')}  {str(p.get('title'))[:40]}  "
                  f"{str(p.get('url'))[:60]}")
        if not own:
            print("        （该站点一个都没开 —— 软件会自己开一个**属于它自己的**标签页，")
            print("          不会用你正在聊天的那个，也不会在你的对话里发消息）")
    allp = rep.get("pagesAll")
    if allp:
        print(f"        （诊断用：通用可抓取页面 {len(allp)} 个，不参与判定）")

    print()
    print("[3] 拿专属标签页 + 挂独立调试会话")
    at = rep.get("attach") or {}
    if at.get("ok"):
        print(OK + f"成功（{at.get('ms')}ms）")
        print("        ⚠ 用的是**软件自己的**标签页；脚本/找图那条调试会话不受影响")
    else:
        print(BAD + f"失败：{at.get('error')}")
        # ⚠ 必须带上 message：只显示错误码时，"OPEN_FAILED" 看不出是
        #   "浏览器一个窗口都没有" / "URL 非法" / "权限不足"（2026-09-26 真机踩过）
        _am = at.get("message") or ""
        fatal.append(f"attach 失败（{at.get('error')}）" + (f"：{_am}" if _am else ""))

    pr = rep.get("probe") or {}
    if pr:
        print()
        print("[4] ★ 输入框长什么样（webAiProbe）")
        inp = pr.get("input")
        if inp:
            kind = ("富文本(contenteditable)" if inp.get("contentEditable")
                    else "原生表单(value)" if inp.get("hasValue") else "未知形态")
            print(OK + f"输入框命中：{inp.get('matchedBy')}")
            print(f"        tag={inp.get('tagName')} role={inp.get('role')!r} "
                  f"{inp.get('width')}x{inp.get('height')} → {kind}")
        else:
            print(BAD + "★ 没找到输入框：inputSelectors 与当前页面不匹配")
            fatal.append("输入框未命中")
        btn = pr.get("sendButton")
        print(OK + f"发送按钮命中：{btn.get('matchedBy')}" if btn
              else WARN + "发送按钮没命中（提交退化为按 Enter，通常也能用）")

    wr = rep.get("write")
    if wr is not None:
        print()
        print("[5] ★ 能不能把字写进输入框（webAiRichType，写完即清空）")
        steps = wr.get("steps") or {}
        prep = steps.get("prepare") or {}
        if prep:
            print(f"        命中输入框：{prep.get('matchedBy')}  可编辑={prep.get('isEditable')}"
                  f"  焦点在自己身上={prep.get('focusIsSelf')}")
        fin = steps.get("verifyAfterFallback") or steps.get("verify") or {}
        print(f"        **回读长度={fin.get('textLength')}**  内容={str(fin.get('text'))[:40]!r}")
        if wr.get("ok"):
            print(OK + "★★ 写入成功且已回读确认 —— 富文本这一关过了！")
        else:
            print(BAD + f"写入失败：{wr.get('error')} {wr.get('message') or ''}")
            fatal.append(f"webAiRichType 失败（{wr.get('error')}）")

    rd = rep.get("reply")
    if rd is not None:
        print()
        print("[6] ★ 能不能读回回答（readAssistantReply）")
        if rd.get("ok"):
            print(OK + f"读到 {rd.get('chars')} 字（{rd.get('blocks')} 块，"
                       f"matchedBy={rd.get('matchedBy')}）")
            print("        开头：" + str(rd.get("head") or "").replace("\n", " ⏎ ")[:200])
        else:
            print(WARN + f"读回答失败：{rep.get('replyError')}")
    else:
        print()
        print("[6] 读回答：本次未执行（前一步已失败）")

    sd = rep.get("send")
    if sd is not None:
        print()
        print("[7] 真发一条消息")
        if sd.get("ok"):
            print(OK + f"★★★ 端到端打通！用时 {sd.get('waitMs')}ms，"
                       f"读回 {sd.get('replyChars')} 字")
            print("        " + str(sd.get("replyHead") or "").replace("\n", " ⏎ ")[:300])
        else:
            print(BAD + f"失败：{sd.get('error')} {sd.get('message') or ''}")
            fatal.append(f"真发一条失败（{sd.get('error')}）")

    # [8] 传图（只有显式 --upload 才有这一段）
    up = rep.get("upload")
    if up is not None:
        print()
        print("[8] ★ 能不能传图（webAiUploadImages，会往输入框挂一张 8×8 测试图）")
        print(f"        宿主侧：解码={up.get('decoded')} {up.get('pngBytes')} 字节 "
              f"→ {up.get('tempPath')}")
        steps = up.get("steps") or {}
        if steps:
            lb = steps.get("listBefore") or {}
            print(f"        页面上的 file input 数={lb.get('count')}"
                  f" · 缩略图 {steps.get('thumbsBefore')}→{steps.get('thumbsAfter')}")
            for row in (lb.get("inputs") or [])[:4]:
                print(f"          #{row.get('idx')} accept={row.get('accept') or '(空)'}"
                      f" multiple={row.get('multiple')} visible={row.get('visible')}"
                      f" {row.get('w')}x{row.get('h')} files={row.get('fileCount')}")
            tried = steps.get("tried") or []
            if tried:
                print("        逐个尝试：" + " | ".join(
                    f"#{t.get('idx')} 回读={t.get('after')} 缩略图={t.get('thumbs')}"
                    f" ok={t.get('ok')}" + (f" err={t.get('error')}" if t.get("error") else "")
                    for t in tried))
            if steps.get("chooser") is not None:
                print(f"        文件选择器：{steps.get('chooser')}"
                      f" · 附件按钮={steps.get('fileButton')}")
        if up.get("ok"):
            print(OK + f"★★ 传图成功（via={up.get('via') or (steps.get('verify') and 'direct')}，"
                       f"{up.get('ms')}ms）")
            print("        ⚠ 输入框里现在留着那个附件 —— **请手动删掉它**。")
        else:
            print(BAD + f"传图失败：{up.get('error')} {up.get('message') or ''}")
            print("        ⇒ 看上面「逐个尝试」：若某个 input 的缩略图变多了，说明图**其实挂上了**")
            warn.append(f"传图失败（{up.get('error')}）")
    return fatal, warn


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--provider", default="doubao")
    ap.add_argument("--send", default="", help="真发一条消息（会点发送）；留空则只做只读检查")
    ap.add_argument("--port", type=int, default=0, help="手动指定桥端口")
    ap.add_argument("--timeout", type=int, default=300, help="等待探针返回的秒数")
    ap.add_argument("--sync", action="store_true", help="只同步扩展源码到 build 副本")
    ap.add_argument("--upload", action="store_true",
                    help="额外验一次传图（会往输入框挂一张 8×8 测试图，验完请手动删掉）")
    args = ap.parse_args()

    print("=" * 66)
    print(" 网页版 AI 真机冒烟（Web AI Backend live probe）")
    print("=" * 66)
    print()

    if args.sync:
        changed, info = sync_extension_to_build()
        if changed is None:
            print(BAD + f"同步失败：{info}")
            return 2
        if changed:
            print(OK + f"已更新 {len(changed)} 个文件：")
            for c in changed:
                print(f"        · {c}")
        else:
            print(OK + "两边已一致，无需更新")
        print(f"\n      目标目录：{info}")
        print("      下一步：edge://extensions/ 点扩展卡片的「重新加载」⟳ → 回豆包页按 F5。")
        return 0

    print("[0] 本机桥 / 扩展连接")
    cfg_path, cfg = find_bridge_config()
    if not cfg:
        print(BAD + "没找到桥配置文件。多半是 QuickScriptTool.exe 没在运行。")
        print("      → 先双击启动：D:\\other\\software\\build\\Release\\QuickScriptTool.exe")
        return 2
    port = int(cfg["port"]) if not args.port else args.port
    token = cfg.get("token", "")
    print(OK + f"桥配置：{cfg_path}")
    print(f"        port={port}  token={token[:8]}…")

    opener = make_opener()
    try:
        st = http_json(opener, f"http://127.0.0.1:{port}/qst/status", timeout=5)
    except Exception as e:
        print(BAD + f"打不开桥的 HTTP 口：{type(e).__name__}: {e}")
        print("      → 软件没在运行（502 是本机代理回的，不是软件回的）。")
        return 2
    if not st.get("ws"):
        print(BAD + f"端口 {port} 上不是本产品的桥：{st}")
        return 2
    print(OK + f"桥在线：port={st.get('port')} running={st.get('running')}")
    # ★★ 扩展离线时**先等一会儿再判定**（2026-09-26 真机踩过）：
    #   `edge://extensions/` 点「重新加载」⟳ 的**那几秒**，service worker 正在重启，
    #   桥看到的就是 0 路连接 ⇒ 探针直接报「离线」，**把"正在重启"误报成"没在跑"**。
    #   ⚠ 扩展自己也有 1.5s 轮询 + 30s alarms 兜底 ⇒ 等 20 秒足够它回来。
    if not st.get("extConnected", False):
        print("  [..]  扩展暂时离线 —— 等它自己连回来（最多 20s，重载扩展后是正常的）…")
        import time as _t
        _deadline = _t.time() + 20
        while _t.time() < _deadline:
            _t.sleep(2)
            try:
                st = http_json(opener, f"http://127.0.0.1:{port}/qst/status", timeout=5)
            except Exception:
                continue
            if st.get("extConnected", False):
                print(OK + "   └─ 扩展：**已连上**（等了 %d 秒）"
                      % int(20 - (_deadline - _t.time())))
                break
    if not st.get("extConnected", False):
        print(BAD + "   └─ 扩展：**离线**（等了 20 秒仍未连上，0 路连接）")
        print("      → 点一下浏览器工具栏的扩展图标唤醒它；或 edge://extensions/ 点「重新加载」⟳；")
        print("      → 若刚点过 ⟳，再等 30 秒（service worker 重启 + 自动重连需要时间）。")
        print("      → 还不行就看扩展的 Service Worker 控制台：")
        print("        edge://extensions/ → 找到「键鼠工坊」→ 点「Service Worker」→ 看有没有报错。")
        return 2
    print(OK + f"   └─ 扩展：**在线**（{st.get('extClients')} 路连接）")
    print()
    print("  ⚠ 真正能驱动扩展的只有**产品进程自己**（桥的 Request 只发给连在本进程上的扩展），")
    print("    所以下面这一步是**产品内的原生探针**，不是从本脚本直连。")
    print()

    payload = {"provider": args.provider, "send": bool(args.send), "text": args.send,
               "upload": bool(args.upload)}
    t0 = time.time()
    try:
        rep = http_json(opener,
                        f"http://127.0.0.1:{port}/qst/web-ai/probe?token={token}",
                        payload=payload, timeout=args.timeout)
    except Exception as e:
        print(BAD + f"原生探针没有返回：{type(e).__name__}: {e}")
        print("      → 若报 404：跑的是**旧版软件**（这个路由是本次新加的）⇒ 重新构建并重启软件。")
        return 1
    dt = time.time() - t0

    fatal, warn = print_report(rep)

    print()
    hr()
    for line in (rep.get("verdict") or []):
        print("  · " + str(line))
    hr()
    if fatal:
        print(f"结论：有 {len(fatal)} 个阻塞问题 —— " + "；".join(fatal))
        print("把上面 [!!] 那几行复制给我。")
    else:
        print(f"结论：关键检查全过（原生探针用时 {dt:.1f}s）。")
        if not args.send:
            print("      想跑真正的端到端：web_ai_probe.cmd --send \"你好\"")
    hr()
    return 0 if not fatal else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n已中断")
        sys.exit(130)
