# 注入对抗测试框架（Injection Testing Framework）

> 用途：对窗口模式 DLL 注入进行**授权对抗性测试**，验证游戏/反作弊系统能否
> 识别与拦截各类注入路径。所有技术均为公开研究的双用途技术，仅用于对自有
> 系统或已获授权的目标做安全测试，禁止用于绕过他人游戏的反作弊、作弊或
> 恶意软件场景。

## 1. 背景：原注入路径与检测面

项目原有的窗口模式注入（`fake_focus` / `cdp_enable`）只有一种实现：

`OpenProcess → VirtualAllocEx(路径) → WriteProcessMemory → CreateRemoteThread(LoadLibraryW)`

这是反作弊最容易检测的路径，主要检测点：

| 检测手段 | 观察点 |
|----------|--------|
| 用户态 API 钩子 | `CreateRemoteThread` / `LoadLibraryW` / `WriteProcessMemory` 调用来源 |
| 内核线程回调 | `PsSetCreateThreadNotifyRoutine`：新线程起点在非映像区域 |
| 模块枚举 + 签名校验 | 目标进程模块列表出现未签名/可疑 DLL（`FakeFocus64.dll` 等） |
| 内存扫描 | 非映像 `PAGE_EXECUTE_READWRITE` 区域（注入缓冲区/桩） |
| 句柄访问监控 | 对目标进程的 `PROCESS_VM_WRITE/CREATE_THREAD` 句柄 |
| 驱动级 ETW/回调 | `LoadImage` 回调、镜像加载日志 |

本框架在 `src/window_mode/injection/` 提供了 7 种注入技术与 2 项附加能力，
用于对照测试上述检测面的覆盖情况。

## 2. 技术矩阵

统一入口：`windowmode::inject::InjectDll(pid, dllPath, Technique, opts, result)`

| 技术 | 枚举/CLI 名 | 原理 | 预期检测面（对照测试用） |
|------|------------|------|--------------------------|
| 经典远线程 | `classic` | `CreateRemoteThread` + `LoadLibraryW`（基线） | 全部：API 钩子、线程回调、模块列表、内存扫描 |
| NtCreateThreadEx | `ntcreatethreadex` | 直调 `NtCreateThreadEx` + `LoadLibraryW` | 绕过 `CreateRemoteThread` 用户态钩子；内核线程回调仍可见 |
| APC 注入 | `apc` | `QueueUserAPC` + `LoadLibraryW`（不新建线程） | 无新线程；依赖目标线程进入可告警等待；模块列表仍可见 |
| 线程劫持 | `threadhijack` | 挂起线程 → `SetThreadContext` 指向桩 → `LoadLibraryW` → `NtSetContextThread` 恢复原上下文 | 无新线程；无 `CreateRemoteThread`；残留非映像只读可执行区（桩，代码/数据已分离） |
| 手动映射 | `manualmap` | 自解 PE：本地解析导入/重定位 → 远端映射 → 桩调用 `DllMain` | **不经 LoadLibrary、不进模块列表**；入口线程+非映像区域可被内存扫描发现；入口桩执行完即释放 |
| XOR 手动映射 | `manualmapxor` | 载荷文件 XOR 加密，内存解密后手动映射 | 对抗磁盘静态扫描；内存侧与 manualmap 相同 |
| 窗口消息钩子 | `setwindowshook` | `SetWindowsHookEx(WH_GETMESSAGE)` 注入钩子 DLL | 走系统钩子加载路径，模块列表可见；无远程线程痕迹 |
| **手动映射+线程劫持**（复合） | `manualmaphijack` | 手动映射 + 借目标现有工作线程执行 `DllMain`（不新建线程） | **无新线程 + 不进模块列表**；残留映射镜像与只读桩代码区 |
| **XOR+手动映射+线程劫持**（复合） | `manualmaphijackxor` | XOR 载荷 + 手动映射 + 线程劫持入口 | 最隐蔽组合：磁盘无明文、无新线程、模块列表不可见 |
| 映像节映射 | `imagemap` | `NtCreateSection(SEC_IMAGE)` + `NtMapViewOfSection` 按系统映像方式映射进目标 | 区域类型为 **MEM_IMAGE**，内存扫描看起来像正常加载的镜像（非映像可执行区计数归零）；重定位由内核完成 |
| **映像节映射+线程劫持**（复合） | `imagemaphijack` | 映像节映射 + 借工作线程执行 `DllMain` | 镜像区域 + 不新建线程；单次注入非映像可执行区仅剩 1 个只读桩 |

> 大白话版（与设置菜单一致）：
> 经典远线程=最普通、先做基线；NtCreateThreadEx=换种方式建线程；
> APC=借目标线程干活；线程劫持=暂停线程用完再还；手动映射=模块列表看不到；
> XOR=文件先加密再加载；窗口钩子=走系统钩子路径；
> 手动映射+线程劫持=组合拳（不建线程+列表看不到）；
> XOR+映射+劫持=最隐蔽全套；映像节映射=按系统方式映射，内存里像正常 DLL。

> **设置菜单只保留“独立方案”与“复合方案”**：线程劫持 / 手动映射 / XOR 手动映射
> 作为复合方案的组成部分已从菜单移除（命令行 `--inject` 仍可单独使用，用于隔离
> 对照实验）。

附加能力：

| 能力 | 说明 |
|------|------|
| `opts.hideModule` | LoadLibrary 系注入后，从目标 PEB 的 LoadOrder/MemoryOrder/InitializationOrder 三条链表摘除模块（Toolhelp/EnumProcessModules 不再枚举）。卸载前需 `RestoreModuleFromPeb` 恢复，否则 `FreeLibrary` 不安全 |
| `--xor-key` | 指定 XOR 密钥（默认 0x5A） |

## 3. 构建与自测

```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" `
  ".\build\QuickScriptTool.sln" /p:Configuration=Release /t:InjectionSelfTest /m /v:minimal

build\Release\InjectionSelfTest.exe --json
```

自测用例（13 项，均要求 `ok:true` / exit 0，且注入后目标进程存活）：

1. `classic_inject` — 经典注入 + 标记载荷 DllMain 执行
2. `nt_createthreadex_inject`
3. `apc_inject`
4. `thread_hijack_inject`
5. `manual_map_inject` — 同时断言模块**不出现**在 Toolhelp 模块列表
6. `manual_map_xor_inject`
7. `manualmap_hijack_inject` — 复合：手动映射+线程劫持入口，模块列表不可见
8. `manualmap_hijack_xor_inject` — 复合：XOR+手动映射+线程劫持（最隐蔽）
9. `imagemap_inject` — 映像节映射（MEM_IMAGE）
10. `imagemap_hijack_inject` — 复合：映像节映射+线程劫持
11. `set_windows_hook_inject`
12. `peb_hide_restore` — 摘除后不可见 → 恢复后可枚举 → `FreeLibrary` 触发 detach
13. `fakefocus_injector_technique` — 产品类 FakeFocusInjector 以映像节映射+线程劫持
    入口 + PEB 隐藏注入真实靶进程并安全卸载

自测会启动 `InjectionTestTarget.exe`（含隐藏窗口消息循环、可告警等待线程、
普通工作线程）并注入 `InjectionTestPayload64.dll`（无 CRT、仅 kernel32/user32
依赖，DllMain 在 `%TEMP%\QstInjectionTestPayload_<pid>.marker` 写标记）。

## 4. 对抗测试驱动（对真实目标）

```powershell
# 注入游戏/目标进程，输出可观测面
build\Release\InjectionSelfTest.exe --inject <pid> <dll路径> <technique> [选项]

# 例：对 PID 12345 注入 FakeFocus64.dll，手动映射
InjectionSelfTest.exe --inject 12345 "build\Release\FakeFocus64.dll" manualmap

# 例：经典注入 + PEB 隐藏
InjectionSelfTest.exe --inject 12345 "build\Release\FakeFocus64.dll" classic --hide

# 例：钩子注入（需要 DLL 导出钩子过程）
InjectionSelfTest.exe --inject 12345 "build\Release\FakeFocus64.dll" setwindowshook --hook-proc FakeFocus_HookProc

# 例：XOR 载荷（自动打包到临时文件）
InjectionSelfTest.exe --inject 12345 "build\Release\FakeFocus64.dll" manualmapxor --xor-key 90

# 例：复合最隐蔽组合（不新建线程 + 模块列表不可见）
InjectionSelfTest.exe --inject 12345 "build\Release\FakeFocus64.dll" manualmaphijackxor

# 例：映像节映射（MEM_IMAGE，内存扫描看起来像正常镜像）
InjectionSelfTest.exe --inject 12345 "build\Release\FakeFocus64.dll" imagemaphijack
```

输出一行 JSON：

```json
{"inject":"ok","technique":"manualmap","module_visible":false,"exec_regions":1,"detail":"..."}
```

解读：
- `module_visible`：载荷是否出现在 Toolhelp 模块列表（false 表示绕过模块枚举）
- `exec_regions`：`VirtualQueryEx` 扫描到的非映像可执行区域数（>0 表示存在
  反作弊可做内存扫描的痕迹）
- `detail`：注入结果/失败原因

典型对照实验：

| 实验 | 方法 | 观察 |
|------|------|------|
| 基线 | `classic` | 反作弊应能通过 API 钩子/线程回调/模块列表全部识别 |
| 绕 API 钩子 | `ntcreatethreadex` | 若反作弊仍命中，说明检测在内核回调层 |
| 无新线程 | `apc` / `threadhijack` | 若仍命中，说明检测在模块列表/内存扫描 |
| 无模块记录 | `manualmap` | 若仍命中，说明有内存扫描/句柄监控 |
| 静态扫描 | `manualmapxor` | 配合杀软/反作弊静态引擎看文件侧 |
| 窗口模式路径 | `setwindowshook` | 观察是否被当作“合法自动化”放过或被识别为注入 |
| **复合最小痕迹** | `manualmaphijack` / `manualmaphijackxor` | 同时绕开新线程回调与模块枚举；若仍命中，基本可断定有内核内存扫描 |
| **镜像级复合** | `imagemap` / `imagemaphijack` | 映射区域为 MEM_IMAGE（像正常加载的 DLL），非映像可执行区归零；若仍命中，说明检测在**内核映像回调/完整性哈希**层 |

建议的对抗测试顺序（从最易检测到最隐蔽逐级替换，用于给反作弊“分级”定位）：

1. `classic` 基线 → 应被识别
2. `ntcreatethreadex` / `apc` / `threadhijack` → 区分“API 钩子”与“内核线程回调”
3. `manualmap` → 区分“模块枚举”与“内存扫描”
4. `manualmaphijack` → 同时无新线程 + 无模块记录（反作弊若仍命中，重点看内存扫描）
5. `manualmaphijackxor` → 最隐蔽全套，验证静态扫描/内存扫描联合检测
6. `imagemaphijack` → 镜像级映射（MEM_IMAGE），若仍命中，检测在**内核映像加载回调或
   内存哈希比对**层（这类手段通常需要驱动，用户态已无更隐蔽空间）

## 5. 产品接入（最真实场景：直接走后台窗口模式）

注入技术已集成到产品真实链路：**设置 → 窗口模式 → 假焦点注入技术**，选择后
保存；之后无论是**录制回放**还是**鼠标宏**，只要以窗口模式/后台窗口模式运行，
`EngineHost` 就会在启动窗口会话时把该技术应用到 `FakeFocusInjector`，
随绑定注入真实执行（默认仍是经典远线程，与原行为一致）。

设置项（`app_settings.json` → `windowMode`）：

| 字段 | 说明 |
|------|------|
| `injectionTechnique` | 0=经典远线程 1=NtCreateThreadEx 2=APC 6=窗口消息钩子 7=手动映射+线程劫持 8=XOR+映射+劫持 10=映像节映射+线程劫持（3/4/5 为复合方案的组成部分，仅命令行可用） |
| `hideInjectedModule` | 注入后从 PEB 模块链表摘除（测试模块枚举检测） |
| `enableFakeFocusInjection` | 假焦点注入总开关（关闭后窗口模式不注入 DLL） |

注入发生在 `WindowModeExecutor::TryInstallFakeFocus()`（`BeginRun` 内，录制与
宏共用该路径）；`FakeFocus_HookProc` 已作为导出加入 `FakeFocus64/32.dll`，
供 `setwindowshook` 技术使用。卸载时若模块曾被隐藏，会自动先恢复 PEB 链表
再 `FreeLibrary`。

命令行快速验证（不依赖 UI）也可直接改设置文件：

```powershell
# app_settings.json 的 windowMode 段写入后，直接绑窗跑宏即可生效
"injectionTechnique": 4, "hideInjectedModule": true
```

## 6. 局限性（对照测试时注意）

- 手动映射 / 线程劫持 / 钩子注入当前仅支持**同架构**（x64→x64）；WOW64
  目标请用 `classic` / `ntcreatethreadex` / `apc`（这三个已在库内做远端
  导出解析，支持 32 位目标）。
- 线程劫持要求目标线程能在有限时间内回到用户态：**处于无限内核等待的线程
  （线程池/系统线程）不会立即应用新上下文**。库会优先选择目标主模块内、
  非 UI 的短等待工作线程；对真实游戏，建议先确认目标线程状态。
- `hideModule` 隐藏的是“枚举可见性”，**不是内存存在性**；内核回调、内存
  扫描仍可发现。卸载前必须先恢复链表。
- 手动映射要求 DLL 的导入依赖已在目标进程中加载且基址与本机一致（同架构、
  同引导的系统 DLL 通常满足）。带 TLS 回调/延迟加载的 DLL 不在支持范围。
- 复合（手动映射+线程劫持）会保留一个小的**只读可执行桩区**与映射镜像本身
  （数据区为不可执行的读写页，已与代码分离）；这是“不新建线程”与
  “无模块记录”之外仍可能被内存扫描看到的固有痕迹。
- 映像节映射（SEC_IMAGE）要求 DLL 是磁盘上的真实文件（不能配 XOR 加密载荷）；
  若映射后仍需改 IAT，会短暂把导入表页置为可写再恢复。
- 钩子注入要求目标线程实际处理窗口消息（消息循环），否则 DLL 不会注入。

## 7. 复合方案调研结论（2026-08）

对本框架最相关的新资料集中在三类：

1. **SEC_IMAGE 映像节映射**（`NtCreateSection` + `NtMapViewOfSection`）：把载荷
   按“映像”映射进目标，区域类型为 MEM_IMAGE、节权限由映像决定（无 RWX）、
   重定位由内核完成——内存扫描器通常会跳过这类区域，因为它看起来就是正常
   加载的 DLL。**已实现**（`imagemap` / `imagemaphijack`）。
2. **模块踩踏（Module Stomping）**：先加载一个合法/牺牲 DLL，再覆盖其 `.text`
   执行载荷。检测侧已有成熟手段（内存与磁盘哈希比对、异常写入监控），且会
   破坏宿主模块的可用性，**不建议纳入本框架**。
3. **VEH/HWBP 重载、进程镂空/幻影进程**：属实验室级或进程创建类技术，与
   “向已运行窗口进程注入 DLL”的测试目标不匹配，**未采纳**。

结论：用户态注入里，`imagemaphijack`（映像节映射 + 线程劫持）已是当前
框架能提供的**检测痕迹最小**组合；再往下（内核映像回调、内存哈希比对）就
超出用户态注入能力范围，应由驱动层方案覆盖。

## 8. 参考来源

- 窗口模式注入现状：`src/window_mode/fake_focus/fake_focus_injector.cpp`、
  `src/window_mode/cdp/cdp_enable_injector.cpp`
- 公开技术资料（用户提供）：
  - CSDN《DLL 注入》系列问答/文章（8856261 / 8563115 / 110037710 / 131563182）
  - FreeBuf《如何防止恶意的第三方 DLL 注入到进程》（缓解策略侧，用于对照）
  - 知乎《DLL 注入技术的 N 种姿势》（26489531）
  - 博客园《免杀对抗——DLL 注入 / 白加黑 / 镂空》（19379704）
  - 博客园《shellcode 分离加载》（19001324，XOR 载荷思路）
- 补充公开资料：ManualMap/Reflective 注入、NtCreateThreadEx、QueueUserAPC、
  线程劫持、PEB 模块隐藏等（均为公开安全研究内容，见 GitHub 相关开源项目）
