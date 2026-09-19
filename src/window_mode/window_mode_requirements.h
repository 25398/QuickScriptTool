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
//
// 9) 后台窗口模式：**不得**为了目标窗口去 SendInput 真键
//    - 方向键兜底（LCA/未登记游戏/冒险岛）只在「目标就是前台窗」时才 `SendKeyboardKey`：
//      目标在后台时 SendInput 打的是当前前台窗（用户正在看的浏览器/视频会收到 ←/→/↑/↓，
//      表现为 B 站等视频跳进度/调音量）。判据 `TargetOwnsForegroundWindow()`。
//    - 冒险岛后台走路靠软键态 + DirectInput 软键，前提是**客户端在轮询**：2009 dinput8
//      失焦即停轮询（靠 WM_ACTIVATE，不逐帧查前台）。IAT 吞失活只拦得住注入之后的失活，
//      所以若点运行时游戏已经不在前台，必须在注入后补一次**真激活**（`WakeMapleStoryInputPolling`，
//      不能用假 WM_ACTIVATE——给冒险岛灌假激活会冻客户端），等 `diState>0` 再把前台还给用户。
//      诊断：`冒险岛钩命中 … diState=0 lastCb=0` + 全程 `gaks=0 gfw=0` ⇒ 客户端根本没在轮询。
//    - 对应用例：`lca_arrow_key_lparam` / `maplestory_bg_fake_focus`（WindowModeSelfTest）。
//
// 10) 冒险岛诊断契约：必须能区分「钩子没装上」和「客户端根本不调这些 API」
//    - 痛点：只原地平A 的日志里 `gfw/gaks/diState/lastCb` 全是 0，但 `diag` 又显示钩子装好了，
//      光看计数器无法定论，容易反复猜（已浪费一轮）。
//    - `fake_focus_dll.cpp` 在 `mapleDiag` **高位**写运行期命中位（低位 0x1..0x10000 是安装位，勿混用）：
//      0x20000 GetKeyState 被调用过 / 0x40000 GetKeyboardState / 0x80000 GetCursorPos /
//      0x100000 GetProcAddress / 0x200000 GetProcAddress 的 IAT 槽已补 /
//      0x400000 dinput8|dinput 的 user32 IAT 补到过槽。
//    - 宿主在 `冒险岛钩安装` 行尾输出 `pollHit=` 人话摘要 + `gpaIat=` + `dinputIat=`。
//    - 判读：`pollHit=无` 且 `gaks=0 diState=0` ⇒ 客户端不走任何被拦的 API（**不是**钩子没装上），
//      别再往「补钩子」方向使劲；`pollHit=GetCursorPos` 但无键态项 ⇒ 客户端确实在轮询 Win32，
//      键态走的是别的入口（打包器手搓导出解析时只能上方法体 JMP，而冒险岛明令禁止）。
//    - `iatPoll=2` 是**异常值**：本地 dinput8 存在时应 ≥4（主程序 2 + dinput user32 的 GAKS/光标 2）。
//      成因是 dinput8/dinput 懒加载、PEB 那一轮还没进进程 —— `InstallMapleIatHooks` 末尾已在
//      DI 虚表阶段之后补走一次 `MapleIatWalkGameDirDinputUser32()`。
//    - 构建前提：`src/window_mode/fake_focus/build_fakefocus32.cmd` 必须真能编出 32 位 DLL。
//      **禁止**用 `if defined ProgramFiles(x86)` 直接判断（括号会打断 if 解析，BuildTools 装在
//      「Program Files (x86)」时永远走 not found，32 位 DLL 被静默跳过 —— 已修，勿回退）。
// =============================================================================
