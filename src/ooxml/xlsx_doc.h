#pragma once
// ──────────────────────────────────────────────────────────────────
// xlsx_doc.h — xlsx 读写（不装 Office、不依赖任何第三方库）
//
// 设计原则（**这是本文件存在的理由，别为了省事改掉**）：
//
// 1. **字节保留式编辑**：改一个单元格时，**只对该 `<c>` 元素做外科手术式替换**。
//    `xl/charts/*`、`xl/styles.xml`、嵌入图片、透视表缓存等几十个 part 由
//    `ZipArchive` 原样搬运；连**同一个 worksheet 里**的 `<mergeCells>`、
//    `<conditionalFormatting>`、`<dataValidations>`、行高/隐藏属性都**一字不动**。
//    ⚠ 反面做法是「解析成对象模型 → 重新生成整个 sheet1.xml」—— 那会把
//    所有我们不认识的元素**静默丢掉**（图表引用、数据验证、条件格式全没）。
//    所以这里是**文本外科手术**，不是模型重建。
//
// 2. **工具层不调模型**：读写是确定性的纯逻辑；模型只负责决定「改哪个格子成什么」。
//
// 3. **产物可验收**：写完能立刻 `GetCell` 读回来核对（确定性），不需要「打开看看」。
//
// 已知边界（诚实标注，不是遗漏）：
//   · 写入的新文本用 **inlineStr**（`t="inlineStr"` + `<is><t>`）。OOXML 允许逐格选择，
//     所以**完全不用碰 `sharedStrings.xml`** —— 又一次减少改动面。旧 sharedStrings 里
//     可能留下不再被引用的条目，无害（OOXML 允许）。
//   · 写公式时**不会计算**，`<v>` 缓存值由调用方给（没给就留空）⇒ Excel 打开时会自己重算，
//     但**直读**（含我们自己的读取）在 Excel 打开前拿不到值。这是 OOXML 的既有语义，
//     不是我们的 bug；要立刻有值就传 `cachedValue`。
//   · `<dimension>` **不更新**（保留原值）。它是提示，Excel/WPS 打开时会重算；
//     保持不动是为了让手术面尽量小。
//   · 不支持 .xls（老二进制格式）与加密文件。
// ──────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string>
#include <vector>

#include "ooxml/zip_archive.h"

namespace qst {
namespace ooxml {

enum class CellKind {
    Empty,
    Number,
    Text,
    Bool,
    Formula,   ///< 有 `<f>`；`text` 存公式原文（不含前导 `=`），`number` 存缓存值（可能无）
    Error,     ///< `t="e"`
};

struct CellValue {
    CellKind kind = CellKind::Empty;
    double number = 0;          ///< Number / Bool(0|1) / Formula 的缓存值
    bool hasNumber = false;     ///< Formula 有没有缓存值（没缓存时 number 无意义）
    bool boolean = false;
    std::string text;           ///< Text 的内容；Formula 的公式原文；Error 的错误码
    std::string formula;        ///< 公式原文（与 kind==Formula 时 text 相同，便于取用）
    std::string cachedValue;    ///< Formula 的 `<v>` 缓存值（原样写；空 ⇒ 不写 `<v>`）
    /// ★**显示值**：当单元格的数字格式是**日期/时间**时，这里是格式化后的 ISO 串。
    /// 为什么必须有：Excel 把日期存成**序列号**（2024-01-01 = 45292），
    /// 直读拿到的是 `45292` —— 对人和模型都是垃圾。原来的 PowerShell 回退分支
    /// 同样只给 `<v>` 原文 ⇒ 日期在 OOXML 路线上**一直是坏的**（只有 COM 路线对）。
    /// ⚠ 只对**日期/时间**填；百分比等其它格式**保持原始数值**（0.12 就是 0.12），
    ///   因为那才是真值，模型要算数。
    /// 导出（TSV / 整簿文本）优先用 `display`，为空才回落到 `number`。
    std::string display;
};

/// 列名 ↔ 序号（"A"=0, "Z"=25, "AA"=26）；`ParseCellRef` 解析 "B12" → col=1,row=12
int ColumnNameToIndex(const std::string& name);
std::string ColumnIndexToName(int index);
bool ParseCellRef(const std::string& ref, int& col, int& row);

struct SheetInfo {
    std::string name;        ///< 工作表名（UTF-8）
    std::string partPath;    ///< `xl/worksheets/sheetN.xml`
    int sheetId = 0;
};

class XlsxDoc {
public:
    bool LoadFromMemory(const uint8_t* data, size_t size, std::string& err);
    bool LoadFromFile(const std::wstring& path, std::string& err);
    bool SaveToMemory(std::vector<uint8_t>& out, std::string& err) const;
    bool SaveToFile(const std::wstring& path, std::string& err) const;

    bool Loaded() const { return loaded_; }
    bool Dirty() const { return dirty_; }
    const std::vector<SheetInfo>& Sheets() const { return sheets_; }
    /// 找不到返回 nullptr
    const SheetInfo* FindSheet(const std::string& name) const;

    /// 读一个单元格。**格子不存在 = 成功且 kind==Empty**（不是错误）；
    /// 只有「工作表名不对 / XML 结构坏」才返回 false。
    bool GetCell(const std::string& sheetName, const std::string& ref,
        CellValue& out, std::string& err) const;

    /// 写一个单元格。**只改这一个 `<c>` 元素**（见文件头第 1 条）。
    /// 不存在则按列序插入；行不存在则按行序插入新行。
    bool SetCell(const std::string& sheetName, const std::string& ref,
        const CellValue& value, std::string& err);
    /// 便捷写法
    bool SetCellText(const std::string& sheetName, const std::string& ref,
        const std::string& utf8Text, std::string& err);
    bool SetCellNumber(const std::string& sheetName, const std::string& ref,
        double value, std::string& err);
    /// 写公式（不含前导 `=`）。`cachedValue` 为空 ⇒ 不写 `<v>`，等 Excel 打开时重算。
    bool SetCellFormula(const std::string& sheetName, const std::string& ref,
        const std::string& formula, const std::string& cachedValue, std::string& err);

    /// 把一张表导成 TSV（制表符分隔、一行一记录；空行/空列按数据边界裁剪）。
    /// 这是给模型看的主格式（与 `readDocument` 的表格式一致）。
    bool DumpSheetAsTsv(const std::string& sheetName, int maxRows, int maxCols,
        std::string& out, std::string& err) const;

    /// 把**整个工作簿**导成文本，格式**对齐** `tools/office/read_doc.ps1` 的 OOXML 回退分支：
    /// 每张表一行 `## 工作表: <名>`，随后是该表的 TSV 行（跳过空行、行尾制表符裁掉）。
    /// 行/列超限时截断（超行数会写一行 `…（更多行已省略）`）。
    /// ⚠ 与脚本的**唯一差异**：公式单元格若**没有缓存值**，这里写 `=公式原文`
    ///   （脚本会写空）。对模型来说「这里有个公式」比「空」有用得多，所以刻意保留。
    bool DumpWorkbookAsText(int maxRows, int maxCols, std::string& out, std::string& err) const;

    /// 新建一个最小可用的 xlsx（一张表），之后用 `SetCell*` 填内容。
    /// 生成 6 个 part（Content_Types / _rels / workbook / workbook.rels / styles / sheet1），
    /// Excel / WPS / LibreOffice 都能直接打开。
    /// ⚠ 这是「**没装 Office 也能写出真 xlsx**」的落点 —— 之前只有 COM 一条路。
    static bool CreateNew(const std::string& sheetName, XlsxDoc& out, std::string& err);

    /// 表里已出现的最大行/列（0 基；空表返回 -1）
    bool SheetBounds(const std::string& sheetName, int& maxRow, int& maxCol, std::string& err) const;

private:
    bool EnsureSheetLoaded(size_t sheetIndex, std::string& err) const;
    /// 懒解析 `xl/styles.xml`（cellXfs → numFmtId、自定义格式表）与 `workbookPr date1904`
    void EnsureStyleTableLoaded() const;
    /// 该样式下标对应的**显示值**（只有日期/时间才有；其它格式返回空串）
    std::string DisplayForStyle(int styleIndex, double value) const;

    bool loaded_ = false;
    bool dirty_ = false;
    std::vector<uint8_t> bytes_;                 ///< 原始 zip 字节（未解析时保留）
    mutable ZipArchive zip_;
    std::vector<SheetInfo> sheets_;
    mutable std::vector<std::string> sharedStrings_;     ///< 懒解析（读路径是 const ⇒ 必须 mutable）
    mutable bool sharedLoaded_ = false;
    /// 每张表的 XML 文本缓存（读时填；改时同步更新并写回 zip_）
    mutable std::vector<std::string> sheetXml_;
    mutable std::vector<bool> sheetXmlLoaded_;
    /// 数字格式表（懒解析）：cellXfs 里第 i 个 xf 的 numFmtId
    mutable bool stylesLoaded_ = false;
    mutable std::vector<int> cellXfNumFmtId_;
    mutable std::vector<std::pair<int, std::string>> customNumFmts_;
    mutable bool date1904_ = false;
};

}  // namespace ooxml
}  // namespace qst
