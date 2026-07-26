// =============================================================================
// VirtualHidSelfTest — QstVHid 注入自检
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:VirtualHidSelfTest
//   build\Release\VirtualHidSelfTest.exe --json
// 要求：本机已安装 QstVHid（设置「安装虚拟 HID」或 _elevate_install.ps1）
// =============================================================================
#include "selftest_harness.h"

#include "input/virtual_hid.h"

#include <windows.h>

#include <string>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"device_open", L"driver", L"Open QST Virtual HID device interface"},
    {L"key_press_release", L"inject", L"SendKey Esc down/up via boot keyboard report"},
    {L"key_caps_and_f1", L"inject", L"CapsLock(0x3A) and F1(0x3B) map/submit"},
    {L"key_numlock_non_ext", L"inject", L"NumLock scan 0x45 without E0 prefix"},
    {L"key_slot_compact", L"inject", L"Release middle key then press another (packed slots)"},
    {L"mouse_rel_move", L"inject", L"MoveRelative small dx/dy"},
    {L"mouse_abs_jump", L"inject", L"MoveAbsoluteScreen updates state (no abs HID submit)"},
    {L"mouse_wheel", L"inject", L"Wheel vertical notch + clear pulse"},
    {L"release_all_no_stick", L"inject", L"ReleaseAll clears keys/buttons"},
};

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
            std::fwprintf(stderr,
                L"  VirtualHidSelfTest.exe [--json] [--list] [--help]\n");
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"VirtualHidSelfTest", kCases,
            sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }
    if (!selftest::gJson) {
        std::fwprintf(stderr, L"=== VirtualHidSelfTest ===\n");
    }

    auto& vh = VirtualHidBackend::Instance();
    std::wstring err;
    const bool opened = vh.Open(&err);
    Emit(L"device_open", opened, opened ? L"" : err.c_str());
    if (!opened) {
        selftest::EmitSummary();
        return selftest::ExitCode();
    }

    bool keyOk = vh.SendKey(0x01, true, false) && vh.SendKey(0x01, false, false);
    Emit(L"key_press_release", keyOk, keyOk ? L"" : vh.LastError().c_str());

    bool capsF1 = vh.SendKey(0x3A, true, false) && vh.SendKey(0x3A, false, false)
        && vh.SendKey(0x3B, true, false) && vh.SendKey(0x3B, false, false);
    Emit(L"key_caps_and_f1", capsF1, capsF1 ? L"" : vh.LastError().c_str());

    bool numLock = vh.SendKey(0x45, true, false) && vh.SendKey(0x45, false, false);
    Emit(L"key_numlock_non_ext", numLock, numLock ? L"" : vh.LastError().c_str());

    // A(0x1E), S(0x1F), D(0x20) → release S → F(0x21)；压缩槽后应仍可按下
    bool compact = vh.SendKey(0x1E, true, false) && vh.SendKey(0x1F, true, false)
        && vh.SendKey(0x20, true, false) && vh.SendKey(0x1F, false, false)
        && vh.SendKey(0x21, true, false) && vh.SendKey(0x1E, false, false)
        && vh.SendKey(0x20, false, false) && vh.SendKey(0x21, false, false);
    Emit(L"key_slot_compact", compact, compact ? L"" : vh.LastError().c_str());

    bool relOk = vh.MoveRelative(3, -2);
    Emit(L"mouse_rel_move", relOk, relOk ? L"" : vh.LastError().c_str());

    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vhScreen = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int cx = vx + vw / 2;
    const int cy = vy + vhScreen / 2;
    bool absOk = vh.MoveAbsoluteScreen(cx, cy);
    Emit(L"mouse_abs_jump", absOk, absOk ? L"" : vh.LastError().c_str());

    bool wheelOk = vh.Wheel(120, false);
    Emit(L"mouse_wheel", wheelOk, wheelOk ? L"" : vh.LastError().c_str());

    bool relAll = vh.ReleaseAll();
    Emit(L"release_all_no_stick", relAll, relAll ? L"" : vh.LastError().c_str());

    vh.Close();
    selftest::EmitSummary();
    return selftest::ExitCode();
}
