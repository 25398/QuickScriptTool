#include "process_utils.h"

#include "utils.h"

#include <exdisp.h>
#include <objbase.h>
#include <shlobj.h>
#include <shlguid.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

namespace {

std::wstring ToLowerCopy(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

std::wstring FileNameFromPath(const std::wstring& path) {
    const auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring ParentDirectoryLower(const std::wstring& pathLower) {
    if (pathLower.empty()) return L"";
    std::wstring trimmed = pathLower;
    while (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'/')) trimmed.pop_back();
    const auto pos = trimmed.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L"";
    return trimmed.substr(0, pos + 1);
}

std::wstring QueryProcessPath(DWORD pid) {
    if (pid == 0) return L"";
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return L"";
    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    if (!QueryFullProcessImageNameW(process, 0, path, &size)) {
        CloseHandle(process);
        return L"";
    }
    CloseHandle(process);
    return path;
}

std::wstring QueryProcessCommandLine(DWORD pid) {
    if (pid == 0) return L"";
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) return L"";

    using NtQueryInformationProcessFn = LONG (WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    auto NtQueryInformationProcess = reinterpret_cast<NtQueryInformationProcessFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    if (!NtQueryInformationProcess) {
        CloseHandle(process);
        return L"";
    }

    struct PROCESS_BASIC_INFORMATION {
        PVOID Reserved1;
        PVOID PebBaseAddress;
        PVOID Reserved2[2];
        ULONG_PTR UniqueProcessId;
        PVOID Reserved3;
    } pbi{};
    ULONG retLen = 0;
    if (NtQueryInformationProcess(process, 0 /*ProcessBasicInformation*/, &pbi, sizeof(pbi), &retLen) != 0
        || !pbi.PebBaseAddress) {
        CloseHandle(process);
        return L"";
    }

    PVOID processParams = nullptr;
#if defined(_WIN64)
    const ULONG_PTR processParametersOffset = 0x20;
#else
    const ULONG_PTR processParametersOffset = 0x10;
#endif
    if (!ReadProcessMemory(process,
            reinterpret_cast<BYTE*>(pbi.PebBaseAddress) + processParametersOffset,
            &processParams, sizeof(processParams), nullptr) || !processParams) {
        CloseHandle(process);
        return L"";
    }

    struct UNICODE_STRING_REMOTE {
        USHORT Length;
        USHORT MaximumLength;
        PVOID Buffer;
    } cmd{};
#if defined(_WIN64)
    const ULONG_PTR commandLineOffset = 0x70;
#else
    const ULONG_PTR commandLineOffset = 0x40;
#endif
    if (!ReadProcessMemory(process,
            reinterpret_cast<BYTE*>(processParams) + commandLineOffset,
            &cmd, sizeof(cmd), nullptr) || !cmd.Buffer || cmd.Length == 0) {
        CloseHandle(process);
        return L"";
    }

    std::wstring result(cmd.Length / sizeof(wchar_t), L'\0');
    if (!ReadProcessMemory(process, cmd.Buffer, result.data(), cmd.Length, nullptr)) {
        CloseHandle(process);
        return L"";
    }
    CloseHandle(process);
    return result;
}

std::wstring ExtractExistingFileFromCommandLine(const std::wstring& cmdLine) {
    if (cmdLine.empty()) return L"";
    std::vector<std::wstring> tokens;
    for (size_t i = 0; i < cmdLine.size();) {
        while (i < cmdLine.size() && iswspace(cmdLine[i])) ++i;
        if (i >= cmdLine.size()) break;
        std::wstring tok;
        if (cmdLine[i] == L'"') {
            ++i;
            while (i < cmdLine.size() && cmdLine[i] != L'"') tok.push_back(cmdLine[i++]);
            if (i < cmdLine.size()) ++i;
        } else {
            while (i < cmdLine.size() && !iswspace(cmdLine[i])) tok.push_back(cmdLine[i++]);
        }
        if (!tok.empty()) tokens.push_back(tok);
    }
    // 跳过 argv0（进程本身），取后续存在的文件路径。
    for (size_t i = 1; i < tokens.size(); ++i) {
        const std::wstring& t = tokens[i];
        if (t.size() < 2) continue;
        if ((t[1] == L':' || t.find(L'\\') != std::wstring::npos || t.find(L'/') != std::wstring::npos)
            && GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES
            && (GetFileAttributesW(t.c_str()) & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return t;
        }
    }
    return L"";
}

HWND WindowFromScreenPoint(int x, int y) {
    POINT pt{x, y};
    HWND hwnd = WindowFromPoint(pt);
    if (!hwnd) return nullptr;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    return root ? root : hwnd;
}

bool IsProtectedProcessName(const std::wstring& processNameLower) {
    static const wchar_t* kProtected[] = {
        L"system",
        L"registry",
        L"smss.exe",
        L"csrss.exe",
        L"wininit.exe",
        L"services.exe",
        L"lsass.exe",
        L"winlogon.exe",
        L"svchost.exe",
        L"dwm.exe",
        L"explorer.exe",
        L"sihost.exe",
        L"fontdrvhost.exe",
        L"taskhostw.exe",
        L"runtimebroker.exe",
        L"searchhost.exe",
        L"startmenuexperiencehost.exe",
        L"shellexperiencehost.exe",
        L"textinputhost.exe",
        L"ctfmon.exe",
        L"conhost.exe",
        L"audiodg.exe",
        L"spoolsv.exe",
        L"lsm.exe",
        L"wmiprvse.exe",
        L"searchindexer.exe",
        L"securityhealthservice.exe",
        L"quickscripttool.exe",
    };
    for (const wchar_t* name : kProtected) {
        if (processNameLower == name) return true;
    }
    return false;
}

bool IsProtectedProcess(DWORD pid, const std::wstring& processNameLower) {
    if (pid == 0 || pid == 4) return true;
    if (pid == GetCurrentProcessId()) return true;
    if (IsProtectedProcessName(processNameLower)) return true;
    return false;
}

bool IsProcessRunning(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    DWORD exitCode = STILL_ACTIVE;
    const BOOL ok = GetExitCodeProcess(process, &exitCode);
    CloseHandle(process);
    return ok && exitCode == STILL_ACTIVE;
}

bool IsControlPanelTarget(const std::wstring& targetLower, const std::wstring& targetFileLower) {
    return targetFileLower == L"control.exe" || targetLower == L"control.exe";
}

bool IsExplorerTarget(const std::wstring& targetFileLower) {
    return targetFileLower == L"explorer.exe";
}

bool IsSystemSettingsTarget(const std::wstring& targetFileLower) {
    return targetFileLower == L"systemsettings.exe";
}

bool TitleLooksLikeControlPanel(const std::wstring& titleLower) {
    if (titleLower.empty()) return false;
    return titleLower.find(L"控制面板") != std::wstring::npos
        || titleLower.find(L"control panel") != std::wstring::npos;
}

bool UrlLooksLikeControlPanel(const std::wstring& urlLower) {
    if (urlLower.empty()) return false;
    return urlLower.find(L"26ee0668-a00a-44d7-9371-beb64c20b05f") != std::wstring::npos
        || urlLower.find(L"21ec2020-3aea-1069-a2dd-08002b30309d") != std::wstring::npos
        || urlLower.find(L"control panel") != std::wstring::npos
        || urlLower.find(L"控制面板") != std::wstring::npos;
}

bool TryCloseWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (PostMessageW(hwnd, WM_CLOSE, 0, 0)) return true;

    DWORD_PTR result = 0;
    SendMessageTimeoutW(hwnd, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 2000, &result);
    PostMessageW(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
    return true;
}

bool CloseShellBrowserWindow(IWebBrowserApp* browser) {
    if (!browser) return false;

    SHANDLE_PTR hwndPtr = 0;
    if (SUCCEEDED(browser->get_HWND(&hwndPtr)) && hwndPtr) {
        if (TryCloseWindow(reinterpret_cast<HWND>(hwndPtr))) return true;
    }

    return SUCCEEDED(browser->Quit());
}

bool ShellBrowserLooksLikeControlPanel(IWebBrowserApp* browser) {
    if (!browser) return false;

    BSTR url = nullptr;
    BSTR name = nullptr;
    browser->get_LocationURL(&url);
    browser->get_LocationName(&name);

    bool match = false;
    if (url) {
        match = UrlLooksLikeControlPanel(ToLowerCopy(url));
        SysFreeString(url);
    }
    if (!match && name) {
        match = TitleLooksLikeControlPanel(ToLowerCopy(name));
        SysFreeString(name);
    }
    return match;
}

struct BoolContext { bool value = false; };

BOOL CALLBACK CloseControlPanelWindowsProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<BoolContext*>(lp);
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return TRUE;

    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, 512);
    if (!TitleLooksLikeControlPanel(ToLowerCopy(title))) return TRUE;

    if (TryCloseWindow(hwnd)) ctx->value = true;
    return TRUE;
}

BOOL CALLBACK CloseCabinetControlPanelProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<BoolContext*>(lp);
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return TRUE;

    wchar_t cls[256]{};
    GetClassNameW(hwnd, cls, 256);
    if (wcscmp(cls, L"CabinetWClass") != 0 && wcscmp(cls, L"ExploreWClass") != 0) return TRUE;

    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, 512);
    if (!TitleLooksLikeControlPanel(ToLowerCopy(title))) return TRUE;

    if (TryCloseWindow(hwnd)) ctx->value = true;
    return TRUE;
}

bool CloseControlPanelViaShellWindows() {
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = SUCCEEDED(hrInit) && hrInit != RPC_E_CHANGED_MODE && hrInit != S_FALSE;

    IShellWindows* shellWindows = nullptr;
    const HRESULT hr = CoCreateInstance(
        CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_IShellWindows, reinterpret_cast<void**>(&shellWindows));
    if (FAILED(hr) || !shellWindows) {
        if (needUninit) CoUninitialize();
        return false;
    }

    long count = 0;
    shellWindows->get_Count(&count);

    bool closedAny = false;
    auto tryIndex = [&](long index) {
        VARIANT variant{};
        VariantInit(&variant);
        variant.vt = VT_I4;
        variant.lVal = index;

        IDispatch* dispatch = nullptr;
        if (FAILED(shellWindows->Item(variant, &dispatch)) || !dispatch) return;

        IWebBrowserApp* browser = nullptr;
        if (SUCCEEDED(dispatch->QueryInterface(IID_IWebBrowserApp, reinterpret_cast<void**>(&browser))) && browser) {
            if (ShellBrowserLooksLikeControlPanel(browser)) {
                if (CloseShellBrowserWindow(browser)) closedAny = true;
            }
            browser->Release();
        }
        dispatch->Release();
    };

    for (long i = 0; i < count; ++i) tryIndex(i);
    if (!closedAny) {
        for (long i = 1; i <= count; ++i) tryIndex(i);
    }

    shellWindows->Release();
    if (needUninit) CoUninitialize();
    return closedAny;
}

bool CloseControlPanelCabinetWindows() {
    BoolContext ctx{};
    for (const wchar_t* cls : {L"CabinetWClass", L"ExploreWClass"}) {
        HWND hwnd = nullptr;
        while ((hwnd = FindWindowExW(nullptr, hwnd, cls, nullptr)) != nullptr) {
            if (!IsWindowVisible(hwnd)) continue;

            wchar_t title[512]{};
            GetWindowTextW(hwnd, title, 512);
            if (!TitleLooksLikeControlPanel(ToLowerCopy(title))) continue;

            if (TryCloseWindow(hwnd)) ctx.value = true;
        }
    }
    return ctx.value;
}

bool CloseControlPanelWindows() {
    bool closedAny = CloseControlPanelViaShellWindows();
    if (CloseControlPanelCabinetWindows()) closedAny = true;

    BoolContext ctx{};
    EnumWindows(CloseControlPanelWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    EnumWindows(CloseCabinetControlPanelProc, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.value) closedAny = true;

    static const wchar_t* kExactTitles[] = {
        L"控制面板",
        L"Control Panel",
    };
    for (const wchar_t* title : kExactTitles) {
        HWND hwnd = FindWindowW(nullptr, title);
        if (hwnd && IsWindowVisible(hwnd) && TryCloseWindow(hwnd)) closedAny = true;
    }

    return closedAny;
}

bool ShouldTryCloseControlPanelWindows(const std::wstring& targetLower,
    const std::wstring& targetFileLower) {
    return IsControlPanelTarget(targetLower, targetFileLower)
        || IsExplorerTarget(targetFileLower)
        || IsSystemSettingsTarget(targetFileLower);
}

bool ProcessMatchesTarget(const std::wstring& processNameLower,
    const std::wstring& processPathLower,
    const std::wstring& targetLower,
    const std::wstring& targetFileLower,
    const std::wstring& targetDirLower,
    bool matchFileNameOnly) {
    if (matchFileNameOnly) {
        return processNameLower == targetFileLower
            || processNameLower == targetLower
            || FileNameFromPath(processPathLower) == targetFileLower;
    }
    if (processPathLower.empty()) return false;
    if (processPathLower == targetLower) return true;

    auto inDirectory = [&](const std::wstring& dirLower) {
        return !dirLower.empty()
            && dirLower.size() > 4
            && processPathLower.rfind(dirLower, 0) == 0;
    };

    if (inDirectory(targetDirLower)) return true;

    if (!targetDirLower.empty()) {
        std::wstring parentDir = targetDirLower;
        if (!parentDir.empty() && (parentDir.back() == L'\\' || parentDir.back() == L'/')) parentDir.pop_back();
        parentDir = ParentDirectoryLower(parentDir);
        if (inDirectory(parentDir)) return true;
    }

    return FileNameFromPath(processPathLower) == targetFileLower && !targetFileLower.empty();
}

struct CloseWindowsContext {
    DWORD pid = 0;
    bool closedAny = false;
};

BOOL CALLBACK CloseAllProcessWindowsProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<CloseWindowsContext*>(lp);
    if (!hwnd || !IsWindow(hwnd)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid) return TRUE;

    if (PostMessageW(hwnd, WM_CLOSE, 0, 0)) ctx->closedAny = true;
    return TRUE;
}

bool CloseAllProcessWindows(DWORD pid) {
    CloseWindowsContext ctx{};
    ctx.pid = pid;
    EnumWindows(CloseAllProcessWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.closedAny;
}

bool TerminateUserProcess(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!process) return false;
    const BOOL ok = TerminateProcess(process, 0);
    CloseHandle(process);
    return ok == TRUE;
}

std::wstring ProcessNameByPid(DWORD pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return L"";

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::wstring name;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == pid) {
                name = entry.szExeFile;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return name;
}

std::vector<DWORD> CollectMatchingPids(const std::wstring& targetLower,
    const std::wstring& targetFileLower,
    const std::wstring& targetDirLower,
    bool matchFileNameOnly) {
    std::vector<DWORD> matched;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return matched;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const std::wstring processName = ToLowerCopy(entry.szExeFile);
            if (IsProtectedProcess(entry.th32ProcessID, processName)) continue;

            const std::wstring processPath = ToLowerCopy(QueryProcessPath(entry.th32ProcessID));
            if (!ProcessMatchesTarget(processName, processPath, targetLower, targetFileLower,
                    targetDirLower, matchFileNameOnly)) {
                continue;
            }
            matched.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return matched;
}

} // namespace

std::wstring GetProcessPathFromPoint(int x, int y) {
    HWND hwnd = WindowFromScreenPoint(x, y);
    if (!hwnd) return L"";
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return QueryProcessPath(pid);
}

WindowInfoFromPoint GetWindowInfoFromPoint(int x, int y) {
    POINT pt{x, y};
    HWND pointHwnd = WindowFromPoint(pt);
    HWND rootHwnd = pointHwnd ? GetAncestor(pointHwnd, GA_ROOT) : nullptr;
    if (!rootHwnd && pointHwnd) rootHwnd = pointHwnd;

    WindowInfoFromPoint info{};
    info.x = x;
    info.y = y;
    if (rootHwnd) {
        wchar_t title[512]{};
        GetWindowTextW(rootHwnd, title, 512);
        info.windowTitle = title;
        wchar_t cls[256]{};
        GetClassNameW(rootHwnd, cls, 256);
        info.windowClassName = cls;
        DWORD pid = 0;
        GetWindowThreadProcessId(rootHwnd, &pid);
        info.processPath = QueryProcessPath(pid);
        info.documentPath = ExtractExistingFileFromCommandLine(QueryProcessCommandLine(pid));
        // 标题里的文档名：完整路径校验存在；裸文件名也写入，供「指定窗口类」按脚本目录打开。
        if (info.documentPath.empty() && info.windowTitle.size() > 2) {
            std::wstring docTitle = info.windowTitle;
            const auto dash = docTitle.find(L" - ");
            if (dash != std::wstring::npos) docTitle = docTitle.substr(0, dash);
            while (!docTitle.empty() && (docTitle.front() == L'*' || docTitle.front() == L' ')) {
                docTitle.erase(docTitle.begin());
            }
            while (!docTitle.empty() && (docTitle.back() == L' ' || docTitle.back() == L'\t')) {
                docTitle.pop_back();
            }
            const bool looksFull = (docTitle.size() >= 2 && docTitle[1] == L':')
                || docTitle.find(L'\\') != std::wstring::npos
                || docTitle.find(L'/') != std::wstring::npos;
            if (looksFull) {
                const DWORD attrs = GetFileAttributesW(docTitle.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES
                    && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    info.documentPath = docTitle;
                }
            } else if (docTitle.find(L'.') != std::wstring::npos
                && docTitle != L"无标题"
                && _wcsicmp(docTitle.c_str(), L"Untitled") != 0) {
                info.documentPath = docTitle;
            }
        }
    }
    if (pointHwnd) {
        wchar_t childCls[256]{};
        GetClassNameW(pointHwnd, childCls, 256);
        info.childWindowClassName = childCls;
    }
    return info;
}

bool LaunchProgram(const std::wstring& path, const std::wstring& args) {
    if (path.empty()) return false;
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = path.c_str();
    if (!args.empty()) info.lpParameters = args.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) return false;
    if (info.hProcess) CloseHandle(info.hProcess);
    return true;
}

bool CloseProgramsByTarget(const std::wstring& target, bool matchFileNameOnly) {
    if (target.empty()) return false;

    const std::wstring targetLower = ToLowerCopy(Trim(target));
    const std::wstring targetFileLower = ToLowerCopy(FileNameFromPath(targetLower));

    bool closedAny = false;

    if (ShouldTryCloseControlPanelWindows(targetLower, targetFileLower)) {
        if (CloseControlPanelWindows()) closedAny = true;
    }

    if (IsProtectedProcessName(targetFileLower) || IsProtectedProcessName(targetLower)) {
        return closedAny;
    }

    if (IsControlPanelTarget(targetLower, targetFileLower) && closedAny) {
        return true;
    }

    const std::wstring targetDirLower = ParentDirectoryLower(targetLower);
    std::vector<DWORD> matchedPids = CollectMatchingPids(
        targetLower, targetFileLower, targetDirLower, matchFileNameOnly);

    for (DWORD pid : matchedPids) {
        if (CloseAllProcessWindows(pid)) closedAny = true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(450));

    matchedPids = CollectMatchingPids(targetLower, targetFileLower, targetDirLower, matchFileNameOnly);
    for (DWORD pid : matchedPids) {
        if (!IsProcessRunning(pid)) continue;

        const std::wstring processName = ToLowerCopy(ProcessNameByPid(pid));
        if (IsProtectedProcess(pid, processName)) continue;

        if (TerminateUserProcess(pid)) closedAny = true;
    }

    return closedAny;
}

int TerminateOtherInstancesOfCurrentExe() {
    wchar_t exePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return 0;
    const std::wstring targetLower = ToLowerCopy(exePath);
    const std::wstring targetFileLower = ToLowerCopy(FileNameFromPath(targetLower));
    const std::wstring targetDirLower = ParentDirectoryLower(targetLower);
    const DWORD selfPid = GetCurrentProcessId();

    int terminated = 0;
    for (DWORD pid : CollectMatchingPids(targetLower, targetFileLower, targetDirLower, false)) {
        if (pid == selfPid || !IsProcessRunning(pid)) continue;
        const std::wstring processName = ToLowerCopy(ProcessNameByPid(pid));
        if (IsProtectedProcess(pid, processName)) continue;
        if (TerminateUserProcess(pid)) ++terminated;
    }
    return terminated;
}
