// ── 通用工具函数实现 ──────────────────────────────────────────
#include "utils.h"

// ── 字符串处理 ────────────────────────────────────────────────────
std::wstring Trim(const std::wstring& value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

// ── 路径工具 ──────────────────────────────────────────────────────
std::wstring AppDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring value(path);
    const auto slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : value.substr(0, slash);
}

std::wstring ScriptsDir() { return AppDir() + L"\\scripts"; }
std::wstring RecordingsDir() { return AppDir() + L"\\recordings"; }

void EnsureScriptsDir() {
    CreateDirectoryW(ScriptsDir().c_str(), nullptr);
    CreateDirectoryW(RecordingsDir().c_str(), nullptr);
}

std::wstring NowText() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04d/%02d/%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

std::wstring TimestampName() {
    using namespace std::chrono;
    const auto ts = duration_cast<seconds>(
        system_clock::now().time_since_epoch()).count();
    return L"鼠标宏-" + std::to_wstring(ts);
}

// ── 窗口文本操作 ──────────────────────────────────────────────────
std::wstring GetText(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(len), L'\0');
    if (len > 0) GetWindowTextW(hwnd, text.data(), len + 1);
    return text;
}

void SetText(HWND hwnd, const std::wstring& text) {
    SetWindowTextW(hwnd, text.c_str());
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return "";
    const int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1,
        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1,
        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) return L"";
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
        static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
        static_cast<int>(text.size()), out.data(), len);
    return out;
}

// ── 数值转换 ──────────────────────────────────────────────────────
int ToInt(HWND edit, int fallback) {
    try {
        const auto text = Trim(GetText(edit));
        return text.empty() ? fallback : std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

double ToDouble(HWND edit, double fallback) {
    try {
        const auto text = Trim(GetText(edit));
        return text.empty() ? fallback : std::stod(text);
    } catch (...) {
        return fallback;
    }
}

std::wstring F3(double value) {
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(3) << value;
    return ss.str();
}

// ── JSON 转义 ─────────────────────────────────────────────────────
std::wstring EscapeJson(const std::wstring& value) {
    std::wstringstream out;
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': out << L"\\\\"; break;
        case L'\"': out << L"\\\""; break;
        case L'\n': out << L"\\n";  break;
        case L'\r': out << L"\\r";  break;
        case L'\t': out << L"\\t";  break;
        default:    out << ch;      break;
        }
    }
    return out.str();
}

std::wstring UnescapeJson(const std::wstring& value) {
    std::wstring out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == L'\\' && i + 1 < value.size()) {
            switch (value[i + 1]) {
            case L'n': out.push_back(L'\n'); ++i; break;
            case L'r': out.push_back(L'\r'); ++i; break;
            case L't': out.push_back(L'\t'); ++i; break;
            case L'\\': out.push_back(L'\\'); ++i; break;
            case L'\"': out.push_back(L'\"'); ++i; break;
            default: out.push_back(value[i]); break;
            }
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

namespace {

size_t FindJsonStringEnd(const std::wstring& src, size_t quotePos) {
    for (size_t i = quotePos + 1; i < src.size(); ++i) {
        if (src[i] == L'\\' && i + 1 < src.size()) {
            ++i;
            continue;
        }
        if (src[i] == L'"') return i;
    }
    return std::wstring::npos;
}

size_t FindMatchingJsonBrace(const std::wstring& src, size_t openPos) {
    int depth = 0;
    for (size_t i = openPos; i < src.size(); ++i) {
        if (src[i] == L'"') {
            const size_t end = FindJsonStringEnd(src, i);
            if (end == std::wstring::npos) return std::wstring::npos;
            i = end;
            continue;
        }
        if (src[i] == L'{') ++depth;
        else if (src[i] == L'}') {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::wstring::npos;
}

}  // namespace

std::vector<std::wstring> ExtractJsonActionBlocks(const std::wstring& content) {
    std::vector<std::wstring> blocks;
    const auto actionsKey = content.find(L"\"actions\"");
    if (actionsKey == std::wstring::npos) return blocks;
    const auto arrayStart = content.find(L'[', actionsKey);
    if (arrayStart == std::wstring::npos) return blocks;

    size_t pos = arrayStart + 1;
    while (pos < content.size()) {
        const auto objStart = content.find(L'{', pos);
        if (objStart == std::wstring::npos) break;
        const auto objEnd = FindMatchingJsonBrace(content, objStart);
        if (objEnd == std::wstring::npos) break;
        blocks.push_back(content.substr(objStart, objEnd - objStart + 1));
        pos = objEnd + 1;
    }
    return blocks;
}

// ── 文件操作 ──────────────────────────────────────────────────────
std::wstring ReadAll(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return L"";
    std::stringstream ss;
    ss << file.rdbuf();
    std::string bytes = ss.str();
    if (bytes.size() >= 3
        && static_cast<unsigned char>(bytes[0]) == 0xEF
        && static_cast<unsigned char>(bytes[1]) == 0xBB
        && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    return FromUtf8(bytes);
}

std::wstring ExtractString(const std::wstring& src,
                            const std::wstring& key) {
    const auto pos = src.find(L"\"" + key + L"\"");
    if (pos == std::wstring::npos) return L"";
    const auto colon = src.find(L':', pos);
    const auto first = src.find(L'\"', colon + 1);
    if (first == std::wstring::npos) return L"";
    const size_t second = FindJsonStringEnd(src, first);
    if (second == std::wstring::npos) return L"";
    return UnescapeJson(src.substr(first + 1, second - first - 1));
}

double ExtractNumber(const std::wstring& src,
                      const std::wstring& key, double fallback) {
    const auto pos = src.find(L"\"" + key + L"\"");
    if (pos == std::wstring::npos) return fallback;
    const auto colon = src.find(L':', pos);
    const auto end = src.find_first_of(L",}\n", colon + 1);
    try {
        return std::stod(Trim(src.substr(colon + 1, end - colon - 1)));
    } catch (...) {
        return fallback;
    }
}

int CountActionsInJson(const std::wstring& content) {
    int count = 0;
    size_t pos = 0;
    while ((pos = content.find(L"\"type\"", pos)) != std::wstring::npos) {
        ++count;
        pos += 6;
    }
    return count;
}

// ── 热键与按键名称 ────────────────────────────────────────────────
std::wstring VkName(UINT vk) {
    if (vk >= 'A' && vk <= 'Z')
        return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= '0' && vk <= '9')
        return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24)
        return L"F" + std::to_wstring(vk - VK_F1 + 1);
    switch (vk) {
    case VK_SPACE:    return L"空格键";
    case VK_RETURN:   return L"Enter";
    case VK_ESCAPE:   return L"Esc";
    case VK_TAB:      return L"Tab";
    case VK_BACK:     return L"Backspace";
    case VK_CAPITAL:  return L"CapsLock";
    case VK_NUMLOCK:  return L"NumLock";
    case VK_SCROLL:   return L"ScrollLock";
    case VK_PAUSE:    return L"Pause";
    case VK_SNAPSHOT: return L"截屏键";
    case VK_INSERT:   return L"Ins";
    case VK_DELETE:   return L"Del";
    case VK_HOME:     return L"Home";
    case VK_END:      return L"End";
    case VK_PRIOR:    return L"PgUp";
    case VK_NEXT:     return L"PgDn";
    case VK_LEFT:     return L"←";
    case VK_UP:       return L"↑";
    case VK_RIGHT:    return L"→";
    case VK_DOWN:     return L"↓";
    case VK_LWIN:     return L"LWin";
    case VK_RWIN:     return L"RWin";
    case VK_LCONTROL: return L"LCtrl";
    case VK_RCONTROL: return L"RCtrl";
    case VK_LMENU:    return L"LAlt";
    case VK_RMENU:    return L"RAlt";
    case VK_LSHIFT:   return L"LShift";
    case VK_RSHIFT:   return L"RShift";
    case VK_APPS:     return L"Apps";
    case VK_MULTIPLY: return L"Num*";
    case VK_ADD:      return L"Num+";
    case VK_SUBTRACT: return L"Num-";
    case VK_DECIMAL:  return L"Num.";
    case VK_DIVIDE:   return L"Num/";
    case VK_NUMPAD0:  return L"Num0";
    case VK_NUMPAD1:  return L"Num1";
    case VK_NUMPAD2:  return L"Num2";
    case VK_NUMPAD3:  return L"Num3";
    case VK_NUMPAD4:  return L"Num4";
    case VK_NUMPAD5:  return L"Num5";
    case VK_NUMPAD6:  return L"Num6";
    case VK_NUMPAD7:  return L"Num7";
    case VK_NUMPAD8:  return L"Num8";
    case VK_NUMPAD9:  return L"Num9";
    case VK_LBUTTON:  return L"鼠标左键";
    case VK_MBUTTON:  return L"鼠标中键";
    case VK_RBUTTON:  return L"鼠标右键";
    case VK_XBUTTON1: return L"鼠标侧键1";
    case VK_XBUTTON2: return L"鼠标侧键2";
    case VK_OEM_COMMA:  return L",";
    case VK_OEM_PERIOD: return L".";
    case VK_OEM_MINUS:  return L"-";
    case VK_OEM_PLUS:   return L"=";
    case VK_OEM_1:      return L";";
    case VK_OEM_2:      return L"/";
    case VK_OEM_3:      return L"`";
    case VK_OEM_4:      return L"[";
    case VK_OEM_5:      return L"\\";
    case VK_OEM_6:      return L"]";
    case VK_OEM_7:      return L"'";
    case VK_CLEAR:      return L"Clear";
    }
    return L"按键" + std::to_wstring(vk);
}

std::wstring HotkeyText(UINT modifiers, UINT vk) {
    std::wstring text;
    if (modifiers & MOD_CONTROL) text += L"Ctrl + ";
    if (modifiers & MOD_ALT)     text += L"Alt + ";
    if (modifiers & MOD_SHIFT)   text += L"Shift + ";
    if (modifiers & MOD_WIN)     text += L"Win + ";
    if (vk == 0) {
        if (text.size() >= 3) text.erase(text.size() - 3);
        return text.empty() ? L"无" : text;
    }
    text += VkName(vk);
    return text;
}

std::wstring FormatDuration(double sec) {
    const int total = std::max(0, static_cast<int>(sec + 0.5));
    const int m = total / 60;
    const int s = total % 60;
    return std::to_wstring(m) + L"' " + std::to_wstring(s) + L"\"";
}
