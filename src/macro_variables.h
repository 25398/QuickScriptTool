// ──────────────────────────────────────────────────────────────────
// macro_variables.h — 宏变量系统
// 管理脚本执行中的动态变量 (找图结果、循环计数器)，
// 支持变量代入、表达式求值和条件判断。
// ──────────────────────────────────────────────────────────────────
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "image_match.h"
#include "ocr_result.h"
#include "script_types.h"

// 快捷输入变量提示项 (用于编辑器的变量自动提示)
struct QuickInputVarItem {
    std::wstring display;    // 显示名 (如 "matchRet.x")
    std::wstring insertText; // 插入到编辑框的文本
    std::wstring tooltip;   // 悬停提示
    std::wstring codeHint;  // 代码补全提示
};

// 宏变量执行上下文 (传递当前脚本的变量状态)
struct MacroVariableContext {
    const std::unordered_map<std::wstring, ImageMatchResult>* matchVars = nullptr;  // 找图结果变量
    const std::unordered_map<std::wstring, OcrVarResult>* ocrVars = nullptr;      // 文字识别变量
    const std::unordered_map<std::wstring, std::wstring>* aiVars = nullptr;       // AI输出变量
    const std::unordered_map<std::wstring, int>* loopVars = nullptr;                // 循环计数变量
    const std::unordered_map<std::wstring, std::chrono::steady_clock::time_point>* timerStarts = nullptr;  // 计时器变量起始时刻
    int curLoops = 0;  // 当前外层循环累计次数
};

// 构建编辑器变量提示列表 (从脚本动作中提取所有定义过的变量)
std::vector<QuickInputVarItem> BuildQuickInputVarItems(const std::vector<ScriptAction>& actions);

// 将文本中的 ${varName} 占位符替换为实际值
std::wstring ResolveMacroVariables(const std::wstring& text, const MacroVariableContext& ctx);

// 解析单个操作数 token (处理变量引用和数值字面量)
std::wstring ResolveMacroOperand(const std::wstring& token, const MacroVariableContext& ctx);

// 尝试将 token 解析为整数操作数
bool TryResolveIntOperand(const std::wstring& token, const MacroVariableContext& ctx, int& out);

// 获取循环动作的最大执行次数 (从 loopVarExpr 或 loopCount 解析)
// loopStartTime: 传入循环进入时刻，用于计时器变量作循环次数时扣除循环体内耗时
int ResolveLoopMaxCount(const ScriptAction& action, const MacroVariableContext& ctx,
    std::optional<std::chrono::steady_clock::time_point> loopStartTime = std::nullopt);

// 解码快捷输入文本中的转义字符 (如 \\n → 换行)
std::wstring DecodeQuickInputEscapes(const std::wstring& text);

// 评估条件表达式 (支持 ==, !=, >, <, >=, <= 比较和 &&, || 逻辑运算)
bool EvaluateConditionExpr(const std::wstring& expr, const MacroVariableContext& ctx);
