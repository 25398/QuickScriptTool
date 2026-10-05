// ──────────────────────────────────────────────────────────────────
// web_ai_image.cpp — 传图通道：data URL 解码 / 抽取 / 落临时文件
//
// ⚠ 本文件分成两半，别混：
//   · 上半（Base64Decode / ImageExtForMime / ExtractImagesFromMessages）**纯逻辑**，
//     编进 `WebAiSelfTest`，任何机器上结果都一样。
//   · 下半（UploadDir / WriteTempImage / PruneUploadDir）碰 Win32 文件系统，
//     只进产品库。
// ──────────────────────────────────────────────────────────────────

#include "web_ai/web_ai_image.h"

#include "utils.h"

#include <windows.h>

#include <cstdio>
#include <fstream>

namespace quickscript::webai {

namespace {

constexpr wchar_t kUploadSubDir[] = L"QuickScriptTool\\web_ai_uploads";
/// 清理时的判据前缀。⚠ 改这里必须同步 `WriteTempImage` 的命名，否则旧文件永远删不掉。
constexpr wchar_t kUploadPrefix[] = L"qst_webai_";

int B64Value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    // URL-safe 变体（部分客户端会用；同一张图两种编码都要能解）
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

}  // namespace

bool Base64Decode(const std::string& in, std::string& out) {
    out.clear();
    // 预分配：base64 膨胀比 4/3，减掉可能的换行
    out.reserve(in.size() / 4 * 3 + 4);

    int acc = 0;      // 累积的 6 位组
    int nbits = 0;    // acc 里已有的位数
    int pad = 0;      // 遇到的 '=' 个数
    for (const char chRaw : in) {
        const unsigned char ch = static_cast<unsigned char>(chRaw);
        if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') continue;
        if (ch == '=') {
            ++pad;
            if (pad > 2) return false;  // 超过两个 padding ⇒ 非法
            continue;
        }
        if (pad > 0) return false;      // padding 之后还有数据 ⇒ 非法
        const int v = B64Value(ch);
        if (v < 0) return false;        // 非法字符 ⇒ **报错**，不静默丢
        acc = (acc << 6) | v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            out.push_back(static_cast<char>((acc >> nbits) & 0xFF));
        }
    }
    // 收尾：剩下的位必须是「全 0 的补齐位」，否则说明输入被截断
    if (nbits >= 6) return false;
    if (nbits > 0 && ((acc & ((1 << nbits) - 1)) != 0)) return false;
    return true;
}

std::string ImageExtForMime(const std::string& mime) {
    std::string m;
    m.reserve(mime.size());
    for (const char c : mime) m.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (m == "image/jpeg" || m == "image/jpg") return ".jpg";
    if (m == "image/png") return ".png";
    if (m == "image/gif") return ".gif";
    if (m == "image/webp") return ".webp";
    if (m == "image/bmp" || m == "image/x-ms-bmp") return ".bmp";
    // 未知 image/*：按 png 落盘（浏览器/网页按**内容**嗅探，扩展名只影响观感）
    return ".png";
}

int ExtractImagesFromMessages(const nlohmann::json& messages,
                              std::vector<WebAiImageBlob>& out,
                              int maxCount, size_t maxTotalBytes,
                              std::string& note) {
    out.clear();
    note.clear();
    if (!messages.is_array()) return 0;

    int skippedRemote = 0;   // http(s) 的图（我们不去下载）
    int skippedBad = 0;      // 解码失败
    int droppedByCount = 0;  // 超出张数上限
    int droppedByBytes = 0;  // 超出体量上限

    // ⚠ 保新弃旧：先从后往前收集，最后再反转回来（保持原顺序给扩展）
    std::vector<WebAiImageBlob> picked;
    size_t totalBytes = 0;

    for (auto mi = messages.rbegin(); mi != messages.rend(); ++mi) {
        const auto& msg = *mi;
        if (!msg.is_object()) continue;
        const auto cit = msg.find("content");
        if (cit == msg.end() || !cit->is_array()) continue;
        for (auto pi = cit->rbegin(); pi != cit->rend(); ++pi) {
            const auto& part = *pi;
            if (!part.is_object()) continue;
            const std::string type = part.value("type", "");
            if (type != "image_url" && type != "image" && type != "input_image") continue;

            // 取出 url：兼容三种形状
            //   {"type":"image_url","image_url":{"url":"data:..."}}   ← OpenAI
            //   {"type":"image_url","image_url":"data:..."}           ← 简写
            //   {"type":"image","image_url":"data:..."}
            std::string url;
            const auto iu = part.find("image_url");
            if (iu != part.end()) {
                if (iu->is_object()) url = iu->value("url", "");
                else if (iu->is_string()) url = iu->get<std::string>();
            }
            if (url.empty()) url = part.value("url", "");
            if (url.empty()) url = part.value("image", "");

            if (url.rfind("data:", 0) != 0) {
                if (!url.empty()) ++skippedRemote;
                continue;
            }
            const size_t semi = url.find(";base64,");
            if (semi == std::string::npos) {
                ++skippedBad;
                continue;
            }
            std::string mime = url.substr(5, semi - 5);
            if (mime.empty()) mime = "image/png";
            // 非图片的 data URL（如 data:text/plain）不是我们要的
            if (mime.rfind("image/", 0) != 0) {
                ++skippedBad;
                continue;
            }

            WebAiImageBlob blob;
            blob.mime = mime;
            blob.ext = ImageExtForMime(mime);
            if (!Base64Decode(url.substr(semi + 8), blob.data) || blob.data.empty()) {
                ++skippedBad;
                continue;
            }

            if (maxCount > 0 && static_cast<int>(picked.size()) >= maxCount) {
                ++droppedByCount;
                continue;
            }
            if (maxTotalBytes > 0 && totalBytes + blob.data.size() > maxTotalBytes) {
                ++droppedByBytes;
                continue;
            }
            totalBytes += blob.data.size();
            picked.push_back(std::move(blob));
        }
    }

    out.assign(picked.rbegin(), picked.rend());  // 反转为**原顺序**

    // 记账：只有真发生了事才写 note（空 note = 一切正常）
    std::string n;
    auto add = [&n](const std::string& s) {
        if (!n.empty()) n += "；";
        n += s;
    };
    if (skippedRemote > 0) {
        add("跳过 " + std::to_string(skippedRemote) + " 张**网络图片**（本通道不下载远程 URL）");
    }
    if (skippedBad > 0) add("跳过 " + std::to_string(skippedBad) + " 张无法解码的图");
    if (droppedByCount > 0) {
        add("超出张数上限丢弃 " + std::to_string(droppedByCount) + " 张（保新弃旧）");
    }
    if (droppedByBytes > 0) {
        add("超出体量上限丢弃 " + std::to_string(droppedByBytes) + " 张（保新弃旧）");
    }
    note = n;
    return static_cast<int>(out.size());
}

// ──────────────────────────────────────────────────────────────────
// 以下碰盘
// ──────────────────────────────────────────────────────────────────

std::wstring UploadDir() {
    wchar_t tmp[MAX_PATH] = {0};
    const DWORD n = ::GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring dir = tmp;
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    dir += kUploadSubDir;
    if (!EnsureDirectoryTree(dir)) return std::wstring();
    return dir;
}

std::wstring WriteTempImage(const WebAiImageBlob& blob, std::string& err) {
    err.clear();
    if (blob.data.empty()) {
        err = "图片字节为空";
        return std::wstring();
    }
    const std::wstring dir = UploadDir();
    if (dir.empty()) {
        err = "无法创建临时上传目录（%TEMP% 不可写？）";
        return std::wstring();
    }

    static volatile LONG s_seq = 0;
    const LONG seq = ::InterlockedIncrement(&s_seq);
    const unsigned long long tick = ::GetTickCount64();

    wchar_t name[128] = {0};
    _snwprintf_s(name, _countof(name), _TRUNCATE, L"%s%llu_%ld%s",
        kUploadPrefix, tick, static_cast<long>(seq), FromUtf8(blob.ext).c_str());

    const std::wstring path = dir + L"\\" + name;
    std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "打不开临时文件（" + ToUtf8(path) + "）";
        return std::wstring();
    }
    f.write(blob.data.data(), static_cast<std::streamsize>(blob.data.size()));
    f.close();
    if (!f) {
        err = "写临时文件失败（磁盘满？）";
        ::DeleteFileW(path.c_str());
        return std::wstring();
    }
    return path;
}

int PruneUploadDir(int olderThanHours) {
    const std::wstring dir = UploadDir();
    if (dir.empty()) return 0;
    if (olderThanHours < 1) olderThanHours = 1;

    FILETIME nowFt;
    ::GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now;
    now.LowPart = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;
    // FILETIME 是 100ns 单位 ⇒ 1 小时 = 3600 * 10^7
    const unsigned long long cutoff = static_cast<unsigned long long>(olderThanHours) * 3600ULL * 10000000ULL;

    const std::wstring pattern = dir + L"\\" + kUploadPrefix + L"*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int removed = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;  // 不递归、不动目录
        ULARGE_INTEGER w;
        w.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        w.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        if (now.QuadPart <= w.QuadPart) continue;                    // 时钟回拨，保守跳过
        if (now.QuadPart - w.QuadPart < cutoff) continue;            // 还没过期
        const std::wstring full = dir + L"\\" + fd.cFileName;
        if (::DeleteFileW(full.c_str())) ++removed;
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
    return removed;
}

}  // namespace quickscript::webai
