// =============================================================================
// ScriptSerializationSelfTest — 脚本序列化往返（D 段：#4 统一序列化的安全网）
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:ScriptSerializationSelfTest
//   build\Release\ScriptSerializationSelfTest.exe --json
//   build\Release\ScriptSerializationSelfTest.exe --json --corpus <脚本目录>
//
// 为什么必须有它（第 1/2/3 轮验收 §7.2 D 反复强调）：
//   改动序列化是**高风险**的——脚本格式一旦读不回来就是用户数据损坏。
//   而三轮里已经两次证明「用例全绿 ≠ 没坏」（saveSettings P0 与 MCP 冒烟）。
//   所以 D 段的硬要求是：**必须用真实历史脚本做往返验证**，不能只跑
//   ScriptIoSelfTest 的合成用例。
//
// 两条腿：
//   1) 默认（CI 安全）：内置合成用例，覆盖历史上出过问题的形状
//      （归一化坐标、multiMatch 的 template_ 路径、watchImage、中文/转义）；
//   2) `--corpus <dir>`：递归扫描真实脚本库，对每个脚本做
//        读 → 写 → 再读，逐动作比对 ScriptActionToJsonString
//      并打印差异清单。这是 D1 的「往返基线」。
//
// 往返判定口径（刻意保守）：
//   · 动作条数必须一致；
//   · 每个动作的 ScriptActionToJsonString 必须逐字一致；
//   · 关键顶层字段（名称/时长/热键/窗口模式/坐标元/计时版本）必须一致。
//   任何一项不同即记为差异，并把前几条差异打进 detail。
// =============================================================================
#include "selftest_harness.h"

#include "coord_space.h"
#include "json_util.h"
#include "script_io.h"
#include "script_types.h"
#include "utils.h"

#include <windows.h>

#include <string>
#include <vector>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"roundtrip_wait_basic", L"default",
        L"最简脚本 读→写→再读：动作逐字一致"},
    {L"roundtrip_normalized_coords", L"default",
        L"归一化坐标 n* 往返不丢（历史上出过 n* 被 pixel 冲掉）"},
    {L"roundtrip_multimatch_template_path", L"default",
        L"multiMatch 的 template_ 路径往返不丢（历史上 \\t 被吃成 tab）"},
    {L"roundtrip_watchimage_fields", L"default",
        L"watchImage 的 watchMode / resumeAfterWatch / watchPollSeconds 往返"},
    {L"roundtrip_unicode_and_escapes", L"default",
        L"中文与转义字符（引号/换行/反斜杠）往返不失真"},
    {L"roundtrip_varcompute_code", L"default",
        L"varCompute 的 computeCode 多行代码往返"},
    {L"roundtrip_invalid_escape_path", L"default",
        L"路径含单个反斜杠（JSON 非法转义）时动作不得被静默丢弃"},
    {L"save_is_deterministic", L"default",
        L"同一份数据连续保存两次，产物逐字节一致（无隐藏时间戳/顺序抖动）"},
    {L"field_mapping_anchors", L"default",
        L"给定输入解析出的字段必须等于期望值（往返测试抓不到稳定映射错误）"},
    {L"schema_version_chain", L"default",
        L"老文件无 v → 视为 v1 → 迁移到当前版本 → 保存写出 v → 读回一致（D3）"},
    {L"legacy_roundtrip_clean", L"default",
        L"无 v 的老脚本整条往返仍零差异（D3 迁移不得改动内容）"},
    {L"corpus_roundtrip_clean", L"corpus",
        L"真实脚本库递归往返：零差异（--corpus <dir>）"},
};

std::wstring DirName(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    const wchar_t last = a.back();
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

bool ReadFileUtf8(const std::wstring& path, std::string* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > (1 << 26)) {
        CloseHandle(h);
        return false;
    }
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = out->empty()
        ? TRUE
        : ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out->resize(read);
    return true;
}

// ── 往返 ─────────────────────────────────────────────────────────
struct RoundTripResult {
    bool loaded = false;
    bool saved = false;
    bool reloaded = false;
    int actionsA = 0;
    int actionsB = 0;
    int lexicalActions = 0;   // 词法上的动作块数（仅信息）
    int unparsableBlocks = 0; // 解析不出 type 的块数 —— 这些会被静默丢弃
    std::vector<std::string> diffs;   // 已窄化，便于打进 JSON detail
    bool captureStamped = false;      // captureSize 被盖上当前屏幕（预期，单独计数）
};

void NoteDiff(RoundTripResult& r, const std::string& text) {
    if (r.diffs.size() < 8) r.diffs.push_back(text);
}

/// 关键顶层字段比对（动作以外的部分）
void CompareTopLevel(const ScriptFileData& a, const ScriptFileData& b, RoundTripResult& r) {
    if (a.schemaVersion != b.schemaVersion) NoteDiff(r, "schemaVersion");
    if (a.scriptName != b.scriptName) NoteDiff(r, "scriptName");
    if (a.durationSeconds != b.durationSeconds) NoteDiff(r, "durationSeconds");
    if (a.breakoutTimeSeconds != b.breakoutTimeSeconds) NoteDiff(r, "breakoutTimeSeconds");
    if (a.recordingCaptureMode != b.recordingCaptureMode) NoteDiff(r, "recordingCaptureMode");
    if (a.inputTimingVersion != b.inputTimingVersion) NoteDiff(r, "inputTimingVersion");
    if (a.coordsNormalized != b.coordsNormalized) NoteDiff(r, "coordsNormalized");
    if (a.windowMode.enabled != b.windowMode.enabled) NoteDiff(r, "windowMode.enabled");
    if (a.windowMode.windowClassName != b.windowMode.windowClassName) {
        NoteDiff(r, "windowMode.windowClassName");
    }
    if (a.coordMeta.space != b.coordMeta.space) NoteDiff(r, "coordMeta.space");
    if (a.coordMeta.refOriginX != b.coordMeta.refOriginX
        || a.coordMeta.refOriginY != b.coordMeta.refOriginY) {
        NoteDiff(r, "coordMeta.refOrigin");
    }
    // captureWidth/Height：**刻意不参与严格比对**。
    // script_io.cpp:946 用 CaptureCurrentCoordMeta() 把它盖成**保存时所在机器的
    // 屏幕尺寸**（找图模板缩放的基准），源码注释即写明「像素→n* 用当前屏幕；
    // JSON coordMeta 固定为标准 2560×1440」。实测 14 个真实脚本全部
    // 2560x1440 -> 1707x960（本机屏幕），属**设计如此**，不是数据丢失。
    // 但它是「脚本跨分辨率不字节稳定」的唯一来源，所以单独计数上报，供 D2/D3 参考。
    if (a.coordMeta.captureWidth != b.coordMeta.captureWidth
        || a.coordMeta.captureHeight != b.coordMeta.captureHeight) {
        r.captureStamped = true;
    }
    if (a.coordMeta.refWidth != b.coordMeta.refWidth
        || a.coordMeta.refHeight != b.coordMeta.refHeight) {
        NoteDiff(r, "coordMeta.refSize " + std::to_string(a.coordMeta.refWidth) + "x"
            + std::to_string(a.coordMeta.refHeight) + " -> "
            + std::to_string(b.coordMeta.refWidth) + "x"
            + std::to_string(b.coordMeta.refHeight));
    }
}

RoundTripResult RoundTripContent(const std::wstring& content, const std::wstring& tmpPath) {
    RoundTripResult r;
    ScriptFileData a = ParseScriptContent(content);
    r.loaded = true;
    r.actionsA = static_cast<int>(a.actions.size());

    // ── 交叉校验：解析出的动作数 vs 文件里**词法上**的动作块数 ──────────
    // 为什么必须加这条（2026-09-19 D2 踩到的盲点）：
    // 只比较「读→写→再读」的 A/B 时，如果**两次都丢掉同样的动作**，差异是 0，
    // 看起来完全正常 —— 实测严格解析会把含非法转义的块整块丢掉，而 A/B 都是丢过的，
    // 于是 D1 报「零差异」。这正是本项目反复出现的「用例全绿 ≠ 没坏」。
    // 词法计数（CountActionsInJson）走的是与解析器**无关**的路径，能戳破这种系统性丢失。
    r.lexicalActions = CountActionsInJson(content);
    // 精确探测「静默丢动作」：ParseScriptContent 用 \ 跳过解析不出
    // type 的块。块数对不上可能只是 NormalizeInputTiming 合法合并了相邻 Wait，
    // 所以判据不是「数目相等」，而是**没有任何块解析不出 type**。
    {
        const auto blocks = ExtractJsonActionBlocks(content);
        for (const auto& b : blocks) {
            if (qst::jsonutil::WideObjectView(b).GetString(L"type").empty()) ++r.unparsableBlocks;
        }
    }

    if (!SaveScriptFileData(tmpPath, a)) {
        NoteDiff(r, "SaveScriptFileData failed");
        return r;
    }
    r.saved = true;

    std::string raw;
    if (!ReadFileUtf8(tmpPath, &raw)) {
        NoteDiff(r, "re-read failed");
        return r;
    }
    // 文件按 UTF-8 存；这里按宽字符再解析，走与产品一致的入口
    const std::wstring wcontent = FromUtf8(raw);
    ScriptFileData b = ParseScriptContent(wcontent);
    r.reloaded = true;
    r.actionsB = static_cast<int>(b.actions.size());

    if (r.actionsA != r.actionsB) {
        NoteDiff(r, "action count " + std::to_string(r.actionsA) + " -> " + std::to_string(r.actionsB));
        return r;
    }
    for (int i = 0; i < r.actionsA; ++i) {
        const std::wstring ja = ScriptActionToJsonString(a.actions[static_cast<size_t>(i)]);
        const std::wstring jb = ScriptActionToJsonString(b.actions[static_cast<size_t>(i)]);
        if (ja != jb) {
            std::string d = "action[" + std::to_string(i) + "] differs";
            NoteDiff(r, d);
            if (r.diffs.size() <= 4) {
                const std::string sa = ToUtf8(ja);
                const std::string sb = ToUtf8(jb);
                NoteDiff(r, "  before: " + (sa.size() > 120 ? sa.substr(0, 120) : sa));
                NoteDiff(r, "  after : " + (sb.size() > 120 ? sb.substr(0, 120) : sb));
            }
        }
    }
    CompareTopLevel(a, b, r);
    return r;
}

std::wstring TempPathFor(const std::wstring& name) {
    wchar_t dir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, dir);
    return JoinPath(dir, L"qst_ser_" + name + L".json");
}

std::wstring NarrowToWide(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

/// 内置用例共用的收尾：把结果打成一条 Emit
void EmitRoundTrip(const wchar_t* name, const RoundTripResult& r) {
    // noDrop：没有任何动作块解析不出 type（那会被静默丢弃 = 数据丢失）。
    const bool noDrop = r.unparsableBlocks == 0;
    const bool ok = r.loaded && r.saved && r.reloaded && r.diffs.empty()
        && r.actionsA == r.actionsB && noDrop;
    std::wstring detail = L"actions=" + std::to_wstring(r.actionsA)
        + L"/块=" + std::to_wstring(r.lexicalActions);
    if (!noDrop) detail += L" | 有 " + std::to_wstring(r.unparsableBlocks)
        + L" 个动作块解析失败（会被静默丢弃）";
    if (!ok) {
        detail += L" | ";
        for (size_t i = 0; i < r.diffs.size(); ++i) {
            if (i) detail += L" ; ";
            detail += NarrowToWide(r.diffs[i]);
        }
    }
    Emit(name, ok, detail.c_str());
}

// ── 内置用例 ─────────────────────────────────────────────────────
void CaseWaitBasic() {
    const std::wstring content = LR"({
  "name": "selftest_wait",
  "actions": [
    { "type": "wait", "duration": 0.5 },
    { "type": "wait", "duration": 1.25 }
  ]
})";
    EmitRoundTrip(L"roundtrip_wait_basic", RoundTripContent(content, TempPathFor(L"wait")));
}

void CaseNormalizedCoords() {
    // n* 是归一化坐标；历史上出过「Load 后存回把 n* 冲成像素」的问题
    const std::wstring content = LR"({
  "name": "selftest_norm",
  "coordsNormalized": true,
  "coordMeta": { "space": "window", "refW": 1920, "refH": 1080 },
  "actions": [
    { "type": "mouseClick", "x": 960, "y": 540, "nX": 0.5, "nY": 0.5, "clickCount": 1 },
    { "type": "moveMouse", "x": 100, "y": 200, "nX": 0.0520833, "nY": 0.185185 }
  ]
})";
    EmitRoundTrip(L"roundtrip_normalized_coords", RoundTripContent(content, TempPathFor(L"norm")));
}

void CaseMultimatchTemplatePath() {
    // template_ 路径里含 \t —— 旧解析器会把 \t 当制表符吃掉
    const std::wstring content = LR"({
  "name": "selftest_multimatch",
  "actions": [
    { "type": "multiMatch", "imagePaths": ["C:\\img\\template_a.png", "D:\\x\\template_b.png"],
      "mode": 1, "duration": 2, "followUp": 2, "findTimeExpr": "3" }
  ]
})";
    EmitRoundTrip(L"roundtrip_multimatch_template_path",
        RoundTripContent(content, TempPathFor(L"multimatch")));
}

void CaseWatchImageFields() {
    const std::wstring content = LR"({
  "name": "selftest_watch",
  "actions": [
    { "type": "watchImage", "imagePath": "C:\\img\\t.png", "watchMode": "time",
      "watchPollSeconds": 2.5, "resumeAfterWatch": true, "duration": 10 }
  ]
})";
    EmitRoundTrip(L"roundtrip_watchimage_fields",
        RoundTripContent(content, TempPathFor(L"watch")));
}

void CaseUnicodeAndEscapes() {
    const std::wstring content = LR"({
  "name": "中文脚本名",
  "actions": [
    { "type": "varCompute", "computeCode": "s = \"含引号\\\"与反斜杠\\\\与换行\\n结束\";\nreturn s;" },
    { "type": "wait", "duration": 0.1 }
  ]
})";
    EmitRoundTrip(L"roundtrip_unicode_and_escapes",
        RoundTripContent(content, TempPathFor(L"unicode")));
}

void CaseVarComputeCode() {
    const std::wstring content = LR"({
  "name": "selftest_varcompute",
  "actions": [
    { "type": "varCompute", "computeCode": "a = 1 + 2;\nb = a * 3;\nreturn b;" }
  ]
})";
    EmitRoundTrip(L"roundtrip_varcompute_code",
        RoundTripContent(content, TempPathFor(L"varcompute")));
}

void CaseSaveDeterministic() {
    const std::wstring content = LR"({
  "name": "selftest_det",
  "actions": [
    { "type": "wait", "duration": 0.25 },
    { "type": "mouseClick", "x": 10, "y": 20, "clickCount": 2 }
  ]
})";
    ScriptFileData d = ParseScriptContent(content);
    const std::wstring p1 = TempPathFor(L"det1");
    const std::wstring p2 = TempPathFor(L"det2");
    const bool s1 = SaveScriptFileData(p1, d);
    const bool s2 = SaveScriptFileData(p2, d);
    std::string a, b;
    const bool r1 = ReadFileUtf8(p1, &a);
    const bool r2 = ReadFileUtf8(p2, &b);
    const bool ok = s1 && s2 && r1 && r2 && !a.empty() && a == b;
    Emit(L"save_is_deterministic", ok,
        ok ? L"两次保存逐字节一致" : L"两次保存产物不同（存在隐藏顺序/时间戳抖动）");
}

// 非法转义（单个反斜杠的 Windows 路径）：**动作绝不能被静默丢弃**。
// 2026-09-19 D2 实测：严格解析遇到 `"images\a.png"`（JSON 里 `\a` 非法）会整块失败，
// 旧代码静默跳过该动作 → 3 个动作变 2 个。产品自己写文件时会正确转义，但手改过的
// 脚本可能没有；静默丢数据是本项目最忌讳的失败方式，所以这里钉死。
void CaseInvalidEscapePath() {
    const std::wstring content =
        L"{\"scriptName\":\"selftest_bad_escape\",\"actions\":["
        L"{\"type\":\"findImage\",\"imagePath\":\"images\\a.png\",\"findImageFollowUp\":0},"
        L"{\"type\":\"wait\",\"duration\":1.5}"
        L"]}";
    const RoundTripResult r = RoundTripContent(content, TempPathFor(L"badesc"));
    const bool noDrop = r.actionsA == 2;
    const bool ok = r.loaded && noDrop && r.diffs.empty() && r.actionsA == r.actionsB;
    std::wstring detail = L"actions=" + std::to_wstring(r.actionsA) + L"/块="
        + std::to_wstring(r.lexicalActions) + L"/坏块="
        + std::to_wstring(r.unparsableBlocks);
    if (!noDrop) detail += L" | 非法转义导致动作被丢弃（数据丢失）";
    Emit(L"roundtrip_invalid_escape_path", ok, detail.c_str());
}

// ── D3：schema 版本 ───────────────────────────────────────────────
// 老文件（无 "v"）→ 视为 v1 → 迁移到当前版本 → 保存时写出 "v" → 再读回仍是当前版本。
// 这条链一旦断了，未来改格式就没有迁移锚点，老脚本会静默读错。
void CaseSchemaVersionChain() {
    const std::wstring legacy =
        LR"({"scriptName":"selftest_legacy","actions":[{"type":"wait","duration":0.1}]})";
    const std::wstring path = TempPathFor(L"schema");

    ScriptFileData d = ParseScriptContent(legacy);
    const bool migrated = d.schemaVersion == kScriptSchemaVersion;

    const bool saved = SaveScriptFileData(path, d);
    std::string raw;
    const bool readOk = ReadFileUtf8(path, &raw);
    const std::wstring w = FromUtf8(raw);
    const std::wstring needle = L"\"v\": " + std::to_wstring(kScriptSchemaVersion);
    const bool wroteV = w.find(needle) != std::wstring::npos;

    ScriptFileData back = LoadScriptFileData(path, false);
    const bool readBack = back.schemaVersion == kScriptSchemaVersion;

    // 老文件（显式 "v":1）也要能迁移
    const std::wstring v1 = LR"({"v":1,"scriptName":"x","actions":[{"type":"wait","duration":0.1}]})";
    const bool v1Migrated = ParseScriptContent(v1).schemaVersion == kScriptSchemaVersion;

    const bool ok = migrated && saved && readOk && wroteV && readBack && v1Migrated;
    std::wstring detail = L"迁移=" + std::to_wstring(d.schemaVersion)
        + L" 写出" + needle + L"=" + (wroteV ? L"1" : L"0")
        + L" 读回=" + std::to_wstring(back.schemaVersion)
        + L" v1迁移=" + (v1Migrated ? L"1" : L"0");
    Emit(L"schema_version_chain", ok, detail.c_str());
}

// 老文件（无 v）在整条往返里仍须零差异
void CaseLegacyRoundTripClean() {
    const std::wstring legacy =
        LR"({"scriptName":"selftest_legacy2","recordTime":"2026-01-01 00:00:00",)"
        R"("actions":[{"type":"wait","duration":0.25},)"
        R"({"type":"mouseClick","x":10,"y":20,"clickCount":2}]})";
    const RoundTripResult r = RoundTripContent(legacy, TempPathFor(L"legacy"));
    const bool ok = r.loaded && r.saved && r.reloaded && r.diffs.empty()
        && r.actionsA == r.actionsB && r.unparsableBlocks == 0;
    std::wstring detail = L"actions=" + std::to_wstring(r.actionsA) + L"/块="
        + std::to_wstring(r.lexicalActions);
    if (!ok) {
        for (const auto& d : r.diffs) detail += L" | " + std::wstring(d.begin(), d.end());
    }
    Emit(L"legacy_roundtrip_clean", ok, detail.c_str());
}

// ── 字段锚定（D4 变异测试暴露的必需补充）────────────────────────────
// 为什么不能只做往返：往返比对的是 A=parse(content) 与 B=parse(save(A))。
// 若解析器把某个字段**稳定地**读错（例如 mouseClick 的 x 读成 y），A 与 B 都错得
// 一模一样，往返是「零差异」—— 实测：注入这个变异后 ScriptIoSelfTest 变红 1 条，
// 而往返套件 0 条变红。
// 所以必须有「给定输入 → 解析出的字段必须等于期望值」的锚定断言。
void CaseFieldMappingAnchors() {
    // 关键：用**老脚本**（无 coordMeta）。解析路径是
    //   像素 → n*（按标准 2560×1440）→ 反归一化到当前虚拟屏
    // 所以像素期望值可以**精确算出**：
    //   x_out = x_in / 2560 * vsW ,  y_out = y_in / 1440 * vsH
    // 这正是能抓到「x 读成 y」的地方 —— 只断言 n* 抓不到（n* 有独立的映射），
    // 只断言往返也抓不到（A/B 都错得一样）。
    const std::wstring content =
        LR"({"scriptName":"anchor","actions":[)"
        LR"({"type":"mouseClick","x":111,"y":222,"clickCount":3},)"
        LR"({"type":"wait","duration":1.75},)"
        LR"({"type":"findImage","imagePath":"images\a.png","matchVarName":"hit"},)"
        LR"({"type":"moveMouse","x":333,"y":444},)"
        LR"({"type":"varCompute","computeCode":"a = 7;\nreturn a;"})"
        R"(]})";
    const ScriptFileData d = ParseScriptContent(content);
    int vx = 0, vy = 0, vsW = 0, vsH = 0;
    GetVirtualScreenBounds(vx, vy, vsW, vsH);

    std::wstring bad;
    auto fail = [&bad](const wchar_t* what) {
        if (!bad.empty()) bad += L", ";
        bad += what;
    };

    if (d.actions.size() != 5) {
        fail(L"动作数");
    } else {
        const ScriptAction& click = d.actions[0];
        if (click.type != ActionType::MouseClick) fail(L"[0].type");
        if (std::fabs(click.x - 111.0 / 2560.0 * vsW) > 1.0) fail(L"[0].x（x/y 读错？）");
        if (std::fabs(click.y - 222.0 / 1440.0 * vsH) > 1.0) fail(L"[0].y（x/y 读错？）");
        if (click.clickCount != 3) fail(L"[0].clickCount");

        const ScriptAction& wait = d.actions[1];
        if (wait.type != ActionType::Wait) fail(L"[1].type");
        if (std::fabs(wait.duration - 1.75) > 1e-6) fail(L"[1].duration");

        const ScriptAction& find = d.actions[2];
        if (find.type != ActionType::FindImage) fail(L"[2].type");
        if (find.imagePath.find(L"images") == std::wstring::npos
            || find.imagePath.find(L"a.png") == std::wstring::npos) {
            fail(L"[2].imagePath（路径未落到该字段？）");
        }
        if (find.matchVarName != L"hit") fail(L"[2].matchVarName");

        const ScriptAction& move = d.actions[3];
        if (move.type != ActionType::MoveMouse) fail(L"[3].type");
        if (std::fabs(move.x - 333.0 / 2560.0 * vsW) > 1.0) fail(L"[3].x（x/y 读错？）");
        if (std::fabs(move.y - 444.0 / 1440.0 * vsH) > 1.0) fail(L"[3].y（x/y 读错？）");

        const ScriptAction& vc = d.actions[4];
        if (vc.type != ActionType::VarCompute) fail(L"[4].type");
        if (vc.computeCode.find(L"a = 7;") == std::wstring::npos) fail(L"[4].computeCode");
    }

    const bool ok = bad.empty();
    std::wstring detail = ok ? std::wstring(L"5 个动作的字段全部落到正确成员（像素值按 2560x1440 参考精确核对）")
                             : (L"字段映射错: " + bad);
    if (!ok && d.actions.size() >= 4) {
        detail += L" | 实际 click=(" + std::to_wstring(d.actions[0].x) + L","
            + std::to_wstring(d.actions[0].y) + L") 期望=("
            + std::to_wstring(static_cast<int>(111.0 / 2560.0 * vsW)) + L","
            + std::to_wstring(static_cast<int>(222.0 / 1440.0 * vsH)) + L") vs="
            + std::to_wstring(vsW) + L"x" + std::to_wstring(vsH);
    }
    Emit(L"field_mapping_anchors", ok, detail.c_str());
}

// ── 语料往返（D1 基线）────────────────────────────────────────────
int g_corpusTotal = 0;
int g_corpusClean = 0;
int g_corpusCaptureStamped = 0;
std::vector<std::string> g_corpusDiffs;

void WalkScripts(const std::wstring& dir, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring full = JoinPath(dir, name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            WalkScripts(full, out);
        } else if (name.size() > 5) {
            const std::wstring ext = name.substr(name.size() - 5);
            if (ext == L".json" || ext == L"json") out.push_back(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void RunCorpus(const std::wstring& dir) {
    std::vector<std::wstring> files;
    WalkScripts(dir, files);
    for (size_t i = 0; i < files.size(); ++i) {
        std::string raw;
        if (!ReadFileUtf8(files[i], &raw)) continue;
        const std::wstring content = FromUtf8(raw);
        // 只统计真正是脚本的文件（含 "actions"）；其它 json 跳过
        if (content.find(L"\"actions\"") == std::wstring::npos) continue;
        ++g_corpusTotal;
        const std::wstring tmp = TempPathFor(L"corpus" + std::to_wstring(i));
        const RoundTripResult r = RoundTripContent(content, tmp);
        if (r.captureStamped) ++g_corpusCaptureStamped;
        if (r.diffs.empty() && r.actionsA == r.actionsB && r.unparsableBlocks == 0) {
            ++g_corpusClean;
        } else if (g_corpusDiffs.size() < 10) {
            std::string line = ToUtf8(files[i].substr(DirName(dir).size()));
            line += " (解析=" + std::to_string(r.actionsA)
                + "/块=" + std::to_string(r.lexicalActions)
                + "/坏块=" + std::to_string(r.unparsableBlocks) + ") -> ";
            for (size_t k = 0; k < r.diffs.size() && k < 3; ++k) {
                if (k) line += " ; ";
                line += r.diffs[k];
            }
            g_corpusDiffs.push_back(line);
        }
    }
}

void CaseCorpus(const std::wstring& dir) {
    if (dir.empty()) {
        Emit(L"corpus_roundtrip_clean", true, L"未提供 --corpus，跳过");
        return;
    }
    RunCorpus(dir);
    const bool ok = g_corpusTotal > 0 && g_corpusDiffs.empty();
    std::wstring detail = L"脚本 " + std::to_wstring(g_corpusTotal)
        + L" 个，零差异 " + std::to_wstring(g_corpusClean)
        + L"（其中 " + std::to_wstring(g_corpusCaptureStamped)
        + L" 个 captureSize 被盖上本机屏幕——设计如此，不计差异）";
    if (g_corpusTotal == 0) detail += L"（目录里没找到含 actions 的脚本）";
    for (size_t i = 0; i < g_corpusDiffs.size(); ++i) {
        detail += L"\n      " + NarrowToWide(g_corpusDiffs[i]);
    }
    Emit(L"corpus_roundtrip_clean", ok, detail.c_str());
}

void PrintHelp() {
    std::fwprintf(stdout,
        L"ScriptSerializationSelfTest — 脚本序列化往返（D 段安全网）\n"
        L"\n"
        L"用法:\n"
        L"  ScriptSerializationSelfTest.exe [--json] [--list] [--help]\n"
        L"  ScriptSerializationSelfTest.exe --json --corpus <脚本目录>\n"
        L"\n"
        L"  --corpus  递归扫描真实脚本库做 读→写→再读 往返（D1 基线）\n"
        L"            CI 不跑这一档（依赖本机脚本库），由开发者手动跑\n"
        L"\n"
        L"Agent: .cursor/skills/module-selftest/SKILL.md\n"
        L"  源码: src/script_io.cpp（LoadScriptFileData / SaveScriptFileData）\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool listOnly = false;
    std::wstring corpusDir;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i] ? argv[i] : L"";
        if (a == L"--json") {
            selftest::gJson = true;
            selftest::InitUtf8Stdout();
        } else if (a == L"--list") {
            listOnly = true;
            selftest::InitUtf8Stdout();
        } else if (a == L"--corpus" && i + 1 < argc) {
            corpusDir = argv[++i] ? argv[i] : L"";
        } else if (a == L"--help" || a == L"-h") {
            PrintHelp();
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"ScriptSerializationSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    CaseWaitBasic();
    CaseNormalizedCoords();
    CaseMultimatchTemplatePath();
    CaseWatchImageFields();
    CaseUnicodeAndEscapes();
    CaseVarComputeCode();
    CaseInvalidEscapePath();
    CaseFieldMappingAnchors();
    CaseSchemaVersionChain();
    CaseLegacyRoundTripClean();
    CaseSaveDeterministic();
    if (!corpusDir.empty()) CaseCorpus(corpusDir);

    selftest::EmitSummary();
    return selftest::ExitCode();
}
