#pragma once
// ──────────────────────────────────────────────────────────────────
// ai_logic_convert.h — AI 动作执行「逻辑转化」：轨迹 → 指令块写回
// 段末固化（下次宏循环生效）；else 回退 AI 可渐进提升快路径。
// ──────────────────────────────────────────────────────────────────

#include "script_types.h"

#include <string>
#include <vector>

/// 单条可固化轨迹（键盘/热键/切窗/定位锚点等）
struct AiLogicTranscriptEntry {
    ScriptAction action{};
    /// locate 成功时的短描述（用于备注）；空=普通动作
    std::wstring locateTarget;
    /// 定位点击生成的找图模板绝对/库路径
    std::wstring templatePath;
    int screenX = 0;
    int screenY = 0;
    bool fromLocate = false;
    /// activateWindow 成功：match 关键词（宏侧无独立切窗类型，编译为短 Wait+备注）
    bool fromActivate = false;
    std::wstring activateMatch;
    /// 该步后 UI settle 耗时（秒）；>0 时用于找图时限 / 步间等待自适应
    double settleSec = 0.0;
};

struct AiLogicConvertCompileInput {
    std::wstring taskPrompt;
    std::wstring preferredBlockName;
    std::vector<AiLogicTranscriptEntry> transcript;
    /// true=块已存在，合并提升快路径；false=首次生成整块
    bool promoteExisting = false;
    int maxNewSteps = 40;
};

struct AiLogicConvertCompileResult {
    bool ok = false;
    std::wstring error;
    std::wstring blockName;
    /// defineBlock + 缩进 body（不含 runBlock）
    std::vector<ScriptAction> defineAndBody;
    ScriptAction runBlock{};
    std::wstring summary;
};

struct AiLogicConvertWritebackRequest {
    std::wstring scriptPath;
    int sourceOriginalNo = 0;
    std::wstring sourcePrompt;
    AiLogicConvertCompileResult compiled;
};

/// 会话：逻辑转化录制开关与轨迹
/// healPromote=true：else 回退 AI 的提升合并会话（勿清 memo / 勿首次整段插入）
void AiLogicConvertSessionBegin(bool enabled, const std::wstring& blockName,
    const std::wstring& prompt, const std::wstring& scriptPath, int sourceOriginalNo,
    bool healPromote = false);
void AiLogicConvertSessionEnd();
bool AiLogicConvertSessionActive();
bool AiLogicConvertSessionIsHeal();
/// 会话路径为空时回填（热键启动曾漏传 path）
void AiLogicConvertSessionSetScriptPathIfEmpty(const std::wstring& scriptPath);
/// AI 段已成功、待写回（路径暂空或脚本未结束时推迟）
void AiLogicConvertMarkPendingWriteback(bool pending);
bool AiLogicConvertPendingWriteback();
const std::wstring& AiLogicConvertSessionBlockName();
const std::wstring& AiLogicConvertSessionPrompt();
const std::wstring& AiLogicConvertSessionScriptPath();
int AiLogicConvertSessionSourceNo();

void AiLogicConvertNoteAction(const ScriptAction& a);
void AiLogicConvertNoteLocate(const std::wstring& target, int screenX, int screenY,
    const std::wstring& button, int clickCount, const std::wstring& templatePath);
void AiLogicConvertNoteWindowActivate(const std::wstring& matchQuery);

/// completeTask reason / 结果文本是否像失败收尾（未找到/卡点等）→ 禁止写回
bool AiLogicConvertLooksLikeFailedComplete(const std::wstring& text);
/// 轨迹是否具备可固化内容（写回前短路）
bool AiLogicConvertTranscriptHasConvertible();
/// 综合成功信号：是否允许段末写回（OpenAdapt：验证后再固化）
bool AiLogicConvertShouldWriteback(bool apiOk, bool actionsAlreadyExecuted,
    bool anyActionsExecuted, const std::wstring& completeReason,
    const std::wstring& textResult);

/// 在点击点周围截模板并入库，返回库相对/绝对路径；失败返回空
std::wstring CaptureAiLogicConvertTemplateAt(int screenX, int screenY);

const std::vector<AiLogicTranscriptEntry>& AiLogicConvertTranscript();

/// 生成块名（字母开头）
std::wstring MakeAiLogicBlockName(const std::wstring& preferred, const std::wstring& prompt);

/// 编译轨迹 → DefineBlock：每个找图独立 timer+findImage(限时)+if/else→AI；步间插入短 Wait
AiLogicConvertCompileResult CompileAiLogicConvert(const AiLogicConvertCompileInput& in);

/// 写回脚本文件（UI 线程或段末同步调用均可；内部只碰磁盘）
bool ApplyAiLogicConvertWriteback(const AiLogicConvertWritebackRequest& req, std::wstring& err);

/// 刷写当前会话（pending 或有可固化轨迹）；pathFallback 在会话路径为空时使用。
/// 成功后清除 pending；失败保留会话数据供重试。summaryOut 为成功摘要。
bool TryFlushAiLogicConvertSession(const std::wstring& pathFallback,
    std::wstring& summaryOut, std::wstring& errOut);

/// 若路径上已有同名 DefineBlock 则 true
bool ScriptHasDefineBlock(const std::vector<ScriptAction>& actions, const std::wstring& blockName);

/// 助手硬闸：用户话术是否明确要求 AI 动作执行 / 逻辑转化
bool UserExplicitlyRequestsAiActionExecute(const std::wstring& userText);
bool UserExplicitlyRequestsLogicConvert(const std::wstring& userText);

/// 文本是否像「列表/历史」OCR（多行或含时间/网址启发）
bool LooksLikeListOcrText(const std::wstring& text);

/// 找图模板纹理分（灰度标准差）；过低易假 100% 匹配
double EstimateFindImageTemplateFeatureScore(const std::wstring& imagePath);

/// 本轮助手对话用户原文（工具硬闸用）；空=不允许生成逻辑转化
void SetAgentToolUserContext(const std::wstring& lastUserText);
std::wstring GetAgentToolUserContext();
/// 清除/拒绝未授权的 aiLogicConvert（就地修改动作列表）
int StripUnauthorizedLogicConvert(std::vector<ScriptAction>& actions);
