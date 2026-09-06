// =============================================================================
// VirtualHidSelfTest — QstVHid 注入自检 + 安装脚本安全门禁
// =============================================================================
// 总索引：.cursor/skills/module-selftest/SKILL.md
//   MSBuild ... /t:VirtualHidSelfTest
//   build\Release\VirtualHidSelfTest.exe --json
// 安装脚本门禁不依赖本机已装驱动；注入用例仍要求 QstVHid 可用。
// =============================================================================
#include "selftest_harness.h"

#include "input/virtual_hid.h"

#include <windows.h>

#include <fstream>
#include <iterator>
#include <string>

namespace {

using selftest::Emit;

const selftest::CaseInfo kCases[] = {
    {L"install_script_present", L"install", L"Find _elevate_install.ps1 next to repo/dist"},
    {L"install_script_no_boot_policy", L"install", L"Installer must not flip BCD / HVCI / firmware reboot"},
    {L"install_script_no_root_certs", L"install", L"Installer must not import fake Root CAs"},
    {L"install_script_no_driverstore_rm", L"install", L"Installer must not wipe DriverStore folders"},
    {L"install_script_filter_after_start", L"install", L"Class LowerFilters only after Interception is Running"},
    {L"product_no_firmware_reboot", L"install", L"Web shell must not shutdown /r /fw"},
    {L"product_no_official_ic_exe_msg", L"install", L"Must not tell users to run install-interception.exe"},
    {L"release_pack_no_lab_installers", L"install", L"package_release must not ship official IC exe or lab sign scripts"},
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

std::wstring DirName(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    if (s == std::wstring::npos) return L".";
    return p.substr(0, s);
}

std::wstring JoinPath(const std::wstring& a, const wchar_t* b) {
    if (a.empty()) return b;
    const wchar_t last = a.back();
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring FindElevateScript() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = DirName(exe);
    const std::wstring cands[] = {
        JoinPath(dir, L"driver\\qst_vhid\\_elevate_install.ps1"),
        JoinPath(DirName(dir), L"driver\\qst_vhid\\_elevate_install.ps1"),
        JoinPath(DirName(DirName(dir)), L"driver\\qst_vhid\\_elevate_install.ps1"),
    };
    for (const auto& c : cands) {
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) return c;
    }
    return {};
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool ReadAllBytes(const std::wstring& path, std::string* out) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (out->size() >= 3 &&
        static_cast<unsigned char>((*out)[0]) == 0xEF &&
        static_cast<unsigned char>((*out)[1]) == 0xBB &&
        static_cast<unsigned char>((*out)[2]) == 0xBF) {
        out->erase(0, 3);
    }
    return true;
}

bool Contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

bool ContainsBareRegisterTask(const std::string& lower) {
    const char* needle = "register-scheduledtask";
    const size_t nlen = 22;  // strlen("register-scheduledtask")
    for (size_t i = 0; i + nlen <= lower.size(); ++i) {
        if (lower.compare(i, nlen, needle) != 0) continue;
        if (i >= 2 && lower.compare(i - 2, 2, "un") == 0) continue;
        return true;
    }
    return false;
}

void RunInstallScriptGates() {
    const std::wstring path = FindElevateScript();
    const bool present = !path.empty();
    Emit(L"install_script_present", present,
        present ? path.c_str() : L"missing _elevate_install.ps1");
    if (!present) {
        Emit(L"install_script_no_boot_policy", false, L"script missing");
        Emit(L"install_script_no_root_certs", false, L"script missing");
        Emit(L"install_script_no_driverstore_rm", false, L"script missing");
        Emit(L"install_script_filter_after_start", false, L"script missing");
        return;
    }

    std::string raw;
    if (!ReadAllBytes(path, &raw)) {
        Emit(L"install_script_no_boot_policy", false, L"unreadable");
        Emit(L"install_script_no_root_certs", false, L"unreadable");
        Emit(L"install_script_no_driverstore_rm", false, L"unreadable");
        Emit(L"install_script_filter_after_start", false, L"unreadable");
        return;
    }
    const std::string lower = ToLowerAscii(raw);

    std::wstring bootDetail;
    bool bootOk = true;
    const char* bootBans[] = {
        "bcdedit /set",
        "testsigning on",
        "hypervisorenforcedcodeintegrity",
        "shutdown",
        "new-scheduledtask",
        "/fw",
    };
    for (const char* ban : bootBans) {
        if (Contains(lower, ban)) {
            bootOk = false;
            bootDetail += L"banned:";
            while (*ban) {
                bootDetail.push_back(static_cast<wchar_t>(*ban++));
            }
            bootDetail.push_back(L' ');
        }
    }
    if (ContainsBareRegisterTask(lower)) {
        bootOk = false;
        bootDetail += L"banned:Register-ScheduledTask ";
    }
    Emit(L"install_script_no_boot_policy", bootOk, bootDetail.c_str());

    const bool certOk = !Contains(lower, "import_certs.reg") && !Contains(lower, "certutil");
    Emit(L"install_script_no_root_certs", certOk,
        certOk ? L"" : L"import_certs.reg or certutil still referenced");

    const bool storeOk = !Contains(lower, "filerepository");
    Emit(L"install_script_no_driverstore_rm", storeOk,
        storeOk ? L"" : L"FileRepository still referenced");

    const size_t marker = raw.find("FILTERS_ONLY_AFTER_RUNNING");
    const size_t refused = raw.find("REFUSED_FILTERS");
    const size_t addFn = raw.find("function Add-InterceptionClassFilters");
    const size_t addCall = raw.rfind("Add-InterceptionClassFilters");
    const bool orderOk = marker != std::string::npos && refused != std::string::npos &&
        addFn != std::string::npos && addCall != std::string::npos &&
        marker < addCall && refused < addCall && addFn < addCall &&
        Contains(raw, "Repair-LeftoverBootChanges");
    Emit(L"install_script_filter_after_start", orderOk,
        orderOk ? L"" : L"FILTERS_ONLY_AFTER_RUNNING / REFUSED_FILTERS missing or after Add-");
}

std::wstring FindRepoRoot() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = DirName(exe);
    for (int i = 0; i < 6; ++i) {
        if (GetFileAttributesW(JoinPath(dir, L"CMakeLists.txt").c_str()) != INVALID_FILE_ATTRIBUTES) {
            return dir;
        }
        const std::wstring parent = DirName(dir);
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

void EmitFileMustNotContain(const wchar_t* testName, const std::wstring& path,
    const char* needle, const wchar_t* skipDetail) {
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Emit(testName, true, skipDetail);
        return;
    }
    std::string raw;
    if (!ReadAllBytes(path, &raw)) {
        Emit(testName, false, L"unreadable");
        return;
    }
    const std::string lower = ToLowerAscii(raw);
    const bool ok = !Contains(lower, needle);
    Emit(testName, ok, ok ? L"" : L"banned token still present");
}

void RunProductBootSafetyGates() {
    const std::wstring root = FindRepoRoot();
    const std::wstring skip = L"src tree not found (dist-only run)";
    EmitFileMustNotContain(L"product_no_firmware_reboot",
        root.empty() ? L"" : JoinPath(root, L"src\\webview\\qst_webview_shell.cpp"),
        "/r /fw", skip.c_str());
    EmitFileMustNotContain(L"product_no_official_ic_exe_msg",
        root.empty() ? L"" : JoinPath(root, L"src\\input\\hid_interception.cpp"),
        "install-interception.exe", skip.c_str());

    if (root.empty()) {
        Emit(L"release_pack_no_lab_installers", true, skip.c_str());
        return;
    }
    const std::wstring pack = JoinPath(root, L"tools\\package_release.ps1");
    std::string raw;
    if (!ReadAllBytes(pack, &raw)) {
        Emit(L"release_pack_no_lab_installers", false, L"package_release.ps1 unreadable");
        return;
    }
    const std::string lower = ToLowerAscii(raw);
    const bool copiesIcExe = Contains(lower, "copy-item $interceptioninstaller")
        || Contains(lower, "fatal: install-interception.exe missing");
    const bool forbidsLab = Contains(lower, "sign_and_install.ps1 must not ship")
        && Contains(lower, "install-interception.exe must not ship");
    const bool ok = !copiesIcExe && forbidsLab;
    Emit(L"release_pack_no_lab_installers", ok,
        ok ? L"" : L"package_release still ships or requires lab/official kernel installers");
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

    RunInstallScriptGates();
    RunProductBootSafetyGates();

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
