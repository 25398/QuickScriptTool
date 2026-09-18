#pragma once
// ──────────────────────────────────────────────────────────────────
// office_doc.h — 办公文档读取（Excel / Word / PPT / PDF / 文本）
//
// 目标：像 Codex 那样「直接读懂文件」，而不是开软件用视觉去抄。
// 实现路线（对齐 Anthropic document-skills 的思路，但落到 Windows 原生能力）：
//   · Office 装了 → 用 COM（值最准：公式结果、日期、表格结构）
//   · 没装 Office → 直接读 OOXML（xlsx/docx/pptx = zip + XML），零第三方依赖
//   · PDF → 有 pdftotext 就用它；否则把页面渲染成 PNG 交给视觉模型读
//   · 文本/CSV → 自动识别 UTF-8(BOM) / UTF-16 / 系统 ANSI(GBK)，中文不乱码
// 具体提取逻辑放在 office/read_doc.ps1（随包分发），本模块只负责调用 + 解析。
// ──────────────────────────────────────────────────────────────────

#include <string>
#include <vector>

struct OfficeDocResult {
    bool ok = false;
    /// 失败原因（给模型看，要求可执行）
    std::wstring error;
    /// xlsx / docx / pptx / pdf / csv / txt …
    std::wstring format;
    /// excel-com / word-com / ooxml / pdftotext / pdf-render / text
    std::wstring engine;
    /// 提取到的正文（表格是 制表符分隔、一行一记录）
    std::wstring text;
    /// PDF 无文本层时渲染出的页面图片路径（交给视觉模型）
    std::vector<std::wstring> images;
    /// 补充说明（截断原因、渲染页数等）
    std::wstring note;
    bool truncated = false;
};

/// 是否是我们支持的办公文档（按扩展名）
bool IsSupportedOfficeDocument(const std::wstring& path, std::wstring* formatOut = nullptr);

/// 扩展名 → 中文类型名（错误提示/日志用）
std::wstring OfficeDocFormatLabel(const std::wstring& format);

/// 读取文档正文。maxChars 为文本上限（超出会截断并标记），pageLimit 为多页文档/PDF 的页数上限。
/// imageDir 为空则用临时目录。
OfficeDocResult ReadOfficeDocument(const std::wstring& path, int maxChars = 20000,
    int pageLimit = 5, const std::wstring& imageDir = std::wstring());

/// 提取脚本路径（随包分发到 <exe目录>\office\read_doc.ps1；找不到返回空）
std::wstring OfficeDocScriptPath();
