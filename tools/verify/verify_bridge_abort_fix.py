# -*- coding: utf-8 -*-
"""verify_bridge_abort_fix.py — 验收「停过脚本后桥仍可用」这个修复。

背景（bug）：
  `EngineHost::StopRun()` 会置 `ExtBridgeServer` 的 `abort_` 闩锁，而清它的
  `WindowModeExecutor::EndRun()` 只在**窗口模式会话开着**时才跑
  ⇒ 跑「窗口模式关闭」的脚本后，常开桥永久变聋（新连接 TCP 连得上但被直接重置，
  且桥不留日志）。修法两道防线：
    1. `StopRun()` 里 NotifyCancel() 之后立刻 ClearAbort()；
    2. `HandleClient()` 不再把 abort_ 当拒绝新连接的门闩（只留 stop_）。

本脚本做的验收：
  [1] 记下当前 /qst/status 的值（基线）。
  [2] 触发一次「停止脚本」（等价于用户按停止热键 / 脚本自然结束）——
      通过桥自身的 HTTP 接口不行，所以这里走 **WS 的 hello + 不发请求** 无法触发；
      改为**直接观测**：请用户手动跑一次脚本并停止，然后回到本脚本按回车。
      ⚠ 这是**故意**的：不能自己造一个 abort 就宣称验证通过 —— 必须走真实路径。
  [3] 停止后再次读 /qst/status：
        · 仍能正常返回 JSON ⇒ 修复生效
        · 报 ConnectionReset/Aborted ⇒ 修复未生效（或跑的是旧 exe）

用法：
  1) 启动 QuickScriptTool.exe
  2) python verify_bridge_abort_fix.py --baseline      # 看基线正常
  3) 在软件里跑一个「窗口模式关闭」的脚本，然后**停止**它（或用停止热键）
  4) python verify_bridge_abort_fix.py --after         # 关键：这一步必须正常
"""
import argparse
import json
import os
import socket
import sys
import time


def read_cfg():
    p = os.path.expandvars(r"%LOCALAPPDATA%\QuickScriptTool\ext_bridge.json")
    if not os.path.exists(p):
        return None, p
    try:
        return json.load(open(p, "r", encoding="utf-8-sig")), p
    except Exception:
        return None, p


def exe_info():
    """报出当前产物 exe 的 mtime/大小。

    为什么值得单独报：**「跑的还是旧 exe」是验收失败最常见的原因**，
    而这个脚本没法从外部读进程的映像时间戳（需要提权/别的接口），
    所以退一步 —— 报出磁盘上产物的时间，让用户自己对照「我是不是刚构建过」。
    """
    p = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))), "build", "Release", "QuickScriptTool.exe")
    if not os.path.exists(p):
        return None, p
    st = os.stat(p)
    import datetime
    return (datetime.datetime.fromtimestamp(st.st_mtime), st.st_size), p


def scan_ports():
    out = []
    for port in range(19220, 19261):
        s = socket.socket()
        s.settimeout(0.12)
        try:
            s.connect(("127.0.0.1", port))
            out.append(port)
        except Exception:
            pass
        finally:
            s.close()
    return out


def probe(port):
    """返回 (kind, payload)。kind ∈ {'ok','reset','refused','proxy','other'}"""
    import urllib.request as u
    op = u.build_opener(u.ProxyHandler({}))
    try:
        with op.open("http://127.0.0.1:%d/qst/status" % port, timeout=6) as r:
            return "ok", json.loads(r.read().decode("utf-8", "replace"))
    except Exception as e:
        msg = "%s: %s" % (type(e).__name__, e)
        if "502" in msg:
            return "proxy", msg
        if "10054" in msg or "ConnectionReset" in msg or "10053" in msg \
                or "ConnectionAborted" in msg:
            return "reset", msg
        if "10061" in msg or "refused" in msg.lower():
            return "refused", msg
        return "other", msg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", action="store_true", help="只看基线")
    ap.add_argument("--after", action="store_true", help="看停止脚本之后（关键一步）")
    args = ap.parse_args()

    cfg, path = read_cfg()
    ports = scan_ports()
    print("=" * 64)
    print(" 验收：停过脚本后，常开桥是否仍然可用")
    print("=" * 64)
    print("配置：%s" % path)
    exe, exe_path = exe_info()
    if exe:
        print("产物：%s" % exe_path)
        print("      mtime=%s  size=%d" % (exe[0], exe[1]))
        print("      ⚠ 若这个时间**早于**你最后一次构建 ⇒ 你跑的可能是旧 exe，先重建。")
    else:
        print("产物：找不到 %s" % exe_path)
    print("扫到的桥端口：%s" % (ports or "（无）"))
    print()

    if not ports:
        print("[!!] 没有任何端口在听 ⇒ 软件没在跑。")
        print("     → 双击启动 D:\\other\\software\\build\\Release\\QuickScriptTool.exe")
        return 2

    port = int(cfg["port"]) if cfg and cfg.get("port") in ports else ports[0]
    if cfg and cfg.get("port") and cfg["port"] not in ports:
        print("[??] 配置文件里的端口 %s 没在听，改用 %d" % (cfg["port"], port))

    kind, payload = probe(port)
    print("探测 %d → %s" % (port, kind))
    if kind == "ok":
        print("      %s" % json.dumps(payload, ensure_ascii=False))
        print()
        print("  [OK] 桥正常应答。")
        if payload.get("extConnected") is not None:
            print("       扩展：%s（%s 路）" % (
                "在线" if payload.get("extConnected") else "离线",
                payload.get("extClients")))
        if args.baseline:
            print()
            print(" 下一步：在软件里【跑一个窗口模式关闭的脚本】然后【停止】它，")
            print("         再执行：  python %s --after" % os.path.basename(__file__))
        return 0

    if kind == "reset":
        print("      %s" % payload)
        print()
        if args.after:
            print("  [FAIL] 停止脚本之后，桥**连得上但请求被重置** ⇒ 修复**未生效**。")
            print("         可能原因：")
            print("           · 跑的还是旧 exe（确认 exe mtime 是本次构建的）")
            print("           · 这次没跑到修复的代码路径")
        else:
            print("  [FAIL] 基线就不正常 —— 先解决这个（多半是旧 exe，或桥早就被闩死）。")
            print("         重启软件再试。")
        return 1

    print("      %s" % payload)
    print("  [??] 非预期状态。若是 proxy ⇒ 系统代理在干扰（本脚本已绕代理，不该出现）。")
    return 1


if __name__ == "__main__":
    sys.exit(main())
