// fake_focus_stage.cpp — 「注入副本」：让安装目录里的 FakeFocus DLL 永远不被任何进程映射。
//
// ── 为什么必须有这一层（真实事故）──────────────────────────────────────────
// 假焦点注入是「让目标进程 LoadLibrary 这个 DLL」。于是**目标进程（游戏/模拟器）
// 会一直映射着这个文件**，而 Windows 对已映射的映像文件不允许写覆盖：
//   · 宿主被强杀 / taskkill /F / 崩溃 ⇒ 根本没有机会远程 FreeLibrary；
//   · 桌面钩（SetWindowsHookEx）没拆干净 ⇒ user32 把 DLL 钉在目标进程里；
//   · 历史残留让模块引用计数 >1 ⇒ 拆一次也归不了零。
// 结果：`<安装目录>\FakeFocus32.dll` 在**软件关掉之后仍然被锁**，不重启电脑删不掉；
// 安装包/卸载器一碰这个文件就卡在那里（Inno 重试 4 次后弹错误框），用户只能重启。
//
// 解法不是「想办法一定拆干净」（做不到：进程被杀时我们没有任何执行机会），
// 而是**换一个可以被锁的文件**：注入前把 DLL 复制到
//   `%LOCALAPPDATA%\QuickScriptTool\module_stage\<源名>.<大小>_<时间戳>\FakeFocus32.dll`
// 再注入这份副本。安装目录那份从此**谁也不映射** ⇒ 覆盖安装 / 卸载 / 手动删除
// 永远不需要重启。副本被锁是无害的：它按「源文件身份」命名，同一构建复用同一份，
// 新构建换目录，旧目录留给启动清扫按年龄回收（删不掉的跳过）。
//
// ⚠ 副本的文件名必须归一成 `FakeFocus32.dll` / `FakeFocus64.dll`（见下面 CanonicalBaseName）：
//   `MapleIsFakeFocusModulePath`、`TargetHasStaleFakeFocusModule` 都是**按名判据**。
//
// ⚠ 不要改成「注入后删掉副本」：卸载发生在**注入之后**，删了就等于把目标进程的映射
//   变成 delete-pending —— 那恰恰是最难查的一类「文件还在、却删不掉」状态。

#include "fake_focus_stage.h"

#include <windows.h>
#include <shlobj.h>

#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

namespace windowmode {
namespace {

constexpr wchar_t kStageLeaf[] = L"QuickScriptTool\\module_stage";

std::wstring DirNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
}

std::wstring BaseNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

/// `FakeFocus32.next.dll` / `FakeFocus32.safe.dll` / `FakeFocus32.dll` 一律归一成
/// `FakeFocus32.dll`（64 位同理）。按名判据只认这两个名字。
std::wstring CanonicalBaseName(const std::wstring& sourceBase) {
    if (_wcsnicmp(sourceBase.c_str(), L"FakeFocus32", 11) == 0) return L"FakeFocus32.dll";
    if (_wcsnicmp(sourceBase.c_str(), L"FakeFocus64", 11) == 0) return L"FakeFocus64.dll";
    return sourceBase;
}

bool FileStamp(const std::wstring& path, ULONGLONG& outBytes, FILETIME& outWrite) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    outBytes = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    outWrite = fad.ftLastWriteTime;
    return true;
}

std::wstring WinErrText(DWORD code) {
    wchar_t buf[64]{};
    swprintf_s(buf, L"Win32=%lu", static_cast<unsigned long>(code));
    return buf;
}

/// 目录的「最后写入时间」是清扫的判据 ⇒ 复用旧副本时也要把它顶到当前时间，
/// 否则一个天天在用的副本会被当成 30 天没人碰而删掉（删不掉也无害，只是白试一次）。
void TouchDirectory(const std::wstring& dir) {
    HANDLE h = CreateFileW(dir.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    SetFileTime(h, nullptr, nullptr, &now);
    CloseHandle(h);
}

/// 年龄（毫秒）。时间戳在**未来**时一律算 0（新鲜）——
/// 少减一次就会按 ULONGLONG 回绕成天文数字，把刚写的副本当老古董删掉。
ULONGLONG AgeMs(const FILETIME& now, const FILETIME& then) {
    if (CompareFileTime(&then, &now) > 0) return 0;
    ULARGE_INTEGER a{}, b{};
    a.LowPart = now.dwLowDateTime;
    a.HighPart = now.dwHighDateTime;
    b.LowPart = then.dwLowDateTime;
    b.HighPart = then.dwHighDateTime;
    const ULONGLONG delta = (a.QuadPart - b.QuadPart) / 10000ull;  // 100ns → ms
    return delta;
}

bool NameHasLockedLeftoverTag(const std::wstring& name) {
    return wcsstr(name.c_str(), kFakeFocusLockedLeftoverTag) != nullptr;
}

/// 只删目录里的**文件**（不递归）：子目录一律不动。
/// 返回 true = 这个目录已经被清空到可以删掉了。
bool DeleteFilesInDir(const std::wstring& dir, bool& outAnyLocked) {
    outAnyLocked = false;
    WIN32_FIND_DATAW fd{};
    const std::wstring glob = dir + L"\\*";
    HANDLE find = FindFirstFileW(glob.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        outAnyLocked = true;
        return false;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring full = dir + L"\\" + fd.cFileName;
        if (!DeleteFileW(full.c_str())) outAnyLocked = true;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return !outAnyLocked;
}

}  // namespace

std::wstring FakeFocusStageRootDir() {
    wchar_t local[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
            SHGFP_TYPE_CURRENT, local)) && local[0]) {
        return std::wstring(local) + L"\\" + kStageLeaf;
    }
    wchar_t self[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) return L"";
    return DirNameOf(self) + L"module_stage";
}

std::wstring FakeFocusStagedPathIn(const std::wstring& stageRoot,
    const std::wstring& sourcePath) {
    if (stageRoot.empty() || sourcePath.empty()) return {};
    ULONGLONG bytes = 0;
    FILETIME write{};
    if (!FileStamp(sourcePath, bytes, write)) return {};
    const std::wstring sourceBase = BaseNameOf(sourcePath);
    // 目录名里带上**源文件名**：`FakeFocus32.next.dll` 与 `FakeFocus32.dll` 万一
    // 大小+时间戳都一样（同步复制出来的），副本也不该共用同一个目录。
    // ⚠ 用 std::wstring 拼而不是 swprintf_s 进定长缓冲：文件名长度不受我们控制，
    // 定长缓冲一旦装不下就会触发 CRT 的无效参数处理**当场终止进程**（不是截断）。
    wchar_t num[64]{};
    swprintf_s(num, L".%llu_%08lX%08lX", bytes,
        static_cast<unsigned long>(write.dwHighDateTime),
        static_cast<unsigned long>(write.dwLowDateTime));
    return stageRoot + L"\\" + sourceBase + num + L"\\" + CanonicalBaseName(sourceBase);
}

bool StageFakeFocusDllInto(const std::wstring& stageRoot,
    const std::wstring& sourcePath, std::wstring& outStagedPath, std::wstring& err) {
    outStagedPath.clear();
    err.clear();

    ULONGLONG sourceBytes = 0;
    FILETIME sourceWrite{};
    if (!FileStamp(sourcePath, sourceBytes, sourceWrite)) {
        err = L"读取假焦点 DLL 失败: " + sourcePath;
        return false;
    }
    const std::wstring staged = FakeFocusStagedPathIn(stageRoot, sourcePath);
    if (staged.empty()) {
        err = L"无法计算假焦点注入副本路径: " + sourcePath;
        return false;
    }
    const std::wstring stageDir = DirNameOf(staged);

    // 已有同身份副本 ⇒ 直接复用。**必须按「大小相同就复用」而不是无条件覆盖**：
    // 上一次注入可能正被某个还活着的目标进程映射（覆盖会失败），而那份副本按构造
    // 就是同一个构建的字节，没有理由重写它。
    ULONGLONG stagedBytes = 0;
    FILETIME stagedWrite{};
    if (FileStamp(staged, stagedBytes, stagedWrite) && stagedBytes == sourceBytes) {
        TouchDirectory(stageDir);
        outStagedPath = staged;
        return true;
    }

    std::error_code ec;
    std::filesystem::create_directories(stageDir, ec);
    if (ec && !std::filesystem::is_directory(stageDir, ec)) {
        err = L"创建注入副本目录失败: " + stageDir + L"（" + WinErrText(GetLastError()) + L"）";
        return false;
    }

    // 先落到 `.tmp<pid>` 再原子改名：进程在任何一步被杀，都不会留下一个**半截的**副本
    // 被下一轮当成「同身份、可复用」（大小判据会拦住它，但没必要给这个机会）。
    const std::wstring tmp = staged + L".tmp" + std::to_wstring(GetCurrentProcessId());
    DeleteFileW(tmp.c_str());
    if (!CopyFileW(sourcePath.c_str(), tmp.c_str(), TRUE)) {
        const DWORD code = GetLastError();
        DeleteFileW(tmp.c_str());
        err = L"复制假焦点 DLL 到注入副本失败: " + WinErrText(code);
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), staged.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD code = GetLastError();
        DeleteFileW(tmp.c_str());
        // 竞争：另一个实例/另一轮注入刚好写好了同一份副本 ⇒ 采信它。
        if (FileStamp(staged, stagedBytes, stagedWrite) && stagedBytes == sourceBytes) {
            outStagedPath = staged;
            return true;
        }
        err = L"落定注入副本失败: " + staged + L"（" + WinErrText(code) + L"）";
        return false;
    }
    if (!FileStamp(staged, stagedBytes, stagedWrite) || stagedBytes != sourceBytes) {
        err = L"注入副本大小与源不一致: " + staged;
        return false;
    }
    outStagedPath = staged;
    return true;
}

bool StageFakeFocusDllForInjection(const std::wstring& sourcePath,
    std::wstring& outStagedPath, std::wstring& err) {
    return StageFakeFocusDllInto(FakeFocusStageRootDir(), sourcePath, outStagedPath, err);
}

FakeFocusSweepResult SweepStaleFakeFocusArtifactsIn(const std::wstring& stageRoot,
    const std::wstring& exeDir, int keepDays) {
    FakeFocusSweepResult result{};
    const ULONGLONG keepMs = keepDays <= 0
        ? 0ull
        : static_cast<ULONGLONG>(keepDays) * 24ull * 60ull * 60ull * 1000ull;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);

    // ① 旧副本目录（按目录最后写入时间判年龄；里面的文件被进程映射 ⇒ 删不掉就跳过）
    if (!stageRoot.empty()) {
        WIN32_FIND_DATAW fd{};
        const std::wstring glob = stageRoot + L"\\*";
        HANDLE find = FindFirstFileW(glob.c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                if (AgeMs(now, fd.ftLastWriteTime) < keepMs) continue;
                const std::wstring dir = stageRoot + L"\\" + fd.cFileName;
                bool anyLocked = false;
                const bool drainable = DeleteFilesInDir(dir, anyLocked);
                if (!drainable) {
                    ++result.stageKeptLocked;
                    continue;
                }
                if (RemoveDirectoryW(dir.c_str())) {
                    ++result.stageDirsRemoved;
                } else {
                    ++result.stageKeptLocked;
                }
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
    }

    // ② 安装程序「让位改名」留下的 `FakeFocus*.dll.locked-*`（当时锁着删不掉，之后就能删了）
    if (!exeDir.empty()) {
        WIN32_FIND_DATAW fd{};
        const std::wstring glob = exeDir + L"\\FakeFocus*" + kFakeFocusLockedLeftoverTag + L"*";
        HANDLE find = FindFirstFileW(glob.c_str(), &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                // 通配符是宽松的 ⇒ 再按名字精确核一遍，绝不误删用户自己的文件。
                if (!NameHasLockedLeftoverTag(fd.cFileName)) continue;
                const std::wstring full = exeDir + L"\\" + fd.cFileName;
                if (DeleteFileW(full.c_str())) {
                    ++result.leftoversRemoved;
                } else {
                    ++result.leftoversKeptLocked;
                }
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
    }
    return result;
}

FakeFocusSweepResult SweepStaleFakeFocusArtifacts(const std::wstring& exeDir, int keepDays) {
    return SweepStaleFakeFocusArtifactsIn(FakeFocusStageRootDir(), exeDir, keepDays);
}

}  // namespace windowmode
