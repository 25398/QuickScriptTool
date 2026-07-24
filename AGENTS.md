# Agent notes (QuickScriptTool)

## Module self-tests（必读入口）

统一约定与 suite 索引：

**[`.cursor/skills/module-selftest/SKILL.md`](.cursor/skills/module-selftest/SKILL.md)**

动作语义注意：`mouseClick`/`keyClick` 等 `duration` 是**重复间隔**（两次之间），不是执行前等待——细节见元 Skill「重复间隔语义」。

流程：选 suite → `MSBuild` Release `/t:<Target>` → `build\Release\<Target>.exe --json` → exit `0` 才宣称修好。

| Suite | Target | Skill |
|-------|--------|-------|
| 窗口模式 | `WindowModeSelfTest` | [window-mode-debug](.cursor/skills/window-mode-debug/SKILL.md)（需求：[cdp-requirements.md](.cursor/skills/window-mode-debug/cdp-requirements.md)；踩坑：[cdp-lessons.md](.cursor/skills/window-mode-debug/cdp-lessons.md)） |
| 定时任务 | `ScheduledTaskSelfTest` | [scheduled-task-debug](.cursor/skills/scheduled-task-debug/SKILL.md) |
| 宏变量 | `MacroVariablesSelfTest` | 见元 Skill FAIL 表 |
| 动作构建 | `ScriptActionBuilderSelfTest` | 见元 Skill FAIL 表 |
| 坐标 | `CoordSpaceSelfTest` | 见元 Skill FAIL 表 |
| 脚本 IO | `ScriptIoSelfTest` | 见元 Skill FAIL 表 |
| 找图引擎 | `ImageMatchSelfTest` | 见元 Skill FAIL 表 |
| AI 路由 | `AiActionRouterSelfTest` | 见元 Skill FAIL 表 |
| 设置库 | `AppSettingsStoreSelfTest` | 见元 Skill FAIL 表 |
| 主题 UI | `ThemeUiSelfTest` | 见元 Skill FAIL 表 |
| 录制回放 | `RecorderSelfTest` → `QstRecorderLogicTest.exe` | 见元 Skill FAIL 表 |

共享 harness：`tools/selftest_harness.h`。各 exe：`tools/*_selftest.cpp`。

### 构建模板

```powershell
& "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" `
  ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:<Target> /m /v:minimal
build\Release\<Target>.exe --json
```

窗口模式可选烟雾：`--macro`。定时任务硬规则见 scheduled-task-debug skill。

PowerShell 不要用 `/t:A;B` 拼多个 target（分号会拆命令）；分开跑。`--list` / `--json` 结果均在 stdout。

## Edge 配套扩展（发版必带）

源码：`extension/edge/`（非编译产物）。规范全文：**[extension/PACKAGING.md](extension/PACKAGING.md)**。

| 动作 | 命令 / 路径 |
|------|-------------|
| 校验 + 打侧载 zip | `powershell -File tools\pack_edge_extension.ps1` → `dist\QstEdgeBridge-<ver>.zip` |
| 打软件 Release / 安装包素材 | `powershell -File tools\package_release.ps1`（内部会先跑上一步） |
| Inno 安装包 | 先 `package_release.ps1`，再编 `installer\QuickScriptTool.iss` |

硬规则：改扩展版本须同步 `manifest.json` 与 `background.js` 的 `BRIDGE_VERSION`；`manifest` 必须是合法 JSON（`version` 行用逗号不是分号）；**禁止**发不含 `extension\edge` 的安装包/zip。
