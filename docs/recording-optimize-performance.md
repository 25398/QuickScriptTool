# 录制优化界面：打开性能剖析

> 面向「动作很多的脚本，打开优化界面很慢」这个报障。所有数字都是**实测**，
> 复现方式见文末 `--bench`。

## 一句话结论

**瓶颈是 C++ 解析，不是桥载荷。** 而且 `loadOptimizeRecording` 原先跑在 **UI 线程**上，
几百毫秒的解析会把**整个界面冻住** —— 那才是「卡」的体感来源。

三处修复（详见下文）：① 逐块重解析 → 借用已解析 DOM；② 加载挪到后台线程；
③ `visualLayout` 的整份重解析 → 复用同一份 DOM。
`LoadScriptFileData` **793.7 → 365.4 ms**，UI 全程不冻。

## 现场

- 录制文件：`键鼠录制-1791021669.json`，**5881 条动作 / 14.4 MB**（每条动作写满 ~104 个字段）
- 硬件/构建：本机 Release 构建

## 实测分解（修复前）

| 步骤 | 耗时 |
|---|---|
| `ReadAll`（读盘 + UTF-8→UTF-16） | 96.5 ms |
| 整文件 `WideObjectView`（`ToUtf8` + nlohmann 解析 14 MB） | 242.4 ms |
| `ExtractJsonActionBlocks`（5881 个块，每块一次宽串拷贝） | 37.8 ms |
| **逐块 `ParseScriptActionBlock`（每块 `ToUtf8` + nlohmann **再**解析）** | **354.6 ms** |
| **`LoadScriptFileData` 合计** | **793.7 ms** |
| 重序列化（逐条 `ScriptActionToJsonString`） | ~170 ms |
| **端到端（桥发送前的全部 C++ 成本）** | **~960 ms** |

⇒ **同一份 JSON 被 nlohmann 解析了两遍**：`LoadScriptFileData` 已经为顶层字段解析过整份文件，
随后又对 `ExtractJsonActionBlocks` 抠出来的每个块**各自解析一次**。597 ms / 793 ms 是重复劳动。

## 修复

### 1. 消除重复解析（`src/script_io.cpp` / `src/json_util.h`）

- `qst::jsonutil::WideObjectView` 增加「**借用**已解析对象」的构造（零拷贝）：
  `explicit WideObjectView(const nlohmann::json& obj)`。
- `ParseScriptActionBlock` 的函数体抽成 `ParseScriptActionBlockImpl(block, J, ...)`，
  对外保留两个薄包装：文本版 `ParseScriptActionBlock`、视图版 `ParseScriptActionBlockWithView`。
- `LoadScriptFileData` 在**同时**满足下面两条时走快路径，否则整体回退到原来的逐块解析
  （行为与旧版完全一致）：
  1. `JC`（整份文件的 DOM）有效，且 `actions` 是数组；
  2. **每个元素都是对象**，且**元素个数 == `ExtractJsonActionBlocks` 的块数**
     （`ExtractJsonActionBlocks` 只抠对象块，非对象元素会被跳过 ⇒ 下标错位）。

⚠ **`block` 文本仍要一起传**：函数体内还有 5 处**必须**依赖 `block` 原文：
3 处原文扫描（`useMode` 的引号判定 / `ParseWatchModeField` / `imageRegionX1` 存在性）
+ 2 处字符串抠数组（`imagePaths` / `imageUseVars`）。
换成视图后这些行为必须原样保留。

（原第 6 处 `ExtractNamedJsonObject(block, L"nestedWindowMode")` 其实是**块级重解析** ——
只对 `RunMacro`/`MousePlayback` 触发，所以本基线里测不出来，但宏多的脚本会按条付费。
已一并改为 `SubObjectFromParsedRoot(J, "nestedWindowMode")`：`J` 就是同一个对象的已解析
视图，dump 参数与 `GetSubObjectText` 严格一致 ⇒ 字节等价。护栏是
`ScriptIoSelfTest` 的 `nested_use_mode_window_roundtrip`（把键名改错即变红，已实测）。）

**结果**：`LoadScriptFileData` **793.7 → 508.8 ms**（−36%）。
等价性：5881 条动作**逐条序列化后逐字节相同**（`--bench` 内置该比对）。

### 2. 别冻住 UI 线程（`src/webview/qst_webview_shell.cpp`）

`HandleBridgeMessage` 是在 WebView2 的 `WebMessageReceived` 回调里**同步**执行的，
而那个回调在 **UI 线程**上 ⇒ 加载期间**整个界面卡住**，连骨架屏的 CSS 动画都停摆。

改成照 `installOcr` 的既有套路：后台线程 + `PostToJsAsync`（`WM_BRIDGE_POST_JS`）。

- `std::mutex g_optLoadMutex` **串行化**：`JsonLoadOptimizeRecording` 会写全局缓存
  `g_optWork`（`ScriptFileData`，含 `vector`），并发赋值是**数据竞争**。
- `std::atomic<uint64_t> g_optLoadSeq` **丢弃过期结果**：用户快速连开两份录制时，
  先发起的那个可能后返回，绝不能让它覆盖后发起的。
- JS 侧配套：结果回来时若 `opt-open` 已不在（用户把优化窗关了），直接丢弃 ——
  否则 `tryRevealOptMode → revealModeAfterPaint` 会对着关掉的界面调 `qst.modeReady()`，
  让壳按优化模式改窗口尺寸。

### 3. 又一处「整份重解析」：`visualLayout`（`src/script_io.cpp`）

修完 1、2 后 `LoadScriptFileData` 仍有 ~190 ms 说不清。给 `--bench` 加了「动作解析之外的
全文扫描」分段后定位到：

```
ExtractNamedJsonObject(visualLayout): 184.9 ms
```

而**这份脚本压根没有 `visualLayout` 这个键**。原因是它的实现走
`ExtractNamedJsonObject → ExtractNamedJsonObjectImpl → GetSubObjectText`，而
`GetSubObjectText` 是 **`nlohmann::json::parse(ToUtf8(整份 14.4 MB))`** —— 又一次全量解析
（+ 一次 `dump`）。**键存不存在都要先付这份钱。**

⇒ 新增 `SubObjectFromParsedRoot(root, key)`：根对象**已经解析好**时，只做 `find` + `dump` 子树。
输出与 `GetSubObjectText` **严格一致**（同样是紧凑 `dump(-1, ' ', false, error_handler_t::replace)`）。
`LoadScriptFileData` / `ParseScriptContent` 两处改为：

```cpp
data.visualLayoutJson = JC.valid()
    ? SubObjectFromParsedRoot(JC, "visualLayout")
    : ExtractNamedJsonObject(content, L"visualLayout");   // JC 不可用时回退
```

**结果**（`--bench` 自己并排打出新旧两条路）：

| | 旧 | 新 |
|---|---|---|
| `ExtractNamedJsonObject(visualLayout)` | **185.6 ms** | — |
| `find` + `dump` 已解析根 | — | **0.0 ms** |
| 全文扫描小计 | 345.6 ms | **160.0 ms** |

⚠ `0.0 ms` 是因为**该脚本没有这个键** ⇒ 新路径只是一次 `find` 未命中。有 `visualLayout`
的脚本新路径要 `dump` 子树（随子树大小，但**远小于**重解析 14.4 MB）。
**无论哪种情况，旧路径都是固定 185 ms 起** —— 这才是问题所在。
`--bench` 会打印 `(本脚本有/没有 visualLayout ⇒ …)` 把这一点写在输出里，避免 0.0 被误读成测量坏了。

## 三轮修复的累计效果

| 指标 | 修复前 | 修 ① | 修 ①+②+③ |
|---|---|---|---|
| `LoadScriptFileData` | 793.7 ms | 508.8 ms | **365.4 ms** |
| 端到端（桥发送前） | ~960 ms | ~700 ms | **~560 ms** |
| UI 是否被冻住 | **是**（骨架屏动画都停） | 否 | 否 |

⚠ 绝对值只对本机当前构建有意义，看**相对**降幅。

## 反面结论：载荷不是瓶颈（别改）

原本怀疑「桥载荷 14.4 MB 太大」。量化后**否掉了**：

| 方案 | 载荷 | JS 侧 |
|---|---|---|
| 现状（`WriteActionJson` 是**缩进美化**输出） | 14.39 MB | `JSON.parse` 23 ms |
| 列式（键名只出现一次） | **0.95 MB** | `JSON.parse` 2 ms **+ 物化 5881×54 个属性 20 ms** |

⇒ **JS 侧合计 22 ms ≈ 23 ms，打平。** 列式/投影只省 C++ 序列化与传输，却要搭上
「`displayActionName` 跨 20+ 类型读 40+ 字段，漏一个就**静默**错标签」的风险。
**结论：不动载荷。要提速得动解析。**

## 同类问题（已发现，**本次未修**）：保存路径

`webview_bridge_backend.cpp` 的 `SaveScript`（约 1676 行）在**同一个函数里**连续做两件事：

```cpp
if (!ParseEditorActionsJson(msgJson, data.actions, err)) return false;   // 1676
...
const std::wstring layout = ExtractNamedJsonObject(FromUtf8(msgJson), L"visualLayout"); // 1680
```

- `ParseEditorActionsJson` 走的是 `ExtractJsonArray` + `SplitJsonObjects` 抠字符串，
  再对每块调 **文本版** `ParseScriptActionBlock` ⇒ 与修 ① 之前的读路径同构（逐块重解析）。
- 紧接着第 1680 行又对**同一份 `msgJson`** 做一次整份重解析（修 ③ 修掉的那类）。

⚠ **未实测**（不想在保存路径上凭估算下结论）。但这两步操作与 bench 里已量化的
`逐块 ParseScriptActionBlock 224 ms` + `ExtractNamedJsonObject(visualLayout) 189 ms`
是**同一份数据、同一组操作**，量级可参考。

**为什么不顺手改**：这是**写用户数据**的路径。复用 DOM 需要把
`SplitJsonObjects` 抠出的块与 DOM 数组元素**按下标一一对应**，前提是「全为对象且个数相等」，
否则静默错位 ⇒ **存错脚本**。风险等级明显高于读路径，应该单独一次改动 + 单独的等价性测试
（比如「保存后逐字节比对读回来的 JSON」）再做。

## 还能往哪走（未做）

按实测占比排序：

1. **`ReadAll` 的 UTF-8→UTF-16→UTF-8 往返**（实测 62 ms）：`ReadAll` 转宽串，`WideObjectView`
   又转回 UTF-8。要省掉需要让解析直接从字节走（`ExtractJsonActionBlocks` /
   `ParseWindowModeJson` / coordMeta 那几个全文扫描也要一并改成字节版）—— 爆炸半径大。
2. **`借用 DOM 取字段` 的 ~70 ms**：每条动作约 180 次 `GetNumber/GetString`，每次都要
   `ToAsciiKey` 造一个 `std::string` 再在 `std::map` 上 `find`。可考虑把键名做成静态表。
3. **整份 nlohmann 解析（实测 155 ms）**：需要 DOM 才能随机取字段，暂无更便宜的选择。
   （这也是「保存路径」改造后的地板价 —— 省不出更多，除非换解析器。）

## 复现 / 回归

```bash
# 分段计时 + 快慢两条解析路径的逐条等价性比对
build\Release\ScriptIoSelfTest.exe --bench "recordings/<脚本>.json" [--rounds 3]

# 逻辑回归（含 ScriptIoSelfTest 59 条）
python build\_tmp\run_logic_suites.py
```

⚠ 构建时若撞上**并行会话**的半成品目标（如 `FakeFocus` 链接不到 `qst_minhook.lib`），
先单独构建那个依赖再重试 —— 别把它当成自己的错误。
