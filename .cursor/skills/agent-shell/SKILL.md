---
name: agent-shell
description: >-
  白名单命令执行与受限文件操作：git 只读、MSBuild 自检、自检 exe、where；
  listDirectory/readAgentFile/searchAgentFiles/writeAgentFile/剪贴板。禁止任意命令。
---

# 命令行与文件操作

## runAgentCommand

```json
{ "command": "git status --short", "cwd": "" }
```

白名单：

| 程序 | 允许 |
|------|------|
| `git` | 只读：status/diff/log/show/rev-parse/branch/ls-files/remote/config/stash list |
| `MSBuild.exe` | 仅 `build\QuickScriptTool.sln` + `/p:Configuration=Release` + `/t:<自检Target>` + `/m /v:minimal /nologo` |
| `build\Release\*SelfTest.exe` | `--json` `--list` `--macro` `--help` |
| `where.exe` | 只读查找 |

参数经 `CommandLineToArgvW` 解析逐条校验，不经 cmd/powershell；输出 64KB、超时 60s。
自检：`MSBuild ... /t:<Target>` 后跑 `build\Release\<Target>.exe --json`，exit 0 才算通过。

## 文件工具

| 工具 | 作用 |
|------|------|
| `listDirectory` / `readAgentFile` | 浏览/读取（默认 64KB） |
| `searchAgentFiles` | 递归文本搜索 `路径:行号: 内容` |
| `writeAgentFile` | 写文本（自动撤销） |
| `copyAgentTextToClipboard` / `pasteAgentClipboardText` | 剪贴板文本 |

允许目录：scripts / recordings / images / library / AppDir；
开发仓库另含 docs、.cursor/skills、tools、ui（可写）。写操作禁止落在
build、WebView2Fixed、WebView2UserData、agent_conversations、agent_changes 与根级配置。

## 安全底线

拒绝未知程序与含 `| ; & > < $ % \`` 的参数；所有写操作进撤销日志。
