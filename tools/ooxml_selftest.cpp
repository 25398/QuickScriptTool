// =============================================================================
// OoxmlSelfTest — OOXML 容器层（自研 inflate + zip 读写）自检
// =============================================================================
// 被测：src/ooxml/inflate.cpp（RFC 1951 解压）、src/ooxml/zip_archive.cpp（zip 容器）
//
// 为什么单独一套：这是「不装 Office 也能读写 xlsx/docx/pptx」的**地基**，
// 而且它有一条**必须钉死的性质**：改一个 part 时，**其余 part 的原始压缩字节必须
// 一字不动地活下来**（否则图表/公式/透视表/样式全丢 —— 那正是「另存一份」路线的病）。
// 纯逻辑断言测不出这条，所以这里用**真的 deflate 流**和**真的 zip**做夹具。
//
// 夹具来源（可复现）：用 Python 的 zlib / zipfile 生成后**硬编码**成字节数组 ——
// 这样自检既不依赖 Python，也不依赖任何外部程序或网络。
//
//   MSBuild ... /t:OoxmlSelfTest
//   build\Release\OoxmlSelfTest.exe --json
// =============================================================================
#include "selftest_harness.h"

#include "ooxml/inflate.h"
#include "ooxml/zip_archive.h"
#include "ooxml/xlsx_doc.h"

#include <string>
#include <vector>

namespace {

using selftest::Emit;
using qst::ooxml::InflateRaw;
using qst::ooxml::ZipArchive;

const selftest::CaseInfo kCases[] = {
    {L"inflate_stored_block", L"default",
        L"stored（BTYPE=00）块：手工构造的 LEN/NLEN + 原始数据能解出来"},
    {L"inflate_fixed_huffman", L"default",
        L"固定 Huffman（BTYPE=01）：真实 deflate 流能解出原文"},
    {L"inflate_dynamic_huffman", L"default",
        L"★动态 Huffman（BTYPE=10）：这是真实 Office 文件最常用的编码，"
        L"码长表的「传输顺序」写错必炸（不是 0..18 的顺序）"},
    {L"inflate_rejects_truncated", L"default",
        L"截断的流必须**返回失败**而不是崩、也不是悄悄少给几个字节"},
    {L"inflate_rejects_length_mismatch", L"default",
        L"★解压长度与 zip 头记录的 uncompSize 不一致必须报错 —— "
        L"它把「解压器有 bug」和「文件本来就坏」分开"},
    {L"zip_reads_deflate_entries", L"default",
        L"读**真实 zip**（deflate 条目）：原仓 zip 代码只拷贝不解压 ⇒ 读不了 Office 文件，这条钉住新能力"},
    {L"zip_reads_stored_entry", L"default",
        L"同一 zip 里 stored 与 deflate 混用都要能读"},
    {L"zip_byte_preserving_save", L"default",
        L"★★**字节保留**：只改一个 part，其余 part 的**原始压缩字节必须逐字节相同**"},
    {L"zip_preserves_method_and_crc", L"default",
        L"★未改动条目的 method / crc / compSize 原样保留（不是「解压后重压」）"},
    {L"zip_roundtrip_added_entry", L"default",
        L"新增条目后重新读回，内容与其它条目都对"},
    {L"zip_rejects_zip64", L"default",
        L"Zip64 必须**明确拒绝**并说清原因，不能按 32 位错读出一堆垃圾"},

    // ── xlsx 文档层（在其上做**外科手术式**单元格读写）────────────────────────
    {L"xlsx_lists_sheets", L"default",
        L"从 workbook.xml + rels 解析出工作表名与 part 路径（含 rId 映射与约定回退）"},
    {L"xlsx_reads_shared_string", L"default",
        L"t=\"s\" 走 sharedStrings；**富文本 run（多个 <r><t>）要拼起来**；实体要解码"},
    {L"xlsx_reads_number_bool_formula", L"default",
        L"数字 / 布尔 / 公式（公式给 `<f>` 原文 + 缓存值）"},
    {L"xlsx_dump_tsv", L"default",
        L"导成 TSV（给模型看的主格式，与 readDocument 的表格式一致）"},
    {L"xlsx_set_cell_replaces_value", L"default",
        L"改一个已有单元格：值变了、能读回"},
    {L"xlsx_set_cell_keeps_style_attr", L"default",
        L"★替换单元格时必须**保留原 `<c>` 上的 `s`（样式）**等属性 —— 否则数字格式/填充全丢"},
    {L"xlsx_set_cell_preserves_other_parts", L"default",
        L"★★**跨 part 字节保留**：改一个格子后，`xl/charts/*`、`xl/styles.xml`、另一张表的"
        L"**原始压缩字节必须逐字节相同**（这是本层存在的理由）"},
    {L"xlsx_set_cell_preserves_sheet_extras", L"default",
        L"★★**同 part 内**也要保留：`<mergeCells>`/`<cols>`/行高属性等我们「不认识」的元素"
        L"一字不动（反面做法=模型重建 ⇒ 全丢）"},
    {L"xlsx_set_cell_inserts_in_column_order", L"default",
        L"新增单元格要按列号插入（不是无条件追加到行尾）"},
    {L"xlsx_set_cell_creates_row_in_order", L"default",
        L"行不存在时按行号升序插入新行"},
    {L"xlsx_roundtrip_after_save", L"default",
        L"存盘再读回：新值在、老值在、表名在"},
    {L"xlsx_escapes_xml_text", L"default",
        L"写含 `&`/`<`/`>` 的文本必须转义（一个裸 `&` 就能把文件写坏）"},
    {L"xlsx_rejects_non_xlsx", L"default",
        L"不是 xlsx（缺 xl/workbook.xml）要给出可读错误，不能当成功"},
    {L"xlsx_create_new_roundtrip", L"default",
        L"★★**从零新建 xlsx**（6 个最小 part）→ 填单元格 → 存盘 → 重新打开能读回"
        L"（这是「没装 Office 也能写出真 xlsx」的落点）"},
    {L"xlsx_dump_workbook_text", L"default",
        L"整簿文本导出：每表一行 `## 工作表: <名>` + TSV 行；**无缓存值的公式要写成 `=公式`**"
        L"（脚本那版会写空 —— 刻意比它更有用）"},
    {L"xlsx_formats_date_and_time", L"default",
        L"★★**日期/时间格式化**：Excel 把日期存成序列号（2024-01-01=45292），直读拿到的是垃圾。"
        L"按 `cellXfs` 的 numFmtId 还原成 ISO；**没有样式的数字保持原始值**（不许乱猜格式）"},
    {L"xlsx_formats_custom_percent_and_leap", L"default",
        L"★自定义格式 `yyyy-mm-dd` 也要认；**百分比不给格式化值**（0.12 是真值，模型要算数）；"
        L"**1900 假闰日**：Excel 认为序列 60 是 1900-02-29，原样还原"},
    {L"xlsx_formats_1904_system", L"default",
        L"★`workbookPr date1904`：同一个序列号在两种日期系统下**差 1462 天**"
        L"（45292 → 2028-01-02 而不是 2024-01-01）⇒ 钉住「基准没写死」"},
};

// ── 夹具 ──────────────────────────────────────────────────────────────────────

/// 原文 "Hello, deflate!" 的**固定 Huffman** deflate 流（Python zlib, wbits=-15）
const uint8_t kFixedDeflate[] = {
    0xF3,0x48,0xCD,0xC9,0xC9,0xD7,0x51,0x48,0x49,0x4D,0xCB,0x49,0x2C,0x49,0x55,0x04,
    0x00
};

/// 2158 字节表格文本的**动态 Huffman** deflate 流（同样 wbits=-15）
const uint8_t kDynamicDeflate[] = {
    0x25,0x95,0x5D,0xCA,0xAC,0x37,0x0C,0x83,0xAF,0xDF,0xAC,0x26,0xFE,0x49,0x62,0x2F,
    0xA7,0x94,0xDE,0x1D,0x38,0x50,0x68,0xBB,0xFD,0xEA,0xF1,0xC7,0xC0,0x30,0x64,0x62,
    0xD9,0x96,0x25,0xE7,0xEF,0xDF,0xFF,0x7D,0x7F,0xFE,0xFE,0xF5,0xFD,0xFB,0xC7,0xAF,
    0x7F,0xFE,0x5A,0xFB,0xD3,0x67,0xD9,0xA7,0xCF,0xF2,0xCF,0xBF,0x5C,0xF1,0xC5,0xD7,
    0x2B,0xBF,0xFC,0xEC,0xAE,0xF3,0x9D,0xCF,0xCF,0xBA,0xDF,0xFD,0xE2,0xAE,0xA7,0xCB,
    0xD9,0xAB,0x74,0xFB,0xE6,0x6A,0x5D,0x2F,0x5B,0xB6,0x15,0x60,0x5B,0x20,0x46,0x8C,
    0xEB,0xC4,0x15,0x65,0x99,0xCB,0x42,0x71,0x76,0x7B,0x59,0x2A,0xD2,0xFA,0x2E,0xD3,
    0x1F,0x9F,0x0B,0xD1,0xAE,0xA2,0xFD,0xE8,0xE4,0x29,0xDC,0x4B,0x77,0x4A,0xE1,0xE1,
    0x8A,0x6A,0x85,0xC7,0x55,0x3D,0x5B,0xE1,0x29,0x64,0x37,0x12,0xA7,0x4E,0x5C,0xE1,
    0x59,0xB9,0x5C,0x31,0xDF,0xF1,0x5E,0xAE,0x98,0xEF,0xBC,0xBB,0xFC,0x28,0xFC,0x0A,
    0xD9,0xAF,0xC2,0x2F,0x27,0x4F,0xE1,0x8F,0x3B,0xA5,0xF0,0x47,0x54,0x2B,0xBC,0x84,
    0x13,0x5B,0xE1,0x2D,0xE4,0x30,0xBA,0x55,0xAE,0x70,0x8A,0xDF,0x4A,0x1F,0x41,0xF5,
    0x5B,0x15,0x45,0x52,0xBE,0xA9,0xC8,0x38,0xD4,0x4F,0xDD,0x71,0xA1,0xCA,0xD5,0x4A,
    0x3C,0x61,0x58,0xA8,0xBB,0x28,0x18,0x48,0x35,0x1C,0x0D,0xCA,0x11,0x07,0xB9,0x41,
    0xB9,0x4A,0x91,0x36,0x24,0x88,0xA9,0x74,0x50,0x9E,0xA8,0xCB,0x00,0xA5,0xC4,0x65,
    0x26,0x28,0x2D,0x72,0xF3,0x40,0xC4,0x56,0x8E,0x54,0xD7,0x9F,0x9B,0xE8,0xCF,0x07,
    0xFF,0xBE,0x75,0xAF,0x84,0xE2,0xB1,0x15,0xDB,0x42,0xF1,0xDC,0xB6,0xCE,0x86,0xCC,
    0xA3,0x1C,0xC7,0x60,0xF3,0x72,0xE6,0xA0,0x3C,0xDD,0x3B,0x01,0x4A,0x29,0xF6,0x24,
    0x28,0xCD,0x38,0x0F,0x83,0x24,0xC7,0xB9,0x42,0x09,0x95,0xBF,0xCE,0x13,0x8A,0x78,
    0xD7,0xBD,0x12,0x4A,0x84,0xEA,0x3B,0x2D,0x94,0x48,0xD5,0x7C,0x37,0x53,0xA1,0x8F,
    0x6B,0x8C,0xE5,0xA9,0xB7,0xEB,0xA0,0x94,0xFA,0xBD,0x01,0x4A,0x8B,0x83,0x9B,0x4C,
    0x66,0x8B,0x97,0x7B,0x90,0x11,0x5C,0x5D,0xDD,0xFA,0x32,0xC4,0xDF,0x7D,0x42,0xC9,
    0x14,0xA7,0xB7,0x84,0x92,0x57,0x3C,0xDF,0x66,0xBA,0x4F,0xDC,0xBF,0x3D,0xBA,0x52,
    0x8E,0x87,0x10,0xCF,0xD6,0x84,0x1E,0x62,0x3C,0xA2,0x68,0x3D,0x04,0x79,0x42,0x73,
    0x7C,0x88,0xF2,0x28,0x64,0x3D,0x74,0x79,0x98,0xF6,0x43,0x9A,0xE7,0x71,0x86,0x3A,
    0x4F,0x73,0x6F,0x04,0xBA,0x89,0x45,0xA3,0x4A,0x66,0xAB,0x50,0xE9,0x45,0x4C,0x85,
    0x4C,0xEF,0x51,0xDE,0xF2,0x51,0x8A,0x6A,0x29,0x84,0x7A,0x4B,0xF5,0x15,0x4A,0x7D,
    0x5B,0x35,0x17,0x52,0x7D,0xF4,0x51,0x68,0xF5,0xA9,0xCD,0x55,0x88,0xF5,0x1D,0xF5,
    0x5B,0xA8,0xF5,0x3D,0x71,0x50,0xC8,0xF5,0xB5,0x78,0x69,0xF4,0x5A,0x58,0xA1,0x11,
    0x6C,0xB9,0xF8,0x6B,0x1F,0xC9,0x61,0x17,0x24,0x5B,0x57,0x3C,0x37,0x9A,0xAD,0x12,
    0xF7,0x8D,0x68,0x9B,0x79,0x34,0xAA,0x15,0x88,0xCE,0x90,0x6D,0x8B,0xCA,0xD5,0xE8,
    0xB6,0xAF,0x66,0xD9,0x08,0xB7,0x6B,0xE3,0x36,0xA4,0xAB,0x6F,0x0C,0xB7,0x6D,0xAC,
    0xE7,0x73,0xFE,0x23,0xE0,0xD4,0x6D,0xDB,0x3F,0x12,0xBE,0xC2,0xD0,0x09,0xF2,0xDB,
    0x25,0x64,0xDB,0x23,0x63,0x23,0x9F,0xFE,0x45,0x82,0xE6,0xC1,0xF9,0x48,0x59,0x0A,
    0xE6,0xFE,0x88,0xD9,0xEE,0x05,0x67,0xE4,0x6C,0x85,0xCB,0x6D,0x04,0xED,0x3F,0x46,
    0x1F,0x49,0x7B,0xE0,0x75,0x1B,0x51,0xFB,0xC1,0xEE,0x36,0xB2,0xF6,0x87,0xE3,0x6D,
    0x84,0xED,0x8D,0xE9,0x0D,0x69,0x5B,0x8C,0xEF,0x0D,0x71,0x5B,0x24,0xD6,0x37,0xE4,
    0x2D,0x0D,0xE2,0x7E,0x43,0xE0,0x16,0xCD,0x02,0x30,0x24,0x6E,0x69,0x97,0x5D,0x82,
    0xC8,0x55,0x1A,0x79,0x1D,0x99,0x9B,0xC8,0xE4,0xDC,0xC7,0x74,0xA5,0x29,0xAB,0x92,
    0xB1,0x9D,0xB2,0xE9,0x37,0x62,0xB7,0x13,0x52,0x84,0xAA,0x02,0x73,0x74,0x62,0x8E,
    0xE0,0xED,0xD4,0x9C,0x23,0x79,0xA1,0xCF,0x7D,0x44,0x6F,0x37,0x06,0x07,0xD9,0xD3,
    0xBA,0xF0,0x63,0xCF,0x06,0x44,0x95,0x16,0x48,0xDF,0xDE,0xD4,0x13,0x88,0xDF,0x5E,
    0x52,0x67,0xC4,0x8F,0xA1,0xA9,0x3F,0x30,0x80,0xBD,0xA6,0xAF,0xC0,0x02,0x56,0xD3,
    0x6F,0x60,0x02,0xAC,0xCE,0x39,0x36,0xB0,0x1A,0x7E,0x02,0x23,0x58,0xEF,0x59,0x93,
    0x58,0x41,0x4B,0x00,0x3E,0x73,0xFF,0xAC,0x4A,0xF2,0xE6,0xEC,0xE5,0x1E,0xFE,0x13,
    0x43,0x68,0xD2,0xCC,0x25,0x63,0x56,0xC5,0xCC,0x2B,0x31,0x85,0xEF,0xC7,0x1C,0x73,
    0xD6,0xF5,0xCF,0x7C,0x13,0x63,0xB8,0x4A,0xE7,0x1C,0x6B,0x48,0x5B,0xE8,0x21,0x31,
    0x87,0x93,0x58,0xBF,0xB1,0x87,0xFB,0xE8,0xE7,0x60,0x10,0x9F,0x6D,0x22,0x26,0xC1,
    0xF4,0xD1,0xDB,0xC1,0x24,0x1E,0xC6,0xFD,0x13,0xB3,0x84,0xD0,0xA6,0x1D,0x8C,0xE2,
    0xF1,0xC0,0x3F,0xB3,0xD5,0x73,0xF2,0x9E,0x59,0xEC,0x19,0xD4,0x73,0x66,0xB7,0x27,
    0x9A,0x17,0xF3,0x60,0x8A,0x06,0x70,0x7A,0x9E,0x15,0xFC,0xA1,0xA2,0xC0,0x3C,0xD3,
    0xEF,0xC5,0x36,0x7E,0xF0,0x92,0x5D,0x8C,0xE3,0xF2,0xAE,0xEE,0xDF,0x98,0xF5,0x86,
    0xEF,0xD4,0x3F,0x98,0xA2,0x5C,0xF8,0x17,0xFB,0xF8,0xFB,0x79,0x4F,0x30,0x90,0xBF,
    0x03,0xFF,0x17,0x0B,0xF9,0xC3,0xCF,0x5A,0xBC,0x60,0x6A,0x16,0xE0,0x60,0x23,0x2F,
    0xBC,0x6F,0x0F,0x1F,0xE9,0xDD,0x21,0xEF,0xC3,0x47,0xDE,0xEC,0x09,0x7B,0xF8,0xC8,
    0xFB,0xA0,0x87,0x17,0xB3,0x38,0xD9,0x29,0x9A,0xF8,0xCF,0xEA,0x44,0x3F,0x0F,0x1F,
    0xC5,0x1E,0x5D,0x3D,0x7C,0x14,0xBB,0xE7,0x1C,0x1F,0x69,0xA9,0xCE,0x7D,0x7C,0x24,
    0xF6,0x07,0xA7,0xE7,0x69,0x63,0xAF,0x59,0xE1,0x23,0xAD,0x5B,0xF2,0x16,0x3E,0x0A,
    0x67,0x07,0x4A,0x2D,0x60,0x86,0x51,0x67,0xE1,0xA3,0x88,0x9C,0x57,0x31,0x67,0x29,
    0x17,0x7D,0xD5,0x99,0xB5,0x3C,0xFD,0x16,0x3E,0x92,0x8D,0xE0,0xA1,0xF0,0x51,0x24,
    0x7B,0x58,0x66,0x05,0xF3,0x04,0xBC,0x15,0x3E,0x8A,0xC3,0xCE,0xD6,0xC0,0xC1,0xBC,
    0xE3,0xDF,0xC6,0x47,0xDA,0xF3,0xF0,0xDF,0xF8,0x48,0x16,0x64,0x2E,0x8D,0x8F,0xB4,
    0xE2,0x99,0x57,0xE7,0xAC,0xFB,0xCB,0x1C,0x7B,0x9E,0x8D,0x9A,0xF9,0xF6,0x3C,0x1C,
    0x7A,0x44,0x39,0x9F,0xA7,0xA3,0x78,0x63,0xA4,0x52,0x30,0xDB,0xD1,0x49,0xCF,0xF3,
    0xD1,0xBC,0x47,0xFF,0x03
};

/// 真 zip（3 个条目：两个 deflate、一个 stored）。内容见下面期望值。
const uint8_t kRealZip[] = {
    0x50,0x4B,0x03,0x04,0x14,0x00,0x00,0x00,0x08,0x00,0x3A,0xA0,0x37,0x5D,0xB1,0x36,
    0x27,0xF8,0x25,0x00,0x00,0x00,0x26,0x00,0x00,0x00,0x0C,0x00,0x00,0x00,0x64,0x6F,
    0x63,0x2F,0x6B,0x65,0x65,0x70,0x2E,0x78,0x6D,0x6C,0xB3,0x29,0xCA,0xCF,0x2F,0xB1,
    0xB3,0x29,0xB3,0x33,0x31,0xB2,0xD1,0x2F,0xB3,0xB3,0x49,0xCE,0x48,0x2C,0x2A,0x51,
    0xC8,0x4C,0xB1,0x55,0x4A,0x36,0x54,0xD2,0xB7,0xB3,0xD1,0x07,0xCB,0x03,0x00,0x50,
    0x4B,0x03,0x04,0x14,0x00,0x00,0x00,0x08,0x00,0x3A,0xA0,0x37,0x5D,0x41,0xFD,0x0B,
    0x10,0x0C,0x00,0x00,0x00,0x0A,0x00,0x00,0x00,0x0C,0x00,0x00,0x00,0x64,0x6F,0x63,
    0x2F,0x65,0x64,0x69,0x74,0x2E,0x78,0x6D,0x6C,0xB3,0x49,0xB4,0xCB,0xCF,0x49,0xB1,
    0xD1,0x4F,0xB4,0x03,0x00,0x50,0x4B,0x03,0x04,0x14,0x00,0x00,0x00,0x00,0x00,0x3A,
    0xA0,0x37,0x5D,0x13,0x86,0xB9,0x8B,0x04,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x0E,
    0x00,0x00,0x00,0x64,0x6F,0x63,0x2F,0x73,0x74,0x6F,0x72,0x65,0x64,0x2E,0x62,0x69,
    0x6E,0x00,0x01,0x02,0x03,0x50,0x4B,0x01,0x02,0x14,0x00,0x14,0x00,0x00,0x00,0x08,
    0x00,0x3A,0xA0,0x37,0x5D,0xB1,0x36,0x27,0xF8,0x25,0x00,0x00,0x00,0x26,0x00,0x00,
    0x00,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x01,0x00,
    0x00,0x00,0x00,0x64,0x6F,0x63,0x2F,0x6B,0x65,0x65,0x70,0x2E,0x78,0x6D,0x6C,0x50,
    0x4B,0x01,0x02,0x14,0x00,0x14,0x00,0x00,0x00,0x08,0x00,0x3A,0xA0,0x37,0x5D,0x41,
    0xFD,0x0B,0x10,0x0C,0x00,0x00,0x00,0x0A,0x00,0x00,0x00,0x0C,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x01,0x4F,0x00,0x00,0x00,0x64,0x6F,0x63,
    0x2F,0x65,0x64,0x69,0x74,0x2E,0x78,0x6D,0x6C,0x50,0x4B,0x01,0x02,0x14,0x00,0x14,
    0x00,0x00,0x00,0x00,0x00,0x3A,0xA0,0x37,0x5D,0x13,0x86,0xB9,0x8B,0x04,0x00,0x00,
    0x00,0x04,0x00,0x00,0x00,0x0E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x80,0x01,0x85,0x00,0x00,0x00,0x64,0x6F,0x63,0x2F,0x73,0x74,0x6F,0x72,0x65,
    0x64,0x2E,0x62,0x69,0x6E,0x50,0x4B,0x05,0x06,0x00,0x00,0x00,0x00,0x03,0x00,0x03,
    0x00,0xB0,0x00,0x00,0x00,0xB5,0x00,0x00,0x00,0x00,0x00
};

const char kKeepXml[] = "<root><v>42</v><chart id=\"c1\"/></root>";
const char kEditXml[] = "<a>old</a>";

/// 与 Python 侧生成动态流时用的原文**完全一致**（公式照抄，别改一处）
std::string ExpectedDynamicPlain() {
    std::string s = "row\tcol\tvalue\n";
    for (int i = 0; i < 200; ++i) {
        s += std::to_string(i) + "\t" + std::to_string(i % 7) + "\t"
            + std::to_string(i * i) + "\n";
    }
    return s;
}

std::vector<uint8_t> Bytes(const uint8_t* p, size_t n) { return std::vector<uint8_t>(p, p + n); }
std::string AsString(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }

// ── 用例 ──────────────────────────────────────────────────────────────────────

void CaseInflateStored() {
    // 手工构造一个 stored 块：BFINAL=1 BTYPE=00 → 首字节 0x01，然后 LEN/NLEN（小端），再原始数据
    const char payload[] = "stored-block-payload";
    const uint16_t len = static_cast<uint16_t>(sizeof(payload) - 1);
    std::vector<uint8_t> stream;
    stream.push_back(0x01);
    stream.push_back(static_cast<uint8_t>(len & 0xFF));
    stream.push_back(static_cast<uint8_t>(len >> 8));
    stream.push_back(static_cast<uint8_t>(~len & 0xFF));
    stream.push_back(static_cast<uint8_t>((~len >> 8) & 0xFF));
    for (uint16_t i = 0; i < len; ++i) stream.push_back(static_cast<uint8_t>(payload[i]));

    std::vector<uint8_t> out;
    std::string err;
    const bool ok = InflateRaw(stream.data(), stream.size(), len, out, err);
    Emit(L"inflate_stored_block", ok && AsString(out) == std::string(payload),
        (L"ok=" + std::to_wstring(ok ? 1 : 0) + L" out=" + std::wstring(out.begin(), out.end())
            + L" err=" + std::wstring(err.begin(), err.end())).c_str());
}

void CaseInflateFixed() {
    std::vector<uint8_t> out;
    std::string err;
    const bool ok = InflateRaw(kFixedDeflate, sizeof(kFixedDeflate), 15, out, err);
    const std::string got = AsString(out);
    Emit(L"inflate_fixed_huffman", ok && got == "Hello, deflate!",
        (L"ok=" + std::to_wstring(ok ? 1 : 0) + L" got=" + std::wstring(got.begin(), got.end())
            + L" err=" + std::wstring(err.begin(), err.end())).c_str());
}

void CaseInflateDynamic() {
    const std::string want = ExpectedDynamicPlain();
    std::vector<uint8_t> out;
    std::string err;
    const bool ok = InflateRaw(kDynamicDeflate, sizeof(kDynamicDeflate), want.size(), out, err);
    const std::string got = AsString(out);
    Emit(L"inflate_dynamic_huffman", ok && got == want,
        (L"ok=" + std::to_wstring(ok ? 1 : 0) + L" gotLen=" + std::to_wstring(got.size())
            + L" wantLen=" + std::to_wstring(want.size())
            + L" err=" + std::wstring(err.begin(), err.end())).c_str());
}

void CaseInflateRejectsTruncated() {
    // 砍掉后半段：必须失败，不能崩、也不能「悄悄少给几个字节」当成功
    std::vector<uint8_t> out;
    std::string err;
    const size_t cut = sizeof(kDynamicDeflate) / 2;
    const bool ok = InflateRaw(kDynamicDeflate, cut, 0, out, err);
    Emit(L"inflate_rejects_truncated", !ok,
        (L"ok=" + std::to_wstring(ok ? 1 : 0) + L" err=" + std::wstring(err.begin(), err.end())).c_str());
}

void CaseInflateRejectsLengthMismatch() {
    // 完整流 + 故意写错的 expectedSize ⇒ 必须报「长度不一致」
    std::vector<uint8_t> out;
    std::string err;
    const bool ok = InflateRaw(kFixedDeflate, sizeof(kFixedDeflate), 999, out, err);
    const std::string e = err;
    const bool honest = !ok && e.find("长度") != std::string::npos;
    Emit(L"inflate_rejects_length_mismatch", honest,
        (L"ok=" + std::to_wstring(ok ? 1 : 0) + L" err=" + std::wstring(e.begin(), e.end())).c_str());
}

void CaseZipReadsDeflateEntries() {
    ZipArchive z;
    std::string err;
    if (!z.LoadFromMemory(kRealZip, sizeof(kRealZip), err)) {
        Emit(L"zip_reads_deflate_entries", false,
            (L"LoadFromMemory 失败：" + std::wstring(err.begin(), err.end())).c_str());
        return;
    }
    std::string keep, edit;
    const bool a = z.GetText("doc/keep.xml", keep, err);
    const bool b = z.GetText("doc/edit.xml", edit, err);
    const bool ok = a && b && keep == kKeepXml && edit == kEditXml;
    Emit(L"zip_reads_deflate_entries", ok,
        (L"n=" + std::to_wstring(z.Entries().size())
            + L" keepOk=" + std::to_wstring(keep == kKeepXml ? 1 : 0)
            + L" editOk=" + std::to_wstring(edit == kEditXml ? 1 : 0)
            + L" err=" + std::wstring(err.begin(), err.end())).c_str());
}

void CaseZipReadsStoredEntry() {
    ZipArchive z;
    std::string err;
    if (!z.LoadFromMemory(kRealZip, sizeof(kRealZip), err)) {
        Emit(L"zip_reads_stored_entry", false, L"LoadFromMemory 失败");
        return;
    }
    std::vector<uint8_t> d;
    const bool ok = z.GetData("doc/stored.bin", d, err);
    const std::vector<uint8_t> want = {0x00, 0x01, 0x02, 0x03};
    Emit(L"zip_reads_stored_entry", ok && d == want,
        (L"ok=" + std::to_wstring(ok ? 1 : 0) + L" n=" + std::to_wstring(d.size())).c_str());
}

/// ★★ 本套最关键的用例：**字节保留**
void CaseZipBytePreservingSave() {
    ZipArchive z;
    std::string err;
    if (!z.LoadFromMemory(kRealZip, sizeof(kRealZip), err)) {
        Emit(L"zip_byte_preserving_save", false, L"LoadFromMemory 失败");
        return;
    }
    const qst::ooxml::ZipEntry* keepBefore = z.Find("doc/keep.xml");
    const qst::ooxml::ZipEntry* storedBefore = z.Find("doc/stored.bin");
    if (keepBefore == nullptr || storedBefore == nullptr) {
        Emit(L"zip_byte_preserving_save", false, L"夹具里找不到预期条目");
        return;
    }
    const std::vector<uint8_t> keepRawBefore = keepBefore->rawComp;
    const std::vector<uint8_t> storedRawBefore = storedBefore->rawComp;

    // 只改一个 part
    const std::string newEdit = "<a>NEW-VALUE</a>";
    if (!z.SetData("doc/edit.xml", std::vector<uint8_t>(newEdit.begin(), newEdit.end()), err)) {
        Emit(L"zip_byte_preserving_save", false, L"SetData 失败");
        return;
    }
    std::vector<uint8_t> saved;
    if (!z.SaveToMemory(saved, err)) {
        Emit(L"zip_byte_preserving_save", false,
            (L"SaveToMemory 失败：" + std::wstring(err.begin(), err.end())).c_str());
        return;
    }
    // 重新读回（走一遍完整解析），逐条对比
    ZipArchive z2;
    if (!z2.LoadFromMemory(saved.data(), saved.size(), err)) {
        Emit(L"zip_byte_preserving_save", false,
            (L"重新解析失败：" + std::wstring(err.begin(), err.end())).c_str());
        return;
    }
    const qst::ooxml::ZipEntry* keepAfter = z2.Find("doc/keep.xml");
    const qst::ooxml::ZipEntry* storedAfter = z2.Find("doc/stored.bin");
    const bool keepRawSame = keepAfter != nullptr && keepAfter->rawComp == keepRawBefore;
    const bool storedRawSame = storedAfter != nullptr && storedAfter->rawComp == storedRawBefore;

    std::string editText;
    const bool editChanged = z2.GetText("doc/edit.xml", editText, err) && editText == newEdit;
    std::string keepText;
    const bool keepTextSame = z2.GetText("doc/keep.xml", keepText, err) && keepText == kKeepXml;
    const bool countSame = z2.Entries().size() == 3;

    const bool ok = keepRawSame && storedRawSame && editChanged && keepTextSame && countSame;
    Emit(L"zip_byte_preserving_save", ok,
        (L"keepRawSame=" + std::to_wstring(keepRawSame ? 1 : 0)
            + L" storedRawSame=" + std::to_wstring(storedRawSame ? 1 : 0)
            + L" editChanged=" + std::to_wstring(editChanged ? 1 : 0)
            + L" keepTextSame=" + std::to_wstring(keepTextSame ? 1 : 0)
            + L" n=" + std::to_wstring(z2.Entries().size())).c_str());
}

void CaseZipPreservesMethodAndCrc() {
    ZipArchive z;
    std::string err;
    if (!z.LoadFromMemory(kRealZip, sizeof(kRealZip), err)) {
        Emit(L"zip_preserves_method_and_crc", false, L"LoadFromMemory 失败");
        return;
    }
    const qst::ooxml::ZipEntry* keepBefore = z.Find("doc/keep.xml");
    if (keepBefore == nullptr) {
        Emit(L"zip_preserves_method_and_crc", false, L"夹具里找不到 doc/keep.xml");
        return;
    }
    const uint16_t methodBefore = keepBefore->method;
    const uint32_t crcBefore = keepBefore->crc32;
    const uint64_t compBefore = keepBefore->compSize;
    // 注意：夹具里 keep.xml 是 **deflate(8)**；如果我们保存时把它「解压后重压」，
    // method 会变成 0、compSize 会变 ⇒ 这条就是那个回归的哨兵。
    z.SetData("doc/edit.xml", std::vector<uint8_t>{'x'}, err);
    std::vector<uint8_t> saved;
    if (!z.SaveToMemory(saved, err)) {
        Emit(L"zip_preserves_method_and_crc", false, L"SaveToMemory 失败");
        return;
    }
    ZipArchive z2;
    if (!z2.LoadFromMemory(saved.data(), saved.size(), err)) {
        Emit(L"zip_preserves_method_and_crc", false, L"重新解析失败");
        return;
    }
    const qst::ooxml::ZipEntry* keepAfter = z2.Find("doc/keep.xml");
    const bool ok = keepAfter != nullptr && keepAfter->method == methodBefore
        && keepAfter->crc32 == crcBefore && keepAfter->compSize == compBefore
        && methodBefore == 8;
    Emit(L"zip_preserves_method_and_crc", ok,
        (L"methodBefore=" + std::to_wstring(methodBefore)
            + L" methodAfter=" + std::to_wstring(keepAfter ? keepAfter->method : 999)
            + L" compBefore=" + std::to_wstring(compBefore)
            + L" compAfter=" + std::to_wstring(keepAfter ? keepAfter->compSize : 0)).c_str());
}

void CaseZipRoundtripAddedEntry() {
    ZipArchive z;
    std::string err;
    if (!z.LoadFromMemory(kRealZip, sizeof(kRealZip), err)) {
        Emit(L"zip_roundtrip_added_entry", false, L"LoadFromMemory 失败");
        return;
    }
    const std::string name = "doc/added.xml";
    const std::string body = "<added/>";
    if (!z.AddEntry(name, std::vector<uint8_t>(body.begin(), body.end()), err)) {
        Emit(L"zip_roundtrip_added_entry", false, L"AddEntry 失败");
        return;
    }
    std::vector<uint8_t> saved;
    if (!z.SaveToMemory(saved, err)) {
        Emit(L"zip_roundtrip_added_entry", false, L"SaveToMemory 失败");
        return;
    }
    ZipArchive z2;
    if (!z2.LoadFromMemory(saved.data(), saved.size(), err)) {
        Emit(L"zip_roundtrip_added_entry", false, L"重新解析失败");
        return;
    }
    std::string got;
    const bool ok = z2.Entries().size() == 4 && z2.GetText(name, got, err) && got == body;
    Emit(L"zip_roundtrip_added_entry", ok,
        (L"n=" + std::to_wstring(z2.Entries().size()) + L" got=" + std::wstring(got.begin(), got.end())).c_str());
}

void CaseZipRejectsZip64() {
    // 把 EOCD 的 totalEntries 改成 0xFFFF（Zip64 哨兵）⇒ 必须明确拒绝
    std::vector<uint8_t> patched = Bytes(kRealZip, sizeof(kRealZip));
    // EOCD 在最后 22 字节处（无注释）：signature(4) + 4 个 u16 + ...，totalEntries 是第 10..11 字节
    const size_t eocd = patched.size() - 22;
    patched[eocd + 10] = 0xFF;
    patched[eocd + 11] = 0xFF;
    ZipArchive z;
    std::string err;
    const bool loaded = z.LoadFromMemory(patched.data(), patched.size(), err);
    const bool honest = !loaded && err.find("Zip64") != std::string::npos;
    Emit(L"zip_rejects_zip64", honest,
        (L"loaded=" + std::to_wstring(loaded ? 1 : 0) + L" err=" + std::wstring(err.begin(), err.end())).c_str());
}

// ── xlsx 文档层 ───────────────────────────────────────────────────────────────

/// 拼一个**结构真实**的 xlsx 夹具：两张表、富文本 sharedString、带样式的单元格、
/// 行高/列宽/合并单元格、以及一个**必须原样存活**的 chart part 与 styles part。
/// 用 `ZipArchive` 现场拼（比内联几千字节十六进制可读得多；deflate 路径已由 zip 用例覆盖）。
std::vector<uint8_t> MakeFixtureXlsx() {
    ZipArchive z;
    auto add = [&](const char* name, const std::string& body) {
        std::string e;
        z.AddEntry(name, std::vector<uint8_t>(body.begin(), body.end()), e);
    };
    add("[Content_Types].xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/></Types>");
    add("_rels/.rels",
        "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/"
        "package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas."
        "openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
        "Target=\"xl/workbook.xml\"/></Relationships>");
    add("xl/workbook.xml",
        "<?xml version=\"1.0\"?><workbook xmlns:r=\"http://schemas.openxmlformats.org/"
        "officeDocument/2006/relationships\"><sheets>"
        "<sheet name=\"Sheet1\" sheetId=\"1\" r:id=\"rId1\"/>"
        "<sheet name=\"Data\" sheetId=\"2\" r:id=\"rId2\"/>"
        "</sheets></workbook>");
    add("xl/_rels/workbook.xml.rels",
        "<?xml version=\"1.0\"?><Relationships>"
        "<Relationship Id=\"rId1\" Type=\"t/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"t/worksheet\" Target=\"worksheets/sheet2.xml\"/>"
        "</Relationships>");
    add("xl/sharedStrings.xml",
        "<?xml version=\"1.0\"?><sst count=\"3\" uniqueCount=\"3\">"
        "<si><t>Name</t></si>"
        "<si><r><t>Rick </t></r><r><t>Astley</t></r></si>"     // 富文本：多个 run
        "<si><t>A &amp; B</t></si>"                            // 实体
        "</sst>");
    add("xl/worksheets/sheet1.xml",
        "<?xml version=\"1.0\"?><worksheet>"
        "<dimension ref=\"A1:C3\"/>"
        "<sheetViews><sheetView workbookViewId=\"0\"/></sheetViews>"
        "<cols><col min=\"1\" max=\"1\" width=\"20\" customWidth=\"1\"/></cols>"
        "<sheetData>"
        "<row r=\"1\" ht=\"24\" customHeight=\"1\">"
        "<c r=\"A1\" s=\"2\" t=\"s\"><v>0</v></c>"
        "<c r=\"B1\" t=\"s\"><v>1</v></c>"
        "<c r=\"C1\" t=\"s\"><v>2</v></c>"
        "</row>"
        "<row r=\"2\">"
        "<c r=\"A2\"><v>42</v></c>"
        "<c r=\"B2\" t=\"b\"><v>1</v></c>"
        "<c r=\"C2\"><f>SUM(A2:A2)</f><v>42</v></c>"
        "</row>"
        "</sheetData>"
        "<mergeCells count=\"1\"><mergeCell ref=\"A3:C3\"/></mergeCells>"
        "<pageMargins left=\"0.7\" right=\"0.7\"/>"
        "</worksheet>");
    add("xl/worksheets/sheet2.xml",
        "<?xml version=\"1.0\"?><worksheet><sheetData/></worksheet>");
    // 这两个 part 是「必须原样存活」的代表（真实文件里还会有 images / pivotCache 等）
    add("xl/styles.xml", "<styleSheet>STYLES-MUST-SURVIVE-INTACT</styleSheet>");
    add("xl/charts/chart1.xml", "<chart>CHART-MUST-SURVIVE-INTACT</chart>");
    std::vector<uint8_t> out;
    std::string err;
    z.SaveToMemory(out, err);
    return out;
}

/// 载入夹具；失败时把原因写进 detail 并返回 false
bool LoadFixture(qst::ooxml::XlsxDoc& doc, std::string& err) {
    const std::vector<uint8_t> bytes = MakeFixtureXlsx();
    return doc.LoadFromMemory(bytes.data(), bytes.size(), err);
}

std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }

void CaseXlsxListsSheets() {
    qst::ooxml::XlsxDoc doc;
    std::string err;
    if (!LoadFixture(doc, err)) {
        Emit(L"xlsx_lists_sheets", false, (L"载入失败：" + W(err)).c_str());
        return;
    }
    const auto& sheets = doc.Sheets();
    const bool ok = sheets.size() == 2
        && sheets[0].name == "Sheet1" && sheets[0].partPath == "xl/worksheets/sheet1.xml"
        && sheets[1].name == "Data" && sheets[1].partPath == "xl/worksheets/sheet2.xml";
    Emit(L"xlsx_lists_sheets", ok,
        (L"n=" + std::to_wstring(sheets.size()) + L" [0]=" + W(sheets.empty() ? std::string() : sheets[0].name)
            + L"→" + W(sheets.empty() ? std::string() : sheets[0].partPath)).c_str());
}

void CaseXlsxReadsSharedString() {
    qst::ooxml::XlsxDoc doc;
    std::string err;
    if (!LoadFixture(doc, err)) {
        Emit(L"xlsx_reads_shared_string", false, (L"载入失败：" + W(err)).c_str());
        return;
    }
    qst::ooxml::CellValue a1, b1, c1;
    const bool got = doc.GetCell("Sheet1", "A1", a1, err)
        && doc.GetCell("Sheet1", "B1", b1, err)
        && doc.GetCell("Sheet1", "C1", c1, err);
    // B1 是**富文本**：两个 run 必须拼成 "Rick Astley"
    const bool ok = got && a1.kind == qst::ooxml::CellKind::Text && a1.text == "Name"
        && b1.kind == qst::ooxml::CellKind::Text && b1.text == "Rick Astley"
        && c1.kind == qst::ooxml::CellKind::Text && c1.text == "A & B";
    Emit(L"xlsx_reads_shared_string", ok,
        (L"A1=" + W(a1.text) + L" B1=" + W(b1.text) + L" C1=" + W(c1.text)).c_str());
}

void CaseXlsxReadsNumberBoolFormula() {
    qst::ooxml::XlsxDoc doc;
    std::string err;
    if (!LoadFixture(doc, err)) {
        Emit(L"xlsx_reads_number_bool_formula", false, (L"载入失败：" + W(err)).c_str());
        return;
    }
    qst::ooxml::CellValue a2, b2, c2, z9;
    const bool got = doc.GetCell("Sheet1", "A2", a2, err)
        && doc.GetCell("Sheet1", "B2", b2, err)
        && doc.GetCell("Sheet1", "C2", c2, err)
        && doc.GetCell("Sheet1", "Z9", z9, err);
    const bool ok = got
        && a2.kind == qst::ooxml::CellKind::Number && a2.number == 42
        && b2.kind == qst::ooxml::CellKind::Bool && b2.boolean
        && c2.kind == qst::ooxml::CellKind::Formula && c2.formula == "SUM(A2:A2)"
        && c2.hasNumber && c2.number == 42
        && z9.kind == qst::ooxml::CellKind::Empty;   // 不存在的格子 = 空，**不是错误**
    Emit(L"xlsx_reads_number_bool_formula", ok,
        (L"A2=" + std::to_wstring(a2.number) + L" B2=" + std::to_wstring(b2.boolean ? 1 : 0)
            + L" C2=" + W(c2.formula) + L" cached=" + std::to_wstring(c2.hasNumber ? 1 : 0)
            + L" Z9empty=" + std::to_wstring(z9.kind == qst::ooxml::CellKind::Empty ? 1 : 0)).c_str());
}

void CaseXlsxDumpTsv() {
    qst::ooxml::XlsxDoc doc;
    std::string err;
    if (!LoadFixture(doc, err)) {
        Emit(L"xlsx_dump_tsv", false, (L"载入失败：" + W(err)).c_str());
        return;
    }
    std::string tsv;
    if (!doc.DumpSheetAsTsv("Sheet1", 0, 0, tsv, err)) {
        Emit(L"xlsx_dump_tsv", false, (L"导出失败：" + W(err)).c_str());
        return;
    }
    // ⚠ 只导出**有单元格的行**：第 3 行只有 mergeCells、没有 `<c>`，不算数据行。
    //   （刻意不按 `<dimension>` 铺满，否则小表后面会拖一大片空行。）
    const std::string want =
        "Name\tRick Astley\tA & B\n"
        "42\tTRUE\t=SUM(A2:A2)\n";
    Emit(L"xlsx_dump_tsv", tsv == want, (L"got=" + W(tsv)).c_str());
}

void CaseXlsxSetCellReplacesValue() {
    qst::ooxml::XlsxDoc doc;
    std::string err;
    if (!LoadFixture(doc, err)) {
        Emit(L"xlsx_set_cell_replaces_value", false, L"载入失败");
        return;
    }
    if (!doc.SetCellNumber("Sheet1", "A2", 1234, err)) {
        Emit(L"xlsx_set_cell_replaces_value", false, (L"SetCell 失败：" + W(err)).c_str());
        return;
    }
    qst::ooxml::CellValue v;
    const bool ok = doc.GetCell("Sheet1", "A2", v, err)
        && v.kind == qst::ooxml::CellKind::Number && v.number == 1234 && doc.Dirty();
    Emit(L"xlsx_set_cell_replaces_value", ok,
        (L"kind=" + std::to_wstring(static_cast<int>(v.kind)) + L" v=" + std::to_wstring(v.number)).c_str());
}

void CaseXlsxSetCellKeepsStyleAttr() {
    qst::ooxml::XlsxDoc doc;
    std::string err;
    if (!LoadFixture(doc, err)) {
        Emit(L"xlsx_set_cell_keeps_style_attr", false, L"载入失败");
        return;
    }
    // A1 原本是 `s="2" t="s"`（有样式）；改成文本后 **s="2" 必须还在**
    if (!doc.SetCellText("Sheet1", "A1", "Renamed", err)) {
        Emit(L"xlsx_set_cell_keeps_style_attr", false, L"SetCell 失败");
        return;
    }
    std::vector<uint8_t> saved;
    if (!doc.SaveToMemory(saved, err)) {
        Emit(L"xlsx_set_cell_keeps_style_attr", false, L"保存失败");
        return;
    }
    qst::ooxml::XlsxDoc d2;
    if (!d2.LoadFromMemory(saved.data(), saved.size(), err)) {
        Emit(L"xlsx_set_cell_keeps_style_attr", false, L"重新载入失败");
        return;
    }
    qst::ooxml::CellValue v;
    const bool valueOk = d2.GetCell("Sheet1", "A1", v, err) && v.text == "Renamed";
    // 直接看 part 文本里那句是不是还带 s="2"
    std::string sheetXml;
    {
        ZipArchive z;
        z.LoadFromMemory(saved.data(), saved.size(), err);
        z.GetText("xl/worksheets/sheet1.xml", sheetXml, err);
    }
    // ⚠ 属性顺序无关紧要，**别按固定顺序搜字符串**（第一版就是这么写错的：
    //   新 `t` 插在 `<c` 之后，于是 `<c r="A1" s="2"` 搜不到，其实样式好好的）。
    //   正确做法：把 A1 那个元素整段取出来，再看它含哪些属性。
    std::string cellA1;
    {
        const size_t p = sheetXml.find("<c r=\"A1\"");
        const size_t alt = sheetXml.find("<c ");
        const size_t from = (p != std::string::npos) ? p : alt;
        if (from != std::string::npos) {
            const size_t end = sheetXml.find('>', from);
            if (end != std::string::npos) cellA1 = sheetXml.substr(from, end - from + 1);
        }
    }
    const bool styleKept = cellA1.find("s=\"2\"") != std::string::npos;
    const bool typeChanged = cellA1.find("t=\"inlineStr\"") != std::string::npos;
    const bool refKept = cellA1.find("r=\"A1\"") != std::string::npos;
    Emit(L"xlsx_set_cell_keeps_style_attr", valueOk && styleKept && typeChanged && refKept,
        (L"valueOk=" + std::to_wstring(valueOk ? 1 : 0) + L" styleKept=" + std::to_wstring(styleKept ? 1 : 0)
            + L" inlineStr=" + std::to_wstring(typeChanged ? 1 : 0)
            + L" refKept=" + std::to_wstring(refKept ? 1 : 0) + L" cell=" + W(cellA1)).c_str());
}

/// ★★ 本套最关键：**跨 part 字节保留**
void CaseXlsxSetCellPreservesOtherParts() {
    const std::vector<uint8_t> bytes = MakeFixtureXlsx();
    qst::ooxml::XlsxDoc doc;
    std::string err;
    if (!doc.LoadFromMemory(bytes.data(), bytes.size(), err)) {
        Emit(L"xlsx_set_cell_preserves_other_parts", false, L"载入失败");
        return;
    }
    // 取改前各 part 的**原始压缩字节**
    ZipArchive before;
    if (!before.LoadFromMemory(bytes.data(), bytes.size(), err)) {
        Emit(L"xlsx_set_cell_preserves_other_parts", false, L"取基准失败");
        return;
    }
    auto rawOf = [](const ZipArchive& z, const char* name) {
        const qst::ooxml::ZipEntry* e = z.Find(name);
        return e ? e->rawComp : std::vector<uint8_t>();
    };
    const auto chartBefore = rawOf(before, "xl/charts/chart1.xml");
    const auto stylesBefore = rawOf(before, "xl/styles.xml");
    const auto sheet2Before = rawOf(before, "xl/worksheets/sheet2.xml");
    const auto ctBefore = rawOf(before, "[Content_Types].xml");

    if (!doc.SetCellText("Sheet1", "D4", "added", err)) {
        Emit(L"xlsx_set_cell_preserves_other_parts", false, L"SetCell 失败");
        return;
    }
    std::vector<uint8_t> saved;
    if (!doc.SaveToMemory(saved, err)) {
        Emit(L"xlsx_set_cell_preserves_other_parts", false, L"保存失败");
        return;
    }
    ZipArchive after;
    if (!after.LoadFromMemory(saved.data(), saved.size(), err)) {
        Emit(L"xlsx_set_cell_preserves_other_parts", false, L"重新解析失败");
        return;
    }
    const bool chartSame = rawOf(after, "xl/charts/chart1.xml") == chartBefore;
    const bool stylesSame = rawOf(after, "xl/styles.xml") == stylesBefore;
    const bool sheet2Same = rawOf(after, "xl/worksheets/sheet2.xml") == sheet2Before;
    const bool ctSame = rawOf(after, "[Content_Types].xml") == ctBefore;
    const bool sheet1Changed = rawOf(after, "xl/worksheets/sheet1.xml") != rawOf(before, "xl/worksheets/sheet1.xml");
    const bool countSame = after.Entries().size() == before.Entries().size();
    const bool ok = chartSame && stylesSame && sheet2Same && ctSame && sheet1Changed && countSame;
    Emit(L"xlsx_set_cell_preserves_other_parts", ok,
        (L"chart=" + std::to_wstring(chartSame ? 1 : 0) + L" styles=" + std::to_wstring(stylesSame ? 1 : 0)
            + L" sheet2=" + std::to_wstring(sheet2Same ? 1 : 0) + L" ct=" + std::to_wstring(ctSame ? 1 : 0)
            + L" sheet1Changed=" + std::to_wstring(sheet1Changed ? 1 : 0)
            + L" n=" + std::to_wstring(after.Entries().size())).c_str());
}

/// ★★ 同一个 part 内：我们**不认识**的元素也必须一字不动
void CaseXlsxSetCellPreservesSheetExtras() {
    qst::ooxml::XlsxDoc doc;
    std::string err;
    if (!LoadFixture(doc, err)) {
        Emit(L"xlsx_set_cell_preserves_sheet_extras", false, L"载入失败");
        return;
    }
    if (!doc.SetCellNumber("Sheet1", "B3", 7, err)) {
        Emit(L"xlsx_set_cell_preserves_sheet_extras", false, L"SetCell 失败");
        return;
    }
    std::vector<uint8_t> saved;
    if (!doc.SaveToMemory(saved, err)) {
        Emit(L"xlsx_set_cell_preserves_sheet_extras", false, L"保存失败");
        return;
    }
    std::string xml;
    {
        ZipArchive z;
        z.LoadFromMemory(saved.data(), saved.size(), err);
        z.GetText("xl/worksheets/sheet1.xml", xml, err);
    }
    const bool merge = xml.find("<mergeCell ref=\"A3:C3\"/>") != std::string::npos;
    const bool cols = xml.find("<col min=\"1\" max=\"1\" width=\"20\" customWidth=\"1\"/>") != std::string::npos;
    const bool rowHt = xml.find("ht=\"24\" customHeight=\"1\"") != std::string::npos;
    const bool views = xml.find("<sheetViews>") != std::string::npos;
    const bool margins = xml.find("<pageMargins left=\"0.7\" right=\"0.7\"/>") != std::string::npos;
    const bool added = xml.find("<c r=\"B3\">") != std::string::npos;
    // 原有单元格没被顺手重写
    const bool a1Intact = xml.find("<c r=\"A1\" s=\"2\" t=\"s\"><v>0</v></c>") != std::string::npos;
    const bool ok = merge && cols && rowHt && views && margins && added && a1Intact;
    Emit(L"xlsx_set_cell_preserves_sheet_extras", ok,
        (L"merge=" + std::to_wstring(merge ? 1 : 0) + L" cols=" + std::to_wstring(cols ? 1 : 0)
            + L" rowHt=" + std::to_wstring(rowHt ? 1 : 0) + L" views=" + std::to_wstring(views ? 1 : 0)
            + L" margins=" + std::to_wstring(margins ? 1 : 0) + L" added=" + std::to_wstring(added ? 1 : 0)
            + L" a1Intact=" + std::to_wstring(a1Intact ? 1 : 0)).c_str());
}

void CaseXlsxSetCellInsertsInColumnOrder() {
    qst::ooxml::XlsxDoc doc;
    std::string err;
    if (!LoadFixture(doc, err)) {
        Emit(L"xlsx_set_cell_inserts_in_column_order", false, L"载入失败");
        return;
    }
    // 第 2 行已有 A2/B2/C2；往 D2 插（列 3），必须排在 C2 之后（不能甩到 `</row>` 外）
    if (!doc.SetCellNumber("Sheet1", "D2", 99, err)) {
        Emit(L"xlsx_set_cell_inserts_in_column_order", false, (L"SetCell 失败：" + W(err)).c_str());
        return;
    }
    std::vector<uint8_t> saved;
    if (!doc.SaveToMemory(saved, err)) {
        Emit(L"xlsx_set_cell_inserts_in_column_order", false, L"保存失败");
        return;
    }
    std::string xml;
    {
        ZipArchive z;
        z.LoadFromMemory(saved.data(), saved.size(), err);
        z.GetText("xl/worksheets/sheet1.xml", xml, err);
    }
    const size_t a2 = xml.find("<c r=\"A2\">");
    const size_t b2 = xml.find("<c r=\"B2\"");
    const size_t c2 = xml.find("<c r=\"C2\">");
    const size_t d2 = xml.find("<c r=\"D2\">");
    const bool ordered = a2 != std::string::npos && b2 != std::string::npos
        && c2 != std::string::npos && d2 != std::string::npos
        && a2 < b2 && b2 < c2 && c2 < d2;

    // 再覆盖「插在已有之前」的分支：空表上**乱序**插 C1 / A1 / B1，最终必须 A<B<C
    if (!doc.SetCellNumber("Data", "C1", 3, err)
        || !doc.SetCellNumber("Data", "A1", 1, err)
        || !doc.SetCellNumber("Data", "B1", 2, err)) {
        Emit(L"xlsx_set_cell_inserts_in_column_order", false, L"乱序插入失败");
        return;
    }
    std::vector<uint8_t> saved2;
    if (!doc.SaveToMemory(saved2, err)) {
        Emit(L"xlsx_set_cell_inserts_in_column_order", false, L"保存失败");
        return;
    }
    std::string xml2;
    {
        ZipArchive z;
        z.LoadFromMemory(saved2.data(), saved2.size(), err);
        z.GetText("xl/worksheets/sheet2.xml", xml2, err);
    }
    const size_t a1 = xml2.find("<c r=\"A1\">");
    const size_t b1 = xml2.find("<c r=\"B1\">");
    const size_t c1 = xml2.find("<c r=\"C1\">");
    const bool ordered2 = a1 != std::string::npos && b1 != std::string::npos && c1 != std::string::npos
        && a1 < b1 && b1 < c1;
    Emit(L"xlsx_set_cell_inserts_in_column_order", ordered && ordered2,
        (L"appendAfterExisting=" + std::to_wstring(ordered ? 1 : 0)
            + L" outOfOrderInserts=" + std::to_wstring(ordered2 ? 1 : 0)).c_str());
}

void CaseXlsxSetCellCreatesRowInOrder() {
    qst::ooxml::XlsxDoc doc;
    std::string err;
    if (!LoadFixture(doc, err)) {
        Emit(L"xlsx_set_cell_creates_row_in_order", false, L"载入失败");
        return;
    }
    // 第 5 行不存在 ⇒ 要新建；且必须排在第 2 行之后
    if (!doc.SetCellText("Sheet1", "A5", "new-row", err)) {
        Emit(L"xlsx_set_cell_creates_row_in_order", false, (L"SetCell 失败：" + W(err)).c_str());
        return;
    }
    std::vector<uint8_t> saved;
    if (!doc.SaveToMemory(saved, err)) {
        Emit(L"xlsx_set_cell_creates_row_in_order", false, L"保存失败");
        return;
    }
    std::string xml;
    {
        ZipArchive z;
        z.LoadFromMemory(saved.data(), saved.size(), err);
        z.GetText("xl/worksheets/sheet1.xml", xml, err);
    }
    const size_t r2 = xml.find("<row r=\"2\">");
    const size_t r5 = xml.find("<row r=\"5\">");
    const bool ok = r2 != std::string::npos && r5 != std::string::npos && r2 < r5
        && xml.find("<c r=\"A5\" t=\"inlineStr\">") != std::string::npos;
    Emit(L"xlsx_set_cell_creates_row_in_order", ok,
        (L"r2<r5=" + std::to_wstring((r2 != std::string::npos && r5 != std::string::npos && r2 < r5) ? 1 : 0)
            + L" cell=" + std::to_wstring(xml.find("<c r=\"A5\" t=\"inlineStr\">") != std::string::npos ? 1 : 0)).c_str());
}

void CaseXlsxRoundtripAfterSave() {
    qst::ooxml::XlsxDoc doc;
    std::string err;
    if (!LoadFixture(doc, err)) {
        Emit(L"xlsx_roundtrip_after_save", false, L"载入失败");
        return;
    }
    if (!doc.SetCellText("Data", "A1", "hello", err) || !doc.SetCellNumber("Sheet1", "A2", 7, err)) {
        Emit(L"xlsx_roundtrip_after_save", false, L"SetCell 失败");
        return;
    }
    std::vector<uint8_t> saved;
    if (!doc.SaveToMemory(saved, err)) {
        Emit(L"xlsx_roundtrip_after_save", false, L"保存失败");
        return;
    }
    qst::ooxml::XlsxDoc d2;
    if (!d2.LoadFromMemory(saved.data(), saved.size(), err)) {
        Emit(L"xlsx_roundtrip_after_save", false, L"重新载入失败");
        return;
    }
    qst::ooxml::CellValue v1, v2, v3;
    const bool r1 = d2.GetCell("Data", "A1", v1, err);
    const bool r2 = d2.GetCell("Sheet1", "A2", v2, err);
    const bool r3 = d2.GetCell("Sheet1", "B1", v3, err);
    const bool ok = d2.Sheets().size() == 2
        && r1 && v1.text == "hello"
        && r2 && v2.number == 7
        && r3 && v3.text == "Rick Astley";
    Emit(L"xlsx_roundtrip_after_save", ok,
        (L"n=" + std::to_wstring(d2.Sheets().size())
            + L" r1=" + std::to_wstring(r1 ? 1 : 0) + L" A1=" + W(v1.text)
            + L" r2=" + std::to_wstring(r2 ? 1 : 0) + L" A2=" + std::to_wstring(v2.number)
            + L" r3=" + std::to_wstring(r3 ? 1 : 0) + L" B1=" + W(v3.text)
            + L" err=" + W(err)).c_str());
}

void CaseXlsxEscapesXmlText() {
    qst::ooxml::XlsxDoc doc;
    std::string err;
    if (!LoadFixture(doc, err)) {
        Emit(L"xlsx_escapes_xml_text", false, L"载入失败");
        return;
    }
    const std::string tricky = "a & b < c > d \" e";
    if (!doc.SetCellText("Sheet1", "E1", tricky, err)) {
        Emit(L"xlsx_escapes_xml_text", false, L"SetCell 失败");
        return;
    }
    std::vector<uint8_t> saved;
    if (!doc.SaveToMemory(saved, err)) {
        Emit(L"xlsx_escapes_xml_text", false, L"保存失败");
        return;
    }
    qst::ooxml::XlsxDoc d2;
    if (!d2.LoadFromMemory(saved.data(), saved.size(), err)) {
        Emit(L"xlsx_escapes_xml_text", false, L"重新载入失败（很可能 XML 被写坏）");
        return;
    }
    qst::ooxml::CellValue v;
    const bool ok = d2.GetCell("Sheet1", "E1", v, err) && v.text == tricky;
    Emit(L"xlsx_escapes_xml_text", ok, (L"got=" + W(v.text)).c_str());
}

void CaseXlsxRejectsNonXlsx() {
    // 一个合法 zip，但没有 xl/workbook.xml
    ZipArchive z;
    std::string err;
    z.AddEntry("hello.txt", std::vector<uint8_t>{'h', 'i'}, err);
    std::vector<uint8_t> bytes;
    z.SaveToMemory(bytes, err);
    qst::ooxml::XlsxDoc doc;
    const bool loaded = doc.LoadFromMemory(bytes.data(), bytes.size(), err);
    const bool honest = !loaded && err.find("workbook.xml") != std::string::npos;
    Emit(L"xlsx_rejects_non_xlsx", honest,
        (L"loaded=" + std::to_wstring(loaded ? 1 : 0) + L" err=" + W(err)).c_str());
}

/// ★★ 从零新建 —— 「没装 Office 也能写出真 xlsx」的全部意义所在
void CaseXlsxCreateNewRoundtrip() {
    qst::ooxml::XlsxDoc doc;
    std::string err;
    if (!qst::ooxml::XlsxDoc::CreateNew("销售数据", doc, err)) {
        Emit(L"xlsx_create_new_roundtrip", false, (L"CreateNew 失败：" + W(err)).c_str());
        return;
    }
    const bool hasSheet = doc.Sheets().size() == 1 && doc.Sheets()[0].name == "销售数据";
    bool wrote = hasSheet;
    if (wrote) {
        wrote = doc.SetCellText("销售数据", "A1", "品名", err)
            && doc.SetCellText("销售数据", "B1", "金额", err)
            && doc.SetCellText("销售数据", "A2", "键盘", err)
            && doc.SetCellNumber("销售数据", "B2", 199.5, err)
            && doc.SetCellNumber("销售数据", "B3", 42, err);
    }
    std::vector<uint8_t> saved;
    if (!wrote || !doc.SaveToMemory(saved, err)) {
        Emit(L"xlsx_create_new_roundtrip", false, (L"写入/保存失败：" + W(err)).c_str());
        return;
    }
    qst::ooxml::XlsxDoc d2;
    if (!d2.LoadFromMemory(saved.data(), saved.size(), err)) {
        Emit(L"xlsx_create_new_roundtrip", false, (L"重新打开失败：" + W(err)).c_str());
        return;
    }
    qst::ooxml::CellValue a1, b2, b3;
    const bool ok = d2.GetCell("销售数据", "A1", a1, err) && a1.text == "品名"
        && d2.GetCell("销售数据", "B2", b2, err) && b2.number == 199.5
        && d2.GetCell("销售数据", "B3", b3, err) && b3.number == 42
        && d2.Sheets().size() == 1;
    Emit(L"xlsx_create_new_roundtrip", ok,
        (L"sheet=" + W(d2.Sheets().empty() ? std::string() : d2.Sheets()[0].name)
            + L" A1=" + W(a1.text) + L" B2=" + std::to_wstring(b2.number)
            + L" B3=" + std::to_wstring(b3.number)).c_str());
}

void CaseXlsxDumpWorkbookText() {
    // ① 主夹具（两张表）必须都出现表头
    qst::ooxml::XlsxDoc doc;
    std::string err;
    if (!LoadFixture(doc, err)) {
        Emit(L"xlsx_dump_workbook_text", false, L"载入失败");
        return;
    }
    std::string text;
    if (!doc.DumpWorkbookAsText(0, 0, text, err)) {
        Emit(L"xlsx_dump_workbook_text", false, (L"导出失败：" + W(err)).c_str());
        return;
    }
    const bool bothSheets = text.find("## 工作表: Sheet1\n") != std::string::npos
        && text.find("## 工作表: Data\n") != std::string::npos;

    // ② 无缓存值的公式：必须写成 `=公式原文`（脚本那版会写空）
    qst::ooxml::XlsxDoc f;
    if (!qst::ooxml::XlsxDoc::CreateNew("报表", f, err)) {
        Emit(L"xlsx_dump_workbook_text", false, L"CreateNew 失败");
        return;
    }
    if (!f.SetCellText("报表", "A1", "项", err)
        || !f.SetCellNumber("报表", "B1", 2, err)
        || !f.SetCellFormula("报表", "B2", "B1*3", "", err)) {
        Emit(L"xlsx_dump_workbook_text", false, L"造公式夹具失败");
        return;
    }
    std::string ftext;
    if (!f.DumpWorkbookAsText(0, 0, ftext, err)) {
        Emit(L"xlsx_dump_workbook_text", false, L"公式夹具导出失败");
        return;
    }
    const std::string want =
        "## 工作表: 报表\n"
        "项\t2\n"
        "\t=B1*3\n";
    const bool formulaOk = ftext == want;
    Emit(L"xlsx_dump_workbook_text", bothSheets && formulaOk,
        (L"bothSheets=" + std::to_wstring(bothSheets ? 1 : 0)
            + L" formulaOk=" + std::to_wstring(formulaOk ? 1 : 0)
            + L" got=" + W(ftext)).c_str());
}

/// 日期/数字格式夹具：`cellXfs` 的下标就是单元格 `s` 属性。
///   s=1 numFmtId 14（日期）  s=2 numFmtId 20（时间 h:mm）  s=3 numFmtId 22（日期+时间）
///   s=4 numFmtId 9（百分比）  s=5 numFmtId 164（自定义 yyyy-mm-dd）
/// 序列号用 Python 独立算过：2024-01-01 = 45292；1899-12-30 起算。
std::vector<uint8_t> MakeFormatFixtureXlsx(bool date1904) {
    ZipArchive z;
    auto add = [&](const char* name, const std::string& body) {
        std::string e;
        z.AddEntry(name, std::vector<uint8_t>(body.begin(), body.end()), e);
    };
    add("[Content_Types].xml",
        "<?xml version=\"1.0\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
        "</Types>");
    add("_rels/.rels",
        "<?xml version=\"1.0\"?><Relationships><Relationship Id=\"rId1\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
        "Target=\"xl/workbook.xml\"/></Relationships>");
    add("xl/workbook.xml",
        std::string("<?xml version=\"1.0\"?><workbook xmlns:r=\"http://schemas.openxmlformats.org/"
        "officeDocument/2006/relationships\">")
        + (date1904 ? "<workbookPr date1904=\"1\"/>" : "<workbookPr/>")
        + "<sheets><sheet name=\"S\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>");
    add("xl/_rels/workbook.xml.rels",
        "<?xml version=\"1.0\"?><Relationships>"
        "<Relationship Id=\"rId1\" Type=\"t/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"t/styles\" Target=\"styles.xml\"/></Relationships>");
    add("xl/styles.xml",
        "<?xml version=\"1.0\"?><styleSheet>"
        "<numFmts count=\"1\"><numFmt numFmtId=\"164\" formatCode=\"yyyy-mm-dd\"/></numFmts>"
        "<cellXfs count=\"6\">"
        "<xf numFmtId=\"0\"/>"     // 0 General
        "<xf numFmtId=\"14\"/>"    // 1 日期
        "<xf numFmtId=\"20\"/>"    // 2 时间
        "<xf numFmtId=\"22\"/>"    // 3 日期+时间
        "<xf numFmtId=\"9\"/>"     // 4 百分比
        "<xf numFmtId=\"164\"/>"   // 5 自定义日期
        "</cellXfs></styleSheet>");
    add("xl/worksheets/sheet1.xml",
        "<?xml version=\"1.0\"?><worksheet><sheetData>"
        "<row r=\"1\">"
        "<c r=\"A1\" s=\"1\"><v>45292</v></c>"          // 2024-01-01
        "<c r=\"B1\" s=\"2\"><v>0.5</v></c>"            // 12:00:00
        "<c r=\"C1\" s=\"3\"><v>45292.75</v></c>"       // 2024-01-01 18:00:00
        "<c r=\"D1\" s=\"4\"><v>0.12</v></c>"           // 百分比：**保持原始值**
        "<c r=\"E1\" s=\"5\"><v>45458</v></c>"          // 2024-06-15（自定义格式）
        "<c r=\"F1\"><v>45292</v></c>"                  // 无样式 ⇒ 原始数值
        "<c r=\"G1\" s=\"1\"><v>60</v></c>"             // 1900 假闰日
        "<c r=\"H1\" s=\"1\"><v>61</v></c>"             // 1900-03-01
        "</row></sheetData></worksheet>");
    std::vector<uint8_t> out;
    std::string err;
    z.SaveToMemory(out, err);
    return out;
}

void CaseXlsxFormatsDateAndTime() {
    const std::vector<uint8_t> bytes = MakeFormatFixtureXlsx(false);
    qst::ooxml::XlsxDoc doc;
    std::string err;
    if (!doc.LoadFromMemory(bytes.data(), bytes.size(), err)) {
        Emit(L"xlsx_formats_date_and_time", false, (L"载入失败：" + W(err)).c_str());
        return;
    }
    qst::ooxml::CellValue a1, b1, c1, f1;
    const bool got = doc.GetCell("S", "A1", a1, err) && doc.GetCell("S", "B1", b1, err)
        && doc.GetCell("S", "C1", c1, err) && doc.GetCell("S", "F1", f1, err);
    // 日期 → ISO 串；无样式 → 原始数值（不许乱猜格式）
    const bool ok = got
        && a1.display == "2024-01-01" && a1.number == 45292
        && b1.display == "12:00:00"
        && c1.display == "2024-01-01 18:00:00"
        && f1.display.empty() && f1.number == 45292;
    Emit(L"xlsx_formats_date_and_time", ok,
        (L"A1=" + W(a1.display) + L" B1=" + W(b1.display) + L" C1=" + W(c1.display)
            + L" F1display='" + W(f1.display) + L"' F1num=" + std::to_wstring(f1.number)).c_str());
}

void CaseXlsxFormatsCustomPercentAndLeap() {
    const std::vector<uint8_t> bytes = MakeFormatFixtureXlsx(false);
    qst::ooxml::XlsxDoc doc;
    std::string err;
    if (!doc.LoadFromMemory(bytes.data(), bytes.size(), err)) {
        Emit(L"xlsx_formats_custom_percent_and_leap", false, L"载入失败");
        return;
    }
    qst::ooxml::CellValue d1, e1, g1, h1;
    const bool got = doc.GetCell("S", "D1", d1, err) && doc.GetCell("S", "E1", e1, err)
        && doc.GetCell("S", "G1", g1, err) && doc.GetCell("S", "H1", h1, err);
    const bool ok = got
        // 百分比**不给格式化值**（0.12 就是真值，模型要拿它算数）
        && d1.display.empty() && d1.number == 0.12
        // 自定义格式 `yyyy-mm-dd` 也要认出来
        && e1.display == "2024-06-15"
        // 1900 闰年 bug：Excel 认为序列 60 是 1900-02-29（那个日期不存在，原样还原）
        && g1.display == "1900-02-29"
        && h1.display == "1900-03-01";
    Emit(L"xlsx_formats_custom_percent_and_leap", ok,
        (L"D1display='" + W(d1.display) + L"' E1=" + W(e1.display)
            + L" G1=" + W(g1.display) + L" H1=" + W(h1.display)).c_str());
}

void CaseXlsxFormats1904System() {
    const std::vector<uint8_t> bytes = MakeFormatFixtureXlsx(true);
    qst::ooxml::XlsxDoc doc;
    std::string err;
    if (!doc.LoadFromMemory(bytes.data(), bytes.size(), err)) {
        Emit(L"xlsx_formats_1904_system", false, L"载入失败");
        return;
    }
    qst::ooxml::CellValue a1, g1;
    // ⚠ 同一个序列号 45292 在**两种日期系统下差 1462 天**：
    //   1900 系统 = 2024-01-01；1904 系统 = 2028-01-02（Python 独立算过）。
    //   所以这条用例同时钉住「date1904 读到了」和「换算基准没写死」。
    const bool ok = doc.GetCell("S", "A1", a1, err) && a1.display == "2028-01-02"
        && doc.GetCell("S", "G1", g1, err) && g1.display == "1904-03-01";
    Emit(L"xlsx_formats_1904_system", ok,
        (L"<workbookPr date1904=\"1\"/>：45292 应为 2028-01-02、60 应为 1904-03-01，实得 "
            + W(a1.display) + L" / " + W(g1.display)).c_str());
}

}  // namespace

/// 诊断用：把「新建 xlsx」的样例写到一个路径，便于用**别的工具**（Excel / Python etree）
/// 独立验证产物是不是真的能打开 —— 自检只能证明「我们自己读得回来」，那是不够的。
bool EmitSampleXlsx(const std::wstring& path) {
    qst::ooxml::XlsxDoc doc;
    std::string err;
    if (!qst::ooxml::XlsxDoc::CreateNew("销售数据", doc, err)) return false;
    if (!doc.SetCellText("销售数据", "A1", "品名", err)) return false;
    if (!doc.SetCellText("销售数据", "B1", "金额", err)) return false;
    if (!doc.SetCellText("销售数据", "A2", "键盘 & 鼠标", err)) return false;   // 含实体
    if (!doc.SetCellNumber("销售数据", "B2", 199.5, err)) return false;
    if (!doc.SetCellText("销售数据", "A3", "显示器", err)) return false;
    if (!doc.SetCellNumber("销售数据", "B3", 1299, err)) return false;
    if (!doc.SetCellFormula("销售数据", "B4", "SUM(B2:B3)", "1498.5", err)) return false;
    if (!doc.SetCellNumber("销售数据", "C4", 1498.5, err)) return false;
    if (!doc.SaveToFile(path, err)) return false;
    return true;
}

int wmain(int argc, wchar_t** argv) {
    bool listOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i] ? argv[i] : L"";
        if (a == L"--json") {
            selftest::gJson = true;
            selftest::InitUtf8Stdout();
        } else if (a == L"--list") {
            listOnly = true;
            selftest::InitUtf8Stdout();
        } else if (a == L"--emit-sample" && i + 1 < argc) {
            // 诊断用，不参与断言：把样例写盘，交给别的工具独立验证
            const bool ok = EmitSampleXlsx(argv[i + 1] ? argv[i + 1] : L"");
            std::printf("emit-sample %s\n", ok ? "OK" : "FAILED");
            return ok ? 0 : 1;
        } else if (a == L"--help" || a == L"-h") {
            selftest::PrintCaseList(L"OoxmlSelfTest", kCases, sizeof(kCases) / sizeof(kCases[0]));
            return 0;
        }
    }
    if (listOnly) {
        selftest::PrintCaseList(L"OoxmlSelfTest", kCases, sizeof(kCases) / sizeof(kCases[0]));
        return 0;
    }

    CaseInflateStored();
    CaseInflateFixed();
    CaseInflateDynamic();
    CaseInflateRejectsTruncated();
    CaseInflateRejectsLengthMismatch();
    CaseZipReadsDeflateEntries();
    CaseZipReadsStoredEntry();
    CaseZipBytePreservingSave();
    CaseZipPreservesMethodAndCrc();
    CaseZipRoundtripAddedEntry();
    CaseZipRejectsZip64();

    CaseXlsxListsSheets();
    CaseXlsxReadsSharedString();
    CaseXlsxReadsNumberBoolFormula();
    CaseXlsxDumpTsv();
    CaseXlsxSetCellReplacesValue();
    CaseXlsxSetCellKeepsStyleAttr();
    CaseXlsxSetCellPreservesOtherParts();
    CaseXlsxSetCellPreservesSheetExtras();
    CaseXlsxSetCellInsertsInColumnOrder();
    CaseXlsxSetCellCreatesRowInOrder();
    CaseXlsxRoundtripAfterSave();
    CaseXlsxEscapesXmlText();
    CaseXlsxRejectsNonXlsx();
    CaseXlsxCreateNewRoundtrip();
    CaseXlsxDumpWorkbookText();
    CaseXlsxFormatsDateAndTime();
    CaseXlsxFormatsCustomPercentAndLeap();
    CaseXlsxFormats1904System();

    selftest::EmitSummary();
    return selftest::ExitCode();
}
