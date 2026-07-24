---
name: window-mode-debug
description: >-
  Debug and regression-test QuickScriptTool 窗口模式 / 后台窗口模式 via
  WindowModeSelfTest.exe. Use when: finding/binding target windows fails,
  find-image is 0%, launch/open-document errors (文件名无效), tray/Z-order issues
  during window mode, or any agent needs a green/red loop before UI reproduce.
  Prefer this skill over guessing — run --json, map FAIL name to source, fix, re-run.
---

# Window Mode Debug（Agent 必读）

总索引：[module-selftest](../module-selftest/SKILL.md)（选 suite / 统一约定）

本仓库给 Cursor Agent 准备的**可执行自检**：`WindowModeSelfTest.exe`。  
目标：不依赖用户手工点宏，也能验证启动/绑定/子控件/快捷输入等核心逻辑，并根据 FAIL 名称定位改代码。

**CDP/扩展需求与踩坑（禁止重复证伪方向）**：  
- 需求：[cdp-requirements.md](cdp-requirements.md)  
- 错误方向 / 定论：[cdp-lessons.md](cdp-lessons.md)

## Agent 取日志（免用户粘贴）

窗口模式日志自动追加到 **`QuickScriptTool.exe` 同目录** 的 `window_mode_debug.log`（Release/Debug 各自一份）。  
排障时读该文件即可，不必让用户从「宏调试信息」窗复制。

```powershell
Get-Content ".\build\Release\window_mode_debug.log" -Tail 80
```

完整游戏脚本无法在 Agent 环境可靠复现（需本机 Edge+扩展+游戏页）；以自检 `--json` + 上述日志为准。

## 何时必须用本 Skill

- 用户反馈：找图 0%、绑错窗（IME/`SoPY`）、「文件名无效」、启动后卡在等待窗口、后台 OK 但「鼠标宏」桌面挂了  
- 你改了 `src/window_mode/**`、启动参数、FindImage 窗口模式路径  
- 准备提「已修好」之前：至少默认用例全绿  

## 构建（Windows / MSBuild）

在仓库根目录 `d:\other\software`（或当前 clone 根）：

```powershell
& "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:WindowModeSelfTest /m /v:minimal
```

需要连带主程序时：

```powershell
# PowerShell 里不要用分号拼两个 /t：会当成两个命令。分开跑或：
& ...\MSBuild.exe ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:QuickScriptTool /m /v:minimal
& ...\MSBuild.exe ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:WindowModeSelfTest /m /v:minimal
```

产物：`build\Release\WindowModeSelfTest.exe`  
（OpenCV / VDA DLL 一般已在 `build\Release\`，若缺 `opencv_world*.dll` 从同目录 QuickScriptTool 旁复制。）

## 运行

| 命令 | 作用 |
|------|------|
| `build\Release\WindowModeSelfTest.exe --help` | 用法 |
| `build\Release\WindowModeSelfTest.exe --list` | 只列用例名与说明（不跑） |
| `build\Release\WindowModeSelfTest.exe --json` | **Agent 默认**：机器可读，exit `0`=全过 |
| `build\Release\WindowModeSelfTest.exe` | 人类可读 PASS/FAIL |
| `... --json --macro` | 额外跑「鼠标宏」桌面烟雾（需 `VirtualDesktopAccessor*.dll`） |

**判读：**

- 进程退出码 `0` → 默认套件绿，可继续 UI 复现或收工  
- 退出码 `N>0` → `N` 个失败用例；stdout 每行一个 case JSON，最后一行 summary  

`--json` 单行 schema：

```json
{"name":"case_id","ok":true|false,"detail":"可选说明/实际值"}
{"passed":6,"failed":0,"ok":true}
```

## 用例 → 含义 → 优先改哪里

详见同目录 [reference.md](reference.md)。速查：

| `name` | 失败通常表示 | 优先查看 |
|--------|--------------|----------|
| `quote_args_strip` | 文档路径引号被二次转义 | `macro_virtual_desktop.cpp` `FormatLaunchArgs`；`window_mode_session.cpp` CreateProcess/文档启动 |
| `no_select_ignores_doc` | 不选择窗口仍打开残留文档/标题 | `ResolveDocumentFileToOpen` / `EffectiveLaunchArgs`（NoSelect 应空参启 exe） |
| `ime_filter_null` | IME 过滤 API 异常 | `window_target.cpp` `IsLikelyImeOrToolWindow` |
| `find_main_window` | 按类找顶层窗失败 | `window_target.cpp` Find/匹配 |
| `post_quick_input` | 后台 PostMessage 输入失败 | `background_window_input.cpp` |
| `background_bind_child` | 未绑到子 `EDIT` | `WaitForBindHwnd` / `ResolveBindHwnd`（`window_mode_session.cpp`） |
| `background_quick_input` | Executor 后台输入不通 | `window_mode_executor.cpp` BeginRun/SendQuickInput |
| `quick_input_cancel` | 热键/取消无法打断快捷输入 | `background_window_input.cpp`；`main_window.h` QuickInput |
| `desktop_quick_input_cancel` | 桌面快捷输入不响应取消 | `action_utils.cpp` `SendQuickInputText` |
| `macro_desktop_launch_bind` | 宏桌面启动或绑定失败（仅 `--macro`） | `macro_virtual_desktop.cpp` + `LaunchTargetOnDesktop` |
| `macro_classic_with_store_open` | 已有商店记事本时经典路径绑不上 | `FindLaunchResultMainWindow` / 启动复用回退 |
| `macro_store_path_class_bind` | 商店路径+指定窗口类/RichEdit 失败 | `LaunchStoreAppFromPath` / `WaitForBindHwnd` |
| `macro_editor_open_named_doc` | 指定窗口类未按标题打开文档、绑到空白窗 | `BuildTargetQuery` 标题 / `ResolveDocumentFileToOpen` / `BeginRun` 身份校验 |
| `fake_focus_json_roundtrip` | fakeFocusEnabled 缺省/读写错 | `window_mode_json.cpp` |
| `fake_focus_minimize_gate` | UsesFakeFocus / 最小化门控错 | `window_mode_types.h` |
| `fake_focus_hook_local` | FakeFocus DLL hook/卸载失败 | `src/window_mode/fake_focus/**` |
| `fake_focus_soft_input` | Phase2 软输入同步失败 | `fake_focus_soft_input*` / DLL GetCursorPos hooks |
| `anjuzhen_script_wm_config` | 安居镇.json windowMode 字段不符 | `build/*/scripts/安居镇.json` + `window_mode_json` |
| `cdp_park_expandable` | CDP 停放后仍 Cloak/Peek 或二次最小化 | `PrepareMacroDesktopForCdpBind`；[cdp-lessons.md](cdp-lessons.md) |

## 硬性约定（改窗口模式时遵守）

完整条文：`src/window_mode/window_mode_requirements.h`（优化时禁止改坏）。

1. **「指定窗口类」≠「不选择窗口」**：指定类按身份找/开文档；不选择窗口只启 exe，忽略残留 launchArgs/标题。  
2. **输入策略分流**：  
   - **CDP/扩展**：Win32 **只** Move 到「鼠标宏」停放；键鼠/找图/保活走扩展。禁 Cloak/PreferMax/pin/Correct/Raise。见 [cdp-lessons.md](cdp-lessons.md)。  
   - **softMessage/假焦点**：宏桌面 + Win32。  
   - 切屏/无法展开 → 多半又在用 Cloak/PreferMax（错误方向）。  
3. **响应要快**：短轮询、找到即返回（无标题约 ≤2s，带文档约 ≤3s）。  
4. **自动打开文档（仅指定窗口类）**：目标程序带文件参数；禁止裸 `ShellExecute(文档)` 当主路径；禁止 `GetFullPathName(cwd)` 误开无关文件。  
5. **命令行参数**：先剥外层 `"`；工作线程不要 WinEvent 狂睡。  
6. **有 `childWindowClassName` 时**：后台与宏桌面都能绑子控件再找图。  

## 推荐迭代循环（复制即用）

```text
loop:
  MSBuild ... /t:WindowModeSelfTest
  run: build\Release\WindowModeSelfTest.exe --json
  if exit==0: break
  read FAIL name + detail → open file from table → patch → continue
optional:
  WindowModeSelfTest.exe --json --macro
  then manual: build\Release\QuickScriptTool.exe
```

## 相关路径

- 自检源码：`tools/window_mode_selftest.cpp`  
- 窗口模式核心：`src/window_mode/`  
- 主程序找图入口：`src/main_window.h`（FindImage + `wmUsesTarget`）  
- 用例详解：`reference.md`  
