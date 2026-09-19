// ──────────────────────────────────────────────────────────────────
// app_instance.cpp — g_instance 的定义（声明在 taskbar_window.h）
//
// 为什么单独一个文件：`g_instance` 原先定义在 src/webview/qst_webview_shell.cpp
// 里，而它被 taskbar_window.h 的 inline 函数、engine_gdi_editor.cpp、
// engine_host_window.h 等**引擎/桌面工具侧**代码引用。
// 结果：`qst_engine` 与 `qst_desktop_tools` 都隐含依赖壳的符号，
// 自检无法只链库（架构评估 P1-5 / 验收报告 §7.2 B1）。
//
// 定义搬到 qst_utils（被各目标传递依赖的 STATIC 基础库）后，
// 引擎、桌面工具、壳、自检都从同一处解析，不再需要链接桩。
// 壳仍在 wWinMain 里赋值（`g_instance = inst;`）。
// ──────────────────────────────────────────────────────────────────
#include <windows.h>

HINSTANCE g_instance = nullptr;
