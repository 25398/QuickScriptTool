// =============================================================================
// ScriptPackageSelfTest — 脚本包（导出/导入）依赖收集
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:ScriptPackageSelfTest
//   build\Release\ScriptPackageSelfTest.exe --json
//
// 为什么有这个 suite（2026-09-19 修复）：
//   导出脚本时**没有递归收集** RunMacro / MousePlayback 的 targetPath 指向的
//   嵌套脚本，导出的 zip 在嵌套场景下本身就不完整。导出为独立 EXE 会把这个
//   缺口放大（EXE 里没有补救机会）。本 suite 锁住这条链路：
//     ① 只收 runMacro/mousePlayback（runProgram/openFile 的 targetPath 不是脚本）
//     ② 同一脚本里**多个** runMacro 都要改写（不能只改第一处）
//     ③ 循环引用安全
//     ④ 嵌套脚本自己的图片也要收
//     ⑤ 包内子目录能解出来
//     ⑥ 清单往返 + 可移植引用在目标机器上真的解析得到
// =============================================================================
#include "selftest_harness.h"

#include "script_package.h"
#include "script_io.h"
#include "utils.h"

#include <fstream>
#include <string>
#include <vector>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"collect_only_nested_types", L"default",
        L"只收 runMacro/mousePlayback；runProgram/openFile/activateWindow 的 targetPath 不收"},
    {L"collect_keeps_targetpath_and_blockname", L"default",
        L"同一个动作的 targetPath 与 blockName 一起返回（跨机器靠后者回退解析）"},
    {L"rewrite_all_nested_targetpaths", L"default",
        L"同一脚本里两个 runMacro 的 targetPath 都要被改写（不能只改第一处）"},
    {L"rewrite_skips_non_nested", L"default",
        L"runProgram 的 targetPath 不得被改写"},
    {L"rewrite_case_insensitive", L"default",
        L"旧值大小写不同也能命中改写表"},
    {L"sanitize_entry_file_name", L"default",
        L"条目名去掉路径分隔符/非法字符/结尾点与空格"},
    {L"plan_includes_nested_script", L"default",
        L"runMacro 指向的子脚本进计划，portableRef 为裸文件名"},
    {L"plan_cycle_safe", L"default",
        L"A→B→A 循环引用不死循环，只收两份"},
    {L"plan_multilevel_recursion", L"default",
        L"三层嵌套（根→A→B→C）全部收进计划 —— 不是只收一层"},
    {L"plan_rewrites_refs_inside_children", L"default",
        L"**子脚本里**指向孙脚本的引用也要被改写成裸文件名（不能只改根脚本）"},
    {L"plan_blockname_only_ref", L"default",
        L"只用 blockName 引用（targetPath 为空）也要收，且 portableRef 可用"},
    {L"plan_unique_names_on_collision", L"default",
        L"两个不同脚本同名时条目名自动去重（x.json / x-2.json）"},
    {L"plan_collects_nested_images", L"default",
        L"嵌套脚本自己引用的图片也要进 images"},
    {L"plan_reports_missing_ref", L"default",
        L"解析不到的嵌套引用进 missingRefs，但整体仍成功"},
    {L"zip_subdir_roundtrip", L"default",
        L"CreateZipFile 的 scripts\\x.json 条目能被 ExtractZipFile 解到子目录"},
    {L"zip_entry_data_forging_eocd", L"default",
        L"条目**数据**里出现 EOCD 签名 50 4B 05 06（用户模板图就是任意二进制）时，"
        L"解包仍必须找到真正的 EOCD —— 否则整包解出 0 个条目，"
        L"报成「脚本数据解包失败（可能被杀毒软件拦截）」把排查方向带偏"},
    {L"zip_non_ascii_entry_name", L"default",
        L"**中文条目名**必须原样解出（条目名是 UTF-8 字节，逐字节加宽会变乱码 ⇒ "
        L"嵌套宏 scripts\\子脚本.json 与中文模板图在目标机上文件名对不上、永远找不到）"},
    {L"manifest_roundtrip", L"default",
        L"清单写出再读回，entry/ref/name 一致"},
    {L"manifest_player_section", L"default",
        L"清单的 player 段（needOpenCv/bundledOpenCv/needOcr/bundledOcr）往返"},
    {L"scan_self_contained", L"default",
        L"纯输入脚本体检为 IsSelfContainedOnly"},
    {L"scan_dependency_matrix", L"default",
        L"**全量依赖矩阵**：44 种动作逐条比对「体检判定」与「引擎实际依赖」"
        L"（找图/OCR/AI/外部程序），任一格对不上就变红"},
    {L"scan_ocr_region_by_image_needs_opencv", L"default",
        L"textRecognition + ocrRegionByImage 必须算「需要找图」——漏了就不打包 OpenCV，"
        L"执行时 delay-load 桩抛 0xC06D007E 把进程带走"},
    {L"scan_image_ocr_ai", L"default",
        L"找图/OCR/AI/窗口/后台窗口模式各自被识别到（getColor 只在 imageLocate 时算找图）"},
    {L"scan_includes_nested", L"default",
        L"嵌套脚本的能力也计入体检（旧实现只看根脚本）"},
    {L"payload_append_read_roundtrip", L"default",
        L"尾部追加后能读回 offset/size，且 offset+size+24 == 文件大小"},
    {L"payload_reject_plain_file", L"default",
        L"没有尾标的普通文件必须被拒绝（不能误判成有 payload）"},
    {L"payload_dump_bytes_equal", L"default",
        L"dump 出来的 payload 与原始 zip 逐字节相同"},
    {L"portable_ref_resolves_on_target", L"default",
        L"改写后的裸文件名在目标机器上能被 ResolveLibraryScriptPath 解析到"},
};

void WriteUtf8File(const std::wstring& path, const std::string& utf8) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

void RemoveDirIfEmpty(const std::wstring& dir) {
    RemoveDirectoryW(dir.c_str());
}

std::wstring TestRoot() { return AppDir() + L"\\__qst_pkgtest"; }

std::wstring MakeTestDir() {
    const std::wstring root = TestRoot();
    CreateDirectoryW(root.c_str(), nullptr);
    return root;
}

/// 写一个「动作数组」脚本 JSON（够用即可，不需要完整字段）。
std::wstring ScriptJson(const std::wstring& name,
    const std::vector<std::pair<std::wstring, std::wstring>>& actions) {
    std::wstring out = L"{\n  \"scriptName\": \"" + name + L"\",\n  \"actions\": [\n";
    for (size_t i = 0; i < actions.size(); ++i) {
        out += L"    {\"type\": \"" + actions[i].first + L"\", \"indent\": 0, "
            L"\"targetPath\": \"" + actions[i].second + L"\", \"blockName\": \"\"}";
        out += (i + 1 == actions.size()) ? L"\n" : L",\n";
    }
    out += L"  ]\n}\n";
    return out;
}

// ── ① 只收 runMacro / mousePlayback ────────────────────────────────
void CaseCollectOnlyNestedTypes() {
    const std::wstring json =
        L"{\"actions\":["
        L"{\"type\":\"runMacro\",\"targetPath\":\"A.json\",\"blockName\":\"\"},"
        L"{\"type\":\"mousePlayback\",\"targetPath\":\"B.json\",\"blockName\":\"\"},"
        L"{\"type\":\"runProgram\",\"targetPath\":\"notepad.exe\",\"blockName\":\"\"},"
        L"{\"type\":\"openFile\",\"targetPath\":\"C:\\\\doc.txt\",\"blockName\":\"\"},"
        L"{\"type\":\"activateWindow\",\"targetPath\":\"某个窗口\",\"blockName\":\"\"},"
        L"{\"type\":\"runBlock\",\"targetPath\":\"\",\"blockName\":\"块\"}"
        L"]}";
    const auto refs = scriptpkg::CollectNestedRefsFromJson(json);
    const bool ok = refs.size() == 2
        && refs[0].targetPath == L"A.json"
        && refs[1].targetPath == L"B.json";
    Emit(L"collect_only_nested_types", ok,
        ok ? L"" : (L"refs=" + std::to_wstring(refs.size())).c_str());
}

// ── ② targetPath 与 blockName 一起返回 ────────────────────────────
void CaseCollectKeepsBothFields() {
    const std::wstring json =
        L"{\"actions\":[{\"type\":\"runMacro\",\"targetPath\":\"D:\\\\old\\\\子脚本.json\","
        L"\"blockName\":\"子脚本\"}]}";
    const auto refs = scriptpkg::CollectNestedRefsFromJson(json);
    const bool ok = refs.size() == 1
        && refs[0].targetPath == L"D:\\old\\子脚本.json"
        && refs[0].blockName == L"子脚本";
    Emit(L"collect_keeps_targetpath_and_blockname", ok,
        ok ? L"" : L"targetPath/blockName were not both captured");
}

// ── ③ 多个 runMacro 都要改写 ─────────────────────────────────────
void CaseRewriteAllOccurrences() {
    std::wstring json =
        L"{\"actions\":["
        L"{\"type\":\"runMacro\",\"targetPath\":\"D:\\\\s\\\\甲.json\",\"blockName\":\"\"},"
        L"{\"type\":\"wait\",\"duration\":0.1,\"targetPath\":\"\"},"
        L"{\"type\":\"runMacro\",\"targetPath\":\"D:\\\\s\\\\乙.json\",\"blockName\":\"\"}"
        L"]}";
    const std::vector<std::pair<std::wstring, std::wstring>> remap = {
        {L"D:\\s\\甲.json", L"甲.json"},
        {L"D:\\s\\乙.json", L"乙.json"},
    };
    const int n = scriptpkg::RewriteNestedTargetPaths(json, remap);
    // 只替换值的字面量，不重排空白 —— 断言不要假设冒号后有空格
    const bool ok = n == 2
        && json.find(L"\"甲.json\"") != std::wstring::npos
        && json.find(L"\"乙.json\"") != std::wstring::npos
        && json.find(L"D:\\\\s\\\\") == std::wstring::npos;
    Emit(L"rewrite_all_nested_targetpaths", ok,
        ok ? L"" : (L"rewrote=" + std::to_wstring(n) + L" json=" + json).c_str());
}

// ── ④ runProgram 的 targetPath 不动 ──────────────────────────────
void CaseRewriteSkipsNonNested() {
    std::wstring json =
        L"{\"actions\":["
        L"{\"type\":\"runProgram\",\"targetPath\":\"notepad.exe\"},"
        L"{\"type\":\"runMacro\",\"targetPath\":\"notepad.exe\"}"
        L"]}";
    // 故意用同一个值：只有 runMacro 那处该被改
    const std::vector<std::pair<std::wstring, std::wstring>> remap = {
        {L"notepad.exe", L"别的.json"},
    };
    const int n = scriptpkg::RewriteNestedTargetPaths(json, remap);
    const auto first = json.find(L"notepad.exe");
    const auto second = json.find(L"别的.json");
    const bool ok = n == 1 && first != std::wstring::npos && second != std::wstring::npos
        && first < second;
    Emit(L"rewrite_skips_non_nested", ok,
        ok ? L"" : (L"rewrote=" + std::to_wstring(n)).c_str());
}

// ── ⑤ 大小写不敏感 ──────────────────────────────────────────────
void CaseRewriteCaseInsensitive() {
    std::wstring json =
        L"{\"actions\":[{\"type\":\"runMacro\",\"targetPath\":\"D:\\\\Scripts\\\\Child.JSON\"}]}";
    const std::vector<std::pair<std::wstring, std::wstring>> remap = {
        {L"d:\\scripts\\child.json", L"child.json"},
    };
    const int n = scriptpkg::RewriteNestedTargetPaths(json, remap);
    const bool ok = n == 1 && json.find(L"\"child.json\"") != std::wstring::npos
        && json.find(L"Child.JSON") == std::wstring::npos;
    Emit(L"rewrite_case_insensitive", ok,
        ok ? L"" : (L"rewrote=" + std::to_wstring(n) + L" json=" + json).c_str());
}

// ── ⑥ 条目名净化 ────────────────────────────────────────────────
void CaseSanitizeEntryFileName() {
    const bool ok =
        scriptpkg::SanitizeEntryFileName(L"scripts\\子 脚本.json") == L"子 脚本.json"
        && scriptpkg::SanitizeEntryFileName(L"a/b:c*d?.json") == L"b_c_d_.json"
        && scriptpkg::SanitizeEntryFileName(L"trail. ") == L"trail"
        && scriptpkg::SanitizeEntryFileName(L"  x.json  ") == L"x.json";
    Emit(L"sanitize_entry_file_name", ok,
        ok ? L"" : (L"got=" + scriptpkg::SanitizeEntryFileName(L"scripts\\子 脚本.json")).c_str());
}

// ── ⑦ 计划包含嵌套脚本 ──────────────────────────────────────────
void CasePlanIncludesNested() {
    EnsureScriptsDir();
    const std::wstring childPath = ScriptsDir() + L"\\__qst_pkg_child.json";
    const std::wstring parentPath = ScriptsDir() + L"\\__qst_pkg_parent.json";
    WriteUtf8File(childPath, "{\"scriptName\":\"pkgchild\",\"actions\":[]}");
    {
        std::wstring p = ScriptJson(L"pkgparent", {{L"runMacro", childPath}});
        WriteUtf8File(parentPath, ToUtf8(p));
    }

    scriptpkg::PackagePlan plan;
    std::wstring err;
    const bool built = scriptpkg::BuildPackagePlan(parentPath, plan, err);
    const bool ok = built
        && plan.scripts.size() == 2
        && plan.hasNested
        && plan.scripts[0].isRoot
        && plan.scripts[0].entryName == L"script.json"
        && plan.scripts[1].portableRef == L"__qst_pkg_child.json"
        && plan.scripts[1].entryName == L"scripts\\__qst_pkg_child.json"
        && plan.missingRefs.empty();

    DeleteFileW(parentPath.c_str());
    DeleteFileW(childPath.c_str());
    Emit(L"plan_includes_nested_script", ok,
        ok ? L"" : (built ? (L"scripts=" + std::to_wstring(plan.scripts.size())
            + L" err=" + err).c_str() : err.c_str()));
}

// ── ⑧ 循环引用安全 ──────────────────────────────────────────────
void CasePlanCycleSafe() {
    EnsureScriptsDir();
    const std::wstring aPath = ScriptsDir() + L"\\__qst_pkg_cycle_a.json";
    const std::wstring bPath = ScriptsDir() + L"\\__qst_pkg_cycle_b.json";
    WriteUtf8File(aPath, ToUtf8(ScriptJson(L"cyca", {{L"runMacro", bPath}})));
    WriteUtf8File(bPath, ToUtf8(ScriptJson(L"cycb", {{L"runMacro", aPath}})));

    scriptpkg::PackagePlan plan;
    std::wstring err;
    const bool built = scriptpkg::BuildPackagePlan(aPath, plan, err);
    const bool ok = built && plan.scripts.size() == 2;

    DeleteFileW(aPath.c_str());
    DeleteFileW(bPath.c_str());
    Emit(L"plan_cycle_safe", ok,
        ok ? L"" : (built ? (L"scripts=" + std::to_wstring(plan.scripts.size())).c_str()
            : err.c_str()));
}

// ── ⑧.1 多层递归：不是只收一层 ──────────────────────────────────
void CasePlanMultilevelRecursion() {
    EnsureScriptsDir();
    const std::wstring cPath = ScriptsDir() + L"\\__qst_pkg_lv_c.json";
    const std::wstring bPath = ScriptsDir() + L"\\__qst_pkg_lv_b.json";
    const std::wstring aPath = ScriptsDir() + L"\\__qst_pkg_lv_a.json";
    const std::wstring rootPath = ScriptsDir() + L"\\__qst_pkg_lv_root.json";
    // C 是叶子；B→C；A→B；root→A。任何"只收一层"的实现都拿不到 C。
    WriteUtf8File(cPath, ToUtf8(ScriptJson(L"lvc", {{L"runProgram", L"cmd.exe"}})));
    WriteUtf8File(bPath, ToUtf8(ScriptJson(L"lvb", {{L"runMacro", cPath}})));
    WriteUtf8File(aPath, ToUtf8(ScriptJson(L"lva", {{L"runMacro", bPath}})));
    WriteUtf8File(rootPath, ToUtf8(ScriptJson(L"lvroot", {{L"runMacro", aPath}})));

    scriptpkg::PackagePlan plan;
    std::wstring err;
    const bool built = scriptpkg::BuildPackagePlan(rootPath, plan, err);

    bool hasC = false;
    for (const auto& e : plan.scripts) {
        if (LibraryPathsEqual(e.srcPath, cPath)) hasC = true;
    }
    const bool ok = built && plan.scripts.size() == 4 && hasC && plan.missingRefs.empty();

    DeleteFileW(aPath.c_str());
    DeleteFileW(bPath.c_str());
    DeleteFileW(cPath.c_str());
    DeleteFileW(rootPath.c_str());
    Emit(L"plan_multilevel_recursion", ok,
        ok ? L"" : (built ? (L"scripts=" + std::to_wstring(plan.scripts.size())
            + L" hasC=" + std::to_wstring(hasC ? 1 : 0)).c_str() : err.c_str()));
}

// ── ⑧.2 子脚本里的引用也要被改写 ────────────────────────────────
// 导出侧是"用全量 remap 改写**每一份**脚本"，漏掉子脚本 = 孙脚本在目标机上找不到。
void CasePlanRewritesRefsInsideChildren() {
    EnsureScriptsDir();
    const std::wstring gPath = ScriptsDir() + L"\\__qst_pkg_rw_grand.json";
    const std::wstring cPath = ScriptsDir() + L"\\__qst_pkg_rw_child.json";
    const std::wstring rPath = ScriptsDir() + L"\\__qst_pkg_rw_root.json";
    WriteUtf8File(gPath, ToUtf8(ScriptJson(L"rwgrand", {{L"runProgram", L"cmd.exe"}})));
    WriteUtf8File(cPath, ToUtf8(ScriptJson(L"rwchild", {{L"runMacro", gPath}})));
    WriteUtf8File(rPath, ToUtf8(ScriptJson(L"rwroot", {{L"runMacro", cPath}})));

    scriptpkg::PackagePlan plan;
    std::wstring err;
    const bool built = scriptpkg::BuildPackagePlan(rPath, plan, err);

    // 按导出侧的方式构建全量 remap
    std::vector<std::pair<std::wstring, std::wstring>> remap;
    for (const auto& e : plan.scripts) {
        if (e.isRoot || e.portableRef.empty()) continue;
        for (const auto& ref : e.originalRefs) {
            if (!Trim(ref).empty()) remap.emplace_back(Trim(ref), e.portableRef);
        }
    }

    // 子脚本内容改写过之后，孙脚本的绝对路径必须消失、裸文件名必须出现
    std::wstring childContent = ReadAll(cPath);
    const int changed = scriptpkg::RewriteNestedTargetPaths(childContent, remap);
    std::wstring grandRef;
    for (const auto& e : plan.scripts) {
        if (LibraryPathsEqual(e.srcPath, gPath)) grandRef = e.portableRef;
    }
    const bool ok = built && changed >= 1 && !grandRef.empty()
        && childContent.find(gPath) == std::wstring::npos
        && childContent.find(grandRef) != std::wstring::npos;

    DeleteFileW(gPath.c_str());
    DeleteFileW(cPath.c_str());
    DeleteFileW(rPath.c_str());
    Emit(L"plan_rewrites_refs_inside_children", ok,
        ok ? L"" : (L"changed=" + std::to_wstring(changed)
            + L" grandRef=" + grandRef).c_str());
}

// ── ⑧.3 只用 blockName 引用 ─────────────────────────────────────
void CasePlanBlockNameOnlyRef() {
    EnsureScriptsDir();
    const std::wstring childPath = ScriptsDir() + L"\\__qst_pkg_bn_child.json";
    const std::wstring rootPath = ScriptsDir() + L"\\__qst_pkg_bn_root.json";
    WriteUtf8File(childPath, ToUtf8(ScriptJson(L"bnchild", {{L"runProgram", L"cmd.exe"}})));
    // targetPath 空、只有 blockName（= 库内脚本文件名，见 ResolveNestedLibraryTarget）
    WriteUtf8File(rootPath, ToUtf8(std::wstring(L"{\"scriptName\":\"bnroot\",\"actions\":[")
        + L"{\"type\":\"runMacro\",\"targetPath\":\"\",\"blockName\":\"__qst_pkg_bn_child\"}]}"));

    scriptpkg::PackagePlan plan;
    std::wstring err;
    const bool built = scriptpkg::BuildPackagePlan(rootPath, plan, err);
    const bool ok = built && plan.scripts.size() == 2
        && !plan.scripts[1].portableRef.empty()
        && plan.missingRefs.empty();

    DeleteFileW(childPath.c_str());
    DeleteFileW(rootPath.c_str());
    Emit(L"plan_blockname_only_ref", ok,
        ok ? L"" : (built ? (L"scripts=" + std::to_wstring(plan.scripts.size())
            + L" missing=" + std::to_wstring(plan.missingRefs.size())).c_str() : err.c_str()));
}

// ── ⑧.4 同名脚本自动去重 ────────────────────────────────────────
void CasePlanUniqueNamesOnCollision() {
    EnsureScriptsDir();
    // 两个**不同文件**但同名（不同目录）的脚本，被根脚本同时引用
    const std::wstring d1 = ScriptsDir() + L"\\__qst_pkg_dup1";
    const std::wstring d2 = ScriptsDir() + L"\\__qst_pkg_dup2";
    CreateDirectoryW(d1.c_str(), nullptr);
    CreateDirectoryW(d2.c_str(), nullptr);
    const std::wstring c1 = d1 + L"\\same.json";
    const std::wstring c2 = d2 + L"\\same.json";
    const std::wstring rootPath = ScriptsDir() + L"\\__qst_pkg_dup_root.json";
    WriteUtf8File(c1, ToUtf8(ScriptJson(L"same1", {{L"runProgram", L"cmd.exe"}})));
    WriteUtf8File(c2, ToUtf8(ScriptJson(L"same2", {{L"runProgram", L"cmd.exe"}})));
    WriteUtf8File(rootPath, ToUtf8(ScriptJson(L"duproot",
        {{L"runMacro", c1}, {L"runMacro", c2}})));

    scriptpkg::PackagePlan plan;
    std::wstring err;
    const bool built = scriptpkg::BuildPackagePlan(rootPath, plan, err);

    bool ok = built && plan.scripts.size() == 3;
    std::wstring r1, r2;
    if (ok) {
        r1 = plan.scripts[1].portableRef;
        r2 = plan.scripts[2].portableRef;
        ok = !r1.empty() && !r2.empty() && r1 != r2;   // 必须去重，否则后一个覆盖前一个
    }

    DeleteFileW(c1.c_str());
    DeleteFileW(c2.c_str());
    DeleteFileW(rootPath.c_str());
    RemoveDirectoryW(d1.c_str());
    RemoveDirectoryW(d2.c_str());
    Emit(L"plan_unique_names_on_collision", ok,
        ok ? L"" : (L"refs=" + r1 + L" / " + r2).c_str());
}

// ── ⑨ 嵌套脚本的图片也收 ────────────────────────────────────────
void CasePlanCollectsNestedImages() {
    EnsureScriptsDir();
    EnsureFindImagesDir();
    const std::wstring imgName = L"__qst_pkg_img.bmp";
    const std::wstring imgPath = FindImagesDir() + L"\\" + imgName;
    WriteUtf8File(imgPath, "BM");

    const std::wstring childPath = ScriptsDir() + L"\\__qst_pkg_imgchild.json";
    const std::wstring parentPath = ScriptsDir() + L"\\__qst_pkg_imgparent.json";
    WriteUtf8File(childPath, ToUtf8(std::wstring(L"{\"scriptName\":\"imgchild\",\"actions\":[")
        + L"{\"type\":\"findImage\",\"imagePath\":\"images/" + imgName + L"\","
          L"\"matchThreshold\":80}]}"));
    WriteUtf8File(parentPath, ToUtf8(ScriptJson(L"imgparent", {{L"runMacro", childPath}})));

    scriptpkg::PackagePlan plan;
    std::wstring err;
    const bool built = scriptpkg::BuildPackagePlan(parentPath, plan, err);
    bool hasImg = false;
    for (const auto& p : plan.images) {
        if (p.find(imgName) != std::wstring::npos) hasImg = true;
    }
    const bool ok = built && hasImg;

    DeleteFileW(parentPath.c_str());
    DeleteFileW(childPath.c_str());
    DeleteFileW(imgPath.c_str());
    Emit(L"plan_collects_nested_images", ok,
        ok ? L"" : (built ? (L"images=" + std::to_wstring(plan.images.size())).c_str()
            : err.c_str()));
}

// ── ⑩ 解析不到的引用进 missingRefs ──────────────────────────────
void CasePlanReportsMissingRef() {
    EnsureScriptsDir();
    const std::wstring parentPath = ScriptsDir() + L"\\__qst_pkg_missing.json";
    WriteUtf8File(parentPath, ToUtf8(ScriptJson(L"missingparent",
        {{L"runMacro", L"__qst_pkg_绝对不存在_xyz.json"}})));

    scriptpkg::PackagePlan plan;
    std::wstring err;
    const bool built = scriptpkg::BuildPackagePlan(parentPath, plan, err);
    const bool ok = built && !plan.missingRefs.empty() && plan.scripts.size() == 1;

    DeleteFileW(parentPath.c_str());
    Emit(L"plan_reports_missing_ref", ok,
        ok ? L"" : (built ? (L"missing=" + std::to_wstring(plan.missingRefs.size())).c_str()
            : err.c_str()));
}

// ── ⑪ zip 子目录往返 ───────────────────────────────────────────
void CaseZipSubdirRoundtrip() {
    const std::wstring root = MakeTestDir();
    const std::wstring srcFile = root + L"\\__qst_pkg_zip_src.json";
    const std::wstring zipPath = root + L"\\__qst_pkg_zip.zip";
    const std::wstring outDir = root + L"\\out";
    WriteUtf8File(srcFile, "{\"scriptName\":\"z\"}");

    const auto zipResult = CreateZipFile(zipPath,
        {{L"script.json", srcFile}, {L"scripts\\nested.json", srcFile}}, srcFile);
    const int extracted = zipResult.success ? ExtractZipFile(zipPath, outDir) : -1;
    const std::wstring nestedOut = outDir + L"\\scripts\\nested.json";
    const bool ok = zipResult.success && extracted == 2
        && GetFileAttributesW(nestedOut.c_str()) != INVALID_FILE_ATTRIBUTES;

    DeleteFileW(nestedOut.c_str());
    RemoveDirectoryW((outDir + L"\\scripts").c_str());
    DeleteFileW((outDir + L"\\script.json").c_str());
    RemoveDirectoryW(outDir.c_str());
    DeleteFileW(zipPath.c_str());
    DeleteFileW(srcFile.c_str());
    Emit(L"zip_subdir_roundtrip", ok,
        ok ? L"" : (L"zipOk=" + std::to_wstring(zipResult.success ? 1 : 0)
            + L" extracted=" + std::to_wstring(extracted)).c_str());
}

// ── ⑪b 条目数据伪造 EOCD 签名 ──────────────────────────────────
// 为什么必须有这条：`ExtractZipFile` 找 EOCD 靠 4 字节签名 `50 4B 05 06`，而这个字节序列
// **完全可能自然出现在条目数据里**（网页在线导出会把用户上传的模板图原样塞进 payload）。
// 旧实现从 searchStart 向前扫描、撞见第一个签名就采信 ⇒ 拿到的是数据里的假 EOCD，
// centralDirOffset/totalEntries 全是垃圾 ⇒ 解包 0 个条目。
// 这条用例构造一个「数据里带假签名、真 EOCD 在末尾」的包，断言它照样能解出来。
void CaseZipEntryDataForgingEocd() {
    const std::wstring root = MakeTestDir();
    const std::wstring payloadFile = root + L"\\__qst_pkg_forge.bin";
    const std::wstring zipPath = root + L"\\__qst_pkg_forge.zip";
    const std::wstring outDir = root + L"\\out_forge";

    // 假 EOCD 放在数据**开头**（保证它在真 EOCD 之前被扫到），后面再来一段普通数据。
    // 20 字节起填，前 4 字节正好是签名 50 4B 05 06 —— 与真实 EOCD 同形。
    std::string data = "PK\x05\x06";
    data += std::string(18, '\x00');                 // 凑成一段"看起来像 EOCD"的 22 字节
    data += "tail-bytes-after-fake-eocd";
    WriteUtf8File(payloadFile, data);

    const auto zipResult = CreateZipFile(zipPath,
        {{L"scripts\\images\\tpl.bin", payloadFile}}, payloadFile);
    const int extracted = zipResult.success ? ExtractZipFile(zipPath, outDir) : -1;
    const std::wstring outFile = outDir + L"\\scripts\\images\\tpl.bin";

    // 解出来的内容也必须逐字节等于原文件（不能解出一个"半个包"）。
    bool bytesEqual = false;
    if (GetFileAttributesW(outFile.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::ifstream in(outFile, std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        bytesEqual = (got == data);
    }
    const bool ok = zipResult.success && extracted == 1 && bytesEqual;

    DeleteFileW(outFile.c_str());
    RemoveDirectoryW((outDir + L"\\scripts\\images").c_str());
    RemoveDirectoryW((outDir + L"\\scripts").c_str());
    RemoveDirectoryW(outDir.c_str());
    DeleteFileW(zipPath.c_str());
    DeleteFileW(payloadFile.c_str());
    Emit(L"zip_entry_data_forging_eocd", ok,
        ok ? L"" : (L"zipOk=" + std::to_wstring(zipResult.success ? 1 : 0)
            + L" extracted=" + std::to_wstring(extracted)
            + L" bytesEqual=" + std::to_wstring(bytesEqual ? 1 : 0)).c_str());
}

// ── ⑪c 非 ASCII 条目名 ─────────────────────────────────────────
// 条目名在 zip 里是 **UTF-8 字节**（CreateZipFile 用 ArchiveNameUtf8 写出）。
// 读取侧若把每个字节直接加宽成 wchar_t，中文名就变成乱码宽字符 —— 文件会被解成
// 另一个名字，而按真名去找的一方（ResolveLibraryScriptPath / ResolveImagePath）
// 永远找不到它。纯英文名一直是好的，所以这条只在中文名上暴露。
void CaseZipNonAsciiEntryName() {
    const std::wstring root = MakeTestDir();
    const std::wstring payloadFile = root + L"\\__qst_pkg_cjk.bin";
    const std::wstring zipPath = root + L"\\__qst_pkg_cjk.zip";
    const std::wstring outDir = root + L"\\out_cjk";
    WriteUtf8File(payloadFile, "cjk-payload");

    const std::wstring cjkName = L"scripts\\\u5B50\u811A\u672C.json";   // scripts\子脚本.json
    const auto zipResult = CreateZipFile(zipPath, {{cjkName, payloadFile}}, payloadFile);
    const int extracted = zipResult.success ? ExtractZipFile(zipPath, outDir) : -1;
    const std::wstring outFile = outDir + L"\\scripts\\\u5B50\u811A\u672C.json";
    const bool exists = GetFileAttributesW(outFile.c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool ok = zipResult.success && extracted == 1 && exists;

    DeleteFileW(outFile.c_str());
    RemoveDirectoryW((outDir + L"\\scripts").c_str());
    RemoveDirectoryW(outDir.c_str());
    DeleteFileW(zipPath.c_str());
    DeleteFileW(payloadFile.c_str());
    Emit(L"zip_non_ascii_entry_name", ok,
        ok ? L"" : (L"zipOk=" + std::to_wstring(zipResult.success ? 1 : 0)
            + L" extracted=" + std::to_wstring(extracted)
            + L" fileExists=" + std::to_wstring(exists ? 1 : 0)).c_str());
}

// ── ⑫ 清单往返 ─────────────────────────────────────────────────
void CaseManifestRoundtrip() {
    scriptpkg::PackagePlan plan;
    scriptpkg::ScriptEntry root;
    root.entryName = L"script.json";
    root.isRoot = true;
    plan.scripts.push_back(root);
    scriptpkg::ScriptEntry e;
    e.entryName = L"scripts\\子脚本.json";
    e.portableRef = L"子脚本.json";
    e.displayName = L"子脚本";
    plan.scripts.push_back(e);
    plan.images.push_back(L"C:\\x\\images\\template_1.bmp");
    plan.hasNested = true;

    const std::wstring json = scriptpkg::BuildPackageManifestJson(plan, scriptpkg::PlayerManifest{});
    std::vector<scriptpkg::ManifestEntry> parsed;
    const bool parsedOk = scriptpkg::ParsePackageManifestJson(json, parsed);
    const bool ok = parsedOk && parsed.size() == 1
        && parsed[0].entryName == L"scripts\\子脚本.json"
        && parsed[0].portableRef == L"子脚本.json"
        && parsed[0].displayName == L"子脚本";
    Emit(L"manifest_roundtrip", ok,
        ok ? L"" : (parsedOk ? (L"entries=" + std::to_wstring(parsed.size())).c_str()
            : L"manifest parse failed"));
}

// ── ⑫b player 段往返 ────────────────────────────────────────────
void CaseManifestPlayerSection() {
    scriptpkg::PackagePlan plan;
    scriptpkg::ScriptEntry root;
    root.entryName = L"script.json";
    root.isRoot = true;
    plan.scripts.push_back(root);

    scriptpkg::PlayerManifest pm;
    pm.needOpenCv = true;
    pm.needOcr = true;
    pm.needFakeFocus = false;
    pm.mode.bundledOpenCv = false;  // 走软件
    pm.mode.bundledOcr = true;      // 自带
    pm.createdBy = L"1.3.4";
    const std::wstring json = scriptpkg::BuildPackageManifestJson(plan, pm);

    scriptpkg::PlayerManifest got;
    const bool parsedOk = scriptpkg::ParsePlayerManifest(json, got);
    const bool ok = parsedOk
        && got.needOpenCv && got.needOcr && !got.needFakeFocus
        && !got.mode.bundledOpenCv && got.mode.bundledOcr && got.mode.bundledFakeFocus
        && got.createdBy == L"1.3.4";

    // 旧包（没有 player 段）必须返回 false，而不是崩或给出脏值
    scriptpkg::PlayerManifest legacy;
    const bool legacyParsed = scriptpkg::ParsePlayerManifest(L"{\"v\":1,\"root\":\"script.json\"}", legacy);
    const bool ok2 = ok && !legacyParsed && legacy.mode.bundledOpenCv;
    Emit(L"manifest_player_section", ok2,
        ok2 ? L"" : (L"parsed=" + std::to_wstring(parsedOk ? 1 : 0)
            + L" legacy=" + std::to_wstring(legacyParsed ? 1 : 0)).c_str());
}

// ── ⑬ 体检：纯输入脚本 ──────────────────────────────────────────
/// 用户实测踩过的坑：`textRecognition` 带 `ocrRegionByImage=1` 时，引擎会**先找图**
/// 拿到区域再 OCR —— 那条路要 OpenCV。体检漏掉它 ⇒ 导出的 exe 不带 OpenCV，
/// 而执行时仍然去调 OpenCV ⇒ delay-load 桩抛 0xC06D007E ⇒ **进程当场死**
/// （没日志、没结束音）。这条用例就是防止再漏。
/// ═══ 全量依赖矩阵 ═══
/// 逐条把「体检判定」与「引擎实际依赖」对齐。这张表是**审计产物**：
/// 引擎回放路径上所有 OpenCV / OCR / AI / 外部程序触点都被逐个追到动作类型上。
///
/// ⚠ 加新动作、或给动作加新的图片字段时，**必须回来改这张表** ——
///   漏一格的代价是"导出的 exe 缺组件"，轻则功能不可用、重则进程当场死（0xC06D007E）。
struct DepRow {
    const wchar_t* type;
    const wchar_t* extra;      // 追加到动作对象里的字段（含前后逗号，或空串）
    int openCv;                // 期望：需要 OpenCV
    int ocr;                   // 期望：需要 OCR
    int ai;                    // 期望：需要 AI
    int external;              // 期望：需要外部程序
};

const DepRow kDepMatrix[] = {
    // ── 纯输入：什么都不需要 ───────────────────────────────────
    { L"moveMouse",          L"", 0, 0, 0, 0 },
    { L"moveMouseRelative",  L"", 0, 0, 0, 0 },
    { L"mouseDown",          L"", 0, 0, 0, 0 },
    { L"mouseUp",            L"", 0, 0, 0, 0 },
    { L"mouseClick",         L"", 0, 0, 0, 0 },
    { L"keyDown",            L"", 0, 0, 0, 0 },
    { L"keyUp",              L"", 0, 0, 0, 0 },
    { L"keyClick",           L"", 0, 0, 0, 0 },
    { L"hotkeyShortcut",     L"", 0, 0, 0, 0 },
    { L"quickInput",         L"", 0, 0, 0, 0 },
    { L"scrollWheel",        L"", 0, 0, 0, 0 },
    { L"wait",               L"", 0, 0, 0, 0 },
    { L"customText",         L"", 0, 0, 0, 0 },
    // ── 流程控制：不需要外部组件 ───────────────────────────────
    { L"loop",               L"", 0, 0, 0, 0 },
    { L"endLoop",            L"", 0, 0, 0, 0 },
    { L"defineBlock",        L"", 0, 0, 0, 0 },
    { L"runBlock",           L"", 0, 0, 0, 0 },
    { L"if",                 L"", 0, 0, 0, 0 },
    { L"else",               L"", 0, 0, 0, 0 },
    { L"goto",               L"", 0, 0, 0, 0 },
    { L"stopMacro",          L"", 0, 0, 0, 0 },
    { L"varCompute",         L"", 0, 0, 0, 0 },
    { L"timerRecordTime",    L"", 0, 0, 0, 0 },
    { L"getCursorPos",       L"", 0, 0, 0, 0 },
    { L"lockScreenshot",     L"", 0, 0, 0, 0 },
    { L"unlockScreenshot",   L"", 0, 0, 0, 0 },
    { L"activateWindow",     L"", 0, 0, 0, 0 },
    // 嵌套宏：本体不直接依赖组件，靠 BuildPackagePlan 递归收集子脚本的能力
    { L"runMacro",           L"", 0, 0, 0, 0 },
    { L"mousePlayback",      L"", 0, 0, 0, 0 },
    // ── 找图族：无条件 OpenCV ─────────────────────────────────
    { L"findImage",          L"", 1, 0, 0, 0 },
    { L"watchImage",         L"", 1, 0, 0, 0 },
    { L"multiMatch",         L"", 1, 0, 0, 0 },
    { L"findColor",          L"", 1, 0, 0, 0 },
    { L"colorMatch",         L"", 1, 0, 0, 0 },
    // ── 条件 OpenCV：只有勾了「找图定位」才要 ──────────────────
    { L"mouseDrag",          L",\"imageLocate\":0", 0, 0, 0, 0 },
    { L"mouseDrag",          L",\"imageLocate\":1", 1, 0, 0, 0 },
    { L"getColor",           L",\"imageLocate\":0", 0, 0, 0, 0 },
    { L"getColor",           L",\"imageLocate\":1", 1, 0, 0, 0 },
    // ── OCR ───────────────────────────────────────────────────
    { L"textRecognition",    L",\"ocrRegionByImage\":0", 0, 1, 0, 0 },
    // 按图取区域：引擎会**先找图**再 OCR → 同时要 OpenCV 和 OCR
    { L"textRecognition",    L",\"ocrRegionByImage\":1", 1, 1, 0, 0 },
    // ── AI ────────────────────────────────────────────────────
    // 文字分析：纯文本，全程不碰图
    { L"aiTextAnalysis",     L"", 0, 0, 1, 0 },
    // 图片分析 / 动作执行：①按图取区域要 LoadBitmapFromFile；
    //                      ②发截图给模型要 cv::imencode（无条件）→ 必须带 OpenCV
    { L"aiImageAnalysis",    L"", 1, 0, 1, 0 },
    { L"aiActionExecute",    L"", 1, 0, 1, 0 },
    // ── 外部程序 ──────────────────────────────────────────────
    { L"runProgram",         L"", 0, 0, 0, 1 },
    { L"openFile",           L"", 0, 0, 0, 1 },
    { L"openWebpage",        L"", 0, 0, 0, 1 },
    // closeProgram 只按进程名关窗口，不依赖任何随包文件
    { L"closeProgram",       L"", 0, 0, 0, 0 },
};

void CaseScanDependencyMatrix() {
    const size_t rows = sizeof(kDepMatrix) / sizeof(kDepMatrix[0]);
    for (size_t i = 0; i < rows; ++i) {
        const DepRow& r = kDepMatrix[i];
        const std::wstring json = std::wstring(L"{\"actions\":[{\"type\":\"") + r.type
            + L"\"" + r.extra + L"}]}";
        scriptpkg::CapabilityScan acc;
        scriptpkg::ScanActionCapabilities(json, acc);

        const int gotCv = acc.imageActions > 0 ? 1 : 0;
        const int gotOcr = acc.ocrActions > 0 ? 1 : 0;
        const int gotAi = acc.aiActions > 0 ? 1 : 0;
        const int gotExt = acc.externalActions > 0 ? 1 : 0;
        if (gotCv == r.openCv && gotOcr == r.ocr && gotAi == r.ai && gotExt == r.external) {
            continue;
        }
        wchar_t buf[512]{};
        swprintf_s(buf,
            L"%ls%ls 期望(找图=%d OCR=%d AI=%d 外部=%d) 实际(找图=%d OCR=%d AI=%d 外部=%d)",
            r.type, r.extra, r.openCv, r.ocr, r.ai, r.external,
            gotCv, gotOcr, gotAi, gotExt);
        Emit(L"scan_dependency_matrix", false, buf);
        return;
    }
    Emit(L"scan_dependency_matrix", true, L"");
}

void CaseScanOcrRegionByImageNeedsOpenCv() {
    // ① 纯 OCR（整屏/固定区域）：不算找图
    {
        scriptpkg::CapabilityScan acc;
        scriptpkg::ScanActionCapabilities(
            L"{\"actions\":[{\"type\":\"textRecognition\",\"ocrRegionByImage\":0}]}", acc);
        if (acc.imageActions != 0 || acc.ocrActions != 1) {
            Emit(L"scan_ocr_region_by_image_needs_opencv", false,
                L"纯 OCR 被误判成需要找图");
            return;
        }
    }
    // ② 按图取区域：必须同时算「找图」+「OCR」
    {
        scriptpkg::CapabilityScan acc;
        scriptpkg::ScanActionCapabilities(
            L"{\"actions\":[{\"type\":\"textRecognition\",\"ocrRegionByImage\":1,"
            L"\"imagePath\":\"images/a.bmp\"}]}", acc);
        const bool ok = acc.imageActions == 1 && acc.ocrActions == 1 && acc.NeedsOpenCv();
        Emit(L"scan_ocr_region_by_image_needs_opencv", ok,
            ok ? L"" : (L"imageActions=" + std::to_wstring(acc.imageActions)
                + L" ocrActions=" + std::to_wstring(acc.ocrActions)).c_str());
    }
}

void CaseScanSelfContained() {
    const std::wstring json =
        L"{\"actions\":["
        L"{\"type\":\"moveMouse\",\"x\":1,\"y\":2},"
        L"{\"type\":\"wait\",\"duration\":0.1},"
        L"{\"type\":\"keyClick\",\"keyVk\":65},"
        L"{\"type\":\"loop\",\"loopCount\":3},"
        L"{\"type\":\"if\",\"conditionExpr\":\"a>1\"}"
        L"]}";
    scriptpkg::CapabilityScan acc;
    scriptpkg::ScanActionCapabilities(json, acc);
    const bool ok = acc.totalActions == 5 && acc.IsSelfContainedOnly()
        && !acc.windowMode && !acc.hasHotkey;
    Emit(L"scan_self_contained", ok,
        ok ? L"" : (L"total=" + std::to_wstring(acc.totalActions)
            + L" img=" + std::to_wstring(acc.imageActions)).c_str());
}

// ── ⑬b 体检：找图 / OCR / AI / 窗口/后台窗口模式 ─────────────────────────
void CaseScanImageOcrAi() {
    const std::wstring json =
        L"{\"hotkeyVk\":119,"
        L"\"windowMode\":{\"enabled\":1,\"selectMethod\":0},"
        L"\"actions\":["
        L"{\"type\":\"findImage\",\"imagePath\":\"images/a.bmp\"},"
        L"{\"type\":\"multiMatch\",\"imagePaths\":[\"images/b.bmp\"]},"
        L"{\"type\":\"watchImage\",\"imagePath\":\"images/c.bmp\"},"
        L"{\"type\":\"findColor\",\"colorR\":1},"
        L"{\"type\":\"getColor\",\"imageLocate\":0},"
        L"{\"type\":\"getColor\",\"imageLocate\":1,\"imagePath\":\"images/d.bmp\"},"
        L"{\"type\":\"mouseDrag\",\"imageLocate\":0},"
        L"{\"type\":\"textRecognition\"},"
        L"{\"type\":\"aiTextAnalysis\"},"
        L"{\"type\":\"runProgram\",\"targetPath\":\"notepad.exe\"}"
        L"]}";
    scriptpkg::CapabilityScan acc;
    scriptpkg::ScanActionCapabilities(json, acc);
    // 找图：findImage + multiMatch + watchImage + findColor + getColor(imageLocate=1) = 5
    // （getColor(imageLocate=0) 与 mouseDrag(imageLocate=0) 不算）
    const bool ok = acc.totalActions == 10
        && acc.imageActions == 5
        && acc.ocrActions == 1
        && acc.aiActions == 1
        && acc.externalActions == 1
        && acc.windowMode
        && acc.hasHotkey;
    Emit(L"scan_image_ocr_ai", ok,
        ok ? L"" : (L"total=" + std::to_wstring(acc.totalActions)
            + L" img=" + std::to_wstring(acc.imageActions)
            + L" ocr=" + std::to_wstring(acc.ocrActions)
            + L" ai=" + std::to_wstring(acc.aiActions)
            + L" wm=" + std::to_wstring(acc.windowMode ? 1 : 0)).c_str());
}

// ── ⑬c 体检要覆盖嵌套脚本 ──────────────────────────────────────
void CaseScanIncludesNested() {
    EnsureScriptsDir();
    const std::wstring childPath = ScriptsDir() + L"\\__qst_pkg_scanchild.json";
    const std::wstring parentPath = ScriptsDir() + L"\\__qst_pkg_scanparent.json";
    WriteUtf8File(childPath, ToUtf8(std::wstring(L"{\"scriptName\":\"scanchild\",\"actions\":[")
        + L"{\"type\":\"findImage\",\"imagePath\":\"images/x.bmp\"}]}"));
    WriteUtf8File(parentPath, ToUtf8(ScriptJson(L"scanparent", {{L"runMacro", childPath}})));

    scriptpkg::PackagePlan plan;
    std::wstring err;
    const bool built = scriptpkg::BuildPackagePlan(parentPath, plan, err);
    const scriptpkg::CapabilityScan acc = scriptpkg::ScanPackageCapabilities(plan);
    // 父脚本 1 个 runMacro + 子脚本 1 个 findImage
    const bool ok = built && acc.totalActions == 2 && acc.imageActions == 1;

    DeleteFileW(parentPath.c_str());
    DeleteFileW(childPath.c_str());
    Emit(L"scan_includes_nested", ok,
        ok ? L"" : (L"total=" + std::to_wstring(acc.totalActions)
            + L" img=" + std::to_wstring(acc.imageActions)).c_str());
}

// ── ⑭ payload 追加 / 读回 ──────────────────────────────────────
void CasePayloadRoundtrip() {
    const std::wstring root = MakeTestDir();
    const std::wstring fakeExe = root + L"\\__qst_payload_player.exe";
    const std::wstring zipPath = root + L"\\__qst_payload.zip";
    WriteUtf8File(fakeExe, "MZ-fake-exe-body");
    {
        const auto zr = CreateZipFile(zipPath, {{L"script.json", fakeExe}}, fakeExe);
        if (!zr.success) {
            Emit(L"payload_append_read_roundtrip", false, L"zip build failed");
            return;
        }
    }

    scriptpkg::PayloadInfo info;
    const bool appended = scriptpkg::AppendPayloadToExe(fakeExe, zipPath, &info);
    scriptpkg::PayloadInfo read;
    const bool readOk = scriptpkg::ReadPayloadInfo(fakeExe, read);

    LARGE_INTEGER size{};
    const HANDLE h = CreateFileW(fakeExe.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { GetFileSizeEx(h, &size); CloseHandle(h); }

    const bool ok = appended && readOk
        && read.offset == info.offset && read.size == info.size
        && info.size > 0
        && info.hash != 0 && read.hash == info.hash
        && read.offset + read.size + 32 == static_cast<uint64_t>(size.QuadPart);

    DeleteFileW(zipPath.c_str());
    DeleteFileW(fakeExe.c_str());
    Emit(L"payload_append_read_roundtrip", ok,
        ok ? L"" : (L"appended=" + std::to_wstring(appended ? 1 : 0)
            + L" read=" + std::to_wstring(readOk ? 1 : 0)
            + L" size=" + std::to_wstring(read.size)).c_str());
}

// ── ⑭b 普通文件不得被当成有 payload ────────────────────────────
void CasePayloadRejectPlainFile() {
    const std::wstring root = MakeTestDir();
    const std::wstring plain = root + L"\\__qst_plain.bin";
    // 故意让末尾出现 QSTPKG01 的**前缀**，验证 magic 是全 8 字节比对
    WriteUtf8File(plain, "hello world QSTPKG0");
    scriptpkg::PayloadInfo info;
    const bool got = scriptpkg::ReadPayloadInfo(plain, info);
    DeleteFileW(plain.c_str());
    Emit(L"payload_reject_plain_file", !got,
        got ? L"plain file was misdetected as having a payload" : L"");
}

// ── ⑭c dump 出来的字节与原始 zip 一致 ──────────────────────────
void CasePayloadDumpBytesEqual() {
    const std::wstring root = MakeTestDir();
    const std::wstring fakeExe = root + L"\\__qst_payload_dump.exe";
    const std::wstring zipPath = root + L"\\__qst_payload_dump.zip";
    const std::wstring outZip = root + L"\\__qst_payload_out.zip";
    WriteUtf8File(fakeExe, "MZ");
    const auto zr = CreateZipFile(zipPath, {{L"script.json", fakeExe}}, fakeExe);
    if (!zr.success) {
        Emit(L"payload_dump_bytes_equal", false, L"zip build failed");
        return;
    }
    scriptpkg::PayloadInfo info;
    const bool appended = scriptpkg::AppendPayloadToExe(fakeExe, zipPath, &info);
    const bool dumped = appended && scriptpkg::ExtractEmbeddedPayload(fakeExe, outZip);

    std::ifstream a(zipPath, std::ios::binary), b(outZip, std::ios::binary);
    const std::string da((std::istreambuf_iterator<char>(a)), std::istreambuf_iterator<char>());
    const std::string db((std::istreambuf_iterator<char>(b)), std::istreambuf_iterator<char>());
    const bool ok = dumped && !da.empty() && da == db;

    DeleteFileW(outZip.c_str());
    DeleteFileW(zipPath.c_str());
    DeleteFileW(fakeExe.c_str());
    Emit(L"payload_dump_bytes_equal", ok,
        ok ? L"" : (L"dumped=" + std::to_wstring(dumped ? 1 : 0)
            + L" a=" + std::to_wstring(da.size()) + L" b=" + std::to_wstring(db.size())).c_str());
}

// ── ⑬ 改写后的裸文件名在目标机器上解析得到 ─────────────────────
void CasePortableRefResolves() {
    EnsureScriptsDir();
    const std::wstring childPath = ScriptsDir() + L"\\__qst_pkg_portable_child.json";
    WriteUtf8File(childPath, "{\"scriptName\":\"portablechild\",\"actions\":[]}");

    const std::wstring ref = L"__qst_pkg_portable_child.json";
    std::wstring resolved;
    const bool ok = ResolveLibraryScriptPath(ref, resolved)
        && LibraryPathsEqual(resolved, childPath);

    DeleteFileW(childPath.c_str());
    Emit(L"portable_ref_resolves_on_target", ok,
        ok ? L"" : (L"resolved=" + resolved).c_str());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--json") == 0) {
            selftest::gJson = true;
            selftest::InitUtf8Stdout();
        } else if (_wcsicmp(argv[i], L"--list") == 0) {
            listOnly = true;
            selftest::InitUtf8Stdout();
        } else if (_wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"-h") == 0) {
            std::fwprintf(stderr, L"  ScriptPackageSelfTest.exe [--json] [--list] [--help]\n");
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"ScriptPackageSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }
    if (!selftest::gJson) {
        std::fwprintf(stderr, L"=== ScriptPackageSelfTest ===\n");
    }

    CaseCollectOnlyNestedTypes();
    CaseCollectKeepsBothFields();
    CaseRewriteAllOccurrences();
    CaseRewriteSkipsNonNested();
    CaseRewriteCaseInsensitive();
    CaseSanitizeEntryFileName();
    CasePlanIncludesNested();
    CasePlanCycleSafe();
    CasePlanMultilevelRecursion();
    CasePlanRewritesRefsInsideChildren();
    CasePlanBlockNameOnlyRef();
    CasePlanUniqueNamesOnCollision();
    CasePlanCollectsNestedImages();
    CasePlanReportsMissingRef();
    CaseZipSubdirRoundtrip();
    CaseZipEntryDataForgingEocd();
    CaseZipNonAsciiEntryName();
    CaseManifestRoundtrip();
    CaseManifestPlayerSection();
    CaseScanSelfContained();
    CaseScanDependencyMatrix();
    CaseScanOcrRegionByImageNeedsOpenCv();
    CaseScanImageOcrAi();
    CaseScanIncludesNested();
    CasePayloadRoundtrip();
    CasePayloadRejectPlainFile();
    CasePayloadDumpBytesEqual();
    CasePortableRefResolves();

    RemoveDirIfEmpty(TestRoot());
    selftest::EmitSummary();
    return selftest::ExitCode();
}
