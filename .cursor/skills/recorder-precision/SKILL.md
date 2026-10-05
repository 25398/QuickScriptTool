---
name: recorder-precision
description: >-
  Debug QuickScriptTool 键鼠录制/回放精度问题（3D 游戏里回放落点/视角总有偏移、
  每次结果不一样）via RecorderSelfTest.exe。Use when: 回放和录制不一致、相对位移
  时间轴被拉伸、高轮询率鼠标（2kHz/4kHz/8kHz）、后台 Raw Input 被限流、
  RepairCompressedRelativeGaps / EmitRelativeMoveEvent / 报告周期模型相关改动。
---

# 录制 / 回放精度 Debug（Agent）

总索引：[module-selftest](../module-selftest/SKILL.md) · 成因分析：[docs/recorder-precision.md](../../../docs/recorder-precision.md)

## 一句话怎么用

```text
1) 先看录制日志的 [鼠标报告] 行 → 2) 判断是「时间轴刻度」还是「帧边界」问题
→ 3) 改 recorder*.cpp / recorder_report_interval.h → 4) 编 RecorderSelfTest → 5) --json 全绿
```

## 构建 / 运行

```bash
cmake --build build --config Release --target RecorderSelfTest
./build/Release/QstRecorderLogicTest.exe --json
```

> 产物名是 `QstRecorderLogicTest.exe`（避开 `Recorder*`+SendInput 的启发式指纹，360 HEUR 会误删）。
> 该 target **不要链 `script_core_common`**（含 SendInput）。

## 日志在哪（先看这里，别去开窗口）

**这三行自动落盘，不需要开任何窗口**：

```
AppDir()\recorder_diag.log                              ← 首选（安装默认 %LOCALAPPDATA%\QuickScriptTool）
%LOCALAPPDATA%\QuickScriptTool\recorder_diag.log        ← 装到 Program Files 等只读目录时的自动回退
```

由设置项「自动输出关键信息」（`autoOutputKeyFunctionDebug`，默认开）控制；
超过 256KB 保留尾部、按**整行**切。窗口存在时同时显示，但 ⚠ 调试浮窗只有
钉/最小化/关闭 —— **没有导出按钮**，别让用户从窗口里手选。

> ⚠ **历史坑（已修）**：这三行原先要求「宏调试窗口已打开」（`KeyFunctionDebugActive()`），
> 录制侧还额外要求 `enableDebugOutputWindow` ⇒ 与回放侧判据不一致，会出现
> 「录制侧三行全无、回放侧却有」，让人误判「录制没问题」。
> 现统一为只看 `autoOutputKeyFunctionDebug`，保真结论一律落盘；
> 详细统计（`[时间轴统计]` 等）仍只进窗口，不写文件（量太大）。
> **改这几行的门控时，录制侧与回放侧必须同判据。**

## 判读第一步：`[鼠标报告]` 日志行

录制结束时：

```
[鼠标报告] 估计周期=1000µs(≈1000Hz) 重建时间戳=3 过短间隔=0（周期偏大=后台Raw Input被限流；重建>0=队列积压）
```

| 读数 | 含义 | 下一步 |
|------|------|--------|
| 估计周期 ≈ 鼠标标称周期 | 采集正常 | 偏移来自帧边界/游戏内非线性（输入层修不了，见 docs §5） |
| 估计周期 ≫ 标称（如 1000Hz 报 8000µs） | 后台 Raw Input 被系统限流/合并（Win11 把后台接收方限到 ~125Hz） | 让目标窗口在前台录制；或走驱动级采集 |
| 重建时间戳 明显 > 0 | 有包被判定为积压，时间戳是重建的 | 查采集线程是否被抢占（截图/杀软/磁盘 IO） |
| 过短间隔 很多但重建为 0 | 真实高轮询率包，时间轴保真 | 正常 |
| `通道切换 绝对↔相对` ≠ 0 | 录制中途换过采集通道（如走位途中开背包） | 切换处前后 ~60ms 可能是幻影相对位移；日志紧跟 `[采集通道]` 说明 |

## 采集通道判定（Auto 模式）

`EvaluateRelativeCapture()`（纯逻辑，`src/recorder.h`）+ `kAutoRelativeStickyUs`：

- 相对捕获生效（光标隐藏 / 被 `ClipCursor` 裁剪）⇒ 按相对；否则按绝对；
- 离开相对态后有**粘滞窗口**防抖 —— **必须短**（现为 60ms）。历史值 250ms 会让
  「抓取态 → 开背包（游戏改为读光标位置）」这 250ms 内的移动被记成幻影相对位移，
  回放时要么多转镜头、要么丢失光标移动，且随回放时序变化 ⇒「每次位置都不一样」。
- ⚠ 改这个常量必须跑 `relative_capture_decision`；A/B：改回 250ms 该例应变红。

## 判读第二步：`[回放保真]` 日志行

回放结束后（同一窗口）：

```
[回放保真] 相对位移 请求=(1204,-337)/183包 注入=(1204,-337)/183事件 ⇒ 位移一致（偏差在目标侧） | 时间轴 预期=4210ms 实际=4235ms
```

**这一行决定你该往哪一侧查**：

| 读数 | 结论 | 下一步 |
|------|------|--------|
| `请求` = `注入` | 输入层忠实 | 偏差在**目标侧**（帧边界 / 游戏内非线性），去 docs §5，别再动注入层 |
| ⚠ `请求` ≠ `注入` | 注入层改动了位移 | 看同行 `split=`（加速未关→拆包）、`fail=`（注入失败） |
| `注入=未统计` | 该模式不经 SendInput 计数器 | **别按「不一致」处理**，见下方两条 |
| `注入事件` ≫ `请求包数` | 拆包生效 | 该环境加速没真正关掉，每个原包被拆成多个 ≤4 计数小包 |
| `实际 ≫ 预期`（>1.2× 且差 >20ms） | **回放阶段被拖慢** | 查低性能模式/机器卡顿；≥4kHz 录制每包预算仅 125~250µs，最容易被拖成慢放 |

> ⚠ **两条「看起来不一致、其实正常」，改这行前必读**（都已在实现里排除）：
> 1. **窗口模式软输入 / CDP**：注入侧只统计 SendInput（`MouseInputRouter`）。
>    窗口模式走 `PostMessage`/软输入或 CDP 时位移不经它 ⇒ 会拿到 0。
>    **`PreferHardwareInput()` 里 `if (FakeFocusActive()) return false;` 排在游戏判据之前**
>    ⇒ **假焦点注入成功的目标（MC 这类）正是走软输入**，最容易踩。
> 2. **脚本含 Loop / Goto**：`请求` 用**本轮实际执行到**的计数（`reqRel*`），
>    不是 `SumRelativeMoves(actions)` 的静态条数（`ScriptIsTimedInputSequence` 把
>    Loop/Goto 算作时间轴脚本）。两者不同时行内会附 `（脚本静态 N 包，本轮执行 M 次）`。
>
> 一句话：**`请求` 是「本轮执行了多少」，不是「脚本里写了多少」。**

### ⚠ 这一行**不覆盖「后台窗口模式 + 软输入」**（判据直接失效，别拿它当结论）

`注入=未统计` 是**故意**的中性读数，但它对应的场景恰恰是问题最集中的场景。
以 Minecraft + 假焦点 + `输入策略=softMessage` 为例，`注入=未统计`，
而 `时间轴 预期=13452ms 实际=12570ms` 看起来「正常」，实际体验是**一卡一卡**：

| 现象 | 机理 |
|------|------|
| 一大堆 `等待 0.008秒` + `late=2000~29000us` | 时间轴节拍是 **8ms**（≈125Hz），每拍要投递 1~2 条 `WM_MOUSEMOVE`；PostMessage 在游戏线程忙时被排队 ⇒ Tick 漂移 → 下一拍追帧 → 表现为卡顿 |
| 「预期 ≈ 实际」却说没被拖慢 | 阈值是 `>1.2×` **且** 差 `>20ms`（`expectedTimelineUs*6/5`）。Tick 抖动是**在一条准确时间轴上左右漂**，总长不变 ⇒ **判据看不见**。要判抖动得看 `[时间轴统计]` 的 `p95/max` 与**逐行 `late=`** |
| `[时间轴统计] SendInput ok=0 fail=0` | 整个回放**一条 SendInput 都没发**（全走软输入投递），相对位移路径就是 `PreferHardwareInput()==false` 的软输入分支 |
| 只有相对移动被降级 | **绝对移动**（`MoveMouseClient`）有 win32 专用后台路径（`background_window_input.cpp`，`SendInput` 绝对模式）；**相对移动没有对应实现**，且 `!UsesInProcFakeFocusSoftInput()` 时还要多打一条 `WM_MOUSEMOVE` |
| 录制时为何更平滑 | 录制回放的是**物理鼠标报文流**且目标当时在**前台**；回放是后台投递 + 真实用户电脑的调度竞争（日志里 `late=29ms` 说明有别的进程在抢） |

**排查顺序（后台窗口模式 + 相对移动）**：

1. 先确认 `late` 是**孤立尖峰还是成片**；成片 = 每拍投递超预算。
   **再看 `rebase=`** —— 它比 `p95` 更能反映「时间轴被反复重基准」（落点漂移的直接来源）；
2. `SendInput ok=0` **不是故障**（软输入才不抢键鼠，是设计意图），别再顺着它查判据链 —— 见下节；
3. **真根因**：相对移动**每拍一次** `PrepareSoftInput()`（含 `EnumChildWindows` 全树）⇒ 见下节修法。

### ✅ 已证伪的旧结论：**不是**「配置类名为空 ⇒ `UsesFakeFocus` 漏判」

> 2026-09-20 我依据「`class=GLFW30` 但 `SendInput ok=0`」推断判据断裂，**2026-09-21 实测证伪**，
> 留在这里免得住回查。别再用读代码的方式推这条判据链 —— **写成断言跑一遍**。

实测（`lca_bg_unknown_game` 现在含 GLFW30/SDL_app/UnrealWindow 三条**整链**断言）：

```
GLFW30 作为 windowClassName：lca=0 needs=1 uses=1   →  判定链完全正确
```

`UsesFakeFocus` 对 MC 就是 **true**，假焦点该装、也确实装了。
关键：`PrefersLcaBackgroundMessages` 里 `if (NeedsFakeFocusInjection(config, hwnd)) return false;`
**排在「未知游戏兜底」之前**，而 `NeedsFakeFocusInjection` 用 `LooksLikeInjectRequiredGameClass`
（含 GLFW/SDL/Unity/Unreal）⇒ 先返回 true，兜底根本没轮到。

**所以 `SendInput ok=0` 是设计意图，不是 bug**：软输入才不抢键鼠。
（`UsesFakeFocus(config, hwnd)` 的回读 HWND 仍然要保留 —— 它救的是**真**类名为空的场景，
只是**不是 MC 卡顿的原因**。守 `fake_focus_uses_bound_hwnd_class`。）

### ⚠ 真根因：「后台 + 软输入」卡顿 = `PrepareSoftInput` 每拍一次全树枚举

2026-09-21 实测确认（MC / GLFW30 / 后台 / 779 个相对移动包）：

| 项 | 实情 |
|---|---|
| 相对移动路径 | **每拍一次** `PrepareSoftInput()`，内含 `RefreshInputBinding` → **`EnumChildWindows` 全树**找 RenderWidget |
| 8ms 一拍 | 779 包 × 全树枚举，**喂不起** ⇒ `late` 成片 >1ms、`p95=4141us max=23874us`、**`rebase=27`** |
| 后果 | 时间轴被反复重基准 ⇒ **卡顿 + 落点漂移**（`rebase` 次数是比 `p95` 更刺眼的指标） |
| 附加坏点 | 4 组调用点**各把 `PrepareSoftInput` 调了两遍**（CDP guard 一次 + 后面又一次）—— 绝对移动每步付两遍 |

**第一轮修复后的实测（用户第二轮日志，同一脚本）**：

| 指标 | 修前 | 修后 | 判读 |
|---|---|---|---|
| `late>1ms` | 66 | **50** | ↓ |
| `p95` | 4141µs | **1505µs** | ↓ **明显**（成片超预算被削掉） |
| `rebase` | 27 | **17** | ↓（落点漂移减轻） |
| `max` | 23874µs | 26636µs | **没降** |

⇒ **成片 `late` 是 `PrepareSoftInput` 造成的（已修，p95/rebase 证明）**；
剩下的 `max` 是**孤立尖峰**，且新日志显示它们**紧跟在键盘动作之后** ——
即 `PostKeyToTarget` 那条**漏改的**全树枚举（第一轮只改了相对移动）。
第二轮把它一并改成快路径。这是「指标改善但主观仍卡」的原因，不是「修了没用」。

**修法**：

1. 删掉 4 组里的**重复**调用（`MoveMouseClient` / `PostMouseButtonAtClient` /
   `PostMouseClickAtClient` / `PostScrollWheelAtClient`；保留 CDP guard 内那次）。
2. **所有「每拍一次」的输入路径**统一走 `PrepareSoftInputFast`，判据抽成**纯函数**
   `CanUseSoftInputFastPathClass`（`window_mode_types.h`）：
   ① 顶层窗仍有效 ② 绑定**就是顶层自身** ③ 客户区几何未变
   ④ **顶层窗类名与缓存一致**（HWND 会被系统复用 —— 旧窗关了开新窗可能拿到同值句柄，
   且尺寸恰好相同；那时 ①②③ 全成立，会拿旧绑定投递到**错误目标**且不报错。
   一次 `GetClassNameW` 就能排除）。任一不成立 ⇒ 退回完整路径并按**刷新后**的目标重记缓存。
   失效点 `RefreshTarget()` / `EndRun()`。守 `soft_input_fast_path`。
3. **键盘（`PostKeyToTarget`）此前漏改**（2026-09-21 第二轮补上）：它同样对**每一次按键**
   调完整 `PrepareSoftInput` ⇒ `EnumChildWindows` 全树。现场表现为
   **大尖峰集中在键盘动作之后**（软键态写完还要等一次全树枚举才轮到下一拍）。
   现在是「相对/绝对移动 + 鼠标按放 + 点击 + 滚轮 + 键盘」全部快路径；
   `SendQuickInputToTarget` 走完整路径（非逐拍动作，自带一次 `RefreshInputBinding`）。

### ⚠ 判读「极短等待几乎每行都带 `late=`」——方向别搞反

现场：`[时间轴统计]` 里 `p95` 在改善、但 `max` 不降，且**每一行 `等待 0.000~0.003 秒`
都跟着一笔显著 `late=`**（如 `late=925us/1401us/2117us/4596us`）。看着像「等待实现不准」。

**实际因果相反**：`WaitDeltaUs` 用的是**累计绝对 deadline**（`origin + elapsedUs`）。
当**注入本身**（全树枚举 / 跨进程投递）耗掉了这段间隔的预算，等到检查时 deadline
**已经过去** ⇒ 它只能立刻返回，并把这笔超支**如实记进 `late`**。
所以 `late` 是**受害者指标**，真正的开销在**上一拍的注入**里。

这些 0.000~0.003s 的等待本身来自 `RepairCompressedRelativeGaps`：相邻两个相对移动之间
会插入一条等待，值取**报告周期中位数**。高轮询鼠标（1kHz→1000µs、2kHz→500µs）
⇒ 注入必须挤进亚毫秒预算，否则必然成片 `late`。

> ⛔ 因此**不要**因为「短等待全带 late」去改 `input_timeline_scheduler` 的自旋 / 定时器切片
> —— 那一层没问题（≤1.5ms 是整段紧自旋，见下）。
> 要查的是**每拍注入的总耗时**。守 `timeline_injection_overrun_eats_short_wait`。

### ✅ 「回放不如改之前精细」= 假指控，已被数值证伪（2026-09-21 第十一轮）

用户看到日志里**满屏 `等待 0.008秒`** 会得出「时间轴反而变粗了」。**方向是反的。**
`RepairCompressedRelativeGaps` 有**两段循环**，必须分开算（脚本：
`.workbuddy-ai/tmp/verify_gap_math.py`、`verify2.py`、`verify3.py`）：

| median 区间 | 旧 target | 新 target | 方向 |
|---|---|---|---|
| **≥2000µs**（日志里 0.008s） | healthy 区间中位数 = 8000 | 全样本中位数 = 8000 | **两段循环完全等价** ⇒ 这套等待**录制时就有**，不是改动引入 |
| **<500µs**（真高轮询率） | 8000（拉伸 32 倍） | 真实周期 | 新**更精细** |
| **(500,2000)** | 8000 | median | 新 target **更小** ⇒ 空白更短 ⇒ 新**更精细** |

**没有任何一个区间是「新的更粗」** ⇒ 这条改动不可能造成精细度下降。
`median≥2000` 时两实现数学等价，是**最容易误判**的一档。

⇒ 再遇到「不如以前精细」，**先跑上面三个脚本把 median 落档**，别去翻 `RepairCompressedRelativeGaps`。

**真差异在宿主**：同一脚本两轮 `p95=5058µs/rebase=34` vs `p95=157µs/rebase=0`，
差 32 倍 ⇒ 环境噪声，不是代码属性。**先看两轮统计是否一致，不一致就别当回归查。**

### 📌 「时间轴是粗的」源头可能在**录制侧**，不只在回放侧

唯一能确凿读出「精细度」的行是 **`[鼠标报告] 估计周期=?µs(≈?Hz) 重建时间戳=? 过短间隔=?`**
（`engine_record_click.cpp:292`），**自动落盘**到 `AppDir()\recorder_diag.log`，
由 `autoOutputKeyFunctionDebug` 门控，**不必开调试窗**。

`估计周期≈8000µs(≈125Hz)` ⇒ **录制时后台 Raw Input 已被 Win11 限流**
⇒ 时间轴本身就是 8ms 刻度，回放再怎么调也细不了 ⇒ **必须前台录制**。
**这条优先于任何回放侧优化** —— 别花力气在粗时间轴上做精细投递。

## 🎯 第一判据：「前台 vs 后台」对照 —— 先要这个，再读代码

**用户做了这个对照，一步定位到精度天花板。这是整条链路上最高价值的判据。**

| 指标 | 前台回放 | 后台回放 |
|---|---|---|
| `SendInput ok` | **881**（走真 HID） | **0**（走软输入投递） |
| `p95` | 668µs | 157 / 5058µs |
| `max` | **23498µs** | 3710 / 28446µs |
| `rebase` | 14 | 0 / 34 |

**两个决定性推论（别再推翻它们）**：

1. **前台/后台的精度差是架构性的**：前台是真 `SendInput`（原生 HID 报文），
   后台是跨进程伪造光标 + 共享内存。**不是 bug，修不了。**
2. **前台 `max=23498µs` 尖峰照样存在** ⇒ **孤立尖峰与注入层无关**，
   是宿主线程调度。**前台已经排除了跨进程投递/软光标/钩法**，所以这条是终判。

**遇到「哪里都不对」时的正确顺序**：让用户跑一次**前台对照** →
若前台好 ⇒ 问题在「后台模式固有代价」，**不要在注入层继续挖**。

## 🎯 第二判据：录制侧的 `[鼠标报告]` 一行决定精度上限

```
[鼠标报告] 估计周期=8039µs(≈124Hz) 重建时间戳=1 过短间隔=0
```

- `估计周期` 就是**时间轴刻度**。124Hz ⇒ 刻度 8ms；1000Hz ⇒ 刻度 1ms。
  **它由录制侧的 Win11 后台限流决定，与回放代码无关。**
- `重建时间戳≈0` + `过短间隔=0` ⇒ **录制干净，QPC 戳真实**，
  日志里那套 `0.008 秒` 等待就是**录制时的物理事实**，
  `RepairCompressedRelativeGaps` 只是写成显式 Wait，**没有放大**。

⇒ **要提升「精细度」唯一有效的操作是「前台录制」**，回放侧已无可做。
  拿到这行之前**不要**再怀疑 `RepairCompressedRelativeGaps`。

### 已排除、别再去查（两轮都查过了）

| 嫌疑 | 结论 | 依据 |
|------|------|------|
| 软键屏障 `WaitSoftKeyPostTurn` | **不是元凶** | ≥3ms 固定 sleep + 两次跨线程 `SendMessageTimeout`，看着很像；但 `TryInstallFakeFocus` 的 **GLFW 分支（`lite && glfwSdl`）没有** `FakeFocusSoftInput_SetPostKeyEvents(true)` ⇒ `PostKeyToTarget` 里 `UsesInProcFakeFocusSoftInput()` 为 false，**根本不会调它**；即便调了，`PostKeyEventsEnabled()` 也是 false ⇒ 直接 return false |
| 键盘路径**阻塞** | **不是元凶**（但**曾是**开销源） | `PostKeyToWindow` 走 `SendNotifyMessageW`（**异步**）+ 共享内存写，不阻塞；`PrimeWindowSoftFocus` 对 GLFW 只发异步 `WM_SETFOCUS`。**但**它前面的完整 `PrepareSoftInput` 是每键一次的全树枚举 —— 2026-09-21 已改快路径 |
| `late` **孤立**大尖峰（>10ms 单点） | **注入层修不了** | 宿主线程被系统抢占（别的前台程序/杀软）。只有**成片** `late` 才是投递超预算 |
| 「回放不如改之前精细」 | **假指控**（数值已证伪） | 见上「精细度」小节：三条 median 区间没有一条是「新的更粗」。真差异是宿主噪声（两轮 p95 差 32 倍） |
| `WM_MOUSEMOVE` 送不到 GLFW 所以卡 | **措辞误导，别顺着查** | 该串只是 `RelativeMoveRouteName()` 的**路径名**。真正起作用的是假焦点软光标（注入 DLL 内 `GetCursorPos`/RawInput 钩子），`WM_MOUSEMOVE` 对 GLFW 无效但也**不是**瓶颈 |
| 前台精度也不高 ⇒ 是我的锅 | **架构性，不是 bug** | 前台 `SendInput ok=881` 走真 HID、后台 `ok=0` 走伪造光标；差异是两个通道的本质区别 |
| 「前台更准」要不要也去优化 | **不要** | 前台已经是本产品最好路径；要提精度是**换录制方式**（前台录制拿 1ms 刻度），不是改回放 |
| 等待实现本身（自旋/切片） | **没问题** | ≤1.5ms 间隔是整段紧自旋（`input_timeline_scheduler.cpp`）。见上面「方向别搞反」 |

> 余下的**成片** `late` 只可能来自「每拍注入超预算」——先确认该路径是否都在
> `PrepareSoftInputFast` 上，再看单拍注入的真实耗时。

## 已审计、别再重复查的环节

| 环节 | 结论 | 依据 |
|------|------|------|
| 保存 / 读取 | **µs 精确，无毫秒化** | `timingUs` 写整数；writer 设 `max_digits10`(17 位)；解析用 nlohmann；`ui/` 全仓不碰 `timingUs` |
| `duration → µs` | 无损 | `SecondsToUs` 用 long double × 1e6 + `llround` |
| 时间轴编译 | 不量化 | `ActionStepUs` **优先 timingUs**；`CompileInputTimeline` 累加整数 µs |
| 优化器 | 跳过含相对移动的区段 | `recording_optimize_ops.cpp` `RangeApply::SkipRelative`，且不自动跑 |
| 等待实现 | ≤1.5ms 纯紧自旋、绝对时间轴不累积漂移 | `input_timeline_scheduler.cpp` |

⚠ **两条通路是独立的**：`timingUs`（整数）与 `duration`（17 位 double）任一都能还原 125µs。
所以**只断言「时间轴总长」抓不到回归** —— A/B 实测：writer 毫秒化后总长依然正确，
必须同时断言「文件里有 `"timingUs": 125`」+「逐条值」+「清空 timingUs 的退化路径」。

## 硬规则（改这块代码前必读）

1. **报告周期只有一个来源**：`src/recorder_report_interval.h` 的 `ReportIntervalModel`。
   录制侧（`recorder.cpp` `EmitRelativeMoveEvent`）与转换侧
   （`recorder_timeline.cpp` `RepairCompressedRelativeGaps`）**必须共用它** ——
   两边各写一套阈值就是「修了一处、另一处继续拉伸」。
2. **不要再引入固定的 µs 阈值**（曾经是「EMA 下限 1000µs」+「<500µs 判压缩」+「只认 [2000,16000] 为健康」）。
   这些常量对 ≥2kHz 鼠标必然错：4kHz 的 250µs 会被判成积压并拉伸到 8000µs（32 倍）。
   判定阈值是**两段式**（`CompressedGapThresholdUs()`）：
   - 估计周期 > 500µs ⇒ **沿用历史阈值 500µs**（手写宏「相对移动 + 1ms 等待」必须保持 1ms，
     改成按比例会把它拉到报告周期上、慢 10 倍）；
   - 估计周期 ≤ 500µs ⇒ 阈值收缩到 `周期/4`，且**绝不高于周期本身**。
   ⚠ 只改其中一段就会破坏另一类输入（这是本模块最容易踩的坑）。
3. **样本 < `kMinSamplesForMedian`(4) 时走保守回退 8000µs**，这是**故意**的：
   与旧行为一致，避免 2 个样本就下结论导致短录制抖动。别「优化」掉它。
4. **可观测范围 125µs(8kHz) ~ 20ms(50Hz)**。越界样本只计数、不入中位数 ——
   长按不动产生的 250ms 停顿不是报告周期，混进去会把中位数带偏。
5. **模型不加锁**：录制侧只有 Raw Input 线程会碰它（与 `g_lastRelStampUs` 用法一致）。
   新增调用点前先确认线程。
6. **回放侧的相对位移是另一套**：`src/input/mouse_input_backend.cpp`（`MOUSEEVENTF_MOVE_NOCOALESCE`、
   加速未关时的亚阈值拆包 `AppendSubThresholdRelativeSteps`、`PaceLocked`）。改时间轴别动它。

## 自检用例 ↔ 语义

| 用例 | 断言什么 |
|------|----------|
| `rel_report_interval_high_polling` | 8k/4k/1kHz 的 `MedianUs()` 等于真实周期且不判为压缩 |
| `rel_report_interval_compressed_burst` | 10µs 积压样本判为压缩、中位数不被污染；样本不足走回退；**手写宏 1ms 等待不被误判** |
| `rel_report_interval_background_cap` | 8ms（Win11 后台）不误判为压缩；越界样本不入统计 |
| `high_polling_rel_gaps_preserved` | **端到端**：4kHz 录制的 250µs 间隔经转换+Repair 后原样保留，总时长 < 3ms |
| `sum_relative_moves_counts_only_rel` | 保真度诊断的分子：只累加 `MoveMouseRelative` 的 x/y 与包数 |
| `relative_capture_decision` | Auto 采集判定：强制模式 / 防抖 / 刻意切换不得被粘滞吞掉 |
| `recorder_diag_trim_line_start` | 诊断日志超限裁剪只按**整行**切；无换行 / 换行在末位都全留（切半行 = 日志开头乱码） |
| `move_fidelity_verdict` | 回放保真判定：**「未统计」必须优先于「不一致」**；两侧同为 0 也不算 Match（软输入模式下会误报） |
| `sub_ms_timeline_survives_io`（ScriptIoSelfTest） | 持久化层：200×125µs 经 保存→读取→时间轴编译 后总长精确 25000µs；含「清空 timingUs 的退化路径」 |
| `micro_gap_relative_merge` | 单样本 150µs 仍按旧行为拉到 8000µs（保守回退的回归锚） |
| `repair_compressed_rel_gaps` | 8ms 基线里夹的 1µs 被拉到 8000µs，且幂等 |

**改判据必须做 A/B**：把判据换回旧行为（固定 500µs 阈值 + 固定回退 8000µs）→
上面 4 个新用例应当**全红、exit 4**；恢复后 65/65、exit 0。

> ⚠ **A/B 收尾时务必重建 `RecorderSelfTest` 本身**。构建主程序（`QstWebViewShell`）不会
> 重建该 target 的 .obj，恢复后直接跑会读到 **A/B 版的旧 exe**，看到假失败。
> 已踩过一次：恢复 60ms 后先跑了产品构建，再跑自检得 66/67，重建该 target 后才是 67/67。

---

## 加「回放期开关」的硬规则（2026-09-21 踩坑后固化）

**给回放加一个影响动作序列的开关时，必须先 `grep` 出被影响函数的全部调用点。**

踩过的坑：给「细分位移铺满窗口」加开关时，只把 `RepairCompressedRelativeGaps` 的
**6 个调用点中的 1 个**换掉 ⇒ 用户勾了开关**完全没效果**（日志里包数仍 790、
等待仍 `0.008秒`），而代码看起来是对的。

调用点分布（当时）：
- `engine_script_run.cpp`：`RunCurrentActions()` 里**有两个平行块**（397→419、506→520）、
  外加 `StartActionsWorker` 内两处 `nested`
- `engine_host_window.h`：1981、10973

**判据**：日志里「精密时间轴回放…」由 **`StartActionsWorker`（694 行起）** 输出，
而 397/506 属于 **`RunCurrentActions`（365 行起）** —— **别按行号就近判断归属**，
先确认「输出该日志的函数」是谁，再往回找动作序列是在哪准备好的。

**固化做法**：不要在每个调用点各写一遍，而是收敛成**一个统一入口**
（`PreparePlaybackTimeline(actions, spreadRelativeMoves)`），全部调用点换成它。
**改完必须复验**：`grep -rn "<被替换的函数名>(" src/ | grep -v <定义所在文件>` **应为空**。

**同时加一条自检**（`prepare_playback_timeline_applies_spread`）：关开关不改包数、
开开关必细分且 Σ位移守恒。

**并且要在日志里给出可判据**：`[时间轴统计]` 行尾加 `| spread=<前>-><后>包` ——
开关关时两数相等、开时后者明显更大。**没有这个指示符，就只能靠"猜"开关是否生效。**

### 补丁：这个项目里「一个设置项」= 4~5 处并行实现（2026-09-21 二次踩坑）

给回放加设置项时，**必须**把下面每一处都补上，漏一处就是「UI 能勾、保存提示成功、
引擎读到的却是旧值」，且**没有任何报错**：

| 环节 | 位置 |
|---|---|
| 结构体字段 | `app_settings.h`（`PlaybackSettings`） |
| 磁盘加载 | `app_settings_store.cpp` `ParseBoolField` |
| 磁盘保存 | `app_settings_store.cpp` `SaveAppSettings` |
| **UI 回显（结构体→JSON）** | `webview_bridge_backend.cpp` 手写 JSON 拼接（`lowPerformanceMode` 附近） |
| **UI 保存（JSON→结构体）** | `webview_bridge_backend.cpp` `ApplySaveSettingsJson` 里的 `GetBool(...)` 列表 |
| UI 读 | `ui/app.js` `setChk($("#<id>"), ...)` |
| UI 写 | `ui/app.js` `assignChk(out, "<key>", "<id>")` |
| UI 控件 | `ui/index.html` 的 `<span class="chk" id="<id>">` |
| AI 工具 | `agent_tools.cpp` 三处：读、schema、`setBool(...)` |

**唯一可靠的补全方法**：找一个**同类且已存在**的设置项当模板（如 `findImageGpuAccel`），
```bash
grep -c "findImageGpuAccel" <每个文件>   # 得到清单
grep -c "<新键名>"        <每个文件>   # 计数必须逐一相等
```
**计数相等才算补全。** 别靠"想起来还有哪一处"。

**另外必须在日志里给可判据**（否则用户只能猜"有没有生效"）：
`[时间轴统计]` 行尾 `| spread=<细分前>-><细分后>包` —— 两数相等 = 开关没生效。

---

## ❌ 已被数据否定的方向：细分位移铺满窗口（2026-09-21 实测）

**不要再把它当优化项。** 同一段 15.164s 宏、790 个相对包：

| 指标 | 关闭 | 开启 | |
|---|---|---|---|
| `spread` | `790->790包` | `790->2107包` | 生效 |
| `waits` | 849 | 3168 | 3.7× |
| `late>1ms` | **11**（1.3%） | **110**（3.5%） | ⬆ 差 10 倍 |
| `p95` | **0µs** | **581µs** | ⬆ |

**机理（数学必然，不是实现问题）**：
1. 每拍预算 8ms → 2ms，同样的绝对抖动占比放大 4 倍 ⇒ `late`/`p95` 必然恶化。
2. 游戏本来就 `yaw += Σdx * sens`（**按帧求和**）—— 帧内求和**本身就是该段的积分**。
   「整包 vs 细分」只是同一积分的不同划分，**不改变每帧总量**，只增加 3.7 倍出错机会。

**教训**：论证「细分能提高保真」时，先问「目标侧是不是本来就在做求和/积分」。
是 ⇒ 细分无收益，只有代价。

## ✅ 已排除：录制侧不存在「后台限流」（2026-09-22 代码核实）

**曾误判** `估计周期=8036µs(≈124Hz)` 是「Windows 11 后台 Raw Input 限流」。
**逐行核实 `src/recorder.cpp` 后确认：不是限流，就是鼠标本身的报告周期。**

证据链（`RawInputThreadMain`，`recorder.cpp:569` 起）：
1. `SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)` —— **最高非实时优先级**
2. `timeBeginPeriod(1)` —— 1ms 定时器精度
3. **专用线程**（不是 UI 线程）+ `HWND_MESSAGE` 消息窗 + `RIDEV_INPUTSINK`
4. 消息循环是**排空式**：`while (PeekMessageW(..., PM_REMOVE)) DispatchMessageW(...)`
   之后才 `MsgWaitForMultipleObjects(0, nullptr, FALSE, 1, QS_ALLINPUT)`
   ⇒ **消息一到就被处理**，不存在积压/量化

**⇒ 若鼠标真是 1000Hz，这里会看到 ~1000µs 间隔；实测 8036µs ⇒ 设备就是 ~125Hz。**
**⇒ 要更细的录制粒度只能换高轮询率鼠标，软件侧无解。**

（`过短间隔=0` 也印证：若是限流/积压，必然出现成簇的亚毫秒间隔。）
