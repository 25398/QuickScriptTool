#pragma once
// ──────────────────────────────────────────────────────────────────
// agent_attachment.h — AI 对话框附件加载与编码
// ──────────────────────────────────────────────────────────────────

#include <windows.h>

#include <string>
#include <vector>

#include "agent_core.h"

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

/// 把脚本引用图片编码为 image_url parts（缩放 1280 / JPEG 82，缩略图自动释放）。
/// maxCount 限制嵌入张数；skipped 输出失败/超限说明。
bool AgentBuildImageParts(const std::vector<std::wstring>& paths,
                          std::vector<ChatContentPart>& parts,
                          std::wstring& skipped,
                          size_t maxCount = 8);

// 从剪贴板读取可粘贴的附件（文件路径或图片）。返回 true 表示剪贴板含附件内容。
bool AgentTryReadClipboardAttachments(HWND owner, std::vector<std::wstring>& filePaths,
    AgentPendingAttachment& imageAttachment, bool& hasImage, std::wstring& error);

ChatMessage AgentBuildUserMessage(const std::wstring& text,
    const std::vector<AgentPendingAttachment>& attachments);
