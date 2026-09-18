#pragma once

// =============================================================================
// 窗口模式硬性需求（用户约定）
// -----------------------------------------------------------------------------
// Agent / 后续优化改 `src/window_mode/**` 时必须遵守。违反即回归。
// 自检入口：WindowModeSelfTest --json / --json --macro
// Skill：.cursor/skills/window-mode-debug/SKILL.md
//
// 总原则：窗口模式 / 后台窗口模式执行时不得影响用户正在做的事。
//
// 1) 窗口模式的工作面是「鼠标宏」虚拟桌面
//    - 被调用的窗口都先移到「鼠标宏」（没有则创建），再在宏桌面完成启动/绑窗/找图等。
//    - 禁止把用户视图切到「鼠标宏」再在上面操作；用户应始终停留在原来的桌面。
//
// 2) 创建宏桌面 / 搬窗 / 宏桌面内操作：禁止切换用户当前视图
//    - 禁止故意 GoToDesktopNumber 切到「鼠标宏」。
//    - CreateDesktop / MoveWindowToDesktopNumber 若被系统短暂带走视图：立刻 GoTo 回用户原桌面
//      （这是「保持用户不被影响」的必要纠正，不是拿切屏当正式流程）。
//    - 搬窗前尽量最小化/隐藏，降低系统因可见窗自动切桌面的概率。
//
// 3) 「指定窗口类」≠「不选择窗口」
//    - 指定窗口类：必须按编辑身份（exe + 类名 + 标题/文档 stem）找/开；
//      有文档时只启动一次「目标程序 + 文档路径」；禁止改开无标题空白窗交差。
//    - 不选择窗口：只按目标 exe 启动/绑定；忽略残留的 launchArgs、windowName、类名。
//      应 CreateProcess(exe) 本身，禁止再自动打开之前指定过的文档。
//
// 4) 响应要快
//    - 短轮询、找到即返回（无标题约 ≤2s，带文档约 ≤3s）。
//
// 5) 其它摘要
//    - 「指定窗口类」自动打开文档：目标程序 + 文件参数；禁止裸 ShellExecute(文档) 当主路径。
//    - 命令行参数先剥外层引号；childWindowClassName 时后台与宏桌面都要能绑子控件。
//
// 6) CDP + 配套扩展（浏览器）输入策略分流
//    A) CDP/扩展：键鼠/找图走扩展；宿主 Minimize→Move「鼠标宏」后 Pin+屏外（工作区尺寸）。
//       异桌裸还原会在鼠标时切屏（已证伪）。观看 UnPin+迁回宏桌面；已展开禁再 Placement。
//    B) softMessage / 假焦点：宏桌面 + Win32；绑后可最小化。
//    详见 .cursor/skills/window-mode-debug/cdp-requirements.md / cdp-lessons.md
//
// 7) 后台逐字投递（快捷输入）时序 —— 禁止零间隔连发
//    - 走窗口消息（LCA/未登记游戏/Qt/AIR/Chromium 壳）时，目标线程自己的 TranslateMessage
//      会把 WM_CHAR 排到**所有已投递消息之后**：DOWN 紧跟 UP ⇒ 字符在「键已抬起」后才到，
//      按帧取键/只在键仍按下时收字的游戏就**吞字**（实测后台输入 "11" 只进一个 1，前台 SendInput 正常）。
//    - 硬规则：必须保证「本键已被目标处理（键处于按下态）且它的 WM_CHAR 已排在 UP 之前」。
//      默认实现是**队列屏障**（`queueBarrier()`：跨线程同步 `SendMessageTimeoutW(top, WM_NULL)`，
//      排在已投递消息之后、目标补发的 WM_CHAR 之前被处理），与目标帧率无关；
//      屏障不可用（UIPI/进程内灌键队列/超时）才回落到 `DOWN→按住≥1帧→UP→间隔` 固定时序。
//    - 用户字间隔只能加大、不得小于兜底下限；修饰键（Ctrl+V）同样先按住再发字符键。
//    - 现场旋钮：`QST_LCA_NO_BARRIER=1`（关屏障 A/B）、`QST_LCA_KEY_MS=0~500`（兜底按住/间隔）。
//    - 对应用例：`posted_quick_keys_timing`（WindowModeSelfTest）。
//
// 8) 软键（假焦点灌键队列）必须「状态跟着事件走」
//    - `down[]` 是**当前值**，事件由 DLL 灌键线程稍后 PostMessage：宿主一次把 DOWN/UP 全写完，
//      目标才在自己的节奏里处理键消息 —— 目标在处理 `WM_KEYDOWN(V)` 时读到的是「Ctrl 已抬起」，
//      Chromium 的 `IsKeyDown(GetKeyboardState(), modifiers)` 判 Ctrl 不在 → **Ctrl+V 退化成普通字符**
//      （实测症状：只出 v 不粘贴；WinForms 宿主 + CEF 的壳同样中招）。
//    - 硬规则：软键每投递一笔必须用 `WaitSoftKeyPostTurn()`（跨线程同步 `WM_NULL` 队列屏障）
//      等目标处理完，再让调用方写下一步键态；冒险岛 DirectInput 共享内存路径除外。
//    - 现场旋钮：`QST_NO_SOFT_KEY_BARRIER=1`（关屏障 A/B，恢复旧行为）。
//    - 对应用例：`soft_key_combo_state_race`（WindowModeSelfTest）。
// =============================================================================
