#pragma once

#include "macro_variables.h"

#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

struct VarComputeResult {
    bool ok = false;
    std::wstring error;
    // return 列出的名字 → 可写回脚本的字符串值；未 return 则为空
    std::unordered_map<std::wstring, std::wstring> exported;
};

/// 只做语法检查，不执行。
bool ParseVarCompute(const std::wstring& source, std::wstring& error);

/// 执行类 C 变量运算。局部变量在返回后丢弃；仅 return a, b 导出。
/// stopFlag 非空时循环回边检查停止。
VarComputeResult RunVarCompute(const std::wstring& source, const MacroVariableContext& ctx,
    const std::atomic<bool>* stopFlag = nullptr);

/// 静态收集 return 列出的导出变量名（编辑器变量下拉 / 快捷输入）。解析失败时尽量扫描 return 行。
std::vector<std::wstring> CollectVarComputeReturnNames(const std::wstring& source);
