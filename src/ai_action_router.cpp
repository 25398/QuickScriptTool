#include "ai_action_router.h"

#include "script_action_builder.h"
#include "utils.h"

#include <algorithm>
#include <cwctype>

namespace {

std::wstring ToLowerCopy(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool HasAny(const std::wstring& p, std::initializer_list<const wchar_t*> keys) {
    for (const wchar_t* k : keys) {
        if (p.find(k) != std::wstring::npos) return true;
    }
    return false;
}

bool HasActionVerb(const std::wstring& p) {
    if (HasAny(p, {L"点击", L"双击", L"输入", L"按键", L"拖动", L"滚动", L"打开", L"关闭", L"运行",
            L"按下"})) {
        return true;
    }
    // 「按」单独算动词，但不能把「按钮」里的「按」算进去
    size_t pos = 0;
    while ((pos = p.find(L"按", pos)) != std::wstring::npos) {
        if (pos + 1 < p.size() && p[pos + 1] == L'钮') {
            pos += 2;
            continue;
        }
        return true;
    }
    return false;
}

}  // namespace

std::wstring AiActionRouteLabel(AiActionRouteKind kind) {
    switch (kind) {
    case AiActionRouteKind::VisionQuery: return L"识图问答";
    case AiActionRouteKind::CompositeClick: return L"组合-识图点击";
    case AiActionRouteKind::MultiTurnTools: return L"复杂组合(多轮工具)";
    default: return L"动作执行(工具)";
    }
}

bool IsAiActionVisionQueryPrompt(const std::wstring& prompt) {
    const std::wstring p = Trim(prompt);
    if (p.empty()) return false;
    if (HasActionVerb(p)) return false;
    return HasAny(p, {L"输出", L"坐标", L"识别", L"分析", L"找到", L"在哪里", L"在哪", L"位置",
        L"多少", L"是什么", L"什么颜色", L"描述", L"读", L"检测", L"有没有", L"有几个", L"看出"});
}

bool IsAiActionCompositeClickPrompt(const std::wstring& prompt) {
    const std::wstring p = Trim(prompt);
    if (p.empty()) return false;
    if (IsAiActionComplexCompositePrompt(p)) return false;
    return HasAny(p, {L"点击", L"双击", L"点一下", L"按下"});
}

bool IsAiActionComplexCompositePrompt(const std::wstring& prompt) {
    const std::wstring p = Trim(prompt);
    if (p.empty()) return false;
    if (!HasActionVerb(p)) return false;
    if (HasAny(p, {L"然后", L"接着", L"之后", L"再", L"并且", L"同时", L"先", L"后",
        L"第一步", L"第二步", L"接下来", L"最后"}))
        return true;
    int verbs = 0;
    if (p.find(L"点击") != std::wstring::npos || p.find(L"双击") != std::wstring::npos) ++verbs;
    if (p.find(L"输入") != std::wstring::npos) ++verbs;
    if (p.find(L"按键") != std::wstring::npos || p.find(L"按下") != std::wstring::npos) ++verbs;
    else {
        size_t pos = 0;
        while ((pos = p.find(L"按", pos)) != std::wstring::npos) {
            if (pos + 1 < p.size() && p[pos + 1] == L'钮') {
                pos += 2;
                continue;
            }
            ++verbs;
            break;
        }
    }
    if (p.find(L"打开") != std::wstring::npos) ++verbs;
    if (p.find(L"关闭") != std::wstring::npos) ++verbs;
    return verbs >= 2;
}

AiActionRouteKind ClassifyAiActionRoute(const std::wstring& prompt, bool withImage) {
    if (!withImage) return AiActionRouteKind::ToolExecute;
    if (IsAiActionVisionQueryPrompt(prompt)) return AiActionRouteKind::VisionQuery;
    if (IsAiActionCompositeClickPrompt(prompt)) return AiActionRouteKind::CompositeClick;
    if (IsAiActionComplexCompositePrompt(prompt)) return AiActionRouteKind::MultiTurnTools;
    if (HasActionVerb(prompt)) return AiActionRouteKind::ToolExecute;
    return AiActionRouteKind::VisionQuery;
}

bool TryParseCoordinatePair(const std::wstring& text, int& outX, int& outY) {
    outX = outY = 0;
    const std::wstring s = Trim(text);
    if (s.empty()) return false;

    auto parseInts = [&](size_t i0) -> bool {
        wchar_t* ex = nullptr;
        const long x = wcstol(s.c_str() + i0, &ex, 10);
        if (ex == s.c_str() + i0) return false;
        while (ex < s.c_str() + s.size() && (*ex == L' ' || *ex == L',' || *ex == L'，')) ++ex;
        wchar_t* yEnd = ex;
        const long y = wcstol(ex, &yEnd, 10);
        if (yEnd == ex) return false;
        outX = static_cast<int>(x);
        outY = static_cast<int>(y);
        return true;
    };

    const size_t lp = s.find(L'(');
    const size_t rp = s.find(L')', lp == std::wstring::npos ? 0 : lp);
    if (lp != std::wstring::npos && rp != std::wstring::npos && rp > lp + 1)
        return parseInts(lp + 1);

    for (size_t i = 0; i < s.size(); ++i) {
        if (iswdigit(s[i]) || (s[i] == L'-' && i + 1 < s.size() && iswdigit(s[i + 1]))) {
            if (parseInts(i)) return true;
            break;
        }
    }
    return false;
}

void MapApiPointToScreen(const AiCaptureMapping& map, int apiX, int apiY, int& screenX, int& screenY) {
    const int regionW = std::max(1, map.capX2 - map.capX1);
    const int regionH = std::max(1, map.capY2 - map.capY1);
    const int apiW = std::max(1, map.apiWidth > 0 ? map.apiWidth : map.srcWidth);
    const int apiH = std::max(1, map.apiHeight > 0 ? map.apiHeight : map.srcHeight);
    screenX = map.capX1 + static_cast<int>(static_cast<double>(apiX) * regionW / apiW);
    screenY = map.capY1 + static_cast<int>(static_cast<double>(apiY) * regionH / apiH);
}

std::wstring BuildScreenClickActionsJson(int screenX, int screenY, bool includeStopMacro) {
    std::wstring json = L"[{\"type\":\"moveMouse\",\"x\":" + std::to_wstring(screenX)
        + L",\"y\":" + std::to_wstring(screenY)
        + L"},{\"type\":\"mouseClick\",\"x\":" + std::to_wstring(screenX)
        + L",\"y\":" + std::to_wstring(screenY) + L",\"button\":\"left\"}";
    if (includeStopMacro) json += L",{\"type\":\"stopMacro\"}";
    json += L"]";
    return json;
}

std::wstring MacroActionCompositeSkill() {
    return LR"(【组合任务 — lookupMacroAction section=composite】

模式 A · 简单「找+点」（程序自动，无需 submitMacroActions）：
  prompt 含「点击/双击」且仅一步 → 内部识图定位 + 本地 mouseClick

模式 B · 复杂多步（submitMacroActions + 共享上下文 aiContextMode=1/2/3）：
  1) 可先 aiImageAnalysis / 识图问答定位，结果存 matchVar（如 aiTarget）
  2) moveMouse/mouseClick 使用 moveFromVar + {aiTarget.cx}/{aiTarget.cy}
  3) 或分多轮：第一轮定位，第二轮 submitMacroActions 提交后续动作

推荐动作链模板：
  findImage → mouseClick  （有模板图时最快）
  aiImageAnalysis(输出坐标) → moveMouse(moveFromVar) → mouseClick
  变量：findImage 后 {name}.cx/.cy；AI 定位后写入 aiTarget 再用 {aiTarget.cx}

复杂任务关键词：然后/接着/再/输入 → 走多轮工具，勿Expect单轮完成
)";
}

std::wstring BuildAiActionHybridSystemPrompt(int imageWidth, int imageHeight) {
    std::wstring s =
        L"你是 Windows 宏编排助手，处理带截图的组合任务。\n"
        L"★ 必须用 submitMacroActions 提交可执行动作；复杂任务可拆多轮，历史上下文会保留。\n"
        L"★ 不确定参数 → lookupMacroAction(type 或 section=composite)。\n"
        L"★ 仅当用户明确「输出/识别」且不要操作时，才直接文字回复坐标。\n"
        L"★ 定位后点击：优先 submitMacroActions 输出 moveMouse+mouseClick；"
        L"或 findImage（有图）/ 上一步坐标。\n\n"
        + ScriptActionCatalog() + L"\n";
    if (imageWidth > 0 && imageHeight > 0) {
        s += L"截图/upload " + std::to_wstring(imageWidth) + L"×" + std::to_wstring(imageHeight)
            + L"，坐标相对 upload 图左上角。\n";
    }
    return s;
}

std::wstring BuildCompositeLocatePrompt(const std::wstring& userTask) {
    std::wstring p = Trim(userTask);
    if (p.empty()) p = L"用户指定的目标";
    return L"在截图中找到：" + p + L"\n"
        L"输出目标中心点坐标，格式 (x,y) 整数，相对截图左上角。只输出坐标，不要解释。";
}

std::wstring BuildAiActionVisionQuerySystemPrompt(int imageWidth, int imageHeight) {
    std::wstring s =
        L"你是屏幕截图分析助手。根据用户附带的截图直接回答问题。\n"
        L"只输出用户要求的结果，禁止 markdown、emoji 和多余解释。\n"
        L"若问物体/圆点/按钮的坐标，输出截图坐标系中的整数 (x,y)，相对截图左上角。";
    if (imageWidth > 0 && imageHeight > 0) {
        s += L"\n当前上传截图尺寸 " + std::to_wstring(imageWidth) + L"×"
            + std::to_wstring(imageHeight) + L"，坐标范围约 (0,0)~("
            + std::to_wstring(imageWidth - 1) + L"," + std::to_wstring(imageHeight - 1) + L")。";
    }
    return s;
}
