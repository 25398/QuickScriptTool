#pragma once

// =============================================================================
// 窗口/后台窗口模式硬性需求（用户约定）
// -----------------------------------------------------------------------------
// Agent / 后续优化改 `src/window_mode/**` 时必须遵守。违反即回归。
// 自检入口：WindowModeSelfTest --json / --json --macro
// Skill：.cursor/skills/window-mode-debug/SKILL.md
//
// 总原则：窗口模式 / 后台窗口模式执行时不得影响用户正在做的事。
//
// 1) 独立桌面模式（executionKind=hiddenDesktop）的工作面是「鼠标宏」虚拟桌面
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
//
// 10.1) 输入路径体检（2026-09-24 加）：`gaks/gfw/diState` 全 0 只能说明「不走这些入口」，
//    回答不了「那走哪条」—— 于是只能继续补钩子（§10 已警告别这么干）。现在 DLL 额外数
//    消息泵调用与消息类别，宿主打一行 `冒险岛输入体检`：
//      泵     = Peek/Get/Dispatch/CallWindowProc 命中次数（=0 ⇒ 客户端连消息都不经我们的钩子，
//               属「晚解析/缓存指针」，靠下面 10.2 的周期补挂兜）；
//      WM_INPUT > 0   ⇒ Raw Input：**后台天生收不到**（真后台走不了路，只能前台/假前台）；
//      WM_KEYDOWN > 0 ⇒ 消息驱动：后台不动 = 客户端自己按激活态门控；
//      软键按下 = 宿主当前在共享内存里标记按下的键数（证明「宿主确实在喂键」）。
//    - **禁止**为了这条路去灌假 WM_INPUT / RegisterRawInputDevices(INPUTSINK)：星辰冒险岛实测立刻闪退（§11）。
//
// 10.2) 周期补挂（`StartMapleRescanThread`，2026-09-24 加）：注入只做一次，而客户端的输入
//    路径是**逐步长出来**的 —— dinput8/dinput 常晚于注入才加载（日志 `dinputIat=0`）、
//    打包器把 Peek/GAKS 之类**先解析后缓存**（缓存点在扫描之后才写）。会话期间按节拍重跑
//    同一批**幂等**补挂（`MapleIatPatchSlot` 对已补槽直接返回，不会二次包装 detour），
//    新增数量写进共享内存 → 宿主日志 `rescan=N轮/+M槽`。判读：`+0` 说明路已铺满。
//    - 必须 SEH 兜住（与安装同一理由：任何一次访问违例先保游戏）。
//    - 消息泵/激活查询 API 也必须在 poll 组里（`MapleIatFillPollPairs`）—— 失活消息正是靠
//      这几个钩子吞的，它们没被补上时「客户端失焦后永远不再轮询」就无解。
//
// 10.3) 进程级指针扫描的覆盖面：**只读数据页也要扫**（2026-09-27 扩；宿主日志仍看 `pwPoll=`/`+M槽`）
//    - 判据来源：`冒险岛输入体检 泵=0 … rescan=3轮/+0槽` ⇒ 客户端既不走我们补过的 IAT，
//      也没有「晚出现」的可写缓存点。而打包器/保护壳的典型形态是「解析出 API 地址 → 写进
//      缓冲区 → `VirtualProtect(PAGE_READONLY)`」（或直接落在只读段）⇒ **只扫可写区的扫描
//      永远看不到这些指针**。现在两个 API 组都扫 `PAGE_READONLY`。
//    - 硬规则：**不扫可执行页** —— 代码页里 8 字节对齐的值可能是指令的立即数/常量，
//      改它 = 改指令语义；这个客户端有明确闪退史，代码页与只读数据页必须区别对待。
//    - 硬规则：只读页**额外收窄**到「匿名内存 + **游戏目录内**模块」——系统 DLL 的只读段装的是
//      **系统自己的分派表**（user32 内部就存着 GAKS/PeekMessage 的地址），改它会波及进程内
//      **所有**调用方（含保护/反作弊模块），收益不变而爆炸半径大得多
//      （同类教训：宽扫把网络栈时钟一起虚拟化 ⇒ 心跳错乱掉线）。
//    - 硬规则：可写页的既有覆盖面**不许顺手缩**；单次扫描 80ms 预算 + **游标轮转**
//      （既避免重扫卡住游戏，又避免「每次从头扫 ⇒ 后半段永远轮不到」）。
//    - 顺带修的隐患：`MapleIatPatchSlot` 临时放开保护时**必须保住执行位**
//      （原先一律 `PAGE_READWRITE`：RWX/代码页被改成 RW 的那一瞬间**执行权限没了**，
//      正好有线程在该页执行就是一次取指异常）。
//    - 待补：还没有专门的自检用例钉「只读页里的缓存指针会被补上」（现有 `window_time_scale_iat`
//      的缓存指针断言覆盖的是变速那一组，不是 maple 的进程级扫描）。
//
// 14) 窗口变速必须能回答「时钟变了但游戏没变快」是哪种失败
//    - 判据来自 LESSONS §3：计数不涨=钩子不在调用路径；猛涨但不变速=时钟不是决定它的东西。
//      后者需要**目标主循环节拍**才能落实：宿主读消息泵计数增量，打
//      `目标主循环节拍：N 次/秒（X 倍时钟下）`。节拍随倍率涨 ⇒ 游戏在按虚拟时钟跑；
//      节拍不涨 ⇒ 帧率被 vsync/固定帧/Sleep 封顶，时钟补丁天生改不动它。
//    - 冒险岛走 IAT-only（禁改函数体/方法体），且**只补游戏目录模块**：宽扫（连系统 DLL 的
//      IAT 槽一起补）会把网络栈的时钟也虚拟化 ⇒ 心跳错乱 ⇒ 掉线（用户实测）。
//      所以「让游戏变快」不能靠重新放宽扫描，要看上面的节拍数据再定（解锁帧率 or 放弃变速）。
//
// 15) 浏览器原生消息宿主：**path 只能是产品 exe**（2026-09-24 无限弹窗事故）
//    - 事故链：`RegisterExtNativeMessagingHost()` 用「当前进程 exe」写宿主 ⇒ 自测/诊断 exe
//      也会起扩展桥常开服务、且**每 15s 重注册** ⇒ 宿主被写成 `WindowModeSelfTest.exe`
//      ⇒ 浏览器每次 `connectNative` 都拉起它 ⇒ 它忽略浏览器参数**跑整套自测**
//      ⇒ 用户体感「对话结束还在不停弹 notepad/txt」。（同源事故：早先 `QstPlayer.exe` 进程风暴。）
//    - 硬规则：只有 `QuickScriptTool.exe` / `QstPlayer.exe` 配当宿主（`NativeHostExeNameIsProduct`）；
//      当前进程不是产品时退回同目录主程序；都没有就**一个字节都不写**（清单与 HKCU 都不碰）。
//    - 第二道护栏：`tools/selftest_harness.h` 在**静态初始化期**识别外部启动器参数
//      （`chrome-extension://` / `--parent-window=` / `--type=`）⇒ 所有 `*SelfTest.exe` 一律
//      不跑用例、打一行说明后 exit 0（浏览器能带任意参数拉起 exe，这道必须留着）。
//    - 对应用例：`native_host_manifest_points_to_product` / `selftest_refuses_foreign_launcher`
//      （WindowModeSelfTest；A/B 已验证：把清单改回自测 exe ⇒ 对应用例转红并点名路径）。
//
// 16) 冒险岛后台会话：**停摆看门狗 + 循环对齐 + 目标身份**（2026-09-29，A/B/C 三项）
//    - 判据（用户 09-28/09-29 日志）：客户端只有两种状态 ——
//      **活跃态** `泵=2924 WM_KEY=172 gaks=255 软键按下=1 rescan=10轮/+0槽`；
//      **休眠态** `泵=0 gaks=0 软键按下=0`（彻底停摆，喂什么都没用）。
//      用户实证「手动点一下游戏窗口就恢复」⇒ 把叫醒自动化。
//    - ⚠ 判「在不在动」**不能只看 `diState`**：活跃态可以是 `WM_KEY=172 gaks=255` 而
//      `diState=0`（走消息路径、没走 DirectInput）。判据是 `MapleClientProgress()` 的**多计数和**；
//      只认 diState 会把活的误判成停摆 ⇒ 反复抢前台。
//    - A `WaitMapleClientAlive`（看门狗）：会话中每 ~5s 检查一次（挂在 `PostKeyToTarget`
//      这条每拍都走的路径上，判据只是一次整数比较），停摆就补一次真激活
//      （复用 `WakeMapleStoryInputPolling`，马上还前台）；**连续两次叫不醒退避到 30s**。
//      日志：`…没在动…补一次真激活叫醒` / `已叫醒` / `仍未见客户端活动`。
//    - B 循环对齐：**每个循环开始**（用户的宏每轮都重新 BeginRun）先等客户端真的在动
//      （`WaitMapleClientAlive(..., 2000, true, L"循环开始")`）再放行动作 —— 取代原先盲等 600ms，
//      那正是「下个循环开头在原地打一会」的来源。
//    - C 目标身份：`LogMapleTargetIdentity` 打 hwnd/pid/类/客户区/标题 + **同类名顶层窗口数**
//      （双开时唯一能证明「输入投给了哪一份」的东西）；hwnd 变化时大声报警。
//      ⚠ 本项目前**只报告、不改判据**（绑定逻辑没动）：若日志显示投给了错的那一份，
//      下一步才在绑定处加「同类名多窗口 ⇒ 必须消歧」，别凭猜收紧绑定。
//
// 11) 注入安全：**注入绝不允许把游戏带走**
//    - 用户现场「注入后游戏立刻闪退」的头号原因是**同一个游戏进程反复注入**：IAT 槽被两套
//      detour 覆盖、DI 方法体 JMP 叠加、卸载时各按自己保存的原始字节回写。现场特征很好认：
//      日志里 `pid`/`hwnd` 跨小时甚至跨天完全不变（用户一直没重启游戏）。
//      `fake_focus_injector.cpp` 在注入前用 `TargetHasStaleFakeFocusModule()`（Toolhelp 按模块名）
//      查残留并**大声警告**「请先完全退出 MapleStoryt.exe」。只警告不阻断，便于仍取到诊断。
//    - 装钩子必须包 SEH：`InstallMapleIatHooksGuarded()` → `__try InstallMapleIatHooks()`
//      `__except` 置 `kMapleInstallFault(0x0800000)`。任何一次访问违例先保游戏。
//    - 安装阶段号写进 `mapleDiag` 最高 8 位（`MapleSetStage`）：1=入口 2=指针就绪
//      3=PEB/IAT 扫完 4=DI 钩完 5=全部完成。宿主日志 `stage=N fault=N` 就是死亡点。
//    - **禁止**再用「文件大小 == 魔术数」判 DLL 新旧：源码一改大小就漂移，曾把 163840 判成
//      「旧」而把源码注释里明写「闪退」的 164352 当成「现行」，把排查整轮带偏。以共享内存
//      `diag`/`hitReady`/`stage` 为准。
//
// 12) 后台窗口模式**不得**退化成「会抢鼠标的假后台」
//    - 症状（MC / GLFW30 实测）：用户关掉「启用假焦点注入」+ 开着窗口变速 ⇒ 引擎只注入时钟补丁
//      ⇒ 没有假焦点钩 ⇒ `PreferHardwareInput()` 为真 ⇒ 回退「假前台 SendInput（绝对坐标）」，
//      **抢走鼠标/键盘**，日志打「假焦点未生效（未注入 / 仅时钟补丁 / 钩已拆），回退假前台 SendInput」。
//    - 硬规则：`ShouldInjectTimeScaleOnly(timeScaleWanted, fakeFocusNeeded)` = `timeScaleWanted && !fakeFocusNeeded`。
//      **判据里不得出现「用户关掉了假焦点注入」** —— 需要假焦点的目标只装时钟补丁必然退化成假后台。
//    - 硬规则：后台模式 + 需要假焦点的 3D/游戏目标（`BackgroundTargetRequiresFakeFocus`）**忽略**
//      「关闭假焦点注入」设置，理由与微信/冒险岛一致：没有假焦点就没有真后台。
//      边界：仅 `BackgroundWindow`；`HiddenDesktop`（宏桌面）保持原行为（那时用户本来就不在这台桌面操作）。
//    - 对应用例：`background_fake_focus_not_degraded`（WindowModeSelfTest）。
//
// 13) 方向键兜底真键：**松开必须按「当初补过没有」，不能按「此刻是否前台」**
//    - 症状（用户原话）：「在游戏前台启动，再去浏览器看视频，就会朝离开时候的那一个方向 瞬移」。
//      根因：`SendKeyboardKey` 的判据原来每次调用都重算 `TargetOwnsForegroundWindow`。
//      按下时游戏在前台（补了真键 ↓）、用户切去浏览器后才收到 KEYUP（那时已不在前台）
//      ⇒ 永远不发 KEYUP ⇒ **真键永久卡在按下状态**：游戏朝那个方向一直走，
//      而且整个系统都认为该键被按住（切回任何程序都在持续按方向键）。
//    - 硬规则：`ShouldMirrorNavKeySend(down, targetOwnsForeground, mirroredDown)`
//      = `down ? targetOwnsForeground : mirroredDown`。
//      按下仍只在目标为前台时补（后台补会打进遮挡窗，见第 9 条）；
//      **松开一律按 `mirroredDown`（本会话是否真的补过 KEYDOWN）决定**。
//    - `ReleaseMirroredLcaNavKeys()` 在 BeginRun 开头与 EndRun 各调一次：脚本中途停止 /
//      目标闪退 / 上一轮本进程异常结束都可能留下没配对的 KEYDOWN，收尾必须松开。
//    - 对应用例：`lca_nav_keyup_released_after_focus_loss`（WindowModeSelfTest，已做 A/B）。
//
// 14) 残留注入模块的判定必须**比完整路径**
//    - 同一进程里挂**两份不同路径**的 FakeFocus（用户换过多次部署目录）才会双重挂钩
//      （IAT 覆盖两次、DI 方法体 JMP 叠加）并直接闪退；**同一份文件**则复用已装实例，是安全的。
//      只比模块名区分不出这两种情况。
//    - `TargetHasStaleFakeFocusModule(pid, outPath)` 回传 `MODULEENTRY32W::szExePath`；
//      日志把「本轮要注入的」与「进程里已有的」两个路径一起打出来，并注明是否同一份文件。
//    - DLL 侧 `InstallCommon` 的**早退路径**（`g_installed` 已为真）必须重新 `OpenSoftInputView`
//      并 `MaplePublishHits()`：宿主每轮 BeginRun 都会 Attach（先 Detach 再建）共享内存，
//      不重开就会让宿主读到全 0（`stage=0 hitReady=0 diag=0`），看着像「注入没生效」，
//      实际只是没人往新映射里写。**诊断读数全 0 时先排除这一条。**
//
// 15) 「最大子表面」兜底不得吃掉**真输入控件**；已绑定的子控件不得被无 config 重解析覆盖
//    - 症状（用户反馈原文，1.3.3 起）：「后台窗口模式按键点击不生效，按 A 打不到其他应用的
//      后台里面」。
//    - 根因：现代记事本（WinUI3）实测树 `Notepad`(top) └ `NotepadTextBox`(755x553)
//      └ `RichEditD2DPT`(755x553)。父子客户区**一样大** + `EnumChildWindows` 是「父先于子」
//      + 最大值判据用**严格大于** ⇒ `FindBackgroundInputChild` 的「最大后代」启发式取到
//      **包装层** `NotepadTextBox`；`ResolveSoftInputHwnd` 又用**无 config** 的重解析把
//      已经绑对的 `RichEditD2DPT` 覆盖掉。**PostMessage 不会向子窗转发** ⇒ 投给包装层
//      等于完全没投（本机实测：`WM_CHAR` 给 NotepadTextBox → 一个字都不进；
//      给 RichEditD2DPT → 正常进字；给顶层 Notepad → 也不进）。
//    - 硬规则 ①：「最大后代」若**包含**真文本控件（`IsChild(surface, input)`）⇒ 让位给真控件。
//      判据必须是 `IsChild`，**不是**「窗口里有没有输入框」—— 后者会把投递目标从主区域
//      挪到别处的小搜索框（既有选择被无理由改掉）。
//    - 硬规则 ②：已绑定到**子控件**时，投递路径不得再用「最大后代」重解析覆盖它
//      （绑定是 `ResolveBindHwnd` 带 config 选的，比无 config 重解析可信）。
//      唯一例外：绑到的只是包装层 ⇒ 让位给里面的真控件（同上）。
//    - 对应用例：`background_input_wrapped_text_control` / `background_input_bound_child_respected`
//      （WindowModeSelfTest，均已做 A/B）。
//
// 16) WinUI（XAML）文本控件**自译** WM_KEYDOWN ⇒ 不得再补 WM_CHAR
//    - 实测 `RichEditD2DPT`（2026-09-23 本机）：只 `KEYDOWN(A)` → `'a'`；只 `WM_CHAR('a')` → `'a'`；
//      `KEYDOWN(A)+WM_CHAR('a')` → **`'aa'`**（一次变两次）。修好第 15 条后这个才会显形。
//    - 硬规则：该类目标下**可打印字符（`ch >= 0x20`）只发 `WM_CHAR`**（字符由宿主用软修饰键态
//      译好 —— 控件的自译只看**真实键态**，脚本按住的 Shift 它看不见，Shift+A 会退化成 'a'）；
//      **其余只发 `KEYDOWN`**（实测 `WM_CHAR` 不换行/不制表；退格/删除/方向键/功能键
//      `SoftVkToChar` 返回 0，本来就只走 KEYDOWN）。只发过 `WM_CHAR` 的键**不得**再补 KEYUP。
//    - 判据抽成纯函数 `ClassSelfTranslatesPostedKeys` / `SelfTranslateKeyUsesWmChar`
//      （`window_mode_types.h`）。
//
// 17) 窗口相对录制产出的 `windowName` 只作参考，**不得**当硬匹配门
//    - 症状（用户反馈原文）：「**窗口模式自动识别，录制回放，不操作后台**」
//      —— 后台窗口模式 + 自动识别录制出来的脚本，回放时按键/点击到不了目标。
//    - 根因：录制保存把**录制瞬间的窗口标题**写进 `windowName`
//      （`SaveScriptFileData`：`wm.windowName = wmTgt.windowTitle`），回放端
//      `BuildTargetQuery` 对 `UseEditorWindowClass` 分支把它按 `" - "` 截成 stem 当
//      `titleContains`，而 `EnumWindowsOnDesktopProc` 里标题匹配是**硬门**
//      （排在类名匹配之前）。标题是**易变量**：换文档 / 换标签页 / 游戏换场景 / 存档改名
//      ⇒ 枚举**一个窗口都命中不了** ⇒ 绑不到目标 ⇒ 表现为「不操作后台」。
//      ⚠ 那不是投递坏了，是**根本没绑上** —— 排查时先看绑没绑到，别去动 DLL。
//    - 实测复现：`tools/verify/probe_record_playback_bind.py`
//      （同一窗口、同一类名，仅标题变化 ⇒ 命中 0；摘掉标题过滤 ⇒ 命中 1）。
//    - 硬规则：窗口相对录制保存时置 `windowNameIsHintOnly = true`；`BuildTargetQuery`
//      在 hint-only 下**不产生** `titleContains`，`DoesTopWindowMatchConfig` 也**不拿标题**
//      判「没绑到」。身份判定退化为 **进程路径 + 顶层类名(+子窗类名)**。
//    - 向后兼容：该字段默认 `false` ⇒ 旧脚本 / 用户手配（「指定窗口类」+ 标题关键词）
//      行为**完全不变**，标题仍是硬门。
//    - 对应用例：`background_recorded_title_is_hint_only`
//      （WindowModeSelfTest；含真查找接口 `FindMainWindowDefault` 的端到端 A/B）。
//
// 18) 「脚本正按着的键」**不是**陈旧位：停摆清理不得清它，计数夹顶不得判停摆
//    - 症状（用户原话，2026-10-02）：录制回放与鼠标宏的后台窗口模式「有时候会吞动作，
//      后台就原地打 然后往左打 偶尔会往右 但直走一小段 就开始往左」。
//    - 根因（两条，缺一不成灾）：
//      ① **计数夹顶**：`MapleBumpHit` 历史写法 `if (v > 255) InterlockedExchange(c, 255)`
//         ⇒ `gaks` 一到 255 就**永远不再变**。而 `EvaluateKeyStatePhase` 用
//         `nowGaks != prevGaks` 判「客户端还在不在查键态」⇒ 夹顶后**每轮看门狗都判停摆**。
//         日志铁证：`冒险岛钩命中 结束前 … gaks=255`（两次独立运行都恰好 255 = 钳位值）。
//      ② **清理误伤脚本意图**：判停摆后 `ClearStaleArrowSoftKeys()` 清掉**所有**按下中的方向键。
//         原注释的前提是「客户端此刻不轮询 ⇒ 清掉不影响任何生效输入」—— 前提在误判时**不成立**。
//         日志铁证：`持键 2 个` 而 `已清方向键陈旧位（1 个）`。录制宏在 t=2.03s 按下 →
//         并一直按到 t=9.55s ⇒ 那 2 个持键 = {→, C}，被清掉的 1 个正是 **→** ⇒ 「原地打」。
//      ③ 且因判据（计数变化）再也无法变真，「轮询恢复」**永不触发** ⇒ `ResyncSoftHeldKeys`
//         从不执行 ⇒ 被清的 → **永久丢失**（不是"晚点会补回来"）。
//    - 硬规则：
//      · **计数必须单调**：`MapleBumpHit` 不得夹取（打包诊断 API 本来就逐字段 `& 0xFF`，
//        去掉夹取不影响它，也不影响 `MapleClientProgress()` 的"变没变"语义）。
//      · **判据要能识别失效**：计数恰好停在同一值且该值是旧钳位值 255 时 ⇒ 返回 `None`（不判）。
//        用 `==` 而非 `>=`：新版计数会长期 `> 255`，`>=` 会把整个功能永久废掉。
//      · **绝不清脚本意图**：`ClearStaleArrowSoftKeys(held)` 必须排除 `held`（= 脚本此刻按着的键）。
//        陈旧位 = 「没人认领却还按着」，判据就是「不在 `held` 里」。
//      · **影子集更新必须在看门狗之前**：否则触发看门狗的那一次按下还没进集合，仍会被误清。
//    - 一般化：**"清理陈旧状态"的判据必须是"没人认领"，不能是"看起来没人用"**。
//      一旦清理对象与"用户当前意图"可能重叠，就必须显式排除意图集合 ——
//      否则误判一次的代价是**不可逆的意图丢失**（本项目里 = 角色乱走，用户只能重录）。
//    - 对应用例：`maple_keystate_stall_resync`（WindowModeSelfTest）
//      新增两格：① `EvaluateKeyStatePhase(255,255,false/true) == None`（夹顶不判停摆）；
//      ② `ClearStaleArrowSoftKeys({VK_RIGHT})` 下被持的 → 必须**原封不动**、无人认领的 ← 照清。
//      两格在修复前**必红**（旧实现分别返回 `Stalled` 与 `cleared=2`）。
//
// 19) Adobe AIR（造梦西游 / 4399 微端）有**两条**独立的「把游戏带走」入口（2026-10-03）
//    - 用户报障原文：「给微端使用后台窗口模式后游戏微端崩溃」（用的是历史构建，
//      `targetExe=…\4399\GameLogin\Zmxyol\zmxy_online.exe`）。
//    - 现场取证要点：用户给的那份日志**只到**「未找到目标窗口，自动打开」→
//      「后台指定窗口类：无文档路径，仅绑定已有标题窗」→ `EndRun`，
//      **一个「假焦点」行都没有** ⇒ 那一轮压根没走到注入，它是**崩溃后的善后轮**
//      （微端已经没了，所以「未找到」）。⚠ 排查这类报障时先问一句
//      「日志里有没有 `假焦点已注入` / `目标窗口已消失 … exit=`」，
//      没有就说明**崩溃发生在更早那一轮**，别在这份日志里找根因。
//    - 入口 ①：**AIR 识别只看顶层窗类名**。`ApolloRuntimeContentWindow` 常常是
//      **内容子窗**（父窗是启动器/包装窗，4399 微端就是这个结构），而
//      `InstallCommon` 拿到的是 `GA_ROOT` ⇒ 漏判 ⇒ 走**全量 Phase2**
//      （子类化 + 光标钩 + RawInput）⇒ AIR「一启动就卡死退出，鼠标原地抽」。
//      修法：`HwndTreeLooksLikeAdobeAir(top)` —— 顶层**或任一子窗**命中即算 AIR，
//      与宿主侧 `adobeAir`（同时看 `windowClassName`/`childWindowClassName`）对齐。
//    - 入口 ②：**变速时钟钩是 `airSafe` 早退之外的**。`InstallCommon` 里
//      `timescale::InitRealTimeApi()/StartPoll()` 在 `if (airSafe) return TRUE;`
//      **之前**执行，且非 IAT-only 模式下 `HookModuleIatTargets` 会把
//      **所有模块**解析到的时钟函数地址都做**内联钩（改函数体）** ⇒ AIR 这种
//      已知脆的目标被改了代码页（同 `fake_focus_time_scale.h:481` 记的
//      「调用面全虚拟化 ⇒ 面太大 ⇒ 开倍速过一会就闪退」）。
//      修法：`SetIatOnlyMode(mapleSafe || airSafe)` —— AIR 与冒险岛同待遇，
//      只补 IAT 槽（数据），代码页一个字节都不动。
//    - 入口 ③：`SetWindowsHook` 注入在**目标 UI 线程**里 `LoadLibrary`。这条
//      闸门原来只列了 GLFW/模拟器/微信/Chromium/3D，**漏了 AIR**。默认技术是
//      classic ⇒ 日常测不出来，但设置里能选 `setwindowshook` ⇒ 一选就带走。
//      修法：判据收进 `ForbidsSetWindowsHookTechnique()`（`window_mode_types.h`），
//      新增脆弱目标**只改那一处**。
//    - 对应用例（`WindowModeSelfTest`）：
//      `fake_focus_air_child_iat_only`（包装窗 + AIR 子窗：不子类化 + 变速 bit6=1；
//      负对照 `STATIC` 窗必须 bit6=0）、
//      `setwindowshook_not_for_fragile_targets`（逐一命中 + 普通目标放行）。
//    - ⚠ 一般化：**「只对某类目标生效的白名单」必须只有一个来源**。本次两条入口
//      都是「宿主认了、DLL 没认」/「早退在安装之后」这类**顺序与来源错位**，
//      不是逻辑写错 —— 而它们只在「用户恰好选了非默认选项 / 恰好是多层窗口结构」
//      时才复现。加新目标类型时，先问「它在**每一条**白名单里都登记了吗」。
//
// 20) 注入不得把目标带走（二）：**调用原函数不许改写目标函数头**（2026-10-03）
//    - 用户报障原文：「当前项目，后台窗口模式/窗口模式会导致一些窗口闪退」。
//      现场日志：Unity 游戏（`UnityWndClass`，`CreatureCurios.exe`）假焦点注入完成后
//      **第一次执行鼠标宏**目标即消失，`exit=0xC0000005`。
//    - 根因：`fake_focus_hook.cpp` 的 `CallThroughOriginal()` 用
//      「**还原 12 字节 → 调用原函数 → 重写 12 字节绝对跳转**」来调原函数，
//      而 x86-64 上 **12 字节写入不是原子的**（只有对齐的 8 字节以内才是）。
//      目标（Unity/UE 这类多线程游戏）在渲染线程 + 主线程上同时调用被钩函数时，
//      会取到「半个跳转」（`48 B8` + 部分地址 + 旧字节）⇒ `mov rax,<垃圾>; jmp rax`
//      ⇒ 跳到非法地址 ⇒ 目标进程当场消失。同一窗口期还会**静默漏钩**
//      （别的线程绕过 detour）⇒「后台输入时灵时不灵」。
//    - 复现（`tools/verify/hook_race/`，直接编译真实实现）：
//        1 线程 → PASS（无并发，还原窗口没人撞上）
//        2 线程 → av≥1（0xC0000005）+ 漏钩十几万
//        8 线程 → 进程直接段错误
//      ⇒ **单线程全绿**，所以它跟「12 字节覆盖越界」无关，是纯粹的并发写入竞态。
//    - 修法：原函数一律走 **trampoline**（MinHook，`third_party/minhook/`，BSD-2）：
//      目标函数头**只在安装时写一次**（`MH_EnableHook` 还会先挂起其他线程再落补丁），
//      之后永远只读。顺带修掉「固定覆盖 12 字节、不看指令边界」（MinHook 用 HDE32/HDE64
//      解码到完整指令并处理 rip-relative 重定位）。
//      所有 `CallOrigXxx` 的第一个参数就是 trampoline，按原签名转型后直接调用。
//    - 回归：`python tools/verify/hook_race/run_hook_race.py`（1/2/8 线程全绿）+
//      `negcontrol_hook_race.py`（**修复前 2/8 线程必红**，否则说明断言覆盖不到）。
//      ⚠ 负对照必须落在**副本**上（`hook_race/legacy/`），别在 `src/` 上做变异。
//
// 21) 注入不得把目标带走（三）：**不许在「装完就退的临时线程」上 attach 第三方运行时**
//    - `InstallCommon` 曾**无条件**调 `timescale::InitRealTimeApi()`，后者又调
//      `fakefocus::unity::Init()` —— 它会 `il2cpp_thread_attach` **当前线程**、
//      遍历所有程序集、`il2cpp_runtime_invoke(Time.get_timeScale)`。
//      而这个「当前线程」是 `CreateRemoteThread` 出来的**注入线程**，执行完
//      `FakeFocus_Install` 就退出，**没人 `il2cpp_thread_detach`**
//      ⇒ IL2CPP 线程表里留下已终止线程的记录 ⇒ 下一次 GC 扫栈 = 访问违例
//      ⇒ 整个游戏进程消失、无日志（`exit=0xC0000005`）。
//      非 Unity 目标只是白付一次遍历程序集的开销。
//    - 修法：`unity::Init()` 从 `InitRealTimeApi()` **移走**，改成**惰性** ——
//      只在 `ApplyScale(num)` 且 `num` 非原速时才解析；该函数只由**轮询线程**调用
//      ⇒ attach 落在常驻线程上，`PollThreadProc` 退出前 `il2cpp_thread_detach`。
//      `Restore()` 会被 `StopPoll()` 在**调用方线程**（宿主的远程线程）上执行 ⇒
//      用 `t_attachedThread` 判断「本来 attach 过没有」，用完就还。
//    - 一般化：**无条件初始化 = 把「用户没开的功能」的风险全吃下**。
//      加任何「目标进程内初始化」前先问：它跑在哪个线程上？那线程什么时候退？退了谁收尾？
//
// 22) 注入期的失败原因**必须能被用户导出** —— 「诊断日志的可见性」也是功能的一部分（2026-10-03）
//    - 用户报障原文：「冒险岛的后台窗口，还是原地不动的平A，不能走A」。
//      （「还是」= §16 / §18 同症状的第三次报障。）
//    - 现场（**决定性**：同一份导出文件里就有 A/B 对照）：`先前台在后台diagnose_report.txt` 里
//      10-02 21:44–21:46 用的是 `QuickScriptTool.exe=7452672@10-01 23:58`，
//      **每轮都有** `冒险岛钩命中 注入后` / `冒险岛输入体检 注入后` / `冒险岛钩安装 注入后`；
//      10-03 22:33–22:36 用的是 `7519232@10-02 23:41`，**每轮一行都没有**（只剩
//      `BeginRun：窗口模式启用` / `BeginRun：生效` / `构建指纹` / `EndRun`）。
//      同一台机器、同一个游戏、同一批脚本 ⇒ **唯一变量是构建**。⚠ 别只看一个构建的日志。
//    - 后果链（与 §9/§12 联动）：假焦点注入失败 ⇒ `PreferHardwareInput()` 在
//      `UsesBackgroundWindow()` 下**永不回退假前台 SendInput**（「后台永不抢鼠标」）
//      ⇒ 键鼠只剩 PostMessage ⇒ **消息驱动的攻击键照打、方向键（依赖
//      `FakeFocusSoftInput_SetKey` 写共享内存 + DLL 内 DI 软键）全部失效**
//      ⇒ 精确复现「原地不动的平A，不能走A」。
//    - 为什么用户**看不到失败原因**（三个通道问题叠加，缺一不可）：
//      ① `diagnose_report.txt` 只抓 `window_mode_debug.log` 的最后 120 行，而
//         `window_mode_debug.log` 只收 `WindowModeLogEvent*`（`AppendPersistentEvent`）；
//         注入期的失败原来一律走 `WindowModeLogf`（**非 Event ⇒ 不落盘**）。
//      ② 宏调试窗那条 sink 用 `runningWindowMode_.enabled` 过滤，而嵌套模式的
//         `publishRunningWm()` 原来排在 `beginWmCfg()` **成功之后** ⇒ 注入期
//         （绑窗 / 注入 / 失败）的日志**全被 sink 丢弃**。
//      ③ DLL 早退路径写的是 `if (!g_softView) OpenSoftInputView(pid);` ——
//         `g_softView` 非空 **≠** 视图有效：宿主每轮重建映射后，旧 view 可能仍指向
//         一个**布局已变**的旧 section（`SoftInputState` 加过字段，`kSoftInputVersion`
//         从 7 抬到 10）⇒ `SoftInputStateLooksValid` 为假 ⇒ `MaplePublishHits()`
//         开头就 `return` ⇒ 宿主读到全 0（诊断行**凭空消失**）+ 软键态/DirectInput 全失效。
//    - 修法（四改 + 一函数 + 一用例）：
//      ① `fake_focus_injector.cpp`：`FakeFocusSoftInput_Attach` 失败、以及
//         「目标里已加载**另一份** FakeFocus 被拒」→ 升为 `WindowModeLogEventf`（落盘）。
//      ② 同文件新增**旧版残留告警**：磁盘 DLL 的写入时间 **晚于** 目标进程启动时间 ⇒
//         进程内必然是旧内容（`LoadLibrary` 同路径**只加引用计数、不重跑 DllMain**）⇒
//         提示「请完全退出游戏进程再运行」。判据收进纯函数
//         `InjectedModuleLooksStale(dllWrite, procStart)`（`window_mode_types.h`），
//         ⚠ **宁可漏报不误报**（任一为 0 即返回 false），且**只报警不阻断**。
//      ③ `window_mode_executor.cpp`：`InjectAndInstall` 失败 → `WindowModeLogEventf`。
//      ④ `engine_script_run.cpp`：`pushNestedUseMode` 里把 `publishRunningWm(cfg)`
//         **提前到 `beginWmCfg` 之前**（失败时 `restoreModeFrame` 会还原）。
//      ⑤ `fake_focus_dll.cpp`：早退路径**去掉** `if (!g_softView)` 守卫，无条件调
//         `OpenSoftInputView`（它内部已处理「非空但无效 ⇒ 关掉重开」，有效时 O(1) 返回）。
//      ⑥ 用例 `injected_module_stale_detection`（6 格，含 3 个负对照：0 值 / 相等）。
//      ⑦ **「假焦点决策」落盘**（`TryInstallFakeFocus` 开头，`WindowModeLogEventf`）：
//         打印 `mapleStory=? fakeFocusNeeded=? timeScaleWanted=? timeScaleOnly=? class=? targetExe=?`。
//         ⚠ 这一条是本次**最有价值**的补充：那三行注入诊断（`冒险岛钩命中`/`钩安装`/`输入体检`）
//         **只在 `lite && mapleStory` 分支里打**，而该分支前面有 **6 条会静默早退**的路径
//         （未登记游戏走 LCA / 不需要假焦点 / 内核反作弊 / 仅时钟补丁 / 远程桌面 / 关掉了假焦点注入）
//         —— 它们原来全是 `WindowModeLog`（非 Event）⇒ 用户导出的日志里
//         **「压根没尝试注入」和「尝试了但失败」长得一模一样**（都只剩 BeginRun/EndRun）。
//      ⑧ 上述早退分支**逐条升为 `WindowModeLogEvent*`**；`lite && mapleStory` 的
//         「冒险岛假焦点已注入」也升为 Event（**正向确认**）。
//         ⇒ 现场日志从此构成一个**完整三分**：
//         **「没尝试」（早退行）/「尝试了但失败」（⛔ 失败行）/「注入成功但 DLL 没发布」
//         （有「已注入」行、却**没有**紧跟「钩命中/钩安装」行 ⇒ 进程内 DLL 是旧的）**。
//    - 一般化（三条，比本次修复更重要）：
//      a) **「失败原因只写到非持久通道」= 现场没有证据**。凡是「用户会来报障」的失败点，
//         都必须走 Event（落盘）；`WindowModeLogf` 只配用来打**正常路径的细节**。
//      b) **「可见性」的判据是「用户能导出」**，不是「代码里搜得到」。写完先问：
//         用户按现有导出流程，**看得到这一行吗**？（前科：§79 的 48 字节描述预算 ——
//         同一族错误的另一张脸：**「我写了」≠「对方收到了」**。）
//      c) **同路径 ≠ 同一份内容**。任何「复用已加载实例」的路径，都要问一句
//         「进程里那份是不是我这份」；时间戳只是**启发式**（宁可漏报），
//         真正的兜底是**每次早退都重新建立共享视图**（本条的 ⑤）。
// =============================================================================

// =============================================================================
// 23) 坐标语义必须三端一致，且**必须能被日志判读**（2026-10-04）
//
//    - 用户报障（两条，同一根因）：
//      ① 「编辑宏的时候，后台窗口模式下鼠标不会移动到指定位置；默认模式能走完流程」
//      ② 「后台窗口模式貌似使用的坐标是绝对坐标，鼠标移动等按坐标移动的动作
//          在窗口移动后就不能使用了」
//      目标 = Unity（`UnityWndClass`），`输入策略=softMessage`，假焦点注入成功。
//
//    - 机制（**不是注入坏了**，是坐标语义错配）：
//      脚本按**屏幕绝对坐标**存（`windowRelativeCoordinates=0`）⇒ 回放时
//      `MapScriptPointToClient` 用 `ScreenToClientPoint(当前窗口)` 换算成客户区
//      ⇒ **偏差 = 窗口位移**。窗口不动时一切正常，所以这个缺陷**只在移动后暴露**。
//      日志指纹：每次移动的「脚本值 − 客户区值」**恒定**（本例恒为 (642,322)）；
//      若是窗口客户区相对，应为原值直传或**等比例**缩放（比例不等即排除缩放路径）。
//
//    - 三端现状（调查结论，别重复查）：
//      · **录制端已自洽**：`ConvertRecordedToActions` 传的开关是
//        `recorderWindowMode_ && GetRecordingWindowTarget().enabled`，与保存时写
//        `windowRelativeCoordinates` 的条件同源；且录制事件入队时就经
//        `MapRecordingPointToClientIfWindowRelative` 把屏幕坐标转成客户区。
//      · **编辑端**：`SyncScriptWindowModeFromEditor()` **不碰** coordSpace /
//        windowRelativeCoordinates / recordClientWidth/Height（保留既有值）——
//        所以「默认模式录的脚本 → 编辑时切成后台窗口模式」会保持 screenAbsolute。
//      · **回放端**：按 `windowRelativeCoordinates` 分流，本身没错。
//      ⇒ 缺口不在某一端写错，而在**「语义不匹配时没有任何提示」**（静默偏移）。
//
//    - 修法：
//      ① `window_mode_executor.cpp` BeginRun：打一行 `坐标语义 …`（Event，落盘），
//         并在 `!windowRelativeCoordinates && UsesBackgroundWindow()` 时追加
//         `⚠ 本脚本按屏幕绝对坐标回放…` 告警（说明偏移成因 + 建议重录/改窗口模式）。
//      ② `engine_window_mode_hooks.cpp` `SyncScriptWindowModeFromEditor()`：切到
//         后台窗口模式且脚本是 screenAbsolute 时 `promptModal_.ShowInfo` 提示一次
//         （静态标志防重复 —— 本函数调用很频繁）。
//      ⚠ **不做自动迁移**：旧脚本没有「录制时窗口位置」，信息已丢失，任何自动换算
//        都只是拿当前窗口位置去猜，会把「看起来能跑」变成「静默错位」。
//
//    - 诊断（同批改动，**通用假焦点目标此前完全没诊断**）：
//      那三行（`冒险岛钩命中`/`钩安装`/`输入体检`）原来**只在 `lite && mapleStory`
//      分支打** ⇒ Unity/UE/GLFW/模拟器目标日志里**一条命中数据都没有**，
//      「后台鼠标不动」只能靠猜。本次：
//      ① `native3d` 分支也调 `LogMapleHookHits(L"注入后")`（文案沿用历史名称，
//         被 skills/LESSONS 多处引用，**不要改字面**；已加前缀说明消歧义）。
//      ② DLL `MapleNotePumpMessage` 去掉 `if (!g_mapleSafe) return;` 门闩，
//         通用泵钩子 `HookPeekMessage` 补 `MapleNotePumpMessage(lpMsg->message)`
//         ⇒ 非冒险岛目标的 `hitPump/msgInput/msgKey` 不再恒 0。
//      ③ 新增 `FakeFocusSoftInput_ReadSoftCursor()` + 一行 `冒险岛软光标 …`：
//         回答「**宿主有没有把光标喂进去**」。
//         ★ 这一条最关键 —— `SyncFakeFocusCursor()` 在共享内存没挂时是**静默 return**，
//           光看「假焦点已注入」永远发现不了（`cursorValid=0` ⇒ GetCursorPos 钩子
//           会回退真光标 ⇒ 后台移动必然无效）。
//
//    - 判读（用户导出日志后一眼定位）：
//      `cursorValid=0`             ⇒ 宿主没喂光标（宿主侧缺陷）
//      `泵=0`                      ⇒ 游戏的消息循环不经我们的钩子（缓存指针/晚解析）
//                                    ⚠ **但 `lite=1`（UE5 精简）时本就不钩泵 ⇒ 恒 0 属正常**，
//                                      先看「假焦点注入技术=… lite=?」再下结论
//      `WM_INPUT>0`                ⇒ 走 Raw Input（我们已伪造 WM_INPUT）
//      `WM_KEY>0` 且后台无响应      ⇒ 消息驱动 + 游戏自己按激活态门控
//      `gfw/gaks/diState` 恒 0     ⇒ ⚠ **那是冒险岛专用的命中位**（`MapleBumpHit` 只在
//                                    `g_mapleSafe` 时递增，本次**故意没放开**）⇒ 对 Unity/UE/GLFW
//                                    表示「**没人在数**」，**不是**「游戏不走这些入口」。
//                                    真正对通用目标有效的是 `泵/WM_INPUT/WM_KEY`。
//                                    （为什么不放开 `gaks`：它还被 `EvaluateKeyStatePhase`
//                                     当作「客户端在不在查键态」的判据，放开会改变既有的
//                                     陈旧方向位清理行为 ⇒ 有回归风险，故只改文案。）
//      三项都正常但游戏仍不动       ⇒ 该换方案（DirectInput 钩 / ViGEmBus / IL2CPP 层）
//
//    - ★★ **「诊断有效」要过五关**（2026-10-04 三轮自查的教训；改任何诊断前先逐关过）：
//      ① **调用点**：诊断函数真的被调到吗？
//         （前科：`结束前` 那次被 `LooksLikeMapleStoryTarget(...)` 挡住 ⇒ 非冒险岛目标
//           只剩「注入后」那一行，而那是注入瞬间打的、计数**必然全 0**）
//      ② **数据源**：计数器真的在数吗？
//         （前科：`MapleNotePumpMessage` 首行 `if (!g_mapleSafe) return;` ⇒ 恒 0，
//           补了调用也读不到东西 ⇒ **两层门闩必须一起查**）
//      ③ **落盘**：走的是 `WindowModeLogEvent*` 吗？
//         （前科：消歧义说明用了 `WindowModeLog` ⇒ 不落盘 ⇒ 用户日志里**只剩
//           「冒险岛钩命中」却没有那句说明**。⚠ `WindowModeLogf` **不是**落盘）
//      ④ **导出**：在导出脚本的抓取窗口内吗？
//         （前科：`tools/diagnose_start.cmd` 只抓 `-Tail 120`，而诊断行分布在**每轮开头**、
//           一轮约 80 行 ⇒ 跑两轮以上就被截掉。已放宽到 600）
//      ⑤ **失败不静默**：失败路径有没有 `return` 却不报？
//         （前科：`PrepareSoftInputFast` 5 个调用点里 4 个直接 `return`、1 个走不落盘的
//           `WindowModeLogf` ⇒ 键鼠被静默丢弃，现场只表现为「移动/点击 → 客户区」没出现）
//      ⇒ ①②是「写了但没数据」，③④是「有数据但传不到」，⑤是「失败没痕迹」。
//        缺任何一关，结果都是「日志里什么都没有 / 看不出问题」。
// =============================================================================
//
// §24 UWP 目标（计算器 / 商店应用）在「后台窗口模式」下的现状与排查路径
// =============================================================================
//   目标特征：`class=ApplicationFrameWindow`、`targetExe=...\ApplicationFrameHost.exe`，
//   内容窗是 `class=Windows.UI.Core.CoreWindow`（**另一个进程**，如 CalculatorApp）。
//
//   - ★★ **禁止注入假焦点**（2026-10-05 用户实测崩溃后加的护栏）
//     `ApplicationFrameHost.exe` 是**系统壳进程**，一个进程托管**所有** UWP 应用
//     ⇒ 注入影响面远超单个应用；实测「会话结束后目标窗口崩溃」。
//     判据：`LooksLikeUwpShellWindowClass()` / `LooksLikeUwpShellExecutable()`。
//     ★ **不影响功能**：UWP 的 `PostMessage` 本来就无效，它靠 **UIA Invoke**。
//     ⚠ 跳过后**不能**再报「★ 未拿到假焦点 …… 请放行 DLL」那条告警
//       （会把用户引向折腾安全中心）—— 已用 `uwpSkippedFakeFocus_` 区分
//       「主动跳过」与「注入失败」。
//
//   - **输入路径**：`PostMessage` **无效**（UWP 不响应）⇒ 点击必须走
//     `TryUiaInvokeAtScreenPoint()`（`background_uia_input.cpp`）。
//     ⚠ **移动**（`PostMouseMoveToWindow`）对 UWP **同样无效**，但
//       **UIA 点击不需要先移动** ⇒ 「移动无效」不影响点击，排查时别被它误导。
//
//   - **「点击没反应」的排查顺序**（2026-10-05 起每层都有日志）：
//     ① `UIA 兜底未执行：<原因>`（`window_mode_executor.cpp` 的 `LogUiaSkipOnce`）
//        ⇒ 没走到 UIA。原因：`PreferHardwareInput()` 为真 / 目标窗口无效 /
//          `ClientToScreenPoint` 失败。
//     ② `⚠ UIA 兜底失败于「<stage>」hr=…`（`background_uia_input.cpp` 的 `LogUiaFailOnce`）
//        ⇒ UIA 走了但失败，stage 直接指出哪一步：
//          · `CoCreateInstance(CUIAutomation)` ⇒ COM 线程模型
//          · `ElementFromHandle` ⇒ 句柄无效
//          · `FindAll(Descendants)` ⇒ 接口报错（看 hr）
//          · `FindAll 返回 0 个元素（重试 + Children 回退后仍为空）` ⇒ UIA 树为空
//          · `该点下没有支持 Invoke/Toggle 的 UIA 元素` ⇒ 坐标不在控件上，或该控件
//            不走 Invoke（需补 `LegacyIAccessible` / `SelectionItem` 等模式）
//     ③ `UIA 点击 屏幕(x,y) 客户区(x,y)` ⇒ **成功**（原来只有这一条，所以失败时
//        日志里连「试过 UIA」都看不出来）。
//     ⚠ `FindAll` 拿到 0 个时会**重试 3×60ms + 回退 `TreeScope_Children`** ——
//       UWP 的 UIA 树是**按需构建**的（跨进程 + 元素虚拟化），首次常为空。
//
//   - **临时替代方案**：改用「**窗口模式**」（非后台）—— 那种模式允许占键鼠，
//     走假前台 `SendInput`，**能驱动 UWP**，代价是运行时会抢占鼠标键盘。
// =============================================================================
