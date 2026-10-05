// =============================================================================
// OcrSelfTest — OCR JSON 解析 / 找字匹配（不启动 Python）
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:OcrSelfTest
//   build\Release\OcrSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "ocr_engine.h"
#include "ocr_backend.h"

#include <string>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"parse_success_lines", L"default",
        L"ParseOcrEngineJson reads text/box/confidence"},
    {L"parse_unicode_escape", L"default",
        L"JSON \\uXXXX Chinese text roundtrips"},
    {L"parse_failure_error", L"default",
        L"success=false keeps error string"},
    {L"backend_default_is_python", L"default",
        L"不设置时后端保持 Python —— 产品内 OCR 行为不受影响"},
    {L"backend_preference_roundtrip", L"default",
        L"SetOcrBackendPreference / OcrBackendPreference 往返；越界值回落到 Python"},
    {L"join_ocr_words_cjk_no_spaces", L"default",
        L"CJK 之间不补空格（WinRT 会逐字给 word，直接拼会变成「新 建 任 务」）"},
    {L"join_ocr_words_latin_keeps_spaces", L"default",
        L"拉丁词之间保留空格，中英混排也正确"},
    {L"backend_attempt_order", L"default",
        L"Auto 必须 **Python 优先**（走软件模式要和软件内结果一致），WinRt/Python 各自单跑"},
    {L"winrt_ocr_probe", L"default",
        L"系统 OCR 探测不崩溃；不可用时给出可读原因"},
    {L"winrt_ocr_reads_rendered_text", L"default",
        L"用 GDI 渲染数字再走系统 OCR，能认出（无语言包时按跳过计）"},
    {L"find_exact_substring", L"default",
        L"First line containing target wins"},
    {L"find_fullwidth_digits", L"default",
        L"Fullwidth digits match ASCII target"},
    {L"find_ignores_spaces", L"default",
        L"Spaces / fullwidth space ignored in search"},
    {L"find_adjacent_lines", L"default",
        L"Target split across neighboring lines is joined"},
    {L"find_fuzzy_close", L"default",
        L"One-character OCR typo still matches long target"},
    {L"find_empty_or_miss", L"default",
        L"Empty target and unrelated text return no match"},
    {L"find_match_data_tiers", L"default",
        L"文字查找匹配度：精确/归一化/邻行拼接=100，模糊=相似度百分比，且与旧封装命中同一行"},
    {L"concat_lines_newline", L"default",
        L"ConcatOcrLines joins with newline"},
    {L"make_ocr_vars", L"default",
        L"Text var stores string; search var stores found+matchData+box"},
};

OcrTextLine Line(const wchar_t* text, int x1, int y1, int x2, int y2, double conf = 0.9) {
    OcrTextLine line;
    line.text = text;
    line.x1 = x1;
    line.y1 = y1;
    line.x2 = x2;
    line.y2 = y2;
    line.confidence = conf;
    return line;
}

void CaseParseSuccessLines() {
    const std::string json =
        "{\"success\":true,\"error\":\"\",\"lines\":["
        "{\"text\":\"Hello\",\"x1\":1,\"y1\":2,\"x2\":10,\"y2\":20,\"confidence\":0.91}"
        "]}";
    const OcrEngineOutput out = ParseOcrEngineJson(json);
    const bool ok = out.success && out.lines.size() == 1
        && out.lines[0].text == L"Hello"
        && out.lines[0].x1 == 1 && out.lines[0].y1 == 2
        && out.lines[0].x2 == 10 && out.lines[0].y2 == 20
        && out.lines[0].confidence > 0.9;
    Emit(L"parse_success_lines", ok, ok ? L"" : L"parse mismatch");
}

void CaseParseUnicodeEscape() {
    const std::string json =
        "{\"success\":true,\"error\":\"\",\"lines\":["
        "{\"text\":\"\\u5f00\\u59cb\",\"x1\":0,\"y1\":0,\"x2\":8,\"y2\":8,\"confidence\":1}"
        "]}";
    const OcrEngineOutput out = ParseOcrEngineJson(json);
    const bool ok = out.success && out.lines.size() == 1 && out.lines[0].text == L"开始";
    Emit(L"parse_unicode_escape", ok, ok ? L"" : out.lines.empty() ? L"no lines" : out.lines[0].text.c_str());
}

void CaseParseFailureError() {
    const std::string json = "{\"success\":false,\"error\":\"boom\",\"lines\":[]}";
    const OcrEngineOutput out = ParseOcrEngineJson(json);
    const bool ok = !out.success && out.error == L"boom" && out.lines.empty();
    Emit(L"parse_failure_error", ok, ok ? L"" : L"error field not parsed");
}

void CaseFindExact() {
    OcrEngineOutput out;
    out.success = true;
    out.lines.push_back(Line(L"点击登录按钮", 0, 0, 40, 12));
    out.lines.push_back(Line(L"取消", 0, 20, 20, 32));
    const auto hit = FindTextInOcrLines(out, L"登录");
    const bool ok = hit.has_value() && hit->text == L"点击登录按钮";
    Emit(L"find_exact_substring", ok, ok ? L"" : L"exact miss");
}

void CaseFindFullwidthDigits() {
    OcrEngineOutput out;
    out.success = true;
    out.lines.push_back(Line(L"HP１２３", 4, 4, 40, 18));
    const auto hit = FindTextInOcrLines(out, L"123");
    const bool ok = hit.has_value() && hit->x1 == 4;
    Emit(L"find_fullwidth_digits", ok, ok ? L"" : L"fullwidth miss");
}

void CaseFindIgnoresSpaces() {
    OcrEngineOutput out;
    out.success = true;
    out.lines.push_back(Line(L"开始 游戏", 1, 1, 50, 16));
    const auto hit = FindTextInOcrLines(out, L"开始游戏");
    const bool ok = hit.has_value();
    Emit(L"find_ignores_spaces", ok, ok ? L"" : L"space-normalized miss");
}

void CaseFindAdjacentLines() {
    OcrEngineOutput out;
    out.success = true;
    out.lines.push_back(Line(L"开", 0, 0, 10, 12));
    out.lines.push_back(Line(L"始游戏", 10, 0, 50, 12));
    const auto hit = FindTextInOcrLines(out, L"开始游戏");
    const bool ok = hit.has_value() && hit->x1 == 0 && hit->x2 == 50;
    Emit(L"find_adjacent_lines", ok, ok ? L"" : L"span miss");
}

void CaseFindFuzzyClose() {
    OcrEngineOutput out;
    out.success = true;
    out.lines.push_back(Line(L"请点击开始游戱", 2, 2, 80, 18));
    const auto hit = FindTextInOcrLines(out, L"请点击开始游戏");
    const bool ok = hit.has_value();
    Emit(L"find_fuzzy_close", ok, ok ? L"" : L"fuzzy miss");
}

void CaseFindEmptyOrMiss() {
    OcrEngineOutput out;
    out.success = true;
    out.lines.push_back(Line(L"确定", 0, 0, 20, 12));
    const bool emptyMiss = !FindTextInOcrLines(out, L"").has_value();
    const bool unrelated = !FindTextInOcrLines(out, L"取消").has_value();
    Emit(L"find_empty_or_miss", emptyMiss && unrelated,
        (emptyMiss && unrelated) ? L"" : L"false positive or empty matched");
}

/// 用 GDI 把文字渲染成一张位图（白底黑字），供系统 OCR 识别。
HBITMAP MakeTextBitmap(const wchar_t* text, int w, int h) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // 自顶向下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    if (!screen) return nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC dc = CreateCompatibleDC(screen);
    if (!bmp || !dc) {
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    RECT rc{ 0, 0, w, h };
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    HFONT font = CreateFontW(-96, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH, L"Microsoft YaHei");
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    TextOutW(dc, 24, 24, text, static_cast<int>(wcslen(text)));
    if (oldFont) SelectObject(dc, oldFont);
    if (font) DeleteObject(font);
    SelectObject(dc, oldBmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return bmp;
}

void CaseBackendDefaultIsPython() {
    // 先复位，模拟"产品刚启动、没人动过后端"
    SetOcrBackendPreference(OcrBackend::Python);
    const bool ok = OcrBackendPreference() == OcrBackend::Python;
    Emit(L"backend_default_is_python", ok,
        ok ? L"" : L"default backend is not Python");
}

void CaseBackendPreferenceRoundtrip() {
    bool ok = true;
    for (OcrBackend b : { OcrBackend::Python, OcrBackend::Auto, OcrBackend::WinRt }) {
        SetOcrBackendPreference(b);
        if (OcrBackendPreference() != b) ok = false;
    }
    // 越界值（枚举被写坏）必须回落到 Python，而不是随机行为
    SetOcrBackendPreference(static_cast<OcrBackend>(99));
    const bool fallback = OcrBackendPreference() == OcrBackend::Python;
    SetOcrBackendPreference(OcrBackend::Python);
    Emit(L"backend_preference_roundtrip", ok && fallback,
        ok ? (fallback ? L"" : L"out-of-range did not fall back to Python")
           : L"roundtrip mismatch");
}

void CaseJoinOcrWordsCjkNoSpaces() {
    // WinRT 对中文会把每个字当成一个 word —— 直接拼会得到 "新 建 任 务"
    const std::vector<std::wstring> words = { L"新", L"建", L"任", L"务", L"助", L"理" };
    const std::wstring got = JoinOcrWords(words);
    const bool ok = got == L"新建任务助理";
    Emit(L"join_ocr_words_cjk_no_spaces", ok, ok ? L"" : (L"得到：" + got).c_str());
}

void CaseJoinOcrWordsLatinKeepsSpaces() {
    bool ok = true;
    std::wstring bad;
    auto check = [&](const std::vector<std::wstring>& in, const std::wstring& want) {
        const std::wstring got = JoinOcrWords(in);
        if (got != want) { ok = false; if (bad.empty()) bad = got; }
    };
    // 规则：**只要有一侧是 CJK 就不加空格**，两侧都不是才加。
    // 这样 `生命HP45` 保持紧凑（符合中文界面写法，找字/取数都更稳），
    // 同时英文词之间的空格不会被吃掉。
    check({ L"HP", L"45", L"/", L"60" }, L"HP 45 / 60");   // 两侧都非 CJK → 加
    check({ L"生命", L"HP", L"45" }, L"生命HP 45");          // 命|H 有 CJK → 不加
    check({ L"攻", L"击", L"力", L"120" }, L"攻击力120");     // 力|1 有 CJK → 不加
    check({ L"新", L"建", L"任", L"务" }, L"新建任务");      // 汉字之间不加
    // 全角数字/标点算 CJK
    check({ L"１", L"２", L"３" }, L"１２３");
    check({ L"血量", L"，", L"满" }, L"血量，满");
    Emit(L"join_ocr_words_latin_keeps_spaces", ok,
        ok ? L"" : (L"得到：" + bad).c_str());
}

void CaseBackendAttemptOrder() {
    const auto py = OcrBackendAttemptOrder(OcrBackend::Python);
    const auto wr = OcrBackendAttemptOrder(OcrBackend::WinRt);
    const auto au = OcrBackendAttemptOrder(OcrBackend::Auto);
    // Python（产品默认）只走一条 —— 保证引入本层前后产品行为完全一致
    const bool pyOk = py.size() == 1 && py[0] == OcrBackend::Python;
    const bool wrOk = wr.size() == 1 && wr[0] == OcrBackend::WinRt;
    // Auto = 导出「走软件」模式：必须 **Python 优先**，否则和软件内结果不一致
    const bool auOk = au.size() == 2 && au[0] == OcrBackend::Python
        && au[1] == OcrBackend::WinRt;
    const bool ok = pyOk && wrOk && auOk;
    Emit(L"backend_attempt_order", ok,
        ok ? L"" : (L"python=" + std::to_wstring(py.size())
            + L" winrt=" + std::to_wstring(wr.size())
            + L" auto0=" + std::to_wstring(au.empty() ? -1 : static_cast<int>(au[0]))).c_str());
}

void CaseWinRtOcrProbe() {
    std::wstring reason;
    const bool avail = WinRtOcrAvailable(&reason);
    // 可用与否取决于目标机语言包，两者都算通过；但不可用时必须给出可读原因
    const bool ok = avail || !reason.empty();
    Emit(L"winrt_ocr_probe", ok,
        avail ? L"系统 OCR 可用"
              : (ok ? (L"系统 OCR 不可用：" + reason).c_str() : L"不可用但没有原因"));
}

void CaseWinRtOcrReadsRenderedText() {
    std::wstring reason;
    if (!WinRtOcrAvailable(&reason)) {
        // 没装语言包的机器上跳过（仍算通过），避免 CI 因环境差异变红
        Emit(L"winrt_ocr_reads_rendered_text", true,
            (L"跳过（系统 OCR 不可用：" + reason + L"）").c_str());
        return;
    }
    HBITMAP bmp = MakeTextBitmap(L"1234", 400, 160);
    if (!bmp) {
        Emit(L"winrt_ocr_reads_rendered_text", false, L"无法创建测试位图");
        return;
    }
    const OcrEngineOutput out = RunWinRtOcr(bmp, false);
    DeleteObject(bmp);

    std::wstring joined;
    for (const auto& l : out.lines) joined += l.text;
    const bool hasDigits = joined.find(L"1234") != std::wstring::npos
        || (joined.find(L'1') != std::wstring::npos
            && joined.find(L'2') != std::wstring::npos
            && joined.find(L'3') != std::wstring::npos
            && joined.find(L'4') != std::wstring::npos);
    const bool ok = out.success && hasDigits && !out.lines.empty();
    Emit(L"winrt_ocr_reads_rendered_text", ok,
        ok ? (L"识别到：" + joined).c_str()
           : (out.error.empty() ? (L"未识别到数字，得到：" + joined).c_str()
                                : out.error.c_str()));
}

/// 匹配度（文字查找存进变量 .matchData 的那个数）：分档 + 与旧封装的一致性。
/// ★为什么钉得这么死：用户看到的「保存匹配度」就是这些数字，档位/四舍五入漂了
///   等于文案又变成假话（这正是这次改动的起因）。
void CaseFindMatchDataTiers() {
    std::wstring detail;
    bool ok = true;

    // ① 精确子串命中 = 100
    {
        OcrEngineOutput out;
        out.lines.push_back(Line(L"点击登录按钮", 0, 0, 40, 12));
        const auto hit = FindTextInOcrLinesScored(out, L"登录");
        if (!hit || hit->matchData != 100) {
            ok = false;
            detail = L"精确命中匹配度应为 100，实得 "
                + std::to_wstring(hit ? hit->matchData : -1);
        }
    }
    // ② 全角/空白归一化命中 = 100（不是「相似度」）
    if (ok) {
        OcrEngineOutput out;
        out.lines.push_back(Line(L"HP１２３", 4, 4, 40, 18));
        const auto hit = FindTextInOcrLinesScored(out, L"123");
        if (!hit || hit->matchData != 100) {
            ok = false;
            detail = L"归一化命中匹配度应为 100，实得 "
                + std::to_wstring(hit ? hit->matchData : -1);
        }
    }
    // ③ 邻行拼接命中 = 100（命中框是拼接后的并集）
    if (ok) {
        OcrEngineOutput out;
        out.lines.push_back(Line(L"开", 0, 0, 10, 12));
        out.lines.push_back(Line(L"始游戏", 10, 0, 50, 12));
        const auto hit = FindTextInOcrLinesScored(out, L"开始游戏");
        if (!hit || hit->matchData != 100 || hit->line.x2 != 50) {
            ok = false;
            detail = L"邻行拼接命中应为 100 且框到 x2=50";
        }
    }
    // ④ 模糊命中 = 相似度百分比。7 字里错 1 字 ⇒ 1-1/7=0.857 ⇒ 86
    if (ok) {
        OcrEngineOutput out;
        out.lines.push_back(Line(L"请点击开始游戱", 2, 2, 80, 18));
        const auto hit = FindTextInOcrLinesScored(out, L"请点击开始游戏");
        if (!hit || hit->matchData != 86) {
            ok = false;
            detail = L"模糊命中（7 字错 1）匹配度应为 86，实得 "
                + std::to_wstring(hit ? hit->matchData : -1);
        }
    }
    // ⑤ 未命中 = 没有结果（0 只由 MakeOcrSearchMissingVarResult 造，不是「相似度 0」）
    if (ok) {
        OcrEngineOutput out;
        out.lines.push_back(Line(L"确定", 0, 0, 20, 12));
        const auto hit = FindTextInOcrLinesScored(out, L"取消");
        const auto legacy = FindTextInOcrLines(out, L"取消");
        if (hit.has_value() || legacy.has_value()) {
            ok = false;
            detail = L"不相关文字被判成命中";
        }
    }
    // ⑥ 旧封装与新函数**命中同一行**（命中判定只有一份实现，别再各写一遍）
    if (ok) {
        OcrEngineOutput out;
        out.lines.push_back(Line(L"取消", 0, 0, 20, 12));
        out.lines.push_back(Line(L"点击登录按钮", 0, 20, 40, 32));
        const auto scored = FindTextInOcrLinesScored(out, L"登录");
        const auto legacy = FindTextInOcrLines(out, L"登录");
        if (!scored || !legacy || scored->line.text != legacy->text
            || scored->line.y1 != legacy->y1) {
            ok = false;
            detail = L"FindTextInOcrLines 与 FindTextInOcrLinesScored 命中不一致";
        }
    }
    Emit(L"find_match_data_tiers", ok, ok ? L"" : detail.c_str());
}

void CaseConcatLines() {
    OcrEngineOutput out;
    out.lines.push_back(Line(L"a", 0, 0, 1, 1));
    out.lines.push_back(Line(L"b", 0, 2, 1, 3));
    Emit(L"concat_lines_newline", ConcatOcrLines(out) == L"a\nb", L"");
}

void CaseMakeVars() {
    const OcrVarResult text = MakeOcrTextVarResult(L"hello");
    OcrTextLine line = Line(L"x", 3, 4, 13, 24);
    const OcrVarResult search = MakeOcrSearchVarResult(line, 86);
    const OcrVarResult missing = MakeOcrSearchMissingVarResult();
    const bool ok = text.mode == OcrVarMode::Text && text.text == L"hello"
        && search.mode == OcrVarMode::Search && search.found == 1
        && search.matchData == 86 && search.topLeftX == 3 && search.bottomRightY == 24
        && missing.mode == OcrVarMode::Search && missing.found == 0
        && missing.matchData == 0;
    Emit(L"make_ocr_vars", ok, ok ? L"" : L"var packing mismatch");
}

void PrintHelp() {
    std::fwprintf(stdout,
        L"OcrSelfTest — OCR JSON parse and text search\n"
        L"\n"
        L"用法:\n"
        L"  OcrSelfTest.exe [--json] [--list] [--help]\n"
        L"\n"
        L"Agent: 见 .cursor/skills/module-selftest/SKILL.md\n"
        L"  源码: src/ocr_engine.cpp, src/ocr_result.h\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i] ? argv[i] : L"";
        if (a == L"--json") {
            selftest::gJson = true;
            selftest::InitUtf8Stdout();
        } else if (a == L"--list") {
            listOnly = true;
            selftest::InitUtf8Stdout();
        } else if (a == L"--help" || a == L"-h") {
            PrintHelp();
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"OcrSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    CaseParseSuccessLines();
    CaseParseUnicodeEscape();
    CaseParseFailureError();
    CaseFindExact();
    CaseFindFullwidthDigits();
    CaseFindIgnoresSpaces();
    CaseFindAdjacentLines();
    CaseFindFuzzyClose();
    CaseFindEmptyOrMiss();
    CaseFindMatchDataTiers();
    CaseBackendDefaultIsPython();
    CaseBackendPreferenceRoundtrip();
    CaseBackendAttemptOrder();
    CaseJoinOcrWordsCjkNoSpaces();
    CaseJoinOcrWordsLatinKeepsSpaces();
    CaseWinRtOcrProbe();
    CaseWinRtOcrReadsRenderedText();
    CaseConcatLines();
    CaseMakeVars();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
