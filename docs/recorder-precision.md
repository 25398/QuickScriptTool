# 录制 / 回放精度（3D 游戏偏移问题）

> 面向问题：键鼠录制回放到 3D 游戏（Minecraft、UE/Unity FPS 等）里「镜头/落点总有偏移，
> 每次跑的位置都不一样」。本文记录成因分类、本项目的对策、以及**剩余未解项**。

## 1. 现象与判读入口

用户报告：录制 → 回放，3D 游戏里视角与最终位置对不上；换别的脚本工具也一样。
「换工具也一样」是重要线索 —— 说明这不是本软件独有 bug，而是**输入注入这一层**的固有难点。

排查第一步**不要猜**，先看录制结束时的两行日志。**这些行会自动落盘，不必开任何窗口**：

| 日志位置 | 说明 |
|---|---|
| `AppDir()\recorder_diag.log` | 首选。与 `findimage_diag.log` 同级（安装默认目录是 `%LOCALAPPDATA%\QuickScriptTool`） |
| `%LOCALAPPDATA%\QuickScriptTool\recorder_diag.log` | 装到 `Program Files` 等只读目录时自动回退到这里 |
| 「宏调试信息输出窗口」 | 窗口存在时同时显示；⚠ 浮窗只有 钉/最小化/关闭，**没有导出按钮**，别指望从窗口复制 |

写盘由设置项「自动输出关键信息」（`autoOutputKeyFunctionDebug`，默认开）控制。
文件超过 256KB 自动保留尾部（按**整行**切，不会出现半行乱码）。

> ⚠ **历史坑**：这三行原先要求「宏调试窗口已打开」（`KeyFunctionDebugActive()`），
> 而录制侧还额外要求 `enableDebugOutputWindow` —— 与回放侧判据不一致，
> 会出现「录制侧三行全无、回放侧却有」，让人误以为录制没问题。
> 现统一为只看 `autoOutputKeyFunctionDebug`，且**保真结论一律落盘**。
> 详细统计（`[时间轴统计]` 等）仍只进窗口，不写文件。

```
[录制结束] 时长=… 动作=… | 键↓… | 鼠↓… | 绝对移动… 相对移动… | 通道切换 绝对↔相对 0/0
[鼠标报告] 估计周期=1000µs(≈1000Hz) 重建时间戳=3 过短间隔=0（周期偏大=后台Raw Input被限流；重建>0=队列积压）
```

- `估计周期` 是**相对位移时间轴的刻度**。它若明显大于鼠标标称周期（1000Hz 鼠标却报 8000µs），
  说明采集端没拿到真实节奏（后台 Raw Input 被限流 / 队列积压），录制时间轴本身就是粗的。
- `重建时间戳 > 0` 说明有包被判定为积压、时间戳是重建出来的，不是真实 QPC。
- `通道切换 绝对↔相对` **非 0** 说明录制中途换过采集通道（例如走位途中开了背包），
  切换处前后各 ~60ms 的移动可能被记成幻影相对位移 —— 日志里会紧跟一条 `[采集通道]` 说明。

回放结束后（同一窗口 / 同一日志文件）还有一行**保真度**结论：

```
[回放保真] 相对位移 请求=(1204,-337)/183包 注入=(1204,-337)/183事件 ⇒ 位移一致（偏差在目标侧） | 时间轴 预期=4210ms 实际=4235ms
```

| 读数 | 含义 | 下一步 |
|------|------|--------|
| `请求` = `注入` | 输入层忠实：引擎请求多少位移，就注入多少 | 落点偏差来自目标侧（帧边界/游戏内非线性），见 §5；别再查注入层 |
| ⚠ `请求` ≠ `注入` | 注入层改动了位移 | 先看同行 `split=`（加速未关时会拆包）、`fail=`（注入失败） |
| `注入=未统计` | 该模式**不经 SendInput 计数器**（窗口模式软输入 / CDP） | **别按「不一致」处理** —— 见下方「两种误报」 |
| `注入事件` ≫ `请求包数` | 拆包生效（加速未真正关闭） | 该环境下每个原始包被拆成多个 ≤4 计数的小包，报告次数变多 |
| `实际 ≫ 预期`（>1.2 倍且差 >20ms） | **回放阶段被拖慢** | 看低性能模式是否开启、机器是否卡顿；高轮询率录制（≥4kHz）时每包预算只有 125~250µs，容易被拖成慢放 |

> ### ⚠ 两种「看起来不一致、其实正常」的情况（都已在实现里排除）
>
> 1. **窗口模式软输入 / CDP**：注入侧统计只覆盖 SendInput 路径（`MouseInputRouter`）。
>    窗口模式下若走 `PostMessage`/软输入或 CDP，位移不经该计数器，会拿到 0。
>    ⚠ 判据里 `PreferHardwareInput()` 的 `if (FakeFocusActive()) return false;` 排在游戏判据
>    **之前** ⇒ **假焦点注入成功的目标（MC 这类）正是走软输入**。此时输出 `注入=未统计` 而非告警。
> 2. **脚本含循环 / 分支**：`请求` 用的是**本轮实际执行到**的位移计数（`reqRel*`），不是
>    `SumRelativeMoves(actions)` 的静态条数 —— `ScriptIsTimedInputSequence` 明确把
>    `Loop`/`Goto` 算作时间轴脚本，静态条数与执行次数不等。两者不同时会在行内附一句
>    `（脚本静态 N 包，本轮执行 M 次）`，而不是报不一致。
>
> 一句话：**`请求` 是「本轮执行了多少」，不是「脚本里写了多少」。**

> 等待调度器本身对短间隔是**整段自旋**（≤1.5ms 走 `kSpinRemainUs` 内的紧自旋，
> 见 `input_timeline_scheduler.cpp`），所以「预期/实际」若接近，就说明回放节奏没被等待实现拉长。

## 2. 成因分类（按可修性排序）

| # | 成因 | 是否可修 | 现状 |
|---|------|----------|------|
| A | **时间轴刻度错误**：报告周期模型写死，高轮询率鼠标的录制被整体拉伸 | ✅ 已修（本次） | 见 §3 |
| B | 系统指针加速（Enhance pointer precision）作用在回放的相对位移上 | ✅ 已处理 | `MouseBallisticsGuard`（阈值/速度归零）+ 加速未关时亚阈值拆包 `AppendSubThresholdRelativeSteps` |
| C | 系统把多次相对移动**合并**成一个报文 | ✅ 已处理 | `MOUSEEVENTF_MOVE_NOCOALESCE`（`input/mouse_input_backend.cpp`） |
| D | **采集端被限流**：Windows 11 把*后台* Raw Input 接收方限流到 ~125Hz，报文被合并 | ⚠ 部分 | 见 §5；前台窗口不受限 |
| E | **帧边界与游戏内积分**：回放时序与游戏帧边界不可能逐帧重合，若游戏对输入有非线性处理（钳制/平滑/固定步长），累计位移就会漂 | ❌ 输入层无法根治 | 见 §5 |
| F | 驱动级保真：`SendInput` 永远走「系统指针」路径，不是真正的 HID 报文 | ⚠ 可选 | 项目已带 Interception / `driver/qst_vhid`，见 §5 |
| G | **采集通道判定错误**：Auto 模式在「抓取态↔菜单态」切换时，粘滞窗口内的移动被记成**幻影相对位移** | ✅ 已修（本次） | 粘滞窗口 250ms → 60ms，见 §3.2 |

## 3. 本次修复：报告周期模型（成因 A）

**缺陷**：两处硬编码把 ≥2kHz 鼠标的时间轴拉伸。

1. `recorder.cpp` 的 EMA 下限写死 1000µs，且只在 `[500, 20000]µs` 内学习
   ⇒ 4kHz(250µs) 学不到真实周期 → 每个包的时间戳被改写成 `prev + 1000µs`（**4 倍拉伸**）；
2. `recorder_timeline.cpp` 只把 `[2000, 16000]µs` 认作「健康间隔」，其余回退 8000µs，
   并把 `< 500µs` 的间隔**全部**改写成回退值
   ⇒ 上例在转换阶段再被拉到 8000µs（**累计 32 倍**）。

为什么这会表现为「位置不一样」：3D 游戏里「转视角 + 走路」的位移与时间强耦合 ——
同样的位移，转得快慢不同，行走轨迹就不同。时间轴被拉伸 32 倍，等于把「边转边走」录成了
「原地慢慢转」，落点当然对不上；而且不同轮询率/不同机器的拉伸倍数不同，看起来就是
「每次都不一样」。

**修法**：新增纯逻辑模型 `src/recorder_report_interval.h`（`qst_recorder::ReportIntervalModel`）：

- 用**中位数**估计报告周期（对少量积压样本稳健），可观测下限 **125µs（8kHz）**；
- 判定「积压 / 被合并」的阈值**两段式**，刻意保守：
  - 估计周期 > 500µs（常规轮询率 / 后台被限流 / 手写宏）⇒ **沿用历史阈值 500µs**，
    保证手写宏与旧录制行为**完全不变**；
  - 估计周期 ≤ 500µs（已实测证实是 2kHz 以上鼠标）⇒ 阈值收缩到 `周期/4`，
    且**绝不高于周期本身**，否则真实报文会被当成积压、时间轴被整体拉伸；
- 样本 < 4 个时退回保守值 8000µs（= Win11 后台限流的真实周期），避免短录制抖动；
- 录制侧（`EmitRelativeMoveEvent`）与转换侧（`RepairCompressedRelativeGaps`）**共用同一个模型**，
  保证两段不会互相打架。

行为对照：

| 鼠标 | 真实间隔 | 旧：录制侧 | 旧：转换侧 | 新 |
|------|----------|-----------|-----------|-----|
| 1kHz | 1000µs | 1000µs ✔ | 1000µs ✔ | 1000µs ✔ |
| 4kHz | 250µs | 1000µs（4×） | 8000µs（32×） | 250µs ✔ |
| 8kHz | 125µs | 1000µs（8×） | 8000µs（64×） | 125µs ✔ |
| 后台录制(Win11 限流) | ~8000µs | 8000µs ✔ | 8000µs ✔ | 8000µs ✔ |
| 手写宏（10ms 间隔夹 1ms 等待） | — | 保留 1ms ✔ | 保留 1ms ✔ | 保留 1ms ✔ |
| 队列积压(10µs) | — | 重建 ✔ | 重建 ✔ | 重建 ✔ |

> ⚠ **旧录制文件不会自动变好**：拉伸是在录制/保存时写进 `Wait` 里的。
> 修复后需要**重新录一遍**；已有脚本里被写死的 8000µs 间隔会被新模型当作真实周期保留。

**自检**：`RecorderSelfTest` 新增 4 例（`rel_report_interval_high_polling` /
`rel_report_interval_compressed_burst` / `rel_report_interval_background_cap` /
`high_polling_rel_gaps_preserved`）。
A/B：把判据换回旧行为（固定 500µs 阈值 + 固定回退 8000µs）→ **4 例全红、exit 4**；恢复后 65/65、exit 0。

## 3.2 采集通道判定（成因 G）

Auto 模式下「此刻按相对还是绝对采集」由 `EvaluateRelativeCapture()`（纯逻辑，`recorder.h`）决定：
相对捕获生效（光标隐藏 / 被 `ClipCursor` 裁剪）就按相对，否则按绝对；**离开相对态后还有一个粘滞窗口**防抖。

**缺陷**：粘滞窗口历史值 **250ms** 太长。用户在抓取态按 `E` 开背包（光标变可见、游戏改为读光标位置）后，
这 250ms 内的真实鼠标移动会被记成**相对镜头位移**，而真正该录的光标移动被丢弃：

- 回放时游戏若仍处于抓取态 ⇒ 注入出**幻影镜头旋转**（镜头多转一点）；
- 回放时游戏若处于菜单态 ⇒ 光标移动缺失 ⇒ 点击落点错。

两者都随回放时序变化 ⇒ 「每次位置都不一样」。抖动是帧级的（~16ms），60ms 足够跨越，故取 **60ms**。

**同时加了证据**：录制结束时打印通道切换次数，并逐条打印切换时刻（最多 8 条）：

```
[录制结束] … | 通道切换 绝对↔相对 1/1
[采集通道] 录制期间在「绝对坐标」与「相对位移」之间切换过 —— 切换处前后各 ~60ms 内的移动可能被记成幻影相对位移…
```

**自检**：`relative_capture_decision`（强制模式 / 防抖 / 刻意切换 / 时间戳回绕）。
A/B：把粘滞窗口改回 250ms → 该例变红、exit 1；恢复 60ms → 67/67、exit 0。

## 3.3 持久化层：亚毫秒间隔不得被毫秒化

**结论：这一层是干净的**（审计确认，非推测），但原先没有断言守着，现已补上。

链路与依据：

| 环节 | 实现 | 是否丢精度 |
|------|------|-----------|
| 落盘 | `script_io.cpp` 写 `"timingUs": <整数>`；writer 在 `script_io.cpp:489` 设了 `max_digits10`(17 位) | 否 |
| 解析 | nlohmann::json（支持整数与指数写法），`GetNumber` 取 double 后转 `uint64_t` | 否（µs 量级远小于 2^53） |
| 换算 | `SecondsToUs` 用 **long double** × 1e6 再 `llround` | 否 |
| 编译 | `ActionStepUs` **优先 `timingUs`**，否则才由 `duration` 换算；`CompileInputTimeline` 累加整数 µs | 否 |
| UI | `ui/` 全仓**不出现** `timingUs`（不会二次写盘时抹掉） | — |

因此「保存/加载把 125µs 变成 0ms」这条路**不成立**，不必再查。

⚠ 但注意这条链有**两条独立通路**：`timingUs`（整数微秒）与 `duration`（17 位 double）。
任意一条单独就足以还原 125µs —— 所以**只断言「时间轴总长」是抓不到回归的**
（A/B-1 实测：writer 毫秒化后 `timeline=25000/25000` 依然正确，红在文件内容与逐条值）。

**自检**：`ScriptIoSelfTest` 新增 `sub_ms_timeline_survives_io` —— 200 包 × 125µs 的
「Wait + moveMouseRelative」序列，断言 5 件事：文件里有 `"timingUs": 125`、逐条间隔/位移不变、
时间轴总长精确 `= 25000µs`、位移总量 `=(600,-200)/200`、以及**清空 timingUs 后的退化路径**仍为 `25000µs`。

**A/B（两条独立通路各打一次）**：
- writer 把 `timingUs` 按毫秒取整 → `sub_ms_timeline_survives_io` 红（`file=0 actions=0`）、exit 2；
- `SecondsToUs` 改成毫秒取整 → 红（`legacy=0`）、exit 1，其余 58 例全绿（无误伤）。
- 恢复后 59/59、exit 0。

## 4. 参考的开源实现与可借鉴点

| 项目 | 做法 | 可借鉴点 |
|------|------|----------|
| [oblitum/Interception](https://github.com/oblitum/Interception) | 内核过滤驱动 + 用户态库，直接往设备栈里插**报文** | 唯一能同时绕开「SendInput 合并」与「系统指针加速」的路子；本项目已带 `interception.dll/.sys` |
| [RawAccel](https://github.com/a1xd/rawaccel) | 内核驱动改 HID 报告流（鼠标加速曲线） | 说明「改的是报告流，不是光标位置」；同理，回放高保真必须在报告层 |
| [MouseTester](https://github.com/microe1/MouseTester) / [PC-Optimization-Hub 的鼠标 FAQ](https://github.com/kvnloo/PC-Optimization-Hub/blob/main/content/peripherals/mouse%20faq.md) | 用 Raw Input 测轮询率；记录了 **Win11 对后台 Raw Input 接收方限流到 ~125Hz 且合并报文** | 解释了本项目为何以 8ms 作保守回退；也说明**采集端要前台**才拿得到全速率 |
| [taojy123/KeymouseGo](https://github.com/taojy123/KeymouseGo) | Python 键鼠录制回放（低层钩子 + `SendInput`） | 反面参照：纯 `SendInput` + 墙钟 `sleep` 的方案在 3D 游戏里普遍「有偏移」——与用户「换别的脚本也一样」吻合 |
| [llorella/use-windows](https://github.com/llorella/use-windows) | `WH_*_LL` 钩子 + 桌面复制，事件时间戳**对齐视频帧** | 帧对齐的时间戳基准；对「按帧回放」有参考价值 |
| TAS 工具（BizHawk / libTAS / Hourglass） | 逐帧记录**输入状态**而非墙钟事件 | 帧同步注入思路：把 delta 按游戏帧切分，而不是按墙钟切分 |

## 5. 剩余未解项（明确记录，避免下次重复分析）

1. **成因 E（帧边界 / 游戏内非线性）无法在输入层根治。**
   只要游戏对输入做了钳制、平滑或固定步长积分，输入流的**切分方式**就会影响结果。
   可行方向：按目标帧率把相对位移**聚合成每帧一个报文**（TAS 思路），
   但需要先能测到目标的帧节奏（`Present`/`SwapBuffers` 或 DWM 帧统计），成本较高，未做。
2. **成因 D（Win11 后台 Raw Input 限流）**：目前只是「如实记录被限流后的时间轴」。
   要拿全速率需要采集窗在前台；或改走驱动级采集（与 3 同一套）。
3. **成因 F（驱动级回放）**：项目已具备 Interception / `driver/qst_vhid`，
   但**默认包不含 `.sys`**（设置里单独下载）。相对位移走虚拟 HID 可获得真实报文边界，
   是「3D 游戏完美回放」的下一步，需评估反作弊与驱动安装成本。
4. **验收手段仍是人工**：`RecorderSelfTest` 只覆盖纯逻辑；「回放落点一致」需要在目标游戏里实测。
   建议验收流程：同一段录制跑 3 次，比对终点坐标/视角；同时看两行日志 ——
   `[鼠标报告]` 的估计周期是否等于鼠标标称周期、`[回放保真]` 的录制/注入位移是否一致。
   这两行能直接判定偏差是**输入层**还是**目标侧**，避免在错误的一侧反复排查。

## 6. 改动文件

- 新增 `src/recorder_report_interval.h`（纯逻辑模型）
- `src/recorder.cpp`：`EmitRelativeMoveEvent` 改用模型；新增诊断计数
- `src/recorder.h`：`RecordingDebugStats` 增 3 个字段
- `src/recorder_timeline.cpp`：`RepairCompressedRelativeGaps` 改自适应；新增 `SumRelativeMoves`
- `src/engine/engine_record_click.cpp`：录制结束打印 `[鼠标报告]` 诊断行
- `src/input/mouse_input_backend.{h,cpp}`：`MouseBackendStats` 增 `movedDx/movedDy`（实际注入位移）
- `src/engine/engine_script_run.cpp`：回放结束打印 `[回放保真]` 对比行（含「预期 vs 实际时长」、
  「注入未统计」分支、执行计数 `reqRel*`）
- `src/recorder_timeline.h`：新增 `MoveFidelityVerdict` / `EvaluateMoveFidelity`（未统计优先，可自检）
- `src/desktop_tools/desktop_tools.{h,cpp}`：新增 `AppendRecorderDiagLog`（诊断行落盘 + 只读目录回退）
- `src/recorder_diag_log.h`：新增（落盘裁剪的行边界纯逻辑，可自检）
- `src/engine/engine_record_click.cpp`：录制侧门控与回放侧统一，三行改走 `emitDiag`（落盘 + 窗口）
- `tools/recorder_selftest.cpp`：+5 例（另 +2 例：`recorder_diag_trim_line_start` / `move_fidelity_verdict`）
- `tools/script_io_selftest.cpp`：+1 例 `sub_ms_timeline_survives_io`（持久化层亚毫秒锁）
