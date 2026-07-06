#pragma once
// ──────────────────────────────────────────────────────────────────
// ai_action_service.h — AI 宏动作服务层
// 封装宏执行中的 AI 调用、截图、桥接定位等逻辑
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "agent_core.h"
#include "ai_action_router.h"
#include "app_settings.h"
#include "script_types.h"

// ── 将 HBITMAP 编码为 JPEG base64 字符串 ──────────────────────────
std::string BitmapToBase64Jpeg(HBITMAP hBitmap, int quality = 80, double scale = 1.0);

/// 大图自动限边（默认最长边 ≤1280px），降低上传与识图耗时
double ComputeEffectiveAiImageScale(int width, int height, double userScale);

struct AiImageEncodeResult {
    std::string base64;
    int srcWidth = 0;
    int srcHeight = 0;
    int outWidth = 0;
    int outHeight = 0;
    double effectiveScale = 1.0;
};

/// 宏 AI 图片分析专用编码（自动缩放 + 适中 JPEG 质量）
AiImageEncodeResult EncodeBitmapForAiAnalysis(HBITMAP hBitmap, double userScale = 0.5);

using AiMacroLogFn = std::function<void(const std::wstring& line)>;

// ── AiActiveContext ───────────────────────────────────────────────
struct AiActiveContext {
    int contextMode = 0;            // 0=无, 1=宏, 2=循环, 3=块
    int depth = 0;                  // 嵌套深度
    std::wstring modelName;
    std::unique_ptr<AgentCore> core;
};

// ── AiActionResult ────────────────────────────────────────────────
struct AiActionResult {
    bool ok = false;
    /// true 表示 textResult 为识图问答纯文本，不是动作 JSON
    bool visionQueryText = false;
    AiActionRouteKind routeKind = AiActionRouteKind::ToolExecute;
    std::wstring textResult;
    std::wstring errorMessage;
};

// ── 工具函数 ──────────────────────────────────────────────────────
bool IsAgentErrorResponse(const std::wstring& text);

AgentSendCallbacks MakeAiMacroSendCallbacks(
    AiMacroLogFn logFn,
    const std::atomic_bool* cancelFlag = nullptr,
    AiHttpAbortSlot* httpAbort = nullptr);

// 从 savedModels 中按 modelName 查找配置，构建 AgentCore（无工具）
std::unique_ptr<AgentCore> CreateAiActionCore(
    const std::wstring& modelName,
    const std::vector<quickscript::AiModelProfile>& savedModels,
    const std::wstring& fallbackApiUrl,
    const std::wstring& fallbackApiKey,
    const std::wstring& extraSystemPrompt = L"",
    int recvTimeoutMs = 120000,
    double temperatureOverride = -1.0,
    int maxTokensOverride = -1);

// 构建 AI 动作执行专用 AgentCore（注册 macro* 工具供 API 调用）
std::unique_ptr<AgentCore> CreateAiActionExecuteCore(
    const std::wstring& modelName,
    const std::vector<quickscript::AiModelProfile>& savedModels,
    const std::wstring& fallbackApiUrl,
    const std::wstring& fallbackApiKey,
    const std::wstring& extraSystemPrompt,
    int recvTimeoutMs = 120000,
    double temperatureOverride = -1.0);

// 执行 AI 文字分析
AiActionResult ExecuteAiTextAnalysis(
    AgentCore* core,
    const std::wstring& resolvedPrompt,
    int outputType,
    const std::atomic_bool& stopFlag,
    int timeoutSec,
    AiMacroLogFn logFn = nullptr,
    AiHttpAbortSlot* httpAbort = nullptr);

// 执行 AI 图片分析
AiActionResult ExecuteAiImageAnalysis(
    AgentCore* core,
    const std::wstring& resolvedPrompt,
    const std::string& screenshotBase64,
    int outputType,
    const std::atomic_bool& stopFlag,
    int timeoutSec,
    AiMacroLogFn logFn = nullptr,
    AiHttpAbortSlot* httpAbort = nullptr);

// 构建 AI 动作执行的 system prompt（带截图）
std::wstring BuildAiActionExecuteSystemPrompt(
    int captureWidth,
    int captureHeight);

// 构建 AI 动作执行的 system prompt（纯文本）
std::wstring BuildAiActionExecuteTextSystemPrompt();

// 视觉/工具请求需要更长等待；返回 effective 超时秒数
int ResolveAiActionExecuteTimeoutSec(int userTimeoutSec, bool withImage);

// AI 图片分析专用超时（比动作执行更短，默认 90s 起；长 prompt 自动加长）
int ResolveAiImageAnalysisTimeoutSec(int userTimeoutSec, size_t promptChars = 0);

// 构建写入 user 消息的动作执行说明（与图片分析同路径，不用 system prompt）
std::wstring BuildAiActionExecuteUserInstruction(
    const std::wstring& taskPrompt,
    int captureWidth,
    int captureHeight,
    bool withImage);

// 执行 AI 动作执行（混合路由：识图问答 / 组合点击 / 工具 / 多轮）
AiActionResult ExecuteAiActionExecute(
    AgentCore* core,
    const std::wstring& resolvedPrompt,
    const std::string& screenshotBase64,
    int captureWidth,
    int captureHeight,
    int contextMode,
    const std::atomic_bool& stopFlag,
    int timeoutSec,
    AiMacroLogFn logFn = nullptr,
    AiHttpAbortSlot* httpAbort = nullptr,
    const AiCaptureMapping* captureMapping = nullptr);
