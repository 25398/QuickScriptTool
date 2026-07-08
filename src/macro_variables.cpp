#include "macro_variables.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

bool TryParseDouble(const std::wstring& text, double& out);

namespace {

void AddFindImageVarItems(const std::wstring& varName, std::vector<QuickInputVarItem>& items) {
    items.push_back({
        varName + L".matchData",
        L"{" + varName + L".matchData}",
        L"找图返回的匹配度",
        varName + L".matchData"
    });
    items.push_back({
        varName + L".x",
        L"{" + varName + L".x}",
        L"找图返回的左上角X",
        varName + L".x"
    });
    items.push_back({
        varName + L".y",
        L"{" + varName + L".y}",
        L"找图返回的左上角Y",
        varName + L".y"
    });
    items.push_back({
        varName + L".x1",
        L"{" + varName + L".x1}",
        L"找图返回的右下角X",
        varName + L".x1"
    });
    items.push_back({
        varName + L".y1",
        L"{" + varName + L".y1}",
        L"找图返回的右下角Y",
        varName + L".y1"
    });
    items.push_back({
        varName + L".cx",
        L"{" + varName + L".cx}",
        L"找图返回的中心X",
        varName + L".cx"
    });
    items.push_back({
        varName + L".cy",
        L"{" + varName + L".cy}",
        L"找图返回的中心Y",
        varName + L".cy"
    });
}

void AddCursorPosVarItems(const std::wstring& varName, std::vector<QuickInputVarItem>& items) {
    items.push_back({
        varName + L".x",
        L"{" + varName + L".x}",
        L"光标横坐标",
        varName + L".x"
    });
    items.push_back({
        varName + L".y",
        L"{" + varName + L".y}",
        L"光标纵坐标",
        varName + L".y"
    });
}

std::wstring LookupMatchVarProperty(const ImageMatchResult& match, const std::wstring& prop) {
    if (!match.found) {
        if (prop == L"matchData" || prop == L"x" || prop == L"y" || prop == L"x1" || prop == L"y1"
            || prop == L"cx" || prop == L"cy") {
            return L"0";
        }
        return L"";
    }
    if (prop == L"matchData") return std::to_wstring(static_cast<int>(match.score));
    if (prop == L"x") return std::to_wstring(match.topLeftX);
    if (prop == L"y") return std::to_wstring(match.topLeftY);
    if (prop == L"x1") return std::to_wstring(match.bottomRightX);
    if (prop == L"y1") return std::to_wstring(match.bottomRightY);
    if (prop == L"cx") return std::to_wstring((match.topLeftX + match.bottomRightX) / 2);
    if (prop == L"cy") return std::to_wstring((match.topLeftY + match.bottomRightY) / 2);
    return L"";
}

void AddOcrSearchVarItems(const std::wstring& varName, std::vector<QuickInputVarItem>& items) {
    items.push_back({
        varName,
        L"{" + varName + L"}",
        L"文字查找是否找到(0/1)",
        varName
    });
    items.push_back({
        varName + L".x",
        L"{" + varName + L".x}",
        L"文字查找左上角X",
        varName + L".x"
    });
    items.push_back({
        varName + L".y",
        L"{" + varName + L".y}",
        L"文字查找左上角Y",
        varName + L".y"
    });
    items.push_back({
        varName + L".x1",
        L"{" + varName + L".x1}",
        L"文字查找右下角X",
        varName + L".x1"
    });
    items.push_back({
        varName + L".y1",
        L"{" + varName + L".y1}",
        L"文字查找右下角Y",
        varName + L".y1"
    });
}

void AddOcrTextVarItems(const std::wstring& varName, std::vector<QuickInputVarItem>& items) {
    items.push_back({
        varName,
        L"{" + varName + L"}",
        L"文字识别结果(字符串)",
        varName
    });
}

std::wstring LookupOcrVarValue(const OcrVarResult& ocr, const std::wstring& prop) {
    if (ocr.mode == OcrVarMode::Text) {
        return prop.empty() ? ocr.text : L"";
    }
    if (prop.empty()) return std::to_wstring(ocr.found);
    if (!ocr.found) {
        if (prop == L"x" || prop == L"y" || prop == L"x1" || prop == L"y1") return L"0";
        return L"";
    }
    if (prop == L"x") return std::to_wstring(ocr.topLeftX);
    if (prop == L"y") return std::to_wstring(ocr.topLeftY);
    if (prop == L"x1") return std::to_wstring(ocr.bottomRightX);
    if (prop == L"y1") return std::to_wstring(ocr.bottomRightY);
    return L"";
}

std::wstring ResolveOcrVarNumericValue(const OcrVarResult& ocr) {
    if (ocr.mode == OcrVarMode::Search) return std::to_wstring(ocr.found);
    double num = 0.0;
    if (TryParseDouble(ocr.text, num)) return std::to_wstring(static_cast<int>(num));
    return L"0";
}

bool LookupOcrVar(const MacroVariableContext& ctx, const std::wstring& varName,
    const std::wstring& prop, std::wstring& out) {
    if (!ctx.ocrVars) return false;
    const auto it = ctx.ocrVars->find(varName);
    if (it == ctx.ocrVars->end()) return false;
    out = LookupOcrVarValue(it->second, prop);
    return true;
}

std::wstring TrimToken(const std::wstring& text) {
    size_t start = 0;
    while (start < text.size() && iswspace(text[start])) ++start;
    size_t end = text.size();
    while (end > start && iswspace(text[end - 1])) --end;
    return text.substr(start, end - start);
}

std::wstring ResolveTimerVarValue(const std::wstring& name, const MacroVariableContext& ctx) {
    if (!ctx.timerStarts || name.empty()) return L"";
    const auto it = ctx.timerStarts->find(name);
    if (it == ctx.timerStarts->end()) return L"";
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - it->second).count();
    return std::to_wstring(static_cast<int>(seconds));
}

bool IsTimerVarName(const std::wstring& name, const MacroVariableContext& ctx) {
    if (!ctx.timerStarts || name.empty()) return false;
    return ctx.timerStarts->find(name) != ctx.timerStarts->end();
}

std::wstring ResolveMacroOperandImpl(const std::wstring& token, const MacroVariableContext& ctx, bool forLoopCount) {
    std::wstring t = TrimToken(token);
    if (t.empty()) return L"";

    if (t.front() == L'{' && t.back() == L'}') {
        return ResolveMacroVariables(t, ctx);
    }

    if (t == L"ctrl:CurLoops()") {
        return std::to_wstring(ctx.curLoops);
    }

    const size_t dot = t.find(L'.');
    if (dot != std::wstring::npos) {
        const std::wstring varName = t.substr(0, dot);
        const std::wstring prop = t.substr(dot + 1);
        if (ctx.matchVars) {
            const auto it = ctx.matchVars->find(varName);
            if (it != ctx.matchVars->end()) {
                return LookupMatchVarProperty(it->second, prop);
            }
        }
        std::wstring ocrVal;
        if (LookupOcrVar(ctx, varName, prop, ocrVal)) return ocrVal;
    } else if (ctx.ocrVars) {
        const auto it = ctx.ocrVars->find(t);
        if (it != ctx.ocrVars->end()) {
            if (forLoopCount) return ResolveOcrVarNumericValue(it->second);
            if (it->second.mode == OcrVarMode::Text) return it->second.text;
            return std::to_wstring(it->second.found);
        }
    }

    if (ctx.aiVars) {
        const auto it = ctx.aiVars->find(t);
        if (it != ctx.aiVars->end()) {
            double num = 0.0;
            if (TryParseDouble(it->second, num)) return std::to_wstring(static_cast<int>(num));
            return it->second;
        }
    }

    if (forLoopCount) {
        const std::wstring timerVal = ResolveTimerVarValue(t, ctx);
        if (!timerVal.empty()) return timerVal;
    }

    if (ctx.loopVars) {
        const auto it = ctx.loopVars->find(t);
        if (it != ctx.loopVars->end()) {
            return std::to_wstring(it->second);
        }
    }

    if (!forLoopCount) {
        const std::wstring timerVal = ResolveTimerVarValue(t, ctx);
        if (!timerVal.empty()) return timerVal;
    }

    double unused;
    if (TryParseDouble(t, unused)) return t;

    return ResolveMacroVariables(L"{" + t + L"}", ctx);
}

}  // namespace

bool TryParseDouble(const std::wstring& text, double& out) {
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    out = wcstod(text.c_str(), &end);
    return end != text.c_str() && *end == L'\0';
}

std::wstring ResolveMacroOperand(const std::wstring& token, const MacroVariableContext& ctx) {
    return ResolveMacroOperandImpl(token, ctx, false);
}

double ResolveFindImageTimeSec(const std::wstring& expr, const MacroVariableContext& ctx) {
    const std::wstring trimmed = TrimToken(expr);
    if (trimmed.empty()) return 0.0;
    double direct = 0.0;
    if (TryParseDouble(trimmed, direct)) return direct;
    const std::wstring resolved = ResolveMacroOperand(trimmed, ctx);
    if (TryParseDouble(resolved, direct)) return direct;
    return 0.0;
}

namespace {

int CompareResolvedValues(const std::wstring& left, const std::wstring& right) {
    double lNum = 0.0, rNum = 0.0;
    if (TryParseDouble(left, lNum) && TryParseDouble(right, rNum)) {
        if (lNum < rNum) return -1;
        if (lNum > rNum) return 1;
        return 0;
    }
    return left.compare(right);
}

bool EvalSingleClause(const std::wstring& clause, const MacroVariableContext& ctx) {
    const std::wstring c = TrimToken(clause);
    if (c.empty()) return false;

    static const struct { const wchar_t* op; size_t len; } ops[] = {
        { L"==", 2 }, { L"!=", 2 }, { L"<=", 2 }, { L">=", 2 },
        { L">>", 2 }, { L"<", 1 }, { L">", 1 },
    };
    for (const auto& op : ops) {
        const size_t pos = c.find(op.op);
        if (pos == std::wstring::npos) continue;
        const std::wstring left = TrimToken(c.substr(0, pos));
        const std::wstring right = TrimToken(c.substr(pos + op.len));
        const std::wstring lVal = ResolveMacroOperand(left, ctx);
        const std::wstring rVal = ResolveMacroOperand(right, ctx);
        if (wcscmp(op.op, L">>") == 0) {
            return lVal.find(rVal) != std::wstring::npos;
        }
        const int cmp = CompareResolvedValues(lVal, rVal);
        if (wcscmp(op.op, L"==") == 0) return cmp == 0;
        if (wcscmp(op.op, L"!=") == 0) return cmp != 0;
        if (wcscmp(op.op, L"<") == 0) return cmp < 0;
        if (wcscmp(op.op, L"<=") == 0) return cmp <= 0;
        if (wcscmp(op.op, L">") == 0) return cmp > 0;
        if (wcscmp(op.op, L">=") == 0) return cmp >= 0;
    }
    return false;
}

struct ConditionPart {
    std::wstring clause;
    std::wstring connector;
};

std::vector<ConditionPart> ParseConditionParts(const std::wstring& expr) {
    std::vector<ConditionPart> parts;
    std::wstring line;
    for (size_t i = 0; i <= expr.size(); ++i) {
        const wchar_t ch = i < expr.size() ? expr[i] : L'\n';
        if (ch == L'\r' || ch == L'\n') {
            if (!line.empty()) {
                std::wstring trimmed = TrimToken(line);
                if (!trimmed.empty()) {
                    ConditionPart part{};
                    size_t connPos = trimmed.rfind(L' ');
                    if (connPos != std::wstring::npos) {
                        const std::wstring maybeConn = TrimToken(trimmed.substr(connPos + 1));
                        if (maybeConn == L"and" || maybeConn == L"or" || maybeConn == L"not") {
                            part.clause = TrimToken(trimmed.substr(0, connPos));
                            part.connector = maybeConn;
                        } else {
                            part.clause = trimmed;
                        }
                    } else {
                        part.clause = trimmed;
                    }
                    if (!part.clause.empty()) parts.push_back(part);
                }
                line.clear();
            }
            if (ch == L'\r' && i + 1 < expr.size() && expr[i + 1] == L'\n') ++i;
            continue;
        }
        line.push_back(ch);
    }
    return parts;
}

}  // namespace

bool TryResolveIntOperand(const std::wstring& token, const MacroVariableContext& ctx, int& out) {
    double num = 0.0;
    if (!TryParseDouble(ResolveMacroOperand(token, ctx), num)) return false;
    out = static_cast<int>(num);
    return true;
}

bool TryResolveIntOperandForLoopCount(const std::wstring& token, const MacroVariableContext& ctx, int& out) {
    double num = 0.0;
    if (!TryParseDouble(ResolveMacroOperandImpl(token, ctx, true), num)) return false;
    out = static_cast<int>(num);
    return true;
}

int ResolveLoopMaxCount(const ScriptAction& action, const MacroVariableContext& ctx,
    std::optional<std::chrono::steady_clock::time_point> loopStartTime) {
    const bool fromVar = action.loopFromVar || !action.loopVarExpr.empty();
    if (!fromVar) return action.loopCount;
    if (action.loopVarExpr.empty()) return 0;
    int maxLoop = 0;
    if (!TryResolveIntOperandForLoopCount(action.loopVarExpr, ctx, maxLoop)) return 0;
    std::wstring timerName = TrimToken(action.loopVarExpr);
    if (timerName.size() >= 2 && timerName.front() == L'{' && timerName.back() == L'}') {
        timerName = TrimToken(timerName.substr(1, timerName.size() - 2));
    }
    if (loopStartTime.has_value() && IsTimerVarName(timerName, ctx)) {
        const double loopElapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - *loopStartTime).count();
        maxLoop -= static_cast<int>(loopElapsed);
        if (maxLoop < 0) maxLoop = 0;
    }
    return maxLoop;
}

bool TryResolveGotoStepNo(const std::wstring& expr, const MacroVariableContext& ctx, int& out) {
    const std::wstring trimmed = TrimToken(expr);
    if (trimmed.empty()) return false;
    return TryResolveIntOperand(trimmed, ctx, out) && out > 0;
}

std::vector<QuickInputVarItem> BuildQuickInputVarItems(const std::vector<ScriptAction>& actions) {
    std::vector<QuickInputVarItem> items;
    std::unordered_set<std::wstring> seen;

    for (const auto& a : actions) {
        if (a.type != ActionType::FindImage || a.matchVarName.empty()) continue;
        if (a.findImageFollowUp != 2) continue;
        if (!seen.insert(a.matchVarName).second) continue;
        AddFindImageVarItems(a.matchVarName, items);
    }
    for (const auto& a : actions) {
        if (a.type != ActionType::GetCursorPos || a.matchVarName.empty()) continue;
        if (!seen.insert(a.matchVarName).second) continue;
        AddCursorPosVarItems(a.matchVarName, items);
    }
    for (const auto& a : actions) {
        if (a.type != ActionType::TextRecognition || a.matchVarName.empty()) continue;
        if (!seen.insert(a.matchVarName).second) continue;
        if (a.ocrResultMode == 1) AddOcrSearchVarItems(a.matchVarName, items);
        else AddOcrTextVarItems(a.matchVarName, items);
    }
    for (const auto& a : actions) {
        if (a.type != ActionType::Loop || a.loopVarName.empty()) continue;
        if (!seen.insert(a.loopVarName).second) continue;
        items.push_back({
            a.loopVarName,
            L"{" + a.loopVarName + L"}",
            L"循环变量:" + a.loopVarName,
            a.loopVarName
        });
    }
    for (const auto& a : actions) {
        if (a.type != ActionType::TimerRecordTime || a.loopVarName.empty()) continue;
        if (!seen.insert(a.loopVarName).second) continue;
        items.push_back({
            a.loopVarName,
            L"{" + a.loopVarName + L"}",
            L"计时器变量:" + a.loopVarName,
            a.loopVarName
        });
    }
    for (const auto& a : actions) {
        if ((a.type != ActionType::AiTextAnalysis && a.type != ActionType::AiImageAnalysis) || a.aiOutputVarName.empty()) continue;
        if (!seen.insert(a.aiOutputVarName).second) continue;
        items.push_back({
            a.aiOutputVarName,
            L"{" + a.aiOutputVarName + L"}",
            L"AI输出变量:" + a.aiOutputVarName,
            a.aiOutputVarName
        });
    }
    items.push_back({
        L"ctrl:CurLoops()",
        L"{ctrl:CurLoops()}",
        L"获取当前宏第几次从头执行",
        L"ctrl:CurLoops()"
    });
    return items;
}

std::wstring ResolveMacroVariables(const std::wstring& text, const MacroVariableContext& ctx) {
    if (text.find(L'{') == std::wstring::npos) return text;

    std::wstring result = text;
    result.reserve(text.size() * 2);  // 预分配空间，减少变量展开时的多次重新分配
    size_t pos = 0;
    while ((pos = result.find(L'{', pos)) != std::wstring::npos) {
        const size_t end = result.find(L'}', pos);
        if (end == std::wstring::npos) break;

        const std::wstring expr = result.substr(pos + 1, end - pos - 1);
        std::wstring replacement;

        if (expr == L"ctrl:CurLoops()") {
            replacement = std::to_wstring(ctx.curLoops);
        } else {
            replacement = ResolveMacroOperand(expr, ctx);
        }

        result.erase(pos, end - pos + 1);
        result.insert(pos, replacement);
        pos += replacement.size();
    }
    return result;
}

std::wstring DecodeQuickInputEscapes(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\\' && i + 1 < text.size()) {
            switch (text[i + 1]) {
            case L'n': out.push_back(L'\n'); ++i; break;
            case L'r': out.push_back(L'\r'); ++i; break;
            case L't': out.push_back(L'\t'); ++i; break;
            case L'\\': out.push_back(L'\\'); ++i; break;
            default: out.push_back(text[i]); break;
            }
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

bool EvaluateConditionExpr(const std::wstring& expr, const MacroVariableContext& ctx) {
    const auto parts = ParseConditionParts(expr);
    if (parts.empty()) return false;

    bool result = EvalSingleClause(parts[0].clause, ctx);
    for (size_t i = 1; i < parts.size(); ++i) {
        const bool next = EvalSingleClause(parts[i].clause, ctx);
        const std::wstring& conn = parts[i - 1].connector;
        if (conn == L"or") result = result || next;
        else if (conn == L"not") result = result && !next;
        else result = result && next;
    }
    return result;
}
