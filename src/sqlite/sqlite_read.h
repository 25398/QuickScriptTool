#pragma once
// ──────────────────────────────────────────────────────────────────
// sqlite_read.h — **只读**的 SQLite 文件解析（够用就好，不做通用库）
//
// 为什么自己写（而不是引入 sqlite3.c）：
//   · 需求窄得可怜：**读一张表、按某列排序、取前 N 行**。要读的现实对象是
//     浏览器历史库（Edge/Chrome 的 `History`，SQLite 3 格式）——
//     它是「用户最近浏览了什么」的**权威且完整**的来源。
//   · 对照 UI 路线（实测）：`observePage` 在 edge:// 内置页必然失败 →
//     `listUiControls` 只能看到进视口的那几十条、标题还被省略号截断 →
//     模型反复 zoom 读像素 → 最后用 Excel COM 收尾，单轮 23.6 秒。
//     而直接读库是**一条命令、全量、标题完整、时间精确**。
//   · 产品原则：**不依赖别人的软件/库运行**（见 AGENTS.md 硬约定）。
//     用户的机器上不能假设有 Python（有 `sqlite3` 模块）或 sqlite3.exe。
//     引入 sqlite3.c 则是 ~25 万行第三方 C 代码 + 许可证声明要长期维护。
//   · 这与 `src/ooxml/` 是同一个判断：**为确切的窄需求写自己的实现**。
//
// 明确**不支持**（真需要时再说，不要偷偷假装支持）：
//   · 写入 / 事务 / WAL（`PRAGMA journal_mode=wal`）—— read_version 必须为 1，
//     是 WAL 就明确报错（浏览器默认是 rollback journal，不命中）。
//   · 索引 / 查询计划 / JOIN / 聚合 / 表达式求值 —— 本模块**不是** SQL 引擎。
//   · 加密库（SQLCipher）—— 文件头魔数对不上就报错。
//
// 纯逻辑：只吃 `std::vector<uint8_t>` 字节，不碰文件系统、不碰 Windows API
//        ⇒ 可以直接进「只链 qst_utils」的自检档。
//        文件读取由调用方负责（**注意：浏览器运行时库文件被占用，要先复制再读**，
//        见 ReadSqliteFileBytes 的说明）。
// ──────────────────────────────────────────────────────────────────

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace qst {
namespace sqlite {

/// 一列的值。SQLite 是**动态类型**：同一列不同行可以存不同类型，所以用变体而不是
/// 按列定类型 —— 硬定类型会在遇到 NULL / 类型混用时报错或给错值。
struct Value {
    enum class Kind { Null, Integer, Real, Text, Blob };
    Kind kind = Kind::Null;
    int64_t integer = 0;
    double real = 0.0;
    std::string text;              // UTF-8 原样（库内即 UTF-8；UTF-16 库会转好再放这里）
    std::vector<uint8_t> blob;

    bool IsNull() const { return kind == Kind::Null; }
    /// 取整数值；Text 会尝试解析（失败给 fallback），Real 截断。
    int64_t AsInt(int64_t fallback = 0) const;
    /// 取文本；Integer/Real 会转成十进制字符串。
    std::string AsText() const;
};

/// 一行（按 schema 列序）。
using Row = std::vector<Value>;

/// 一个已打开（解析完 schema）的**只读**数据库。
class Database {
public:
    Database() = default;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    /// 从内存字节解析。失败时 `err` 给可读原因（不是「打开失败」四个字）。
    /// 只解析 100 字节文件头 + 第 1 页的 `sqlite_master`（表清单），**不**遍历数据页。
    bool LoadFromMemory(std::vector<uint8_t> bytes, std::string& err);
    bool LoadFromFile(const std::string& pathUtf8, std::string& err);

    bool ok() const { return ok_; }
    int pageSize() const { return pageSize_; }
    int pageCount() const { return pageCount_; }
    /// 库内文本编码：1=UTF-8、2=UTF-16LE、3=UTF-16BE（已在读取时统一转成 UTF-8）。
    int textEncoding() const { return textEncoding_; }

    /// 表是否存在（`sqlite_master` 里 type='table'）。
    bool HasTable(const std::string& table) const;
    /// 表的列名（按 `CREATE TABLE` 里的声明顺序 = record 里的顺序）。
    bool TableColumns(const std::string& table, std::vector<std::string>& out) const;
    /// 表名清单（含 `sqlite_*` 内部表；调用方自己过滤）。
    std::vector<std::string> TableNames() const;

    /// 扫描整张表并按 `orderByColumn`（空 = 不排序）取前 `limit` 行。
    /// `orderByColumn` 必须是 schema 里的列名；**只支持数值/文本的自然序**，
    /// 不解析 SQL（`ORDER BY x DESC LIMIT 10` 这类语义由调用方表达成
    /// `ascending=false` + `limit=10`）。
    /// ⚠ 全表扫描：不建索引、不做二分。Edge 的 urls 表约 2000 行 / 2MB，
    ///    实测毫秒级；表很大时调用方应先限 `limit`（排序仍需扫完，这是取舍）。
    bool ScanTable(const std::string& table, const std::string& orderByColumn,
        bool ascending, size_t limit, std::vector<Row>& out, std::string& err) const;

    /// 便捷：`SELECT <columns> FROM <table>` 全表扫（不排序），带列名映射。
    /// `columns` 里的名字在 schema 里找不到 ⇒ 该列给空 Value 并把名字记进 `err`
    /// （不整体失败：少一列不该让整次读取白费）。
    bool SelectColumns(const std::string& table, const std::vector<std::string>& columns,
        size_t limit, std::vector<Row>& out, std::string& err) const;

    /// 把最后一次 Load 的 schema 原文（`sqlite_master.sql`）取出来，供排查。
    std::string SchemaSqlOf(const std::string& table) const;

private:
    struct TableInfo {
        std::string name;
        std::string sql;
        int rootPage = 0;
        std::vector<std::string> columns;
    };

    bool ReadPage(int pageNo, const uint8_t** out) const;
    bool WalkTree(int pageNo, size_t rowLimit,
        const std::function<bool(int64_t rowid, const Row& row)>& visit,
        std::string& err) const;
    bool RecordToRow(const uint8_t* page, size_t cellPtr, Row& out,
        int64_t& rowid, std::string& err) const;

    std::vector<uint8_t> bytes_;
    bool ok_ = false;
    int pageSize_ = 0;
    int pageCount_ = 0;
    int usableSize_ = 0;      // pageSize - reservedBytes
    int textEncoding_ = 1;
    std::vector<TableInfo> tables_;

    friend struct SqliteReaderTestAccess;
};

/// 读取文件全部字节。**给浏览器历史库用**：库常被浏览器以**共享读锁**持有，
/// Windows 上直接 `std::ifstream` 打不开（ERROR_SHARING_VIOLATION）。
/// 本函数用 `FILE_SHARE_READ|WRITE|DELETE` 的句柄读当前内容
/// （SQLite 的原子提交保证只读方看到一致快照，不需要自己 CopyFile）。
/// ⚠ **这个函数依赖 Win32**，所以放在 `sqlite_file.cpp` 里 —— `sqlite_read.cpp`
///    保持纯逻辑（只吃字节），自检档只需链纯逻辑部分。
/// 仍失败时返回 false，`err` 给 Windows 错误码（不要静默返回空）。
bool ReadSqliteFileBytes(const std::string& pathUtf8, std::vector<uint8_t>& out, std::string& err);

}  // namespace sqlite
}  // namespace qst
