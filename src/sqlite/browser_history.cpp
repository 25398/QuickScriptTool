// ──────────────────────────────────────────────────────────────────
// browser_history.cpp — Edge / Chrome 历史与书签的只读读取
//
// ★ 定位：UI 自动化拿浏览器历史是「下策」，本文件是「正路」。
//   实测对比（同一任务「统计最近 10 条浏览记录」）：
//     UI 路线：8+ 轮往返 + 多次截图 + 3 次 zoom + Excel COM 23.6 秒，且 observePage 必然失败
//     本模块：1 次调用拿到完整标题 / 完整 URL / 精确时间
//
// ★ 关掉浏览器才能读吗？**不用**。
//   Chromium 运行时一直持有 History 库的共享锁。默认共享模式（不共享写）打开会得到
//   `ERROR_SHARING_VIOLATION`；SQLite 的 `mode=ro` 也需要共享锁，同样会 `database is locked`。
//   ⇒ 必须用 `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE` 打开。
//   **不需要 CopyFile**：SQLite 是原子提交的，只要不加写锁，读到的就是一致快照。
//   （实测：复制整个 2.3MB 库没必要，直接共享读就能解析出 2107 条记录。）
//
// ★ 历史库 schema（实测 Edge 141 / Chrome 同构）
//   `urls(id, url, title, visit_count, typed_count, last_visit_time, hidden)`
//   `last_visit_time` = **1601-01-01 UTC 起的微秒数**（量级 ~1.34e16）。
//   ⚠⚠ 这个量级**超过 2^53**，任何「转 double 再比大小」都会静默错序 ——
//      参见 `sqlite_read.cpp` 里 `ScanTable` 的比较器注释（那是真踩过的坑）。
//
// ★ 书签不是 SQLite，是 JSON 文件（`Bookmarks`，无扩展名）。
//   Chromium 的 JSON 里字符串**可能含非 BMP 字符的转义对**，且 url 里必然有
//   `\/` 这种转义写法。这里只做「够用的」解析：不引入 JSON 依赖，手写抽取 name/url。
// ──────────────────────────────────────────────────────────────────

#include "sqlite/browser_history.h"

#include "sqlite/sqlite_read.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <shlobj.h>

namespace browser_history {

// 解析器在 `qst::sqlite` 里；本文件到处要用，就地引进来。
using qst::sqlite::Database;
using qst::sqlite::ReadSqliteFileBytes;
using qst::sqlite::Row;
using qst::sqlite::Value;

namespace {

// ── 通用小工具 ────────────────────────────────────────────────────

std::wstring GetEnv(const wchar_t* name) {
    wchar_t buf[32768]{};
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return std::wstring();
    return std::wstring(buf, n);
}

bool FileExistsW(const std::wstring& p) {
    if (p.empty()) return false;
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/// 目录是否存在。
/// ⚠⚠ 必须与 `FileExistsW` **分开** —— 实测踩过：`KnownRoots()` 里拿 `FileExistsW`
///   去判 `User Data`（那是个**目录**），而 `FileExistsW` 明确排除目录
///   ⇒ **永远返回 false** ⇒ `ListBrowsers()` 恒为空，整个工具静默不可用。
///   这类 bug 不会报错、不会崩，只会「什么都找不到」。
bool DirExistsW(const std::wstring& p) {
    if (p.empty()) return false;
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::wstring r = a;
    if (r.back() != L'\\' && r.back() != L'/') r += L'\\';
    r += b;
    return r;
}

/// UTF-8 → UTF-16（浏览器库里存的是 UTF-8）
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), need);
    return out;
}

/// UTF-16 → UTF-8（`sqlite` 那半边一律收 UTF-8 路径）
std::string Utf8FromWideLocal(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
        nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
        out.data(), need, nullptr, nullptr);
    return out;
}

/// 去掉 JSON 字符串里的转义。Chromium 会写 `\/`（URL 一定大量出现）、
/// `\"`、`\\`、`\uXXXX`（中文书签名可能是这种形式）。
std::string UnescapeJsonString(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\\' || i + 1 >= in.size()) { out += in[i]; continue; }
        const char c = in[++i];
        switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case '/': out += '/'; break;    // ★ `\/` → `/`，URL 里到处都是
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case 'u': {
                if (i + 4 >= in.size()) { out += 'u'; break; }
                unsigned cp = 0;
                bool bad = false;
                for (int k = 1; k <= 4; ++k) {
                    const char h = in[i + k];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                    else { bad = true; break; }
                }
                if (bad) { out += 'u'; break; }
                i += 4;
                // 代理对：高代理后面必须跟 `\uDC00..DFFF`
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < in.size()
                    && in[i + 1] == '\\' && in[i + 2] == 'u') {
                    unsigned lo = 0;
                    bool bad2 = false;
                    for (int k = 3; k <= 6; ++k) {
                        const char h = in[i + k];
                        lo <<= 4;
                        if (h >= '0' && h <= '9') lo |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') lo |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') lo |= static_cast<unsigned>(h - 'A' + 10);
                        else { bad2 = true; break; }
                    }
                    if (!bad2 && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                // UTF-8 编码
                if (cp < 0x80) out += static_cast<char>(cp);
                else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xF0 | (cp >> 18));
                    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: out += c; break;
        }
    }
    return out;
}

/// `YYYY-MM-DD HH:MM:SS`（本地时间）。失败返回空串，调用方自行回退。
std::string FormatLocalTime(int64_t microsSince1601) {
    // 1601-01-01 UTC → 1970-01-01 UTC 的秒数差 = 11644473600
    const int64_t kEpochDiffSec = 11644473600LL;
    const int64_t totalSec = microsSince1601 / 1000000LL - kEpochDiffSec;
    if (totalSec <= 0 || totalSec > 4102444800LL) return std::string();  // 越界（0 或者 2100 年后）
    FILETIME ft{};
    ULARGE_INTEGER u{};
    // FILETIME 就是 1601 起的 100ns 数 —— 直接换算，避免自己处理闰年
    u.QuadPart = static_cast<unsigned long long>(microsSince1601) * 10ULL;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    SYSTEMTIME utc{};
    if (!FileTimeToSystemTime(&ft, &utc)) return std::string();
    SYSTEMTIME local{};
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return std::string();
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
        local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
    return std::string(buf);
}

// ── 浏览器探测 ───────────────────────────────────────────────────

struct BrowserRoot {
    std::wstring name;      ///< `Edge` / `Chrome`
    std::wstring userData;  ///< `...\User Data`
};

/// Chromium 系的 User Data 根目录（都从 LOCALAPPDATA 推）
std::vector<BrowserRoot> KnownRoots() {
    std::vector<BrowserRoot> out;
    const std::wstring local = GetEnv(L"LOCALAPPDATA");
    if (local.empty()) return out;
    const std::wstring edge = JoinPath(local, L"Microsoft\\Edge\\User Data");
    const std::wstring chrome = JoinPath(local, L"Google\\Chrome\\User Data");
    if (DirExistsW(edge)) out.push_back({ L"Edge", edge });
    if (DirExistsW(chrome)) out.push_back({ L"Chrome", chrome });
    return out;
}

/// 找出一个 User Data 下所有 profile 目录（`Default` / `Profile 1` …）。
/// 判定依据：该目录下有 `History` 或 `Bookmarks` 文件。
std::vector<std::wstring> ListProfiles(const std::wstring& userData) {
    std::vector<std::wstring> profiles;
    const std::wstring pattern = JoinPath(userData, L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return profiles;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::wstring nm = fd.cFileName;
        if (nm == L"." || nm == L"..") continue;
        // 只看 `Default` 与 `Profile N`；其它（`System Profile`、`Guest Profile`）跳过，
        // 它们通常没有用户可用的历史，列出来只会让模型困惑。
        const bool looksProfile = (nm == L"Default") || (nm.rfind(L"Profile ", 0) == 0);
        if (!looksProfile) continue;
        const std::wstring dir = JoinPath(userData, nm);
        if (FileExistsW(JoinPath(dir, L"History")) || FileExistsW(JoinPath(dir, L"Bookmarks")))
            profiles.push_back(nm);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    // `Default` 优先，其余按名字排（`Profile 1` < `Profile 2`）
    std::sort(profiles.begin(), profiles.end(), [](const std::wstring& a, const std::wstring& b) {
        if (a == L"Default") return b != L"Default";
        if (b == L"Default") return false;
        return a < b;
    });
    return profiles;
}

std::wstring DatLower(std::wstring s) {
    for (auto& c : s) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return s;
}

// ── 历史读取 ─────────────────────────────────────────────────────

/// 在给定 profile 目录里读历史。
bool ReadHistoryAt(const std::wstring& dbPath, const std::wstring& sourceName,
    int limit, ReadResult& r) {
    std::vector<uint8_t> bytes;
    std::string err;
    if (!ReadSqliteFileBytes(Utf8FromWideLocal(dbPath), bytes, err)) {
        r.error = L"读不了 " + sourceName + L" 的历史库：\n  路径：" + dbPath + L"\n  原因："
            + Utf8ToWide(err);
        return false;
    }

    Database db;
    std::string perr;
    if (!db.LoadFromMemory(std::move(bytes), perr)) {
        r.error = sourceName + L" 的历史库解析失败（可能版本不兼容或文件损坏）："
            + Utf8ToWide(perr);
        return false;
    }
    if (!db.HasTable("urls")) {
        std::string names;
        for (const auto& t : db.TableNames()) {
            if (!names.empty()) names += ", ";
            names += t;
        }
        r.error = sourceName + L" 的历史库里没有 `urls` 表（现有表：" + Utf8ToWide(names)
            + L"）。可能是别的浏览器版本，或这个文件不是历史库。";
        return false;
    }

    // 先拿总数，好告诉模型「你只要 10 条，库里其实有 N 条」
    {
        std::vector<Row> all;
        std::string e2;
        if (db.ScanTable("urls", {}, true, 0, all, e2)) r.totalHistory = static_cast<int64_t>(all.size());
    }

    std::vector<Row> rows;
    std::string serr;
    // ★ 按 last_visit_time 降序 + limit —— 这就是「最近 N 条」的正路。
    //   （比较器里的 int64 修正是必需的：这里的时间戳 > 2^53，转 double 会错序。）
    if (!db.ScanTable("urls", "last_visit_time", false,
            limit > 0 ? static_cast<size_t>(limit) : 0, rows, serr)) {
        r.error = sourceName + L" 历史查询失败：" + Utf8ToWide(serr);
        return false;
    }

    std::vector<std::string> cols;
    db.TableColumns("urls", cols);
    int iUrl = -1, iTitle = -1, iVisit = -1, iTime = -1;
    for (size_t i = 0; i < cols.size(); ++i) {
        if (cols[i] == "url") iUrl = static_cast<int>(i);
        else if (cols[i] == "title") iTitle = static_cast<int>(i);
        else if (cols[i] == "visit_count") iVisit = static_cast<int>(i);
        else if (cols[i] == "last_visit_time") iTime = static_cast<int>(i);
    }
    if (iUrl < 0 || iTime < 0) {
        r.error = sourceName + L" 历史库的 `urls` 表缺少 url / last_visit_time 列（实际列：";
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i) r.error += L", ";
            r.error += Utf8ToWide(cols[i]);
        }
        r.error += L"）。";
        return false;
    }

    r.history.clear();
    r.history.reserve(rows.size());
    for (const auto& row : rows) {
        const auto cell = [&](int idx) -> const Value* {
            if (idx < 0 || static_cast<size_t>(idx) >= row.size()) return nullptr;
            return &row[static_cast<size_t>(idx)];
        };
        HistoryEntry e;
        if (const Value* v = cell(iUrl)) e.url = v->AsText();
        if (const Value* v = cell(iTitle)) e.title = v->AsText();
        if (const Value* v = cell(iVisit)) e.visits = v->AsInt();
        if (const Value* v = cell(iTime)) e.visitTime = FormatLocalTime(v->AsInt());
        if (e.visitTime.empty()) e.visitTime = "(未知时间)";
        r.history.push_back(std::move(e));
    }
    r.ok = true;
    r.dbPath = dbPath;
    r.source = sourceName;
    return true;
}

// ── 书签读取（JSON，手写抽取） ────────────────────────────────────

/// 从 Chromium `Bookmarks` JSON 里抽出成对的 `"name": "...", ... "url": "..."`。
/// 只认 `"type": "url"` 的条目 —— `folder` 没有 `url`，自然会被跳过。
/// ⚠ 不做完整 JSON 解析（不引依赖、不值得）：只按顺序扫 `"name"` / `"url"` 两个键。
///    Chromium 的书签对象里 `name` 一定在 `url` 之前，所以「读到 name 就等下一个 url」是稳的。
bool ParseBookmarksJson(const std::string& json, int limit, std::vector<Bookmark>& out) {
    out.clear();
    size_t pos = 0;
    std::string pendingName;
    bool haveName = false;
    while (pos < json.size()) {
        const size_t k = json.find('"', pos);
        if (k == std::string::npos) break;
        const size_t kEnd = json.find('"', k + 1);
        if (kEnd == std::string::npos) break;
        const std::string key = json.substr(k + 1, kEnd - k - 1);
        // 找这个 key 的 value（跳过空白与冒号）
        size_t v = kEnd + 1;
        while (v < json.size() && (json[v] == ' ' || json[v] == ':' || json[v] == '\t'
                || json[v] == '\r' || json[v] == '\n')) ++v;
        if (v >= json.size() || json[v] != '"') { pos = kEnd + 1; continue; }
        const size_t vEnd = json.find('"', v + 1);
        if (vEnd == std::string::npos) break;
        std::string value = json.substr(v + 1, vEnd - v - 1);
        pos = vEnd + 1;

        if (key == "name") {
            pendingName = UnescapeJsonString(value);
            haveName = true;
        } else if (key == "url" && haveName) {
            Bookmark b;
            b.name = pendingName;
            b.url = UnescapeJsonString(value);
            out.push_back(std::move(b));
            haveName = false;
            if (limit > 0 && static_cast<int>(out.size()) >= limit) break;
        }
    }
    return true;
}

} // namespace

// ── 公开接口 ─────────────────────────────────────────────────────

std::vector<BrowserInfo> ListBrowsers() {
    std::vector<BrowserInfo> out;
    for (const auto& root : KnownRoots()) {
        for (const auto& prof : ListProfiles(root.userData)) {
            BrowserInfo bi;
            bi.name = root.name;
            bi.profile = prof;
            const std::wstring dir = JoinPath(root.userData, prof);
            const std::wstring h = JoinPath(dir, L"History");
            const std::wstring b = JoinPath(dir, L"Bookmarks");
            bi.hasHistory = FileExistsW(h);
            bi.hasBookmarks = FileExistsW(b);
            if (bi.hasHistory) bi.historyDb = h;
            if (bi.hasBookmarks) bi.bookmarkDb = b;
            out.push_back(std::move(bi));
        }
    }
    return out;
}

bool ReadHistory(const std::wstring& browser, const std::wstring& profile,
    int limit, ReadResult& r) {
    r = ReadResult{};
    const std::vector<BrowserInfo> all = ListBrowsers();
    if (all.empty()) {
        r.error =
            L"没找到 Edge / Chrome 的浏览器数据目录。我找过这两个位置：\n"
            L"  %LOCALAPPDATA%\\Microsoft\\Edge\\User Data\n"
            L"  %LOCALAPPDATA%\\Google\\Chrome\\User Data\n"
            L"可能原因：没装这两个浏览器，或用了别的浏览器（如 Firefox / 360 / QQ 浏览器，"
            L"它们的库格式不同，当前不支持）。\n"
            L"替代路线：如果浏览器已打开，用 listUiControls（typeFilter=ListItem）"
            L"读那个页面的控件名字 —— 那是唯一能做的方式，别去截图（列表内容截图必然被截断）。";
        return false;
    }

    const std::wstring wantBr = DatLower(browser);
    const std::wstring wantProf = profile;

    // 挑目标：优先「浏览器+profile 都对上」，其次「只对上浏览器」，最后「第一个有历史的」
    const BrowserInfo* picked = nullptr;
    for (const auto& bi : all) {
        if (!bi.hasHistory) continue;
        if (!wantBr.empty() && DatLower(bi.name) != wantBr) continue;
        if (!wantProf.empty() && bi.profile != wantProf) continue;
        picked = &bi;
        break;
    }
    if (!picked && !wantBr.empty()) {
        for (const auto& bi : all) {
            if (bi.hasHistory && DatLower(bi.name) == wantBr) { picked = &bi; break; }
        }
    }
    if (!picked) {
        for (const auto& bi : all) {
            if (bi.hasHistory) { picked = &bi; break; }
        }
    }
    if (!picked) {
        // 列出来让模型知道「我看到了什么、但它没有历史」
        std::wstring seen;
        for (const auto& bi : all) {
            if (!seen.empty()) seen += L"；";
            seen += bi.name + L" / " + bi.profile;
        }
        r.error = L"找到了浏览器 profile，但没有一个有 History 文件。看到的 profile：" + seen
            + L"。\n可能这个 profile 从没用过，或被清过历史。";
        return false;
    }

    if (!ReadHistoryAt(picked->historyDb, picked->name, limit, r)) return false;

    // 读到就顺带说清「这是哪个 profile、库里一共多少条」
    r.note = L"来源：" + picked->name + L" / " + picked->profile;
    return true;
}

bool ReadBookmarks(const std::wstring& browser, const std::wstring& profile,
    int limit, ReadResult& r) {
    r = ReadResult{};
    const std::vector<BrowserInfo> all = ListBrowsers();
    const std::wstring wantBr = DatLower(browser);

    const BrowserInfo* picked = nullptr;
    for (const auto& bi : all) {
        if (!bi.hasBookmarks) continue;
        if (!wantBr.empty() && DatLower(bi.name) != wantBr) continue;
        if (!profile.empty() && bi.profile != profile) continue;
        picked = &bi;
        break;
    }
    if (!picked && !wantBr.empty()) {
        for (const auto& bi : all) {
            if (bi.hasBookmarks && DatLower(bi.name) == wantBr) { picked = &bi; break; }
        }
    }
    if (!picked) {
        for (const auto& bi : all) {
            if (bi.hasBookmarks) { picked = &bi; break; }
        }
    }
    if (!picked) {
        r.error = L"没找到任何浏览器书签文件（`Bookmarks`）。";
        return false;
    }

    std::vector<uint8_t> bytes;
    std::string err;
    if (!ReadSqliteFileBytes(Utf8FromWideLocal(picked->bookmarkDb), bytes, err)) {
        r.error = L"读不了书签文件：" + picked->bookmarkDb + L"\n  原因：" + Utf8ToWide(err);
        return false;
    }
    const std::string json(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    ParseBookmarksJson(json, limit, r.bookmarks);
    r.totalBookmarks = static_cast<int64_t>(r.bookmarks.size());
    r.ok = true;
    r.dbPath = picked->bookmarkDb;
    r.source = picked->name;
    r.note = L"来源：" + picked->name + L" / " + picked->profile + L"（书签是 JSON 文件）";
    return true;
}

std::wstring FormatReadResult(const ReadResult& r) {
    std::wstring out;
    if (!r.error.empty()) return L"[错误] " + r.error;
    out += L"来源：" + r.source;
    if (!r.dbPath.empty()) out += L"（" + r.dbPath + L"）";
    if (!r.note.empty()) out += L"\n" + r.note;

    if (!r.history.empty()) {
        out += L"\n\n最近 " + std::to_wstring(r.history.size()) + L" 条浏览记录"
            L"（按时间倒序；库里共 " + std::to_wstring(r.totalHistory) + L" 条）：\n";
        out += L"序号\t时间\t访问次数\t标题\tURL\n";
        for (size_t i = 0; i < r.history.size(); ++i) {
            const auto& e = r.history[i];
            out += std::to_wstring(i + 1) + L"\t"
                + Utf8ToWide(e.visitTime) + L"\t"
                + std::to_wstring(e.visits) + L"\t"
                + (e.title.empty() ? std::wstring(L"(无标题)") : Utf8ToWide(e.title)) + L"\t"
                + Utf8ToWide(e.url) + L"\n";
        }
        out += L"\n★ 以上是**完整**标题与 URL（没有截断、没有省略号）。"
            L"直接把这五行抄进表格即可，**不要**再去截图或开 edge://history 核对。";
    }

    if (!r.bookmarks.empty()) {
        out += L"\n\n书签 " + std::to_wstring(r.bookmarks.size()) + L" 条：\n";
        out += L"序号\t名称\tURL\n";
        for (size_t i = 0; i < r.bookmarks.size(); ++i) {
            const auto& b = r.bookmarks[i];
            out += std::to_wstring(i + 1) + L"\t"
                + (b.name.empty() ? std::wstring(L"(无名称)") : Utf8ToWide(b.name)) + L"\t"
                + Utf8ToWide(b.url) + L"\n";
        }
    }

    if (r.history.empty() && r.bookmarks.empty()) {
        out += L"\n（这个库里没有可读记录。可能历史被清空过，或 limit 太小。）";
    }
    return out;
}

} // namespace browser_history
