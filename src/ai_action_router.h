#pragma once
// ──────────────────────────────────────────────────────────────────
// ai_action_router.h — AI 动作执行混合路由（方案 E）
// ──────────────────────────────────────────────────────────────────

#include <string>

enum class AiActionRouteKind {
    ToolExecute,       // 纯动作 / 文本：tools 单轮（AgentCore 内多轮 tool-call）
    VisionQuery,       // 识图问答：无 tools，直接文本
    CompositeClick,    // 简单组合：1 次识图 + 本地点击
    MultiTurnTools,    // 复杂组合：tools + 可选共享上下文、多轮 user 消息
};

/// 截图区域与上传图之间的坐标映射
struct AiCaptureMapping {
    int capX1 = 0;
    int capY1 = 0;
    int capX2 = 0;
    int capY2 = 0;
    int srcWidth = 0;
    int srcHeight = 0;
    int apiWidth = 0;
    int apiHeight = 0;
};

std::wstring AiActionRouteLabel(AiActionRouteKind kind);

AiActionRouteKind ClassifyAiActionRoute(const std::wstring& prompt, bool withImage);

bool IsAiActionVisionQueryPrompt(const std::wstring& prompt);

bool IsAiActionCompositeClickPrompt(const std::wstring& prompt);

bool IsAiActionComplexCompositePrompt(const std::wstring& prompt);

/// 从 AI 文本中解析 (x,y) / x,y
bool TryParseCoordinatePair(const std::wstring& text, int& outX, int& outY);

void MapApiPointToScreen(const AiCaptureMapping& map, int apiX, int apiY, int& screenX, int& screenY);

/// 构建 moveMouse + mouseClick (+ stopMacro) 动作 JSON
std::wstring BuildScreenClickActionsJson(int screenX, int screenY, bool includeStopMacro = true);

/// 组合任务 Skill（lookupMacroAction section=composite）
std::wstring MacroActionCompositeSkill();

std::wstring BuildAiActionHybridSystemPrompt(int imageWidth, int imageHeight);

/// 简单「找目标并点击」的识图 prompt
std::wstring BuildCompositeLocatePrompt(const std::wstring& userTask);

std::wstring BuildAiActionVisionQuerySystemPrompt(int imageWidth, int imageHeight);
