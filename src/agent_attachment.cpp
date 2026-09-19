#include "agent_attachment.h"

#include "base64.h"
#include "opencv_runtime.h"
#include "utils.h"

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <fstream>
#include <shellapi.h>
#include <vector>

namespace {

// Base64 唯一实现见 src/base64.h（原三份重复实现已收敛）
using qst::base64::Encode;

std::wstring ToLowerExt(const std::wstring& path) {
    const auto pos = path.find_last_of(L'.');
    if (pos == std::wstring::npos) return L"";
    std::wstring ext = path.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    return ext;
}

std::vector<uint8_t> ReadBinaryFile(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

HBITMAP CreateBitmapFromMatBGR(const cv::Mat& bgr, int size) {
    if (bgr.empty()) return nullptr;
    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    cv::Mat thumb;
    cv::resize(rgb, thumb, cv::Size(size, size), 0, 0, cv::INTER_AREA);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC hdc = GetDC(nullptr);
    if (!hdc) return nullptr;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!bmp || !bits) return nullptr;

    const int stride = ((size * 3 + 3) / 4) * 4;
    for (int y = 0; y < size; ++y) {
        memcpy(static_cast<uint8_t*>(bits) + y * stride,
            thumb.ptr(y), static_cast<size_t>(size * 3));
    }
    return bmp;
}

cv::Mat DecodeImageFile(const std::wstring& path) {
    if (!OpenCvAvailable()) return {};
    const auto bytes = ReadBinaryFile(path);
    if (bytes.empty()) return {};
    return cv::imdecode(bytes, cv::IMREAD_COLOR);
}

const uint8_t* DibPixelBytes(const BITMAPINFOHEADER* hdr) {
    const int colorCount = hdr->biClrUsed
        ? static_cast<int>(hdr->biClrUsed)
        : (hdr->biBitCount <= 8 ? (1 << hdr->biBitCount) : 0);
    return reinterpret_cast<const uint8_t*>(hdr) + hdr->biSize
        + static_cast<size_t>(colorCount) * sizeof(RGBQUAD);
}

cv::Mat MatFromBitmap(HBITMAP hbmp) {
    if (!hbmp || !OpenCvAvailable()) return {};
    BITMAP bm{};
    if (!GetObject(hbmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return {};

    // 32bpp：避免 24bpp GetDIBits 与 CV_8UC3 行对齐不一致写越界
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bm.bmWidth;
    bmi.bmiHeader.biHeight = -bm.bmHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    cv::Mat bgra(bm.bmHeight, bm.bmWidth, CV_8UC4);
    HDC hdc = GetDC(nullptr);
    if (!hdc) return {};
    const int lines = GetDIBits(hdc, hbmp, 0, static_cast<UINT>(bm.bmHeight),
        bgra.ptr(), &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    if (lines <= 0) return {};

    cv::Mat bgr;
    cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
    return bgr;
}

cv::Mat MatFromDib(HGLOBAL hMem) {
    if (!hMem || !OpenCvAvailable()) return {};
    const void* locked = GlobalLock(hMem);
    if (!locked) return {};
    const auto* hdr = static_cast<const BITMAPINFOHEADER*>(locked);
    if (hdr->biSize < sizeof(BITMAPINFOHEADER)) {
        GlobalUnlock(hMem);
        return {};
    }

    const int w = hdr->biWidth;
    const int h = std::abs(hdr->biHeight);
    if (w <= 0 || h <= 0) {
        GlobalUnlock(hMem);
        return {};
    }

    const uint8_t* pixels = DibPixelBytes(hdr);
    cv::Mat result;
    if (hdr->biBitCount == 32) {
        cv::Mat src(h, w, CV_8UC4, const_cast<uint8_t*>(pixels));
        cv::Mat bgr;
        cv::cvtColor(src, bgr, cv::COLOR_BGRA2BGR);
        if (hdr->biHeight > 0) cv::flip(bgr, result, 0);
        else result = bgr;
    } else if (hdr->biBitCount == 24) {
        const int stride = ((w * 3 + 3) / 4) * 4;
        result.create(h, w, CV_8UC3);
        for (int y = 0; y < h; ++y) {
            const int srcY = hdr->biHeight > 0 ? (h - 1 - y) : y;
            memcpy(result.ptr(y), pixels + static_cast<size_t>(srcY) * stride, static_cast<size_t>(w * 3));
        }
    } else {
        HDC hdc = GetDC(nullptr);
        if (!hdc) {
            GlobalUnlock(hMem);
            return {};
        }
        const auto* bmi = reinterpret_cast<const BITMAPINFO*>(hdr);
        HBITMAP srcBmp = CreateDIBitmap(hdc, hdr, CBM_INIT, pixels, bmi, DIB_RGB_COLORS);
        GlobalUnlock(hMem);
        if (!srcBmp) {
            ReleaseDC(nullptr, hdc);
            return {};
        }
        result = MatFromBitmap(srcBmp);
        DeleteObject(srcBmp);
        ReleaseDC(nullptr, hdc);
        return result;
    }

    GlobalUnlock(hMem);
    return result;
}

std::vector<std::wstring> PathsFromHDrop(HDROP hDrop) {
    std::vector<std::wstring> paths;
    if (!hDrop) return paths;
    const UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        const UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
        std::wstring path(len, L'\0');
        DragQueryFileW(hDrop, i, path.data(), len + 1);
        path.resize(len);
        paths.push_back(path);
    }
    return paths;
}

bool BuildAttachmentFromImageMat(const cv::Mat& bgr, const std::wstring& fileName,
    AgentPendingAttachment& out, std::wstring& error) {
    if (bgr.empty()) {
        error = L"图片数据为空。";
        return false;
    }

    cv::Mat apiImage = bgr;
    constexpr int kMaxLongEdge = 1280;
    const int longEdge = std::max(apiImage.cols, apiImage.rows);
    if (longEdge > kMaxLongEdge) {
        const double scale = static_cast<double>(kMaxLongEdge) / longEdge;
        cv::Mat resized;
        cv::resize(apiImage, resized, cv::Size(), scale, scale, cv::INTER_AREA);
        apiImage = resized;
    }

    std::vector<uint8_t> encoded;
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 82};
    if (!cv::imencode(".jpg", apiImage, encoded, params)) {
        error = L"无法编码图片。";
        return false;
    }

    AgentPendingAttachment item;
    item.fileName = fileName;
    item.isImage = true;
    item.mime = L"image/jpeg";
    item.base64 = Encode(encoded);
    item.thumbnail = CreateBitmapFromMatBGR(bgr, 40);
    if (item.base64.empty()) {
        error = L"无法读取图片。";
        AgentReleaseAttachmentBitmap(item);
        return false;
    }

    out = std::move(item);
    return true;
}

}  // namespace

bool AgentIsImagePath(const std::wstring& path) {
    const std::wstring ext = ToLowerExt(path);
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".gif"
        || ext == L".bmp" || ext == L".webp" || ext == L".tif" || ext == L".tiff";
}

std::wstring AgentMimeTypeForPath(const std::wstring& path) {
    const std::wstring ext = ToLowerExt(path);
    if (ext == L".png") return L"image/png";
    if (ext == L".jpg" || ext == L".jpeg") return L"image/jpeg";
    if (ext == L".gif") return L"image/gif";
    if (ext == L".bmp") return L"image/bmp";
    if (ext == L".webp") return L"image/webp";
    if (ext == L".tif" || ext == L".tiff") return L"image/tiff";
    if (ext == L".json") return L"application/json";
    if (ext == L".txt") return L"text/plain";
    return L"application/octet-stream";
}

std::string AgentBase64EncodeFile(const std::wstring& path) {
    return Encode(ReadBinaryFile(path));
}

HBITMAP AgentCreateImageThumbnail(const std::wstring& path, int size) {
    return CreateBitmapFromMatBGR(DecodeImageFile(path), size);
}

void AgentReleaseAttachmentBitmap(AgentPendingAttachment& attachment) {
    if (attachment.thumbnail) {
        DeleteObject(attachment.thumbnail);
        attachment.thumbnail = nullptr;
    }
}

void AgentReleaseAttachmentBitmaps(std::vector<AgentPendingAttachment>& attachments) {
    for (auto& item : attachments) AgentReleaseAttachmentBitmap(item);
    attachments.clear();
}

bool AgentLoadAttachmentFromPath(const std::wstring& path, AgentPendingAttachment& out,
    std::wstring& error) {
    if (path.empty()) {
        error = L"文件路径为空。";
        return false;
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"文件不存在：" + path;
        return false;
    }

    AgentPendingAttachment item;
    item.path = path;
    const auto slash = path.find_last_of(L"\\/");
    item.fileName = slash == std::wstring::npos ? path : path.substr(slash + 1);
    item.isImage = AgentIsImagePath(path);
    item.mime = AgentMimeTypeForPath(path);

    if (item.isImage) {
        const cv::Mat bgr = DecodeImageFile(path);
        if (bgr.empty()) {
            error = L"无法读取图片：" + item.fileName;
            return false;
        }
        const std::wstring savedPath = item.path;
        if (!BuildAttachmentFromImageMat(bgr, item.fileName, item, error))
            return false;
        item.path = savedPath;
        item.thumbnail = AgentCreateImageThumbnail(path, 40);
    } else {
        const auto bytes = ReadBinaryFile(path);
        if (bytes.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            // empty file allowed
        }
        if (bytes.size() > 512 * 1024) {
            error = L"文本附件过大（上限 512KB）：" + item.fileName;
            return false;
        }
    }

    out = std::move(item);
    return true;
}

bool AgentTryReadClipboardAttachments(HWND owner, std::vector<std::wstring>& filePaths,
    AgentPendingAttachment& imageAttachment, bool& hasImage, std::wstring& error) {
    filePaths.clear();
    hasImage = false;
    error.clear();
    AgentReleaseAttachmentBitmap(imageAttachment);
    imageAttachment = {};

    if (!OpenClipboard(owner)) return false;

    bool hasAttachFormat = false;
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        hasAttachFormat = true;
        filePaths = PathsFromHDrop(static_cast<HDROP>(GetClipboardData(CF_HDROP)));
    } else if (IsClipboardFormatAvailable(CF_DIB)) {
        hasAttachFormat = true;
        const HGLOBAL hMem = static_cast<HGLOBAL>(GetClipboardData(CF_DIB));
        const cv::Mat mat = MatFromDib(hMem);
        if (!BuildAttachmentFromImageMat(mat, L"粘贴的图片.png", imageAttachment, error))
            hasImage = false;
        else
            hasImage = true;
    } else if (IsClipboardFormatAvailable(CF_BITMAP)) {
        hasAttachFormat = true;
        const HBITMAP hbmp = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
        const cv::Mat mat = MatFromBitmap(hbmp);
        if (!BuildAttachmentFromImageMat(mat, L"粘贴的图片.png", imageAttachment, error))
            hasImage = false;
        else
            hasImage = true;
    }

    CloseClipboard();
    if (!hasAttachFormat) return false;
    return !filePaths.empty() || hasImage || !error.empty();
}

ChatMessage AgentBuildUserMessage(const std::wstring& text,
    const std::vector<AgentPendingAttachment>& attachments) {
    ChatMessage msg;
    msg.role = L"user";

    if (attachments.empty()) {
        msg.content = text;
        return msg;
    }

    if (!text.empty()) {
        ChatContentPart textPart;
        textPart.type = L"text";
        textPart.text = text;
        msg.parts.push_back(textPart);
    }

    for (const auto& att : attachments) {
        if (att.isImage) {
            ChatContentPart imagePart;
            imagePart.type = L"image_url";
            imagePart.image_url = L"data:" + att.mime + L";base64," + FromUtf8(att.base64);
            msg.parts.push_back(imagePart);
        } else {
            std::wstring fileText = ReadAll(att.path);
            if (fileText.size() > 512 * 1024) {
                fileText = fileText.substr(0, 512 * 1024) + L"\n...(内容已截断)";
            }
            ChatContentPart filePart;
            filePart.type = L"text";
            filePart.text = L"[附件文件: " + att.fileName + L"]\n" + fileText;
            msg.parts.push_back(filePart);
        }
    }

    if (msg.parts.empty()) msg.content = text;
    return msg;
}

bool AgentExtractImageMarkers(const std::wstring& text, std::wstring& textOut,
                              std::vector<std::wstring>& paths) {
    textOut = text;
    paths.clear();
    const std::wstring open = L"[[AGENT_IMG:";
    const std::wstring close = L"]]";
    bool found = false;
    for (;;) {
        const size_t start = textOut.find(open);
        if (start == std::wstring::npos) break;
        const size_t end = textOut.find(close, start + open.size());
        if (end == std::wstring::npos) break;
        std::wstring path = Trim(textOut.substr(start + open.size(),
            end - start - open.size()));
        textOut.erase(start, end + close.size() - start);
        if (!path.empty()) {
            paths.push_back(path);
            found = true;
        }
    }
    return found;
}

bool AgentBuildImageParts(const std::vector<std::wstring>& paths,
                          std::vector<ChatContentPart>& parts,
                          std::wstring& skipped,
                          size_t maxCount) {
    if (paths.empty()) return false;
    if (maxCount == 0) maxCount = 8;
    size_t embedded = 0;
    for (const auto& path : paths) {
        if (embedded >= maxCount) {
            if (!skipped.empty()) skipped += L"\n";
            skipped += L"[已跳过] 图片超过单轮嵌入上限（" + std::to_wstring(maxCount)
                + L"）：" + path;
            continue;
        }
        AgentPendingAttachment item;
        std::wstring error;
        if (!AgentLoadAttachmentFromPath(path, item, error)) {
            if (!skipped.empty()) skipped += L"\n";
            skipped += L"[读取失败] " + path + L"（" + error + L"）";
            continue;
        }
        if (!item.isImage || item.base64.empty()) {
            AgentReleaseAttachmentBitmap(item);
            if (!skipped.empty()) skipped += L"\n";
            skipped += L"[跳过] 非图片或编码失败：" + path;
            continue;
        }
        ChatContentPart part;
        part.type = L"image_url";
        part.image_url = L"data:" + item.mime + L";base64," + FromUtf8(item.base64);
        AgentReleaseAttachmentBitmap(item);
        parts.push_back(std::move(part));
        ++embedded;
    }
    return embedded > 0;
}
