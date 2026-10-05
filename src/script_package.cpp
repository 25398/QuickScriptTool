// ──────────────────────────────────────────────────────────────────
// script_package.cpp — 脚本包依赖收集实现（见 script_package.h 的说明）
// ──────────────────────────────────────────────────────────────────
#include "script_package.h"

#include "utils.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <sstream>
#include <unordered_set>

namespace scriptpkg {
namespace {

/// 动作块在原文里的位置（**带偏移**，便于精确替换）。
/// utils.h 的 ExtractJsonActionBlocks 只返回文本，无法定位，所以这里自己扫一遍。
struct BlockSpan {
    size_t begin = 0;  // '{' 的全局下标
    size_t end = 0;    // '}' 的全局下标
    std::wstring text;
};

/// 扫描 JSON 里某个顶层对象数组的成员块（默认 key = "actions"）。
bool ScanObjectArrayBlocks(const std::wstring& content, const std::wstring& key,
    std::vector<BlockSpan>& out) {
    out.clear();
    const auto keyPos = content.find(L"\"" + key + L"\"");
    if (keyPos == std::wstring::npos) return false;
    const auto arrayStart = content.find(L'[', keyPos);
    if (arrayStart == std::wstring::npos) return false;

    size_t pos = arrayStart + 1;
    while (pos < content.size()) {
        while (pos < content.size() && (content[pos] == L' ' || content[pos] == L'\n'
            || content[pos] == L'\r' || content[pos] == L'\t' || content[pos] == L',')) {
            ++pos;
        }
        if (pos >= content.size() || content[pos] == L']') break;
        if (content[pos] != L'{') {
            if (content[pos] == L'"') {
                ++pos;
                while (pos < content.size()) {
                    if (content[pos] == L'\\') { pos += 2; continue; }
                    if (content[pos] == L'"') { ++pos; break; }
                    ++pos;
                }
            } else {
                ++pos;
            }
            continue;
        }
        const size_t objEnd = FindMatchingJsonBrace(content, pos);
        if (objEnd == std::wstring::npos) break;
        BlockSpan span;
        span.begin = pos;
        span.end = objEnd;
        span.text = content.substr(pos, objEnd - pos + 1);
        out.push_back(std::move(span));
        pos = objEnd + 1;
    }
    return true;
}

std::wstring ToLower(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& ch : out) ch = static_cast<wchar_t>(towlower(ch));
    return out;
}

std::wstring BaseNameNoExt(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    std::wstring name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name = name.substr(0, dot);
    return name;
}

/// 规范化为小写全路径，作为访问集 / 去重的 key。
std::wstring NormalizeKey(const std::wstring& path) {
    wchar_t buf[MAX_PATH]{};
    if (GetFullPathNameW(path.c_str(), MAX_PATH, buf, nullptr) != 0)
        return ToLower(buf);
    return ToLower(path);
}

const std::wstring* LookupRemap(
    const std::vector<std::pair<std::wstring, std::wstring>>& remap,
    const std::wstring& oldValue) {
    const std::wstring lower = ToLower(Trim(oldValue));
    for (const auto& kv : remap) {
        if (ToLower(Trim(kv.first)) == lower) return &kv.second;
    }
    return nullptr;
}

/// 在 block 内定位 `"<field>": "<value>"` 的值字面量，返回 block 内 [start, end) 区间。
bool LocateFieldValueSpan(const std::wstring& block, const std::wstring& field,
    size_t& valueStart, size_t& valueEnd) {
    const std::wstring prefix = L"\"" + field + L"\"";
    auto p = block.find(prefix);
    if (p == std::wstring::npos) return false;
    auto q = block.find(L'"', p + prefix.size());
    if (q == std::wstring::npos) return false;
    size_t i = q + 1;
    while (i < block.size()) {
        if (block[i] == L'\\') { i += 2; continue; }
        if (block[i] == L'"') break;
        ++i;
    }
    if (i >= block.size()) return false;
    valueStart = q + 1;
    valueEnd = i;
    return true;
}

}  // namespace

std::vector<NestedRef> CollectNestedRefsFromJson(const std::wstring& json) {
    std::vector<NestedRef> refs;
    std::vector<BlockSpan> blocks;
    if (!ScanObjectArrayBlocks(json, L"actions", blocks)) return refs;
    for (const auto& b : blocks) {
        const auto type = ExtractString(b.text, L"type");
        if (type != L"runMacro" && type != L"mousePlayback") continue;
        NestedRef ref;
        ref.targetPath = Trim(ExtractString(b.text, L"targetPath"));
        ref.blockName = Trim(ExtractString(b.text, L"blockName"));
        if (ref.targetPath.empty() && ref.blockName.empty()) continue;
        refs.push_back(std::move(ref));
    }
    return refs;
}

int RewriteNestedTargetPaths(std::wstring& json,
    const std::vector<std::pair<std::wstring, std::wstring>>& remap) {
    if (remap.empty()) return 0;
    std::vector<BlockSpan> blocks;
    if (!ScanObjectArrayBlocks(json, L"actions", blocks)) return 0;

    // **倒序**替换：后面的替换不影响前面块的偏移。
    int count = 0;
    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
        const auto& b = *it;
        const auto type = ExtractString(b.text, L"type");
        if (type != L"runMacro" && type != L"mousePlayback") continue;
        const auto oldValue = ExtractString(b.text, L"targetPath");
        if (oldValue.empty()) continue;
        const std::wstring* newValue = LookupRemap(remap, oldValue);
        if (!newValue || newValue->empty()) continue;
        size_t vs = 0, ve = 0;
        if (!LocateFieldValueSpan(b.text, L"targetPath", vs, ve)) continue;
        const size_t gStart = b.begin + vs;
        const size_t gEnd = b.begin + ve;
        if (gEnd > json.size() || gStart > gEnd) continue;
        json.replace(gStart, gEnd - gStart, EscapeJson(*newValue));
        ++count;
    }
    return count;
}

std::wstring SanitizeEntryFileName(const std::wstring& name) {
    std::wstring out = Trim(name);
    const auto slash = out.find_last_of(L"\\/");
    if (slash != std::wstring::npos) out = out.substr(slash + 1);
    for (wchar_t& ch : out) {
        if (ch < 32 || wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
    }
    // Windows 不允许结尾是点或空格
    while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) out.pop_back();
    return out;
}

bool BuildPackagePlan(const std::wstring& rootPath, PackagePlan& out, std::wstring& err) {
    out = PackagePlan{};
    err.clear();
    if (Trim(rootPath).empty()) {
        err = L"脚本路径为空";
        return false;
    }

    std::unordered_set<std::wstring> visited;
    std::unordered_set<std::wstring> usedStems;

    ScriptEntry root;
    root.srcPath = rootPath;
    root.entryName = kRootEntry;
    root.displayName = BaseNameNoExt(rootPath);
    root.isRoot = true;
    out.scripts.push_back(root);
    visited.insert(NormalizeKey(rootPath));

    // BFS：边遍历边追加（out.scripts 同时充当队列）
    size_t head = 0;
    while (head < out.scripts.size()) {
        const ScriptEntry cur = out.scripts[head++];
        const std::wstring content = ReadAll(cur.srcPath);
        if (content.empty()) {
            if (cur.isRoot) {
                err = L"脚本内容为空或读取失败：" + cur.displayName;
                return false;
            }
            if (std::find(out.missingRefs.begin(), out.missingRefs.end(), cur.displayName)
                == out.missingRefs.end()) {
                out.missingRefs.push_back(cur.displayName);
            }
            continue;
        }

        for (const auto& img : CollectImagePathsFromJson(content)) {
            if (img.empty()) continue;
            if (std::find(out.images.begin(), out.images.end(), img) == out.images.end())
                out.images.push_back(img);
        }

        for (const auto& ref : CollectNestedRefsFromJson(content)) {
            std::wstring resolved;
            // 先 targetPath（同机器导出时最准），再 blockName（跨机器/名字解析）
            if (!ref.targetPath.empty()) ResolveLibraryScriptPath(ref.targetPath, resolved);
            if (resolved.empty() && !ref.blockName.empty())
                ResolveLibraryScriptPath(ref.blockName, resolved);
            if (resolved.empty()) {
                const std::wstring label = ref.targetPath.empty() ? ref.blockName : ref.targetPath;
                if (!label.empty()
                    && std::find(out.missingRefs.begin(), out.missingRefs.end(), label)
                        == out.missingRefs.end()) {
                    out.missingRefs.push_back(label);
                }
                continue;
            }
            const std::wstring key = NormalizeKey(resolved);
            if (visited.count(key)) continue;  // 循环引用 / 重复引用：已收过
            visited.insert(key);

            std::wstring stem = SanitizeEntryFileName(BaseNameNoExt(resolved));
            if (stem.empty()) stem = L"script";
            std::wstring unique = stem;
            int suffix = 2;
            while (usedStems.count(ToLower(unique))) {
                unique = stem + L"-" + std::to_wstring(suffix++);
            }
            usedStems.insert(ToLower(unique));

            ScriptEntry e;
            e.srcPath = resolved;
            e.entryName = std::wstring(kNestedDir) + L"\\" + unique + L".json";
            e.portableRef = unique + L".json";
            e.displayName = unique;
            if (!ref.targetPath.empty()) e.originalRefs.push_back(ref.targetPath);
            if (!ref.blockName.empty()) e.originalRefs.push_back(ref.blockName);
            out.scripts.push_back(std::move(e));
            out.hasNested = true;
        }
    }
    return true;
}

std::wstring BuildPackageManifestJson(const PackagePlan& plan, const PlayerManifest& player) {
    std::wstringstream oss;
    oss << L"{\n";
    oss << L"  \"v\": " << kManifestVersion << L",\n";
    oss << L"  \"root\": \"" << EscapeJson(kRootEntry) << L"\",\n";
    oss << L"  \"scripts\": [";
    bool first = true;
    for (const auto& e : plan.scripts) {
        if (e.isRoot) continue;
        oss << (first ? L"\n" : L",\n");
        first = false;
        oss << L"    {\"entry\": \"" << EscapeJson(e.entryName)
            << L"\", \"ref\": \"" << EscapeJson(e.portableRef)
            << L"\", \"name\": \"" << EscapeJson(e.displayName) << L"\"}";
    }
    oss << (first ? L"" : L"\n  ") << L"],\n";
    oss << L"  \"images\": [";
    for (size_t i = 0; i < plan.images.size(); ++i) {
        const auto slash = plan.images[i].find_last_of(L"\\/");
        const std::wstring base = (slash == std::wstring::npos)
            ? plan.images[i] : plan.images[i].substr(slash + 1);
        oss << (i ? L", " : L"") << L"\"" << EscapeJson(base) << L"\"";
    }
    oss << L"],\n";
    // player 段：player 启动时据此决定「找图 / OCR 用哪一份」
    oss << L"  \"player\": {\n";
    oss << L"    \"v\": " << player.version << L",\n";
    oss << L"    \"needOpenCv\": " << (player.needOpenCv ? 1 : 0) << L",\n";
    oss << L"    \"needOcr\": " << (player.needOcr ? 1 : 0) << L",\n";
    oss << L"    \"needFakeFocus\": " << (player.needFakeFocus ? 1 : 0) << L",\n";
    oss << L"    \"bundledOpenCv\": " << (player.mode.bundledOpenCv ? 1 : 0) << L",\n";
    oss << L"    \"bundledOcr\": " << (player.mode.bundledOcr ? 1 : 0) << L",\n";
    oss << L"    \"bundledFakeFocus\": " << (player.mode.bundledFakeFocus ? 1 : 0) << L",\n";
    oss << L"    \"createdBy\": \"" << EscapeJson(player.createdBy) << L"\"\n";
    oss << L"  }\n";
    oss << L"}\n";
    return oss.str();
}

bool ParsePlayerManifest(const std::wstring& json, PlayerManifest& out) {
    out = PlayerManifest{};
    if (json.empty()) return false;
    // "player" 是顶层对象（不是数组）：定位 key 后取第一个 '{' 到匹配的 '}'
    const auto key = json.find(L"\"player\"");
    if (key == std::wstring::npos) return false;
    const auto open = json.find(L'{', key);
    if (open == std::wstring::npos) return false;
    const auto close = FindMatchingJsonBrace(json, open);
    if (close == std::wstring::npos) return false;
    const std::wstring seg = json.substr(open, close - open + 1);

    out.version = static_cast<int>(ExtractNumber(seg, L"v", 1));
    out.needOpenCv = ExtractNumber(seg, L"needOpenCv", 0) != 0;
    out.needOcr = ExtractNumber(seg, L"needOcr", 0) != 0;
    out.needFakeFocus = ExtractNumber(seg, L"needFakeFocus", 0) != 0;
    out.mode.bundledOpenCv = ExtractNumber(seg, L"bundledOpenCv", 1) != 0;
    out.mode.bundledOcr = ExtractNumber(seg, L"bundledOcr", 1) != 0;
    out.mode.bundledFakeFocus = ExtractNumber(seg, L"bundledFakeFocus", 1) != 0;
    out.createdBy = ExtractString(seg, L"createdBy");
    return true;
}

// ── 能力体检 ──────────────────────────────────────────────────────

namespace {

/// 这个动作是否依赖 OpenCV（找图 / 找色）。
/// AI 动作里哪些要 OpenCV —— 逐条对着引擎代码核过：
///   · `aiImageAnalysis` / `aiActionExecute`：
///       ① `aiRegionByImage` + `aiTargetImagePath` → `LoadBitmapFromFile`（找图定位截屏区）
///       ② **发截图给模型**要 `BitmapToBase64Jpeg` → `cv::MatFromBitmap` / `cv::resize` /
///          `cv::imencode`。这条是**无条件**的 —— 没有 OpenCV 时它返回空串，
///          AI 拿不到图，动作会以"截图编码失败"告终（不崩，但一定失败）。
///   · `aiTextAnalysis`：纯文本，全程不碰图 —— 不打包 OpenCV，省 65MB。
bool AiActionNeedsOpenCv(const std::wstring& type) {
    return type == L"aiImageAnalysis" || type == L"aiActionExecute";
}

bool ActionNeedsOpenCv(const std::wstring& type, const std::wstring& block) {
    if (type == L"findImage" || type == L"multiMatch" || type == L"watchImage") return true;
    if (type == L"findColor" || type == L"colorMatch") return true;
    // getColor / mouseDrag 只在「找图定位」时才用 OpenCV
    if (type == L"getColor" || type == L"mouseDrag") {
        return ExtractNumber(block, L"imageLocate", 0) != 0;
    }
    // ⚠ 文字识别按图取区域（ocrRegionByImage）：引擎会**先找图**拿到区域再 OCR，
    //   那条路要 OpenCV（LoadBitmapFromFile 里有 cv::Mat）。
    //   漏掉这一条 = 导出的 exe 不带 OpenCV，而执行时仍然去调 OpenCV →
    //   delay-load 桩找不到 DLL → 抛 0xC06D007E → **进程当场死**
    //   （没日志、没结束音，用户完全无法理解）。实测踩过。
    if (type == L"textRecognition") {
        return ExtractNumber(block, L"ocrRegionByImage", 0) != 0;
    }
    // AI 图片分析 / 动作执行：见 AiActionNeedsOpenCv 的说明（发截图那一步无条件要 OpenCV）
    if (AiActionNeedsOpenCv(type)) return true;
    return false;
}

bool ActionNeedsOcr(const std::wstring& type) {
    return type == L"textRecognition";
}

bool ActionNeedsAi(const std::wstring& type) {
    return type == L"aiTextAnalysis" || type == L"aiImageAnalysis" || type == L"aiActionExecute";
}


bool ActionIsExternal(const std::wstring& type) {
    return type == L"runProgram" || type == L"openFile" || type == L"openWebpage";
}

}  // namespace

void ScanActionCapabilities(const std::wstring& json, CapabilityScan& acc) {
    if (json.empty()) return;
    std::vector<BlockSpan> blocks;
    if (!ScanObjectArrayBlocks(json, L"actions", blocks)) return;
    for (const auto& b : blocks) {
        const auto type = ExtractString(b.text, L"type");
        if (type.empty()) continue;
        ++acc.totalActions;
        if (ActionNeedsOpenCv(type, b.text)) ++acc.imageActions;
        if (ActionNeedsOcr(type)) ++acc.ocrActions;
        if (ActionNeedsAi(type)) ++acc.aiActions;
        if (ActionIsExternal(type)) ++acc.externalActions;
    }
    // 脚本级配置：后台窗口模式 / 自带热键
    const auto wmKey = json.find(L"\"windowMode\"");
    if (wmKey != std::wstring::npos) {
        const auto open = json.find(L'{', wmKey);
        if (open != std::wstring::npos) {
            const auto close = FindMatchingJsonBrace(json, open);
            if (close != std::wstring::npos) {
                const std::wstring seg = json.substr(open, close - open + 1);
                if (ExtractNumber(seg, L"enabled", 0) != 0) acc.windowMode = true;
            }
        }
    }
    if (ExtractNumber(json, L"hotkeyVk", 0) > 0) acc.hasHotkey = true;
}

CapabilityScan ScanPackageCapabilities(const PackagePlan& plan) {
    CapabilityScan acc;
    for (const auto& e : plan.scripts) {
        const std::wstring content = ReadAll(e.srcPath);
        if (content.empty()) continue;
        ScanActionCapabilities(content, acc);
    }
    return acc;
}

// ── payload 尾部 ──────────────────────────────────────────────────

namespace {

constexpr char kPayloadMagic[8] = { 'Q', 'S', 'T', 'P', 'K', 'G', '0', '1' };
/// 8 magic + 8 offset + 8 size + 8 hash。
/// 哈希写进尾标而不是每次启动重算 —— payload 可能 70MB，重算要读全量。
constexpr size_t kTailSize = 32;

uint64_t Fnv1a64Step(uint64_t h, const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}

void WriteU64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
}

uint64_t ReadU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (i * 8);
    return v;
}

}  // namespace

bool AppendPayloadToExe(const std::wstring& targetExePath, const std::wstring& payloadZipPath,
    PayloadInfo* outInfo) {
    if (targetExePath.empty() || payloadZipPath.empty()) return false;

    const HANDLE src = CreateFileW(payloadZipPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (src == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER srcSize{};
    if (!GetFileSizeEx(src, &srcSize) || srcSize.QuadPart <= 0) {
        CloseHandle(src);
        return false;
    }

    const HANDLE dst = CreateFileW(targetExePath.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dst == INVALID_HANDLE_VALUE) {
        CloseHandle(src);
        return false;
    }
    LARGE_INTEGER exeSize{};
    if (!GetFileSizeEx(dst, &exeSize) || exeSize.QuadPart <= 0) {
        CloseHandle(src);
        CloseHandle(dst);
        return false;
    }

    // 追加点 = 当前 exe 末尾（调用方必须传「已复制出来的干净副本」，不能是模板本体）
    LARGE_INTEGER pos{};
    pos.QuadPart = exeSize.QuadPart;
    bool ok = SetFilePointerEx(dst, pos, nullptr, FILE_BEGIN) != 0;

    uint64_t hash = 1469598103934665603ull;
    uint8_t buf[64 * 1024];
    uint64_t written = 0;
    while (ok) {
        DWORD got = 0;
        if (!ReadFile(src, buf, sizeof(buf), &got, nullptr) || got == 0) break;
        hash = Fnv1a64Step(hash, buf, got);
        DWORD put = 0;
        if (!WriteFile(dst, buf, got, &put, nullptr) || put != got) {
            ok = false;
            break;
        }
        written += got;
    }
    if (ok && written != static_cast<uint64_t>(srcSize.QuadPart)) ok = false;

    if (ok) {
        uint8_t tail[kTailSize];
        memcpy(tail, kPayloadMagic, 8);
        WriteU64(tail + 8, static_cast<uint64_t>(exeSize.QuadPart));
        WriteU64(tail + 16, written);
        WriteU64(tail + 24, hash);
        DWORD put = 0;
        ok = WriteFile(dst, tail, kTailSize, &put, nullptr) && put == kTailSize;
    }

    CloseHandle(src);
    CloseHandle(dst);
    if (!ok) return false;

    if (outInfo) {
        outInfo->offset = static_cast<uint64_t>(exeSize.QuadPart);
        outInfo->size = written;
        outInfo->hash = hash;
    }
    return true;
}

bool ReadPayloadInfo(const std::wstring& exePath, PayloadInfo& out) {
    out = PayloadInfo{};
    const HANDLE h = CreateFileW(exePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= static_cast<LONGLONG>(kTailSize)) {
        CloseHandle(h);
        return false;
    }
    LARGE_INTEGER pos{};
    pos.QuadPart = size.QuadPart - static_cast<LONGLONG>(kTailSize);
    uint8_t tail[kTailSize]{};
    DWORD got = 0;
    const bool read = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0
        && ReadFile(h, tail, kTailSize, &got, nullptr) && got == kTailSize;
    CloseHandle(h);
    if (!read) return false;
    if (memcmp(tail, kPayloadMagic, 8) != 0) return false;

    const uint64_t offset = ReadU64(tail + 8);
    const uint64_t payloadSize = ReadU64(tail + 16);
    if (payloadSize == 0) return false;
    if (offset + payloadSize + kTailSize != static_cast<uint64_t>(size.QuadPart)) return false;

    out.offset = offset;
    out.size = payloadSize;
    out.hash = ReadU64(tail + 24);
    return true;
}

bool DumpPayloadToZip(const std::wstring& exePath, const PayloadInfo& info,
    const std::wstring& zipOutPath) {
    if (info.size == 0) return false;
    const HANDLE src = CreateFileW(exePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (src == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(info.offset);
    if (SetFilePointerEx(src, pos, nullptr, FILE_BEGIN) == 0) {
        CloseHandle(src);
        return false;
    }
    const HANDLE dst = CreateFileW(zipOutPath.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dst == INVALID_HANDLE_VALUE) {
        CloseHandle(src);
        return false;
    }

    uint8_t buf[64 * 1024];
    uint64_t remain = info.size;
    bool ok = true;
    while (remain > 0) {
        const DWORD want = static_cast<DWORD>(remain > sizeof(buf) ? sizeof(buf) : remain);
        DWORD got = 0;
        if (!ReadFile(src, buf, want, &got, nullptr) || got == 0) {
            ok = false;
            break;
        }
        DWORD put = 0;
        if (!WriteFile(dst, buf, got, &put, nullptr) || put != got) {
            ok = false;
            break;
        }
        remain -= got;
    }
    CloseHandle(src);
    CloseHandle(dst);
    if (!ok) {
        DeleteFileW(zipOutPath.c_str());
        return false;
    }
    return true;
}

bool ExtractEmbeddedPayload(const std::wstring& exePath, const std::wstring& zipOutPath,
    PayloadInfo* outInfo) {
    PayloadInfo info;
    if (!ReadPayloadInfo(exePath, info)) return false;
    if (!DumpPayloadToZip(exePath, info, zipOutPath)) return false;
    if (outInfo) *outInfo = info;
    return true;
}

bool ParsePackageManifestJson(const std::wstring& json, std::vector<ManifestEntry>& out) {
    out.clear();
    if (json.empty()) return false;
    std::vector<BlockSpan> blocks;
    if (!ScanObjectArrayBlocks(json, L"scripts", blocks)) return false;
    for (const auto& b : blocks) {
        ManifestEntry e;
        e.entryName = ExtractString(b.text, L"entry");
        e.portableRef = ExtractString(b.text, L"ref");
        e.displayName = ExtractString(b.text, L"name");
        if (e.entryName.empty()) continue;
        if (e.displayName.empty()) e.displayName = BaseNameNoExt(e.entryName);
        if (e.portableRef.empty()) e.portableRef = e.displayName + L".json";
        out.push_back(std::move(e));
    }
    return true;
}

}  // namespace scriptpkg
