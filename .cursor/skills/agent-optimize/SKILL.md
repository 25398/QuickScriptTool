---
name: agent-optimize
description: >-
  脚本/录制优化：必须调用 optimizeRecording / optimizeScript 的 merge（与产品「鼠标移动合并」同一算法），
  禁止擅自 compressPath，禁止 readScript 拉全文或手写 JSON 改动作。
---

# 脚本/录制优化

用户要求优化键鼠录制或宏的鼠标轨迹时，**只调用工具**，不要自己读脚本、理解逐步动作再改 JSON。

## 用哪个工具

| 对象 | 工具 |
|------|------|
| 键鼠录制 | `optimizeRecording`（默认 `recordings`） |
| 鼠标宏 | `optimizeScript`（自动在 scripts / recordings 查找） |

流程：`listScripts` / `listRecordings` 确认文件名 → **直接 `mergeMode=merge`**。
**禁止**为优化调用 `readScript` 拉全文。不必先 `getScriptStats`。

## 模式（与产品「录制优化」对话框相同）

- **merge（默认，用户说「优化」时必须用）**：与产品「鼠标移动合并」相同。按关键动作分段，每段移动+等待合并为一次等待 + 一次移动；等待时间按**该段**计算（默认 `sum` 累加）。含相对位移的段跳过。密集轨迹通常能从几百步收到十几步。
- **compressPath（不要默认用）**：与产品「鼠标移动压缩」相同，只去掉过密的绝对移动点，动作数只会略减。仅当用户明确说「压缩路径 / 去掉过密移动点」时才用。

`waitCalculation`：`sum` / `average` / `first` / `last` / `fixed`（`fixed` 时传 `mergeWaitValue` 秒）。

## 另存

用户要保留原版时传 `outputFileName`；省略则覆盖原文件（可撤销）。

路径压缩/合并不等于「转为找图」。转找图请用户用产品内录制优化对话框，本工具不做。
