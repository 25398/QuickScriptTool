#include "macro_variables.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

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
}

std::wstring LookupMatchVarProperty(const ImageMatchResult& match, const std::wstring& prop) {
    if (prop == L"matchData") return std::to_wstring(static_cast<int>(match.score));
    if (prop == L"x") return std::to_wstring(match.topLeftX);
    if (prop == L"y") return std::to_wstring(match.topLeftY);
    if (prop == L"x1") return std::to_wstring(match.bottomRightX);
    if (prop == L"y1") return std::to_wstring(match.bottomRightY);
    return L"";
}

std::wstring TrimToken(const std::wstring& text) {
    size_t start = 0;
    while (start < text.size() && iswspace(text[start])) ++start;
    size_t end = text.size();
    while (end > start && iswspace(text[end - 1])) --end;
    return text.substr(start, end - start);
}

std::wstring ResolveConditionOperand(const std::wstring& token, const MacroVariableContext& ctx) {
    std::wstring t = TrimToken(token);
    if (t.empty()) return L"";

    if (t.front() == L'{' && t.back() == L'}') {
        return ResolveMacroVariables(t, ctx);
    }

    if (t == L"ctrl:CurLoops()") {
        return std::to_wstring(ctx.curLoops);
    }

    const size_t dot = t.find(L'.');
    if (dot != std::wstring::npos && ctx.matchVars) {
        const std::wstring varName = t.substr(0, dot);
        const std::wstring prop = t.substr(dot + 1);
        const auto it = ctx.matchVars->find(varName);
        if (it != ctx.matchVars->end()) {
            return LookupMatchVarProperty(it->second, prop);
        }
    }

    if (ctx.loopVars) {
        const auto it = ctx.loopVars->find(t);
        if (it != ctx.loopVars->end()) {
            return std::to_wstring(it->second);
        }
    }

    return ResolveMacroVariables(L"{" + t + L"}", ctx);
}

bool TryParseDouble(const std::wstring& text, double& out) {
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    out = wcstod(text.c_str(), &end);
    return end != text.c_str() && *end == L'\0';
}

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
        const std::wstring lVal = ResolveConditionOperand(left, ctx);
        const std::wstring rVal = ResolveConditionOperand(right, ctx);
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
        if (a.type != ActionType::Loop || a.loopVarName.empty()) continue;
        if (!seen.insert(a.loopVarName).second) continue;
        items.push_back({
            a.loopVarName,
            L"{" + a.loopVarName + L"}",
            L"循环变量:" + a.loopVarName,
            a.loopVarName
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
    size_t pos = 0;
    while ((pos = result.find(L'{', pos)) != std::wstring::npos) {
        const size_t end = result.find(L'}', pos);
        if (end == std::wstring::npos) break;

        const std::wstring expr = result.substr(pos + 1, end - pos - 1);
        std::wstring replacement;

        if (expr == L"ctrl:CurLoops()") {
            replacement = std::to_wstring(ctx.curLoops);
        } else {
            const size_t dot = expr.find(L'.');
            if (dot != std::wstring::npos && ctx.matchVars) {
                const std::wstring varName = expr.substr(0, dot);
                const std::wstring prop = expr.substr(dot + 1);
                const auto it = ctx.matchVars->find(varName);
                if (it != ctx.matchVars->end()) {
                    replacement = LookupMatchVarProperty(it->second, prop);
                }
            } else if (ctx.loopVars) {
                const auto it = ctx.loopVars->find(expr);
                if (it != ctx.loopVars->end()) {
                    replacement = std::to_wstring(it->second);
                }
            }
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
