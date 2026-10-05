"""探针：优化列表虚拟滚动（窗口化）的**端到端**验证 —— headless Edge 真跑。

为什么需要它（而不是只靠 tools/verify/opt_list_virtual_scroll.js）
────────────────────────────────────────────────────────────────
那个脚本只覆盖**纯函数数学**。「DOM 是否随滚动增长」「全选后未渲染区域
是否也是勾选态」这两件事只能在真 DOM 里看 —— 而它们正是用户报障的两个症状：

  1. 5.8 万条动作时界面卡顿（旧「分批追加」DOM 只增不减，滚到底累积到全量）
  2. 「全选」只能选到已加载的部分（选择态用了**渲染期快照**）

做法：把 ui/ 复制到 build/_tmp/optvirt/，在 index.html 副本末尾注入 harness.js
（mock WebView2 桥 → 灌 5.8 万条动作 → 滚动 → 点「快速选择 → 全选」→ 再滚到底；
再换 200 万条重跑一遍，验证压缩映射下**仍能滚到最后一行**），
结果写进 document.title，用 `msedge --dump-dom` 取回并断言。

⚠ 负对照（变异测试）用 `QST_PROBE_SRC=<ui 的副本>` 指过去 —— 绝不改 ui/ 本体
（有并行会话把 ui/ 同步进 build/Release/ui/，变异版会进共享产物）。

已知取舍（不是缺陷）
────────────────────
headless + `--virtual-time-budget` 下没有真实帧循环，rAF 不会被调度
⇒ harness 把 rAF polyfill 成 setTimeout（两者语义都是「下一个 tick」，
被测代码只在运行时引用 rAF，因此对该逻辑等价）。

用法：
    python tools/verify/probe_opt_list_virtual_scroll.py
    python tools/verify/probe_opt_list_virtual_scroll.py --raw     # 只打印，不断言
"""
import io
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = r"D:\other\software"
# QST_PROBE_SRC 可指向 ui/ 的**副本**（负对照用）—— 绝不改 ui/ 本体。
SRC = os.environ.get("QST_PROBE_SRC") or os.path.join(ROOT, "ui")
DST = os.path.join(ROOT, "build", "_tmp", "optvirt")
PROFILE = os.path.join(ROOT, "build", "_tmp", "optvirt_profile")

EDGE_CANDIDATES = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
]

HARNESS = r"""
/* 优化列表虚拟滚动 · 端到端探针（headless Edge）
 * 验证 A：5.8 万条动作时 DOM 行数恒定（不随滚动增长）
 * 验证 B：「全选」后滚到未渲染区域，新出现的行也是勾选态（旧实现是空白）
 * 验证 C：70 万条（真实总高 ≈39.7M > 浏览器 33.5M 单元素上限）时，
 *         压缩映射仍能**滚到最后一行**，且勾选态照旧正确。
 * 结果写进 document.title，供 --dump-dom 提取。 */
(function () {
  var out = [];
  function note(k, v) { out.push(k + "=" + v); document.title = "PROBE " + out.join(" | "); }
  function flush() { document.title = "PROBE " + out.join(" | "); }
  note("harness_loaded", 1);

  // headless + --virtual-time-budget 下没有真实帧循环，rAF 不会被调度
  // ⇒ 把 rAF 落到 setTimeout（两者语义都是「下一个 tick/帧」，对本逻辑等价）。
  if (!window.__qstRealRaf) {
    window.__qstRealRaf = window.requestAnimationFrame;
    window.requestAnimationFrame = function (cb) {
      return setTimeout(function () { cb(Date.now()); }, 16);
    };
    window.cancelAnimationFrame = function (id) { clearTimeout(id); };
  }

  var listeners = [];
  window.chrome = window.chrome || {};
  window.chrome.webview = {
    addEventListener: function (t, cb) { if (t === "message") listeners.push(cb); },
    postMessage: function () {},
  };
  window.qstBridge = { post: function () {} };
  function feed(obj) {
    listeners.slice().forEach(function (cb) {
      try { cb({ data: obj }); } catch (e) { note("feedErr", e && e.message); }
    });
  }

  var N = 58000;
  // 超长录制：200 万条 × ≈54.4px ≈ 108.8M px ≫ Chrome/Edge 的 33.5M 单元素高度上限
  // ⇒ 旧实现 spacer 撑不开、scrollbar 失效、**滚不到底**（静默）。
  // 取 200 万而非 70 万，是为了让「视口真实容量算成 viewH/ratio」这类**多渲染**
  // 缺陷也放大到可判（70 万时只多渲染 3 行，量级上区分不出来）。
  var BIG = 2000000;
  function mkActions(n) {
    var a = new Array(n);
    for (var i = 0; i < n; ++i) {
      a[i] = (i % 500 === 0)
        ? { type: "mouseDown", button: 1, remark: "\u70b9\u51fb" }
        : { type: "moveMouse", x: i % 1920, y: i % 1080, remark: "" };
    }
    return a;
  }

  function rows() { return document.querySelectorAll("#optList .arow").length; }
  function nodes() { return document.querySelectorAll("#optList *").length; }
  function checked() { return document.querySelectorAll("#optList .arow.checked").length; }
  function lastIdx() {
    var all = document.querySelectorAll("#optList .arow");
    return all.length ? all[all.length - 1].dataset.i : "-1";
  }
  function firstIdx() {
    var el = document.querySelector("#optList .arow");
    return el ? el.dataset.i : "-1";
  }
  function vspaceH() {
    var v = document.getElementById("optVspace");
    return v ? Math.round(parseFloat(v.style.height) || 0) : -1;
  }
  function raf2(fn) { requestAnimationFrame(function () { requestAnimationFrame(fn); }); }

  // 点「快速选择 → 全选」；找不到就把 err 写进结果并返回 false
  function pickAll() {
    var btn = document.getElementById("btnQuickSelect");
    if (!btn) { note("err", "no_btnQuickSelect"); flush(); return false; }
    btn.click();
    var menu = document.querySelector(".popup-menu.show");
    if (!menu) { note("err", "no_popup"); flush(); return false; }
    var items = menu.querySelectorAll(".mi");
    var all = null;
    for (var i = 0; i < items.length; ++i) {
      if ((items[i].textContent || "").trim() === "\u5168\u9009") { all = items[i]; break; }
    }
    if (!all) { note("err", "no_quanxuan_item"); flush(); return false; }
    all.click();
    return true;
  }

  // ── 场景 D：斑马纹必须按「动作序号」稳定，不能随滚动变化 ──────────
  // 用户报障：「滚动时两种色调不断变化」。
  // 根因：编辑器那条全局规则 `.arow:nth-child(even)`（特异性 0,4,0）压过了
  // 优化列表的 `.opt-list .arow.tone-a`（0,3,0）⇒ 行的底色由**它在窗口 DOM 里的
  // 位置**决定，而窗口起点随滚动变 ⇒ 奇偶翻转 ⇒ 同一行换色。
  function toneMap() {
    var m = {};
    var rows = document.querySelectorAll("#optList .arow");
    for (var i = 0; i < rows.length; ++i) {
      m[rows[i].dataset.i] = getComputedStyle(rows[i]).backgroundColor;
    }
    return m;
  }
  function startD() {
    note("D0_begin", 1);
    var list = document.getElementById("optList");
    if (!list) { note("err", "no_optList_D"); flush(); return; }
    var probeRow = list.querySelector(".arow");
    var rowH = probeRow ? probeRow.getBoundingClientRect().height : 54;
    var mid = Math.round((list.scrollHeight - list.clientHeight) / 2);
    list.scrollTop = mid;
    list.dispatchEvent(new Event("scroll"));
    raf2(function () {
      var a = toneMap();
      // 斑马纹只看**非关键操作**行：关键操作（.key-op）有自己的主色底，不参与斑马。
      var even = {}, odd = {}, keyOps = 0;
      var rowsA = document.querySelectorAll("#optList .arow");
      for (var i = 0; i < rowsA.length; ++i) {
        var cls = rowsA[i].classList;
        if (cls.contains("key-op")) { keyOps++; continue; }
        var bg = getComputedStyle(rowsA[i]).backgroundColor;
        (Number(rowsA[i].dataset.i) % 2 === 0 ? even : odd)[bg] = 1;
      }
      note("D1_rows_a", Object.keys(a).length);
      note("D1b_keyop_rows", keyOps);
      note("D2_even_colors", Object.keys(even).length);
      note("D3_odd_colors", Object.keys(odd).length);
      note("D4_even_c0", Object.keys(even)[0] || "-");
      note("D5_odd_c0", Object.keys(odd)[0] || "-");

      // 恰好滚一行 ⇒ 窗口起始下标的奇偶翻转（这是复现的关键）
      list.scrollTop = mid + Math.round(rowH);
      list.dispatchEvent(new Event("scroll"));
      raf2(function () {
        var b = toneMap();
        var changed = 0, compared = 0, sample = "-";
        for (var k in a) {
          if (!(k in b)) continue;
          compared++;
          if (a[k] !== b[k]) { changed++; if (sample === "-") sample = "#" + k + " " + a[k] + " -> " + b[k]; }
        }
        note("D6_compared", compared);
        note("D7_changed", changed);
        note("D8_sample", sample);
        startE();
      });
    });
  }

  // ── 场景 E：加载挪到后台线程后，**晚到的结果**必须按面板状态处理 ────
  // C++ 侧把大录制的解析放到后台线程（几百 ms），结果可能晚到。若用户在这期间已经
  // 关掉优化窗，就**不能**再用它填列表 —— 否则 tryRevealOptMode → revealModeAfterPaint
  // 会对着一个已关掉的界面调 qst.modeReady()，让壳按优化模式改窗口尺寸。
  // 观测量选 `#optSelCount`：`renderOptListRows` 的 finish() 无条件刷新它，
  // 所以「有没有被填」看得见。
  function startE() {
    note("E0_begin", 1);
    note("E1_open_before", document.body.classList.contains("opt-open") ? 1 : 0);
    feed({
      type: "loadOptimizeRecording.result", ok: true,
      recording: { path: "C:\\fake\\e1.json", name: "E1", durationSeconds: 1, actions: mkActions(7) },
    });
    raf2(function () {
      note("E2_count_open", (document.getElementById("optSelCount") || {}).textContent || "?");
      // 模拟「用户把优化窗关掉」
      document.body.classList.remove("opt-open");
      // 再送一份**条数不同**的结果（晚到的那一份）
      feed({
        type: "loadOptimizeRecording.result", ok: true,
        recording: { path: "C:\\fake\\e2.json", name: "E2", durationSeconds: 1, actions: mkActions(3) },
      });
      raf2(function () {
        note("E3_count_after_late", (document.getElementById("optSelCount") || {}).textContent || "?");
        document.body.classList.add("opt-open");   // 还原，别影响收尾
        flush();
      });
    });
  }

  // ── 场景 C：超长录制（200 万条）的压缩映射 ──────────────────────
  // 行按真实行高渲染 ⇒ 屏幕上只装得下 viewH/rowH 行；压缩必须对齐「可滚动区间」，
  // 否则滚到底时最后若干行永远看不到（列表越长漏得越多）。
  function startC() {
    note("C0_begin", 1);
    feed({
      type: "loadOptimizeRecording.result", ok: true,
      recording: {
        path: "C:\\fake\\huge.json", name: "\u8d85\u957f\u538b\u529b",
        durationSeconds: 3600, actions: mkActions(BIG),
      },
    });
    raf2(function () {
      var list = document.getElementById("optList");
      if (!list) { note("err", "no_optList_C"); flush(); return; }
      note("C1_total", BIG);
      note("C2_vspace_h", vspaceH());
      note("C3_scrollH", list.scrollHeight);
      note("C4_rows_open", rows());
      note("C5_clientH", list.clientHeight);

      list.scrollTop = list.scrollHeight; // 请求滚到底（会被浏览器夹到 max）
      list.dispatchEvent(new Event("scroll"));
      raf2(function () {
        note("C6_rows_bottom", rows());
        note("C7_lastIdx_bottom", lastIdx());
        note("C8_firstIdx_bottom", firstIdx());

        if (!pickAll()) return;
        raf2(function () {
          note("C9_checked_bottom_after_all", checked());
          note("C10_rows_bottom_after_all", rows());
          note("C11_selN", (document.getElementById("optSelN") || {}).textContent || "?");
          list.scrollTop = 0;
          list.dispatchEvent(new Event("scroll"));
          raf2(function () {
            note("C12_checked_top", checked());
            note("C13_rows_top", rows());
            note("C14_firstIdx_top", firstIdx());
            startD();
          });
        });
      });
    });
  }

  function start() {
    note("bridge_listeners", listeners.length);
    document.body.classList.add("opt-open");
    var ov = document.getElementById("ov-opt");
    if (ov) ov.classList.add("show");

    feed({
      type: "loadOptimizeRecording.result", ok: true,
      recording: {
        path: "C:\\fake\\stress.json", name: "\u538b\u529b\u6d4b\u8bd5",
        durationSeconds: 123.45, actions: mkActions(N),
      },
    });

    raf2(function () {
      var list = document.getElementById("optList");
      if (!list) { note("err", "no_optList"); flush(); return; }
      note("total", N);
      note("A1_rows_open", rows());
      note("A2_nodes_open", nodes());
      note("A3_vspace_h", vspaceH());
      note("A4_clientH", list.clientHeight);
      note("A5_scrollH", list.scrollHeight);

      list.scrollTop = Math.round((list.scrollHeight - list.clientHeight) / 2);
      list.dispatchEvent(new Event("scroll"));
      raf2(function () {
        note("A6_rows_mid", rows());
        note("A7_nodes_mid", nodes());
        note("A8_firstIdx_mid", firstIdx());

        list.scrollTop = list.scrollHeight;
        list.dispatchEvent(new Event("scroll"));
        raf2(function () {
          note("A9_rows_bottom", rows());
          note("A10_nodes_bottom", nodes());
          note("A11_lastIdx_bottom", lastIdx());

          list.scrollTop = 0;
          list.dispatchEvent(new Event("scroll"));
          raf2(function () {
            note("B1_rows_back_top", rows());
            if (!pickAll()) return;
            raf2(function () {
              note("B2_checked_top", checked());
              note("B3_selN", (document.getElementById("optSelN") || {}).textContent || "?");
              note("B4_selCount", (document.getElementById("optSelCount") || {}).textContent || "?");

              list.scrollTop = list.scrollHeight;
              list.dispatchEvent(new Event("scroll"));
              raf2(function () {
                note("B5_rows_bottom_after_all", rows());
                note("B6_checked_bottom", checked());
                note("B7_lastIdx_bottom", lastIdx());
                note("B8_all_visible_checked", checked() === rows());
                startC();
              });
            });
          });
        });
      });
    });
  }

  if (document.readyState === "complete" || document.readyState === "interactive") {
    setTimeout(start, 400);
  } else {
    document.addEventListener("DOMContentLoaded", function () { setTimeout(start, 400); });
  }
})();
"""


def find_edge():
    for p in EDGE_CANDIDATES:
        if os.path.exists(p):
            return p
    raise SystemExit("找不到 msedge.exe")


def build_probe():
    # ⚠ 不用 shutil.rmtree：宿主有 safe-delete shim，会转 trash 且可能失败。
    os.makedirs(DST, exist_ok=True)
    shutil.copytree(SRC, DST, dirs_exist_ok=True)

    with io.open(os.path.join(DST, "harness.js"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(HARNESS.lstrip("\n"))

    idx = os.path.join(DST, "index.html")
    with io.open(idx, "r", encoding="utf-8", newline="") as f:
        html = f.read()
    # 每次重建：先把上一次注入的 harness 标签摘掉，避免叠加
    html = re.sub(r'<script src="harness\.js"></script>\s*', "", html)
    tag = '<script src="harness.js"></script>'
    m = re.search(r"</body\s*>", html, re.I)
    html = (html[: m.start()] + tag + "\n" + html[m.start():]) if m else (html + "\n" + tag + "\n")

    # ⚠⚠ 必须给本地 js 加时间戳 query：Chromium 对 file:// 的脚本**会走磁盘缓存**，
    #    复用同一个 --user-data-dir 时，改了 app.js 也可能仍加载上一次的版本
    #    ⇒ 负对照会拿到假绿灯（真踩过：变异没生效却报 GREEN）。
    stamp = str(int(time.time() * 1000))
    html = re.sub(r'src="([^"]+\.js)"', lambda mm: 'src="%s?v=%s"' % (mm.group(1), stamp), html)

    with io.open(idx, "w", encoding="utf-8", newline="") as f:
        f.write(html)
    return idx


def run_probe(idx):
    edge = find_edge()
    cmd = [
        edge, "--headless=new", "--disable-gpu", "--no-sandbox", "--no-first-run",
        "--user-data-dir=" + PROFILE,
        "--window-size=1640,1140",
        "--virtual-time-budget=30000",
        "--dump-dom",
        "file:///" + idx.replace("\\", "/"),
    ]
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=300)
    m = re.search(r"<title>(PROBE[^<]*)</title>", p.stdout or "")
    if not m:
        raise SystemExit("探针未产出结果（title 里没有 PROBE）。stdout 尾部：\n"
                         + (p.stdout or "")[-800:])
    kv = {}
    for part in m.group(1)[len("PROBE"):].split("|"):
        part = part.strip()
        if "=" in part:
            k, v = part.split("=", 1)
            kv[k.strip()] = v.strip()
    return kv


def assert_results(kv):
    """对探针结果做断言。返回失败项列表（空 = 全通过）。

    抽成独立函数是为了**负对照**：注入变异后直接调它，确认断言真的会红
    （见文件末尾 __main__ 的 --raw 分支用法）。
    """
    fails = []

    def chk(name, cond, detail=""):
        if cond:
            print("  \u2714 " + name)
        else:
            fails.append(name + (" :: " + detail if detail else ""))
            print("  \u2716 " + name + (" :: " + detail if detail else ""))

    def num(k):
        try:
            return float(kv.get(k, "nan"))
        except ValueError:
            return float("nan")

    print("断言：")
    chk("桥已接上（app.js 注册了 qst:bridge 监听）", num("bridge_listeners") >= 1,
        "bridge_listeners=" + kv.get("bridge_listeners", "?"))
    chk("灌入 5.8 万条动作", num("total") == 58000, "total=" + kv.get("total", "?"))

    # A. DOM 恒定：无论首屏/中部/底部，渲染行数都应是「视口行数 + 2*overscan」量级。
    #    视口 601px、行高 ≈56.66px、overscan 12 ⇒ 理论上界 ceil(601/56.66)+24 = 35。
    #    ⚠ 阈值必须贴住这个上界：若有人把视口真实容量算成 viewH/ratio（多渲染 1/ratio 倍），
    #      70 万条时会渲染 ~51 行 —— 用 80 当阈值就漏了，必须收窄。
    ROWS_CAP = 45
    for k in ("A1_rows_open", "A6_rows_mid", "A9_rows_bottom"):
        chk("%s 渲染行数 ≤ %d（DOM 恒定，不随滚动累积）" % (k, ROWS_CAP),
            0 < num(k) <= ROWS_CAP, "%s=%s" % (k, kv.get(k, "?")))
    for k in ("A2_nodes_open", "A7_nodes_mid", "A10_nodes_bottom"):
        chk("%s DOM 节点 ≤ 500（旧实现滚到底约 23 万）" % k, 0 < num(k) <= 500, "%s=%s" % (k, kv.get(k, "?")))
    chk("总高撑起滚动条（vspace ≈ total × 行高）", num("A3_vspace_h") > 1_000_000,
        "vspace_h=" + kv.get("A3_vspace_h", "?"))
    chk("中部窗口不在列表开头（真窗口化，不是「渲染前 N 条」）", num("A8_firstIdx_mid") > 1000,
        "firstIdx_mid=" + kv.get("A8_firstIdx_mid", "?"))
    chk("滚到底能看到最后一条（end 夹取正确）", num("A11_lastIdx_bottom") == 57999,
        "lastIdx_bottom=" + kv.get("A11_lastIdx_bottom", "?"))

    # B. 选择态：全选后滚到未渲染区域，新出现的行也必须是勾选态
    chk("「全选」把选择数设为全部 5.8 万", num("B3_selN") == 58000, "selN=" + kv.get("B3_selN", "?"))
    chk("全选后顶部可见行全勾选", num("B2_checked_top") == num("B1_rows_back_top"),
        "checked_top=%s rows_top=%s" % (kv.get("B2_checked_top", "?"), kv.get("B1_rows_back_top", "?")))
    chk("★ 全选后滚到底部，新渲染的行同样是勾选态（用户报障点）",
        num("B5_rows_bottom_after_all") > 0 and num("B6_checked_bottom") == num("B5_rows_bottom_after_all"),
        "checked_bottom=%s rows_bottom=%s（旧实现这里是 0）" % (
            kv.get("B6_checked_bottom", "?"), kv.get("B5_rows_bottom_after_all", "?")))
    chk("底部仍是最后一条（滚到底且没越界）", num("B7_lastIdx_bottom") == 57999,
        "lastIdx_bottom=" + kv.get("B7_lastIdx_bottom", "?"))
    chk("B8 可见行全部勾选", kv.get("B8_all_visible_checked") == "true",
        "B8=" + kv.get("B8_all_visible_checked", "?"))

    # ── C. 压缩映射：200 万条（真实总高 ≈108.8M ≫ Chrome/Edge 的 33.5M 上限）──
    # 旧实现 spacer 撑不开 ⇒ scrollbar 失效 ⇒ **滚不到底**（静默，用户看到「列表少了尾巴」）。
    if num("C1_total") != 2000000:
        fails.append("C 场景未跑起来（C1_total=%s）" % kv.get("C1_total", "?"))
        print("  \u2716 C 场景未跑起来（C1_total=%s）" % kv.get("C1_total", "?"))
        return fails

    chk("C2 压缩后 spacer 高度 ≤ 16M（在浏览器上限内，滚动条有效）",
        0 < num("C2_vspace_h") <= 16_000_000, "vspace_h=" + kv.get("C2_vspace_h", "?"))
    chk("C3 列表 scrollHeight 也在上限内（不会撑破）",
        0 < num("C3_scrollH") <= 16_100_000, "scrollH=" + kv.get("C3_scrollH", "?"))
    chk("C4 压缩态首屏仍只渲染几十行（DOM 与条数无关）",
        0 < num("C4_rows_open") <= 45, "rows_open=" + kv.get("C4_rows_open", "?"))
    chk("C6 压缩态滚到底仍只渲染几十行（DOM 恒定）",
        0 < num("C6_rows_bottom") <= 45, "rows_bottom=" + kv.get("C6_rows_bottom", "?"))
    chk("★★ C7 压缩态滚到底能看到最后一条（按总高比压缩时此处看不到末行）",
        num("C7_lastIdx_bottom") == 1999999, "lastIdx_bottom=" + kv.get("C7_lastIdx_bottom", "?"))
    chk("★ C9 压缩态全选后底部新渲染的行仍是勾选态",
        num("C10_rows_bottom_after_all") > 0 and
        num("C9_checked_bottom_after_all") == num("C10_rows_bottom_after_all"),
        "checked=%s rows=%s" % (kv.get("C9_checked_bottom_after_all", "?"),
                                kv.get("C10_rows_bottom_after_all", "?")))
    chk("C11 全选把选择数设为全部 200 万", num("C11_selN") == 2000000,
        "selN=" + kv.get("C11_selN", "?"))
    chk("C12 回到顶部可见行同样全勾选", num("C12_checked_top") == num("C13_rows_top"),
        "checked_top=%s rows_top=%s" % (kv.get("C12_checked_top", "?"), kv.get("C13_rows_top", "?")))
    chk("C14 回到顶部窗口从第 0 行开始（压缩态顶部不偏移）",
        num("C14_firstIdx_top") == 0, "firstIdx_top=" + kv.get("C14_firstIdx_top", "?"))

    # ── D. 斑马纹稳定性：底色必须只由「动作序号」决定 ──────────────
    # 用户报障：「滚动时两种色调不断变化」。若底色由行在**窗口 DOM 里的位置**
    # （:nth-child 奇偶）决定，窗口起点一变奇偶就翻转 ⇒ 同一行换色。
    if num("D1_rows_a") > 0:
        chk("D2 偶数序号只有一种底色（斑马纹由序号决定）",
            num("D2_even_colors") == 1, "even_colors=%s" % kv.get("D2_even_colors", "?"))
        chk("D3 奇数序号只有一种底色",
            num("D3_odd_colors") == 1, "odd_colors=%s" % kv.get("D3_odd_colors", "?"))
        chk("D4/D5 两种底色确实不同（斑马纹存在，不是退化成纯色）",
            kv.get("D4_even_c0", "-") != kv.get("D5_odd_c0", "-"),
            "even=%s odd=%s" % (kv.get("D4_even_c0", "?"), kv.get("D5_odd_c0", "?")))
        chk("D6 滚动前后有足够多行可比对（≥10）", num("D6_compared") >= 10,
            "compared=" + kv.get("D6_compared", "?"))
        chk("★★ D7 滚动一行后同一动作序号的底色不变（用户报障点）",
            num("D6_compared") >= 10 and num("D7_changed") == 0,
            "changed=%s/%s 例：%s" % (kv.get("D7_changed", "?"), kv.get("D6_compared", "?"),
                                     kv.get("D8_sample", "?")))
    else:
        fails.append("D 场景未跑起来（D1_rows_a=%s）" % kv.get("D1_rows_a", "?"))
        print("  \u2716 D 场景未跑起来")

    # ── E. 异步加载：面板已关时，晚到的结果必须丢弃 ──────────────────
    # 观测量 `#optSelCount` = "<总数> · 已选 <n>"；`renderOptListRows` 的 finish()
    # 无条件刷新它 ⇒ 「列表有没有被这份结果填过」看得见。
    if kv.get("E2_count_open", "?") != "?":
        c_open = kv.get("E2_count_open", "?")
        c_late = kv.get("E3_count_after_late", "?")
        chk("E1 送结果前优化窗是开着的（前置条件）", num("E1_open_before") == 1,
            "open_before=" + kv.get("E1_open_before", "?"))
        chk("E2 面板开着时结果正常填入（7 条）",
            "7" in c_open, "count=%s" % c_open)
        chk("★★ E3 面板已关时，晚到的结果被丢弃（计数不变）",
            c_open == c_late and "7" in c_late,
            "open=%s late=%s" % (c_open, c_late))
    else:
        fails.append("E 场景未跑起来（E2_count_open 缺失）")
        print("  \u2716 E 场景未跑起来")

    return fails


def main():
    raw = "--raw" in sys.argv
    idx = build_probe()
    kv = run_probe(idx)

    print("探针原始结果：")
    for k, v in kv.items():
        print("  %-26s %s" % (k, v))

    if raw:
        return 0

    fails = assert_results(kv)

    print("")
    if fails:
        print("失败 %d 项：" % len(fails))
        for f in fails:
            print("  - " + f)
        return 1
    print("优化列表虚拟滚动端到端：全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
