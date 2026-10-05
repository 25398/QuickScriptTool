// =============================================================================
// HotkeyScopeSelfTest — 专属热键「作用域」判据（纯逻辑，无 HWND / 无引擎状态）
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   build\Release\HotkeyScopeSelfTest.exe --json
//
// 回归焦点：**作用域外的专属热键必须被彻底释放**（既不启动，也不吞键）。
//
// 历史 bug（本用例要钉死的行为）：作用域门控只挡了「启动」，RegisterAllHotkeys
// 仍照常调 RegisterHotKey(P) ⇒ 系统直接吞掉该物理键 ⇒ 用户切到别的 TAB 后
// 「按 P 既不起脚本、又打不出 P」。
//
// 现契约（见 src/engine/hotkey_scope.h）：
//   ① 作用域外 ⇒ 不注册 RegisterHotKey（否则系统吞键）
//   ② 作用域外 ⇒ LL 钩子放行（h.inScope=false ⇒ `if (!h.inScope) continue;`）
//   ③ 例外：有会话在跑（keepForStop）⇒ 保留注册，保证同一热键还能停止
// =============================================================================
#include "selftest_harness.h"

#include "engine/hotkey_scope.h"

#include <string>

namespace {

using selftest::Emit;
using qst::hotkey_scope::AllowSystemRegister;
using qst::hotkey_scope::DedicatedInScope;

/// 主页 TAB 的抽象（不引入 UI 头，保持纯逻辑）。
constexpr bool kMacroTab = true;     // 当前页 = 鼠标宏
constexpr bool kRecorderTab = true;  // 当前页 = 键鼠录制

std::wstring YN(bool v) { return v ? L"1" : L"0"; }

const selftest::CaseInfo kCases[] = {
    {L"out_of_scope_never_registers", L"default",
        L"作用域外绝不注册 RegisterHotKey —— 历史 bug 回归：注册了系统会吞键，用户「按 P 打不出 P」"},
    {L"in_scope_registers", L"default",
        L"作用域内必须注册，否则热键没反应"},
    {L"running_session_keeps_registration", L"default",
        L"有会话在跑时作用域外仍保留注册，保证同一热键还能停止"},
    {L"cross_tab_switch_releases_letter", L"default",
        L"宏页设 P → 切到键鼠录制页：P 必须被释放（系统不再吞键）"},
    {L"switch_back_reacquires_letter", L"default",
        L"切回宏页：P 必须重新注册（按 P 能启动）"},
    {L"macro_tab_hides_recording_entries", L"default",
        L"鼠标宏页不认录制条目的热键"},
    {L"recorder_tab_hides_macro_entries", L"default",
        L"键鼠录制页不认脚本条目的热键（对称）"},
    {L"other_tabs_have_no_scope", L"default",
        L"设置 / AI 等页面：两类条目都不在作用域内，一律放行"},
    {L"scope_all_pages_keeps_everything", L"default",
        L"「全部页面生效」打开 ⇒ 任意页面都在作用域内并保持注册"},
    {L"scope_truth_table_16", L"default",
        L"作用域判据 16 组合真值表（逐行手写期望，防重构漂移）"},
    {L"register_truth_table_4", L"default",
        L"注册许可 4 组合真值表（含历史 bug 那一格）"},
    {L"stop_path_ignores_scope", L"default",
        L"停止路径不受作用域限制：运行中切页仍能按热键停"},
};

// -----------------------------------------------------------------------------
// 单点契约
// -----------------------------------------------------------------------------

void CaseOutOfScopeNeverRegisters() {
    // 宏页看录制条目 ⇒ 不在作用域；且当前无会话在跑 ⇒ 绝不允许注册。
    // 旧实现这一格为 true（照常注册）⇒ 系统吞掉该字母 ⇒ 用户打不出 P。
    const bool inScope = DedicatedInScope(false, /*isRecording*/true, kMacroTab, !kRecorderTab);
    const bool allow = AllowSystemRegister(inScope, /*keepForStop*/false);
    Emit(L"out_of_scope_never_registers", !inScope && !allow,
        (L"inScope=" + YN(inScope) + L" allowRegister=" + YN(allow)
            + L" (旧实现 allowRegister=1)").c_str());
}

void CaseInScopeRegisters() {
    const bool inScope = DedicatedInScope(false, /*isRecording*/false, kMacroTab, !kRecorderTab);
    const bool allow = AllowSystemRegister(inScope, /*keepForStop*/false);
    Emit(L"in_scope_registers", inScope && allow,
        (L"inScope=" + YN(inScope) + L" allowRegister=" + YN(allow)).c_str());
}

void CaseRunningSessionKeepsRegistration() {
    // 宏正在跑，用户切到录制页：作用域外，但必须保留注册，否则按 P 停不下来。
    const bool inScope = DedicatedInScope(false, /*isRecording*/false, !kMacroTab, kRecorderTab);
    const bool allow = AllowSystemRegister(inScope, /*keepForStop*/true);
    Emit(L"running_session_keeps_registration", !inScope && allow,
        (L"inScope=" + YN(inScope) + L" keepForStop=1 allowRegister=" + YN(allow)).c_str());
}

void CaseCrossTabSwitchReleasesLetter() {
    // 用户在宏页给脚本设了 P，然后切到键鼠录制页 —— 这正是用户报的场景。
    const bool onMacro = DedicatedInScope(false, false, kMacroTab, !kRecorderTab);
    const bool onRecorder = DedicatedInScope(false, false, !kMacroTab, kRecorderTab);
    const bool allowOnMacro = AllowSystemRegister(onMacro, false);
    const bool allowOnRecorder = AllowSystemRegister(onRecorder, false);
    Emit(L"cross_tab_switch_releases_letter", allowOnMacro && !allowOnRecorder,
        (L"宏页 allowRegister=" + YN(allowOnMacro)
            + L" 录制页 allowRegister=" + YN(allowOnRecorder)
            + L" (切走后必须为 0，P 才能打出来)").c_str());
}

void CaseSwitchBackReacquiresLetter() {
    const bool onMacro = DedicatedInScope(false, false, kMacroTab, !kRecorderTab);
    const bool allow = AllowSystemRegister(onMacro, false);
    Emit(L"switch_back_reacquires_letter", onMacro && allow,
        (L"inScope=" + YN(onMacro) + L" allowRegister=" + YN(allow)).c_str());
}

void CaseMacroTabHidesRecordingEntries() {
    const bool rec = DedicatedInScope(false, /*isRecording*/true, kMacroTab, !kRecorderTab);
    Emit(L"macro_tab_hides_recording_entries", !rec, (L"inScope=" + YN(rec)).c_str());
}

void CaseRecorderTabHidesMacroEntries() {
    const bool macro = DedicatedInScope(false, /*isRecording*/false, !kMacroTab, kRecorderTab);
    Emit(L"recorder_tab_hides_macro_entries", !macro, (L"inScope=" + YN(macro)).c_str());
}

void CaseOtherTabsHaveNoScope() {
    // 既不在宏页也不在录制页（设置 / AI 助手 / 关于…）
    const bool macro = DedicatedInScope(false, false, !kMacroTab, !kRecorderTab);
    const bool rec = DedicatedInScope(false, true, !kMacroTab, !kRecorderTab);
    Emit(L"other_tabs_have_no_scope", !macro && !rec,
        (L"脚本条目 inScope=" + YN(macro) + L" 录制条目 inScope=" + YN(rec)).c_str());
}

void CaseScopeAllPagesKeepsEverything() {
    // 开了「全部页面生效」：任意页面两类条目都在作用域内，且必须保持注册。
    bool ok = true;
    std::wstring detail;
    for (int onMacro = 0; onMacro <= 1; ++onMacro) {
        for (int onRec = 0; onRec <= 1; ++onRec) {
            for (int rec = 0; rec <= 1; ++rec) {
                const bool inScope = DedicatedInScope(true, rec != 0, onMacro != 0, onRec != 0);
                if (!inScope || !AllowSystemRegister(inScope, false)) {
                    ok = false;
                    detail = L"all=1 rec=" + YN(rec != 0) + L" macro=" + YN(onMacro != 0)
                        + L" recorder=" + YN(onRec != 0);
                }
            }
        }
    }
    Emit(L"scope_all_pages_keeps_everything", ok,
        ok ? L"8 组合全部 inScope=1 且允许注册" : detail.c_str());
}

// -----------------------------------------------------------------------------
// 真值表（期望值逐行手写 —— 不复用被测表达式，否则等于自证）
// -----------------------------------------------------------------------------

struct ScopeRow {
    bool all;
    bool isRecording;
    bool onMacroTab;
    bool onRecorderTab;
    bool expectInScope;
};

const ScopeRow kScopeTable[] = {
    // all  rec    macro  recorder  期望：在作用域内？
    {false, false, false, false, false},  // 无关页面看脚本条目
    {false, false, false, true,  false},  // 录制页看脚本条目 ⇒ 不在
    {false, false, true,  false, true },  // 宏页看脚本条目   ⇒ 在
    {false, false, true,  true,  true },  // 两页标记同时为真（异常态，宏页优先）
    {false, true,  false, false, false},  // 无关页面看录制条目
    {false, true,  false, true,  true },  // 录制页看录制条目 ⇒ 在
    {false, true,  true,  false, false},  // 宏页看录制条目   ⇒ 不在
    {false, true,  true,  true,  true },  // 同上，录制页优先
    {true,  false, false, false, true },  // 「全部页面生效」：一律在
    {true,  false, false, true,  true },
    {true,  false, true,  false, true },
    {true,  false, true,  true,  true },
    {true,  true,  false, false, true },
    {true,  true,  false, true,  true },
    {true,  true,  true,  false, true },
    {true,  true,  true,  true,  true },
};

void CaseScopeTruthTable16() {
    const int n = static_cast<int>(sizeof(kScopeTable) / sizeof(kScopeTable[0]));
    int bad = -1;
    for (int i = 0; i < n; ++i) {
        const auto& r = kScopeTable[i];
        if (DedicatedInScope(r.all, r.isRecording, r.onMacroTab, r.onRecorderTab) != r.expectInScope) {
            bad = i;
            break;
        }
    }
    Emit(L"scope_truth_table_16", bad < 0,
        bad < 0 ? L"16/16 行与手写期望一致"
                : (L"第 " + std::to_wstring(bad) + L" 行不符").c_str());
}

struct RegRow {
    bool inScope;
    bool keepForStop;
    bool expectAllow;
};

const RegRow kRegTable[] = {
    {false, false, false},  // ★历史 bug 那一格：旧实现为 true ⇒ 系统吞键
    {false, true,  true },  // 运行中：保留注册以便停止
    {true,  false, true },
    {true,  true,  true },
};

void CaseRegisterTruthTable4() {
    const int n = static_cast<int>(sizeof(kRegTable) / sizeof(kRegTable[0]));
    int bad = -1;
    for (int i = 0; i < n; ++i) {
        const auto& r = kRegTable[i];
        if (AllowSystemRegister(r.inScope, r.keepForStop) != r.expectAllow) {
            bad = i;
            break;
        }
    }
    Emit(L"register_truth_table_4", bad < 0,
        bad < 0 ? L"4/4 行与手写期望一致"
                : (L"第 " + std::to_wstring(bad) + L" 行不符").c_str());
}

void CaseStopPathIgnoresScope() {
    // 停止路径：宏在跑 + 已切到作用域外 ⇒ 注册必须保留（能按热键停），
    // 且这一判断不依赖「当前页是否列出该条目」以外的新条件。
    const bool inScope = DedicatedInScope(false, false, !kMacroTab, kRecorderTab);
    const bool allow = AllowSystemRegister(inScope, /*keepForStop*/true);
    Emit(L"stop_path_ignores_scope", allow,
        (L"inScope=" + YN(inScope) + L" keepForStop=1 allowRegister=" + YN(allow)).c_str());
}

void PrintHelp() {
    std::fwprintf(stdout,
        L"HotkeyScopeSelfTest — 专属热键作用域判据（作用域外必须彻底释放）\n"
        L"\n"
        L"用法:\n"
        L"  HotkeyScopeSelfTest.exe [--json] [--list] [--help]\n"
        L"\n"
        L"Agent: 见 .cursor/skills/module-selftest/SKILL.md\n"
        L"  源码: src/engine/hotkey_scope.h\n"
        L"  调用: src/engine/engine_script_run.cpp (DedicatedHotkeyInScope)\n"
        L"        src/engine/engine_hotkeys.cpp    (RefreshDedicatedHotkeyScope)\n");
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
        selftest::PrintCaseList(L"HotkeyScopeSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    CaseOutOfScopeNeverRegisters();
    CaseInScopeRegisters();
    CaseRunningSessionKeepsRegistration();
    CaseCrossTabSwitchReleasesLetter();
    CaseSwitchBackReacquiresLetter();
    CaseMacroTabHidesRecordingEntries();
    CaseRecorderTabHidesMacroEntries();
    CaseOtherTabsHaveNoScope();
    CaseScopeAllPagesKeepsEverything();
    CaseScopeTruthTable16();
    CaseRegisterTruthTable4();
    CaseStopPathIgnoresScope();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
