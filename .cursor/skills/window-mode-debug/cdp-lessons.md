# CDP / 扩展窗口模式 — 踩坑与定论（必读）

**需求（用户要什么）**：[cdp-requirements.md](cdp-requirements.md)  
每次改 CDP/宏桌面/扩展截图前先读需求 + 本节，**禁止重复已证伪的方向**。

## 硬规则（与需求文档一致）

1. **扩展执行**：键鼠、找图、保活只走配套扩展。  
2. **Win32/VDA 只搬窗**：目标浏览器 Move 到「鼠标宏」停放。  
3. **禁止切桌面干预**：禁止运行期 GoTo 钉桌泵 / anti-steal / Correct 踢回 / 「视图被带走→钉回」。  
4. **禁止误 Raise**：用户不在宏桌面时 ForceReveal/Raise → 必切到宏桌面目标窗。  
5. **可展开**：用户已在宏桌面时可 Raise 一次揭隐身；Stop 必须刮掉 α=1，禁止鬼影残留。  
6. **Move 瞬时**：仅 `PreservingView` 在搬窗当下拉回一次；禁止事后长 HoldView。  
7. **日志**：`window_mode_debug.log`（Agent 自读，免粘贴）。

---

## 错误方向（已证伪，禁止再做）

| 方向 | 为何错 | 日志/现象 |
|------|--------|-----------|
| Cloak + PreferMax「保活」 | 切屏；cloak=1 无法展开；脚本绑桌面逻辑 | `cloak=1` `已铺满工作区` |
| 清 Peek 后又 Pin | 再 SuppressPeek，宏桌面难点开 | 停放后仍抑制 Peek |
| **Pin 后先屏上 SoftRestore 再挪屏外** | 启动白屏闪一下 | **已证伪**；placement 直接设屏外再还原 |
| **热键 NotifyCancel join 观看泵** | UI 线程卡在 Reveal/VDA，像「半天才中断」 | **已证伪**；NotifyCancel 只 Abort+信号，join 留 EndRun |
| **VDA 误报离桌 → 再 Pin** | 已展开又收回屏外，表现为无法展开 | 离桌确认 ~4s + 收回前再验 UserOnMacro |
| **Pin+屏外但观看只 SetPlacement** | 非 iconic 屏外窗不挪位；刷假成功日志 | **已证伪**；须 SetWindowPos；成功须验 GetWindowRect/UnPin |
| **startFp 空仍标 contentMoved** | 假新帧 / 沿用旧 JPEG；bestNcc 恒定 | **已证伪**；须基线指纹；宿主比对 JPEG 哈希 |
| **长期最小化避切屏** | WebGL 静帧 | **已证伪作唯一策略** |
| **屏外缩窗** | iframe 点偏（675→820） | 屏外保持工作区尺寸 |
| **CDP 运行期 GoTo「钉用户桌」泵** | 违反「只搬窗」；反复切虚拟桌面 | `视图被带走→已钉回` |
| **ScheduleCdpParkViewPin 持续 GoTo** | 同上 | 绑窗后桌面来回跳 |
| **Stop 后再 GoTo/HoldView 钉用户桌** | 结束时又切一次桌面 | 停脚本瞬间切屏 |
| CorrectSameProcess 踢回 | 用户无法在宏桌面观看 | `已纠正抢屏` |
| 仅最小化且无扩展保活 | WebGL 冻（旧）；现靠扩展 lifecycle | 角色无反应 |
| 找图热路径 Prepare/PreferMax/ShowWindow | 抢屏 | 找图时 UserDesk 变 |
| Raise + PreferMax / SWP_SHOWWINDOW | 抢视图切屏 | 键鼠/观看路径 |
| **DWM Cloak + α=1 停放** | `SWP_FRAMECHANGED` → Win11 切宏桌面；窗不可展开（ghost） | `cloakApp=1 layered=1` 但仍切屏 |
| **裸屏外无 Pin（用户桌）** | 未进宏桌面 / 或 Move 时切屏 | 须先 Move 宏桌面；出帧用 **Pin+屏外** |
| **CDP 迁回用户桌停放** | 违背窗口模式工作面 | 用户已否定 |
| **用户不在宏桌面时 Raise/ForceReveal** | 刮 α=1 → Win11 切到宏桌面窗 | `观看：已揭隐身` 后切屏 |
| **观看泵 dwell≥0.5s 仍误 Raise** | VDA 误报/清风险窗后误判观看 | 步骤中 `观看：已揭隐身` |
| 已在宏桌面仍强制最小化 | 用户展开后又被关掉 | iconic 0→1 |
| 扩展点击失败回退软 PostMessage | 切屏；违背扩展执行 | `回退软点击` |
| pageclip / PrintWindow 找图 | 白闪或切屏 | `http-pageclip` / 全屏找图 |
| mirror 缩 iframeCss；整包 JPEG；冻帧后 pageclip | 匹配崩或白闪 | 见历史 1.1.24–32 |
| 按可见性 hidden→pageclip | Unlock 卡 ~75% | 1.1.31–32 |
| 假新帧 / 永久最小化避切屏 | Unlock `:static` | `iconic=1` + static |
| ClearPeek 内 ForceReveal | 刚隐身立刻揭开切屏 | — |
| 已在宏桌面仍全量 Park 迁回 Move | 二次切屏 | 两次 `Move宏桌` |
| 落宏桌面立刻 ForceReveal | 切到宏桌面目标窗 | `揭隐身` |
| 找图热路径 Raise | 瞬时 FgDesk=宏 + Raise | 中途切屏 |
| attach 漏 pageCssW/H | 点偏 | `pageCss=0x0` |
| **canvas 找图整窗拉伸映射** | 顶部匹配 → surface y≈0 → iframe y&lt;0，点击落空 | **已证伪**；须按 iframeCss 映入 surface（安居镇 46） |
| **Pin+屏外客户区 215×28 当 surface** | MapCanvas/点击与扩展 dpr 脱节；46 步点中后 49 永不匹配 | **已证伪**；`surface/pageCss` 不在 (0.5,4) 时用 **pageCss×1.5**（与扩展 mapHostToTarget） |
| **Win32 客户区当 surface 致 scaleX≠scaleY** | 含标题栏比例畸变；`scale=1.543x1.661` 点偏；找图跟点漂 | **已证伪**；\|sx−sy\|>0.05 时强制 **pageCss×dpr（均匀）** |
| **FindImage 每次 EnsureTargetBound→Park** | 刷停放日志；与观看泵互殴；可能干扰出帧 | **已证伪**；CDP 找图热路径禁止再 Park |
| **BeginRun 对已 Pin 窗再 EnsureTarget/Move** | Pin 后 desk=-1，误判不在宏桌→Move+SoftRestore 拆停放并切屏 | **已证伪**；已停放只启观看泵；EnsureTarget 遇 Pin/屏外直接 return |
| HoldPreferred 在 Park **之前** | Move 偷视图发生在 Hold 后 | 开始运行切宏桌面 |
| **CDP 绑窗后再多次 HoldPreferred/钉桌** | 等同切桌面 | 绑窗闪屏 |
| CDP Stop 跳过 Release latch | 结束后鬼影打不开 | α=1 残留 |
| ForceReveal 依赖污染 latch 的 EndCloak | 还原成 α=1 | 无法展开 |

### 找图截图定论

扩展 mirror-only；禁止 pageclip；禁止找图热路径 ShowWindow。  
MapCanvas：**独立 sx/sy**；http-mirror 须 **iframe 矩形映射**（禁整窗拉伸）。

### 搬窗定论

```
绑窗/BeginRun: Minimize→Move「鼠标宏」→ Pin + SoftRestore + 屏外（尺寸≥工作区）
      证伪：异桌裸还原 → 鼠标一切屏
      证伪：长期最小化 → 静帧
观看: dwell≈0.3s 且仍需揭开 → UnPin + Move宏桌面 + **SetWindowPos 强制回屏**（禁仅 Placement）；验位置后再记成功；失败限次重试
离开: 再 Pin+屏外（禁最小化）
运行: 扩展键鼠/找图；禁运行期 Move；JPEG 同哈希→丢弃重拍
结束: UnPin + 恢复坐标 + 最小化
```

## 症状速查

| 现象 | 原因 | 处理 |
|------|------|------|
| 一点鼠标就进宏桌面 | 异桌裸还原 | Pin+屏外 |
| 无法展开 | Pin 未揭开 / 只 SetPlacement 不挪屏外窗 | dwell 后 UnPin+SetWindowPos；看「展开未完成」日志 |
| 点击漂 | 屏外缩窗 / surface=215×28 | 保持工作区尺寸；日志须见 `找图表面: pageCss×1.5` 且 surface≈page×1.5 |
| 展开很慢 | 观看泵确认过长 | dwell≈0.3s（非 4s） |
| Unlock 静帧 / bestNcc 恒定 | iconic 或假新帧 | Pin+屏外出帧；扩 1.1.41+ 基线指纹；宿主丢弃同哈希 JPEG |
| 找图/连按卡帧（观看宏桌面） | 每次 Input/vision 都 `startScreencast`×120ms | 扩 **1.1.43+**：键鼠轻量保活；找图每轮最多 1–2 次 heavy screencast（≥450ms 节流） |
| 开始跑宏时用户桌白闪一下 | Pin 后 SoftRestore 仍 `ShowWindow`（Pin 窗全桌可见） | SoftRestore：屏外/Pin 时 quietPark（禁 Show/再写 SHOW Placement）；出帧先挪屏外再 Pin；迁桌先 Minimize 再 UnPin；SetWindowPos 加 NOREDRAW |
