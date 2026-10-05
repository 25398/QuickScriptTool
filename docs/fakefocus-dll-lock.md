# FakeFocus DLL 被长期锁住 → 安装/卸载必须重启电脑（2026-09-30 修复）

## 一、用户报障

> 「`F:\QuickScriptTool\FakeFocus32.dll` 软件关闭后会遗留，不重启电脑就无法删除，
> 而这又导致了用户在使用我们软件安装包时必须重启电脑才能正常安装，否则会卡死在这里。」

现场形态：安装目录里只剩一个 `FakeFocus32.dll`（其余文件都删掉了），删不掉、也覆盖不了。

## 二、根因

注入假焦点 = **让目标进程（游戏/模拟器）`LoadLibrary` 这个 DLL**
（`src/window_mode/injection/`，默认技术 `classic`）。于是这个 DLL 文件被目标进程
**长期映射**，而 Windows 对已映射的映像文件不允许写覆盖 / 删除。

三种让映射「活得比软件久」的路径（前两条都不需要任何异常操作）：

| # | 路径 | 后果 |
|---|---|---|
| 1 | **宿主被强杀**（任务管理器结束进程、崩溃、安装包自己的 `taskkill /F`） | 没有任何执行机会去远程 `FreeLibrary` ⇒ 引用计数永远是 1 |
| 2 | **桌面钩路径把 HHOOK 丢了**（`inject_hook.cpp` 旧实现把 `HHOOK`/本地 `HMODULE` 放在函数局部变量里） | 钩子永远拆不掉 ⇒ user32 把 DLL **钉在**目标进程里，远程 `FreeLibrary` 归不了零（而且钩子过程一直在目标线程里跑） |
| 3 | **引用计数残留**：上一轮被杀掉一半，下一轮又 `LoadLibrary` 一次 | 只 `FreeLibrary` 一次卸不掉，DLL 常驻 |

于是：只要那个游戏/模拟器还开着，安装目录里的 `FakeFocus32.dll` 就是**只读加锁**的。

安装包为什么"卡"在这里：`installer\QuickScriptTool.iss` 里
`CloseApplicationsFilter=QuickScriptTool.exe,QstWebViewShell.exe` —— 这个指令
**不是"允许关闭哪些程序"，而是「文件名通配符」**（Inno 文档）：
只有匹配它的**文件**才会被 Restart Manager 检查是否被占用，默认值是 `*.exe,*.dll`。
写成两个 exe 名之后，**`FakeFocus32.dll` 根本不进检测**，Inno 直接去覆盖 →
共享冲突 → 按文档「重试 4 次、每次隔 1 秒」→ 仍然失败 → **弹错误框**（用户看到的就是"卡死"）。

## 三、修法（产品侧：换一个可以被锁的文件）

新增 `src\window_mode\fake_focus\fake_focus_stage.{h,cpp}`：注入前把 DLL
**复制到暂存目录**再从副本注入：

```
%LOCALAPPDATA%\QuickScriptTool\module_stage\<源名>.<大小>_<时间戳>\FakeFocus32.dll
```

- 安装目录那份 DLL 从此**没有任何进程映射它** ⇒ 覆盖安装 / 卸载 / 手动删除都不再需要重启。
- 副本按「源文件身份（大小 + 时间戳）」命名：同一构建复用同一份，**新构建换目录**，
  旧目录留给启动清扫按年龄（30 天）回收，被进程占用的一律跳过。
- 副本文件名**归一成 `FakeFocus32.dll` / `FakeFocus64.dll`**：`FakeFocus32.next.dll`
  这类旁路槽如果按原名注入，`MapleIsFakeFocusModulePath` / `TargetHasStaleFakeFocusModule`
  这些**按名判据**会漏掉它（顺带修掉这个旧隐患）。
- 刻意**不做**「注入后删掉副本」：那会把目标进程的映射变成 delete-pending，
  反而制造出最难查的那种"文件还在、名字没了、删不掉"状态。

同时补齐卸载路径（`fake_focus_injector.cpp`）：

- `InjectResult` 增加 `hookModule` / `hookHandle`，**桌面钩的 HHOOK 与本地模块句柄必须交回
  调用方**，`UnloadOne` 里 `UnhookWindowsHookEx` + `FreeLibrary`（路径 2 的根因）；
- `UnloadOne` 按「目标进程里这个模块还在不在」**循环**远程 `FreeLibrary`（有上限 8 次），
  把历史残留的引用计数一次清到 0（路径 3 的根因），并打一行 `引用计数残留 … FreeLibrary N 次`。

启动清扫（`SweepStaleFakeFocusArtifacts`）：产品壳 `wWinMain` 与播放器各调一次，
删旧副本目录 + 删安装目录里让位改名留下的 `FakeFocus*.dll.locked-*`，
打一行 `fakefocus stage sweep dirs=… keptLocked=… leftovers=…`（被占用的跳过，不报错）。

## 四、修法（安装包侧：对**存量**用户兜底）

暂存副本只保证**新版本之后**不再锁安装目录那份；已经锁住的老用户仍然要能装上去：

1. `[Code]` 新增 `ReleaseLockedInjectModules`：在开始拷文件之前（`PrepareToInstall`），
   对 `FakeFocus32/64.dll` 做一次「写打开」探测 ——
   - 不算锁着 → 不动它，交给 Inno 正常覆盖；
   - 真锁着 → **改名让位** 成 `FakeFocus32.dll.locked-<时间戳>`。
     Windows 允许改名一个已映射的映像文件（只是不允许删/写），文件名一空出来，
     Inno 就能写新版本，**全程不需要重启**。残留文件由下次启动的清扫删掉。
     实测（`.tmp\lockprobe`）：映射中的 DLL「写打开失败 / **改名成功** / 删除失败」。
2. `[Files]` 把这两份 DLL 单独列出并加 `restartreplace` + `uninsrestartdelete`：
   万一让位也失败，最差是"排到重启时替换/删除"（Inno 原生语义），**不再卡在错误框**。
3. 卸载收尾 `ReportLockedInjectLeftovers`：目录里还剩这两份 DLL 时，
   用 `tasklist /m` 列出**是哪些进程还加载着它**，明确告诉用户"退出这些程序即可，或重启一次"，
   而不是留一个神秘文件让人猜（这正是报障截图里的形态）。
4. `CloseApplicationsFilter` 保持只列我们自己的两个 exe（**故意**不写 `*.dll`）：
   否则 Restart Manager 会把用户正在玩的游戏（它映射着 FakeFocus DLL）也当成"要更新的文件占用者"，
   而 `CloseApplications=force` 是真强杀 —— 那等于安装包去杀用户的游戏。占用问题由第 1 条解决。

## 五、自检与验收

- `WindowModeSelfTest`：
  - `fake_focus_inject_copy`：副本落盘、**源文件不动**、文件名归一、同源同路径、异源异路径；
  - `fake_focus_stage_sweep`：老副本被清、**被占用（独占打开）的跳过**、放开后再扫即清、
    让位残留 `FakeFocus*.dll.locked-*` 被清且**不动用户自己的 `*.locked-*` 文件**。
- 手工复现口径：注入一次 → 任务管理器**强杀** QuickScriptTool.exe → 此时
  `%LOCALAPPDATA%\QuickScriptTool\module_stage\*\FakeFocusXX.dll` 被锁是**预期**，
  而**安装目录**那份应当随手可删（这就是本次修复的判据）。
- 安装包 Pascal 脚本语法快速校验（秒级，不碰 dist）：
  `.tmp\iss_check\run_check.ps1` —— 抽出 `#define` + `[Code]` 段单独编译。
