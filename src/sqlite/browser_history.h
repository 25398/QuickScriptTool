#pragma once
// ──────────────────────────────────────────────────────────────────
// browser_history.h — 直接读 Chromium 系浏览器（Edge / Chrome）的历史与书签
//
// ★ 为什么要有这个模块（替代 UI 自动化路线）
//   实测「统计 Edge 最近 10 条浏览记录」走 UI 路线（observePage + listUiControls + zoom）
//   要 8+ 轮、多次截图、23.6 秒，而且 `observePage` 在 `edge://history` 上**必然失败** ——
//   Chrome/Edge 的 `edge://` / `chrome://` 内置页**不允许扩展注入 content script**，
//   重试没有任何意义。正路是**直接读浏览器自己的 SQLite 库**：
//   一条查询就拿到「完整标题 + 完整 URL + 精确时间戳」，不用截图、不用识图、不会抄错。
//
// ★ 本模块只读，绝不写浏览器库（写库会和浏览器抢锁、可能损坏用户数据）。
//
// ★ 为什么自己做而不调外部程序
//   用户的纪律：**产品不依赖别人的软件运行**（见 LESSONS.md §18）。
//   所以不调 `sqlite3.exe`、不调 Python，格式解析由 `src/sqlite/sqlite_read.*` 自己做。
// ──────────────────────────────────────────────────────────────────

#include <string>
#include <vector>

namespace browser_history {

/// 一条历史记录（时间已转成「本地时间字符串」，模型直接可读）
struct HistoryEntry {
    std::string title;     ///< UTF-8；可能为空（未加载完 / 无标题）
    std::string url;       ///< UTF-8
    std::string visitTime; ///< 本地时间 `YYYY-MM-DD HH:MM:SS`
    int64_t visits = 0;    ///< 访问次数（visit_count 列；没有该列时为 0）
};

/// 一个书签
struct Bookmark {
    std::string name; ///< UTF-8
    std::string url;  ///< UTF-8
};

/// 一次查询结果
struct ReadResult {
    bool ok = false;
    /// 失败原因（给模型看，必须可执行：说清「哪个文件不存在」「为什么读不了」「下一步怎么办」）
    std::wstring error;
    /// 实际读到的库路径（模型据此知道数据来源，也便于用户核对）
    std::wstring dbPath;
    /// 数据来源描述，如 `Edge` / `Chrome`
    std::wstring source;
    std::vector<HistoryEntry> history;
    std::vector<Bookmark> bookmarks;
    /// 补充说明（例如「库被浏览器占用但已按只读方式打开」）
    std::wstring note;
    /// 该浏览器库里的记录总数（用于告诉模型「你只要了 10 条，库里其实有 2096 条」）
    int64_t totalHistory = 0;
    int64_t totalBookmarks = 0;
};

/// 已探测到的浏览器（供 `listBrowsers` 用，也让错误信息能说清「我找过哪些地方」）
struct BrowserInfo {
    std::wstring name;      ///< `Edge` / `Chrome`
    std::wstring profile;   ///< `Default` / `Profile 1` …
    std::wstring historyDb; ///< History 库完整路径
    std::wstring bookmarkDb;///< Bookmarks 文件完整路径（是 JSON，不是 SQLite）
    bool hasHistory = false;
    bool hasBookmarks = false;
};

/// 列出本机探测到的所有浏览器 profile（不做读取，只报路径与存在性）。
/// ★ 用途：模型先 `listBrowsers` 确认目标，再 `readBrowserHistory` ——
///   比「猜路径 + 试错」少一轮往返。
std::vector<BrowserInfo> ListBrowsers();

/// 读历史记录。
/// @param browser   `edge` / `chrome` / 空（空 = 自动挑第一个有历史的）
/// @param profile   如 `Default`；空 = 用 `Default`，找不到就挑第一个有历史的
/// @param limit     最多返回多少条（按访问时间倒序，最近的在前）
/// @param result    输出
/// @return 成功与否（失败细节在 `result.error`）
bool ReadHistory(const std::wstring& browser, const std::wstring& profile,
    int limit, ReadResult& result);

/// 读书签（Chromium 的书签是 JSON 文件，不是 SQLite）。
bool ReadBookmarks(const std::wstring& browser, const std::wstring& profile,
    int limit, ReadResult& result);

/// ★ 把结果渲染成给模型看的文本（表格式，一行一条，便于直接抄进表格）。
/// 放在这里而不是工具里，是为了让自检也能钉住格式（不依赖 Win32）。
std::wstring FormatReadResult(const ReadResult& r);

} // namespace browser_history
