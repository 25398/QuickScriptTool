#pragma once
// ──────────────────────────────────────────────────────────────────
// agent_attachment.h — AI 对话框附件加载与编码
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <string>
#include <vector>

#include "agent_core.h"

// ── ★「附图送进请求」的长边上限：**一处事实，只此一份** ────────────────────
// 由来（实测）：`zoom` 工具从原始分辨率裁剪回传 PNG，回执里如实写着
// 「回传图 1538×175、倍率 ×2.5、图内坐标 → upload = x1 + 图x/2.5」。可它交出去的是
// **文件**，真正进请求时 `AgentLoadAttachmentFromPath` → `BuildAttachmentFromImageMat`
// 会把长边缩到 1280 再重编 JPEG ⇒ 模型看到的其实是 1280×146，倍率是 ×2.0 而不是 ×2.5
// ⇒ 模型照回执换算出来的 upload 坐标**系统性偏移**（它按 /2.5 算，实际该 /2.0）,
// 于是「想点 600 的卡、点到 400 的卡上」。回执越自信，偏得越远。
// ⇒ 凡是「按图内坐标换算」的工具，必须用**同一个**上限把自己交出去的图钉死在这里，
//   这样「文件里的图」与「模型看到的图」逐像素相同，回执里的倍率才是真的。
// ⚠ 改这个常量只需改这一行；**别再在别处写 1280**（agent_attachment.cpp 也读它）。
constexpr int kAgentAttachmentMaxLongEdge = 1280;

/// 单轮附图**总字节预算**（docs §57）。上限原先只管张数（8 张），不管体量 ⇒
/// 实测 `图 956 KB（3 张）`、请求体 1037 KB（92% 是图），每轮上传 ~1 MB。
/// 超预算时**保新弃旧**，并在附图说明里逐张写明被丢了哪几张。
///
/// ★400 KB → 240 KB（实测再收紧）。这一条是**多轮累积成本的闸门**：
///   它和 `image_keep_rounds` 一起决定「图片在请求里能叠几层」。
///   超出的部分不是丢掉，而是**剥成文字占位**（见 agent_core.cpp 的 strip 分支），
///   模型仍然知道「那一轮有过图、要看画面就自己重新取」。
///   实测量级：zoom 两块裁剪图 = 50+25 KB、截图 22~96 KB，
///   240 KB 仍容得下「两整屏截图」或「一次多块 zoom」这类**真的需要同时看**的组合；
///   被挡掉的是「攒了 5 轮的陈旧快照」，那部分本来就只在付钱。
constexpr size_t kAgentImageBudgetBytes = 240u * 1024u;

// ── ★附图重编码的**质量**：同样是一处事实 ────────────────────────────────
// 由来（实测）：`zoom` 曾把原始分辨率裁剪**存成 PNG**（一张 1150×725 的游戏截图
// 就是 **1365 KB**），而它真正进请求时会被 `BuildAttachmentFromImageMat` 重编成
// JPEG q82（同样尺寸只剩 ~200 KB）⇒ 回执里那句「1365 KB」说的**不是**模型手里的东西。
// 现在 `zoom` 落盘就用这个质量、这个长边上限**一次编成交付形态**（`SaveHbitmapJpeg`），
// 于是「磁盘上的图」= 「模型看到的图」，回执的字节数/倍率不可能再说谎。
constexpr int kAgentAttachmentJpegQuality = 82;

struct AgentPendingAttachment {
    std::wstring path;
    std::wstring fileName;
    bool isImage = false;
    std::wstring mime;
    std::string base64;
    HBITMAP thumbnail = nullptr;
};

bool AgentIsImagePath(const std::wstring& path);
std::wstring AgentMimeTypeForPath(const std::wstring& path);
std::string AgentBase64EncodeFile(const std::wstring& path);
HBITMAP AgentCreateImageThumbnail(const std::wstring& path, int size);
void AgentReleaseAttachmentBitmap(AgentPendingAttachment& attachment);
void AgentReleaseAttachmentBitmaps(std::vector<AgentPendingAttachment>& attachments);

bool AgentLoadAttachmentFromPath(const std::wstring& path, AgentPendingAttachment& out,
    std::wstring& error);

/// 从工具结果文本中提取 [[AGENT_IMG:<绝对路径>]] 图片标记：
/// textOut 为移除标记后的文本；paths 为提取到的图片绝对路径（原顺序）。
bool AgentExtractImageMarkers(const std::wstring& text, std::wstring& textOut,
                              std::vector<std::wstring>& paths);

/// 把工具结果里带出的图片编码为 image_url parts（长边缩到
/// `kAgentAttachmentMaxLongEdge` / JPEG 82，缩略图自动释放）。
/// maxCount 限制嵌入张数；skipped 输出失败/超限说明。
/// `maxTotalBytes` 限制**这一轮图的总体量**（默认 `kAgentImageBudgetBytes`），
/// 超了就**保新弃旧**并逐张写进 skipped —— 张数管不住的正是这个（一张 zoom 就能 363 KB）。
/// labels 可选：与 paths 等长，第 i 张图成功嵌入时会在它**前面**插一段同序号的文字
/// 标题 —— 一张图一条工具结果，模型才能把图和自己那一次调用对上（原来多张图挤在
/// 一个 user 消息里、且旧实现用的是同一个文件名，模型只能看见两张一模一样的图）。
bool AgentBuildImageParts(const std::vector<std::wstring>& paths,
                          std::vector<ChatContentPart>& parts,
                          std::wstring& skipped,
                          size_t maxCount = 8,
                          const std::vector<std::wstring>* labels = nullptr,
                          size_t maxTotalBytes = kAgentImageBudgetBytes);

// 从剪贴板读取可粘贴的附件（文件路径或图片）。返回 true 表示剪贴板含附件内容。
bool AgentTryReadClipboardAttachments(HWND owner, std::vector<std::wstring>& filePaths,
    AgentPendingAttachment& imageAttachment, bool& hasImage, std::wstring& error);

ChatMessage AgentBuildUserMessage(const std::wstring& text,
    const std::vector<AgentPendingAttachment>& attachments);
