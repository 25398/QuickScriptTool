#pragma once
// ──────────────────────────────────────────────────────────────────
// qst_engine.h — 产品路径 Engine 门面（D2 / E4）
// WebShell / bridge 只依赖本头；实现：src/engine/engine_runtime.cpp
// 宿主窗类：src/engine/engine_host_window.h（禁止产品 #include "main_window.h"）
// 新增能力请加在 qst::engine，禁止再扩 EngineHost 产品 UI 绘制路径。
// ──────────────────────────────────────────────────────────────────

#include "webview/qst_engine_host.h"
