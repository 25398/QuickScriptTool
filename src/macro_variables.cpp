#include "macro_variables.h"
#include "var_compute.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <cwctype>
#include <random>
#include <unordered_set>

#include <shellapi.h>
#include <windows.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

bool TryParseDouble(const std::wstring& text, double& out);

namespace {

std::wstring TrimToken(const std::wstring& text) {
    size_t start = 0;
    while (start < text.size() && iswspace(text[start])) ++start;
    size_t end = text.size();
    while (end > start && iswspace(text[end - 1])) --end;
    return text.substr(start, end - start);
}

std::wstring ResolveRandomToken(const std::wstring& spec);

// 时间魔法变量格式化：格式用 yyyy/yy/MM/dd/HH/mm/ss，其余字符原样保留。
// 例：{time:yyyy-MM-dd HH:mm:ss} → 2026-08-06 15:40:00；{Now} → 同默认格式。
std::wstring FormatTimeToken(const std::wstring& fmt) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    std::wstring out;
    out.reserve(fmt.size() + 8);
    wchar_t buf[32]{};
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == L'y' && i + 3 < fmt.size() && fmt.substr(i, 4) == L"yyyy") {
            swprintf_s(buf, L"%04d", tm.tm_year + 1900); out += buf; i += 3;
        } else if (fmt[i] == L'y' && i + 1 < fmt.size() && fmt[i + 1] == L'y') {
            swprintf_s(buf, L"%02d", (tm.tm_year + 1900) % 100); out += buf; ++i;
        } else if (fmt[i] == L'M' && i + 1 < fmt.size() && fmt[i + 1] == L'M') {
            swprintf_s(buf, L"%02d", tm.tm_mon + 1); out += buf; ++i;
        } else if (fmt[i] == L'd' && i + 1 < fmt.size() && fmt[i + 1] == L'd') {
            swprintf_s(buf, L"%02d", tm.tm_mday); out += buf; ++i;
        } else if (fmt[i] == L'H' && i + 1 < fmt.size() && fmt[i + 1] == L'H') {
            swprintf_s(buf, L"%02d", tm.tm_hour); out += buf; ++i;
        } else if (fmt[i] == L'm' && i + 1 < fmt.size() && fmt[i + 1] == L'm') {
            swprintf_s(buf, L"%02d", tm.tm_min); out += buf; ++i;
        } else if (fmt[i] == L's' && i + 1 < fmt.size() && fmt[i + 1] == L's') {
            swprintf_s(buf, L"%02d", tm.tm_sec); out += buf; ++i;
        } else {
            out.push_back(fmt[i]);
        }
    }
    return out;
}

std::tm LocalNowTm() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    return tm;
}

std::vector<std::wstring> PathsFromHDrop(HDROP drop) {
    std::vector<std::wstring> paths;
    if (!drop) return paths;
    const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        if (len == 0) continue;
        std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
        const UINT copied = DragQueryFileW(drop, i, buf.data(), len + 1);
        if (copied > 0) {
            buf.resize(copied);
            paths.push_back(std::move(buf));
        }
    }
    return paths;
}

HBITMAP BitmapFromDibHandle(HANDLE hMem) {
    if (!hMem) return nullptr;
    const auto* info = static_cast<const BITMAPINFO*>(GlobalLock(hMem));
    if (!info) return nullptr;
    const BITMAPINFOHEADER& hdr = info->bmiHeader;
    if (hdr.biSize < sizeof(BITMAPINFOHEADER) || hdr.biWidth == 0 || hdr.biHeight == 0) {
        GlobalUnlock(hMem);
        return nullptr;
    }
    DWORD headerSize = hdr.biSize;
    if (hdr.biBitCount <= 8) {
        const DWORD colors = hdr.biClrUsed ? hdr.biClrUsed : (1u << hdr.biBitCount);
        headerSize += colors * sizeof(RGBQUAD);
    } else if (hdr.biCompression == BI_BITFIELDS) {
        headerSize += 3 * sizeof(DWORD);
    }
    const auto* bits = reinterpret_cast<const BYTE*>(info) + headerSize;
    HDC hdc = GetDC(nullptr);
    HBITMAP bmp = nullptr;
    if (hdc) {
        bmp = CreateDIBitmap(hdc, &hdr, CBM_INIT, bits, info, DIB_RGB_COLORS);
        ReleaseDC(nullptr, hdc);
    }
    GlobalUnlock(hMem);
    return bmp;
}

bool OpenClipboardRetry() {
    for (int i = 0; i < 10; ++i) {
        if (OpenClipboard(nullptr)) return true;
        Sleep(5);
    }
    return false;
}

// 调用方须已 OpenClipboard。
HBITMAP CopyClipboardBitmapWhileOpen() {
    if (IsClipboardFormatAvailable(CF_BITMAP)) {
        if (HANDLE h = GetClipboardData(CF_BITMAP)) {
            HBITMAP copy = static_cast<HBITMAP>(CopyImage(h, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
            if (copy) return copy;
        }
    }
    if (IsClipboardFormatAvailable(CF_DIB)) {
        if (HANDLE h = GetClipboardData(CF_DIB)) {
            return BitmapFromDibHandle(h);
        }
    }
    return nullptr;
}

std::wstring ClipboardAnsiToWide(HANDLE hMem) {
    if (!hMem) return L"";
    const char* p = static_cast<const char*>(GlobalLock(hMem));
    if (!p) return L"";
    const int n = MultiByteToWideChar(CP_ACP, 0, p, -1, nullptr, 0);
    std::wstring out;
    if (n > 1) {
        out.resize(static_cast<size_t>(n - 1));
        MultiByteToWideChar(CP_ACP, 0, p, -1, out.data(), n);
    }
    GlobalUnlock(hMem);
    return out;
}

std::wstring JoinPaths(const std::vector<std::wstring>& files) {
    std::wstring out;
    for (size_t i = 0; i < files.size(); ++i) {
        if (i) out += L'\n';
        out += files[i];
    }
    return out;
}

bool TextDuplicatesFiles(const std::wstring& text, const std::vector<std::wstring>& files) {
    if (text.empty() || files.empty()) return false;
    if (text == JoinPaths(files)) return true;
    std::wstring crlf;
    for (size_t i = 0; i < files.size(); ++i) {
        if (i) crlf += L"\r\n";
        crlf += files[i];
    }
    if (text == crlf) return true;
    if (files.size() == 1 && text == files[0]) return true;
    return false;
}

const MacroClipboardSnapshot& SnapshotOrLive(const MacroVariableContext& ctx,
    MacroClipboardSnapshot& liveStorage) {
    if (ctx.clipboardSnapshot) return *ctx.clipboardSnapshot;
    liveStorage = ReadMacroClipboardSnapshot();
    return liveStorage;
}

std::wstring FormatClipboardCondition(const MacroClipboardSnapshot& snap) {
    if (!snap.text.empty() || !snap.files.empty() || snap.hasBitmap) return L"1";
    return L"0";
}

std::wstring FormatClipboardTextExpansion(const MacroClipboardSnapshot& snap) {
    if (!snap.files.empty()) return JoinPaths(snap.files);
    return snap.text;
}

std::wstring FormatClipboardAiReplacement(const MacroClipboardSnapshot& snap) {
    std::wstring out;
    if (!snap.text.empty() && !TextDuplicatesFiles(snap.text, snap.files)) {
        out = snap.text;
    }
    for (const auto& file : snap.files) {
        if (LooksLikeImageFilePath(file)) continue;
        if (!out.empty()) out += L'\n';
        out += file;
    }
    bool anyImage = snap.hasBitmap;
    if (!anyImage) {
        for (const auto& file : snap.files) {
            if (LooksLikeImageFilePath(file)) { anyImage = true; break; }
        }
    }
    if (anyImage) {
        if (!out.empty()) out += L'\n';
        out += L"（见附图：剪贴板图片）";
    }
    return out;
}

std::wstring ResolveClipboardToken(const MacroVariableContext& ctx, bool conditionOperand) {
    MacroClipboardSnapshot live;
    const MacroClipboardSnapshot& snap = SnapshotOrLive(ctx, live);
    if (conditionOperand) return FormatClipboardCondition(snap);
    if (ctx.clipboardExpandMode == ClipboardExpandMode::Ai) {
        return FormatClipboardAiReplacement(snap);
    }
    return FormatClipboardTextExpansion(snap);
}

std::wstring ResolveClipboardPlainText(const MacroVariableContext& ctx) {
    MacroClipboardSnapshot live;
    const MacroClipboardSnapshot& snap = SnapshotOrLive(ctx, live);
    return snap.text;
}

bool TryResolveBuiltinExpr(const std::wstring& expr, const MacroVariableContext& ctx,
    bool conditionOperand, std::wstring& out) {
    if (expr == L"ctrl:CurLoops()") {
        out = std::to_wstring(ctx.curLoops);
        return true;
    }
    if (expr == L"ctrl:Random()") {
        out = ResolveRandomToken(L"1,100");
        return true;
    }
    if (expr == L"ctrl:Hour()") {
        out = std::to_wstring(LocalNowTm().tm_hour);
        return true;
    }
    if (expr == L"ctrl:Minute()") {
        out = std::to_wstring(LocalNowTm().tm_min);
        return true;
    }
    if (expr == L"ctrl:Clipboard()") {
        out = ResolveClipboardToken(ctx, conditionOperand);
        return true;
    }
    if (expr == L"Now") {
        out = FormatTimeToken(L"yyyy-MM-dd HH:mm:ss");
        return true;
    }
    if (expr.rfind(L"time:", 0) == 0 || expr.rfind(L"date:", 0) == 0) {
        out = FormatTimeToken(expr.substr(5));
        return true;
    }
    if (expr == L"clipboard") {
        out = ResolveClipboardPlainText(ctx);
        return true;
    }
    if (expr.rfind(L"random:", 0) == 0) {
        out = ResolveRandomToken(expr.substr(7));
        return true;
    }
    if (expr == L"cursor.x" || expr == L"cursor.y") {
        POINT pt{};
        GetCursorPos(&pt);
        out = std::to_wstring(expr == L"cursor.x" ? pt.x : pt.y);
        return true;
    }
    if (expr == L"screen.w" || expr == L"screen.h") {
        out = std::to_wstring(expr == L"screen.w"
            ? GetSystemMetrics(SM_CXSCREEN) : GetSystemMetrics(SM_CYSCREEN));
        return true;
    }
    if (expr == L"username") {
        wchar_t buf[256]{};
        DWORD n = 256;
        out.clear();
        if (GetUserNameW(buf, &n)) out = buf;
        return true;
    }
    return false;
}

void AppendFixedVarItems(std::vector<QuickInputVarItem>& items) {
    std::unordered_set<std::wstring> displays;
    for (const auto& it : items) displays.insert(it.display);
    auto add = [&](const wchar_t* code, const wchar_t* tip) {
        if (!displays.insert(code).second) return;
        items.push_back({
            code,
            std::wstring(L"{") + code + L"}",
            tip,
            code
        });
    };
    add(L"ctrl:CurLoops()", L"宏运行次数：当前宏从头执行的第几次（固定变量）");
    add(L"ctrl:Random()", L"随机变量：每次引用随机取 1~100 的整数（固定变量）");
    add(L"ctrl:Hour()", L"当前小时：本地时 0–23（固定变量）");
    add(L"ctrl:Minute()", L"当前分钟：本地时 0–59（固定变量）");
    add(L"ctrl:Clipboard()",
        L"剪贴板：条件里有内容为1否则0；输入展开文字或全部文件路径；AI图片分析/动作可附图（固定变量）");
    // {Now}/{clipboard}/{cursor.x} 等仍是引擎魔法变量，不进下拉——用户手写 {…} 才引用
}

// {random:min,max}：闭区间随机整数
std::wstring ResolveRandomToken(const std::wstring& spec) {
    const size_t comma = spec.find(L',');
    if (comma == std::wstring::npos) return L"";
    long long lo = 0, hi = 0;
    try {
        lo = std::stoll(spec.substr(0, comma));
        hi = std::stoll(spec.substr(comma + 1));
    } catch (...) {
        return L"";
    }
    if (hi < lo) std::swap(lo, hi);
    if (hi - lo > 100000000LL) hi = lo + 100000000LL;
    thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<long long> dist(lo, hi);
    return std::to_wstring(dist(rng));
}

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

std::wstring MissingMatchNumeric(const std::wstring& prop) {
    if (prop.empty() || prop == L"matchData" || prop == L"x" || prop == L"y" || prop == L"x1"
        || prop == L"y1" || prop == L"cx" || prop == L"cy" || prop == L"count" || prop == L"hit") {
        return L"0";
    }
    return L"";
}

struct IndexedVarRef {
    std::wstring name;
    bool hasIndex = false;
    int index = 0;
    bool indexIsPlaceholder = false;
    std::wstring prop;
};

bool ParseIndexedVarRef(const std::wstring& token, IndexedVarRef& out) {
    out = {};
    if (token.empty()) return false;
    const size_t lb = token.find(L'[');
    if (lb != std::wstring::npos) {
        const size_t rb = token.find(L']', lb + 1);
        if (rb == std::wstring::npos || rb < lb + 1) return false;
        out.name = token.substr(0, lb);
        if (out.name.empty()) return false;
        const std::wstring idx = token.substr(lb + 1, rb - lb - 1);
        if (idx == L"n" || idx == L"N") {
            out.indexIsPlaceholder = true;
        } else {
            if (idx.empty()) return false;
            for (wchar_t ch : idx) {
                if (!iswdigit(ch)) return false;
            }
            out.index = static_cast<int>(wcstol(idx.c_str(), nullptr, 10));
            if (out.index < 0) return false;
        }
        out.hasIndex = true;
        if (rb + 1 < token.size()) {
            if (token[rb + 1] != L'.') return false;
            out.prop = token.substr(rb + 2);
        }
        return true;
    }
    const size_t dot = token.find(L'.');
    if (dot == std::wstring::npos) {
        out.name = token;
        return !out.name.empty();
    }
    out.name = token.substr(0, dot);
    out.prop = token.substr(dot + 1);
    return !out.name.empty();
}

std::wstring LookupMatchListVar(const ImageMatchListVar& list, const IndexedVarRef& ref) {
    if (ref.indexIsPlaceholder) return L"";
    if (ref.prop == L"count") return std::to_wstring(static_cast<int>(list.hits.size()));
    const ImageMatchListHit* hit = nullptr;
    if (ref.hasIndex) {
        if (ref.index < 0 || ref.index >= static_cast<int>(list.hits.size())) {
            return MissingMatchNumeric(ref.prop);
        }
        hit = &list.hits[static_cast<size_t>(ref.index)];
    } else {
        if (ref.prop.empty()) return L"";
        if (list.hits.empty()) return MissingMatchNumeric(ref.prop);
        hit = &list.hits.front();
    }
    if (ref.prop.empty()) return hit->match.found ? L"1" : L"0";
    if (ref.prop == L"hit") return std::to_wstring(hit->templateIndex + 1);
    if (ref.prop == L"hitName") return hit->templateName;
    return LookupMatchVarProperty(hit->match, ref.prop);
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

    IndexedVarRef indexed;
    if (ParseIndexedVarRef(t, indexed) && (indexed.hasIndex || !indexed.prop.empty())) {
        if (indexed.indexIsPlaceholder) return L"";
        if (ctx.matchListVars) {
            const auto it = ctx.matchListVars->find(indexed.name);
            if (it != ctx.matchListVars->end()) {
                return LookupMatchListVar(it->second, indexed);
            }
        }
        if (ctx.matchVars) {
            const auto it = ctx.matchVars->find(indexed.name);
            if (it != ctx.matchVars->end()) {
                if (indexed.hasIndex && indexed.index != 0) {
                    return MissingMatchNumeric(indexed.prop);
                }
                if (indexed.prop == L"count") return it->second.found ? L"1" : L"0";
                if (indexed.prop.empty()) return it->second.found ? L"1" : L"0";
                return LookupMatchVarProperty(it->second, indexed.prop);
            }
        }
    }

    const size_t dot = t.find(L'.');
    if (dot != std::wstring::npos) {
        const std::wstring varName = t.substr(0, dot);
        const std::wstring prop = t.substr(dot + 1);
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

    if (ctx.userVars) {
        const auto it = ctx.userVars->find(t);
        if (it != ctx.userVars->end()) return it->second;
    }

    if (ctx.aiVars) {
        const auto it = ctx.aiVars->find(t);
        if (it != ctx.aiVars->end()) {
            double num = 0.0;
            if (TryParseDouble(it->second, num)) return std::to_wstring(static_cast<int>(num));
            return it->second;
        }
    }

    if (ctx.imageVars) {
        const auto it = ctx.imageVars->find(t);
        if (it != ctx.imageVars->end()) return it->second;
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

    std::wstring builtin;
    if (TryResolveBuiltinExpr(t, ctx, true, builtin)) return builtin;

    double unused;
    if (TryParseDouble(t, unused)) return t;

    // Unknown identifier: do not re-wrap into {t} (ResolveMacroVariables → Operand
    // would recurse forever and overflow the stack).
    return L"";
}

}  // namespace

MacroClipboardSnapshot ReadMacroClipboardSnapshot() {
    MacroClipboardSnapshot snap;
    if (!OpenClipboardRetry()) return snap;
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        if (HANDLE h = GetClipboardData(CF_HDROP)) {
            snap.files = PathsFromHDrop(static_cast<HDROP>(h));
        }
    }
    snap.hasBitmap = IsClipboardFormatAvailable(CF_BITMAP)
        || IsClipboardFormatAvailable(CF_DIB);
    if (snap.hasBitmap) {
        snap.bitmap = CopyClipboardBitmapWhileOpen();
    }
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                snap.text = p;
                GlobalUnlock(h);
            }
        }
    } else if (IsClipboardFormatAvailable(CF_TEXT)) {
        if (HANDLE h = GetClipboardData(CF_TEXT)) {
            snap.text = ClipboardAnsiToWide(h);
        }
    }
    CloseClipboard();
    snap.text = TrimToken(snap.text);
    return snap;
}

bool LooksLikeImageFilePath(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) {
        return false;
    }
    std::wstring ext = path.substr(dot);
    for (wchar_t& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".bmp"
        || ext == L".gif" || ext == L".webp" || ext == L".tif" || ext == L".tiff"
        || ext == L".ico";
}

bool PromptMentionsCtrlClipboard(const std::wstring& text) {
    return text.find(L"ctrl:Clipboard()") != std::wstring::npos;
}

void* DuplicateClipboardBitmapRaw() {
    if (!OpenClipboardRetry()) return nullptr;
    HBITMAP result = CopyClipboardBitmapWhileOpen();
    CloseClipboard();
    return result;
}

bool TryParseDouble(const std::wstring& text, double& out) {
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    out = wcstod(text.c_str(), &end);
    return end != text.c_str() && *end == L'\0';
}

std::wstring ResolveMacroOperand(const std::wstring& token, const MacroVariableContext& ctx) {
    return ResolveMacroOperandImpl(token, ctx, false);
}

std::wstring ResolveClipboardVarCompute(const MacroVariableContext& ctx) {
    MacroClipboardSnapshot live;
    const MacroClipboardSnapshot* snap = ctx.clipboardSnapshot;
    if (!snap) {
        live = ReadMacroClipboardSnapshot();
        snap = &live;
    }
    if (!snap->files.empty()) {
        std::wstring out;
        for (size_t i = 0; i < snap->files.size(); ++i) {
            if (i) out += L'\n';
            out += snap->files[i];
        }
        return out;
    }
    return snap->text;
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

    // 前缀 not：整子句取反（`not a == b`）。不变量：只有 `not` 后跟空白才算前缀。
    if (c.size() > 4 && (c[0] == L'n' || c[0] == L'N')) {
        const bool notPrefixed = (c[1] == L'o' || c[1] == L'O')
            && (c[2] == L't' || c[2] == L'T')
            && (c[3] == L' ' || c[3] == L'\t');
        if (notPrefixed) {
            return !EvalSingleClause(c.substr(4), ctx);
        }
    }

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
        if (a.findImageFollowUp == 3) {
            // 持久路径不当变量提示；临时变量名注册
            if (a.matchVarName.find(L'\\') != std::wstring::npos
                || a.matchVarName.find(L'/') != std::wstring::npos
                || (a.matchVarName.size() >= 2 && a.matchVarName[1] == L':')) {
                continue;
            }
            if (!seen.insert(a.matchVarName).second) continue;
            items.push_back({
                a.matchVarName,
                L"{" + a.matchVarName + L"}",
                L"图片变量:" + a.matchVarName,
                a.matchVarName
            });
            continue;
        }
        if (a.findImageFollowUp != 2) continue;
        if (!seen.insert(a.matchVarName).second) continue;
        AddFindImageVarItems(a.matchVarName, items);
    }
    for (const auto& a : actions) {
        if (a.type != ActionType::MultiMatch || a.matchVarName.empty()) continue;
        if (!seen.insert(a.matchVarName + L"#mm").second) continue;
        const std::wstring n = a.matchVarName;
        items.push_back({n + L".count", L"{" + n + L".count}", L"多图匹配命中个数", n + L".count"});
        items.push_back({n + L"[n]", L"{" + n + L"[n]}", L"多图匹配第 n 处（把 n 换成 0、1、2…）", n + L"[n]"});
        items.push_back({n + L"[0]", L"{" + n + L"[0]}", L"多图匹配第一处是否命中", n + L"[0]"});
        AddFindImageVarItems(n + L"[0]", items);
        items.push_back({n + L"[0].hit", L"{" + n + L"[0].hit}", L"命中模板序号(从1计)", n + L"[0].hit"});
        items.push_back({n + L"[0].hitName", L"{" + n + L"[0].hitName}", L"命中模板文件名", n + L"[0].hitName"});
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
    for (const auto& a : actions) {
        if (a.type != ActionType::VarCompute) continue;
        for (const auto& name : CollectVarComputeReturnNames(a.computeCode)) {
            if (name.empty() || !seen.insert(name).second) continue;
            items.push_back({
                name,
                L"{" + name + L"}",
                L"变量运算:" + name,
                name
            });
        }
    }
    AppendFixedVarItems(items);
    return items;
}

namespace {

std::wstring StripQuickInputControlChars(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t ch : text) {
        if (ch == L'\n' || ch == L'\r' || ch == L'\t') continue;
        out.push_back(ch);
    }
    return out;
}

std::wstring ResolveMacroVariablesMapped(const std::wstring& text, const MacroVariableContext& ctx,
    std::wstring (*mapReplacement)(const std::wstring&)) {
    // 防御嵌套花括号 `{{{{...}}}}`：限制展开深度，避免恶意/畸形脚本递归爆栈。
    thread_local int depth = 0;
    constexpr int kMaxDepth = 16;
    if (depth >= kMaxDepth) return L"";
    struct DepthGuard { ~DepthGuard() { --depth; } } guard;
    ++depth;
    if (text.find(L'{') == std::wstring::npos) return text;

    std::wstring result = text;
    result.reserve(text.size() * 2);  // 预分配空间，减少变量展开时的多次重新分配
    size_t pos = 0;
    while ((pos = result.find(L'{', pos)) != std::wstring::npos) {
        const size_t end = result.find(L'}', pos);
        if (end == std::wstring::npos) break;

        const std::wstring expr = result.substr(pos + 1, end - pos - 1);
        std::wstring replacement;
        if (!TryResolveBuiltinExpr(expr, ctx, false, replacement)) {
            replacement = ResolveMacroOperand(expr, ctx);
        }
        if (mapReplacement) replacement = mapReplacement(replacement);

        result.erase(pos, end - pos + 1);
        result.insert(pos, replacement);
        pos += replacement.size();
    }
    return result;
}

}  // namespace

std::wstring ResolveMacroVariables(const std::wstring& text, const MacroVariableContext& ctx) {
    return ResolveMacroVariablesMapped(text, ctx, nullptr);
}

std::wstring ResolveQuickInputText(const std::wstring& text, const MacroVariableContext& ctx, bool parseEscapes) {
    if (parseEscapes) {
        return DecodeQuickInputEscapes(ResolveMacroVariables(text, ctx));
    }
    return ResolveMacroVariablesMapped(text, ctx, StripQuickInputControlChars);
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
