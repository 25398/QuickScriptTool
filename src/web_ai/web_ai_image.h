#pragma once
// ──────────────────────────────────────────────────────────────────
// web_ai_image.h — 网页版 AI 的**传图通道**（data URL → 临时文件 → CDP 挂到 file input）
//
// 为什么需要这一层（设计文档 §13.6 第 1 条）：
//   请求里的 `image_url` 是 `data:image/png;base64,...`，而网页输入框只吃**文本**。
//   要让网页 AI 真的看到图，只能把图落到磁盘，再用 CDP 的
//   `DOM.setFileInputFiles` 挂到页面的 `<input type=file>` 上。
//   ⚠ 这是唯一可行的路径：浏览器安全模型**不允许** JS 给 file input 赋文件
//     （`input.files` 是只读的 FileList，构造不了 File 塞进去 —— 塞了也不算"用户选择"，
//      且多数框架会忽略）。CDP 走的是浏览器进程的真实文件选择管线，才会派发
//      input/change 事件，页面才认。
//
// 分层（便于自检）：
//   · `Base64Decode` / `ExtractImagesFromMessages` —— **纯逻辑**，不碰盘、不碰 Win32
//     ⇒ `WebAiSelfTest` 可以逐格断言（本仓硬规则：纯逻辑必须能被单测钉住）。
//   · `UploadDir` / `WriteTempImage` / `PruneUploadDir` —— 碰盘，但只碰**我们自己的**
//     临时目录（`%TEMP%\QuickScriptTool\web_ai_uploads`），且只删带 `qst_webai_` 前缀的文件。
// ──────────────────────────────────────────────────────────────────

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace quickscript::webai {

/// 一张从请求里解出来的图
struct WebAiImageBlob {
    std::string mime;   ///< 如 `image/png`（缺省 `image/png`）
    std::string ext;    ///< 落盘用的扩展名（含点，如 `.png`）
    std::string data;   ///< **已解码**的原始字节
};

/// 标准/URL-safe base64 解码（容忍 `\r\n` / 空格 / 缺省 padding）。
/// @return false = 含非法字符或长度非法（**不**静默截断：宁可报错也别把半张图发出去）
bool Base64Decode(const std::string& in, std::string& out);

/// mime → 扩展名（`image/jpeg` → `.jpg`）。未知 image/* 一律 `.png`（浏览器按内容嗅探）。
std::string ImageExtForMime(const std::string& mime);

/// 从 OpenAI `messages` 里抽出所有**内联**图片（`data:<mime>;base64,<b64>`）。
///
/// ⚠ 只认 data URL：`http(s)://` 的图**一律跳过并记账** —— 我们不去下载别人的图
///   （那会把用户的 cookie/UA 带出去，属于越权行为）。跳过原因写进 `note`。
///
/// 上限语义与 `AgentBuildImageParts` 一致：**保新弃旧**（越靠后的消息越新）。
///
/// @param maxCount       最多取几张（超出写进 note）
/// @param maxTotalBytes  解码后总字节上限（超出写进 note）
/// @param note           出参：人类可读的"发生了什么"（空 = 一切正常、没图也没被砍）
/// @return 实际解出来的张数（== out.size()）
int ExtractImagesFromMessages(const nlohmann::json& messages,
                              std::vector<WebAiImageBlob>& out,
                              int maxCount, size_t maxTotalBytes,
                              std::string& note);

/// 我们自己的临时上传目录（**保证存在**；失败返回空串）。
std::wstring UploadDir();

/// 把一张图解到临时文件，返回**绝对路径**（失败返回空串并写 err）。
/// 文件名形如 `qst_webai_<tick>_<n>.<ext>` —— 前缀是清理时的判据，别改。
std::wstring WriteTempImage(const WebAiImageBlob& blob, std::string& err);

/// 清理本目录里**超过 olderThanHours 小时**的 `qst_webai_*` 文件。
/// ⚠ 只删本目录、只删本前缀、不递归、不删子目录（临时目录里可能有别人的东西）。
/// @return 删掉的个数
int PruneUploadDir(int olderThanHours);

}  // namespace quickscript::webai
