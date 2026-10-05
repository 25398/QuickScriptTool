# MinHook（vendored）

来源：<https://github.com/TsudaKageyu/minhook>（master 快照，2026-10-03 抓取）
许可证：BSD-2-Clause（见 `LICENSE.txt`，**必须随二进制一起保留**）
版本标识：无 tag 的 master；文件清单见下。

## 为什么引入

`src/window_mode/fake_focus/fake_focus_hook.cpp` 原来的内联钩用
「**临时还原 + 重写跳转**」来调用原函数：

```
还原 12 字节 → 调用原函数 → 重新写入 12 字节绝对跳转
```

x86-64 上 **12 字节写入不是原子的**（只有对齐的 8 字节以内才是）。
目标进程（Unity / UE 这类多线程游戏）在多个线程上同时调用被钩函数时，
会取到「半个跳转」⇒ `mov rax, <垃圾>; jmp rax` ⇒ **目标进程 0xC0000005 闪退**。

复现（`build/_tmp/hookrace/`，直接编译真实实现）：

| 场景 | calls | av | missedDetour | 结果 |
|---|---|---|---|---|
| 单线程 5s | 1,676,000 | 0 | 0 | PASS |
| 双线程 5s | 1,924,095 | **1**（0xC0000005） | **262,996** | FAIL |
| 八线程 6s | — | — | — | 进程直接段错误 |

单线程全绿 ⇒ 与「12 字节覆盖越界」无关，纯粹是并发写入竞态。

## 用了它的什么

只用 `MH_Initialize` / `MH_CreateHook`（拿 trampoline）/ `MH_EnableHook` /
`MH_DisableHook` / `MH_RemoveHook` / `MH_Uninitialize`。

关键收益是 **trampoline**：原函数前 N 字节被复制到一块独立内存并跳到
`target+N`，原函数从此**按原签名直接调用**——目标函数头**只在安装时写一次**，
之后永远只读，写入竞态从根上消失。

顺带解决了两个同源问题：
1. **无长度反汇编**：旧实现固定覆盖 12 字节，不看指令边界；MinHook 用
   HDE32/HDE64 解码到完整指令边界（并处理 rip-relative 重定位）。
2. **安装瞬间的撕裂窗口**：`MH_EnableHook` 会挂起其他线程再落补丁。

## 没有改动的部分

**原样 vendor，一个字节都没改。** 升级只需整目录替换。
`src/hde/` 是 Z0MBiE 的 HDE（公共领域/BSD 双许可，MinHook 已在其头部注明）。

## 构建接线

- 主工程：`CMakeLists.txt` 的 `qst_minhook`（STATIC）→ `FakeFocus` 链接它。
- 32 位 DLL：`src/window_mode/fake_focus/build_fakefocus32.cmd` 直接 `cl` 编译，
  同一份源文件清单（MinHook 用 `_M_X64` / `_M_IX86` 自己选 HDE32/HDE64）。

⚠ 运行时库必须与 `FakeFocus` 一致（`/MT`，Debug 下 `/MTd`），
否则 DLL 里会同时存在两套 CRT 堆。
