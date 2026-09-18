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

#include <windows.h>

enum class ClipboardExpandMode {
    Text,  // 快捷输入 / OCR / AI 文字分析：文本或文件路径；位图展开为空
    Ai     // AI 图片分析 / AI 动作执行：文本+非图片路径；图片改为「见附图」
};

struct MacroClipboardSnapshot {
    std::wstring text;
    std::vector<std::wstring> files;
    bool hasBitmap = false;
    HBITMAP bitmap = nullptr;  // 快照持有；析构时 DeleteObject

    MacroClipboardSnapshot() = default;
    ~MacroClipboardSnapshot() { ClearBitmap(); }
    MacroClipboardSnapshot(const MacroClipboardSnapshot&) = delete;
    MacroClipboardSnapshot& operator=(const MacroClipboardSnapshot&) = delete;
    MacroClipboardSnapshot(MacroClipboardSnapshot&& o) noexcept
        : text(std::move(o.text))
        , files(std::move(o.files))
        , hasBitmap(o.hasBitmap)
        , bitmap(o.bitmap) {
        o.bitmap = nullptr;
        o.hasBitmap = false;
    }
    MacroClipboardSnapshot& operator=(MacroClipboardSnapshot&& o) noexcept {
        if (this != &o) {
            ClearBitmap();
            text = std::move(o.text);
            files = std::move(o.files);
            hasBitmap = o.hasBitmap;
            bitmap = o.bitmap;
            o.bitmap = nullptr;
            o.hasBitmap = false;
        }
        return *this;
    }
    void ClearBitmap() {
        if (bitmap) {
            DeleteObject(bitmap);
            bitmap = nullptr;
        }
    }
};

MacroClipboardSnapshot ReadMacroClipboardSnapshot();
bool LooksLikeImageFilePath(const std::wstring& path);
bool PromptMentionsCtrlClipboard(const std::wstring& text);
/// 返回 HBITMAP（调用方 DeleteObject）；无位图时为空。
void* DuplicateClipboardBitmapRaw();

// 快捷输入变量提示项 (用于编辑器的变量自动提示)
struct QuickInputVarItem {
    std::wstring display;    // 显示名 (如 "matchRet.x")
    std::wstring insertText; // 插入到编辑框的文本
    std::wstring tooltip;   // 悬停提示
    std::wstring codeHint;  // 代码补全提示
};

/// 多图匹配保存的全部命中（matchRet[0].x）
struct ImageMatchListHit {
    ImageMatchResult match;
    int templateIndex = 0;          // 0-based 模板序号
    std::wstring templateName;
};

struct ImageMatchListVar {
    std::vector<ImageMatchListHit> hits;
};

// 宏变量执行上下文 (传递当前脚本的变量状态)
struct MacroVariableContext {
    const std::unordered_map<std::wstring, ImageMatchResult>* matchVars = nullptr;  // 找图结果变量
    const std::unordered_map<std::wstring, ImageMatchListVar>* matchListVars = nullptr;  // 多图/多处结果
    const std::unordered_map<std::wstring, OcrVarResult>* ocrVars = nullptr;      // 文字识别变量
    const std::unordered_map<std::wstring, std::wstring>* aiVars = nullptr;       // AI输出变量
    const std::unordered_map<std::wstring, std::wstring>* userVars = nullptr;     // 变量运算 return 导出
    const std::unordered_map<std::wstring, std::wstring>* imageVars = nullptr;    // 图片变量→绝对路径
    const std::unordered_map<std::wstring, int>* loopVars = nullptr;                // 循环计数变量
    const std::unordered_map<std::wstring, std::chrono::steady_clock::time_point>* timerStarts = nullptr;  // 计时器变量起始时刻
    int curLoops = 0;  // 当前外层循环累计次数
    const MacroClipboardSnapshot* clipboardSnapshot = nullptr;  // 非空则不再读系统剪贴板
    ClipboardExpandMode clipboardExpandMode = ClipboardExpandMode::Text;
};

// 构建编辑器变量提示列表 (从脚本动作中提取所有定义过的变量)
std::vector<QuickInputVarItem> BuildQuickInputVarItems(const std::vector<ScriptAction>& actions);

// 将文本中的 {varName} 占位符替换为实际值
std::wstring ResolveMacroVariables(const std::wstring& text, const MacroVariableContext& ctx);

// 快捷输入：展开 {var} 后按 parseEscapes 处理转义。
// 勾选：整段（含变量值）把 \n \r \t \\ 转成换行/Tab/反斜杠。
// 未勾选：文本框字面保留；变量里已有的换行/Tab 直接丢掉，避免 OCR/剪贴板换行仍被当成回车。
std::wstring ResolveQuickInputText(const std::wstring& text, const MacroVariableContext& ctx, bool parseEscapes);

// 解析单个操作数 token (处理变量引用和数值字面量)
std::wstring ResolveMacroOperand(const std::wstring& token, const MacroVariableContext& ctx);

/// 变量运算中的剪贴板：有文件则路径（多文件换行分隔），否则为文本；空则空串。
std::wstring ResolveClipboardVarCompute(const MacroVariableContext& ctx);

// 尝试将 token 解析为整数操作数
bool TryResolveIntOperand(const std::wstring& token, const MacroVariableContext& ctx, int& out);

// 解析找图时限（秒）：负数=直到找到；0=只找一次；正数=最大搜索秒数；非数字视为 0
double ResolveFindImageTimeSec(const std::wstring& expr, const MacroVariableContext& ctx);

// 获取循环动作的最大执行次数 (从 loopVarExpr 或 loopCount 解析)
// loopStartTime: 传入循环进入时刻，用于计时器变量作循环次数时扣除循环体内耗时
int ResolveLoopMaxCount(const ScriptAction& action, const MacroVariableContext& ctx,
    std::optional<std::chrono::steady_clock::time_point> loopStartTime = std::nullopt);

// 解析跳转目标序号（支持变量名或 {变量} 表达式）
bool TryResolveGotoStepNo(const std::wstring& expr, const MacroVariableContext& ctx, int& out);

// 解码快捷输入文本中的转义字符 (如 \\n → 换行)
std::wstring DecodeQuickInputEscapes(const std::wstring& text);

// 评估条件表达式 (支持 ==, !=, >, <, >=, <= 比较和 &&, || 逻辑运算)
bool EvaluateConditionExpr(const std::wstring& expr, const MacroVariableContext& ctx);
