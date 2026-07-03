// ── 通用工具函数实现 ──────────────────────────────────────────
#include "utils.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_set>

// ── 字符串处理 ────────────────────────────────────────────────────
std::wstring Trim(const std::wstring& value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

// ── 路径工具 ──────────────────────────────────────────────────────
std::wstring AppDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring value(path);
    const auto slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : value.substr(0, slash);
}

std::wstring ScriptsDir() { return AppDir() + L"\\scripts"; }
std::wstring RecordingsDir() { return AppDir() + L"\\recordings"; }

void EnsureScriptsDir() {
    CreateDirectoryW(ScriptsDir().c_str(), nullptr);
    CreateDirectoryW(RecordingsDir().c_str(), nullptr);
}

std::wstring NowText() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04d/%02d/%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

std::wstring TimestampName() {
    using namespace std::chrono;
    const auto ts = duration_cast<seconds>(
        system_clock::now().time_since_epoch()).count();
    return L"鼠标宏-" + std::to_wstring(ts);
}

// ── 窗口文本操作 ──────────────────────────────────────────────────
std::wstring GetText(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return L"";
    // 分配 len+1 个字符以确保 GetWindowTextW 写入的空终止符有足够空间
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(static_cast<size_t>(len)); // 去掉尾部的空终止符
    return text;
}

void SetText(HWND hwnd, const std::wstring& text) {
    SetWindowTextW(hwnd, text.c_str());
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return "";
    const int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1,
        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1,
        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) return L"";
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
        static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
        static_cast<int>(text.size()), out.data(), len);
    return out;
}

// ── 数值转换 ──────────────────────────────────────────────────────
int ToInt(HWND edit, int fallback) {
    try {
        const auto text = Trim(GetText(edit));
        return text.empty() ? fallback : std::stoi(text);
    } catch (...) {
        return fallback;
    }
}

double ToDouble(HWND edit, double fallback) {
    try {
        const auto text = Trim(GetText(edit));
        return text.empty() ? fallback : std::stod(text);
    } catch (...) {
        return fallback;
    }
}

std::wstring F3(double value) {
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(3) << value;
    return ss.str();
}

// ── JSON 转义 ─────────────────────────────────────────────────────
std::wstring EscapeJson(const std::wstring& value) {
    std::wstringstream out;
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\': out << L"\\\\"; break;
        case L'\"': out << L"\\\""; break;
        case L'\n': out << L"\\n";  break;
        case L'\r': out << L"\\r";  break;
        case L'\t': out << L"\\t";  break;
        default:    out << ch;      break;
        }
    }
    return out.str();
}

std::wstring UnescapeJson(const std::wstring& value) {
    std::wstring out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == L'\\' && i + 1 < value.size()) {
            switch (value[i + 1]) {
            case L'n': out.push_back(L'\n'); ++i; break;
            case L'r': out.push_back(L'\r'); ++i; break;
            case L't': out.push_back(L'\t'); ++i; break;
            case L'\\': out.push_back(L'\\'); ++i; break;
            case L'\"': out.push_back(L'\"'); ++i; break;
            default: out.push_back(value[i]); break;
            }
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

namespace {

size_t FindJsonStringEnd(const std::wstring& src, size_t quotePos) {
    for (size_t i = quotePos + 1; i < src.size(); ++i) {
        if (src[i] == L'\\' && i + 1 < src.size()) {
            ++i;
            continue;
        }
        if (src[i] == L'"') return i;
    }
    return std::wstring::npos;
}

size_t FindMatchingJsonBrace(const std::wstring& src, size_t openPos) {
    int depth = 0;
    for (size_t i = openPos; i < src.size(); ++i) {
        if (src[i] == L'"') {
            const size_t end = FindJsonStringEnd(src, i);
            if (end == std::wstring::npos) return std::wstring::npos;
            i = end;
            continue;
        }
        if (src[i] == L'{') ++depth;
        else if (src[i] == L'}') {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::wstring::npos;
}

}  // namespace

std::vector<std::wstring> ExtractJsonActionBlocks(const std::wstring& content) {
    std::vector<std::wstring> blocks;
    const auto actionsKey = content.find(L"\"actions\"");
    if (actionsKey == std::wstring::npos) return blocks;
    const auto arrayStart = content.find(L'[', actionsKey);
    if (arrayStart == std::wstring::npos) return blocks;

    size_t pos = arrayStart + 1;
    while (pos < content.size()) {
        const auto objStart = content.find(L'{', pos);
        if (objStart == std::wstring::npos) break;
        const auto objEnd = FindMatchingJsonBrace(content, objStart);
        if (objEnd == std::wstring::npos) break;
        blocks.push_back(content.substr(objStart, objEnd - objStart + 1));
        pos = objEnd + 1;
    }
    return blocks;
}

// ── 文件操作 ──────────────────────────────────────────────────────
std::wstring ReadAll(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return L"";
    std::stringstream ss;
    ss << file.rdbuf();
    std::string bytes = ss.str();
    if (bytes.size() >= 3
        && static_cast<unsigned char>(bytes[0]) == 0xEF
        && static_cast<unsigned char>(bytes[1]) == 0xBB
        && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    return FromUtf8(bytes);
}

std::wstring ExtractString(const std::wstring& src,
                            const std::wstring& key) {
    const auto pos = src.find(L"\"" + key + L"\"");
    if (pos == std::wstring::npos) return L"";
    const auto colon = src.find(L':', pos);
    const auto first = src.find(L'\"', colon + 1);
    if (first == std::wstring::npos) return L"";
    const size_t second = FindJsonStringEnd(src, first);
    if (second == std::wstring::npos) return L"";
    return UnescapeJson(src.substr(first + 1, second - first - 1));
}

double ExtractNumber(const std::wstring& src,
                      const std::wstring& key, double fallback) {
    const auto pos = src.find(L"\"" + key + L"\"");
    if (pos == std::wstring::npos) return fallback;
    const auto colon = src.find(L':', pos);
    const auto end = src.find_first_of(L",}\n", colon + 1);
    try {
        return std::stod(Trim(src.substr(colon + 1, end - colon - 1)));
    } catch (...) {
        return fallback;
    }
}

int CountActionsInJson(const std::wstring& content) {
    return static_cast<int>(ExtractJsonActionBlocks(content).size());
}

std::wstring UpdateJsonStringField(const std::wstring& content,
                                    const std::wstring& key,
                                    const std::wstring& value) {
    const auto pos = content.find(L"\"" + key + L"\"");
    if (pos == std::wstring::npos) return content;
    const auto colon = content.find(L':', pos);
    if (colon == std::wstring::npos) return content;
    const auto firstQuote = content.find(L'\"', colon + 1);
    if (firstQuote == std::wstring::npos) return content;
    const size_t secondQuote = FindJsonStringEnd(content, firstQuote);
    if (secondQuote == std::wstring::npos) return content;
    std::wstring out = content;
    out.replace(firstQuote + 1, secondQuote - firstQuote - 1, EscapeJson(value));
    return out;
}

// ── 图片管理 ──────────────────────────────────────────────────────
std::wstring FindImagesDir() {
    return ScriptsDir() + L"\\images";
}

void EnsureFindImagesDir() {
    CreateDirectoryW(ScriptsDir().c_str(), nullptr);
    CreateDirectoryW(FindImagesDir().c_str(), nullptr);
}

bool IsPathInImageDir(const std::wstring& path) {
    if (path.empty()) return false;
    const std::wstring imgDir = FindImagesDir() + L"\\";
    return path.size() >= imgDir.size() &&
           _wcsnicmp(path.c_str(), imgDir.c_str(), imgDir.size()) == 0;
}

static bool IsAbsolutePath(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') return true;
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') return true;
    return false;
}

std::wstring ResolveImagePath(const std::wstring& stored) {
    if (stored.empty()) return L"";
    if (IsAbsolutePath(stored)) return stored;
    std::wstring normalized = stored;
    for (wchar_t& ch : normalized) {
        if (ch == L'/') ch = L'\\';
    }
    if (normalized.rfind(L"images\\", 0) == 0) {
        return ScriptsDir() + L"\\" + normalized;
    }
    if (normalized.find(L'\\') == std::wstring::npos) {
        return FindImagesDir() + L"\\" + normalized;
    }
    return ScriptsDir() + L"\\" + normalized;
}

std::wstring ImagePathForJson(const std::wstring& absolutePath) {
    if (absolutePath.empty()) return L"";
    std::wstring normalized = absolutePath;
    for (wchar_t& ch : normalized) {
        if (ch == L'/') ch = L'\\';
    }
    const std::wstring imgDir = FindImagesDir();
    if (_wcsnicmp(normalized.c_str(), imgDir.c_str(), imgDir.size()) == 0) {
        const wchar_t* rest = normalized.c_str() + imgDir.size();
        if (*rest == L'\\') ++rest;
        if (*rest == L'\0') return L"";
        return L"images\\" + std::wstring(rest);
    }
    const auto slash = normalized.find_last_of(L"\\/");
    const std::wstring fileName = slash == std::wstring::npos ? normalized : normalized.substr(slash + 1);
    return fileName.empty() ? L"" : L"images\\" + fileName;
}

std::wstring EnsureImageInLibrary(const std::wstring& path) {
    if (path.empty()) return L"";
    const std::wstring resolved = ResolveImagePath(path);
    if (IsPathInImageDir(resolved) &&
        GetFileAttributesW(resolved.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return resolved;
    }
    const std::wstring source = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES ? path : resolved;
    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) return resolved;

    EnsureFindImagesDir();
    const auto slash = source.find_last_of(L"\\/");
    std::wstring fileName = slash == std::wstring::npos ? source : source.substr(slash + 1);
    if (fileName.empty()) return resolved;

    std::wstring dest = FindImagesDir() + L"\\" + fileName;
    if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const auto dot = fileName.find_last_of(L'.');
        const std::wstring stem = dot == std::wstring::npos ? fileName : fileName.substr(0, dot);
        const std::wstring ext = dot == std::wstring::npos ? L"" : fileName.substr(dot);
        dest = FindImagesDir() + L"\\" + stem + L"_" + std::to_wstring(GetTickCount()) + ext;
    }
    if (CopyFileW(source.c_str(), dest.c_str(), FALSE)) return dest;
    return resolved;
}

std::unordered_set<std::wstring> CollectImagePathsFromJson(const std::wstring& jsonContent) {
    std::unordered_set<std::wstring> paths;
    const auto blocks = ExtractJsonActionBlocks(jsonContent);
    for (const auto& block : blocks) {
        const auto type = ExtractString(block, L"type");
        const auto imgPath = ExtractString(block, L"imagePath");
        if (imgPath.empty()) continue;
        if (type == L"findImage") {
            paths.insert(ResolveImagePath(imgPath));
        } else if (type == L"textRecognition"
            && ExtractNumber(block, L"ocrRegionByImage", 0) != 0) {
            paths.insert(ResolveImagePath(imgPath));
        }
    }
    return paths;
}

std::unordered_set<std::wstring> CollectAllReferencedImages() {
    std::unordered_set<std::wstring> allPaths;
    WIN32_FIND_DATAW fd{};
    const auto scriptsDir = ScriptsDir();
    const auto recordingsDir = RecordingsDir();
    const std::vector<std::wstring> dirs = {scriptsDir, recordingsDir};
    for (const auto& dir : dirs) {
        const std::wstring pattern = dir + L"\\*.json";
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring filePath = dir + L"\\" + fd.cFileName;
            const auto content = ReadAll(filePath);
            if (content.empty()) continue;
            auto imgPaths = CollectImagePathsFromJson(content);
            allPaths.insert(imgPaths.begin(), imgPaths.end());
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    return allPaths;
}

int CleanOrphanImages() {
    const auto imgDir = FindImagesDir();
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = imgDir + L"\\*.*";
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;
    const auto referenced = CollectAllReferencedImages();
    int deleted = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring filePath = imgDir + L"\\" + fd.cFileName;
        bool isReferenced = referenced.find(filePath) != referenced.end();
        if (!isReferenced) {
            for (const auto& ref : referenced) {
                if (_wcsicmp(ref.c_str(), filePath.c_str()) == 0) {
                    isReferenced = true;
                    break;
                }
            }
        }
        if (!isReferenced && DeleteFileW(filePath.c_str())) ++deleted;
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    return deleted;
}

void DeleteUnreferencedImagesOfScript(const std::wstring& scriptPath) {
    if (scriptPath.empty()) return;
    const auto content = ReadAll(scriptPath);
    if (content.empty()) return;
    const auto scriptImages = CollectImagePathsFromJson(content);
    if (scriptImages.empty()) return;
    // 检查其他脚本是否引用了这些图片
    const auto allReferenced = CollectAllReferencedImages();
    // 注意：CollectAllReferencedImages 会包含当前脚本的引用（因为文件还在），
    // 但实际上我们在删除脚本文件之后才调用此函数，所以文件已经不存在了。
    // 这里传入的是路径，但脚本可能已被删除。为了安全，我们重新收集所有引用（排除当前脚本）。
    // 由于当前脚本文件可能已被删除，CollectAllReferencedImages 已经不会包含它的引用了。
    // 所以我们直接检查 allReferenced 中是否包含这些图片即可。
    // 但如果脚本文件尚未删除，我们需要排除当前脚本的引用。
    
    // 为安全起见，手动排除当前脚本的引用后检查
    std::unordered_set<std::wstring> otherRefs;
    const auto scriptsDir = ScriptsDir();
    const auto recordingsDir = RecordingsDir();
    const std::vector<std::wstring> dirs = {scriptsDir, recordingsDir};
    WIN32_FIND_DATAW fd{};
    for (const auto& dir : dirs) {
        const std::wstring pattern = dir + L"\\*.json";
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring fp = dir + L"\\" + fd.cFileName;
            if (_wcsicmp(fp.c_str(), scriptPath.c_str()) == 0) continue; // 跳过当前脚本
            const auto c = ReadAll(fp);
            if (!c.empty()) {
                auto imgs = CollectImagePathsFromJson(c);
                otherRefs.insert(imgs.begin(), imgs.end());
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    for (const auto& img : scriptImages) {
        if (otherRefs.find(img) == otherRefs.end()) {
            DeleteFileW(img.c_str());
        }
    }
}

// ── ZIP 压缩包操作（stored 方式）────────────────────────────────

#pragma pack(push, 1)
struct ZipLocalFileHeader {
    uint32_t signature = 0x04034b50;
    uint16_t versionNeeded = 20;
    uint16_t flags = 0;
    uint16_t compression = 0; // 0 = stored
    uint16_t modTime = 0;
    uint16_t modDate = 0;
    uint32_t crc32 = 0;
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    uint16_t fileNameLen = 0;
    uint16_t extraLen = 0;
};

struct ZipCentralDirHeader {
    uint32_t signature = 0x02014b50;
    uint16_t versionMadeBy = 20;
    uint16_t versionNeeded = 20;
    uint16_t flags = 0;
    uint16_t compression = 0;
    uint16_t modTime = 0;
    uint16_t modDate = 0;
    uint32_t crc32 = 0;
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    uint16_t fileNameLen = 0;
    uint16_t extraLen = 0;
    uint16_t commentLen = 0;
    uint16_t diskStart = 0;
    uint16_t internalAttr = 0;
    uint32_t externalAttr = 0;
    uint32_t localHeaderOffset = 0;
};

struct ZipEndOfCentralDir {
    uint32_t signature = 0x06054b50;
    uint16_t diskNum = 0;
    uint16_t centralDirDisk = 0;
    uint16_t entriesOnDisk = 0;
    uint16_t totalEntries = 0;
    uint32_t centralDirSize = 0;
    uint32_t centralDirOffset = 0;
    uint16_t commentLen = 0;
};
#pragma pack(pop)

static uint32_t ComputeCrc32(const void* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
            else crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

static std::string ArchiveNameUtf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    const int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "";
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

static bool ReadBinaryFileW(const std::wstring& path, std::vector<uint8_t>& out) {
    out.clear();
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > 0x7FFFFFFF) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    return ok && read == out.size();
}

static bool WriteAllBytes(HANDLE h, const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    size_t written = 0;
    while (written < size) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - written, 0x7FFFFFFF));
        DWORD n = 0;
        if (!WriteFile(h, p + written, chunk, &n, nullptr) || n == 0) return false;
        written += n;
    }
    return true;
}

static bool FindEocdInBuffer(const std::vector<uint8_t>& buf, ZipEndOfCentralDir& eocd, uint32_t& eocdOffset) {
    if (buf.size() < sizeof(ZipEndOfCentralDir)) return false;
    const size_t searchStart = (buf.size() > 65557) ? (buf.size() - 65557) : 0;
    for (size_t pos = searchStart; pos + 4 <= buf.size(); ++pos) {
        uint32_t sig = 0;
        memcpy(&sig, buf.data() + pos, 4);
        if (sig == 0x06054b50) {
            if (pos + sizeof(ZipEndOfCentralDir) > buf.size()) return false;
            memcpy(&eocd, buf.data() + pos, sizeof(eocd));
            eocdOffset = static_cast<uint32_t>(pos);
            return true;
        }
    }
    return false;
}

CreateZipResult CreateZipFile(const std::wstring& zipPath,
                              const std::vector<std::pair<std::wstring, std::wstring>>& files,
                              const std::wstring& requiredLocalPath) {
    CreateZipResult result{};
    struct FileEntry {
        std::string name;
        std::vector<uint8_t> data;
        uint32_t crc32 = 0;
    };
    std::vector<FileEntry> entries;
    entries.reserve(files.size());

    for (const auto& [archiveName, localPath] : files) {
        std::vector<uint8_t> data;
        if (!ReadBinaryFileW(localPath, data)) {
            if (!requiredLocalPath.empty() && _wcsicmp(localPath.c_str(), requiredLocalPath.c_str()) == 0) {
                return result;
            }
            result.skippedFiles.push_back(localPath);
            continue;
        }
        FileEntry entry;
        entry.name = ArchiveNameUtf8(archiveName);
        if (entry.name.empty()) {
            result.skippedFiles.push_back(localPath);
            continue;
        }
        entry.data = std::move(data);
        entry.crc32 = ComputeCrc32(entry.data.data(), entry.data.size());
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) return result;

    std::vector<uint32_t> localOffsets;
    localOffsets.reserve(entries.size());
    uint32_t offset = 0;
    for (const auto& e : entries) {
        localOffsets.push_back(offset);
        offset += static_cast<uint32_t>(sizeof(ZipLocalFileHeader) + e.name.size() + e.data.size());
    }
    const uint32_t centralDirStart = offset;

    const HANDLE h = CreateFileW(zipPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return result;

    auto fail = [&]() {
        CloseHandle(h);
        DeleteFileW(zipPath.c_str());
        result.success = false;
        return result;
    };

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        ZipLocalFileHeader lh{};
        lh.compSize = static_cast<uint32_t>(e.data.size());
        lh.uncompSize = static_cast<uint32_t>(e.data.size());
        lh.crc32 = e.crc32;
        lh.fileNameLen = static_cast<uint16_t>(e.name.size());
        if (!WriteAllBytes(h, &lh, sizeof(lh)) ||
            !WriteAllBytes(h, e.name.data(), e.name.size()) ||
            !WriteAllBytes(h, e.data.data(), e.data.size())) {
            return fail();
        }
    }

    uint32_t centralDirSize = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        ZipCentralDirHeader cd{};
        cd.compSize = static_cast<uint32_t>(e.data.size());
        cd.uncompSize = static_cast<uint32_t>(e.data.size());
        cd.crc32 = e.crc32;
        cd.fileNameLen = static_cast<uint16_t>(e.name.size());
        cd.localHeaderOffset = localOffsets[i];
        centralDirSize += static_cast<uint32_t>(sizeof(ZipCentralDirHeader) + e.name.size());
        if (!WriteAllBytes(h, &cd, sizeof(cd)) ||
            !WriteAllBytes(h, e.name.data(), e.name.size())) {
            return fail();
        }
    }

    ZipEndOfCentralDir eocd{};
    eocd.entriesOnDisk = static_cast<uint16_t>(entries.size());
    eocd.totalEntries = static_cast<uint16_t>(entries.size());
    eocd.centralDirSize = centralDirSize;
    eocd.centralDirOffset = centralDirStart;
    if (!WriteAllBytes(h, &eocd, sizeof(eocd))) {
        return fail();
    }

    CloseHandle(h);
    result.success = true;
    return result;
}

int ExtractZipFile(const std::wstring& zipPath, const std::wstring& destDir) {
    std::vector<uint8_t> buf;
    if (!ReadBinaryFileW(zipPath, buf)) return -1;

    ZipEndOfCentralDir eocd{};
    uint32_t eocdOffset = 0;
    if (!FindEocdInBuffer(buf, eocd, eocdOffset)) return -1;
    if (eocd.totalEntries == 0) return 0;

    CreateDirectoryW(destDir.c_str(), nullptr);

    size_t pos = eocd.centralDirOffset;
    int extracted = 0;
    for (uint16_t i = 0; i < eocd.totalEntries; ++i) {
        if (pos + sizeof(ZipCentralDirHeader) > buf.size()) break;
        ZipCentralDirHeader cd{};
        memcpy(&cd, buf.data() + pos, sizeof(cd));
        pos += sizeof(cd);
        if (cd.signature != 0x02014b50) break;

        if (pos + cd.fileNameLen > buf.size()) break;
        std::string archiveName(reinterpret_cast<const char*>(buf.data() + pos), cd.fileNameLen);
        pos += cd.fileNameLen + cd.extraLen + cd.commentLen;

        if (!archiveName.empty() && archiveName.back() == '/') continue;

        if (cd.localHeaderOffset + sizeof(ZipLocalFileHeader) > buf.size()) continue;
        ZipLocalFileHeader lh{};
        memcpy(&lh, buf.data() + cd.localHeaderOffset, sizeof(lh));
        if (lh.signature != 0x04034b50) continue;

        const size_t dataOffset = cd.localHeaderOffset + sizeof(ZipLocalFileHeader)
            + lh.fileNameLen + lh.extraLen;
        if (dataOffset + cd.compSize > buf.size()) continue;

        const std::wstring destPath = destDir + L"\\" +
            std::wstring(archiveName.begin(), archiveName.end());
        const HANDLE h = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        const bool ok = WriteAllBytes(h, buf.data() + dataOffset, cd.compSize);
        CloseHandle(h);
        if (ok) ++extracted;
    }
    return extracted;
}

std::string ReadTextFromZip(const std::wstring& zipPath, const std::string& archiveName) {
    std::vector<uint8_t> buf;
    if (!ReadBinaryFileW(zipPath, buf)) return "";

    ZipEndOfCentralDir eocd{};
    uint32_t eocdOffset = 0;
    if (!FindEocdInBuffer(buf, eocd, eocdOffset)) return "";

    size_t pos = eocd.centralDirOffset;
    for (uint16_t i = 0; i < eocd.totalEntries; ++i) {
        if (pos + sizeof(ZipCentralDirHeader) > buf.size()) break;
        ZipCentralDirHeader cd{};
        memcpy(&cd, buf.data() + pos, sizeof(cd));
        pos += sizeof(cd);
        if (cd.signature != 0x02014b50) break;

        if (pos + cd.fileNameLen > buf.size()) break;
        std::string name(reinterpret_cast<const char*>(buf.data() + pos), cd.fileNameLen);
        pos += cd.fileNameLen + cd.extraLen + cd.commentLen;

        if (name == archiveName && cd.compSize > 0) {
            if (cd.localHeaderOffset + sizeof(ZipLocalFileHeader) > buf.size()) return "";
            ZipLocalFileHeader lh{};
            memcpy(&lh, buf.data() + cd.localHeaderOffset, sizeof(lh));
            const size_t dataOffset = cd.localHeaderOffset + sizeof(ZipLocalFileHeader)
                + lh.fileNameLen + lh.extraLen;
            if (dataOffset + cd.compSize > buf.size()) return "";
            return std::string(reinterpret_cast<const char*>(buf.data() + dataOffset), cd.compSize);
        }
    }
    return "";
}

// ── 热键与按键名称 ────────────────────────────────────────────────
std::wstring VkName(UINT vk) {
    if (vk >= 'A' && vk <= 'Z')
        return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= '0' && vk <= '9')
        return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24)
        return L"F" + std::to_wstring(vk - VK_F1 + 1);
    switch (vk) {
    case VK_SPACE:    return L"空格键";
    case VK_RETURN:   return L"Enter";
    case VK_ESCAPE:   return L"Esc";
    case VK_TAB:      return L"Tab";
    case VK_BACK:     return L"Backspace";
    case VK_CAPITAL:  return L"CapsLock";
    case VK_NUMLOCK:  return L"NumLock";
    case VK_SCROLL:   return L"ScrollLock";
    case VK_PAUSE:    return L"Pause";
    case VK_SNAPSHOT: return L"截屏键";
    case VK_INSERT:   return L"Ins";
    case VK_DELETE:   return L"Del";
    case VK_HOME:     return L"Home";
    case VK_END:      return L"End";
    case VK_PRIOR:    return L"PgUp";
    case VK_NEXT:     return L"PgDn";
    case VK_LEFT:     return L"←";
    case VK_UP:       return L"↑";
    case VK_RIGHT:    return L"→";
    case VK_DOWN:     return L"↓";
    case VK_LWIN:     return L"LWin";
    case VK_RWIN:     return L"RWin";
    case VK_LCONTROL: return L"LCtrl";
    case VK_RCONTROL: return L"RCtrl";
    case VK_LMENU:    return L"LAlt";
    case VK_RMENU:    return L"RAlt";
    case VK_LSHIFT:   return L"LShift";
    case VK_RSHIFT:   return L"RShift";
    case VK_APPS:     return L"Apps";
    case VK_MULTIPLY: return L"Num*";
    case VK_ADD:      return L"Num+";
    case VK_SUBTRACT: return L"Num-";
    case VK_DECIMAL:  return L"Num.";
    case VK_DIVIDE:   return L"Num/";
    case VK_NUMPAD0:  return L"Num0";
    case VK_NUMPAD1:  return L"Num1";
    case VK_NUMPAD2:  return L"Num2";
    case VK_NUMPAD3:  return L"Num3";
    case VK_NUMPAD4:  return L"Num4";
    case VK_NUMPAD5:  return L"Num5";
    case VK_NUMPAD6:  return L"Num6";
    case VK_NUMPAD7:  return L"Num7";
    case VK_NUMPAD8:  return L"Num8";
    case VK_NUMPAD9:  return L"Num9";
    case VK_LBUTTON:  return L"鼠标左键";
    case VK_MBUTTON:  return L"鼠标中键";
    case VK_RBUTTON:  return L"鼠标右键";
    case VK_XBUTTON1: return L"鼠标侧键1";
    case VK_XBUTTON2: return L"鼠标侧键2";
    case VK_OEM_COMMA:  return L",";
    case VK_OEM_PERIOD: return L".";
    case VK_OEM_MINUS:  return L"-";
    case VK_OEM_PLUS:   return L"=";
    case VK_OEM_1:      return L";";
    case VK_OEM_2:      return L"/";
    case VK_OEM_3:      return L"`";
    case VK_OEM_4:      return L"[";
    case VK_OEM_5:      return L"\\";
    case VK_OEM_6:      return L"]";
    case VK_OEM_7:      return L"'";
    case VK_CLEAR:      return L"Clear";
    }
    return L"按键" + std::to_wstring(vk);
}

std::wstring HotkeyText(UINT modifiers, UINT vk) {
    std::wstring text;
    if (modifiers & MOD_CONTROL) text += L"Ctrl + ";
    if (modifiers & MOD_ALT)     text += L"Alt + ";
    if (modifiers & MOD_SHIFT)   text += L"Shift + ";
    if (modifiers & MOD_WIN)     text += L"Win + ";
    if (vk == 0) {
        if (text.size() >= 3) text.erase(text.size() - 3);
        return text.empty() ? L"无" : text;
    }
    text += VkName(vk);
    return text;
}

std::wstring FormatDuration(double sec) {
    const int total = std::max(0, static_cast<int>(sec + 0.5));
    const int m = total / 60;
    const int s = total % 60;
    return std::to_wstring(m) + L"' " + std::to_wstring(s) + L"\"";
}
