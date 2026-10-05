// ──────────────────────────────────────────────────────────────────
// sqlite_file.cpp — 把 SQLite 文件读成字节（唯一依赖 Win32 的部分）
//
// 为什么单独一个文件、为什么不用 `std::ifstream`：
//   **浏览器正在运行时，它的 History 库是被打开状态**。Chromium 用 SQLite，
//   SQLite 的文件句柄带 `FILE_SHARE_READ|WRITE`，而我们若用默认共享模式打开，
//   Windows 会直接给 `ERROR_SHARING_VIOLATION`（32）—— 表现为「文件明明在那儿却打不开」，
//   很容易被误判成「没有权限」或「浏览器锁死了」（实测就是这么被绕去的）。
//   带上 `FILE_SHARE_READ|WRITE|DELETE` 就能正常读。
//
// 是否要 CopyFile：**不需要**。SQLite 的提交是原子换页 + 文件头变更计数，
// 只读方要么看到提交前、要么看到提交后，不会读到半截记录。
// （WAL 模式是例外，但那种库我们在 `LoadFromMemory` 里已明确拒绝 ——
//   因为主库文件本身确实不完整。）
// ──────────────────────────────────────────────────────────────────

#include "sqlite/sqlite_read.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <string>

namespace qst {
namespace sqlite {
namespace {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
        static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), need);
    return out;
}

/// 超过这个大小就整体拒绝：本实现把整库读进内存再遍历，
/// 遇到超大库（比如用户拿它开别的东西）会白占内存甚至拖死宿主。
/// 浏览器 History 实测 2–40MB，64MB 留足余量。
constexpr long long kMaxBytes = 64LL << 20;

}  // namespace

bool ReadSqliteFileBytes(const std::string& pathUtf8, std::vector<uint8_t>& out,
    std::string& err) {
    out.clear();
    const std::wstring path = Utf8ToWide(pathUtf8);
    if (path.empty()) {
        err = "SQLite 路径为空或不是合法 UTF-8";
        return false;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        err = "打开 SQLite 文件失败（Win32 错误 " + std::to_string(e) + "）";
        if (e == ERROR_SHARING_VIOLATION) {
            err += "：文件被独占占用。若这是浏览器历史库，"
                   "请确认路径指向 History 文件本身（而不是它所在的目录）";
        } else if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
            err += "：文件不存在";
        } else if (e == ERROR_ACCESS_DENIED) {
            err += "：没有读取权限";
        }
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        err = "取文件大小失败（Win32 错误 " + std::to_string(GetLastError()) + "）";
        return false;
    }
    if (size.QuadPart <= 0) {
        CloseHandle(h);
        err = "SQLite 文件为空（0 字节）";
        return false;
    }
    if (size.QuadPart > kMaxBytes) {
        CloseHandle(h);
        err = "SQLite 文件 " + std::to_string(size.QuadPart / (1 << 20))
            + "MB，超过本只读实现的上限 64MB（放弃整体读入，避免拖死宿主）";
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t got = 0;
    while (got < out.size()) {
        const DWORD want = static_cast<DWORD>((std::min)(out.size() - got,
            static_cast<size_t>(1u << 20)));
        DWORD chunk = 0;
        if (!ReadFile(h, out.data() + got, want, &chunk, nullptr) || chunk == 0) {
            CloseHandle(h);
            out.clear();
            err = "读取 SQLite 内容失败（Win32 错误 " + std::to_string(GetLastError()) + "）";
            return false;
        }
        got += chunk;
    }
    CloseHandle(h);
    return true;
}

bool Database::LoadFromFile(const std::string& pathUtf8, std::string& err) {
    std::vector<uint8_t> bytes;
    if (!ReadSqliteFileBytes(pathUtf8, bytes, err)) return false;
    return LoadFromMemory(std::move(bytes), err);
}

}  // namespace sqlite
}  // namespace qst
