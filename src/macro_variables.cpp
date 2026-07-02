#include "macro_variables.h"

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
