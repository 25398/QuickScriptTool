#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#include <windows.h>
#include <string>

namespace {

constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
    L"{A8F3C2E1-9B4D-4A7E-8C1F-QuickScriptTool01}_is1";

std::wstring DirOfModule() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring value(path);
    const auto slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : value.substr(0, slash);
}

bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool LaunchElevated(const std::wstring& exe, const std::wstring& args, const std::wstring& dir) {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.empty() ? nullptr : args.c_str();
    sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return false;
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
}

std::wstring FindInnoUninstaller(const std::wstring& dir) {
    const std::wstring preferred = dir + L"\\unins000.exe";
    if (GetFileAttributesW(preferred.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return preferred;
    }
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\unins*.exe").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return {};
    std::wstring hit;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring name = fd.cFileName;
        if (EqualsIgnoreCase(name, L"Uninstall.exe")) continue;
        hit = dir + L"\\" + name;
        break;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return hit;
}

std::wstring QueryUninstallString() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, KEY_READ | KEY_WOW64_64KEY, &key)
        != ERROR_SUCCESS) {
        return {};
    }
    wchar_t buf[MAX_PATH * 2]{};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    const LONG q = RegQueryValueExW(key, L"UninstallString", nullptr, &type,
        reinterpret_cast<BYTE*>(buf), &size);
    RegCloseKey(key);
    if (q != ERROR_SUCCESS || type != REG_SZ) return {};
    return buf;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const std::wstring dir = DirOfModule();
    std::wstring unins = FindInnoUninstaller(dir);
    std::wstring args;

    if (unins.empty()) {
        const std::wstring fromReg = QueryUninstallString();
        if (!fromReg.empty()) {
            std::wstring cmd = fromReg;
            if (!cmd.empty() && cmd.front() == L'"') {
                const auto end = cmd.find(L'"', 1);
                if (end != std::wstring::npos) {
                    unins = cmd.substr(1, end - 1);
                    args = cmd.substr(end + 1);
                    while (!args.empty() && (args.front() == L' ' || args.front() == L'\t')) {
                        args.erase(args.begin());
                    }
                }
            } else {
                const auto sp = cmd.find(L' ');
                if (sp == std::wstring::npos) unins = cmd;
                else {
                    unins = cmd.substr(0, sp);
                    args = cmd.substr(sp + 1);
                }
            }
        }
    }

    if (unins.empty() || GetFileAttributesW(unins.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(nullptr,
            L"未检测到键鼠工坊的安装记录。\n\n"
            L"若这是安装版：请到「设置 → 应用 → 已安装的应用」中卸载。\n"
            L"若这是便携版：请直接删除本文件夹，无需运行卸载程序。",
            L"卸载 键鼠工坊",
            MB_OK | MB_ICONINFORMATION);
        return 1;
    }

    if (!LaunchElevated(unins, args, dir)) {
        const DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) return 0;
        MessageBoxW(nullptr,
            L"无法启动卸载程序。请以管理员身份重试，或到系统「应用和功能」中卸载键鼠工坊。",
            L"卸载 键鼠工坊",
            MB_OK | MB_ICONERROR);
        return 1;
    }
    return 0;
}
