// ──────────────────────────────────────────────────────────────────
// player_runtime.cpp — 见 player_runtime.h 的说明
// ──────────────────────────────────────────────────────────────────
#include "player_runtime.h"

#include <windows.h>

#include <cstdio>

namespace playerruntime {
namespace {

constexpr wchar_t kProductRegKey[] = L"Software\\QuickScriptTool";
constexpr wchar_t kAppPathsKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\QuickScriptTool.exe";
constexpr wchar_t kAppExeName[] = L"QuickScriptTool.exe";
constexpr wchar_t kOpenCvDll[] = L"opencv_world4100.dll";

std::wstring JoinPath(const std::wstring& dir, const wchar_t* name) {
    if (dir.empty()) return name ? name : L"";
    std::wstring out = dir;
    if (out.back() != L'\\' && out.back() != L'/') out += L'\\';
    out += name ? name : L"";
    return out;
}

bool FileExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

/// 读注册表字符串值（不写、不创建 —— 产物必须做到"不写注册表"）。
std::wstring ReadRegString(HKEY root, const wchar_t* subKey, const wchar_t* value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS || !key) return L"";
    wchar_t buf[MAX_PATH * 2]{};
    DWORD cb = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    const LONG rc = RegQueryValueExW(key, value, nullptr, &type,
        reinterpret_cast<LPBYTE>(buf), &cb);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return L"";
    buf[(sizeof(buf) / sizeof(wchar_t)) - 1] = L'\0';
    std::wstring out(buf);
    if (type == REG_EXPAND_SZ && out.find(L'%') != std::wstring::npos) {
        wchar_t expanded[MAX_PATH * 2]{};
        if (ExpandEnvironmentStringsW(out.c_str(), expanded, MAX_PATH * 2) > 0) {
            out = expanded;
        }
    }
    while (!out.empty() && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

/// 目录可用性校验：必须真的有 QuickScriptTool.exe，
/// 否则注册表里的过期路径会把"走软件"模式引到空目录。
bool LooksLikeInstallDir(const std::wstring& dir) {
    if (dir.empty() || !DirExists(dir)) return false;
    return FileExists(JoinPath(dir, kAppExeName));
}

}  // namespace

const wchar_t* ComponentSourceName(ComponentSource s) {
    switch (s) {
    case ComponentSource::Bundled:      return L"自带";
    case ComponentSource::InstalledApp: return L"走软件";
    case ComponentSource::SystemOcr:    return L"系统OCR";
    case ComponentSource::Unavailable:  return L"不可用";
    default:                            return L"不需要";
    }
}

std::wstring FindInstalledAppDir() {
    // 1) LastRunDir：主程序每次启动都写，最能反映"用户实际在用的那份"
    {
        const std::wstring dir = ReadRegString(HKEY_CURRENT_USER, kProductRegKey, L"LastRunDir");
        if (LooksLikeInstallDir(dir)) return dir;
    }
    // 2/3) 安装包写入的 InstallPath（HKLM 优先，其次 HKCU）
    for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER }) {
        const std::wstring dir = ReadRegString(root, kProductRegKey, L"InstallPath");
        if (LooksLikeInstallDir(dir)) return dir;
    }
    // 4) App Paths
    {
        const std::wstring dir = ReadRegString(HKEY_LOCAL_MACHINE, kAppPathsKey, L"");
        if (LooksLikeInstallDir(dir)) return dir;
    }
    {
        const std::wstring dir = ReadRegString(HKEY_CURRENT_USER, kAppPathsKey, L"");
        if (LooksLikeInstallDir(dir)) return dir;
    }
    return L"";
}

std::wstring RuntimeDirFor(uint64_t payloadHash) {
    wchar_t local[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) == 0) {
        if (GetTempPathW(MAX_PATH, local) == 0) return L"";
    }
    wchar_t hex[32]{};
    // 16 位十六进制足够区分（碰撞概率对"同一台机器上的几个脚本"可忽略）
    swprintf_s(hex, L"%016llx", static_cast<unsigned long long>(payloadHash));
    return std::wstring(local) + L"\\QstPlayer\\rt\\" + hex;
}

std::wstring LoadOpenCvFrom(const std::wstring& dir, std::wstring& reason) {
    reason.clear();
    // 已经加载过（比如上一次尝试成功）直接复用
    if (HMODULE m = GetModuleHandleW(kOpenCvDll)) {
        (void)m;
        wchar_t buf[MAX_PATH]{};
        if (GetModuleFileNameW(GetModuleHandleW(kOpenCvDll), buf, MAX_PATH) > 0) return buf;
        return kOpenCvDll;
    }
    if (dir.empty()) {
        reason = L"未指定组件目录";
        return L"";
    }
    const std::wstring dll = JoinPath(dir, kOpenCvDll);
    if (!FileExists(dll)) {
        reason = L"目录里没有 " + std::wstring(kOpenCvDll) + L"：" + dir;
        return L"";
    }
    // 用绝对路径加载，避免被当前工作目录的同名 DLL 劫持。
    // ⚠ 必须带重试：这个 DLL 可能刚由本进程/父进程从包里搬出来（60+ MB），
    //   杀软扫描期间会占着它，LoadLibrary 报 ERROR_SHARING_VIOLATION(32)。
    //   实测就是这么撞上的 —— 一次性 LoadLibrary 会让首次运行必失败。
    HMODULE loaded = nullptr;
    DWORD lastErr = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        loaded = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (loaded) return dll;
        lastErr = GetLastError();
        if (lastErr != ERROR_SHARING_VIOLATION && lastErr != ERROR_ACCESS_DENIED) break;
        Sleep(400);
    }
    if (loaded) return dll;
    // 依赖缺失（多半是 CRT 没旁路）时给出具体原因
    const DWORD err = lastErr ? lastErr : GetLastError();
    wchar_t msg[512]{};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), msg, 512, nullptr);
    std::wstring detail(msg);
    while (!detail.empty() && (detail.back() == L'\r' || detail.back() == L'\n')) detail.pop_back();
    reason = L"无法加载 " + dll + L"（错误 " + std::to_wstring(err) + L"：" + detail + L"）";
    return L"";
}

ResolveOutput ResolveRuntime(const ResolveInput& in) {
    ResolveOutput out;
    const auto& manifest = in.manifest;
    const auto& mode = manifest.mode;

    // ── 找图 ──────────────────────────────────────────────────────
    if (!manifest.needOpenCv) {
        out.openCv = ComponentSource::None;
    } else if (mode.bundledOpenCv) {
        std::wstring reason;
        out.openCvPath = LoadOpenCvFrom(in.bundledDir, reason);
        if (!out.openCvPath.empty()) {
            out.openCv = ComponentSource::Bundled;
        } else {
            out.openCv = ComponentSource::Unavailable;
            out.ok = false;
            out.fatalError = L"本脚本需要图像识别，但随包的组件释放失败。\n\n" + reason
                + L"\n\n请检查杀毒软件是否隔离了文件，或重新导出。";
        }
    } else {
        // 走软件：先试已装软件目录
        std::wstring reason;
        if (!in.installedAppDir.empty()) {
            out.openCvPath = LoadOpenCvFrom(in.installedAppDir, reason);
        } else {
            reason = L"没有找到已安装的「键鼠工坊」";
        }
        if (!out.openCvPath.empty()) {
            out.openCv = ComponentSource::InstalledApp;
        } else {
            // **不得硬失败**：明确告诉用户两条出路
            out.openCv = ComponentSource::Unavailable;
            out.ok = false;
            out.fatalError = L"本脚本的图像识别组件被设置为「使用已安装的键鼠工坊」，"
                L"但在本机没有找到可用的软件。\n\n原因：" + reason
                + L"\n\n解决办法（任选其一）：\n"
                L"  1. 安装「键鼠工坊」后重新运行；\n"
                L"  2. 请对方重新导出，并把「找图」设为「自带」（这样不依赖软件）。";
        }
    }

    // ── OCR ───────────────────────────────────────────────────────
    // P0 只解析来源、不创建后端：真正的后端在 P2 接入（自带 = 系统 WinRT OCR；
    // 走软件 = 软件目录的 Python OCR）。这里不做致命判定，交给运行时按动作提示。
    if (!manifest.needOcr) {
        out.ocr = ComponentSource::None;
    } else if (mode.bundledOcr) {
        out.ocr = ComponentSource::SystemOcr;
        out.warnings.push_back(
            L"本脚本包含文字识别：将使用系统自带 OCR。识别结果可能与「键鼠工坊」内不一致。");
    } else {
        out.ocr = ComponentSource::InstalledApp;
        out.warnings.push_back(
            L"本脚本包含文字识别：将使用已安装「键鼠工坊」的 OCR 运行环境。");
    }

    return out;
}

}  // namespace playerruntime
