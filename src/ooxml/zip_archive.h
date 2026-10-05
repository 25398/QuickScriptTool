#pragma once
// ──────────────────────────────────────────────────────────────────
// zip_archive.h — zip 容器读写，**支持「未改动条目原样搬运」**
//
// 这是「字节保留式编辑」的地基，也是我们和「另存一份」路线的根本区别：
//   改一个 .xlsx 里的某个单元格时，OOXML 里只有 `xl/worksheets/sheetN.xml` 该变；
//   `xl/charts/*`、`xl/styles.xml`、嵌入图片等**几十个 part 必须一字不动地活下来** ——
//   一旦走「解析成对象模型再重新生成」，图表/公式/透视表/样式全丢。
//   ⇒ 本类的保存策略是：**改过的条目重新编码，没改过的把原始压缩字节原样拷贝**。
//
// 为什么自己写：产品**不依赖别人的软件/库**。本仓原有 zip 代码只支持 stored（不解压），
// 读不了真实 Office 文件；解压能力见 `ooxml/inflate.h`（自研，RFC 1951）。
//
// 纯逻辑：解析/构造全在内存里；只有 `LoadFromFile` / `SaveToFile` 两个便捷函数碰磁盘。
// ──────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string>
#include <vector>

namespace qst {
namespace ooxml {

/// zip 里的一个条目。**`rawComp` 是原始压缩字节**，原样搬运靠它。
struct ZipEntry {
    std::string name;                 ///< UTF-8（OOXML 的 part 名都是 ASCII）
    uint16_t method = 0;              ///< 0 = stored, 8 = deflate
    uint32_t crc32 = 0;               ///< 未压缩内容的 CRC-32
    uint64_t compSize = 0;
    uint64_t uncompSize = 0;
    std::vector<uint8_t> rawComp;     ///< 压缩后的字节（原样搬运）
    std::vector<uint8_t> data;        ///< 解压后的内容（`GetData` 时懒填充）
    bool dataLoaded = false;
    bool modified = false;            ///< true ⇒ 保存时重新编码（method 0）
};

class ZipArchive {
public:
    /// 解析一个 zip（内存）。失败时 `err` 给出可读原因。
    /// ⚠ Zip64（>4GB 或 >65535 条目）当前**明确不支持**，会返回可读错误而不是静默解析错。
    bool LoadFromMemory(const uint8_t* data, size_t size, std::string& err);
    bool LoadFromFile(const std::wstring& path, std::string& err);

    /// 序列化成 zip 字节。未改动条目：原始压缩字节 + 原始 method/crc 原样写入；
    /// 改动/新增条目：以 stored（method 0）重新编码。
    bool SaveToMemory(std::vector<uint8_t>& out, std::string& err) const;
    bool SaveToFile(const std::wstring& path, std::string& err) const;

    const std::vector<ZipEntry>& Entries() const { return entries_; }
    /// 找不到返回 nullptr（名字**区分大小写**：OOXML 的 part 名是大小写敏感的）
    const ZipEntry* Find(const std::string& name) const;
    bool Has(const std::string& name) const { return Find(name) != nullptr; }

    /// 取解压后的内容（懒解压 + 缓存；stored 条目直接就是内容）
    bool GetData(const std::string& name, std::vector<uint8_t>& out, std::string& err);
    /// 取文本（同 GetData，只是转成 string）
    bool GetText(const std::string& name, std::string& out, std::string& err);

    /// 覆盖已有条目的内容 ⇒ 该条目 `modified = true`（保存时重新编码）
    bool SetData(const std::string& name, std::vector<uint8_t> data, std::string& err);
    /// 新增条目（重名会失败）
    bool AddEntry(const std::string& name, std::vector<uint8_t> data, std::string& err);
    /// 删除条目
    bool RemoveEntry(const std::string& name);

private:
    std::vector<ZipEntry> entries_;
};

/// CRC-32（IEEE 802.3，多项式 0xEDB88320）—— 与 `utils.cpp` 里那份算法一致。
/// ⚠ 这里**故意重复实现**：本模块要保持自包含（能在「只链 qst_utils」的纯逻辑自检里单独跑），
///   10 行标准算法跨模块重复一次，比让容器层反向依赖 utils 更划算。
uint32_t ZipCrc32(const void* data, size_t len);

/// 文件 IO（供容器/文档层复用，避免各处再写一遍 Win32）
bool ReadZipFileBytes(const std::wstring& path, std::vector<uint8_t>& out);
bool WriteZipFileBytes(const std::wstring& path, const std::vector<uint8_t>& data);

}  // namespace ooxml
}  // namespace qst
