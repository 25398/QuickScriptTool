// ──────────────────────────────────────────────────────────────────
// window_ai_profile.cpp — 窗口反代的纯逻辑实现
//
// ⚠ 本 TU 只 include UIAutomation.h 取**常量**（控件类型 id），**不调用任何 UIA 函数**
//   ⇒ 自检目标链它不需要 uiautomationcore，也不需要窗口/COM。
//   （数值单一事实来源：不自己抄 50004 这种魔数，抄了必然漂移。）
// ──────────────────────────────────────────────────────────────────

#include "web_ai/window_ai_profile.h"

#include <UIAutomation.h>

#include <algorithm>
#include <cwctype>

namespace quickscript::webai {

using json = nlohmann::json;

namespace {

std::wstring ToLowerW(const std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (wchar_t c : s) o.push_back(static_cast<wchar_t>(towlower(c)));
    return o;
}

/// ★★ UTF-8 → UTF-16（**必须真的解码，不能逐字节加宽**）
///
/// 踩过的坑（自检当场抓出，两条用例同时转红）：配置文件里的中文判据
/// （`"nameContains": ["输入"]`、别名 `"豆包客户端"`）是 **UTF-8 字节**，
/// 而 `std::wstring(s.begin(), s.end())` 会把每个字节当成一个 wchar
/// ⇒ 「输入」变成两个乱码字符 ⇒ **判据永远匹配不上，而且是静默的**
/// （探针会说"没找到输入框"，而配置看着完全正确）。
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
        nullptr, 0);
    if (n <= 0) {
        // 非法 UTF-8：退化成逐字节（至少不崩），但**不要**假装它是对的
        return std::wstring(s.begin(), s.end());
    }
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool ContainsNoCase(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return false;
    return ToLowerW(hay).find(ToLowerW(needle)) != std::wstring::npos;
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    return ToLowerW(a) == ToLowerW(b);
}

std::wstring TrimW(const std::wstring& s) {
    size_t b = 0;
    size_t e = s.size();
    auto isWs = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x3000;
    };
    while (b < e && isWs(s[b])) ++b;
    while (e > b && isWs(s[e - 1])) --e;
    return s.substr(b, e - b);
}

/// 结构：id → 英文名（配置里写的名字，必须稳定；中文标签会随语言变）
struct TypeNameRow {
    int id;
    const wchar_t* name;
};

const TypeNameRow kTypeNames[] = {
    {UIA_ButtonControlTypeId, L"Button"},
    {UIA_CalendarControlTypeId, L"Calendar"},
    {UIA_CheckBoxControlTypeId, L"CheckBox"},
    {UIA_ComboBoxControlTypeId, L"ComboBox"},
    {UIA_EditControlTypeId, L"Edit"},
    {UIA_HyperlinkControlTypeId, L"Hyperlink"},
    {UIA_ImageControlTypeId, L"Image"},
    {UIA_ListItemControlTypeId, L"ListItem"},
    {UIA_ListControlTypeId, L"List"},
    {UIA_MenuControlTypeId, L"Menu"},
    {UIA_MenuBarControlTypeId, L"MenuBar"},
    {UIA_MenuItemControlTypeId, L"MenuItem"},
    {UIA_ProgressBarControlTypeId, L"ProgressBar"},
    {UIA_RadioButtonControlTypeId, L"RadioButton"},
    {UIA_ScrollBarControlTypeId, L"ScrollBar"},
    {UIA_SliderControlTypeId, L"Slider"},
    {UIA_SpinnerControlTypeId, L"Spinner"},
    {UIA_StatusBarControlTypeId, L"StatusBar"},
    {UIA_TabControlTypeId, L"Tab"},
    {UIA_TabItemControlTypeId, L"TabItem"},
    {UIA_TextControlTypeId, L"Text"},
    {UIA_ToolBarControlTypeId, L"ToolBar"},
    {UIA_ToolTipControlTypeId, L"ToolTip"},
    {UIA_TreeControlTypeId, L"Tree"},
    {UIA_TreeItemControlTypeId, L"TreeItem"},
    {UIA_CustomControlTypeId, L"Custom"},
    {UIA_GroupControlTypeId, L"Group"},
    {UIA_ThumbControlTypeId, L"Thumb"},
    {UIA_DataGridControlTypeId, L"DataGrid"},
    {UIA_DataItemControlTypeId, L"DataItem"},
    {UIA_DocumentControlTypeId, L"Document"},
    {UIA_SplitButtonControlTypeId, L"SplitButton"},
    {UIA_WindowControlTypeId, L"Window"},
    {UIA_PaneControlTypeId, L"Pane"},
    {UIA_HeaderControlTypeId, L"Header"},
    {UIA_HeaderItemControlTypeId, L"HeaderItem"},
    {UIA_TableControlTypeId, L"Table"},
    {UIA_TitleBarControlTypeId, L"TitleBar"},
    {UIA_SeparatorControlTypeId, L"Separator"},
    {UIA_SemanticZoomControlTypeId, L"SemanticZoom"},
    {UIA_AppBarControlTypeId, L"AppBar"},
};

/// 名字里出现这些，说明更像"输入区"（含中文关键词：国产客户端 UIA 名字常是中文）
bool LooksLikeComposerName(const std::wstring& name) {
    static const wchar_t* kKeys[] = {
        L"输入", L"提问", L"发送", L"message", L"chat", L"prompt", L"ask", L"composer",
        L"输入框", L"说点什么", L"发消息",
    };
    for (const wchar_t* k : kKeys) {
        if (ContainsNoCase(name, k)) return true;
    }
    return false;
}

std::vector<std::wstring> JsonStringArray(const json& obj, const char* key,
    const std::vector<std::wstring>& fallback) {
    if (!obj.is_object() || !obj.contains(key) || !obj[key].is_array()) return fallback;
    std::vector<std::wstring> out;
    for (const auto& v : obj[key]) {
        if (!v.is_string()) continue;
        const std::string s = v.get<std::string>();
        if (!s.empty()) out.push_back(Utf8ToWide(s));
    }
    return out;
}

}  // namespace

const wchar_t* WindowAiControlTypeName(int controlTypeId) {
    for (const auto& row : kTypeNames) {
        if (row.id == controlTypeId) return row.name;
    }
    return L"";
}

int WindowAiControlTypeIdFromName(const std::wstring& name) {
    if (name.empty()) return 0;
    for (const auto& row : kTypeNames) {
        if (EqualsNoCase(name, row.name)) return row.id;
    }
    // 宽容：允许写 "Text/Edit"、"edit " 这种，或直接写数字
    for (const auto& row : kTypeNames) {
        if (ContainsNoCase(name, row.name)) return row.id;
    }
    return 0;
}

std::vector<WindowAiClientProfile> BuiltinWindowAiClients() {
    std::vector<WindowAiClientProfile> out;

    // ── 豆包客户端（Electron 系；方案 §3.4 点名的第一个验证对象）─────────
    {
        WindowAiClientProfile p;
        p.id = "doubao-app";
        p.label = "豆包客户端";
        p.aliases = {L"豆包客户端", L"豆包App", L"doubao-client"};
        p.processNames = {L"Doubao.exe", L"doubao.exe", L"豆包.exe"};
        p.titleContains = {L"豆包"};
        p.input.controlTypes = {L"Edit", L"Document"};
        p.input.minWidth = 160;
        p.input.minHeight = 18;
        p.input.pick = L"largest-lowest";
        p.reply.containerTypes = {L"Document", L"List", L"Pane", L"Group"};
        p.reply.maxChars = 12000;
        p.submitMethod = L"enter";
        p.sendButtonNames = {L"发送", L"Send"};
        out.push_back(std::move(p));
    }

    // ── Cursor（VS Code 分支；Electron，UIA 树质量较好）──────────────────
    {
        WindowAiClientProfile p;
        p.id = "cursor";
        p.label = "Cursor";
        p.aliases = {L"cursor-app", L"cursor编辑器"};
        p.processNames = {L"Cursor.exe", L"cursor.exe"};
        p.titleContains = {};                 // 标题随文件变，不做限制
        p.titleExcludes = {L"Welcome", L"欢迎"};  // 空欢迎窗没有会话可读
        // VS Code/Cursor 的输入区是 contenteditable ⇒ UIA 里是 Document；
        // ⚠ 但**主编辑器也是 Document** ⇒ 必须开安全闸（见 requireHints 的长注释）。
        //   默认**留空两张提示名单** = 显式"未校准"：先用探针 dump 出真实
        //   AutomationId/Name，填进 window_ai_providers.json 之后功能才启用。
        //   这是刻意的失效安全：宁可 NO_INPUT，也不把提示词打进用户的代码里。
        p.input.controlTypes = {L"Edit", L"Document"};
        p.input.requireHints = true;
        p.input.minWidth = 120;
        p.input.minHeight = 16;
        p.input.pick = L"largest-lowest";
        p.reply.containerTypes = {L"Document", L"List", L"Pane", L"Group"};
        p.reply.maxChars = 12000;
        p.submitMethod = L"enter";
        p.sendButtonNames = {L"Send", L"发送", L"Submit"};
        out.push_back(std::move(p));
    }

    // ── Codex / 终端 TUI（方案 §3.3 判为"低可行度"，保留为尽力而为档）───
    //   Windows Terminal 对 UIA 暴露文本区（TextPattern 可用），但**没有输入控件**：
    //   输入区就是整个终端文档 ⇒ 判据是"面积最大的 Document/Text"，提交靠回车。
    //   ⚠ 这一档**不建议**当主力：TUI 逐行重绘，稳定判据容易把"光标闪烁"当成内容变化。
    {
        WindowAiClientProfile p;
        p.id = "codex";
        p.label = "Codex / 终端 TUI（尽力而为）";
        p.aliases = {L"codex-cli", L"终端AI"};
        p.processNames = {L"WindowsTerminal.exe", L"wt.exe"};
        p.titleContains = {};      // 用户可覆盖成 {"codex"} 以限定那个标签页
        p.input.controlTypes = {L"Document", L"Edit", L"Text"};
        p.input.minWidth = 200;
        p.input.minHeight = 40;
        p.input.requireFocusable = false;   // 终端文档常常不报 focusable
        p.input.pick = L"largest";
        p.reply.containerTypes = {L"Document", L"Text", L"Pane"};
        p.submitMethod = L"enter";
        p.idleTimeoutMs = 12000;   // TUI 重绘慢，给宽一点
        out.push_back(std::move(p));
    }

    return out;
}

std::vector<WindowAiClientProfile> MergeWindowAiProfiles(const std::string& jsonUtf8,
    const std::vector<WindowAiClientProfile>& base, std::string& err) {
    err.clear();
    std::vector<WindowAiClientProfile> out = base;
    if (jsonUtf8.empty()) return out;

    const json doc = json::parse(jsonUtf8, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        err = "window_ai_providers.json 不是合法 JSON 对象（已忽略，继续用内置档案）";
        return out;
    }
    const json* clients = nullptr;
    if (doc.contains("clients") && doc["clients"].is_object()) clients = &doc["clients"];
    else if (doc.contains("providers") && doc["providers"].is_object()) clients = &doc["providers"];
    if (!clients) {
        err = "window_ai_providers.json 里没有 clients 对象（已忽略）";
        return out;
    }

    for (auto it = clients->begin(); it != clients->end(); ++it) {
        const std::string id = it.key();
        if (id.empty() || !it.value().is_object()) continue;
        const json& src = it.value();

        auto found = std::find_if(out.begin(), out.end(),
            [&](const WindowAiClientProfile& p) { return p.id == id; });
        WindowAiClientProfile p = (found != out.end()) ? *found : WindowAiClientProfile{};
        p.id = id;
        if (src.contains("label") && src["label"].is_string()) {
            p.label = src["label"].get<std::string>();
        } else if (p.label.empty()) {
            p.label = id;
        }
        p.aliases = JsonStringArray(src, "aliases", p.aliases);
        p.processNames = JsonStringArray(src, "processNames", p.processNames);
        p.titleContains = JsonStringArray(src, "titleContains", p.titleContains);
        p.titleExcludes = JsonStringArray(src, "titleExcludes", p.titleExcludes);
        if (src.contains("minWindowWidth") && src["minWindowWidth"].is_number_integer())
            p.minWindowWidth = std::clamp(src["minWindowWidth"].get<int>(), 80, 20000);
        if (src.contains("minWindowHeight") && src["minWindowHeight"].is_number_integer())
            p.minWindowHeight = std::clamp(src["minWindowHeight"].get<int>(), 80, 20000);
        p.sendButtonNames = JsonStringArray(src, "sendButtonNames", p.sendButtonNames);
        if (src.contains("submitMethod") && src["submitMethod"].is_string()) {
            const std::string m = src["submitMethod"].get<std::string>();
            if (!m.empty()) p.submitMethod = Utf8ToWide(m);
        }
        if (src.contains("idleTimeoutMs") && src["idleTimeoutMs"].is_number_integer()) {
            p.idleTimeoutMs = std::clamp(src["idleTimeoutMs"].get<int>(), 1000, 120000);
        }
        if (src.contains("maxTotalMs") && src["maxTotalMs"].is_number_integer()) {
            p.maxTotalMs = std::clamp(src["maxTotalMs"].get<int>(), 5000, 900000);
        }
        if (src.contains("input") && src["input"].is_object()) {
            const json& in = src["input"];
            p.input.controlTypes = JsonStringArray(in, "controlTypes", p.input.controlTypes);
            p.input.nameContains = JsonStringArray(in, "nameContains", p.input.nameContains);
            p.input.automationIdContains = JsonStringArray(in, "automationIdContains",
                p.input.automationIdContains);
            if (in.contains("minWidth") && in["minWidth"].is_number_integer())
                p.input.minWidth = std::clamp(in["minWidth"].get<int>(), 10, 5000);
            if (in.contains("minHeight") && in["minHeight"].is_number_integer())
                p.input.minHeight = std::clamp(in["minHeight"].get<int>(), 8, 2000);
            if (in.contains("requireFocusable") && in["requireFocusable"].is_boolean())
                p.input.requireFocusable = in["requireFocusable"].get<bool>();
            if (in.contains("requireHints") && in["requireHints"].is_boolean())
                p.input.requireHints = in["requireHints"].get<bool>();
            if (in.contains("abortIfInputNotEmpty") && in["abortIfInputNotEmpty"].is_boolean())
                p.input.abortIfInputNotEmpty = in["abortIfInputNotEmpty"].get<bool>();
            // ★ 几何兜底矩形（相对客户区 0..1，4 个数）。⚠ 个数不对**直接忽略**而不是
            //   取前 4 个：它会真的点下去并打字，半份矩形等于点到不知道哪里去。
            p.input.placeholderHints = JsonStringArray(in, "placeholderHints", p.input.placeholderHints);
        // ★ 输入方式：`background`（默认，不抢前台）| `foreground`（老行为）
        //   ⚠ 用户明确要求"不能抢前台"，所以配置里写了才改成前台。
        {
            const std::string im = (src.contains("inputMethod") && src["inputMethod"].is_string())
                ? src["inputMethod"].get<std::string>() : std::string();
            if (!im.empty()) p.inputMethod = Utf8ToWide(im);
            if (src.contains("allowForegroundFallback") && src["allowForegroundFallback"].is_boolean()) {
                p.allowForegroundFallback = src["allowForegroundFallback"].get<bool>();
            }
            p.newConversationButtonNames = JsonStringArray(src, "newConversationButtonNames",
                p.newConversationButtonNames);
        }
            if (in.contains("rectHint") && in["rectHint"].is_array()) {
                std::vector<double> rel;
                for (const auto& v : in["rectHint"]) {
                    if (v.is_number()) rel.push_back(v.get<double>());
                }
                p.input.rectHint = (rel.size() == 4) ? rel : std::vector<double>{};
            }
            if (in.contains("pick") && in["pick"].is_string()) {
                const std::string s = in["pick"].get<std::string>();
                if (!s.empty()) p.input.pick = Utf8ToWide(s);
            }
        }
        if (src.contains("reply") && src["reply"].is_object()) {
            const json& r = src["reply"];
            p.reply.containerTypes = JsonStringArray(r, "containerTypes", p.reply.containerTypes);
            if (r.contains("maxChars") && r["maxChars"].is_number_integer())
                p.reply.maxChars = std::clamp(r["maxChars"].get<int>(), 200, 200000);
            if (r.contains("preferTextPattern") && r["preferTextPattern"].is_boolean())
                p.reply.preferTextPattern = r["preferTextPattern"].get<bool>();
            if (r.contains("ocrFallback") && r["ocrFallback"].is_boolean())
                p.reply.ocrFallback = r["ocrFallback"].get<bool>();
            // ★ 剪贴板通道（首选读法）
            if (r.contains("useClipboard") && r["useClipboard"].is_boolean())
                p.reply.useClipboard = r["useClipboard"].get<bool>();
            if (r.contains("restoreClipboard") && r["restoreClipboard"].is_boolean())
                p.reply.restoreClipboard = r["restoreClipboard"].get<bool>();
            if (r.contains("clipboardWaitMs") && r["clipboardWaitMs"].is_number_integer())
                p.reply.clipboardWaitMs = std::clamp(r["clipboardWaitMs"].get<int>(), 200, 30000);
            p.reply.copyButtonNames = JsonStringArray(r, "copyButtonNames", p.reply.copyButtonNames);
            p.reply.copyButtonIds = JsonStringArray(r, "copyButtonIds", p.reply.copyButtonIds);
            if (r.contains("copyButtonHint") && r["copyButtonHint"].is_array()) {
                std::vector<double> rel;
                for (const auto& v : r["copyButtonHint"]) {
                    if (v.is_number()) rel.push_back(v.get<double>());
                }
                p.reply.copyButtonHint = (rel.size() == 4) ? rel : std::vector<double>{};
            }
            // ★ 几何锚点那几个字段**必须一起解析** —— 漏了它们，json 里写了也不生效，
            //   而表现是"锚点抓到了侧栏里的文本、点下去什么也没发生"（实测踩到）：
            //   配置值看起来生效了（文件里明明写着 0.30），实际用的还是默认 0.24。
            if (r.contains("copyAnchorFallback") && r["copyAnchorFallback"].is_boolean())
                p.reply.copyAnchorFallback = r["copyAnchorFallback"].get<bool>();
            if (r.contains("copyColumnLeftRel") && r["copyColumnLeftRel"].is_number())
                p.reply.copyColumnLeftRel = std::clamp(r["copyColumnLeftRel"].get<double>(), 0.0, 1.0);
            if (r.contains("copyColumnRightRel") && r["copyColumnRightRel"].is_number())
                p.reply.copyColumnRightRel = std::clamp(r["copyColumnRightRel"].get<double>(), 0.0, 1.0);
            if (r.contains("copyButtonXRel") && r["copyButtonXRel"].is_number())
                p.reply.copyButtonXRel = std::clamp(r["copyButtonXRel"].get<double>(), 0.0, 1.0);
            if (r.contains("copyButtonOffset") && r["copyButtonOffset"].is_array()) {
                std::vector<double> off;
                for (const auto& v : r["copyButtonOffset"]) {
                    if (v.is_number()) off.push_back(v.get<double>());
                }
                if (off.size() == 2) p.reply.copyButtonOffset = off;
            }
            if (r.contains("ocrRegion") && r["ocrRegion"].is_array()) {                std::vector<double> rel;
                for (const auto& v : r["ocrRegion"]) {
                    if (v.is_number()) rel.push_back(v.get<double>());
                }
                p.reply.ocrRegion = (rel.size() == 4) ? rel : std::vector<double>{};
            }
        }

        if (found != out.end()) *found = std::move(p);
        else out.push_back(std::move(p));
    }
    return out;
}

int FindWindowAiClient(const std::vector<WindowAiClientProfile>& clients,
    const std::string& modelOrIdUtf8) {
    if (modelOrIdUtf8.empty()) return -1;
    const std::wstring want = Utf8ToWide(modelOrIdUtf8);
    for (size_t i = 0; i < clients.size(); ++i) {
        if (EqualsNoCase(Utf8ToWide(clients[i].id), want)) {
            return static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < clients.size(); ++i) {
        for (const auto& a : clients[i].aliases) {
            if (EqualsNoCase(a, want)) return static_cast<int>(i);
        }
    }
    const std::wstring lower = ToLowerW(want);
    for (size_t i = 0; i < clients.size(); ++i) {
        const std::wstring id = Utf8ToWide(clients[i].id);
        if (!id.empty() && lower.find(ToLowerW(id)) != std::wstring::npos) {
            return static_cast<int>(i);
        }
        for (const auto& a : clients[i].aliases) {
            if (!a.empty() && lower.find(ToLowerW(a)) != std::wstring::npos) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

bool WindowAiWindowMatches(const WindowAiClientProfile& profile,
    const std::wstring& processName, const std::wstring& title) {
    for (const auto& ex : profile.titleExcludes) {
        if (ContainsNoCase(title, ex)) return false;
    }
    bool procOk = profile.processNames.empty();
    for (const auto& p : profile.processNames) {
        if (EqualsNoCase(processName, p)) { procOk = true; break; }
    }
    if (!procOk) return false;
    if (profile.titleContains.empty()) return true;
    for (const auto& t : profile.titleContains) {
        if (ContainsNoCase(title, t)) return true;
    }
    return false;
}

int ScoreWindowAiInput(const WindowAiInputSpec& spec, const WindowAiUiNode& node,
    const RECT& clientArea) {
    if (!node.enabled || node.offscreen) return -1;
    if (node.Width() < spec.minWidth || node.Height() < spec.minHeight) return -1;
    if (spec.requireFocusable && !node.focusable) return -1;

    // ★★「整页容器」不是输入框 —— 实测踩到（2026-09-26，豆包客户端）：
    //   Chromium 把整个页面暴露成一个 `automationId == "RootWebArea"` 的 Document，
    //   面积 ≈ 整个客户区，而且**同时支持 ValuePattern + TextPattern + 可聚焦**
    //   ⇒ 打分 97 分、排第一。真走"给它 SetValue"就等于往**整页**设值。
    //   两条判据一起挡：① 名字就是 RootWebArea 的容器；② 面积占客户区 ≥60% 的控件
    //   （聊天输入框永远不可能是窗口的大部分）。
    if (EqualsNoCase(node.automationId, L"RootWebArea")) return -1;
    const long long clientAreaPx = static_cast<long long>(
        (std::max)(0, static_cast<int>(clientArea.right - clientArea.left)))
        * (std::max)(0, static_cast<int>(clientArea.bottom - clientArea.top));
    if (clientAreaPx > 0 && node.Area() * 100 >= clientAreaPx * 60) return -1;

    // ★★安全闸（先于一切"像不像输入框"的加分）：
    //   这类客户端里"主编辑器"也是 Document，而且面积最大、位置也靠下 ——
    //   光靠类型+面积打分很可能把提示词**打进代码文件**里（真·破坏性）。
    //   所以：要求显式提示词命中，且**配置未填提示词时一个候选都不给**（失效安全）。
    if (spec.requireHints) {
        if (spec.nameContains.empty() && spec.automationIdContains.empty()) return -1;
        bool hit = false;
        for (const auto& n : spec.nameContains) {
            if (ContainsNoCase(node.name, n)) { hit = true; break; }
        }
        if (!hit) {
            for (const auto& a : spec.automationIdContains) {
                if (ContainsNoCase(node.automationId, a)) { hit = true; break; }
            }
        }
        if (!hit) return -1;
    }

    if (!spec.controlTypes.empty()) {
        bool typeOk = false;
        for (const auto& t : spec.controlTypes) {
            if (EqualsNoCase(t, node.controlType)) { typeOk = true; break; }
        }
        if (!typeOk) return -1;
    }
    if (!spec.nameContains.empty()) {
        bool ok = false;
        for (const auto& n : spec.nameContains) {
            if (ContainsNoCase(node.name, n)) { ok = true; break; }
        }
        if (!ok) return -1;
    }
    if (!spec.automationIdContains.empty()) {
        bool ok = false;
        for (const auto& a : spec.automationIdContains) {
            if (ContainsNoCase(node.automationId, a)) { ok = true; break; }
        }
        if (!ok) return -1;
    }

    int score = 0;
    // 面积：压缩到 0~60（否则一个巨大的 Document 会把真正的输入框压死）
    const long long area = node.Area();
    score += static_cast<int>((std::min)(60LL, area / 20000));
    // 位置：在客户区下半部 = 更像聊天输入框
    const int ch = clientArea.bottom - clientArea.top;
    if (ch > 0 && node.rect.top >= clientArea.top + ch * 45 / 100) score += 40;
    // 能力
    if (node.valuePattern) score += 15;   // 能直接设值 ⇒ 最省事
    if (node.textPattern) score += 10;
    if (node.keyboardFocus) score += 50;  // 已经聚焦 ⇒ 十有八九就是它
    // 类型偏好
    if (EqualsNoCase(node.controlType, L"Edit")) score += 20;
    else if (EqualsNoCase(node.controlType, L"Document")) score += 12;
    else if (EqualsNoCase(node.controlType, L"Custom")) score -= 10;
    // 名字提示
    if (LooksLikeComposerName(node.name)) score += 25;
    // 太扁（<24px 高）在聊天客户端里通常是**会话里的单行文本**，不是输入框
    if (node.Height() < 24) score -= 20;
    return score;
}

int PickWindowAiInput(const WindowAiInputSpec& spec,
    const std::vector<WindowAiUiNode>& nodes, const RECT& clientArea) {
    const std::wstring pick = ToLowerW(spec.pick.empty() ? L"largest-lowest" : spec.pick);
    const int ch = clientArea.bottom - clientArea.top;
    const int lowerGate = (ch > 0) ? (clientArea.top + ch * 45 / 100) : 0;

    int best = -1;
    int bestScore = -1;
    long long bestArea = -1;
    int bestTop = INT_MIN;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const int score = ScoreWindowAiInput(spec, nodes[i], clientArea);
        if (score < 0) continue;

        // ① largest-lowest：先只在下半部挑；下半部一个都没有才退到全窗
        if (pick == L"largest-lowest" && ch > 0 && nodes[i].rect.top < lowerGate) {
            bool anyLower = false;
            for (const auto& n : nodes) {
                if (ScoreWindowAiInput(spec, n, clientArea) >= 0 && n.rect.top >= lowerGate) {
                    anyLower = true;
                    break;
                }
            }
            if (anyLower) continue;
        }

        const long long area = nodes[i].Area();
        const int top = nodes[i].rect.top;
        bool better = false;
        if (pick == L"lowest") {
            better = (top > bestTop) || (top == bestTop && area > bestArea);
        } else if (pick == L"largest" || pick == L"largest-lowest") {
            // 质量分优先，其次面积（同一份判据命中多个时，分数高的更像输入框）
            better = (score > bestScore) || (score == bestScore && area > bestArea);
        } else {
            better = (score > bestScore);
        }
        if (best < 0 || better) {
            best = static_cast<int>(i);
            bestScore = score;
            bestArea = area;
            bestTop = top;
        }
    }
    return best;
}

bool ResolveRelativeRect(const std::vector<double>& rel, const RECT& clientArea, RECT& out) {
    if (rel.size() != 4) return false;
    for (double v : rel) {
        // 含 NaN（NaN 与任何比较都为 false）与越界值
        if (!(v >= 0.0 && v <= 1.0)) return false;
    }
    if (rel[2] <= rel[0] || rel[3] <= rel[1]) return false;
    const int cw = clientArea.right - clientArea.left;
    const int ch = clientArea.bottom - clientArea.top;
    if (cw <= 0 || ch <= 0) return false;
    RECT r{};
    r.left = clientArea.left + static_cast<int>(rel[0] * cw + 0.5);
    r.top = clientArea.top + static_cast<int>(rel[1] * ch + 0.5);
    r.right = clientArea.left + static_cast<int>(rel[2] * cw + 0.5);
    r.bottom = clientArea.top + static_cast<int>(rel[3] * ch + 0.5);
    if (r.right <= r.left || r.bottom <= r.top) return false;
    out = r;
    return true;
}

bool WindowAiLooksLikePlaceholder(const std::vector<std::wstring>& hints,
    const std::wstring& textIn) {
    const std::wstring text = TrimW(textIn);
    if (text.empty()) return true;               // 什么都没有 = 空框
    if (text.size() > 40) return false;          // 太长 ⇒ 一定是真内容（判严）
    std::wstring head;
    int taken = 0;
    for (wchar_t c : text) {
        if (c == L' ' || c == L'\t' || c == L'\n' || c == L'　') continue;
        head.push_back(c);
        if (++taken >= 6) break;                 // 只看开头 6 个非空白字符
    }
    const std::wstring lowerHead = ToLowerW(head);
    for (const auto& h : hints) {
        if (h.empty()) continue;
        const std::wstring hl = ToLowerW(h);
        if (lowerHead.rfind(hl, 0) == 0) return true;              // 提示词在开头
        if (lowerHead.find(hl) != std::wstring::npos) return true;  // 或落在开头 6 字内
    }
    // ★★ 模糊档：**OCR 会把占位符读花**，精确匹配必然落空 —— 实测
    //   「发消息或按住空格说话…」被读成「发氵肖息或按任空格说话“」（"消"被拆成"氵肖"、
    //   "按住"读成"按任"）⇒ 任何提示词都匹配不上 ⇒ 空框被当成用户草稿 ⇒ **永远拒绝工作**。
    //   判据：**字符集重合度** ≥ 60%（不看顺序，能容忍拆字/错字），且整段很短。
    //   ⚠ 仍然判严：既要求短（≤40），又要求重合度高 —— 用户草稿里通常不会大段命中占位语。
    if (text.size() <= 40) {
        for (const auto& h : hints) {
            if (h.size() < 3) continue;
            size_t hit = 0;
            for (wchar_t ch : h) {
                const wchar_t lc = static_cast<wchar_t>(towlower(ch));
                if (lc == L' ' || lc == L'\t') continue;
                if (text.find(ch) != std::wstring::npos
                    || ToLowerW(text).find(lc) != std::wstring::npos) {
                    ++hit;
                }
            }
            size_t need = 0;
            for (wchar_t ch : h) {
                if (ch == L' ' || ch == L'\t') continue;
                ++need;
            }
            if (need >= 3 && hit * 10 >= need * 6) return true;   // ≥60% 命中
        }
    }
    return false;
}

bool FindWindowAiEchoIgnoringSpaces(const std::wstring& hay, const std::wstring& needle,
    size_t& outBegin, size_t& outEnd) {
    outBegin = std::wstring::npos;
    outEnd = std::wstring::npos;
    std::wstring compactNeedle;
    for (wchar_t c : needle) {
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'　') continue;
        compactNeedle.push_back(c);
    }
    if (compactNeedle.empty() || hay.empty()) return false;
    const std::wstring lowerNeedle = ToLowerW(compactNeedle);
    std::wstring compactHay;
    std::vector<size_t> posMap;   // 紧凑串每个字符在 hay 里的原始下标
    compactHay.reserve(hay.size());
    posMap.reserve(hay.size());
    for (size_t i = 0; i < hay.size(); ++i) {
        const wchar_t c = hay[i];
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'　') continue;
        compactHay.push_back(static_cast<wchar_t>(towlower(c)));
        posMap.push_back(i);
    }
    // 取**最后一次**出现（同一个问题可能问过不止一次，只有最后一次是新回答）
    size_t from = 0;
    size_t hit = std::wstring::npos;
    for (;;) {
        const size_t at = compactHay.find(lowerNeedle, from);
        if (at == std::wstring::npos) break;
        hit = at;
        from = at + 1;
    }
    if (hit == std::wstring::npos) return false;
    outBegin = posMap[hit];
    outEnd = posMap[hit + lowerNeedle.size() - 1] + 1;
    return true;
}

int PickWindowAiCopyButton(const WindowAiReplySpec& spec,
    const std::vector<WindowAiUiNode>& nodes, const RECT& clientArea) {
    int best = -1;
    int bestBottom = -1;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        if (n.controlType != L"Button" && n.controlType != L"Hyperlink") continue;
        if (!n.enabled || n.offscreen) continue;
        if (n.Width() < 8 || n.Height() < 8) continue;
        bool hit = false;
        for (const auto& nm : spec.copyButtonNames) {
            if (!nm.empty() && ContainsNoCase(n.name, nm)) { hit = true; break; }
        }
        if (!hit) {
            for (const auto& id : spec.copyButtonIds) {
                if (!id.empty() && ContainsNoCase(n.automationId, id)) { hit = true; break; }
            }
        }
        if (!hit) continue;
        // ★取**最靠下**的那个：用户自己那条消息下面也有复制按钮，
        //   点错就会把我们的提问复制回来（表现为"回答 = 提问"，而回执看着一切正常）。
        if (n.rect.bottom > bestBottom) {
            bestBottom = n.rect.bottom;
            best = static_cast<int>(i);
        }
    }
    (void)clientArea;
    return best;
}

bool PickWindowAiCopyAnchor(const WindowAiReplySpec& spec, const std::vector<WindowAiUiNode>& nodes,
    const RECT& clientArea, const std::wstring& echoHint, int bottomLimitY, RECT& outAnchor) {
    const int cw = clientArea.right - clientArea.left;
    const int ch = clientArea.bottom - clientArea.top;
    if (cw <= 0 || ch <= 0) return false;
    const int limit = (bottomLimitY > clientArea.top) ? bottomLimitY : clientArea.bottom;
    // 回显（我们刚发的那句提问）在不在树里 —— 在的话，回答必须在它**下面**
    int echoBottom = -1;
    if (!echoHint.empty()) {
        auto compactOf = [](const std::wstring& s) {
            std::wstring o;
            for (wchar_t c : s) {
                if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'　') continue;
                o.push_back(static_cast<wchar_t>(towlower(c)));
            }
            return o;
        };
        const std::wstring compactEcho = compactOf(echoHint);
        for (const auto& n : nodes) {
            if (n.name.empty()) continue;
            // ⚠⚠ 客户端会把用户消息**拆成多个文本节点**（实测豆包：「请只回答一个数字：」
            //   与「5+5 等于几？」是两个节点）⇒ 只按整句比对，拆开的那半**逃过排除**、
            //   被当成"回答"当锚点，于是操作栏落点全错（实测踩到，白扫 40 次）。
            //   ⇒ 判据改成：**这段节点文本基本被提问覆盖**（子串 或 相似度 ≥60%）就算它。
            const std::wstring cn = compactOf(n.name);
            bool isEcho = false;
            if (!cn.empty() && !compactEcho.empty()) {
                if (compactEcho.find(cn) != std::wstring::npos) {
                    isEcho = true;
                } else {
                    const size_t lcs = LongestCommonSubstringLen(cn, compactEcho);
                    if (cn.size() >= 4 && lcs * 10 >= cn.size() * 6) isEcho = true;
                }
            }
            if (!isEcho) continue;
            echoBottom = (std::max)(echoBottom, static_cast<int>(n.rect.bottom));
        }
    }
    bool found = false;
    RECT anchor{};
    // ★★ 首选：**不是我们刚发出去的那句提问**的最靠下文本 = 助手那条回答。
    //   ⚠⚠ 为什么必须这样（实测踩到）：会话里最靠下的文本**往往就是用户自己刚发的那条**
    //   （回答还没出现、或回答在上面），拿它当锚点 ⇒ 点下去复制到的是**提问**，
    //   甚至（图标偏移差一点时）什么也复制不到。回答才是我们要的。
    for (int pass = 0; pass < 2 && !found; ++pass) {
        for (const auto& n : nodes) {
            if (n.controlType != L"Text" && n.controlType != L"Document"
                && n.controlType != L"Hyperlink" && n.controlType != L"Group") {
                continue;
            }
            if (n.name.empty()) continue;
            if (!n.enabled || n.offscreen) continue;
            if (n.Width() < 12 || n.Height() < 8) continue;
            // ② 必须在客户区内**可见**（会话很长时 DOM 未渲染部分 y 会超出窗口）
            if (n.rect.top < clientArea.top || n.rect.bottom > clientArea.bottom) continue;
            if (n.rect.left < clientArea.left || n.rect.right > clientArea.right) continue;
            // ④ 必须在输入框**上方**（否则会锚到输入区自己的控件/附件菜单）
            if (n.rect.bottom > limit) continue;
            // ③ 横向落在会话列内（排除左侧会话列表与右侧摘要栏）
            const double xRel = static_cast<double>(n.rect.left - clientArea.left) / cw;
            if (xRel < spec.copyColumnLeftRel || xRel > spec.copyColumnRightRel) continue;
            // ⑥ 必须在提问回显下面（回答在提问下方）；找不到回显就不加这条限制
            if (echoBottom > 0 && n.rect.top <= echoBottom) continue;
            // ★ 第一遍：排除"就是我们那句提问"的节点（**含被拆开的片段**）；第二遍不再排除
            if (pass == 0 && !echoHint.empty()) {
                auto compactOf = [](const std::wstring& s) {
                    std::wstring o;
                    for (wchar_t c : s) {
                        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'　') continue;
                        o.push_back(static_cast<wchar_t>(towlower(c)));
                    }
                    return o;
                };
                const std::wstring cn = compactOf(n.name);
                const std::wstring ce = compactOf(echoHint);
                if (!cn.empty() && !ce.empty()) {
                    if (ce.find(cn) != std::wstring::npos) continue;      // 片段 ⊂ 提问
                    const size_t lcs = LongestCommonSubstringLen(cn, ce);
                    if (cn.size() >= 4 && lcs * 10 >= cn.size() * 6) continue;   // 与提问高度相似
                }
            }
            if (!found || n.rect.bottom > anchor.bottom) {
                anchor = n.rect;
                found = true;
            }
        }
    }
    if (!found) return false;
    outAnchor = anchor;
    return true;
}

size_t LongestCommonSubstringLen(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return 0;
    // 滚动数组 DP（O(n*m) 时间、O(m) 空间）；这两段文本都很短（≤ 数百字），够用
    const std::wstring& row = (a.size() <= b.size()) ? a : b;
    const std::wstring& col = (a.size() <= b.size()) ? b : a;
    std::vector<size_t> prev(col.size() + 1, 0);
    std::vector<size_t> cur(col.size() + 1, 0);
    size_t best = 0;
    for (size_t i = 1; i <= row.size(); ++i) {
        for (size_t j = 1; j <= col.size(); ++j) {
            if (row[i - 1] == col[j - 1]) {
                cur[j] = prev[j - 1] + 1;
                if (cur[j] > best) best = cur[j];
            } else {
                cur[j] = 0;
            }
        }
        prev.swap(cur);
        std::fill(cur.begin(), cur.end(), static_cast<size_t>(0));
    }
    return best;
}

int PickWindowAiCopyRowButton(const WindowAiReplySpec& spec,
    const std::vector<WindowAiUiNode>& nodes, const RECT& clientArea, int bottomLimitY) {
    const int cw = clientArea.right - clientArea.left;
    if (cw <= 0) return -1;
    const int limit = (bottomLimitY > clientArea.top) ? bottomLimitY : clientArea.bottom;
    const int minSize = spec.copyRowMinSize > 0 ? spec.copyRowMinSize : 20;
    const int maxSize = spec.copyRowMaxSize > 0 ? spec.copyRowMaxSize : 56;
    const int minRow = spec.copyRowMinCount > 0 ? spec.copyRowMinCount : 3;
    // 候选：会话列内、输入框上方、方形按钮
    std::vector<int> idx;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        if (n.controlType != L"Button") continue;
        if (!n.enabled || n.offscreen) continue;
        const int w = n.Width();
        const int h = n.Height();
        if (w < minSize || h < minSize || w > maxSize || h > maxSize) continue;
        if (n.rect.bottom > limit) continue;
        if (n.rect.top < clientArea.top || n.rect.left < clientArea.left
            || n.rect.right > clientArea.right) {
            continue;
        }
        const double xRel = static_cast<double>(n.rect.left - clientArea.left) / cw;
        if (xRel < spec.copyColumnLeftRel || xRel > spec.copyColumnRightRel) continue;
        idx.push_back(static_cast<int>(i));
    }
    if (idx.empty()) return -1;
    // 按 y 分组（±6px 同排）⇒ 取最靠下、且成员 ≥minRow 的那一排
    std::sort(idx.begin(), idx.end(), [&](int a, int b) {
        return nodes[static_cast<size_t>(a)].rect.top < nodes[static_cast<size_t>(b)].rect.top;
    });
    int bestRowStart = -1;
    int bestRowEnd = -1;
    int i = 0;
    while (i < static_cast<int>(idx.size())) {
        int j = i + 1;
        const int rowY = nodes[static_cast<size_t>(idx[static_cast<size_t>(i)])].rect.top;
        while (j < static_cast<int>(idx.size())
            && (std::abs)(nodes[static_cast<size_t>(idx[static_cast<size_t>(j)])].rect.top - rowY)
                <= 6) {
            ++j;
        }
        if (j - i >= minRow) {
            bestRowStart = i;
            bestRowEnd = j;   // 后面的行 y 更大 ⇒ 覆盖即可（排序保证递增）
        }
        i = j;
    }
    if (bestRowStart < 0) return -1;
    // 该排里取**最左**的一颗 = 复制
    int leftmost = idx[static_cast<size_t>(bestRowStart)];
    for (int k = bestRowStart; k < bestRowEnd; ++k) {
        const auto& a = nodes[static_cast<size_t>(idx[static_cast<size_t>(k)])];
        const auto& b = nodes[static_cast<size_t>(leftmost)];
        if (a.rect.left < b.rect.left) leftmost = idx[static_cast<size_t>(k)];
    }
    return leftmost;
}

bool WindowAiClipboardReplyUsable(const std::wstring& prev, const std::wstring& now,
    const std::wstring& prompt, int maxChars, std::string& why) {
    why.clear();
    if (now.empty()) {
        why = "剪贴板是空的（复制按钮可能没点中，或该客户端复制的是图片/富文本）";
        return false;
    }
    if (now == prev) {
        why = "剪贴板没变化（复制按钮没点中，或它复制的不是新回答）";
        return false;
    }
    // ② 不能是我们刚发出去的那句提问（点到了"用户消息"的复制按钮）
    if (!prompt.empty() && TrimW(now) == TrimW(prompt)) {
        why = "剪贴板里是**我们刚发出去的提问**（点到了用户消息的复制按钮，"
              "应按 y 最大的那个按钮取最新回答）";
        return false;
    }
    // 忽略空白后比对：整段几乎就是那句提问 ⇒ 也算点错
    if (!prompt.empty()) {
        size_t b = 0;
        size_t e = 0;
        if (FindWindowAiEchoIgnoringSpaces(now, prompt, b, e)) {
            auto compactLen = [](const std::wstring& s) {
                size_t c = 0;
                for (wchar_t ch : s) {
                    if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' || ch == L'　') continue;
                    ++c;
                }
                return c;
            };
            const size_t promptCompact = compactLen(prompt);
            const size_t nowCompact = compactLen(now);
            if (promptCompact > 0 && nowCompact <= promptCompact + 4) {
                why = "剪贴板内容与提问基本一致（点错按钮 / 该客户端复制的是提问）";
                return false;
            }
        }
    }
    // ③ 大到离谱的（> 4 倍上限且上限 > 0）拒绝：多半复制到了别的东西
    if (maxChars > 0 && static_cast<int>(now.size()) > maxChars * 4) {
        why = "剪贴板内容异常长（" + std::to_string(now.size()) + " 字，配置上限 "
            + std::to_string(maxChars) + "）：可能复制到了整个页面";
        return false;
    }
    return true;
}

/// ★★ 剔除「**我们自己的提问回声**」（OCR 兜底路径的最后一道保险）。
///
/// 为什么不能只靠精确匹配：OCR 会把提示词读花 —— 实测 `结构化请求` → `结构化讠青求`、
/// `网页 AI 桥接` → `网页AI桥接`。提示词一花，`FindWindowAiEchoIgnoringSpaces` 就落空，
/// 而兜底的"基线公共前缀剥离"只能剥到**基线为止** ⇒ 整段提示词被当成回答返回
/// （用户实测现象：回答里出现 `〖键鼠工坊·网页AI桥接〕…=对话=〖用户〗`）。
///
/// 判据（任一命中即视为"我们自己的行"）：
///   ① 含我们的固定标识（`键鼠工坊` / `网页 AI 桥接` / `=====` 分隔行）；
///   ② 与**提示词的某一行**足够像（去空白后 LCS ≥ 较短者的 60%）。
/// ⚠ 判松了会吞用户内容 —— 所以阈值取 60%，且只在**短行**上生效（长行不猜）。
bool WindowAiLooksLikeOurOwnLine(const std::wstring& line, const std::wstring& prompt) {
    std::wstring compact;
    compact.reserve(line.size());
    for (wchar_t c : line) {
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'　') continue;
        compact.push_back(static_cast<wchar_t>(towlower(c)));
    }
    if (compact.empty()) return true;   // 空行当噪声丢掉
    if (compact.find(L"键鼠工坊") != std::wstring::npos) return true;
    if (compact.find(L"网页ai桥接") != std::wstring::npos) return true;
    if (compact.find(L"网页ai橋接") != std::wstring::npos) return true;
    // 说话人/分隔行：`〖用户〗` `【用户】` `用户：` `=对话=` `=====` ……（OCR 读得五花八门）
    {
        std::wstring bare;
        for (wchar_t c : compact) {
            if (c == L'=' || c == L'〖' || c == L'〗' || c == L'【' || c == L'】'
                || c == L'[' || c == L']' || c == L'（' || c == L'）'
                || c == L'(' || c == L')' || c == L'：' || c == L':' || c == L'·') {
                continue;
            }
            bare.push_back(c);
        }
        if (bare.empty()) return true;   // 纯分隔线（去掉符号后为空）
        if (bare == L"用户" || bare == L"助手" || bare == L"系统" || bare == L"对话") return true;
    }
    // 与提示词某一行相似 ⇒ 也是我们的
    size_t p = 0;
    while (p <= prompt.size()) {
        size_t e = prompt.find(L'\n', p);
        if (e == std::wstring::npos) e = prompt.size();
        std::wstring pline = prompt.substr(p, e - p);
        std::wstring pcompact;
        for (wchar_t c : pline) {
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'　') continue;
            pcompact.push_back(static_cast<wchar_t>(towlower(c)));
        }
        // ⚠ 门槛放到 4 字：提示词里的短行（`=对话=` `〖用户〗`）也要能认出来，
        //   否则 OCR 会把它们残留在"回答"里（自检 `reply_strips_our_own_prompt_echo` 钉住）。
        if (pcompact.size() >= 4 && compact.size() >= 4) {
            const size_t shorter = (std::min)(compact.size(), pcompact.size());
            if (LongestCommonSubstringLen(compact, pcompact) * 10 >= shorter * 6) return true;
        }
        if (e >= prompt.size()) break;
        p = e + 1;
    }
    return false;
}

/// 这段文本里**有没有我们自己的痕迹**（决定要不要做逐行剔除）。
///
/// ⚠⚠ 这是"只在我们确实回声了"才动手的闸：没有它，逐行剔除会把**正常回答**里的短行
///   （例如 `42`、`好的`）也当成我们的而删掉 —— 实测把 `extract_reply_after_prompt`
///   用例判红（回答整个被剔空）。判据只看**强标识**（`键鼠工坊` / `网页 AI 桥接` / 分隔行）。
bool WindowAiTextLooksLikeOurEcho(const std::wstring& text) {
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find(L'\n', p);
        if (e == std::wstring::npos) e = text.size();
        std::wstring compact;
        for (size_t i = p; i < e; ++i) {
            const wchar_t c = text[i];
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'　') continue;
            compact.push_back(static_cast<wchar_t>(towlower(c)));
        }
        if (compact.find(L"键鼠工坊") != std::wstring::npos) return true;
        if (compact.find(L"网页ai桥接") != std::wstring::npos) return true;
        if (compact.find(L"网页ai橋接") != std::wstring::npos) return true;
        if (compact.find(L"结构化") != std::wstring::npos) return true;
        if (compact.find(L"不要复述本说明") != std::wstring::npos) return true;
        if (e >= text.size()) break;
        p = e + 1;
    }
    return false;
}

/// 把一段文本里"我们自己的行"剔掉；剩下的才是回答（全是我们的话 ⇒ 返回空）。
std::wstring StripWindowAiPromptEcho(const std::wstring& text, const std::wstring& prompt) {
    // ⚠ 只在这段文本**确实带我们的痕迹**时才逐行剔除（否则会误删正常回答里的短行）
    if (!WindowAiTextLooksLikeOurEcho(text)) return text;
    std::wstring out;
    size_t p = 0;
    int kept = 0;
    while (p <= text.size()) {
        size_t e = text.find(L'\n', p);
        if (e == std::wstring::npos) e = text.size();
        const std::wstring line = text.substr(p, e - p);
        if (!WindowAiLooksLikeOurOwnLine(line, prompt)) {
            if (!out.empty()) out += L"\n";
            out += line;
            ++kept;
        }
        if (e >= text.size()) break;
        p = e + 1;
    }
    if (kept == 0) return std::wstring();
    return out;
}

std::wstring ExtractWindowAiReply(const std::wstring& baseline, const std::wstring& after,
    const std::wstring& prompt, int maxChars) {
    auto cap = [&](std::wstring s) {
        if (maxChars > 0 && static_cast<int>(s.size()) > maxChars) {
            s.resize(static_cast<size_t>(maxChars));
        }
        return s;
    };

    // ① 我们发出去的那句话在会话里的**最后一次**出现 ⇒ 它之后就是回答。
    //    ⚠ 匹配**忽略空白**：OCR 会在数字/字母间插空格（`11+12` → `1 1 + 1 2`），
    //      精确 rfind 在真机上必然落空（实测两次），退回前缀剥离就会把整段会话当回答。
    if (!prompt.empty()) {
        size_t at = std::wstring::npos;
        size_t end = std::wstring::npos;
        if (FindWindowAiEchoIgnoringSpaces(after, prompt, at, end)) {
            if (end > at && end <= after.size()) {
                std::wstring tail = TrimW(after.substr(end));
                // ⚠ 尾部为空 = 回答还没出现（客户端刚回显了提问）⇒ 返回空串让调用方继续等，
                //    **绝不**退回去取"提问之前的内容"（那会把上一轮回答当新回答）。
                if (!tail.empty()) {
                    // ★ 再剔一遍"我们自己的行"（OCR 会把提示词读花 ⇒ 精确回声匹配可能落空）
                    std::wstring cleaned = StripWindowAiPromptEcho(tail, prompt);
                    if (!cleaned.empty()) return cap(cleaned);
                    return std::wstring();
                }
            }
        }
    }

    // ② 与基线做公共前缀剥离（会话是追加式的）
    size_t n = 0;
    const size_t maxN = (std::min)(baseline.size(), after.size());
    while (n < maxN && baseline[n] == after[n]) ++n;
    if (n < after.size()) {
        std::wstring tail = after.substr(n);
        // ③ 尾部若正好挂着"还没发出去的输入框内容"（等于我们刚写的文本），剥掉
        if (!prompt.empty()) {
            const size_t q = tail.rfind(prompt);
            if (q != std::wstring::npos && q + prompt.size() == tail.size()) {
                tail = tail.substr(0, q);
            }
        }
        tail = TrimW(tail);
        if (!tail.empty()) {
            std::wstring cleaned = StripWindowAiPromptEcho(tail, prompt);
            if (!cleaned.empty()) return cap(cleaned);
        }
    }
    return std::wstring();
}

bool FeedWindowAiStability(WindowAiStability& st, const std::wstring& current,
    int requiredSame) {
    if (requiredSame < 2) requiredSame = 2;
    if (current == st.last && !current.empty()) {
        ++st.sameCount;
    } else {
        st.last = current;
        st.sameCount = 1;
    }
    return st.sameCount >= requiredSame;
}

std::string FormatWindowAiProfile(const WindowAiClientProfile& p) {
    // ⚠ `std::string(w.begin(), w.end())` 会把中文压成单字节（既丢数据又乱码），
    //   而这份 JSON 是**探针报告**给人看的（label/别名/进程名都可能含中文）
    //   ⇒ 必须真转 UTF-8。这是上面 `Utf8ToWide` 的镜像坑，同样踩过。
    auto u8 = [](const std::wstring& w) {
        if (w.empty()) return std::string();
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
            nullptr, 0, nullptr, nullptr);
        if (n <= 0) {
            // 兜底：不可转时用 '?' 顶替。
            // ⚠ **不要**写 `std::string(w.begin(), w.end())`：那既是丢数据又触发 C4244
            //   （wchar_t → char 窄化）。本仓 /W4 要求干净。
            std::string s;
            s.reserve(w.size());
            for (wchar_t c : w) s.push_back(c < 128 ? static_cast<char>(c) : '?');
            return s;
        }
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n,
            nullptr, nullptr);
        return s;
    };
    json j;
    j["id"] = p.id;
    j["label"] = p.label;
    j["processNames"] = json::array();
    for (const auto& s : p.processNames) {
        j["processNames"].push_back(u8(s));
    }
    j["submitMethod"] = u8(p.submitMethod);
    j["idleTimeoutMs"] = p.idleTimeoutMs;
    j["maxTotalMs"] = p.maxTotalMs;
    j["inputTypes"] = json::array();
    for (const auto& s : p.input.controlTypes) {
        j["inputTypes"].push_back(u8(s));
    }
    j["pick"] = u8(p.input.pick);
    return j.dump();
}

}  // namespace quickscript::webai
