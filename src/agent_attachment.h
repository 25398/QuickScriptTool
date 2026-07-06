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

// 从剪贴板读取可粘贴的附件（文件路径或图片）。返回 true 表示剪贴板含附件内容。
bool AgentTryReadClipboardAttachments(HWND owner, std::vector<std::wstring>& filePaths,
    AgentPendingAttachment& imageAttachment, bool& hasImage, std::wstring& error);

ChatMessage AgentBuildUserMessage(const std::wstring& text,
    const std::vector<AgentPendingAttachment>& attachments);
