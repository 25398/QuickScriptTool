// =============================================================================
// OcrSelfTest — OCR JSON 解析 / 找字匹配（不启动 Python）
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:OcrSelfTest
//   build\Release\OcrSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "ocr_engine.h"

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
    {L"concat_lines_newline", L"default",
        L"ConcatOcrLines joins with newline"},
    {L"make_ocr_vars", L"default",
        L"Text var stores string; search var stores found+box"},
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

void CaseConcatLines() {
    OcrEngineOutput out;
    out.lines.push_back(Line(L"a", 0, 0, 1, 1));
    out.lines.push_back(Line(L"b", 0, 2, 1, 3));
    Emit(L"concat_lines_newline", ConcatOcrLines(out) == L"a\nb", L"");
}

void CaseMakeVars() {
    const OcrVarResult text = MakeOcrTextVarResult(L"hello");
    OcrTextLine line = Line(L"x", 3, 4, 13, 24);
    const OcrVarResult search = MakeOcrSearchVarResult(line, true);
    const bool ok = text.mode == OcrVarMode::Text && text.text == L"hello"
        && search.mode == OcrVarMode::Search && search.found == 1
        && search.topLeftX == 3 && search.bottomRightY == 24;
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
    CaseConcatLines();
    CaseMakeVars();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
