# QuickScriptTool — 功能表与代码模块对照

> 便于阅读、理解与维护。功能以用户可见能力为主；模块以仓库内主要源码路径为准。  
> 相关：[`AGENTS.md`](../AGENTS.md)（自检入口）、[`docs/window-mode-design.md`](window-mode-design.md)、[`extension/PACKAGING.md`](../extension/PACKAGING.md)。

---

## 1. 产品概览

QuickScriptTool 是 Windows 上的键鼠脚本工具：主界面四页（连点 / 录制 / 宏 / 脚本定制），支持宏可视化编辑与回放、录制回放、找图/OCR、AI Agent 写脚本、后台窗口模式，以及定时任务与系统托盘。

```text
┌─────────────────────────────────────────────────────────────┐
│                     MainWindow (UI 壳)                       │
│  主页四 Tab · 宏编辑器 · 设置/热键/托盘 · 执行调度            │
└──────────┬──────────────┬──────────────┬────────────────────┘
           │              │              │
    ┌──────▼──────┐ ┌─────▼─────┐ ┌─────▼──────────────┐
    │ 连点/录制    │ │ 宏执行引擎 │ │ Agent / AI 动作     │
    │ clicker     │ │ executeOne │ │ agent_* / ai_*     │
    │ recorder*   │ │ + 变量/找图 │ └────────────────────┘
    └─────────────┘ └─────┬─────┘
                          │
              ┌───────────▼───────────┐
              │ 窗口模式 window_mode/  │
              │ + Edge 扩展桥          │
              └───────────────────────┘
```

---

## 2. 功能总表（用户能力）

| 分类 | 功能 | 简要说明 | 主要代码 |
|------|------|----------|----------|
| **主界面** | 四标签主页 | 连点 / 录制 / 宏 / 脚本定制 | `main_features.h`（`MainTab`）、`main_window.h` |
| **连点** | 鼠标连点 | 左/中/右键；自定义 / 高效 / 极限间隔 | `clicker.cpp`、`main_features.h`（`ClickerSettings`） |
| **连点** | 连点增强 | 随机间隔、按下抬起间隔、坐标抖动、定点、次数上限 | `app_settings.h`（`ClickTabSettings`）、`settings_dialog.*` |
| **录制** | 键鼠录制 | 全局 LL 钩子；窗口/全局范围；自动/绝对/FPS 相对；点击自动截模板 / 转为找图 | `recorder.h/.cpp`、`recorder_timeline.*`、`recording_to_findimage.*` |
| **录制** | 相对位移录制 | 光标隐藏 / ClipCursor 时 Raw Input（FPS 视角） | `recorder.cpp`、`RecordingCaptureMode` |
| **录制** | 录制优化 | 录制后优化/整理；「转为找图点击」语义升级 | `recording_optimize_dialog.*`、`recording_to_findimage.*` |
| **录制** | 精密时间轴回放 | 间隔为显式 Wait；键鼠动作为瞬时（duration/timingUs=0）；`inputTimingVersion=2`；绝对微秒轴（小抖动追赶、大卡顿拉伸原点）；短间隔自旋减抖动 | `recorder_timeline.*`、`input_timeline_scheduler.*` |
| **宏脚本** | 可视化编辑 | 动作树、拖拽嵌套、备注、批量编辑 | `main_window.h`、`action_tree.h`、`editor_*.*` |
| **宏脚本** | 脚本存读 | JSON 导入导出；录制/宏路径归类 | `script_io.*`、`script_types.h` |
| **宏脚本** | 动作构建 | 从 JSON/Agent 工具安全构建动作 | `script_action_builder.*` |
| **宏脚本** | 回放执行 | 前台 `SendInput` 或可选 Interception HID；窗口模式路由 | `main_window.h`（`executeOne`）、`action_utils.*`、`input/foreground_input_router.*`、`window_mode_executor.*` |
| **宏脚本** | 宏变量 / 条件 | `{var}`、If/Else、循环计数、goto | `macro_variables.*` |
| **宏脚本** | 跨分辨率坐标 | 归一化 0~1 + `coordMeta` | `coord_space.*` |
| **宏脚本** | 脱离时间 | 默认模式检测用户键鼠，超时停止 | `breakout_input.h`、`script_io`（`breakoutTimeSeconds`） |
| **宏脚本** | 调试输出窗 | 回放调试信息 | `macro_debug_window.*`、`PlaybackTabSettings` |
| **找图** | 模板匹配 | 多引擎共识、阈值、缩放、区域；模板预览裁切 | `image_match.*`、`image_match_engines.*`、`findimage_template_crop.*`、`findimage_crop_editor.*` |
| **找图** | 匹配叠加层 | 找图结果可视化 | `match_overlay.*`、`find_image_ui_debug.*` |
| **OCR** | 文字识别/查找 | PaddleOCR；安装引导；会话复用 | `ocr_engine.*`、`ocr_install_dialog.*`、`ocr_overlay.*` |
| **截图** | 区域截图 | 找图/OCR/AI 选区 | `screenshot_overlay.*` |
| **窗口模式** | 绑窗后台跑宏 | 虚拟桌面隔离；不抢用户焦点 | `src/window_mode/**` |
| **窗口模式** | 窗口捕获 | PrintWindow / WGC 等 | `window_capture.*`、`window_capture_wgc.*` |
| **窗口模式** | 后台输入 | UIA / PostMessage / FakeFocus / CDP / Ext 桥 | 见 §4 |
| **窗口模式** | 选窗 / 预览 | 拾取对话框、缩略图健康状态 | `window_pick_dialog.*`、`window_mode_preview.*` |
| **浏览器桥** | Edge 扩展 | 网页侧键鼠桥；本机 `ext_bridge` | `extension/edge/**`、`window_mode/ext_bridge/**` |
| **定时任务** | 录制/宏定时跑 | 小时/天/周/自定义；全局禁用 | `scheduled_task_*.*` |
| **热键** | 全局启停 / 脚本热键 | RegisterHotKey + LL 钩子；按住启停；IME 透传 | `hotkey_dialog.*`、`main_window.h`（热键钩子段） |
| **设置** | 应用设置 | 连点/回放/其他/窗口模式/AI；开机自启、托盘等 | `app_settings.h`、`app_settings_store.*`、`settings_dialog.*` |
| **主题** | 预设 / 自定义主题 | 主色、强调色、取色弹窗 | `app_theme.*`、`theme_custom_dialog.*`、`theme_ui_layout.h` |
| **托盘** | 托盘菜单 | 关闭到托盘、快捷操作 | `tray_menu.*` |
| **Agent** | AI 脚本定制 | 对话写宏、工具调用、附件、会话存储 | `agent_*.*`、`main`「脚本定制」Tab |
| **AI 动作** | 宏内 AI 步骤 | 文字/图片分析、混合路由执行 | `ai_action_*.*`、`ActionType::Ai*` |
| **进程工具** | 启停程序 | 运行/关闭程序、网页、文件 | `process_utils.*`、对应 `ActionType` |
| **输入后端** | 鼠标注入抽象 | 桌面绝对等后端；可选 Interception HID | `input/mouse_input_backend.*`、`input/hid_interception.*`、`input/foreground_input_router.*` |
| **UI 基础** | 自绘控件 / 渲染 | GDI/D2D、缩放、现代编辑框 | `controls.*`、`render_*.*`、`modern_edit.*`、`ui_scale.*` |
| **发版** | 安装包 / 扩展打包 | Release 素材、Inno、Edge zip | `tools/package_release.ps1`、`tools/pack_edge_extension.ps1`、`installer/` |

---

## 3. 主界面四 Tab

| Tab | 枚举 | 用户用途 | 关键模块 |
|-----|------|----------|----------|
| 连点击 | `MainTab::Clicker` | 快速连点，可不进宏编辑器 | `clicker.cpp`、`IClickerFeature` |
| 录制器 | `MainTab::Recorder` | 录制键鼠 → 保存为录制脚本 → 回放 | `recorder*`、录制列表 UI（`main_window`） |
| 宏脚本 | `MainTab::Macro` | 宏列表、打开编辑器、热键/定时关联 | 编辑器 + `script_io` + 执行引擎 |
| 脚本定制 | `MainTab::ScriptCustom` | AI Agent 对话生成/修改宏 | `agent_dialog.*`、`agent_core.*`、`agent_tools.*` |

设置入口、全局热键、托盘等跨 Tab 能力挂在 `MainWindow` / `settings_dialog` / `tray_menu`。

---

## 4. 宏动作类型 ↔ 代码

数据模型：`src/script_types.h`（`ActionType` / `ScriptAction`）。执行主路径在 `MainWindow` 的动作执行逻辑，窗口模式下经 `window_mode_executor` 代理。

| ActionType | 中文含义 | 典型依赖模块 |
|------------|----------|--------------|
| `MoveMouse` / `MoveMouseRelative` | 绝对/相对移动 | `action_utils`、`coord_space`、输入后端 |
| `Wait` | 等待 | 执行循环内定时 |
| `MouseDown` / `MouseUp` / `MouseClick` | 鼠标按下/抬起/点击 | `action_utils`、窗口模式输入 |
| `KeyDown` / `KeyUp` / `KeyClick` | 键盘按下/抬起/单击 | 同上 |
| `ScrollWheel` | 滚轮 | 同上 |
| `HotkeyShortcut` | 快捷组合键 | 同上 |
| `QuickInput` | 快捷文本输入 | `macro_variables`（转义）、输入 |
| `Loop` / `EndLoop` | 循环 | `macro_variables`（循环变量） |
| `If` / `Else` | 条件分支 | `macro_variables`（`EvaluateConditionExpr`） |
| `Goto` | 跳转步骤 | `macro_variables`（`TryResolveGotoStepNo`） |
| `DefineBlock` / `RunBlock` | 指令块定义/调用 | `action_tree`、执行栈 |
| `RunMacro` | 调用其它宏 | `script_io` |
| `MousePlayback` | 回放录制轨迹 | `recorder_timeline`、`input_timeline_scheduler` |
| `FindImage` | 找图 | `image_match*`、截图、`match_overlay` |
| `LockScreenshot` / `UnlockScreenshot` | 锁定/解锁截图缓存 | 找图执行路径 |
| `TextRecognition` | OCR / 文字查找 | `ocr_engine`、`ocr_overlay` |
| `GetCursorPos` | 取光标坐标 | 系统 API + 变量 |
| `TimerRecordTime` | 计时器 | `macro_variables`（timer） |
| `RunProgram` / `CloseProgram` | 启动/关闭进程 | `process_utils` |
| `OpenWebpage` / `OpenFile` | 打开网页/文件 | Shell / 进程工具 |
| `StopMacro` | 停止宏 | 执行控制 |
| `CustomText` | 自定义显示文本 | 编辑器展示 |
| `AiTextAnalysis` | AI 文字分析 | `ai_action_service`、`ai_action_runtime` |
| `AiImageAnalysis` | AI 图片分析 | 同上 + 截图 |
| `AiActionExecute` | AI 动作执行（混合路由） | `ai_action_router`、`agent` 工具通道 |

容器判断：`IsExpandableContainer` / `IsSubtreeContainer` / `SkipsInMainFlow`（同文件）。

**注意：** `mouseClick` / `keyClick` 等动作的 `duration` 表示**重复间隔**（两次之间），不是执行前等待。见 `.cursor/skills/module-selftest/SKILL.md`。

---

## 5. 窗口模式子模块

目录：`src/window_mode/`。设计说明：[`docs/window-mode-design.md`](window-mode-design.md)。硬性需求：`window_mode_requirements.h`。

| 子模块 | 路径 | 职责 |
|--------|------|------|
| 会话 | `window_mode_session.*` | 绑定目标、健康检查、生命周期 |
| 执行路由 | `window_mode_executor.*` | 包装宏动作：坐标、桌面、输入、找图 |
| 类型 / JSON | `window_mode_types.*`、`window_mode_json.*` | 配置、选择方式、健康枚举、脚本序列化 |
| 目标窗口 | `window_target.*`、`window_pick_dialog.*` | 查找/拾取窗口 |
| 坐标 | `window_coords.*` | 客户区 ↔ 屏幕 |
| 捕获 | `window_capture.*`、`window_capture_wgc.*` | 窗口截图（找图用） |
| 预览 | `window_mode_preview.*` | 缩略图刷新 |
| 虚拟桌面 | `macro_virtual_desktop.*`、`virtual_desktop_accessor.*`、`hidden_desktop.*` | 宏桌面隔离 |
| 后台输入 | `background_window_input.*`、`background_uia_input.*` | 通用后台键鼠 |
| FakeFocus | `fake_focus/**` | 注入假焦点 / soft input |
| CDP | `cdp/**` | Chromium DevTools 输入（浏览器类） |
| Ext 桥 | `ext_bridge/**` | 本机桥 ↔ Edge 扩展 |
| 权限 / 日志 | `window_mode_permission.*`、`window_mode_log.*` | 权限与诊断日志 |
| COM | `com_apartment.*` | COM 公寓 |

配套扩展源码：`extension/edge/`（发版必带，见 `extension/PACKAGING.md`）。

---

## 6. AI / Agent 模块

| 模块 | 路径 | 职责 |
|------|------|------|
| Agent 核心 | `agent_core.*` | OpenAI 兼容 API、流式、tool-call 循环 |
| Agent UI | `agent_dialog.*`、`agent_ui_notify.*` | 脚本定制页对话界面 |
| 工具定义 | `agent_tools.*`、`macro_execute_tools.*` | 读写宏、定时任务 CRUD、执行宏等 |
| 脚本操作 | `agent_script_ops.*` | 对脚本内容的增删改 |
| 系统提示 / 参考 | `agent_system_prompt.*`、`agent_reference.*` | Prompt 与动作说明 |
| 附件 | `agent_attachment.*` | 图片等附件 |
| 会话存储 | `agent_conversation_store.*` | 对话持久化 |
| AI 宏动作 | `agent_ai_actions.*`、`ai_action_service.*`、`ai_action_runtime.*` | 宏内 AI 步骤运行时 |
| 混合路由 | `ai_action_router.*` | Vision / 点击组合 / 多轮 tools 分类 |

API 配置：`app_settings.h` → `AiApiSettings` / `AiModelProfile`。

---

## 7. 定时任务模块

| 文件 | 职责 |
|------|------|
| `scheduled_task_types.*` | 任务结构、频率、周掩码、是否应触发 |
| `scheduled_task_store.*` | 持久化 |
| `scheduled_task_scheduler.*` | Tick / 调度 |
| `scheduled_task_dialog.*`、`scheduled_task_ui.h`、`scheduled_task_datetime_picker.*` | UI |
| Agent 侧 | `agent_tools.cpp`（创建/更新/删除后须 Reload） |

专项自检：`ScheduledTaskSelfTest`（见 `AGENTS.md`）。

---

## 8. 目录与基础设施速查

| 路径 | 内容 |
|------|------|
| `src/` | 主程序业务与 UI |
| `src/window_mode/` | 窗口模式与浏览器桥宿主端 |
| `src/input/` | 鼠标输入后端 |
| `extension/edge/` | Edge/Chrome MV3 扩展（非 C++ 编译产物） |
| `tools/` | 自检 exe、打包脚本、OCR helper |
| `installer/` | Inno Setup 安装脚本 |
| `docs/` | 设计/功能文档与综合用例 |
| `.cursor/skills/` | Agent 调试技能（窗口模式、定时任务、模块自检） |
| `DuiLib_Ultimate/`、`third_party/` | 第三方 / 历史 UI 库（主路径以自绘 Win32 为主） |
| `resources/`、`scripts/` | 资源与辅助脚本 |

**入口：** `src/main.cpp` → `MainWindow`（声明与大量实现内联于 `main_window.h`，部分功能拆到 `clicker.cpp`、`recorder` 相关调用等）。

**配置常量 / 布局：** `config.h`。  
**工具函数：** `utils.*`。  
**品牌：** `app_branding.*`。

---

## 9. 模块自检对照（维护改代码时）

改下列能力时，优先跑对应 suite（`MSBuild` Release → `exe --json`，exit 0 再宣称修好）。完整表见 [`AGENTS.md`](../AGENTS.md) 与 [`.cursor/skills/module-selftest/SKILL.md`](../.cursor/skills/module-selftest/SKILL.md)。

| 能力 | SelfTest Target |
|------|-----------------|
| 窗口模式 | `WindowModeSelfTest` |
| 定时任务 | `ScheduledTaskSelfTest` |
| 宏变量 | `MacroVariablesSelfTest` |
| 动作构建 | `ScriptActionBuilderSelfTest` |
| 坐标 | `CoordSpaceSelfTest` |
| 脚本 IO | `ScriptIoSelfTest` |
| 找图 | `ImageMatchSelfTest` |
| AI 路由 | `AiActionRouterSelfTest` |
| 设置库 | `AppSettingsStoreSelfTest` |
| 主题 UI | `ThemeUiSelfTest` |
| 录制回放逻辑 | `RecorderSelfTest` → `QstRecorderLogicTest.exe` |

连点观感、OCR 安装、Agent 对话体验等仍偏手工：见 [`docs/comprehensive-test-cases.md`](comprehensive-test-cases.md)。

---

## 10. 建议阅读顺序（新人）

1. 本文 §2–§3：建立功能地图  
2. `script_types.h`：弄清宏能表达什么  
3. `script_io.h` + `coord_space.h`：脚本文件长什么样  
4. `main_features.h` + 主页四 Tab 相关 UI 代码  
5. `recorder.h` → 录制；`macro_variables.h` → 条件/变量  
6. `docs/window-mode-design.md` + `window_mode_session.h`：后台跑宏  
7. `agent_core.h` + `agent_tools`：脚本定制  
8. 改具体模块前打开对应 `.cursor/skills/*/SKILL.md` 与自检

---

*文档随代码演进；若功能表与源码不一致，以 `script_types.h`、`app_settings.h`、`window_mode_types.h` 及各模块自检为准。*
