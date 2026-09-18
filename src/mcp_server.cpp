#include "mcp_server.h"

#include "action_utils.h"
#include "agent_attachment.h"
#include "input/mouse_input_backend.h"
#include "macro_execute_tools.h"
#include "ai_action_service.h"
#include "image_match.h"
#include "office_doc.h"
#include "process_utils.h"
#include "utils.h"
#include "window_mode/ui_element_probe.h"
#include "window_mode/window_list.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

// MCP 协议版本：客户端会带自己的版本，我们回一个自己支持的（客户端按需降级/协商）
constexpr const char* kMcpProtocolVersion = "2025-06-18";

/// 工具描述（暴露给外部 agent 的能力清单）。
/// 刻意保持**少而准**：每个工具都对应我们已经在产品里验证过的原生链路。
json BuildToolCatalog() {
    auto tool = [](const char* name, const char* desc, const char* schema) {
        json t;
        t["name"] = name;
        t["description"] = desc;
        t["inputSchema"] = json::parse(schema);
        return t;
    };
    json tools = json::array();
    tools.push_back(tool("screenshot",
        "截取当前屏幕（整屏或指定区域），返回 PNG 图片与像素尺寸。"
        "后续 click/move 的坐标就是这张图的像素坐标。",
        R"({"type":"object","properties":{
            "x1":{"type":"integer"},"y1":{"type":"integer"},
            "x2":{"type":"integer"},"y2":{"type":"integer"},
            "max_width":{"type":"integer","description":"可选：等比缩到该宽度内（默认不缩）"}
        }})"));
    tools.push_back(tool("click",
        "在屏幕像素坐标点击（左/右/中键，可双击）。坐标按最近一次 screenshot 的像素解释。",
        R"({"type":"object","properties":{
            "x":{"type":"integer"},"y":{"type":"integer"},
            "button":{"type":"string","enum":["left","right","middle"],"description":"默认 left"},
            "double":{"type":"boolean","description":"true=双击"}
        },"required":["x","y"]})"));
    tools.push_back(tool("move",
        "把鼠标移动到屏幕像素坐标。",
        R"({"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"}},
            "required":["x","y"]})"));
    tools.push_back(tool("type_text",
        "向当前焦点输入文本（宿主会先切英文输入法，避免中文组字污染）。",
        R"({"type":"object","properties":{
            "text":{"type":"string"},
            "clear_first":{"type":"boolean","description":"true=先全选清空再输入（替换语义）"}
        },"required":["text"]})"));
    tools.push_back(tool("key",
        "按键或组合键，如 \"Enter\" / \"Escape\" / \"ctrl+s\" / \"alt+tab\"。",
        R"({"type":"object","properties":{"key":{"type":"string"}},"required":["key"]})"));
    tools.push_back(tool("scroll",
        "滚轮。direction=up/down/left/right，amount=步数（默认 3）。",
        R"({"type":"object","properties":{
            "direction":{"type":"string","enum":["up","down","left","right"]},
            "amount":{"type":"integer","minimum":1,"maximum":50},
            "x":{"type":"integer","description":"可选：滚动前把光标移到这里"},
            "y":{"type":"integer"}
        }})"));
    tools.push_back(tool("cursor_position", "读取当前鼠标屏幕坐标。",
        R"({"type":"object","properties":{}})"));
    tools.push_back(tool("list_windows",
        "列出可切换的窗口（标题/进程/是否前台，按 Z 序）。纯文本、不烧图。",
        R"({"type":"object","properties":{"match":{"type":"string","description":"可选标题/进程关键词"}}})"));
    tools.push_back(tool("activate_window",
        "按标题/进程关键词把窗口切到前台。",
        R"({"type":"object","properties":{"match":{"type":"string"}},"required":["match"]})"));
    tools.push_back(tool("list_ui_controls",
        "用 UI Automation 枚举前台窗口（含弹窗/菜单）里的可交互控件：按钮/菜单/输入框及其准确名称与编号。"
        "点控件**优先用这个**，而不是猜坐标。",
        R"({"type":"object","properties":{"max_count":{"type":"integer","minimum":1,"maximum":200},
            "match":{"type":"string","description":"可选：只看名字含该关键词的控件"}}})"));
    tools.push_back(tool("invoke_ui_control",
        "按控件名称触发 UIA 控件（优先 InvokePattern，其次点击控件中心）。名称是稳定键，编号会随界面变化。",
        R"({"type":"object","properties":{
            "name":{"type":"string"},"id":{"type":"integer","description":"可选：交叉校验用"}
        },"required":["name"]})"));
    tools.push_back(tool("read_document",
        "读取本地办公文档正文：xlsx/xlsm/xls/csv/docx/doc/pptx/ppt/pdf/txt 等，"
        "表格返回制表符分隔的一行一记录；PDF 无文本层时会渲染成图片。",
        R"({"type":"object","properties":{
            "path":{"type":"string"},
            "max_chars":{"type":"integer","minimum":200,"maximum":60000},
            "pages":{"type":"integer","minimum":1,"maximum":20}
        },"required":["path"]})"));
    return tools;
}

json TextContent(const std::wstring& text) {
    json c;
    c["type"] = "text";
    c["text"] = ToUtf8(text);
    return c;
}

json ImageContent(const std::string& mimeType, const std::string& base64) {
    json c;
    c["type"] = "image";
    c["data"] = base64;
    c["mimeType"] = mimeType;
    return c;
}

json OkResult(json content) {
    json r;
    r["content"] = std::move(content);
    r["isError"] = false;
    return r;
}

json ErrResult(const std::wstring& text) {
    json r;
    r["content"] = json::array({ TextContent(text) });
    r["isError"] = true;
    return r;
}

std::wstring JsonStr(const json& j, const char* key, const std::wstring& fallback = L"") {
    if (!j.is_object() || !j.contains(key) || !j[key].is_string()) return fallback;
    return FromUtf8(j[key].get<std::string>());
}

int JsonInt(const json& j, const char* key, int fallback) {
    if (!j.is_object() || !j.contains(key)) return fallback;
    if (j[key].is_number_integer()) return j[key].get<int>();
    if (j[key].is_number()) return static_cast<int>(j[key].get<double>());
    return fallback;
}

bool JsonBool(const json& j, const char* key, bool fallback) {
    if (!j.is_object() || !j.contains(key)) return fallback;
    if (j[key].is_boolean()) return j[key].get<bool>();
    if (j[key].is_number_integer()) return j[key].get<int>() != 0;
    return fallback;
}

/// 解析 "ctrl+shift+t" / "Enter" 这类按键描述 → VK + 修饰键
bool ParseKeySpec(const std::wstring& spec, UINT* outVk, bool* ctrl, bool* alt, bool* shift,
    bool* win, std::wstring* mainKeyOut) {
    *ctrl = *alt = *shift = *win = false;
    std::vector<std::wstring> parts;
    size_t pos = 0;
    while (pos <= spec.size()) {
        const size_t plus = spec.find(L'+', pos);
        parts.push_back(Trim(spec.substr(pos,
            plus == std::wstring::npos ? std::wstring::npos : plus - pos)));
        if (plus == std::wstring::npos) break;
        pos = plus + 1;
    }
    if (parts.empty()) return false;
    std::wstring mainKey = parts.back();
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        std::wstring m = parts[i];
        for (auto& c : m)
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        if (m == L"ctrl" || m == L"control") *ctrl = true;
        else if (m == L"alt") *alt = true;
        else if (m == L"shift") *shift = true;
        else if (m == L"win" || m == L"meta" || m == L"cmd") *win = true;
    }
    if (mainKey.empty()) return false;
    if (mainKeyOut) *mainKeyOut = mainKey;
    // 单字符 → 字母/数字 VK；其余走 VkName 反查常见键名
    if (mainKey.size() == 1) {
        const wchar_t c = mainKey[0];
        if (c >= L'a' && c <= L'z') { *outVk = static_cast<UINT>(c - L'a' + L'A'); return true; }
        if (c >= L'A' && c <= L'Z') { *outVk = static_cast<UINT>(c); return true; }
        if (c >= L'0' && c <= L'9') { *outVk = static_cast<UINT>(c); return true; }
    }
    std::wstring up = mainKey;
    for (auto& c : up)
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    static const struct { const wchar_t* name; UINT vk; } kNames[] = {
        { L"ENTER", VK_RETURN }, { L"RETURN", VK_RETURN }, { L"ESC", VK_ESCAPE },
        { L"ESCAPE", VK_ESCAPE }, { L"TAB", VK_TAB }, { L"SPACE", VK_SPACE },
        { L"BACKSPACE", VK_BACK }, { L"DELETE", VK_DELETE }, { L"DEL", VK_DELETE },
        { L"HOME", VK_HOME }, { L"END", VK_END }, { L"PAGEUP", VK_PRIOR },
        { L"PAGEDOWN", VK_NEXT }, { L"UP", VK_UP }, { L"DOWN", VK_DOWN },
        { L"LEFT", VK_LEFT }, { L"RIGHT", VK_RIGHT }, { L"F1", VK_F1 }, { L"F2", VK_F2 },
        { L"F3", VK_F3 }, { L"F4", VK_F4 }, { L"F5", VK_F5 }, { L"F6", VK_F6 },
        { L"F7", VK_F7 }, { L"F8", VK_F8 }, { L"F9", VK_F9 }, { L"F10", VK_F10 },
        { L"F11", VK_F11 }, { L"F12", VK_F12 },
    };
    for (const auto& item : kNames) {
        if (up == item.name) { *outVk = item.vk; return true; }
    }
    return false;
}

void PressKey(UINT vk, bool ctrl, bool alt, bool shift, bool win) {
    if (ctrl) SendKeyboardKey(VK_LCONTROL, true);
    if (alt) SendKeyboardKey(VK_LMENU, true);
    if (shift) SendKeyboardKey(VK_LSHIFT, true);
    if (win) SendKeyboardKey(VK_LWIN, true);
    Sleep(20);
    SendKeyboardKey(vk, true);
    SendKeyboardKey(vk, false);
    Sleep(20);
    if (win) SendKeyboardKey(VK_LWIN, false);
    if (shift) SendKeyboardKey(VK_LSHIFT, false);
    if (alt) SendKeyboardKey(VK_LMENU, false);
    if (ctrl) SendKeyboardKey(VK_LCONTROL, false);
}

/// 工具调用实现。返回 MCP CallToolResult。
json CallTool(const std::wstring& name, const json& args) {
    if (name == L"screenshot") {
        int vx = 0, vy = 0, vw = 0, vh = 0;
        GetVirtualScreenRect(vx, vy, vw, vh);
        int x1 = JsonInt(args, "x1", vx);
        int y1 = JsonInt(args, "y1", vy);
        int x2 = JsonInt(args, "x2", vx + vw);
        int y2 = JsonInt(args, "y2", vy + vh);
        if (x2 <= x1 || y2 <= y1) return ErrResult(L"[错误] 区域无效。");
        HBITMAP bmp = CaptureScreenRegion(x1, y1, x2, y2);
        if (!bmp) return ErrResult(L"[错误] 截屏失败。");
        const std::string b64 = BitmapToBase64Jpeg(bmp, 85);
        BITMAP bm{};
        GetObjectW(bmp, sizeof(bm), &bm);
        const int bw = bm.bmWidth;
        const int bh = bm.bmHeight;
        DeleteBitmapHandle(bmp);
        if (b64.empty()) return ErrResult(L"[错误] 图片编码失败。");
        return OkResult(json::array({
            ImageContent("image/jpeg", b64),
            TextContent(L"截图区域 (" + std::to_wstring(x1) + L"," + std::to_wstring(y1) + L")-("
                + std::to_wstring(x2) + L"," + std::to_wstring(y2) + L")，"
                + std::to_wstring(bw) + L"×" + std::to_wstring(bh)
                + L" 像素；click/move 请用**屏幕绝对像素**坐标（与上面的 x1,y1 同坐标系）。"),
        }));
    }
    if (name == L"click" || name == L"move") {
        const int x = JsonInt(args, "x", INT_MIN);
        const int y = JsonInt(args, "y", INT_MIN);
        if (x == INT_MIN || y == INT_MIN) return ErrResult(L"[错误] 需要 x/y。");
        if (name == L"move") {
            SendMouseMoveAbsoluteScreen(x, y);
            return OkResult(json::array({
                TextContent(L"已移动鼠标到 (" + std::to_wstring(x) + L"," + std::to_wstring(y) + L")")
            }));
        }
        std::wstring button = JsonStr(args, "button", L"left");
        MouseButtonType type = MouseButtonType::Left;
        if (button == L"right") type = MouseButtonType::Right;
        else if (button == L"middle") type = MouseButtonType::Middle;
        const bool dbl = JsonBool(args, "double", false);
        SendMouseClickAtScreen(x, y, type);
        if (dbl) {
            Sleep(60);
            SendMouseClickAtScreen(x, y, type);
        }
        return OkResult(json::array({
            TextContent(L"已" + std::wstring(dbl ? L"双击" : L"点击") + L" (" + std::to_wstring(x)
                + L"," + std::to_wstring(y) + L") 按钮=" + button)
        }));
    }
    if (name == L"type_text") {
        const std::wstring text = JsonStr(args, "text");
        if (text.empty()) return ErrResult(L"[错误] 需要 text。");
        const bool clearFirst = JsonBool(args, "clear_first", false);
        PrepareImeForTextInput(text);
        if (clearFirst) {
            PressKey('A', true, false, false, false);
            PressKey(VK_DELETE, false, false, false, false);
            Sleep(60);
        }
        SendQuickInputText(text, 0.008, nullptr);
        return OkResult(json::array({
            TextContent(L"已输入 " + std::to_wstring(text.size()) + L" 个字符"
                + (clearFirst ? L"（已先清空）" : L""))
        }));
    }
    if (name == L"key") {
        const std::wstring spec = JsonStr(args, "key");
        if (spec.empty()) return ErrResult(L"[错误] 需要 key。");
        UINT vk = 0;
        bool ctrl = false, alt = false, shift = false, win = false;
        std::wstring mainKey;
        if (!ParseKeySpec(spec, &vk, &ctrl, &alt, &shift, &win, &mainKey)) {
            return ErrResult(L"[错误] 无法识别的按键：" + spec
                + L"（示例：Enter / Escape / ctrl+s / alt+tab / F5）");
        }
        PressKey(vk, ctrl, alt, shift, win);
        return OkResult(json::array({ TextContent(L"已按键 " + spec) }));
    }
    if (name == L"scroll") {
        std::wstring dir = JsonStr(args, "direction", L"down");
        const int amount = std::clamp(JsonInt(args, "amount", 3), 1, 50);
        const int x = JsonInt(args, "x", INT_MIN);
        const int y = JsonInt(args, "y", INT_MIN);
        if (x != INT_MIN && y != INT_MIN) {
            SendMouseMoveAbsoluteScreen(x, y);
            Sleep(30);
        }
        const bool vertical = (dir == L"up" || dir == L"down");
        const int delta = ((dir == L"up" || dir == L"left") ? 1 : -1) * WHEEL_DELTA;
        for (int i = 0; i < amount; ++i) {
            MouseInputRouter::Instance().Wheel(delta, !vertical);
            Sleep(15);
        }
        return OkResult(json::array({
            TextContent(L"已滚动 " + dir + L" " + std::to_wstring(amount) + L" 步")
        }));
    }
    if (name == L"cursor_position") {
        POINT pt{};
        if (!GetCursorPos(&pt)) return ErrResult(L"[错误] 取光标位置失败。");
        return OkResult(json::array({
            TextContent(L"光标屏幕坐标 (" + std::to_wstring(pt.x) + L"," + std::to_wstring(pt.y)
                + L")")
        }));
    }
    if (name == L"list_windows") {
        const std::wstring match = JsonStr(args, "match");
        const DWORD selfPid = GetCurrentProcessId();
        auto list = windowmode::ListSwitchableWindows(selfPid);
        if (!match.empty()) list = windowmode::MatchWindows(list, match);
        if (list.empty()) return OkResult(json::array({ TextContent(L"没有匹配的窗口。") }));
        return OkResult(json::array({ TextContent(windowmode::FormatWindowList(list)) }));
    }
    if (name == L"activate_window") {
        const std::wstring match = JsonStr(args, "match");
        if (match.empty()) return ErrResult(L"[错误] 需要 match。");
        const DWORD selfPid = GetCurrentProcessId();
        auto list = windowmode::MatchWindows(windowmode::ListSwitchableWindows(selfPid), match);
        if (list.empty()) return ErrResult(L"[错误] 找不到标题/进程含「" + match + L"」的窗口。");
        std::wstring err;
        if (!windowmode::ActivateWindow(list.front().hwnd, err)) {
            return ErrResult(L"[错误] 切窗失败：" + err);
        }
        return OkResult(json::array({ TextContent(L"已切到：" + list.front().title) }));
    }
    if (name == L"list_ui_controls") {
        const int maxCount = std::clamp(JsonInt(args, "max_count", 60), 1, 200);
        std::vector<windowmode::UiControlInfo> items =
            windowmode::ListInteractiveUiControls(nullptr, maxCount);
        const std::wstring match = JsonStr(args, "match");
        if (!match.empty()) {
            std::wstring needle = match;
            for (auto& c : needle)
                if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
            std::vector<windowmode::UiControlInfo> filtered;
            for (const auto& it : items) {
                std::wstring n = it.name;
                for (auto& c : n)
                    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
                if (n.find(needle) != std::wstring::npos) filtered.push_back(it);
            }
            items = std::move(filtered);
        }
        if (items.empty()) return OkResult(json::array({ TextContent(L"没有枚举到可交互控件。") }));
        return OkResult(json::array({
            TextContent(windowmode::FormatUiControlListForAgent(items, 4000))
        }));
    }
    if (name == L"invoke_ui_control") {
        const std::wstring ctlName = JsonStr(args, "name");
        if (ctlName.empty()) return ErrResult(L"[错误] 需要 name。");
        const int wantId = JsonInt(args, "id", -1);
        std::wstring actualName;
        std::wstring warn;
        int actualId = -1;
        RECT rect{};
        bool invoked = false;
        if (!windowmode::InvokeUiControlByName(ctlName, wantId,
                actualName, actualId, rect, invoked, warn)) {
            return ErrResult(L"[错误] 未能触发控件「" + ctlName + L"」。"
                L"可先用 list_ui_controls 看准确名称（名称才是稳定键）。"
                + (warn.empty() ? std::wstring() : (L" 提示：" + warn)));
        }
        std::wstring how = invoked ? L"InvokePattern（未打像素）" : L"点击控件中心";
        if (rect.right > rect.left && rect.bottom > rect.top) {
            const int cx = (rect.left + rect.right) / 2;
            const int cy = (rect.top + rect.bottom) / 2;
            if (!invoked) {
                SendMouseClickAtScreen(cx, cy, MouseButtonType::Left);
                how += L" (" + std::to_wstring(cx) + L"," + std::to_wstring(cy) + L")";
            }
        }
        return OkResult(json::array({
            TextContent(L"已触发「" + (actualName.empty() ? ctlName : actualName) + L"」：" + how
                + (warn.empty() ? std::wstring() : (L"（" + warn + L"）")))
        }));
    }
    if (name == L"read_document") {
        const std::wstring path = JsonStr(args, "path");
        if (path.empty()) return ErrResult(L"[错误] 需要 path。");
        const int maxChars = std::clamp(JsonInt(args, "max_chars", 6000), 200, 60000);
        const int pages = std::clamp(JsonInt(args, "pages", 3), 1, 20);
        const OfficeDocResult r = ReadOfficeDocument(path, maxChars, pages);
        if (!r.ok) return ErrResult(L"[错误] " + r.error);
        std::wstring text = L"引擎 " + r.engine;
        if (r.truncated) text += L"（已截断至 " + std::to_wstring(maxChars) + L" 字）";
        if (!r.note.empty()) text += L"\n" + r.note;
        text += L"\n---- 正文 ----\n" + r.text;
        json content = json::array({ TextContent(text) });
        for (const auto& img : r.images) {
            const std::string b64 = AgentBase64EncodeFile(img);
            if (!b64.empty()) content.push_back(ImageContent("image/png", b64));
        }
        return OkResult(std::move(content));
    }
    return ErrResult(L"[错误] 未知工具：" + name);
}

json MakeError(const json& id, int code, const std::string& message) {
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id.is_null() ? json(nullptr) : id;
    resp["error"] = json::object({ { "code", code }, { "message", message } });
    return resp;
}

}  // namespace

int McpToolCount() {
    return static_cast<int>(BuildToolCatalog().size());
}

std::string McpHandleRequestLine(const std::string& line) {
    const std::string trimmed = [&] {
        size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string();
        size_t e = line.find_last_not_of(" \t\r\n");
        return line.substr(b, e - b + 1);
    }();
    if (trimmed.empty()) return std::string();

    json req;
    try {
        req = json::parse(trimmed);
    } catch (const std::exception& e) {
        return MakeError(nullptr, -32700, std::string("Parse error: ") + e.what()).dump();
    }
    if (!req.is_object()) return MakeError(nullptr, -32600, "Invalid Request").dump();

    const json id = req.contains("id") ? req["id"] : json(nullptr);
    const std::string method = req.contains("method") && req["method"].is_string()
        ? req["method"].get<std::string>() : std::string();
    const json params = req.contains("params") && req["params"].is_object()
        ? req["params"] : json::object();
    const bool isNotification = !req.contains("id");

    auto result = [&](json r) {
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        resp["result"] = std::move(r);
        return resp.dump();
    };

    if (method == "initialize") {
        // 回显客户端请求的协议版本（有就跟随，避免版本协商失败），否则用我们的默认值
        std::string version = kMcpProtocolVersion;
        if (params.contains("protocolVersion") && params["protocolVersion"].is_string())
            version = params["protocolVersion"].get<std::string>();
        json r;
        r["protocolVersion"] = version;
        r["capabilities"] = json::object({ { "tools", json::object() } });
        r["serverInfo"] = json::object({
            { "name", "quickscripttool" },
            { "version", "1.0" },
            { "title", "QuickScriptTool 原生桌面控制（WGC 截屏 / SendInput / UIA / 文档读取）" },
        });
        return result(std::move(r));
    }
    if (method == "notifications/initialized" || method == "notifications/cancelled") {
        return std::string();   // 通知：不响应
    }
    if (method == "ping") return result(json::object());
    if (method == "tools/list") {
        json r;
        r["tools"] = BuildToolCatalog();
        return result(std::move(r));
    }
    if (method == "tools/call") {
        const std::string toolName = params.contains("name") && params["name"].is_string()
            ? params["name"].get<std::string>() : std::string();
        const json args = params.contains("arguments") && params["arguments"].is_object()
            ? params["arguments"] : json::object();
        if (toolName.empty()) return MakeError(id, -32602, "tools/call 需要 name").dump();
        json callResult;
        try {
            callResult = CallTool(FromUtf8(toolName), args);
        } catch (const std::exception& e) {
            return result(ErrResult(L"[错误] 工具执行异常：" + FromUtf8(std::string(e.what()))));
        }
        return result(std::move(callResult));
    }
    if (isNotification) return std::string();
    return MakeError(id, -32601, "Method not found: " + method).dump();
}

int RunMcpStdioServer() {
    // stdout 只走协议（一行一个 JSON）；日志一律走 stderr，否则会污染 MCP 流
    SetConsoleOutputCP(CP_UTF8);
    std::string line;
    int c = 0;
    bool eof = false;
    while (!eof) {
        line.clear();
        for (;;) {
            c = std::getchar();
            if (c == EOF) { eof = true; break; }
            if (c == '\n') break;
            if (c != '\r') line.push_back(static_cast<char>(c));
        }
        if (line.empty()) continue;
        const std::string response = McpHandleRequestLine(line);
        if (response.empty()) continue;
        std::fwrite(response.data(), 1, response.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
    return 0;
}
